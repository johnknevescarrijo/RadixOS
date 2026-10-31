#include <radix/sched.h>
#include <radix/hal.h>
#include <radix/kprintf.h>

static tcb_t s_tasks[MAX_TASKS];
static int   s_current_task_idx = -1;
static uint32_t s_next_task_id = 1;

static uint8_t s_idle_stack[IDLE_TASK_STACK_SIZE] __attribute__((aligned(16)));

/* Idle task implementation */
static void idle_task_entry(void *arg) {
    (void)arg;
    while (1) {
#ifndef RADIX_HOST_TEST
        __asm__ volatile ("waiti 0");
#else
        sched_yield();
#endif
    }
}

void task_exit(void) {
    if (s_current_task_idx >= 0) {
        kprintf("[SCHED] Task '%s' (id=%u) exited.\n",
                s_tasks[s_current_task_idx].name,
                (unsigned int)s_tasks[s_current_task_idx].task_id);
        s_tasks[s_current_task_idx].state = TASK_STATE_SUSPENDED;
    }
    sched_yield();

    /* Unreachable safeguard */
    while (1) {}
}

void sched_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        s_tasks[i].state = TASK_STATE_UNUSED;
        s_tasks[i].task_id = 0;
        s_tasks[i].ticks_run = 0;
    }
    s_current_task_idx = -1;
    s_next_task_id = 1;

    /* Create the idle task at lowest priority 0 */
    int idle_id = task_create("idle_task", idle_task_entry, NULL, 0,
                              s_idle_stack, sizeof(s_idle_stack));
    (void)idle_id;

    kprintf("[SCHED] Initialized pre-emptive nano-kernel scheduler (Max Tasks: %d)\n", MAX_TASKS);
}

int task_create(const char *name, task_entry_t entry, void *arg, uint32_t priority,
                void *stack_mem, uint32_t stack_size) {
    if (!name || !entry || !stack_mem || stack_size < sizeof(cpu_context_t) + 64) {
        return -1;
    }

    int free_slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (s_tasks[i].state == TASK_STATE_UNUSED) {
            free_slot = i;
            break;
        }
    }

    if (free_slot == -1) {
        return -2; /* Out of task slots */
    }

    tcb_t *task = &s_tasks[free_slot];
    task->task_id = (priority == 0) ? 0 : s_next_task_id++;
    task->priority = priority;
    task->state = TASK_STATE_READY;
    task->stack_base = stack_mem;
    task->stack_size = stack_size;
    task->ticks_run = 0;
    task->slice_remaining = DEFAULT_TIME_SLICE_TICKS;

    /* Copy name safely */
    size_t n = 0;
    while (name[n] != '\0' && n < TASK_NAME_MAX_LEN - 1) {
        task->name[n] = name[n];
        n++;
    }
    task->name[n] = '\0';

    /* Prepare initial stack frame */
    uintptr_t stack_top = ((uintptr_t)stack_mem + stack_size) & ~15UL;
    cpu_context_t *ctx = (cpu_context_t *)(stack_top - sizeof(cpu_context_t));

    /* Initialize context fields */
    ctx->pc = (uint32_t)(uintptr_t)entry;
    ctx->ps = 0x00000000;               /* Call0 ABI: ring 0, interrupts enabled */
    ctx->a0 = (uint32_t)(uintptr_t)task_exit; /* Return hook */
    ctx->a2 = (uint32_t)(uintptr_t)arg;       /* First function parameter */
    ctx->a3 = 0;
    ctx->a4 = 0;
    ctx->a5 = 0;
    ctx->a6 = 0;
    ctx->a7 = 0;
    ctx->a8 = 0;
    ctx->a9 = 0;
    ctx->a10 = 0;
    ctx->a11 = 0;
    ctx->a12 = 0;
    ctx->a13 = 0;
    ctx->a14 = 0;
    ctx->a15 = 0;

    task->sp = ctx;

    kprintf("[TASK] Created task '%s' (id=%u, prio=%u, sp=%p, stack=%u B)\n",
            task->name, (unsigned int)task->task_id, (unsigned int)task->priority,
            (void *)task->sp, (unsigned int)task->stack_size);

    return (int)task->task_id;
}

static int sched_select_next_task(void) {
    uint32_t highest_prio = 0;
    bool found_ready = false;

    /* 1. Identify the highest active priority among ready/running tasks */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (s_tasks[i].state == TASK_STATE_READY || s_tasks[i].state == TASK_STATE_RUNNING) {
            if (!found_ready || s_tasks[i].priority > highest_prio) {
                highest_prio = s_tasks[i].priority;
                found_ready = true;
            }
        }
    }

    if (!found_ready) {
        return 0; /* Fallback to idle task slot */
    }

    /* 2. Round-Robin among ready tasks sharing highest priority */
    int start_idx = (s_current_task_idx >= 0) ? (s_current_task_idx + 1) : 0;
    for (int count = 0; count < MAX_TASKS; count++) {
        int idx = (start_idx + count) % MAX_TASKS;
        if (s_tasks[idx].priority == highest_prio &&
            (s_tasks[idx].state == TASK_STATE_READY || s_tasks[idx].state == TASK_STATE_RUNNING)) {
            return idx;
        }
    }

    return 0;
}

tcb_t *sched_get_current_task(void) {
    if (s_current_task_idx >= 0 && s_current_task_idx < MAX_TASKS) {
        return &s_tasks[s_current_task_idx];
    }
    return NULL;
}

#ifdef RADIX_HOST_TEST
/* Host simulation stubs for context switch */
void arch_context_switch(cpu_context_t **prev_ctx, cpu_context_t *next_ctx) {
    if (prev_ctx && s_current_task_idx >= 0) {
        *prev_ctx = s_tasks[s_current_task_idx].sp;
    }
    (void)next_ctx;
}

void arch_start_first_task(cpu_context_t *first_ctx) {
    (void)first_ctx;
    /* In host testing mode, control is driven by test harness */
}
#endif

void sched_yield(void) {
    int next_idx = sched_select_next_task();

    if (next_idx == s_current_task_idx && s_tasks[next_idx].state == TASK_STATE_RUNNING) {
        /* Task continues running */
        return;
    }

    int prev_idx = s_current_task_idx;
    tcb_t *prev_task = (prev_idx >= 0) ? &s_tasks[prev_idx] : NULL;
    tcb_t *next_task = &s_tasks[next_idx];

    if (prev_task && prev_task->state == TASK_STATE_RUNNING) {
        prev_task->state = TASK_STATE_READY;
    }

    next_task->state = TASK_STATE_RUNNING;
    next_task->slice_remaining = DEFAULT_TIME_SLICE_TICKS;
    s_current_task_idx = next_idx;

    if (prev_task) {
        arch_context_switch(&prev_task->sp, next_task->sp);
    } else {
        arch_start_first_task(next_task->sp);
    }
}

void sched_tick(void) {
    if (s_current_task_idx < 0) {
        return;
    }

    tcb_t *curr = &s_tasks[s_current_task_idx];
    curr->ticks_run++;

    if (curr->slice_remaining > 0) {
        curr->slice_remaining--;
    }

    if (curr->slice_remaining == 0) {
        sched_yield();
    }
}

void sched_start(void) {
    kprintf("[SCHED] Starting preemptive scheduling...\n");
    int first_idx = sched_select_next_task();
    s_current_task_idx = first_idx;
    s_tasks[first_idx].state = TASK_STATE_RUNNING;
    s_tasks[first_idx].slice_remaining = DEFAULT_TIME_SLICE_TICKS;

    kprintf("[SCHED] Dispatched initial task '%s' (id=%u)\n",
            s_tasks[first_idx].name, (unsigned int)s_tasks[first_idx].task_id);

    arch_start_first_task(s_tasks[first_idx].sp);

#ifndef RADIX_HOST_TEST
    /* Unreachable */
    while (1) {}
#endif
}

void sched_block_current(void) {
    tcb_t *curr = sched_get_current_task();
    if (curr) {
        curr->state = TASK_STATE_BLOCKED;
    }
    sched_yield();
}

void sched_block_current_task(void) {
    sched_block_current();
}

void sched_unblock(tcb_t *task) {
    if (task && task->state == TASK_STATE_BLOCKED) {
        task->state = TASK_STATE_READY;
    }
}

void sched_unblock_task(tcb_t *task) {
    sched_unblock(task);
}
