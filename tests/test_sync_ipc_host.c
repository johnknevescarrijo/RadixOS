/*
 * tests/test_sync_ipc_host.c - Host Unit Test Suite for Step 3
 *
 * Verifies:
 *   1. Hardware Atomics & Memory Barriers
 *   2. Spinlocks & IRQ-Safe locking
 *   3. Mutexes & Scheduler Wait-Queue Blocking/FIFO Wakeup
 *   4. Semaphores with token exhaustion & Wait-Queue Wakeup
 *   5. Zero-Copy Lock-Free SPSC Ring Buffer IPC & Circular Wrap-Around
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <radix/types.h>
#include <radix/kprintf.h>
#include <radix/atomic.h>
#include <radix/sync.h>
#include <radix/ipc.h>
#include <radix/sched.h>

#define TEST_STACK_SIZE 2048

static uint8_t s_stack_task_a[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_stack_task_b[TEST_STACK_SIZE] __attribute__((aligned(16)));

static void dummy_task_entry(void *arg) {
    (void)arg;
}

/* ========================================================================= */
/* Test 1: Atomic Primitives                                                 */
/* ========================================================================= */
static void test_atomics(void) {
    kprintf("[TEST 1] Testing Xtensa S32C1I / Host atomic primitives...\n");

    /* CAS tests */
    volatile uint32_t target = 100;
    bool ok = arch_atomic_cas(&target, 100, 200);
    assert(ok == true);
    assert(target == 200);

    /* CAS mismatch must fail without modifying target */
    ok = arch_atomic_cas(&target, 999, 500);
    assert(ok == false);
    assert(target == 200);

    /* Atomic Swap */
    uint32_t old_val = arch_atomic_swap(&target, 777);
    assert(old_val == 200);
    assert(target == 777);

    /* Atomic Fetch Add */
    old_val = arch_atomic_fetch_add(&target, 23);
    assert(old_val == 777);
    assert(target == 800);

    /* Memory Barrier */
    arch_memory_barrier();

    /* IRQ Save / Restore */
    uint32_t irq_state = arch_irq_save();
    assert(g_host_irq_flags == 1);
    arch_irq_restore(irq_state);

    kprintf("[TEST 1] Atomic primitives PASSED.\n");
}

/* ========================================================================= */
/* Test 2: Spinlocks                                                         */
/* ========================================================================= */
static void test_spinlocks(void) {
    kprintf("\n[TEST 2] Testing spinlocks & IRQ-safe variants...\n");

    spinlock_t lock;
    spinlock_init(&lock);
    assert(lock.locked == 0);

    /* Try acquire */
    assert(spinlock_try_acquire(&lock) == true);
    assert(lock.locked == 1);

    /* Subsequent try acquire must fail */
    assert(spinlock_try_acquire(&lock) == false);

    /* Release */
    spinlock_release(&lock);
    assert(lock.locked == 0);

    /* Standard acquire & release */
    spinlock_acquire(&lock);
    assert(lock.locked == 1);
    spinlock_release(&lock);
    assert(lock.locked == 0);

    /* IRQ-safe acquire & release */
    uint32_t flags = 0;
    spinlock_acquire_irqsave(&lock, &flags);
    assert(lock.locked == 1);
    spinlock_release_irqrestore(&lock, flags);
    assert(lock.locked == 0);

    kprintf("[TEST 2] Spinlocks PASSED.\n");
}

/* ========================================================================= */
/* Test 3: Mutex & Task Blocking Contention                                  */
/* ========================================================================= */
static void test_mutexes(void) {
    kprintf("\n[TEST 3] Testing mutex lock, trylock & wait-queue blocking...\n");

    mutex_t m;
    mutex_init(&m);
    assert(m.locked == 0);
    assert(m.owner == NULL);
    assert(m.count == 0);

    /* Basic uncontended trylock / unlock */
    assert(mutex_trylock(&m) == true);
    assert(m.locked == 1);
    assert(mutex_trylock(&m) == false);
    mutex_unlock(&m);
    assert(m.locked == 0);
    assert(m.owner == NULL);

    /* Contention test with scheduler: Task A acquires, Task B blocks */
    sched_init();
    int id_a = task_create("task_A", dummy_task_entry, NULL, 2,
                           s_stack_task_a, sizeof(s_stack_task_a));
    int id_b = task_create("task_B", dummy_task_entry, NULL, 2,
                           s_stack_task_b, sizeof(s_stack_task_b));
    assert(id_a > 0 && id_b > 0);

    /* Switch to Task A */
    sched_yield();
    tcb_t *curr = sched_get_current_task();
    assert(curr != NULL);
    assert(strcmp(curr->name, "task_A") == 0);

    /* Task A locks mutex */
    mutex_lock(&m);
    assert(m.locked == 1);
    assert(m.owner == curr);

    /* Switch to Task B */
    sched_yield();
    curr = sched_get_current_task();
    assert(strcmp(curr->name, "task_B") == 0);

    /* Task B attempts to lock: must block! */
    mutex_lock(&m);

    /* Task B is now enqueued in wait_queue and marked BLOCKED */
    assert(curr->state == TASK_STATE_BLOCKED);
    assert(m.count == 1);
    assert(m.wait_queue[0] == curr);

    /* Scheduler must bypass blocked Task B and run ready Task A */
    tcb_t *now_running = sched_get_current_task();
    assert(now_running != NULL);
    assert(strcmp(now_running->name, "task_A") == 0);

    /* Task A unlocks mutex: ownership transferred directly to Task B */
    mutex_unlock(&m);
    assert(m.owner == curr); /* Task B is now owner */
    assert(curr->state == TASK_STATE_READY); /* Task B unblocked */

    /* Yield to Task B */
    sched_yield();
    curr = sched_get_current_task();
    assert(strcmp(curr->name, "task_B") == 0);
    assert(curr->state == TASK_STATE_RUNNING);

    /* Task B completes critical section and releases mutex */
    mutex_unlock(&m);
    assert(m.locked == 0);
    assert(m.owner == NULL);
    assert(m.count == 0);

    kprintf("[TEST 3] Mutex contention & FIFO blocking PASSED.\n");
}

/* ========================================================================= */
/* Test 4: Semaphores & Wait Queue Wakeup                                    */
/* ========================================================================= */
static void test_semaphores(void) {
    kprintf("\n[TEST 4] Testing counting semaphores & wait-queue sleep...\n");

    semaphore_t sem;
    sem_init(&sem, 2, 2);
    assert(sem_get_count(&sem) == 2);

    /* Trywait */
    assert(sem_trywait(&sem) == true);
    assert(sem_get_count(&sem) == 1);
    assert(sem_trywait(&sem) == true);
    assert(sem_get_count(&sem) == 0);

    /* Token exhaustion */
    assert(sem_trywait(&sem) == false);

    /* Post returns tokens */
    sem_post(&sem);
    assert(sem_get_count(&sem) == 1);
    sem_wait(&sem);
    assert(sem_get_count(&sem) == 0);

    /* Blocking test: Task A waits on empty semaphore */
    sched_init();
    task_create("waiter_task", dummy_task_entry, NULL, 2,
                s_stack_task_a, sizeof(s_stack_task_a));
    task_create("poster_task", dummy_task_entry, NULL, 2,
                s_stack_task_b, sizeof(s_stack_task_b));

    sched_yield();
    tcb_t *task_a = sched_get_current_task();
    assert(strcmp(task_a->name, "waiter_task") == 0);

    /* Waiter calls sem_wait: blocks because count is 0 */
    sem_wait(&sem);
    assert(task_a->state == TASK_STATE_BLOCKED);
    assert(sem.waiters == 1);

    /* Active task is now poster_task */
    tcb_t *task_b = sched_get_current_task();
    assert(strcmp(task_b->name, "poster_task") == 0);

    /* Poster posts semaphore: awakens waiter */
    sem_post(&sem);
    assert(task_a->state == TASK_STATE_READY);
    assert(sem.waiters == 0);

    /* Yield back to waiter */
    sched_yield();
    assert(sched_get_current_task() == task_a);
    assert(task_a->state == TASK_STATE_RUNNING);

    kprintf("[TEST 4] Semaphore exhaustion & task waking PASSED.\n");
}

/* ========================================================================= */
/* Test 5: Zero-Copy Lock-Free SPSC IPC Channel                              */
/* ========================================================================= */
static void test_zero_copy_ipc(void) {
    kprintf("\n[TEST 5] Testing zero-copy SPSC lock-free IPC channel...\n");

    ipc_channel_t ch;
    ipc_channel_init(&ch);

    assert(ipc_is_empty(&ch) == true);
    assert(ipc_is_full(&ch) == false);
    assert(ipc_available_count(&ch) == 0);

    /* 1. Verify Zero-Copy Pointer Identity */
    ipc_msg_t *claim1 = ipc_claim_slot(&ch);
    assert(claim1 != NULL);
    /* The pointer MUST point directly into ch.slots[0] in SRAM */
    assert(claim1 == &ch.slots[0]);

    claim1->msg_id = 0xA1B2C3D4;
    claim1->len = 8;
    claim1->flags = 0x55;
    for (int i = 0; i < 8; i++) {
        claim1->data[i] = (uint8_t)(0x10 + i);
    }
    ipc_commit_slot(&ch);

    assert(ipc_is_empty(&ch) == false);
    assert(ipc_available_count(&ch) == 1);

    /* Consumer Zero-Copy Peek */
    const ipc_msg_t *peek1 = ipc_peek_slot(&ch);
    assert(peek1 != NULL);
    /* The pointer MUST be identical to ch.slots[0] - ZERO copies! */
    assert(peek1 == &ch.slots[0]);
    assert(peek1->msg_id == 0xA1B2C3D4);
    assert(peek1->len == 8);
    assert(peek1->flags == 0x55);
    for (int i = 0; i < 8; i++) {
        assert(peek1->data[i] == (uint8_t)(0x10 + i));
    }

    ipc_release_slot(&ch);
    assert(ipc_is_empty(&ch) == true);
    assert(ipc_available_count(&ch) == 0);

    /* 2. Buffer Full Boundary Condition */
    for (uint32_t i = 0; i < IPC_MAX_SLOTS; i++) {
        ipc_msg_t *s = ipc_claim_slot(&ch);
        assert(s != NULL);
        s->msg_id = 1000 + i;
        ipc_commit_slot(&ch);
    }

    assert(ipc_is_full(&ch) == true);
    assert(ipc_available_count(&ch) == IPC_MAX_SLOTS);
    assert(ipc_claim_slot(&ch) == NULL); /* Cannot claim when full */
    assert(ipc_send_noblock(&ch, 9999, NULL, 0) == false);

    /* Consume all slots */
    for (uint32_t i = 0; i < IPC_MAX_SLOTS; i++) {
        const ipc_msg_t *s = ipc_peek_slot(&ch);
        assert(s != NULL);
        assert(s->msg_id == 1000 + i);
        ipc_release_slot(&ch);
    }

    assert(ipc_is_empty(&ch) == true);
    assert(ipc_peek_slot(&ch) == NULL);

    /* 3. Circular Wrap-Around Past IPC_MAX_SLOTS */
    /* head and tail are at 9. Write 8 more items to advance head across the array boundary */
    uint32_t start_head = ch.head;
    for (uint32_t i = 0; i < IPC_MAX_SLOTS; i++) {
        ipc_msg_t *s = ipc_claim_slot(&ch);
        assert(s != NULL);
        /* Check that modulo wrapping maps correctly to array slots */
        assert(s == &ch.slots[(start_head + i) % IPC_MAX_SLOTS]);
        s->msg_id = 2000 + i;
        ipc_commit_slot(&ch);
    }

    assert(ipc_available_count(&ch) == IPC_MAX_SLOTS);

    uint32_t start_tail = ch.tail;
    for (uint32_t i = 0; i < IPC_MAX_SLOTS; i++) {
        const ipc_msg_t *s = ipc_peek_slot(&ch);
        assert(s != NULL);
        assert(s == &ch.slots[(start_tail + i) % IPC_MAX_SLOTS]);
        assert(s->msg_id == 2000 + i);
        ipc_release_slot(&ch);
    }

    assert(ipc_is_empty(&ch) == true);

    /* 4. Non-Blocking Convenience Helpers */
    const char *payload_str = "RadixOS Zero-Copy IPC";
    uint16_t str_len = (uint16_t)(strlen(payload_str) + 1);

    bool sent = ipc_send_noblock(&ch, 777, payload_str, str_len);
    assert(sent == true);
    assert(ipc_available_count(&ch) == 1);

    char recv_buf[64] = {0};
    uint32_t recv_id = 0;
    uint16_t recv_len = 0;

    bool recvd = ipc_recv_noblock(&ch, &recv_id, recv_buf, &recv_len);
    assert(recvd == true);
    assert(recv_id == 777);
    assert(recv_len == str_len);
    assert(strcmp(recv_buf, payload_str) == 0);
    assert(ipc_is_empty(&ch) == true);

    kprintf("[TEST 5] Zero-copy SPSC IPC channel PASSED.\n");
}

int main(void) {
    kprintf("=====================================================\n");
    kprintf("  RadixOS Step 3: Atomics, Sync & Zero-Copy IPC Tests\n");
    kprintf("=====================================================\n");

    test_atomics();
    test_spinlocks();
    test_mutexes();
    test_semaphores();
    test_zero_copy_ipc();

    kprintf("\n=====================================================\n");
    kprintf(" [SUCCESS] All Step 3 Host Simulation Tests Passed!\n");
    kprintf("=====================================================\n");

    return 0;
}
