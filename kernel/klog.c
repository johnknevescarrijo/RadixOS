/*
 * kernel/klog.c - IRQ/Spinlock-Safe Kernel Ring Buffer Logger
 *
 * Provides structured circular log storage without dynamic memory allocation,
 * safe for both thread context and ISR context (never blocks on UART inside ISR).
 */

#include <radix/klog.h>
#include <radix/kprintf.h>
#include <radix/sync.h>
#include <radix/sched.h>
#include <radix/amp.h>

static spinlock_t   s_klog_lock = SPINLOCK_INIT;
static klog_entry_t s_klog_entries[KLOG_MAX_ENTRIES];
static klog_level_t s_klog_min_level = KLOG_LEVEL_INFO;
static bool         s_klog_console_mirror = true;
static uint32_t     s_klog_next_seq = 0u;
static uint32_t     s_klog_head_idx = 0u;
static uint32_t     s_klog_write_idx = 0u;
static uint32_t     s_klog_count = 0u;
static uint32_t     s_klog_dropped = 0u;

static void klog_copy_msg(char *dst, const char *src) {
    size_t i = 0u;
    if (src) {
        while (src[i] != '\0' && i + 1u < KLOG_MSG_MAX_LEN) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static void klog_copy_entry(klog_entry_t *dst, const klog_entry_t *src) {
    dst->seq = src->seq;
    dst->tick = src->tick;
    dst->core_id = src->core_id;
    dst->level = src->level;
    klog_copy_msg(dst->msg, src->msg);
}

static char klog_tolower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

static bool klog_streq_ci(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (klog_tolower(*a) != klog_tolower(*b)) {
            return false;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

const char *klog_level_to_str(klog_level_t level) {
    switch (level) {
        case KLOG_LEVEL_DEBUG: return "DEBUG";
        case KLOG_LEVEL_INFO:  return "INFO";
        case KLOG_LEVEL_WARN:  return "WARN";
        case KLOG_LEVEL_ERROR: return "ERR";
        case KLOG_LEVEL_PANIC: return "PANIC";
        default:               return "UNK";
    }
}

bool klog_parse_level(const char *str, klog_level_t *out_level) {
    if (!str || !out_level) {
        return false;
    }
    if (klog_streq_ci(str, "debug")) {
        *out_level = KLOG_LEVEL_DEBUG;
        return true;
    }
    if (klog_streq_ci(str, "info")) {
        *out_level = KLOG_LEVEL_INFO;
        return true;
    }
    if (klog_streq_ci(str, "warn") || klog_streq_ci(str, "warning")) {
        *out_level = KLOG_LEVEL_WARN;
        return true;
    }
    if (klog_streq_ci(str, "err") || klog_streq_ci(str, "error")) {
        *out_level = KLOG_LEVEL_ERROR;
        return true;
    }
    if (klog_streq_ci(str, "panic")) {
        *out_level = KLOG_LEVEL_PANIC;
        return true;
    }
    return false;
}

void klog_init(void) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_klog_lock, &flags);

    s_klog_min_level = KLOG_LEVEL_INFO;
    s_klog_console_mirror = true;
    s_klog_next_seq = 0u;
    s_klog_head_idx = 0u;
    s_klog_write_idx = 0u;
    s_klog_count = 0u;
    s_klog_dropped = 0u;

    for (uint32_t i = 0u; i < KLOG_MAX_ENTRIES; i++) {
        s_klog_entries[i].seq = 0u;
        s_klog_entries[i].tick = 0u;
        s_klog_entries[i].core_id = 0u;
        s_klog_entries[i].level = KLOG_LEVEL_INFO;
        s_klog_entries[i].msg[0] = '\0';
    }

    spinlock_release_irqrestore(&s_klog_lock, flags);
}

void klog_set_level(klog_level_t min_level) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_klog_lock, &flags);
    s_klog_min_level = min_level;
    spinlock_release_irqrestore(&s_klog_lock, flags);
}

klog_level_t klog_get_level(void) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_klog_lock, &flags);
    klog_level_t lvl = s_klog_min_level;
    spinlock_release_irqrestore(&s_klog_lock, flags);
    return lvl;
}

void klog_set_console_mirror(bool enable) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_klog_lock, &flags);
    s_klog_console_mirror = enable;
    spinlock_release_irqrestore(&s_klog_lock, flags);
}

bool klog_get_console_mirror(void) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_klog_lock, &flags);
    bool mirror = s_klog_console_mirror;
    spinlock_release_irqrestore(&s_klog_lock, flags);
    return mirror;
}

void klog_vwrite(klog_level_t level, const char *fmt, va_list ap) {
    /* Snapshot level early to avoid formatting filtered-out messages */
    if (level < klog_get_level() && level != KLOG_LEVEL_PANIC) {
        return;
    }

    char msg_buf[KLOG_MSG_MAX_LEN];
    kvsnprintf(msg_buf, sizeof(msg_buf), fmt ? fmt : "", ap);

    /* Capture metadata before acquiring IRQ-saved spinlock */
    uint64_t tick = sched_get_ticks();
    uint32_t core = arch_get_core_id();
    bool in_isr = sched_in_isr();

    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_klog_lock, &flags);

    if (level < s_klog_min_level && level != KLOG_LEVEL_PANIC) {
        spinlock_release_irqrestore(&s_klog_lock, flags);
        return;
    }

    klog_entry_t *slot = &s_klog_entries[s_klog_write_idx];
    s_klog_next_seq++;
    slot->seq = s_klog_next_seq;
    slot->tick = tick;
    slot->core_id = core;
    slot->level = level;
    klog_copy_msg(slot->msg, msg_buf);

    if (s_klog_count >= KLOG_MAX_ENTRIES) {
        s_klog_head_idx = (s_klog_head_idx + 1u) % KLOG_MAX_ENTRIES;
        s_klog_dropped++;
    } else {
        s_klog_count++;
    }
    s_klog_write_idx = (s_klog_write_idx + 1u) % KLOG_MAX_ENTRIES;

    bool do_mirror = (s_klog_console_mirror && !in_isr);
    spinlock_release_irqrestore(&s_klog_lock, flags);

    if (do_mirror) {
        if (level == KLOG_LEVEL_PANIC) {
            kprintf_raw("[%s][%u][C%u] %s\n",
                        klog_level_to_str(level), (unsigned int)tick,
                        (unsigned int)core, msg_buf);
        } else {
            kprintf("[%s][%u][C%u] %s\n",
                    klog_level_to_str(level), (unsigned int)tick,
                    (unsigned int)core, msg_buf);
        }
    }
}

void klog_write(klog_level_t level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    klog_vwrite(level, fmt, ap);
    va_end(ap);
}

uint32_t klog_count(void) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_klog_lock, &flags);
    uint32_t count = s_klog_count;
    spinlock_release_irqrestore(&s_klog_lock, flags);
    return count;
}

uint32_t klog_dropped_count(void) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_klog_lock, &flags);
    uint32_t dropped = s_klog_dropped;
    spinlock_release_irqrestore(&s_klog_lock, flags);
    return dropped;
}

bool klog_get_entry(uint32_t index, klog_entry_t *out_entry) {
    if (!out_entry) {
        return false;
    }

    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_klog_lock, &flags);

    if (index >= s_klog_count) {
        spinlock_release_irqrestore(&s_klog_lock, flags);
        return false;
    }

    uint32_t phys_idx = (s_klog_head_idx + index) % KLOG_MAX_ENTRIES;
    klog_copy_entry(out_entry, &s_klog_entries[phys_idx]);

    spinlock_release_irqrestore(&s_klog_lock, flags);
    return true;
}

void klog_dump_filtered(klog_level_t min_level) {
    uint32_t total = klog_count();
    uint32_t dropped = klog_dropped_count();
    uint32_t printed = 0u;

    if (dropped > 0u) {
        kprintf("[klog] (%u older entries dropped)\n", (unsigned int)dropped);
    }

    for (uint32_t i = 0u; i < total; i++) {
        klog_entry_t entry;
        if (klog_get_entry(i, &entry) && entry.level >= min_level) {
            kprintf("#%u [%s][t=%u][C%u] %s\n",
                    (unsigned int)entry.seq,
                    klog_level_to_str(entry.level),
                    (unsigned int)entry.tick,
                    (unsigned int)entry.core_id,
                    entry.msg);
            printed++;
        }
    }

    if (printed == 0u) {
        kprintf("[klog] (no entries matching >= %s)\n", klog_level_to_str(min_level));
    }
}

void klog_dump_all(void) {
    klog_dump_filtered(KLOG_LEVEL_DEBUG);
}

void klog_clear(void) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_klog_lock, &flags);
    s_klog_head_idx = 0u;
    s_klog_write_idx = 0u;
    s_klog_count = 0u;
    s_klog_dropped = 0u;
    spinlock_release_irqrestore(&s_klog_lock, flags);
}
