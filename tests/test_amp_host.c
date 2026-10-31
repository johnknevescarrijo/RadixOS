/*
 * tests/test_amp_host.c - Host Simulation & Unit Test Suite for Step 4
 *
 * Verifies:
 *   1. Core 1 Boot Sequence & Hardware Reset / Halt Control
 *   2. Hardware Inter-Processor Interrupts (IPIs), Coalescing & Bitmask IRQ
 *   3. Zero-Copy Cross-Core Ring Buffer IPC (Ping-Pong & Work-Request Dispatch)
 *   4. Remote Cross-Core Function Execution (amp_call_on_core)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <radix/types.h>
#include <radix/kprintf.h>
#include <radix/atomic.h>
#include <radix/ipc.h>
#include <radix/amp.h>

/* Mock Core 1 entry trampoline for host test execution */
void _start_core1(void) {
    /* Dummy symbol representing arch/xtensa/boot_core1.S entry */
}

/* ========================================================================= */
/* Test 1: Core 1 Boot and Halt Lifecycle                                    */
/* ========================================================================= */
static void test_core1_boot_and_halt(void) {
    kprintf("[TEST 1] Testing Core 1 boot and halt sequence...\n");

    host_set_current_core_id(RADIX_CORE_0);
    amp_shared_state_t *st = amp_get_state();
    assert(st != NULL);

    st->core_state[RADIX_CORE_1] = CORE_STATE_OFFLINE;

    /* Boot Core 1 using system hardware driver */
    system_boot_core1(_start_core1);

    /* Verify boot entry address was recorded */
    assert(system_get_core1_boot_addr() == (uint32_t)(uintptr_t)_start_core1);

    /* Verify control register flags */
    uint32_t ctrl0 = system_get_core1_ctrl0_reg();
    /* CLKGATE_EN (bit 1) must be set */
    assert((ctrl0 & (1UL << 1)) != 0);
    /* RUNSTALL (bit 0) must be cleared */
    assert((ctrl0 & (1UL << 0)) == 0);
    /* RESETING (bit 2) must be cleared after reset pulse */
    assert((ctrl0 & (1UL << 2)) == 0);

    /* Core 1 state must now be BOOTING */
    assert(st->core_state[RADIX_CORE_1] == CORE_STATE_BOOTING);

    /* Transition Core 1 to ONLINE (simulating Core 1 startup) */
    st->core_state[RADIX_CORE_1] = CORE_STATE_ONLINE;
    assert(st->core_state[RADIX_CORE_1] == CORE_STATE_ONLINE);

    /* Now halt Core 1 */
    system_halt_core1();

    ctrl0 = system_get_core1_ctrl0_reg();
    /* RUNSTALL (bit 0) must be set */
    assert((ctrl0 & (1UL << 0)) != 0);
    /* CLKGATE_EN (bit 1) must be cleared */
    assert((ctrl0 & (1UL << 1)) == 0);

    /* Core 1 state must now be HALTED */
    assert(st->core_state[RADIX_CORE_1] == CORE_STATE_HALTED);

    kprintf("[TEST 1] Core 1 boot and halt sequence PASSED.\n");
}

/* ========================================================================= */
/* Test 2: Hardware IPI Generation, Coalescing and Clearing                  */
/* ========================================================================= */
static void test_ipi_generation_and_handling(void) {
    kprintf("[TEST 2] Testing hardware IPI delivery & bitmask coalescing...\n");

    ipi_init();
    assert(ipi_get_hardware_intr_reg(RADIX_CORE_0) == 0);
    assert(ipi_get_hardware_intr_reg(RADIX_CORE_1) == 0);

    host_set_current_core_id(RADIX_CORE_0);
    amp_shared_state_t *st = amp_get_state();
    st->ipi_tx_count[RADIX_CORE_0] = 0;
    st->ipi_rx_count[RADIX_CORE_1] = 0;
    st->pending_ipi[RADIX_CORE_1] = 0;
    st->resched_requests[RADIX_CORE_1] = 0;

    /* Invalid core target or 0 reason should fail */
    assert(ipi_send(RADIX_NUM_CORES, IPI_REASON_PING) == false);
    assert(ipi_send(RADIX_CORE_1, 0) == false);

    /* Send single IPI */
    bool ok = ipi_send(RADIX_CORE_1, IPI_REASON_PING);
    assert(ok == true);
    assert(st->ipi_tx_count[RADIX_CORE_0] == 1);
    assert(ipi_get_hardware_intr_reg(RADIX_CORE_1) == 1);
    assert(st->pending_ipi[RADIX_CORE_1] == IPI_REASON_PING);

    /* Coalesce additional reasons before Core 1 processes them */
    ok = ipi_send(RADIX_CORE_1, IPI_REASON_RESCHEDULE);
    assert(ok == true);
    ok = ipi_send(RADIX_CORE_1, IPI_REASON_IPC_NOTIFY);
    assert(ok == true);
    assert(st->ipi_tx_count[RADIX_CORE_0] == 3);

    uint32_t expected_mask = (IPI_REASON_PING | IPI_REASON_RESCHEDULE | IPI_REASON_IPC_NOTIFY);
    assert(st->pending_ipi[RADIX_CORE_1] == expected_mask);

    /* Switch context to Core 1 and handle the IPI */
    host_set_current_core_id(RADIX_CORE_1);
    uint32_t handled = ipi_handle_irq();

    assert(handled == expected_mask);
    assert(st->pending_ipi[RADIX_CORE_1] == 0);
    assert(st->ipi_rx_count[RADIX_CORE_1] == 1);
    assert(st->resched_requests[RADIX_CORE_1] == 1);
    assert(ipi_get_hardware_intr_reg(RADIX_CORE_1) == 0);

    /* Calling again when nothing is pending should be a no-op */
    assert(ipi_handle_irq() == 0);
    assert(st->ipi_rx_count[RADIX_CORE_1] == 1);

    kprintf("[TEST 2] Hardware IPI generation & handling PASSED.\n");
}

/* ========================================================================= */
/* Test 3: Zero-Copy Cross-Core Ring Buffer IPC                              */
/* ========================================================================= */
static void test_cross_core_zero_copy_ipc(void) {
    kprintf("[TEST 3] Testing cross-core zero-copy SPSC IPC channel...\n");

    host_set_current_core_id(RADIX_CORE_0);
    amp_init();
    amp_shared_state_t *st = amp_get_state();
    st->core_state[RADIX_CORE_1] = CORE_STATE_ONLINE;

    /* Core 0 sends AMP_MSG_WORK_REQUEST (value 21) to Core 1 */
    uint32_t work_in = 21;
    bool ok = amp_send_to_core(RADIX_CORE_1, AMP_MSG_WORK_REQUEST, &work_in, sizeof(work_in));
    assert(ok == true);

    /* Core 0 sends AMP_MSG_PING to Core 1 */
    ok = amp_send_to_core(RADIX_CORE_1, AMP_MSG_PING, NULL, 0);
    assert(ok == true);

    /* Switch to Core 1 and poll messages */
    host_set_current_core_id(RADIX_CORE_1);
    uint32_t reasons = amp_core1_poll_once();
    assert(reasons & IPI_REASON_IPC_NOTIFY);
    assert(st->core_heartbeats[RADIX_CORE_1] >= 1);

    /* Switch back to Core 0 to receive responses */
    host_set_current_core_id(RADIX_CORE_0);

    uint32_t msg_id = 0;
    uint32_t work_out = 0;
    uint16_t len = 0;

    /* Response 1: AMP_MSG_WORK_RESPONSE (expected: 21 * 2 + 1 = 43) */
    ok = amp_recv_from_peer(&msg_id, &work_out, &len);
    assert(ok == true);
    assert(msg_id == AMP_MSG_WORK_RESPONSE);
    assert(len == sizeof(uint32_t));
    assert(work_out == 43);

    /* Response 2: AMP_MSG_PONG */
    ok = amp_recv_from_peer(&msg_id, NULL, &len);
    assert(ok == true);
    assert(msg_id == AMP_MSG_PONG);
    assert(len == 0);

    /* Queue must now be empty */
    ok = amp_recv_from_peer(&msg_id, NULL, &len);
    assert(ok == false);

    kprintf("[TEST 3] Cross-core zero-copy SPSC IPC channel PASSED.\n");
}

/* ========================================================================= */
/* Test 4: Remote Cross-Core Function Execution                              */
/* ========================================================================= */
typedef struct {
    uint32_t input;
    uint32_t output;
    bool     executed;
} remote_call_ctx_t;

static void remote_worker(void *arg) {
    remote_call_ctx_t *ctx = (remote_call_ctx_t *)arg;
    if (ctx) {
        ctx->output = ctx->input * 10;
        ctx->executed = true;
    }
}

static void test_remote_function_call(void) {
    kprintf("[TEST 4] Testing remote cross-core function call (amp_call_on_core)...\n");

    host_set_current_core_id(RADIX_CORE_0);
    amp_shared_state_t *st = amp_get_state();

    remote_call_ctx_t ctx = {
        .input = 42,
        .output = 0,
        .executed = false
    };

    /* Schedule function to run on Core 1 */
    bool ok = amp_call_on_core(RADIX_CORE_1, remote_worker, &ctx);
    assert(ok == true);
    assert(st->call_mailbox[RADIX_CORE_1].fn == remote_worker);
    assert(st->call_mailbox[RADIX_CORE_1].done == 0);
    assert(ctx.executed == false);

    /* Switch context to Core 1 and dispatch IRQ */
    host_set_current_core_id(RADIX_CORE_1);
    uint32_t reasons = ipi_handle_irq();

    assert(reasons & IPI_REASON_CALL_FUNC);
    assert(ctx.executed == true);
    assert(ctx.output == 420);
    assert(st->call_mailbox[RADIX_CORE_1].done == 1);

    kprintf("[TEST 4] Remote cross-core function execution PASSED.\n");
}

/* ========================================================================= */
/* Main Test Runner                                                          */
/* ========================================================================= */
int main(void) {
    kprintf("=====================================================\n");
    kprintf("  RadixOS Step 4: AMP Dual-Core & IPI Test Suite\n");
    kprintf("=====================================================\n");

    test_core1_boot_and_halt();
    test_ipi_generation_and_handling();
    test_cross_core_zero_copy_ipc();
    test_remote_function_call();

    kprintf("=====================================================\n");
    kprintf(" [SUCCESS] All Step 4 AMP Host Simulation Tests Passed!\n");
    kprintf("=====================================================\n");
    return 0;
}
