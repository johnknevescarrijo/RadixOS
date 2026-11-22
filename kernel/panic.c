/*
 * kernel/panic.c - Kernel Panic Handler, Register Dump & Call0 Backtrace Decoder
 */

#include <radix/panic.h>
#include <radix/klog.h>
#include <radix/kprintf.h>
#include <radix/sched.h>
#include <radix/sync.h>
#include <radix/amp.h>
#include <radix/world.h>
#include <radix/atomic.h>
#include <xtensa_regs.h>

#ifndef RADIX_HOST_TEST
#include <esp32s3_soc.h>
extern uint8_t _iram_start[];
extern uint8_t _iram_end[];
extern uint8_t _stack_bottom[];
extern uint8_t _stack_top[];
#else
#include <esp32s3_soc.h>
#endif

static spinlock_t     s_panic_lock = SPINLOCK_INIT;
static panic_report_t s_last_report;
static uint32_t       s_panic_count = 0u;

static void panic_memcpy(void *dst, const void *src, size_t len) {
    volatile uint8_t *d = (volatile uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0u; i < len; i++) {
        d[i] = s[i];
    }
}

static void panic_copy_str(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0u) {
        return;
    }
    size_t i = 0u;
    if (src) {
        while (src[i] != '\0' && i + 1u < dst_size) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static void panic_default_code_bounds(uintptr_t *code_low, uintptr_t *code_high) {
#ifndef RADIX_HOST_TEST
    *code_low = (uintptr_t)_iram_start;
    *code_high = (uintptr_t)_iram_end;
#else
    *code_low = (uintptr_t)ESP32S3_IRAM_START;
    *code_high = (uintptr_t)(ESP32S3_IRAM_START + 0x60000u);
#endif
}

static void panic_resolve_stack_bounds(const tcb_t *curr,
                                       const trap_frame_t *regs,
                                       uintptr_t *out_sp,
                                       uintptr_t *out_low,
                                       uintptr_t *out_high) {
    *out_sp = 0u;
    *out_low = 0u;
    *out_high = 0u;

    if (curr && curr->stack_base && curr->stack_size > TASK_STACK_CANARY_SIZE) {
        uintptr_t low = (uintptr_t)curr->stack_base + TASK_STACK_CANARY_SIZE;
        uintptr_t high = (uintptr_t)curr->stack_base + curr->stack_size;
        uintptr_t sp = 0u;
        if (regs) {
            sp = (uintptr_t)regs->a1;
        } else {
            sp = (uintptr_t)curr->sp;
        }
        *out_low = low;
        *out_high = high;
        *out_sp = sp;
        return;
    }

#ifndef RADIX_HOST_TEST
    uintptr_t low = (uintptr_t)_stack_bottom;
    uintptr_t high = (uintptr_t)_stack_top;
    uintptr_t sp = 0u;
    if (regs) {
        sp = (uintptr_t)regs->a1;
    } else {
        __asm__ volatile ("mov %0, a1" : "=r"(sp));
    }
    *out_low = low;
    *out_high = high;
    *out_sp = sp;
#else
    if (regs && regs->a1 != 0u) {
        *out_sp = (uintptr_t)regs->a1;
    }
#endif
}

const char *panic_exccause_str(uint32_t exccause) {
    switch (exccause) {
        case EXCCAUSE_ILLEGAL_INSTRUCTION:      return "IllegalInstruction";
        case EXCCAUSE_SYSCALL:                  return "Syscall";
        case EXCCAUSE_INSTR_FETCH_ERROR:        return "InstructionFetchError";
        case EXCCAUSE_LOAD_STORE_ERROR:         return "LoadStoreError";
        case EXCCAUSE_LEVEL1_INTERRUPT:         return "Level1Interrupt";
        case EXCCAUSE_ALLOCA:                   return "Alloca";
        case EXCCAUSE_INTEGER_DIVIDE_BY_ZERO:   return "IntegerDivideByZero";
        case EXCCAUSE_SPECULATION:              return "Speculation";
        case EXCCAUSE_PRIVILEGED:               return "Privileged";
        case EXCCAUSE_LOAD_STORE_ALIGNMENT:     return "LoadStoreAlignment";
        case EXCCAUSE_INSTR_PIF_DATA_ERROR:     return "InstrPIFDataError";
        case EXCCAUSE_LOAD_STORE_PIF_DATA_ERROR:return "LoadStorePIFDataError";
        case EXCCAUSE_INSTR_PIF_ADDR_ERROR:     return "InstrPIFAddrError";
        case EXCCAUSE_LOAD_STORE_PIF_ADDR_ERROR:return "LoadStorePIFAddrError";
        case EXCCAUSE_ITLB_MISS:                return "InstTLBMiss";
        case EXCCAUSE_ITLB_MULTIHIT:            return "InstTLBMultiHit";
        case EXCCAUSE_INSTR_FETCH_PRIVILEGE:    return "InstFetchPrivilege";
        case EXCCAUSE_INSTR_FETCH_PROHIBITED:   return "InstFetchProhibited";
        case EXCCAUSE_DTLB_MISS:                return "LoadStoreTLBMiss";
        case EXCCAUSE_DTLB_MULTIHIT:            return "LoadStoreTLBMultiHit";
        case EXCCAUSE_LOAD_STORE_PRIVILEGE:     return "LoadStorePrivilege";
        case EXCCAUSE_LOAD_PROHIBITED:          return "LoadProhibited";
        case EXCCAUSE_STORE_PROHIBITED:         return "StoreProhibited";
        default:
            if (exccause >= 32u && exccause <= 39u) {
                return "CoprocessorDisabled";
            }
            return "UnknownException";
    }
}

uint32_t panic_backtrace_call0(uintptr_t sp,
                               uintptr_t stack_low,
                               uintptr_t stack_high,
                               uintptr_t code_low,
                               uintptr_t code_high,
                               uintptr_t *out_pcs,
                               uint32_t max_frames) {
    if (!out_pcs || max_frames == 0u || stack_low >= stack_high || code_low >= code_high) {
        return 0u;
    }

    if (sp < stack_low || sp >= stack_high) {
        return 0u;
    }

    uintptr_t cur = (sp + 3u) & ~(uintptr_t)3u;
    uint32_t count = 0u;

    if (sizeof(uintptr_t) > sizeof(uint32_t) && code_high > (uintptr_t)0xFFFFFFFFu) {
        while (cur + sizeof(uintptr_t) <= stack_high && count < max_frames) {
            const uint8_t *bytes = (const uint8_t *)cur;
            uintptr_t val = 0u;
            for (size_t b = 0u; b < sizeof(uintptr_t); b++) {
                val |= ((uintptr_t)bytes[b]) << (b * 8u);
            }
            if (val >= code_low && val < code_high) {
                out_pcs[count++] = val;
                cur += sizeof(uintptr_t);
            } else {
                cur += sizeof(uint32_t);
            }
        }
        return count;
    }

    while (cur + sizeof(uint32_t) <= stack_high && count < max_frames) {
        uint32_t pc32 = *(const uint32_t *)cur;
        uintptr_t pc = (uintptr_t)pc32;
        if (pc >= code_low && pc < code_high) {
            out_pcs[count++] = pc;
        }
        cur += sizeof(uint32_t);
    }

    return count;
}

static void panic_print_report(const panic_report_t *r) {
    const char *cause_name = panic_exccause_str(r->exccause);

    kprintf_raw("\n=== RADIXOS KERNEL PANIC / EXCEPTION DUMP ===\n");
    if (r->reason[0] != '\0') {
        kprintf_raw("Reason   : %s\n", r->reason);
    }
    kprintf_raw("Cause    : %s (EXCCAUSE=%u)\n", cause_name, (unsigned int)r->exccause);
    kprintf_raw("EPC1     : 0x%08x | EXCVADDR: 0x%08x | PS: 0x%08x | World: %u\n",
                (unsigned int)r->epc1,
                (unsigned int)r->excvaddr,
                (unsigned int)r->ps,
                (unsigned int)r->caller_world);
    kprintf_raw("Task     : '%s' (id=%u) | Core: %u | Stack Canary: %s\n",
                r->task_name,
                (unsigned int)r->task_id,
                (unsigned int)r->core_id,
                r->stack_canary_ok ? "OK" : "CORRUPTED");

    if (r->has_regs) {
        const trap_frame_t *tf = &r->regs;
        kprintf_raw("Regs     : a0 =0x%08x a1 =0x%08x a2 =0x%08x a3 =0x%08x\n",
                    (unsigned int)tf->a0, (unsigned int)tf->a1,
                    (unsigned int)tf->a2, (unsigned int)tf->a3);
        kprintf_raw("           a4 =0x%08x a5 =0x%08x a6 =0x%08x a7 =0x%08x\n",
                    (unsigned int)tf->a4, (unsigned int)tf->a5,
                    (unsigned int)tf->a6, (unsigned int)tf->a7);
        kprintf_raw("           a8 =0x%08x a9 =0x%08x a10=0x%08x a11=0x%08x\n",
                    (unsigned int)tf->a8, (unsigned int)tf->a9,
                    (unsigned int)tf->a10, (unsigned int)tf->a11);
        kprintf_raw("           a12=0x%08x a13=0x%08x a14=0x%08x a15=0x%08x\n",
                    (unsigned int)tf->a12, (unsigned int)tf->a13,
                    (unsigned int)tf->a14, (unsigned int)tf->a15);
    }

    kprintf_raw("Backtrace (%u frames):", (unsigned int)r->backtrace_count);
    for (uint32_t i = 0u; i < r->backtrace_count; i++) {
        kprintf_raw(" #%u=%p", (unsigned int)i, (void *)r->backtrace[i]);
    }
    kprintf_raw("\n=============================================\n");
}

void panic_dump_exception(uint32_t exccause,
                          uint32_t epc1,
                          uint32_t excvaddr,
                          const trap_frame_t *regs) {
    uint64_t tick = sched_get_ticks();
    uint32_t core_id = arch_get_core_id();
    const tcb_t *curr = sched_get_current_task();

    panic_report_t report;
    report.valid = true;
    report.tick = tick;
    report.core_id = core_id;
    report.exccause = exccause;
    report.epc1 = epc1;
    report.excvaddr = excvaddr;
    report.ps = regs ? regs->ps : 0u;
    report.caller_world = regs ? regs->caller_world
                               : (curr ? (uint32_t)curr->world : (uint32_t)world_get_current());
    report.task_id = curr ? curr->task_id : 0u;
    panic_copy_str(report.task_name, sizeof(report.task_name), curr ? curr->name : "none");
    report.stack_canary_ok = curr ? task_check_stack_canary(curr) : true;
    report.backtrace_count = 0u;
    for (uint32_t i = 0u; i < PANIC_MAX_BACKTRACE_FRAMES; i++) {
        report.backtrace[i] = 0u;
    }
    panic_copy_str(report.reason, sizeof(report.reason), panic_exccause_str(exccause));

    if (regs) {
        report.has_regs = true;
        panic_memcpy(&report.regs, regs, sizeof(report.regs));
    } else {
        report.has_regs = false;
        uint32_t *z = (uint32_t *)&report.regs;
        for (size_t i = 0u; i < sizeof(report.regs) / sizeof(uint32_t); i++) {
            z[i] = 0u;
        }
    }

    uintptr_t code_low = 0u, code_high = 0u;
    panic_default_code_bounds(&code_low, &code_high);

    if (regs && (uintptr_t)regs->a0 >= code_low && (uintptr_t)regs->a0 < code_high) {
        report.backtrace[report.backtrace_count++] = (uintptr_t)regs->a0;
    }

    uintptr_t sp = 0u, stack_low = 0u, stack_high = 0u;
    panic_resolve_stack_bounds(curr, regs, &sp, &stack_low, &stack_high);
    if (stack_high > stack_low && report.backtrace_count < PANIC_MAX_BACKTRACE_FRAMES) {
        uint32_t scanned = panic_backtrace_call0(
            sp, stack_low, stack_high, code_low, code_high,
            &report.backtrace[report.backtrace_count],
            PANIC_MAX_BACKTRACE_FRAMES - report.backtrace_count);
        report.backtrace_count += scanned;
    }

    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_panic_lock, &flags);
    s_panic_count++;
    report.panic_seq = s_panic_count;
    panic_memcpy(&s_last_report, &report, sizeof(s_last_report));
    spinlock_release_irqrestore(&s_panic_lock, flags);

    KLOG_PANIC("EXC %s(%u) epc=0x%08x vaddr=0x%08x task=%s(%u) W%u",
               panic_exccause_str(exccause),
               (unsigned int)exccause,
               (unsigned int)epc1,
               (unsigned int)excvaddr,
               report.task_name,
               (unsigned int)report.task_id,
               (unsigned int)report.caller_world);

    panic_print_report(&report);
}

void kpanic(const char *reason) {
    const char *msg = reason ? reason : "fatal kernel panic";
    uint64_t tick = sched_get_ticks();
    uint32_t core_id = arch_get_core_id();
    const tcb_t *curr = sched_get_current_task();

    panic_report_t report;
    report.valid = true;
    report.tick = tick;
    report.core_id = core_id;
    report.exccause = 0u;
    report.epc1 = 0u;
    report.excvaddr = 0u;
    report.ps = 0u;
    report.caller_world = curr ? (uint32_t)curr->world : (uint32_t)world_get_current();
    report.task_id = curr ? curr->task_id : 0u;
    panic_copy_str(report.task_name, sizeof(report.task_name), curr ? curr->name : "kernel");
    report.stack_canary_ok = curr ? task_check_stack_canary(curr) : true;
    report.has_regs = false;
    uint32_t *z = (uint32_t *)&report.regs;
    for (size_t i = 0u; i < sizeof(report.regs) / sizeof(uint32_t); i++) {
        z[i] = 0u;
    }
    report.backtrace_count = 0u;
    for (uint32_t i = 0u; i < PANIC_MAX_BACKTRACE_FRAMES; i++) {
        report.backtrace[i] = 0u;
    }
    panic_copy_str(report.reason, sizeof(report.reason), msg);

    uintptr_t code_low = 0u, code_high = 0u;
    panic_default_code_bounds(&code_low, &code_high);

    uintptr_t sp = 0u, stack_low = 0u, stack_high = 0u;
    panic_resolve_stack_bounds(curr, NULL, &sp, &stack_low, &stack_high);
    if (stack_high > stack_low) {
        report.backtrace_count = panic_backtrace_call0(
            sp, stack_low, stack_high, code_low, code_high,
            report.backtrace, PANIC_MAX_BACKTRACE_FRAMES);
    }

    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_panic_lock, &flags);
    s_panic_count++;
    report.panic_seq = s_panic_count;
    panic_memcpy(&s_last_report, &report, sizeof(s_last_report));
    spinlock_release_irqrestore(&s_panic_lock, flags);

    KLOG_PANIC("PANIC: %s (task=%s id=%u)", msg, report.task_name, (unsigned int)report.task_id);
    panic_print_report(&report);

#ifndef RADIX_HOST_TEST
    arch_irq_save();
    while (1) {
        __asm__ volatile ("waiti 0");
    }
#endif
}

const panic_report_t *panic_get_last_report(void) {
    return &s_last_report;
}

uint32_t panic_get_count(void) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_panic_lock, &flags);
    uint32_t count = s_panic_count;
    spinlock_release_irqrestore(&s_panic_lock, flags);
    return count;
}

void panic_reset_stats(void) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_panic_lock, &flags);
    s_panic_count = 0u;
    uint8_t *bytes = (uint8_t *)&s_last_report;
    for (size_t i = 0u; i < sizeof(s_last_report); i++) {
        bytes[i] = 0u;
    }
    spinlock_release_irqrestore(&s_panic_lock, flags);
}
