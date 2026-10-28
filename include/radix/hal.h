#ifndef RADIX_HAL_H
#define RADIX_HAL_H

#include <radix/types.h>

/* Forward declaration */
struct cpu_context;

/* Hardware Initialization & Control */
void hal_early_init(void);
void wdt_disable_all(void);

/* Serial UART HAL */
void uart_early_init(void);
void uart_putc(char c);
int  uart_getc_nonblock(void);

/* Systimer HAL */
void systimer_init(uint32_t tick_rate_hz);
uint64_t systimer_get_ticks(void);

/* Architecture-specific context switch */
void arch_context_switch(struct cpu_context **prev_ctx, struct cpu_context *next_ctx);
#ifndef RADIX_HOST_TEST
void arch_start_first_task(struct cpu_context *first_ctx) __attribute__((noreturn));
#else
void arch_start_first_task(struct cpu_context *first_ctx);
#endif

#endif /* RADIX_HAL_H */
