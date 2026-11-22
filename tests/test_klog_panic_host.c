/*
 * tests/test_klog_panic_host.c - Host Unit Test Suite for Feature 6
 *
 * Verifies:
 *   1. kvsnprintf / ksnprintf & IRQ-safe kprintf / kprintf_raw
 *   2. klog Ring Buffer (levels, filtering, wrap-around + dropped count, ISR non-blocking mode, clear)
 *   3. Radix-Shell dmesg Command (dmesg, dmesg <level>, dmesg -c, invalid args)
 *   4. Panic EXCCAUSE Decoder & Safe Call0 Stack Backtrace Scanner
 *   5. radix_exception_handler_c() & kpanic() Panic Report + WORLD_1 Fault Isolation
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

#include <radix/types.h>
#include <radix/kprintf.h>
#include <radix/klog.h>
#include <radix/panic.h>
#include <radix/shell.h>
#include <radix/sched.h>
#include <radix/world.h>
#include <radix/pms.h>
#include <radix/syscall.h>
#include <radix/amp.h>
#include <radix/ulp.h>
#include <xtensa_regs.h>
#include <esp32s3_soc.h>

/* Mock Core 1 entry trampoline for host test execution */
void _start_core1(void) {
}

#define TEST_STACK_SIZE 2048u

static uint8_t s_user_stack_a[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_user_stack_b[TEST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t s_kernel_stack[TEST_STACK_SIZE] __attribute__((aligned(16)));

static void dummy_task_entry(void *arg) {
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
/* Test 1: kvsnprintf / ksnprintf & kprintf_raw                              */
/* ========================================================================= */
static void test_kvsnprintf_and_kprintf(void) {
    kprintf("[TEST 1] Testing kvsnprintf, ksnprintf & panic-safe kprintf_raw...\n");

    char buf[32];
    int len = ksnprintf(buf, sizeof(buf), "id=%d hex=0x%04x s=%s %%", -42, 0xABu, "ok");
    assert(len == (int)strlen("id=-42 hex=0x00ab s=ok %"));
    assert(strcmp(buf, "id=-42 hex=0x00ab s=ok %") == 0);

    /* Truncation preserves NUL termination and returns full intended length */
    char small[8];
    int full_len = ksnprintf(small, sizeof(small), "0123456789ABCDEF");
    assert(full_len == 16);
    assert(strcmp(small, "0123456") == 0);

    /* INT32_MIN and NULL string safety */
    char min_buf[48];
    const char * volatile null_str = NULL;
    ksnprintf(min_buf, sizeof(min_buf), "%d %s", INT32_MIN, null_str);
    assert(strcmp(min_buf, "-2147483648 (null)") == 0);

    /* Pointer formatting */
    char ptr_buf[32];
    int ptr_len = ksnprintf(ptr_buf, sizeof(ptr_buf), "%p", (void *)0x40378124u);
    assert(ptr_len > 2);
    assert(strncmp(ptr_buf, "0x", 2) == 0);
    assert(strstr(ptr_buf, "40378124") != NULL);

    /* Raw output helper works without deadlock */
    kprintf_raw_puts("[RAW] lock-free puts check\n");
    kprintf_raw("[RAW] formatted check %u\n", 123u);

    kprintf("[TEST 1] kvsnprintf & kprintf_raw PASSED.\n\n");
}

/* ========================================================================= */
/* Test 2: klog Levels, Wrap-Around, Dropped Count, ISR Mode & Clear         */
/* ========================================================================= */
static void test_klog_ring_buffer(void) {
    kprintf("[TEST 2] Testing klog ring buffer levels, wrap-around & ISR mode...\n");

    klog_init();
    klog_set_console_mirror(false);

    assert(klog_get_level() == KLOG_LEVEL_INFO);
    assert(klog_count() == 0u);
    assert(klog_dropped_count() == 0u);

    /* DEBUG should be filtered out at default INFO level */
    KLOG_DEBUG("filtered debug %u", 1u);
    assert(klog_count() == 0u);

    KLOG_INFO("info msg %u", 10u);
    KLOG_WARN("warn msg %u", 20u);
    KLOG_ERR("err msg %u", 30u);
    assert(klog_count() == 3u);

    /* Enable DEBUG level */
    klog_set_level(KLOG_LEVEL_DEBUG);
    assert(klog_get_level() == KLOG_LEVEL_DEBUG);
    KLOG_DEBUG("debug msg %u", 40u);
    assert(klog_count() == 4u);

    klog_entry_t e;
    assert(klog_get_entry(0u, &e) == true);
    assert(e.level == KLOG_LEVEL_INFO);
    assert(strcmp(e.msg, "info msg 10") == 0);

    assert(klog_get_entry(3u, &e) == true);
    assert(e.level == KLOG_LEVEL_DEBUG);
    assert(strcmp(e.msg, "debug msg 40") == 0);
    assert(klog_get_entry(4u, &e) == false);

    /* Test ISR safety: even with console_mirror=true, ISR logs to ring only */
    klog_set_console_mirror(true);
    sched_enter_isr();
    assert(sched_in_isr() == true);
    KLOG_WARN("isr safe entry %u", 77u);
    sched_exit_isr();
    assert(sched_in_isr() == false);
    assert(klog_count() == 5u);
    assert(klog_get_entry(4u, &e) == true);
    assert(e.level == KLOG_LEVEL_WARN);
    assert(strcmp(e.msg, "isr safe entry 77") == 0);

    /* Test wrap-around & dropped count */
    klog_clear();
    assert(klog_count() == 0u);
    assert(klog_dropped_count() == 0u);

    klog_set_console_mirror(false);
    const uint32_t extra = 12u;
    const uint32_t total_writes = KLOG_MAX_ENTRIES + extra;
    for (uint32_t i = 0u; i < total_writes; i++) {
        KLOG_INFO("wrap entry %u", i);
    }

    assert(klog_count() == KLOG_MAX_ENTRIES);
    assert(klog_dropped_count() == extra);

    /* Oldest retained entry should be "wrap entry 12" */
    assert(klog_get_entry(0u, &e) == true);
    assert(strcmp(e.msg, "wrap entry 12") == 0);

    /* Newest retained entry should be "wrap entry 75" */
    assert(klog_get_entry(KLOG_MAX_ENTRIES - 1u, &e) == true);
    assert(strcmp(e.msg, "wrap entry 75") == 0);

    klog_clear();
    assert(klog_count() == 0u);
    assert(klog_dropped_count() == 0u);

    kprintf("[TEST 2] klog ring buffer PASSED.\n\n");
}

/* ========================================================================= */
/* Test 3: Shell dmesg Command                                               */
/* ========================================================================= */
static void test_shell_dmesg(void) {
    kprintf("[TEST 3] Testing Radix-Shell dmesg command...\n");

    klog_init();
    klog_set_level(KLOG_LEVEL_DEBUG);
    klog_set_console_mirror(false);

    KLOG_DEBUG("shell debug log");
    KLOG_INFO("shell info log");
    KLOG_WARN("shell warn log");
    KLOG_ERR("shell err log");
    assert(klog_count() == 4u);

    assert(shell_execute_line("dmesg") == SHELL_OK);
    assert(shell_execute_line("dmesg warn") == SHELL_OK);
    assert(shell_execute_line("dmesg err") == SHELL_OK);
    assert(shell_execute_line("dmesg invalid_lvl") == SHELL_ERR_BAD_ARG);
    assert(klog_count() == 4u);

    /* dmesg -c dumps and clears the buffer */
    assert(shell_execute_line("dmesg -c") == SHELL_OK);
    assert(klog_count() == 0u);

    kprintf("[TEST 3] Shell dmesg command PASSED.\n\n");
}

/* ========================================================================= */
/* Test 4: EXCCAUSE Decoder & Call0 Backtrace Scanner                        */
/* ========================================================================= */
static void test_exccause_and_call0_backtrace(void) {
    kprintf("[TEST 4] Testing EXCCAUSE decoder & Call0 stack backtrace...\n");

    assert(strcmp(panic_exccause_str(EXCCAUSE_ILLEGAL_INSTRUCTION), "IllegalInstruction") == 0);
    assert(strcmp(panic_exccause_str(EXCCAUSE_SYSCALL), "Syscall") == 0);
    assert(strcmp(panic_exccause_str(EXCCAUSE_INSTR_FETCH_ERROR), "InstructionFetchError") == 0);
    assert(strcmp(panic_exccause_str(EXCCAUSE_LOAD_STORE_ERROR), "LoadStoreError") == 0);
    assert(strcmp(panic_exccause_str(EXCCAUSE_LEVEL1_INTERRUPT), "Level1Interrupt") == 0);
    assert(strcmp(panic_exccause_str(EXCCAUSE_INTEGER_DIVIDE_BY_ZERO), "IntegerDivideByZero") == 0);
    assert(strcmp(panic_exccause_str(EXCCAUSE_PRIVILEGED), "Privileged") == 0);
    assert(strcmp(panic_exccause_str(EXCCAUSE_LOAD_STORE_ALIGNMENT), "LoadStoreAlignment") == 0);
    assert(strcmp(panic_exccause_str(EXCCAUSE_INSTR_PIF_DATA_ERROR), "InstrPIFDataError") == 0);
    assert(strcmp(panic_exccause_str(EXCCAUSE_LOAD_STORE_PIF_DATA_ERROR), "LoadStorePIFDataError") == 0);
    assert(strcmp(panic_exccause_str(EXCCAUSE_INSTR_PIF_ADDR_ERROR), "InstrPIFAddrError") == 0);
    assert(strcmp(panic_exccause_str(EXCCAUSE_LOAD_STORE_PIF_ADDR_ERROR), "LoadStorePIFAddrError") == 0);
    assert(strcmp(panic_exccause_str(EXCCAUSE_INSTR_FETCH_PROHIBITED), "InstFetchProhibited") == 0);
    assert(strcmp(panic_exccause_str(EXCCAUSE_LOAD_PROHIBITED), "LoadProhibited") == 0);
    assert(strcmp(panic_exccause_str(EXCCAUSE_STORE_PROHIBITED), "StoreProhibited") == 0);
    assert(strcmp(panic_exccause_str(999u), "UnknownException") == 0);

    /* Synthetic Call0 stack buffer */
    uint32_t stack_words[32];
    for (size_t i = 0u; i < 32u; i++) {
        stack_words[i] = 0x3FC90000u + (uint32_t)(i * 4u); /* DRAM / non-code data */
    }

    /* Sentinel outside scan range [stack_low, stack_high) must never be read */
    stack_words[0] = 0x40378050u;
    stack_words[31] = 0x40378060u;

    /* Valid stack slice: words 4..27 */
    uintptr_t stack_low = (uintptr_t)&stack_words[4];
    uintptr_t stack_high = (uintptr_t)&stack_words[28];
    uintptr_t sp = (uintptr_t)&stack_words[6];

    /* Entry before SP inside stack_low..sp should not be included */
    stack_words[4] = 0x40378090u;

    /* Real Call0 frames in ascending stack address order */
    stack_words[7]  = 0x40378120u;
    stack_words[11] = 0x40378450u;
    stack_words[18] = 0x40379004u;
    stack_words[25] = 0x40380010u;

    uintptr_t code_low = 0x40378000u;
    uintptr_t code_high = 0x40390000u;
    uintptr_t pcs[8] = {0};

    uint32_t n = panic_backtrace_call0(sp, stack_low, stack_high, code_low, code_high, pcs, 8u);
    assert(n == 4u);
    assert(pcs[0] == 0x40378120u);
    assert(pcs[1] == 0x40378450u);
    assert(pcs[2] == 0x40379004u);
    assert(pcs[3] == 0x40380010u);

    /* Truncation via max_frames */
    uintptr_t pcs_short[2] = {0};
    assert(panic_backtrace_call0(sp, stack_low, stack_high, code_low, code_high, pcs_short, 2u) == 2u);
    assert(pcs_short[0] == 0x40378120u);
    assert(pcs_short[1] == 0x40378450u);

    /* Unaligned SP rounds up safely without reading below SP */
    uintptr_t pcs_unaligned[8] = {0};
    assert(panic_backtrace_call0((uintptr_t)&stack_words[7] + 1u,
                                 stack_low, stack_high, code_low, code_high,
                                 pcs_unaligned, 8u) == 3u);
    assert(pcs_unaligned[0] == 0x40378450u);

    /* Out-of-bounds SP rejected */
    assert(panic_backtrace_call0(stack_low - 4u, stack_low, stack_high, code_low, code_high, pcs, 8u) == 0u);
    assert(panic_backtrace_call0(stack_high, stack_low, stack_high, code_low, code_high, pcs, 8u) == 0u);
    assert(panic_backtrace_call0(sp, stack_low, stack_high, code_low, code_high, NULL, 8u) == 0u);

    kprintf("[TEST 4] EXCCAUSE decoder & Call0 backtrace PASSED.\n\n");
}

/* ========================================================================= */
/* Test 5: Exception Handler Panic Report & WORLD_1 Isolation                */
/* ========================================================================= */
static void test_exception_handler_panic_and_isolation(void) {
    kprintf("[TEST 5] Testing radix_exception_handler_c panic report & WORLD_1 isolation...\n");

    world_init();
    pms_init();
    sched_init();
    klog_init();
    klog_set_console_mirror(false);
    panic_reset_stats();

    int id_ua = task_create_in_world("fault_ua", dummy_task_entry, NULL, 3,
                                     s_user_stack_a, sizeof(s_user_stack_a), WORLD_1);
    int id_kern = task_create("kern_ok", dummy_task_entry, NULL, 2,
                              s_kernel_stack, sizeof(s_kernel_stack));
    assert(id_ua > 0 && id_kern > 0);

    tcb_t *t_ua = find_task_by_id((uint32_t)id_ua);
    tcb_t *t_kern = find_task_by_id((uint32_t)id_kern);
    assert(t_ua && t_kern);

    sched_start();
    assert(sched_get_current_task() == t_ua);
    assert(world_get_current() == WORLD_1);

    /* Place synthetic Call0 return frames inside fault_ua's stack above canary */
    uint32_t *ua_words = (uint32_t *)(void *)(s_user_stack_a + TASK_STACK_CANARY_SIZE + 64u);
    ua_words[0] = 0x40378300u;
    ua_words[3] = 0x40378500u;

    trap_frame_t tf;
    memset(&tf, 0, sizeof(tf));
    tf.a0 = 0x40378200u;
    tf.a1 = (uint32_t)(uintptr_t)ua_words;
    tf.a2 = 0x11111111u;
    tf.a3 = 0x22222222u;
    tf.a15 = 0xF15F15F1u;
    tf.epc1 = 0x40379040u;
    tf.ps = 0x00000020u;
    tf.exccause = EXCCAUSE_LOAD_PROHIBITED;
    tf.excvaddr = 0x3FC88100u;
    tf.caller_world = WORLD_1;

    uint32_t violations_before = pms_get_stats()->pms_violations;

    radix_exception_handler_c(EXCCAUSE_LOAD_PROHIBITED, 0x40379040u, 0x3FC88100u, (uint32_t *)&tf);

    /* Verify panic_report_t populated with registers & backtrace */
    assert(panic_get_count() == 1u);
    const panic_report_t *rep = panic_get_last_report();
    assert(rep != NULL && rep->valid == true);
    assert(rep->exccause == EXCCAUSE_LOAD_PROHIBITED);
    assert(rep->epc1 == 0x40379040u);
    assert(rep->excvaddr == 0x3FC88100u);
    assert(rep->ps == 0x00000020u);
    assert(rep->caller_world == WORLD_1);
    assert(rep->task_id == (uint32_t)id_ua);
    assert(strcmp(rep->task_name, "fault_ua") == 0);
    assert(rep->stack_canary_ok == true);
    assert(rep->has_regs == true);
    assert(rep->regs.a2 == 0x11111111u);
    assert(rep->regs.a15 == 0xF15F15F1u);
    /* Backtrace should have a0 + scanned stack frames when stack fits 32-bit trap_frame.a1 */
    assert(rep->backtrace_count >= 1u);
    assert(rep->backtrace[0] == 0x40378200u);
    if ((uintptr_t)ua_words <= 0xFFFFFFFFu) {
        assert(rep->backtrace_count == 3u);
        assert(rep->backtrace[1] == 0x40378300u);
        assert(rep->backtrace[2] == 0x40378500u);
    }

    /* Verify KLOG_PANIC entry recorded */
    assert(klog_count() >= 1u);
    klog_entry_t ke;
    assert(klog_get_entry(klog_count() - 1u, &ke) == true);
    assert(ke.level == KLOG_LEVEL_PANIC);
    assert(strstr(ke.msg, "LoadProhibited") != NULL);

    /* Verify WORLD_1 fault isolation: fault_ua terminated, kern_ok now running in WORLD_0 */
    assert(pms_get_stats()->pms_violations == violations_before + 1u);
    assert(t_ua->state == TASK_STATE_SUSPENDED);
    assert(sched_get_current_task() == t_kern);
    assert(t_kern->state == TASK_STATE_RUNNING);
    assert(world_get_current() == WORLD_0);

    /* Also test WORLD_0 exception does not loop infinitely under RADIX_HOST_TEST */
    trap_frame_t tf_k;
    memset(&tf_k, 0, sizeof(tf_k));
    tf_k.a0 = 0x40378800u;
    tf_k.epc1 = 0x40378900u;
    tf_k.exccause = EXCCAUSE_INTEGER_DIVIDE_BY_ZERO;
    tf_k.caller_world = WORLD_0;

    radix_exception_handler_c(EXCCAUSE_INTEGER_DIVIDE_BY_ZERO, 0x40378900u, 0u, (uint32_t *)&tf_k);
    assert(panic_get_count() == 2u);
    rep = panic_get_last_report();
    assert(rep->exccause == EXCCAUSE_INTEGER_DIVIDE_BY_ZERO);
    assert(rep->caller_world == WORLD_0);

    /* Verify kpanic() API */
    kpanic("unit test explicit panic");
    assert(panic_get_count() == 3u);
    rep = panic_get_last_report();
    assert(strcmp(rep->reason, "unit test explicit panic") == 0);

    (void)s_user_stack_b;

    kprintf("[TEST 5] radix_exception_handler_c & WORLD_1 isolation PASSED.\n\n");
}

int main(void) {
    kprintf("=====================================================\n");
    kprintf("  RadixOS Feature 6: klog, dmesg & Panic/Backtrace\n");
    kprintf("=====================================================\n");

    amp_init();
    ulp_init();

    test_kvsnprintf_and_kprintf();
    test_klog_ring_buffer();
    test_shell_dmesg();
    test_exccause_and_call0_backtrace();
    test_exception_handler_panic_and_isolation();

    kprintf("=====================================================\n");
    kprintf(" [SUCCESS] All Feature 6 klog & Panic Tests Passed!\n");
    kprintf("=====================================================\n");
    return 0;
}
