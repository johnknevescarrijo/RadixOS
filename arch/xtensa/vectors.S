/*
 * arch/xtensa/vectors.S - Xtensa LX7 Exception & Interrupt Vector Table
 */

#include "xtensa_regs.h"

    .section .vectors, "ax"
    .align 1024
    .global _vector_table

_vector_table:
    /* Offset 0x000: Reset Vector / WindowOverflow4 (unused in Call0 ABI) */
    j       _start
    .fill   (0x180 - (. - _vector_table)), 1, 0

    /* Offset 0x180: Level 2 Interrupt Vector */
_level2_vector:
    rfi     2
    .fill   (0x1C0 - (. - _vector_table)), 1, 0

    /* Offset 0x1C0: Level 3 Interrupt Vector */
_level3_vector:
    rfi     3
    .fill   (0x200 - (. - _vector_table)), 1, 0

    /* Offset 0x200: Level 4 Interrupt Vector */
_level4_vector:
    rfi     4
    .fill   (0x240 - (. - _vector_table)), 1, 0

    /* Offset 0x240: Level 5 Interrupt Vector */
_level5_vector:
    rfi     5
    .fill   (0x280 - (. - _vector_table)), 1, 0

    /* Offset 0x280: Debug Exception Vector */
_debug_vector:
    break   1, 15
    .fill   (0x2C0 - (. - _vector_table)), 1, 0

    /* Offset 0x2C0: NMI Exception Vector (Level 7) */
_nmi_vector:
    rfi     7
    .fill   (0x300 - (. - _vector_table)), 1, 0

    /* Offset 0x300: Kernel Exception & Level 1 Interrupt Vector (PS.UM = 0) */
_kernel_exception_vector:
    j       _level1_vector_handler
    .fill   (0x340 - (. - _vector_table)), 1, 0

    /* Offset 0x340: User Exception / Syscall & Level 1 Interrupt Vector (PS.UM = 1) */
_user_exception_vector:
    j       _user_exception_handler
    .fill   (0x3C0 - (. - _vector_table)), 1, 0

    /* Offset 0x3C0: Double Exception Vector */
_double_exception_vector:
    break   1, 4
    .fill   (0x400 - (. - _vector_table)), 1, 0

/*
 * Exception Trampolines & Level-1 ISR Frame Handler
 *
 * ISR Stack Frame Layout (96 bytes, 16-byte aligned):
 *   0x00: a0
 *   0x04: a1 (original sp)
 *   0x08: a2
 *   0x0C: a3
 *   0x10: a4
 *   0x14: a5
 *   0x18: a6
 *   0x1C: a7
 *   0x20: a8
 *   0x24: a9
 *   0x28: a10
 *   0x2C: a11
 *   0x30: a12
 *   0x34: a13
 *   0x38: a14
 *   0x3C: a15
 *   0x40: EPC1
 *   0x44: PS
 *   0x48: SAR
 *   0x4C: LBEG
 *   0x50: LEND
 *   0x54: LCOUNT
 *   0x58..0x5F: alignment padding
 */
#define ISR_FRAME_SIZE 96

    .section .iram0.text, "ax"
    .align 4
    .global _level1_vector_handler
    .type _level1_vector_handler, @function

_level1_vector_handler:
    /* Save temporary working register a2 in EXCSAVE1 */
    wsr     a2, SR_EXCSAVE1

    /* Read exception cause */
    rsr     a2, SR_EXCCAUSE

    /* Check if this is a Level 1 hardware interrupt */
    beqi    a2, EXCCAUSE_LEVEL1_INTERRUPT, .L_handle_interrupt

    /* Forward syscall or hardware fault to full trap frame handler */
    j       _xtensa_syscall_and_fault_entry

.L_handle_interrupt:
    /* Allocate 16-byte aligned ISR frame on current task stack */
    addi    a1, a1, -ISR_FRAME_SIZE

    /* Save return address a0 and original stack pointer */
    s32i    a0, a1, 0x00
    addi    a0, a1, ISR_FRAME_SIZE
    s32i    a0, a1, 0x04

    /* Retrieve original a2 saved in EXCSAVE1 */
    rsr     a0, SR_EXCSAVE1
    s32i    a0, a1, 0x08

    /* Save general-purpose registers a3..a15 */
    s32i    a3, a1, 0x0C
    s32i    a4, a1, 0x10
    s32i    a5, a1, 0x14
    s32i    a6, a1, 0x18
    s32i    a7, a1, 0x1C
    s32i    a8, a1, 0x20
    s32i    a9, a1, 0x24
    s32i    a10, a1, 0x28
    s32i    a11, a1, 0x2C
    s32i    a12, a1, 0x30
    s32i    a13, a1, 0x34
    s32i    a14, a1, 0x38
    s32i    a15, a1, 0x3C

    /* Save special registers (EPC1, PS, SAR, LBEG, LEND, LCOUNT) */
    rsr     a0, SR_EPC1
    s32i    a0, a1, 0x40
    rsr     a0, SR_PS
    s32i    a0, a1, 0x44
    rsr     a0, SR_SAR
    s32i    a0, a1, 0x48
    rsr     a0, SR_LBEG
    s32i    a0, a1, 0x4C
    rsr     a0, SR_LEND
    s32i    a0, a1, 0x50
    rsr     a0, SR_LCOUNT
    s32i    a0, a1, 0x54

    /* Compute pending = INTERRUPT & INTENABLE and clear latched edge/sw bits */
    rsr     a2, SR_INTERRUPT
    rsr     a3, SR_INTENABLE
    and     a2, a2, a3
    wsr     a2, SR_INTCLEAR
    rsync

    /* Dispatch pending Level-1 interrupts in C: radix_level1_interrupt_handler(pending_mask) */
    call0   radix_level1_interrupt_handler

    /* Check if deferred preemption is needed (returns false if inside nested ISR) */
    call0   sched_consume_need_resched
    beqz    a2, .L_isr_restore

    /* Switch context while full interrupted frame stays preserved above SP */
    call0   sched_yield

.L_isr_restore:
    /* Restore special registers before general registers */
    l32i    a0, a1, 0x40
    wsr     a0, SR_EPC1
    l32i    a0, a1, 0x44
    wsr     a0, SR_PS
    l32i    a0, a1, 0x48
    wsr     a0, SR_SAR
    l32i    a0, a1, 0x4C
    wsr     a0, SR_LBEG
    l32i    a0, a1, 0x50
    wsr     a0, SR_LEND
    l32i    a0, a1, 0x54
    wsr     a0, SR_LCOUNT
    rsync

    /* Restore general-purpose registers a2..a15 */
    l32i    a2, a1, 0x08
    l32i    a3, a1, 0x0C
    l32i    a4, a1, 0x10
    l32i    a5, a1, 0x14
    l32i    a6, a1, 0x18
    l32i    a7, a1, 0x1C
    l32i    a8, a1, 0x20
    l32i    a9, a1, 0x24
    l32i    a10, a1, 0x28
    l32i    a11, a1, 0x2C
    l32i    a12, a1, 0x30
    l32i    a13, a1, 0x34
    l32i    a14, a1, 0x38
    l32i    a15, a1, 0x3C

    /* Restore a0, deallocate ISR frame, and return from Level-1 exception */
    l32i    a0, a1, 0x00
    addi    a1, a1, ISR_FRAME_SIZE
    rfe

    .align 4
_user_exception_handler:
    wsr     a2, SR_EXCSAVE1
    rsr     a2, SR_EXCCAUSE
    beqi    a2, EXCCAUSE_LEVEL1_INTERRUPT, .L_handle_interrupt
    j       _xtensa_syscall_and_fault_entry

    .align 4
    .global radix_unhandled_exception
    .type radix_unhandled_exception, @function
radix_unhandled_exception:
    /* Trap unhandled crash */
    waiti   0
    j       radix_unhandled_exception
