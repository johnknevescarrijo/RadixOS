/*
 * tests/test_mm_stack_host.c - Host Unit Test Suite for Feature 5
 *
 * Verifies:
 *   1. Deterministic Kernel Heap Allocator (mm_init, kmalloc 16B alignment, kcalloc + overflow,
 *      krealloc shrink/grow in-place & relocate, kfree + full coalescing, OOM, double-free,
 *      mm_check_integrity(), and live stats/peak tracking)
 *   2. Stack Canary Guard & High-Water Mark (paint byte 0xA5, canary integrity, HWM growth,
 *      sched_check_all_stacks(), and safe offender suspension on sched_yield())
 *   3. Radix-Shell 'mem', 'ps', 'info', and 'help' integration
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <radix/types.h>
#include <radix/kprintf.h>
#include <radix/mm.h>
#include <radix/sched.h>
#include <radix/task.h>
#include <radix/shell.h>
#include <radix/world.h>
#include <radix/pms.h>
#include <radix/amp.h>
#include <radix/ulp.h>

/* Mock Core 1 entry trampoline for host test linking with kernel/amp.c / shell.c */
void _start_core1(void) {
}

#define TEST_HEAP_SIZE  16384u
#define TEST_STACK_SIZE 2048u

static uint8_t s_test_heap[TEST_HEAP_SIZE] __attribute__((aligned(16)));
static uint8_t s_stack_a[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_stack_b[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_stack_u[TEST_STACK_SIZE] __attribute__((aligned(16)));

static void dummy_task(void *arg) {
    (void)arg;
}

static tcb_t *find_task_by_id(uint32_t task_id) {
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        const tcb_t *t = sched_get_task_by_index(i);
        if (t && t->state != TASK_STATE_UNUSED && t->task_id == task_id) {
            return (tcb_t *)(uintptr_t)t;
        }
    }
    return NULL;
}

/* ========================================================================= */
/* Test 1: kmalloc Alignment, Splitting, kcalloc & Overflow Protection       */
/* ========================================================================= */
static void test_mm_alloc_and_calloc(void) {
    kprintf("[TEST 1] Testing mm_init, kmalloc (16B align), kcalloc & overflow check...\n");

    mm_init(s_test_heap, sizeof(s_test_heap));
    assert(mm_check_integrity() == true);

    mm_stats_t st;
    mm_get_stats(&st);
    assert(st.total_bytes == TEST_HEAP_SIZE);
    assert(st.used_bytes == 0u);
    assert(st.active_blocks == 0u);
    assert(st.free_bytes == st.largest_free_block);
    size_t initial_largest = st.largest_free_block;
    assert(initial_largest > 0u);

    /* kmalloc(0) returns NULL cleanly without corrupting heap */
    assert(kmalloc(0u) == NULL);

    /* Allocate odd sizes and verify strict 16-byte alignment */
    const size_t req_sizes[] = { 1u, 7u, 15u, 16u, 17u, 63u, 129u, 513u, 1024u };
    const size_t num_reqs = sizeof(req_sizes) / sizeof(req_sizes[0]);
    void *ptrs[9];

    for (size_t i = 0; i < num_reqs; i++) {
        ptrs[i] = kmalloc(req_sizes[i]);
        assert(ptrs[i] != NULL);
        assert((((uintptr_t)ptrs[i]) & 15u) == 0u);
        memset(ptrs[i], (int)(0x20u + i), req_sizes[i]);
    }
    assert(mm_check_integrity() == true);

    /* Verify contents did not overlap */
    for (size_t i = 0; i < num_reqs; i++) {
        const uint8_t *p = (const uint8_t *)ptrs[i];
        for (size_t j = 0; j < req_sizes[i]; j++) {
            assert(p[j] == (uint8_t)(0x20u + i));
        }
    }

    mm_get_stats(&st);
    assert(st.active_blocks == num_reqs);
    assert(st.alloc_count == num_reqs);
    assert(st.used_bytes > 0u);
    assert(st.peak_used_bytes == st.used_bytes);

    /* kcalloc: verify zeroed payload */
    uint32_t *arr = (uint32_t *)kcalloc(32u, sizeof(uint32_t));
    assert(arr != NULL);
    assert((((uintptr_t)arr) & 15u) == 0u);
    for (size_t i = 0; i < 32u; i++) {
        assert(arr[i] == 0u);
    }

    /* kcalloc: reject multiplication overflow */
    mm_get_stats(&st);
    uint32_t fails_before = st.failed_allocs;
    size_t half_shift = (sizeof(size_t) * 4u);
    size_t overflow_factor = ((size_t)1u << half_shift) + 1u;
    assert(kcalloc(overflow_factor, overflow_factor) == NULL);
    assert(kcalloc((size_t)-1, 2u) == NULL);

    mm_get_stats(&st);
    assert(st.failed_allocs == fails_before + 2u);
    assert(mm_check_integrity() == true);

    /* Free everything and verify 100% coalescing recovery */
    kfree(arr);
    for (size_t i = 0; i < num_reqs; i++) {
        kfree(ptrs[i]);
    }

    mm_get_stats(&st);
    assert(st.used_bytes == 0u);
    assert(st.active_blocks == 0u);
    assert(st.largest_free_block == initial_largest);
    assert(st.free_bytes == initial_largest);
    assert(st.peak_used_bytes > 0u);
    assert(mm_check_integrity() == true);

    kprintf("[TEST 1] kmalloc, kcalloc & 100%% coalescing PASSED.\n\n");
}

/* ========================================================================= */
/* Test 2: krealloc (Shrink, Grow In-Place, Relocate, OOM Preservation)      */
/* ========================================================================= */
static void test_mm_krealloc_and_coalescing(void) {
    kprintf("[TEST 2] Testing krealloc & multi-order coalescing...\n");

    mm_init(s_test_heap, sizeof(s_test_heap));
    mm_stats_t st0;
    mm_get_stats(&st0);
    size_t initial_largest = st0.largest_free_block;

    /* krealloc(NULL, n) -> kmalloc(n) */
    uint8_t *p = (uint8_t *)krealloc(NULL, 64u);
    assert(p != NULL);
    for (size_t i = 0; i < 64u; i++) {
        p[i] = (uint8_t)(i ^ 0x5Au);
    }

    /* Grow in-place (next block is free) */
    uint8_t *p_in_place = (uint8_t *)krealloc(p, 256u);
    assert(p_in_place == p);
    for (size_t i = 0; i < 64u; i++) {
        assert(p_in_place[i] == (uint8_t)(i ^ 0x5Au));
    }
    for (size_t i = 64u; i < 256u; i++) {
        p_in_place[i] = (uint8_t)(i & 0xFFu);
    }
    assert(mm_check_integrity() == true);

    /* Place a barrier block right after p_in_place so next grow must relocate */
    void *barrier = kmalloc(128u);
    assert(barrier != NULL);

    uint8_t *p_reloc = (uint8_t *)krealloc(p_in_place, 1024u);
    assert(p_reloc != NULL && p_reloc != p_in_place);
    for (size_t i = 0; i < 64u; i++) {
        assert(p_reloc[i] == (uint8_t)(i ^ 0x5Au));
    }
    for (size_t i = 64u; i < 256u; i++) {
        assert(p_reloc[i] == (uint8_t)(i & 0xFFu));
    }
    assert(mm_check_integrity() == true);

    /* Shrink and verify prefix preserved and tail returned to free pool */
    uint8_t *p_shrunk = (uint8_t *)krealloc(p_reloc, 32u);
    assert(p_shrunk == p_reloc);
    for (size_t i = 0; i < 32u; i++) {
        assert(p_shrunk[i] == (uint8_t)(i ^ 0x5Au));
    }
    assert(mm_check_integrity() == true);

    /* Failed OOM krealloc must preserve original pointer and content */
    assert(krealloc(p_shrunk, TEST_HEAP_SIZE * 2u) == NULL);
    for (size_t i = 0; i < 32u; i++) {
        assert(p_shrunk[i] == (uint8_t)(i ^ 0x5Au));
    }
    assert(mm_check_integrity() == true);

    /* krealloc(ptr, 0) frees block and returns NULL */
    assert(krealloc(p_shrunk, 0u) == NULL);
    kfree(barrier);

    /* Middle-coalesce check: allocate A, B, C, D -> free B, D, C, A */
    void *a = kmalloc(256u);
    void *b = kmalloc(256u);
    void *c = kmalloc(256u);
    void *d = kmalloc(256u);
    assert(a && b && c && d);
    kfree(b);
    kfree(d);
    kfree(c); /* Coalesces with both B (prev) and D (next) */
    kfree(a);

    mm_stats_t st;
    mm_get_stats(&st);
    assert(st.used_bytes == 0u);
    assert(st.active_blocks == 0u);
    assert(st.largest_free_block == initial_largest);
    assert(mm_check_integrity() == true);

    kprintf("[TEST 2] krealloc & multi-order coalescing PASSED.\n\n");
}

/* ========================================================================= */
/* Test 3: OOM, Safe Double-Free & Invalid Pointer Rejection                 */
/* ========================================================================= */
static void test_mm_oom_and_double_free(void) {
    kprintf("[TEST 3] Testing OOM, NULL free, double-free & bad pointer safety...\n");

    mm_init(s_test_heap, sizeof(s_test_heap));
    mm_stats_t st;
    mm_get_stats(&st);
    size_t initial_largest = st.largest_free_block;

    /* OOM request */
    assert(kmalloc(initial_largest + 64u) == NULL);
    mm_get_stats(&st);
    assert(st.failed_allocs == 1u);
    assert(mm_check_integrity() == true);

    /* Exact largest_free_block allocation must succeed */
    void *full = kmalloc(initial_largest);
    assert(full != NULL);
    assert(kmalloc(16u) == NULL);
    mm_get_stats(&st);
    assert(st.failed_allocs == 2u);
    assert(st.active_blocks == 1u);

    /* kfree(NULL) is a no-op */
    kfree(NULL);
    mm_get_stats(&st);
    assert(st.corrupt_frees == 0u);

    /* Double-free must be caught safely without corrupting free list */
    kfree(full);
    kfree(full);
    mm_get_stats(&st);
    assert(st.corrupt_frees == 1u);
    assert(st.active_blocks == 0u);
    assert(st.used_bytes == 0u);
    assert(st.largest_free_block == initial_largest);
    assert(mm_check_integrity() == true);

    /* Foreign/misaligned pointers rejected safely */
    uint8_t stack_dummy[32];
    void *p = kmalloc(64u);
    assert(p != NULL);
    kfree(stack_dummy);
    kfree((uint8_t *)p + 8u);
    mm_get_stats(&st);
    assert(st.corrupt_frees == 3u);
    assert(st.active_blocks == 1u);
    assert(mm_check_integrity() == true);

    kfree(p);
    assert(mm_check_integrity() == true);

    kprintf("[TEST 3] OOM & double-free protection PASSED.\n\n");
}

/* ========================================================================= */
/* Test 4: Stack Canary & High-Water Mark Diagnostics                        */
/* ========================================================================= */
static void test_stack_canary_and_high_water_mark(void) {
    kprintf("[TEST 4] Testing stack canary, paint HWM & overflow recovery...\n");

    world_init();
    pms_init();
    sched_init();

    int id_a = task_create("worker_a", dummy_task, NULL, 3, s_stack_a, sizeof(s_stack_a));
    int id_b = task_create("worker_b", dummy_task, NULL, 2, s_stack_b, sizeof(s_stack_b));
    int id_u = task_create_in_world("user_u", dummy_task, NULL, 1, s_stack_u, sizeof(s_stack_u), WORLD_1);
    assert(id_a > 0 && id_b > 0 && id_u > 0);

    tcb_t *t_a = find_task_by_id((uint32_t)id_a);
    tcb_t *t_b = find_task_by_id((uint32_t)id_b);
    tcb_t *t_u = find_task_by_id((uint32_t)id_u);
    tcb_t *t_idle = find_task_by_id(0u);
    assert(t_a && t_b && t_u && t_idle);

    /* All stacks start with intact canary and untouched paint bytes */
    assert(task_check_stack_canary(t_a) == true);
    assert(task_check_stack_canary(t_b) == true);
    assert(task_check_stack_canary(t_u) == true);
    assert(task_check_stack_canary(t_idle) == true);
    assert(sched_check_all_stacks() == 0u);
    assert(sched_get_stack_overflow_count() == 0u);

    size_t free_init = task_stack_free_min_bytes(t_a);
    size_t hwm_init = task_stack_high_water_mark(t_a);
    assert(hwm_init > 0u);
    assert(free_init + hwm_init == t_a->stack_size);

    /* Simulate real stack growth downward to offset 512 (leaving 512 - 16 = 496 free bytes) */
    for (size_t off = 512u; off < 1024u; off++) {
        s_stack_a[off] = (uint8_t)(off & 0x7Fu);
    }
    size_t free_after = task_stack_free_min_bytes(t_a);
    size_t hwm_after = task_stack_high_water_mark(t_a);
    assert(free_after == (512u - TASK_STACK_CANARY_SIZE));
    assert(hwm_after == (t_a->stack_size - free_after));
    assert(hwm_after > hwm_init);
    assert(task_check_stack_canary(t_a) == true);

    /* Start scheduler -> worker_a (prio 3) runs */
    sched_start();
    assert(sched_get_current_task() == t_a);

    /* Corrupt bottom 16-byte canary of running task worker_a */
    s_stack_a[0] = 0x00u;
    s_stack_a[15] = 0x00u;
    assert(task_check_stack_canary(t_a) == false);
    assert(sched_check_all_stacks() == 1u);

    /* sched_yield() must detect overflow, increment counter, suspend worker_a, and switch to worker_b */
    sched_yield();
    assert(sched_get_stack_overflow_count() == 1u);
    assert(t_a->state == TASK_STATE_SUSPENDED);
    assert(sched_get_current_task() == t_b);
    assert(t_b->state == TASK_STATE_RUNNING);

    /* Corrupt idle task canary and confirm idle task is NOT suspended when yielding */
    uint8_t *idle_bytes = (uint8_t *)t_idle->stack_base;
    idle_bytes[4] ^= 0xFFu;
    assert(task_check_stack_canary(t_idle) == false);
    assert(sched_check_all_stacks() == 2u);

    /* Suspend worker_b and user_u so idle runs, then yield from idle */
    task_exit(); /* exits worker_b -> switches to user_u */
    assert(sched_get_current_task() == t_u);
    task_exit(); /* exits user_u -> switches to idle */
    assert(sched_get_current_task() == t_idle);
    sched_yield();
    assert(sched_get_stack_overflow_count() == 2u);
    assert(t_idle->state == TASK_STATE_RUNNING);

    kprintf("[TEST 4] Stack canary & HWM diagnostics PASSED.\n\n");
}

/* ========================================================================= */
/* Test 5: Radix-Shell 'mem', 'ps', 'info', and 'help' Commands              */
/* ========================================================================= */
static void test_shell_mem_and_ps_commands(void) {
    kprintf("[TEST 5] Testing Radix-Shell 'mem', 'ps', 'info' & 'help' commands...\n");

    world_init();
    pms_init();
    amp_init();
    ulp_init();
    mm_init_default();
    sched_init();
    shell_init();

    int id_a = task_create("shell_mon", dummy_task, NULL, 2, s_stack_a, sizeof(s_stack_a));
    assert(id_a > 0);
    sched_start();

    void *blk1 = kmalloc(256u);
    void *blk2 = kcalloc(4u, 64u);
    assert(blk1 != NULL && blk2 != NULL);

    mm_stats_t st;
    mm_get_stats(&st);
    assert(st.active_blocks == 2u);
    assert(st.used_bytes > 0u);
    assert(mm_check_integrity() == true);

    assert(shell_execute_line("help") == SHELL_OK);
    assert(shell_execute_line("mem") == SHELL_OK);
    assert(shell_execute_line("info") == SHELL_OK);
    assert(shell_execute_line("ps") == SHELL_OK);

    kfree(blk1);
    kfree(blk2);
    assert(shell_execute_line("mem") == SHELL_OK);
    assert(mm_check_integrity() == true);

    kprintf("[TEST 5] Radix-Shell 'mem' & 'ps' commands PASSED.\n\n");
}

int main(void) {
    kprintf("=====================================================\n");
    kprintf("  RadixOS Feature 5: MM Allocator & Stack Guard Suite\n");
    kprintf("=====================================================\n");

    test_mm_alloc_and_calloc();
    test_mm_krealloc_and_coalescing();
    test_mm_oom_and_double_free();
    test_stack_canary_and_high_water_mark();
    test_shell_mem_and_ps_commands();

    kprintf("=====================================================\n");
    kprintf(" [SUCCESS] All Feature 5 MM & Stack Guard Tests Passed!\n");
    kprintf("=====================================================\n");
    return 0;
}
