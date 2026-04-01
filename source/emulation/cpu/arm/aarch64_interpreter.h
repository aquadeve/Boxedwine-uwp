/*
 * Boxedwine Android - AArch64 CPU Interpreter
 *
 * Emulates an AArch64 (ARM64) processor to run Android native (.so) libraries.
 * Supports the base A64 instruction set including SIMD/FP (NEON).
 *
 * Reference: ARM Architecture Reference Manual for A-profile architecture
 */

#ifndef __AARCH64_INTERPRETER_H__
#define __AARCH64_INTERPRETER_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Register file ---------- */

#define AARCH64_GP_COUNT    31
#define AARCH64_LR          30

/* NZCV condition flag bits (PSTATE / NZCV system register) */
#define NZCV_N  (1u << 31)  /* Negative */
#define NZCV_Z  (1u << 30)  /* Zero     */
#define NZCV_C  (1u << 29)  /* Carry    */
#define NZCV_V  (1u << 28)  /* Overflow */

/* SIMD/FP: 32 128-bit registers (stored as double for scalar FP) */
#define AARCH64_VREG_COUNT  32

/* ---------- CPU state ---------- */

typedef struct {
    uint64_t x[AARCH64_GP_COUNT]; /* General-purpose registers X0-X30 */
    uint64_t sp;                  /* Stack pointer (SP_EL0) */
    uint64_t pc;                  /* Program counter */
    uint32_t nzcv;                /* Condition flags N,Z,C,V in bits 31-28 */
    double   v[AARCH64_VREG_COUNT]; /* SIMD/FP registers V0-V31 */
    uint32_t fpcr;                /* Floating-Point Control Register */
    uint32_t fpsr;                /* Floating-Point Status Register */

    /* Execution control */
    bool     running;
    int      exit_code;

    /* Memory access callbacks (set by the embedding layer) */
    uint8_t  (*mem_read8 )(void *ctx, uint64_t addr);
    uint16_t (*mem_read16)(void *ctx, uint64_t addr);
    uint32_t (*mem_read32)(void *ctx, uint64_t addr);
    uint64_t (*mem_read64)(void *ctx, uint64_t addr);
    void     (*mem_write8 )(void *ctx, uint64_t addr, uint8_t  val);
    void     (*mem_write16)(void *ctx, uint64_t addr, uint16_t val);
    void     (*mem_write32)(void *ctx, uint64_t addr, uint32_t val);
    void     (*mem_write64)(void *ctx, uint64_t addr, uint64_t val);

    /* Syscall / SVC handler (set by the embedding layer) */
    void     (*syscall_handler)(void *ctx, uint32_t svc_num);

    /* Undefined instruction handler */
    void     (*undef_handler)(void *ctx, uint32_t instr);

    /* Opaque context pointer passed back to all callbacks */
    void    *callback_ctx;
} AArch64State;

/* ---------- Condition codes ---------- */
typedef enum {
    AARCH64_COND_EQ = 0,  /* Z=1 */
    AARCH64_COND_NE,      /* Z=0 */
    AARCH64_COND_CS,      /* C=1 */
    AARCH64_COND_CC,      /* C=0 */
    AARCH64_COND_MI,      /* N=1 */
    AARCH64_COND_PL,      /* N=0 */
    AARCH64_COND_VS,      /* V=1 */
    AARCH64_COND_VC,      /* V=0 */
    AARCH64_COND_HI,      /* C=1 && Z=0 */
    AARCH64_COND_LS,      /* C=0 || Z=1 */
    AARCH64_COND_GE,      /* N==V */
    AARCH64_COND_LT,      /* N!=V */
    AARCH64_COND_GT,      /* Z=0 && N==V */
    AARCH64_COND_LE,      /* Z=1 || N!=V */
    AARCH64_COND_AL,      /* Always */
    AARCH64_COND_NV       /* Always (alternate encoding) */
} AArch64Condition;

/* ---------- Public API ---------- */

/* Initialise a CPU state to power-on defaults */
void aarch64_init(AArch64State *cpu);

/* Reset the CPU (clear registers and flags) */
void aarch64_reset(AArch64State *cpu);

/* Execute a single A64 instruction pointed to by PC */
void aarch64_step(AArch64State *cpu);

/* Run the CPU until cpu->running becomes false */
void aarch64_run(AArch64State *cpu);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __AARCH64_INTERPRETER_H__ */
