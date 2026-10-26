#include <ttak/thread/pool.h>
#include <ttak/async/future.h>
#include <ttak/timing/timing.h>
#include <unistd.h>
#include "test_macros.h"

void *thread_func(void *arg) {
    int *val = (int *)arg;
    *val = 42;
    return (void *)123;
}

void test_thread_pool_basic() {
    uint64_t now = ttak_get_tick_count();
    ttak_thread_pool_t *pool = ttak_thread_pool_create(2, 0, now);
    ASSERT(pool != NULL);
    
    int data = 0;
    ttak_future_t *fut = ttak_thread_pool_submit_task(pool, thread_func, &data, 0, now);
    ASSERT(fut != NULL);
    
    void *result = ttak_future_get(fut);
    ASSERT(result == (void *)123);
    ASSERT(data == 42);
    ttak_future_release(fut);

    ttak_thread_pool_destroy(pool);
}

int main() {
    RUN_TEST(test_thread_pool_basic);
    return 0;
}
