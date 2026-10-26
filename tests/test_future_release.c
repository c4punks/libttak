#include <ttak/async/promise.h>
#include <ttak/async/future.h>
#include <ttak/async/function.h>
#include <ttak/thread/pool.h>
#include <ttak/timing/timing.h>
#include <ttak/compat/baremetal.h>
#include "test_macros.h"

/* Enough iterations that leaking each pair exhausts a 512 KiB MCU. */
#ifndef TEST_RELEASE_CHURN
#define TEST_RELEASE_CHURN 3000
#endif

static void *double_it(void *arg) {
    return (void *)((uintptr_t)arg * 2);
}

/* Fulfil first, then release: the pair is freed by the release. */
static void test_release_after_fulfil(void) {
    uint64_t now = ttak_get_tick_count();
    ttak_promise_t *promise = ttak_promise_create(now);
    ASSERT(promise != NULL);
    ttak_future_t *future = ttak_promise_get_future(promise);
    ASSERT(future != NULL);

    ttak_promise_set_value(promise, (void *)7, now);
    ASSERT(ttak_future_get(future) == (void *)7);
    ttak_future_release(future);
}

/* Release first, then fulfil: the pair is freed by the fulfilment. */
static void test_release_before_fulfil(void) {
    uint64_t now = ttak_get_tick_count();
    ttak_promise_t *promise = ttak_promise_create(now);
    ASSERT(promise != NULL);

    ttak_future_release(ttak_promise_get_future(promise));
    ttak_promise_set_value(promise, (void *)9, now);
}

static void test_destroy_unfulfilled(void) {
    uint64_t now = ttak_get_tick_count();
    ttak_promise_t *promise = ttak_promise_create(now);
    ASSERT(promise != NULL);
    ttak_promise_destroy(promise);
    ttak_promise_destroy(NULL);
    ttak_future_release(NULL);
}

/* Each async call allocates a pair; without release this runs out. */
static void test_async_call_churn(void) {
    for (uintptr_t i = 0; i < TEST_RELEASE_CHURN; ++i) {
        ttak_async_future_t handle = ttak_async_call(double_it, (void *)i, NULL, 0);
        ASSERT(handle.future != NULL);
        ASSERT(ttak_async_await(&handle) == (void *)(i * 2));
        ttak_async_future_cleanup(&handle);
    }
}

#if !TTAK_PLATFORM_BAREMETAL || TTAK_PLATFORM_FREERTOS
static void test_pool_submit_churn(void) {
    uint64_t now = ttak_get_tick_count();
    ttak_thread_pool_t *pool = ttak_thread_pool_create(2, 0, now);
    ASSERT(pool != NULL);

    for (uintptr_t i = 0; i < TEST_RELEASE_CHURN; ++i) {
        ttak_future_t *future = ttak_thread_pool_submit_task(pool, double_it, (void *)i, 0, now);
        ASSERT(future != NULL);
        ASSERT(ttak_future_get(future) == (void *)(i * 2));
        ttak_future_release(future);
    }

    /* Abandon results: release while the tasks may still be queued. Kept
     * short so the backlog fits in MCU memory. */
    for (uintptr_t i = 0; i < 64; ++i) {
        ttak_future_t *future = ttak_thread_pool_submit_task(pool, double_it, (void *)i, 0, now);
        ASSERT(future != NULL);
        ttak_future_release(future);
    }

    ttak_thread_pool_destroy(pool);
}
#endif

int main(void) {
    RUN_TEST(test_release_after_fulfil);
    RUN_TEST(test_release_before_fulfil);
    RUN_TEST(test_destroy_unfulfilled);
    RUN_TEST(test_async_call_churn);
#if !TTAK_PLATFORM_BAREMETAL || TTAK_PLATFORM_FREERTOS
    RUN_TEST(test_pool_submit_churn);
#endif
    return 0;
}
