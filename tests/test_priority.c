#include <ttak/priority/queue.h>
#include <ttak/async/task.h>
#include <stdint.h>
#include "test_macros.h"

void *dummy_func(void *arg) { (void)arg; return NULL; }

void test_priority_queue_basic(void) {
    struct __internal_ttak_proc_priority_queue_t q;
    ttak_priority_queue_init(&q);
    
    uint64_t now = 100;
    ttak_task_t *t1 = ttak_task_create(dummy_func, NULL, NULL, now);
    ttak_task_t *t2 = ttak_task_create(dummy_func, NULL, NULL, now);
    
    q.push(&q, t1, 10, now);
    q.push(&q, t2, 20, now); // Higher priority
    
    ASSERT(q.get_size(&q) == 2);
    
    ttak_task_t *p1 = q.pop(&q, now);
    ASSERT(p1 == t2); // t2 has higher priority (20 > 10)
    
    ttak_task_t *p2 = q.pop(&q, now);
    ASSERT(p2 == t1);
    
    ASSERT(q.get_size(&q) == 0);
    
    ttak_task_destroy(t1, now);
    ttak_task_destroy(t2, now);
}

/* A priority-0 task in heap slot 0 must survive push/pop (it used to
 * encode to NULL and be silently dropped). */
void test_priority_queue_zero_priority_slot0(void) {
    struct __internal_ttak_proc_priority_queue_t q;
    ttak_priority_queue_init(&q);

    uint64_t now = 100;
    ttak_task_t *t0 = ttak_task_create(dummy_func, NULL, NULL, now);
    ASSERT(t0 != NULL);

    q.push(&q, t0, 0, now);
    ASSERT(q.get_size(&q) == 1);
    ASSERT(q.pop(&q, now) == t0);
    ASSERT(q.get_size(&q) == 0);

    /* Slot-0 entry buried under higher priorities must still come out. */
    q.push(&q, t0, 0, now);
    for (int i = 0; i < 32; i++) {
        q.push(&q, t0, i + 1, now);
    }
    ASSERT(q.get_size(&q) == 33);
    for (int i = 0; i < 32; i++) {
        ASSERT(q.pop(&q, now) == t0);
    }
    ASSERT(q.pop(&q, now) == t0);
    ASSERT(q.get_size(&q) == 0);

    ttak_task_destroy(t0, now);
}

void test_priority_queue_heap_stress(void) {
    struct __internal_ttak_proc_priority_queue_t q;
    ttak_priority_queue_init(&q);

    uint64_t now = 100;
    const int N = 5000;
    ttak_task_t *t = ttak_task_create(dummy_func, NULL, NULL, now);
    ASSERT(t != NULL);

    /* Deterministic LCG; skewed toward 0 with duplicates to exercise the
     * free list and the comparator. */
    uint32_t rng = 0x12345678u;
    int model[5000];
    for (int i = 0; i < N; i++) {
        rng = rng * 1664525u + 1013904223u;
        int p;
        switch (rng % 4) {
        case 0: p = 0; break;
        case 1: p = 0; break;   /* half of all pushes are priority 0 */
        case 2: p = 1; break;
        default: p = 7; break;
        }
        if (i % 997 == 0) p = 42;
        model[i] = p;
        q.push(&q, t, p, now);
    }
    ASSERT(q.get_size(&q) == (size_t)N);

    /* Pop all and check order against a max-extraction model. */
    char removed[5000] = {0};
    int prev = INT32_MAX;
    for (int k = 0; k < N; k++) {
        ttak_task_t *popped = q.pop(&q, now);
        ASSERT(popped == t);
        int best = -1;
        for (int i = 0; i < N; i++) {
            if (!removed[i] && (best < 0 || model[i] > model[best])) best = i;
        }
        ASSERT(best >= 0);
        removed[best] = 1;
        ASSERT_MSG(model[best] <= prev,
                   "pop %d: priority %d after %d (not non-increasing)",
                   k, model[best], prev);
        prev = model[best];
    }
    ASSERT(q.get_size(&q) == 0);
    ASSERT(q.pop(&q, now) == NULL);

    ttak_task_destroy(t, now);
}

int main(void) {
    RUN_TEST(test_priority_queue_basic);
    RUN_TEST(test_priority_queue_zero_priority_slot0);
    RUN_TEST(test_priority_queue_heap_stress);
    return 0;
}
