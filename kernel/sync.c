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
/* Mutex Implementation                                                      */
/* ========================================================================= */

static void mutex_enqueue(mutex_t *mutex, tcb_t *task) {
    if (mutex->count < MAX_TASKS) {
        mutex->wait_queue[mutex->tail] = task;
        mutex->tail = (uint8_t)((mutex->tail + 1) % MAX_TASKS);
        mutex->count++;
    }
}

static tcb_t *mutex_dequeue(mutex_t *mutex) {
    if (mutex->count == 0) {
        return NULL;
    }
    tcb_t *task = mutex->wait_queue[mutex->head];
    mutex->wait_queue[mutex->head] = NULL;
    mutex->head = (uint8_t)((mutex->head + 1) % MAX_TASKS);
    mutex->count--;
    return task;
}

void mutex_init(mutex_t *mutex) {
    if (!mutex) {
        return;
    }
    spinlock_init(&mutex->lock);
    mutex->locked = 0;
    mutex->owner = NULL;
    mutex->head = 0;
    mutex->tail = 0;
    mutex->count = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        mutex->wait_queue[i] = NULL;
    }
}

bool mutex_trylock(mutex_t *mutex) {
    if (!mutex) {
        return false;
    }
    uint32_t flags = 0;
    spinlock_acquire_irqsave(&mutex->lock, &flags);

    if (!mutex->locked) {
        mutex->locked = 1;
        mutex->owner = sched_get_current_task();
        spinlock_release_irqrestore(&mutex->lock, flags);
        return true;
    }

    spinlock_release_irqrestore(&mutex->lock, flags);
    return false;
}

void mutex_lock(mutex_t *mutex) {
    if (!mutex) {
        return;
    }

    while (1) {
        uint32_t flags = 0;
        spinlock_acquire_irqsave(&mutex->lock, &flags);

        tcb_t *curr = sched_get_current_task();
        if (!mutex->locked) {
            mutex->locked = 1;
            mutex->owner = curr;
            spinlock_release_irqrestore(&mutex->lock, flags);
            return;
        } else if (curr != NULL && mutex->owner == curr) {
            /* Ownership was transferred to us on wake up */
            spinlock_release_irqrestore(&mutex->lock, flags);
            return;
        }

        /* Mutex is owned by another task: block and sleep */
        if (curr) {
            mutex_enqueue(mutex, curr);
            curr->state = TASK_STATE_BLOCKED;
        }
        spinlock_release_irqrestore(&mutex->lock, flags);

        sched_yield();
    }
}

void mutex_unlock(mutex_t *mutex) {
    if (!mutex) {
        return;
    }
    uint32_t flags = 0;
    spinlock_acquire_irqsave(&mutex->lock, &flags);

    tcb_t *waiter = mutex_dequeue(mutex);
    if (waiter) {
        mutex->locked = 1;
        mutex->owner = waiter;
        sched_unblock(waiter);
    } else {
        mutex->locked = 0;
        mutex->owner = NULL;
    }

    spinlock_release_irqrestore(&mutex->lock, flags);
}

/* ========================================================================= */
/* Semaphore Implementation                                                  */
/* ========================================================================= */

static void sem_enqueue(semaphore_t *sem, tcb_t *task) {
    if (sem->waiters < MAX_TASKS) {
        sem->wait_queue[sem->tail] = task;
        sem->tail = (uint8_t)((sem->tail + 1) % MAX_TASKS);
        sem->waiters++;
    }
}

static tcb_t *sem_dequeue(semaphore_t *sem) {
    if (sem->waiters == 0) {
        return NULL;
    }
    tcb_t *task = sem->wait_queue[sem->head];
    sem->wait_queue[sem->head] = NULL;
    sem->head = (uint8_t)((sem->head + 1) % MAX_TASKS);
    sem->waiters--;
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
            spinlock_release_irqrestore(&sem->lock, flags);
            return;
        }

        /* Semaphore exhausted: block and sleep */
        tcb_t *curr = sched_get_current_task();
        if (curr) {
            sem_enqueue(sem, curr);
            curr->state = TASK_STATE_BLOCKED;
        }
        spinlock_release_irqrestore(&sem->lock, flags);

        sched_yield();
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
    if (waiter) {
        sched_unblock(waiter);
    }

    spinlock_release_irqrestore(&sem->lock, flags);
}

uint32_t sem_get_count(const semaphore_t *sem) {
    if (!sem) {
        return 0;
    }
    /* Simple atomic read of 32-bit word */
    return sem->count;
}
