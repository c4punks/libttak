/**
 * @file worker.c
 * @brief Worker thread lifecycle: spawn, fault-isolate, and restart.
 *
 * Each thread runs inside a setjmp recovery frame so that a recoverable
 * fault (SIGSEGV caught by the pool) causes a longjmp back to the frame
 * rather than terminating the whole process.
 *
 * Workers use shard-affine dequeuing: each worker has a preferred shard
 * derived from its index (ttak_shard_for_worker()).  It first tries to pop
 * from that shard under the shard's own lock.  If the shard is empty the
 * worker performs work stealing: it scans the remaining shards in rotation
 * order until it finds a task or determines all shards are empty, at which
 * point it blocks on its preferred shard's condition variable.
 */

#include <ttak/thread/worker.h>
#include <ttak/thread/pool.h>
#include <ttak/mem/mem.h>
#include <ttak/mem/epoch.h>
#include <ttak/timing/timing.h>
#include <ttak/priority/scheduler.h>
#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/resource.h>
    #include <unistd.h>
    #include <time.h>
#endif
#include <stdlib.h>
#if defined(__TINYC__)
#include <ttak/compat/pthread.h>
#endif
#include "../../internal/app_types.h"
#include "../../internal/tt_jmp.h"
#include "../../internal/ttak/shard_map.h"

#if defined(__TINYC__)
static pthread_once_t worker_tls_once = PTHREAD_ONCE_INIT;
static pthread_key_t worker_tls_key;
static void worker_tls_init(void) { pthread_key_create(&worker_tls_key, NULL); }
static ttak_worker_t *get_current_worker(void) { pthread_once(&worker_tls_once, worker_tls_init); return (ttak_worker_t *)pthread_getspecific(worker_tls_key); }
static void set_current_worker(ttak_worker_t *worker) { pthread_once(&worker_tls_once, worker_tls_init); pthread_setspecific(worker_tls_key, worker); }
#else
static TTAK_THREAD_LOCAL ttak_worker_t *current_worker = NULL;
static inline ttak_worker_t *get_current_worker(void) { return current_worker; }
static inline void set_current_worker(ttak_worker_t *worker) { current_worker = worker; }
#endif

void ttak_worker_abort(void) {
    ttak_worker_t *worker = get_current_worker();
    if (worker && worker->wrapper && worker->wrapper->jmp_magic == TT_JMP_MAGIC) {
        tt_longjmp(worker->wrapper->env, &worker->wrapper->jmp_magic, &worker->wrapper->jmp_tid, 1);
    }
}

static void threaded_function_wrapper(ttak_worker_t *worker, ttak_task_t *task) {
    (void)worker;
    if (task) {
        uint64_t start_time = ttak_get_tick_count();
        ttak_task_set_start_ts(task, start_time);
        ttak_task_execute(task, start_time);
        uint64_t end_time = ttak_get_tick_count();
        ttak_scheduler_record_execution(task, (end_time >= start_time) ? (end_time - start_time) : 0);
    }
}

/**
 * @brief Try to steal a task from any shard other than @p skip_shard.
 *
 * Scans shards in index order (wrapping around @p skip_shard) and returns
 * the first task found, or NULL if all shards are empty.  Each try-lock
 * attempt is non-blocking so as not to starve the preferred shard.
 *
 * @param pool       Pool to steal from.
 * @param skip_shard Shard index already tried (the preferred shard).
 * @param now        Timestamp for queue operations.
 * @return Stolen task, or NULL.
 */
static ttak_task_t *worker_steal_task(ttak_thread_pool_t *pool, size_t skip_shard, uint64_t now) {
    for (size_t s = 0; s < TTAK_THREAD_POOL_SHARDS; s++) {
        if (s == skip_shard) continue;
        ttak_pool_shard_t *shard = &pool->shards[s];
        if (pthread_mutex_trylock(&shard->lock) != 0) continue;
        ttak_task_t *task = shard->queue.pop(&shard->queue, now);
        pthread_mutex_unlock(&shard->lock);
        if (task) return task;
    }
    return NULL;
}

void *ttak_worker_routine(void *arg) {
    ttak_worker_t *self = (ttak_worker_t *)arg;
    ttak_thread_pool_t *pool = self->pool;
#ifdef TTAK_DEBUG_WORKER
    fprintf(stderr, "[worker] start %p\n", (void*)self);
    fflush(stderr);
#endif
    set_current_worker(self);
    ttak_epoch_register_thread();
    ttak_epoch_exit();
    if (self->wrapper) {
#ifdef _WIN32
        int p = THREAD_PRIORITY_NORMAL;
        if (self->wrapper->nice_val <= -10) p = THREAD_PRIORITY_HIGHEST;
        else if (self->wrapper->nice_val < 0) p = THREAD_PRIORITY_ABOVE_NORMAL;
        else if (self->wrapper->nice_val > 10) p = THREAD_PRIORITY_LOWEST;
        else if (self->wrapper->nice_val > 0) p = THREAD_PRIORITY_BELOW_NORMAL;
        SetThreadPriority(GetCurrentThread(), p);
#else
        setpriority(PRIO_PROCESS, 0, self->wrapper->nice_val);
#endif
    }

    /* Use the shard assigned at creation time for affinity-first scheduling */
    size_t pref = self->preferred_shard;
    ttak_pool_shard_t *pref_shard = &pool->shards[pref];

    while (!self->should_stop && !pool->is_shutdown) {
        volatile uint64_t now = ttak_get_tick_count();
        ttak_task_t *task = NULL;

        /* --- Shard-affine fetch with robust fallback to work stealing --- */
        while (!task && !self->should_stop && !pool->is_shutdown) {
            /* 1. Try preferred shard (fast path) */
            pthread_mutex_lock(&pref_shard->lock);
            task = pref_shard->queue.pop(&pref_shard->queue, now);
            pthread_mutex_unlock(&pref_shard->lock);
            if (task) break;

            /* 2. Try stealing from other shards (throughput path).  The
             *    preferred shard lock is intentionally NOT held here so
             *    the shard stays available to its affine worker. */
            task = worker_steal_task(pool, pref, now);
            if (task) break;

            /* 3. Still idle?  Re-lock the preferred shard, re-check for
             *    work under the lock (no lost wakeups), then wait. */
            pthread_mutex_lock(&pref_shard->lock);
            if (self->should_stop || pool->is_shutdown) {
                pthread_mutex_unlock(&pref_shard->lock);
                break;
            }
            task = pref_shard->queue.pop(&pref_shard->queue, now);
            if (!task) {
                struct timespec ts;
#if defined(_WIN32)
                timespec_get(&ts, TIME_UTC);
#else
                clock_gettime(CLOCK_REALTIME, &ts);
#endif
                ts.tv_nsec += 10000000L; /* 10ms timeout */
                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec += 1;
                    ts.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&pref_shard->cond, &pref_shard->lock, &ts);
            }
            pthread_mutex_unlock(&pref_shard->lock);
        }

        if (task) {
#ifdef TTAK_DEBUG_WORKER
            fprintf(stderr, "[worker] %p executing task %p\n", (void*)self, (void*)task);
#endif
            volatile _Bool epoch_active = 0;
            if (tt_setjmp(self->wrapper->env, &self->wrapper->jmp_magic, &self->wrapper->jmp_tid) == 0) {
                ttak_epoch_enter(); epoch_active = 1;
                threaded_function_wrapper(self, task);
                ttak_epoch_exit(); epoch_active = 0;
            } else if (epoch_active) { ttak_epoch_exit(); epoch_active = 0; self->exit_code = TTAK_ERR_FATAL_EXIT; }
            ttak_task_destroy(task, now);
        }
    }
    ttak_epoch_deregister_thread();
    return (void *)(uintptr_t)self->exit_code;
}
