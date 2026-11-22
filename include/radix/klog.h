#ifndef RADIX_KLOG_H
#define RADIX_KLOG_H

#include <radix/types.h>
#include <stdarg.h>

#define KLOG_MAX_ENTRIES  64u
#define KLOG_MSG_MAX_LEN  96u

typedef enum {
    KLOG_LEVEL_DEBUG = 0,
    KLOG_LEVEL_INFO  = 1,
    KLOG_LEVEL_WARN  = 2,
    KLOG_LEVEL_ERROR = 3,
    KLOG_LEVEL_PANIC = 4
} klog_level_t;

typedef struct klog_entry {
    uint32_t     seq;
    uint64_t     tick;
    uint32_t     core_id;
    klog_level_t level;
    char         msg[KLOG_MSG_MAX_LEN];
} klog_entry_t;

void         klog_init(void);
void         klog_set_level(klog_level_t min_level);
klog_level_t klog_get_level(void);
void         klog_set_console_mirror(bool enable);
bool         klog_get_console_mirror(void);

void         klog_vwrite(klog_level_t level, const char *fmt, va_list ap);
void         klog_write(klog_level_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

uint32_t     klog_count(void);
uint32_t     klog_dropped_count(void);
bool         klog_get_entry(uint32_t index, klog_entry_t *out_entry);
void         klog_dump_all(void);
void         klog_dump_filtered(klog_level_t min_level);
void         klog_clear(void);

const char  *klog_level_to_str(klog_level_t level);
bool         klog_parse_level(const char *str, klog_level_t *out_level);

#define KLOG_DEBUG(...) klog_write(KLOG_LEVEL_DEBUG, __VA_ARGS__)
#define KLOG_INFO(...)  klog_write(KLOG_LEVEL_INFO,  __VA_ARGS__)
#define KLOG_WARN(...)  klog_write(KLOG_LEVEL_WARN,  __VA_ARGS__)
#define KLOG_ERR(...)   klog_write(KLOG_LEVEL_ERROR, __VA_ARGS__)
#define KLOG_PANIC(...) klog_write(KLOG_LEVEL_PANIC, __VA_ARGS__)

#endif /* RADIX_KLOG_H */
