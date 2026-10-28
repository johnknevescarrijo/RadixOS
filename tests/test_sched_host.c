#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <radix/types.h>
#include <radix/kprintf.h>
#include <radix/task.h>
#include <radix/sched.h>

#define TEST_STACK_SIZE 2048

static uint8_t s_stack_worker1[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_stack_worker2[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_stack_worker3[TEST_STACK_SIZE] __attribute__((aligned(16)));

static int s_worker1_run_count = 0;
static int s_worker2_run_count = 0;

static void worker1_entry(void *arg) {
    (void)arg;
    s_worker1_run_count++;
}

static void worker2_entry(void *arg) {
    (void)arg;
    s_worker2_run_count++;
}

static void test_kprintf_formatting(void) {
    kprintf("[TEST] Testing kprintf formatting capabilities...\n");
    kprintf("  - String: %s\n", "RadixOS Nano-Kernel");
    kprintf("  - Signed Decimal: %d, Negative: %d\n", 42, -99);
    kprintf("  - Unsigned Decimal: %u\n", 1234567);
    kprintf("  - Hexadecimal: 0x%x, Uppercase: 0x%X\n", 0xcafe, 0xbeef);
    kprintf("  - Pointer: %p\n", (void *)0x40378000);
    kprintf("  - Char: %c, Percent: %%\n", 'X');
    kprintf("[TEST] kprintf formatting test PASSED.\n");
}

static void test_scheduler_lifecycle(void) {
    kprintf("\n[TEST] Testing scheduler initialization & task creation...\n");

    sched_init();

    tcb_t *curr = sched_get_current_task();
    assert(curr == NULL); /* No task running before sched starts */

    int id1 = task_create("worker_alpha", worker1_entry, NULL, 2,
                          s_stack_worker1, sizeof(s_stack_worker1));
    assert(id1 > 0);

    int id2 = task_create("worker_beta", worker2_entry, NULL, 2,
                          s_stack_worker2, sizeof(s_stack_worker2));
    assert(id2 > 0);
    assert(id2 != id1);

    int id3 = task_create("low_prio", worker1_entry, NULL, 1,
                          s_stack_worker3, sizeof(s_stack_worker3));
    assert(id3 > 0);

    /* Test invalid task parameters */
    assert(task_create(NULL, worker1_entry, NULL, 1, s_stack_worker1, 1024) == -1);
    assert(task_create("bad_stack", worker1_entry, NULL, 1, s_stack_worker1, 16) == -1);

    kprintf("[TEST] Task creation and bounds checks PASSED.\n");
}

static void test_priority_and_round_robin(void) {
    kprintf("\n[TEST] Testing priority selection and round-robin scheduling...\n");

    /* Initial yield will select the first ready task with highest priority (id1) */
    sched_yield();
    tcb_t *curr = sched_get_current_task();
    assert(curr != NULL);
    assert(strcmp(curr->name, "worker_alpha") == 0);
    assert(curr->priority == 2);
    assert(curr->state == TASK_STATE_RUNNING);

    /* Next yield should switch to worker_beta (sharing priority 2) */
    sched_yield();
    curr = sched_get_current_task();
    assert(curr != NULL);
    assert(strcmp(curr->name, "worker_beta") == 0);
    assert(curr->priority == 2);

    /* Next yield should cycle back to worker_alpha (Round-Robin) */
    sched_yield();
    curr = sched_get_current_task();
    assert(curr != NULL);
    assert(strcmp(curr->name, "worker_alpha") == 0);

    kprintf("[TEST] Priority and Round-Robin scheduling PASSED.\n");
}

static void test_preemptive_tick_time_slice(void) {
    kprintf("\n[TEST] Testing pre-emption via periodic timer ticks...\n");

    tcb_t *curr = sched_get_current_task();
    assert(curr != NULL);
    assert(strcmp(curr->name, "worker_alpha") == 0);

    /* Simulate timer ticks up to DEFAULT_TIME_SLICE_TICKS */
    for (int i = 0; i < DEFAULT_TIME_SLICE_TICKS - 1; i++) {
        sched_tick();
        curr = sched_get_current_task();
        assert(strcmp(curr->name, "worker_alpha") == 0); /* Still within time slice */
    }

    /* Next tick must exhaust slice and preemptively switch to worker_beta */
    sched_tick();
    curr = sched_get_current_task();
    assert(strcmp(curr->name, "worker_beta") == 0);

    kprintf("[TEST] Preemptive timer tick time-slicing PASSED.\n");
}

int main(void) {
    kprintf("=====================================================\n");
    kprintf("  RadixOS Host Simulation & Scheduler Test Suite\n");
    kprintf("=====================================================\n");

    test_kprintf_formatting();
    test_scheduler_lifecycle();
    test_priority_and_round_robin();
    test_preemptive_tick_time_slice();

    kprintf("\n=====================================================\n");
    kprintf(" [SUCCESS] All Step 1 & Step 2 unit tests passed!\n");
    kprintf("=====================================================\n");

    return 0;
}
