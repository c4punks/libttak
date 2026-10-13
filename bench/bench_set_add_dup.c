/**
 * A/B benchmark for ttak_set_add duplicate-heavy insert throughput.
 *
 * Prefills a set with PREFILL distinct keys, then performs ITERS add
 * attempts where 90% of the keys already exist (duplicates) and 10% are
 * rotating fresh keys. Old ttak_set_add performs a get probe plus a put
 * probe per attempt; the new implementation resolves each attempt with a
 * single probe.
 */
#define _XOPEN_SOURCE 700
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ttak/container/set.h>

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

#define PREFILL 100000u
#define ITERS   1000000u
#define REPEAT  5

static int cmp_keys(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

int main(void) {
    char **pool = malloc((PREFILL + PREFILL / 4) * sizeof(char *));
    for (size_t i = 0; i < PREFILL + PREFILL / 4; i++) {
        pool[i] = malloc(24);
        snprintf(pool[i], 24, "key-%zu", i);
    }

    double best = 1e30;
    for (int r = 0; r < REPEAT; r++) {
        uint64_t now = 0;
        ttak_set_t set;
        ttak_set_init(&set, PREFILL * 2, NULL, cmp_keys, NULL);

        for (size_t i = 0; i < PREFILL; i++)
            ttak_table_put(&set.table, pool[i], strlen(pool[i]) + 1, (void *)1, now);

        double t0 = now_ns();
        for (size_t i = 0; i < ITERS; i++) {
            /* 90% duplicates from the prefilled range, 10% rotating fresh keys */
            size_t idx = (i % 10 == 9) ? PREFILL + ((i / 10) % (PREFILL / 4)) : (i % PREFILL);
            ttak_set_add(&set, pool[idx], strlen(pool[idx]) + 1, now);
        }
        double t1 = now_ns();

        double ns = t1 - t0;
        if (ns < best) best = ns;
        ttak_set_destroy(&set, now);
    }

    printf("best of %d: %.0f ns total, %.2f ns/add\n", REPEAT, best, best / ITERS);
    for (size_t i = 0; i < PREFILL + PREFILL / 4; i++) free(pool[i]);
    free(pool);
    return 0;
}
