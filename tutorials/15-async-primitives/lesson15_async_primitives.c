#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <ttak/async/future.h>
#include <ttak/async/promise.h>
#include <ttak/timing/timing.h>

int main(void) {
    uint64_t now = ttak_get_tick_count();
    ttak_promise_t *promise = ttak_promise_create(now);
    if (!promise) {
        fputs("promise allocation failed\n", stderr);
        return 1;
    }

    ttak_future_t *future = ttak_promise_get_future(promise);
    ttak_promise_set_value(promise, (void *)"promise delivered", now);
    char *result = (char *)ttak_future_get(future);
    printf("future result: %s\n", result);
    ttak_future_release(future); /* frees the future and its promise */
    return 0;
}
