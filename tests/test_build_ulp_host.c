/*
 * tests/test_build_ulp_host.c - Host Unit Test Suite for Build, Image & ULP Invariants
 *
 * Verifies:
 *   1. ULP mailbox layout bounds (<= 256 B at 0x50001F00) & firmware size/alignment checks
 *   2. ULP state machine transitions (OFF -> LOADED -> RUNNING -> HALTED -> RUNNING)
 *   3. Generated ESP32-S3 ROM image (build/radixos.bin) compactness (< 256 KB) & header fields
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <radix/types.h>
#include <radix/kprintf.h>
#include <radix/ulp.h>

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_ulp_bounds_and_state_machine(void) {
    kprintf("[TEST 1] Testing ULP mailbox bounds, firmware limits & state machine...\n");

    /* Mailbox must fit inside the 256-byte region 0x50001F00..0x50001FFF */
    assert(sizeof(ulp_rtc_mailbox_t) <= 0x100u);

    ulp_init();
    volatile ulp_rtc_mailbox_t *mb = ulp_get_mailbox();
    assert(mb != NULL);
    assert(mb->magic == ULP_MAILBOX_MAGIC);
    assert(mb->state == ULP_STATE_OFF);

    /* Cannot start or send commands before firmware is loaded */
    assert(ulp_start(50000) == false);
    assert(ulp_send_command(ULP_CMD_SAMPLE_NOW) == false);

    /* Boundary checks on firmware size */
    static uint8_t max_fw[0x1F00];
    memset(max_fw, 0x13, sizeof(max_fw));
    assert(ulp_load_firmware(NULL, 4) == false);
    assert(ulp_load_firmware(max_fw, 0) == false);
    assert(ulp_load_firmware(max_fw, 0x1F01) == false);
    assert(ulp_load_firmware(max_fw, 0x2000) == false);
    assert(ulp_load_firmware(max_fw, 0x1F00) == true);
    assert(mb->fw_size_bytes == 0x1F00u);
    assert(mb->state == ULP_STATE_LOADED);

    /* Load default (compiled or fallback) RV32IMC firmware */
    ulp_init();
    assert(ulp_load_default_firmware() == true);
    assert(mb->state == ULP_STATE_LOADED);
    assert(mb->fw_size_bytes > 0u);
    assert(mb->fw_size_bytes <= 0x1F00u);
    /* RV32IMC instructions are 2-byte compressed or 4-byte aligned */
    assert((mb->fw_size_bytes % 2u) == 0u);

    const uint8_t *rtc_base = ulp_get_rtc_slow_mem_base();
    assert(rtc_base != NULL);
    assert(read_u32_le(rtc_base) == 0x50002537u); /* lui a0, 0x50002 */

    /* Start -> Stop -> Restart transition */
    assert(ulp_start(25000) == true);
    assert(mb->state == ULP_STATE_RUNNING);
    assert(mb->wakeup_period_us == 25000u);
    assert(mb->wakeup_count == 1u);

    assert(ulp_send_command(ULP_CMD_NONE) == false);
    assert(ulp_send_command(ULP_CMD_SAMPLE_NOW) == true);
    assert(mb->cmd_ack_count == 1u);

    ulp_stop();
    assert(mb->state == ULP_STATE_HALTED);
    assert(ulp_send_command(ULP_CMD_SAMPLE_NOW) == false);

    /* Restart from HALTED state */
    assert(ulp_start(10000) == true);
    assert(mb->state == ULP_STATE_RUNNING);
    assert(mb->wakeup_period_us == 10000u);

    kprintf("[TEST 1] ULP bounds & state machine PASSED.\n");
}

static void test_target_image_compactness_and_header(void) {
    kprintf("\n[TEST 2] Verifying ESP32-S3 flashable image (build/radixos.bin)...\n");

    FILE *fp = fopen("build/radixos.bin", "rb");
    if (!fp) {
        kprintf("[TEST 2] build/radixos.bin not present; skipping binary header check.\n");
        return;
    }

    assert(fseek(fp, 0, SEEK_END) == 0);
    long sz = ftell(fp);
    assert(sz > 24 + 32);
    /* Must NOT contain the ~7 MB DRAM->IRAM objcopy gap */
    assert(sz < 256 * 1024);
    assert(fseek(fp, 0, SEEK_SET) == 0);

    uint8_t hdr[24];
    assert(fread(hdr, 1, sizeof(hdr), fp) == sizeof(hdr));
    fclose(fp);

    /* Check ESP32-S3 ROM header magic (0xE9), segments (>=1), chip_id (0x0009), hash_appended (1) */
    assert(hdr[0] == 0xE9u);
    assert(hdr[1] >= 1u && hdr[1] <= 16u);
    uint32_t entry = read_u32_le(&hdr[4]);
    assert(entry >= 0x40378000u && entry < 0x403E0000u);
    assert(hdr[8] == 0xEEu);                  /* wp_pin disabled */
    assert(read_u16_le(&hdr[12]) == 0x0009u); /* ESP32-S3 chip_id */
    assert(hdr[23] == 0x01u);                 /* hash_appended */

    kprintf("  build/radixos.bin: %ld bytes, segments=%u, entry=0x%x, chip_id=0x0009\n",
            sz, (unsigned int)hdr[1], (unsigned int)entry);
    kprintf("[TEST 2] ESP32-S3 flashable image check PASSED.\n");
}

int main(void) {
    kprintf("=====================================================\n");
    kprintf("  RadixOS Build, Image & ULP Verification Suite\n");
    kprintf("=====================================================\n\n");

    test_ulp_bounds_and_state_machine();
    test_target_image_compactness_and_header();

    kprintf("\n=====================================================\n");
    kprintf(" [SUCCESS] All Build & ULP Verification Tests Passed!\n");
    kprintf("=====================================================\n");
    return 0;
}
