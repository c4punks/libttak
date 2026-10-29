#ifndef TTAK_CONTAINER_RINGBUF_H
#define TTAK_CONTAINER_RINGBUF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <ttak/sync/sync.h>
#include <ttak/compat/static_alloc.h>

/**
 * @brief Bytes of caller-provided storage required for a static ring buffer.
 *
 * Covers the item buffer (capacity rounded up to a power of two, matching
 * the dynamic constructor) plus base alignment slack:
 *   static uint8_t storage[TTAK_RINGBUF_STATIC_STORAGE_SIZE(64, sizeof(item))];
 */
#define TTAK_RINGBUF_STATIC_STORAGE_SIZE(capacity, item_size) \
    TTAK_STATIC_ALIGN_UP( \
        TTAK_STATIC_POW2(capacity) * (item_size) + TTAK_STATIC_STORAGE_ALIGN, \
        TTAK_STATIC_STORAGE_ALIGN)

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
 * @brief Initializes a ring buffer in caller-provided storage (zero-allocation).
 *
 * @param rb           Ring buffer structure to initialize (caller-owned).
 * @param capacity     Number of items.
 * @param item_size    Size of each item.
 * @param storage      Backing storage for the item buffer; must hold at
 *                     least TTAK_RINGBUF_STATIC_STORAGE_SIZE(capacity,
 *                     item_size) bytes.
 * @param storage_size Size of @p storage in bytes.
 * @return true on success, false if arguments or storage size are invalid.
 */
bool ttak_ringbuf_init_static(ttak_ringbuf_t *rb, size_t capacity, size_t item_size,
                              void *storage, size_t storage_size);

/**
 * @brief Destroys ring buffer.
 */
void ttak_ringbuf_destroy(ttak_ringbuf_t *rb);

/**
 * @brief Pushes an item to the buffer.
 *
 * Task context only. Do not mix with the _isr variants on the same
 * buffer; see ttak_ringbuf_push_isr().
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
 * @brief ISR-context push: callable only from interrupt context.
 *
 * Never takes the rwlock (a pthread lock is illegal in ISR); the
 * operation is serialized by the ISR critical section instead. If the
 * buffer is shared between a task and an ISR, both sides must use the
 * _isr variants — do not mix with the task-context APIs.
 */
bool ttak_ringbuf_push_isr(ttak_ringbuf_t *rb, const void *item);

/**
 * @brief ISR-context pop: callable only from interrupt context.
 *
 * Same locking rules as ttak_ringbuf_push_isr().
 */
bool ttak_ringbuf_pop_isr(ttak_ringbuf_t *rb, void *out_item);

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