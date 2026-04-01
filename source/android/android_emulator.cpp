/*
 * Boxedwine Android Emulator - Core Implementation
 *
 * Orchestrates APK loading, ELF linking, JNI setup, syscall routing,
 * and ARMv7/AArch64 CPU execution for running Android native apps on UWP.
 *
 * Architecture overview:
 *
 *   APK file  -->  apk_loader  -->  raw ELF .so bytes
 *                                        |
 *                              android_linker
 *                              (maps ELFs into flat 256 MB
 *                               emulated address space,
 *                               applies ARM relocations)
 *                                        |
 *                              android_jni_init
 *                              (creates JNIEnv / JavaVM stubs)
 *                                        |
 *                              cpu: ArmV7State or AArch64State
 *                              (interprets ARMv7/Thumb or A64 instructions)
 *                                   |       |
 *                              SWI/SVC handler
 *                                   |       |
 *                         android_syscall   |
 *                         (Linux ARM ABI)   |
 *                                           |
 *                                     bionic stubs registered
 *                                     as linker symbol overrides
 *
 * Reference:
 *   referenceCode/apkenv/apkenv.c
 *   referenceCode/Bridge/BridgeLib/android_init.cpp
 */

#include "android_emulator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <ctype.h>
#include <wctype.h>
#include <wchar.h>
#include <locale.h>
#include <time.h>
#include <errno.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <poll.h>
#  include <sys/uio.h>
#  include <sys/stat.h>
#  include <dirent.h>
#  include <fcntl.h>
#endif

/* -------------------------------------------------------------------------
 * Debug / error logging macros.
 *
 * On MSVC (UWP), fprintf(stderr) is invisible — the Visual Studio Output
 * window only captures OutputDebugStringA.  We route both debug-only and
 * always-on error messages through that API when building with MSVC in a
 * debug configuration.
 * ---------------------------------------------------------------------- */
#if defined(_MSC_VER)
#  include <windows.h>                 /* OutputDebugStringA              */
#  include <stdarg.h>
   static inline void _emu_output_dbg(const char *fmt, ...) {
       char buf[1024];
       va_list ap;
       va_start(ap, fmt);
       vsnprintf(buf, sizeof(buf), fmt, ap);
       va_end(ap);
       OutputDebugStringA(buf);
   }
#  ifdef _DEBUG
#    define EMU_LOG_DEBUG(fmt, ...) _emu_output_dbg("[EMU DEBUG] " fmt "\n", ##__VA_ARGS__)
#  else
#    define EMU_LOG_DEBUG(fmt, ...) ((void)0)
#  endif
#  define EMU_LOG_ERR(fmt, ...)   _emu_output_dbg("[EMU ERROR] " fmt "\n", ##__VA_ARGS__)
#  define EMU_LOG_INFO(fmt, ...)  _emu_output_dbg("[EMU INFO]  " fmt "\n", ##__VA_ARGS__)
#else
#  if !defined(NDEBUG)
#    define EMU_LOG_DEBUG(fmt, ...) fprintf(stderr, "[EMU DEBUG] " fmt "\n", ##__VA_ARGS__)
#  else
#    define EMU_LOG_DEBUG(fmt, ...) ((void)0)
#  endif
#  define EMU_LOG_ERR(fmt, ...)   fprintf(stderr, "[EMU ERROR] " fmt "\n", ##__VA_ARGS__)
#  define EMU_LOG_INFO(fmt, ...)  fprintf(stdout, "[EMU INFO]  " fmt "\n", ##__VA_ARGS__)
#endif

/* -------------------------------------------------------------------------
 * Helpers for extracting ARM ABI arguments
 *
 * armeabi-v7a uses soft-float: floats pass in r0-r3 as bit-patterns.
 * Extra args (>4) are on the stack at SP.
 * ---------------------------------------------------------------------- */
static inline float reg_to_float(uint32_t r) {
    float f; memcpy(&f, &r, 4); return f;
}

static inline uint32_t stack_u32(const uint8_t *mem, uint32_t mem_size, uint32_t sp, int index) {
    uint32_t addr = sp + (uint32_t)(index * 4);
    if (addr + 4 > mem_size) return 0;
    uint32_t v; memcpy(&v, mem + addr, 4); return v;
}

static inline float stack_float(const uint8_t *mem, uint32_t mem_size, uint32_t sp, int index) {
    uint32_t bits = stack_u32(mem, mem_size, sp, index);
    return reg_to_float(bits);
}

/* -------------------------------------------------------------------------
 * SWI handler called from the ARMv7 CPU interpreter
 * ---------------------------------------------------------------------- */
static void swi_handler(void *ctx, uint32_t swi_num) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    android_syscall_dispatch(&emu->cpu, &emu->syscall_state, swi_num);
}

/* -------------------------------------------------------------------------
 * Undefined instruction handler
 * ---------------------------------------------------------------------- */
static void undef_handler(void *ctx, uint32_t instr) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    fprintf(stderr, "android_emulator: undefined instruction 0x%08X at PC=0x%08X\n",
            instr, emu->cpu.r[ARM_PC]);
    emu->cpu.running = false;
}

/* -------------------------------------------------------------------------
 * AArch64 SVC handler
 * ---------------------------------------------------------------------- */
static void svc_handler_64(void *ctx, uint32_t svc_num) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    /* Forward to bionic stub dispatch or syscall */
    if (svc_num > 0 && svc_num < 256) {
        /* For now, bionic stubs for AArch64 use the same SVC numbering.
         * The stub dispatch reads registers from cpu64 instead of cpu. */
        /* TODO: implement AArch64-specific bionic dispatch */
    }
    /* Map AArch64 syscall: x8 = syscall number, x0-x5 = args */
    (void)svc_num;
}

/* -------------------------------------------------------------------------
 * AArch64 undefined instruction handler
 * ---------------------------------------------------------------------- */
static void undef_handler_64(void *ctx, uint32_t instr) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    fprintf(stderr, "android_emulator: undefined A64 instruction 0x%08X at PC=0x%016llX\n",
            instr, (unsigned long long)emu->cpu64.pc);
    emu->cpu64.running = false;
}

/* -------------------------------------------------------------------------
 * Memory access callbacks for ARMv7 (32-bit address, pass-through to flat memory)
 * ---------------------------------------------------------------------- */
static uint8_t mem_r8(void *ctx, uint32_t addr) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    if (addr < emu->mem_size) return emu->mem[addr];
    fprintf(stderr, "android_emulator: read8 out of bounds addr=0x%08X\n", addr);
    return 0;
}
static uint16_t mem_r16(void *ctx, uint32_t addr) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    uint16_t v = 0;
    if (addr + 2 <= emu->mem_size) memcpy(&v, emu->mem + addr, 2);
    return v;
}
static uint32_t mem_r32(void *ctx, uint32_t addr) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    uint32_t v = 0;
    if (addr <= emu->mem_size - 4) memcpy(&v, emu->mem + addr, 4);
    return v;
}
static void mem_w8(void *ctx, uint32_t addr, uint8_t v) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    if (addr < emu->mem_size) emu->mem[addr] = v;
}
static void mem_w16(void *ctx, uint32_t addr, uint16_t v) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    if (addr + 2 <= emu->mem_size) memcpy(emu->mem + addr, &v, 2);
}
static void mem_w32(void *ctx, uint32_t addr, uint32_t v) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    if (addr + 4 <= emu->mem_size) memcpy(emu->mem + addr, &v, 4);
}

/* -------------------------------------------------------------------------
 * Memory access callbacks for AArch64 (64-bit address, truncated to 32-bit range)
 * ---------------------------------------------------------------------- */
static uint8_t mem64_r8(void *ctx, uint64_t addr) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    uint32_t a = (uint32_t)addr;
    if (a < emu->mem_size) return emu->mem[a];
    fprintf(stderr, "android_emulator: read8 out of bounds addr=0x%016llX\n", (unsigned long long)addr);
    return 0;
}
static uint16_t mem64_r16(void *ctx, uint64_t addr) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    uint32_t a = (uint32_t)addr;
    uint16_t v = 0;
    if (a + 2 <= emu->mem_size) memcpy(&v, emu->mem + a, 2);
    return v;
}
static uint32_t mem64_r32(void *ctx, uint64_t addr) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    uint32_t a = (uint32_t)addr;
    uint32_t v = 0;
    if (a + 4 <= emu->mem_size) memcpy(&v, emu->mem + a, 4);
    return v;
}
static uint64_t mem64_r64(void *ctx, uint64_t addr) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    uint32_t a = (uint32_t)addr;
    uint64_t v = 0;
    if (a + 8 <= emu->mem_size) memcpy(&v, emu->mem + a, 8);
    return v;
}
static void mem64_w8(void *ctx, uint64_t addr, uint8_t v) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    uint32_t a = (uint32_t)addr;
    if (a < emu->mem_size) emu->mem[a] = v;
}
static void mem64_w16(void *ctx, uint64_t addr, uint16_t v) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    uint32_t a = (uint32_t)addr;
    if (a + 2 <= emu->mem_size) memcpy(emu->mem + a, &v, 2);
}
static void mem64_w32(void *ctx, uint64_t addr, uint32_t v) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    uint32_t a = (uint32_t)addr;
    if (a + 4 <= emu->mem_size) memcpy(emu->mem + a, &v, 4);
}
static void mem64_w64(void *ctx, uint64_t addr, uint64_t v) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    uint32_t a = (uint32_t)addr;
    if (a + 8 <= emu->mem_size) memcpy(emu->mem + a, &v, 8);
}

/* -------------------------------------------------------------------------
 * Install Bionic shim stubs in emulated memory
 *
 * We reserve a small trampoline area just below ANDROID_LOAD_BASE
 * and fill it with "SVC 0 / BX LR" pairs.  Each stub is registered
 * as a symbol override in the linker so native code can call our
 * host implementations.
 *
 * The SVC handler reads a stub index from memory at the SVC address
 * to dispatch to the right host function.
 * ---------------------------------------------------------------------- */

#define BIONIC_STUB_BASE    0x1000u
#define BIONIC_STUB_STRIDE  8u          /* 8 bytes per stub: SVC imm24 + BX LR */

/* Stub index for each bionic function.
 * Each entry gets a Thumb SVC+BX LR trampoline at a fixed VA. */
enum BionicStub {
    /* ---- Original 25 stubs (1-25) ---- */
    STUB_PTHREAD_CREATE = 1,
    STUB_PTHREAD_JOIN,
    STUB_PTHREAD_MUTEX_LOCK,
    STUB_PTHREAD_MUTEX_UNLOCK,
    STUB_MALLOC,
    STUB_FREE,
    STUB_CALLOC,
    STUB_REALLOC,
    STUB_MEMCPY,
    STUB_MEMMOVE,
    STUB_MEMSET,
    STUB_STRLEN,
    STUB_STRCPY,
    STUB_STRNCPY,
    STUB_STRCMP,
    STUB_STRNCMP,
    STUB_PRINTF,
    STUB_SPRINTF,
    STUB_SNPRINTF,
    STUB_PUTS,
    STUB_ABORT,
    STUB_DLOPEN,
    STUB_DLSYM,
    STUB_DLCLOSE,
    STUB_DLERROR,

    /* ---- pthread extensions ---- */
    STUB_PTHREAD_MUTEX_INIT,
    STUB_PTHREAD_MUTEX_DESTROY,
    STUB_PTHREAD_SELF,
    STUB_PTHREAD_EQUAL,
    STUB_PTHREAD_ATTR_INIT,
    STUB_PTHREAD_ATTR_DESTROY,
    STUB_PTHREAD_ATTR_SETDETACHSTATE,
    STUB_PTHREAD_ATTR_SETSCHEDPARAM,
    STUB_PTHREAD_ATTR_SETSTACKSIZE,
    STUB_PTHREAD_COND_INIT,
    STUB_PTHREAD_COND_DESTROY,
    STUB_PTHREAD_COND_WAIT,
    STUB_PTHREAD_COND_SIGNAL,
    STUB_PTHREAD_COND_BROADCAST,
    STUB_PTHREAD_COND_TIMEDWAIT,
    STUB_PTHREAD_DETACH,
    STUB_PTHREAD_KEY_CREATE,
    STUB_PTHREAD_KEY_DELETE,
    STUB_PTHREAD_GETSPECIFIC,
    STUB_PTHREAD_SETSPECIFIC,
    STUB_PTHREAD_ONCE,
    STUB_PTHREAD_MUTEXATTR_INIT,
    STUB_PTHREAD_MUTEXATTR_DESTROY,

    /* ---- C library / stdio ---- */
    STUB_FOPEN,
    STUB_FCLOSE,
    STUB_FREAD,
    STUB_FWRITE,
    STUB_FSEEK,
    STUB_FTELL,
    STUB_FGETS,
    STUB_FPUTS,
    STUB_FEOF,
    STUB_FERROR,
    STUB_FFLUSH,
    STUB_FPRINTF,
    STUB_FSCANF,
    STUB_SSCANF,
    STUB_VSNPRINTF,
    STUB_VSPRINTF,
    STUB_FPUTC,
    STUB_FDOPEN,
    STUB_SETVBUF,
    STUB_GETC,
    STUB_PUTC,
    STUB_UNGETC,
    STUB_PUTCHAR,

    /* ---- C string/memory ---- */
    STUB_MEMCMP,
    STUB_MEMCHR,
    STUB_STRCHR,
    STUB_STRPBRK,
    STUB_STRCAT,
    STUB_STRTOL,
    STUB_STRTOK,
    STUB_STRERROR,
    STUB_STRTOULL,
    STUB_STRTOD,
    STUB_STRCASECMP,
    STUB_STRNCASECMP,
    STUB_STRCOLL,
    STUB_STRXFRM,
    STUB_ATOI,

    /* ---- Math ---- */
    STUB_SIN,
    STUB_COS,
    STUB_SINF,
    STUB_COSF,
    STUB_ATAN,
    STUB_ATAN2,
    STUB_ATAN2F,
    STUB_SQRT,
    STUB_SQRTF,
    STUB_POW,
    STUB_POWF,
    STUB_CEIL,
    STUB_CEILF,
    STUB_FLOOR,
    STUB_FLOORF,
    STUB_FMOD,
    STUB_FMODF,
    STUB_MODFF,
    STUB_LDEXP,
    STUB_LOGF,

    /* ---- Time ---- */
    STUB_GETTIMEOFDAY,
    STUB_USLEEP,
    STUB_TIME,
    STUB_NANOSLEEP,
    STUB_GMTIME,
    STUB_FTIME,
    STUB_STRFTIME,

    /* ---- Process ---- */
    STUB_GETPID,
    STUB_RAISE,
    STUB_BSD_SIGNAL,
    STUB_SYSCONF,
    STUB_SETLOCALE,

    /* ---- I/O / filesystem ---- */
    STUB_OPEN_POSIX,   /* "open" (not syscall, libc wrapper) */
    STUB_CLOSE_POSIX,
    STUB_READ_POSIX,
    STUB_WRITE_POSIX,
    STUB_LSEEK_POSIX,
    STUB_FCNTL,
    STUB_PIPE,
    STUB_REMOVE,
    STUB_RENAME,
    STUB_MKDIR,
    STUB_ACCESS,
    STUB_OPENDIR,
    STUB_READDIR,
    STUB_CLOSEDIR,
    STUB_FSTAT,
    STUB_WRITEV,
    STUB_POLL,
    STUB_IOCTL,
    STUB_SELECT,

    /* ---- Network (BSD sockets) ---- */
    STUB_SOCKET,
    STUB_BIND,
    STUB_LISTEN,
    STUB_ACCEPT,
    STUB_CONNECT,
    STUB_SEND,
    STUB_RECV,
    STUB_SENDTO,
    STUB_RECVFROM,
    STUB_SETSOCKOPT,
    STUB_GETSOCKOPT,
    STUB_GETSOCKNAME,
    STUB_GETHOSTBYNAME,
    STUB_GETHOSTNAME,
    STUB_INET_NTOA,
    STUB_INET_ADDR,
    STUB_SHUTDOWN,

    /* ---- zlib ---- */
    STUB_DEFLATEINIT,
    STUB_DEFLATEEND,
    STUB_DEFLATE,
    STUB_INFLATEINIT,
    STUB_INFLATEEND,
    STUB_INFLATE,
    STUB_DEFLATEINIT2,
    STUB_UNCOMPRESS,

    /* ---- Random ---- */
    STUB_LRAND48,
    STUB_SRAND48,
    STUB_DIV,

    /* ---- Wide char ---- */
    STUB_WCSLEN,
    STUB_WMEMCHR,
    STUB_WMEMCPY,
    STUB_WMEMSET,
    STUB_WMEMMOVE,
    STUB_WMEMCMP,
    STUB_PUTWC,
    STUB_GETWC,
    STUB_UNGETWC,
    STUB_WCRTOMB,
    STUB_MBRTOWC,
    STUB_WCSCOLL,
    STUB_WCSXFRM,
    STUB_WCSFTIME,
    STUB_WCTYPE,
    STUB_TOWUPPER,
    STUB_TOWLOWER,
    STUB_ISWCTYPE,
    STUB_WCTOB,
    STUB_BTOWC,

    /* ---- Bionic / Android internal ---- */
    STUB___ERRNO,
    STUB___STACK_CHK_FAIL,
    STUB___CXA_ATEXIT,
    STUB___CXA_FINALIZE,
    STUB___ANDROID_LOG_PRINT,
    STUB_SYSCALL,

    /* ---- OpenGL ES 1.x fixed function ---- */
    STUB_GLENABLE,
    STUB_GLDISABLE,
    STUB_GLCLEAR,
    STUB_GLCLEARCOLOR,
    STUB_GLVIEWPORT,
    STUB_GLSCISSOR,
    STUB_GLMATRIXMODE,
    STUB_GLLOADIDENTITY,
    STUB_GLPUSHMATRIX,
    STUB_GLPOPMATRIX,
    STUB_GLTRANSLATEF,
    STUB_GLSCALEF,
    STUB_GLROTATEF,
    STUB_GLORTHOF,
    STUB_GLMULTMATRIXF,
    STUB_GLCOLOR4F,
    STUB_GLBLENDFUNC,
    STUB_GLDEPTHFUNC,
    STUB_GLDEPTHMASK,
    STUB_GLDEPTHRANGEF,
    STUB_GLALPHAFUNC,
    STUB_GLCULLFACE,
    STUB_GLSHADEMODEL,
    STUB_GLENABLECLIENTSTATE,
    STUB_GLDISABLECLIENTSTATE,
    STUB_GLHINT,
    STUB_GLSTENCILFUNC,
    STUB_GLSTENCILMASK,
    STUB_GLSTENCILOP,
    STUB_GLLIGHTMODELF,
    STUB_GLLIGHTFV,
    STUB_GLPOLYGONOFFSET,
    STUB_GLLINEWIDTH,
    STUB_GLCOLORMASK,
    STUB_GLFOGF,
    STUB_GLFOGFV,
    STUB_GLFOGX,
    STUB_GLBINDTEXTURE,
    STUB_GLGENTEXTURES,
    STUB_GLDELETETEXTURES,
    STUB_GLTEXIMAGE2D,
    STUB_GLTEXSUBIMAGE2D,
    STUB_GLTEXPARAMETERI,
    STUB_GLVERTEXPOINTER,
    STUB_GLTEXCOORDPOINTER,
    STUB_GLCOLORPOINTER,
    STUB_GLNORMALPOINTER,
    STUB_GLBINDBUFFER,
    STUB_GLGENBUFFERS,
    STUB_GLDELETEBUFFERS,
    STUB_GLBUFFERDATA,
    STUB_GLDRAWARRAYS,
    STUB_GLDRAWELEMENTS,
    STUB_GLGETERROR,
    STUB_GLGETSTRING,
    STUB_GLGETFLOATV,
    STUB_GLREADPIXELS,

    /* ---- EGL ---- */
    STUB_EGLGETDISPLAY,
    STUB_EGLINITIALIZE,
    STUB_EGLCHOOSECONFIG,
    STUB_EGLGETCONFIGATTRIB,
    STUB_EGLCREATEWINDOWSURFACE,
    STUB_EGLCREATECONTEXT,
    STUB_EGLMAKECURRENT,
    STUB_EGLQUERYSURFACE,
    STUB_EGLSWAPBUFFERS,
    STUB_EGLSWAPINTERVAL,
    STUB_EGLGETCURRENTDISPLAY,
    STUB_EGLDESTROYCONTEXT,
    STUB_EGLDESTROYSURFACE,
    STUB_EGLTERMINATE,

    /* ---- Android NDK ---- */
    STUB_AASSETMANAGER_OPEN,
    STUB_AASSET_GETLENGTH,
    STUB_AASSET_GETBUFFER,
    STUB_AASSET_CLOSE,
    STUB_AINPUTEVENT_GETTYPE,
    STUB_AKEYEVENT_GETACTION,
    STUB_AKEYEVENT_GETKEYCODE,
    STUB_AKEYEVENT_GETMETASTATE,
    STUB_AINPUTEVENT_GETDEVICEID,
    STUB_AKEYEVENT_GETREPEATCOUNT,
    STUB_AINPUTEVENT_GETSOURCE,
    STUB_AMOTIONEVENT_GETACTION,
    STUB_AMOTIONEVENT_GETPOINTERID,
    STUB_AMOTIONEVENT_GETX,
    STUB_AMOTIONEVENT_GETY,
    STUB_AMOTIONEVENT_GETPOINTERCOUNT,
    STUB_AINPUTQUEUE_GETEVENT,
    STUB_AINPUTQUEUE_FINISHEVENT,
    STUB_AINPUTQUEUE_PREDISPATCHEVENT,
    STUB_AINPUTQUEUE_ATTACHLOOPER,
    STUB_AINPUTQUEUE_DETACHLOOPER,
    STUB_ALOOPER_POLLALL,
    STUB_ALOOPER_PREPARE,
    STUB_ALOOPER_ADDFD,
    STUB_ANATIVEACTIVITY_FINISH,
    STUB_ANATIVEWINDOW_SETBUFFERSGEOMETRY,
    STUB_ACONFIGURATION_NEW,
    STUB_ACONFIGURATION_FROMASSETMANAGER,
    STUB_ACONFIGURATION_GETLANGUAGE,
    STUB_ACONFIGURATION_GETCOUNTRY,
    STUB_ACONFIGURATION_DELETE,

    /* ---- OpenSL ES ---- */
    STUB_SLCREATEENGINE,
    STUB_SL_ENGINE_CREATEOUTPUTMIX,
    STUB_SL_ENGINE_CREATEAUDIOPLAYER,
    STUB_SL_ENGINE_GETINTERFACE,
    STUB_SL_OUTPUTMIX_REALIZE,
    STUB_SL_PLAYER_REALIZE,
    STUB_SL_PLAYER_GETINTERFACE,
    STUB_SL_PLAYER_SETPLAYSTATE,
    STUB_SL_BUFFERQUEUE_ENQUEUE,
    STUB_SL_BUFFERQUEUE_REGISTERCALLBACK,
    STUB_SL_BUFFERQUEUE_CLEAR,
    STUB_SL_VOLUME_SETVOLUME,
    STUB_SL_OBJECT_DESTROY,

    STUB_COUNT  /* must be last */
};

/* Data symbol virtual addresses (not trampolines, just reserved memory) */
#define DATA_SYMBOL_BASE  0x0E00u
#define DATA_SYMBOL_STRIDE 64u

/* Map stub index to symbol name */
static const char *stub_names[STUB_COUNT] = {
    NULL,
    /* 1-25: original stubs */
    "pthread_create", "pthread_join", "pthread_mutex_lock", "pthread_mutex_unlock",
    "malloc", "free", "calloc", "realloc",
    "memcpy", "memmove", "memset",
    "strlen", "strcpy", "strncpy", "strcmp", "strncmp",
    "printf", "sprintf", "snprintf", "puts", "abort",
    "dlopen", "dlsym", "dlclose", "dlerror",
    /* pthread extensions */
    "pthread_mutex_init", "pthread_mutex_destroy",
    "pthread_self", "pthread_equal",
    "pthread_attr_init", "pthread_attr_destroy",
    "pthread_attr_setdetachstate", "pthread_attr_setschedparam",
    "pthread_attr_setstacksize",
    "pthread_cond_init", "pthread_cond_destroy",
    "pthread_cond_wait", "pthread_cond_signal", "pthread_cond_broadcast",
    "pthread_cond_timedwait", "pthread_detach",
    "pthread_key_create", "pthread_key_delete",
    "pthread_getspecific", "pthread_setspecific",
    "pthread_once",
    "pthread_mutexattr_init", "pthread_mutexattr_destroy",
    /* C stdio */
    "fopen", "fclose", "fread", "fwrite",
    "fseek", "ftell", "fgets", "fputs",
    "feof", "ferror", "fflush", "fprintf", "fscanf", "sscanf",
    "vsnprintf", "vsprintf", "fputc", "fdopen", "setvbuf",
    "getc", "putc", "ungetc", "putchar",
    /* C string/memory */
    "memcmp", "memchr", "strchr", "strpbrk", "strcat",
    "strtol", "strtok", "strerror", "strtoull", "strtod",
    "strcasecmp", "strncasecmp", "strcoll", "strxfrm", "atoi",
    /* math */
    "sin", "cos", "sinf", "cosf",
    "atan", "atan2", "atan2f",
    "sqrt", "sqrtf", "pow", "powf",
    "ceil", "ceilf", "floor", "floorf",
    "fmod", "fmodf", "modff", "ldexp", "logf",
    /* time */
    "gettimeofday", "usleep", "time", "nanosleep",
    "gmtime", "ftime", "strftime",
    /* process */
    "getpid", "raise", "bsd_signal", "sysconf", "setlocale",
    /* I/O / filesystem */
    "open", "close", "read", "write", "lseek",
    "fcntl", "pipe", "remove", "rename", "mkdir", "access",
    "opendir", "readdir", "closedir",
    "fstat", "writev", "poll", "ioctl", "select",
    /* network */
    "socket", "bind", "listen", "accept", "connect",
    "send", "recv", "sendto", "recvfrom",
    "setsockopt", "getsockopt", "getsockname",
    "gethostbyname", "gethostname", "inet_ntoa", "inet_addr", "shutdown",
    /* zlib */
    "deflateInit_", "deflateEnd", "deflate",
    "inflateInit_", "inflateEnd", "inflate",
    "deflateInit2_", "uncompress",
    /* random */
    "lrand48", "srand48", "div",
    /* wide char */
    "wcslen", "wmemchr", "wmemcpy", "wmemset", "wmemmove", "wmemcmp",
    "putwc", "getwc", "ungetwc",
    "wcrtomb", "mbrtowc", "wcscoll", "wcsxfrm", "wcsftime",
    "wctype", "towupper", "towlower", "iswctype", "wctob", "btowc",
    /* bionic internal */
    "__errno", "__stack_chk_fail",
    "__cxa_atexit", "__cxa_finalize",
    "__android_log_print", "syscall",
    /* GL ES 1.x */
    "glEnable", "glDisable", "glClear", "glClearColor",
    "glViewport", "glScissor",
    "glMatrixMode", "glLoadIdentity", "glPushMatrix", "glPopMatrix",
    "glTranslatef", "glScalef", "glRotatef", "glOrthof", "glMultMatrixf",
    "glColor4f", "glBlendFunc",
    "glDepthFunc", "glDepthMask", "glDepthRangef",
    "glAlphaFunc", "glCullFace", "glShadeModel",
    "glEnableClientState", "glDisableClientState",
    "glHint", "glStencilFunc", "glStencilMask", "glStencilOp",
    "glLightModelf", "glLightfv", "glPolygonOffset", "glLineWidth",
    "glColorMask",
    "glFogf", "glFogfv", "glFogx",
    "glBindTexture", "glGenTextures", "glDeleteTextures",
    "glTexImage2D", "glTexSubImage2D", "glTexParameteri",
    "glVertexPointer", "glTexCoordPointer", "glColorPointer", "glNormalPointer",
    "glBindBuffer", "glGenBuffers", "glDeleteBuffers", "glBufferData",
    "glDrawArrays", "glDrawElements",
    "glGetError", "glGetString", "glGetFloatv", "glReadPixels",
    /* EGL */
    "eglGetDisplay", "eglInitialize", "eglChooseConfig", "eglGetConfigAttrib",
    "eglCreateWindowSurface", "eglCreateContext", "eglMakeCurrent",
    "eglQuerySurface", "eglSwapBuffers", "eglSwapInterval",
    "eglGetCurrentDisplay", "eglDestroyContext", "eglDestroySurface", "eglTerminate",
    /* Android NDK */
    "AAssetManager_open", "AAsset_getLength", "AAsset_getBuffer", "AAsset_close",
    "AInputEvent_getType", "AKeyEvent_getAction", "AKeyEvent_getKeyCode",
    "AKeyEvent_getMetaState", "AInputEvent_getDeviceId", "AKeyEvent_getRepeatCount",
    "AInputEvent_getSource",
    "AMotionEvent_getAction", "AMotionEvent_getPointerId",
    "AMotionEvent_getX", "AMotionEvent_getY", "AMotionEvent_getPointerCount",
    "AInputQueue_getEvent", "AInputQueue_finishEvent",
    "AInputQueue_preDispatchEvent", "AInputQueue_attachLooper", "AInputQueue_detachLooper",
    "ALooper_pollAll", "ALooper_prepare", "ALooper_addFd",
    "ANativeActivity_finish", "ANativeWindow_setBuffersGeometry",
    "AConfiguration_new", "AConfiguration_fromAssetManager",
    "AConfiguration_getLanguage", "AConfiguration_getCountry", "AConfiguration_delete",
    /* OpenSL ES */
    "slCreateEngine",
    "sl_engine_CreateOutputMix",
    "sl_engine_CreateAudioPlayer",
    "sl_engine_GetInterface",
    "sl_outputmix_Realize",
    "sl_player_Realize",
    "sl_player_GetInterface",
    "sl_player_SetPlayState",
    "sl_bufferqueue_Enqueue",
    "sl_bufferqueue_RegisterCallback",
    "sl_bufferqueue_Clear",
    "sl_volume_SetVolumeLevel",
    "sl_object_Destroy",
};

/* ARM-mode SWI + BX LR encoding (32-bit ARM with 24-bit immediate).
 *
 * Previous implementation used Thumb SVC #imm8, but Thumb SVC only
 * supports an 8-bit immediate (0-255).  With 294 bionic stubs, stubs
 * 256+ (including eglSwapBuffers, EGL, NDK, and OpenSL ES) were
 * silently truncated and never dispatched correctly.
 *
 * ARM SWI supports a 24-bit immediate, which is more than enough.
 * The stubs are registered WITHOUT the Thumb bit (bit 0 clear) so
 * BX/BLX from Thumb code switches to ARM mode, executes SWI, then
 * BX LR (with Thumb-flagged LR) switches back to the caller's mode.
 */
static void install_stub(uint8_t *mem, uint32_t va, uint32_t stub_id) {
    /* ARM encoding of "SWI #stub_id" = 0xEF000000 | (imm24) */
    /* ARM encoding of "BX LR"        = 0xE12FFF1E              */
    uint32_t swi_instr = 0xEF000000u | (stub_id & 0x00FFFFFFu);
    uint32_t bx_lr     = 0xE12FFF1Eu;
    memcpy(mem + va,     &swi_instr, 4);
    memcpy(mem + va + 4, &bx_lr,     4);
}

/* AArch64 SVC + RET encoding (A64 fixed-width 4-byte instructions) */
static void install_stub_a64(uint8_t *mem, uint32_t va, uint32_t stub_id) {
    /* SVC #imm16 = 0xD4000001 | (imm16 << 5) */
    /* RET (to X30) = 0xD65F03C0 */
    uint32_t svc_instr = 0xD4000001u | ((stub_id & 0xFFFFu) << 5);
    uint32_t ret_instr = 0xD65F03C0u;
    memcpy(mem + va,     &svc_instr, 4);
    memcpy(mem + va + 4, &ret_instr, 4);
}

/* -------------------------------------------------------------------------
 * Bionic stub SVC dispatcher
 *
 * When native code calls one of our stubs, the SVC fires with swi_num
 * equal to the stub_id (< STUB_COUNT).  The syscall dispatcher already
 * handled real Linux syscalls (swi_num >= 1 for Linux).
 * We intercept low stub IDs here.
 *
 * For simplicity many libc stubs are implemented inline here.
 * ---------------------------------------------------------------------- */
static void bionic_stub_dispatch(AndroidEmulator *emu, uint32_t stub_id) {
    ArmV7State *cpu = &emu->cpu;

#ifdef _DEBUG
    /* Track and log first invocation of each stub for debugging */
    static bool stub_seen[STUB_COUNT] = {};
    static unsigned stub_call_count = 0;
    stub_call_count++;
    if (stub_id < STUB_COUNT && !stub_seen[stub_id]) {
        stub_seen[stub_id] = true;
        EMU_LOG_DEBUG("stub[%u]: first call to %s (call #%u, pc=0x%08X, lr=0x%08X, r0=0x%08X)",
                      stub_id,
                      stub_id < STUB_COUNT ? stub_names[stub_id] : "???",
                      stub_call_count,
                      cpu->r[15], cpu->r[ARM_LR], cpu->r[0]);
    }
    /* Log every 100000th call so we can see the emulator is alive */
    if ((stub_call_count % 100000) == 0) {
        EMU_LOG_DEBUG("stub dispatch: %u total calls, pc=0x%08X sp=0x%08X lr=0x%08X",
                      stub_call_count, cpu->r[15], cpu->r[13], cpu->r[ARM_LR]);
    }
#endif

    /* Helper macros for reading emulated memory strings/pointers */
#define EMU_STR(va) ((va) < emu->mem_size ? (char*)(emu->mem + (va)) : (char*)"")
#define EMU_PTR(va) ((va) < emu->mem_size ? (emu->mem + (va)) : (uint8_t*)NULL)
#define BOUNDS_OK(va, sz) ((va) + (sz) <= emu->mem_size)

    switch ((BionicStub)stub_id) {
        /* ==================================================================
         *  Memory allocation (bump allocator)
         * ================================================================== */
        case STUB_MALLOC: {
            uint32_t sz = (cpu->r[0] + 7) & ~7u;
            uint32_t base = emu->syscall_state.brk_current;
            if (base + sz <= emu->syscall_state.brk_max) {
                memset(emu->mem + base, 0, sz);
                emu->syscall_state.brk_current += sz;
                cpu->r[0] = base;
            } else { cpu->r[0] = 0; }
            break;
        }
        case STUB_CALLOC: {
            uint32_t total = (cpu->r[0] * cpu->r[1] + 7) & ~7u;
            uint32_t base  = emu->syscall_state.brk_current;
            if (base + total <= emu->syscall_state.brk_max) {
                memset(emu->mem + base, 0, total);
                emu->syscall_state.brk_current += total;
                cpu->r[0] = base;
            } else { cpu->r[0] = 0; }
            break;
        }
        case STUB_FREE:
            cpu->r[0] = 0; break;
        case STUB_REALLOC: {
            uint32_t old_ptr = cpu->r[0], new_sz = (cpu->r[1] + 7) & ~7u;
            uint32_t base = emu->syscall_state.brk_current;
            if (base + new_sz <= emu->syscall_state.brk_max) {
                if (old_ptr && old_ptr < base) memcpy(emu->mem + base, emu->mem + old_ptr, new_sz);
                emu->syscall_state.brk_current += new_sz;
                cpu->r[0] = base;
            } else { cpu->r[0] = 0; }
            break;
        }

        /* ==================================================================
         *  String / memory operations
         * ================================================================== */
        case STUB_MEMCPY: {
            uint32_t d = cpu->r[0], s = cpu->r[1], n = cpu->r[2];
            if (BOUNDS_OK(d, n) && BOUNDS_OK(s, n)) memcpy(emu->mem + d, emu->mem + s, n);
            break; /* r0 = dst already */
        }
        case STUB_MEMMOVE: {
            uint32_t d = cpu->r[0], s = cpu->r[1], n = cpu->r[2];
            if (BOUNDS_OK(d, n) && BOUNDS_OK(s, n)) memmove(emu->mem + d, emu->mem + s, n);
            break;
        }
        case STUB_MEMSET: {
            uint32_t d = cpu->r[0], v = cpu->r[1], n = cpu->r[2];
            if (BOUNDS_OK(d, n)) memset(emu->mem + d, (int)v, n);
            break;
        }
        case STUB_MEMCMP: {
            uint32_t a = cpu->r[0], b = cpu->r[1], n = cpu->r[2];
            cpu->r[0] = (a < emu->mem_size && b < emu->mem_size)
                ? (uint32_t)memcmp(emu->mem + a, emu->mem + b, n) : 1;
            break;
        }
        case STUB_MEMCHR: {
            uint32_t s = cpu->r[0], c = cpu->r[1], n = cpu->r[2];
            if (s < emu->mem_size) {
                uint8_t *p = (uint8_t*)memchr(emu->mem + s, (int)c, n);
                cpu->r[0] = p ? (uint32_t)(p - emu->mem) : 0;
            } else { cpu->r[0] = 0; }
            break;
        }
        case STUB_STRLEN: {
            uint32_t p = cpu->r[0];
            cpu->r[0] = (p < emu->mem_size) ? (uint32_t)strnlen(EMU_STR(p), emu->mem_size - p) : 0;
            break;
        }
        case STUB_STRCPY: {
            uint32_t d = cpu->r[0], s = cpu->r[1];
            if (d < emu->mem_size && s < emu->mem_size)
                strncpy((char*)emu->mem + d, (char*)emu->mem + s, emu->mem_size - d - 1);
            break;
        }
        case STUB_STRNCPY: {
            uint32_t d = cpu->r[0], s = cpu->r[1], n = cpu->r[2];
            if (BOUNDS_OK(d, n) && s < emu->mem_size)
                strncpy((char*)emu->mem + d, (char*)emu->mem + s, n);
            break;
        }
        case STUB_STRCMP: {
            uint32_t a = cpu->r[0], b = cpu->r[1];
            cpu->r[0] = (a < emu->mem_size && b < emu->mem_size)
                ? (uint32_t)strcmp(EMU_STR(a), EMU_STR(b)) : 1;
            break;
        }
        case STUB_STRNCMP: {
            uint32_t a = cpu->r[0], b = cpu->r[1], n = cpu->r[2];
            cpu->r[0] = (a < emu->mem_size && b < emu->mem_size)
                ? (uint32_t)strncmp(EMU_STR(a), EMU_STR(b), n) : 1;
            break;
        }
        case STUB_STRCHR: {
            uint32_t s = cpu->r[0]; int c = (int)cpu->r[1];
            if (s < emu->mem_size) {
                char *p = strchr(EMU_STR(s), c);
                cpu->r[0] = p ? (uint32_t)(p - (char*)emu->mem) : 0;
            } else { cpu->r[0] = 0; }
            break;
        }
        case STUB_STRPBRK: {
            uint32_t s = cpu->r[0], a = cpu->r[1];
            if (s < emu->mem_size && a < emu->mem_size) {
                char *p = strpbrk(EMU_STR(s), EMU_STR(a));
                cpu->r[0] = p ? (uint32_t)(p - (char*)emu->mem) : 0;
            } else { cpu->r[0] = 0; }
            break;
        }
        case STUB_STRCAT: {
            uint32_t d = cpu->r[0], s = cpu->r[1];
            if (d < emu->mem_size && s < emu->mem_size) {
                size_t dlen = strnlen(EMU_STR(d), emu->mem_size - d);
                size_t slen = strnlen(EMU_STR(s), emu->mem_size - s);
                if (d + dlen + slen + 1 <= emu->mem_size)
                    memcpy(emu->mem + d + dlen, emu->mem + s, slen + 1);
            }
            break;
        }
        case STUB_STRTOL: {
            uint32_t s = cpu->r[0]; int base = (int)cpu->r[2];
            cpu->r[0] = (s < emu->mem_size) ? (uint32_t)strtol(EMU_STR(s), NULL, base) : 0;
            break;
        }
        case STUB_STRTOK: {
            uint32_t s = cpu->r[0], d = cpu->r[1];
            /* strtok with emulated memory is tricky; simplified */
            (void)s; (void)d;
            cpu->r[0] = 0; break;
        }
        case STUB_STRERROR:
            /* Return pointer to a static "error" string in emu memory */
            cpu->r[0] = 0; break;
        case STUB_STRTOULL: {
            uint32_t s = cpu->r[0]; int base = (int)cpu->r[2];
            uint64_t v = (s < emu->mem_size) ? strtoull(EMU_STR(s), NULL, base) : 0;
            cpu->r[0] = (uint32_t)v; cpu->r[1] = (uint32_t)(v >> 32);
            break;
        }
        case STUB_STRTOD: {
            uint32_t s = cpu->r[0];
            double v = (s < emu->mem_size) ? strtod(EMU_STR(s), NULL) : 0.0;
            uint64_t bits; memcpy(&bits, &v, 8);
            cpu->r[0] = (uint32_t)bits; cpu->r[1] = (uint32_t)(bits >> 32);
            break;
        }
        case STUB_STRCASECMP: {
            uint32_t a = cpu->r[0], b = cpu->r[1];
#ifdef _WIN32
            cpu->r[0] = (a < emu->mem_size && b < emu->mem_size)
                ? (uint32_t)_stricmp(EMU_STR(a), EMU_STR(b)) : 1;
#else
            cpu->r[0] = (a < emu->mem_size && b < emu->mem_size)
                ? (uint32_t)strcasecmp(EMU_STR(a), EMU_STR(b)) : 1;
#endif
            break;
        }
        case STUB_STRNCASECMP: {
            uint32_t a = cpu->r[0], b = cpu->r[1], n = cpu->r[2];
#ifdef _WIN32
            cpu->r[0] = (a < emu->mem_size && b < emu->mem_size)
                ? (uint32_t)_strnicmp(EMU_STR(a), EMU_STR(b), n) : 1;
#else
            cpu->r[0] = (a < emu->mem_size && b < emu->mem_size)
                ? (uint32_t)strncasecmp(EMU_STR(a), EMU_STR(b), n) : 1;
#endif
            break;
        }
        case STUB_STRCOLL: {
            uint32_t a = cpu->r[0], b = cpu->r[1];
            cpu->r[0] = (a < emu->mem_size && b < emu->mem_size)
                ? (uint32_t)strcoll(EMU_STR(a), EMU_STR(b)) : 0;
            break;
        }
        case STUB_STRXFRM:
            cpu->r[0] = 0; break;
        case STUB_ATOI:
            cpu->r[0] = (cpu->r[0] < emu->mem_size) ? (uint32_t)atoi(EMU_STR(cpu->r[0])) : 0;
            break;

        /* ==================================================================
         *  stdio
         * ================================================================== */
        case STUB_PRINTF:
        case STUB_FPRINTF:
        case STUB_SPRINTF:
        case STUB_SNPRINTF:
        case STUB_VSNPRINTF:
        case STUB_VSPRINTF: {
            uint32_t fmt_ptr = (stub_id == STUB_PRINTF) ? cpu->r[0] : cpu->r[1];
            if (fmt_ptr < emu->mem_size) {
#if defined(_MSC_VER)
                OutputDebugStringA("[bionic] ");
                OutputDebugStringA(EMU_STR(fmt_ptr));
                OutputDebugStringA("\n");
#else
                fprintf(stdout, "[bionic] %s\n", EMU_STR(fmt_ptr));
#endif
            }
            cpu->r[0] = 0; break;
        }
        case STUB_PUTS: {
            if (cpu->r[0] < emu->mem_size) puts(EMU_STR(cpu->r[0]));
            cpu->r[0] = 0; break;
        }
        case STUB_FOPEN:
        case STUB_FCLOSE:
        case STUB_FREAD:
        case STUB_FWRITE:
        case STUB_FSEEK:
        case STUB_FTELL:
        case STUB_FGETS:
        case STUB_FPUTS:
        case STUB_FEOF:
        case STUB_FERROR:
        case STUB_FFLUSH:
        case STUB_FSCANF:
        case STUB_SSCANF:
        case STUB_FPUTC:
        case STUB_FDOPEN:
        case STUB_SETVBUF:
        case STUB_GETC:
        case STUB_PUTC:
        case STUB_UNGETC:
        case STUB_PUTCHAR:
            /* File I/O stubs - return 0/NULL for now.
             * Real file operations go through syscalls (open/read/write). */
            cpu->r[0] = 0; break;

        /* ==================================================================
         *  Math functions (read float from r0, return float in r0)
         * ================================================================== */
        case STUB_SIN: case STUB_COS: case STUB_ATAN: case STUB_SQRT:
        case STUB_CEIL: case STUB_FLOOR: case STUB_FMOD: case STUB_POW:
        case STUB_ATAN2: case STUB_LDEXP: {
            /* Double args in r0:r1 (and r2:r3 for 2-arg) */
            double a, b = 0, result = 0;
            uint64_t ab; memcpy(&ab, &cpu->r[0], 8); memcpy(&a, &ab, 8);
            if (stub_id == STUB_ATAN2 || stub_id == STUB_POW || stub_id == STUB_FMOD || stub_id == STUB_LDEXP) {
                uint64_t bb; memcpy(&bb, &cpu->r[2], 8); memcpy(&b, &bb, 8);
            }
            switch ((BionicStub)stub_id) {
                case STUB_SIN:   result = sin(a); break;
                case STUB_COS:   result = cos(a); break;
                case STUB_ATAN:  result = atan(a); break;
                case STUB_SQRT:  result = sqrt(a); break;
                case STUB_CEIL:  result = ceil(a); break;
                case STUB_FLOOR: result = floor(a); break;
                case STUB_FMOD:  result = fmod(a, b); break;
                case STUB_POW:   result = pow(a, b); break;
                case STUB_ATAN2: result = atan2(a, b); break;
                case STUB_LDEXP: result = ldexp(a, (int)cpu->r[2]); break;
                default: break;
            }
            uint64_t rb; memcpy(&rb, &result, 8);
            cpu->r[0] = (uint32_t)rb; cpu->r[1] = (uint32_t)(rb >> 32);
            break;
        }
        case STUB_SINF: case STUB_COSF: case STUB_SQRTF: case STUB_POWF:
        case STUB_CEILF: case STUB_FLOORF: case STUB_FMODF: case STUB_ATAN2F:
        case STUB_MODFF: case STUB_LOGF: {
            /* Float arg in r0, second in r1 for 2-arg */
            float a, b = 0, result = 0;
            memcpy(&a, &cpu->r[0], 4);
            memcpy(&b, &cpu->r[1], 4);
            switch ((BionicStub)stub_id) {
                case STUB_SINF:   result = sinf(a); break;
                case STUB_COSF:   result = cosf(a); break;
                case STUB_SQRTF:  result = sqrtf(a); break;
                case STUB_POWF:   result = powf(a, b); break;
                case STUB_CEILF:  result = ceilf(a); break;
                case STUB_FLOORF: result = floorf(a); break;
                case STUB_FMODF:  result = fmodf(a, b); break;
                case STUB_ATAN2F: result = atan2f(a, b); break;
                case STUB_LOGF:   result = logf(a); break;
                case STUB_MODFF:  { float ipart; result = modff(a, &ipart); /* simplified */ break; }
                default: break;
            }
            memcpy(&cpu->r[0], &result, 4);
            break;
        }

        /* ==================================================================
         *  Time
         * ================================================================== */
        case STUB_GETTIMEOFDAY:
        case STUB_TIME:
        case STUB_USLEEP:
        case STUB_NANOSLEEP:
        case STUB_GMTIME:
        case STUB_FTIME:
        case STUB_STRFTIME:
            /* Time operations are handled by syscalls; stubs return 0. */
            cpu->r[0] = 0; break;

        /* ==================================================================
         *  Process
         * ================================================================== */
        case STUB_ABORT:
            fprintf(stderr, "android: abort() called\n");
            emu->cpu.running = false;
            emu->exit_code = 134;
            break;
        case STUB_GETPID:
            cpu->r[0] = (uint32_t)emu->syscall_state.pid; break;
        case STUB_RAISE:
        case STUB_BSD_SIGNAL:
            cpu->r[0] = 0; break;
        case STUB_SYSCONF:
            /* sysconf(_SC_PAGESIZE=0x27) -> 4096 */
            cpu->r[0] = (cpu->r[0] == 0x27) ? 4096 : (uint32_t)-1;
            break;
        case STUB_SETLOCALE:
            cpu->r[0] = 0; break;

        /* ==================================================================
         *  Dynamic linking
         * ================================================================== */
        case STUB_DLOPEN: case STUB_DLSYM: case STUB_DLCLOSE: case STUB_DLERROR:
            cpu->r[0] = 0; break;

        /* ==================================================================
         *  pthread (single-threaded emulation: all return success / no-op)
         * ================================================================== */
        case STUB_PTHREAD_CREATE:
        case STUB_PTHREAD_JOIN:
        case STUB_PTHREAD_MUTEX_LOCK:
        case STUB_PTHREAD_MUTEX_UNLOCK:
        case STUB_PTHREAD_MUTEX_INIT:
        case STUB_PTHREAD_MUTEX_DESTROY:
        case STUB_PTHREAD_ATTR_INIT:
        case STUB_PTHREAD_ATTR_DESTROY:
        case STUB_PTHREAD_ATTR_SETDETACHSTATE:
        case STUB_PTHREAD_ATTR_SETSCHEDPARAM:
        case STUB_PTHREAD_ATTR_SETSTACKSIZE:
        case STUB_PTHREAD_COND_INIT:
        case STUB_PTHREAD_COND_DESTROY:
        case STUB_PTHREAD_COND_WAIT:
        case STUB_PTHREAD_COND_SIGNAL:
        case STUB_PTHREAD_COND_BROADCAST:
        case STUB_PTHREAD_COND_TIMEDWAIT:
        case STUB_PTHREAD_DETACH:
        case STUB_PTHREAD_KEY_CREATE:
        case STUB_PTHREAD_KEY_DELETE:
        case STUB_PTHREAD_GETSPECIFIC:
        case STUB_PTHREAD_SETSPECIFIC:
        case STUB_PTHREAD_ONCE:
        case STUB_PTHREAD_MUTEXATTR_INIT:
        case STUB_PTHREAD_MUTEXATTR_DESTROY:
            cpu->r[0] = 0; break;
        case STUB_PTHREAD_SELF:
            cpu->r[0] = 1; break; /* fake thread id */
        case STUB_PTHREAD_EQUAL:
            cpu->r[0] = (cpu->r[0] == cpu->r[1]) ? 1 : 0; break;

        /* ==================================================================
         *  POSIX file I/O (libc wrappers → delegate to syscall layer)
         * ================================================================== */
        case STUB_OPEN_POSIX:
        case STUB_CLOSE_POSIX:
        case STUB_READ_POSIX:
        case STUB_WRITE_POSIX:
        case STUB_LSEEK_POSIX:
        case STUB_FCNTL:
        case STUB_PIPE:
        case STUB_REMOVE:
        case STUB_RENAME:
        case STUB_MKDIR:
        case STUB_ACCESS:
        case STUB_OPENDIR:
        case STUB_READDIR:
        case STUB_CLOSEDIR:
        case STUB_FSTAT:
        case STUB_WRITEV:
        case STUB_POLL:
        case STUB_IOCTL:
        case STUB_SELECT:
            /* These are normally called via syscall; libc stubs return -1/0 */
            cpu->r[0] = (uint32_t)-1; break;

        /* ==================================================================
         *  Network (stubs - no real networking in emulator)
         * ================================================================== */
        case STUB_SOCKET:
        case STUB_BIND:
        case STUB_LISTEN:
        case STUB_ACCEPT:
        case STUB_CONNECT:
        case STUB_SEND:
        case STUB_RECV:
        case STUB_SENDTO:
        case STUB_RECVFROM:
        case STUB_SETSOCKOPT:
        case STUB_GETSOCKOPT:
        case STUB_GETSOCKNAME:
        case STUB_GETHOSTBYNAME:
        case STUB_GETHOSTNAME:
        case STUB_INET_NTOA:
        case STUB_INET_ADDR:
        case STUB_SHUTDOWN:
            cpu->r[0] = (uint32_t)-1; break;

        /* ==================================================================
         *  zlib (stubs - return error codes)
         * ================================================================== */
        case STUB_DEFLATEINIT:
        case STUB_DEFLATEEND:
        case STUB_DEFLATE:
        case STUB_INFLATEINIT:
        case STUB_INFLATEEND:
        case STUB_INFLATE:
        case STUB_DEFLATEINIT2:
        case STUB_UNCOMPRESS:
            cpu->r[0] = (uint32_t)-2; /* Z_STREAM_ERROR */ break;

        /* ==================================================================
         *  Random
         * ================================================================== */
        case STUB_LRAND48:
            cpu->r[0] = (uint32_t)(rand() & 0x7FFFFFFF); break;
        case STUB_SRAND48:
            srand((unsigned)cpu->r[0]); cpu->r[0] = 0; break;
        case STUB_DIV: {
            int32_t num = (int32_t)cpu->r[0], den = (int32_t)cpu->r[1];
            if (den != 0) { cpu->r[0] = (uint32_t)(num / den); cpu->r[1] = (uint32_t)(num % den); }
            else { cpu->r[0] = 0; cpu->r[1] = 0; }
            break;
        }

        /* ==================================================================
         *  Wide char (stubs)
         * ================================================================== */
        case STUB_WCSLEN: {
            /* wchar_t is 4 bytes on ARM Linux */
            uint32_t p = cpu->r[0]; uint32_t len = 0;
            if (p < emu->mem_size) {
                while (p + 4 <= emu->mem_size) {
                    uint32_t c; memcpy(&c, emu->mem + p, 4);
                    if (c == 0) break;
                    p += 4; len++;
                }
            }
            cpu->r[0] = len; break;
        }
        case STUB_WMEMCHR: case STUB_WMEMCPY: case STUB_WMEMSET:
        case STUB_WMEMMOVE: case STUB_WMEMCMP:
        case STUB_PUTWC: case STUB_GETWC: case STUB_UNGETWC:
        case STUB_WCRTOMB: case STUB_MBRTOWC:
        case STUB_WCSCOLL: case STUB_WCSXFRM: case STUB_WCSFTIME:
        case STUB_WCTYPE: case STUB_TOWUPPER: case STUB_TOWLOWER:
        case STUB_ISWCTYPE: case STUB_WCTOB: case STUB_BTOWC:
            cpu->r[0] = 0; break;

        /* ==================================================================
         *  Bionic / Android internals
         * ================================================================== */
        case STUB___ERRNO:
            /* Return pointer to an errno location in emulated memory.
             * We reserve a small area at DATA_SYMBOL_BASE for this. */
            cpu->r[0] = DATA_SYMBOL_BASE; break;
        case STUB___STACK_CHK_FAIL:
            fprintf(stderr, "android: __stack_chk_fail() - stack corruption detected\n");
            emu->cpu.running = false;
            emu->exit_code = 134;
            break;
        case STUB___CXA_ATEXIT:
        case STUB___CXA_FINALIZE:
            cpu->r[0] = 0; break;
        case STUB___ANDROID_LOG_PRINT: {
            /* __android_log_print(priority, tag, fmt, ...) */
            uint32_t tag = cpu->r[1], fmt = cpu->r[2];
            if (tag < emu->mem_size && fmt < emu->mem_size) {
#if defined(_MSC_VER)
                char buf[512];
                snprintf(buf, sizeof(buf), "[%s] %s\n", EMU_STR(tag), EMU_STR(fmt));
                OutputDebugStringA(buf);
#else
                fprintf(stderr, "[%s] %s\n", EMU_STR(tag), EMU_STR(fmt));
#endif
            }
            cpu->r[0] = 0; break;
        }
        case STUB_SYSCALL:
            /* libc syscall() wrapper: syscall number in r0, args shifted */
            cpu->r[7] = cpu->r[0]; /* move syscall nr to r7 */
            cpu->r[0] = cpu->r[1]; cpu->r[1] = cpu->r[2];
            cpu->r[2] = cpu->r[3]; /* shift args */
            android_syscall_dispatch(cpu, &emu->syscall_state, 0);
            break;

        /* ==================================================================
         *  OpenGL ES 1.x → forwarded through GL ES 2.0 emulation layer
         * ================================================================== */
        case STUB_GLENABLE:
            gles1_enable(emu->gles1, cpu->r[0]); break;
        case STUB_GLDISABLE:
            gles1_disable(emu->gles1, cpu->r[0]); break;
        case STUB_GLCLEAR:
            gles1_clear(emu->gles1, cpu->r[0]); break;
        case STUB_GLCLEARCOLOR:
            gles1_clearColor(emu->gles1, reg_to_float(cpu->r[0]), reg_to_float(cpu->r[1]),
                             reg_to_float(cpu->r[2]), reg_to_float(cpu->r[3])); break;
        case STUB_GLVIEWPORT:
            gles1_viewport(emu->gles1, (int)cpu->r[0], (int)cpu->r[1],
                           (int)cpu->r[2], (int)cpu->r[3]); break;
        case STUB_GLSCISSOR:
            gles1_scissor(emu->gles1, (int)cpu->r[0], (int)cpu->r[1],
                          (int)cpu->r[2], (int)cpu->r[3]); break;
        case STUB_GLMATRIXMODE:
            gles1_matrixMode(emu->gles1, cpu->r[0]); break;
        case STUB_GLLOADIDENTITY:
            gles1_loadIdentity(emu->gles1); break;
        case STUB_GLPUSHMATRIX:
            gles1_pushMatrix(emu->gles1); break;
        case STUB_GLPOPMATRIX:
            gles1_popMatrix(emu->gles1); break;
        case STUB_GLTRANSLATEF:
            gles1_translatef(emu->gles1, reg_to_float(cpu->r[0]),
                             reg_to_float(cpu->r[1]), reg_to_float(cpu->r[2])); break;
        case STUB_GLSCALEF:
            gles1_scalef(emu->gles1, reg_to_float(cpu->r[0]),
                         reg_to_float(cpu->r[1]), reg_to_float(cpu->r[2])); break;
        case STUB_GLROTATEF:
            gles1_rotatef(emu->gles1, reg_to_float(cpu->r[0]),
                          reg_to_float(cpu->r[1]), reg_to_float(cpu->r[2]),
                          reg_to_float(cpu->r[3])); break;
        case STUB_GLORTHOF:
            gles1_orthof(emu->gles1,
                         reg_to_float(cpu->r[0]), reg_to_float(cpu->r[1]),
                         reg_to_float(cpu->r[2]), reg_to_float(cpu->r[3]),
                         stack_float(emu->mem, emu->mem_size, cpu->r[13], 0),
                         stack_float(emu->mem, emu->mem_size, cpu->r[13], 1)); break;
        case STUB_GLMULTMATRIXF:
            if (cpu->r[0] && cpu->r[0] + 64 <= emu->mem_size) {
                gles1_multMatrixf(emu->gles1, (const float *)(emu->mem + cpu->r[0]));
            }
            break;
        case STUB_GLCOLOR4F:
            gles1_color4f(emu->gles1, reg_to_float(cpu->r[0]), reg_to_float(cpu->r[1]),
                          reg_to_float(cpu->r[2]), reg_to_float(cpu->r[3])); break;
        case STUB_GLBLENDFUNC:
            gles1_blendFunc(emu->gles1, cpu->r[0], cpu->r[1]); break;
        case STUB_GLDEPTHFUNC:
            gles1_depthFunc(emu->gles1, cpu->r[0]); break;
        case STUB_GLDEPTHMASK:
            gles1_depthMask(emu->gles1, (uint8_t)cpu->r[0]); break;
        case STUB_GLDEPTHRANGEF:
            gles1_depthRangef(emu->gles1, reg_to_float(cpu->r[0]), reg_to_float(cpu->r[1])); break;
        case STUB_GLALPHAFUNC:
            gles1_alphaFunc(emu->gles1, cpu->r[0], reg_to_float(cpu->r[1])); break;
        case STUB_GLCULLFACE:
            gles1_cullFace(emu->gles1, cpu->r[0]); break;
        case STUB_GLSHADEMODEL:
            gles1_shadeModel(emu->gles1, cpu->r[0]); break;
        case STUB_GLENABLECLIENTSTATE:
            gles1_enableClientState(emu->gles1, cpu->r[0]); break;
        case STUB_GLDISABLECLIENTSTATE:
            gles1_disableClientState(emu->gles1, cpu->r[0]); break;
        case STUB_GLHINT:
            gles1_hint(emu->gles1, cpu->r[0], cpu->r[1]); break;
        case STUB_GLSTENCILFUNC:
            gles1_stencilFunc(emu->gles1, cpu->r[0], (int)cpu->r[1], cpu->r[2]); break;
        case STUB_GLSTENCILMASK:
            gles1_stencilMask(emu->gles1, cpu->r[0]); break;
        case STUB_GLSTENCILOP:
            gles1_stencilOp(emu->gles1, cpu->r[0], cpu->r[1], cpu->r[2]); break;
        case STUB_GLLIGHTMODELF:
            gles1_lightModelf(emu->gles1, cpu->r[0], reg_to_float(cpu->r[1])); break;
        case STUB_GLLIGHTFV:
            if (cpu->r[2] && cpu->r[2] + 16 <= emu->mem_size) {
                gles1_lightfv(emu->gles1, cpu->r[0], cpu->r[1],
                              (const float *)(emu->mem + cpu->r[2]));
            }
            break;
        case STUB_GLPOLYGONOFFSET:
            gles1_polygonOffset(emu->gles1, reg_to_float(cpu->r[0]), reg_to_float(cpu->r[1])); break;
        case STUB_GLLINEWIDTH:
            gles1_lineWidth(emu->gles1, reg_to_float(cpu->r[0])); break;
        case STUB_GLCOLORMASK:
            gles1_colorMask(emu->gles1, (uint8_t)cpu->r[0], (uint8_t)cpu->r[1],
                            (uint8_t)cpu->r[2], (uint8_t)cpu->r[3]); break;
        case STUB_GLFOGF:
            gles1_fogf(emu->gles1, cpu->r[0], reg_to_float(cpu->r[1])); break;
        case STUB_GLFOGFV:
            if (cpu->r[1] && cpu->r[1] + 16 <= emu->mem_size) {
                gles1_fogfv(emu->gles1, cpu->r[0], (const float *)(emu->mem + cpu->r[1]));
            }
            break;
        case STUB_GLFOGX:
            gles1_fogx(emu->gles1, cpu->r[0], (int32_t)cpu->r[1]); break;

        /* Textures */
        case STUB_GLBINDTEXTURE:
            gles1_bindTexture(emu->gles1, cpu->r[0], cpu->r[1]); break;
        case STUB_GLGENTEXTURES:
            gles1_genTextures(emu->gles1, (int)cpu->r[0], cpu->r[1]); break;
        case STUB_GLDELETETEXTURES:
            gles1_deleteTextures(emu->gles1, (int)cpu->r[0], cpu->r[1]); break;
        case STUB_GLTEXIMAGE2D: {
            uint32_t sp = cpu->r[13];
            gles1_texImage2D(emu->gles1, cpu->r[0], (int)cpu->r[1], (int)cpu->r[2],
                             (int)cpu->r[3],
                             (int)stack_u32(emu->mem, emu->mem_size, sp, 0),  /* height */
                             (int)stack_u32(emu->mem, emu->mem_size, sp, 1),  /* border */
                             stack_u32(emu->mem, emu->mem_size, sp, 2),       /* format */
                             stack_u32(emu->mem, emu->mem_size, sp, 3),       /* type */
                             stack_u32(emu->mem, emu->mem_size, sp, 4));      /* pixels */
            break;
        }
        case STUB_GLTEXSUBIMAGE2D: {
            uint32_t sp = cpu->r[13];
            gles1_texSubImage2D(emu->gles1, cpu->r[0], (int)cpu->r[1],
                                (int)cpu->r[2], (int)cpu->r[3],
                                (int)stack_u32(emu->mem, emu->mem_size, sp, 0),  /* w */
                                (int)stack_u32(emu->mem, emu->mem_size, sp, 1),  /* h */
                                stack_u32(emu->mem, emu->mem_size, sp, 2),       /* fmt */
                                stack_u32(emu->mem, emu->mem_size, sp, 3),       /* type */
                                stack_u32(emu->mem, emu->mem_size, sp, 4));      /* pixels */
            break;
        }
        case STUB_GLTEXPARAMETERI:
            gles1_texParameteri(emu->gles1, cpu->r[0], cpu->r[1], (int)cpu->r[2]); break;

        /* Vertex arrays */
        case STUB_GLVERTEXPOINTER:
            gles1_vertexPointer(emu->gles1, (int)cpu->r[0], cpu->r[1],
                                (int)cpu->r[2], cpu->r[3]); break;
        case STUB_GLTEXCOORDPOINTER:
            gles1_texCoordPointer(emu->gles1, (int)cpu->r[0], cpu->r[1],
                                  (int)cpu->r[2], cpu->r[3]); break;
        case STUB_GLCOLORPOINTER:
            gles1_colorPointer(emu->gles1, (int)cpu->r[0], cpu->r[1],
                               (int)cpu->r[2], cpu->r[3]); break;
        case STUB_GLNORMALPOINTER:
            gles1_normalPointer(emu->gles1, cpu->r[0], (int)cpu->r[1], cpu->r[2]); break;

        /* VBO */
        case STUB_GLBINDBUFFER:
            gles1_bindBuffer(emu->gles1, cpu->r[0], cpu->r[1]); break;
        case STUB_GLGENBUFFERS:
            gles1_genBuffers(emu->gles1, (int)cpu->r[0], cpu->r[1]); break;
        case STUB_GLDELETEBUFFERS:
            gles1_deleteBuffers(emu->gles1, (int)cpu->r[0], cpu->r[1]); break;
        case STUB_GLBUFFERDATA:
            gles1_bufferData(emu->gles1, cpu->r[0], (int)cpu->r[1], cpu->r[2], cpu->r[3]); break;

        /* Draw calls */
        case STUB_GLDRAWARRAYS:
            gles1_drawArrays(emu->gles1, cpu->r[0], (int)cpu->r[1], (int)cpu->r[2]); break;
        case STUB_GLDRAWELEMENTS:
            gles1_drawElements(emu->gles1, cpu->r[0], (int)cpu->r[1], cpu->r[2], cpu->r[3]); break;

        /* Queries */
        case STUB_GLGETERROR:
            cpu->r[0] = gles1_getError(emu->gles1); break;
        case STUB_GLGETSTRING: {
            uint32_t guest_ptr = 0;
            gles1_getString(emu->gles1, cpu->r[0], emu->mem, emu->mem_size, &guest_ptr);
            cpu->r[0] = guest_ptr;
            break;
        }
        case STUB_GLGETFLOATV:
            gles1_getFloatv(emu->gles1, cpu->r[0], cpu->r[1]); break;
        case STUB_GLREADPIXELS: {
            uint32_t sp = cpu->r[13];
            gles1_readPixels(emu->gles1, (int)cpu->r[0], (int)cpu->r[1],
                             (int)cpu->r[2], (int)cpu->r[3],
                             stack_u32(emu->mem, emu->mem_size, sp, 0),
                             stack_u32(emu->mem, emu->mem_size, sp, 1),
                             stack_u32(emu->mem, emu->mem_size, sp, 2));
            break;
        }

        /* ==================================================================
         *  EGL (stub implementations — guest uses our host GL context)
         * ================================================================== */
        case STUB_EGLGETDISPLAY:
            EMU_LOG_DEBUG("eglGetDisplay called");
            cpu->r[0] = 0xC00FA15Eu; break; /* fake display handle */
        case STUB_EGLINITIALIZE:
            EMU_LOG_DEBUG("eglInitialize called");
            cpu->r[0] = 1; break; /* EGL_TRUE */
        case STUB_EGLCHOOSECONFIG:
            /* Write num_config = 1 */
            if (cpu->r[4] && cpu->r[4] + 4 <= emu->mem_size) {
                uint32_t one = 1; memcpy(emu->mem + cpu->r[4], &one, 4);
            }
            cpu->r[0] = 1; break;
        case STUB_EGLGETCONFIGATTRIB:
            cpu->r[0] = 1; break;
        case STUB_EGLCREATEWINDOWSURFACE:
            EMU_LOG_DEBUG("eglCreateWindowSurface called");
            cpu->r[0] = 0xCAFEBABEu; break; /* fake surface */
        case STUB_EGLCREATECONTEXT:
            EMU_LOG_DEBUG("eglCreateContext called");
            cpu->r[0] = 0xF00DFACEu; break; /* fake context */
        case STUB_EGLMAKECURRENT:
            EMU_LOG_DEBUG("eglMakeCurrent called");
            cpu->r[0] = 1; break;
        case STUB_EGLQUERYSURFACE:
            cpu->r[0] = 1; break;
        case STUB_EGLSWAPBUFFERS:
            /* Signal the host main loop that a frame is ready */
            emu->frame_ready = true;
            EMU_LOG_DEBUG("eglSwapBuffers called — frame_ready=true");
            cpu->r[0] = 1; break;
        case STUB_EGLSWAPINTERVAL:
            cpu->r[0] = 1; break;
        case STUB_EGLGETCURRENTDISPLAY:
            cpu->r[0] = 0xC00FA15Eu; break;
        case STUB_EGLDESTROYCONTEXT:
        case STUB_EGLDESTROYSURFACE:
        case STUB_EGLTERMINATE:
            cpu->r[0] = 1; break;

        /* ==================================================================
         *  Android NDK (stub implementations)
         * ================================================================== */
        case STUB_AASSETMANAGER_OPEN:
        case STUB_AASSET_GETLENGTH:
        case STUB_AASSET_GETBUFFER:
        case STUB_AASSET_CLOSE:
            cpu->r[0] = 0; break;
        case STUB_AINPUTEVENT_GETTYPE:
        case STUB_AKEYEVENT_GETACTION:
        case STUB_AKEYEVENT_GETKEYCODE:
        case STUB_AKEYEVENT_GETMETASTATE:
        case STUB_AINPUTEVENT_GETDEVICEID:
        case STUB_AKEYEVENT_GETREPEATCOUNT:
        case STUB_AINPUTEVENT_GETSOURCE:
        case STUB_AMOTIONEVENT_GETACTION:
        case STUB_AMOTIONEVENT_GETPOINTERID:
        case STUB_AMOTIONEVENT_GETX:
        case STUB_AMOTIONEVENT_GETY:
        case STUB_AMOTIONEVENT_GETPOINTERCOUNT:
            cpu->r[0] = 0; break;
        case STUB_AINPUTQUEUE_GETEVENT:
            cpu->r[0] = (uint32_t)-1; break; /* no event */
        case STUB_AINPUTQUEUE_FINISHEVENT:
        case STUB_AINPUTQUEUE_PREDISPATCHEVENT:
        case STUB_AINPUTQUEUE_ATTACHLOOPER:
        case STUB_AINPUTQUEUE_DETACHLOOPER:
            cpu->r[0] = 0; break;
        case STUB_ALOOPER_POLLALL:
            cpu->r[0] = (uint32_t)-1; break; /* ALOOPER_POLL_TIMEOUT (no events) */
        case STUB_ALOOPER_PREPARE:
            cpu->r[0] = 0xA100BE50u; break; /* fake looper handle */
        case STUB_ALOOPER_ADDFD:
            cpu->r[0] = 1; break;
        case STUB_ANATIVEACTIVITY_FINISH:
            emu->cpu.running = false;
            emu->exit_code = 0;
            break;
        case STUB_ANATIVEWINDOW_SETBUFFERSGEOMETRY:
            cpu->r[0] = 0; break;
        case STUB_ACONFIGURATION_NEW:
            /* Allocate a small block for the config struct */
            {
                uint32_t base = emu->syscall_state.brk_current;
                uint32_t sz = 64;
                if (base + sz <= emu->syscall_state.brk_max) {
                    memset(emu->mem + base, 0, sz);
                    emu->syscall_state.brk_current += sz;
                    cpu->r[0] = base;
                } else { cpu->r[0] = 0; }
            }
            break;
        case STUB_ACONFIGURATION_FROMASSETMANAGER:
            cpu->r[0] = 0; break;
        case STUB_ACONFIGURATION_GETLANGUAGE:
            /* Write "en" to the buffer in r1 */
            if (cpu->r[1] + 3 <= emu->mem_size) {
                emu->mem[cpu->r[1]] = 'e';
                emu->mem[cpu->r[1]+1] = 'n';
                emu->mem[cpu->r[1]+2] = '\0';
            }
            cpu->r[0] = 0; break;
        case STUB_ACONFIGURATION_GETCOUNTRY:
            if (cpu->r[1] + 3 <= emu->mem_size) {
                emu->mem[cpu->r[1]] = 'U';
                emu->mem[cpu->r[1]+1] = 'S';
                emu->mem[cpu->r[1]+2] = '\0';
            }
            cpu->r[0] = 0; break;
        case STUB_ACONFIGURATION_DELETE:
            cpu->r[0] = 0; break;

        /* ==================================================================
         *  OpenSL ES (audio via SDL2)
         *
         *  OpenSL ES uses a COM-like interface-ID driven object model.
         *  Only slCreateEngine is directly linked by name; all subsequent
         *  calls go through vtable function pointers in emulated memory.
         *
         *  Current implementation:
         *    - slCreateEngine: creates SDL2 audio backend, returns success
         *    - Remaining stubs are registered but won't be called until
         *      vtable routing is implemented (future enhancement).
         *    - Audio infrastructure (android_audio.h) is ready for when
         *      vtable-based Enqueue/SetPlayState calls are routed through.
         *
         *  TODO: Write fake SLObjectItf/SLEngineItf vtables into emulated
         *  memory so that (*obj)->Method() calls jump to our SVC trampolines.
         * ================================================================== */
        case STUB_SLCREATEENGINE: {
            EMU_LOG_DEBUG("slCreateEngine called (r0=objPtr=0x%08X)", cpu->r[0]);
            /* Write a fake engine object handle to *pEngine (r0) */
            if (cpu->r[0] && cpu->r[0] + 4 <= emu->mem_size) {
                uint32_t fake_engine = 0xA0D10001u;
                memcpy(emu->mem + cpu->r[0], &fake_engine, 4);
            }
            /* Create the audio context if not already done */
            if (!emu->audio) {
                AndroidAudioConfig acfg = {};
                acfg.sample_rate = 44100;
                acfg.channels = 2;
                acfg.bits_per_sample = 16;
                acfg.buffer_size = 4096;
                emu->audio = android_audio_create(&acfg);
                if (emu->audio) {
                    EMU_LOG_INFO("OpenSL ES: audio engine created (SDL2 backend)");
                } else {
                    EMU_LOG_ERR("OpenSL ES: failed to create audio engine");
                }
            }
            cpu->r[0] = 0; /* SL_RESULT_SUCCESS */
            break;
        }
        case STUB_SL_ENGINE_CREATEOUTPUTMIX: {
            EMU_LOG_DEBUG("Engine::CreateOutputMix called");
            /* Write fake output mix handle */
            if (cpu->r[1] && cpu->r[1] + 4 <= emu->mem_size) {
                uint32_t fake_mix = 0xA0D10002u;
                memcpy(emu->mem + cpu->r[1], &fake_mix, 4);
            }
            cpu->r[0] = 0; break;
        }
        case STUB_SL_ENGINE_CREATEAUDIOPLAYER: {
            EMU_LOG_DEBUG("Engine::CreateAudioPlayer called");
            /* Write fake player handle */
            if (cpu->r[1] && cpu->r[1] + 4 <= emu->mem_size) {
                uint32_t fake_player = 0xA0D10003u;
                memcpy(emu->mem + cpu->r[1], &fake_player, 4);
            }
            cpu->r[0] = 0; break;
        }
        case STUB_SL_ENGINE_GETINTERFACE:
            EMU_LOG_DEBUG("Engine::GetInterface called");
            /* Write a fake interface pointer */
            if (cpu->r[2] && cpu->r[2] + 4 <= emu->mem_size) {
                uint32_t fake_iface = 0xA0D10010u;
                memcpy(emu->mem + cpu->r[2], &fake_iface, 4);
            }
            cpu->r[0] = 0; break;
        case STUB_SL_OUTPUTMIX_REALIZE:
            EMU_LOG_DEBUG("OutputMix::Realize called");
            cpu->r[0] = 0; break;
        case STUB_SL_PLAYER_REALIZE:
            EMU_LOG_DEBUG("Player::Realize called");
            cpu->r[0] = 0; break;
        case STUB_SL_PLAYER_GETINTERFACE:
            EMU_LOG_DEBUG("Player::GetInterface called (iid_ptr=0x%08X, out=0x%08X)", cpu->r[1], cpu->r[2]);
            if (cpu->r[2] && cpu->r[2] + 4 <= emu->mem_size) {
                uint32_t fake_iface = 0xA0D10020u;
                memcpy(emu->mem + cpu->r[2], &fake_iface, 4);
            }
            cpu->r[0] = 0; break;
        case STUB_SL_PLAYER_SETPLAYSTATE: {
            uint32_t state = cpu->r[1];
            EMU_LOG_DEBUG("Player::SetPlayState(%u)", state);
            if (emu->audio) {
                if (state == 1 /* SL_PLAYSTATE_STOPPED */) {
                    android_audio_pause(emu->audio);
                } else if (state == 2 /* SL_PLAYSTATE_PAUSED */) {
                    android_audio_pause(emu->audio);
                } else if (state == 3 /* SL_PLAYSTATE_PLAYING */) {
                    android_audio_play(emu->audio);
                }
            }
            cpu->r[0] = 0; break;
        }
        case STUB_SL_BUFFERQUEUE_ENQUEUE: {
            uint32_t data_va = cpu->r[1];
            uint32_t data_sz = cpu->r[2];
            if (emu->audio && data_va && data_sz && BOUNDS_OK(data_va, data_sz)) {
                android_audio_enqueue(emu->audio, emu->mem + data_va, data_sz);
            }
            cpu->r[0] = 0; break;
        }
        case STUB_SL_BUFFERQUEUE_REGISTERCALLBACK:
            EMU_LOG_DEBUG("BufferQueue::RegisterCallback(cb=0x%08X, ctx=0x%08X)", cpu->r[1], cpu->r[2]);
            /* We don't call back into guest code for buffer completion yet.
             * A full implementation would store the callback VA and invoke it
             * by setting PC when a buffer finishes playing. */
            cpu->r[0] = 0; break;
        case STUB_SL_BUFFERQUEUE_CLEAR:
            EMU_LOG_DEBUG("BufferQueue::Clear called");
            cpu->r[0] = 0; break;
        case STUB_SL_VOLUME_SETVOLUME:
            EMU_LOG_DEBUG("Volume::SetVolumeLevel(%d)", (int32_t)cpu->r[1]);
            cpu->r[0] = 0; break;
        case STUB_SL_OBJECT_DESTROY:
            EMU_LOG_DEBUG("Object::Destroy(0x%08X)", cpu->r[0]);
            cpu->r[0] = 0; break;

        /* ==================================================================
         *  Default
         * ================================================================== */
        default:
            EMU_LOG_ERR("unknown bionic stub %u (%s)",
                    stub_id, (stub_id < STUB_COUNT && stub_names[stub_id]) ? stub_names[stub_id] : "?");
            cpu->r[0] = 0; break;
    }
#undef EMU_STR
#undef EMU_PTR
#undef BOUNDS_OK
}

/* Combined SWI dispatcher: bionic stubs have IDs < STUB_COUNT */
static void combined_swi_handler(void *ctx, uint32_t swi_num) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    if (swi_num > 0 && swi_num < STUB_COUNT) {
        bionic_stub_dispatch(emu, swi_num);
    } else {
#ifdef _DEBUG
        static unsigned syscall_count = 0;
        syscall_count++;
        if (syscall_count <= 20 || (syscall_count % 10000) == 0) {
            uint32_t r7 = emu->cpu.r[7]; /* ARM EABI: syscall # in r7 */
            EMU_LOG_DEBUG("syscall dispatch: swi=%u r7=%u (call #%u, pc=0x%08X sp=0x%08X lr=0x%08X)",
                          swi_num, r7, syscall_count,
                          emu->cpu.r[15], emu->cpu.r[13], emu->cpu.r[ARM_LR]);
        }
#endif
        android_syscall_dispatch(&emu->cpu, &emu->syscall_state, swi_num);
    }
}

/* AArch64 combined SVC dispatcher */
static void combined_svc_handler_64(void *ctx, uint32_t svc_num) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;

    if (svc_num > 0 && svc_num < STUB_COUNT) {
        /* AArch64 bionic stub: mirror registers to the ARMv7 state for
         * the existing bionic_stub_dispatch, then copy back.
         * AArch64 calling convention puts args in X0-X7. */
        emu->cpu.r[0] = (uint32_t)emu->cpu64.x[0];
        emu->cpu.r[1] = (uint32_t)emu->cpu64.x[1];
        emu->cpu.r[2] = (uint32_t)emu->cpu64.x[2];
        emu->cpu.r[3] = (uint32_t)emu->cpu64.x[3];

        bionic_stub_dispatch(emu, svc_num);

        /* Copy result back to X0 */
        emu->cpu64.x[0] = (uint64_t)emu->cpu.r[0];
        /* Propagate halted state */
        if (!emu->cpu.running)
            emu->cpu64.running = false;
    } else {
        /* Real Linux syscall: AArch64 Linux uses X8 as syscall number */
        /* For now, log and stop */
        fprintf(stderr, "android_emulator: AArch64 syscall SVC #%u (x8=%llu) not yet implemented\n",
                svc_num, (unsigned long long)emu->cpu64.x[8]);
        emu->cpu64.running = false;
    }
}

/* -------------------------------------------------------------------------
 * android_emulator_init
 * ---------------------------------------------------------------------- */

bool android_emulator_init(AndroidEmulator *emu, const AndroidEmulatorConfig *config) {
    memset(emu, 0, sizeof(*emu));
    emu->config = *config;

    EMU_LOG_DEBUG("init: starting (apk=%s, screen=%dx%d, verbosity=%d)",
                  config->apk_path ? config->apk_path : "(null)",
                  config->screen_width, config->screen_height, config->verbosity);

    /* Allocate emulated address space */
    emu->mem_size = ANDROID_MEM_SIZE;
    emu->mem = (uint8_t*)calloc(1, emu->mem_size);
    if (!emu->mem) {
        EMU_LOG_ERR("failed to allocate %u MB of emulated memory", ANDROID_MEM_SIZE / (1024*1024));
        EMU_LOG_DEBUG("init: FAILED — memory allocation (%u MB)", ANDROID_MEM_SIZE / (1024*1024));
        return false;
    }
    EMU_LOG_DEBUG("init: allocated %u MB emulated memory at %p",
                  ANDROID_MEM_SIZE / (1024*1024), (void*)emu->mem);

    /* Initialise subsystems */
    android_syscall_init(&emu->syscall_state, emu->mem, emu->mem_size);
    android_jni_init(&emu->jni);
    android_linker_init(&emu->linker, emu->mem, emu->mem_size, ANDROID_LOAD_BASE);
    EMU_LOG_DEBUG("init: subsystems initialised (syscall, jni, linker)");

    /* Load the APK — must happen BEFORE ABI detection and stub installation */
    EMU_LOG_DEBUG("init: opening APK '%s'", config->apk_path ? config->apk_path : "(null)");
    if (!apk_open(config->apk_path, &emu->apk)) {
        EMU_LOG_ERR("failed to open APK: %s", config->apk_path ? config->apk_path : "(null)");
        EMU_LOG_DEBUG("init: FAILED — apk_open returned false");
        return false;
    }
    EMU_LOG_DEBUG("init: APK opened (abi='%s', libs=%u, pkg='%s')",
                  emu->apk.target_abi, emu->apk.lib_count, emu->apk.package_name);

    if (config->verbosity >= 1)
        EMU_LOG_INFO("loaded APK '%s' (%s), %u native lib(s)",
                config->apk_path, emu->apk.target_abi, emu->apk.lib_count);

    /* Detect ABI: arm64-v8a -> AArch64, everything else -> ARMv7 */
    emu->is_arm64 = (strcmp(emu->apk.target_abi, "arm64-v8a") == 0);
    EMU_LOG_DEBUG("init: ABI='%s' -> %s mode", emu->apk.target_abi,
                  emu->is_arm64 ? "AArch64" : "ARMv7");

    /* Install bionic stubs (encoding depends on target ABI) */
    for (unsigned i = 1; i < STUB_COUNT; i++) {
        uint32_t stub_va = BIONIC_STUB_BASE + i * BIONIC_STUB_STRIDE;
        if (emu->is_arm64) {
            install_stub_a64(emu->mem, stub_va, i);
            android_linker_add_override(&emu->linker, stub_names[i], stub_va);
        } else {
            install_stub(emu->mem, stub_va, i);
            /* ARM-mode stubs: do NOT set bit 0 (Thumb flag).
             * BX/BLX from Thumb code will switch to ARM mode to execute
             * the SWI, then BX LR switches back to the caller's mode. */
            android_linker_add_override(&emu->linker, stub_names[i], stub_va);
        }
    }
    EMU_LOG_DEBUG("init: installed %u bionic stubs (%s encoding)",
                  STUB_COUNT - 1, emu->is_arm64 ? "A64" : "ARM SWI");

    /* Install data symbols: these are not trampolines but reserved memory
     * locations that native code reads as global variables. */
    {
        uint32_t dbase = DATA_SYMBOL_BASE;

        /* __stack_chk_guard: stack canary value (4 bytes) */
        uint32_t canary = 0x00000FFL; /* non-zero canary */
        memcpy(emu->mem + dbase, &canary, 4);
        android_linker_add_override(&emu->linker, "__stack_chk_guard", dbase);
        dbase += DATA_SYMBOL_STRIDE;

        /* _tolower_tab_: 256+1 byte array for tolower */
        for (int i = 0; i <= 256; i++) {
            int c = (i < 256) ? i : 0;
            emu->mem[dbase + i] = (uint8_t)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        }
        android_linker_add_override(&emu->linker, "_tolower_tab_", dbase);
        dbase += 512;

        /* _ctype_: 256+1 byte ctype classification table */
        memset(emu->mem + dbase, 0, 257);
        for (int i = 0; i < 256; i++) {
            uint8_t flags = 0;
            if (i >= 'A' && i <= 'Z') flags |= 0x01; /* _U */
            if (i >= 'a' && i <= 'z') flags |= 0x02; /* _L */
            if (i >= '0' && i <= '9') flags |= 0x04; /* _N */
            if (i == ' ' || i == '\t' || i == '\n' || i == '\r' || i == '\f' || i == '\v') flags |= 0x08; /* _S */
            if (i >= 0x20 && i <= 0x7E) flags |= 0x10; /* _P (printable) */
            if (i < 0x20 || i == 0x7F) flags |= 0x20; /* _C (control) */
            emu->mem[dbase + 1 + i] = flags; /* offset by 1 for EOF=-1 indexing */
        }
        android_linker_add_override(&emu->linker, "_ctype_", dbase);
        dbase += 512;

        /* __sF: stdio FILE structures (3 entries: stdin, stdout, stderr)
         * Each Android FILE is 0x54 bytes (NDK struct). Zero them out. */
        memset(emu->mem + dbase, 0, 0x54 * 3);
        android_linker_add_override(&emu->linker, "__sF", dbase);
        dbase += 0x54 * 3 + 16;

        /* __gnu_Unwind_Find_exidx: ARM exception table lookup.
         * We install a stub that returns 0 (no exception tables). */
        {
            uint32_t stub_va = BIONIC_STUB_BASE + STUB_COUNT * BIONIC_STUB_STRIDE;
            if (emu->is_arm64) {
                /* AArch64: MOV X0, #0; RET */
                uint32_t mov_x0_0 = 0xD2800000u; /* MOV X0, #0 */
                uint32_t ret      = 0xD65F03C0u;  /* RET */
                memcpy(emu->mem + stub_va,     &mov_x0_0, 4);
                memcpy(emu->mem + stub_va + 4, &ret,      4);
                android_linker_add_override(&emu->linker, "__gnu_Unwind_Find_exidx", stub_va);
            } else {
                /* ARM mode: MOV R0, #0; BX LR */
                uint32_t mov_r0_0 = 0xE3A00000u; /* MOV R0, #0 */
                uint32_t bx_lr    = 0xE12FFF1Eu;  /* BX LR */
                memcpy(emu->mem + stub_va,     &mov_r0_0, 4);
                memcpy(emu->mem + stub_va + 4, &bx_lr,    4);
                android_linker_add_override(&emu->linker, "__gnu_Unwind_Find_exidx", stub_va);
            }
        }

        /* SL_IID_* OpenSL ES interface IDs (16-byte UUIDs each, all zeros = stub) */
        memset(emu->mem + dbase, 0, 16 * 4);
        android_linker_add_override(&emu->linker, "SL_IID_ENGINE",      dbase);
        android_linker_add_override(&emu->linker, "SL_IID_PLAY",        dbase + 16);
        android_linker_add_override(&emu->linker, "SL_IID_VOLUME",      dbase + 32);
        android_linker_add_override(&emu->linker, "SL_IID_BUFFERQUEUE", dbase + 48);
        dbase += 16 * 4;
    }
    EMU_LOG_DEBUG("init: installed data symbols");

    /* Load all native libraries into emulated memory */
    unsigned loaded_count = 0;
    for (unsigned i = 0; i < emu->apk.lib_count; i++) {
        const ApkLibEntry *lib = &emu->apk.libs[i];
        EMU_LOG_DEBUG("init: loading lib[%u] '%s' (%zu bytes)", i, lib->name, lib->size);
        int idx = android_linker_load(&emu->linker, lib->name, lib->data, lib->size);
        if (idx < 0) {
            EMU_LOG_ERR("failed to load %s", lib->name);
            EMU_LOG_DEBUG("init: FAILED to load '%s'", lib->name);
        } else {
            loaded_count++;
            if (config->verbosity >= 2) {
                EMU_LOG_INFO("loaded %s at VA 0x%08X", lib->name, emu->linker.libs[idx].load_base);
            }
            EMU_LOG_DEBUG("init: loaded '%s' at VA 0x%08X (idx=%d)",
                          lib->name, emu->linker.libs[idx].load_base, idx);
        }
    }
    EMU_LOG_DEBUG("init: loaded %u/%u native libraries", loaded_count, emu->apk.lib_count);

    if (loaded_count == 0 && emu->apk.lib_count > 0) {
        EMU_LOG_ERR("all %u native libraries failed to load", emu->apk.lib_count);
        EMU_LOG_DEBUG("init: FAILED — zero libraries loaded");
        return false;
    }

    /* Resolve relocations */
    EMU_LOG_DEBUG("init: starting relocations for %u libraries", emu->linker.lib_count);
    if (!android_linker_relocate_all(&emu->linker)) {
        EMU_LOG_ERR("relocation failed");
        EMU_LOG_DEBUG("init: FAILED at relocation");
        return false;
    }
    EMU_LOG_DEBUG("init: relocations complete");

    /* Initialise CPU based on detected ABI */
    EMU_LOG_DEBUG("init: initialising %s CPU", emu->is_arm64 ? "AArch64" : "ARMv7");
    if (emu->is_arm64) {
        aarch64_init(&emu->cpu64);
        emu->cpu64.callback_ctx     = emu;
        emu->cpu64.mem_read8        = mem64_r8;
        emu->cpu64.mem_read16       = mem64_r16;
        emu->cpu64.mem_read32       = mem64_r32;
        emu->cpu64.mem_read64       = mem64_r64;
        emu->cpu64.mem_write8       = mem64_w8;
        emu->cpu64.mem_write16      = mem64_w16;
        emu->cpu64.mem_write32      = mem64_w32;
        emu->cpu64.mem_write64      = mem64_w64;
        emu->cpu64.syscall_handler  = combined_svc_handler_64;
        emu->cpu64.undef_handler    = undef_handler_64;

        /* Also init ARMv7 state (used by bionic_stub_dispatch bridge) */
        armv7_init(&emu->cpu);
    } else {
        armv7_init(&emu->cpu);
        emu->cpu.callback_ctx    = emu;
        emu->cpu.mem_read8       = mem_r8;
        emu->cpu.mem_read16      = mem_r16;
        emu->cpu.mem_read32      = mem_r32;
        emu->cpu.mem_write8      = mem_w8;
        emu->cpu.mem_write16     = mem_w16;
        emu->cpu.mem_write32     = mem_w32;
        emu->cpu.syscall_handler = combined_swi_handler;
        emu->cpu.undef_handler   = undef_handler;
    }

    /* Set up stack */
    uint32_t stack_top = ANDROID_STACK_TOP;
    if (emu->is_arm64) {
        emu->cpu64.sp = (uint64_t)stack_top;
    } else {
        emu->cpu.r[ARM_SP] = stack_top;
    }

    /* Locate entry point: prefer "ANativeActivity_onCreate", then "android_main",
     * then "Java_*_nativeInit", then the first library's entry point. */
    uint32_t entry = 0;
    const char *entry_names[] = {
        "ANativeActivity_onCreate",
        "android_main",
        "SDL_main",
        "nativeStart",
        "nativeInit",
        NULL
    };
    for (int ei = 0; entry_names[ei] && !entry; ei++) {
        entry = android_linker_lookup(&emu->linker, entry_names[ei]);
    }
    if (!entry && emu->linker.lib_count > 0) {
        entry = emu->linker.libs[0].entry_va;
    }

    if (!entry) {
        EMU_LOG_ERR("no entry point found");
        EMU_LOG_DEBUG("init: FAILED — no entry point found in any library");
        return false;
    }

    if (config->verbosity >= 1)
        EMU_LOG_INFO("entry point at VA 0x%08X (%s mode)",
                entry, emu->is_arm64 ? "AArch64" : "ARMv7");

    /* Set up fake argc/argv on stack */
    const char fake_argv0[] = "/data/app/com.android.app/base.apk";
    uint32_t argv0_va = stack_top - (uint32_t)sizeof(fake_argv0);
    memcpy(emu->mem + argv0_va, fake_argv0, sizeof(fake_argv0));
    stack_top = argv0_va - 16;
    uint32_t argc = 1, argv_va = stack_top + 4;
    memcpy(emu->mem + stack_top,     &argc,    4);
    memcpy(emu->mem + argv_va,       &argv0_va, 4);
    uint32_t null_ptr = 0;
    memcpy(emu->mem + argv_va + 4,   &null_ptr, 4); /* NULL terminator */

    if (emu->is_arm64) {
        /* AArch64: PC = entry (no Thumb bit), args in X0/X1 */
        emu->cpu64.pc = (uint64_t)entry;
        emu->cpu64.sp = (uint64_t)stack_top;
        emu->cpu64.x[0] = (uint64_t)argc;
        emu->cpu64.x[1] = (uint64_t)argv_va;
        /* Set X30 (LR) to 0 so RET halts */
        emu->cpu64.x[AARCH64_LR] = 0;
    } else {
        /* ARMv7: Set PC; Thumb if bit 0 set */
        if (entry & 1) {
            emu->cpu.cpsr |= ARM_CPSR_T;
            emu->cpu.r[ARM_PC] = entry & ~1u;
        } else {
            emu->cpu.r[ARM_PC] = entry;
        }
        emu->cpu.r[ARM_SP] = stack_top;
        emu->cpu.r[0] = argc;
        emu->cpu.r[1] = argv_va;
    }

    emu->initialised = true;
    emu->running     = true;
    emu->frame_ready = false;

    /* Create GL ES 1.x emulation context (host GL context must be current) */
    emu->gles1 = gles1_create(emu->mem, emu->mem_size);
    if (!emu->gles1) {
        EMU_LOG_ERR("failed to create GLES1 emulation context");
        /* Non-fatal: GL rendering will fail but emulator can still run */
    }

    EMU_LOG_DEBUG("init: SUCCESS — emulator initialised and ready to run");
    return true;
}

/* -------------------------------------------------------------------------
 * android_emulator_run
 * ---------------------------------------------------------------------- */

int android_emulator_run(AndroidEmulator *emu) {
    if (!emu->initialised) return -1;
    if (emu->is_arm64) {
        aarch64_run(&emu->cpu64);
        emu->running   = false;
        emu->exit_code = emu->cpu64.exit_code;
    } else {
        armv7_run(&emu->cpu);
        emu->running   = false;
        emu->exit_code = emu->cpu.exit_code;
    }
    return emu->exit_code;
}

/* -------------------------------------------------------------------------
 * android_emulator_step
 * ---------------------------------------------------------------------- */

bool android_emulator_step(AndroidEmulator *emu, unsigned max_instructions) {
    if (!emu->initialised) return false;
    emu->frame_ready = false;
    if (emu->is_arm64) {
        if (!emu->cpu64.running) return false;
        for (unsigned i = 0; i < max_instructions && emu->cpu64.running && !emu->frame_ready; i++) {
            aarch64_step(&emu->cpu64);
        }
        return emu->cpu64.running;
    } else {
        if (!emu->cpu.running) {
            EMU_LOG_DEBUG("step: cpu not running (pc=0x%08X)", emu->cpu.r[15]);
            return false;
        }
        unsigned i;
        for (i = 0; i < max_instructions && emu->cpu.running && !emu->frame_ready; i++) {
            armv7_step(&emu->cpu);
        }
#ifdef _DEBUG
        static unsigned step_call_count = 0;
        step_call_count++;
        if (step_call_count <= 5 || (step_call_count % 60 == 0 && step_call_count <= 600)) {
            EMU_LOG_DEBUG("step[%u]: executed %u/%u instructions, running=%d frame_ready=%d pc=0x%08X sp=0x%08X",
                          step_call_count, i, max_instructions,
                          emu->cpu.running, emu->frame_ready, emu->cpu.r[15], emu->cpu.r[13]);
        }
#endif
        return emu->cpu.running;
    }
}

/* -------------------------------------------------------------------------
 * android_emulator_touch / key
 * ---------------------------------------------------------------------- */

void android_emulator_touch(AndroidEmulator *emu, int action, int x, int y, int pointer_id) {
    /* Look up the onTouchEvent native handler if registered */
    uint32_t va = android_jni_find_native(&emu->jni, "nativeOnTouch");
    if (!va) return;
    emu->cpu.r[0] = (uint32_t)action;
    emu->cpu.r[1] = (uint32_t)x;
    emu->cpu.r[2] = (uint32_t)y;
    emu->cpu.r[3] = (uint32_t)pointer_id;
    emu->cpu.r[ARM_LR] = 0; /* Return to 0 = halt */
    if (va & 1) { emu->cpu.cpsr |= ARM_CPSR_T; emu->cpu.r[ARM_PC] = va & ~1u; }
    else        { emu->cpu.cpsr &= ~ARM_CPSR_T; emu->cpu.r[ARM_PC] = va; }
    android_emulator_step(emu, 100000);
}

void android_emulator_key(AndroidEmulator *emu, int action, int keycode) {
    uint32_t va = android_jni_find_native(&emu->jni, "nativeOnKey");
    if (!va) return;
    emu->cpu.r[0] = (uint32_t)action;
    emu->cpu.r[1] = (uint32_t)keycode;
    emu->cpu.r[ARM_LR] = 0;
    if (va & 1) { emu->cpu.cpsr |= ARM_CPSR_T; emu->cpu.r[ARM_PC] = va & ~1u; }
    else        { emu->cpu.cpsr &= ~ARM_CPSR_T; emu->cpu.r[ARM_PC] = va; }
    android_emulator_step(emu, 100000);
}

void android_emulator_gamepad(AndroidEmulator *emu,
                              uint32_t button_mask,
                              int16_t left_x,  int16_t left_y,
                              int16_t right_x, int16_t right_y,
                              int16_t left_trigger, int16_t right_trigger) {
    /*
     * Map gamepad state to Android input events.
     *
     * Many native Android games register "nativeOnGamepad" or similar JNI
     * callbacks.  We also translate D-pad and buttons into Android keycodes
     * that the app might read via nativeOnKey.
     *
     * For apps that use the NativeActivity input queue, the events would be
     * pushed into the AInputQueue.  This stub implementation dispatches
     * through the JNI handler we found during linking.
     */

    /* Try the dedicated gamepad handler first */
    uint32_t va = android_jni_find_native(&emu->jni, "nativeOnGamepad");
    if (va) {
        emu->cpu.r[0] = button_mask;
        emu->cpu.r[1] = (uint32_t)(int32_t)left_x;
        emu->cpu.r[2] = (uint32_t)(int32_t)left_y;
        emu->cpu.r[3] = (uint32_t)(int32_t)right_x;
        /* Additional args through the emulated stack if needed */
        emu->cpu.r[ARM_LR] = 0;
        if (va & 1) { emu->cpu.cpsr |= ARM_CPSR_T; emu->cpu.r[ARM_PC] = va & ~1u; }
        else        { emu->cpu.cpsr &= ~ARM_CPSR_T; emu->cpu.r[ARM_PC] = va; }
        android_emulator_step(emu, 100000);
        return;
    }

    /* Fallback: translate D-pad and A/B buttons to Android keycodes via nativeOnKey */
    /* Android keycodes: DPAD_UP=19, DPAD_DOWN=20, DPAD_LEFT=21, DPAD_RIGHT=22,
     * BUTTON_A=96, BUTTON_B=97, BUTTON_X=99, BUTTON_Y=100,
     * BUTTON_L1=102, BUTTON_R1=103, BUTTON_SELECT=109, BUTTON_START=108 */
    static const struct { uint32_t mask; int keycode; } mapping[] = {
        { 1u << 12, 19 },  /* DPAD_UP    */
        { 1u << 13, 20 },  /* DPAD_DOWN  */
        { 1u << 14, 21 },  /* DPAD_LEFT  */
        { 1u << 15, 22 },  /* DPAD_RIGHT */
        { 1u << 0,  96 },  /* BUTTON_A   */
        { 1u << 1,  97 },  /* BUTTON_B   */
        { 1u << 2,  99 },  /* BUTTON_X   */
        { 1u << 3, 100 },  /* BUTTON_Y   */
        { 1u << 4, 102 },  /* BUTTON_L1  */
        { 1u << 5, 103 },  /* BUTTON_R1  */
        { 1u << 8, 109 },  /* SELECT     */
        { 1u << 9, 108 },  /* START      */
    };

    static uint32_t prev_mask = 0;
    for (unsigned i = 0; i < sizeof(mapping)/sizeof(mapping[0]); i++) {
        bool was = (prev_mask & mapping[i].mask) != 0;
        bool now = (button_mask & mapping[i].mask) != 0;
        if (now && !was) android_emulator_key(emu, 0, mapping[i].keycode); /* key down */
        if (!now && was) android_emulator_key(emu, 1, mapping[i].keycode); /* key up   */
    }
    prev_mask = button_mask;

    /* Translate left stick into virtual D-pad if no dedicated handler */
    const int16_t DEADZONE = 8000;
    bool stick_up    = left_y < -DEADZONE;
    bool stick_down  = left_y >  DEADZONE;
    bool stick_left  = left_x < -DEADZONE;
    bool stick_right = left_x >  DEADZONE;

    static bool prev_stick_up = false, prev_stick_down = false;
    static bool prev_stick_left = false, prev_stick_right = false;

    if (stick_up    && !prev_stick_up)    android_emulator_key(emu, 0, 19);
    if (!stick_up   && prev_stick_up)     android_emulator_key(emu, 1, 19);
    if (stick_down  && !prev_stick_down)  android_emulator_key(emu, 0, 20);
    if (!stick_down && prev_stick_down)   android_emulator_key(emu, 1, 20);
    if (stick_left  && !prev_stick_left)  android_emulator_key(emu, 0, 21);
    if (!stick_left && prev_stick_left)   android_emulator_key(emu, 1, 21);
    if (stick_right && !prev_stick_right) android_emulator_key(emu, 0, 22);
    if (!stick_right&& prev_stick_right)  android_emulator_key(emu, 1, 22);

    prev_stick_up = stick_up;     prev_stick_down  = stick_down;
    prev_stick_left = stick_left; prev_stick_right = stick_right;

    /* Suppress unused-parameter warnings for axes not yet consumed */
    (void)right_x; (void)right_y; (void)left_trigger; (void)right_trigger;
}

/* -------------------------------------------------------------------------
 * android_emulator_destroy
 * ---------------------------------------------------------------------- */

void android_emulator_destroy(AndroidEmulator *emu) {
    if (emu->audio) {
        android_audio_destroy(emu->audio);
        emu->audio = NULL;
    }
    if (emu->gles1) {
        gles1_destroy(emu->gles1);
        emu->gles1 = NULL;
    }
    android_linker_destroy(&emu->linker);
    android_jni_destroy(&emu->jni);
    apk_close(&emu->apk);
    free(emu->mem);
    memset(emu, 0, sizeof(*emu));
}
