/*
 * arch/xtensa/boot_core1.S - Core 1 (APP_CPU) boot trampoline for ESP32-S3
 */

#include "xtensa_regs.h"

    .section .iram1.text, "ax"
    .align 4
    .global _start_core1
    .type _start_core1, @function

_start_core1:
    /* 1. Disable all interrupts */
    movi    a0, 0x0000000F
    wsr     a0, SR_PS
    rsync

    /* 2. Configure VECBASE to shared vector table */
    movi    a0, _vector_table
    wsr     a0, SR_VECBASE
    rsync

    /* 3. Configure ATOMCTL for atomic transactions (0x28: internal SRAM atomics) */
    movi    a0, 0x28
    wsr     a0, SR_ATOMCTL

    /* 4. Initialize Core 1 stack pointer */
    movi    a1, core1_boot_stack_top

    /* 5. Clear frame pointer */
    movi    a0, 0

    /* 6. Clear PS for Call0 ABI (INTLEVEL=0, WOE=0) */
    movi    a0, 0x00000000
    wsr     a0, SR_PS
    rsync

    /* 7. Call Core 1 main entry point */
    call0   kernel_core1_main

    /* 8. Trap CPU in low-power halt if kernel_core1_main returns */
1:
    waiti   15
    j       1b

    .size _start_core1, . - _start_core1

    /* Dedicated boot stack for Core 1 (4096 bytes) */
    .section .bss
    .align 16
    .global core1_boot_stack
    .global core1_boot_stack_top
core1_boot_stack:
    .skip 4096
core1_boot_stack_top:
