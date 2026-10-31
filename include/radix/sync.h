#ifndef RADIX_SYNC_H
#define RADIX_SYNC_H

#include <radix/types.h>
#include <radix/atomic.h>
#include <radix/sched.h>

/* ========================================================================= */
/* Spinlock Primitive                                                        */
/* ========================================================================= */

typedef struct spinlock {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT       { .locked = 0 }

void spinlock_init(spinlock_t *lock);
void spinlock_acquire(spinlock_t *lock);
bool spinlock_try_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);
void spinlock_acquire_irqsave(spinlock_t *lock, uint32_t *flags);
void spinlock_release_irqrestore(spinlock_t *lock, uint32_t flags);

/* ========================================================================= */
/* Blocking Mutex Primitive                                                  */
/* ========================================================================= */

typedef struct mutex {
    spinlock_t          lock;
    volatile uint32_t   locked;
    tcb_t              *owner;
    tcb_t              *wait_queue[MAX_TASKS];
    uint8_t             head;
    uint8_t             tail;
    uint8_t             count;
} mutex_t;

#define MUTEX_INIT          { .lock = SPINLOCK_INIT, .locked = 0, .owner = NULL, .wait_queue = {0}, .head = 0, .tail = 0, .count = 0 }

void mutex_init(mutex_t *mutex);
void mutex_lock(mutex_t *mutex);
bool mutex_trylock(mutex_t *mutex);
void mutex_unlock(mutex_t *mutex);

/* ========================================================================= */
/* Counting Semaphore Primitive                                              */
/* ========================================================================= */

typedef struct semaphore {
    spinlock_t          lock;
    volatile uint32_t   count;
    uint32_t            max_count;
    tcb_t              *wait_queue[MAX_TASKS];
    uint8_t             head;
    uint8_t             tail;
    uint8_t             waiters;
} semaphore_t;

#define SEMAPHORE_INIT(initial, max) { .lock = SPINLOCK_INIT, .count = (initial), .max_count = (max), .wait_queue = {0}, .head = 0, .tail = 0, .waiters = 0 }

void     sem_init(semaphore_t *sem, uint32_t initial_count, uint32_t max_count);
void     sem_wait(semaphore_t *sem);
bool     sem_trywait(semaphore_t *sem);
void     sem_post(semaphore_t *sem);
uint32_t sem_get_count(const semaphore_t *sem);

#endif /* RADIX_SYNC_H */
