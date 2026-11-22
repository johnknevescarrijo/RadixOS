/*
 * tests/test_timers_uart_host.c - Host Unit Test Suite for Timed Sleep, ktimer_t & UART RX IRQ
 *
 * Verifies:
 *   1. task_sleep_ticks / task_sleep_ms, TASK_STATE_SLEEPING, exact wakeup & ISR preemption
 *   2. Software timers (ktimer_t): one-shot, periodic, stop, simultaneous, callback restart/stop
 *   3. Interrupt-driven UART0 RX ring buffer, blocking read wakeup, wrap-around & overflow
 *   4. Non-starvation of lower-priority tasks when high-priority shell blocks on UART RX
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <radix/types.h>
#include <radix/kprintf.h>
#include <radix/sched.h>
#include <radix/timer.h>
#include <radix/sync.h>
#include <radix/uart.h>
#include <radix/intc.h>
#include <radix/amp.h>
#include <radix/pms.h>
#include <radix/ulp.h>
#include <radix/shell.h>

#define TEST_STACK_SIZE 2048

/* Mock Core 1 entry trampoline for host test linking with kernel/amp.c / shell.c */
void _start_core1(void) {
}

static uint8_t s_stack_high[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_stack_med[TEST_STACK_SIZE]  __attribute__((aligned(16)));
static uint8_t s_stack_low[TEST_STACK_SIZE]  __attribute__((aligned(16)));

static void dummy_task(void *arg) {
    (void)arg;
}

typedef struct {
    uint32_t fires;
    bool     active_in_cb;
    uint64_t last_tick;
    ktimer_t *self;
    ktimer_t *other;
    uint32_t max_fires;
    uint64_t rearm_ticks;
} timer_test_ctx_t;

static void oneshot_cb(void *arg) {
    timer_test_ctx_t *ctx = (timer_test_ctx_t *)arg;
    ctx->fires++;
    ctx->active_in_cb = ktimer_is_active(ctx->self);
    ctx->last_tick = sched_get_ticks();
}

static void periodic_cb(void *arg) {
    timer_test_ctx_t *ctx = (timer_test_ctx_t *)arg;
    ctx->fires++;
    ctx->active_in_cb = ktimer_is_active(ctx->self);
    ctx->last_tick = sched_get_ticks();
}

static void oneshot_rearm_cb(void *arg) {
    timer_test_ctx_t *ctx = (timer_test_ctx_t *)arg;
    ctx->fires++;
    ctx->active_in_cb = ktimer_is_active(ctx->self);
    ctx->last_tick = sched_get_ticks();
    if (ctx->fires < ctx->max_fires) {
        assert(ktimer_start_ticks(ctx->self, ctx->rearm_ticks) == true);
    }
}

static void periodic_self_stop_cb(void *arg) {
    timer_test_ctx_t *ctx = (timer_test_ctx_t *)arg;
    ctx->fires++;
    ctx->last_tick = sched_get_ticks();
    if (ctx->fires >= ctx->max_fires) {
        assert(ktimer_stop(ctx->self) == true);
    }
}

static void cancel_other_cb(void *arg) {
    timer_test_ctx_t *ctx = (timer_test_ctx_t *)arg;
    ctx->fires++;
    if (ctx->other) {
        ktimer_stop(ctx->other);
    }
}

/* ========================================================================= */
/* Test 1: Timed Sleep & Preemptive Wakeup from ISR                          */
/* ========================================================================= */
static void test_task_sleep_and_wakeup(void) {
    kprintf("[TEST 1] Testing task_sleep_ticks / task_sleep_ms & ISR wakeup...\n");

    sched_init();

    int id_hi = task_create("high_task", dummy_task, NULL, 3, s_stack_high, sizeof(s_stack_high));
    int id_lo = task_create("low_task", dummy_task, NULL, 1, s_stack_low, sizeof(s_stack_low));
    assert(id_hi > 0 && id_lo > 0);

    /* Initial dispatch selects highest priority task (high_task) */
    sched_yield();
    tcb_t *hi = sched_get_current_task();
    assert(hi != NULL && strcmp(hi->name, "high_task") == 0);

    /* sleep_ticks(0) and sleep_ms(0) yield without sleeping */
    task_sleep_ticks(0);
    assert(hi->state == TASK_STATE_RUNNING);
    task_sleep_ms(0);
    assert(hi->state == TASK_STATE_RUNNING);

    /* Sleep high_task for 4 ticks -> low_task must run without starvation */
    task_sleep_ticks(4);
    assert(hi->state == TASK_STATE_SLEEPING);
    assert(hi->wake_tick == 4u);

    tcb_t *lo = sched_get_current_task();
    assert(lo != NULL && strcmp(lo->name, "low_task") == 0);
    assert(lo->state == TASK_STATE_RUNNING);

    /* Verify 'ps' shell command recognizes TASK_STATE_SLEEPING ("SLEEP") */
    assert(shell_execute_line("ps") == SHELL_OK);

    /* Ticks 1..3 inside ISR: high_task remains SLEEPING, low_task keeps CPU */
    for (int t = 1; t <= 3; t++) {
        sched_enter_isr();
        sched_tick();
        assert(hi->state == TASK_STATE_SLEEPING);
        assert(sched_need_resched() == false);
        sched_exit_isr();
        assert(sched_consume_need_resched() == false);
        assert(sched_get_current_task() == lo);
    }

    /* Tick 4 inside ISR: high_task wakes to READY and sets need_resched */
    sched_enter_isr();
    sched_tick();
    assert(sched_get_ticks() == 4u);
    assert(hi->state == TASK_STATE_READY);
    assert(sched_need_resched() == true);
    /* Must NOT switch task inside ISR */
    assert(sched_get_current_task() == lo);
    sched_exit_isr();

    /* ISR tail consumes need_resched and switches back to high_task */
    assert(sched_consume_need_resched() == true);
    sched_yield();
    assert(sched_get_current_task() == hi);
    assert(hi->state == TASK_STATE_RUNNING);

    /* Test task_sleep_ms ceiling rounding at 100 Hz (10 ms/tick) */
    uint64_t base = sched_get_ticks();
    task_sleep_ms(1); /* ceil(1 / 10) = 1 tick */
    assert(hi->state == TASK_STATE_SLEEPING);
    assert(hi->wake_tick == base + 1u);
    sched_tick();
    assert(sched_get_current_task() == hi);

    base = sched_get_ticks();
    task_sleep_ms(25); /* ceil(25 / 10) = 3 ticks */
    assert(hi->state == TASK_STATE_SLEEPING);
    assert(hi->wake_tick == base + 3u);
    sched_tick();
    assert(hi->state == TASK_STATE_SLEEPING);
    sched_tick();
    assert(hi->state == TASK_STATE_SLEEPING);
    sched_tick();
    assert(sched_get_current_task() == hi);

    kprintf("[TEST 1] task_sleep_ticks / task_sleep_ms PASSED.\n");
}

/* ========================================================================= */
/* Test 2: Software Timers (ktimer_t)                                        */
/* ========================================================================= */
static void test_software_timers(void) {
    kprintf("\n[TEST 2] Testing ktimer_t (one-shot, periodic, stop, rearm)...\n");

    sched_init();
    task_create("worker", dummy_task, NULL, 1, s_stack_low, sizeof(s_stack_low));
    sched_yield();

    /* 2a. One-shot timer */
    ktimer_t t_one;
    timer_test_ctx_t ctx_one = { .fires = 0, .active_in_cb = true, .self = &t_one };
    ktimer_init(&t_one, oneshot_cb, &ctx_one, false);
    assert(ktimer_is_active(&t_one) == false);
    assert(ktimer_start_ticks(&t_one, 3) == true);
    assert(ktimer_is_active(&t_one) == true);

    sched_tick(); /* tick 1 */
    sched_tick(); /* tick 2 */
    assert(ctx_one.fires == 0u);
    assert(ktimer_is_active(&t_one) == true);

    sched_tick(); /* tick 3: expires */
    assert(ctx_one.fires == 1u);
    assert(ctx_one.last_tick == 3u);
    /* One-shot timer must already be deactivated BEFORE callback ran */
    assert(ctx_one.active_in_cb == false);
    assert(ktimer_is_active(&t_one) == false);

    /* Further ticks must not fire one-shot again */
    for (int i = 0; i < 5; i++) {
        sched_tick();
    }
    assert(ctx_one.fires == 1u);

    /* 2b. Periodic timer (20 ms -> 2 ticks) */
    ktimer_t t_per;
    timer_test_ctx_t ctx_per = { .fires = 0, .active_in_cb = false, .self = &t_per };
    ktimer_init(&t_per, periodic_cb, &ctx_per, true);
    assert(ktimer_start(&t_per, 20) == true);
    assert(ktimer_is_active(&t_per) == true);

    for (int i = 0; i < 6; i++) {
        sched_tick();
    }
    assert(ctx_per.fires == 3u);
    assert(ctx_per.active_in_cb == true);
    assert(ktimer_is_active(&t_per) == true);
    assert(ktimer_stop(&t_per) == true);
    assert(ktimer_is_active(&t_per) == false);
    assert(ktimer_stop(&t_per) == false); /* Already stopped */

    sched_tick();
    sched_tick();
    assert(ctx_per.fires == 3u);

    /* 2c. Simultaneous timers + callback stopping another timer */
    ktimer_t t_a, t_b;
    timer_test_ctx_t ctx_a = { .fires = 0, .self = &t_a, .other = &t_b };
    timer_test_ctx_t ctx_b = { .fires = 0, .self = &t_b };
    ktimer_init(&t_a, cancel_other_cb, &ctx_a, false);
    ktimer_init(&t_b, oneshot_cb, &ctx_b, false);
    assert(ktimer_start_ticks(&t_a, 2) == true);
    assert(ktimer_start_ticks(&t_b, 2) == true);

    sched_tick();
    sched_tick();
    assert(ctx_a.fires == 1u);
    assert(ctx_b.fires == 0u);
    assert(ktimer_is_active(&t_b) == false);

    /* 2d. One-shot callback restarting itself & periodic callback stopping itself */
    ktimer_t t_rearm, t_self_stop;
    timer_test_ctx_t ctx_rearm = {
        .fires = 0, .active_in_cb = true, .self = &t_rearm, .max_fires = 3, .rearm_ticks = 2
    };
    timer_test_ctx_t ctx_stop = {
        .fires = 0, .self = &t_self_stop, .max_fires = 2
    };
    ktimer_init(&t_rearm, oneshot_rearm_cb, &ctx_rearm, false);
    ktimer_init(&t_self_stop, periodic_self_stop_cb, &ctx_stop, true);
    assert(ktimer_start_ticks(&t_rearm, 2) == true);
    assert(ktimer_start_ticks(&t_self_stop, 3) == true);

    for (int i = 0; i < 8; i++) {
        sched_tick();
    }
    assert(ctx_rearm.fires == 3u);
    assert(ctx_rearm.active_in_cb == false);
    assert(ktimer_is_active(&t_rearm) == false);

    assert(ctx_stop.fires == 2u);
    assert(ktimer_is_active(&t_self_stop) == false);

    ktimer_reset_all();
    kprintf("[TEST 2] Software timers (ktimer_t) PASSED.\n");
}

/* ========================================================================= */
/* Test 3: UART0 RX IRQ, Blocking Wakeup & Ring Buffer Wrap-Around/Overflow  */
/* ========================================================================= */
static void test_uart_rx_irq_and_shell_wakeup(void) {
    kprintf("\n[TEST 3] Testing UART0 RX IRQ, blocking wakeup & ring buffer...\n");

    host_set_current_core_id(RADIX_CORE_0);
    intc_init(RADIX_CORE_0);
    sched_init();
    uart_host_reset();
    shell_init();

    assert(intc_register_handler(INTC_CPU_LINE_UART0, uart_isr, NULL) == true);
    assert(intc_route_irq(RADIX_CORE_0, INT_SRC_UART0, INTC_CPU_LINE_UART0) == true);
    uart_rx_irq_enable();
    assert(intc_enable_irq(INTC_CPU_LINE_UART0) == true);

    /* Create high-priority shell_task (3) and lower-priority worker_task (2) */
    int id_shell = task_create("shell", shell_task, NULL, 3, s_stack_high, sizeof(s_stack_high));
    int id_work  = task_create("worker", dummy_task, NULL, 2, s_stack_med, sizeof(s_stack_med));
    assert(id_shell > 0 && id_work > 0);

    sched_yield();
    tcb_t *shell_tcb = sched_get_current_task();
    assert(shell_tcb != NULL && strcmp(shell_tcb->name, "shell") == 0);

    /* Simulate shell_task calling uart_getc_blocking() on empty RX buffer */
    assert(uart_rx_available() == 0u);
    assert(uart_getc_blocking() == -1);
    assert(shell_tcb->state == TASK_STATE_BLOCKED);

    /* Lower-priority worker_task must now be running (no starvation!) */
    tcb_t *work_tcb = sched_get_current_task();
    assert(work_tcb != NULL && strcmp(work_tcb->name, "worker") == 0);
    assert(work_tcb->state == TASK_STATE_RUNNING);

    /* Inject UART0 RX IRQ while inside ISR context -> wakes shell_task */
    sched_enter_isr();
    uart_host_inject_rx_irq('p');
    assert(uart_rx_available() == 1u);
    assert(shell_tcb->state == TASK_STATE_READY);
    assert(sched_need_resched() == true);
    /* Must NOT switch inside ISR */
    assert(sched_get_current_task() == work_tcb);
    sched_exit_isr();

    /* ISR tail preempts worker_task and resumes shell_task */
    assert(sched_consume_need_resched() == true);
    sched_yield();
    assert(sched_get_current_task() == shell_tcb);
    assert(shell_tcb->state == TASK_STATE_RUNNING);

    /* Consume 'p', then feed 's' and '\n' via INTC dispatch path */
    int c1 = uart_getc_blocking();
    assert(c1 == 'p');
    assert(uart_rx_available() == 0u);

    uart_host_push_hw_fifo('s');
    uart_host_push_hw_fifo('\n');
    sched_enter_isr();
    assert(intc_dispatch_pending((uint32_t)BIT(INTC_CPU_LINE_UART0)) == 1u);
    sched_exit_isr();
    assert(uart_rx_available() == 2u);

    uint32_t cmds_before = shell_get_executed_cmd_count();
    assert(shell_poll_char(c1) == false);
    int c2 = uart_getc_blocking();
    int c3 = uart_getc_blocking();
    assert(c2 == 's' && c3 == '\n');
    assert(shell_poll_char(c2) == false);
    assert(shell_poll_char(c3) == true);
    assert(shell_get_executed_cmd_count() == cmds_before + 1u);
    assert(uart_rx_available() == 0u);

    /* Non-blocking compatibility check */
    assert(uart_getc_nonblocking() == -1);
    uart_host_inject_rx_irq('Z');
    assert(uart_rx_available() == 1u);
    assert(uart_getc_nonblocking() == 'Z');
    assert(uart_getc_nonblocking() == -1);
    assert(uart_rx_available() == 0u);

    /* Ring buffer full + overflow + circular wrap-around check */
    uart_host_reset();
    for (uint32_t i = 0u; i < UART_RX_BUF_SIZE; i++) {
        uart_host_inject_rx_irq((char)('A' + (i % 26u)));
    }
    assert(uart_rx_available() == UART_RX_BUF_SIZE);
    assert(uart_host_get_rx_overflows() == 0u);

    /* Next byte overflows and is counted without corrupting existing bytes */
    uart_host_inject_rx_irq('!');
    assert(uart_rx_available() == UART_RX_BUF_SIZE);
    assert(uart_host_get_rx_overflows() == 1u);

    /* Drain half (64 bytes) */
    for (uint32_t i = 0u; i < 64u; i++) {
        int ch = uart_getc_nonblocking();
        assert(ch == (int)('A' + (i % 26u)));
    }
    assert(uart_rx_available() == 64u);

    /* Push 64 bytes wrapping around the 128-byte buffer end */
    for (uint32_t i = 0u; i < 64u; i++) {
        uart_host_inject_rx_irq((char)('0' + (i % 10u)));
    }
    assert(uart_rx_available() == UART_RX_BUF_SIZE);

    /* Verify remaining original 64 bytes followed by wrapped 64 bytes */
    for (uint32_t i = 64u; i < UART_RX_BUF_SIZE; i++) {
        int ch = uart_getc_blocking();
        assert(ch == (int)('A' + (i % 26u)));
    }
    for (uint32_t i = 0u; i < 64u; i++) {
        int ch = uart_getc_blocking();
        assert(ch == (int)('0' + (i % 10u)));
    }
    assert(uart_rx_available() == 0u);

    kprintf("[TEST 3] UART0 RX IRQ, blocking wakeup & ring buffer PASSED.\n");
}

int main(void) {
    kprintf("=====================================================\n");
    kprintf("  RadixOS Timers, Timed Sleep & UART RX IRQ Suite\n");
    kprintf("=====================================================\n");

    world_init();
    pms_init();
    amp_init();
    ulp_init();

    test_task_sleep_and_wakeup();
    test_software_timers();
    test_uart_rx_irq_and_shell_wakeup();

    kprintf("\n=====================================================\n");
    kprintf(" [SUCCESS] All Sleep, ktimer_t & UART RX IRQ Tests Passed!\n");
    kprintf("=====================================================\n");
    return 0;
}
