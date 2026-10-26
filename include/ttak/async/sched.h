/**
 * @file sched.h
 * @brief Async task scheduler — init, shutdown, schedule, and yield.
 *
 * Manages the global worker-thread pool.  Tasks submitted via
 * ttak_async_schedule() are picked up by threads in priority order.
 * ttak_async_yield() relinquishes the CPU slice without sleeping.
 */

#ifndef TTAK_ASYNC_SCHED_H
#define TTAK_ASYNC_SCHED_H

#include <ttak/async/task.h>
#include <stdint.h>
#ifndef _WIN32
#include <sched.h>
#include <ttak/compat/baremetal.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/**
 * @brief Initialises the global async thread pool.
 *
 * @param nice Base nice value applied to worker threads (0 = normal).
 */
void ttak_async_init(int nice);

/** @brief Signals all workers to drain their queues and exit. */
void ttak_async_shutdown(void);

/**
 * @brief Submits a task for asynchronous execution.
 *
 * @param task     Task to enqueue.
 * @param now      Current monotonic timestamp in nanoseconds.
 * @param priority Scheduling priority (lower = higher priority).
 */
void ttak_async_schedule(ttak_task_t *task, uint64_t now, int priority);

/** @brief Yields the current CPU time slice without sleeping. */
static inline void ttak_async_yield(void) {
#ifndef _WIN32
    sched_yield();
#else
    SwitchToThread();
#endif
}

#endif // TTAK_ASYNC_SCHED_H
