#include <radix/hal.h>
#include <radix/types.h>
#include <radix/sched.h>
#include <radix/kprintf.h>

#ifdef RADIX_HOST_TEST

static uint64_t s_host_ticks = 0;

void systimer_init(uint32_t tick_rate_hz) {
    (void)tick_rate_hz;
    s_host_ticks = 0;
}

uint64_t systimer_get_ticks(void) {
    return s_host_ticks;
}

void systimer_host_step_tick(void) {
    s_host_ticks++;
    sched_tick();
}

#else

#include <esp32s3_soc.h>

#define SYSTIMER_CLK_FREQ_HZ 16000000ULL

static volatile uint64_t s_system_ticks = 0;

void systimer_init(uint32_t tick_rate_hz) {
    if (tick_rate_hz == 0) {
        tick_rate_hz = 100; /* Default 100 Hz (10ms tick) */
    }

    uint32_t period_cycles = (uint32_t)(SYSTIMER_CLK_FREQ_HZ / tick_rate_hz);

    /* 1. Enable Systimer clock gate */
    REG_SET_BITS(SYSTIMER_CONF_REG, SYSTIMER_CLK_EN_BIT);

    /* 2. Configure Target 0 period */
    /* Target0 Period: 26 bits (bits 0..25) */
    uint32_t target0_conf = (period_cycles & 0x03FFFFFF) |
                            SYSTIMER_TARGET0_PERIOD_EN |
                            SYSTIMER_TARGET0_WORK_EN;
    REG_WRITE(SYSTIMER_TARGET0_CONF_REG, target0_conf);

    /* 3. Enable Target 0 interrupt in Systimer */
    REG_WRITE(SYSTIMER_INT_CLR_REG, SYSTIMER_INT_TARGET0_BIT);
    REG_SET_BITS(SYSTIMER_INT_ENA_REG, SYSTIMER_INT_TARGET0_BIT);

    kprintf("[SYSTIMER] Configured 52-bit hardware timer @ %u Hz (Period=%u cycles)\n",
            (unsigned int)tick_rate_hz, (unsigned int)period_cycles);
}

uint64_t systimer_get_ticks(void) {
    return s_system_ticks;
}

/*
 * Called from radix_level1_interrupt_handler when Systimer triggers
 */
void systimer_isr(void) {
    uint32_t int_st = REG_READ(SYSTIMER_INT_RAW_REG);
    if (int_st & SYSTIMER_INT_TARGET0_BIT) {
        /* Clear interrupt status */
        REG_WRITE(SYSTIMER_INT_CLR_REG, SYSTIMER_INT_TARGET0_BIT);
        s_system_ticks++;
        sched_tick();
    }
}

#endif /* RADIX_HOST_TEST */
