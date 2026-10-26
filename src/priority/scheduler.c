/**
 * @file scheduler.c
 * @brief Priority scheduler — task history tracking and EWMA prediction.
 *
 * Maintains per-task execution time history using a sharded hash-map array.
 * The shard for each task is selected deterministically via the same
 * Fibonacci-hashing / Latin-square routing used by the thread pool, so that
 * the same task hash always lands in the same shard.  Each shard has its own
 * mutex, eliminating the single global hot-lock that previously serialised
 * all history reads and writes.
 */

#include <ttak/priority/scheduler.h>
#include <ttak/mem/mem.h>
#include <ttak/ht/map.h>
#include <ttak/timing/timing.h>
#include <ttak/compat/pthread.h>
#include <stddef.h>
#include <string.h>
#include "../../internal/ttak/shard_map.h"

/* -----------------------------------------------------------------------
 * Sharded history storage
 * ----------------------------------------------------------------------- */

/* History is keyed by task hash, i.e. by (func, arg). Callers that pass a
 * fresh arg per task would grow the map without bound, so each shard stops
 * learning new hashes at this many entries; known hashes keep updating. */
#ifndef TTAK_SCHED_HISTORY_MAX_PER_SHARD
#  if defined(TTAK_PLATFORM_BAREMETAL) && TTAK_PLATFORM_BAREMETAL
#    define TTAK_SCHED_HISTORY_MAX_PER_SHARD 32
#  else
#    define TTAK_SCHED_HISTORY_MAX_PER_SHARD 4096
#  endif
#endif

/** One independent segment of the execution-history map. */
typedef struct {
    tt_map_t       *map;
    pthread_mutex_t lock;
    size_t          entries;
    _Bool           initialized;
} ttak_sched_history_shard_t;

static ttak_sched_history_shard_t history_shards[TTAK_POOL_SHARD_COUNT];

/* One-time global initialisation guard */
static pthread_once_t history_once = PTHREAD_ONCE_INIT;

static void history_shards_init_once(void) {
    uint64_t now = ttak_get_tick_count();
    for (size_t s = 0; s < TTAK_POOL_SHARD_COUNT; s++) {
        pthread_mutex_init(&history_shards[s].lock, NULL);
        history_shards[s].map = ttak_create_map(32, now);
        history_shards[s].initialized = (history_shards[s].map != NULL);
    }
}

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

/**
 * @brief Select the history shard for @p hash using the routing table.
 *
 * Uses ttak_shard_for_hash() so the same hash always maps to the same shard,
 * matching the routing used in ttak_thread_pool_schedule_task().
 *
 * @param hash  Task hash.
 * @return      Pointer to the corresponding history shard.
 */
static inline ttak_sched_history_shard_t *sched_shard_for_hash(uint64_t hash) {
    return &history_shards[ttak_shard_for_hash(hash)];
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

void ttak_scheduler_init(void) {
    pthread_once(&history_once, history_shards_init_once);
}

void ttak_scheduler_record_execution(ttak_task_t *task, uint64_t duration_ms) {
    if (!task) return;
    uint64_t hash = ttak_task_get_hash(task);
    if (hash == 0) return;

    uint64_t now = ttak_get_tick_count();
    ttak_sched_history_shard_t *shard = sched_shard_for_hash(hash);
    if (!shard->initialized) return;

    pthread_mutex_lock(&shard->lock);
    size_t old_avg = 0;
    if (ttak_map_get_key(shard->map, (uintptr_t)hash, &old_avg, now)) {
        size_t new_avg = (size_t)((old_avg * 0.7) + (duration_ms * 0.3));
        ttak_insert_to_map(shard->map, (uintptr_t)hash, new_avg, now);
    } else if (shard->entries < TTAK_SCHED_HISTORY_MAX_PER_SHARD) {
        ttak_insert_to_map(shard->map, (uintptr_t)hash, (size_t)duration_ms, now);
        shard->entries++;
    }
    pthread_mutex_unlock(&shard->lock);
}

int ttak_scheduler_get_adjusted_priority(ttak_task_t *task, int base_priority) {
    if (!task) return base_priority;
    uint64_t hash = ttak_task_get_hash(task);
    if (hash == 0) return base_priority;

    int adj_priority = base_priority;
    uint64_t now = ttak_get_tick_count();
    size_t avg_runtime = 0;
    _Bool found = 0;

    ttak_sched_history_shard_t *shard = sched_shard_for_hash(hash);
    if (shard->initialized) {
        pthread_mutex_lock(&shard->lock);
        found = ttak_map_get_key(shard->map, (uintptr_t)hash, &avg_runtime, now);
        pthread_mutex_unlock(&shard->lock);
    }

    if (found) {
        if (avg_runtime < 10) adj_priority += 5;
        else if (avg_runtime < 50) adj_priority += 2;
        else if (avg_runtime > 2000) adj_priority -= 5;
        else if (avg_runtime > 500) adj_priority -= 2;
    } else adj_priority += 1;
    return adj_priority;
}

static int sched_get_current_priority(ttak_scheduler_t *sched) { (void)sched; return 0; }
static void sched_set_priority_override(ttak_scheduler_t *sched, ttak_task_t *task, int new_priority) { (void)sched; (void)task; (void)new_priority; }
static size_t sched_get_pending_count(ttak_scheduler_t *sched) { (void)sched; return 0; }
static size_t sched_get_running_count(ttak_scheduler_t *sched) { (void)sched; return 0; }
static double sched_get_load_average(ttak_scheduler_t *sched) { (void)sched; return 0.0; }

static ttak_scheduler_t global_scheduler = {
    .get_current_priority = sched_get_current_priority,
    .set_priority_override = sched_set_priority_override,
    .get_pending_count = sched_get_pending_count,
    .get_running_count = sched_get_running_count,
    .get_load_average = sched_get_load_average
};

ttak_scheduler_t *ttak_scheduler_get_instance(void) { return &global_scheduler; }
