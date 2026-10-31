/*
 * arch/xtensa/syscall_handler.S - Syscall & Access Violation Exception Handler
 *
 * Handles transition between World 1 (User) and World 0 (Kernel),
 * decodes syscall arguments, invokes dispatcher, and handles PMS violations.
 */

#include "xtensa_regs.h"

    .section .iram0.text, "ax"
    .align 4
    .global _xtensa_syscall_and_fault_entry
    .type _xtensa_syscall_and_fault_entry, @function

/*
 * Stack Frame Layout (Total: 96 bytes, 16-byte aligned)
 *   0x00: a0
 *   0x04: a1 (original caller sp)
 *   0x08: a2 (syscall_id / return value)
 *   0x0C: a3 (arg1)
 *   0x10: a4 (arg2)
 *   0x14: a5 (arg3)
 *   0x18: a6 (arg4)
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
 *   0x48: EXCCAUSE
 *   0x4C: EXCVADDR
 *   0x50..0x5F: reserved
 */
#define TRAP_FRAME_SIZE 96

_xtensa_syscall_and_fault_entry:
    /* Allocate trap frame on stack */
    addi    a1, a1, -TRAP_FRAME_SIZE

    /* Save caller return address a0 */
    s32i    a0, a1, 0x00

    /* Save original stack pointer */
    addi    a0, a1, TRAP_FRAME_SIZE
    s32i    a0, a1, 0x04

    /* Retrieve original a2 saved in EXCSAVE1 by vector stub */
    rsr     a0, SR_EXCSAVE1
    s32i    a0, a1, 0x08

    /* Save remaining general registers a3..a15 */
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

    /* Save special registers */
    rsr     a0, SR_EPC1
    s32i    a0, a1, 0x40
    rsr     a0, SR_PS
    s32i    a0, a1, 0x44
    rsr     a0, SR_EXCCAUSE
    s32i    a0, a1, 0x48
    rsr     a0, SR_EXCVADDR
    s32i    a0, a1, 0x4C

    /* Read exception cause */
    l32i    a3, a1, 0x48

    /* Check if this is a syscall (EXCCAUSE == 1) */
    beqi    a3, EXCCAUSE_SYSCALL, .L_syscall_entry

    /* Check memory/access violation causes */
    beqi    a3, EXCCAUSE_ILLEGAL_INSTRUCTION, .L_pms_violation_entry
    beqi    a3, EXCCAUSE_LOAD_STORE_ERROR, .L_pms_violation_entry
    movi    a4, EXCCAUSE_LOAD_PROHIBITED
    beq     a3, a4, .L_pms_violation_entry
    movi    a4, EXCCAUSE_STORE_PROHIBITED
    beq     a3, a4, .L_pms_violation_entry

    /* Otherwise, dispatch general exception */
    call0   world_enter_kernel_from_trap
    l32i    a2, a1, 0x48    /* exccause */
    l32i    a3, a1, 0x40    /* epc1 */
    l32i    a4, a1, 0x4C    /* excvaddr */
    mov     a5, a1          /* regs ptr */
    call0   radix_exception_handler_c
    j       .L_restore_and_rfe

.L_syscall_entry:
    /* Advance EPC1 by 3 bytes past the 'syscall' opcode */
    l32i    a3, a1, 0x40
    addi    a3, a3, 3
    s32i    a3, a1, 0x40

    /* Switch logical/hardware domain back to Kernel */
    call0   world_enter_kernel_from_trap

    /*
     * Call radix_syscall_dispatch(syscall_id, arg1, arg2, arg3, arg4)
     * Arguments passed in a2..a6 per Call0 ABI
     */
    l32i    a2, a1, 0x08    /* syscall_id */
    l32i    a3, a1, 0x0C    /* arg1 */
    l32i    a4, a1, 0x10    /* arg2 */
    l32i    a5, a1, 0x14    /* arg3 */
    l32i    a6, a1, 0x18    /* arg4 */
    call0   radix_syscall_dispatch

    /* Store return value into saved a2 slot */
    s32i    a2, a1, 0x08

    /* Switch domain back to World 1 (User) with return address */
    l32i    a2, a1, 0x40
    call0   world_switch_to_user

    j       .L_restore_and_rfe

.L_pms_violation_entry:
    /* Notify World Controller of trap into Kernel */
    call0   world_enter_kernel_from_trap

    /* Call pms_handle_violation_exception(exccause, excvaddr) */
    l32i    a2, a1, 0x48    /* exccause */
    l32i    a3, a1, 0x4C    /* excvaddr */
    call0   pms_handle_violation_exception

    /* Fall through to restore */
    j       .L_restore_and_rfe

.L_restore_and_rfe:
    /* Restore special registers EPC1 and PS */
    l32i    a0, a1, 0x40
    wsr     a0, SR_EPC1
    l32i    a0, a1, 0x44
    wsr     a0, SR_PS

    /* Restore general registers */
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

    /* Restore a0 and deallocate trap frame */
    l32i    a0, a1, 0x00
    addi    a1, a1, TRAP_FRAME_SIZE
    rfe
