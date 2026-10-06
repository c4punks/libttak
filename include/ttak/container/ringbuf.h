#ifndef TTAK_CONTAINER_RINGBUF_H
#define TTAK_CONTAINER_RINGBUF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <ttak/sync/sync.h>

/**
 * @brief Thread-safe Ring Buffer structure.
 * 
 * Uses a read-write lock to allow multiple readers or exclusive writers.
 * (Note: Standard ring buffers are often single-producer/single-consumer lock-free,
 * this one is generic and locked).
 */
typedef struct ttak_ringbuf {
    void *buffer;       /**< Internal data buffer. */
    size_t item_size;   /**< Size of each item. */
    size_t capacity;    /**< Maximum number of items. */
    size_t mask;        /**< Power-of-two mask for fast index wrapping. */
    size_t head;        /**< Write index (where next item goes). */
    size_t tail;        /**< Read index (where next item is taken). */
    bool full;          /**< Flag indicating buffer is full (distinguishes empty vs full). */
    ttak_rwlock_t lock; /**< Lock for thread safety. */
} ttak_ringbuf_t;

typedef ttak_ringbuf_t tt_ringbuf_t;

/**
 * @brief Creates a ring buffer.
 * 
 * @param capacity Number of items.
 * @param item_size Size of each item.
 * @return Pointer to ring buffer.
 */
ttak_ringbuf_t *ttak_ringbuf_create(size_t capacity, size_t item_size);

/**
 * @brief Destroys ring buffer.
 */
void ttak_ringbuf_destroy(ttak_ringbuf_t *rb);

/**
 * @brief Pushes an item to the buffer.
 * 
 * @param rb Buffer.
 * @param item Pointer to data to copy in.
 * @return true if pushed, false if full.
 */
bool ttak_ringbuf_push(ttak_ringbuf_t *rb, const void *item);

/**
 * @brief Pops an item from the buffer.
 * 
 * @param rb Buffer.
 * @param out_item Pointer to copy data out to.
 * @return true if popped, false if empty.
 */
bool ttak_ringbuf_pop(ttak_ringbuf_t *rb, void *out_item);

/**
 * @brief Checks if empty.
 */
bool ttak_ringbuf_is_empty(ttak_ringbuf_t *rb);

/**
 * @brief Checks if full.
 */
bool ttak_ringbuf_is_full(ttak_ringbuf_t *rb);

/**
 * @brief Returns current item count.
 */
size_t ttak_ringbuf_count(ttak_ringbuf_t *rb);

#endif // TTAK_CONTAINER_RINGBUF_H