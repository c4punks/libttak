#include <ttak/phys/mem/buddy.h>
#include <ttak/mem/mem.h>
#include <ttak/mem/epoch.h>
#include <ttak/compat/pthread.h>
#include <ttak/compat/baremetal.h>
#include <string.h>
#include "test_macros.h"

#define THREADS_AVAILABLE (!TTAK_PLATFORM_BAREMETAL || TTAK_PLATFORM_FREERTOS)

/* EMBEDDED builds already serve ttak_mem from the global pool, so the
 * tests share it; elsewhere nothing else uses the pool and the tests
 * install their own. */
#if EMBEDDED
static void pool_setup(void) {
    ttak_dangerous_free(ttak_dangerous_alloc(1)); /* bootstraps the pool */
}
#else
static _Alignas(64) unsigned char test_pool[64 * 1024];
static void pool_setup(void) {
    ttak_mem_buddy_init(test_pool, sizeof(test_pool), 1);
}
#endif

static void *buddy_alloc(size_t bytes) {
    ttak_mem_req_t req = { .size_bytes = bytes, .priority = TTAK_PRIORITY_BEST_FIT };
    return ttak_mem_buddy_alloc(&req);
}

static ttak_mem_buddy_stats_t stats(void) {
    ttak_mem_buddy_stats_t s;
    ttak_mem_buddy_get_stats(&s);
    return s;
}

/* Freed blocks come back at once; they used to wait for an epoch reclaim
 * that bare metal never ran, so this ran a 64 KiB pool dry in ~100 rounds. */
static void test_free_returns_memory(void) {
    ttak_mem_buddy_stats_t before = stats();
    for (int i = 0; i < 5000; ++i) {
        void *p = buddy_alloc(200);
        ASSERT_MSG(p != NULL, "allocation %d failed", i);
        ttak_mem_buddy_free(p);
    }
    ttak_mem_buddy_stats_t after = stats();
    ASSERT(after.bytes_in_use == before.bytes_in_use);
    ASSERT(after.largest_free == before.largest_free);
}

/* Blocks with mixed lifetimes leave holes; once all are freed the pool
 * must coalesce back to its largest block. */
static void test_interleaved_free_coalesces(void) {
    enum { N = 48 };
    void *blocks[N];
    ttak_mem_buddy_stats_t before = stats();
    for (int i = 0; i < N; ++i) {
        blocks[i] = buddy_alloc(40 + (size_t)(i % 5) * 60);
        ASSERT(blocks[i] != NULL);
    }
    for (int i = 1; i < N; i += 2) ttak_mem_buddy_free(blocks[i]);
    void *big = buddy_alloc(2048);
    ASSERT(big != NULL);
    for (int i = 0; i < N; i += 2) ttak_mem_buddy_free(blocks[i]);
    ttak_mem_buddy_free(big);
    ttak_mem_buddy_stats_t after = stats();
    ASSERT(after.bytes_in_use == before.bytes_in_use);
    ASSERT(after.largest_free == before.largest_free);
}

/* Buddy and ttak_mem payloads both honour TTAK_MEM_HEADER_ALIGN. */
static void test_payload_alignment(void) {
    for (size_t len = 1; len < 600; len += 37) {
        void *b = buddy_alloc(len);
        void *m = ttak_mem_alloc(len, __TTAK_UNSAFE_MEM_FOREVER__, 0);
        ASSERT(b != NULL && m != NULL);
        ASSERT((uintptr_t)b % TTAK_MEM_HEADER_ALIGN == 0);
        ASSERT((uintptr_t)m % TTAK_MEM_HEADER_ALIGN == 0);
        ttak_mem_buddy_free(b);
        ttak_mem_free(m);
    }
}

#if THREADS_AVAILABLE
#define CHURN_THREADS 4
#define CHURN_ROUNDS 2000

static void *churn_thread(void *arg) {
    unsigned char tag = (unsigned char)(uintptr_t)arg;
    uintptr_t corrupt = 0;
    void *held[4] = { NULL, NULL, NULL, NULL };
    size_t held_len[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < CHURN_ROUNDS; ++i) {
        int slot = i % 4;
        if (held[slot]) {
            const unsigned char *b = held[slot];
            for (size_t k = 0; k < held_len[slot]; ++k) {
                if (b[k] != tag) { corrupt++; break; }
            }
            ttak_mem_buddy_free(held[slot]);
        }
        size_t len = 16 + (size_t)((i * 37 + tag * 11) % 400);
        held[slot] = buddy_alloc(len);
        held_len[slot] = held[slot] ? len : 0;
        if (held[slot]) memset(held[slot], tag, len);
    }
    for (int slot = 0; slot < 4; ++slot) ttak_mem_buddy_free(held[slot]);
    return (void *)corrupt;
}

/* Splitting and coalescing from several threads must never hand the same
 * bytes to two owners. */
static void test_concurrent_churn(void) {
    pthread_t threads[CHURN_THREADS];
    ttak_mem_buddy_stats_t before = stats();
    for (uintptr_t t = 0; t < CHURN_THREADS; ++t) {
        ASSERT(pthread_create(&threads[t], NULL, churn_thread, (void *)(t + 1)) == 0);
    }
    uintptr_t corrupt = 0;
    for (int t = 0; t < CHURN_THREADS; ++t) {
        void *ret = NULL;
        pthread_join(threads[t], &ret);
        corrupt += (uintptr_t)ret;
    }
    ASSERT_MSG(corrupt == 0, "%lu blocks were overwritten by another thread", (unsigned long)corrupt);
    ASSERT(stats().bytes_in_use == before.bytes_in_use);
}
#endif

#if EMBEDDED && THREADS_AVAILABLE
static void *epoch_user(void *arg) {
    (void)arg;
    ttak_epoch_enter();
    ttak_epoch_exit();
    return NULL;
}

static void spawn_epoch_users(int count) {
    for (int i = 0; i < count; ++i) {
        pthread_t th;
        ASSERT(pthread_create(&th, NULL, epoch_user, NULL) == 0);
        pthread_join(th, NULL);
    }
}

/* Each thread that enters an epoch gets per-thread state from the pool;
 * exited threads hand it to the next one instead of leaking it. */
static void test_epoch_state_reused(void) {
    spawn_epoch_users(2);
    ttak_mem_buddy_stats_t before = stats();
    spawn_epoch_users(50);
    ASSERT(stats().bytes_in_use == before.bytes_in_use);
}
#endif

int main(void) {
    pool_setup();
    RUN_TEST(test_free_returns_memory);
    RUN_TEST(test_interleaved_free_coalesces);
    RUN_TEST(test_payload_alignment);
#if THREADS_AVAILABLE
    RUN_TEST(test_concurrent_churn);
#endif
#if EMBEDDED && THREADS_AVAILABLE
    RUN_TEST(test_epoch_state_reused);
#endif
    return 0;
}
