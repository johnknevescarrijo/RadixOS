#ifndef RADIX_KPRINTF_H
#define RADIX_KPRINTF_H

#include <radix/types.h>
#include <stdarg.h>

void kputc(char c);
void kputs(const char *str);
int  kvprintf(const char *fmt, va_list args);
int  kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* RADIX_KPRINTF_H */
