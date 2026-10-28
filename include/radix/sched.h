#ifndef RADIX_SCHED_H
#define RADIX_SCHED_H

#include <radix/types.h>
#include <radix/task.h>

#define MAX_TASKS               8
#define DEFAULT_TIME_SLICE_TICKS 5
#define IDLE_TASK_STACK_SIZE    1024

/* Scheduler Initialization and Life Cycle */
void sched_init(void);
int  task_create(const char *name, task_entry_t entry, void *arg, uint32_t priority, void *stack_mem, uint32_t stack_size);
#ifndef RADIX_HOST_TEST
void sched_start(void) __attribute__((noreturn));
#else
void sched_start(void);
#endif

/* Preemption and Context Management */
void sched_yield(void);
void sched_tick(void);
tcb_t *sched_get_current_task(void);

/* Default task exit handler if a task function returns */
void task_exit(void) __attribute__((noreturn));

#endif /* RADIX_SCHED_H */
