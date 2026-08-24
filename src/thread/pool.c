#include <ttak/thread/pool.h>
#include <ttak/mem/mem.h>
#include <ttak/sync/sync.h>
#include <ttak/async/promise.h>
#include <stdlib.h>
#include <stdio.h>
#include <ttak/compat/pthread.h>
#include <signal.h>
#include <errno.h>
#include <ttak/compat/stdatomic.h>

#include <ttak/priority/scheduler.h>
#include "../../internal/ttak/shard_map.h"

#define TTAK_BURST_DOMAIN_COUNT 4U
#define TTAK_BURST_ALPHA_PCT 25U
#define TTAK_BURST_EWMA_SCALE 1024U
#define TTAK_BURST_SIMILAR_THRESHOLD_Q10 7U
#define TTAK_BURST_COOL_DEFAULT_Q10 TTAK_BURST_EWMA_SCALE

typedef struct ttak_burst_tracker {
    _Atomic uint32_t ewma_row[TTAK_POOL_SHARD_COUNT];
    _Atomic uint32_t ewma_col[TTAK_POOL_SHARD_COUNT];
    _Atomic uint32_t total_ewma;
} ttak_burst_tracker_t;

static ttak_burst_tracker_t g_burst_trackers[TTAK_BURST_DOMAIN_COUNT];

static inline uint32_t ttak_burst_domain_index(ttak_task_domain_t domain) {
    switch (domain) {
        case TTAK_TASK_DOMAIN_THREAD: return 1U;
        case TTAK_TASK_DOMAIN_IO:     return 2U;
        case TTAK_TASK_DOMAIN_NET:    return 3U;
        case TTAK_TASK_DOMAIN_UNKNOWN:
        default:                      return 0U;
    }
}

static inline void ttak_burst_update_ewma(_Atomic uint32_t *slot, uint32_t sample_q10) {
    uint32_t oldv = atomic_load_explicit(slot, memory_order_relaxed);
    uint64_t next64 = ((uint64_t)oldv * (100U - TTAK_BURST_ALPHA_PCT) +
                       (uint64_t)sample_q10 * TTAK_BURST_ALPHA_PCT) / 100U;
    uint32_t next = (next64 > UINT32_MAX) ? UINT32_MAX : (uint32_t)next64;
    atomic_store_explicit(slot, next, memory_order_relaxed);
}

static inline uint64_t ttak_burst_len_sq(uint32_t x_q10, uint32_t y_q10) {
    return ((uint64_t)x_q10 * (uint64_t)x_q10) + ((uint64_t)y_q10 * (uint64_t)y_q10);
}

static inline _Bool ttak_burst_vectors_similar(uint32_t hx_q10, uint32_t hy_q10,
                                               uint32_t cx_q10, uint32_t cy_q10) {
    uint64_t dot = (uint64_t)hx_q10 * (uint64_t)cx_q10 + (uint64_t)hy_q10 * (uint64_t)cy_q10;
    uint64_t hlen = ttak_burst_len_sq(hx_q10, hy_q10);
    uint64_t clen = ttak_burst_len_sq(cx_q10, cy_q10);
    if (dot == 0U || hlen == 0U || clen == 0U) return false;
    long double threshold = (long double)TTAK_BURST_SIMILAR_THRESHOLD_Q10 / 10.0L;
    long double lhs = (long double)dot * (long double)dot;
    long double rhs = (threshold * threshold) * (long double)hlen * (long double)clen;
    return lhs >= rhs;
}

static inline size_t ttak_rotate_shard_index(size_t shard_idx,
                                             uint32_t hot_row_q10,
                                             uint32_t hot_col_q10,
                                             uint32_t cool_row_q10,
                                             uint32_t cool_col_q10,
                                             size_t active_shards) {
    if (active_shards == 0U) return 0U;
    uint32_t angle_step = (uint32_t)(active_shards / 4U); /* 90° default */
    if (angle_step == 0U) angle_step = 1U;
    if (ttak_burst_vectors_similar(hot_row_q10, hot_col_q10, cool_row_q10, cool_col_q10)) {
        angle_step = 1U; /* conservative narrower rotation when similar */
    }
    return (shard_idx + angle_step) % active_shards;
}

static void ttak_pool_burst_record(ttak_thread_pool_t *pool,
                                   ttak_task_domain_t domain,
                                   uint32_t row,
                                   uint32_t col,
                                   uint32_t urgency_q10) {
    if (!pool || row >= TTAK_POOL_SHARD_COUNT || col >= TTAK_POOL_SHARD_COUNT) return;
    uint32_t d = ttak_burst_domain_index(domain);
    ttak_burst_tracker_t *tracker = &g_burst_trackers[d];
    uint32_t sample = TTAK_BURST_EWMA_SCALE + urgency_q10;
    ttak_burst_update_ewma(&tracker->ewma_row[row], sample);
    ttak_burst_update_ewma(&tracker->ewma_col[col], sample);
    ttak_burst_update_ewma(&tracker->total_ewma, sample);

    uint32_t row_w = atomic_load_explicit(&tracker->ewma_row[row], memory_order_relaxed);
    uint32_t col_w = atomic_load_explicit(&tracker->ewma_col[col], memory_order_relaxed);
    uint32_t mag = row_w + col_w;
    atomic_store_explicit(&pool->burst_hot_row_q10[d], row_w, memory_order_relaxed);
    atomic_store_explicit(&pool->burst_hot_col_q10[d], col_w, memory_order_relaxed);
    atomic_store_explicit(&pool->burst_mag_q10[d], mag, memory_order_relaxed);
}

static size_t ttak_pool_select_shard_with_burst(ttak_thread_pool_t *pool, ttak_task_t *task) {
    uint64_t hash = ttak_task_get_hash(task);
    size_t shard_idx = (hash != 0) ? ttak_shard_for_hash(hash) : 0;

    uint32_t row = 0U, col = 0U;
    ttak_shard_hash_to_coords(hash, &row, &col);
    ttak_task_domain_t domain = ttak_task_get_domain(task);
    uint32_t d = ttak_burst_domain_index(domain);
    uint32_t urgency_q10 = 0U;
    if (domain == TTAK_TASK_DOMAIN_NET) {
        urgency_q10 = ((uint32_t)ttak_task_get_urgency(task) * TTAK_BURST_EWMA_SCALE) / 100U;
    }

    ttak_pool_burst_record(pool, domain, row, col, urgency_q10);

    ttak_burst_tracker_t *tracker = &g_burst_trackers[d];
    uint32_t total = atomic_load_explicit(&tracker->total_ewma, memory_order_relaxed);
    uint32_t row_w = atomic_load_explicit(&tracker->ewma_row[row], memory_order_relaxed);
    uint32_t col_w = atomic_load_explicit(&tracker->ewma_col[col], memory_order_relaxed);
    uint32_t burst_score = row_w + col_w + urgency_q10;
    size_t active_shards = pool->num_threads;
    if (active_shards == 0U || active_shards > TTAK_THREAD_POOL_SHARDS) {
        active_shards = TTAK_THREAD_POOL_SHARDS;
    }
    if (burst_score > (total + TTAK_BURST_EWMA_SCALE)) {
        uint32_t cool_row = TTAK_BURST_COOL_DEFAULT_Q10;
        uint32_t cool_col = TTAK_BURST_COOL_DEFAULT_Q10;
        shard_idx = ttak_rotate_shard_index(shard_idx, row_w, col_w, cool_row, cool_col, active_shards);
    }
    if (shard_idx >= active_shards) {
        shard_idx %= active_shards;
    }
    return shard_idx;
}

/**
 * @brief Stop all workers and signal shutdown.
 *
 * Acquires the coarse pool_lock (used only here and for is_shutdown checks)
 * to flip the flag, then broadcasts on every shard condition variable so that
 * workers blocked in their preferred shard's cond_wait wake up immediately.
 *
 * @param pool Pool to shut down.
 */
static void pool_force_shutdown(ttak_thread_pool_t *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->pool_lock);
    pool->is_shutdown = true;
    for (size_t i = 0; i < pool->num_threads; i++) {
        if (pool->workers[i]) {
            pool->workers[i]->should_stop = true;
        }
    }
    /* Wake workers waiting on any shard cond */
    for (size_t s = 0; s < TTAK_THREAD_POOL_SHARDS; s++) {
        pthread_cond_broadcast(&pool->shards[s].cond);
    }
    pthread_cond_broadcast(&pool->task_cond);
    pthread_mutex_unlock(&pool->pool_lock);
}

/**
 * @brief Create a thread pool with the given worker count.
 *
 * Initialises TTAK_THREAD_POOL_SHARDS independent queue shards, each with its
 * own mutex and condition variable, then spawns @p num_threads workers.  Each
 * worker is assigned a preferred shard via ttak_shard_for_worker().
 *
 * @param num_threads Number of worker threads.
 * @param default_nice Initial nice value for workers.
 * @param now         Timestamp for memory tracking.
 * @return Pointer to the created pool or NULL on failure.
 */
ttak_thread_pool_t *ttak_thread_pool_create(size_t num_threads, int default_nice, uint64_t now) {
    const pthread_attr_t *attr_for_threads = NULL;

    /* Ensure smart scheduler is ready */
    ttak_scheduler_init();

    ttak_thread_pool_t *pool = (ttak_thread_pool_t *)ttak_mem_alloc_raw(sizeof(ttak_thread_pool_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!pool) {
        return NULL;
    }

    pool->num_threads = num_threads;
    pool->creation_ts = now;
    pool->is_shutdown = false;
    for (size_t d = 0; d < TTAK_BURST_DOMAIN_COUNT; d++) {
        atomic_store_explicit(&pool->burst_hot_row_q10[d], 0U, memory_order_relaxed);
        atomic_store_explicit(&pool->burst_hot_col_q10[d], 0U, memory_order_relaxed);
        atomic_store_explicit(&pool->burst_mag_q10[d], 0U, memory_order_relaxed);
    }
    pool->force_shutdown = pool_force_shutdown;

    /* Coarse lock used only for shutdown signalling */
    pthread_mutex_init(&pool->pool_lock, NULL);
    pthread_cond_init(&pool->task_cond, NULL);

    /* Initialise all per-shard queues, locks, and condition variables */
    for (size_t s = 0; s < TTAK_THREAD_POOL_SHARDS; s++) {
        ttak_priority_queue_init(&pool->shards[s].queue);
        pthread_mutex_init(&pool->shards[s].lock, NULL);
        pthread_cond_init(&pool->shards[s].cond, NULL);
    }

    pool->workers = (ttak_worker_t **)ttak_mem_alloc_raw(sizeof(ttak_worker_t *) * num_threads, __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!pool->workers) {
        fprintf(stderr, "[FATAL] Failed to allocate worker array\n");
        for (size_t s = 0; s < TTAK_THREAD_POOL_SHARDS; s++) {
            pthread_mutex_destroy(&pool->shards[s].lock);
            pthread_cond_destroy(&pool->shards[s].cond);
        }
        pthread_mutex_destroy(&pool->pool_lock);
        pthread_cond_destroy(&pool->task_cond);
        ttak_mem_free(pool);
        return NULL;
    }

    for (size_t i = 0; i < num_threads; i++) {
        pool->workers[i] = (ttak_worker_t *)ttak_mem_alloc_raw(sizeof(ttak_worker_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
        if (!pool->workers[i]) {
            fprintf(stderr, "[FATAL] Failed to allocate worker %zu\n", i);
            pool->num_threads = i;
            pool_force_shutdown(pool);
            return NULL;
        }
        pool->workers[i]->pool = pool;
        pool->workers[i]->should_stop = false;
        pool->workers[i]->exit_code = 0;
        /* Assign shard affinity: spread workers evenly across shards */
        pool->workers[i]->preferred_shard = ttak_shard_for_worker(i);

        pool->workers[i]->wrapper = (ttak_worker_wrapper_t *)ttak_mem_alloc_raw(sizeof(ttak_worker_wrapper_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
        if (!pool->workers[i]->wrapper) {
            fprintf(stderr, "[FATAL] Failed to allocate worker wrapper %zu\n", i);
            ttak_mem_free(pool->workers[i]);
            pool->workers[i] = NULL;
            pool->num_threads = i;
            pool_force_shutdown(pool);
            return NULL;
        }
        pool->workers[i]->wrapper->nice_val = default_nice;
        pool->workers[i]->wrapper->ts = now;
        /* Pre-initialize jump magic so abort checks don't fail before first setjmp */
        pool->workers[i]->wrapper->jmp_magic = 0;
        pool->workers[i]->wrapper->jmp_tid = 0;

        int rc = pthread_create(&pool->workers[i]->thread, NULL, ttak_worker_routine, pool->workers[i]);
        if (rc != 0) {
            fprintf(stderr, "[FATAL] Failed to create worker thread %zu: %d\n", i, rc);
            pool->num_threads = i;
            pool_force_shutdown(pool);
            return NULL;
        }
    }

    return pool;
}

/**
 * @brief Submit a function to be executed asynchronously.
 *
 * @param pool     Pool receiving the work.
 * @param func     Function pointer to execute.
 * @param arg      Argument passed to the function.
 * @param priority Scheduling priority hint.
 * @param now      Timestamp for memory bookkeeping.
 * @return Future representing the eventual result, or NULL on failure.
 */
ttak_future_t *ttak_thread_pool_submit_task(ttak_thread_pool_t *pool, void *(*func)(void *), void *arg, int priority, uint64_t now) {
    if (!pool) return NULL;

    ttak_promise_t *promise = ttak_promise_create(now);
    if (!promise) return NULL;

    ttak_task_t *task = ttak_task_create((ttak_task_func_t)func, arg, promise, now);
    if (!task) {
        ttak_mem_free(promise->future);
        ttak_mem_free(promise); /* destroys promise here. */
        return NULL; 
    }

    /* Apply smart scheduling adjustment */
    int adjusted_priority = ttak_scheduler_get_adjusted_priority(task, priority);

    if (!ttak_thread_pool_schedule_task(pool, task, adjusted_priority, now)) {
        ttak_task_destroy(task, now);
        ttak_mem_free(promise->future);
        ttak_mem_free(promise);
        return NULL;
    }

    return ttak_promise_get_future(promise);
}

/**
 * @brief Queue a prepared task for execution using deterministic shard routing.
 *
 * The task's hash is mapped to (row, col) coordinates via Fibonacci hashing,
 * then indexed into the Latin-square routing table to select a shard.  Only
 * the selected shard's lock is held during the push, keeping the critical
 * section small.
 *
 * When the task hash is 0 (unknown), the task falls back to shard 0.
 *
 * @param pool     Pool to enqueue into.
 * @param task     Task instance created earlier.
 * @param priority Priority hint.
 * @param now      Timestamp for queue bookkeeping.
 * @return true if scheduled, false if the pool is shutting down.
 */
_Bool ttak_thread_pool_schedule_task(ttak_thread_pool_t *pool, ttak_task_t *task, int priority, uint64_t now) {
    if (!pool || !task) return 0;

    /* Abort early without touching any lock */
    if (pool->is_shutdown) return 0;

    /* Deterministic shard selection via hash → coordinate → table lookup */
    size_t shard_idx = ttak_pool_select_shard_with_burst(pool, task);
    ttak_pool_shard_t *shard = &pool->shards[shard_idx];

    pthread_mutex_lock(&shard->lock);
    if (pool->is_shutdown) {
        pthread_mutex_unlock(&shard->lock);
        return 0;
    }

    shard->queue.push(&shard->queue, task, priority, now);
    pthread_cond_broadcast(&shard->cond);
    pthread_mutex_unlock(&shard->lock);

    /*
     * Wake idle workers on other shards as a work-stealing hint without
     * keeping this shard's mutex locked.
     */
    for (size_t s = 0; s < TTAK_THREAD_POOL_SHARDS; s++) {
        if (s == shard_idx) continue;
        pthread_cond_broadcast(&pool->shards[s].cond);
    }

    return 1;
}

/**
 * @brief Destroy the pool, wait for workers, and free pending tasks.
 *
 * @param pool Pool to destroy.
 */
void ttak_thread_pool_destroy(ttak_thread_pool_t *pool) {
    if (!pool) return;

    pool_force_shutdown(pool);

    for (size_t i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->workers[i]->thread, NULL);
        ttak_mem_free(pool->workers[i]->wrapper);
        ttak_mem_free(pool->workers[i]);
    }

    ttak_mem_free(pool->workers);
    pthread_mutex_destroy(&pool->pool_lock);
    pthread_cond_destroy(&pool->task_cond);

    /* Drain any remaining tasks from each shard and tear down shard resources */
    for (size_t s = 0; s < TTAK_THREAD_POOL_SHARDS; s++) {
        ttak_task_t *t;
        while ((t = pool->shards[s].queue.pop(&pool->shards[s].queue, pool->creation_ts)) != NULL) {
            ttak_task_destroy(t, pool->creation_ts);
        }
        pthread_mutex_destroy(&pool->shards[s].lock);
        pthread_cond_destroy(&pool->shards[s].cond);
    }

    ttak_mem_free(pool);
}
