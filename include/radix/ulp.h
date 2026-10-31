#ifndef RADIX_ULP_H
#define RADIX_ULP_H

#include <radix/types.h>

#define ULP_MAILBOX_MAGIC 0x52554C50u /* "RULP" */

typedef enum {
    ULP_STATE_OFF = 0,
    ULP_STATE_LOADED = 1,
    ULP_STATE_RUNNING = 2,
    ULP_STATE_HALTED = 3
} ulp_state_t;

typedef enum {
    ULP_CMD_NONE = 0,
    ULP_CMD_SAMPLE_NOW = 1,
    ULP_CMD_RESET_STATS = 2
} ulp_cmd_t;

/*
 * Shared RTC Slow Memory Mailbox structure between Xtensa core and ULP RISC-V.
 * Located at offset 0x1F00 in RTC Slow Memory (0x50001F00 on ESP32-S3).
 */
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

void ulp_init(void);
bool ulp_load_firmware(const uint8_t *fw_bin, size_t size_bytes);
bool ulp_load_default_firmware(void);
bool ulp_start(uint32_t wakeup_period_us);
void ulp_stop(void);
volatile ulp_rtc_mailbox_t *ulp_get_mailbox(void);
bool ulp_send_command(uint32_t cmd);
void ulp_simulate_wakeup_cycle(void);
const uint8_t *ulp_get_rtc_slow_mem_base(void);

#endif /* RADIX_ULP_H */
