#include <radix/sched.h>
#include <radix/sync.h>
#include <radix/timer.h>
#include <radix/hal.h>
#include <radix/atomic.h>
#include <radix/kprintf.h>
#include <radix/pms.h>
#include <xtensa_regs.h>

#ifdef RADIX_HOST_TEST
extern uint32_t arch_get_core_id(void) __attribute__((weak));
#else
#include <radix/amp.h>
#endif

static inline uint32_t sched_current_core(void) {
#ifdef RADIX_HOST_TEST
    uint32_t core_id = arch_get_core_id ? arch_get_core_id() : 0u;
#else
    uint32_t core_id = arch_get_core_id();
#endif
    return (core_id < RADIX_NUM_CORES) ? core_id : 0u;
}

static tcb_t s_tasks[MAX_TASKS];
static int   s_current_task_idx[RADIX_NUM_CORES] = { -1, -1 };
static tcb_t *s_current_task[RADIX_NUM_CORES] = { NULL, NULL };
static uint32_t s_next_task_id = 1;
static volatile uint32_t s_isr_depth[RADIX_NUM_CORES] = { 0u, 0u };
static volatile bool s_need_resched[RADIX_NUM_CORES] = { false, false };
static volatile uint64_t s_total_ticks = 0;
static uint32_t s_stack_overflow_count = 0;
static spinlock_t s_sched_lock = SPINLOCK_INIT;

static uint8_t s_idle_stacks[RADIX_NUM_CORES][IDLE_TASK_STACK_SIZE] __attribute__((aligned(16)));

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

void sched_reset_core_state(void) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_sched_lock, &flags);
    for (uint32_t c = 0u; c < RADIX_NUM_CORES; c++) {
        if (s_current_task[c] && s_current_task[c]->state == TASK_STATE_RUNNING) {
            s_current_task[c]->state = TASK_STATE_READY;
        }
        s_current_task_idx[c] = -1;
        s_current_task[c] = NULL;
        s_isr_depth[c] = 0u;
        s_need_resched[c] = false;
    }
    spinlock_release_irqrestore(&s_sched_lock, flags);
}

void task_exit(void) {
    uint32_t core_id = sched_current_core();
    tcb_t *curr = s_current_task[core_id];
    if (curr) {
        kprintf("[SCHED] Task '%s' (id=%u) exited.\n",
                curr->name,
                (unsigned int)curr->task_id);
        curr->state = TASK_STATE_SUSPENDED;
        sync_on_task_killed(curr);
    }
    sched_yield();

#ifndef RADIX_HOST_TEST
    /* Unreachable safeguard */
    while (1) {}
#endif
}

void sched_init(void) {
    spinlock_init(&s_sched_lock);
    for (int i = 0; i < MAX_TASKS; i++) {
        s_tasks[i].state = TASK_STATE_UNUSED;
        s_tasks[i].task_id = 0;
        s_tasks[i].priority = 0;
        s_tasks[i].base_priority = 0;
        s_tasks[i].core_affinity = RADIX_CORE_ANY;
        s_tasks[i].ticks_run = 0;
        s_tasks[i].wake_tick = 0;
        s_tasks[i].world = WORLD_0;
        s_tasks[i].wait_type = TASK_WAIT_NONE;
        s_tasks[i].wait_obj = NULL;
        s_tasks[i].owned_mutexes = NULL;
        s_tasks[i].started = false;
        s_tasks[i].pms.stack_base = 0;
        s_tasks[i].pms.stack_size = 0;
        s_tasks[i].pms.user_data_base = 0;
        s_tasks[i].pms.user_data_size = 0;
    }
    sched_reset_core_state();
    s_next_task_id = 1;
    s_total_ticks = 0;
    s_stack_overflow_count = 0;
    ktimer_reset_all();

    /* Create per-core idle tasks at lowest priority 0 */
    int idle0 = task_create_pinned("idle_task", idle_task_entry, NULL, 0,
                                   s_idle_stacks[RADIX_CORE_0], IDLE_TASK_STACK_SIZE,
                                   WORLD_0, (int8_t)RADIX_CORE_0);
    int idle1 = task_create_pinned("idle_core1", idle_task_entry, NULL, 0,
                                   s_idle_stacks[RADIX_CORE_1], IDLE_TASK_STACK_SIZE,
                                   WORLD_0, (int8_t)RADIX_CORE_1);
    (void)idle0;
    (void)idle1;

    kprintf("[SCHED] Initialized pre-emptive multi-core scheduler (Cores: %u, Max Tasks: %d)\n",
            (unsigned int)RADIX_NUM_CORES, MAX_TASKS);
}

int task_create_pinned(const char *name, task_entry_t entry, void *arg, uint32_t priority,
                       void *stack_mem, uint32_t stack_size, world_id_t world, int8_t core_affinity) {
    if (!name || !entry || !stack_mem || stack_size < sizeof(cpu_context_t) + 64) {
        return -1;
    }
    if (core_affinity != RADIX_CORE_ANY &&
        (core_affinity < 0 || core_affinity >= (int8_t)RADIX_NUM_CORES)) {
        return -1;
    }

    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_sched_lock, &flags);

    int free_slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (s_tasks[i].state == TASK_STATE_UNUSED) {
            free_slot = i;
            break;
        }
    }

    if (free_slot == -1) {
        spinlock_release_irqrestore(&s_sched_lock, flags);
        return -2; /* Out of task slots */
    }

    tcb_t *task = &s_tasks[free_slot];
    task->task_id = (priority == 0) ? 0 : s_next_task_id++;
    task->priority = priority;
    task->base_priority = (priority > 255u) ? 255u : (uint8_t)priority;
    task->core_affinity = core_affinity;
    task->state = TASK_STATE_READY;
    task->stack_base = stack_mem;
    task->stack_size = stack_size;
    task->ticks_run = 0;
    task->slice_remaining = DEFAULT_TIME_SLICE_TICKS;
    task->wake_tick = 0;
    task->world = (world == WORLD_1) ? WORLD_1 : WORLD_0;
    task->wait_type = TASK_WAIT_NONE;
    task->wait_obj = NULL;
    task->owned_mutexes = NULL;
    task->started = false;
    task->pms.stack_base = (uintptr_t)stack_mem;
    task->pms.stack_size = (size_t)stack_size;
    task->pms.user_data_base = 0;
    task->pms.user_data_size = 0;

    /* Copy name safely */
    size_t n = 0;
    while (name[n] != '\0' && n < TASK_NAME_MAX_LEN - 1) {
        task->name[n] = name[n];
        n++;
    }
    task->name[n] = '\0';

    /* Paint stack free area and place bottom 16-byte canary guard */
    uint8_t *stack_bytes = (uint8_t *)stack_mem;
    for (uint32_t i = 0; i < stack_size; i++) {
        stack_bytes[i] = TASK_STACK_PAINT_BYTE;
    }
    for (uint32_t i = 0; i < TASK_STACK_CANARY_SIZE; i++) {
        stack_bytes[i] = (uint8_t)(TASK_STACK_CANARY_MAGIC >> ((i & 3u) * 8u));
    }

    /* Prepare initial stack frame */
    uintptr_t stack_top = ((uintptr_t)stack_mem + stack_size) & ~15UL;
    cpu_context_t *ctx = (cpu_context_t *)(stack_top - sizeof(cpu_context_t));

    /* Initialize context fields */
    ctx->pc = (uint32_t)(uintptr_t)entry;
    /* Call0 ABI: WOE=0, INTLEVEL=0; PS.UM=1 for World 1 tasks to route traps via _user_exception_vector */
    ctx->ps = (task->world == WORLD_1) ? PS_UM : 0x00000000u;
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

    spinlock_release_irqrestore(&s_sched_lock, flags);

    kprintf("[TASK] Created task '%s' (id=%u, prio=%u, world=%u, core=%d, sp=%p, stack=%u B)\n",
            task->name, (unsigned int)task->task_id, (unsigned int)task->priority,
            (unsigned int)task->world, (int)task->core_affinity,
            (void *)task->sp, (unsigned int)task->stack_size);

    return (int)task->task_id;
}

int task_create_in_world(const char *name, task_entry_t entry, void *arg, uint32_t priority,
                         void *stack_mem, uint32_t stack_size, world_id_t world) {
    return task_create_pinned(name, entry, arg, priority, stack_mem, stack_size, world, RADIX_CORE_ANY);
}

int task_create(const char *name, task_entry_t entry, void *arg, uint32_t priority,
                void *stack_mem, uint32_t stack_size) {
    return task_create_pinned(name, entry, arg, priority, stack_mem, stack_size, WORLD_0, RADIX_CORE_ANY);
}

bool task_set_user_region(uint32_t task_id, void *base, size_t size) {
    uint32_t core_id = sched_current_core();
    for (int i = 0; i < MAX_TASKS; i++) {
        if (s_tasks[i].state != TASK_STATE_UNUSED && s_tasks[i].task_id == task_id) {
            s_tasks[i].pms.user_data_base = (uintptr_t)base;
            s_tasks[i].pms.user_data_size = size;
            if (i == s_current_task_idx[core_id]) {
                pms_apply_task_isolation(&s_tasks[i]);
            }
            return true;
        }
    }
    return false;
}

static bool sched_is_task_eligible_for_core(int idx, uint32_t core_id) {
    const tcb_t *t = &s_tasks[idx];
    if (t->core_affinity != RADIX_CORE_ANY && t->core_affinity != (int8_t)core_id) {
        return false;
    }

    for (uint32_t c = 0u; c < RADIX_NUM_CORES; c++) {
        if (c != core_id && s_current_task[c] == t) {
            return false;
        }
    }

    if (t->state == TASK_STATE_READY) {
        return true;
    }
    if (t->state == TASK_STATE_RUNNING && s_current_task_idx[core_id] == idx) {
        return true;
    }
    return false;
}

static int sched_select_next_task_for_core(uint32_t core_id) {
    uint32_t highest_prio = 0;
    bool found_ready = false;

    /* 1. Identify highest active priority among eligible tasks for core_id */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (sched_is_task_eligible_for_core(i, core_id)) {
            if (!found_ready || s_tasks[i].priority > highest_prio) {
                highest_prio = s_tasks[i].priority;
                found_ready = true;
            }
        }
    }

    if (!found_ready) {
        return (int)core_id; /* Fallback to per-core idle task slot */
    }

    /* 2. Round-Robin among eligible tasks sharing highest priority */
    int cur_idx = s_current_task_idx[core_id];
    int start_idx = (cur_idx >= 0) ? (cur_idx + 1) : 0;
    for (int count = 0; count < MAX_TASKS; count++) {
        int idx = (start_idx + count) % MAX_TASKS;
        if (s_tasks[idx].priority == highest_prio && sched_is_task_eligible_for_core(idx, core_id)) {
            return idx;
        }
    }

    return (int)core_id;
}

tcb_t *sched_get_current_task(void) {
    return s_current_task[sched_current_core()];
}

tcb_t *sched_get_current_task_on_core(uint32_t core_id) {
    if (core_id >= RADIX_NUM_CORES) {
        return NULL;
    }
    return s_current_task[core_id];
}

#ifdef RADIX_HOST_TEST
/* Host simulation stubs for context switch */
void arch_context_switch(cpu_context_t **prev_ctx, cpu_context_t *next_ctx) {
    uint32_t core_id = sched_current_core();
    if (prev_ctx && s_current_task_idx[core_id] >= 0) {
        *prev_ctx = s_tasks[s_current_task_idx[core_id]].sp;
    }
    (void)next_ctx;
}

void arch_start_first_task(cpu_context_t *first_ctx) {
    (void)first_ctx;
    /* In host testing mode, control is driven by test harness */
}
#endif

void sched_enter_isr(void) {
    uint32_t core_id = sched_current_core();
    s_isr_depth[core_id]++;
}

void sched_exit_isr(void) {
    uint32_t core_id = sched_current_core();
    if (s_isr_depth[core_id] > 0u) {
        s_isr_depth[core_id]--;
    }
}

bool sched_in_isr(void) {
    return (s_isr_depth[sched_current_core()] > 0u);
}

void sched_set_need_resched(void) {
    s_need_resched[sched_current_core()] = true;
}

bool sched_need_resched(void) {
    return s_need_resched[sched_current_core()];
}

bool sched_consume_need_resched(void) {
    uint32_t core_id = sched_current_core();
    if (s_isr_depth[core_id] > 0u || !s_need_resched[core_id]) {
        return false;
    }
    s_need_resched[core_id] = false;
    return true;
}

void sched_yield(void) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_sched_lock, &flags);

    uint32_t core_id = sched_current_core();
    int prev_idx = s_current_task_idx[core_id];
    tcb_t *prev_task = (prev_idx >= 0) ? &s_tasks[prev_idx] : NULL;

    if (prev_task &&
        prev_task->state != TASK_STATE_UNUSED &&
        prev_task->state != TASK_STATE_SUSPENDED) {
        if (!task_check_stack_canary(prev_task)) {
            s_stack_overflow_count++;
            kprintf("[SCHED] ERROR: Stack canary corrupted in task '%s' (id=%u)!\n",
                    prev_task->name, (unsigned int)prev_task->task_id);
            if (prev_task->task_id != 0u) {
                prev_task->state = TASK_STATE_SUSPENDED;
            }
        }
    }

    int next_idx = sched_select_next_task_for_core(core_id);

    if (next_idx == prev_idx && s_tasks[next_idx].state == TASK_STATE_RUNNING) {
        /* Task continues running; replenish slice if exhausted */
        if (s_tasks[next_idx].slice_remaining == 0) {
            s_tasks[next_idx].slice_remaining = DEFAULT_TIME_SLICE_TICKS;
        }
        spinlock_release_irqrestore(&s_sched_lock, flags);
        return;
    }

    tcb_t *next_task = &s_tasks[next_idx];

    if (prev_task && prev_task->state == TASK_STATE_RUNNING) {
        prev_task->state = TASK_STATE_READY;
    }

    next_task->state = TASK_STATE_RUNNING;
    next_task->slice_remaining = DEFAULT_TIME_SLICE_TICKS;
    s_current_task_idx[core_id] = next_idx;
    s_current_task[core_id] = next_task;

    /* Apply per-task PMS isolation and update active World domain */
    pms_apply_task_isolation(next_task);

#ifdef RADIX_HOST_TEST
    next_task->started = true;
    if (next_task->world == WORLD_1) {
        world_switch_to_user((uintptr_t)next_task->sp->pc);
    } else {
        world_set_current(WORLD_0);
    }
#else
    if (!next_task->started) {
        next_task->started = true;
        if (next_task->world == WORLD_1) {
            world_switch_to_user((uintptr_t)next_task->sp->pc);
        } else {
            world_set_current(WORLD_0);
        }
    } else {
        /* Resumed task wakes in kernel trap/ISR frame; RFE restores its World */
        world_set_current(WORLD_0);
    }
#endif

    spinlock_release_irqrestore(&s_sched_lock, flags);

    if (prev_task) {
        arch_context_switch(&prev_task->sp, next_task->sp);
    } else {
        arch_start_first_task(next_task->sp);
    }
}

uint64_t sched_get_ticks(void) {
    uint32_t flags = arch_irq_save();
    uint64_t ticks = s_total_ticks;
    arch_irq_restore(flags);
    return ticks;
}

void task_sleep_ticks(uint64_t ticks) {
    if (ticks == 0u) {
        if (!sched_in_isr()) {
            sched_yield();
        }
        return;
    }

    uint32_t flags = arch_irq_save();
    tcb_t *curr = sched_get_current_task();
    if (!curr || curr->task_id == 0u || sched_in_isr()) {
        arch_irq_restore(flags);
        return;
    }

    curr->wake_tick = s_total_ticks + ticks;
    curr->state = TASK_STATE_SLEEPING;
    arch_irq_restore(flags);

    sched_yield();
}

void task_sleep_ms(uint32_t ms) {
    if (ms == 0u) {
        if (!sched_in_isr()) {
            sched_yield();
        }
        return;
    }

    uint64_t ticks = ((uint64_t)ms + RADIX_TICK_MS - 1u) / RADIX_TICK_MS;
    if (ticks == 0u) {
        ticks = 1u;
    }
    task_sleep_ticks(ticks);
}

void sched_tick(void) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_sched_lock, &flags);

    uint32_t core_id = sched_current_core();
    s_total_ticks++;
    uint64_t now = s_total_ticks;

    tcb_t *curr = s_current_task[core_id];
    bool woke_higher_prio = false;

    for (int i = 0; i < MAX_TASKS; i++) {
        if (s_tasks[i].state == TASK_STATE_SLEEPING && s_tasks[i].wake_tick <= now) {
            s_tasks[i].state = TASK_STATE_READY;
            if (curr && s_tasks[i].priority > curr->priority &&
                (s_tasks[i].core_affinity == RADIX_CORE_ANY ||
                 s_tasks[i].core_affinity == (int8_t)core_id)) {
                woke_higher_prio = true;
            }
        }
    }

    bool need_resched = woke_higher_prio;
    if (curr) {
        curr->ticks_run++;

        if (curr->slice_remaining > 0) {
            curr->slice_remaining--;
        }
        if (curr->slice_remaining == 0) {
            need_resched = true;
        }
    }

    spinlock_release_irqrestore(&s_sched_lock, flags);

    ktimer_tick(now);

    if (need_resched) {
        if (sched_in_isr()) {
            sched_set_need_resched();
        } else if (curr) {
            sched_yield();
        }
    }
}

void sched_start(void) {
    kprintf("[SCHED] Starting preemptive scheduling...\n");
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_sched_lock, &flags);

    uint32_t core_id = sched_current_core();
    int first_idx = sched_select_next_task_for_core(core_id);
    s_current_task_idx[core_id] = first_idx;
    s_current_task[core_id] = &s_tasks[first_idx];
    s_tasks[first_idx].state = TASK_STATE_RUNNING;
    s_tasks[first_idx].slice_remaining = DEFAULT_TIME_SLICE_TICKS;
    s_tasks[first_idx].started = true;

    pms_apply_task_isolation(&s_tasks[first_idx]);
    if (s_tasks[first_idx].world == WORLD_1) {
        world_switch_to_user((uintptr_t)s_tasks[first_idx].sp->pc);
    } else {
        world_set_current(WORLD_0);
    }

    spinlock_release_irqrestore(&s_sched_lock, flags);

    kprintf("[SCHED] Dispatched initial task '%s' (id=%u, core=%u)\n",
            s_tasks[first_idx].name, (unsigned int)s_tasks[first_idx].task_id, (unsigned int)core_id);

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

const tcb_t *sched_get_task_by_index(uint32_t index) {
    if (index >= MAX_TASKS) {
        return NULL;
    }
    return &s_tasks[index];
}

bool sched_kill_task(uint32_t task_id) {
    if (task_id == 0) {
        /* Idle task is protected */
        return false;
    }

    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_sched_lock, &flags);

    tcb_t *target = NULL;
    int target_idx = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (s_tasks[i].state != TASK_STATE_UNUSED && s_tasks[i].task_id == task_id) {
            target = &s_tasks[i];
            target_idx = i;
            target->state = TASK_STATE_SUSPENDED;
            break;
        }
    }

    if (!target) {
        spinlock_release_irqrestore(&s_sched_lock, flags);
        return false;
    }

    spinlock_release_irqrestore(&s_sched_lock, flags);

    sync_on_task_killed(target);

    spinlock_acquire_irqsave(&s_sched_lock, &flags);
    uint32_t core_id = sched_current_core();
    bool was_current = (target_idx == s_current_task_idx[core_id]);
    for (uint32_t c = 0u; c < RADIX_NUM_CORES; c++) {
        if (c != core_id && s_current_task_idx[c] == target_idx) {
            s_need_resched[c] = true;
        }
    }
    target->state = TASK_STATE_UNUSED;
    spinlock_release_irqrestore(&s_sched_lock, flags);

    kprintf("[SCHED] Task '%s' (id=%u) killed.\n", target->name, (unsigned int)task_id);
    if (was_current) {
        sched_yield();
    }
    return true;
}

bool task_check_stack_canary(const tcb_t *task) {
    if (!task || !task->stack_base || task->stack_size < TASK_STACK_CANARY_SIZE) {
        return false;
    }

    const uint8_t *stack_bytes = (const uint8_t *)task->stack_base;
    for (uint32_t i = 0; i < TASK_STACK_CANARY_SIZE; i++) {
        uint8_t expected = (uint8_t)(TASK_STACK_CANARY_MAGIC >> ((i & 3u) * 8u));
        if (stack_bytes[i] != expected) {
            return false;
        }
    }
    return true;
}

size_t task_stack_free_min_bytes(const tcb_t *task) {
    if (!task || !task->stack_base || task->stack_size <= TASK_STACK_CANARY_SIZE) {
        return 0u;
    }

    const uint8_t *stack_bytes = (const uint8_t *)task->stack_base;
    size_t i = TASK_STACK_CANARY_SIZE;
    while (i < (size_t)task->stack_size && stack_bytes[i] == TASK_STACK_PAINT_BYTE) {
        i++;
    }
    return (i - TASK_STACK_CANARY_SIZE);
}

size_t task_stack_high_water_mark(const tcb_t *task) {
    if (!task || !task->stack_base || task->stack_size <= TASK_STACK_CANARY_SIZE) {
        return 0u;
    }

    size_t free_min = task_stack_free_min_bytes(task);
    return (size_t)task->stack_size - free_min;
}

uint32_t sched_check_all_stacks(void) {
    uint32_t corrupted = 0u;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (s_tasks[i].state != TASK_STATE_UNUSED) {
            if (!task_check_stack_canary(&s_tasks[i])) {
                corrupted++;
            }
        }
    }
    return corrupted;
}

uint32_t sched_get_stack_overflow_count(void) {
    return s_stack_overflow_count;
}
