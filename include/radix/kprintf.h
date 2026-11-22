#ifndef RADIX_KPRINTF_H
#define RADIX_KPRINTF_H

#include <radix/types.h>
#include <stdarg.h>

void kputc(char c);
void kputs(const char *str);
int  kvprintf(const char *fmt, va_list args);
int  kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

int  kvsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int  ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Lock-free panic console output helpers (safe when kprintf spinlock may be held) */
void kprintf_raw_putc(char c);
void kprintf_raw_puts(const char *str);
int  kprintf_raw(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* RADIX_KPRINTF_H */
