/*
 * arch/xtensa/vectors.S - Xtensa LX7 Exception & Interrupt Vector Table
 */

#include "xtensa_regs.h"

    .section .vectors, "ax"
    .align 1024
    .global _vector_table

_vector_table:
    /* Offset 0x000: Reset Vector / Base Entry */
    j       _start
    .fill   (0x180 - (. - _vector_table)), 1, 0

    /* Offset 0x180: Debug Exception Vector */
_debug_vector:
    break   1, 15
    .fill   (0x1C0 - (. - _vector_table)), 1, 0

    /* Offset 0x1C0: Double Exception Vector */
_double_exception_vector:
    break   1, 4
    .fill   (0x200 - (. - _vector_table)), 1, 0

    /* Offset 0x200: Level 1 Exception & General Interrupt Vector */
_level1_vector:
    j       _level1_vector_handler
    .fill   (0x280 - (. - _vector_table)), 1, 0

    /* Offset 0x280: User Exception / Syscall Vector */
_user_exception_vector:
    j       _user_exception_handler
    .fill   (0x340 - (. - _vector_table)), 1, 0

    /* Offset 0x340: Level 2 Interrupt Vector */
_level2_vector:
    rfi     2
    .fill   (0x380 - (. - _vector_table)), 1, 0

    /* Offset 0x380: Level 3 Interrupt Vector */
_level3_vector:
    rfi     3
    .fill   (0x3C0 - (. - _vector_table)), 1, 0

    /* Offset 0x3C0: Level 4 Interrupt Vector */
_level4_vector:
    rfi     4
    .fill   (0x400 - (. - _vector_table)), 1, 0

/*
 * Exception Trampolines & Default Handlers
 */
    .section .iram0.text, "ax"
    .align 4
    .global _level1_vector_handler
    .type _level1_vector_handler, @function

_level1_vector_handler:
    /* Save temporary working registers in EXCSAVE1 */
    wsr     a2, SR_EXCSAVE1

    /* Read exception cause */
    rsr     a2, SR_EXCCAUSE

    /* Check if this is a Level 1 hardware interrupt */
    beqi    a2, EXCCAUSE_LEVEL1_INTERRUPT, .L_handle_interrupt

    /* Check if this is a syscall */
    beqi    a2, EXCCAUSE_SYSTEM_CALL, .L_handle_syscall

    /* Otherwise, general exception handler */
    rsr     a2, SR_EXCSAVE1
    j       radix_unhandled_exception

.L_handle_interrupt:
    rsr     a2, SR_EXCSAVE1
    /* Call C interrupt dispatcher (e.g. Systimer tick) */
    addi    a1, a1, -16
    s32i    a0, a1, 0
    call0   radix_level1_interrupt_handler
    l32i    a0, a1, 0
    addi    a1, a1, 16
    rfe

.L_handle_syscall:
    rsr     a2, SR_EXCSAVE1
    /* Advance EPC1 past syscall instruction (3 bytes in Xtensa) */
    addi    a1, a1, -16
    s32i    a0, a1, 0
    call0   radix_syscall_handler
    l32i    a0, a1, 0
    addi    a1, a1, 16
    rfe

    .align 4
_user_exception_handler:
    j       _level1_vector_handler

    .align 4
    .global radix_unhandled_exception
    .type radix_unhandled_exception, @function
radix_unhandled_exception:
    /* Trap unhandled crash */
    waiti   0
    j       radix_unhandled_exception
