#include <radix/kprintf.h>
#include <radix/hal.h>
#include <radix/sync.h>

static spinlock_t s_kprintf_lock = SPINLOCK_INIT;

typedef struct {
    char  *buf;
    size_t capacity;
    size_t pos;
    bool   to_uart;
} kfmt_sink_t;

static void sink_putc(kfmt_sink_t *sink, char c) {
    if (sink->to_uart) {
        uart_putc(c);
    } else if (sink->buf && sink->capacity > 0u && sink->pos + 1u < sink->capacity) {
        sink->buf[sink->pos] = c;
    }
    sink->pos++;
}

static void sink_puts(kfmt_sink_t *sink, const char *str) {
    if (!str) {
        str = "(null)";
    }
    while (*str) {
        sink_putc(sink, *str++);
    }
}

static void sink_print_uint(kfmt_sink_t *sink, uint32_t val, int base, bool uppercase,
                            int min_width, char pad_char) {
    char buf[32];
    int idx = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0u) {
        buf[idx++] = '0';
    } else {
        while (val > 0u) {
            buf[idx++] = digits[val % (uint32_t)base];
            val /= (uint32_t)base;
        }
    }

    while (idx < min_width) {
        sink_putc(sink, pad_char);
        min_width--;
    }

    while (idx > 0) {
        sink_putc(sink, buf[--idx]);
    }
}

static void sink_print_int(kfmt_sink_t *sink, int32_t val, int min_width, char pad_char) {
    if (val < 0) {
        sink_putc(sink, '-');
        if (min_width > 0) {
            min_width--;
        }
        uint32_t uval = 0u - (uint32_t)val;
        sink_print_uint(sink, uval, 10, false, min_width, pad_char);
    } else {
        sink_print_uint(sink, (uint32_t)val, 10, false, min_width, pad_char);
    }
}

static void sink_print_ptr(kfmt_sink_t *sink, uintptr_t val) {
    static const char hex_digits[] = "0123456789abcdef";
    const int nibbles = (int)(sizeof(uintptr_t) * 2u);
    sink_puts(sink, "0x");
    for (int i = nibbles - 1; i >= 0; i--) {
        uint8_t nib = (uint8_t)((val >> (unsigned)(i * 4)) & 0xFu);
        sink_putc(sink, hex_digits[nib]);
    }
}

static int kfmt_vformat(kfmt_sink_t *sink, const char *fmt, va_list args) {
    if (!fmt) {
        if (!sink->to_uart && sink->buf && sink->capacity > 0u) {
            sink->buf[0] = '\0';
        }
        return 0;
    }

    for (size_t i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            sink_putc(sink, fmt[i]);
            continue;
        }

        i++; /* Skip '%' */
        if (fmt[i] == '\0') {
            break;
        }

        char pad_char = ' ';
        int min_width = 0;

        if (fmt[i] == '0') {
            pad_char = '0';
            i++;
        }

        while (fmt[i] >= '0' && fmt[i] <= '9') {
            min_width = (min_width * 10) + (fmt[i] - '0');
            i++;
        }

        if (fmt[i] == 'l') {
            i++; /* Skip length modifier for 32-bit long */
        }

        if (fmt[i] == '\0') {
            break;
        }

        switch (fmt[i]) {
            case 'c': {
                char c = (char)va_arg(args, int);
                sink_putc(sink, c);
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                sink_puts(sink, s);
                break;
            }
            case 'd':
            case 'i': {
                int32_t val = va_arg(args, int32_t);
                sink_print_int(sink, val, min_width, pad_char);
                break;
            }
            case 'u': {
                uint32_t val = va_arg(args, uint32_t);
                sink_print_uint(sink, val, 10, false, min_width, pad_char);
                break;
            }
            case 'x': {
                uint32_t val = va_arg(args, uint32_t);
                sink_print_uint(sink, val, 16, false, min_width, pad_char);
                break;
            }
            case 'X': {
                uint32_t val = va_arg(args, uint32_t);
                sink_print_uint(sink, val, 16, true, min_width, pad_char);
                break;
            }
            case 'p': {
                uintptr_t val = (uintptr_t)va_arg(args, void *);
                sink_print_ptr(sink, val);
                break;
            }
            case '%': {
                sink_putc(sink, '%');
                break;
            }
            default: {
                sink_putc(sink, '%');
                sink_putc(sink, fmt[i]);
                break;
            }
        }
    }

    if (!sink->to_uart && sink->buf && sink->capacity > 0u) {
        size_t term_idx = (sink->pos < sink->capacity) ? sink->pos : (sink->capacity - 1u);
        sink->buf[term_idx] = '\0';
    }

    return (int)sink->pos;
}

void kprintf_raw_putc(char c) {
    uart_putc(c);
}

void kprintf_raw_puts(const char *str) {
    if (!str) {
        str = "(null)";
    }
    while (*str) {
        uart_putc(*str++);
    }
}

int kprintf_raw(const char *fmt, ...) {
    kfmt_sink_t sink = {
        .buf = NULL,
        .capacity = 0u,
        .pos = 0u,
        .to_uart = true
    };
    va_list args;
    va_start(args, fmt);
    int ret = kfmt_vformat(&sink, fmt, args);
    va_end(args);
    return ret;
}

void kputc(char c) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_kprintf_lock, &flags);
    uart_putc(c);
    spinlock_release_irqrestore(&s_kprintf_lock, flags);
}

void kputs(const char *str) {
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_kprintf_lock, &flags);
    kprintf_raw_puts(str);
    spinlock_release_irqrestore(&s_kprintf_lock, flags);
}

int kvprintf(const char *fmt, va_list args) {
    kfmt_sink_t sink = {
        .buf = NULL,
        .capacity = 0u,
        .pos = 0u,
        .to_uart = true
    };
    uint32_t flags = 0u;
    spinlock_acquire_irqsave(&s_kprintf_lock, &flags);
    int ret = kfmt_vformat(&sink, fmt, args);
    spinlock_release_irqrestore(&s_kprintf_lock, flags);
    return ret;
}

int kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = kvprintf(fmt, args);
    va_end(args);
    return ret;
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    kfmt_sink_t sink = {
        .buf = buf,
        .capacity = size,
        .pos = 0u,
        .to_uart = false
    };
    return kfmt_vformat(&sink, fmt, args);
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = kvsnprintf(buf, size, fmt, args);
    va_end(args);
    return ret;
}
