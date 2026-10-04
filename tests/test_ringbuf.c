#include <ttak/container/ringbuf.h>
#include <pthread.h>
#include <stdatomic.h>
#include "test_macros.h"

static void test_ringbuf_push_pop_cycle(void) {
    ttak_ringbuf_t *rb = ttak_ringbuf_create(5, sizeof(int));
    int in = 10;
    int out = 0;

    ASSERT(ttak_ringbuf_is_empty(rb));
    ttak_ringbuf_push(rb, &in);
    ASSERT(ttak_ringbuf_count(rb) == 1);
    ttak_ringbuf_pop(rb, &out);
    ASSERT(out == in);
    ASSERT(ttak_ringbuf_is_empty(rb));

    for (int i = 0; i < 5; ++i) {
        ttak_ringbuf_push(rb, &i);
    }
    ASSERT(ttak_ringbuf_is_full(rb));
    ASSERT(ttak_ringbuf_push(rb, &in) == false);

    ttak_ringbuf_destroy(rb);
}

/* Query functions must stay consistent under concurrent push/pop. */
#define RB_CAPACITY 64
#define RB_ROUNDS   20000

typedef struct {
    ttak_ringbuf_t *rb;
    _Atomic int *violations;
    _Atomic int *pushed;
    _Atomic int *popped;
} rb_shared_t;

static void *rb_producer(void *arg) {
    rb_shared_t *s = (rb_shared_t *)arg;
    for (int i = 0; i < RB_ROUNDS; ++i) {
        if (ttak_ringbuf_push(s->rb, &i)) {
            atomic_fetch_add(s->pushed, 1);
        }
    }
    return NULL;
}

static void *rb_consumer(void *arg) {
    rb_shared_t *s = (rb_shared_t *)arg;
    int out = 0;
    for (int i = 0; i < RB_ROUNDS; ++i) {
        if (ttak_ringbuf_pop(s->rb, &out)) {
            atomic_fetch_add(s->popped, 1);
        }
    }
    return NULL;
}

static void *rb_querier(void *arg) {
    rb_shared_t *s = (rb_shared_t *)arg;
    for (int i = 0; i < RB_ROUNDS * 4; ++i) {
        size_t count = ttak_ringbuf_count(s->rb);
        /* Each query call is a separate locked snapshot, so cross-checks
         * between them would race; only per-snapshot bounds are asserted. */
        (void)ttak_ringbuf_is_empty(s->rb);
        (void)ttak_ringbuf_is_full(s->rb);
        if (count > RB_CAPACITY) {
            atomic_fetch_add(s->violations, 1);
            return NULL;
        }
    }
    return NULL;
}

static void test_ringbuf_concurrent_query_race(void) {
    ttak_ringbuf_t *rb = ttak_ringbuf_create(RB_CAPACITY, sizeof(int));
    _Atomic int violations = 0;
    _Atomic int pushed = 0;
    _Atomic int popped = 0;
    rb_shared_t shared = { rb, &violations, &pushed, &popped };

    pthread_t producer, consumer, querier1, querier2;
    ASSERT(pthread_create(&producer, NULL, rb_producer, &shared) == 0);
    ASSERT(pthread_create(&consumer, NULL, rb_consumer, &shared) == 0);
    ASSERT(pthread_create(&querier1, NULL, rb_querier, &shared) == 0);
    ASSERT(pthread_create(&querier2, NULL, rb_querier, &shared) == 0);

    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);
    pthread_join(querier1, NULL);
    pthread_join(querier2, NULL);

    ASSERT(violations == 0);

    /* Buffered items must equal pushes minus pops. */
    size_t count = ttak_ringbuf_count(rb);
    ASSERT(count <= RB_CAPACITY);
    ASSERT(count == (size_t)(pushed - popped));

    ttak_ringbuf_destroy(rb);
}

int main(void) {
    RUN_TEST(test_ringbuf_push_pop_cycle);
    RUN_TEST(test_ringbuf_concurrent_query_race);
    return 0;
}
