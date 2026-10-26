#include <ttak/timing/timing.h>
#include <ttak/timing/deadline.h>
#include "test_macros.h"
#include <unistd.h>

static void test_timing_basic(void) {
    uint64_t t1 = ttak_get_tick_count();
    usleep(10000); // 10ms
    uint64_t t2 = ttak_get_tick_count();
    
    /* Bare-metal clocks may start at 0, so check the elapsed time instead. */
    ASSERT(t2 - t1 >= 10);
}

static void test_deadline_expiration(void) {
    ttak_deadline_t dl;
    ttak_deadline_set(&dl, 50);
    ASSERT(ttak_deadline_is_expired(&dl) == false);

    /* Poll with a generous wall-clock timeout so the test survives
       scheduler jitter and coarse tick resolution. */
    const uint64_t poll_step_us = 5000;
    const int max_polls = 80; /* 5 ms * 80 = 400 ms ceiling */
    for (int i = 0; i < max_polls; ++i) {
        if (ttak_deadline_is_expired(&dl)) {
            return;
        }
        usleep((useconds_t)poll_step_us);
    }
    ASSERT(ttak_deadline_is_expired(&dl) == true);
}

int main(void) {
    RUN_TEST(test_timing_basic);
    RUN_TEST(test_deadline_expiration);
    return 0;
}
