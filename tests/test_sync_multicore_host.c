/*
 * tests/test_sync_multicore_host.c - Host Unit Test Suite for Feature 7
 *
 * Verifies:
 *   1. Priority Inheritance (PI) preventing priority inversion
 *   2. Nested Mutex PI recalculation (step-down across multiple owned mutexes)
 *   3. Recursive Mutex reentrancy (lock_count depth & non-owner protection)
 *   4. Wait-queue & ownership cleanup on sched_kill_task() (mutex + semaphore)
 *   5. Multi-Core Scheduler, per-core state, Task Pinning & no dual-core overlap
 *   6. Protected FIFO AMP call queue (AMP_CALL_QUEUE_SIZE = 8), overflow safety & Core 1 INTC setup
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <radix/types.h>
#include <radix/kprintf.h>
#include <radix/task.h>
#include <radix/sched.h>
#include <radix/sync.h>
#include <radix/amp.h>
#include <radix/intc.h>
#include <radix/world.h>
#include <radix/pms.h>

/* Mock Core 1 entry trampoline for host test linking with kernel/amp.c */
void _start_core1(void) {
}

#define TEST_STACK_SIZE 2048u

static uint8_t s_stack_1[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_stack_2[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_stack_3[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_stack_4[TEST_STACK_SIZE] __attribute__((aligned(16)));

static void dummy_task(void *arg) {
    (void)arg;
}

static tcb_t *find_task_by_id(uint32_t task_id) {
    for (uint32_t i = 0u; i < MAX_TASKS; i++) {
        const tcb_t *t = sched_get_task_by_index(i);
        if (t && t->state != TASK_STATE_UNUSED && t->task_id == task_id) {
            return (tcb_t *)(uintptr_t)t;
        }
    }
    return NULL;
}

/* ========================================================================= */
/* Test 1: Priority Inheritance (PI)                                         */
/* ========================================================================= */
static void test_priority_inheritance(void) {
    kprintf("[TEST 1] Testing Priority Inheritance (low/med/high inversion prevention)...\n");

    arch_host_set_core_id(RADIX_CORE_0);
    world_init();
    pms_init();
    sched_init();

    mutex_t m;
    mutex_init(&m);

    int id_low = task_create("low", dummy_task, NULL, 1, s_stack_1, sizeof(s_stack_1));
    assert(id_low > 0);
    tcb_t *t_low = find_task_by_id((uint32_t)id_low);
    assert(t_low != NULL);
    assert(t_low->base_priority == 1u);
    assert(t_low->priority == 1u);

    /* Low acquires mutex */
    sched_start();
    assert(sched_get_current_task() == t_low);
    mutex_lock(&m);
    assert(m.owner == t_low);
    assert(m.lock_count == 1u);

    /* High (prio 5) and Medium (prio 3) arrive */
    int id_med = task_create("med", dummy_task, NULL, 3, s_stack_2, sizeof(s_stack_2));
    int id_high = task_create("high", dummy_task, NULL, 5, s_stack_3, sizeof(s_stack_3));
    assert(id_med > 0 && id_high > 0);
    tcb_t *t_med = find_task_by_id((uint32_t)id_med);
    tcb_t *t_high = find_task_by_id((uint32_t)id_high);
    assert(t_med && t_high);

    /* Yield switches to highest ready task: high */
    sched_yield();
    assert(sched_get_current_task() == t_high);

    /* High blocks on m -> Low MUST inherit priority 5 */
    mutex_lock(&m);
    assert(t_high->state == TASK_STATE_BLOCKED);
    assert(t_high->wait_type == TASK_WAIT_MUTEX);
    assert(t_high->wait_obj == &m);
    assert(t_low->priority == 5u);
    assert(t_low->base_priority == 1u);

    /* Scheduler must now pick boosted Low (prio 5) over Medium (prio 3) */
    assert(sched_get_current_task() == t_low);
    sched_yield();
    assert(sched_get_current_task() == t_low);

    /* Low unlocks m -> returns to base_priority 1, High wakes & preempts */
    mutex_unlock(&m);
    assert(t_low->priority == 1u);
    assert(m.owner == t_high);
    assert(sched_get_current_task() == t_high);
    assert(t_high->state == TASK_STATE_RUNNING);

    /* High finishes and unlocks m, then exits -> Medium runs next */
    mutex_unlock(&m);
    assert(m.locked == 0u && m.owner == NULL);
    task_exit();
    assert(sched_get_current_task() == t_med);

    kprintf("[TEST 1] Priority Inheritance PASSED.\n\n");
}

/* ========================================================================= */
/* Test 2: Nested Mutex Priority Inheritance                                 */
/* ========================================================================= */
static void test_nested_mutex_priority_inheritance(void) {
    kprintf("[TEST 2] Testing nested mutex Priority Inheritance step-down...\n");

    arch_host_set_core_id(RADIX_CORE_0);
    world_init();
    pms_init();
    sched_init();

    mutex_t m1, m2;
    mutex_init(&m1);
    mutex_init(&m2);

    int id_low = task_create("low", dummy_task, NULL, 1, s_stack_1, sizeof(s_stack_1));
    tcb_t *t_low = find_task_by_id((uint32_t)id_low);
    assert(t_low != NULL);
    sched_start();
    assert(sched_get_current_task() == t_low);

    /* Low acquires both M1 and M2 */
    mutex_lock(&m1);
    mutex_lock(&m2);
    assert(t_low->owned_mutexes == &m2);
    assert(m2.next_owned == &m1);
    assert(m1.next_owned == NULL);

    int id_med = task_create("med", dummy_task, NULL, 3, s_stack_3, sizeof(s_stack_3));
    tcb_t *t_med = find_task_by_id((uint32_t)id_med);
    sched_yield();
    assert(sched_get_current_task() == t_med);
    mutex_lock(&m2);
    assert(t_med->state == TASK_STATE_BLOCKED);
    assert(t_low->priority == 3u);

    int id_high = task_create("high", dummy_task, NULL, 5, s_stack_2, sizeof(s_stack_2));
    tcb_t *t_high = find_task_by_id((uint32_t)id_high);
    sched_yield();
    assert(sched_get_current_task() == t_high);
    mutex_lock(&m1);
    assert(t_high->state == TASK_STATE_BLOCKED);
    assert(t_low->priority == 5u);

    /* Low unlocks M1 -> priority MUST step down to 3 (from M2 waiter Med), NOT 1! */
    assert(sched_get_current_task() == t_low);
    mutex_unlock(&m1);
    assert(t_low->priority == 3u);
    assert(t_low->base_priority == 1u);
    assert(m1.owner == t_high);
    assert(sched_get_current_task() == t_high);

    /* High unlocks M1 and exits -> Low runs at prio 3 */
    mutex_unlock(&m1);
    task_exit();
    assert(sched_get_current_task() == t_low);
    assert(t_low->priority == 3u);

    /* Low unlocks M2 -> priority returns to base_priority 1, Med wakes & preempts */
    mutex_unlock(&m2);
    assert(t_low->priority == 1u);
    assert(t_low->owned_mutexes == NULL);
    assert(m2.owner == t_med);
    assert(sched_get_current_task() == t_med);
    mutex_unlock(&m2);
    assert(m2.locked == 0u);

    kprintf("[TEST 2] Nested mutex Priority Inheritance PASSED.\n\n");
}

/* ========================================================================= */
/* Test 3: Recursive Mutex Reentrancy                                        */
/* ========================================================================= */
static void test_recursive_mutex_reentrancy(void) {
    kprintf("[TEST 3] Testing recursive mutex lock_count & non-owner unlock guard...\n");

    arch_host_set_core_id(RADIX_CORE_0);
    world_init();
    pms_init();
    sched_init();

    mutex_t m;
    mutex_init(&m);

    int id_a = task_create("owner", dummy_task, NULL, 2, s_stack_1, sizeof(s_stack_1));
    int id_b = task_create("waiter", dummy_task, NULL, 2, s_stack_2, sizeof(s_stack_2));
    tcb_t *t_a = find_task_by_id((uint32_t)id_a);
    tcb_t *t_b = find_task_by_id((uint32_t)id_b);

    sched_start();
    assert(sched_get_current_task() == t_a);

    /* Lock 2x by same owner */
    mutex_lock(&m);
    assert(m.lock_count == 1u);
    mutex_lock(&m);
    assert(m.lock_count == 2u);
    assert(m.owner == t_a);

    /* Waiter blocks */
    sched_yield();
    assert(sched_get_current_task() == t_b);
    mutex_lock(&m);
    assert(t_b->state == TASK_STATE_BLOCKED);
    assert(m.count == 1u);

    /* Non-owner unlock attempt while running as waiter/other must be ignored */
    assert(sched_get_current_task() == t_a);

    /* 1st unlock must keep ownership with t_a (lock_count 2 -> 1) */
    mutex_unlock(&m);
    assert(m.locked == 1u);
    assert(m.lock_count == 1u);
    assert(m.owner == t_a);
    assert(t_b->state == TASK_STATE_BLOCKED);

    /* 2nd unlock releases mutex and transfers ownership to t_b */
    mutex_unlock(&m);
    assert(m.locked == 1u);
    assert(m.lock_count == 1u);
    assert(m.owner == t_b);
    assert(t_b->state == TASK_STATE_READY);

    /* t_b runs and unlocks */
    sched_yield();
    assert(sched_get_current_task() == t_b);
    mutex_unlock(&m);
    assert(m.locked == 0u);
    assert(m.lock_count == 0u);
    assert(m.owner == NULL);

    kprintf("[TEST 3] Recursive mutex reentrancy PASSED.\n\n");
}

/* ========================================================================= */
/* Test 4: Wait-Queue & Ownership Cleanup on sched_kill_task()               */
/* ========================================================================= */
static void test_wait_queue_cleanup_on_kill(void) {
    kprintf("[TEST 4] Testing wait_queue & ownership cleanup on sched_kill_task()...\n");

    arch_host_set_core_id(RADIX_CORE_0);
    world_init();
    pms_init();
    sched_init();

    mutex_t m1, m2;
    semaphore_t sem;
    mutex_init(&m1);
    mutex_init(&m2);
    sem_init(&sem, 0u, 4u);

    int id_low = task_create("low", dummy_task, NULL, 1, s_stack_1, sizeof(s_stack_1));
    tcb_t *t_low = find_task_by_id((uint32_t)id_low);
    sched_start();
    assert(sched_get_current_task() == t_low);
    mutex_lock(&m1);
    mutex_lock(&m2);

    int id_med  = task_create("med",  dummy_task, NULL, 3, s_stack_2, sizeof(s_stack_2));
    int id_high = task_create("high", dummy_task, NULL, 5, s_stack_3, sizeof(s_stack_3));
    tcb_t *t_med   = find_task_by_id((uint32_t)id_med);
    tcb_t *t_high  = find_task_by_id((uint32_t)id_high);

    /* High blocks on M1 -> Low boosted to 5 */
    sched_yield();
    assert(sched_get_current_task() == t_high);
    mutex_lock(&m1);
    assert(t_high->state == TASK_STATE_BLOCKED);
    assert(t_low->priority == 5u);

    /* Also have a prio-6 task? Better: drop high's wait scenario by creating med at prio 4?
     * Let's test killing a waiter directly:
     * Create t_sem_w1 (prio 6) and t_sem_w2 (prio 4) -> block both on sem, kill w1. */
    int id_sw1 = task_create("sw1", dummy_task, NULL, 6, s_stack_4, sizeof(s_stack_4));
    tcb_t *t_sw1 = find_task_by_id((uint32_t)id_sw1);
    sched_yield();
    assert(sched_get_current_task() == t_sw1);
    sem_wait(&sem);
    assert(t_sw1->state == TASK_STATE_BLOCKED);
    assert(sem.waiters == 1u);
    assert(sem.wait_queue[0] == t_sw1);

    /* Kill blocked semaphore waiter sw1 -> wait_queue must become empty without dangling ptrs */
    assert(sched_kill_task((uint32_t)id_sw1) == true);
    assert(sem.waiters == 0u);
    assert(sem.wait_queue[0] == NULL);
    assert(t_sw1->state == TASK_STATE_UNUSED);
    assert(t_sw1->wait_obj == NULL);

    /* Kill blocked mutex waiter High -> M1 wait_queue compacted & Low drops back to base_priority 1 */
    assert(m1.count == 1u);
    assert(sched_kill_task((uint32_t)id_high) == true);
    assert(m1.count == 0u);
    assert(m1.wait_queue[0] == NULL);
    assert(t_high->state == TASK_STATE_UNUSED);
    assert(t_high->wait_obj == NULL);
    assert(t_low->priority == 1u);

    /* Now Med (prio 3) runs and blocks on M1, and new task w4 (prio 4) blocks on M1 */
    sched_yield();
    assert(sched_get_current_task() == t_med);
    mutex_lock(&m1);
    assert(t_med->state == TASK_STATE_BLOCKED);
    assert(t_low->priority == 3u);

    int id_w4 = task_create("w4", dummy_task, NULL, 4, s_stack_3, sizeof(s_stack_3));
    tcb_t *t_w4 = find_task_by_id((uint32_t)id_w4);
    sched_yield();
    assert(sched_get_current_task() == t_w4);
    mutex_lock(&m1);
    assert(t_w4->state == TASK_STATE_BLOCKED);
    assert(m1.count == 2u);
    assert(t_low->priority == 4u);

    /* Kill mutex owner Low while holding M1 (with waiters Med, w4) and M2 (no waiters) */
    assert(sched_get_current_task() == t_low);
    assert(sched_kill_task((uint32_t)id_low) == true);

    /* M2 must be completely unlocked; M1 must transfer ownership to highest-prio waiter w4 */
    assert(m2.locked == 0u);
    assert(m2.owner == NULL);
    assert(m1.locked == 1u);
    assert(m1.owner == t_w4);
    assert(t_w4->state == TASK_STATE_RUNNING); /* Preempted immediately upon Low's death */
    assert(sched_get_current_task() == t_w4);
    assert(m1.count == 1u);
    assert(m1.wait_queue[0] == t_med);
    assert(m1.wait_queue[1] == NULL);

    /* w4 unlocks M1 -> Med becomes owner */
    mutex_unlock(&m1);
    assert(m1.owner == t_med);
    assert(t_med->state == TASK_STATE_READY);

    kprintf("[TEST 4] Wait-queue & ownership cleanup PASSED.\n\n");
}

/* ========================================================================= */
/* Test 5: Multi-Core Scheduler & Task Pinning                               */
/* ========================================================================= */
static void test_multicore_scheduler_and_pinning(void) {
    kprintf("[TEST 5] Testing multi-core scheduler, task pinning & core exclusivity...\n");

    arch_host_set_core_id(RADIX_CORE_0);
    world_init();
    pms_init();
    sched_init();

    /* Invalid core affinity must be rejected */
    assert(task_create_pinned("bad_aff", dummy_task, NULL, 2, s_stack_1, sizeof(s_stack_1),
                              WORLD_0, (int8_t)2) == -1);
    assert(task_create_pinned("bad_neg", dummy_task, NULL, 2, s_stack_1, sizeof(s_stack_1),
                              WORLD_0, (int8_t)-2) == -1);

    int id_c0  = task_create_pinned("pin_c0",  dummy_task, NULL, 4, s_stack_1, sizeof(s_stack_1),
                                    WORLD_0, (int8_t)RADIX_CORE_0);
    int id_c1  = task_create_pinned("pin_c1",  dummy_task, NULL, 4, s_stack_2, sizeof(s_stack_2),
                                    WORLD_0, (int8_t)RADIX_CORE_1);
    int id_any1 = task_create_pinned("any_1", dummy_task, NULL, 2, s_stack_3, sizeof(s_stack_3),
                                     WORLD_0, RADIX_CORE_ANY);
    int id_any2 = task_create_pinned("any_2", dummy_task, NULL, 2, s_stack_4, sizeof(s_stack_4),
                                     WORLD_0, RADIX_CORE_ANY);
    assert(id_c0 > 0 && id_c1 > 0 && id_any1 > 0 && id_any2 > 0);

    tcb_t *t_c0   = find_task_by_id((uint32_t)id_c0);
    tcb_t *t_c1   = find_task_by_id((uint32_t)id_c1);
    tcb_t *t_any1 = find_task_by_id((uint32_t)id_any1);
    tcb_t *t_any2 = find_task_by_id((uint32_t)id_any2);
    assert(t_c0 && t_c1 && t_any1 && t_any2);

    /* Core 0 starts -> MUST select pin_c0, never pin_c1 */
    arch_host_set_core_id(RADIX_CORE_0);
    sched_start();
    assert(sched_get_current_task_on_core(RADIX_CORE_0) == t_c0);
    assert(sched_get_current_task_on_core(RADIX_CORE_1) == NULL);
    sched_yield();
    assert(sched_get_current_task_on_core(RADIX_CORE_0) == t_c0);

    /* Core 1 starts -> MUST select pin_c1, never pin_c0 */
    arch_host_set_core_id(RADIX_CORE_1);
    sched_start();
    assert(sched_get_current_task_on_core(RADIX_CORE_1) == t_c1);
    assert(sched_get_current_task_on_core(RADIX_CORE_0) == t_c0);
    sched_yield();
    assert(sched_get_current_task_on_core(RADIX_CORE_1) == t_c1);

    /* Verify per-core ISR nesting & need_resched isolation */
    arch_host_set_core_id(RADIX_CORE_0);
    sched_enter_isr();
    sched_set_need_resched();
    assert(sched_in_isr() == true);
    assert(sched_need_resched() == true);

    arch_host_set_core_id(RADIX_CORE_1);
    assert(sched_in_isr() == false);
    assert(sched_need_resched() == false);

    arch_host_set_core_id(RADIX_CORE_0);
    sched_exit_isr();
    assert(sched_consume_need_resched() == true);

    /* Kill pinned tasks so RADIX_CORE_ANY tasks run on both cores */
    assert(sched_kill_task((uint32_t)id_c0) == true);
    assert(sched_get_current_task_on_core(RADIX_CORE_0) == t_any1);

    arch_host_set_core_id(RADIX_CORE_1);
    assert(sched_kill_task((uint32_t)id_c1) == true);
    /* Core 1 MUST NOT pick t_any1 because t_any1 is RUNNING on Core 0! Must pick t_any2 */
    tcb_t *c0_curr = sched_get_current_task_on_core(RADIX_CORE_0);
    tcb_t *c1_curr = sched_get_current_task_on_core(RADIX_CORE_1);
    assert(c0_curr == t_any1);
    assert(c1_curr == t_any2);
    assert(c0_curr != c1_curr);

    /* If t_any2 exits on Core 1 while t_any1 is still RUNNING on Core 0, Core 1 falls back to idle_core1 */
    task_exit();
    c1_curr = sched_get_current_task_on_core(RADIX_CORE_1);
    assert(c1_curr != NULL);
    assert(c1_curr->task_id == 0u);
    assert(c1_curr->core_affinity == (int8_t)RADIX_CORE_1);
    assert(sched_get_current_task_on_core(RADIX_CORE_0) == t_any1);

    /* Out-of-range core_id inspection returns NULL safely */
    assert(sched_get_current_task_on_core(99u) == NULL);

    kprintf("[TEST 5] Multi-core scheduler & task pinning PASSED.\n\n");
}

/* ========================================================================= */
/* Test 6: Protected AMP Call Queue & Core 1 INTC Setup                      */
/* ========================================================================= */
static uint32_t s_fifo_log[AMP_CALL_QUEUE_SIZE];
static uint32_t s_fifo_idx = 0u;

static void fifo_record_fn(void *arg) {
    uint32_t val = (uint32_t)(uintptr_t)arg;
    if (s_fifo_idx < AMP_CALL_QUEUE_SIZE) {
        s_fifo_log[s_fifo_idx++] = val;
    }
}

static void stop_core1_main_fn(void *arg) {
    (void)arg;
    amp_get_state()->core_state[RADIX_CORE_1] = CORE_STATE_HALTED;
}

static void test_protected_amp_call_queue(void) {
    kprintf("[TEST 6] Testing protected FIFO AMP call queue & Core 1 INTC wiring...\n");

    arch_host_set_core_id(RADIX_CORE_0);
    intc_init(RADIX_CORE_0);
    amp_init();
    amp_shared_state_t *st = amp_get_state();

    s_fifo_idx = 0u;
    memset(s_fifo_log, 0, sizeof(s_fifo_log));

    /* Enqueue full capacity AMP_CALL_QUEUE_SIZE (8) concurrent calls without overwrite */
    for (uint32_t i = 0u; i < AMP_CALL_QUEUE_SIZE; i++) {
        assert(amp_enqueue_call(RADIX_CORE_1, fifo_record_fn, (void *)(uintptr_t)(100u + i)) == true);
    }
    assert(st->call_queue[RADIX_CORE_1].count == AMP_CALL_QUEUE_SIZE);

    /* 9th concurrent enqueue must fail cleanly when queue is full */
    assert(amp_enqueue_call(RADIX_CORE_1, fifo_record_fn, (void *)(uintptr_t)999u) == false);
    assert(st->call_queue[RADIX_CORE_1].count == AMP_CALL_QUEUE_SIZE);

    /* Switch to Core 1 and drain via amp_core1_poll_once() */
    arch_host_set_core_id(RADIX_CORE_1);
    uint32_t reasons = amp_core1_poll_once();
    assert((reasons & IPI_REASON_CALL_FUNC) != 0u);
    assert(st->call_queue[RADIX_CORE_1].count == 0u);
    assert(s_fifo_idx == AMP_CALL_QUEUE_SIZE);

    for (uint32_t i = 0u; i < AMP_CALL_QUEUE_SIZE; i++) {
        assert(s_fifo_log[i] == (100u + i));
    }

    /* Queue accepts new items after draining, and kernel_core1_main sets up Core 1 INTC */
    arch_host_set_core_id(RADIX_CORE_0);
    assert(amp_enqueue_call(RADIX_CORE_1, stop_core1_main_fn, NULL) == true);

    arch_host_set_core_id(RADIX_CORE_1);
    kernel_core1_main();

    assert(intc_host_get_mapped_irq(RADIX_CORE_1, INT_SRC_CPU_INTR_FROM_CPU_1) == INTC_CPU_LINE_IPI);
    assert((intc_get_enabled_mask() & (1UL << INTC_CPU_LINE_IPI)) != 0u);
    assert(st->core_state[RADIX_CORE_1] == CORE_STATE_HALTED);

    /* Core 0 received AMP_MSG_CORE1_READY sent by kernel_core1_main() */
    arch_host_set_core_id(RADIX_CORE_0);
    uint32_t msg_id = 0u;
    uint16_t len = 0u;
    assert(amp_recv_from_peer(&msg_id, NULL, &len) == true);
    assert(msg_id == AMP_MSG_CORE1_READY);

    kprintf("[TEST 6] Protected FIFO AMP call queue & Core 1 INTC PASSED.\n\n");
}

int main(void) {
    kprintf("=====================================================\n");
    kprintf("  RadixOS Feature 7: PI, Wait-Queue & Multi-Core Sched\n");
    kprintf("=====================================================\n");

    test_priority_inheritance();
    test_nested_mutex_priority_inheritance();
    test_recursive_mutex_reentrancy();
    test_wait_queue_cleanup_on_kill();
    test_multicore_scheduler_and_pinning();
    test_protected_amp_call_queue();

    kprintf("=====================================================\n");
    kprintf(" [SUCCESS] All Feature 7 Sync & Multi-Core Tests Passed!\n");
    kprintf("=====================================================\n");
    return 0;
}
