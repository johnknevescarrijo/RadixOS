/*
 * tests/test_pms_syscall_host.c - Host Unit Test Suite for Step 5
 *
 * Verifies:
 *   1. World Controller Domain Switching (World 0 <-> World 1) & Transition Counters
 *   2. PMS Region Isolation, Access Permissions & Boundary/Overflow Checking
 *   3. Hardware Access Violation Handling & Offending Task Termination
 *   4. Syscall Gateway, Dispatch Table & Confused Deputy Attack Protection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <radix/types.h>
#include <radix/kprintf.h>
#include <radix/sched.h>
#include <radix/pms.h>
#include <radix/syscall.h>
#include <radix/ipc.h>
#include <radix/hal.h>
#include <xtensa_regs.h>

/* Mock Core 1 entry trampoline for host test execution */
void _start_core1(void) {
}

#define TEST_STACK_SIZE 2048

static uint8_t s_stack_offender[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_stack_survivor[TEST_STACK_SIZE] __attribute__((aligned(16)));

/* Buffers for Syscall testing */
static uint8_t s_user_buffer[512] __attribute__((aligned(16)));
static uint8_t s_kernel_buffer[512] __attribute__((aligned(16)));

static void dummy_task_entry(void *arg) {
    (void)arg;
}

/* ========================================================================= */
/* Test 1: World Controller Switching                                        */
/* ========================================================================= */
static void test_world_controller_switching(void) {
    kprintf("[TEST 1] Testing World Controller domain transitions...\n");

    world_init();
    assert(world_get_current() == RADIX_WORLD_0_KERNEL);

    const world_pms_stats_t *stats = pms_get_stats();
    uint32_t switches_before = stats->world0_to_world1_switches;
    uint32_t traps_before = stats->world1_to_world0_traps;

    /* Transition to User mode (World 1) */
    world_switch_to_user(0x40379000);
    assert(world_get_current() == RADIX_WORLD_1_USER);
    assert(stats->world0_to_world1_switches == switches_before + 1);

    /* Transition to Kernel mode (World 0) via trap */
    world_enter_kernel_from_trap();
    assert(world_get_current() == RADIX_WORLD_0_KERNEL);
    assert(stats->world1_to_world0_traps == traps_before + 1);

    /* Repeat multiple cycles to verify monotonicity */
    for (int i = 0; i < 5; i++) {
        world_switch_to_user(0x40379100 + i * 4);
        assert(world_get_current() == RADIX_WORLD_1_USER);
        world_enter_kernel_from_trap();
        assert(world_get_current() == RADIX_WORLD_0_KERNEL);
    }

    assert(stats->world0_to_world1_switches == switches_before + 6);
    assert(stats->world1_to_world0_traps == traps_before + 6);

    kprintf("[TEST 1] World Controller switching PASSED.\n\n");
}

/* ========================================================================= */
/* Test 2: PMS Region Permissions & Boundary Checking                        */
/* ========================================================================= */
static void test_pms_region_permissions(void) {
    kprintf("[TEST 2] Testing PMS region isolation and permissions...\n");

    pms_init();

    /*
     * Default regions configured by pms_init():
     *   Region 0: Kernel IRAM (0x40378000, 0x18000) -> World 0: RX, World 1: NONE
     *   Region 1: Kernel DRAM (0x3FC88000, 0x10000) -> World 0: RW, World 1: NONE
     */

    /* Kernel (World 0) permissions check */
    assert(pms_check_access(RADIX_WORLD_0_KERNEL, 0x40378000, 256, PMS_PERM_RX) == true);
    assert(pms_check_access(RADIX_WORLD_0_KERNEL, 0x40378000, 256, PMS_PERM_R) == true);
    assert(pms_check_access(RADIX_WORLD_0_KERNEL, 0x40378000, 256, PMS_PERM_X) == true);
    /* Kernel IRAM does not have write permission */
    assert(pms_check_access(RADIX_WORLD_0_KERNEL, 0x40378000, 256, PMS_PERM_W) == false);

    assert(pms_check_access(RADIX_WORLD_0_KERNEL, 0x3FC88000, 512, PMS_PERM_RW) == true);
    assert(pms_check_access(RADIX_WORLD_0_KERNEL, 0x3FC88000, 512, PMS_PERM_X) == false);

    /* User (World 1) access to Kernel areas must be strictly denied */
    assert(pms_check_access(RADIX_WORLD_1_USER, 0x40378000, 128, PMS_PERM_R) == false);
    assert(pms_check_access(RADIX_WORLD_1_USER, 0x40378000, 128, PMS_PERM_X) == false);
    assert(pms_check_access(RADIX_WORLD_1_USER, 0x3FC88000, 128, PMS_PERM_R) == false);
    assert(pms_check_access(RADIX_WORLD_1_USER, 0x3FC88000, 128, PMS_PERM_W) == false);

    /*
     * Configure Region 2: User Sandbox Data/Stack (0x3FC98000, 0x10000)
     *   World 0: RW
     *   World 1: RW (No Execute)
     */
    bool configured = pms_configure_region(2, 0x3FC98000, 0x10000, PMS_PERM_RW, PMS_PERM_RW);
    assert(configured == true);

    /* World 1 can read and write inside user sandbox */
    assert(pms_check_access(RADIX_WORLD_1_USER, 0x3FC98000, 1024, PMS_PERM_RW) == true);
    assert(pms_check_access(RADIX_WORLD_1_USER, 0x3FC98000, 1024, PMS_PERM_R) == true);
    assert(pms_check_access(RADIX_WORLD_1_USER, 0x3FC98000, 1024, PMS_PERM_W) == true);

    /* World 1 CANNOT execute in data stack (NX protection) */
    assert(pms_check_access(RADIX_WORLD_1_USER, 0x3FC98000, 1024, PMS_PERM_X) == false);

    /* Boundary overflow check: buffer crosses the end of region 2 */
    uintptr_t near_end = 0x3FC98000 + 0x10000 - 16;
    assert(pms_check_access(RADIX_WORLD_1_USER, near_end, 16, PMS_PERM_R) == true);
    assert(pms_check_access(RADIX_WORLD_1_USER, near_end, 32, PMS_PERM_R) == false);

    /* Integer wrap-around check */
    assert(pms_check_access(RADIX_WORLD_1_USER, (uintptr_t)-10, 20, PMS_PERM_R) == false);

    /* Unmapped address check */
    assert(pms_check_access(RADIX_WORLD_1_USER, 0x20000000, 64, PMS_PERM_R) == false);

    /* Zero length check */
    assert(pms_check_access(RADIX_WORLD_1_USER, 0x20000000, 0, PMS_PERM_R) == true);

    kprintf("[TEST 2] PMS region permissions PASSED.\n\n");
}

/* ========================================================================= */
/* Test 3: Hardware Exception / Access Violation Handling                    */
/* ========================================================================= */
static void test_pms_violation_exception_handling(void) {
    kprintf("[TEST 3] Testing PMS access violation exception handling & task termination...\n");

    /* Initialize scheduler */
    sched_init();

    int id_offender = task_create("offender", dummy_task_entry, NULL, 2, s_stack_offender, sizeof(s_stack_offender));
    int id_survivor = task_create("survivor", dummy_task_entry, NULL, 2, s_stack_survivor, sizeof(s_stack_survivor));
    assert(id_offender > 0);
    assert(id_survivor > 0);

    /* Schedule first task */
    sched_yield();
    tcb_t *curr = sched_get_current_task();
    assert(curr != NULL);
    uint32_t current_task_id = curr->task_id;

    const world_pms_stats_t *stats = pms_get_stats();
    uint32_t violations_before = stats->pms_violations;

    /* Simulate illegal memory access trap in current task */
    uintptr_t illegal_addr = 0x3FC89000;
    pms_handle_violation_exception(EXCCAUSE_LOAD_STORE_ERROR, illegal_addr);

    /* Verify PMS violation was recorded */
    assert(stats->pms_violations == violations_before + 1);
    assert(stats->last_fault_addr == illegal_addr);
    assert(stats->last_fault_exccause == EXCCAUSE_LOAD_STORE_ERROR);
    assert(stats->last_killed_task_id == current_task_id);

    /* Verify offending task was terminated */
    assert(curr->state == TASK_STATE_SUSPENDED);

    /* Verify survivor task was dispatched and is now RUNNING */
    tcb_t *next_task = sched_get_current_task();
    assert(next_task != NULL);
    assert(next_task->task_id != current_task_id);
    assert(next_task->state == TASK_STATE_RUNNING);

    kprintf("[TEST 3] Access violation task termination PASSED.\n\n");
}

/* ========================================================================= */
/* Test 4: Syscall Gateway & Confused Deputy Protection                      */
/* ========================================================================= */
static void test_syscall_gateway_and_confused_deputy(void) {
    kprintf("[TEST 4] Testing Syscall gateway & Confused Deputy protection...\n");

    /* Configure user memory region covering s_user_buffer */
    bool ok = pms_configure_region(3, (uintptr_t)s_user_buffer, sizeof(s_user_buffer),
                                  PMS_PERM_RW, PMS_PERM_RW);
    assert(ok == true);

    /* Configure restricted kernel-only memory region covering s_kernel_buffer */
    ok = pms_configure_region(4, (uintptr_t)s_kernel_buffer, sizeof(s_kernel_buffer),
                              PMS_PERM_RW, PMS_PERM_NONE);
    assert(ok == true);

    /* Initialize an IPC channel within user-accessible memory */
    ipc_channel_t *ch = (ipc_channel_t *)s_user_buffer;
    ipc_channel_init(ch);

    /* Test 4.1: sys_yield */
    int32_t res = sys_yield();
    assert(res == RADIX_SYSCALL_OK);

    /* Test 4.2: sys_get_time with valid user buffer */
    uint64_t *user_ticks = (uint64_t *)(s_user_buffer + 256);
    *user_ticks = 0;
    res = sys_get_time(user_ticks);
    assert(res == RADIX_SYSCALL_OK);

    /* Test 4.3: sys_uart_write with valid user buffer */
    const char *test_msg = "RadixOS Syscall Gateway Test OK\n";
    size_t msg_len = strlen(test_msg);
    memcpy(s_user_buffer + 300, test_msg, msg_len + 1);
    res = sys_uart_write((const char *)(s_user_buffer + 300), msg_len);
    assert(res == (int32_t)msg_len);

    /* Test 4.4: sys_ipc_send and sys_ipc_recv with valid user buffer */
    const char *send_data = "Hello Syscall IPC";
    uint16_t send_len = (uint16_t)strlen(send_data);
    memcpy(s_user_buffer + 350, send_data, send_len);

    res = sys_ipc_send(ch, 101, s_user_buffer + 350, send_len);
    assert(res == RADIX_SYSCALL_OK);

    uint32_t *recv_id = (uint32_t *)(s_user_buffer + 400);
    uint16_t *recv_len = (uint16_t *)(s_user_buffer + 404);
    uint8_t  *recv_data = s_user_buffer + 410;

    res = sys_ipc_recv(ch, recv_id, recv_data, recv_len);
    assert(res == RADIX_SYSCALL_OK);
    assert(*recv_id == 101);
    assert(*recv_len == send_len);
    assert(memcmp(recv_data, send_data, send_len) == 0);

    /*
     * Confused Deputy Attack Prevention:
     * A malicious User Task calls Kernel Syscalls passing pointers into
     * Kernel-only memory (s_kernel_buffer), expecting the privileged kernel
     * to read or write sensitive kernel structures on its behalf.
     */

    /* Confused Deputy Attempt 1: sys_uart_write reading restricted kernel memory */
    res = sys_uart_write((const char *)s_kernel_buffer, 32);
    assert(res == RADIX_SYSCALL_ERR_ACCESS);

    /* Confused Deputy Attempt 2: sys_ipc_send reading restricted kernel memory */
    res = sys_ipc_send(ch, 202, s_kernel_buffer, 32);
    assert(res == RADIX_SYSCALL_ERR_ACCESS);

    /* Confused Deputy Attempt 3: sys_get_time writing into restricted kernel memory */
    res = sys_get_time((uint64_t *)s_kernel_buffer);
    assert(res == RADIX_SYSCALL_ERR_ACCESS);

    /* Confused Deputy Attempt 4: sys_ipc_recv writing into restricted kernel memory */
    res = sys_ipc_recv(ch, (uint32_t *)s_kernel_buffer, recv_data, recv_len);
    assert(res == RADIX_SYSCALL_ERR_ACCESS);

    /* Test 4.5: Invalid Syscall ID */
    res = radix_syscall_dispatch(SYS_MAX_SYSCALLS + 50, 0, 0, 0, 0);
    assert(res == RADIX_SYSCALL_ERR_NOSYS);

    /* Verify total syscall count */
    assert(syscall_get_total_count() > 0);

    kprintf("[TEST 4] Syscall gateway & Confused Deputy protection PASSED.\n\n");
}

/* ========================================================================= */
/* Main Test Runner                                                          */
/* ========================================================================= */
int main(void) {
    kprintf("=====================================================\n");
    kprintf("  RadixOS Step 5: World Controller, PMS & Syscalls\n");
    kprintf("=====================================================\n");

    test_world_controller_switching();
    test_pms_region_permissions();
    test_pms_violation_exception_handling();
    test_syscall_gateway_and_confused_deputy();

    kprintf("=====================================================\n");
    kprintf(" [SUCCESS] All Step 5 Host Simulation Tests Passed!\n");
    kprintf("=====================================================\n");

    return 0;
}
