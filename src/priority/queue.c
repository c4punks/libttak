#include <ttak/priority/internal/queue.h>
#include <ttak/mem/mem.h>
#include <stddef.h>
#include <stdio.h>

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
    struct __internal_ttak_qnode_t *node = (struct __internal_ttak_qnode_t *)ttak_dangerous_alloc(sizeof(struct __internal_ttak_qnode_t));
    if (!node) {
#ifdef TTAK_DEBUG_QUEUE
        fprintf(stderr, "[queue] push failed: node alloc failed\n");
#endif
        return;
    }
#ifdef TTAK_DEBUG_QUEUE
    fprintf(stderr, "[queue] push task=%p priority=%d shard=%p head=%p\n", (void*)task, priority, (void*)q, (void*)q->head);
#endif
    node->task = task;
    node->priority = priority;
    node->next = NULL;
    if (!q->head || q->head->priority < priority) {
        node->next = q->head;
        q->head = node;
    } else {
        struct __internal_ttak_qnode_t *current = q->head;
        while (current->next && current->next->priority >= priority) {
            current = current->next;
        }
        node->next = current->next;
        current->next = node;
    }
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
    if (!q || !q->head) return NULL;
    struct __internal_ttak_qnode_t *node = q->head;
    
    ttak_task_t *task = node->task;
    q->head = node->next;
    q->size--;
    ttak_dangerous_free(node);
#ifdef TTAK_DEBUG_QUEUE
    fprintf(stderr, "[queue] pop task=%p shard=%p new_head=%p size=%zu\n", (void*)task, (void*)q, (void*)q->head, q->size);
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
    while (q->head == NULL) {
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
    q->head = NULL;
    q->size = 0;
    q->cap = 0;
    q->push = q_push;
    q->pop = q_pop;
    q->pop_blocking = q_pop_blocking;
    q->get_size = q_get_size;
    q->get_cap = q_get_cap;
}
