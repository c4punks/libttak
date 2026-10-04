/**
 * @file ttak_mem_vma.c
 * @brief Region-backed medium and large allocators.
 *
 * Dual-Path Architecture:
 *   - OS Managed (TTAK_OS_MANAGED_MEMORY == 1): Each allocation is satisfied
 *     by a direct OS native call (mmap / VirtualAlloc).  The returned block is
 *     unmapped on free so RSS drops immediately.
 *   - Legacy / Embedded (TTAK_OS_MANAGED_MEMORY == 0): Uses static
 *     segregated free-list regions with split/coalescing inside pre-reserved
 *     buffers.
 */

#include <string.h>
#include <stdio.h>
#include <stddef.h>

#include "../../internal/ttak/mem_internal.h"
#include "../../include/ttak/mem/mem.h"
#include <ttak/mem/fastpath.h>

#if !TTAK_OS_MANAGED_MEMORY

typedef struct ttak_region_block_t {
    size_t size;
    struct ttak_region_block_t *prev;
    struct ttak_region_block_t *next;
    struct ttak_region_block_t *free_next;
    uint8_t is_free;
} ttak_region_block_t;

typedef struct ttak_region_allocator_t {
    uint8_t *base;
    size_t len;
    ttak_region_block_t *head;
    ttak_region_block_t *free_bins[16];
    pthread_mutex_t lock;
} ttak_region_allocator_t;

#define TTAK_MIN_SPLIT_BLOCK 256
#define TTAK_BIN_COUNT 16

static _Alignas(64) uint8_t vma_region_buffer[TTAK_VMA_REGION_SIZE];
static _Alignas(64) uint8_t large_region_buffer[TTAK_LARGE_REGION_SIZE];

static ttak_region_allocator_t vma_allocator = {
    .base = vma_region_buffer,
    .len = sizeof(vma_region_buffer),
    .head = NULL,
    .free_bins = {0},
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static ttak_region_allocator_t large_allocator = {
    .base = large_region_buffer,
    .len = sizeof(large_region_buffer),
    .head = NULL,
    .free_bins = {0},
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

ttak_mem_vma_region_t global_vma_region = {
    .start_addr = vma_region_buffer,
    .current_cursor = (uintptr_t)vma_region_buffer,
};

static int size_to_bin(size_t sz) {
    size_t limit = 512;
    for (int i = 0; i < TTAK_BIN_COUNT - 1; ++i) {
        if (sz <= limit) return i;
        limit <<= 1;
    }
    return TTAK_BIN_COUNT - 1;
}

static inline size_t payload_offset(void) {
    size_t off = sizeof(ttak_mem_header_t);
    return (off + TTAK_VMA_ALIGNMENT - 1) & ~((size_t)TTAK_VMA_ALIGNMENT - 1);
}

static void free_bin_insert(ttak_region_allocator_t *alloc, ttak_region_block_t *blk) {
    int bin = size_to_bin(blk->size);
    blk->free_next = alloc->free_bins[bin];
    alloc->free_bins[bin] = blk;
}

static void free_bin_remove(ttak_region_allocator_t *alloc, ttak_region_block_t *blk) {
    int bin = size_to_bin(blk->size);
    ttak_region_block_t *prev = NULL;
    ttak_region_block_t *cur = alloc->free_bins[bin];
    while (cur) {
        if (cur == blk) {
            if (prev) prev->free_next = cur->free_next;
            else alloc->free_bins[bin] = cur->free_next;
            blk->free_next = NULL;
            return;
        }
        prev = cur;
        cur = cur->free_next;
    }
}

static void region_init_once(ttak_region_allocator_t *alloc) {
    if (alloc->head) return;
    ttak_region_block_t *head = (ttak_region_block_t *)alloc->base;
    head->size = alloc->len - payload_offset();
    head->prev = NULL;
    head->next = NULL;
    head->free_next = NULL;
    head->is_free = 1;
    alloc->head = head;
    free_bin_insert(alloc, head);
}

static ttak_region_block_t *region_find_fit(ttak_region_allocator_t *alloc, size_t req_size) {
    int bin = size_to_bin(req_size);
    for (int i = bin; i < TTAK_BIN_COUNT; ++i) {
        ttak_region_block_t *cur = alloc->free_bins[i];
        while (cur) {
            if (cur->is_free && cur->size >= req_size) return cur;
            cur = cur->free_next;
        }
    }
    return NULL;
}

static void region_split_block(ttak_region_allocator_t *alloc, ttak_region_block_t *blk, size_t req_size) {
    if (blk->size < req_size + payload_offset() + TTAK_MIN_SPLIT_BLOCK) {
        return;
    }

    uint8_t *new_addr = ((uint8_t *)blk + payload_offset() + req_size);
    ttak_region_block_t *new_blk = (ttak_region_block_t *)new_addr;
    new_blk->size = blk->size - req_size - payload_offset();
    new_blk->prev = blk;
    new_blk->next = blk->next;
    new_blk->free_next = NULL;
    new_blk->is_free = 1;
    if (blk->next) blk->next->prev = new_blk;
    blk->next = new_blk;
    blk->size = req_size;
    free_bin_insert(alloc, new_blk);
}

static ttak_region_block_t *region_coalesce(ttak_region_allocator_t *alloc, ttak_region_block_t *blk) {
    if (blk->prev && blk->prev->is_free) {
        ttak_region_block_t *left = blk->prev;
        free_bin_remove(alloc, left);
        left->size += payload_offset() + blk->size;
        left->next = blk->next;
        if (blk->next) blk->next->prev = left;
        blk = left;
    }

    if (blk->next && blk->next->is_free) {
        ttak_region_block_t *right = blk->next;
        free_bin_remove(alloc, right);
        blk->size += payload_offset() + right->size;
        blk->next = right->next;
        if (right->next) right->next->prev = blk;
    }

    return blk;
}

static ttak_mem_header_t *region_alloc(ttak_region_allocator_t *alloc, size_t user_requested_size) {
    if (user_requested_size == 0 || user_requested_size > SIZE_MAX - sizeof(ttak_mem_header_t)) return NULL;

    size_t total_alloc_size = sizeof(ttak_mem_header_t) + user_requested_size;
    if (total_alloc_size > SIZE_MAX - (TTAK_VMA_ALIGNMENT - 1)) return NULL;
    size_t aligned_total_alloc_size = (total_alloc_size + TTAK_VMA_ALIGNMENT - 1) & ~((size_t)TTAK_VMA_ALIGNMENT - 1);

    pthread_mutex_lock(&alloc->lock);
    region_init_once(alloc);

    ttak_region_block_t *blk = region_find_fit(alloc, aligned_total_alloc_size);
    if (!blk) {
        pthread_mutex_unlock(&alloc->lock);
        return NULL;
    }

    free_bin_remove(alloc, blk);
    blk->is_free = 0;
    region_split_block(alloc, blk, aligned_total_alloc_size);

    ttak_mem_header_t *header = (ttak_mem_header_t *)((uint8_t *)blk + payload_offset());
    ttak_mem_stream_zero(header, aligned_total_alloc_size);
    pthread_mutex_unlock(&alloc->lock);
    return header;
}

static void region_free(ttak_region_allocator_t *alloc, ttak_mem_header_t *header) {
    if (!header) return;

    pthread_mutex_lock(&alloc->lock);
    ttak_region_block_t *blk = (ttak_region_block_t *)((uint8_t *)header - payload_offset());
    blk->is_free = 1;
    blk = region_coalesce(alloc, blk);
    free_bin_insert(alloc, blk);
    pthread_mutex_unlock(&alloc->lock);
}

#endif /* !TTAK_OS_MANAGED_MEMORY */

/* ------------------------------------------------------------------ */
/*  Public tier entry points                                           */
/* ------------------------------------------------------------------ */

ttak_mem_header_t* ttak_mem_vma_alloc_internal(size_t user_requested_size) {
#if TTAK_OS_MANAGED_MEMORY
    size_t total = sizeof(ttak_mem_header_t) + user_requested_size;
    total = (total + TTAK_VMA_ALIGNMENT - 1) & ~((size_t)TTAK_VMA_ALIGNMENT - 1);
    ttak_mem_header_t *header = (ttak_mem_header_t *)ttak_os_mem_alloc(total);
    if (header) {
        memset(header, 0, total);
    }
    return header;
#else
    return region_alloc(&vma_allocator, user_requested_size);
#endif
}

void _vma_free_internal(ttak_mem_header_t* header) {
#if TTAK_OS_MANAGED_MEMORY
    if (!header) return;
    ttak_os_mem_free(header, header->mapped_size);
#else
    region_free(&vma_allocator, header);
#endif
}

ttak_mem_header_t* ttak_mem_large_alloc_internal(size_t user_requested_size) {
#if TTAK_OS_MANAGED_MEMORY
    size_t total = sizeof(ttak_mem_header_t) + user_requested_size;
    total = (total + TTAK_VMA_ALIGNMENT - 1) & ~((size_t)TTAK_VMA_ALIGNMENT - 1);
    ttak_mem_header_t *header = (ttak_mem_header_t *)ttak_os_mem_alloc(total);
    if (header) {
        memset(header, 0, total);
    }
    return header;
#else
    return region_alloc(&large_allocator, user_requested_size);
#endif
}

void _large_free_internal(ttak_mem_header_t* header) {
#if TTAK_OS_MANAGED_MEMORY
    if (!header) return;
    ttak_os_mem_free(header, header->mapped_size);
#else
    region_free(&large_allocator, header);
#endif
}
