#ifndef RADIX_TASK_H
#define RADIX_TASK_H

#include <radix/types.h>

#define TASK_NAME_MAX_LEN       16
#define TASK_STACK_MIN_SIZE     1024

/* Task execution states */
typedef enum {
    TASK_STATE_UNUSED = 0,
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_BLOCKED,
    TASK_STATE_SUSPENDED
} task_state_t;

/* Xtensa LX7 Stack Frame Context (Call0 ABI) */
typedef struct cpu_context {
    uint32_t pc;       /* Task program counter / entry */
    uint32_t ps;       /* Processor status register */
    uint32_t a0;       /* Return address (points to task_exit) */
    uint32_t a2;       /* First argument to task function */
    uint32_t a3;
    uint32_t a4;
    uint32_t a5;
    uint32_t a6;
    uint32_t a7;
    uint32_t a8;
    uint32_t a9;
    uint32_t a10;
    uint32_t a11;
    uint32_t a12;      /* Callee-saved */
    uint32_t a13;      /* Callee-saved */
    uint32_t a14;      /* Callee-saved */
    uint32_t a15;      /* Callee-saved */
} cpu_context_t;

/* Task function entry prototype */
typedef void (*task_entry_t)(void *arg);

/* Task Control Block (TCB) */
typedef struct tcb {
    uint32_t        task_id;
    char            name[TASK_NAME_MAX_LEN];
    uint32_t        priority;
    task_state_t    state;
    cpu_context_t  *sp;              /* Saved stack pointer pointing to cpu_context */
    void           *stack_base;      /* Base of stack memory block */
    uint32_t        stack_size;      /* Size of stack in bytes */
    uint32_t        ticks_run;       /* Cumulative ticks spent running */
    uint32_t        slice_remaining; /* Remaining ticks in current time slice */
} tcb_t;

#endif /* RADIX_TASK_H */
