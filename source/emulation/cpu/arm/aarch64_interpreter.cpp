/*
 * Boxedwine Android - AArch64 CPU Interpreter Implementation
 *
 * Full A64 instruction set emulator supporting:
 *   - Data processing (immediate and register)
 *   - Load/store (immediate, register offset, pre/post-index, pair)
 *   - Branches (unconditional, conditional, compare-and-branch, test-and-branch)
 *   - System instructions (SVC, NOP, MSR/MRS)
 *   - Conditional select and compare
 *   - Bit manipulation (CLZ, CLS, RBIT, REV)
 *
 * Reference: ARM Architecture Reference Manual for A-profile architecture
 */

#include "aarch64_interpreter.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* MSVC does not provide __builtin_popcount; use intrinsics instead */
#ifdef _MSC_VER
#include <intrin.h>
#define __builtin_popcount(x)   __popcnt(x)
#define __builtin_popcountll(x) ((int)__popcnt64(x))
#endif

/* =========================================================================
 * Helpers
 * ========================================================================= */

static inline uint32_t nzcv_n(const AArch64State *cpu) { return (cpu->nzcv >> 31) & 1; }
static inline uint32_t nzcv_z(const AArch64State *cpu) { return (cpu->nzcv >> 30) & 1; }
static inline uint32_t nzcv_c(const AArch64State *cpu) { return (cpu->nzcv >> 29) & 1; }
static inline uint32_t nzcv_v(const AArch64State *cpu) { return (cpu->nzcv >> 28) & 1; }

static inline void set_nzcv(AArch64State *cpu, bool n, bool z, bool c, bool v) {
    cpu->nzcv = (n ? NZCV_N : 0)
              | (z ? NZCV_Z : 0)
              | (c ? NZCV_C : 0)
              | (v ? NZCV_V : 0);
}

/* Sign-extend a value from 'bits' bits to 64 bits */
static inline int64_t sign_extend64(uint64_t val, unsigned bits) {
    unsigned shift = 64 - bits;
    return (int64_t)(val << shift) >> shift;
}

/* Rotate right 64-bit */
static inline uint64_t ror64(uint64_t val, unsigned shift) {
    shift &= 63;
    if (!shift) return val;
    return (val >> shift) | (val << (64u - shift));
}

/* Rotate right 32-bit */
static inline uint32_t ror32(uint32_t val, unsigned shift) {
    shift &= 31;
    if (!shift) return val;
    return (val >> shift) | (val << (32u - shift));
}

/* Count leading zeros (64-bit) */
static inline unsigned clz64(uint64_t val) {
    if (val == 0) return 64;
    unsigned n = 0;
    if ((val & 0xFFFFFFFF00000000ULL) == 0) { n += 32; val <<= 32; }
    if ((val & 0xFFFF000000000000ULL) == 0) { n += 16; val <<= 16; }
    if ((val & 0xFF00000000000000ULL) == 0) { n +=  8; val <<=  8; }
    if ((val & 0xF000000000000000ULL) == 0) { n +=  4; val <<=  4; }
    if ((val & 0xC000000000000000ULL) == 0) { n +=  2; val <<=  2; }
    if ((val & 0x8000000000000000ULL) == 0) { n +=  1; }
    return n;
}

/* Count leading zeros (32-bit) */
static inline unsigned clz32(uint32_t val) {
    if (val == 0) return 32;
    unsigned n = 0;
    if ((val & 0xFFFF0000u) == 0) { n += 16; val <<= 16; }
    if ((val & 0xFF000000u) == 0) { n +=  8; val <<=  8; }
    if ((val & 0xF0000000u) == 0) { n +=  4; val <<=  4; }
    if ((val & 0xC0000000u) == 0) { n +=  2; val <<=  2; }
    if ((val & 0x80000000u) == 0) { n +=  1; }
    return n;
}

/* Count leading sign bits (64-bit): number of consecutive bits matching the sign bit, minus 1 */
static inline unsigned cls64(uint64_t val) {
    if (val == 0 || val == ~(uint64_t)0) return 63;
    uint64_t x = ((int64_t)val < 0) ? ~val : val;
    return clz64(x) - 1;
}

static inline unsigned cls32(uint32_t val) {
    if (val == 0 || val == ~(uint32_t)0) return 31;
    uint32_t x = ((int32_t)val < 0) ? ~val : val;
    return clz32(x) - 1;
}

/* Reverse bits (64-bit) */
static inline uint64_t rbit64(uint64_t val) {
    uint64_t r = 0;
    for (int i = 0; i < 64; i++) {
        r |= ((val >> i) & 1) << (63 - i);
    }
    return r;
}

static inline uint32_t rbit32(uint32_t val) {
    uint32_t r = 0;
    for (int i = 0; i < 32; i++) {
        r |= ((val >> i) & 1) << (31 - i);
    }
    return r;
}

/* Reverse bytes */
static inline uint64_t rev64(uint64_t val) {
    val = ((val & 0x00FF00FF00FF00FFULL) <<  8) | ((val & 0xFF00FF00FF00FF00ULL) >>  8);
    val = ((val & 0x0000FFFF0000FFFFULL) << 16) | ((val & 0xFFFF0000FFFF0000ULL) >> 16);
    val = (val << 32) | (val >> 32);
    return val;
}

static inline uint32_t rev32(uint32_t val) {
    val = ((val & 0x00FF00FFu) <<  8) | ((val & 0xFF00FF00u) >>  8);
    val = (val << 16) | (val >> 16);
    return val;
}

static inline uint32_t rev16_in_32(uint32_t val) {
    return ((val & 0x00FF00FFu) << 8) | ((val & 0xFF00FF00u) >> 8);
}

/* Set NZCV for 32-bit ADD result */
static inline void set_flags_add32(AArch64State *cpu, uint32_t a, uint32_t b, uint32_t result) {
    uint64_t r64 = (uint64_t)a + (uint64_t)b;
    bool c = (r64 >> 32) != 0;
    bool v = ((~(a ^ b) & (a ^ result)) >> 31) != 0;
    set_nzcv(cpu, (int32_t)result < 0, result == 0, c, v);
}

/* Set NZCV for 64-bit ADD result */
static inline void set_flags_add64(AArch64State *cpu, uint64_t a, uint64_t b, uint64_t result) {
    bool c = (result < a); /* unsigned overflow */
    bool v = (((~(a ^ b)) & (a ^ result)) >> 63) != 0;
    set_nzcv(cpu, (int64_t)result < 0, result == 0, c, v);
}

/* Set NZCV for 32-bit SUB result (a - b) */
static inline void set_flags_sub32(AArch64State *cpu, uint32_t a, uint32_t b, uint32_t result) {
    bool c = (a >= b); /* no borrow */
    bool v = (((a ^ b) & (a ^ result)) >> 31) != 0;
    set_nzcv(cpu, (int32_t)result < 0, result == 0, c, v);
}

/* Set NZCV for 64-bit SUB result (a - b) */
static inline void set_flags_sub64(AArch64State *cpu, uint64_t a, uint64_t b, uint64_t result) {
    bool c = (a >= b);
    bool v = (((a ^ b) & (a ^ result)) >> 63) != 0;
    set_nzcv(cpu, (int64_t)result < 0, result == 0, c, v);
}

/* =========================================================================
 * Condition evaluation
 * ========================================================================= */

static bool eval_cond(const AArch64State *cpu, unsigned cond) {
    bool n = nzcv_n(cpu), z = nzcv_z(cpu), c = nzcv_c(cpu), v = nzcv_v(cpu);
    switch ((AArch64Condition)(cond & 0xE)) {
        case AARCH64_COND_EQ: return  z;
        case AARCH64_COND_CS: return  c;
        case AARCH64_COND_MI: return  n;
        case AARCH64_COND_VS: return  v;
        case AARCH64_COND_HI: return  c && !z;
        case AARCH64_COND_GE: return  n == v;
        case AARCH64_COND_GT: return !z && (n == v);
        case AARCH64_COND_AL: return true;
        default: return true;
    }
}

static bool aarch64_eval_cond(const AArch64State *cpu, unsigned cond) {
    bool result = eval_cond(cpu, cond);
    /* Invert for odd condition codes (NE, CC, PL, VC, LS, LT, LE) */
    if ((cond & 1) && cond != 0xF)
        result = !result;
    return result;
}

/* =========================================================================
 * Register access helpers
 * ========================================================================= */

/* Read GP register: reg 31 = zero register (XZR/WZR) */
static inline uint64_t reg_x(const AArch64State *cpu, unsigned reg) {
    if (reg == 31) return 0;
    return cpu->x[reg];
}

/* Read GP register: reg 31 = SP */
static inline uint64_t reg_x_or_sp(const AArch64State *cpu, unsigned reg) {
    if (reg == 31) return cpu->sp;
    return cpu->x[reg];
}

/* Write GP register 64-bit: reg 31 = zero register (write discarded) */
static inline void write_x(AArch64State *cpu, unsigned reg, uint64_t val) {
    if (reg == 31) return;
    cpu->x[reg] = val;
}

/* Write GP register 64-bit: reg 31 = SP */
static inline void write_x_or_sp(AArch64State *cpu, unsigned reg, uint64_t val) {
    if (reg == 31) { cpu->sp = val; return; }
    cpu->x[reg] = val;
}

/* Write GP register 32-bit (zero-extends to 64 bits): reg 31 = zero register */
static inline void write_w(AArch64State *cpu, unsigned reg, uint32_t val) {
    if (reg == 31) return;
    cpu->x[reg] = (uint64_t)val; /* zero-extended */
}

/* Write GP register 32-bit (zero-extends to 64 bits): reg 31 = SP */
static inline void write_w_or_sp(AArch64State *cpu, unsigned reg, uint32_t val) {
    if (reg == 31) { cpu->sp = (uint64_t)val; return; }
    cpu->x[reg] = (uint64_t)val;
}

/* =========================================================================
 * Memory helpers
 * ========================================================================= */

static inline uint8_t  mem_r8 (AArch64State *cpu, uint64_t a) { return cpu->mem_read8 (cpu->callback_ctx, a); }
static inline uint16_t mem_r16(AArch64State *cpu, uint64_t a) { return cpu->mem_read16(cpu->callback_ctx, a); }
static inline uint32_t mem_r32(AArch64State *cpu, uint64_t a) { return cpu->mem_read32(cpu->callback_ctx, a); }
static inline uint64_t mem_r64(AArch64State *cpu, uint64_t a) { return cpu->mem_read64(cpu->callback_ctx, a); }
static inline void mem_w8 (AArch64State *cpu, uint64_t a, uint8_t  v) { cpu->mem_write8 (cpu->callback_ctx, a, v); }
static inline void mem_w16(AArch64State *cpu, uint64_t a, uint16_t v) { cpu->mem_write16(cpu->callback_ctx, a, v); }
static inline void mem_w32(AArch64State *cpu, uint64_t a, uint32_t v) { cpu->mem_write32(cpu->callback_ctx, a, v); }
static inline void mem_w64(AArch64State *cpu, uint64_t a, uint64_t v) { cpu->mem_write64(cpu->callback_ctx, a, v); }

/* =========================================================================
 * Logical immediate decoder
 * ========================================================================= */

/* Decode an A64 logical immediate value from N:immr:imms fields.
 * Returns true on success and writes the bitmask to *result. */
static bool decode_bitmask(unsigned N, unsigned immr, unsigned imms, bool is64, uint64_t *result) {
    unsigned len;
    /* Find highest set bit in N:~imms */
    unsigned combined = (N << 6) | (~imms & 0x3F);
    if (combined == 0) return false;
    /* Compute highest set bit position */
    len = 0;
    {
        unsigned tmp = combined;
        while (tmp >>= 1) len++;
    }

    if (len < 1) return false;
    unsigned esize = 1u << len;
    unsigned levels = esize - 1;
    unsigned s = imms & levels;
    unsigned r = immr & levels;

    if (s == levels) return false; /* all-ones element reserved */

    uint64_t welem = ((uint64_t)1 << (s + 1)) - 1;
    /* Rotate right within the element */
    if (r) {
        welem = ((welem >> r) | (welem << (esize - r))) & (((uint64_t)1 << esize) - 1);
    }

    /* Replicate the element across 64 bits */
    uint64_t mask = 0;
    for (unsigned i = 0; i < 64; i += esize) {
        mask |= welem << i;
    }

    if (!is64) mask &= 0xFFFFFFFFULL;
    *result = mask;
    return true;
}

/* =========================================================================
 * Shift helpers for register operands
 * ========================================================================= */

static inline uint64_t shift_reg64(uint64_t val, unsigned shift_type, unsigned amount) {
    if (amount == 0) return val;
    switch (shift_type) {
        case 0: return val << amount;                         /* LSL */
        case 1: return val >> amount;                         /* LSR */
        case 2: return (uint64_t)((int64_t)val >> amount);    /* ASR */
        case 3: return ror64(val, amount);                     /* ROR */
    }
    return val;
}

static inline uint32_t shift_reg32(uint32_t val, unsigned shift_type, unsigned amount) {
    if (amount == 0) return val;
    switch (shift_type) {
        case 0: return val << amount;
        case 1: return val >> amount;
        case 2: return (uint32_t)((int32_t)val >> amount);
        case 3: return ror32(val, amount);
    }
    return val;
}

/* =========================================================================
 * Unhandled instruction
 * ========================================================================= */

static void do_undef(AArch64State *cpu, uint32_t instr) {
    if (cpu->undef_handler) {
        cpu->undef_handler(cpu->callback_ctx, instr);
    } else {
        fprintf(stderr, "aarch64: undefined instruction 0x%08X at PC=0x%016llX\n",
                instr, (unsigned long long)(cpu->pc - 4));
        cpu->running = false;
    }
}

/* =========================================================================
 * Data Processing -- Immediate
 * ========================================================================= */

/* ADD/SUB immediate (with optional shift) */
static void exec_add_sub_imm(AArch64State *cpu, uint32_t instr) {
    unsigned sf   = (instr >> 31) & 1;
    unsigned op   = (instr >> 30) & 1; /* 0=ADD, 1=SUB */
    unsigned S    = (instr >> 29) & 1;
    unsigned sh   = (instr >> 22) & 1; /* 0=no shift, 1=LSL#12 */
    uint64_t imm12 = (instr >> 10) & 0xFFF;
    unsigned Rn   = (instr >>  5) & 0x1F;
    unsigned Rd   = instr & 0x1F;

    if (sh) imm12 <<= 12;

    /* Rn and Rd use SP encoding (reg 31 = SP) */
    uint64_t a = reg_x_or_sp(cpu, Rn);

    if (sf) {
        /* 64-bit */
        uint64_t result;
        if (op == 0) {
            result = a + imm12;
            if (S) set_flags_add64(cpu, a, imm12, result);
        } else {
            result = a - imm12;
            if (S) set_flags_sub64(cpu, a, imm12, result);
        }
        if (S) write_x(cpu, Rd, result);      /* ADDS/SUBS: Rd=31 is ZR */
        else   write_x_or_sp(cpu, Rd, result); /* ADD/SUB: Rd=31 is SP */
    } else {
        /* 32-bit */
        uint32_t a32 = (uint32_t)a;
        uint32_t imm = (uint32_t)imm12;
        uint32_t result;
        if (op == 0) {
            result = a32 + imm;
            if (S) set_flags_add32(cpu, a32, imm, result);
        } else {
            result = a32 - imm;
            if (S) set_flags_sub32(cpu, a32, imm, result);
        }
        if (S) write_w(cpu, Rd, result);
        else   write_w_or_sp(cpu, Rd, result);
    }
}

/* Move wide: MOVZ / MOVK / MOVN */
static void exec_mov_wide(AArch64State *cpu, uint32_t instr) {
    unsigned sf  = (instr >> 31) & 1;
    unsigned opc = (instr >> 29) & 3;
    unsigned hw  = (instr >> 21) & 3;
    uint64_t imm16 = (instr >> 5) & 0xFFFF;
    unsigned Rd  = instr & 0x1F;

    unsigned shift = hw * 16;

    switch (opc) {
        case 0: { /* MOVN */
            uint64_t val = ~(imm16 << shift);
            if (!sf) val = (uint32_t)val;
            write_x(cpu, Rd, val);
            break;
        }
        case 2: { /* MOVZ */
            uint64_t val = imm16 << shift;
            if (!sf) val = (uint32_t)val;
            write_x(cpu, Rd, val);
            break;
        }
        case 3: { /* MOVK */
            uint64_t val = reg_x(cpu, Rd);
            uint64_t mask = (uint64_t)0xFFFF << shift;
            val = (val & ~mask) | (imm16 << shift);
            if (!sf) val = (uint32_t)val;
            write_x(cpu, Rd, val);
            break;
        }
        default:
            do_undef(cpu, instr);
            break;
    }
}

/* Logical immediate */
static void exec_logical_imm(AArch64State *cpu, uint32_t instr) {
    unsigned sf   = (instr >> 31) & 1;
    unsigned opc  = (instr >> 29) & 3;
    unsigned N    = (instr >> 22) & 1;
    unsigned immr = (instr >> 16) & 0x3F;
    unsigned imms = (instr >> 10) & 0x3F;
    unsigned Rn   = (instr >>  5) & 0x1F;
    unsigned Rd   = instr & 0x1F;

    uint64_t bitmask;
    if (!decode_bitmask(N, immr, imms, sf != 0, &bitmask)) {
        do_undef(cpu, instr);
        return;
    }

    uint64_t a = reg_x(cpu, Rn);
    uint64_t result;

    switch (opc) {
        case 0: result = a & bitmask; break;  /* AND */
        case 1: result = a | bitmask; break;  /* ORR */
        case 2: result = a ^ bitmask; break;  /* EOR */
        case 3: result = a & bitmask; break;  /* ANDS */
        default: result = 0; break;
    }

    if (!sf) result = (uint32_t)result;

    if (opc == 3) {
        /* ANDS - set flags */
        if (sf)
            set_nzcv(cpu, (int64_t)result < 0, result == 0, false, false);
        else
            set_nzcv(cpu, ((int32_t)(uint32_t)result) < 0, (uint32_t)result == 0, false, false);
        write_x(cpu, Rd, result);  /* Rd=31 is ZR */
    } else {
        /* AND/ORR/EOR: Rd=31 is SP */
        write_x_or_sp(cpu, Rd, result);
    }
}

/* PC-relative addressing: ADR / ADRP */
static void exec_adr(AArch64State *cpu, uint32_t instr) {
    unsigned op   = (instr >> 31) & 1;
    uint64_t immlo = (instr >> 29) & 3;
    uint64_t immhi = (instr >> 5) & 0x7FFFF;
    unsigned Rd   = instr & 0x1F;

    int64_t imm = (int64_t)sign_extend64((immhi << 2) | immlo, 21);
    uint64_t base = cpu->pc - 4; /* PC of this instruction */

    if (op) {
        /* ADRP: page-relative */
        imm <<= 12;
        base &= ~((uint64_t)0xFFF);
    }

    write_x(cpu, Rd, (uint64_t)((int64_t)base + imm));
}

/* Bitfield: SBFM / BFM / UBFM */
static void exec_bitfield(AArch64State *cpu, uint32_t instr) {
    unsigned sf   = (instr >> 31) & 1;
    unsigned opc  = (instr >> 29) & 3;
    unsigned N    = (instr >> 22) & 1;
    unsigned immr = (instr >> 16) & 0x3F;
    unsigned imms = (instr >> 10) & 0x3F;
    unsigned Rn   = (instr >>  5) & 0x1F;
    unsigned Rd   = instr & 0x1F;

    (void)N;
    unsigned regsize = sf ? 64 : 32;
    uint64_t src = reg_x(cpu, Rn);
    uint64_t dst = reg_x(cpu, Rd);
    uint64_t result;

    if (imms >= immr) {
        /* Extract bitfield: bits [imms:immr] placed at [imms-immr:0] */
        unsigned width = imms - immr + 1;
        uint64_t mask = ((uint64_t)1 << width) - 1;
        uint64_t extracted = (src >> immr) & mask;

        switch (opc) {
            case 0: /* SBFM: sign-extend */
                result = (uint64_t)sign_extend64(extracted, width);
                break;
            case 1: /* BFM: insert into Rd, keeping other bits */
                result = (dst & ~mask) | extracted;
                break;
            case 2: /* UBFM: zero-extend */
                result = extracted;
                break;
            default:
                do_undef(cpu, instr);
                return;
        }
    } else {
        /* Insert bitfield: bits [imms:0] of src placed at [regsize-immr+imms : regsize-immr] */
        unsigned width = imms + 1;
        uint64_t mask = ((uint64_t)1 << width) - 1;
        uint64_t bits = src & mask;
        unsigned pos = regsize - immr;
        uint64_t placed = bits << pos;
        uint64_t placed_mask = mask << pos;

        switch (opc) {
            case 0: { /* SBFM: sign-extend the rotated field */
                /* ROR(src, immr) with sign extension from bit imms */
                uint64_t rotated;
                if (sf)
                    rotated = ror64(src, immr);
                else
                    rotated = ror32((uint32_t)src, immr);
                /* Sign extend from bit imms */
                result = (uint64_t)sign_extend64(rotated, imms + 1);
                break;
            }
            case 1: /* BFM: insert */
                result = (dst & ~placed_mask) | placed;
                break;
            case 2: /* UBFM: zero the rest */
                result = placed;
                break;
            default:
                do_undef(cpu, instr);
                return;
        }
    }

    if (!sf) result = (uint32_t)result;
    write_x(cpu, Rd, result);
}

/* =========================================================================
 * Data Processing -- Register
 * ========================================================================= */

/* ADD/SUB shifted register */
static void exec_add_sub_shifted(AArch64State *cpu, uint32_t instr) {
    unsigned sf    = (instr >> 31) & 1;
    unsigned op    = (instr >> 30) & 1; /* 0=ADD, 1=SUB */
    unsigned S     = (instr >> 29) & 1;
    unsigned shift = (instr >> 22) & 3;
    unsigned Rm    = (instr >> 16) & 0x1F;
    unsigned imm6  = (instr >> 10) & 0x3F;
    unsigned Rn    = (instr >>  5) & 0x1F;
    unsigned Rd    = instr & 0x1F;

    if (sf) {
        uint64_t a = reg_x(cpu, Rn);
        uint64_t b = shift_reg64(reg_x(cpu, Rm), shift, imm6);
        uint64_t result;
        if (op == 0) {
            result = a + b;
            if (S) set_flags_add64(cpu, a, b, result);
        } else {
            result = a - b;
            if (S) set_flags_sub64(cpu, a, b, result);
        }
        write_x(cpu, Rd, result);
    } else {
        uint32_t a = (uint32_t)reg_x(cpu, Rn);
        uint32_t b = shift_reg32((uint32_t)reg_x(cpu, Rm), shift, imm6);
        uint32_t result;
        if (op == 0) {
            result = a + b;
            if (S) set_flags_add32(cpu, a, b, result);
        } else {
            result = a - b;
            if (S) set_flags_sub32(cpu, a, b, result);
        }
        write_w(cpu, Rd, result);
    }
}

/* Logical shifted register: AND/ORR/EOR/BIC/ORN/EON/ANDS/BICS */
static void exec_logical_shifted(AArch64State *cpu, uint32_t instr) {
    unsigned sf    = (instr >> 31) & 1;
    unsigned opc   = (instr >> 29) & 3;
    unsigned shift = (instr >> 22) & 3;
    unsigned N     = (instr >> 21) & 1; /* invert second source */
    unsigned Rm    = (instr >> 16) & 0x1F;
    unsigned imm6  = (instr >> 10) & 0x3F;
    unsigned Rn    = (instr >>  5) & 0x1F;
    unsigned Rd    = instr & 0x1F;

    uint64_t a, b, result;

    if (sf) {
        a = reg_x(cpu, Rn);
        b = shift_reg64(reg_x(cpu, Rm), shift, imm6);
        if (N) b = ~b;

        switch (opc) {
            case 0: result = a & b; break;  /* AND / BIC */
            case 1: result = a | b; break;  /* ORR / ORN */
            case 2: result = a ^ b; break;  /* EOR / EON */
            case 3: result = a & b; break;  /* ANDS / BICS */
            default: result = 0; break;
        }
        if (opc == 3) {
            set_nzcv(cpu, (int64_t)result < 0, result == 0, false, false);
        }
        write_x(cpu, Rd, result);
    } else {
        a = (uint32_t)reg_x(cpu, Rn);
        b = shift_reg32((uint32_t)reg_x(cpu, Rm), shift, imm6);
        if (N) b = (uint32_t)~b;

        uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
        uint32_t res32;
        switch (opc) {
            case 0: res32 = a32 & b32; break;
            case 1: res32 = a32 | b32; break;
            case 2: res32 = a32 ^ b32; break;
            case 3: res32 = a32 & b32; break;
            default: res32 = 0; break;
        }
        if (opc == 3) {
            set_nzcv(cpu, (int32_t)res32 < 0, res32 == 0, false, false);
        }
        write_w(cpu, Rd, res32);
    }
}

/* Multiply: MADD / MSUB */
static void exec_madd_msub(AArch64State *cpu, uint32_t instr) {
    unsigned sf = (instr >> 31) & 1;
    unsigned Rm = (instr >> 16) & 0x1F;
    unsigned o0 = (instr >> 15) & 1; /* 0=MADD, 1=MSUB */
    unsigned Ra = (instr >> 10) & 0x1F;
    unsigned Rn = (instr >>  5) & 0x1F;
    unsigned Rd = instr & 0x1F;

    if (sf) {
        uint64_t a = reg_x(cpu, Rn);
        uint64_t b = reg_x(cpu, Rm);
        uint64_t addend = reg_x(cpu, Ra);
        uint64_t result = o0 ? (addend - a * b) : (addend + a * b);
        write_x(cpu, Rd, result);
    } else {
        uint32_t a = (uint32_t)reg_x(cpu, Rn);
        uint32_t b = (uint32_t)reg_x(cpu, Rm);
        uint32_t addend = (uint32_t)reg_x(cpu, Ra);
        uint32_t result = o0 ? (addend - a * b) : (addend + a * b);
        write_w(cpu, Rd, result);
    }
}

/* Division: SDIV / UDIV */
static void exec_div(AArch64State *cpu, uint32_t instr) {
    unsigned sf = (instr >> 31) & 1;
    unsigned Rm = (instr >> 16) & 0x1F;
    unsigned o1 = (instr >> 10) & 1; /* 1=SDIV, 0=UDIV */
    unsigned Rn = (instr >>  5) & 0x1F;
    unsigned Rd = instr & 0x1F;

    if (sf) {
        uint64_t a = reg_x(cpu, Rn);
        uint64_t b = reg_x(cpu, Rm);
        uint64_t result;
        if (b == 0) {
            result = 0;
        } else if (o1) {
            int64_t sa = (int64_t)a, sb = (int64_t)b;
            if (sa == INT64_MIN && sb == -1) result = (uint64_t)sa;
            else result = (uint64_t)(sa / sb);
        } else {
            result = a / b;
        }
        write_x(cpu, Rd, result);
    } else {
        uint32_t a = (uint32_t)reg_x(cpu, Rn);
        uint32_t b = (uint32_t)reg_x(cpu, Rm);
        uint32_t result;
        if (b == 0) {
            result = 0;
        } else if (o1) {
            int32_t sa = (int32_t)a, sb = (int32_t)b;
            if (sa == INT32_MIN && sb == -1) result = (uint32_t)sa;
            else result = (uint32_t)(sa / sb);
        } else {
            result = a / b;
        }
        write_w(cpu, Rd, result);
    }
}

/* Variable shift: LSLV / LSRV / ASRV / RORV */
static void exec_shift_reg(AArch64State *cpu, uint32_t instr) {
    unsigned sf  = (instr >> 31) & 1;
    unsigned Rm  = (instr >> 16) & 0x1F;
    unsigned op2 = (instr >> 10) & 3;
    unsigned Rn  = (instr >>  5) & 0x1F;
    unsigned Rd  = instr & 0x1F;

    if (sf) {
        uint64_t val = reg_x(cpu, Rn);
        unsigned amount = (unsigned)(reg_x(cpu, Rm) & 63);
        uint64_t result;
        switch (op2) {
            case 0: result = val << amount; break;
            case 1: result = val >> amount; break;
            case 2: result = (uint64_t)((int64_t)val >> amount); break;
            case 3: result = ror64(val, amount); break;
            default: result = val; break;
        }
        write_x(cpu, Rd, result);
    } else {
        uint32_t val = (uint32_t)reg_x(cpu, Rn);
        unsigned amount = (unsigned)(reg_x(cpu, Rm) & 31);
        uint32_t result;
        switch (op2) {
            case 0: result = val << amount; break;
            case 1: result = val >> amount; break;
            case 2: result = (uint32_t)((int32_t)val >> amount); break;
            case 3: result = ror32(val, amount); break;
            default: result = val; break;
        }
        write_w(cpu, Rd, result);
    }
}

/* CLZ / CLS / RBIT / REV */
static void exec_data_proc_1src(AArch64State *cpu, uint32_t instr) {
    unsigned sf    = (instr >> 31) & 1;
    unsigned opcode = (instr >> 10) & 0x3F;
    unsigned Rn    = (instr >>  5) & 0x1F;
    unsigned Rd    = instr & 0x1F;

    uint64_t val = reg_x(cpu, Rn);

    switch (opcode) {
        case 0: /* RBIT */
            if (sf) write_x(cpu, Rd, rbit64(val));
            else    write_w(cpu, Rd, rbit32((uint32_t)val));
            break;
        case 1: /* REV16 */
            if (sf) {
                /* REV16 on 64-bit: reverse bytes within each 16-bit halfword */
                uint64_t r = 0;
                for (int i = 0; i < 4; i++) {
                    uint16_t hw = (uint16_t)(val >> (i * 16));
                    hw = (uint16_t)((hw >> 8) | (hw << 8));
                    r |= (uint64_t)hw << (i * 16);
                }
                write_x(cpu, Rd, r);
            } else {
                write_w(cpu, Rd, rev16_in_32((uint32_t)val));
            }
            break;
        case 2: /* REV32 (64-bit) or REV (32-bit) */
            if (sf) {
                /* REV32 on X register: reverse bytes in each 32-bit word */
                uint32_t lo = rev32((uint32_t)val);
                uint32_t hi = rev32((uint32_t)(val >> 32));
                write_x(cpu, Rd, ((uint64_t)hi) | ((uint64_t)lo << 32));
            } else {
                write_w(cpu, Rd, rev32((uint32_t)val));
            }
            break;
        case 3: /* REV (64-bit) */
            if (sf) write_x(cpu, Rd, rev64(val));
            else    do_undef(cpu, instr); /* REV with sf=0 and opcode=3 is unallocated */
            break;
        case 4: /* CLZ */
            if (sf) write_x(cpu, Rd, clz64(val));
            else    write_w(cpu, Rd, clz32((uint32_t)val));
            break;
        case 5: /* CLS */
            if (sf) write_x(cpu, Rd, cls64(val));
            else    write_w(cpu, Rd, cls32((uint32_t)val));
            break;
        default:
            do_undef(cpu, instr);
            break;
    }
}

/* =========================================================================
 * Conditional select: CSEL / CSINC / CSINV / CSNEG
 * ========================================================================= */

static void exec_cond_select(AArch64State *cpu, uint32_t instr) {
    unsigned sf   = (instr >> 31) & 1;
    unsigned op   = (instr >> 30) & 1;
    unsigned S    = (instr >> 29) & 1;
    unsigned Rm   = (instr >> 16) & 0x1F;
    unsigned cond = (instr >> 12) & 0xF;
    unsigned op2  = (instr >> 10) & 1;
    unsigned Rn   = (instr >>  5) & 0x1F;
    unsigned Rd   = instr & 0x1F;

    (void)S;

    bool cond_true = aarch64_eval_cond(cpu, cond);

    if (sf) {
        uint64_t a = reg_x(cpu, Rn);
        uint64_t b = reg_x(cpu, Rm);
        uint64_t result;
        if (cond_true) {
            result = a;
        } else {
            if (op == 0 && op2 == 0)      result = b;           /* CSEL */
            else if (op == 0 && op2 == 1)  result = b + 1;       /* CSINC */
            else if (op == 1 && op2 == 0)  result = ~b;          /* CSINV */
            else                            result = (uint64_t)(-(int64_t)b); /* CSNEG */
        }
        write_x(cpu, Rd, result);
    } else {
        uint32_t a = (uint32_t)reg_x(cpu, Rn);
        uint32_t b = (uint32_t)reg_x(cpu, Rm);
        uint32_t result;
        if (cond_true) {
            result = a;
        } else {
            if (op == 0 && op2 == 0)      result = b;
            else if (op == 0 && op2 == 1)  result = b + 1;
            else if (op == 1 && op2 == 0)  result = ~b;
            else                            result = (uint32_t)(-(int32_t)b);
        }
        write_w(cpu, Rd, result);
    }
}

/* =========================================================================
 * Conditional compare: CCMP / CCMN
 * ========================================================================= */

static void exec_cond_cmp(AArch64State *cpu, uint32_t instr) {
    unsigned sf   = (instr >> 31) & 1;
    unsigned op   = (instr >> 30) & 1; /* 0=CCMN, 1=CCMP */
    unsigned imm_or_reg = (instr >> 11) & 1; /* 1=immediate, 0=register */
    unsigned Rm_or_imm5 = (instr >> 16) & 0x1F;
    unsigned cond = (instr >> 12) & 0xF;
    unsigned Rn   = (instr >>  5) & 0x1F;
    unsigned nzcv_val = instr & 0xF;

    if (!aarch64_eval_cond(cpu, cond)) {
        cpu->nzcv = nzcv_val << 28;
        return;
    }

    uint64_t b;
    if (imm_or_reg)
        b = Rm_or_imm5;
    else
        b = reg_x(cpu, Rm_or_imm5);

    if (sf) {
        uint64_t a = reg_x(cpu, Rn);
        uint64_t result;
        if (op) {
            result = a - b;
            set_flags_sub64(cpu, a, b, result);
        } else {
            result = a + b;
            set_flags_add64(cpu, a, b, result);
        }
    } else {
        uint32_t a = (uint32_t)reg_x(cpu, Rn);
        uint32_t b32 = (uint32_t)b;
        uint32_t result;
        if (op) {
            result = a - b32;
            set_flags_sub32(cpu, a, b32, result);
        } else {
            result = a + b32;
            set_flags_add32(cpu, a, b32, result);
        }
    }
}

/* =========================================================================
 * Branches
 * ========================================================================= */

/* Unconditional branch (immediate): B / BL */
static void exec_branch_imm(AArch64State *cpu, uint32_t instr) {
    unsigned op = (instr >> 31) & 1; /* 0=B, 1=BL */
    int64_t imm26 = sign_extend64(instr & 0x3FFFFFF, 26) << 2;
    uint64_t pc = cpu->pc - 4; /* address of this instruction */
    if (op) cpu->x[AARCH64_LR] = cpu->pc; /* return address = next instruction */
    cpu->pc = (uint64_t)((int64_t)pc + imm26);
}

/* Conditional branch: B.cond */
static void exec_branch_cond(AArch64State *cpu, uint32_t instr) {
    unsigned cond = instr & 0xF;
    int64_t imm19 = sign_extend64((instr >> 5) & 0x7FFFF, 19) << 2;
    if (aarch64_eval_cond(cpu, cond)) {
        uint64_t pc = cpu->pc - 4;
        cpu->pc = (uint64_t)((int64_t)pc + imm19);
    }
}

/* Compare and branch: CBZ / CBNZ */
static void exec_cbz_cbnz(AArch64State *cpu, uint32_t instr) {
    unsigned sf  = (instr >> 31) & 1;
    unsigned op  = (instr >> 24) & 1; /* 0=CBZ, 1=CBNZ */
    int64_t imm19 = sign_extend64((instr >> 5) & 0x7FFFF, 19) << 2;
    unsigned Rt  = instr & 0x1F;

    uint64_t val = reg_x(cpu, Rt);
    if (!sf) val = (uint32_t)val;

    bool take = (op == 0) ? (val == 0) : (val != 0);
    if (take) {
        uint64_t pc = cpu->pc - 4;
        cpu->pc = (uint64_t)((int64_t)pc + imm19);
    }
}

/* Test and branch: TBZ / TBNZ */
static void exec_tbz_tbnz(AArch64State *cpu, uint32_t instr) {
    unsigned b5  = (instr >> 31) & 1;
    unsigned op  = (instr >> 24) & 1; /* 0=TBZ, 1=TBNZ */
    unsigned b40 = (instr >> 19) & 0x1F;
    int64_t imm14 = sign_extend64((instr >> 5) & 0x3FFF, 14) << 2;
    unsigned Rt  = instr & 0x1F;

    unsigned bit_pos = (b5 << 5) | b40;
    uint64_t val = reg_x(cpu, Rt);
    bool bit_set = ((val >> bit_pos) & 1) != 0;
    bool take = (op == 0) ? !bit_set : bit_set;
    if (take) {
        uint64_t pc = cpu->pc - 4;
        cpu->pc = (uint64_t)((int64_t)pc + imm14);
    }
}

/* Unconditional branch (register): BR / BLR / RET */
static void exec_branch_reg(AArch64State *cpu, uint32_t instr) {
    unsigned opc = (instr >> 21) & 0xF;
    unsigned Rn  = (instr >>  5) & 0x1F;

    uint64_t target = reg_x_or_sp(cpu, Rn);
    if (Rn == 31) target = cpu->x[AARCH64_LR]; /* RET default = X30 */

    switch (opc) {
        case 0: /* BR */
            cpu->pc = target;
            break;
        case 1: /* BLR */
            cpu->x[AARCH64_LR] = cpu->pc;
            cpu->pc = target;
            break;
        case 2: /* RET */
            cpu->pc = reg_x(cpu, Rn);
            break;
        default:
            do_undef(cpu, instr);
            break;
    }
}

/* =========================================================================
 * Load / Store
 * ========================================================================= */

/* LDR/STR (unsigned immediate offset)
 * Also handles LDRB/LDRH/LDRSB/LDRSH/LDRSW, STRB/STRH
 * Encoding: size[31:30] | 11 1 0 01 | opc[23:22] | imm12[21:10] | Rn[9:5] | Rt[4:0] */
static void exec_ldr_str_unsigned(AArch64State *cpu, uint32_t instr) {
    unsigned size = (instr >> 30) & 3;
    unsigned opc  = (instr >> 22) & 3;
    unsigned imm12 = (instr >> 10) & 0xFFF;
    unsigned Rn   = (instr >>  5) & 0x1F;
    unsigned Rt   = instr & 0x1F;

    unsigned scale = size;
    uint64_t offset = (uint64_t)imm12 << scale;
    uint64_t addr = reg_x_or_sp(cpu, Rn) + offset;

    bool is_load = (opc & 1) || (opc >= 2);

    if (opc == 0) {
        /* STR (store) */
        switch (size) {
            case 0: mem_w8 (cpu, addr, (uint8_t )reg_x(cpu, Rt)); break;
            case 1: mem_w16(cpu, addr, (uint16_t)reg_x(cpu, Rt)); break;
            case 2: mem_w32(cpu, addr, (uint32_t)reg_x(cpu, Rt)); break;
            case 3: mem_w64(cpu, addr, reg_x(cpu, Rt));           break;
        }
    } else if (opc == 1) {
        /* LDR (zero-extend for size < 3, full for size 3) */
        switch (size) {
            case 0: write_w(cpu, Rt, mem_r8 (cpu, addr));       break; /* LDRB */
            case 1: write_w(cpu, Rt, mem_r16(cpu, addr));       break; /* LDRH */
            case 2: write_w(cpu, Rt, mem_r32(cpu, addr));       break; /* LDR W */
            case 3: write_x(cpu, Rt, mem_r64(cpu, addr));       break; /* LDR X */
        }
    } else if (opc == 2) {
        /* LDRS* (sign-extend to 64-bit) */
        switch (size) {
            case 0: write_x(cpu, Rt, (uint64_t)(int64_t)(int8_t )mem_r8 (cpu, addr)); break; /* LDRSB -> X */
            case 1: write_x(cpu, Rt, (uint64_t)(int64_t)(int16_t)mem_r16(cpu, addr)); break; /* LDRSH -> X */
            case 2: write_x(cpu, Rt, (uint64_t)(int64_t)(int32_t)mem_r32(cpu, addr)); break; /* LDRSW */
            default: do_undef(cpu, instr); break;
        }
    } else { /* opc == 3 */
        /* LDRS* (sign-extend to 32-bit) */
        switch (size) {
            case 0: write_w(cpu, Rt, (uint32_t)(int32_t)(int8_t )mem_r8 (cpu, addr)); break; /* LDRSB -> W */
            case 1: write_w(cpu, Rt, (uint32_t)(int32_t)(int16_t)mem_r16(cpu, addr)); break; /* LDRSH -> W */
            default: do_undef(cpu, instr); break;
        }
    }
    (void)is_load;
}

/* LDR/STR (pre-index and post-index)
 * Encoding: size[31:30] | 111 0 00 | opc[23:22] | 0 | imm9[20:12] | idx[11:10] | Rn[9:5] | Rt[4:0]
 * idx: 01=post-index, 11=pre-index, 00=unscaled offset */
static void exec_ldr_str_imm9(AArch64State *cpu, uint32_t instr) {
    unsigned size = (instr >> 30) & 3;
    unsigned opc  = (instr >> 22) & 3;
    int64_t  imm9 = sign_extend64((instr >> 12) & 0x1FF, 9);
    unsigned idx  = (instr >> 10) & 3;
    unsigned Rn   = (instr >>  5) & 0x1F;
    unsigned Rt   = instr & 0x1F;

    uint64_t base = reg_x_or_sp(cpu, Rn);
    uint64_t addr;
    bool writeback = false;

    if (idx == 1) {
        /* Post-index */
        addr = base;
        writeback = true;
    } else if (idx == 3) {
        /* Pre-index */
        addr = base + (uint64_t)imm9;
        writeback = true;
    } else {
        /* Unscaled offset (LDUR/STUR) */
        addr = base + (uint64_t)imm9;
    }

    if (opc == 0) {
        /* Store */
        switch (size) {
            case 0: mem_w8 (cpu, addr, (uint8_t )reg_x(cpu, Rt)); break;
            case 1: mem_w16(cpu, addr, (uint16_t)reg_x(cpu, Rt)); break;
            case 2: mem_w32(cpu, addr, (uint32_t)reg_x(cpu, Rt)); break;
            case 3: mem_w64(cpu, addr, reg_x(cpu, Rt));           break;
        }
    } else if (opc == 1) {
        /* Load (zero-extend) */
        switch (size) {
            case 0: write_w(cpu, Rt, mem_r8 (cpu, addr)); break;
            case 1: write_w(cpu, Rt, mem_r16(cpu, addr)); break;
            case 2: write_w(cpu, Rt, mem_r32(cpu, addr)); break;
            case 3: write_x(cpu, Rt, mem_r64(cpu, addr)); break;
        }
    } else if (opc == 2) {
        /* Sign-extend to 64-bit */
        switch (size) {
            case 0: write_x(cpu, Rt, (uint64_t)(int64_t)(int8_t )mem_r8 (cpu, addr)); break;
            case 1: write_x(cpu, Rt, (uint64_t)(int64_t)(int16_t)mem_r16(cpu, addr)); break;
            case 2: write_x(cpu, Rt, (uint64_t)(int64_t)(int32_t)mem_r32(cpu, addr)); break;
            default: do_undef(cpu, instr); break;
        }
    } else {
        /* Sign-extend to 32-bit */
        switch (size) {
            case 0: write_w(cpu, Rt, (uint32_t)(int32_t)(int8_t )mem_r8 (cpu, addr)); break;
            case 1: write_w(cpu, Rt, (uint32_t)(int32_t)(int16_t)mem_r16(cpu, addr)); break;
            default: do_undef(cpu, instr); break;
        }
    }

    if (writeback) {
        uint64_t wb = base + (uint64_t)imm9;
        write_x_or_sp(cpu, Rn, wb);
    }
}

/* LDR/STR (register offset)
 * Encoding: size[31:30] | 111 0 00 | opc[23:22] | 1 | Rm[20:16] | opt[15:13] | S[12] | 10 | Rn[9:5] | Rt[4:0] */
static void exec_ldr_str_reg(AArch64State *cpu, uint32_t instr) {
    unsigned size = (instr >> 30) & 3;
    unsigned opc  = (instr >> 22) & 3;
    unsigned Rm   = (instr >> 16) & 0x1F;
    unsigned opt  = (instr >> 13) & 7;
    unsigned S    = (instr >> 12) & 1;
    unsigned Rn   = (instr >>  5) & 0x1F;
    unsigned Rt   = instr & 0x1F;

    uint64_t base = reg_x_or_sp(cpu, Rn);
    uint64_t offset;
    uint64_t m = reg_x(cpu, Rm);

    /* Extend the offset register */
    switch (opt) {
        case 2: /* UXTW */
            offset = (uint32_t)m;
            break;
        case 3: /* LSL (default) */
            offset = m;
            break;
        case 6: /* SXTW */
            offset = (uint64_t)(int64_t)(int32_t)(uint32_t)m;
            break;
        case 7: /* SXTX */
            offset = m;
            break;
        default:
            offset = m;
            break;
    }

    if (S) offset <<= size;

    uint64_t addr = base + offset;

    if (opc == 0) {
        switch (size) {
            case 0: mem_w8 (cpu, addr, (uint8_t )reg_x(cpu, Rt)); break;
            case 1: mem_w16(cpu, addr, (uint16_t)reg_x(cpu, Rt)); break;
            case 2: mem_w32(cpu, addr, (uint32_t)reg_x(cpu, Rt)); break;
            case 3: mem_w64(cpu, addr, reg_x(cpu, Rt));           break;
        }
    } else if (opc == 1) {
        switch (size) {
            case 0: write_w(cpu, Rt, mem_r8 (cpu, addr)); break;
            case 1: write_w(cpu, Rt, mem_r16(cpu, addr)); break;
            case 2: write_w(cpu, Rt, mem_r32(cpu, addr)); break;
            case 3: write_x(cpu, Rt, mem_r64(cpu, addr)); break;
        }
    } else if (opc == 2) {
        switch (size) {
            case 0: write_x(cpu, Rt, (uint64_t)(int64_t)(int8_t )mem_r8 (cpu, addr)); break;
            case 1: write_x(cpu, Rt, (uint64_t)(int64_t)(int16_t)mem_r16(cpu, addr)); break;
            case 2: write_x(cpu, Rt, (uint64_t)(int64_t)(int32_t)mem_r32(cpu, addr)); break;
            default: do_undef(cpu, instr); break;
        }
    } else {
        switch (size) {
            case 0: write_w(cpu, Rt, (uint32_t)(int32_t)(int8_t )mem_r8 (cpu, addr)); break;
            case 1: write_w(cpu, Rt, (uint32_t)(int32_t)(int16_t)mem_r16(cpu, addr)); break;
            default: do_undef(cpu, instr); break;
        }
    }
}

/* LDR (literal / PC-relative)
 * Encoding: opc[31:30] | 011 0 00 | imm19[23:5] | Rt[4:0] */
static void exec_ldr_literal(AArch64State *cpu, uint32_t instr) {
    unsigned opc = (instr >> 30) & 3;
    int64_t imm19 = sign_extend64((instr >> 5) & 0x7FFFF, 19) << 2;
    unsigned Rt  = instr & 0x1F;

    uint64_t pc = cpu->pc - 4;
    uint64_t addr = (uint64_t)((int64_t)pc + imm19);

    switch (opc) {
        case 0: write_w(cpu, Rt, mem_r32(cpu, addr)); break;        /* LDR Wt */
        case 1: write_x(cpu, Rt, mem_r64(cpu, addr)); break;        /* LDR Xt */
        case 2: write_x(cpu, Rt, (uint64_t)(int64_t)(int32_t)mem_r32(cpu, addr)); break; /* LDRSW */
        default: do_undef(cpu, instr); break;
    }
}

/* LDP / STP (load/store pair)
 * Encoding: opc[31:30] | 101 | V[26] | idx[24:23] | L[22] | imm7[21:15] | Rt2[14:10] | Rn[9:5] | Rt[4:0]
 * idx: 01=post-index, 10=signed-offset, 11=pre-index */
static void exec_ldp_stp(AArch64State *cpu, uint32_t instr) {
    unsigned opc = (instr >> 30) & 3;
    unsigned idx = (instr >> 23) & 3;
    unsigned L   = (instr >> 22) & 1;
    int64_t  imm7 = sign_extend64((instr >> 15) & 0x7F, 7);
    unsigned Rt2  = (instr >> 10) & 0x1F;
    unsigned Rn   = (instr >>  5) & 0x1F;
    unsigned Rt   = instr & 0x1F;

    unsigned scale = (opc == 0) ? 2 : 3; /* 0 => 32-bit (4 bytes), 2 => 64-bit (8 bytes) */
    int64_t offset = imm7 << scale;

    uint64_t base = reg_x_or_sp(cpu, Rn);
    uint64_t addr;
    bool writeback = false;

    if (idx == 1) {
        /* Post-index */
        addr = base;
        writeback = true;
    } else if (idx == 3) {
        /* Pre-index */
        addr = base + (uint64_t)offset;
        writeback = true;
    } else {
        /* Signed offset */
        addr = base + (uint64_t)offset;
    }

    if (opc == 0) {
        /* 32-bit pair */
        if (L) {
            write_w(cpu, Rt,  mem_r32(cpu, addr));
            write_w(cpu, Rt2, mem_r32(cpu, addr + 4));
        } else {
            mem_w32(cpu, addr,     (uint32_t)reg_x(cpu, Rt));
            mem_w32(cpu, addr + 4, (uint32_t)reg_x(cpu, Rt2));
        }
    } else if (opc == 2) {
        /* 64-bit pair */
        if (L) {
            write_x(cpu, Rt,  mem_r64(cpu, addr));
            write_x(cpu, Rt2, mem_r64(cpu, addr + 8));
        } else {
            mem_w64(cpu, addr,     reg_x(cpu, Rt));
            mem_w64(cpu, addr + 8, reg_x(cpu, Rt2));
        }
    } else if (opc == 1 && L) {
        /* LDPSW (sign-extend 32-bit to 64-bit) */
        write_x(cpu, Rt,  (uint64_t)(int64_t)(int32_t)mem_r32(cpu, addr));
        write_x(cpu, Rt2, (uint64_t)(int64_t)(int32_t)mem_r32(cpu, addr + 4));
    } else {
        do_undef(cpu, instr);
        return;
    }

    if (writeback) {
        write_x_or_sp(cpu, Rn, base + (uint64_t)offset);
    }
}

/* =========================================================================
 * System instructions
 * ========================================================================= */

static void exec_system(AArch64State *cpu, uint32_t instr) {
    /* MSR/MRS NZCV, FPCR, FPSR */
    unsigned L   = (instr >> 21) & 1; /* 1=MRS (read), 0=MSR (write) */
    unsigned op0 = (instr >> 19) & 3;
    unsigned op1 = (instr >> 16) & 7;
    unsigned CRn = (instr >> 12) & 0xF;
    unsigned CRm = (instr >>  8) & 0xF;
    unsigned op2 = (instr >>  5) & 7;
    unsigned Rt  = instr & 0x1F;

    (void)op0;

    /* Detect NOP: op1=3, CRn=2, CRm=0, op2=0 (HINT #0) */
    if (!L && op1 == 3 && CRn == 2 && CRm == 0 && op2 == 0 && Rt == 31) {
        return; /* NOP */
    }

    /* NZCV: op0=3, op1=3, CRn=4, CRm=2, op2=0 */
    if (op1 == 3 && CRn == 4 && CRm == 2 && op2 == 0) {
        if (L) {
            write_x(cpu, Rt, (uint64_t)cpu->nzcv);
        } else {
            cpu->nzcv = (uint32_t)reg_x(cpu, Rt) & 0xF0000000u;
        }
        return;
    }

    /* FPCR: op0=3, op1=3, CRn=4, CRm=4, op2=0 */
    if (op1 == 3 && CRn == 4 && CRm == 4 && op2 == 0) {
        if (L) write_x(cpu, Rt, (uint64_t)cpu->fpcr);
        else   cpu->fpcr = (uint32_t)reg_x(cpu, Rt);
        return;
    }

    /* FPSR: op0=3, op1=3, CRn=4, CRm == 4, op2=1 */
    if (op1 == 3 && CRn == 4 && CRm == 4 && op2 == 1) {
        if (L) write_x(cpu, Rt, (uint64_t)cpu->fpsr);
        else   cpu->fpsr = (uint32_t)reg_x(cpu, Rt);
        return;
    }

    /* HINT instructions (NOP, YIELD, WFE, WFI, SEV, SEVL etc.) */
    if (!L && CRn == 2) {
        return; /* treat all hints as NOP */
    }

    /* Data/instruction barriers: DSB, DMB, ISB */
    if (!L && CRn == 3) {
        return; /* treat barriers as NOP for emulation */
    }

    do_undef(cpu, instr);
}

/* SVC (supervisor call / syscall) */
static void exec_svc(AArch64State *cpu, uint32_t instr) {
    uint32_t imm16 = (instr >> 5) & 0xFFFF;
    if (cpu->syscall_handler) {
        cpu->syscall_handler(cpu->callback_ctx, imm16);
    }
}

/* =========================================================================
 * ADD/SUB (extended register)
 * Encoding: sf | op | S | 01011 00 1 | Rm | option | imm3 | Rn | Rd
 * ========================================================================= */

static void exec_add_sub_ext(AArch64State *cpu, uint32_t instr) {
    unsigned sf    = (instr >> 31) & 1;
    unsigned op    = (instr >> 30) & 1;
    unsigned S     = (instr >> 29) & 1;
    unsigned Rm    = (instr >> 16) & 0x1F;
    unsigned option = (instr >> 13) & 7;
    unsigned imm3  = (instr >> 10) & 7;
    unsigned Rn    = (instr >>  5) & 0x1F;
    unsigned Rd    = instr & 0x1F;

    uint64_t m = reg_x(cpu, Rm);
    uint64_t ext_val;

    switch (option) {
        case 0: ext_val = (uint64_t)(uint8_t )m; break; /* UXTB */
        case 1: ext_val = (uint64_t)(uint16_t)m; break; /* UXTH */
        case 2: ext_val = (uint64_t)(uint32_t)m; break; /* UXTW */
        case 3: ext_val = m;                      break; /* UXTX */
        case 4: ext_val = (uint64_t)(int64_t)(int8_t )m; break; /* SXTB */
        case 5: ext_val = (uint64_t)(int64_t)(int16_t)m; break; /* SXTH */
        case 6: ext_val = (uint64_t)(int64_t)(int32_t)m; break; /* SXTW */
        case 7: ext_val = m;                               break; /* SXTX */
        default: ext_val = m; break;
    }
    ext_val <<= imm3;

    /* Rn and Rd: SP encoding */
    uint64_t a = reg_x_or_sp(cpu, Rn);

    if (sf) {
        uint64_t result;
        if (op == 0) {
            result = a + ext_val;
            if (S) set_flags_add64(cpu, a, ext_val, result);
        } else {
            result = a - ext_val;
            if (S) set_flags_sub64(cpu, a, ext_val, result);
        }
        if (S) write_x(cpu, Rd, result);
        else   write_x_or_sp(cpu, Rd, result);
    } else {
        uint32_t a32 = (uint32_t)a;
        uint32_t e32 = (uint32_t)ext_val;
        uint32_t result;
        if (op == 0) {
            result = a32 + e32;
            if (S) set_flags_add32(cpu, a32, e32, result);
        } else {
            result = a32 - e32;
            if (S) set_flags_sub32(cpu, a32, e32, result);
        }
        if (S) write_w(cpu, Rd, result);
        else   write_w_or_sp(cpu, Rd, result);
    }
}

/* =========================================================================
 * Top-level decode and execute
 * ========================================================================= */

static void decode_exec(AArch64State *cpu, uint32_t instr) {
    /* Major grouping by bits [28:25] */
    unsigned op0 = (instr >> 25) & 0xF;

    /* NOP encoding: 0xD503201F */
    if (instr == 0xD503201F) return;

    /* ---------- Data Processing -- Immediate ---------- */
    if ((op0 & 0xE) == 0x8) {
        /* op0 = 100x */
        unsigned op1 = (instr >> 23) & 7;

        if (op1 == 0 || op1 == 1) {
            /* PC-relative addressing: ADR/ADRP */
            exec_adr(cpu, instr);
            return;
        }
        if (op1 == 2 || op1 == 3) {
            /* Add/subtract immediate */
            exec_add_sub_imm(cpu, instr);
            return;
        }
        if (op1 == 4) {
            /* Logical immediate */
            exec_logical_imm(cpu, instr);
            return;
        }
        if (op1 == 5) {
            /* Move wide (MOVZ/MOVK/MOVN) */
            exec_mov_wide(cpu, instr);
            return;
        }
        if (op1 == 6) {
            /* Bitfield (SBFM/BFM/UBFM) */
            exec_bitfield(cpu, instr);
            return;
        }
        if (op1 == 7) {
            /* Extract: EXTR - simplified: treat as undef for now */
            do_undef(cpu, instr);
            return;
        }
    }

    /* ---------- Branches, Exception, System ---------- */
    if ((op0 & 0xE) == 0xA) {
        /* op0 = 101x */
        if ((instr >> 26) == 0x5) {
            /* B (unconditional): 000101 */
            exec_branch_imm(cpu, instr);
            return;
        }
        if ((instr >> 26) == 0x25) {
            /* BL (unconditional): 100101 */
            exec_branch_imm(cpu, instr);
            return;
        }
        if (((instr >> 25) & 0x7F) == 0x2A) {
            /* B.cond: 0101010 0 */
            exec_branch_cond(cpu, instr);
            return;
        }
        if (((instr >> 25) & 0x7F) == 0x6B) {
            /* Unconditional branch (register): 1101011 */
            exec_branch_reg(cpu, instr);
            return;
        }

        /* SVC / HVC / SMC */
        if (((instr >> 21) & 0x7FF) == 0x6A0) {
            /* Exception generation: 11010100 000 */
            unsigned opc_exc = (instr >> 21) & 7;
            if (opc_exc == 0) {
                unsigned ll = (instr >> 0) & 3;
                if (ll == 1) { exec_svc(cpu, instr); return; }
            }
        }

        /* System instructions (MSR/MRS/NOP/hints/barriers) */
        if (((instr >> 22) & 0x3FF) == 0x354) {
            exec_system(cpu, instr);
            return;
        }

        /* CBZ/CBNZ */
        if (((instr >> 25) & 0x7F) == 0x34 || ((instr >> 25) & 0x7F) == 0x35) {
            exec_cbz_cbnz(cpu, instr);
            return;
        }

        /* TBZ/TBNZ */
        if (((instr >> 25) & 0x7F) == 0x36 || ((instr >> 25) & 0x7F) == 0x37) {
            exec_tbz_tbnz(cpu, instr);
            return;
        }
    }

    /* ---------- Load / Store ---------- */
    if ((op0 & 0x5) == 0x4) {
        /* op0 = x1x0 */

        /* LDP/STP: check bits [29:27] = 101 and bit [26] = 0 */
        if (((instr >> 27) & 0x7) == 5 && ((instr >> 26) & 1) == 0) {
            exec_ldp_stp(cpu, instr);
            return;
        }

        /* LDR (literal / PC-relative): bits [29:27] = 011, bit [26] = 0 */
        if (((instr >> 27) & 0x7) == 3 && ((instr >> 26) & 1) == 0) {
            exec_ldr_literal(cpu, instr);
            return;
        }

        /* LDR/STR with various addressing modes */
        if (((instr >> 27) & 0x7) == 7 && ((instr >> 26) & 1) == 0) {
            unsigned bit24 = (instr >> 24) & 1;
            if (bit24) {
                /* Unsigned offset */
                exec_ldr_str_unsigned(cpu, instr);
                return;
            } else {
                /* Check bit [21] for register offset vs imm9 */
                unsigned bit21 = (instr >> 21) & 1;
                if (bit21) {
                    /* Register offset */
                    unsigned bit11_10 = (instr >> 10) & 3;
                    if (bit11_10 == 2) {
                        exec_ldr_str_reg(cpu, instr);
                        return;
                    }
                }
                /* imm9 (pre/post-index or unscaled) */
                exec_ldr_str_imm9(cpu, instr);
                return;
            }
        }
    }

    /* ---------- Data Processing -- Register ---------- */
    if ((op0 & 0xE) == 0xA || (op0 & 0x8) == 0x8) {
        /* Catch-all area; the specific patterns are checked below */
    }

    /* Logical shifted register: bits [28:24] = 01010 */
    if (((instr >> 24) & 0x1F) == 0x0A) {
        exec_logical_shifted(cpu, instr);
        return;
    }

    /* ADD/SUB shifted register: bits [28:24] = 01011, bit [21] = 0 */
    if (((instr >> 24) & 0x1F) == 0x0B && ((instr >> 21) & 1) == 0) {
        exec_add_sub_shifted(cpu, instr);
        return;
    }

    /* ADD/SUB extended register: bits [28:24] = 01011, bit [21] = 1 */
    if (((instr >> 24) & 0x1F) == 0x0B && ((instr >> 21) & 1) == 1) {
        exec_add_sub_ext(cpu, instr);
        return;
    }

    /* Conditional select: bits [28:21] = 11010100 */
    if (((instr >> 21) & 0xFF) == 0xD4 && ((instr >> 29) & 1) == 0) {
        exec_cond_select(cpu, instr);
        return;
    }

    /* Conditional compare: bits [28:24] = 11010010 (reg) or 11010010 (imm) */
    if (((instr >> 24) & 0x1F) == 0x1A && ((instr >> 21) & 1) == 0) {
        /* Could be cond-select or cond-compare; disambiguate via bit [11] */
        unsigned bit11 = (instr >> 11) & 1;
        unsigned bit4  = (instr >>  4) & 1;
        if (bit4 == 0 && bit11 == 0) {
            /* This range covers CCMP/CCMN for register */
        }
    }
    /* CCMP/CCMN (immediate): bits [28:21] pattern */
    if (((instr >> 24) & 0x1F) == 0x1A && ((instr >> 10) & 1) == 0 && ((instr >> 4) & 1) == 0) {
        unsigned op2_field = (instr >> 21) & 0xF;
        if (op2_field == 2 || op2_field == 6) {
            exec_cond_cmp(cpu, instr);
            return;
        }
    }

    /* Data processing (2 source): SDIV, UDIV, LSLV, LSRV, ASRV, RORV
     * bits [28:21] = 11010110 */
    if (((instr >> 21) & 0xFF) == 0xD6 && ((instr >> 29) & 1) == 0) {
        unsigned opcode2 = (instr >> 10) & 0x3F;
        if (opcode2 == 2 || opcode2 == 3) {
            exec_div(cpu, instr);
            return;
        }
        if (opcode2 >= 8 && opcode2 <= 11) {
            exec_shift_reg(cpu, instr);
            return;
        }
        do_undef(cpu, instr);
        return;
    }

    /* Data processing (1 source): CLZ, CLS, RBIT, REV etc.
     * bits [28:21] = 11010110, S=1 ... actually bits [30:21] = 1101011 0 10 */
    if (((instr >> 21) & 0x3FF) == 0x2D6) {
        exec_data_proc_1src(cpu, instr);
        return;
    }

    /* Multiply: MADD/MSUB
     * bits [28:24] = 11011, bits [23:21] = 000 */
    if (((instr >> 24) & 0x1F) == 0x1B && ((instr >> 21) & 7) == 0) {
        exec_madd_msub(cpu, instr);
        return;
    }

    /* Conditional select (wider pattern check) */
    if (((instr >> 21) & 0x7FE) == 0x1A8) {
        /* bits [29:21] = 1 1010 100 x */
        exec_cond_select(cpu, instr);
        return;
    }

    do_undef(cpu, instr);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void aarch64_init(AArch64State *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->running = true;
}

void aarch64_reset(AArch64State *cpu) {
    /* Preserve callbacks */
    void *ctx = cpu->callback_ctx;
    uint8_t  (*r8 )(void*, uint64_t)            = cpu->mem_read8;
    uint16_t (*r16)(void*, uint64_t)            = cpu->mem_read16;
    uint32_t (*r32)(void*, uint64_t)            = cpu->mem_read32;
    uint64_t (*r64)(void*, uint64_t)            = cpu->mem_read64;
    void (*w8 )(void*, uint64_t, uint8_t)       = cpu->mem_write8;
    void (*w16)(void*, uint64_t, uint16_t)      = cpu->mem_write16;
    void (*w32)(void*, uint64_t, uint32_t)      = cpu->mem_write32;
    void (*w64)(void*, uint64_t, uint64_t)      = cpu->mem_write64;
    void (*sys)(void*, uint32_t)                = cpu->syscall_handler;
    void (*udf)(void*, uint32_t)                = cpu->undef_handler;

    memset(cpu, 0, sizeof(*cpu));
    cpu->running         = true;
    cpu->callback_ctx    = ctx;
    cpu->mem_read8       = r8;
    cpu->mem_read16      = r16;
    cpu->mem_read32      = r32;
    cpu->mem_read64      = r64;
    cpu->mem_write8      = w8;
    cpu->mem_write16     = w16;
    cpu->mem_write32     = w32;
    cpu->mem_write64     = w64;
    cpu->syscall_handler = sys;
    cpu->undef_handler   = udf;
}

void aarch64_step(AArch64State *cpu) {
    uint32_t instr = mem_r32(cpu, cpu->pc);
    cpu->pc += 4;
    decode_exec(cpu, instr);
}

void aarch64_run(AArch64State *cpu) {
    while (cpu->running) {
        aarch64_step(cpu);
    }
}
