#include <ttak/priority/internal/queue.h>
#include <ttak/mem/mem.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief Max-heap comparator on the priority score.
 *
 * Heap elements encode (priority, item slot) into the pointer value:
 * the upper 32 bits hold the sign-preserved priority and the lower 32
 * bits hold the index into q->items.  The comparator orders by priority
 * so that the highest priority score sits at the heap root, matching
 * the pop order of the former sorted list (higher priority value
 * dequeued first).
 */
static int q_heap_cmp(const void *a, const void *b) {
    int32_t pa = (int32_t)((uintptr_t)a >> 32);
    int32_t pb = (int32_t)((uintptr_t)b >> 32);
    return (pa > pb) - (pa < pb);
}

/**
 * @brief Grow a buffer, replacing it via copy when out of capacity.
 *
 * @param ptr     Current buffer pointer.
 * @param count   Current element count in use.
 * @param cap     Current capacity (updated on success).
 * @param elem    Element size.
 * @return New buffer pointer, or NULL on allocation failure.
 */
static void *q_grow_buffer(void *ptr, size_t count, size_t *cap, size_t elem) {
    size_t new_cap = (*cap == 0) ? 16 : (*cap * 2);
    void *next = ttak_dangerous_alloc(new_cap * elem);
    if (!next) return NULL;
    if (ptr && count > 0) memcpy(next, ptr, count * elem);
    ttak_dangerous_free(ptr);
    *cap = new_cap;
    return next;
}

/**
 * @brief Insert a task into the queue according to its priority.
 *
 * @param q        Queue to update.
 * @param task     Task to enqueue.
 * @param priority Priority score (higher = sooner).
 * @param now      Timestamp for allocations.
 */
static void q_push(struct __internal_ttak_proc_priority_queue_t *q, ttak_task_t *task, int priority, uint64_t now) {
    (void)now;
    if (!q) return;
#ifdef TTAK_DEBUG_QUEUE
    fprintf(stderr, "[queue] push task=%p priority=%d shard=%p size=%zu\n", (void*)task, priority, (void*)q, q->size);
#endif
    size_t slot;
    if (q->free_count > 0) {
        slot = q->free_slots[--q->free_count];
    } else {
        if (q->items_count == q->items_cap) {
            void *grown = q_grow_buffer(q->items, q->items_count, &q->items_cap, sizeof(*q->items));
            if (!grown) {
#ifdef TTAK_DEBUG_QUEUE
                fprintf(stderr, "[queue] push failed: item alloc failed\n");
#endif
                return;
            }
            q->items = (struct __internal_ttak_qnode_t *)grown;
        }
        slot = q->items_count++;
    }
    q->items[slot].task = task;
    q->items[slot].priority = priority;
    /* Slot + 1 keeps the encoded element non-NULL even for priority 0 at
     * slot 0; the heap uses a NULL return to signal emptiness, so a NULL
     * payload would be dropped and desync q->size from the heap. */
    void *element = (void *)(((uintptr_t)(uint32_t)(int32_t)priority << 32) | (uintptr_t)(uint32_t)(slot + 1));
    ttak_heap_tree_push(&q->heap, element, now);
    q->size++;
}

/**
 * @brief Remove the highest-priority task if one exists.
 *
 * @param q   Queue to pop from.
 * @param now Timestamp for pointer access validation.
 * @return Task pointer or NULL when empty.
 */
static ttak_task_t *q_pop(struct __internal_ttak_proc_priority_queue_t *q, uint64_t now) {
    (void)now;
    if (!q || q->size == 0) return NULL;
    /* Emptiness is determined by q->size above; the popped element is a
     * (priority, slot+1) encoding and is never NULL for a stored entry. */
    void *element = ttak_heap_tree_pop(&q->heap, now);
    size_t slot = (size_t)(uint32_t)(uintptr_t)element - 1;
    ttak_task_t *task = q->items[slot].task;
    if (q->free_count == q->free_cap) {
        void *grown = q_grow_buffer(q->free_slots, q->free_count, &q->free_cap, sizeof(*q->free_slots));
        if (!grown) {
            /* Out of memory: leak the slot rather than corrupting the heap. */
            q->size--;
            return task;
        }
        q->free_slots = (size_t *)grown;
    }
    q->free_slots[q->free_count++] = slot;
    q->size--;
#ifdef TTAK_DEBUG_QUEUE
    fprintf(stderr, "[queue] pop task=%p shard=%p size=%zu\n", (void*)task, (void*)q, q->size);
#endif
    return task;
}

/**
 * @brief Block until a task is available, then pop it.
 *
 * @param q     Queue to pop from.
 * @param mutex Mutex associated with the condition variable.
 * @param cond  Condition variable signaled when tasks arrive.
 * @param now   Timestamp for pointer validation.
 * @return Popped task pointer.
 */
static ttak_task_t *q_pop_blocking(struct __internal_ttak_proc_priority_queue_t *q, pthread_mutex_t *mutex, pthread_cond_t *cond, uint64_t now) {
    if (!q) return NULL;
    while (q->size == 0) {
        pthread_cond_wait(cond, mutex);
    }
    return q_pop(q, now);
}

/**
 * @brief Return the number of queued tasks.
 *
 * @param q Queue to inspect.
 * @return Element count.
 */
static size_t q_get_size(struct __internal_ttak_proc_priority_queue_t *q) {
    return q ? q->size : 0;
}

/**
 * @brief Return the configured capacity hint.
 *
 * @param q Queue to inspect.
 * @return Capacity stored on the queue.
 */
static size_t q_get_cap(struct __internal_ttak_proc_priority_queue_t *q) {
    return q ? q->cap : 0;
}

/**
 * @brief Initialize the process priority queue dispatch table.
 *
 * @param q Queue structure to configure.
 */
void ttak_priority_queue_init(struct __internal_ttak_proc_priority_queue_t *q) {
    if (!q) return;
    ttak_heap_tree_init(&q->heap, 16, q_heap_cmp);
    q->items = NULL;
    q->items_count = 0;
    q->items_cap = 0;
    q->free_slots = NULL;
    q->free_count = 0;
    q->free_cap = 0;
    q->size = 0;
    q->cap = 0;
    q->push = q_push;
    q->pop = q_pop;
    q->pop_blocking = q_pop_blocking;
    q->get_size = q_get_size;
    q->get_cap = q_get_cap;
}
