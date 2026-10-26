/**
 * @file future.h
 * @brief Simple blocking future for async task result retrieval.
 *
 * A @c ttak_future_t is produced by ttak_promise_get_future() and consumed
 * by calling ttak_future_get(), which blocks the caller until the associated
 * promise is fulfilled. Call ttak_future_release() once the result is no
 * longer needed; otherwise the future and its promise are never freed.
 */

#ifndef TTAK_ASYNC_FUTURE_H
#define TTAK_ASYNC_FUTURE_H

#include <ttak/compat/pthread.h>
#include <stdbool.h>

struct ttak_promise;

/**
 * @brief Blocking future that carries a single void* result.
 */
typedef struct ttak_future {
    void            *result;   /**< Result pointer set by the fulfilling promise. */
    _Bool           ready;     /**< Non-zero once the result is available. */
    _Bool           released;  /**< Consumer called ttak_future_release(). */
    pthread_mutex_t mutex;     /**< Guards @c ready, @c released and @c result. */
    pthread_cond_t  cond;      /**< Signalled when @c ready becomes true. */
    struct ttak_promise *promise; /**< Owning promise, freed together with this future. */
} ttak_future_t;

/**
 * @brief Blocks until the future is fulfilled and returns its result.
 *
 * @param future Pointer to an initialised future.
 * @return       The result pointer set by the corresponding promise.
 */
void *ttak_future_get(ttak_future_t *future);

/**
 * @brief Gives up the caller's reference to a future.
 *
 * The future and the promise that fulfils it are freed once both sides are
 * done: immediately if the promise has already been fulfilled, otherwise
 * when ttak_promise_set_value() runs. Releasing before fulfilment is safe,
 * for example to abandon a task's result.
 *
 * After this call neither the future nor its result slot may be accessed,
 * and no other thread may still be waiting in ttak_future_get(). Passing
 * NULL is a no-op.
 *
 * @param future Future obtained from ttak_promise_get_future() or
 *               ttak_thread_pool_submit_task().
 */
void ttak_future_release(ttak_future_t *future);

#endif // TTAK_ASYNC_FUTURE_H
