/*
 * kernel/mm.c - Deterministic Segregated Free-List Heap Allocator
 *
 * Features:
 *   - 16-byte aligned payload and block metadata (32-bit & 64-bit portable)
 *   - Segregated free-list bins with bitmap lookup for deterministic O(1) class search
 *   - Boundary tags (header + footer) for immediate O(1) forward/backward coalescing
 *   - Magic + checksum validation to safely catch double-free and heap corruption
 *   - IRQ-safe spinlock protection
 */

#include <radix/mm.h>
#include <radix/sync.h>

#define MM_ALIGNMENT        16u
#define MM_NUM_BINS         16u

#define MM_MAGIC_ALLOC      0x5244584Du /* "RDXM" */
#define MM_MAGIC_FREE       0x46524545u /* "FREE" */

#define MM_STATE_ALLOC      0xA110CA7Eu
#define MM_STATE_FREE       0xF2EEB10Cu

typedef struct mm_block_hdr {
    uint32_t magic;
    uint32_t block_size;
    uint32_t req_size;
    uint32_t state;
} mm_block_hdr_t;

typedef struct mm_block_ftr {
    uint32_t magic;
    uint32_t block_size;
    uint32_t state;
    uint32_t checksum;
} mm_block_ftr_t;

typedef struct mm_free_node {
    struct mm_free_node *prev;
    struct mm_free_node *next;
} mm_free_node_t;

#define MM_HDR_SIZE         ((uint32_t)sizeof(mm_block_hdr_t))
#define MM_FTR_SIZE         ((uint32_t)sizeof(mm_block_ftr_t))
#define MM_OVERHEAD         (MM_HDR_SIZE + MM_FTR_SIZE)
#define MM_MIN_PAYLOAD      16u
#define MM_MIN_BLOCK_SIZE   (MM_OVERHEAD + MM_MIN_PAYLOAD)

static const uint32_t s_bin_limits[MM_NUM_BINS - 1u] = {
    48u, 64u, 96u, 128u, 192u, 256u, 384u, 512u,
    1024u, 2048u, 4096u, 8192u, 16384u, 32768u, 65536u
};

static spinlock_t      s_mm_lock = SPINLOCK_INIT;
static bool            s_mm_initialized = false;
static uint8_t        *s_heap_base = NULL;
static uint8_t        *s_heap_end = NULL;
static size_t          s_heap_size = 0;

static mm_free_node_t *s_free_bins[MM_NUM_BINS];
static uint32_t        s_free_bitmap = 0;
static mm_stats_t      s_stats = {0};

static void mm_memzero(void *dst, size_t len) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < len; i++) {
        d[i] = 0u;
    }
}

static void mm_memcpy(void *dst, const void *src, size_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < len; i++) {
        d[i] = s[i];
    }
}

static uint32_t mm_ftr_checksum(uint32_t magic, uint32_t block_size, uint32_t state) {
    return (magic ^ block_size ^ state ^ 0xA55A5AA5u);
}

static mm_block_ftr_t *mm_get_ftr(const mm_block_hdr_t *blk) {
    return (mm_block_ftr_t *)(void *)((uint8_t *)(uintptr_t)blk + blk->block_size - MM_FTR_SIZE);
}

static void *mm_hdr_to_payload(mm_block_hdr_t *blk) {
    return (void *)((uint8_t *)blk + MM_HDR_SIZE);
}

static mm_free_node_t *mm_hdr_to_node(mm_block_hdr_t *blk) {
    return (mm_free_node_t *)mm_hdr_to_payload(blk);
}

static mm_block_hdr_t *mm_node_to_hdr(mm_free_node_t *node) {
    return (mm_block_hdr_t *)(void *)((uint8_t *)node - MM_HDR_SIZE);
}

static void mm_write_tags(mm_block_hdr_t *blk, uint32_t block_size, uint32_t req_size, bool is_alloc) {
    uint32_t magic = is_alloc ? MM_MAGIC_ALLOC : MM_MAGIC_FREE;
    uint32_t state = is_alloc ? MM_STATE_ALLOC : MM_STATE_FREE;

    blk->magic = magic;
    blk->block_size = block_size;
    blk->req_size = is_alloc ? req_size : 0u;
    blk->state = state;

    mm_block_ftr_t *ftr = mm_get_ftr(blk);
    ftr->magic = magic;
    ftr->block_size = block_size;
    ftr->state = state;
    ftr->checksum = mm_ftr_checksum(magic, block_size, state);
}

static uint32_t mm_size_to_bin(uint32_t block_size) {
    for (uint32_t i = 0; i < MM_NUM_BINS - 1u; i++) {
        if (block_size <= s_bin_limits[i]) {
            return i;
        }
    }
    return MM_NUM_BINS - 1u;
}

static bool mm_ptr_in_heap_range(const void *ptr) {
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)s_heap_base;
    uintptr_t end = (uintptr_t)s_heap_end;
    return (p >= base && p < end);
}

static bool mm_is_valid_block(const mm_block_hdr_t *blk, bool expect_alloc) {
    if (!blk || (((uintptr_t)blk & (MM_ALIGNMENT - 1u)) != 0u)) {
        return false;
    }
    if ((uintptr_t)blk < (uintptr_t)s_heap_base ||
        (uintptr_t)blk + MM_MIN_BLOCK_SIZE > (uintptr_t)s_heap_end) {
        return false;
    }

    uint32_t expected_magic = expect_alloc ? MM_MAGIC_ALLOC : MM_MAGIC_FREE;
    uint32_t expected_state = expect_alloc ? MM_STATE_ALLOC : MM_STATE_FREE;

    if (blk->magic != expected_magic || blk->state != expected_state) {
        return false;
    }
    if (blk->block_size < MM_MIN_BLOCK_SIZE || (blk->block_size & (MM_ALIGNMENT - 1u)) != 0u) {
        return false;
    }
    if ((uintptr_t)blk + blk->block_size > (uintptr_t)s_heap_end) {
        return false;
    }

    const mm_block_ftr_t *ftr = mm_get_ftr(blk);
    if (ftr->magic != expected_magic ||
        ftr->state != expected_state ||
        ftr->block_size != blk->block_size ||
        ftr->checksum != mm_ftr_checksum(expected_magic, blk->block_size, expected_state)) {
        return false;
    }

    if (expect_alloc && blk->req_size > (blk->block_size - MM_OVERHEAD)) {
        return false;
    }

    return true;
}

static void mm_bin_insert(mm_block_hdr_t *blk) {
    uint32_t bin = mm_size_to_bin(blk->block_size);
    mm_free_node_t *node = mm_hdr_to_node(blk);

    node->prev = NULL;
    node->next = s_free_bins[bin];
    if (s_free_bins[bin]) {
        s_free_bins[bin]->prev = node;
    }
    s_free_bins[bin] = node;
    s_free_bitmap |= (1u << bin);
}

static void mm_bin_remove(mm_block_hdr_t *blk) {
    uint32_t bin = mm_size_to_bin(blk->block_size);
    mm_free_node_t *node = mm_hdr_to_node(blk);

    if (node->prev) {
        node->prev->next = node->next;
    } else {
        s_free_bins[bin] = node->next;
    }

    if (node->next) {
        node->next->prev = node->prev;
    }

    node->prev = NULL;
    node->next = NULL;

    if (!s_free_bins[bin]) {
        s_free_bitmap &= ~(1u << bin);
    }
}

static mm_block_hdr_t *mm_find_free_block(uint32_t needed_size) {
    uint32_t start_bin = mm_size_to_bin(needed_size);

    /* First check matching bin (first-fit within bin) */
    if ((s_free_bitmap & (1u << start_bin)) != 0u) {
        mm_free_node_t *curr = s_free_bins[start_bin];
        while (curr) {
            mm_block_hdr_t *blk = mm_node_to_hdr(curr);
            if (blk->block_size >= needed_size) {
                return blk;
            }
            curr = curr->next;
        }
    }

    /* Check strictly larger bins: any block in a higher bin is guaranteed to fit */
    if (start_bin + 1u < MM_NUM_BINS) {
        uint32_t higher_mask = s_free_bitmap & (~0u << (start_bin + 1u));
        for (uint32_t bin = start_bin + 1u; higher_mask != 0u && bin < MM_NUM_BINS; bin++) {
            if ((higher_mask & (1u << bin)) != 0u && s_free_bins[bin] != NULL) {
                return mm_node_to_hdr(s_free_bins[bin]);
            }
        }
    }

    return NULL;
}

static size_t mm_align16_payload(size_t size) {
    size_t aligned = (size + (MM_ALIGNMENT - 1u)) & ~(size_t)(MM_ALIGNMENT - 1u);
    return (aligned < MM_MIN_PAYLOAD) ? MM_MIN_PAYLOAD : aligned;
}

static void *mm_alloc_locked(size_t size) {
    if (!s_mm_initialized || size == 0u) {
        if (s_mm_initialized && size == 0u) {
            return NULL;
        }
        return NULL;
    }

    if (size > (size_t)UINT32_MAX - MM_OVERHEAD - MM_ALIGNMENT) {
        s_stats.failed_allocs++;
        return NULL;
    }

    size_t aligned_payload = mm_align16_payload(size);
    uint32_t needed = (uint32_t)(aligned_payload + MM_OVERHEAD);

    mm_block_hdr_t *blk = mm_find_free_block(needed);
    if (!blk) {
        s_stats.failed_allocs++;
        return NULL;
    }

    mm_bin_remove(blk);

    uint32_t rem = blk->block_size - needed;
    if (rem >= MM_MIN_BLOCK_SIZE) {
        mm_block_hdr_t *tail = (mm_block_hdr_t *)(void *)((uint8_t *)blk + needed);
        mm_write_tags(tail, rem, 0u, false);
        mm_bin_insert(tail);
        blk->block_size = needed;
    }

    mm_write_tags(blk, blk->block_size, (uint32_t)size, true);

    s_stats.alloc_count++;
    s_stats.active_blocks++;
    s_stats.used_bytes += blk->block_size;
    if (s_stats.used_bytes > s_stats.peak_used_bytes) {
        s_stats.peak_used_bytes = s_stats.used_bytes;
    }

    return mm_hdr_to_payload(blk);
}

static mm_block_hdr_t *mm_validate_user_ptr_locked(void *ptr) {
    if (!s_mm_initialized || !ptr) {
        return NULL;
    }

    uintptr_t p = (uintptr_t)ptr;
    if ((p & (MM_ALIGNMENT - 1u)) != 0u) {
        return NULL;
    }
    if (p < (uintptr_t)s_heap_base + MM_HDR_SIZE ||
        p > (uintptr_t)s_heap_end - MM_FTR_SIZE - MM_MIN_PAYLOAD) {
        return NULL;
    }

    mm_block_hdr_t *blk = (mm_block_hdr_t *)(void *)((uint8_t *)ptr - MM_HDR_SIZE);
    if (!mm_is_valid_block(blk, true)) {
        return NULL;
    }

    return blk;
}

static void mm_coalesce_and_insert_locked(mm_block_hdr_t *blk) {
    /* Coalesce with next physical block if free */
    uint8_t *next_addr = (uint8_t *)blk + blk->block_size;
    if (next_addr < s_heap_end) {
        mm_block_hdr_t *next = (mm_block_hdr_t *)(void *)next_addr;
        if (mm_is_valid_block(next, false)) {
            mm_bin_remove(next);
            blk->block_size += next->block_size;
        }
    }

    /* Coalesce with previous physical block if free */
    if ((uint8_t *)blk > s_heap_base) {
        mm_block_ftr_t *prev_ftr = (mm_block_ftr_t *)(void *)((uint8_t *)blk - MM_FTR_SIZE);
        if (prev_ftr->magic == MM_MAGIC_FREE &&
            prev_ftr->state == MM_STATE_FREE &&
            prev_ftr->block_size >= MM_MIN_BLOCK_SIZE &&
            (prev_ftr->block_size & (MM_ALIGNMENT - 1u)) == 0u &&
            prev_ftr->checksum == mm_ftr_checksum(MM_MAGIC_FREE, prev_ftr->block_size, MM_STATE_FREE) &&
            (uintptr_t)blk - prev_ftr->block_size >= (uintptr_t)s_heap_base) {
            mm_block_hdr_t *prev = (mm_block_hdr_t *)(void *)((uint8_t *)blk - prev_ftr->block_size);
            if (mm_is_valid_block(prev, false) && prev->block_size == prev_ftr->block_size) {
                mm_bin_remove(prev);
                prev->block_size += blk->block_size;
                blk = prev;
            }
        }
    }

    mm_write_tags(blk, blk->block_size, 0u, false);
    mm_bin_insert(blk);
}

static bool mm_free_locked(void *ptr) {
    if (!ptr) {
        return true;
    }

    mm_block_hdr_t *blk = mm_validate_user_ptr_locked(ptr);
    if (!blk) {
        s_stats.corrupt_frees++;
        return false;
    }

    s_stats.used_bytes -= blk->block_size;
    s_stats.active_blocks--;
    s_stats.free_count++;

    mm_coalesce_and_insert_locked(blk);
    return true;
}

void mm_init(void *heap_start, size_t heap_size) {
    uint32_t flags;
    spinlock_acquire_irqsave(&s_mm_lock, &flags);

    for (uint32_t i = 0; i < MM_NUM_BINS; i++) {
        s_free_bins[i] = NULL;
    }
    s_free_bitmap = 0u;
    mm_memzero(&s_stats, sizeof(s_stats));
    s_heap_base = NULL;
    s_heap_end = NULL;
    s_heap_size = 0u;
    s_mm_initialized = false;

    if (!heap_start || heap_size < MM_MIN_BLOCK_SIZE) {
        spinlock_release_irqrestore(&s_mm_lock, flags);
        return;
    }

    uintptr_t raw_start = (uintptr_t)heap_start;
    uintptr_t raw_end = raw_start + heap_size;
    if (raw_end < raw_start) {
        spinlock_release_irqrestore(&s_mm_lock, flags);
        return;
    }

    uintptr_t aligned_start = (raw_start + (MM_ALIGNMENT - 1u)) & ~(uintptr_t)(MM_ALIGNMENT - 1u);
    uintptr_t aligned_end = raw_end & ~(uintptr_t)(MM_ALIGNMENT - 1u);

    if (aligned_end <= aligned_start || (aligned_end - aligned_start) < MM_MIN_BLOCK_SIZE) {
        spinlock_release_irqrestore(&s_mm_lock, flags);
        return;
    }

    size_t usable_arena = (size_t)(aligned_end - aligned_start);
    if (usable_arena > (size_t)UINT32_MAX) {
        usable_arena = (size_t)UINT32_MAX & ~(size_t)(MM_ALIGNMENT - 1u);
        aligned_end = aligned_start + usable_arena;
    }

    s_heap_base = (uint8_t *)aligned_start;
    s_heap_end = (uint8_t *)aligned_end;
    s_heap_size = usable_arena;

    mm_block_hdr_t *initial = (mm_block_hdr_t *)(void *)s_heap_base;
    mm_write_tags(initial, (uint32_t)s_heap_size, 0u, false);
    mm_bin_insert(initial);

    s_stats.total_bytes = s_heap_size;
    s_stats.free_bytes = s_heap_size - MM_OVERHEAD;
    s_stats.largest_free_block = s_heap_size - MM_OVERHEAD;
    s_mm_initialized = true;

    spinlock_release_irqrestore(&s_mm_lock, flags);
}

void mm_init_default(void) {
#ifndef RADIX_HOST_TEST
    extern uint8_t _heap_start[];
    extern uint8_t _heap_end[];
    uintptr_t start = (uintptr_t)_heap_start;
    uintptr_t end = (uintptr_t)_heap_end;
    size_t size = (end > start) ? (size_t)(end - start) : 0u;
    mm_init((void *)start, size);
#else
    static uint8_t s_host_default_heap[64u * 1024u] __attribute__((aligned(16)));
    mm_init(s_host_default_heap, sizeof(s_host_default_heap));
#endif
}

void *kmalloc(size_t size) {
    uint32_t flags;
    spinlock_acquire_irqsave(&s_mm_lock, &flags);
    void *ptr = mm_alloc_locked(size);
    spinlock_release_irqrestore(&s_mm_lock, flags);
    return ptr;
}

void *kcalloc(size_t count, size_t size) {
    if (count == 0u || size == 0u) {
        return NULL;
    }

    if (count > ((size_t)-1) / size) {
        uint32_t flags;
        spinlock_acquire_irqsave(&s_mm_lock, &flags);
        if (s_mm_initialized) {
            s_stats.failed_allocs++;
        }
        spinlock_release_irqrestore(&s_mm_lock, flags);
        return NULL;
    }

    size_t total = count * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        mm_memzero(ptr, total);
    }
    return ptr;
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) {
        return kmalloc(new_size);
    }
    if (new_size == 0u) {
        kfree(ptr);
        return NULL;
    }

    uint32_t flags;
    spinlock_acquire_irqsave(&s_mm_lock, &flags);

    mm_block_hdr_t *blk = mm_validate_user_ptr_locked(ptr);
    if (!blk) {
        if (s_mm_initialized) {
            s_stats.corrupt_frees++;
        }
        spinlock_release_irqrestore(&s_mm_lock, flags);
        return NULL;
    }

    if (new_size > (size_t)UINT32_MAX - MM_OVERHEAD - MM_ALIGNMENT) {
        s_stats.failed_allocs++;
        spinlock_release_irqrestore(&s_mm_lock, flags);
        return NULL;
    }

    uint32_t old_req = blk->req_size;
    uint32_t old_block_size = blk->block_size;
    size_t aligned_new = mm_align16_payload(new_size);
    uint32_t needed = (uint32_t)(aligned_new + MM_OVERHEAD);

    /* Case 1: Fits inside current block (shrink or same class) */
    if (needed <= old_block_size) {
        uint32_t rem = old_block_size - needed;
        if (rem >= MM_MIN_BLOCK_SIZE) {
            mm_write_tags(blk, needed, (uint32_t)new_size, true);
            mm_block_hdr_t *tail = (mm_block_hdr_t *)(void *)((uint8_t *)blk + needed);
            mm_write_tags(tail, rem, 0u, false);
            mm_coalesce_and_insert_locked(tail);
            s_stats.used_bytes -= rem;
        } else {
            blk->req_size = (uint32_t)new_size;
        }
        spinlock_release_irqrestore(&s_mm_lock, flags);
        return ptr;
    }

    /* Case 2: Try in-place expansion into adjacent free next block */
    uint8_t *next_addr = (uint8_t *)blk + old_block_size;
    if (next_addr < s_heap_end) {
        mm_block_hdr_t *next = (mm_block_hdr_t *)(void *)next_addr;
        if (mm_is_valid_block(next, false) && (old_block_size + next->block_size) >= needed) {
            uint32_t combined = old_block_size + next->block_size;
            mm_bin_remove(next);

            uint32_t rem = combined - needed;
            if (rem >= MM_MIN_BLOCK_SIZE) {
                mm_write_tags(blk, needed, (uint32_t)new_size, true);
                mm_block_hdr_t *tail = (mm_block_hdr_t *)(void *)((uint8_t *)blk + needed);
                mm_write_tags(tail, rem, 0u, false);
                mm_bin_insert(tail);
                s_stats.used_bytes += (needed - old_block_size);
            } else {
                mm_write_tags(blk, combined, (uint32_t)new_size, true);
                s_stats.used_bytes += next->block_size;
            }

            if (s_stats.used_bytes > s_stats.peak_used_bytes) {
                s_stats.peak_used_bytes = s_stats.used_bytes;
            }

            spinlock_release_irqrestore(&s_mm_lock, flags);
            return ptr;
        }
    }

    /* Case 3: Allocate new block, copy old payload, free old block */
    void *new_ptr = mm_alloc_locked(new_size);
    if (!new_ptr) {
        spinlock_release_irqrestore(&s_mm_lock, flags);
        return NULL;
    }

    size_t copy_len = (old_req < new_size) ? (size_t)old_req : new_size;
    mm_memcpy(new_ptr, ptr, copy_len);
    mm_free_locked(ptr);

    spinlock_release_irqrestore(&s_mm_lock, flags);
    return new_ptr;
}

void kfree(void *ptr) {
    if (!ptr) {
        return;
    }

    uint32_t flags;
    spinlock_acquire_irqsave(&s_mm_lock, &flags);
    mm_free_locked(ptr);
    spinlock_release_irqrestore(&s_mm_lock, flags);
}

void mm_get_stats(mm_stats_t *out) {
    if (!out) {
        return;
    }

    uint32_t flags;
    spinlock_acquire_irqsave(&s_mm_lock, &flags);

    if (!s_mm_initialized) {
        mm_memzero(out, sizeof(*out));
        spinlock_release_irqrestore(&s_mm_lock, flags);
        return;
    }

    size_t free_sum = 0u;
    size_t largest = 0u;
    for (uint32_t bin = 0u; bin < MM_NUM_BINS; bin++) {
        mm_free_node_t *curr = s_free_bins[bin];
        while (curr) {
            mm_block_hdr_t *blk = mm_node_to_hdr(curr);
            if (blk->block_size >= MM_MIN_BLOCK_SIZE) {
                size_t payload = (size_t)(blk->block_size - MM_OVERHEAD);
                free_sum += payload;
                if (payload > largest) {
                    largest = payload;
                }
            }
            curr = curr->next;
        }
    }

    s_stats.free_bytes = free_sum;
    s_stats.largest_free_block = largest;
    mm_memcpy(out, &s_stats, sizeof(*out));

    spinlock_release_irqrestore(&s_mm_lock, flags);
}

bool mm_check_integrity(void) {
    uint32_t flags;
    spinlock_acquire_irqsave(&s_mm_lock, &flags);

    if (!s_mm_initialized || !s_heap_base || s_heap_end <= s_heap_base) {
        spinlock_release_irqrestore(&s_mm_lock, flags);
        return false;
    }

    bool ok = true;
    uint8_t *cursor = s_heap_base;
    uint32_t walk_alloc_blocks = 0u;
    uint32_t walk_free_blocks = 0u;
    size_t walk_used_bytes = 0u;
    bool prev_was_free = false;
    uint32_t max_blocks = (uint32_t)(s_heap_size / MM_MIN_BLOCK_SIZE) + 1u;
    uint32_t step = 0u;

    /* 1. Linear physical block walk */
    while (cursor < s_heap_end && step < max_blocks) {
        step++;
        mm_block_hdr_t *blk = (mm_block_hdr_t *)(void *)cursor;
        if (blk->state == MM_STATE_ALLOC) {
            if (!mm_is_valid_block(blk, true)) {
                ok = false;
                break;
            }
            walk_alloc_blocks++;
            walk_used_bytes += blk->block_size;
            prev_was_free = false;
        } else if (blk->state == MM_STATE_FREE) {
            if (!mm_is_valid_block(blk, false) || prev_was_free) {
                ok = false;
                break;
            }
            walk_free_blocks++;
            prev_was_free = true;
        } else {
            ok = false;
            break;
        }
        cursor += blk->block_size;
    }

    if (cursor != s_heap_end) {
        ok = false;
    }
    if (walk_alloc_blocks != s_stats.active_blocks || walk_used_bytes != s_stats.used_bytes) {
        ok = false;
    }

    /* 2. Segregated free-list cross-check */
    if (ok) {
        uint32_t list_free_blocks = 0u;
        for (uint32_t bin = 0u; bin < MM_NUM_BINS; bin++) {
            bool bit_set = ((s_free_bitmap & (1u << bin)) != 0u);
            if (bit_set != (s_free_bins[bin] != NULL)) {
                ok = false;
                break;
            }

            mm_free_node_t *prev = NULL;
            mm_free_node_t *curr = s_free_bins[bin];
            while (curr) {
                if (!mm_ptr_in_heap_range(curr) || curr->prev != prev) {
                    ok = false;
                    break;
                }
                mm_block_hdr_t *blk = mm_node_to_hdr(curr);
                if (!mm_is_valid_block(blk, false) || mm_size_to_bin(blk->block_size) != bin) {
                    ok = false;
                    break;
                }
                list_free_blocks++;
                if (list_free_blocks > walk_free_blocks) {
                    ok = false;
                    break;
                }
                prev = curr;
                curr = curr->next;
            }
            if (!ok) {
                break;
            }
        }
        if (list_free_blocks != walk_free_blocks) {
            ok = false;
        }
    }

    spinlock_release_irqrestore(&s_mm_lock, flags);
    return ok;
}
