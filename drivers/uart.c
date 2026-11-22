#include <radix/hal.h>
#include <radix/uart.h>
#include <radix/types.h>
#include <radix/atomic.h>
#include <radix/sched.h>
#include <radix/sync.h>
#include <radix/intc.h>

#ifdef RADIX_HOST_TEST
#include <stdio.h>
#else
#include <esp32s3_soc.h>
#endif

static uint8_t     s_rx_buf[UART_RX_BUF_SIZE];
static uint16_t    s_rx_head = 0u;
static uint16_t    s_rx_tail = 0u;
static uint16_t    s_rx_count = 0u;
static uint32_t    s_rx_overflows = 0u;
static semaphore_t s_rx_sem;
static bool        s_rx_initialized = false;

#ifdef RADIX_HOST_TEST
static uint8_t  s_host_hw_fifo[UART_RX_BUF_SIZE];
static uint16_t s_host_hw_head = 0u;
static uint16_t s_host_hw_tail = 0u;
static uint16_t s_host_hw_count = 0u;
#endif

static void uart_rx_ensure_init(void) {
    if (!s_rx_initialized) {
        s_rx_head = 0u;
        s_rx_tail = 0u;
        s_rx_count = 0u;
        s_rx_overflows = 0u;
        sem_init(&s_rx_sem, 0u, UART_RX_BUF_SIZE);
        s_rx_initialized = true;
    }
}

static bool uart_rx_push_byte_locked(uint8_t byte) {
    if (s_rx_count >= UART_RX_BUF_SIZE) {
        s_rx_overflows++;
        return false;
    }

    s_rx_buf[s_rx_head] = byte;
    s_rx_head = (uint16_t)((s_rx_head + 1u) % UART_RX_BUF_SIZE);
    s_rx_count++;
    return true;
}

static int uart_rx_pop_byte(void) {
    uint32_t flags = arch_irq_save();
    if (s_rx_count == 0u) {
        arch_irq_restore(flags);
        return -1;
    }

    uint8_t byte = s_rx_buf[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1u) % UART_RX_BUF_SIZE);
    s_rx_count--;
    arch_irq_restore(flags);
    return (int)byte;
}

uint32_t uart_rx_available(void) {
    uart_rx_ensure_init();
    uint32_t flags = arch_irq_save();
    uint32_t avail = s_rx_count;
    arch_irq_restore(flags);
    return avail;
}

int uart_getc_blocking(void) {
    uart_rx_ensure_init();
    sem_wait(&s_rx_sem);
    return uart_rx_pop_byte();
}

int uart_getc_nonblocking(void) {
    return uart_getc_nonblock();
}

#ifdef RADIX_HOST_TEST

void uart_early_init(void) {
    uart_host_reset();
}

void uart_putc(char c) {
    putchar(c);
    fflush(stdout);
}

int uart_getc_nonblock(void) {
    uart_rx_ensure_init();
    if (sem_trywait(&s_rx_sem)) {
        return uart_rx_pop_byte();
    }
    return -1;
}

void uart_rx_irq_enable(void) {
    uart_rx_ensure_init();
}

void uart_isr(uint32_t irq, void *arg) {
    (void)irq;
    (void)arg;
    uart_rx_ensure_init();

    uint32_t pushed = 0u;
    uint32_t flags = arch_irq_save();
    while (s_host_hw_count > 0u) {
        uint8_t byte = s_host_hw_fifo[s_host_hw_tail];
        s_host_hw_tail = (uint16_t)((s_host_hw_tail + 1u) % UART_RX_BUF_SIZE);
        s_host_hw_count--;

        if (uart_rx_push_byte_locked(byte)) {
            pushed++;
        }
    }
    arch_irq_restore(flags);

    for (uint32_t i = 0u; i < pushed; i++) {
        sem_post(&s_rx_sem);
    }
}

void uart_host_reset(void) {
    uint32_t flags = arch_irq_save();
    s_rx_head = 0u;
    s_rx_tail = 0u;
    s_rx_count = 0u;
    s_rx_overflows = 0u;
    s_host_hw_head = 0u;
    s_host_hw_tail = 0u;
    s_host_hw_count = 0u;
    sem_init(&s_rx_sem, 0u, UART_RX_BUF_SIZE);
    s_rx_initialized = true;
    arch_irq_restore(flags);
}

void uart_host_push_hw_fifo(char c) {
    uint32_t flags = arch_irq_save();
    if (s_host_hw_count < UART_RX_BUF_SIZE) {
        s_host_hw_fifo[s_host_hw_head] = (uint8_t)c;
        s_host_hw_head = (uint16_t)((s_host_hw_head + 1u) % UART_RX_BUF_SIZE);
        s_host_hw_count++;
    }
    arch_irq_restore(flags);
}

void uart_host_inject_rx_irq(char c) {
    uart_host_push_hw_fifo(c);

    bool entered_isr = false;
    if (!sched_in_isr()) {
        sched_enter_isr();
        entered_isr = true;
    }
    uart_isr(INTC_CPU_LINE_UART0, NULL);
    if (entered_isr) {
        sched_exit_isr();
    }
}

uint32_t uart_host_get_rx_overflows(void) {
    return s_rx_overflows;
}

#else

void uart_early_init(void) {
    /*
     * The ESP32-S3 ROM bootloader normally initializes UART0 at 115200 baud (8N1).
     * Ensure RX state is initialized and clear any pending interrupt flags.
     */
    s_rx_initialized = false;
    uart_rx_ensure_init();
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
    uart_rx_ensure_init();
    if (sem_trywait(&s_rx_sem)) {
        return uart_rx_pop_byte();
    }

    uint32_t status = REG_READ(UART_STATUS_REG(DR_REG_UART0_BASE));
    uint32_t rx_cnt = (status & UART_RXFIFO_CNT_MASK) >> UART_RXFIFO_CNT_SHIFT;

    if (rx_cnt == 0u) {
        return -1;
    }

    return (int)(REG_READ(UART_FIFO_REG(DR_REG_UART0_BASE)) & 0xFFu);
}

void uart_rx_irq_enable(void) {
    uart_rx_ensure_init();

    /* Set RX FIFO full threshold to 1 byte and enable RX timeout for burst tails */
    uint32_t conf1 = REG_READ(UART_CONF1_REG(DR_REG_UART0_BASE));
    conf1 &= ~UART_RXFIFO_FULL_THRHD_MASK;
    conf1 |= (1u << UART_RXFIFO_FULL_THRHD_SHIFT) | UART_RX_TOUT_EN;
    REG_WRITE(UART_CONF1_REG(DR_REG_UART0_BASE), conf1);

    REG_WRITE(UART_INT_CLR_REG(DR_REG_UART0_BASE),
              UART_RXFIFO_FULL_INT_CLR | UART_RXFIFO_TOUT_INT_CLR);
    REG_SET_BITS(UART_INT_ENA_REG(DR_REG_UART0_BASE),
                 UART_RXFIFO_FULL_INT_ENA | UART_RXFIFO_TOUT_INT_ENA);
}

void uart_isr(uint32_t irq, void *arg) {
    (void)irq;
    (void)arg;
    uart_rx_ensure_init();

    uint32_t int_st = REG_READ(UART_INT_ST_REG(DR_REG_UART0_BASE));
    uint32_t pushed = 0u;
    uint32_t flags = arch_irq_save();

    while (((REG_READ(UART_STATUS_REG(DR_REG_UART0_BASE)) & UART_RXFIFO_CNT_MASK) >> UART_RXFIFO_CNT_SHIFT) > 0u) {
        uint8_t byte = (uint8_t)(REG_READ(UART_FIFO_REG(DR_REG_UART0_BASE)) & 0xFFu);
        if (uart_rx_push_byte_locked(byte)) {
            pushed++;
        }
    }

    /* Clear interrupt flags AFTER draining the FIFO so level-triggered RX bits stay low */
    REG_WRITE(UART_INT_CLR_REG(DR_REG_UART0_BASE),
              (int_st != 0u) ? int_st : (UART_RXFIFO_FULL_INT_CLR | UART_RXFIFO_TOUT_INT_CLR));
    arch_irq_restore(flags);

    for (uint32_t i = 0u; i < pushed; i++) {
        sem_post(&s_rx_sem);
    }
}

#endif /* RADIX_HOST_TEST */
