/*
 * Boxedwine Android - Linux/Android Syscall Layer
 *
 * Implements the Linux system call interface required by Android NDK applications
 * running via the ARMv7 CPU interpreter.  Called from the CPU's SWI handler
 * with Linux syscall numbers (arm-linux-gnueabi ABI).
 *
 * Key syscalls implemented:
 *   - Process: exit, exit_group, getpid, gettid, clone, fork
 *   - Memory:  mmap2, munmap, mprotect, brk, mremap
 *   - File I/O: open, close, read, write, lseek, fstat, stat, access
 *   - Time:    clock_gettime, gettimeofday, nanosleep
 *   - Thread:  futex, set_tls
 *   - Misc:    uname, ioctl, prctl, sysinfo
 *
 * Reference:
 *   referenceCode/Bridge/BridgeLib/kernel/  (FLinux syscall implementations)
 *   Linux ARM EABI syscall ABI (arm-linux-gnueabi)
 */

#ifndef __ANDROID_SYSCALL_H__
#define __ANDROID_SYSCALL_H__

#include <stdint.h>
#include <stdbool.h>
#include "../emulation/cpu/arm/armv7_interpreter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * ARM Linux EABI syscall numbers (kernel 3.x / 4.x)
 * ---------------------------------------------------------------------- */
#define SYS_ARM_exit         1
#define SYS_ARM_fork         2
#define SYS_ARM_read         3
#define SYS_ARM_write        4
#define SYS_ARM_open         5
#define SYS_ARM_close        6
#define SYS_ARM_waitpid      7
#define SYS_ARM_unlink       10
#define SYS_ARM_execve       11
#define SYS_ARM_getpid       20
#define SYS_ARM_access       33
#define SYS_ARM_brk          45
#define SYS_ARM_ioctl        54
#define SYS_ARM_getdents     141
#define SYS_ARM_lseek        19
#define SYS_ARM_mmap         90
#define SYS_ARM_munmap       91
#define SYS_ARM_stat         106
#define SYS_ARM_lstat        107
#define SYS_ARM_fstat        108
#define SYS_ARM_uname        122
#define SYS_ARM_mprotect     125
#define SYS_ARM_clone        120
#define SYS_ARM_mmap2        192
#define SYS_ARM_stat64       195
#define SYS_ARM_lstat64      196
#define SYS_ARM_fstat64      197
#define SYS_ARM_getuid32     199
#define SYS_ARM_getgid32     200
#define SYS_ARM_geteuid32    201
#define SYS_ARM_getegid32    202
#define SYS_ARM_exit_group   248
#define SYS_ARM_set_thread_area 243
#define SYS_ARM_futex        240
#define SYS_ARM_gettid       224
#define SYS_ARM_clock_gettime 263
#define SYS_ARM_clock_getres  264
#define SYS_ARM_statfs64     265
#define SYS_ARM_fstatfs64    266
#define SYS_ARM_nanosleep    162
#define SYS_ARM_gettimeofday 78
#define SYS_ARM_prctl        172
#define SYS_ARM_sysinfo      116
#define SYS_ARM_socket       281
#define SYS_ARM_connect      282
#define SYS_ARM_send         289
#define SYS_ARM_recv         291
#define SYS_ARM_pipe         42
#define SYS_ARM_fcntl        55
#define SYS_ARM_getdents64   217
#define SYS_ARM_set_tls      0xF0005   /* ARM private */
#define SYS_ARM_cacheflush   0xF0002   /* ARM private */
#define SYS_ARM_openat       322
#define SYS_ARM_mkdirat      323
#define SYS_ARM_fstatat64    327
#define SYS_ARM_pread64      180
#define SYS_ARM_pwrite64     181
#define SYS_ARM_writev       146
#define SYS_ARM_readv        145

/* -------------------------------------------------------------------------
 * Emulated memory layout
 * ---------------------------------------------------------------------- */
#define ANDROID_MEM_SIZE     (256u * 1024u * 1024u)  /* 256 MB emulated RAM */
#define ANDROID_STACK_TOP    (ANDROID_MEM_SIZE - 4u) /* Stack grows downward */
#define ANDROID_STACK_SIZE   (8u * 1024u * 1024u)    /* 8 MB stack */
#define ANDROID_LOAD_BASE    0x10000u                /* Libraries start here */
#define ANDROID_HEAP_BASE    0x08000000u             /* brk starts here */

/* -------------------------------------------------------------------------
 * Syscall context (one per emulated thread)
 * ---------------------------------------------------------------------- */
typedef struct {
    /* Emulated address space */
    uint8_t *mem;
    uint32_t mem_size;

    /* Process state */
    int32_t  pid;
    int32_t  tid;
    uint32_t brk_current;
    uint32_t brk_max;

    /* Thread-local storage pointer (set via set_tls) */
    uint32_t tls_ptr;

    /* Simple open file descriptors (fd -> host FILE*) */
    struct { int fd; void *host_fp; char path[512]; } fds[64];
    unsigned fd_count;
} AndroidSyscallState;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * Initialise the syscall state.
 */
void android_syscall_init(AndroidSyscallState *state, uint8_t *mem, uint32_t mem_size);

/**
 * Main syscall dispatcher.  Called from the ARMv7 SWI handler with the
 * syscall number in R7 and arguments in R0-R6.
 *
 * The result is written back to cpu->r[0] (or the error in cpu->r[0] negated).
 */
void android_syscall_dispatch(ArmV7State *cpu, AndroidSyscallState *state, uint32_t swi_num);

/**
 * UWP-friendly path translation: maps Android-style /data/... paths to
 * local storage paths accessible from UWP.
 */
const char *android_syscall_translate_path(const AndroidSyscallState *state,
                                            const char *android_path,
                                            char *out_buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* __ANDROID_SYSCALL_H__ */
