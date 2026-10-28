/*
 * arch/xtensa/boot.S - Bare-metal bootloader entry point for ESP32-S3 (Xtensa LX7)
 */

#include "xtensa_regs.h"

    .section .iram0.text, "ax"
    .align 4
    .global _start
    .type _start, @function

_start:
    /* 1. Disable all interrupts while initializing processor state */
    movi    a2, 0x0000000F      /* Mask all interrupts (INTLEVEL = 15) */
    wsr     a2, SR_PS
    rsync

    /* 2. Setup initial boot stack pointer (sp = a1) */
    movi    a1, _stack_top

    /* 3. Set Vector Base Register (VECBASE) */
    movi    a2, _vector_table
    wsr     a2, SR_VECBASE
    rsync

    /* 4. Clear .bss section with 32-bit word stores */
    movi    a2, _bss_start
    movi    a3, _bss_end
    movi    a4, 0

.L_bss_loop:
    bgeu    a2, a3, .L_bss_done
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       .L_bss_loop

.L_bss_done:
    /* 5. Initialize Processor State (PS) for Level 0 interrupts enabled */
    movi    a2, 0x00000000      /* Call0 ABI: WOE=0, INTLEVEL=0, Ring=0 */
    wsr     a2, SR_PS
    rsync

    /* 6. Call early kernel initialization (in C) */
    call0   kernel_early_init

    /* 7. Trap CPU in low-power halt if kernel_early_init returns */
.L_halt:
    waiti   0
    j       .L_halt

    .size _start, . - _start
