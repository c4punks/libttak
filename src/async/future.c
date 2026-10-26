#include <ttak/async/future.h>
#include <ttak/async/promise.h>
#include <ttak/mem/epoch.h>
#include <stddef.h>

/**
 * @brief Retrieve the computed result from the future.
 * * Synchronously blocks the calling thread until the producer signals completion.
 * To prevent epoch stalls and potential deadlocks with the background GC thread,
 * this function temporarily exits the epoch critical section during the wait period.
 *
 * @param future Pointer to the future object.
 * @return void* The result pointer assigned by the task, or NULL if invalid.
 */
void *ttak_future_get(ttak_future_t *future) {
    if (!future) {
        return NULL;
    }

    pthread_mutex_lock(&future->mutex);

    /* * Transition the current thread to an inactive state before blocking.
     * This allows the background GC (Epoch Manager) to advance the global epoch
     * without being stalled by this waiting thread.
     */
    ttak_epoch_exit();

    while (!future->ready) {
        /* Suspend execution until the future state is set to ready by the worker. */
        pthread_cond_wait(&future->cond, &future->mutex);
    }

    /* * Re-enter the epoch critical section upon wakeup to ensure that
     * any subsequent memory access to the returned result remains safe.
     */
    ttak_epoch_enter();

    void *res = future->result;
    pthread_mutex_unlock(&future->mutex);

    return res;
}

/**
 * @brief Drop the consumer's reference to a future.
 *
 * Frees the future and its promise now if the promise was already
 * fulfilled; otherwise marks the future so ttak_promise_set_value() frees
 * both once it has published the result.
 *
 * @param future Future to release.
 */
void ttak_future_release(ttak_future_t *future) {
    if (!future) {
        return;
    }

    pthread_mutex_lock(&future->mutex);
    bool reclaim = future->ready;
    future->released = true;
    pthread_mutex_unlock(&future->mutex);

    if (reclaim) {
        ttak_promise_destroy(future->promise);
    }
}
