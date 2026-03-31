/*
 * Boxedwine Android - Linux/Android Syscall Implementation
 *
 * Dispatches Linux ARM EABI system calls to host implementations.
 * For UWP targets, file system access is redirected to the app's
 * local storage folder.
 *
 * Reference:
 *   referenceCode/Bridge/BridgeLib/kernel/  (FLinux syscall impl)
 *   Linux kernel ARM syscall table
 */

#include "android_syscall.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  define HOST_MKDIR(p,m) CreateDirectoryA(p, NULL)
#else
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <fcntl.h>
#  include <unistd.h>
#  define HOST_MKDIR(p,m) mkdir(p, m)
#endif

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Write a 32-bit value into the emulated address space */
static void mem_write32(AndroidSyscallState *s, uint32_t addr, uint32_t val) {
    if (addr + 4 <= s->mem_size)
        memcpy(s->mem + addr, &val, 4);
}

static uint32_t mem_read32(AndroidSyscallState *s, uint32_t addr) {
    uint32_t v = 0;
    if (addr + 4 <= s->mem_size) memcpy(&v, s->mem + addr, 4);
    return v;
}

static const char *mem_str(AndroidSyscallState *s, uint32_t addr) {
    if (addr >= s->mem_size) return "";
    return (const char*)(s->mem + addr);
}

/* Return next available host fd slot, or -1 */
static int alloc_fd(AndroidSyscallState *state, void *host_fp, const char *path) {
    for (unsigned i = 0; i < 64; i++) {
        if (!state->fds[i].host_fp && state->fds[i].fd == 0) {
            state->fds[i].fd      = (int)(i + 3); /* 0,1,2 = stdin/out/err */
            state->fds[i].host_fp = host_fp;
            strncpy(state->fds[i].path, path, 511);
            return state->fds[i].fd;
        }
    }
    return -1;
}

static void *get_host_fp(AndroidSyscallState *state, int fd) {
    for (unsigned i = 0; i < 64; i++) {
        if (state->fds[i].fd == fd) return state->fds[i].host_fp;
    }
    return NULL;
}

static void free_fd(AndroidSyscallState *state, int fd) {
    for (unsigned i = 0; i < 64; i++) {
        if (state->fds[i].fd == fd) {
            state->fds[i].fd = 0;
            state->fds[i].host_fp = NULL;
            state->fds[i].path[0] = '\0';
            return;
        }
    }
}

/* -------------------------------------------------------------------------
 * android_syscall_init
 * ---------------------------------------------------------------------- */

void android_syscall_init(AndroidSyscallState *state, uint8_t *mem, uint32_t mem_size) {
    memset(state, 0, sizeof(*state));
    state->mem       = mem;
    state->mem_size  = mem_size;
    state->pid       = 1;
    state->tid       = 1;
    state->brk_current = ANDROID_HEAP_BASE;
    state->brk_max     = ANDROID_HEAP_BASE + 32u * 1024u * 1024u; /* 32 MB heap */
}

/* -------------------------------------------------------------------------
 * android_syscall_translate_path
 * Maps /data/data/<pkg>/ -> local storage.
 * Maps /sdcard/ -> local "sdcard" folder.
 * Other paths pass through (or get a safe sandbox prefix on UWP).
 * ---------------------------------------------------------------------- */

const char *android_syscall_translate_path(const AndroidSyscallState *state,
                                            const char *android_path,
                                            char *out_buf, size_t buf_size) {
    (void)state;
    /* On UWP all file access should go through the local app data folder.
     * We use a relative path rooted at "AndroidData" to keep it simple. */
    if (strncmp(android_path, "/data/", 6) == 0) {
        snprintf(out_buf, buf_size, "AndroidData%s", android_path + 5);
    } else if (strncmp(android_path, "/sdcard/", 8) == 0) {
        snprintf(out_buf, buf_size, "AndroidSdcard/%s", android_path + 8);
    } else if (strncmp(android_path, "/proc/", 6) == 0 ||
               strncmp(android_path, "/sys/", 5) == 0) {
        /* Virtual filesystems - return as-is, we'll fake the contents */
        snprintf(out_buf, buf_size, "%s", android_path);
    } else {
        snprintf(out_buf, buf_size, "%s", android_path);
    }
    return out_buf;
}

/* -------------------------------------------------------------------------
 * Emulated stat64 structure (Android ARM 32-bit layout)
 *
 * Field names use emu_ prefix to avoid clashing with POSIX st_atime/st_mtime/st_ctime
 * macros that some systems define as member-accessor macros.
 * ---------------------------------------------------------------------- */
typedef struct {
    uint64_t emu_st_dev;
    uint8_t  emu_pad0[4];
    uint32_t emu_st_ino_old;
    uint32_t emu_st_mode;
    uint32_t emu_st_nlink;
    uint32_t emu_st_uid;
    uint32_t emu_st_gid;
    uint64_t emu_st_rdev;
    uint8_t  emu_pad3[4];
    int64_t  emu_st_size;
    uint32_t emu_st_blksize;
    uint64_t emu_st_blocks;
    uint32_t emu_st_atime_sec;
    uint32_t emu_st_atime_nsec;
    uint32_t emu_st_mtime_sec;
    uint32_t emu_st_mtime_nsec;
    uint32_t emu_st_ctime_sec;
    uint32_t emu_st_ctime_nsec;
    uint64_t emu_st_ino;
} AndroidStat64;

/* -------------------------------------------------------------------------
 * android_syscall_dispatch
 * ---------------------------------------------------------------------- */

void android_syscall_dispatch(ArmV7State *cpu, AndroidSyscallState *state, uint32_t swi_num) {
    /* On ARM Linux the syscall number is in R7, args in R0-R6 */
    uint32_t nr   = cpu->r[7];
    uint32_t arg0 = cpu->r[0];
    uint32_t arg1 = cpu->r[1];
    uint32_t arg2 = cpu->r[2];
    uint32_t arg3 = cpu->r[3];
    uint32_t arg4 = cpu->r[4];
    /* uint32_t arg5 = cpu->r[5]; */
    (void)swi_num;

    switch (nr) {
        /* ------ exit / exit_group ------ */
        case SYS_ARM_exit:
        case SYS_ARM_exit_group:
            cpu->running    = false;
            cpu->exit_code  = (int)arg0;
            cpu->r[0]       = 0;
            return;

        /* ------ getpid / gettid ------ */
        case SYS_ARM_getpid:
            cpu->r[0] = (uint32_t)state->pid; return;
        case SYS_ARM_gettid:
            cpu->r[0] = (uint32_t)state->tid; return;

        /* ------ getuid/getgid etc (return 0 = root) ------ */
        case SYS_ARM_getuid32:
        case SYS_ARM_getgid32:
        case SYS_ARM_geteuid32:
        case SYS_ARM_getegid32:
            cpu->r[0] = 0; return;

        /* ------ brk ------ */
        case SYS_ARM_brk:
            if (arg0 == 0 || arg0 < state->brk_current) {
                cpu->r[0] = state->brk_current;
            } else if (arg0 <= state->brk_max) {
                /* Zero new pages */
                if (arg0 > state->brk_current)
                    memset(state->mem + state->brk_current, 0,
                           arg0 - state->brk_current);
                state->brk_current = arg0;
                cpu->r[0] = state->brk_current;
            } else {
                cpu->r[0] = (uint32_t)-12; /* -ENOMEM */
            }
            return;

        /* ------ mmap2 ------ */
        case SYS_ARM_mmap2: {
            /* Android uses mmap2 extensively for anonymous mappings (malloc etc.) */
            uint32_t length = (arg1 + 4095) & ~4095u;
            int32_t  fd     = (int32_t)arg3;

            if (fd < 0) {
                /* Anonymous mapping: allocate from heap area */
                if (state->brk_current + length <= state->brk_max) {
                    uint32_t base = state->brk_current;
                    memset(state->mem + base, 0, length);
                    state->brk_current += length;
                    cpu->r[0] = base;
                } else {
                    cpu->r[0] = (uint32_t)-12; /* -ENOMEM */
                }
            } else {
                /* File-backed mmap: simplified - load data */
                void *fp = get_host_fp(state, fd);
                if (fp && state->brk_current + length <= state->brk_max) {
                    uint32_t base = state->brk_current;
                    long offset = (long)arg4 * 4096;
                    fseek((FILE*)fp, offset, SEEK_SET);
                    size_t rd = fread(state->mem + base, 1, length, (FILE*)fp);
                    if (rd < length) memset(state->mem + base + rd, 0, length - rd);
                    state->brk_current += length;
                    cpu->r[0] = base;
                } else {
                    cpu->r[0] = (uint32_t)-9; /* -EBADF */
                }
            }
            return;
        }

        /* ------ munmap / mprotect ------ */
        case SYS_ARM_munmap:
        case SYS_ARM_mprotect:
            cpu->r[0] = 0; return;

        /* ------ open / openat ------ */
        case SYS_ARM_open:
        case SYS_ARM_openat: {
            const char *path_arg = (nr == SYS_ARM_openat) ?
                                   mem_str(state, arg1) : mem_str(state, arg0);
            uint32_t flags_arg   = (nr == SYS_ARM_openat) ? arg2 : arg1;

            char translated[512];
            android_syscall_translate_path(state, path_arg, translated, sizeof(translated));

            const char *mode = (flags_arg & 1) ? "r+b" : "rb";
            if (flags_arg & 0x40) mode = "w+b"; /* O_CREAT */

            FILE *fp = fopen(translated, mode);
            if (!fp && !(flags_arg & 0x40)) fp = fopen(translated, "rb");
            if (fp) {
                int fd = alloc_fd(state, fp, translated);
                cpu->r[0] = (fd >= 0) ? (uint32_t)fd : (uint32_t)-24; /* -EMFILE */
            } else {
                cpu->r[0] = (uint32_t)-2; /* -ENOENT */
            }
            return;
        }

        /* ------ close ------ */
        case SYS_ARM_close: {
            void *fp = get_host_fp(state, (int)arg0);
            if (fp) { fclose((FILE*)fp); free_fd(state, (int)arg0); cpu->r[0] = 0; }
            else cpu->r[0] = (uint32_t)-9; /* -EBADF */
            return;
        }

        /* ------ read ------ */
        case SYS_ARM_read: {
            void *fp = get_host_fp(state, (int)arg0);
            if (!fp) { cpu->r[0] = (uint32_t)-9; return; }
            if (arg1 + arg2 > state->mem_size) { cpu->r[0] = (uint32_t)-14; return; }
            size_t n = fread(state->mem + arg1, 1, arg2, (FILE*)fp);
            cpu->r[0] = (uint32_t)n;
            return;
        }

        /* ------ write ------ */
        case SYS_ARM_write: {
            if ((int)arg0 == 1 || (int)arg0 == 2) {
                /* stdout / stderr: print to host console */
                if (arg1 + arg2 <= state->mem_size) {
                    fwrite(state->mem + arg1, 1, arg2, ((int)arg0 == 2) ? stderr : stdout);
                }
                cpu->r[0] = arg2; return;
            }
            void *fp = get_host_fp(state, (int)arg0);
            if (!fp) { cpu->r[0] = (uint32_t)-9; return; }
            size_t n = fwrite(state->mem + arg1, 1, arg2, (FILE*)fp);
            cpu->r[0] = (uint32_t)n;
            return;
        }

        /* ------ lseek ------ */
        case SYS_ARM_lseek: {
            void *fp = get_host_fp(state, (int)arg0);
            if (!fp) { cpu->r[0] = (uint32_t)-9; return; }
            int whence = (int)arg2;
            fseek((FILE*)fp, (long)(int32_t)arg1, whence);
            cpu->r[0] = (uint32_t)ftell((FILE*)fp);
            return;
        }

        /* ------ fstat64 ------ */
        case SYS_ARM_fstat64:
        case SYS_ARM_stat64:
        case SYS_ARM_lstat64: {
            AndroidStat64 st;
            memset(&st, 0, sizeof(st));
            /* Fake a regular file */
            st.emu_st_mode = 0x81A4; /* S_IFREG | 0644 */
            st.emu_st_nlink = 1;
            st.emu_st_size = 0;
            if (arg1 + sizeof(st) <= state->mem_size)
                memcpy(state->mem + arg1, &st, sizeof(st));
            cpu->r[0] = 0;
            return;
        }

        /* ------ access ------ */
        case SYS_ARM_access: {
            char translated[512];
            android_syscall_translate_path(state, mem_str(state, arg0),
                                           translated, sizeof(translated));
#ifdef _WIN32
            DWORD attr = GetFileAttributesA(translated);
            cpu->r[0] = (attr != INVALID_FILE_ATTRIBUTES) ? 0 : (uint32_t)-2;
#else
            cpu->r[0] = (access(translated, F_OK) == 0) ? 0 : (uint32_t)-2;
#endif
            return;
        }

        /* ------ uname ------ */
        case SYS_ARM_uname: {
            /* struct utsname: 5 fields of 65 bytes each */
            if (arg0 + 65*5 <= state->mem_size) {
                uint8_t *u = state->mem + arg0;
                memset(u, 0, 65*5);
                strncpy((char*)u,            "Linux",  64);
                strncpy((char*)u + 65,       "android", 64);
                strncpy((char*)u + 65*2,     "3.18.0-android", 64);
                strncpy((char*)u + 65*3,     "#1 SMP",  64);
                strncpy((char*)u + 65*4,     "armv7l",  64);
            }
            cpu->r[0] = 0;
            return;
        }

        /* ------ clock_gettime ------ */
        case SYS_ARM_clock_gettime: {
            if (arg1 + 8 <= state->mem_size) {
#ifdef _WIN32
                LARGE_INTEGER freq, cnt;
                QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&cnt);
                uint64_t ns = (uint64_t)cnt.QuadPart * 1000000000ULL / (uint64_t)freq.QuadPart;
                mem_write32(state, arg1,     (uint32_t)(ns / 1000000000ULL));
                mem_write32(state, arg1 + 4, (uint32_t)(ns % 1000000000ULL));
#else
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                mem_write32(state, arg1,     (uint32_t)ts.tv_sec);
                mem_write32(state, arg1 + 4, (uint32_t)ts.tv_nsec);
#endif
            }
            cpu->r[0] = 0;
            return;
        }

        /* ------ gettimeofday ------ */
        case SYS_ARM_gettimeofday: {
#ifdef _WIN32
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            t -= 116444736000000000ULL; /* to Unix epoch in 100-ns units */
            mem_write32(state, arg0,     (uint32_t)(t / 10000000ULL));
            mem_write32(state, arg0 + 4, (uint32_t)((t % 10000000ULL) * 100 / 1000));
#else
            struct timeval tv;
            gettimeofday(&tv, NULL);
            mem_write32(state, arg0,     (uint32_t)tv.tv_sec);
            mem_write32(state, arg0 + 4, (uint32_t)tv.tv_usec);
#endif
            cpu->r[0] = 0;
            return;
        }

        /* ------ nanosleep ------ */
        case SYS_ARM_nanosleep: {
            if (arg0 + 8 <= state->mem_size) {
                uint32_t secs  = mem_read32(state, arg0);
                uint32_t nsecs = mem_read32(state, arg0 + 4);
#ifdef _WIN32
                DWORD ms = secs * 1000 + nsecs / 1000000;
                if (ms) Sleep(ms);
#else
                struct timespec req = { (time_t)secs, (long)nsecs };
                nanosleep(&req, NULL);
#endif
            }
            cpu->r[0] = 0;
            return;
        }

        /* ------ futex (simplified) ------ */
        case SYS_ARM_futex:
            cpu->r[0] = 0; return;

        /* ------ set_tls ------ */
        case SYS_ARM_set_tls:
        case SYS_ARM_set_thread_area:
            state->tls_ptr = arg0;
            cpu->r[0] = 0;
            return;

        /* ------ prctl ------ */
        case SYS_ARM_prctl:
            cpu->r[0] = 0; return;

        /* ------ ioctl ------ */
        case SYS_ARM_ioctl:
            cpu->r[0] = (uint32_t)-25; /* -ENOTTY */ return;

        /* ------ pipe ------ */
        case SYS_ARM_pipe:
            /* Return dummy fds */
            if (arg0 + 8 <= state->mem_size) {
                mem_write32(state, arg0,     3);
                mem_write32(state, arg0 + 4, 4);
            }
            cpu->r[0] = 0; return;

        /* ------ cacheflush (ARM private) ------ */
        case SYS_ARM_cacheflush:
            cpu->r[0] = 0; return;

        /* ------ sysinfo ------ */
        case SYS_ARM_sysinfo:
            /* Fake sysinfo struct (64 bytes) */
            if (arg0 + 64 <= state->mem_size) memset(state->mem + arg0, 0, 64);
            cpu->r[0] = 0; return;

        /* ------ unlink ------ */
        case SYS_ARM_unlink: {
            char translated[512];
            android_syscall_translate_path(state, mem_str(state, arg0),
                                           translated, sizeof(translated));
            remove(translated);
            cpu->r[0] = 0; return;
        }

        /* ------ pread64 ------ */
        case SYS_ARM_pread64: {
            void *fp = get_host_fp(state, (int)arg0);
            if (!fp) { cpu->r[0] = (uint32_t)-9; return; }
            long offset = ((long)arg4 << 32) | (long)arg3;
            long pos = ftell((FILE*)fp);
            fseek((FILE*)fp, offset, SEEK_SET);
            size_t n = fread(state->mem + arg1, 1, arg2, (FILE*)fp);
            fseek((FILE*)fp, pos, SEEK_SET);
            cpu->r[0] = (uint32_t)n;
            return;
        }

        default:
            /* Unknown syscall: return -ENOSYS */
            fprintf(stderr, "android_syscall: unimplemented nr=%u (0x%x)\n", nr, nr);
            cpu->r[0] = (uint32_t)-38; /* -ENOSYS */
            return;
    }
}
