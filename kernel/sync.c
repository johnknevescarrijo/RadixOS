/*
 * kernel/sync.c - Synchronization Primitives for RadixOS
 *
 * Implements:
 *   - Hardware/Host Spinlocks with IRQ-safe variants
 *   - Blocking Mutex with Wait Queue
 *   - Counting Semaphore with Wait Queue
 */

#include <radix/sync.h>
#include <radix/sched.h>
#include <radix/atomic.h>

#ifdef RADIX_HOST_TEST
volatile uint32_t g_host_irq_flags = 0;
#endif

/* ========================================================================= */
/* Spinlock Implementation                                                   */
/* ========================================================================= */

void spinlock_init(spinlock_t *lock) {
    if (!lock) {
        return;
    }
    lock->locked = 0;
    arch_memory_barrier();
}

bool spinlock_try_acquire(spinlock_t *lock) {
    if (!lock) {
        return false;
    }
    return (arch_atomic_swap(&lock->locked, 1) == 0);
}

void spinlock_acquire(spinlock_t *lock) {
    if (!lock) {
        return;
    }
    while (arch_atomic_swap(&lock->locked, 1) != 0) {
        /* Spin until lock is released */
    }
}

void spinlock_release(spinlock_t *lock) {
    if (!lock) {
        return;
    }
    arch_memory_barrier();
    arch_atomic_swap(&lock->locked, 0);
}

void spinlock_acquire_irqsave(spinlock_t *lock, uint32_t *flags) {
    uint32_t saved = arch_irq_save();
    if (flags) {
        *flags = saved;
    }
    if (lock) {
        while (arch_atomic_swap(&lock->locked, 1) != 0) {
            /* Spin until lock is acquired */
        }
    }
}

void spinlock_release_irqrestore(spinlock_t *lock, uint32_t flags) {
    if (lock) {
        arch_memory_barrier();
        arch_atomic_swap(&lock->locked, 0);
    }
    arch_irq_restore(flags);
}

/* ========================================================================= */
/* Mutex Implementation (Priority Inheritance & Recursive Reentrancy)        */
/* ========================================================================= */

/* Global lock ordering: s_sync_pi_lock -> mutex->lock */
static spinlock_t s_sync_pi_lock = SPINLOCK_INIT;

static void mutex_list_add(tcb_t *owner, mutex_t *mutex) {
    if (!owner || !mutex) {
        return;
    }
    for (mutex_t *it = owner->owned_mutexes; it != NULL; it = it->next_owned) {
        if (it == mutex) {
            return;
        }
    }
    mutex->next_owned = owner->owned_mutexes;
    owner->owned_mutexes = mutex;
}

static void mutex_list_remove(tcb_t *owner, mutex_t *mutex) {
    if (!owner || !mutex) {
        return;
    }
    mutex_t **pp = &owner->owned_mutexes;
    while (*pp != NULL) {
        if (*pp == mutex) {
            *pp = mutex->next_owned;
            mutex->next_owned = NULL;
            return;
        }
        pp = &((*pp)->next_owned);
    }
}

static void mutex_enqueue(mutex_t *mutex, tcb_t *task) {
    if (!mutex || !task || mutex->count >= MAX_TASKS) {
        return;
    }
    /* Keep compact array [0 .. count-1] in FIFO arrival order */
    mutex->wait_queue[mutex->count] = task;
    mutex->count++;
    mutex->head = 0u;
    mutex->tail = (uint8_t)(mutex->count % MAX_TASKS);
}

static void mutex_remove_at(mutex_t *mutex, uint8_t idx) {
    if (!mutex || idx >= mutex->count) {
        return;
    }
    for (uint8_t i = idx; (uint8_t)(i + 1u) < mutex->count; i++) {
        mutex->wait_queue[i] = mutex->wait_queue[i + 1u];
    }
    mutex->count--;
    mutex->wait_queue[mutex->count] = NULL;
    mutex->head = 0u;
    mutex->tail = (uint8_t)(mutex->count % MAX_TASKS);
}

static bool mutex_remove_waiter(mutex_t *mutex, tcb_t *task) {
    if (!mutex || !task) {
        return false;
    }
    for (uint8_t i = 0u; i < mutex->count; i++) {
        if (mutex->wait_queue[i] == task) {
            mutex_remove_at(mutex, i);
            return true;
        }
    }
    return false;
}

static tcb_t *mutex_dequeue(mutex_t *mutex) {
    if (!mutex || mutex->count == 0u) {
        return NULL;
    }
    /* Select highest-priority waiter (strict '>' preserves FIFO order on ties) */
    uint8_t best_idx = 0u;
    for (uint8_t i = 1u; i < mutex->count; i++) {
        if (mutex->wait_queue[i] &&
            (!mutex->wait_queue[best_idx] ||
             mutex->wait_queue[i]->priority > mutex->wait_queue[best_idx]->priority)) {
            best_idx = i;
        }
    }
    tcb_t *task = mutex->wait_queue[best_idx];
    mutex_remove_at(mutex, best_idx);
    return task;
}

static uint32_t mutex_max_waiter_prio(const mutex_t *mutex) {
    uint32_t max_prio = 0u;
    for (uint8_t i = 0u; i < mutex->count; i++) {
        if (mutex->wait_queue[i] && mutex->wait_queue[i]->priority > max_prio) {
            max_prio = mutex->wait_queue[i]->priority;
        }
    }
    return max_prio;
}

static void mutex_recalc_owner_priority(tcb_t *owner, const mutex_t *already_locked) {
    if (!owner) {
        return;
    }
    uint32_t effective = (uint32_t)owner->base_priority;
    for (mutex_t *m = owner->owned_mutexes; m != NULL; m = m->next_owned) {
        uint32_t wprio = 0u;
        if (m == already_locked) {
            wprio = mutex_max_waiter_prio(m);
        } else {
            spinlock_acquire(&m->lock);
            wprio = mutex_max_waiter_prio(m);
            spinlock_release(&m->lock);
        }
        if (wprio > effective) {
            effective = wprio;
        }
    }
    owner->priority = effective;
}

static void mutex_propagate_pi(tcb_t *start_owner, uint32_t waiter_prio, const mutex_t *already_locked) {
    tcb_t *owner = start_owner;
    uint32_t depth = 0u;
    while (owner != NULL && depth < MAX_TASKS) {
        if (waiter_prio > owner->priority) {
            owner->priority = waiter_prio;
        }
        if (owner->state == TASK_STATE_BLOCKED &&
            owner->wait_type == TASK_WAIT_MUTEX &&
            owner->wait_obj != NULL) {
            mutex_t *next_m = (mutex_t *)owner->wait_obj;
            if (next_m == already_locked) {
                break;
            }
            spinlock_acquire(&next_m->lock);
            tcb_t *next_owner = next_m->owner;
            spinlock_release(&next_m->lock);
            owner = next_owner;
            depth++;
        } else {
            break;
        }
    }
}

void mutex_init(mutex_t *mutex) {
    if (!mutex) {
        return;
    }
    spinlock_init(&mutex->lock);
    mutex->locked = 0u;
    mutex->lock_count = 0u;
    mutex->owner = NULL;
    mutex->next_owned = NULL;
    mutex->head = 0u;
    mutex->tail = 0u;
    mutex->count = 0u;
    for (int i = 0; i < MAX_TASKS; i++) {
        mutex->wait_queue[i] = NULL;
    }
}

bool mutex_trylock(mutex_t *mutex) {
    if (!mutex) {
        return false;
    }
    uint32_t flags = 0;
    spinlock_acquire_irqsave(&s_sync_pi_lock, &flags);
    spinlock_acquire(&mutex->lock);

    if (!mutex->locked) {
        tcb_t *curr = sched_get_current_task();
        mutex->locked = 1u;
        mutex->lock_count = 1u;
        mutex->owner = curr;
        if (curr) {
            mutex_list_add(curr, mutex);
        }
        spinlock_release(&mutex->lock);
        spinlock_release_irqrestore(&s_sync_pi_lock, flags);
        return true;
    }

    spinlock_release(&mutex->lock);
    spinlock_release_irqrestore(&s_sync_pi_lock, flags);
    return false;
}

void mutex_lock(mutex_t *mutex) {
    if (!mutex) {
        return;
    }

    bool waiting_on_mutex = false;

    while (1) {
        uint32_t flags = 0;
        spinlock_acquire_irqsave(&s_sync_pi_lock, &flags);
        spinlock_acquire(&mutex->lock);

        tcb_t *curr = sched_get_current_task();
        if (!mutex->locked) {
            mutex->locked = 1u;
            mutex->lock_count = 1u;
            mutex->owner = curr;
            if (curr) {
                curr->wait_type = TASK_WAIT_NONE;
                curr->wait_obj = NULL;
                mutex_list_add(curr, mutex);
            }
            spinlock_release(&mutex->lock);
            spinlock_release_irqrestore(&s_sync_pi_lock, flags);
            return;
        } else if (curr != NULL && mutex->owner == curr) {
            /*
             * Either ownership was just transferred to us upon wakeup
             * (waiting_on_mutex == true, lock_count already 1), or this is
             * a recursive reentrant lock by the current owner.
             */
            if (!waiting_on_mutex) {
                mutex->lock_count++;
            }
            spinlock_release(&mutex->lock);
            spinlock_release_irqrestore(&s_sync_pi_lock, flags);
            return;
        }

        /* Mutex is owned by another task: block and apply Priority Inheritance */
        if (curr) {
            mutex_enqueue(mutex, curr);
            curr->state = TASK_STATE_BLOCKED;
            curr->wait_type = TASK_WAIT_MUTEX;
            curr->wait_obj = mutex;
            waiting_on_mutex = true;
            if (mutex->owner) {
                mutex_propagate_pi(mutex->owner, curr->priority, mutex);
            }
        }
        spinlock_release(&mutex->lock);
        spinlock_release_irqrestore(&s_sync_pi_lock, flags);

        sched_yield();
#ifdef RADIX_HOST_TEST
        if (curr && curr->state == TASK_STATE_BLOCKED) {
            return;
        }
#endif
    }
}

void mutex_unlock(mutex_t *mutex) {
    if (!mutex) {
        return;
    }
    uint32_t flags = 0;
    spinlock_acquire_irqsave(&s_sync_pi_lock, &flags);
    spinlock_acquire(&mutex->lock);

    if (!mutex->locked) {
        spinlock_release(&mutex->lock);
        spinlock_release_irqrestore(&s_sync_pi_lock, flags);
        return;
    }

    tcb_t *curr = sched_get_current_task();
    if (mutex->owner != NULL && curr != NULL && mutex->owner != curr) {
        /* Reject unlock attempt from non-owner */
        spinlock_release(&mutex->lock);
        spinlock_release_irqrestore(&s_sync_pi_lock, flags);
        return;
    }

    if (mutex->lock_count > 1u) {
        mutex->lock_count--;
        spinlock_release(&mutex->lock);
        spinlock_release_irqrestore(&s_sync_pi_lock, flags);
        return;
    }

    tcb_t *prev_owner = mutex->owner;
    if (prev_owner) {
        mutex_list_remove(prev_owner, mutex);
        mutex_recalc_owner_priority(prev_owner, mutex);
    }

    tcb_t *waiter = mutex_dequeue(mutex);
    bool in_isr = sched_in_isr();
    bool preempt = false;

    if (waiter) {
        mutex->locked = 1u;
        mutex->lock_count = 1u;
        mutex->owner = waiter;
        waiter->wait_type = TASK_WAIT_NONE;
        waiter->wait_obj = NULL;
        mutex_list_add(waiter, mutex);
        mutex_recalc_owner_priority(waiter, mutex);
        sched_unblock(waiter);
        if (!prev_owner || waiter->priority > prev_owner->priority) {
            preempt = true;
        }
    } else {
        mutex->locked = 0u;
        mutex->lock_count = 0u;
        mutex->owner = NULL;
    }

    spinlock_release(&mutex->lock);
    spinlock_release_irqrestore(&s_sync_pi_lock, flags);

    if (preempt) {
        if (in_isr) {
            sched_set_need_resched();
        } else {
            sched_yield();
        }
    }
}

/* ========================================================================= */
/* Semaphore Implementation                                                  */
/* ========================================================================= */

static void sem_enqueue(semaphore_t *sem, tcb_t *task) {
    if (!sem || !task || sem->waiters >= MAX_TASKS) {
        return;
    }
    sem->wait_queue[sem->waiters] = task;
    sem->waiters++;
    sem->head = 0u;
    sem->tail = (uint8_t)(sem->waiters % MAX_TASKS);
}

static void sem_remove_at(semaphore_t *sem, uint8_t idx) {
    if (!sem || idx >= sem->waiters) {
        return;
    }
    for (uint8_t i = idx; (uint8_t)(i + 1u) < sem->waiters; i++) {
        sem->wait_queue[i] = sem->wait_queue[i + 1u];
    }
    sem->waiters--;
    sem->wait_queue[sem->waiters] = NULL;
    sem->head = 0u;
    sem->tail = (uint8_t)(sem->waiters % MAX_TASKS);
}

static bool sem_remove_waiter(semaphore_t *sem, tcb_t *task) {
    if (!sem || !task) {
        return false;
    }
    for (uint8_t i = 0u; i < sem->waiters; i++) {
        if (sem->wait_queue[i] == task) {
            sem_remove_at(sem, i);
            return true;
        }
    }
    return false;
}

static tcb_t *sem_dequeue(semaphore_t *sem) {
    if (!sem || sem->waiters == 0u) {
        return NULL;
    }
    uint8_t best_idx = 0u;
    for (uint8_t i = 1u; i < sem->waiters; i++) {
        if (sem->wait_queue[i] &&
            (!sem->wait_queue[best_idx] ||
             sem->wait_queue[i]->priority > sem->wait_queue[best_idx]->priority)) {
            best_idx = i;
        }
    }
    tcb_t *task = sem->wait_queue[best_idx];
    sem_remove_at(sem, best_idx);
    return task;
}

void sem_init(semaphore_t *sem, uint32_t initial_count, uint32_t max_count) {
    if (!sem) {
        return;
    }
    spinlock_init(&sem->lock);
    sem->count = initial_count;
    sem->max_count = (max_count > 0) ? max_count : 1;
    sem->head = 0;
    sem->tail = 0;
    sem->waiters = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        sem->wait_queue[i] = NULL;
    }
}

bool sem_trywait(semaphore_t *sem) {
    if (!sem) {
        return false;
    }
    uint32_t flags = 0;
    spinlock_acquire_irqsave(&sem->lock, &flags);

    if (sem->count > 0) {
        sem->count--;
        spinlock_release_irqrestore(&sem->lock, flags);
        return true;
    }

    spinlock_release_irqrestore(&sem->lock, flags);
    return false;
}

void sem_wait(semaphore_t *sem) {
    if (!sem) {
        return;
    }

    while (1) {
        uint32_t flags = 0;
        spinlock_acquire_irqsave(&sem->lock, &flags);

        if (sem->count > 0) {
            sem->count--;
            tcb_t *curr = sched_get_current_task();
            if (curr && curr->wait_obj == sem) {
                curr->wait_type = TASK_WAIT_NONE;
                curr->wait_obj = NULL;
            }
            spinlock_release_irqrestore(&sem->lock, flags);
            return;
        }

        /* Semaphore exhausted: block and sleep */
        tcb_t *curr = sched_get_current_task();
        if (curr) {
            sem_enqueue(sem, curr);
            curr->state = TASK_STATE_BLOCKED;
            curr->wait_type = TASK_WAIT_SEM;
            curr->wait_obj = sem;
        }
        spinlock_release_irqrestore(&sem->lock, flags);

        sched_yield();
#ifdef RADIX_HOST_TEST
        if (curr && curr->state == TASK_STATE_BLOCKED) {
            return;
        }
#endif
    }
}

void sem_post(semaphore_t *sem) {
    if (!sem) {
        return;
    }
    uint32_t flags = 0;
    spinlock_acquire_irqsave(&sem->lock, &flags);

    if (sem->count < sem->max_count) {
        sem->count++;
    }

    tcb_t *waiter = sem_dequeue(sem);
    tcb_t *curr = sched_get_current_task();
    bool in_isr = sched_in_isr();
    bool preempt = false;

    if (waiter) {
        waiter->wait_type = TASK_WAIT_NONE;
        waiter->wait_obj = NULL;
        sched_unblock(waiter);
        if (!curr || waiter->priority > curr->priority) {
            preempt = true;
        }
    }

    spinlock_release_irqrestore(&sem->lock, flags);

    if (preempt) {
        if (in_isr) {
            sched_set_need_resched();
        } else {
            sched_yield();
        }
    }
}

uint32_t sem_get_count(const semaphore_t *sem) {
    if (!sem) {
        return 0;
    }
    /* Simple atomic read of 32-bit word */
    return sem->count;
}

/* ========================================================================= */
/* Task Termination Cleanup Hook                                             */
/* ========================================================================= */

void sync_on_task_killed(tcb_t *task) {
    if (!task) {
        return;
    }

    uint32_t flags = 0;
    spinlock_acquire_irqsave(&s_sync_pi_lock, &flags);

    /* 1. Remove killed task from any wait_queue it was blocked on */
    if (task->wait_obj != NULL) {
        if (task->wait_type == TASK_WAIT_MUTEX) {
            mutex_t *m = (mutex_t *)task->wait_obj;
            spinlock_acquire(&m->lock);
            mutex_remove_waiter(m, task);
            task->wait_type = TASK_WAIT_NONE;
            task->wait_obj = NULL;
            if (m->owner) {
                mutex_recalc_owner_priority(m->owner, m);
            }
            spinlock_release(&m->lock);
        } else if (task->wait_type == TASK_WAIT_SEM) {
            semaphore_t *s = (semaphore_t *)task->wait_obj;
            spinlock_acquire(&s->lock);
            sem_remove_waiter(s, task);
            task->wait_type = TASK_WAIT_NONE;
            task->wait_obj = NULL;
            spinlock_release(&s->lock);
        } else {
            task->wait_type = TASK_WAIT_NONE;
            task->wait_obj = NULL;
        }
    }

    /* 2. Release all mutexes still owned by the killed task */
    while (task->owned_mutexes != NULL) {
        mutex_t *m = task->owned_mutexes;
        spinlock_acquire(&m->lock);
        mutex_list_remove(task, m);
        m->lock_count = 0u;

        tcb_t *waiter = mutex_dequeue(m);
        if (waiter) {
            m->locked = 1u;
            m->lock_count = 1u;
            m->owner = waiter;
            waiter->wait_type = TASK_WAIT_NONE;
            waiter->wait_obj = NULL;
            mutex_list_add(waiter, m);
            mutex_recalc_owner_priority(waiter, m);
            sched_unblock(waiter);
        } else {
            m->locked = 0u;
            m->owner = NULL;
        }
        spinlock_release(&m->lock);
    }

    task->priority = (uint32_t)task->base_priority;

    spinlock_release_irqrestore(&s_sync_pi_lock, flags);
}
