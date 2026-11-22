/*
 * RadixOS ESP32-S3 RISC-V ULP Coprocessor Firmware
 * Executes in RTC Slow Memory (0x50000000) to perform background sensing and telemetry.
 */

#include <stdint.h>

#define DR_REG_RTCCNTL_BASE     0x60008000
#define RTC_CNTL_COCPU_CTRL_REG (DR_REG_RTCCNTL_BASE + 0x0104)
#define RTC_CNTL_COCPU_DONE     (1U << 25)

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

void _ulp_start(void) __attribute__((naked, section(".entry")));
void _ulp_main(volatile ulp_rtc_mailbox_t *mb) __attribute__((noreturn));

/*
 * Reset vector at 0x50000000:
 * Initializes a0 to the shared RTC mailbox (0x50001F00), places the stack
 * right below the mailbox, and jumps into _ulp_main.
 */
void _ulp_start(void)
{
    __asm__ volatile (
        "lui  a0, 0x50002\n"
        "addi a0, a0, -256\n"
        "mv   sp, a0\n"
        "j    _ulp_main\n"
    );
}

void _ulp_main(volatile ulp_rtc_mailbox_t *mb)
{
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

    /* Signal COCPU_DONE (BIT 25) to ESP32-S3 RTC Controller (0x60008104) */
    volatile uint32_t *cocpu_ctrl = (volatile uint32_t *)RTC_CNTL_COCPU_CTRL_REG;
    *cocpu_ctrl |= RTC_CNTL_COCPU_DONE;

    /* Wait for RTC sleep power-down */
    while (1) {
        __asm__ volatile ("nop");
    }
}
