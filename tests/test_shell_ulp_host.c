/*
 * tests/test_shell_ulp_host.c - Host Unit Test Suite for Step 6
 *
 * Verifies:
 *   1. ULP RISC-V Firmware Loader, RTC Mailbox & Background Cycles
 *   2. Non-blocking UART Line Editing, Backspace & Prompt Rendering
 *   3. Built-in Commands Dispatch (help, info, ps, amp, ulp, reboot)
 *   4. Task Inspection & Protected Task Termination (kill <id>)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <radix/types.h>
#include <radix/kprintf.h>
#include <radix/sched.h>
#include <radix/shell.h>
#include <radix/ulp.h>
#include <radix/amp.h>
#include <radix/pms.h>
#include <radix/syscall.h>

/* Mock Core 1 entry trampoline for host test execution */
void _start_core1(void) {
}

static uint8_t s_victim_stack[1024] __attribute__((aligned(16)));

static void dummy_victim_task(void *arg) {
    (void)arg;
    while (1) {
        sched_yield();
    }
}

/* ========================================================================= */
/* Test 1: ULP RISC-V Firmware Loader & RTC Mailbox                           */
/* ========================================================================= */
static void test_ulp_firmware_loader_and_mailbox(void) {
    kprintf("[TEST 1] Testing ULP RISC-V Firmware Loader & RTC Mailbox...\n");

    ulp_init();
    volatile ulp_rtc_mailbox_t *mb = ulp_get_mailbox();
    assert(mb != NULL);
    assert(mb->magic == ULP_MAILBOX_MAGIC);
    assert(mb->state == ULP_STATE_OFF);
    assert(mb->wakeup_count == 0);
    assert(mb->sensor_sample == 0);
    assert(mb->last_cmd == ULP_CMD_NONE);
    assert(mb->cmd_ack_count == 0);

    /* Validation: reject NULL, 0 size, or overflow beyond mailbox */
    const uint8_t dummy_fw[16] = { 0x13, 0x00, 0x00, 0x00 };
    assert(ulp_load_firmware(NULL, 16) == false);
    assert(ulp_load_firmware(dummy_fw, 0) == false);
    assert(ulp_load_firmware(dummy_fw, 0x1F01) == false);

    /* Load default compiled RV32IMC firmware */
    assert(ulp_load_default_firmware() == true);
    assert(mb->state == ULP_STATE_LOADED);
    assert(mb->fw_size_bytes > 0);

    /* Verify first instruction written to RTC Slow Memory base */
    const uint8_t *base = ulp_get_rtc_slow_mem_base();
    assert(base != NULL);
    uint32_t first_instr = *(const uint32_t *)(const void *)base;
    assert(first_instr == 0x50002537); /* lui a0, 0x50002 */

    /* Start ULP coprocessor with 50,000 us (50 ms) wakeup timer */
    assert(ulp_start(50000) == true);
    assert(mb->state == ULP_STATE_RUNNING);
    assert(mb->wakeup_period_us == 50000);
    assert(mb->wakeup_count == 1);
    assert(mb->sensor_sample == 1007); /* 1000 + 1 * 7 */

    /* Simulate periodic wakeups */
    ulp_simulate_wakeup_cycle();
    assert(mb->wakeup_count == 2);
    assert(mb->sensor_sample == 1014); /* 1000 + 2 * 7 */

    /* Send command ULP_CMD_SAMPLE_NOW */
    assert(ulp_send_command(ULP_CMD_SAMPLE_NOW) == true);
    assert(mb->cmd_ack_count == 1);
    assert(mb->last_cmd == ULP_CMD_NONE);
    assert(mb->sensor_sample == (2000 + mb->wakeup_count * 3));

    /* Send command ULP_CMD_RESET_STATS */
    assert(ulp_send_command(ULP_CMD_RESET_STATS) == true);
    assert(mb->wakeup_count == 0);
    assert(mb->sensor_sample == 0);
    assert(mb->cmd_ack_count == 2);

    /* Stop ULP coprocessor */
    ulp_stop();
    assert(mb->state == ULP_STATE_HALTED);
    assert(ulp_send_command(ULP_CMD_SAMPLE_NOW) == false);

    kprintf("[TEST 1] ULP RISC-V Firmware Loader & RTC Mailbox PASSED.\n\n");
}

/* ========================================================================= */
/* Test 2: Non-blocking UART Line Editing & Backspace                         */
/* ========================================================================= */
static void test_uart_line_editing(void) {
    kprintf("[TEST 2] Testing Non-blocking UART Line Editing & Backspace...\n");

    shell_init();
    uint32_t cmds_before = shell_get_executed_cmd_count();

    /*
     * Simulate typing error "xy", erasing both via backspace 0x7F and '\b',
     * then typing "ps" and pressing Enter '\n'.
     */
    assert(shell_poll_char('x') == false);
    assert(shell_poll_char('y') == false);
    assert(shell_poll_char(0x7F) == false); /* Deletes 'y' */
    assert(shell_poll_char('\b') == false); /* Deletes 'x' */
    assert(shell_poll_char('p') == false);
    assert(shell_poll_char('s') == false);
    assert(shell_poll_char('\n') == true);  /* Triggers line execution */

    assert(shell_get_executed_cmd_count() == cmds_before + 1);

    /* Test empty line input */
    assert(shell_poll_char(' ') == false);
    assert(shell_poll_char(' ') == false);
    assert(shell_poll_char('\r') == true);
    /* Executed command count unchanged for empty line */
    assert(shell_get_executed_cmd_count() == cmds_before + 1);

    kprintf("[TEST 2] Non-blocking UART Line Editing & Backspace PASSED.\n\n");
}

/* ========================================================================= */
/* Test 3: Built-in Commands Dispatch                                        */
/* ========================================================================= */
static void test_builtin_commands(void) {
    kprintf("[TEST 3] Testing Built-in Commands Dispatch...\n");

    assert(shell_execute_line("help") == SHELL_OK);
    assert(shell_execute_line("info") == SHELL_OK);
    assert(shell_execute_line("ps") == SHELL_OK);
    assert(shell_execute_line("amp") == SHELL_OK);
    assert(shell_execute_line("ulp") == SHELL_OK);
    assert(shell_execute_line("ulp start") == SHELL_OK);
    assert(shell_execute_line("ulp sample") == SHELL_OK);
    assert(shell_execute_line("ulp reset") == SHELL_OK);
    assert(shell_execute_line("ulp stop") == SHELL_OK);
    assert(shell_execute_line("ulp invalid_sub") == SHELL_ERR_BAD_ARG);

    /* Test unknown command */
    assert(shell_execute_line("non_existent_command") == SHELL_ERR_UNKNOWN_CMD);

    /* Test reboot command */
    assert(shell_was_reboot_requested() == false);
    assert(shell_execute_line("reboot") == SHELL_OK);
    assert(shell_was_reboot_requested() == true);

    kprintf("[TEST 3] Built-in Commands Dispatch PASSED.\n\n");
}

/* ========================================================================= */
/* Test 4: Task Inspection & kill <id> Command                               */
/* ========================================================================= */
static void test_task_inspection_and_kill(void) {
    kprintf("[TEST 4] Testing Task Inspection & kill <id> Command...\n");

    sched_init();

    int victim_id = task_create("victim_task", dummy_victim_task, NULL, 2,
                                s_victim_stack, sizeof(s_victim_stack));
    assert(victim_id > 0);

    /* Verify victim task is visible in scheduler table */
    bool found_victim = false;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        const tcb_t *t = sched_get_task_by_index(i);
        if (t && t->task_id == (uint32_t)victim_id && t->state == TASK_STATE_READY) {
            found_victim = true;
            break;
        }
    }
    assert(found_victim);

    /* Refuse killing idle task (id 0) */
    assert(sched_kill_task(0) == false);
    assert(shell_execute_line("kill 0") == SHELL_ERR_BAD_ARG);

    /* Refuse killing non-existent task */
    assert(shell_execute_line("kill 999") == SHELL_ERR_NOT_FOUND);

    /* Kill victim task */
    char kill_cmd[32];
    snprintf(kill_cmd, sizeof(kill_cmd), "kill %d", victim_id);
    assert(shell_execute_line(kill_cmd) == SHELL_OK);

    /* Verify state changed to TASK_STATE_UNUSED */
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        const tcb_t *t = sched_get_task_by_index(i);
        if (t && t->task_id == (uint32_t)victim_id) {
            assert(t->state == TASK_STATE_UNUSED);
        }
    }

    kprintf("[TEST 4] Task Inspection & kill <id> Command PASSED.\n\n");
}

/* ========================================================================= */
/* Main Test Runner                                                          */
/* ========================================================================= */
int main(void) {
    kprintf("=====================================================\n");
    kprintf("  RadixOS Step 6: Shell & RISC-V ULP Test Suite\n");
    kprintf("=====================================================\n\n");

    /* Subsystem prerequisites */
    world_init();
    pms_init();
    amp_init();

    test_ulp_firmware_loader_and_mailbox();
    test_uart_line_editing();
    test_builtin_commands();
    test_task_inspection_and_kill();

    kprintf("=====================================================\n");
    kprintf(" [SUCCESS] All Step 6 Shell & ULP Tests Passed!\n");
    kprintf("=====================================================\n");

    return 0;
}
