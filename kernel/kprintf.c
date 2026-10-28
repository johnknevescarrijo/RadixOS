#include <radix/kprintf.h>
#include <radix/hal.h>

void kputc(char c) {
    uart_putc(c);
}

void kputs(const char *str) {
    if (!str) {
        str = "(null)";
    }
    while (*str) {
        kputc(*str++);
    }
}

static void print_uint(uint32_t val, int base, bool uppercase, int min_width, char pad_char) {
    char buf[32];
    int idx = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0) {
        buf[idx++] = '0';
    } else {
        while (val > 0) {
            buf[idx++] = digits[val % (uint32_t)base];
            val /= (uint32_t)base;
        }
    }

    /* Print leading padding */
    while (idx < min_width) {
        kputc(pad_char);
        min_width--;
    }

    /* Print reversed digits */
    while (idx > 0) {
        kputc(buf[--idx]);
    }
}

static void print_int(int32_t val, int min_width, char pad_char) {
    if (val < 0) {
        kputc('-');
        if (min_width > 0) {
            min_width--;
        }
        print_uint((uint32_t)(-val), 10, false, min_width, pad_char);
    } else {
        print_uint((uint32_t)val, 10, false, min_width, pad_char);
    }
}

int kvprintf(const char *fmt, va_list args) {
    int count = 0;

    for (size_t i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            kputc(fmt[i]);
            count++;
            continue;
        }

        i++; /* Skip '%' */
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

        switch (fmt[i]) {
            case 'c': {
                char c = (char)va_arg(args, int);
                kputc(c);
                count++;
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                kputs(s);
                break;
            }
            case 'd':
            case 'i': {
                int32_t val = va_arg(args, int32_t);
                print_int(val, min_width, pad_char);
                break;
            }
            case 'u': {
                uint32_t val = va_arg(args, uint32_t);
                print_uint(val, 10, false, min_width, pad_char);
                break;
            }
            case 'x': {
                uint32_t val = va_arg(args, uint32_t);
                print_uint(val, 16, false, min_width, pad_char);
                break;
            }
            case 'X': {
                uint32_t val = va_arg(args, uint32_t);
                print_uint(val, 16, true, min_width, pad_char);
                break;
            }
            case 'p': {
                uintptr_t val = (uintptr_t)va_arg(args, void *);
                kputs("0x");
                print_uint(val, 16, false, sizeof(uintptr_t) * 2, '0');
                break;
            }
            case '%': {
                kputc('%');
                count++;
                break;
            }
            default: {
                kputc('%');
                kputc(fmt[i]);
                count += 2;
                break;
            }
        }
    }

    return count;
}

int kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = kvprintf(fmt, args);
    va_end(args);
    return ret;
}
