/**
 * @file function.h
 * @brief Convenience wrappers for fire-and-forget async jobs that return futures.
 *
 * Higher level helpers to simplify the pattern of creating a promise/future
 * pair, scheduling a worker task, and awaiting the result.
 */

#ifndef TTAK_ASYNC_FUNCTION_H
#define TTAK_ASYNC_FUNCTION_H

#include <ttak/async/future.h>
#include <ttak/async/promise.h>
#include <ttak/async/task.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Cleanup callback invoked if a task fails to start.
 */
typedef void (*ttak_async_cleanup_t)(void *ctx);

/**
 * @brief Lightweight handle that bundles a promise and its paired future.
 */
typedef struct ttak_async_future {
    ttak_promise_t *promise; /**< Promise populated by the worker. */
    ttak_future_t  *future;  /**< Blocking future returned to callers. */
} ttak_async_future_t;

/**
 * @brief Schedules a task for asynchronous execution and returns its future.
 *
 * On any failure, @p cleanup is invoked with @p ctx before returning an empty
 * handle to ensure resources aren't leaked.
 *
 * @param func     Worker function to execute.
 * @param ctx      Context pointer passed to the worker.
 * @param cleanup  Optional cleanup callback for @p ctx when scheduling fails.
 * @param priority Scheduler priority hint (lower == higher priority).
 * @return A handle holding the created promise and future (zeroed on failure).
 */
ttak_async_future_t ttak_async_call(ttak_task_func_t func,
                                    void *ctx,
                                    ttak_async_cleanup_t cleanup,
                                    int priority);

/**
 * @brief Blocks until the async task is resolved and returns the worker result.
 *
 * @param future Handle returned from ttak_async_call().
 * @return Result pointer from the worker, or NULL if the handle is invalid.
 */
void *ttak_async_await(ttak_async_future_t *future);

/**
 * @brief Releases the promise/future pair allocated for the async call.
 *
 * May be called before the task has finished: the pair is then freed once
 * the task fulfils it (see ttak_future_release()). Tasks dropped unexecuted,
 * for example by destroying the async pool, keep their pair allocated.
 * Safe to call multiple times; subsequent invocations become no-ops.
 *
 * @param future Handle previously obtained via ttak_async_call().
 */
void ttak_async_future_cleanup(ttak_async_future_t *future);

#ifdef __cplusplus
}
#endif

#endif // TTAK_ASYNC_FUNCTION_H
