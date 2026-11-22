/*
 * tests/test_intc_host.c - Host Simulation & Unit Test Suite for Interrupt Matrix & ISR Preemption
 *
 * Verifies:
 *   1. ESP32-S3 Interrupt Matrix initialization & IRQ routing (Core 0/1 + bounds)
 *   2. INTENABLE masking (enable/disable/get_enabled_mask) per core
 *   3. Handler registration & intc_dispatch_pending (single, multi, partial, unregistered)
 *   4. ISR nesting depth (sched_enter_isr / sched_exit_isr) & deferred preemption (need_resched)
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <radix/types.h>
#include <radix/kprintf.h>
#include <radix/intc.h>
#include <radix/amp.h>
#include <radix/sched.h>

#define TEST_STACK_SIZE 2048

/* Mock Core 1 entry trampoline for host test linking with kernel/amp.c */
void _start_core1(void) {
}

static uint8_t s_stack_a[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_stack_b[TEST_STACK_SIZE] __attribute__((aligned(16)));

static void dummy_task(void *arg) {
    (void)arg;
}

typedef struct {
    uint32_t count;
    uint32_t last_line;
    void *expected_arg;
} test_irq_ctx_t;

static void test_irq_callback(uint32_t cpu_int_num, void *arg) {
    test_irq_ctx_t *ctx = (test_irq_ctx_t *)arg;
    assert(ctx != NULL);
    assert(arg == ctx->expected_arg);
    ctx->count++;
    ctx->last_line = cpu_int_num;
}

static void tick_irq_callback(uint32_t cpu_int_num, void *arg) {
    (void)cpu_int_num;
    uint32_t *irq_count = (uint32_t *)arg;
    if (irq_count) {
        (*irq_count)++;
    }
    sched_tick();
}

/* ========================================================================= */
/* Test 1: Interrupt Matrix Initialization & Routing Bounds                  */
/* ========================================================================= */
static void test_intc_init_and_routing(void) {
    kprintf("[TEST 1] Testing Interrupt Matrix init & IRQ routing bounds...\n");

    host_set_current_core_id(RADIX_CORE_0);
    intc_init(RADIX_CORE_0);
    intc_init(RADIX_CORE_1);

    /* All sources must start disconnected (16) */
    for (uint32_t src = 0; src < INTC_NUM_SOURCES; src++) {
        assert(intc_host_get_mapped_irq(RADIX_CORE_0, src) == INTC_DISCONNECTED_LINE);
        assert(intc_host_get_mapped_irq(RADIX_CORE_1, src) == INTC_DISCONNECTED_LINE);
    }

    /* Valid Core 0 and Core 1 routes */
    assert(intc_route_irq(RADIX_CORE_0, INT_SRC_SYSTIMER_TARGET0, INTC_CPU_LINE_SYSTIMER) == true);
    assert(intc_route_irq(RADIX_CORE_0, INT_SRC_CPU_INTR_FROM_CPU_0, INTC_CPU_LINE_IPI) == true);
    assert(intc_route_irq(RADIX_CORE_0, INT_SRC_UART0, 18u) == true);
    assert(intc_route_irq(RADIX_CORE_1, INT_SRC_CPU_INTR_FROM_CPU_1, 3u) == true);

    assert(intc_host_get_mapped_irq(RADIX_CORE_0, INT_SRC_SYSTIMER_TARGET0) == INTC_CPU_LINE_SYSTIMER);
    assert(intc_host_get_mapped_irq(RADIX_CORE_0, INT_SRC_CPU_INTR_FROM_CPU_0) == INTC_CPU_LINE_IPI);
    assert(intc_host_get_mapped_irq(RADIX_CORE_0, INT_SRC_UART0) == 18u);
    assert(intc_host_get_mapped_irq(RADIX_CORE_1, INT_SRC_CPU_INTR_FROM_CPU_1) == 3u);
    /* Core 0 route must not leak into Core 1 map */
    assert(intc_host_get_mapped_irq(RADIX_CORE_1, INT_SRC_SYSTIMER_TARGET0) == INTC_DISCONNECTED_LINE);

    /* Invalid core IDs */
    assert(intc_route_irq(2u, INT_SRC_UART0, 1u) == false);
    assert(intc_route_irq(99u, INT_SRC_UART0, 1u) == false);

    /* Invalid peripheral source IDs */
    assert(intc_route_irq(RADIX_CORE_0, INTC_NUM_SOURCES, 1u) == false);
    assert(intc_route_irq(RADIX_CORE_0, 255u, 1u) == false);

    /* Invalid / non-Level-1-external CPU interrupt lines */
    assert(intc_route_irq(RADIX_CORE_0, INT_SRC_UART0, 6u) == false);   /* Internal Timer0 */
    assert(intc_route_irq(RADIX_CORE_0, INT_SRC_UART0, 7u) == false);   /* Internal SW L1 */
    assert(intc_route_irq(RADIX_CORE_0, INT_SRC_UART0, 14u) == false);  /* NMI */
    assert(intc_route_irq(RADIX_CORE_0, INT_SRC_UART0, 16u) == false);  /* Internal Timer2 */
    assert(intc_route_irq(RADIX_CORE_0, INT_SRC_UART0, 19u) == false);  /* Level 2 */
    assert(intc_route_irq(RADIX_CORE_0, INT_SRC_UART0, 24u) == false);  /* Level 4 */
    assert(intc_route_irq(RADIX_CORE_0, INT_SRC_UART0, 32u) == false);  /* Out of range */

    kprintf("[TEST 1] Interrupt Matrix init & routing PASSED.\n");
}

/* ========================================================================= */
/* Test 2: Enable / Disable IRQ Masking                                      */
/* ========================================================================= */
static void test_intc_enable_disable_mask(void) {
    kprintf("[TEST 2] Testing IRQ enable/disable & INTENABLE masks...\n");

    host_set_current_core_id(RADIX_CORE_0);
    intc_init(RADIX_CORE_0);
    intc_init(RADIX_CORE_1);

    assert(intc_get_enabled_mask() == 0u);

    assert(intc_enable_irq(1u) == true);
    assert(intc_enable_irq(2u) == true);
    assert(intc_enable_irq(18u) == true);
    assert(intc_get_enabled_mask() == (BIT(1) | BIT(2) | BIT(18)));

    /* Invalid lines rejected without altering mask */
    assert(intc_enable_irq(6u) == false);
    assert(intc_enable_irq(16u) == false);
    assert(intc_enable_irq(32u) == false);
    assert(intc_disable_irq(19u) == false);
    assert(intc_get_enabled_mask() == (BIT(1) | BIT(2) | BIT(18)));

    /* Disable single line */
    assert(intc_disable_irq(2u) == true);
    assert(intc_get_enabled_mask() == (BIT(1) | BIT(18)));

    /* Core 1 has independent INTENABLE */
    host_set_current_core_id(RADIX_CORE_1);
    assert(intc_get_enabled_mask() == 0u);
    assert(intc_enable_irq(3u) == true);
    assert(intc_get_enabled_mask() == BIT(3));

    host_set_current_core_id(RADIX_CORE_0);
    assert(intc_get_enabled_mask() == (BIT(1) | BIT(18)));

    kprintf("[TEST 2] IRQ enable/disable & INTENABLE masks PASSED.\n");
}

/* ========================================================================= */
/* Test 3: Handler Registration & Pending Mask Dispatch                      */
/* ========================================================================= */
static void test_intc_register_and_dispatch(void) {
    kprintf("[TEST 3] Testing handler registration & intc_dispatch_pending...\n");

    host_set_current_core_id(RADIX_CORE_0);
    intc_init(RADIX_CORE_0);

    test_irq_ctx_t ctx1 = { 0, 0, NULL };
    test_irq_ctx_t ctx2 = { 0, 0, NULL };
    test_irq_ctx_t ctx8 = { 0, 0, NULL };
    ctx1.expected_arg = &ctx1;
    ctx2.expected_arg = &ctx2;
    ctx8.expected_arg = &ctx8;

    assert(intc_register_handler(1u, test_irq_callback, &ctx1) == true);
    assert(intc_register_handler(2u, test_irq_callback, &ctx2) == true);
    assert(intc_register_handler(8u, test_irq_callback, &ctx8) == true);
    assert(intc_register_handler(16u, test_irq_callback, &ctx1) == false);
    assert(intc_register_handler(32u, test_irq_callback, &ctx1) == false);

    /* Zero pending mask dispatches nothing */
    assert(intc_dispatch_pending(0u) == 0u);

    /* Partial mask: only line 1 runs */
    assert(intc_dispatch_pending((uint32_t)BIT(1)) == 1u);
    assert(ctx1.count == 1u && ctx1.last_line == 1u);
    assert(ctx2.count == 0u && ctx8.count == 0u);

    /* Multiple lines + unregistered line bit (line 10 has no handler) */
    uint32_t dispatched = intc_dispatch_pending((uint32_t)(BIT(1) | BIT(8) | BIT(10)));
    assert(dispatched == 2u);
    assert(ctx1.count == 2u && ctx1.last_line == 1u);
    assert(ctx8.count == 1u && ctx8.last_line == 8u);
    assert(ctx2.count == 0u);

    /* Unregister line 8 and verify it is no longer called */
    assert(intc_register_handler(8u, NULL, NULL) == true);
    assert(intc_dispatch_pending((uint32_t)(BIT(2) | BIT(8))) == 1u);
    assert(ctx2.count == 1u && ctx2.last_line == 2u);
    assert(ctx8.count == 1u);

    kprintf("[TEST 3] Handler registration & dispatch PASSED.\n");
}

/* ========================================================================= */
/* Test 4: ISR Nesting & Deferred Preemption (need_resched)                  */
/* ========================================================================= */
static void test_isr_nesting_and_deferred_preemption(void) {
    kprintf("[TEST 4] Testing ISR nesting & deferred preemption via need_resched...\n");

    host_set_current_core_id(RADIX_CORE_0);
    intc_init(RADIX_CORE_0);
    sched_init();

    int id_a = task_create("task_A", dummy_task, NULL, 2, s_stack_a, sizeof(s_stack_a));
    int id_b = task_create("task_B", dummy_task, NULL, 2, s_stack_b, sizeof(s_stack_b));
    assert(id_a > 0 && id_b > 0);

    /* Dispatch task_A */
    sched_yield();
    tcb_t *curr = sched_get_current_task();
    assert(curr != NULL && strcmp(curr->name, "task_A") == 0);
    assert(sched_in_isr() == false);
    assert(sched_need_resched() == false);
    assert(sched_consume_need_resched() == false);

    /* Enter nested ISR (depth = 2) and exhaust task_A's time slice */
    sched_enter_isr();
    assert(sched_in_isr() == true);
    sched_enter_isr();
    assert(sched_in_isr() == true);

    for (int i = 0; i < DEFAULT_TIME_SLICE_TICKS; i++) {
        sched_tick();
    }

    /* Preemption MUST be deferred while inside ISR */
    assert(strcmp(sched_get_current_task()->name, "task_A") == 0);
    assert(sched_need_resched() == true);

    /* Still at depth 1 after first exit: consume must refuse */
    sched_exit_isr();
    assert(sched_in_isr() == true);
    assert(sched_consume_need_resched() == false);
    assert(sched_need_resched() == true);

    /* Exit outermost ISR (depth = 0): consume succeeds and clears flag */
    sched_exit_isr();
    assert(sched_in_isr() == false);
    assert(sched_consume_need_resched() == true);
    assert(sched_need_resched() == false);
    assert(sched_consume_need_resched() == false);

    /* Simulate ISR exit tail call to sched_yield() */
    sched_yield();
    assert(strcmp(sched_get_current_task()->name, "task_B") == 0);

    /* Full INTC + deferred preemption round-trip back to task_A */
    uint32_t tick_irqs = 0;
    assert(intc_register_handler(INTC_CPU_LINE_SYSTIMER, tick_irq_callback, &tick_irqs) == true);
    assert(intc_enable_irq(INTC_CPU_LINE_SYSTIMER) == true);

    for (int i = 0; i < DEFAULT_TIME_SLICE_TICKS - 1; i++) {
        sched_enter_isr();
        intc_dispatch_pending((uint32_t)BIT(INTC_CPU_LINE_SYSTIMER));
        sched_exit_isr();
        assert(sched_consume_need_resched() == false);
        assert(strcmp(sched_get_current_task()->name, "task_B") == 0);
    }

    /* Final tick of slice inside simulated ISR */
    sched_enter_isr();
    intc_dispatch_pending((uint32_t)BIT(INTC_CPU_LINE_SYSTIMER));
    assert(strcmp(sched_get_current_task()->name, "task_B") == 0);
    assert(sched_need_resched() == true);
    sched_exit_isr();

    assert(tick_irqs == DEFAULT_TIME_SLICE_TICKS);
    assert(sched_consume_need_resched() == true);
    sched_yield();
    assert(strcmp(sched_get_current_task()->name, "task_A") == 0);

    kprintf("[TEST 4] ISR nesting & deferred preemption PASSED.\n");
}

int main(void) {
    kprintf("=====================================================\n");
    kprintf("  RadixOS INTC & Deferred Preemption Test Suite\n");
    kprintf("=====================================================\n");

    test_intc_init_and_routing();
    test_intc_enable_disable_mask();
    test_intc_register_and_dispatch();
    test_isr_nesting_and_deferred_preemption();

    kprintf("\n=====================================================\n");
    kprintf(" [SUCCESS] All Interrupt Matrix & ISR Tests Passed!\n");
    kprintf("=====================================================\n");
    return 0;
}
