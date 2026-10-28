#include <radix/hal.h>
#include <radix/types.h>

#ifdef RADIX_HOST_TEST
#include <stdio.h>

void uart_early_init(void) {
    /* No hardware UART init required in host mode */
}

void uart_putc(char c) {
    putchar(c);
    fflush(stdout);
}

int uart_getc_nonblock(void) {
    return -1;
}

#else

#include <esp32s3_soc.h>

void uart_early_init(void) {
    /*
     * The ESP32-S3 ROM bootloader normally initializes UART0 at 115200 baud (8N1).
     * We ensure TX FIFO is ready and clear any pending interrupt flags.
     */
    REG_WRITE(UART_INT_CLR_REG(DR_REG_UART0_BASE), 0xFFFFFFFF);
}

void uart_putc(char c) {
    /* Convert newline to CRLF for standard serial terminals */
    if (c == '\n') {
        uart_putc('\r');
    }

    /* Wait while TX FIFO is full (FIFO depth is 128 bytes) */
    while (((REG_READ(UART_STATUS_REG(DR_REG_UART0_BASE)) >> UART_TXFIFO_CNT_SHIFT) & 0xFF) >= 126) {
        /* Busy wait for FIFO space */
    }

    /* Push byte to UART0 TX FIFO */
    REG_WRITE(UART_FIFO_REG(DR_REG_UART0_BASE), (uint32_t)(uint8_t)c);
}

int uart_getc_nonblock(void) {
    uint32_t status = REG_READ(UART_STATUS_REG(DR_REG_UART0_BASE));
    uint32_t rx_cnt = (status & UART_RXFIFO_CNT_MASK) >> UART_RXFIFO_CNT_SHIFT;

    if (rx_cnt == 0) {
        return -1;
    }

    return (int)(REG_READ(UART_FIFO_REG(DR_REG_UART0_BASE)) & 0xFF);
}

#endif /* RADIX_HOST_TEST */
