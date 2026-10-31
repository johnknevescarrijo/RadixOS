#ifndef RADIX_ATOMIC_H
#define RADIX_ATOMIC_H

#include <radix/types.h>

#ifdef RADIX_HOST_TEST

extern volatile uint32_t g_host_irq_flags;

static inline bool arch_atomic_cas(volatile uint32_t *addr, uint32_t expected, uint32_t desired) {
    return __atomic_compare_exchange_n(addr, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline uint32_t arch_atomic_swap(volatile uint32_t *addr, uint32_t val) {
    return __atomic_exchange_n(addr, val, __ATOMIC_SEQ_CST);
}

static inline uint32_t arch_atomic_fetch_add(volatile uint32_t *addr, uint32_t val) {
    return __atomic_fetch_add(addr, val, __ATOMIC_SEQ_CST);
}

static inline void arch_memory_barrier(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static inline uint32_t arch_irq_save(void) {
    uint32_t prev = g_host_irq_flags;
    g_host_irq_flags = 1;
    return prev;
}

static inline void arch_irq_restore(uint32_t flags) {
    g_host_irq_flags = flags;
}

#else

/* Low-level Xtensa LX7 assembly implementations */
bool     arch_atomic_cas(volatile uint32_t *addr, uint32_t expected, uint32_t desired);
uint32_t arch_atomic_swap(volatile uint32_t *addr, uint32_t val);
uint32_t arch_atomic_fetch_add(volatile uint32_t *addr, uint32_t val);
uint32_t arch_irq_save(void);
void     arch_irq_restore(uint32_t flags);
void     arch_memory_barrier(void);

#endif /* RADIX_HOST_TEST */

#endif /* RADIX_ATOMIC_H */
