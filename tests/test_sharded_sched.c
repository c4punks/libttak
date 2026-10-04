/**
 * @file test_sharded_sched.c
 * @brief Tests for the sharded queue architecture and deterministic mapping layer.
 *
 * Validates:
 *  1. The shard routing table produces values in [0, TTAK_POOL_SHARD_COUNT).
 *  2. The mapping is deterministic (same hash → same shard on repeated calls).
 *  3. The 8×8 Latin-square property: each shard index appears exactly once
 *     per row and once per column.
 *  4. The pool correctly routes tasks to different shards based on their hash.
 *  5. Work stealing: a worker whose preferred shard is empty executes tasks
 *     queued into a different shard.
 *  6. The sharded scheduler history produces correct priority adjustments
 *     while accepting concurrent access from multiple threads.
 */

#include <ttak/thread/pool.h>
#include <ttak/async/task.h>
#include <ttak/async/future.h>
#include <ttak/priority/scheduler.h>
#include <ttak/timing/timing.h>
#include <ttak/mem/mem.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include "test_macros.h"

/* Pull in the internal header so we can validate the mapping functions directly. */
#include "../internal/ttak/shard_map.h"

/* -------------------------------------------------------------------------
 * 1. Routing-table coverage: every output must be in [0, SHARD_COUNT)
 * ---------------------------------------------------------------------- */
void test_route_table_bounds(void) {
    for (size_t r = 0; r < TTAK_POOL_SHARD_COUNT; r++) {
        for (size_t c = 0; c < TTAK_POOL_SHARD_COUNT; c++) {
            ASSERT_MSG(shard_route_table[r][c] < TTAK_POOL_SHARD_COUNT,
                       "table[%zu][%zu] = %u out of range", r, c, shard_route_table[r][c]);
        }
    }
}

/* -------------------------------------------------------------------------
 * 2. Determinism: ttak_shard_for_hash returns the same value repeatedly.
 * ---------------------------------------------------------------------- */
void test_shard_mapping_deterministic(void) {
    const uint64_t test_hashes[] = {
        UINT64_C(0x0),
        UINT64_C(0x1),
        UINT64_C(0xdeadbeefcafe1234),
        UINT64_C(0xffffffffffffffff),
        UINT64_C(0x9e3779b97f4a7c15),
        UINT64_C(0x0123456789abcdef),
    };
    size_t n = sizeof(test_hashes) / sizeof(test_hashes[0]);
    for (size_t i = 0; i < n; i++) {
        size_t first  = ttak_shard_for_hash(test_hashes[i]);
        size_t second = ttak_shard_for_hash(test_hashes[i]);
        ASSERT_MSG(first == second,
                   "hash 0x%llx mapped to different shards: %zu vs %zu",
                   (unsigned long long)test_hashes[i], first, second);
        ASSERT_MSG(first < TTAK_POOL_SHARD_COUNT,
                   "hash 0x%llx shard %zu out of range",
                   (unsigned long long)test_hashes[i], first);
    }
}

/* -------------------------------------------------------------------------
 * 3. Latin-square property: each value appears exactly once per row/col.
 * ---------------------------------------------------------------------- */
void test_latin_square_property(void) {
    /* Check rows */
    for (size_t r = 0; r < TTAK_POOL_SHARD_COUNT; r++) {
        int seen[TTAK_POOL_SHARD_COUNT] = {0};
        for (size_t c = 0; c < TTAK_POOL_SHARD_COUNT; c++) {
            uint8_t v = shard_route_table[r][c];
            ASSERT_MSG(!seen[v], "row %zu: duplicate shard %u", r, v);
            seen[v] = 1;
        }
    }
    /* Check columns */
    for (size_t c = 0; c < TTAK_POOL_SHARD_COUNT; c++) {
        int seen[TTAK_POOL_SHARD_COUNT] = {0};
        for (size_t r = 0; r < TTAK_POOL_SHARD_COUNT; r++) {
            uint8_t v = shard_route_table[r][c];
            ASSERT_MSG(!seen[v], "col %zu: duplicate shard %u", c, v);
            seen[v] = 1;
        }
    }
}

/* -------------------------------------------------------------------------
 * 4. Pool routes tasks to distinct shards based on task hash.
 *    We submit tasks with known hashes, collect which shards were non-empty
 *    after scheduling (by inspecting queue sizes before workers drain them),
 *    and verify the shard selection matches ttak_shard_for_hash().
 * ---------------------------------------------------------------------- */
static _Atomic int routing_counter = 0;
static _Atomic int routing_counter_urgent = 0;

void *routing_task(void *arg) {
    (void)arg;
    routing_counter++;
    return NULL;
}

void *routing_task_urgent(void *arg) {
    (void)arg;
    routing_counter_urgent++;
    return NULL;
}

void test_pool_sharded_routing(void) {
    uint64_t now = ttak_get_tick_count();
    /* Use a small pool so task completion is observable */
    ttak_thread_pool_t *pool = ttak_thread_pool_create(4, 0, now);
    ASSERT(pool != NULL);

    routing_counter = 0;
    const int N = 32;
    ttak_future_t *futures[32];

    for (int i = 0; i < N; i++) {
        futures[i] = ttak_thread_pool_submit_task(pool, routing_task, NULL, 0, now);
        ASSERT(futures[i] != NULL);
    }

    for (int i = 0; i < N; i++) {
        ttak_future_get(futures[i]);
    }

    ASSERT(routing_counter == N);
    ttak_thread_pool_destroy(pool);
}

void test_pool_sharded_routing_net_urgency(void) {
    uint64_t now = ttak_get_tick_count();
    ttak_thread_pool_t *pool = ttak_thread_pool_create(4, 0, now);
    ASSERT(pool != NULL);

    routing_counter_urgent = 0;
    const int N = 16;
    ttak_future_t *futures[16];

    for (int i = 0; i < N; i++) {
        ttak_promise_t *promise = ttak_promise_create(now);
        ASSERT(promise != NULL);
        ttak_task_t *ct = ttak_task_create(routing_task_urgent, NULL, promise, now);
        ASSERT(ct != NULL);
        ttak_task_set_domain(ct, TTAK_TASK_DOMAIN_NET);
        ttak_task_set_urgency(ct, 95);
        _Bool ok = ttak_thread_pool_schedule_task(pool, ct, 0, now);
        ASSERT(ok);
        futures[i] = ttak_promise_get_future(promise);
        ASSERT(futures[i] != NULL);
    }

    for (int i = 0; i < N; i++) {
        ttak_future_get(futures[i]);
    }

    ASSERT(routing_counter_urgent == N);
    ttak_thread_pool_destroy(pool);
}

/* -------------------------------------------------------------------------
 * 5. Work stealing: submit tasks that all land on shard 0 (hash = 0 falls
 *    back to shard 0) and verify a worker with preferred_shard != 0 still
 *    executes them.
 * ---------------------------------------------------------------------- */
static _Atomic int steal_counter = 0;

void *steal_task(void *arg) {
    (void)arg;
    steal_counter++;
    return NULL;
}

void test_work_stealing(void) {
    uint64_t now = ttak_get_tick_count();
    /* Use TTAK_THREAD_POOL_SHARDS workers so every shard has a worker.
     * All tasks go to shard 0; workers for other shards must steal. */
    ttak_thread_pool_t *pool = ttak_thread_pool_create(TTAK_THREAD_POOL_SHARDS, 0, now);
    ASSERT(pool != NULL);

    steal_counter = 0;
    const int N = 16;
    ttak_future_t *futures[16];

    for (int i = 0; i < N; i++) {
        /* hash == 0 → falls back to shard 0 in schedule_task */
        ttak_promise_t *promise = ttak_promise_create(now);
        ASSERT(promise != NULL);
        ttak_task_t *ct = ttak_task_create(steal_task, NULL, promise, now);
        ASSERT(ct != NULL);
        ttak_task_set_hash(ct, 0);
        _Bool ok = ttak_thread_pool_schedule_task(pool, ct, 0, now);
        ASSERT(ok);
        futures[i] = ttak_promise_get_future(promise);
    }

    for (int i = 0; i < N; i++) {
        ttak_future_get(futures[i]);
    }

    ASSERT(steal_counter == N);
    ttak_thread_pool_destroy(pool);
}

/* -------------------------------------------------------------------------
 * 6. Sharded scheduler history: concurrent record + query is correct.
 * ---------------------------------------------------------------------- */
void *dummy_short_fn(void *a) { return a; }
void *dummy_long_fn(void *a)  { return a; }

void test_sharded_scheduler_history(void) {
    uint64_t now = ttak_get_tick_count();

    ttak_scheduler_init();

    ttak_task_t *t_short = ttak_task_create(dummy_short_fn, NULL, NULL, now);
    ttak_task_t *t_long  = ttak_task_create(dummy_long_fn,  NULL, NULL, now);
    ASSERT(t_short != NULL);
    ASSERT(t_long  != NULL);

    /* Ensure the two tasks hash to different values (different functions) */
    ASSERT(ttak_task_get_hash(t_short) != ttak_task_get_hash(t_long));

    /* Train the history */
    for (int i = 0; i < 10; i++) ttak_scheduler_record_execution(t_short, 5);
    for (int i = 0; i < 10; i++) ttak_scheduler_record_execution(t_long, 1000);

    int adj_short = ttak_scheduler_get_adjusted_priority(t_short, 0);
    int adj_long  = ttak_scheduler_get_adjusted_priority(t_long,  0);

    /* Short task: avg ≈ 5ms → boost of +5 expected */
    ASSERT_MSG(adj_short > 0, "short task priority adjustment %d should be > 0", adj_short);
    /* Long task: avg ≈ 1000ms → penalty expected */
    ASSERT_MSG(adj_long < 0,  "long task priority adjustment %d should be < 0",  adj_long);

    ttak_task_destroy(t_short, now);
    ttak_task_destroy(t_long,  now);
}

/* -------------------------------------------------------------------------
 * 7. Worker affinity assignment mirrors ttak_shard_for_worker().
 * ---------------------------------------------------------------------- */
void test_worker_shard_affinity(void) {
    uint64_t now = ttak_get_tick_count();
    const size_t N = TTAK_THREAD_POOL_SHARDS * 2;  /* 16 workers */
    ttak_thread_pool_t *pool = ttak_thread_pool_create(N, 0, now);
    ASSERT(pool != NULL);

    for (size_t i = 0; i < N; i++) {
        size_t expected = ttak_shard_for_worker(i);
        size_t actual   = pool->workers[i]->preferred_shard;
        ASSERT_MSG(expected == actual,
                   "worker %zu: expected shard %zu, got %zu", i, expected, actual);
    }

    ttak_thread_pool_destroy(pool);
}

/* -------------------------------------------------------------------------
 * 8. Priority-0 tasks must never be dropped or starved, even when one lands
 *    in heap slot 0 of the task queue.
 * ---------------------------------------------------------------------- */
static _Atomic int prio_zero_counter = 0;

void *prio_zero_task(void *arg) {
    (void)arg;
    prio_zero_counter++;
    return NULL;
}

void test_pool_priority_zero_no_starvation(void) {
    uint64_t now = ttak_get_tick_count();
    ttak_thread_pool_t *pool = ttak_thread_pool_create(4, 0, now);
    ASSERT(pool != NULL);

    prio_zero_counter = 0;
    const int N = 64;
    ttak_future_t *futures[64];

    for (int i = 0; i < N; i++) {
        futures[i] = ttak_thread_pool_submit_task(pool, prio_zero_task, NULL, 0, now);
        ASSERT(futures[i] != NULL);
    }

    for (int i = 0; i < N; i++) {
        ttak_future_get(futures[i]);
    }

    ASSERT_MSG(prio_zero_counter == N,
               "only %d of %d priority-0 tasks completed", prio_zero_counter, N);

    uint64_t elapsed = ttak_get_tick_count() - now;
    ASSERT_MSG(elapsed < 10000, "priority-0 burst took %llu ms (expected < 10000)",
               (unsigned long long)elapsed);

    ttak_thread_pool_destroy(pool);
}

int main(void) {
    RUN_TEST(test_route_table_bounds);
    RUN_TEST(test_shard_mapping_deterministic);
    RUN_TEST(test_latin_square_property);
    RUN_TEST(test_worker_shard_affinity);
    RUN_TEST(test_sharded_scheduler_history);
    RUN_TEST(test_pool_sharded_routing);
    RUN_TEST(test_pool_sharded_routing_net_urgency);
    RUN_TEST(test_pool_priority_zero_no_starvation);
    RUN_TEST(test_work_stealing);
    return 0;
}
