#ifndef RADIX_TASK_H
#define RADIX_TASK_H

#include <radix/types.h>
#include <radix/world.h>

#define TASK_NAME_MAX_LEN       16
#define TASK_STACK_MIN_SIZE     1024
#define TASK_STACK_CANARY_SIZE  16u
#define TASK_STACK_CANARY_MAGIC 0xDEADBEEFu
#define TASK_STACK_PAINT_BYTE   0xA5u

/* Core Affinity & SMP Constants */
#ifndef RADIX_CORE_ANY
#define RADIX_CORE_ANY          (-1)
#endif
#ifndef RADIX_CORE_0
#define RADIX_CORE_0            0u
#endif
#ifndef RADIX_CORE_1
#define RADIX_CORE_1            1u
#endif
#ifndef RADIX_NUM_CORES
#define RADIX_NUM_CORES         2u
#endif

struct mutex;

/* Wait-object types for blocked tasks */
typedef enum {
    TASK_WAIT_NONE = 0,
    TASK_WAIT_MUTEX,
    TASK_WAIT_SEM
} task_wait_type_t;

/* Task execution states */
typedef enum {
    TASK_STATE_UNUSED = 0,
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_BLOCKED,
    TASK_STATE_SLEEPING,
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

/* Task PMS memory profile */
typedef struct pms_task_profile {
    uintptr_t stack_base;
    size_t    stack_size;
    uintptr_t user_data_base;
    size_t    user_data_size;
} pms_task_profile_t;

/* Task Control Block (TCB) */
typedef struct tcb {
    uint32_t           task_id;
    char               name[TASK_NAME_MAX_LEN];
    uint32_t           priority;
    uint8_t            base_priority;   /* Original priority before PI boost */
    int8_t             core_affinity;   /* RADIX_CORE_ANY (-1), RADIX_CORE_0 (0), RADIX_CORE_1 (1) */
    task_state_t       state;
    cpu_context_t     *sp;              /* Saved stack pointer pointing to cpu_context */
    void              *stack_base;      /* Base of stack memory block */
    uint32_t           stack_size;      /* Size of stack in bytes */
    uint32_t           ticks_run;       /* Cumulative ticks spent running */
    uint32_t           slice_remaining; /* Remaining ticks in current time slice */
    uint64_t           wake_tick;       /* Absolute tick at which a sleeping task wakes */
    world_id_t         world;           /* Execution World (WORLD_0 or WORLD_1) */
    pms_task_profile_t pms;             /* Per-task PMS protection profile */
    task_wait_type_t   wait_type;       /* Sync primitive type if TASK_STATE_BLOCKED */
    void              *wait_obj;        /* Pointer to mutex_t or semaphore_t if blocked */
    struct mutex      *owned_mutexes;   /* Intrusive list of currently owned mutexes */
    bool               started;         /* True once initial context dispatch has occurred */
} tcb_t;

#endif /* RADIX_TASK_H */
