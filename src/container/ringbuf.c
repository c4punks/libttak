#include <ttak/container/ringbuf.h>
#include <ttak/mem/mem.h>
#include <ttak/timing/timing.h>
#include <ttak/arch/ttak_critical.h>
#include <string.h>
#include <stdlib.h>

static bool ringbuf_is_empty_locked(const ttak_ringbuf_t *rb);
static bool ringbuf_push_unlocked(ttak_ringbuf_t *rb, const void *item);
static bool ringbuf_pop_unlocked(ttak_ringbuf_t *rb, void *out_item);

static inline size_t next_power_of_two(size_t x) {
    if (x <= 1) return 1;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
#if SIZE_MAX > 0xFFFFFFFFULL
    x |= x >> 32;
#endif
    return x + 1;
}

/**
 * @brief Initializes a ring buffer in caller-provided storage (zero-allocation).
 */
bool ttak_ringbuf_init_static(ttak_ringbuf_t *rb, size_t capacity, size_t item_size,
                              void *storage, size_t storage_size) {
    if (!rb || !capacity || !item_size) return false;
    size_t alloc_cap = next_power_of_two(capacity);
    if (alloc_cap < capacity || alloc_cap > SIZE_MAX / item_size) return false;

    size_t needed = TTAK_STATIC_ALIGN_UP(alloc_cap * item_size, TTAK_STATIC_STORAGE_ALIGN)
                    + TTAK_STATIC_STORAGE_ALIGN;
    if (!storage || storage_size < needed) return false;

    void *aligned = (void *)TTAK_STATIC_ALIGN_UP((uintptr_t)storage, TTAK_STATIC_STORAGE_ALIGN);
    *rb = (ttak_ringbuf_t){
        .buffer = aligned,
        .capacity = capacity,
        .item_size = item_size,
        .mask = (alloc_cap == capacity) ? (alloc_cap - 1) : 0,
        .head = 0,
        .tail = 0,
        .full = false,
    };
    ttak_rwlock_init(&rb->lock);
    return true;
}

#ifndef TTAK_NO_MALLOC
/**
 * @brief Creates ring buffer.
 */
ttak_ringbuf_t *ttak_ringbuf_create(size_t capacity, size_t item_size) {
    if (!capacity || !item_size) return NULL;
    if (capacity > SIZE_MAX / item_size) return NULL;

    size_t alloc_cap = next_power_of_two(capacity);
    /* Check overflow when rounding capacity to next power of two */
    if (alloc_cap < capacity || alloc_cap > SIZE_MAX / item_size) return NULL;

    ttak_ringbuf_t *rb = malloc(sizeof(ttak_ringbuf_t));
    if (!rb) return NULL;
    
    // Using simple malloc for internal buffer to avoid lifecycle complexity inside ringbuf
    rb->buffer = malloc(alloc_cap * item_size);
    if (!rb->buffer) {
        free(rb);
        return NULL;
    }
    
    rb->capacity = capacity;
    rb->item_size = item_size;
    rb->mask = (alloc_cap == capacity) ? (alloc_cap - 1) : 0;
    rb->head = 0;
    rb->tail = 0;
    rb->full = false;
    ttak_rwlock_init(&rb->lock);
    
    return rb;
}

/**
 * @brief Destroys ring buffer.
 */
void ttak_ringbuf_destroy(ttak_ringbuf_t *rb) {
    if (rb) {
        free(rb->buffer);
        ttak_rwlock_destroy(&rb->lock);
        free(rb);
    }
}
#endif /* TTAK_NO_MALLOC */

/**
 * @brief Pushes item (copy).
 */
bool ttak_ringbuf_push(ttak_ringbuf_t *rb, const void *item) {
    ttak_rwlock_wrlock(&rb->lock);
    bool ok = ringbuf_push_unlocked(rb, item);
    ttak_rwlock_unlock(&rb->lock);
    return ok;
}

/**
 * @brief Pops item (copy).
 */
bool ttak_ringbuf_pop(ttak_ringbuf_t *rb, void *out_item) {
    ttak_rwlock_wrlock(&rb->lock);
    bool ok = ringbuf_pop_unlocked(rb, out_item);
    ttak_rwlock_unlock(&rb->lock);
    return ok;
}

/**
 * @brief ISR-context push (see ringbuf.h): never takes the rwlock, so it
 * cannot deadlock against a task holding it. Both sides of an
 * ISR-shared buffer must use the _isr variants.
 */
bool ttak_ringbuf_push_isr(ttak_ringbuf_t *rb, const void *item) {
    ttak_critical_state_t state;
    TTAK_ENTER_CRITICAL_ISR(state);
    bool ok = ringbuf_push_unlocked(rb, item);
    TTAK_EXIT_CRITICAL_ISR(state);
    return ok;
}

/**
 * @brief ISR-context pop (see ttak_ringbuf_push_isr).
 */
bool ttak_ringbuf_pop_isr(ttak_ringbuf_t *rb, void *out_item) {
    ttak_critical_state_t state;
    TTAK_ENTER_CRITICAL_ISR(state);
    bool ok = ringbuf_pop_unlocked(rb, out_item);
    TTAK_EXIT_CRITICAL_ISR(state);
    return ok;
}

static bool ringbuf_push_unlocked(ttak_ringbuf_t *rb, const void *item) {
    if (rb->full) {
        return false;
    }

    char *dest = (char *)rb->buffer + (rb->head * rb->item_size);
    memcpy(dest, item, rb->item_size);

    size_t next_head = rb->mask ? ((rb->head + 1) & rb->mask)
                                : ((rb->head + 1 < rb->capacity) ? (rb->head + 1) : 0);
    rb->head = next_head;
    if (rb->head == rb->tail) {
        rb->full = true;
    }
    return true;
}

static bool ringbuf_pop_unlocked(ttak_ringbuf_t *rb, void *out_item) {
    if (ringbuf_is_empty_locked(rb)) {
        return false;
    }

    char *src = (char *)rb->buffer + (rb->tail * rb->item_size);
    if (out_item) {
        memcpy(out_item, src, rb->item_size);
    }

    size_t next_tail = rb->mask ? ((rb->tail + 1) & rb->mask)
                                : ((rb->tail + 1 < rb->capacity) ? (rb->tail + 1) : 0);
    rb->tail = next_tail;
    rb->full = false;
    return true;
}

static bool ringbuf_is_empty_locked(const ttak_ringbuf_t *rb) {
    return (!rb->full && (rb->head == rb->tail));
}

bool ttak_ringbuf_is_empty(ttak_ringbuf_t *rb) {
    ttak_rwlock_rdlock(&rb->lock);
    bool empty = ringbuf_is_empty_locked(rb);
    ttak_rwlock_unlock(&rb->lock);
    return empty;
}

bool ttak_ringbuf_is_full(ttak_ringbuf_t *rb) {
    ttak_rwlock_rdlock(&rb->lock);
    bool full = rb->full;
    ttak_rwlock_unlock(&rb->lock);
    return full;
}

size_t ttak_ringbuf_count(ttak_ringbuf_t *rb) {
    ttak_rwlock_rdlock(&rb->lock);
    size_t count;
    if (rb->full) count = rb->capacity;
    else if (rb->head >= rb->tail) count = rb->head - rb->tail;
    else count = rb->capacity + rb->head - rb->tail;
    ttak_rwlock_unlock(&rb->lock);
    return count;
}