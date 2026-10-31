/*
 * arch/xtensa/atomic.S - Xtensa LX7 S32C1I Atomic Primitives
 */

#include "xtensa_regs.h"

    .section .iram0.text, "ax"
    .align 4
    .global arch_atomic_cas
    .type arch_atomic_cas, @function

/*
 * bool arch_atomic_cas(volatile uint32_t *addr, uint32_t expected, uint32_t desired)
 *   a2: addr
 *   a3: expected
 *   a4: desired
 *   Returns: 1 if successful (previous *addr == expected), 0 otherwise
 */
arch_atomic_cas:
    wsr     a3, SR_SCOMPARE1
    mov     a5, a4
    s32c1i  a5, a2, 0
    memw
    bne     a5, a3, .L_cas_fail
    movi    a2, 1
    ret
.L_cas_fail:
    movi    a2, 0
    ret
    .size arch_atomic_cas, . - arch_atomic_cas

/*
 * uint32_t arch_atomic_swap(volatile uint32_t *addr, uint32_t val)
 *   a2: addr
 *   a3: val
 *   Returns: previous value of *addr
 */
    .align 4
    .global arch_atomic_swap
    .type arch_atomic_swap, @function

arch_atomic_swap:
.L_swap_retry:
    l32i    a4, a2, 0
    wsr     a4, SR_SCOMPARE1
    mov     a5, a3
    s32c1i  a5, a2, 0
    memw
    bne     a5, a4, .L_swap_retry
    mov     a2, a5
    ret
    .size arch_atomic_swap, . - arch_atomic_swap

/*
 * uint32_t arch_atomic_fetch_add(volatile uint32_t *addr, uint32_t val)
 *   a2: addr
 *   a3: val
 *   Returns: previous value of *addr
 */
    .align 4
    .global arch_atomic_fetch_add
    .type arch_atomic_fetch_add, @function

arch_atomic_fetch_add:
.L_add_retry:
    l32i    a4, a2, 0
    add     a5, a4, a3
    wsr     a4, SR_SCOMPARE1
    s32c1i  a5, a2, 0
    memw
    bne     a5, a4, .L_add_retry
    mov     a2, a4
    ret
    .size arch_atomic_fetch_add, . - arch_atomic_fetch_add

/*
 * uint32_t arch_irq_save(void)
 *   Disables interrupts up to level 15 and returns previous PS.
 */
    .align 4
    .global arch_irq_save
    .type arch_irq_save, @function

arch_irq_save:
    rsil    a2, 15
    rsync
    ret
    .size arch_irq_save, . - arch_irq_save

/*
 * void arch_irq_restore(uint32_t flags)
 *   a2: flags (previous PS)
 */
    .align 4
    .global arch_irq_restore
    .type arch_irq_restore, @function

arch_irq_restore:
    wsr     a2, SR_PS
    rsync
    ret
    .size arch_irq_restore, . - arch_irq_restore

/*
 * void arch_memory_barrier(void)
 */
    .align 4
    .global arch_memory_barrier
    .type arch_memory_barrier, @function

arch_memory_barrier:
    memw
    ret
    .size arch_memory_barrier, . - arch_memory_barrier
