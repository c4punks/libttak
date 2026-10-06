#include <ttak/math/bigreal.h>
#include <ttak/math/bigcomplex.h>
#include <ttak/math/ntt.h>
#include <ttak/mem/mem.h>
#include "test_macros.h"
#include <string.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static uint64_t bigint_as_u64(const ttak_bigint_t *bi) {
    const limb_t *limbs = bi->is_dynamic ? bi->data.dyn_ptr : bi->data.sso_buf;
    uint64_t value = 0;
    size_t max_limbs = bi->used < 2 ? bi->used : 2;
    for (size_t i = 0; i < max_limbs; ++i) {
        value |= ((uint64_t)limbs[i]) << (i * 32);
    }
    return value;
}

void test_bigreal_init() {
    ttak_bigreal_t *br = ttak_mem_alloc_raw(sizeof(ttak_bigreal_t), __TTAK_UNSAFE_MEM_FOREVER__, 100);
    ASSERT(br != NULL);
    ttak_bigreal_init(br, 100);
    ASSERT(br->exponent == 0);
    ttak_bigreal_free(br, 101);
    ttak_mem_free(br);
}

void test_bigcomplex_init() {
    ttak_bigcomplex_t *bc = ttak_mem_alloc_raw(sizeof(ttak_bigcomplex_t), __TTAK_UNSAFE_MEM_FOREVER__, 200);
    ASSERT(bc != NULL);
    ttak_bigcomplex_init(bc, 200);
    ASSERT(bc->real.exponent == 0);
    ASSERT(bc->imag.exponent == 0);
    ttak_bigcomplex_free(bc, 201);
    ttak_mem_free(bc);
}

void test_bigreal_add_basic() {
    ttak_bigreal_t lhs, rhs, dst;
    ttak_bigreal_init(&lhs, 300);
    ttak_bigreal_init(&rhs, 301);
    ttak_bigreal_init(&dst, 302);

    lhs.exponent = rhs.exponent = 7;
    ASSERT(ttak_bigint_set_u64(&lhs.mantissa, 100, 303));
    ASSERT(ttak_bigint_set_u64(&rhs.mantissa, 50, 304));

    ASSERT(ttak_bigreal_add(&dst, &lhs, &rhs, 305));
    ASSERT(dst.exponent == 7);
    ASSERT(bigint_as_u64(&dst.mantissa) == 150);

    rhs.exponent = 8;
    ASSERT(ttak_bigreal_add(&dst, &lhs, &rhs, 306));
    ASSERT(dst.exponent == 7);
    ASSERT(bigint_as_u64(&dst.mantissa) == 600); // 100*10^7 + 50*10^8 = 100*10^7 + 500*10^7 = 600*10^7

    ttak_bigreal_free(&lhs, 307);
    ttak_bigreal_free(&rhs, 308);
    ttak_bigreal_free(&dst, 309);
}

void test_bigcomplex_add_basic() {
    ttak_bigcomplex_t lhs, rhs, dst;
    ttak_bigcomplex_init(&lhs, 400);
    ttak_bigcomplex_init(&rhs, 401);
    ttak_bigcomplex_init(&dst, 402);

    lhs.real.exponent = rhs.real.exponent = 1;
    lhs.imag.exponent = rhs.imag.exponent = 3;
    ASSERT(ttak_bigint_set_u64(&lhs.real.mantissa, 10, 403));
    ASSERT(ttak_bigint_set_u64(&rhs.real.mantissa, 20, 404));
    ASSERT(ttak_bigint_set_u64(&lhs.imag.mantissa, 5, 405));
    ASSERT(ttak_bigint_set_u64(&rhs.imag.mantissa, 15, 406));

    ASSERT(ttak_bigcomplex_add(&dst, &lhs, &rhs, 407));
    ASSERT(dst.real.exponent == 1);
    ASSERT(dst.imag.exponent == 3);
    ASSERT(bigint_as_u64(&dst.real.mantissa) == 30);
    ASSERT(bigint_as_u64(&dst.imag.mantissa) == 20);

    rhs.imag.exponent = 4;
    ASSERT(ttak_bigcomplex_add(&dst, &lhs, &rhs, 408));
    ASSERT(dst.real.exponent == 1);
    ASSERT(dst.imag.exponent == 3);
    ASSERT(bigint_as_u64(&dst.real.mantissa) == 30);
    ASSERT(bigint_as_u64(&dst.imag.mantissa) == 155); // 5*10^3 + 15*10^4 = 5*10^3 + 150*10^3 = 155*10^3

    ttak_bigcomplex_free(&lhs, 409);
    ttak_bigcomplex_free(&rhs, 410);
    ttak_bigcomplex_free(&dst, 411);
}

void test_ntt_roundtrip() {
    const ttak_ntt_prime_t *prime = &ttak_ntt_primes[0];
    uint64_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t expected[8];
    memcpy(expected, data, sizeof(data));

    ASSERT(ttak_ntt_transform(data, ARRAY_SIZE(data), prime, false));
    ASSERT(ttak_ntt_transform(data, ARRAY_SIZE(data), prime, true));

    for (size_t i = 0; i < ARRAY_SIZE(data); ++i) {
        ASSERT(data[i] == expected[i] % prime->modulus);
    }
}

void test_ntt_pointwise_mul() {
    const ttak_ntt_prime_t *prime = &ttak_ntt_primes[1];
    size_t n = 4;
    uint64_t left[4] = {1, 2, 3, 4};
    uint64_t right[4] = {5, 6, 7, 8};
    uint64_t result[4];

    ASSERT(ttak_ntt_transform(left, n, prime, false));
    ASSERT(ttak_ntt_transform(right, n, prime, false));
    ttak_ntt_pointwise_mul(result, left, right, n, prime);
    ASSERT(ttak_ntt_transform(result, n, prime, true));

    uint64_t expected[4];
    for (size_t i = 0; i < n; ++i) expected[i] = 0;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            expected[(i + j) % n] = (expected[(i + j) % n] + (i + 1) * (j + 5)) % prime->modulus;
        }
    }

    for (size_t i = 0; i < n; ++i) {
        ASSERT(result[i] == expected[i]);
    }
}

void test_crt_combine_basic() {
    ttak_u128_t value = ttak_u128_shl(ttak_u128_from_u64(1), 96);
    value = ttak_u128_add64(value, 0x123456789ULL);
    ttak_crt_term_t terms[2];
    terms[0].modulus = ttak_ntt_primes[0].modulus;
    terms[0].residue = ttak_u128_mod_u64(value, terms[0].modulus);
    terms[1].modulus = ttak_ntt_primes[1].modulus;
    terms[1].residue = ttak_u128_mod_u64(value, terms[1].modulus);

    ttak_u128_t result, modulus;
    ASSERT(ttak_crt_combine(terms, 2, &result, &modulus));

    ASSERT(ttak_u128_mod_u64(result, terms[0].modulus) == terms[0].residue);
    ASSERT(ttak_u128_mod_u64(result, terms[1].modulus) == terms[1].residue);
    ASSERT(ttak_u128_cmp(result, modulus) < 0);
}

void test_next_power_of_two(void) {
    ASSERT(ttak_next_power_of_two(0) == 1);
    ASSERT(ttak_next_power_of_two(1) == 1);
    ASSERT(ttak_next_power_of_two(3) == 4);
    ASSERT(ttak_next_power_of_two(17) == 32);
}

#include <ttak/math/calculus.h>
#include <ttak/math/matrix.h>

static _Bool multivar_quad_func(ttak_bigreal_t *res, const ttak_bigreal_t *x_vec, uint8_t dim, void *ctx, uint64_t now) {
    (void)ctx;
    if (dim < 2) return false;
    // f(x, y) = 3*x^2 + 5*y
    ttak_bigreal_t c3, c5, x2, t1, t2;
    ttak_bigreal_init_u64(&c3, 3, now);
    ttak_bigreal_init_u64(&c5, 5, now);
    ttak_bigreal_init(&x2, now);
    ttak_bigreal_init(&t1, now);
    ttak_bigreal_init(&t2, now);

    ttak_bigreal_mul(&x2, &x_vec[0], &x_vec[0], now);
    ttak_bigreal_mul(&t1, &c3, &x2, now);
    ttak_bigreal_mul(&t2, &c5, &x_vec[1], now);
    ttak_bigreal_add(res, &t1, &t2, now);

    ttak_bigreal_free(&c3, now);
    ttak_bigreal_free(&c5, now);
    ttak_bigreal_free(&x2, now);
    ttak_bigreal_free(&t1, now);
    ttak_bigreal_free(&t2, now);
    return true;
}

void test_calculus_partial_diff(void) {
    uint64_t now = 5000;
    ttak_bigreal_t pt[2];
    ttak_bigreal_init_u64(&pt[0], 2, now); // x = 2
    ttak_bigreal_init_u64(&pt[1], 4, now); // y = 4

    ttak_bigreal_t df_dx, df_dy;
    ttak_bigreal_init(&df_dx, now);
    ttak_bigreal_init(&df_dy, now);

    // df/dx = 6*x = 12 at x=2
    ASSERT(ttak_calculus_partial_diff(&df_dx, multivar_quad_func, pt, 2, 0, NULL, now));
    // df/dy = 5 at y=4
    ASSERT(ttak_calculus_partial_diff(&df_dy, multivar_quad_func, pt, 2, 1, NULL, now));

    // df/dx = 12 * 10^0
    // df/dy = 5 * 10^0
    ttak_bigreal_t exp_dx, exp_dy, diff_val, eps;
    ttak_bigreal_init_u64(&exp_dx, 12, now);
    ttak_bigreal_init_u64(&exp_dy, 5, now);
    ttak_bigreal_init(&diff_val, now);
    ttak_bigreal_init_u64(&eps, 1, now); // eps = 0.1
    eps.exponent = -1;

    ASSERT(ttak_bigreal_sub(&diff_val, &df_dx, &exp_dx, now));
    diff_val.mantissa.is_negative = false;
    ASSERT(ttak_bigreal_cmp(&diff_val, &eps, now) <= 0);

    ASSERT(ttak_bigreal_sub(&diff_val, &df_dy, &exp_dy, now));
    diff_val.mantissa.is_negative = false;
    ASSERT(ttak_bigreal_cmp(&diff_val, &eps, now) <= 0);

    ttak_bigreal_free(&exp_dx, now);
    ttak_bigreal_free(&exp_dy, now);
    ttak_bigreal_free(&diff_val, now);
    ttak_bigreal_free(&eps, now);

    ttak_bigreal_free(&pt[0], now);
    ttak_bigreal_free(&pt[1], now);
    ttak_bigreal_free(&df_dx, now);
    ttak_bigreal_free(&df_dy, now);
}

void test_matrix_shearing(void) {
    uint64_t now = 6000;
    tt_owner_t *owner = ttak_owner_create(0);
    ASSERT(owner != NULL);

    tt_shared_matrix_t *sm = ttak_matrix_create(2, 2, owner, now);
    ASSERT(sm != NULL);

    ttak_bigreal_t factor;
    ttak_bigreal_init_u64(&factor, 3, now);

    ASSERT(ttak_matrix_set_shearing(sm, owner, 0, &factor, now));

    ttak_bigreal_t *elem01 = ttak_matrix_get(sm, owner, 0, 1, now);
    ASSERT(elem01 != NULL);
    ASSERT(bigint_as_u64(&elem01->mantissa) == 3);

    ttak_bigreal_free(&factor, now);
    ttak_owner_destroy(owner);
}

void test_matrix_determinant_invert(void) {
    uint64_t now = 7000;
    tt_owner_t *owner = ttak_owner_create(0);
    ASSERT(owner != NULL);

    tt_shared_matrix_t *sm = ttak_matrix_create(2, 2, owner, now);
    tt_shared_matrix_t *inv = ttak_matrix_create(2, 2, owner, now);
    ASSERT(sm != NULL && inv != NULL);

    // [[4, 7], [2, 6]] -> det = 24 - 14 = 10
    ttak_bigreal_t v4, v7, v2, v6;
    ttak_bigreal_init_u64(&v4, 4, now);
    ttak_bigreal_init_u64(&v7, 7, now);
    ttak_bigreal_init_u64(&v2, 2, now);
    ttak_bigreal_init_u64(&v6, 6, now);

    ASSERT(ttak_matrix_set(sm, owner, 0, 0, &v4, now));
    ASSERT(ttak_matrix_set(sm, owner, 0, 1, &v7, now));
    ASSERT(ttak_matrix_set(sm, owner, 1, 0, &v2, now));
    ASSERT(ttak_matrix_set(sm, owner, 1, 1, &v6, now));

    ttak_bigreal_t det;
    ttak_bigreal_init(&det, now);
    ASSERT(ttak_matrix_determinant(&det, sm, owner, now));
    ASSERT(bigint_as_u64(&det.mantissa) == 10);

    ASSERT(ttak_matrix_invert(inv, sm, owner, now));

    ttak_bigreal_free(&v4, now);
    ttak_bigreal_free(&v7, now);
    ttak_bigreal_free(&v2, now);
    ttak_bigreal_free(&v6, now);
    ttak_bigreal_free(&det, now);
    ttak_owner_destroy(owner);
}

int main(void) {
    RUN_TEST(test_bigreal_init);
    RUN_TEST(test_bigcomplex_init);
    RUN_TEST(test_bigreal_add_basic);
    RUN_TEST(test_bigcomplex_add_basic);
    RUN_TEST(test_ntt_roundtrip);
    RUN_TEST(test_ntt_pointwise_mul);
    RUN_TEST(test_crt_combine_basic);
    RUN_TEST(test_next_power_of_two);
    RUN_TEST(test_calculus_partial_diff);
    RUN_TEST(test_matrix_shearing);
    RUN_TEST(test_matrix_determinant_invert);
    return 0;
}
