/*
 * drivers/ulp.c - ESP32-S3 RISC-V ULP Coprocessor Driver & RTC Mailbox
 *
 * Provides firmware loading, RTC slow memory management, hardware COCPU/timer
 * control on ESP32-S3, and bidirectional mailbox communication with the ULP
 * RV32IMC coprocessor.
 */

#include <radix/types.h>
#include <radix/ulp.h>

#ifndef RADIX_HOST_TEST
#include <esp32s3_soc.h>
#else
/* Hardware definitions for host simulation */
#define DR_REG_RTC_SLOWMEM_BASE 0x50000000
#define RTC_SLOWMEM_SIZE        8192
#define RTC_ULP_MAILBOX_OFFSET  0x1F00
#endif

#ifdef RADIX_HAVE_ULP_FW_BIN_H
#include "ulp_fw_bin.h"
#else
/*
 * Explicit, documented pre-assembled RV32IMC fallback firmware for ulp/ulp_main.c
 * (used only when no RISC-V cross-compiler is installed on the build host).
 * Fits well within RTC Slow Memory before mailbox offset 0x1F00.
 */
static const uint32_t g_ulp_default_rv32imc_fw[] = {
    0x50002537, /* lui  a0, 0x50002          ; Mailbox page base */
    0xF0050513, /* addi a0, a0, -256         ; a0 = 0x50001F00 (mailbox) */
    0x00852583, /* lw   a1, 8(a0)            ; a1 = mb->wakeup_count */
    0x00158593, /* addi a1, a1, 1            ; a1++ */
    0x00B52423, /* sw   a1, 8(a0)            ; mb->wakeup_count = a1 */
    0x00700613, /* li   a2, 7                ; a2 = 7 */
    0x02B60633, /* mul  a2, a2, a1           ; a2 = wakeup_count * 7 */
    0x3E860613, /* addi a2, a2, 1000         ; a2 = 1000 + wakeup_count * 7 */
    0x00C52623, /* sw   a2, 12(a0)           ; mb->sensor_sample = a2 */
    0x01052683, /* lw   a3, 16(a0)           ; a3 = mb->last_cmd */
    0x00068863, /* beqz a3, 16               ; if (last_cmd == 0) skip */
    0x01452703, /* lw   a4, 20(a0)           ; a4 = mb->cmd_ack_count */
    0x00170713, /* addi a4, a4, 1            ; a4++ */
    0x00E52A23, /* sw   a4, 20(a0)           ; mb->cmd_ack_count = a4 */
    0x00052823, /* sw   zero, 16(a0)         ; mb->last_cmd = ULP_CMD_NONE */
    0x60008537, /* lui  a0, 0x60008          ; RTCCNTL base 0x60008000 */
    0x020005B7, /* lui  a1, 0x2000           ; BIT(25) = RTC_CNTL_COCPU_DONE */
    0x10452603, /* lw   a2, 260(a0)          ; RTC_CNTL_COCPU_CTRL_REG (+0x104) */
    0x00B66633, /* or   a2, a2, a1           ; set COCPU_DONE */
    0x10C52223, /* sw   a2, 260(a0)          ; trigger done */
    0x0000006F  /* j    .                    ; wait for sleep power-down */
};
#endif

#ifdef RADIX_HOST_TEST
/* Host memory buffer simulating 8 KB of RTC Slow Memory */
static uint8_t g_host_rtc_slow_mem[RTC_SLOWMEM_SIZE] __attribute__((aligned(4)));
#else
static void ulp_delay_cycles(volatile uint32_t iters) {
    while (iters--) {
        __asm__ volatile ("nop");
    }
}
#endif

const uint8_t *ulp_get_rtc_slow_mem_base(void) {
#ifdef RADIX_HOST_TEST
    return g_host_rtc_slow_mem;
#else
    return (const uint8_t *)DR_REG_RTC_SLOWMEM_BASE;
#endif
}

volatile ulp_rtc_mailbox_t *ulp_get_mailbox(void) {
#ifdef RADIX_HOST_TEST
    return (volatile ulp_rtc_mailbox_t *)&g_host_rtc_slow_mem[RTC_ULP_MAILBOX_OFFSET];
#else
    return (volatile ulp_rtc_mailbox_t *)(DR_REG_RTC_SLOWMEM_BASE + RTC_ULP_MAILBOX_OFFSET);
#endif
}

void ulp_init(void) {
    uint8_t *base;
#ifdef RADIX_HOST_TEST
    base = g_host_rtc_slow_mem;
#else
    base = (uint8_t *)DR_REG_RTC_SLOWMEM_BASE;
#endif

    /* Zero out RTC Slow Memory */
    for (size_t i = 0; i < RTC_SLOWMEM_SIZE; i++) {
        base[i] = 0;
    }

    volatile ulp_rtc_mailbox_t *mb = ulp_get_mailbox();
    mb->magic = ULP_MAILBOX_MAGIC;
    mb->state = ULP_STATE_OFF;
    mb->wakeup_count = 0;
    mb->sensor_sample = 0;
    mb->last_cmd = ULP_CMD_NONE;
    mb->cmd_ack_count = 0;
    mb->wakeup_period_us = 0;
    mb->fw_size_bytes = 0;

#ifndef RADIX_HOST_TEST
    /* Disable timer, hold RISC-V COCPU in reset, and gate ULP clock */
    REG_CLR_BITS(RTC_CNTL_ULP_CP_TIMER_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN);
    REG_SET_BITS(RTC_CNTL_COCPU_CTRL_REG, RTC_CNTL_COCPU_SHUT_RESET_EN);
    REG_CLR_BITS(RTC_CNTL_COCPU_CTRL_REG, RTC_CNTL_COCPU_CLK_FO);
    __asm__ volatile ("memw" ::: "memory");
#endif
}

bool ulp_load_firmware(const uint8_t *fw_bin, size_t size_bytes) {
    if (fw_bin == NULL || size_bytes == 0 || size_bytes > RTC_ULP_MAILBOX_OFFSET) {
        return false;
    }

    uint8_t *base;
#ifdef RADIX_HOST_TEST
    base = g_host_rtc_slow_mem;
#else
    base = (uint8_t *)DR_REG_RTC_SLOWMEM_BASE;
#endif

    for (size_t i = 0; i < size_bytes; i++) {
        base[i] = fw_bin[i];
    }

    volatile ulp_rtc_mailbox_t *mb = ulp_get_mailbox();
    mb->fw_size_bytes = (uint32_t)size_bytes;
    mb->state = ULP_STATE_LOADED;

#ifndef RADIX_HOST_TEST
    __asm__ volatile ("memw" ::: "memory");
#endif

    return true;
}

bool ulp_load_default_firmware(void) {
    return ulp_load_firmware((const uint8_t *)g_ulp_default_rv32imc_fw,
                             sizeof(g_ulp_default_rv32imc_fw));
}

bool ulp_start(uint32_t wakeup_period_us) {
    volatile ulp_rtc_mailbox_t *mb = ulp_get_mailbox();
    if (mb->state != ULP_STATE_LOADED && mb->state != ULP_STATE_HALTED) {
        return false;
    }

    mb->wakeup_period_us = wakeup_period_us;
    mb->state = ULP_STATE_RUNNING;

#ifndef RADIX_HOST_TEST
    __asm__ volatile ("memw" ::: "memory");

    /* 1. Reset COCPU and force ULP-RISC-V clock on */
    REG_SET_BITS(RTC_CNTL_COCPU_CTRL_REG, RTC_CNTL_COCPU_SHUT_RESET_EN);
    REG_SET_BITS(RTC_CNTL_COCPU_CTRL_REG, RTC_CNTL_COCPU_CLK_FO);

    /* 2. Disable ULP timer and wait >= 1 RTC_SLOW_CLK cycle (~20 us) */
    REG_CLR_BITS(RTC_CNTL_ULP_CP_TIMER_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN);
    ulp_delay_cycles(4800);

    /* 3. Route DONE signal to RISC-V COCPU and enable clock gating */
    REG_SET_BITS(RTC_CNTL_COCPU_CTRL_REG,
                 RTC_CNTL_COCPU_DONE_FORCE | RTC_CNTL_COCPU_CLKGATE_EN);

    /* 4. Program sleep period in RTC slow clock cycles (~136 kHz internal RC) */
    uint32_t slp_cycles = (wakeup_period_us * 136u) / 1000u;
    if (slp_cycles == 0u) {
        slp_cycles = 1u;
    } else if (slp_cycles > 0x00FFFFFFu) {
        slp_cycles = 0x00FFFFFFu;
    }
    REG_WRITE(RTC_CNTL_ULP_CP_TIMER_1_REG,
              (slp_cycles << RTC_CNTL_ULP_CP_TIMER_SLP_CYCLE_S) &
                  RTC_CNTL_ULP_CP_TIMER_SLP_CYCLE_MASK);

    /* 5. Configure timer-driven wakeup and enable ULP sleep timer */
    REG_CLR_BITS(RTC_CNTL_ULP_CP_CTRL_REG, RTC_CNTL_ULP_CP_FORCE_START_TOP);
    REG_SET_BITS(RTC_CNTL_ULP_CP_TIMER_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN);

    /* 6. Select RISC-V COCPU (COCPU_SEL = 0) last per ESP32-S3 TRM/IDF-4510 */
    REG_CLR_BITS(RTC_CNTL_COCPU_CTRL_REG, RTC_CNTL_COCPU_SEL);

    /* 7. Clear spurious ULP/COCPU wakeup interrupts */
    ulp_delay_cycles(4800);
    REG_WRITE(RTC_CNTL_INT_CLR_REG,
              RTC_CNTL_COCPU_INT_CLR | RTC_CNTL_COCPU_TRAP_INT_CLR |
                  RTC_CNTL_ULP_CP_INT_CLR);
#else
    /* Host simulation executes initial wakeup cycle deterministically */
    ulp_simulate_wakeup_cycle();
#endif

    return true;
}

void ulp_stop(void) {
    volatile ulp_rtc_mailbox_t *mb = ulp_get_mailbox();

#ifndef RADIX_HOST_TEST
    REG_CLR_BITS(RTC_CNTL_ULP_CP_TIMER_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN);
    REG_SET_BITS(RTC_CNTL_COCPU_CTRL_REG, RTC_CNTL_COCPU_DONE);
    REG_SET_BITS(RTC_CNTL_COCPU_CTRL_REG, RTC_CNTL_COCPU_SHUT_RESET_EN);
    REG_CLR_BITS(RTC_CNTL_COCPU_CTRL_REG, RTC_CNTL_COCPU_CLK_FO);
    __asm__ volatile ("memw" ::: "memory");
#endif

    mb->state = ULP_STATE_HALTED;
}

bool ulp_send_command(uint32_t cmd) {
    if (cmd == ULP_CMD_NONE) {
        return false;
    }

    volatile ulp_rtc_mailbox_t *mb = ulp_get_mailbox();
    if (mb->state != ULP_STATE_RUNNING) {
        return false;
    }

    mb->last_cmd = cmd;
#ifndef RADIX_HOST_TEST
    __asm__ volatile ("memw" ::: "memory");
#else
    ulp_simulate_wakeup_cycle();
#endif
    return true;
}

#ifdef RADIX_HOST_TEST
void ulp_simulate_wakeup_cycle(void) {
    volatile ulp_rtc_mailbox_t *mb = ulp_get_mailbox();
    if (mb->state != ULP_STATE_RUNNING) {
        return;
    }

    mb->wakeup_count++;
    mb->sensor_sample = 1000 + (mb->wakeup_count * 7);

    uint32_t cmd = mb->last_cmd;
    if (cmd != ULP_CMD_NONE) {
        if (cmd == ULP_CMD_SAMPLE_NOW) {
            mb->sensor_sample = 2000 + (mb->wakeup_count * 3);
        } else if (cmd == ULP_CMD_RESET_STATS) {
            mb->wakeup_count = 0;
            mb->sensor_sample = 0;
        }
        mb->cmd_ack_count++;
        mb->last_cmd = ULP_CMD_NONE;
    }
}
#endif
