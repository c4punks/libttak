/**
 * @file worker.h
 * @brief Resilient worker-thread wrapper with fault isolation via setjmp.
 *
 * Each worker thread runs inside a @c ttak_worker_wrapper_t that installs
 * a signal-safe recovery point.  On fatal signals the thread can be
 * restarted without crashing the whole process.
 */

#ifndef TTAK_THREAD_WORKER_H
#define TTAK_THREAD_WORKER_H

#include <ttak/compat/pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <ttak/async/promise.h>

#define TTAK_ERR_JOIN_FAILED     -101
#define TTAK_ERR_SHUTDOWN_RETRY  -102
#define TTAK_ERR_FATAL_EXIT      -103

typedef struct ttak_worker_wrapper {
#if defined(_WIN32) || TTAK_PLATFORM_BAREMETAL
    jmp_buf         env;
#else
    sigjmp_buf      env;
#endif
    void            *(*func)(void *);
    void            *arg;
    ttak_promise_t  *promise;
    uint64_t        ts;
    int             nice_val;
    volatile uint64_t jmp_magic;
    volatile uint64_t jmp_tid;
} ttak_worker_wrapper_t;

typedef struct ttak_worker {
    pthread_t               thread;
    struct ttak_thread_pool *pool;
    ttak_worker_wrapper_t   *wrapper;
    _Bool                   should_stop;
    int                     exit_code;
    size_t                  preferred_shard; /**< Shard index this worker drains first. */
} ttak_worker_t;

void *ttak_worker_routine(void *arg);

/**
 * @brief Aborts the current task execution using longjmp.
 * Must be called from within a worker thread during task execution.
 */
void ttak_worker_abort(void);

#endif // TTAK_THREAD_WORKER_H
