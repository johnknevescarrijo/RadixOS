/*
 * arch/xtensa/context_switch.S - Xtensa LX7 Preemptive / Cooperative Context Switch
 */

#include "xtensa_regs.h"

#define CTX_FRAME_SIZE      80

#define CTX_OFF_PC          0x00
#define CTX_OFF_PS          0x04
#define CTX_OFF_A0          0x08
#define CTX_OFF_A2          0x0C
#define CTX_OFF_A3          0x10
#define CTX_OFF_A4          0x14
#define CTX_OFF_A5          0x18
#define CTX_OFF_A6          0x1C
#define CTX_OFF_A7          0x20
#define CTX_OFF_A8          0x24
#define CTX_OFF_A9          0x28
#define CTX_OFF_A10         0x2C
#define CTX_OFF_A11         0x30
#define CTX_OFF_A12         0x34
#define CTX_OFF_A13         0x38
#define CTX_OFF_A14         0x3C
#define CTX_OFF_A15         0x40

    .section .iram0.text, "ax"
    .align 4
    .global arch_context_switch
    .type arch_context_switch, @function

/*
 * void arch_context_switch(cpu_context_t **prev_ctx, cpu_context_t *next_ctx)
 *   a2: prev_ctx (pointer to store current stack pointer)
 *   a3: next_ctx (pointer to new task stack context)
 */
arch_context_switch:
    /* Allocate stack frame */
    addi    a1, a1, -CTX_FRAME_SIZE

    /* Save PC (return address from call0 is in a0) */
    s32i    a0, a1, CTX_OFF_PC

    /* Save Processor Status (PS) */
    rsr     a4, SR_PS
    s32i    a4, a1, CTX_OFF_PS

    /* Save caller return address a0 */
    s32i    a0, a1, CTX_OFF_A0

    /* Save callee-saved registers */
    s32i    a12, a1, CTX_OFF_A12
    s32i    a13, a1, CTX_OFF_A13
    s32i    a14, a1, CTX_OFF_A14
    s32i    a15, a1, CTX_OFF_A15

    /* If prev_ctx != NULL, save current stack pointer */
    beqz    a2, .L_skip_save_prev
    s32i    a1, a2, 0

.L_skip_save_prev:
    /* Switch stack pointer to next task */
    mov     a1, a3

    /* Restore Processor Status */
    l32i    a4, a1, CTX_OFF_PS
    wsr     a4, SR_PS
    rsync

    /* Restore registers from incoming task */
    l32i    a0, a1, CTX_OFF_PC
    l32i    a2, a1, CTX_OFF_A2
    l32i    a3, a1, CTX_OFF_A3
    l32i    a12, a1, CTX_OFF_A12
    l32i    a13, a1, CTX_OFF_A13
    l32i    a14, a1, CTX_OFF_A14
    l32i    a15, a1, CTX_OFF_A15

    /* Deallocate stack frame and branch to resumed task */
    addi    a1, a1, CTX_FRAME_SIZE
    jx      a0

    .size arch_context_switch, . - arch_context_switch

/*
 * void arch_start_first_task(cpu_context_t *first_ctx)
 *   a2: first_ctx (stack context of the initial task)
 */
    .align 4
    .global arch_start_first_task
    .type arch_start_first_task, @function

arch_start_first_task:
    /* Switch directly to first task stack */
    mov     a1, a2

    /* Restore Processor Status */
    l32i    a4, a1, CTX_OFF_PS
    wsr     a4, SR_PS
    rsync

    /* Restore target entry address and initial argument */
    l32i    a0, a1, CTX_OFF_PC
    l32i    a2, a1, CTX_OFF_A2
    l32i    a12, a1, CTX_OFF_A12
    l32i    a13, a1, CTX_OFF_A13
    l32i    a14, a1, CTX_OFF_A14
    l32i    a15, a1, CTX_OFF_A15

    /* Deallocate frame and jump to task entry point */
    addi    a1, a1, CTX_FRAME_SIZE
    jx      a0

    .size arch_start_first_task, . - arch_start_first_task
