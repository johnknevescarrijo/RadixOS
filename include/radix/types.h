#ifndef RADIX_TYPES_H
#define RADIX_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Bit manipulation macros */
#define BIT(n)              (1UL << (n))
#define BITS_MASK(high, low) (((1UL << ((high) - (low) + 1)) - 1) << (low))

/* Memory-Mapped I/O Register Access Macros */
#define REG_READ(addr)           (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG_WRITE(addr, val)      do { (*(volatile uint32_t *)(uintptr_t)(addr)) = (uint32_t)(val); } while (0)
#define REG_SET_BITS(addr, mask)  do { (*(volatile uint32_t *)(uintptr_t)(addr)) |= (uint32_t)(mask); } while (0)
#define REG_CLR_BITS(addr, mask)  do { (*(volatile uint32_t *)(uintptr_t)(addr)) &= ~(uint32_t)(mask); } while (0)

#endif /* RADIX_TYPES_H */
