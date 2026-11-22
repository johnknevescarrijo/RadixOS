/*
 * kernel/timer.c - Freestanding Software Timer Subsystem (ktimer_t)
 *
 * Manages caller-allocated one-shot and periodic timers driven by sched_tick()
 * with IRQ-safe list management and reentrant callback execution.
 */

#include <radix/timer.h>
#include <radix/sched.h>
#include <radix/atomic.h>

static ktimer_t *s_timer_head = NULL;

static void ktimer_unlink_locked(ktimer_t *timer) {
    ktimer_t *prev = NULL;
    ktimer_t *cur = s_timer_head;

    while (cur != NULL) {
        if (cur == timer) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                s_timer_head = cur->next;
            }
            cur->next = NULL;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static void ktimer_append_locked(ktimer_t *timer) {
    timer->next = NULL;
    if (s_timer_head == NULL) {
        s_timer_head = timer;
        return;
    }
    ktimer_t *cur = s_timer_head;
    while (cur->next != NULL) {
        cur = cur->next;
    }
    cur->next = timer;
}

void ktimer_init(ktimer_t *timer, ktimer_cb_t cb, void *arg, bool periodic) {
    if (!timer) {
        return;
    }
    timer->expire_tick = 0u;
    timer->period_ticks = 0u;
    timer->callback = cb;
    timer->arg = arg;
    timer->periodic = periodic;
    timer->active = false;
    timer->next = NULL;
}

bool ktimer_start_ticks(ktimer_t *timer, uint64_t delay_ticks) {
    if (!timer || !timer->callback) {
        return false;
    }

    if (delay_ticks == 0u) {
        delay_ticks = 1u;
    }

    uint32_t flags = arch_irq_save();
    uint64_t now = sched_get_ticks();

    ktimer_unlink_locked(timer);

    timer->period_ticks = timer->periodic ? delay_ticks : 0u;
    timer->expire_tick = now + delay_ticks;
    timer->active = true;
    ktimer_append_locked(timer);

    arch_irq_restore(flags);
    return true;
}

bool ktimer_start(ktimer_t *timer, uint32_t delay_ms) {
    uint64_t ticks = ((uint64_t)delay_ms + RADIX_TICK_MS - 1u) / RADIX_TICK_MS;
    if (ticks == 0u) {
        ticks = 1u;
    }
    return ktimer_start_ticks(timer, ticks);
}

bool ktimer_stop(ktimer_t *timer) {
    if (!timer) {
        return false;
    }

    uint32_t flags = arch_irq_save();
    bool was_active = timer->active;
    ktimer_unlink_locked(timer);
    timer->active = false;
    arch_irq_restore(flags);

    return was_active;
}

bool ktimer_is_active(const ktimer_t *timer) {
    if (!timer) {
        return false;
    }

    uint32_t flags = arch_irq_save();
    bool active = timer->active;
    arch_irq_restore(flags);

    return active;
}

void ktimer_tick(uint64_t now_ticks) {
    while (1) {
        uint32_t flags = arch_irq_save();
        ktimer_t *expired = NULL;
        ktimer_t *cur = s_timer_head;

        while (cur != NULL) {
            if (cur->active && cur->expire_tick <= now_ticks) {
                expired = cur;
                break;
            }
            cur = cur->next;
        }

        if (expired == NULL) {
            arch_irq_restore(flags);
            break;
        }

        ktimer_cb_t cb = expired->callback;
        void *arg = expired->arg;

        /*
         * Update timer state BEFORE invoking the callback so the callback
         * can safely inspect ktimer_is_active(), stop, or restart this timer.
         */
        if (expired->periodic) {
            uint64_t period = (expired->period_ticks > 0u) ? expired->period_ticks : 1u;
            expired->expire_tick = now_ticks + period;
        } else {
            ktimer_unlink_locked(expired);
            expired->active = false;
        }

        arch_irq_restore(flags);

        if (cb != NULL) {
            cb(arg);
        }
    }
}

void ktimer_reset_all(void) {
    uint32_t flags = arch_irq_save();
    ktimer_t *cur = s_timer_head;
    while (cur != NULL) {
        ktimer_t *next = cur->next;
        cur->active = false;
        cur->next = NULL;
        cur = next;
    }
    s_timer_head = NULL;
    arch_irq_restore(flags);
}
