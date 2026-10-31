/*
 * RadixOS ESP32-S3 RISC-V ULP Coprocessor Firmware
 * Executes in RTC Slow Memory to perform background sensing and telemetry.
 */

#include <stdint.h>

#define DR_REG_RTCCNTL_BASE     0x60008000
#define RTC_CNTL_COCPU_CTRL_REG (DR_REG_RTCCNTL_BASE + 0x0100)
#define RTC_CNTL_COCPU_DONE     (1U << 21)

#define ULP_MAILBOX_ADDR        0x50001F00

#define ULP_CMD_NONE            0
#define ULP_CMD_SAMPLE_NOW      1
#define ULP_CMD_RESET_STATS     2

typedef struct {
    volatile uint32_t magic;
    volatile uint32_t state;
    volatile uint32_t wakeup_count;
    volatile uint32_t sensor_sample;
    volatile uint32_t last_cmd;
    volatile uint32_t cmd_ack_count;
    volatile uint32_t wakeup_period_us;
    volatile uint32_t fw_size_bytes;
} ulp_rtc_mailbox_t;

void _ulp_start(void) __attribute__((section(".entry")));

void _ulp_start(void)
{
    volatile ulp_rtc_mailbox_t *mb = (volatile ulp_rtc_mailbox_t *)ULP_MAILBOX_ADDR;

    /* Increment wakeup cycle count */
    mb->wakeup_count++;

    /* Deterministic telemetry update */
    mb->sensor_sample = 1000 + (mb->wakeup_count * 7);

    /* Process pending commands from main Xtensa cores */
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

    /* Signal COCPU DONE to RTC Controller */
    volatile uint32_t *cocpu_ctrl = (volatile uint32_t *)RTC_CNTL_COCPU_CTRL_REG;
    *cocpu_ctrl |= RTC_CNTL_COCPU_DONE;

    /* Wait for sleep power-down */
    while (1) {
        __asm__ volatile ("nop");
    }
}
