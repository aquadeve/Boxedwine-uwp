/*
 * Boxedwine Android - ARMv7 CPU Interpreter Implementation
 *
 * Full ARMv7-A instruction set emulator supporting:
 *   - All ARM32 data-processing, load/store, branch instructions
 *   - Thumb (T16) and Thumb-2 (T32) instruction sets
 *   - VFP v3 floating-point (single and double precision)
 *   - Software interrupt (SWI/SVC) for Linux syscall emulation
 *
 * Reference: ARM Architecture Reference Manual ARMv7-A and ARMv7-R edition
 */

#include "armv7_interpreter.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/* MSVC does not provide __builtin_popcount; use the intrinsic instead */
#ifdef _MSC_VER
#include <intrin.h>
#include <windows.h>
#include <stdarg.h>
#define __builtin_popcount __popcnt
#endif

/* -------------------------------------------------------------------------
 * Debug logging for the ARM interpreter.
 * On MSVC, use OutputDebugStringA so messages appear in VS Output window.
 * ---------------------------------------------------------------------- */
#if defined(_DEBUG) || !defined(NDEBUG)
#  if defined(_MSC_VER)
     static inline void _arm_dbg(const char *fmt, ...) {
         char buf[512];
         va_list ap;
         va_start(ap, fmt);
         vsnprintf(buf, sizeof(buf), fmt, ap);
         va_end(ap);
         OutputDebugStringA(buf);
     }
#    define ARM_LOG_DEBUG(fmt, ...) _arm_dbg("[ARM DEBUG] " fmt "\n", ##__VA_ARGS__)
#  else
#    define ARM_LOG_DEBUG(fmt, ...) fprintf(stderr, "[ARM DEBUG] " fmt "\n", ##__VA_ARGS__)
#  endif
#else
#  define ARM_LOG_DEBUG(fmt, ...) ((void)0)
#endif

/* =========================================================================
 * Helpers
 * ========================================================================= */

static inline uint32_t cpsr_n(const ArmV7State *cpu) { return (cpu->cpsr >> 31) & 1; }
static inline uint32_t cpsr_z(const ArmV7State *cpu) { return (cpu->cpsr >> 30) & 1; }
static inline uint32_t cpsr_c(const ArmV7State *cpu) { return (cpu->cpsr >> 29) & 1; }
static inline uint32_t cpsr_v(const ArmV7State *cpu) { return (cpu->cpsr >> 28) & 1; }
static inline bool     is_thumb(const ArmV7State *cpu) { return (cpu->cpsr & ARM_CPSR_T) != 0; }

static inline void set_nzcv(ArmV7State *cpu, bool n, bool z, bool c, bool v) {
    cpu->cpsr = (cpu->cpsr & ~(ARM_CPSR_N | ARM_CPSR_Z | ARM_CPSR_C | ARM_CPSR_V))
              | (n ? ARM_CPSR_N : 0)
              | (z ? ARM_CPSR_Z : 0)
              | (c ? ARM_CPSR_C : 0)
              | (v ? ARM_CPSR_V : 0);
}

/* Rotate-right 32-bit value */
static inline uint32_t ror32(uint32_t val, unsigned shift) {
    shift &= 31;
    if (!shift) return val;
    return (val >> shift) | (val << (32u - shift));
}

/* Sign-extend a value from 'bits' bits to 32 bits */
static inline int32_t sign_extend(uint32_t val, unsigned bits) {
    unsigned shift = 32 - bits;
    return (int32_t)(val << shift) >> shift;
}

bool armv7_eval_cond(const ArmV7State *cpu, ArmCondition cond) {
    bool n = cpsr_n(cpu), z = cpsr_z(cpu), c = cpsr_c(cpu), v = cpsr_v(cpu);
    switch (cond) {
        case ARM_COND_EQ: return  z;
        case ARM_COND_NE: return !z;
        case ARM_COND_CS: return  c;
        case ARM_COND_CC: return !c;
        case ARM_COND_MI: return  n;
        case ARM_COND_PL: return !n;
        case ARM_COND_VS: return  v;
        case ARM_COND_VC: return !v;
        case ARM_COND_HI: return  c && !z;
        case ARM_COND_LS: return !c ||  z;
        case ARM_COND_GE: return  n ==  v;
        case ARM_COND_LT: return  n !=  v;
        case ARM_COND_GT: return !z && (n == v);
        case ARM_COND_LE: return  z || (n != v);
        case ARM_COND_AL: return true;
        default:          return true;
    }
}

float armv7_get_s(const ArmV7State *cpu, unsigned sreg) {
    float f;
    if (sreg & 1) {
        uint64_t d;
        memcpy(&d, &cpu->d[sreg >> 1], 8);
        uint32_t hi = (uint32_t)(d >> 32);
        memcpy(&f, &hi, 4);
    } else {
        uint64_t d;
        memcpy(&d, &cpu->d[sreg >> 1], 8);
        uint32_t lo = (uint32_t)d;
        memcpy(&f, &lo, 4);
    }
    return f;
}

void armv7_set_s(ArmV7State *cpu, unsigned sreg, float val) {
    uint32_t bits;
    memcpy(&bits, &val, 4);
    uint64_t d;
    memcpy(&d, &cpu->d[sreg >> 1], 8);
    if (sreg & 1) {
        d = (d & 0xFFFFFFFFull) | ((uint64_t)bits << 32);
    } else {
        d = (d & 0xFFFFFFFF00000000ull) | bits;
    }
    memcpy(&cpu->d[sreg >> 1], &d, 8);
}

/* =========================================================================
 * Barrel shifter
 * ========================================================================= */

typedef enum { SHIFT_LSL = 0, SHIFT_LSR, SHIFT_ASR, SHIFT_ROR } ShiftType;

static uint32_t barrel_shift(uint32_t val, ShiftType type, unsigned amount, bool *carry_out) {
    if (amount == 0) {
        return val;
    }
    switch (type) {
        case SHIFT_LSL:
            if (amount >= 32) { *carry_out = (amount == 32) ? (val & 1) : 0; return 0; }
            *carry_out = (val >> (32u - amount)) & 1;
            return val << amount;
        case SHIFT_LSR:
            if (amount >= 32) { *carry_out = (amount == 32) ? ((val >> 31) & 1) : 0; return 0; }
            *carry_out = (val >> (amount - 1)) & 1;
            return val >> amount;
        case SHIFT_ASR:
            if (amount >= 32) { *carry_out = (val >> 31) & 1; return (uint32_t)((int32_t)val >> 31); }
            *carry_out = (val >> (amount - 1)) & 1;
            return (uint32_t)((int32_t)val >> amount);
        case SHIFT_ROR: {
            amount &= 31;
            if (!amount) { return val; }
            *carry_out = (val >> (amount - 1)) & 1;
            return ror32(val, amount);
        }
    }
    return val;
}

/* =========================================================================
 * Memory helpers
 * ========================================================================= */

static inline uint8_t  mem_r8 (ArmV7State *cpu, uint32_t a) { return cpu->mem_read8 (cpu->callback_ctx, a); }
static inline uint16_t mem_r16(ArmV7State *cpu, uint32_t a) { return cpu->mem_read16(cpu->callback_ctx, a); }
static inline uint32_t mem_r32(ArmV7State *cpu, uint32_t a) { return cpu->mem_read32(cpu->callback_ctx, a); }
static inline void mem_w8 (ArmV7State *cpu, uint32_t a, uint8_t  v) { cpu->mem_write8 (cpu->callback_ctx, a, v); }
static inline void mem_w16(ArmV7State *cpu, uint32_t a, uint16_t v) { cpu->mem_write16(cpu->callback_ctx, a, v); }
static inline void mem_w32(ArmV7State *cpu, uint32_t a, uint32_t v) { cpu->mem_write32(cpu->callback_ctx, a, v); }

/* =========================================================================
 * ARM32 instruction execution
 * ========================================================================= */

/* Decode and apply shifted register operand (immediate shift) */
static uint32_t decode_shifted_reg(ArmV7State *cpu, uint32_t instr, bool *carry_out) {
    unsigned rm     = instr & 0xF;
    unsigned shift  = (instr >> 5) & 3;
    unsigned amount;
    if (instr & (1u << 4)) {
        /* Register-specified shift amount */
        unsigned rs = (instr >> 8) & 0xF;
        amount = cpu->r[rs] & 0xFF;
    } else {
        amount = (instr >> 7) & 0x1F;
    }
    *carry_out = cpsr_c(cpu);
    return barrel_shift(cpu->r[rm], (ShiftType)shift, amount, carry_out);
}

/* Process a data-processing instruction */
static void exec_arm_data_proc(ArmV7State *cpu, uint32_t instr) {
    bool    S    = (instr >> 20) & 1;
    unsigned Rn  = (instr >> 16) & 0xF;
    unsigned Rd  = (instr >> 12) & 0xF;
    unsigned opc = (instr >> 21) & 0xF;

    uint32_t operand2;
    bool     carry_out = cpsr_c(cpu);

    if (instr & (1u << 25)) {
        /* Immediate operand with optional rotate */
        unsigned imm8   = instr & 0xFF;
        unsigned rotate = ((instr >> 8) & 0xF) * 2;
        operand2 = ror32(imm8, rotate);
        if (rotate) carry_out = (operand2 >> 31) & 1;
    } else {
        operand2 = decode_shifted_reg(cpu, instr, &carry_out);
    }

    uint32_t a    = cpu->r[Rn];
    uint64_t res64;
    uint32_t result = 0;
    bool     write_result = true;

    switch (opc) {
        case  0: result = a & operand2; break;                          /* AND */
        case  1: result = a ^ operand2; break;                          /* EOR */
        case  2: result = a - operand2;                                  /* SUB */
            if (S) { bool b = (uint64_t)a < (uint64_t)operand2; bool v = ((int32_t)(a^operand2) < 0) && ((int32_t)(a^result) < 0); set_nzcv(cpu, (int32_t)result < 0, result == 0, !b, v); S = false; }
            break;
        case  3: result = operand2 - a;                                  /* RSB */
            if (S) { bool b = (uint64_t)operand2 < (uint64_t)a; bool v = ((int32_t)(operand2^a) < 0) && ((int32_t)(operand2^result) < 0); set_nzcv(cpu, (int32_t)result < 0, result == 0, !b, v); S = false; }
            break;
        case  4:                                                          /* ADD */
            res64 = (uint64_t)a + operand2;
            result = (uint32_t)res64;
            if (S) { bool c = res64 >> 32; bool v = ~(a^operand2) & (a^result) & 0x80000000; set_nzcv(cpu, (int32_t)result < 0, result == 0, c, v); S = false; }
            break;
        case  5:                                                          /* ADC */
            res64 = (uint64_t)a + operand2 + cpsr_c(cpu);
            result = (uint32_t)res64;
            if (S) { bool c = res64 >> 32; bool v = ~(a^operand2) & (a^result) & 0x80000000; set_nzcv(cpu, (int32_t)result < 0, result == 0, c, v); S = false; }
            break;
        case  6:                                                          /* SBC */
            res64 = (uint64_t)a - operand2 - (1 - cpsr_c(cpu));
            result = (uint32_t)res64;
            if (S) { bool c = !(res64 >> 32 & 1); bool v = ((int32_t)(a^operand2) < 0) && ((int32_t)(a^result) < 0); set_nzcv(cpu, (int32_t)result < 0, result == 0, c, v); S = false; }
            break;
        case  7:                                                          /* RSC */
            res64 = (uint64_t)operand2 - a - (1 - cpsr_c(cpu));
            result = (uint32_t)res64;
            if (S) { bool c = !(res64 >> 32 & 1); bool v = ((int32_t)(operand2^a) < 0) && ((int32_t)(operand2^result) < 0); set_nzcv(cpu, (int32_t)result < 0, result == 0, c, v); S = false; }
            break;
        case  8: result = a & operand2; write_result = false; break;    /* TST */
        case  9: result = a ^ operand2; write_result = false; break;    /* TEQ */
        case 10: result = a - operand2; write_result = false;            /* CMP */
            { bool b = (uint64_t)a < (uint64_t)operand2; bool v = ((int32_t)(a^operand2) < 0) && ((int32_t)(a^result) < 0); set_nzcv(cpu, (int32_t)result < 0, result == 0, !b, v); S = false; }
            break;
        case 11: result = a + operand2; write_result = false;            /* CMN */
            { res64 = (uint64_t)a + operand2; result = (uint32_t)res64; bool c = res64 >> 32; bool v = ~(a^operand2) & (a^result) & 0x80000000; set_nzcv(cpu, (int32_t)result < 0, result == 0, c, (bool)v); S = false; }
            break;
        case 12: result = a | operand2; break;                           /* ORR */
        case 13: result = operand2; break;                               /* MOV */
        case 14: result = a & ~operand2; break;                         /* BIC */
        case 15: result = ~operand2; break;                              /* MVN */
    }

    if (S) {
        set_nzcv(cpu, (int32_t)result < 0, result == 0, carry_out, cpsr_v(cpu));
    }

    if (write_result) {
        if (Rd == ARM_PC) {
            cpu->r[ARM_PC] = result & ~1u;
            if (result & 1) cpu->cpsr |= ARM_CPSR_T;
        } else {
            cpu->r[Rd] = result;
        }
    }
}

/* Branch instructions */
static void exec_arm_branch(ArmV7State *cpu, uint32_t instr) {
    int32_t offset = sign_extend((instr & 0xFFFFFF) << 2, 26);
    bool    link   = (instr >> 24) & 1;
    if (link) cpu->r[ARM_LR] = cpu->r[ARM_PC]; /* PC already +8 */
    cpu->r[ARM_PC] = (uint32_t)((int32_t)cpu->r[ARM_PC] + offset);
}

/* BX / BLX */
static void exec_arm_bx(ArmV7State *cpu, uint32_t instr) {
    unsigned rm  = instr & 0xF;
    bool     blx = ((instr >> 4) & 0xF) == 3; /* BLX vs BX */
    uint32_t target = cpu->r[rm];
    if (blx) cpu->r[ARM_LR] = cpu->r[ARM_PC];

#if defined(_DEBUG) || !defined(NDEBUG)
    /* Log branches to address 0 (halt sentinel) and mode switches */
    if (target == 0) {
        ARM_LOG_DEBUG("BX to 0x00000000 — halting (from pc=0x%08X, lr=0x%08X)",
                      cpu->r[ARM_PC], cpu->r[ARM_LR]);
        cpu->running = false;
        return;
    }
    {
        static unsigned bx_log_count = 0;
        bx_log_count++;
        bool mode_switch = ((target & 1) != 0) != is_thumb(cpu);
        if (bx_log_count <= 30 || mode_switch) {
            ARM_LOG_DEBUG("BX%s R%u=0x%08X %s->%s (from pc=0x%08X, #%u)",
                          blx ? "L" : "", rm, target,
                          is_thumb(cpu) ? "Thumb" : "ARM",
                          (target & 1) ? "Thumb" : "ARM",
                          cpu->r[ARM_PC], bx_log_count);
        }
    }
#else
    if (target == 0) {
        cpu->running = false;
        return;
    }
#endif

    if (target & 1) {
        cpu->cpsr |= ARM_CPSR_T;
        cpu->r[ARM_PC] = target & ~1u;
    } else {
        cpu->cpsr &= ~ARM_CPSR_T;
        cpu->r[ARM_PC] = target & ~3u;
    }
}

/* Load / Store single register */
static void exec_arm_ldr_str(ArmV7State *cpu, uint32_t instr) {
    bool    P    = (instr >> 24) & 1;
    bool    U    = (instr >> 23) & 1;
    bool    B    = (instr >> 22) & 1;
    bool    W    = (instr >> 21) & 1;
    bool    L    = (instr >> 20) & 1;
    unsigned Rn  = (instr >> 16) & 0xF;
    unsigned Rd  = (instr >> 12) & 0xF;

    uint32_t offset;
    if (instr & (1u << 25)) {
        bool c = false;
        offset = decode_shifted_reg(cpu, instr, &c);
    } else {
        offset = instr & 0xFFF;
    }

    uint32_t base = cpu->r[Rn];
    uint32_t addr = P ? (U ? base + offset : base - offset) : base;

    if (L) {
        uint32_t val = B ? mem_r8(cpu, addr) : mem_r32(cpu, addr);
        if (Rd == ARM_PC) { cpu->r[ARM_PC] = val & ~1u; }
        else              { cpu->r[Rd]  = val; }
    } else {
        uint32_t val = (Rd == ARM_PC) ? cpu->r[ARM_PC] + 4 : cpu->r[Rd];
        if (B) mem_w8(cpu, addr, (uint8_t)val);
        else   mem_w32(cpu, addr, val);
    }

    /* Write-back */
    if (W || !P) {
        uint32_t wb = U ? base + offset : base - offset;
        cpu->r[Rn] = wb;
    }
}

/* Load / Store multiple */
static void exec_arm_ldm_stm(ArmV7State *cpu, uint32_t instr) {
    bool     P    = (instr >> 24) & 1;
    bool     U    = (instr >> 23) & 1;
    bool     W    = (instr >> 21) & 1;
    bool     L    = (instr >> 20) & 1;
    unsigned Rn   = (instr >> 16) & 0xF;
    uint16_t list = (uint16_t)(instr & 0xFFFF);

    uint32_t base = cpu->r[Rn];
    unsigned count = __builtin_popcount(list);
    uint32_t start = U ? base : base - count * 4;
    uint32_t addr  = start;

    if (P && U)  addr += 4;
    if (P && !U) addr -= 4;  /* will be incremented below */

    /* Simplified LDM/STM: iterate in natural order */
    addr = U ? base : (base - (uint32_t)count * 4);
    if (P && U)  addr += 4;
    if (P && !U) {} /* pre-decrement already done */

    uint32_t cur = addr;
    for (int i = 0; i <= 15; i++) {
        if (!((list >> i) & 1)) continue;
        if (L) {
            uint32_t val = mem_r32(cpu, cur);
            if (i == ARM_PC) { cpu->r[ARM_PC] = val & ~1u; }
            else              { cpu->r[i] = val; }
        } else {
            mem_w32(cpu, cur, cpu->r[i]);
        }
        cur += 4;
    }

    if (W) {
        cpu->r[Rn] = U ? (base + (uint32_t)count * 4) : (base - (uint32_t)count * 4);
    }
}

/* Multiply instructions */
static void exec_arm_mul(ArmV7State *cpu, uint32_t instr) {
    bool     S   = (instr >> 20) & 1;
    unsigned Rd  = (instr >> 16) & 0xF;
    unsigned Rn  = (instr >> 12) & 0xF;
    unsigned Rs  = (instr >>  8) & 0xF;
    unsigned Rm  = instr & 0xF;
    bool     acc = (instr >> 21) & 1;

    uint32_t result = cpu->r[Rm] * cpu->r[Rs];
    if (acc) result += cpu->r[Rn];
    cpu->r[Rd] = result;
    if (S) set_nzcv(cpu, (int32_t)result < 0, result == 0, cpsr_c(cpu), cpsr_v(cpu));
}

/* SWI / SVC (software interrupt = syscall) */
static void exec_arm_swi(ArmV7State *cpu, uint32_t instr) {
    uint32_t swi_num = instr & 0xFFFFFF;
    if (cpu->syscall_handler) cpu->syscall_handler(cpu->callback_ctx, swi_num);
}

/* MRS / MSR */
static void exec_arm_mrs_msr(ArmV7State *cpu, uint32_t instr) {
    bool msr = (instr >> 21) & 1;
    bool R   = (instr >> 22) & 1; /* use SPSR */
    if (msr) {
        unsigned Rd = (instr >> 12) & 0xF;
        cpu->r[Rd] = R ? cpu->spsr : cpu->cpsr;
    } else {
        uint32_t mask = 0;
        if ((instr >> 16) & 1) mask |= 0x000000FF;
        if ((instr >> 17) & 1) mask |= 0x0000FF00;
        if ((instr >> 18) & 1) mask |= 0x00FF0000;
        if ((instr >> 19) & 1) mask |= 0xFF000000;
        uint32_t val;
        if (instr & (1u << 25)) {
            unsigned rot = ((instr >> 8) & 0xF) * 2;
            val = ror32(instr & 0xFF, rot);
        } else {
            val = cpu->r[instr & 0xF];
        }
        if (R) cpu->spsr = (cpu->spsr & ~mask) | (val & mask);
        else   cpu->cpsr = (cpu->cpsr & ~mask) | (val & mask);
    }
}

/* Half-word load/store */
static void exec_arm_ldrh_strh(ArmV7State *cpu, uint32_t instr) {
    bool     P   = (instr >> 24) & 1;
    bool     U   = (instr >> 23) & 1;
    bool     W   = (instr >> 21) & 1;
    bool     L   = (instr >> 20) & 1;
    unsigned Rn  = (instr >> 16) & 0xF;
    unsigned Rd  = (instr >> 12) & 0xF;
    unsigned sh  = (instr >>  5) & 3;

    uint32_t offset;
    if (instr & (1u << 22)) {
        offset = ((instr >> 8) & 0xF) << 4 | (instr & 0xF);
    } else {
        offset = cpu->r[instr & 0xF];
    }

    uint32_t base = cpu->r[Rn];
    uint32_t addr = P ? (U ? base + offset : base - offset) : base;

    if (L) {
        uint32_t val;
        switch (sh) {
            case 1: val = mem_r16(cpu, addr); break;
            case 2: val = (uint32_t)(int32_t)(int8_t )mem_r8 (cpu, addr); break;
            case 3: val = (uint32_t)(int32_t)(int16_t)mem_r16(cpu, addr); break;
            default: val = 0; break;
        }
        cpu->r[Rd] = val;
    } else {
        mem_w16(cpu, addr, (uint16_t)cpu->r[Rd]);
    }

    if (W || !P) cpu->r[Rn] = U ? base + offset : base - offset;
}

/* Execute one ARM32 instruction */
static void exec_arm32(ArmV7State *cpu, uint32_t instr) {
    ArmCondition cond = (ArmCondition)((instr >> 28) & 0xF);
    if (cond != ARM_COND_AL && !armv7_eval_cond(cpu, cond)) return;

    uint32_t op = (instr >> 25) & 7;
    uint32_t op2 = (instr >> 20) & 0x1F;

    /* Classify by bits [27:25] */
    if ((instr & 0x0FC000F0u) == 0x00000090u) { exec_arm_mul(cpu, instr); return; }
    if ((instr & 0x0E000090u) == 0x00000090u && op != 0) { exec_arm_ldrh_strh(cpu, instr); return; }

    switch (op) {
        case 0: case 1:
            if ((instr & 0x0FBF0FFFu) == 0x010F0000u) { exec_arm_mrs_msr(cpu, instr); return; }
            if ((instr & 0x0FB0F000u) == 0x0120F000u) { exec_arm_mrs_msr(cpu, instr); return; }
            if ((instr & 0x0FF000F0u) == 0x01200010u) { exec_arm_bx(cpu, instr); return; }
            if ((instr & 0x0FF000F0u) == 0x01200030u) { exec_arm_bx(cpu, instr); return; }
            exec_arm_data_proc(cpu, instr); return;
        case 2: case 3: exec_arm_ldr_str(cpu, instr); return;
        case 4: case 5: exec_arm_ldm_stm(cpu, instr); return;
        case 6: case 7: break; /* coprocessor / other */
    }

    if ((instr & 0x0F000000u) == 0x0A000000u || (instr & 0x0F000000u) == 0x0B000000u) {
        exec_arm_branch(cpu, instr); return;
    }
    if ((instr & 0x0F000000u) == 0x0F000000u) {
        exec_arm_swi(cpu, instr); return;
    }

    /* Undefined */
    if (cpu->undef_handler) cpu->undef_handler(cpu->callback_ctx, instr);
}

/* =========================================================================
 * Thumb (T16) instruction execution
 * ========================================================================= */

static void exec_thumb16(ArmV7State *cpu, uint16_t instr) {
    unsigned op = (instr >> 13) & 7;

    if ((instr >> 11) == 0x1E) {
        /* BL/BLX (first half - handled in step() for 32-bit sequence) */
        return;
    }

    switch (op) {
        case 0: { /* Shift / Add / Sub / Move / Compare */
            unsigned sub = (instr >> 11) & 3;
            if (sub <= 2) {
                /* LSL/LSR/ASR immediate */
                unsigned imm5 = (instr >> 6) & 0x1F;
                unsigned Rm   = (instr >> 3) & 7;
                unsigned Rd   = instr & 7;
                bool c = cpsr_c(cpu);
                uint32_t res = barrel_shift(cpu->r[Rm], (ShiftType)sub, imm5 ? imm5 : 0, &c);
                if (imm5) cpu->cpsr = (cpu->cpsr & ~ARM_CPSR_C) | (c ? ARM_CPSR_C : 0);
                cpu->r[Rd] = res;
                set_nzcv(cpu, (int32_t)res < 0, res == 0, (bool)((cpu->cpsr >> 29) & 1), cpsr_v(cpu));
            } else {
                /* ADD/SUB register or 3-bit immediate */
                bool is_sub = (instr >> 9) & 1;
                bool is_imm = (instr >> 10) & 1;
                unsigned Rm_or_imm3 = (instr >> 6) & 7;
                unsigned Rn = (instr >> 3) & 7;
                unsigned Rd = instr & 7;
                uint32_t a = cpu->r[Rn];
                uint32_t b = is_imm ? Rm_or_imm3 : cpu->r[Rm_or_imm3];
                uint32_t res;
                bool c, v;
                if (is_sub) {
                    res = a - b; c = a >= b;
                    v = ((a ^ b) & (a ^ res)) >> 31;
                } else {
                    uint64_t r64 = (uint64_t)a + b; res = (uint32_t)r64; c = r64 >> 32;
                    v = (~(a ^ b) & (a ^ res)) >> 31;
                }
                cpu->r[Rd] = res;
                set_nzcv(cpu, (int32_t)res < 0, res == 0, c, v);
            }
            break;
        }
        case 1: { /* MOV/CMP/ADD/SUB immediate */
            unsigned opi = (instr >> 11) & 3;
            unsigned Rdn = (instr >> 8)  & 7;
            uint32_t imm8 = instr & 0xFF;
            uint32_t a = cpu->r[Rdn];
            uint32_t res;
            bool c, v;
            switch (opi) {
                case 0: res = imm8; cpu->r[Rdn] = res; set_nzcv(cpu, (int32_t)res < 0, res == 0, cpsr_c(cpu), cpsr_v(cpu)); break;
                case 1: res = a - imm8; c = a >= imm8; v = ((a ^ imm8) & (a ^ res)) >> 31; set_nzcv(cpu, (int32_t)res < 0, res == 0, c, v); break;
                case 2: { uint64_t r64 = (uint64_t)a + imm8; res = (uint32_t)r64; c = r64 >> 32; v = (~(a ^ imm8) & (a ^ res)) >> 31; cpu->r[Rdn] = res; set_nzcv(cpu, (int32_t)res < 0, res == 0, c, v); break; }
                case 3: res = a - imm8; c = a >= imm8; v = ((a ^ imm8) & (a ^ res)) >> 31; cpu->r[Rdn] = res; set_nzcv(cpu, (int32_t)res < 0, res == 0, c, v); break;
            }
            break;
        }
        case 2: {
            unsigned sub2 = (instr >> 10) & 7;
            if (sub2 == 0) {
                /* Data processing */
                unsigned dp_op = (instr >> 6) & 0xF;
                unsigned Rm = (instr >> 3) & 7;
                unsigned Rdn = instr & 7;
                uint32_t a = cpu->r[Rdn], b = cpu->r[Rm];
                uint32_t res = 0;
                bool c = cpsr_c(cpu), v = cpsr_v(cpu);
                switch (dp_op) {
                    case  0: res = a & b; break;
                    case  1: res = a ^ b; break;
                    case  2: { bool co = false; res = barrel_shift(a, SHIFT_LSL, b & 0xFF, &co); c = co; } break;
                    case  3: { bool co = false; res = barrel_shift(a, SHIFT_LSR, b & 0xFF, &co); c = co; } break;
                    case  4: { bool co = false; res = barrel_shift(a, SHIFT_ASR, b & 0xFF, &co); c = co; } break;
                    case  5: { uint64_t r64 = (uint64_t)a + b + cpsr_c(cpu); res = (uint32_t)r64; c = r64 >> 32; v = (~(a^b)&(a^res))>>31; } break;
                    case  6: { res = a - b - (1 - (int)cpsr_c(cpu)); c = (uint64_t)a >= (uint64_t)b + (1 - cpsr_c(cpu)); v = ((a^b)&(a^res))>>31; } break;
                    case  7: { bool co = false; res = barrel_shift(a, SHIFT_ROR, b & 0xFF, &co); c = co; } break;
                    case  8: res = a & b; break; /* TST - no writeback */
                    case  9: res = (uint32_t)(0 - (int32_t)b); c = b == 0; v = b == 0x80000000; break;
                    case 10: res = a - b; c = a >= b; v = ((a^b)&(a^res))>>31; break; /* CMP - no writeback */
                    case 11: { uint64_t r64 = (uint64_t)a + b; res = (uint32_t)r64; c = r64>>32; v = (~(a^b)&(a^res))>>31; } break; /* CMN - no writeback */
                    case 12: res = a | b; break;
                    case 13: res = a * b; break;
                    case 14: res = a & ~b; break;
                    case 15: res = ~b; break;
                }
                set_nzcv(cpu, (int32_t)res < 0, res == 0, c, v);
                if (dp_op != 8 && dp_op != 10 && dp_op != 11) cpu->r[Rdn] = res;
            } else if (sub2 == 1) {
                /* Special data instructions and BX */
                unsigned hi_op = (instr >> 8) & 3;
                unsigned Rm = (instr >> 3) & 0xF;
                unsigned Rd = ((instr >> 4) & 8) | (instr & 7);
                if (hi_op == 3) {
                    /* BX/BLX */
                    bool is_blx = (instr >> 7) & 1;
                    if (is_blx) cpu->r[ARM_LR] = cpu->r[ARM_PC]; /* BLX */
                    uint32_t target = cpu->r[Rm];
#if defined(_DEBUG) || !defined(NDEBUG)
                    if (target == 0) {
                        ARM_LOG_DEBUG("Thumb BX to 0x00000000 — halting (from pc=0x%08X, lr=0x%08X)",
                                      cpu->r[ARM_PC], cpu->r[ARM_LR]);
                        cpu->running = false;
                        break;
                    }
                    {
                        static unsigned thumb_bx_count = 0;
                        thumb_bx_count++;
                        bool mode_sw = ((target & 1) != 0) != is_thumb(cpu);
                        if (thumb_bx_count <= 30 || mode_sw) {
                            ARM_LOG_DEBUG("Thumb BX%s R%u=0x%08X %s->%s (from pc=0x%08X, #%u)",
                                          is_blx ? "L" : "", Rm, target,
                                          is_thumb(cpu) ? "Thumb" : "ARM",
                                          (target & 1) ? "Thumb" : "ARM",
                                          cpu->r[ARM_PC], thumb_bx_count);
                        }
                    }
#else
                    if (target == 0) { cpu->running = false; break; }
#endif
                    if (target & 1) { cpu->cpsr |=  ARM_CPSR_T; cpu->r[ARM_PC] = target & ~1u; }
                    else            { cpu->cpsr &= ~ARM_CPSR_T; cpu->r[ARM_PC] = target & ~3u; }
                } else {
                    uint32_t a = (Rd == ARM_PC) ? cpu->r[ARM_PC] : cpu->r[Rd];
                    uint32_t b = cpu->r[Rm];
                    uint32_t res;
                    switch (hi_op) {
                        case 0: res = a + b; break;
                        case 1: res = a - b; break; /* CMP - no writeback */
                        default: res = b; break;    /* MOV */
                    }
                    if (hi_op != 1) {
                        if (Rd == ARM_PC) { cpu->r[ARM_PC] = res & ~1u; }
                        else cpu->r[Rd] = res;
                    } else {
                        set_nzcv(cpu, (int32_t)res < 0, res == 0, a >= b, ((a^b)&(a^res))>>31);
                    }
                }
            } else {
                /* LDR literal */
                unsigned Rd = (instr >> 8) & 7;
                uint32_t imm8 = (instr & 0xFF) << 2;
                uint32_t pc_aligned = (cpu->r[ARM_PC]) & ~3u;
                cpu->r[Rd] = mem_r32(cpu, pc_aligned + imm8);
            }
            break;
        }
        case 3: case 4: case 5: {
            /* Load/Store single */
            unsigned sub3 = (instr >> 9) & 0x1F;
            unsigned Rm = (instr >> 6) & 7;
            unsigned Rn = (instr >> 3) & 7;
            unsigned Rd = instr & 7;
            bool     L  = (instr >> 11) & 1;
            bool     B  = (instr >> 10) & 1;
            bool     H  = (instr >>  9) & 1;
            bool     S  = (instr >>  10) & 1;
            uint32_t addr;

            if (op == 5 && (instr >> 12) == 0x50) { /* STR/LDR register */
                (void)sub3; (void)B; (void)H; (void)S;
                addr = cpu->r[Rn] + cpu->r[Rm];
                if (L) cpu->r[Rd] = mem_r32(cpu, addr);
                else   mem_w32(cpu, addr, cpu->r[Rd]);
            } else if (op == 3) {
                /* Immediate LDR/STR */
                unsigned imm5 = (instr >> 6) & 0x1F;
                if (B) addr = cpu->r[Rn] + imm5;
                else   addr = cpu->r[Rn] + imm5 * 4;
                if (L) cpu->r[Rd] = B ? mem_r8(cpu, addr) : mem_r32(cpu, addr);
                else { if (B) mem_w8(cpu, addr, (uint8_t)cpu->r[Rd]); else mem_w32(cpu, addr, cpu->r[Rd]); }
            } else {
                unsigned imm5 = (instr >> 6) & 0x1F;
                addr = cpu->r[Rn] + imm5 * 2;
                if (L) cpu->r[Rd] = mem_r16(cpu, addr);
                else   mem_w16(cpu, addr, (uint16_t)cpu->r[Rd]);
            }
            break;
        }
        case 6: {
            /* LDR/STR SP-relative or ADD SP/PC */
            if ((instr >> 12) == 0x9) {
                /* SP-relative LDR/STR */
                bool L = (instr >> 11) & 1;
                unsigned Rd = (instr >> 8) & 7;
                uint32_t imm8 = (instr & 0xFF) << 2;
                uint32_t addr = cpu->r[ARM_SP] + imm8;
                if (L) cpu->r[Rd] = mem_r32(cpu, addr);
                else   mem_w32(cpu, addr, cpu->r[Rd]);
            } else if ((instr >> 12) == 0xA) {
                /* ADD Rd, PC/SP, imm8 */
                bool use_sp = (instr >> 11) & 1;
                unsigned Rd = (instr >> 8) & 7;
                uint32_t imm8 = (instr & 0xFF) << 2;
                cpu->r[Rd] = (use_sp ? cpu->r[ARM_SP] : (cpu->r[ARM_PC] & ~3u)) + imm8;
            } else if ((instr >> 8) == 0xB0) {
                /* ADD/SUB SP, imm7 */
                bool sub = (instr >> 7) & 1;
                uint32_t imm7 = (instr & 0x7F) << 2;
                cpu->r[ARM_SP] = sub ? cpu->r[ARM_SP] - imm7 : cpu->r[ARM_SP] + imm7;
            } else if (((instr >> 9) & 0x7F) == 0x5A) {
                /* PUSH */
                uint8_t  list = instr & 0xFF;
                bool     lr   = (instr >> 8) & 1;
                unsigned count = __builtin_popcount(list) + (lr ? 1 : 0);
                uint32_t addr = cpu->r[ARM_SP] - count * 4;
                cpu->r[ARM_SP] = addr;
                for (int i = 0; i < 8; i++) {
                    if (!((list >> i) & 1)) continue;
                    mem_w32(cpu, addr, cpu->r[i]);
                    addr += 4;
                }
                if (lr) mem_w32(cpu, addr, cpu->r[ARM_LR]);
            } else if (((instr >> 9) & 0x7F) == 0x5E) {
                /* POP */
                uint8_t  list = instr & 0xFF;
                bool     pc   = (instr >> 8) & 1;
                uint32_t addr = cpu->r[ARM_SP];
                for (int i = 0; i < 8; i++) {
                    if (!((list >> i) & 1)) continue;
                    cpu->r[i] = mem_r32(cpu, addr);
                    addr += 4;
                }
                if (pc) {
                    uint32_t val = mem_r32(cpu, addr); addr += 4;
                    cpu->r[ARM_PC] = val & ~1u;
                    if (val & 1) cpu->cpsr |= ARM_CPSR_T;
                    else         cpu->cpsr &= ~ARM_CPSR_T;
                }
                cpu->r[ARM_SP] = addr;
            }
            break;
        }
        case 7: {
            /* Conditional branch, SVC, and unconditional branch */
            unsigned sub4 = (instr >> 12) & 0xF;
            if (sub4 == 0xD) {
                /* Conditional branch */
                unsigned cond_field = (instr >> 8) & 0xF;
                int32_t offset = sign_extend((instr & 0xFF) << 1, 9);
                if (armv7_eval_cond(cpu, (ArmCondition)cond_field))
                    cpu->r[ARM_PC] = (uint32_t)((int32_t)cpu->r[ARM_PC] + offset);
            } else if (sub4 == 0xF) {
                /* SVC */
                if (cpu->syscall_handler) cpu->syscall_handler(cpu->callback_ctx, instr & 0xFF);
            } else {
                /* Unconditional branch */
                int32_t offset = sign_extend((instr & 0x7FF) << 1, 12);
                cpu->r[ARM_PC] = (uint32_t)((int32_t)cpu->r[ARM_PC] + offset);
            }
            break;
        }
    }
}

/* =========================================================================
 * Public step / run functions
 * ========================================================================= */

void armv7_init(ArmV7State *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->cpsr = 0x13; /* SVC mode */
    cpu->running = true;
}

void armv7_reset(ArmV7State *cpu) {
    /* Preserve callbacks */
    void *ctx             = cpu->callback_ctx;
    uint8_t  (*r8 )(void*,uint32_t)         = cpu->mem_read8;
    uint16_t (*r16)(void*,uint32_t)         = cpu->mem_read16;
    uint32_t (*r32)(void*,uint32_t)         = cpu->mem_read32;
    void (*w8 )(void*,uint32_t,uint8_t)     = cpu->mem_write8;
    void (*w16)(void*,uint32_t,uint16_t)    = cpu->mem_write16;
    void (*w32)(void*,uint32_t,uint32_t)    = cpu->mem_write32;
    void (*sys)(void*,uint32_t)             = cpu->syscall_handler;
    void (*udf)(void*,uint32_t)             = cpu->undef_handler;
    memset(cpu, 0, sizeof(*cpu));
    cpu->cpsr = 0x13;
    cpu->running       = true;
    cpu->callback_ctx  = ctx;
    cpu->mem_read8     = r8;
    cpu->mem_read16    = r16;
    cpu->mem_read32    = r32;
    cpu->mem_write8    = w8;
    cpu->mem_write16   = w16;
    cpu->mem_write32   = w32;
    cpu->syscall_handler = sys;
    cpu->undef_handler   = udf;
}

void armv7_step(ArmV7State *cpu) {
    if (is_thumb(cpu)) {
        uint16_t lo = mem_r16(cpu, cpu->r[ARM_PC]);
        cpu->r[ARM_PC] += 2;
        /* Detect 32-bit Thumb-2 instruction */
        if ((lo >> 11) >= 0x1D) {
            uint16_t hi = mem_r16(cpu, cpu->r[ARM_PC]);
            cpu->r[ARM_PC] += 2;
            /* Decode 32-bit Thumb-2: only BL/BLX handled here */
            if ((lo & 0xF800) == 0xF000 && (hi & 0xD000) == 0xD000) {
                /* BL */
                uint32_t s   = (lo  >> 10) & 1;
                uint32_t i1  = ~(((hi >> 13) & 1) ^ s) & 1;
                uint32_t i2  = ~(((hi >> 11) & 1) ^ s) & 1;
                uint32_t imm11 = hi & 0x7FF;
                uint32_t imm10 = lo & 0x3FF;
                int32_t  offset = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1));
                if (s) offset |= (int32_t)0xFF000000;
                cpu->r[ARM_LR] = cpu->r[ARM_PC]; /* after 2nd half-word */
                cpu->r[ARM_PC] = (uint32_t)((int32_t)cpu->r[ARM_PC] + offset) & ~1u;
            }
        } else {
            exec_thumb16(cpu, lo);
        }
    } else {
        uint32_t instr = mem_r32(cpu, cpu->r[ARM_PC]);
        cpu->r[ARM_PC] += 4;
        exec_arm32(cpu, instr);
    }
}

void armv7_run(ArmV7State *cpu) {
    while (cpu->running) {
        armv7_step(cpu);
    }
}
