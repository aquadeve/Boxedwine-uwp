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
    if (addr + 4 <= emu->mem_size) memcpy(&v, emu->mem + addr, 4);
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

/* Stub index for each bionic function */
enum BionicStub {
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
    STUB_COUNT
};

/* Map stub index to symbol name */
static const char *stub_names[] = {
    NULL,
    "pthread_create",
    "pthread_join",
    "pthread_mutex_lock",
    "pthread_mutex_unlock",
    "malloc",
    "free",
    "calloc",
    "realloc",
    "memcpy",
    "memmove",
    "memset",
    "strlen",
    "strcpy",
    "strncpy",
    "strcmp",
    "strncmp",
    "printf",
    "sprintf",
    "snprintf",
    "puts",
    "abort",
    "dlopen",
    "dlsym",
    "dlclose",
    "dlerror",
};

/* ARM thumb SVC + BX LR encoding (Thumb-2 with SVC stub_id) */
static void install_stub(uint8_t *mem, uint32_t va, uint32_t stub_id) {
    /* Thumb encoding of "SVC stub_id; BX LR" */
    /* SVC imm8 in Thumb = 0xDF00 | (imm8) */
    /* BX LR in Thumb    = 0x4770 */
    uint16_t svc_instr  = (uint16_t)(0xDF00 | (stub_id & 0xFF));
    uint16_t bx_lr_instr = 0x4770;
    memcpy(mem + va,     &svc_instr,   2);
    memcpy(mem + va + 2, &bx_lr_instr, 2);
    /* Pad with NOP */
    uint16_t nop = 0x46C0;
    memcpy(mem + va + 4, &nop, 2);
    memcpy(mem + va + 6, &nop, 2);
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

    switch ((BionicStub)stub_id) {
        /* ---- Memory ---- */
        case STUB_MALLOC: {
            uint32_t sz = cpu->r[0];
            uint32_t base = emu->syscall_state.brk_current;
            sz = (sz + 7) & ~7u;
            if (base + sz <= emu->syscall_state.brk_max) {
                memset(emu->mem + base, 0, sz);
                emu->syscall_state.brk_current += sz;
                cpu->r[0] = base;
            } else {
                cpu->r[0] = 0;
            }
            break;
        }
        case STUB_CALLOC: {
            uint32_t n = cpu->r[0], sz = cpu->r[1];
            uint32_t total = (n * sz + 7) & ~7u;
            uint32_t base  = emu->syscall_state.brk_current;
            if (base + total <= emu->syscall_state.brk_max) {
                memset(emu->mem + base, 0, total);
                emu->syscall_state.brk_current += total;
                cpu->r[0] = base;
            } else { cpu->r[0] = 0; }
            break;
        }
        case STUB_FREE:
            /* No-op for simple bump allocator */
            cpu->r[0] = 0; break;
        case STUB_REALLOC: {
            /* Simplified: just allocate new, copy old */
            uint32_t old_ptr = cpu->r[0];
            uint32_t new_sz  = cpu->r[1];
            uint32_t base    = emu->syscall_state.brk_current;
            new_sz = (new_sz + 7) & ~7u;
            if (base + new_sz <= emu->syscall_state.brk_max) {
                if (old_ptr && old_ptr < base) memcpy(emu->mem + base, emu->mem + old_ptr, new_sz);
                emu->syscall_state.brk_current += new_sz;
                cpu->r[0] = base;
            } else { cpu->r[0] = 0; }
            break;
        }

        /* ---- String / memory ops ---- */
        case STUB_MEMCPY: {
            uint32_t dst = cpu->r[0], src = cpu->r[1], n = cpu->r[2];
            if (dst + n <= emu->mem_size && src + n <= emu->mem_size)
                memcpy(emu->mem + dst, emu->mem + src, n);
            cpu->r[0] = cpu->r[0]; break;
        }
        case STUB_MEMMOVE: {
            uint32_t dst = cpu->r[0], src = cpu->r[1], n = cpu->r[2];
            if (dst + n <= emu->mem_size && src + n <= emu->mem_size)
                memmove(emu->mem + dst, emu->mem + src, n);
            cpu->r[0] = cpu->r[0]; break;
        }
        case STUB_MEMSET: {
            uint32_t dst = cpu->r[0], val = cpu->r[1], n = cpu->r[2];
            if (dst + n <= emu->mem_size)
                memset(emu->mem + dst, (int)val, n);
            cpu->r[0] = cpu->r[0]; break;
        }
        case STUB_STRLEN: {
            uint32_t ptr = cpu->r[0];
            if (ptr < emu->mem_size) {
                cpu->r[0] = (uint32_t)strnlen((char*)emu->mem + ptr, emu->mem_size - ptr);
            } else { cpu->r[0] = 0; }
            break;
        }
        case STUB_STRCPY: {
            uint32_t dst = cpu->r[0], src = cpu->r[1];
            if (dst < emu->mem_size && src < emu->mem_size)
                strncpy((char*)emu->mem + dst, (char*)emu->mem + src,
                        emu->mem_size - dst - 1);
            cpu->r[0] = cpu->r[0]; break;
        }
        case STUB_STRNCPY: {
            uint32_t dst = cpu->r[0], src = cpu->r[1], n = cpu->r[2];
            if (dst + n <= emu->mem_size && src + n <= emu->mem_size)
                strncpy((char*)emu->mem + dst, (char*)emu->mem + src, n);
            cpu->r[0] = cpu->r[0]; break;
        }
        case STUB_STRCMP: {
            uint32_t a = cpu->r[0], b = cpu->r[1];
            if (a < emu->mem_size && b < emu->mem_size) {
                cpu->r[0] = (uint32_t)strcmp((char*)emu->mem + a, (char*)emu->mem + b);
            } else { cpu->r[0] = 1; }
            break;
        }
        case STUB_STRNCMP: {
            uint32_t a = cpu->r[0], b = cpu->r[1], n = cpu->r[2];
            if (a < emu->mem_size && b < emu->mem_size) {
                cpu->r[0] = (uint32_t)strncmp((char*)emu->mem + a, (char*)emu->mem + b, n);
            } else { cpu->r[0] = 1; }
            break;
        }

        /* ---- stdio ---- */
        case STUB_PRINTF:
        case STUB_SPRINTF:
        case STUB_SNPRINTF: {
            /* Simplified: just log the format string */
            uint32_t fmt_ptr = (stub_id == STUB_PRINTF) ? cpu->r[0] : cpu->r[1];
            if (fmt_ptr < emu->mem_size)
                fprintf(stdout, "[bionic] %s\n", (char*)emu->mem + fmt_ptr);
            cpu->r[0] = 0; break;
        }
        case STUB_PUTS: {
            uint32_t ptr = cpu->r[0];
            if (ptr < emu->mem_size)
                puts((char*)emu->mem + ptr);
            cpu->r[0] = 0; break;
        }

        /* ---- Process control ---- */
        case STUB_ABORT:
            fprintf(stderr, "android: abort() called\n");
            emu->cpu.running = false;
            emu->exit_code   = 134;
            break;

        /* ---- Dynamic linking (return NULL - already linked) ---- */
        case STUB_DLOPEN:
        case STUB_DLSYM:
        case STUB_DLCLOSE:
        case STUB_DLERROR:
            cpu->r[0] = 0; break;

        /* ---- Threads (no-op for single-threaded emulation) ---- */
        case STUB_PTHREAD_CREATE:
        case STUB_PTHREAD_JOIN:
        case STUB_PTHREAD_MUTEX_LOCK:
        case STUB_PTHREAD_MUTEX_UNLOCK:
            cpu->r[0] = 0; break;

        default:
            fprintf(stderr, "android_emulator: unknown bionic stub %u\n", stub_id);
            cpu->r[0] = 0; break;
    }
}

/* Combined SWI dispatcher: bionic stubs have IDs < STUB_COUNT */
static void combined_swi_handler(void *ctx, uint32_t swi_num) {
    AndroidEmulator *emu = (AndroidEmulator*)ctx;
    if (swi_num > 0 && swi_num < STUB_COUNT) {
        bionic_stub_dispatch(emu, swi_num);
    } else {
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

    /* Allocate emulated address space */
    emu->mem_size = ANDROID_MEM_SIZE;
    emu->mem = (uint8_t*)calloc(1, emu->mem_size);
    if (!emu->mem) {
        fprintf(stderr, "android_emulator: failed to allocate %u MB of emulated memory\n",
                ANDROID_MEM_SIZE / (1024*1024));
        return false;
    }

    /* Initialise subsystems */
    android_syscall_init(&emu->syscall_state, emu->mem, emu->mem_size);
    android_jni_init(&emu->jni);
    android_linker_init(&emu->linker, emu->mem, emu->mem_size, ANDROID_LOAD_BASE);

    /* Detect ABI: arm64-v8a → AArch64, everything else → ARMv7 */
    emu->is_arm64 = (strcmp(emu->apk.target_abi, "arm64-v8a") == 0);

    /* Install bionic stubs (encoding depends on target ABI) */
    for (unsigned i = 1; i < STUB_COUNT; i++) {
        uint32_t stub_va = BIONIC_STUB_BASE + i * BIONIC_STUB_STRIDE;
        if (emu->is_arm64) {
            install_stub_a64(emu->mem, stub_va, i);
            /* AArch64 doesn't use Thumb bit; register plain VA */
            android_linker_add_override(&emu->linker, stub_names[i], stub_va);
        } else {
            install_stub(emu->mem, stub_va, i);
            /* Register as Thumb address (set bit 0) for BX/BLX */
            android_linker_add_override(&emu->linker, stub_names[i], stub_va | 1u);
        }
    }

    /* Load the APK */
    if (!apk_open(config->apk_path, &emu->apk)) {
        fprintf(stderr, "android_emulator: failed to open APK: %s\n", config->apk_path);
        return false;
    }

    if (config->verbosity >= 1)
        fprintf(stdout, "android_emulator: loaded APK '%s' (%s), %u native lib(s)\n",
                config->apk_path, emu->apk.target_abi, emu->apk.lib_count);

    /* Load all native libraries into emulated memory */
    for (unsigned i = 0; i < emu->apk.lib_count; i++) {
        const ApkLibEntry *lib = &emu->apk.libs[i];
        int idx = android_linker_load(&emu->linker, lib->name, lib->data, lib->size);
        if (idx < 0) {
            fprintf(stderr, "android_emulator: failed to load %s\n", lib->name);
        } else if (config->verbosity >= 2) {
            fprintf(stdout, "android_emulator: loaded %s at VA 0x%08X\n",
                    lib->name, emu->linker.libs[idx].load_base);
        }
    }

    /* Resolve relocations */
    if (!android_linker_relocate_all(&emu->linker)) {
        fprintf(stderr, "android_emulator: relocation failed\n");
        return false;
    }

    /* Initialise CPU based on detected ABI */
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
        fprintf(stderr, "android_emulator: no entry point found\n");
        return false;
    }

    if (config->verbosity >= 1)
        fprintf(stdout, "android_emulator: entry point at VA 0x%08X (%s mode)\n",
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
    if (emu->is_arm64) {
        if (!emu->cpu64.running) return false;
        for (unsigned i = 0; i < max_instructions && emu->cpu64.running; i++) {
            aarch64_step(&emu->cpu64);
        }
        return emu->cpu64.running;
    } else {
        if (!emu->cpu.running) return false;
        for (unsigned i = 0; i < max_instructions && emu->cpu.running; i++) {
            armv7_step(&emu->cpu);
        }
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
    android_linker_destroy(&emu->linker);
    android_jni_destroy(&emu->jni);
    apk_close(&emu->apk);
    free(emu->mem);
    memset(emu, 0, sizeof(*emu));
}
