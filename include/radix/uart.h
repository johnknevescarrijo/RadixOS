#ifndef RADIX_UART_H
#define RADIX_UART_H

#include <radix/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UART_RX_BUF_SIZE 128u

/* Serial UART0 HAL & Interrupt-Driven RX API */
void     uart_early_init(void);
void     uart_putc(char c);
int      uart_getc_nonblock(void);
int      uart_getc_nonblocking(void);
int      uart_getc_blocking(void);
uint32_t uart_rx_available(void);
void     uart_rx_irq_enable(void);
void     uart_isr(uint32_t irq, void *arg);

#ifdef RADIX_HOST_TEST
/* Host test helpers for simulated UART0 RX FIFO and IRQ delivery */
void     uart_host_reset(void);
void     uart_host_push_hw_fifo(char c);
void     uart_host_inject_rx_irq(char c);
uint32_t uart_host_get_rx_overflows(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RADIX_UART_H */
