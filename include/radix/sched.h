#ifndef RADIX_SCHED_H
#define RADIX_SCHED_H

#include <radix/types.h>
#include <radix/task.h>

#define MAX_TASKS               8
#define DEFAULT_TIME_SLICE_TICKS 5
#define IDLE_TASK_STACK_SIZE    1024
#define RADIX_TICK_HZ           100u
#define RADIX_TICK_MS           (1000u / RADIX_TICK_HZ)

/* Scheduler Initialization and Life Cycle */
void sched_init(void);
void sched_reset_core_state(void);
int  task_create(const char *name, task_entry_t entry, void *arg, uint32_t priority, void *stack_mem, uint32_t stack_size);
int  task_create_in_world(const char *name, task_entry_t entry, void *arg, uint32_t priority,
                          void *stack_mem, uint32_t stack_size, world_id_t world);
int  task_create_pinned(const char *name, task_entry_t entry, void *arg, uint32_t priority,
                        void *stack_mem, uint32_t stack_size, world_id_t world, int8_t core_affinity);
bool task_set_user_region(uint32_t task_id, void *base, size_t size);
#ifndef RADIX_HOST_TEST
void sched_start(void) __attribute__((noreturn));
#else
void sched_start(void);
#endif

/* Preemption and Context Management */
void sched_yield(void);
void sched_tick(void);
uint64_t sched_get_ticks(void);
tcb_t *sched_get_current_task(void);
tcb_t *sched_get_current_task_on_core(uint32_t core_id);

/* Timed Task Sleep */
void task_sleep_ticks(uint64_t ticks);
void task_sleep_ms(uint32_t ms);

/* ISR nesting depth and deferred preemption control */
void sched_enter_isr(void);
void sched_exit_isr(void);
bool sched_in_isr(void);
void sched_set_need_resched(void);
bool sched_need_resched(void);
bool sched_consume_need_resched(void);

/* Default task exit handler if a task function returns */
#ifndef RADIX_HOST_TEST
void task_exit(void) __attribute__((noreturn));
#else
void task_exit(void);
#endif

/* Task blocking and unblocking helpers */
void sched_block_current(void);
void sched_block_current_task(void);
void sched_unblock(tcb_t *task);
void sched_unblock_task(tcb_t *task);

/* Task inspection and management */
const tcb_t *sched_get_task_by_index(uint32_t index);
bool sched_kill_task(uint32_t task_id);

/* Stack canary & high-water mark diagnostics */
bool     task_check_stack_canary(const tcb_t *task);
size_t   task_stack_high_water_mark(const tcb_t *task);
size_t   task_stack_free_min_bytes(const tcb_t *task);
uint32_t sched_check_all_stacks(void);
uint32_t sched_get_stack_overflow_count(void);

#endif /* RADIX_SCHED_H */
