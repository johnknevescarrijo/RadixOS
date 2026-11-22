/*
 * tests/test_world_isolation_host.c - Host Unit Test Suite for Feature 4
 *
 * Verifies:
 *   1. ESP32-S3 PMS Hardware Shadow Register Programming (Split Lines & World 0/1 Constrains)
 *   2. task_create() (WORLD_0) vs task_create_in_world(..., WORLD_1) & Context-Switch PMS Updates
 *   3. Real Per-Task Isolation Between Two WORLD_1 Tasks & Confused Deputy Syscall Blocking
 *   4. World-Aware Syscall Return (WORLD_0 stays WORLD_0; WORLD_1 returns to WORLD_1)
 *   5. Safe Termination of Offending WORLD_1 Task on PMS/Illegal Instruction Violation
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <radix/types.h>
#include <radix/kprintf.h>
#include <radix/sched.h>
#include <radix/world.h>
#include <radix/pms.h>
#include <radix/syscall.h>
#include <radix/ipc.h>
#include <esp32s3_soc.h>
#include <xtensa_regs.h>

/* Mock Core 1 entry trampoline for host test execution */
void _start_core1(void) {
}

#define TEST_STACK_SIZE 2048

typedef struct {
    ipc_channel_t ch;
    uint64_t      ticks;
    uint32_t      msg_id;
    uint16_t      len;
    char          text[32];
    uint8_t       payload[IPC_SLOT_PAYLOAD_SIZE];
    uint8_t       recv_buf[IPC_SLOT_PAYLOAD_SIZE];
} user_sandbox_t;

static uint8_t s_stack_kernel[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_stack_user_a[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_stack_user_b[TEST_STACK_SIZE] __attribute__((aligned(16)));

static user_sandbox_t s_sandbox_a __attribute__((aligned(16)));
static user_sandbox_t s_sandbox_b __attribute__((aligned(16)));
static uint8_t        s_kernel_secret[256] __attribute__((aligned(16)));

static void dummy_task_entry(void *arg) {
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
/* Test 1: PMS Hardware Shadow Register Programming                          */
/* ========================================================================= */
static void test_pms_hw_shadow_registers(void) {
    kprintf("[TEST 1] Testing ESP32-S3 PMS hardware shadow registers...\n");

    world_init();
    pms_init();

    const pms_hw_regs_t *hw = pms_get_hw_regs();
    assert(hw != NULL);
    assert(hw->split_line_ctrl0 == 0u);
    assert(hw->iram0_pms_ctrl0 == 0u);
    assert(hw->dram0_pms_ctrl0 == 0u);
    assert((hw->iram0_monitor_ctrl1 & SENSITIVE_PMS_MONITOR_VIOLATE_EN) != 0u);
    assert((hw->dram0_monitor_ctrl1 & SENSITIVE_PMS_MONITOR_VIOLATE_EN) != 0u);

    /* World 0 IRAM0 PMS_0 has R|X (0x5); World 1 IRAM0 PMS_0 has NONE (0x0) */
    uint32_t iram_w0_p0 = (hw->iram0_pms_world0 >> SENSITIVE_IRAM0_PMS_AREA_SHIFT(0)) & 0x7u;
    uint32_t iram_w1_p0 = (hw->iram0_pms_world1 >> SENSITIVE_IRAM0_PMS_AREA_SHIFT(0)) & 0x7u;
    assert(iram_w0_p0 == (SENSITIVE_IRAM0_PMS_SRAM_R | SENSITIVE_IRAM0_PMS_SRAM_F));
    assert(iram_w1_p0 == 0u);

    /* Before any task region is active: World 0 DRAM0 PMS_0 has RW (0x3), World 1 PMS_0/1 = 0 */
    uint32_t dram_w0_p0 = (hw->dram0_pms_world01 >> SENSITIVE_DRAM0_PMS_W0_AREA_SHIFT(0)) & 0x3u;
    uint32_t dram_w1_p0 = (hw->dram0_pms_world01 >> SENSITIVE_DRAM0_PMS_W1_AREA_SHIFT(0)) & 0x3u;
    uint32_t dram_w1_p1 = (hw->dram0_pms_world01 >> SENSITIVE_DRAM0_PMS_W1_AREA_SHIFT(1)) & 0x3u;
    assert(dram_w0_p0 == (SENSITIVE_DRAM0_PMS_SRAM_R | SENSITIVE_DRAM0_PMS_SRAM_W));
    assert(dram_w1_p0 == 0u);
    assert(dram_w1_p1 == 0u);

    /* Program a simulated ESP32-S3 SRAM task stack + extra user region */
    pms_apply_task_region(0x3FCA1200u, 2048u, 0x3FCA4000u, 1024u);

    /* Split line DRAM0_0: 0x3FCA1200 -> level 4 (cat2 == EQUAL_SA = 0x2), splitaddr = 0x12 */
    uint32_t split_dram0_addr = (hw->split_line_dram_0 >> SENSITIVE_SPLIT_LINE_SPLITADDR_SHIFT) &
                                SENSITIVE_SPLIT_LINE_SPLITADDR_MASK;
    uint32_t split_dram0_cat2 = (hw->split_line_dram_0 >> 4u) & 0x3u;
    assert(split_dram0_addr == 0x12u);
    assert(split_dram0_cat2 == SENSITIVE_SPLIT_LINE_CAT_EQUAL);

    /* Split line DRAM0_1: end = 0x3FCA1200 + 2048 + 0xFF -> aligned 0x3FCA1A00 -> splitaddr = 0x1A */
    uint32_t split_dram1_addr = (hw->split_line_dram_1 >> SENSITIVE_SPLIT_LINE_SPLITADDR_SHIFT) &
                                SENSITIVE_SPLIT_LINE_SPLITADDR_MASK;
    assert(split_dram1_addr == 0x1Au);

    /* World 1 DRAM0 PMS_1 (stack) & PMS_2 (extra) now have RW (0x3), while PMS_0 (kernel) stays 0 */
    dram_w1_p0 = (hw->dram0_pms_world01 >> SENSITIVE_DRAM0_PMS_W1_AREA_SHIFT(0)) & 0x3u;
    dram_w1_p1 = (hw->dram0_pms_world01 >> SENSITIVE_DRAM0_PMS_W1_AREA_SHIFT(1)) & 0x3u;
    uint32_t dram_w1_p2 = (hw->dram0_pms_world01 >> SENSITIVE_DRAM0_PMS_W1_AREA_SHIFT(2)) & 0x3u;
    assert(dram_w1_p0 == 0u);
    assert(dram_w1_p1 == (SENSITIVE_DRAM0_PMS_SRAM_R | SENSITIVE_DRAM0_PMS_SRAM_W));
    assert(dram_w1_p2 == (SENSITIVE_DRAM0_PMS_SRAM_R | SENSITIVE_DRAM0_PMS_SRAM_W));

    kprintf("[TEST 1] PMS hardware shadow registers PASSED.\n\n");
}

/* ========================================================================= */
/* Test 2: task_create (WORLD_0) vs task_create_in_world (WORLD_1)           */
/* ========================================================================= */
static void test_task_world_creation_and_switching(void) {
    kprintf("[TEST 2] Testing task_create vs task_create_in_world & context switch PMS sync...\n");

    world_init();
    pms_init();
    sched_init();

    int id_kern = task_create("kern_srv", dummy_task_entry, NULL, 2,
                              s_stack_kernel, sizeof(s_stack_kernel));
    int id_ua = task_create_in_world("user_a", dummy_task_entry, NULL, 2,
                                     s_stack_user_a, sizeof(s_stack_user_a), WORLD_1);
    int id_ub = task_create_in_world("user_b", dummy_task_entry, NULL, 2,
                                     s_stack_user_b, sizeof(s_stack_user_b), WORLD_1);

    assert(id_kern > 0);
    assert(id_ua > 0);
    assert(id_ub > 0);

    tcb_t *t_kern = find_task_by_id((uint32_t)id_kern);
    tcb_t *t_ua = find_task_by_id((uint32_t)id_ua);
    tcb_t *t_ub = find_task_by_id((uint32_t)id_ub);
    assert(t_kern != NULL && t_ua != NULL && t_ub != NULL);

    /* Verify initial world and PS.UM values */
    assert(t_kern->world == WORLD_0);
    assert((t_kern->sp->ps & PS_UM) == 0u);
    assert(t_ua->world == WORLD_1);
    assert((t_ua->sp->ps & PS_UM) == PS_UM);
    assert(t_ub->world == WORLD_1);
    assert((t_ub->sp->ps & PS_UM) == PS_UM);

    /* Dispatch first task -> kern_srv (WORLD_0) */
    sched_start();
    assert(sched_get_current_task() == t_kern);
    assert(world_get_current() == WORLD_0);

    const pms_hw_regs_t *hw = pms_get_hw_regs();
    assert(((hw->dram0_pms_world01 >> SENSITIVE_DRAM0_PMS_W1_AREA_SHIFT(1)) & 0x3u) == 0u);

    /* Yield -> user_a (WORLD_1) */
    sched_yield();
    assert(sched_get_current_task() == t_ua);
    assert(world_get_current() == WORLD_1);
    assert(((hw->dram0_pms_world01 >> SENSITIVE_DRAM0_PMS_W1_AREA_SHIFT(1)) & 0x3u) ==
           (SENSITIVE_DRAM0_PMS_SRAM_R | SENSITIVE_DRAM0_PMS_SRAM_W));

    /* Yield -> user_b (WORLD_1) */
    sched_yield();
    assert(sched_get_current_task() == t_ub);
    assert(world_get_current() == WORLD_1);
    assert(((hw->dram0_pms_world01 >> SENSITIVE_DRAM0_PMS_W1_AREA_SHIFT(1)) & 0x3u) ==
           (SENSITIVE_DRAM0_PMS_SRAM_R | SENSITIVE_DRAM0_PMS_SRAM_W));

    /* Yield -> back to kern_srv (WORLD_0) */
    sched_yield();
    assert(sched_get_current_task() == t_kern);
    assert(world_get_current() == WORLD_0);
    assert(((hw->dram0_pms_world01 >> SENSITIVE_DRAM0_PMS_W1_AREA_SHIFT(1)) & 0x3u) == 0u);

    kprintf("[TEST 2] Task creation & context-switch World/PMS sync PASSED.\n\n");
}

/* ========================================================================= */
/* Test 3: Per-Task Isolation Between Two WORLD_1 Tasks & Syscalls           */
/* ========================================================================= */
static void test_inter_task_world1_isolation(void) {
    kprintf("[TEST 3] Testing isolation between two WORLD_1 tasks & Confused Deputy syscalls...\n");

    world_init();
    pms_init();
    sched_init();

    /* Configure explicit kernel-only region for s_kernel_secret */
    assert(pms_configure_region(4, (uintptr_t)s_kernel_secret, sizeof(s_kernel_secret),
                                PMS_PERM_RW, PMS_PERM_NONE) == true);

    ipc_channel_init(&s_sandbox_a.ch);
    ipc_channel_init(&s_sandbox_b.ch);
    memcpy(s_sandbox_a.text, "TaskA_OK", 8);
    memcpy(s_sandbox_b.text, "TaskB_OK", 8);
    memcpy(s_sandbox_a.payload, "MsgFromA", 8);
    memcpy(s_sandbox_b.payload, "MsgFromB", 8);

    int id_ua = task_create_in_world("user_a", dummy_task_entry, NULL, 2,
                                     s_stack_user_a, sizeof(s_stack_user_a), WORLD_1);
    int id_ub = task_create_in_world("user_b", dummy_task_entry, NULL, 2,
                                     s_stack_user_b, sizeof(s_stack_user_b), WORLD_1);
    assert(id_ua > 0 && id_ub > 0);

    assert(task_set_user_region((uint32_t)id_ua, &s_sandbox_a, sizeof(s_sandbox_a)) == true);
    assert(task_set_user_region((uint32_t)id_ub, &s_sandbox_b, sizeof(s_sandbox_b)) == true);

    /* Start scheduler -> user_a runs */
    sched_start();
    assert(sched_get_current_task()->task_id == (uint32_t)id_ua);
    assert(world_get_current() == WORLD_1);

    /* Task A can access its own stack & sandbox, but NOT Task B's stack/sandbox or kernel memory */
    assert(pms_check_access(WORLD_1, (uintptr_t)s_stack_user_a, 128, PMS_PERM_RW) == true);
    assert(pms_check_access(WORLD_1, (uintptr_t)&s_sandbox_a, sizeof(s_sandbox_a), PMS_PERM_RW) == true);
    assert(pms_check_access(WORLD_1, (uintptr_t)s_stack_user_b, 128, PMS_PERM_R) == false);
    assert(pms_check_access(WORLD_1, (uintptr_t)&s_sandbox_b, 64, PMS_PERM_R) == false);
    assert(pms_check_access(WORLD_1, (uintptr_t)s_kernel_secret, 64, PMS_PERM_R) == false);
    assert(pms_check_access(WORLD_1, 0x3FC88000u, 64, PMS_PERM_R) == false);

    /* Task A legitimate syscalls */
    assert(sys_uart_write(s_sandbox_a.text, 8) == 8);
    assert(sys_get_time(&s_sandbox_a.ticks) == RADIX_SYSCALL_OK);
    assert(sys_ipc_send(&s_sandbox_a.ch, 11, s_sandbox_a.payload, 8) == RADIX_SYSCALL_OK);
    assert(sys_ipc_recv(&s_sandbox_a.ch, &s_sandbox_a.msg_id, s_sandbox_a.recv_buf, &s_sandbox_a.len) == RADIX_SYSCALL_OK);
    assert(s_sandbox_a.msg_id == 11u);
    assert(s_sandbox_a.len == 8u);
    assert(memcmp(s_sandbox_a.recv_buf, "MsgFromA", 8) == 0);

    /* Task A Confused Deputy attempts against Task B and Kernel */
    assert(sys_uart_write((const char *)s_stack_user_b, 16) == RADIX_SYSCALL_ERR_ACCESS);
    assert(sys_uart_write(s_sandbox_b.text, 8) == RADIX_SYSCALL_ERR_ACCESS);
    assert(sys_uart_write((const char *)s_kernel_secret, 16) == RADIX_SYSCALL_ERR_ACCESS);
    assert(sys_get_time(&s_sandbox_b.ticks) == RADIX_SYSCALL_ERR_ACCESS);
    assert(sys_get_time((uint64_t *)s_kernel_secret) == RADIX_SYSCALL_ERR_ACCESS);
    assert(sys_ipc_send(&s_sandbox_b.ch, 12, s_sandbox_a.payload, 8) == RADIX_SYSCALL_ERR_ACCESS);
    assert(sys_ipc_send(&s_sandbox_a.ch, 12, s_stack_user_b, 8) == RADIX_SYSCALL_ERR_ACCESS);
    assert(sys_ipc_recv(&s_sandbox_a.ch, (uint32_t *)s_stack_user_b, s_sandbox_a.recv_buf, &s_sandbox_a.len) == RADIX_SYSCALL_ERR_ACCESS);

    /* Switch to Task B */
    sched_yield();
    assert(sched_get_current_task()->task_id == (uint32_t)id_ub);
    assert(world_get_current() == WORLD_1);

    /* Task B can access its own stack & sandbox, but NOT Task A's */
    assert(pms_check_access(WORLD_1, (uintptr_t)s_stack_user_b, 128, PMS_PERM_RW) == true);
    assert(pms_check_access(WORLD_1, (uintptr_t)&s_sandbox_b, sizeof(s_sandbox_b), PMS_PERM_RW) == true);
    assert(pms_check_access(WORLD_1, (uintptr_t)s_stack_user_a, 128, PMS_PERM_R) == false);
    assert(pms_check_access(WORLD_1, (uintptr_t)&s_sandbox_a, 64, PMS_PERM_R) == false);

    assert(sys_uart_write(s_sandbox_b.text, 8) == 8);
    assert(sys_uart_write(s_sandbox_a.text, 8) == RADIX_SYSCALL_ERR_ACCESS);
    assert(sys_ipc_send(&s_sandbox_a.ch, 13, s_sandbox_b.payload, 8) == RADIX_SYSCALL_ERR_ACCESS);
    assert(sys_ipc_send(&s_sandbox_b.ch, 14, s_sandbox_b.payload, 8) == RADIX_SYSCALL_OK);

    kprintf("\n[TEST 3] Inter-task WORLD_1 isolation & Confused Deputy blocking PASSED.\n\n");
}

/* ========================================================================= */
/* Test 4: World-Aware Syscall Return (WORLD_0 stays WORLD_0)                */
/* ========================================================================= */
static void test_world_aware_syscall_return(void) {
    kprintf("[TEST 4] Testing world-aware syscall return (WORLD_0 vs WORLD_1)...\n");

    world_init();
    pms_init();
    sched_init();

    int id_kern = task_create("kern_only", dummy_task_entry, NULL, 3,
                              s_stack_kernel, sizeof(s_stack_kernel));
    int id_user = task_create_in_world("user_low", dummy_task_entry, NULL, 1,
                                       s_stack_user_a, sizeof(s_stack_user_a), WORLD_1);
    assert(id_kern > 0 && id_user > 0);
    assert(task_set_user_region((uint32_t)id_user, &s_sandbox_a, sizeof(s_sandbox_a)) == true);

    /* Highest priority task is WORLD_0 */
    sched_start();
    assert(sched_get_current_task()->task_id == (uint32_t)id_kern);
    assert(world_get_current() == WORLD_0);

    const world_pms_stats_t *stats = pms_get_stats();
    uint32_t w0_to_w1_before = stats->world0_to_world1_switches;
    uint32_t w1_to_w0_before = stats->world1_to_world0_traps;

    /* Syscall from WORLD_0 MUST preserve WORLD_0 and NOT transition to WORLD_1 */
    assert(sys_yield() == RADIX_SYSCALL_OK);
    assert(world_get_current() == WORLD_0);
    assert(stats->world0_to_world1_switches == w0_to_w1_before);
    assert(stats->world1_to_world0_traps == w1_to_w0_before);

    /* Terminate WORLD_0 task so WORLD_1 task gets scheduled */
    task_exit();
    assert(sched_get_current_task()->task_id == (uint32_t)id_user);
    assert(world_get_current() == WORLD_1);

    w0_to_w1_before = stats->world0_to_world1_switches;
    w1_to_w0_before = stats->world1_to_world0_traps;

    /* Syscall from WORLD_1 MUST trap to WORLD_0 and return back to WORLD_1 */
    assert(sys_get_time(&s_sandbox_a.ticks) == RADIX_SYSCALL_OK);
    assert(world_get_current() == WORLD_1);
    assert(stats->world1_to_world0_traps == w1_to_w0_before + 1u);
    assert(stats->world0_to_world1_switches == w0_to_w1_before + 1u);

    kprintf("[TEST 4] World-aware syscall return PASSED.\n\n");
}

/* ========================================================================= */
/* Test 5: WORLD_1 Violation Exception Kills Only Offender                   */
/* ========================================================================= */
static void test_world1_violation_keeps_kernel_alive(void) {
    kprintf("[TEST 5] Testing WORLD_1 violation exception handling keeps kernel operational...\n");

    world_init();
    pms_init();
    sched_init();

    int id_ua = task_create_in_world("bad_user_a", dummy_task_entry, NULL, 3,
                                     s_stack_user_a, sizeof(s_stack_user_a), WORLD_1);
    int id_ub = task_create_in_world("bad_user_b", dummy_task_entry, NULL, 2,
                                     s_stack_user_b, sizeof(s_stack_user_b), WORLD_1);
    int id_kern = task_create("kern_survivor", dummy_task_entry, NULL, 1,
                              s_stack_kernel, sizeof(s_stack_kernel));
    assert(id_ua > 0 && id_ub > 0 && id_kern > 0);

    tcb_t *t_ua = find_task_by_id((uint32_t)id_ua);
    tcb_t *t_ub = find_task_by_id((uint32_t)id_ub);
    tcb_t *t_kern = find_task_by_id((uint32_t)id_kern);
    assert(t_ua && t_ub && t_kern);

    sched_start();
    assert(sched_get_current_task() == t_ua);
    assert(world_get_current() == WORLD_1);

    const world_pms_stats_t *stats = pms_get_stats();
    uint32_t violations_before = stats->pms_violations;

    /* Simulate Load Prohibited PMS exception inside WORLD_1 task A via exception handler */
    radix_exception_handler_c(EXCCAUSE_LOAD_PROHIBITED, 0x40379010u, 0x3FC88200u, NULL);

    assert(stats->pms_violations == violations_before + 1u);
    assert(stats->last_fault_exccause == EXCCAUSE_LOAD_PROHIBITED);
    assert(stats->last_fault_addr == 0x3FC88200u);
    assert(stats->last_killed_task_id == (uint32_t)id_ua);
    assert(t_ua->state == TASK_STATE_SUSPENDED);

    /* Scheduler dispatches surviving user_b in WORLD_1 */
    assert(sched_get_current_task() == t_ub);
    assert(t_ub->state == TASK_STATE_RUNNING);
    assert(world_get_current() == WORLD_1);

    /* Simulate Illegal Instruction exception inside WORLD_1 task B */
    radix_exception_handler_c(EXCCAUSE_ILLEGAL_INSTRUCTION, 0x40379050u, 0x0u, NULL);

    assert(stats->pms_violations == violations_before + 2u);
    assert(stats->last_fault_exccause == EXCCAUSE_ILLEGAL_INSTRUCTION);
    assert(stats->last_killed_task_id == (uint32_t)id_ub);
    assert(t_ub->state == TASK_STATE_SUSPENDED);

    /* Kernel survivor task takes over in WORLD_0 and remains fully operational */
    assert(sched_get_current_task() == t_kern);
    assert(t_kern->state == TASK_STATE_RUNNING);
    assert(world_get_current() == WORLD_0);
    assert(sys_yield() == RADIX_SYSCALL_OK);
    assert(world_get_current() == WORLD_0);

    kprintf("[TEST 5] WORLD_1 violation safe termination PASSED.\n\n");
}

int main(void) {
    kprintf("=====================================================\n");
    kprintf("  RadixOS Feature 4: Per-Task PMS & World 1 Suite\n");
    kprintf("=====================================================\n");

    test_pms_hw_shadow_registers();
    test_task_world_creation_and_switching();
    test_inter_task_world1_isolation();
    test_world_aware_syscall_return();
    test_world1_violation_keeps_kernel_alive();

    kprintf("=====================================================\n");
    kprintf(" [SUCCESS] All Feature 4 World Isolation Tests Passed!\n");
    kprintf("=====================================================\n");
    return 0;
}
