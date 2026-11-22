#ifndef RADIX_PANIC_H
#define RADIX_PANIC_H

#include <radix/types.h>
#include <radix/task.h>

#define PANIC_MAX_BACKTRACE_FRAMES 16u
#define PANIC_REASON_MAX_LEN       64u

/*
 * Xtensa LX7 Trap Frame Layout (96 bytes, 16-byte aligned).
 * Must match arch/xtensa/syscall_handler.S offsets 0x00..0x5F exactly.
 */
typedef struct trap_frame {
    uint32_t a0;           /* 0x00 */
    uint32_t a1;           /* 0x04 (original caller sp) */
    uint32_t a2;           /* 0x08 */
    uint32_t a3;           /* 0x0C */
    uint32_t a4;           /* 0x10 */
    uint32_t a5;           /* 0x14 */
    uint32_t a6;           /* 0x18 */
    uint32_t a7;           /* 0x1C */
    uint32_t a8;           /* 0x20 */
    uint32_t a9;           /* 0x24 */
    uint32_t a10;          /* 0x28 */
    uint32_t a11;          /* 0x2C */
    uint32_t a12;          /* 0x30 */
    uint32_t a13;          /* 0x34 */
    uint32_t a14;          /* 0x38 */
    uint32_t a15;          /* 0x3C */
    uint32_t epc1;         /* 0x40 */
    uint32_t ps;           /* 0x44 */
    uint32_t exccause;     /* 0x48 */
    uint32_t excvaddr;     /* 0x4C */
    uint32_t caller_world; /* 0x50 */
    uint32_t reserved[3];  /* 0x54..0x5F */
} trap_frame_t;

_Static_assert(sizeof(trap_frame_t) == 96, "trap_frame_t must be 96 bytes");

typedef struct panic_report {
    bool         valid;
    uint32_t     panic_seq;
    uint64_t     tick;
    uint32_t     core_id;
    uint32_t     exccause;
    uint32_t     epc1;
    uint32_t     excvaddr;
    uint32_t     ps;
    uint32_t     caller_world;
    uint32_t     task_id;
    char         task_name[TASK_NAME_MAX_LEN];
    bool         stack_canary_ok;
    bool         has_regs;
    trap_frame_t regs;
    uint32_t     backtrace_count;
    uintptr_t    backtrace[PANIC_MAX_BACKTRACE_FRAMES];
    char         reason[PANIC_REASON_MAX_LEN];
} panic_report_t;

const char *panic_exccause_str(uint32_t exccause);

uint32_t panic_backtrace_call0(uintptr_t sp,
                               uintptr_t stack_low,
                               uintptr_t stack_high,
                               uintptr_t code_low,
                               uintptr_t code_high,
                               uintptr_t *out_pcs,
                               uint32_t max_frames);

void panic_dump_exception(uint32_t exccause,
                          uint32_t epc1,
                          uint32_t excvaddr,
                          const trap_frame_t *regs);

#ifndef RADIX_HOST_TEST
void kpanic(const char *reason) __attribute__((noreturn));
#else
void kpanic(const char *reason);
#endif

const panic_report_t *panic_get_last_report(void);
uint32_t              panic_get_count(void);
void                  panic_reset_stats(void);

#endif /* RADIX_PANIC_H */
