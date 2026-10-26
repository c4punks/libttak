/**
 * @file buddy.h
 * @brief Power-of-two buddy allocator for embedded and bare-metal targets.
 *
 * Manages a contiguous memory pool using the classic buddy-system algorithm.
 * When @c EMBEDDED is defined to 1 the allocator operates without any OS
 * memory services.  Three placement policies are available: best-fit,
 * worst-fit, and first-fit.
 */

#ifndef TTAK_PHYS_MEM_BUDDY_H
#define TTAK_PHYS_MEM_BUDDY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Set to 1 to compile the allocator without OS dependencies. */
#ifndef EMBEDDED
#define EMBEDDED 0
#endif

/** @brief Allocation placement policy for the buddy allocator. */
typedef enum ttak_priority {
    TTAK_PRIORITY_BEST_FIT,   /**< Smallest block that fits (reduces waste). */
    TTAK_PRIORITY_WORST_FIT,  /**< Largest available block (keeps large holes). */
    TTAK_PRIORITY_FIRST_FIT   /**< First block that fits (fastest search). */
} ttak_priority_t;

/**
 * @brief Allocation request descriptor passed to ttak_mem_buddy_alloc().
 */
typedef struct ttak_mem_req {
    size_t size_bytes;       /**< Number of bytes to allocate. */
    ttak_priority_t priority;/**< Placement policy. */
    uint32_t owner_tag;      /**< Caller tag for debugging/tracking. */
    uint32_t call_safety;    /**< Non-zero to enable guard word checks. */
    uint32_t flags;          /**< Reserved; set to 0. */
} ttak_mem_req_t;

/** @brief Snapshot of the buddy pool, from ttak_mem_buddy_get_stats(). */
typedef struct ttak_mem_buddy_stats {
    size_t capacity;      /**< Bytes managed by the pool. */
    size_t bytes_in_use;  /**< Bytes in allocated blocks, headers and rounding included. */
    size_t largest_free;  /**< Largest free block; requests (plus a header) up to this fit. */
} ttak_mem_buddy_stats_t;

/**
 * @brief Initialises the buddy allocator over a caller-supplied memory region.
 *
 * @param pool_start   Start of the memory pool.
 * @param pool_len     Total pool size in bytes.
 * @param embedded_mode Non-zero to disable OS-level features.
 */
void ttak_mem_buddy_init(void *pool_start, size_t pool_len, int embedded_mode);

/**
 * @brief Replaces the active pool without re-initialising internal state.
 *
 * @param pool_start New pool base address.
 * @param pool_len   New pool size in bytes.
 */
void ttak_mem_buddy_set_pool(void *pool_start, size_t pool_len);

/**
 * @brief Allocates a block from the buddy pool.
 *
 * @param req Allocation request specifying size and policy.
 * @return    Aligned pointer to allocated memory, or NULL on failure.
 */
void *ttak_mem_buddy_alloc(const ttak_mem_req_t *req);

/**
 * @brief Returns a previously allocated block to the buddy pool.
 *
 * @param ptr Pointer returned by ttak_mem_buddy_alloc().
 */
void ttak_mem_buddy_free(void *ptr);

/**
 * @brief Reports pool usage and fragmentation.
 *
 * A largest_free well below capacity - bytes_in_use means the free space is
 * split into small blocks.
 *
 * @param out Receives the snapshot.
 */
void ttak_mem_buddy_get_stats(ttak_mem_buddy_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TTAK_PHYS_MEM_BUDDY_H */
