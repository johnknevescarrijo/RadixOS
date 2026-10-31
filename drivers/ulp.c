/*
 * drivers/ulp.c - ESP32-S3 RISC-V ULP Coprocessor Driver & RTC Mailbox
 *
 * Provides firmware loading, RTC slow memory management, and bidirectional
 * mailbox communication with the ULP RV32IMC coprocessor.
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

/*
 * Default compiled RV32IMC instructions for ulp/ulp_main.c:
 * Updates wakeup counter, telemetry sensor sample, processes commands,
 * and signals COCPU_DONE before sleeping.
 */
static const uint32_t g_ulp_default_rv32imc_fw[] = {
    0x50002537, /* lui  a0, 0x50002          ; Mailbox base page */
    0xf0050513, /* addi a0, a0, -256         ; a0 = 0x50001F00 (mailbox) */
    0x00852583, /* lw   a1, 8(a0)            ; a1 = mb->wakeup_count */
    0x00158593, /* addi a1, a1, 1            ; a1++ */
    0x00b52423, /* sw   a1, 8(a0)            ; mb->wakeup_count = a1 */
    0x00700613, /* li   a2, 7                ; a2 = 7 */
    0x02b60633, /* mul  a2, a2, a1           ; a2 = wakeup_count * 7 */
    0x3e860613, /* addi a2, a2, 1000         ; a2 = 1000 + wakeup_count * 7 */
    0x00c52623, /* sw   a2, 12(a0)           ; mb->sensor_sample = a2 */
    0x01052683, /* lw   a3, 16(a0)           ; a3 = mb->last_cmd */
    0x00068863, /* beqz a3, 16               ; if (last_cmd == 0) skip */
    0x01452703, /* lw   a4, 20(a0)           ; a4 = mb->cmd_ack_count */
    0x00170713, /* addi a4, a4, 1            ; a4++ */
    0x00e52a23, /* sw   a4, 20(a0)           ; mb->cmd_ack_count = a4 */
    0x00052823, /* sw   zero, 16(a0)         ; mb->last_cmd = ULP_CMD_NONE */
    0x60008537, /* lui  a0, 0x60008          ; RTCCNTL base 0x60008000 */
    0x00200593, /* li   a1, 0x200000         ; BIT(21) = RTC_CNTL_COCPU_DONE */
    0x10052603, /* lw   a2, 256(a0)          ; RTC_CNTL_COCPU_CTRL_REG */
    0x00b66633, /* or   a2, a2, a1           ; set COCPU_DONE */
    0x10c52023, /* sw   a2, 256(a0)          ; trigger done */
    0x0000006f  /* j    .                    ; halt / sleep loop */
};

#ifdef RADIX_HOST_TEST
/* Host memory buffer simulating 8 KB of RTC Slow Memory */
static uint8_t g_host_rtc_slow_mem[RTC_SLOWMEM_SIZE] __attribute__((aligned(4)));
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
    /* Disable timer and gate ULP clock */
    REG_CLR_BITS(RTC_CNTL_ULP_CP_TIMER_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN);
    REG_CLR_BITS(RTC_CNTL_COCPU_CTRL_REG, RTC_CNTL_COCPU_CLK_FO);
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

#ifndef RADIX_HOST_TEST
    /* Enable ULP COCPU clock */
    REG_SET_BITS(RTC_CNTL_COCPU_CTRL_REG, RTC_CNTL_COCPU_CLK_FO);

    /* Set wakeup timer interval */
    REG_WRITE(RTC_CNTL_ULP_CP_TIMER_1_REG, wakeup_period_us);

    /* Enable ULP periodic sleep timer */
    REG_SET_BITS(RTC_CNTL_ULP_CP_TIMER_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN);
#endif

    mb->wakeup_period_us = wakeup_period_us;
    mb->state = ULP_STATE_RUNNING;

    /* Execute initial telemetry cycle */
    ulp_simulate_wakeup_cycle();
    return true;
}

void ulp_stop(void) {
    volatile ulp_rtc_mailbox_t *mb = ulp_get_mailbox();

#ifndef RADIX_HOST_TEST
    REG_CLR_BITS(RTC_CNTL_ULP_CP_TIMER_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN);
    REG_CLR_BITS(RTC_CNTL_COCPU_CTRL_REG, RTC_CNTL_COCPU_CLK_FO);
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
    ulp_simulate_wakeup_cycle();
    return true;
}

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
