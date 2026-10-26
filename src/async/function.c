#include <ttak/async/function.h>
#include <ttak/async/sched.h>
#include <ttak/mem/mem.h>
#include <ttak/timing/timing.h>

static inline void ttak_async_cleanup_ctx(ttak_async_cleanup_t cleanup, void *ctx) {
    if (cleanup) {
        cleanup(ctx);
    }
}

ttak_async_future_t ttak_async_call(ttak_task_func_t func,
                                    void *ctx,
                                    ttak_async_cleanup_t cleanup,
                                    int priority) {
    ttak_async_future_t handle = {0};
    if (!func) {
        ttak_async_cleanup_ctx(cleanup, ctx);
        return handle;
    }

    uint64_t now = ttak_get_tick_count();
    handle.promise = ttak_promise_create(now);
    if (!handle.promise) {
        ttak_async_cleanup_ctx(cleanup, ctx);
        return handle;
    }

    handle.future = ttak_promise_get_future(handle.promise);
    if (!handle.future) {
        ttak_async_cleanup_ctx(cleanup, ctx);
        ttak_promise_destroy(handle.promise);
        return (ttak_async_future_t){0};
    }

    ttak_task_t *task = ttak_task_create(func, ctx, handle.promise, now);
    if (!task) {
        ttak_async_cleanup_ctx(cleanup, ctx);
        ttak_promise_destroy(handle.promise);
        return (ttak_async_future_t){0};
    }

    ttak_task_set_start_ts(task, now);
    ttak_async_schedule(task, now, priority);
    ttak_task_destroy(task, now);
    return handle;
}

void *ttak_async_await(ttak_async_future_t *future) {
    if (!future || !future->future) {
        return NULL;
    }
    return ttak_future_get(future->future);
}

void ttak_async_future_cleanup(ttak_async_future_t *future) {
    if (!future) {
        return;
    }

    /* The task may still be running: release defers the free until it
     * has fulfilled the promise. */
    ttak_future_release(future->future);
    future->future = NULL;
    future->promise = NULL;
}
