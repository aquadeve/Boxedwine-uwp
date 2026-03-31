/*
 * Boxedwine Android - ARMv7 CPU Interpreter
 *
 * Emulates an ARMv7-A processor to run Android native (.so) libraries.
 * Supports the full ARM32 and Thumb instruction sets including VFP/NEON.
 *
 * Reference: ARM Architecture Reference Manual ARMv7-A and ARMv7-R edition
 */

#ifndef __ARMV7_INTERPRETER_H__
#define __ARMV7_INTERPRETER_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Register file ---------- */

#define ARM_REG_COUNT   16
#define ARM_SP          13
#define ARM_LR          14
#define ARM_PC          15

/* CPSR / SPSR flag bits */
#define ARM_CPSR_N      (1u << 31)  /* Negative */
#define ARM_CPSR_Z      (1u << 30)  /* Zero     */
#define ARM_CPSR_C      (1u << 29)  /* Carry    */
#define ARM_CPSR_V      (1u << 28)  /* Overflow */
#define ARM_CPSR_Q      (1u << 27)  /* Saturation */
#define ARM_CPSR_J      (1u << 24)  /* Jazelle   */
#define ARM_CPSR_GE_MASK (0xFu << 16)
#define ARM_CPSR_E      (1u <<  9)  /* Endian    */
#define ARM_CPSR_A      (1u <<  8)  /* Async abort disable */
#define ARM_CPSR_I      (1u <<  7)  /* IRQ disable */
#define ARM_CPSR_F      (1u <<  6)  /* FIQ disable */
#define ARM_CPSR_T      (1u <<  5)  /* Thumb state */
#define ARM_CPSR_MODE   (0x1Fu)     /* Processor mode */

/* VFP/NEON: 32 double-precision registers (= 64 single-precision) */
#define ARM_VFP_DREG_COUNT 32

/* ---------- CPU state ---------- */

typedef struct {
    uint32_t r[ARM_REG_COUNT];  /* General-purpose registers R0-R15 */
    uint32_t cpsr;              /* Current Program Status Register */
    uint32_t spsr;              /* Saved Program Status Register */
    double   d[ARM_VFP_DREG_COUNT]; /* VFP/NEON double-precision registers */
    uint32_t fpscr;             /* Floating-Point Status and Control Register */

    /* Execution control */
    bool     running;
    int      exit_code;

    /* Memory access callbacks (set by the embedding layer) */
    uint8_t  (*mem_read8 )(void *ctx, uint32_t addr);
    uint16_t (*mem_read16)(void *ctx, uint32_t addr);
    uint32_t (*mem_read32)(void *ctx, uint32_t addr);
    void     (*mem_write8 )(void *ctx, uint32_t addr, uint8_t  val);
    void     (*mem_write16)(void *ctx, uint32_t addr, uint16_t val);
    void     (*mem_write32)(void *ctx, uint32_t addr, uint32_t val);

    /* Syscall / SWI handler (set by the embedding layer) */
    void     (*syscall_handler)(void *ctx, uint32_t swi_num);

    /* Undefined instruction handler */
    void     (*undef_handler)(void *ctx, uint32_t instr);

    /* Opaque context pointer passed back to all callbacks */
    void    *callback_ctx;
} ArmV7State;

/* ---------- Condition codes ---------- */
typedef enum {
    ARM_COND_EQ = 0,  /* Z=1 */
    ARM_COND_NE,      /* Z=0 */
    ARM_COND_CS,      /* C=1 */
    ARM_COND_CC,      /* C=0 */
    ARM_COND_MI,      /* N=1 */
    ARM_COND_PL,      /* N=0 */
    ARM_COND_VS,      /* V=1 */
    ARM_COND_VC,      /* V=0 */
    ARM_COND_HI,      /* C=1 && Z=0 */
    ARM_COND_LS,      /* C=0 || Z=1 */
    ARM_COND_GE,      /* N==V */
    ARM_COND_LT,      /* N!=V */
    ARM_COND_GT,      /* Z=0 && N==V */
    ARM_COND_LE,      /* Z=1 || N!=V */
    ARM_COND_AL,      /* Always */
    ARM_COND_UNCOND   /* Unconditional (special encodings) */
} ArmCondition;

/* ---------- Public API ---------- */

/* Initialise a CPU state to power-on defaults */
void armv7_init(ArmV7State *cpu);

/* Reset the CPU (clear registers, set CPSR to supervisor mode) */
void armv7_reset(ArmV7State *cpu);

/* Execute a single ARM or Thumb instruction pointed to by PC */
void armv7_step(ArmV7State *cpu);

/* Run the CPU until cpu->running becomes false */
void armv7_run(ArmV7State *cpu);

/* Helper: evaluate a condition code against the current CPSR */
bool armv7_eval_cond(const ArmV7State *cpu, ArmCondition cond);

/* Helper: read/write VFP single-precision registers as float */
float  armv7_get_s(const ArmV7State *cpu, unsigned sreg);
void   armv7_set_s(ArmV7State *cpu,       unsigned sreg, float val);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __ARMV7_INTERPRETER_H__ */
