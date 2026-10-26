#include <ttak/async/task.h>
#include <ttak/async/sched.h>
#include <ttak/async/promise.h>
#include <ttak/mem/mem.h>
#include "test_macros.h"

void *my_task_func(void *arg) {
    int *val = (int *)arg;
    (*val)++;
    return NULL;
}

void test_task_create_execute() {
    uint64_t now = 1000;
    int data = 0;
    ttak_task_t *task = ttak_task_create(my_task_func, &data, NULL, now);
    ASSERT(task != NULL);
    
    ttak_task_execute(task, now + 10);
    ASSERT(data == 1);
    
    ttak_task_destroy(task, now + 20);
}

void test_promise_future_basic() {
    uint64_t now = 2000;
    ttak_promise_t *promise = ttak_promise_create(now);
    ASSERT(promise != NULL);
    
    ttak_future_t *future = ttak_promise_get_future(promise);
    ASSERT(future != NULL);
    
    int val = 99;
    ttak_promise_set_value(promise, &val, now + 10);
    
    void *res = ttak_future_get(future);
    ASSERT(res == &val);
    ASSERT(*(int *)res == 99);
    
    ttak_future_release(future);
}

void test_async_schedule_fallback() {
    uint64_t now = 3000;
    int data = 0;
    ttak_task_t *task = ttak_task_create(my_task_func, &data, NULL, now);
    
    ttak_async_schedule(task, now + 10, 0);
    ASSERT(data == 1);
    
    ttak_task_destroy(task, now + 20);
}

void test_async_schedule_with_pool() {
    uint64_t now = 4000;
    int data = 0;
    ttak_promise_t *promise = ttak_promise_create(now);
    ASSERT(promise != NULL);
    ttak_future_t *future = ttak_promise_get_future(promise);
    ASSERT(future != NULL);

    ttak_task_t *task = ttak_task_create(my_task_func, &data, promise, now);
    ASSERT(task != NULL);

    ttak_async_init(0);
    ttak_async_schedule(task, now + 10, 1);

    void *res = ttak_future_get(future);
    ASSERT(res == NULL);
    ASSERT(data == 1);

    ttak_task_destroy(task, now + 20);
    ttak_async_shutdown();

    ttak_future_release(future);
}

void test_task_metadata_domain_urgency() {
    uint64_t now = 5000;
    ttak_task_t *task = ttak_task_create(my_task_func, NULL, NULL, now);
    ASSERT(task != NULL);

    ASSERT(ttak_task_get_domain(task) == TTAK_TASK_DOMAIN_THREAD);
    ASSERT(ttak_task_get_urgency(task) == 0);

    ttak_task_set_domain(task, TTAK_TASK_DOMAIN_NET);
    ttak_task_set_urgency(task, 77);
    ASSERT(ttak_task_get_domain(task) == TTAK_TASK_DOMAIN_NET);
    ASSERT(ttak_task_get_urgency(task) == 77);

    ttak_task_set_urgency(task, 250);
    ASSERT(ttak_task_get_urgency(task) == 100);

    ttak_task_destroy(task, now + 10);
}

int main() {
    RUN_TEST(test_task_create_execute);
    RUN_TEST(test_promise_future_basic);
    RUN_TEST(test_async_schedule_fallback);
    RUN_TEST(test_async_schedule_with_pool);
    RUN_TEST(test_task_metadata_domain_urgency);
    return 0;
}
