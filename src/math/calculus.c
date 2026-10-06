#include <ttak/math/calculus.h>
#include <ttak/thread/pool.h>
#include <ttak/async/future.h>
#include <ttak/mem/mem.h>
#include <stdio.h>
#include <math.h>

_Bool ttak_calculus_diff(ttak_bigreal_t *res, ttak_math_func_t f, const ttak_bigreal_t *x, void *ctx, uint64_t now) {
    if (!res || !f || !x) return false;
    
    // f'(x) approx (f(x+h) - f(x-h)) / 2h
    ttak_bigreal_t h, x_plus, x_minus, f_plus, f_minus, num, den;
    ttak_bigreal_init(&h, now);
    ttak_bigreal_init(&x_plus, now);
    ttak_bigreal_init(&x_minus, now);
    ttak_bigreal_init(&f_plus, now);
    ttak_bigreal_init(&f_minus, now);
    ttak_bigreal_init(&num, now);
    ttak_bigreal_init(&den, now);
    
    // Set small h
    ttak_bigreal_init_u64(&h, 1, now);
    h.exponent = -10; // h = 2^-10
    
    ttak_bigreal_add(&x_plus, x, &h, now);
    ttak_bigreal_sub(&x_minus, x, &h, now);
    
    if (!f(&f_plus, &x_plus, ctx, now) || !f(&f_minus, &x_minus, ctx, now)) {
        goto cleanup;
    }
    
    ttak_bigreal_sub(&num, &f_plus, &f_minus, now);
    ttak_bigreal_init_u64(&den, 2, now);
    ttak_bigreal_mul(&den, &den, &h, now);
    
    ttak_bigreal_div(res, &num, &den, now);
    
cleanup:
    ttak_bigreal_free(&h, now);
    ttak_bigreal_free(&x_plus, now);
    ttak_bigreal_free(&x_minus, now);
    ttak_bigreal_free(&f_plus, now);
    ttak_bigreal_free(&f_minus, now);
    ttak_bigreal_free(&num, now);
    ttak_bigreal_free(&den, now);
    
    return true;
}

_Bool ttak_calculus_partial_diff(ttak_bigreal_t *res, ttak_math_func_t f, const ttak_bigreal_t *x_vec, uint8_t dim, void *ctx, uint64_t now) {
    (void)res; (void)f; (void)x_vec; (void)dim; (void)ctx; (void)now;
    return false; // Not implemented for now.
}

typedef struct {
    ttak_math_func_t f;
    ttak_bigreal_t a, b;
    void *ctx;
    uint64_t now;
    ttak_bigreal_t result;
} integrate_interval_t;

static void* integrate_worker(void *arg) {
    integrate_interval_t *task = (integrate_interval_t *)arg;
    
    // Simple Simpson's rule for the interval [a, b]
    ttak_bigreal_t h, mid, fa, fb, fmid, t1, t2;
    ttak_bigreal_init(&h, task->now);
    ttak_bigreal_init(&mid, task->now);
    ttak_bigreal_init(&fa, task->now);
    ttak_bigreal_init(&fb, task->now);
    ttak_bigreal_init(&fmid, task->now);
    ttak_bigreal_init(&t1, task->now);
    ttak_bigreal_init(&t2, task->now);
    
    task->f(&fa, &task->a, task->ctx, task->now);
    task->f(&fb, &task->b, task->ctx, task->now);
    
    ttak_bigreal_add(&mid, &task->a, &task->b, task->now);
    ttak_bigreal_init_u64(&t1, 2, task->now);
    ttak_bigreal_div(&mid, &mid, &t1, task->now);
    
    task->f(&fmid, &mid, task->ctx, task->now);
    
    // Result = (b-a)/6 * (fa + 4*fmid + fb)
    ttak_bigreal_sub(&h, &task->b, &task->a, task->now);
    ttak_bigreal_init_u64(&t1, 6, task->now);
    ttak_bigreal_div(&h, &h, &t1, task->now);
    
    /* Simpson's rule weighted sum: fa + 4*fmid + fb */
    ttak_bigreal_init_u64(&t1, 4, task->now);
    ttak_bigreal_mul(&t1, &t1, &fmid, task->now);
    ttak_bigreal_add(&t1, &t1, &fa, task->now);
    ttak_bigreal_add(&t1, &t1, &fb, task->now);
    
    ttak_bigreal_mul(&task->result, &h, &t1, task->now);
    
    ttak_bigreal_free(&h, task->now);
    ttak_bigreal_free(&mid, task->now);
    ttak_bigreal_free(&fa, task->now);
    ttak_bigreal_free(&fb, task->now);
    ttak_bigreal_free(&fmid, task->now);
    ttak_bigreal_free(&t1, task->now);
    ttak_bigreal_free(&t2, task->now);
    
    return NULL;
}

_Bool ttak_calculus_integrate(ttak_bigreal_t *res, ttak_math_func_t f, const ttak_bigreal_t *a, const ttak_bigreal_t *b, void *ctx, uint64_t now) {
    if (!res || !f || !a || !b) return false;
    
    int num_intervals = 4;
    integrate_interval_t intervals[4];
    ttak_future_t *futures[4];
    
    ttak_bigreal_t step, current;
    ttak_bigreal_init(&step, now);
    ttak_bigreal_init(&current, now);
    
    ttak_bigreal_sub(&step, b, a, now);
    ttak_bigreal_init_u64(&current, num_intervals, now);
    ttak_bigreal_div(&step, &step, &current, now);
    
    ttak_bigreal_copy(&current, a, now);
    
    for (int i = 0; i < num_intervals; i++) {
        intervals[i].f = f;
        intervals[i].ctx = ctx;
        intervals[i].now = now;
        ttak_bigreal_init(&intervals[i].result, now);
        ttak_bigreal_copy(&intervals[i].a, &current, now);
        ttak_bigreal_add(&current, &current, &step, now);
        ttak_bigreal_copy(&intervals[i].b, &current, now);
        
        futures[i] = ttak_thread_pool_submit_task(async_pool, integrate_worker, &intervals[i], __TT_SCHED_NORMAL__, now);
    }
    
    ttak_bigreal_init_u64(res, 0, now);
    for (int i = 0; i < num_intervals; i++) {
        if (futures[i]) {
            ttak_future_get(futures[i]);
            ttak_bigreal_add(res, res, &intervals[i].result, now);
        }
    }
    
    for (int i = 0; i < num_intervals; i++) {
        ttak_bigreal_free(&intervals[i].a, now);
        ttak_bigreal_free(&intervals[i].b, now);
        ttak_bigreal_free(&intervals[i].result, now);
    }
    ttak_bigreal_free(&step, now);
    ttak_bigreal_free(&current, now);
    
    return true;
}

_Bool ttak_calculus_rk4_step(ttak_bigreal_t *y_next, ttak_math_func_t f, const ttak_bigreal_t *t, const ttak_bigreal_t *y, const ttak_bigreal_t *h, void *ctx, uint64_t now) {
    if (!y_next || !f || !t || !y || !h) return false;

    _Bool success = false;
    ttak_bigreal_t k1, k2, k3, k4;
    ttak_bigreal_t tmp_t, tmp_y, h_half, h_sixth;
    
    ttak_bigreal_init(&k1, now);
    ttak_bigreal_init(&k2, now);
    ttak_bigreal_init(&k3, now);
    ttak_bigreal_init(&k4, now);
    ttak_bigreal_init(&tmp_t, now);
    ttak_bigreal_init(&tmp_y, now);
    ttak_bigreal_init(&h_half, now);
    ttak_bigreal_init(&h_sixth, now);

    ttak_bigreal_init_u64(&tmp_t, 2, now);
    ttak_bigreal_div(&h_half, h, &tmp_t, now);
    ttak_bigreal_init_u64(&tmp_t, 6, now);
    ttak_bigreal_div(&h_sixth, h, &tmp_t, now);

    // k1 = f(t, y)
    if (!f(&k1, y, ctx, now)) goto cleanup;

    // k2 = f(t + h/2, y + h/2 * k1)
    ttak_bigreal_add(&tmp_t, t, &h_half, now);
    ttak_bigreal_mul(&tmp_y, &h_half, &k1, now);
    ttak_bigreal_add(&tmp_y, y, &tmp_y, now);
    if (!f(&k2, &tmp_y, ctx, now)) goto cleanup;

    // k3 = f(t + h/2, y + h/2 * k2)
    ttak_bigreal_mul(&tmp_y, &h_half, &k2, now);
    ttak_bigreal_add(&tmp_y, y, &tmp_y, now);
    if (!f(&k3, &tmp_y, ctx, now)) goto cleanup;

    // k4 = f(t + h, y + h * k3)
    ttak_bigreal_add(&tmp_t, t, h, now);
    ttak_bigreal_mul(&tmp_y, h, &k3, now);
    ttak_bigreal_add(&tmp_y, y, &tmp_y, now);
    if (!f(&k4, &tmp_y, ctx, now)) goto cleanup;

    // y_next = y + h/6 * (k1 + 2*k2 + 2*k3 + k4)
    ttak_bigreal_t sum_k, factor_2;
    ttak_bigreal_init(&sum_k, now);
    ttak_bigreal_init_u64(&factor_2, 2, now);

    /* Weighted stage accumulation: k1 + 2*k2 + 2*k3 + k4 */
    ttak_bigreal_copy(&sum_k, &k1, now);
    
    ttak_bigreal_mul(&tmp_y, &factor_2, &k2, now);
    ttak_bigreal_add(&sum_k, &sum_k, &tmp_y, now);
    
    ttak_bigreal_mul(&tmp_y, &factor_2, &k3, now);
    ttak_bigreal_add(&sum_k, &sum_k, &tmp_y, now);
    
    ttak_bigreal_add(&sum_k, &sum_k, &k4, now);

    ttak_bigreal_mul(&tmp_y, &h_sixth, &sum_k, now);
    ttak_bigreal_add(y_next, y, &tmp_y, now);

    ttak_bigreal_free(&sum_k, now);
    ttak_bigreal_free(&factor_2, now);

    success = true;
cleanup:
    ttak_bigreal_free(&k1, now);
    ttak_bigreal_free(&k2, now);
    ttak_bigreal_free(&k3, now);
    ttak_bigreal_free(&k4, now);
    ttak_bigreal_free(&tmp_t, now);
    ttak_bigreal_free(&tmp_y, now);
    ttak_bigreal_free(&h_half, now);
    ttak_bigreal_free(&h_sixth, now);
    return success;
}
