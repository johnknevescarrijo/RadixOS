#ifndef RADIX_TIMER_H
#define RADIX_TIMER_H

#include <radix/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Software timer callback signature */
typedef void (*ktimer_cb_t)(void *arg);

/* Caller-allocated software timer object (no dynamic allocation) */
typedef struct ktimer {
    uint64_t       expire_tick;
    uint64_t       period_ticks;
    ktimer_cb_t    callback;
    void          *arg;
    bool           periodic;
    bool           active;
    struct ktimer *next;
} ktimer_t;

void ktimer_init(ktimer_t *timer, ktimer_cb_t cb, void *arg, bool periodic);
bool ktimer_start_ticks(ktimer_t *timer, uint64_t delay_ticks);
bool ktimer_start(ktimer_t *timer, uint32_t delay_ms);
bool ktimer_stop(ktimer_t *timer);
bool ktimer_is_active(const ktimer_t *timer);
void ktimer_tick(uint64_t now_ticks);
void ktimer_reset_all(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIX_TIMER_H */
