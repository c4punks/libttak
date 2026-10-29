#ifndef TTAK_CONTAINER_POOL_H
#define TTAK_CONTAINER_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <ttak/sync/sync.h>
#include <ttak/sync/spinlock.h>
#include <ttak/compat/static_alloc.h>

/**
 * @brief Bytes of caller-provided storage required for a static object pool.
 *
 * Covers the item buffer and the allocation bitmap:
 *   static uint8_t storage[TTAK_POOL_STATIC_STORAGE_SIZE(128, sizeof(obj))];
 */
#define TTAK_POOL_STATIC_STORAGE_SIZE(capacity, item_size) \
    TTAK_STATIC_ALIGN_UP( \
        TTAK_STATIC_ALIGN_UP((capacity) * (item_size), TTAK_STATIC_STORAGE_ALIGN) + \
        TTAK_STATIC_ALIGN_UP(((capacity) + 7U) / 8U, TTAK_STATIC_STORAGE_ALIGN), \
        TTAK_STATIC_STORAGE_ALIGN)

/**
 * @brief Fixed-size Object Pool structure.
 * 
 * Efficiently manages a set of pre-allocated objects of the same size.
 * Uses a bitmap for allocation tracking and a spinlock for thread safety.
 */
typedef struct ttak_object_pool {
    void *buffer;           /**< Contiguous memory block holding all items. */
    uint8_t *bitmap;        /**< Bitmask indicating allocation status. */
    size_t item_size;       /**< Size of a single item. */
    size_t capacity;        /**< Total capacity of the pool. */
    size_t used_count;      /**< Number of currently allocated items. */

    /* Fast O(1) LIFO free list (valid when item_size >= sizeof(void *)). */
    void *free_list;        /**< Linked list of returned items. */

    /* Orthogonal Latin Square traversal state (order 8, 64 slots per lattice). */
    size_t ols_chunk_count;   /**< Number of 8x8 tiles covering the capacity. */
    size_t ols_chunk_cursor;  /**< Current tile cursor. */
    uint8_t ols_lane_seed;    /**< Current 6-bit lane seed within the tile. */
    uint8_t ols_lane_guard;   /**< Cycle guard to detect when to advance tiles. */
    uint8_t ols_lane_stride;  /**< Coprime stride applied to the lane seed. */
    size_t last_recycled_index; /**< Hot-slot recycled on the next allocation. */

    ttak_spin_t lock;       /**< Spinlock for protecting the bitmap/free list. */
} ttak_object_pool_t;

typedef ttak_object_pool_t tt_object_pool_t;

/**
 * @brief Creates a new object pool.
 * 
 * @param capacity Maximum number of items.
 * @param item_size Size of each item in bytes.
 * @return Pointer to the new pool, or NULL on failure.
 */
ttak_object_pool_t *ttak_object_pool_create(size_t capacity, size_t item_size);

/**
 * @brief Initializes an object pool in caller-provided storage
 * (zero-allocation).
 *
 * The item buffer and allocation bitmap are carved out of @p storage (the
 * bitmap is zero-initialized by this call; item contents are left untouched).
 *
 * @param pool         Pool structure to initialize (caller-owned).
 * @param capacity     Maximum number of items.
 * @param item_size    Size of each item in bytes.
 * @param storage      Backing storage; must hold at least
 *                     TTAK_POOL_STATIC_STORAGE_SIZE(capacity, item_size)
 *                     bytes.
 * @param storage_size Size of @p storage in bytes.
 * @return true on success, false if arguments or storage size are invalid.
 */
bool ttak_object_pool_init_static(ttak_object_pool_t *pool, size_t capacity, size_t item_size,
                                  void *storage, size_t storage_size);

/**
 * @brief Destroys the object pool and frees memory.
 * 
 * @param pool Pointer to the pool.
 */
void ttak_object_pool_destroy(ttak_object_pool_t *pool);

/**
 * @brief Allocates an object from the pool.
 * 
 * @param pool Pointer to the pool.
 * @return Pointer to the allocated object, or NULL if pool is full.
 */
void *ttak_object_pool_alloc(ttak_object_pool_t *pool);

/**
 * @brief Returns an object to the pool (frees it).
 * 
 * @param pool Pointer to the pool.
 * @param ptr Pointer to the object to free.
 */
void ttak_object_pool_free(ttak_object_pool_t *pool, void *ptr);

#endif // TTAK_CONTAINER_POOL_H
