#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <ttak/async/future.h>
#include <ttak/thread/pool.h>
#include <ttak/timing/timing.h>

static void *echo_task(void *arg) {
    return arg;
}

int main(void) {
    uint64_t now = ttak_get_tick_count();
    ttak_thread_pool_t *pool = ttak_thread_pool_create(2, 0, now);
    if (!pool) {
        fputs("thread pool unavailable\n", stderr);
        return 1;
    }

    ttak_future_t *future =
        ttak_thread_pool_submit_task(pool, echo_task, (void *)"async hello", 0, now);
    if (future) {
        char *result = (char *)ttak_future_get(future);
        printf("future resolved: %s\n", result);
        ttak_future_release(future);
    }

    ttak_thread_pool_destroy(pool);
    return 0;
}
