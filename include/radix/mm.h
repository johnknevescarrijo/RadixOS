#ifndef RADIX_MM_H
#define RADIX_MM_H

#include <radix/types.h>

/* Deterministic Kernel Heap Statistics */
typedef struct mm_stats {
    size_t   total_bytes;
    size_t   used_bytes;
    size_t   free_bytes;
    size_t   peak_used_bytes;
    size_t   largest_free_block;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t active_blocks;
    uint32_t failed_allocs;
    uint32_t corrupt_frees;
} mm_stats_t;

/* Heap Initialization */
void mm_init(void *heap_start, size_t heap_size);
void mm_init_default(void);

/* Deterministic Allocation & Deallocation API (16-byte aligned) */
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t new_size);
void  kfree(void *ptr);

/* Telemetry & Integrity Verification */
void mm_get_stats(mm_stats_t *out);
bool mm_check_integrity(void);

#endif /* RADIX_MM_H */
