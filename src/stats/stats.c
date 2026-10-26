#include <ttak/stats/stats.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/**
 * @brief Initializes stats with defined histogram range.
 */
void ttak_stats_init(ttak_stats_t *s, uint64_t hist_min, uint64_t hist_max) {
    s->count = 0;
    s->sum = 0;
    s->min = UINT64_MAX;
    s->max = 0;
    memset(s->histogram, 0, sizeof(s->histogram));
    s->hist_min = hist_min;
    s->hist_max = hist_max;
    // Calculate bucket size
    s->hist_step = (hist_max > hist_min) ? (hist_max - hist_min) / TTAK_STATS_HIST_BUCKETS : 1;
    if (s->hist_step == 0) s->hist_step = 1;
    ttak_spin_init(&s->lock);
}

/**
 * @brief Threads-safe recording of a value.
 */
void ttak_stats_record(ttak_stats_t *s, uint64_t value) {
    ttak_spin_lock(&s->lock);
    s->count++;
    s->sum += value;
    if (value < s->min) s->min = value;
    if (value > s->max) s->max = value;
    
    // Update histogram bucket
    if (value >= s->hist_min && value < s->hist_max) {
        size_t idx = (value - s->hist_min) / s->hist_step;
        if (idx >= TTAK_STATS_HIST_BUCKETS) idx = TTAK_STATS_HIST_BUCKETS - 1;
        s->histogram[idx]++;
    }
    ttak_spin_unlock(&s->lock);
}

#include <stdlib.h>
#include <ttak/math/bigreal.h>

static int compare_u64(const void *a, const void *b) {
    uint64_t val_a = *(const uint64_t *)a;
    uint64_t val_b = *(const uint64_t *)b;
    if (val_a < val_b) return -1;
    if (val_a > val_b) return 1;
    return 0;
}

_Bool ttak_stats_compute_percentiles(uint64_t *data, size_t count, 
                                   ttak_bigreal_t *p50, ttak_bigreal_t *p95, 
                                   ttak_bigreal_t *p99, ttak_bigreal_t *p999, 
                                   uint64_t now) {
    if (!data || count == 0) return false;
    
    // Sort a copy of the data
    uint64_t *sorted = malloc(count * sizeof(uint64_t));
    if (!sorted) return false;
    memcpy(sorted, data, count * sizeof(uint64_t));
    qsort(sorted, count, sizeof(uint64_t), compare_u64);
    
    size_t i50 = (count * 50) / 100;
    size_t i95 = (count * 95) / 100;
    size_t i99 = (count * 99) / 100;
    size_t i999 = (count * 999) / 1000;
    
    if (i50 >= count) i50 = count - 1;
    if (i95 >= count) i95 = count - 1;
    if (i99 >= count) i99 = count - 1;
    if (i999 >= count) i999 = count - 1;
    
    ttak_bigint_set_u64(&p50->mantissa, sorted[i50], now);
    p50->exponent = 0;
    ttak_bigint_set_u64(&p95->mantissa, sorted[i95], now);
    p95->exponent = 0;
    ttak_bigint_set_u64(&p99->mantissa, sorted[i99], now);
    p99->exponent = 0;
    ttak_bigint_set_u64(&p999->mantissa, sorted[i999], now);
    p999->exponent = 0;
    
    free(sorted);
    return true;
}

/**
 * @brief Calculates mean.
 */
double ttak_stats_mean(ttak_stats_t *s) {
    ttak_spin_lock(&s->lock);
    double mean = (s->count > 0) ? (double)s->sum / s->count : 0.0;
    ttak_spin_unlock(&s->lock);
    return mean;
}

/**
 * @brief Prints ASCII representation of stats.
 */
void ttak_stats_print_ascii(ttak_stats_t *s) {
    ttak_spin_lock(&s->lock);
    printf("Stats: Count=%" PRIu64 ", Min=%" PRIu64 ", Max=%" PRIu64 ", Mean=%.2f\n", 
           s->count, s->min, s->max, (s->count > 0) ? (double)s->sum / s->count : 0.0);
    printf("Histogram:\n");
    
    // Find max count for scaling
    uint64_t max_count = 0;
    for (int i = 0; i < TTAK_STATS_HIST_BUCKETS; i++) {
        if (s->histogram[i] > max_count) max_count = s->histogram[i];
    }
    
    // Print bars
    for (int i = 0; i < TTAK_STATS_HIST_BUCKETS; i++) {
        uint64_t lower = s->hist_min + i * s->hist_step;
        uint64_t upper = lower + s->hist_step;
        printf("[%4" PRIu64 "-%4" PRIu64 "] ", lower, upper); 
        
        int bars = 0;
        if (max_count > 0) {
            bars = (s->histogram[i] * 20) / max_count;
        }
        
        for (int j = 0; j < bars; j++) printf("#");
        printf(" (%" PRIu64 ")\n", s->histogram[i]);
    }
    ttak_spin_unlock(&s->lock);
}