#include <ttak/phys/mem/buddy.h>
#include <ttak/mem/mem.h>
#include <ttak/types/ttak_compiler.h>

#include <stdalign.h>
#include <ttak/compat/stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <ttak/compat/pthread.h>
#if defined(_MSC_VER)
#include <intrin.h>
#include <malloc.h>
#elif defined(_WIN32)
#include <malloc.h>
#endif

#ifndef EMBEDDED
#define EMBEDDED 0
#endif

#define TTAK_BUDDY_MIN_ORDER   6U   /* 64 bytes */
#define TTAK_BUDDY_MAX_ORDER   60U  /* 2^60 bytes (~1 EB) */
#define TTAK_BUDDY_MAX_SEGMENTS 32U
#define TTAK_BUDDY_GROWTH_THRESHOLD 80U
#define TTAK_BUDDY_MIN_GROWTH_BYTES (1ULL << 20)
#define TTAK_BUDDY_GROWTH_FACTOR 2U
#define TTAK_BUDDY_THEORETICAL_LIMIT_BYTES \
    (1000ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL)
#define TTAK_BUDDY_CAPACITY_LIMIT_BYTES \
    ((TTAK_BUDDY_THEORETICAL_LIMIT_BYTES > (size_t)-1) ? \
         (size_t)-1 : (size_t)TTAK_BUDDY_THEORETICAL_LIMIT_BYTES)

typedef struct ttak_buddy_segment {
    unsigned char *start;
    size_t length;
    uint8_t owns_buffer;
} ttak_buddy_segment_t;

typedef struct ttak_buddy_block {
    struct ttak_buddy_block *next;
    ttak_buddy_segment_t *segment;
    uint32_t owner_tag;
    uint32_t call_safety;
    uint8_t order;
    uint8_t in_use;
} ttak_buddy_block_t;

/* Payloads start this far into a block, which keeps them aligned to
 * TTAK_MEM_HEADER_ALIGN: blocks sit at multiples of 64 bytes from an
 * aligned segment start. Padded by hand because TinyCC drops _Alignas on
 * struct members. */
#define TTAK_BUDDY_HEADER_SIZE \
    ((sizeof(ttak_buddy_block_t) + TTAK_MEM_HEADER_ALIGN - 1) & ~((size_t)TTAK_MEM_HEADER_ALIGN - 1))

typedef struct ttak_buddy_zone {
    ttak_buddy_block_t *free_lists[TTAK_BUDDY_MAX_ORDER + 1];
    ttak_buddy_segment_t segments[TTAK_BUDDY_MAX_SEGMENTS];
    uint8_t segment_count;
    uint8_t max_order;
    uint8_t embedded_mode;
    uint8_t anti_fragmentation;
    void *pool_start;
    _Atomic size_t pool_len;
    _Atomic size_t bytes_in_use;
} ttak_buddy_zone_t;

static ttak_buddy_zone_t g_zone;
/* One lock for every free-list and block-header change. Splitting a block
 * and coalescing its buddy both touch two orders at once; with per-order
 * locks a free could merge a buddy that another thread was still
 * splitting, handing the same memory out twice. */
static pthread_mutex_t g_buddy_lock = PTHREAD_MUTEX_INITIALIZER;
/* Free-list bitmask: one residue bit per buddy order. */
static _Atomic uint64_t g_free_mask = 0;

static inline size_t order_size(uint8_t order);
static inline uint8_t max_order_for_pool(size_t pool_len);
static void list_push(uint8_t order, ttak_buddy_block_t *block);
static inline size_t buddy_capacity(void);
static inline size_t buddy_capacity_limit(void);
static inline size_t buddy_align_up(size_t value, size_t alignment);

static int buddy_debug_enabled(void) {
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    const char *flag = getenv("TTAK_MEM_BUDDY_DEBUG");
    cached = (flag && *flag) ? 1 : 0;
    return cached;
}

static inline uint8_t ttak_buddy_ctz64(uint64_t value) {
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanForward64(&index, value);
    return (uint8_t)index;
#else
    return (uint8_t)__builtin_ctzll(value);
#endif
}

static inline uint8_t ttak_buddy_msb64(uint64_t value) {
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanReverse64(&index, value);
    return (uint8_t)index;
#else
    return (uint8_t)(63U - __builtin_clzll(value));
#endif
}

static inline void buddy_lock(void) {
    pthread_mutex_lock(&g_buddy_lock);
}

static inline void buddy_unlock(void) {
    pthread_mutex_unlock(&g_buddy_lock);
}

static inline void buddy_account_alloc(uint8_t order) {
    atomic_fetch_add_explicit(&g_zone.bytes_in_use, order_size(order), memory_order_relaxed);
}

static inline void buddy_account_free(uint8_t order) {
    atomic_fetch_sub_explicit(&g_zone.bytes_in_use, order_size(order), memory_order_relaxed);
}

static inline void *buddy_heap_alloc(size_t bytes) {
    if (bytes == 0) {
        return NULL;
    }
#if defined(_WIN32)
    return _aligned_malloc(bytes, 64);
#elif TTAK_PLATFORM_BAREMETAL
    /* newlib hides posix_memalign without POSIX visibility; C11
     * aligned_alloc wants a size that is a multiple of the alignment. */
    return aligned_alloc(64, (bytes + 63u) & ~(size_t)63u);
#else
    void *ptr = NULL;
    if (posix_memalign(&ptr, 64, bytes) != 0) {
        ptr = NULL;
    }
    return ptr;
#endif
}

static inline void buddy_heap_free(void *ptr) {
    if (!ptr) {
        return;
    }
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

static void buddy_populate_segment(ttak_buddy_segment_t *segment, uint8_t segment_max_order) {
    if (!segment || !segment->start) {
        return;
    }

    unsigned char *ptr = segment->start;
    size_t remaining = segment->length;
    size_t min_block = order_size(TTAK_BUDDY_MIN_ORDER);

    while (remaining >= min_block) {
        uint8_t order = segment_max_order;
        while (order > TTAK_BUDDY_MIN_ORDER) {
            size_t size = order_size(order);
            size_t offset = (size_t)(ptr - segment->start);
            if (size <= remaining && (offset % size == 0)) {
                break;
            }
            order--;
        }

        ttak_buddy_block_t *block = (ttak_buddy_block_t *)ptr;
        memset(block, 0, sizeof(ttak_buddy_block_t));
        block->segment = segment;
        block->order = order;
        block->in_use = 0;
        list_push(order, block);

        size_t used = order_size(order);
        ptr += used;
        remaining -= used;
    }
}

static void buddy_release_owned_segments_locked(void) {
    for (uint8_t i = 0; i < g_zone.segment_count; ++i) {
        if (g_zone.segments[i].owns_buffer && g_zone.segments[i].start) {
            buddy_heap_free(g_zone.segments[i].start);
        }
    }
}

static bool buddy_add_segment_locked(void *pool_start, size_t pool_len, uint8_t owns_buffer) {
    size_t min_block = order_size(TTAK_BUDDY_MIN_ORDER);
    if (!pool_start || g_zone.segment_count >= TTAK_BUDDY_MAX_SEGMENTS) {
        return false;
    }
    /* Start on a min_block boundary (heap segments already do), whatever
     * alignment the caller's buffer got. */
    size_t skip = (size_t)(-(uintptr_t)pool_start & (min_block - 1U));
    if (pool_len < skip + min_block) {
        return false;
    }
    pool_start = (unsigned char *)pool_start + skip;
    pool_len -= skip;

    ttak_buddy_segment_t *segment = &g_zone.segments[g_zone.segment_count++];
    segment->start = (unsigned char *)pool_start;
    segment->length = pool_len;
    segment->owns_buffer = owns_buffer;

    if (!g_zone.pool_start) {
        g_zone.pool_start = pool_start;
    }
    atomic_fetch_add_explicit(&g_zone.pool_len, pool_len, memory_order_relaxed);

    uint8_t segment_max = max_order_for_pool(pool_len);
    if (segment_max > g_zone.max_order) {
        g_zone.max_order = segment_max;
    }

    buddy_populate_segment(segment, segment_max);
    return true;
}

static inline size_t buddy_capacity(void) {
    return atomic_load_explicit(&g_zone.pool_len, memory_order_relaxed);
}

static inline size_t buddy_capacity_limit(void) {
    return TTAK_BUDDY_CAPACITY_LIMIT_BYTES;
}

static inline size_t buddy_align_up(size_t value, size_t alignment) {
    if (alignment <= 1U) {
        return value;
    }
    size_t mask = alignment - 1U;
    if (value > SIZE_MAX - mask) {
        return 0;
    }
    size_t aligned = (value + mask) & ~mask;
    return aligned;
}

static bool buddy_should_grow(size_t upcoming_bytes) {
    if (g_zone.embedded_mode) {
        return false;
    }
    size_t capacity = buddy_capacity();
    if (capacity == 0) {
        return true;
    }

    size_t used = atomic_load_explicit(&g_zone.bytes_in_use, memory_order_relaxed);
    size_t projected = used + upcoming_bytes;
    if (projected >= capacity) {
        return true;
    }

    size_t whole = capacity / 100U;
    size_t rem = capacity % 100U;
    size_t threshold = whole * TTAK_BUDDY_GROWTH_THRESHOLD +
                       (rem * TTAK_BUDDY_GROWTH_THRESHOLD) / 100U;
    return projected >= threshold;
}

static bool buddy_expand_zone(size_t min_bytes) {
    if (g_zone.embedded_mode || g_zone.segment_count >= TTAK_BUDDY_MAX_SEGMENTS) {
        return false;
    }

    size_t min_block = order_size(TTAK_BUDDY_MIN_ORDER);
    if (min_bytes < min_block) {
        min_bytes = min_block;
    }

    size_t current_capacity = buddy_capacity();
    size_t capacity_limit = buddy_capacity_limit();

    if (current_capacity >= capacity_limit) {
        if (buddy_debug_enabled()) {
            fprintf(stderr, "[Buddy] Auto-growth capped at %zu bytes (limit reached).\n", capacity_limit);
        }
        return false;
    }

    size_t limit_remaining = capacity_limit - current_capacity;
    size_t limit_aligned = limit_remaining & ~(min_block - 1U);
    if (limit_aligned < min_block) {
        return false;
    }

    size_t desired = min_bytes;
    if (current_capacity) {
        if (current_capacity > limit_aligned / TTAK_BUDDY_GROWTH_FACTOR) {
            desired = limit_aligned;
        } else {
            desired = current_capacity * TTAK_BUDDY_GROWTH_FACTOR;
        }
    }

    if (desired < min_bytes) {
        desired = min_bytes;
    }
    if (desired < TTAK_BUDDY_MIN_GROWTH_BYTES) {
        desired = TTAK_BUDDY_MIN_GROWTH_BYTES;
    }
    if (desired > limit_aligned) {
        desired = limit_aligned;
    }

    size_t aligned = buddy_align_up(desired, min_block);
    if (!aligned || aligned > limit_aligned) {
        aligned = limit_aligned;
    }
    if (aligned < min_block) {
        return false;
    }

    void *buffer = buddy_heap_alloc(aligned);
    if (!buffer) {
        return false;
    }

    bool added = false;
    uint8_t new_segment_count = 0;
    buddy_lock();
    if (g_zone.segment_count < TTAK_BUDDY_MAX_SEGMENTS) {
        added = buddy_add_segment_locked(buffer, aligned, 1U);
        new_segment_count = g_zone.segment_count;
    }
    buddy_unlock();

    if (!added) {
        buddy_heap_free(buffer);
    } else if (buddy_debug_enabled()) {
        size_t cap = buddy_capacity();
        fprintf(stderr, "[Buddy] Auto-expanded pool by %zu bytes (segments=%u, capacity=%zu)\n",
                aligned, new_segment_count, cap);
    }
    return added;
}

static inline void mark_order_nonempty(uint8_t order) {
    atomic_fetch_or_explicit(&g_free_mask, 1ULL << order, memory_order_release);
}

static inline void mark_order_empty(uint8_t order) {
    atomic_fetch_and_explicit(&g_free_mask, ~(1ULL << order), memory_order_release);
}

static inline size_t order_size(uint8_t order) {
    if (order > TTAK_BUDDY_MAX_ORDER) return 0;
    return (size_t)1 << order;
}

static inline uint8_t order_for_size(size_t bytes) {
    size_t min_block = order_size(TTAK_BUDDY_MIN_ORDER);
    if (bytes <= min_block) {
        return TTAK_BUDDY_MIN_ORDER;
    }
    uint64_t value = (uint64_t)(bytes - 1ULL);
    uint8_t order = (uint8_t)(ttak_buddy_msb64(value) + 1U);
    if (order < TTAK_BUDDY_MIN_ORDER) {
        return TTAK_BUDDY_MIN_ORDER;
    }
    if (order > TTAK_BUDDY_MAX_ORDER) {
        return TTAK_BUDDY_MAX_ORDER;
    }
    return order;
}

static inline uint8_t max_order_for_pool(size_t pool_len) {
    uint8_t order = TTAK_BUDDY_MIN_ORDER;
    size_t min_block = order_size(TTAK_BUDDY_MIN_ORDER);
    if (pool_len < min_block || min_block == 0) {
        return TTAK_BUDDY_MIN_ORDER;
    }
    while (order < TTAK_BUDDY_MAX_ORDER) {
        size_t next_size = order_size(order + 1U);
        if (next_size == 0 || next_size > pool_len) {
            break;
        }
        order++;
    }
    return order;
}

static inline size_t block_offset(ttak_buddy_block_t *block) {
    if (!block || !block->segment) {
        return 0;
    }
    return (size_t)((unsigned char *)block - block->segment->start);
}

static inline ttak_buddy_block_t *buddy_pair(ttak_buddy_block_t *block, uint8_t order) {
    if (!block || !block->segment) {
        return NULL;
    }
    size_t offset = block_offset(block);
    size_t pair_offset = offset ^ order_size(order);
    if (pair_offset >= block->segment->length) {
        return NULL;
    }
    return (ttak_buddy_block_t *)(block->segment->start + pair_offset);
}

/* With anti_fragmentation (static pools), each free list stays sorted by
 * address, so allocations pack toward the low end and the high end stays
 * whole for large blocks. LIFO order instead scatters small blocks across
 * the pool until no large block can coalesce. */
static void list_push(uint8_t order, ttak_buddy_block_t *block) {
    ttak_buddy_block_t **link = &g_zone.free_lists[order];
    if (g_zone.anti_fragmentation) {
        while (*link && (uintptr_t)*link < (uintptr_t)block) {
            link = &(*link)->next;
        }
    }
    block->next = *link;
    *link = block;
    block->order = order;
    block->in_use = 0;
    mark_order_nonempty(order);
}

static ttak_buddy_block_t *list_pop_head(uint8_t order) {
    ttak_buddy_block_t *block = g_zone.free_lists[order];
    if (block) {
        g_zone.free_lists[order] = block->next;
        block->next = NULL;
        if (!g_zone.free_lists[order]) {
            mark_order_empty(order);
        }
    }
    return block;
}

static void list_remove(uint8_t order, ttak_buddy_block_t *target) {
    ttak_buddy_block_t **cur = &g_zone.free_lists[order];
    while (*cur) {
        if (*cur == target) {
            *cur = target->next;
            target->next = NULL;
            if (!g_zone.free_lists[order]) {
                mark_order_empty(order);
            }
            return;
        }
        cur = &((*cur)->next);
    }
}

/* Caller holds g_buddy_lock. */
static void buddy_defragment_locked(void) {
    for (uint8_t order = TTAK_BUDDY_MIN_ORDER; order < g_zone.max_order; ++order) {
        ttak_buddy_block_t *block = g_zone.free_lists[order];
        while (block) {
            ttak_buddy_block_t *next = block->next;
            ttak_buddy_block_t *pair = buddy_pair(block, order);
            if (pair && !pair->in_use && pair->order == order) {
                list_remove(order, block);
                list_remove(order, pair);
                ttak_buddy_block_t *merged =
                    (block_offset(block) < block_offset(pair)) ? block : pair;
                merged->order = order + 1;
                list_push(order + 1, merged);
                block = g_zone.free_lists[order];
                continue;
            }
            block = next;
        }
    }
}

void ttak_mem_buddy_init(void *pool_start, size_t pool_len, int embedded_mode) {
    buddy_lock();
    buddy_release_owned_segments_locked();
    atomic_store_explicit(&g_free_mask, 0, memory_order_relaxed);
    memset(&g_zone, 0, sizeof(g_zone));
    atomic_store_explicit(&g_zone.pool_len, 0, memory_order_relaxed);
    atomic_store_explicit(&g_zone.bytes_in_use, 0, memory_order_relaxed);
    g_zone.embedded_mode = (uint8_t)(embedded_mode ? 1 : 0);
    g_zone.anti_fragmentation = (uint8_t)(embedded_mode ? 1 : 0);
    if (pool_start && pool_len >= order_size(TTAK_BUDDY_MIN_ORDER)) {
        buddy_add_segment_locked(pool_start, pool_len, 0U);
    }
    if (buddy_debug_enabled()) {
        size_t cap = atomic_load_explicit(&g_zone.pool_len, memory_order_relaxed);
        fprintf(stderr, "[Buddy] Initialized pool_len=%zu embedded_mode=%u segments=%u\n",
                cap, g_zone.embedded_mode, g_zone.segment_count);
    }
    buddy_unlock();
}

void ttak_mem_buddy_set_pool(void *pool_start, size_t pool_len) {
    ttak_mem_buddy_init(pool_start, pool_len, g_zone.embedded_mode);
}

/* Residue lookup for size-class selection to reduce fragmentation.
 * Caller holds g_buddy_lock. */
static ttak_buddy_block_t *select_block(uint8_t desired, ttak_priority_t priority) {
    if (desired > g_zone.max_order) {
        return NULL;
    }

    const uint64_t limit_mask =
        (g_zone.max_order >= 63U) ? ~0ULL : ((1ULL << (g_zone.max_order + 1U)) - 1ULL);
    while (true) {
        uint64_t snapshot = atomic_load_explicit(&g_free_mask, memory_order_acquire) & limit_mask;
        if (!snapshot) {
            return NULL;
        }

        uint64_t mask = snapshot & (~0ULL << desired);
        if (!mask) {
            return NULL;
        }

        uint8_t order = (priority == TTAK_PRIORITY_WORST_FIT)
                            ? ttak_buddy_msb64(mask)
                            : ttak_buddy_ctz64(mask);
        ttak_buddy_block_t *block = list_pop_head(order);
        if (block) {
            return block;
        }
        mark_order_empty(order);
    }
}

/* Caller holds g_buddy_lock. */
static void split_block(uint8_t target_order, ttak_buddy_block_t *block) {
    while (block->order > target_order) {
        uint8_t new_order = block->order - 1;
        size_t size = order_size(new_order);
        ttak_buddy_block_t *buddy =
            (ttak_buddy_block_t *)((unsigned char *)block + size);
        buddy->segment = block->segment;
        buddy->in_use = 0;
        buddy->owner_tag = 0;
        buddy->call_safety = 0;
        buddy->order = new_order;
        buddy->next = NULL;
        list_push(new_order, buddy);
        block->order = new_order;
    }
    block->in_use = 1;
}

/* Pops, splits and accounts a block of @p order, or returns NULL. */
static ttak_buddy_block_t *buddy_take_block(uint8_t order, const ttak_mem_req_t *req) {
    buddy_lock();
    ttak_buddy_block_t *block = select_block(order, req->priority);
    if (!block) {
        buddy_defragment_locked();
        block = select_block(order, req->priority);
    }
    if (block) {
        split_block(order, block);
        buddy_account_alloc(block->order);
        block->owner_tag = req->owner_tag;
        block->call_safety = req->call_safety;
    }
    buddy_unlock();
    return block;
}

void *ttak_mem_buddy_alloc(const ttak_mem_req_t *req) {
    if (!req) {
        return NULL;
    }

    /* Overflow check */
    if (req->size_bytes > (size_t)-1 - TTAK_BUDDY_HEADER_SIZE) {
        fprintf(stderr, "[Buddy] ENOMEM(12): Requested size %zu overflows size_t.\n", req->size_bytes);
        return NULL;
    }

    size_t needed = req->size_bytes + TTAK_BUDDY_HEADER_SIZE;
    uint8_t order = order_for_size(needed);
    size_t block_bytes = order_size(order);

    if (!g_zone.pool_start && g_zone.embedded_mode) {
        return NULL;
    }

    if (order > g_zone.max_order && !g_zone.embedded_mode) {
        if (buddy_expand_zone(block_bytes)) {
            /* max_order updated inside buddy_expand_zone */
        }
    }
    if (order > g_zone.max_order) {
        fprintf(stderr, "[Buddy] ENOMEM(12): Requested size %zu + header exceeds max_order %u. g_zone: pool_len=%zu\n",
                req->size_bytes, g_zone.max_order, buddy_capacity());
        return NULL;
    }

    if (!g_zone.embedded_mode && buddy_should_grow(block_bytes)) {
        buddy_expand_zone(block_bytes);
    }

    ttak_buddy_block_t *block = buddy_take_block(order, req);
    if (!block && !g_zone.embedded_mode) {
        /* Hit capacity: grow silently instead of surfacing ENOMEM immediately. */
        if (buddy_expand_zone(block_bytes)) {
            block = buddy_take_block(order, req);
        }
    }
    if (!block) {
        fprintf(stderr, "[Buddy] ENOMEM(12): Allocation failed for needed size %zu (order %u). Current pool state: pool_len=%zu, max_order=%u\n",
                needed, order, buddy_capacity(), g_zone.max_order);
        return NULL;
    }
    return (unsigned char *)block + TTAK_BUDDY_HEADER_SIZE;
}

static void ttak_buddy_cleanup(void *ptr) {
    if (!ptr) return;
    ttak_buddy_block_t *block = (ttak_buddy_block_t *)ptr;
    buddy_lock();
    buddy_account_free(block->order);
    while (true) {
        uint8_t order = block->order;
        block->in_use = 0;
        if (order >= g_zone.max_order) {
            list_push(order, block);
            break;
        }
        ttak_buddy_block_t *pair = buddy_pair(block, order);
        if (!pair || pair->in_use || pair->order != order) {
            list_push(order, block);
            break;
        }
        list_remove(order, pair);
        block = (block_offset(block) < block_offset(pair)) ? block : pair;
        block->order = order + 1U;
    }
    buddy_unlock();
}

/* Frees immediately, like free() behind ttak_dangerous_free() on hosted
 * builds. Deferring through ttak_epoch_retire() held every block until
 * someone called ttak_epoch_reclaim(), which nothing does on bare metal, and
 * cost a pool block per retire node. */
void ttak_mem_buddy_free(void *ptr) {
    if (!ptr) return;
    ttak_buddy_block_t *block = (ttak_buddy_block_t *)((unsigned char *)ptr - TTAK_BUDDY_HEADER_SIZE);
    block->owner_tag = 0;
    block->call_safety = 0;
    ttak_buddy_cleanup(block);
}

void ttak_mem_buddy_get_stats(ttak_mem_buddy_stats_t *out) {
    if (!out) return;
    buddy_lock();
    uint64_t mask = atomic_load_explicit(&g_free_mask, memory_order_relaxed);
    out->capacity = buddy_capacity();
    out->bytes_in_use = atomic_load_explicit(&g_zone.bytes_in_use, memory_order_relaxed);
    out->largest_free = mask ? order_size(ttak_buddy_msb64(mask)) : 0;
    buddy_unlock();
}
