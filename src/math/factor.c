#include <ttak/math/factor.h>
#include <ttak/mem/mem.h>

#include <stdbool.h>
#include <string.h>

#if defined(__SIZEOF_INT128__)
#define HAVE_UINT128 1
#endif

typedef struct {
    ttak_prime_factor_t **factors;
    size_t *count;
    size_t *capacity;
    uint64_t rng_state;
    uint64_t now;
} ttak_factor_ctx_t;

static const uint16_t k_small_primes[] = {
    2,  3,  5,  7, 11, 13, 17, 19, 23, 29,
    31, 37, 41, 43, 47, 53, 59, 61, 67, 71,
    73, 79, 83, 89, 97,101,103,107,109,113,
    127,131,137,139,149,151,157,163,167,173,
    179,181,191,193,197,199,211,223,227,229,
    233,239,241,251,257,263,269,271,277,281,
    283,293,307,311,313,317,331,337,347,349,
    353,359,367,373,379,383,389,397,401,409,
    419,421,431,433,439,443,449,457,461,463,
    467,479,487,491,499,503,509,521,523,541,
    547,557,563,569,571,577,587,593,599,601,
    607,613,617,619,631,641,643,647,653,659,
    661,673,677,683,691,701,709,719,727,733,
    739,743,751,757,761,769,773,787,797,809,
    811,821,823,827,829,839,853,857,859,863,
    877,881,883,887,907,911,919,929,937,941,
    947,953,967,971,977,983,991,997
};

/**
 * @brief Compute Greatest Common Divisor using the binary GCD (Stein) algorithm.
 *
 * Uses shift/subtract steps instead of division for efficient modular arithmetic.
 */
static inline uint64_t ttak_gcd_u64(uint64_t a, uint64_t b) {
    if (a == 0) return b;
    if (b == 0) return a;

    int shift = __builtin_ctzll(a | b);
    a >>= __builtin_ctzll(a);
    do {
        b >>= __builtin_ctzll(b);
        if (a > b) {
            uint64_t t = b;
            b = a;
            a = t;
        }
        b = b - a;
    } while (b != 0);

    return a << shift;
}

static inline uint64_t ttak_abs_diff_u64(uint64_t a, uint64_t b) {
    return (a > b) ? (a - b) : (b - a);
}

static inline uint64_t ttak_prng_next(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline uint64_t ttak_mulmod_u64(uint64_t a, uint64_t b, uint64_t mod) {
#if HAVE_UINT128
    return (uint64_t)(((__uint128_t)a * (__uint128_t)b) % mod);
#else
    uint64_t result = 0;
    while (b > 0) {
        if (b & 1) {
            result = (result + a) % mod;
        }
        a = (a << 1) % mod;
        b >>= 1;
    }
    return result;
#endif
}

static uint64_t ttak_powmod_u64(uint64_t base, uint64_t exponent, uint64_t mod) {
    uint64_t result = 1ULL;
    while (exponent > 0) {
        if (exponent & 1ULL) {
            result = ttak_mulmod_u64(result, base, mod);
        }
        base = ttak_mulmod_u64(base, base, mod);
        exponent >>= 1ULL;
    }
    return result;
}

static bool ttak_miller_rabin_u64(uint64_t n) {
    if (n < 2) return false;
    for (size_t i = 0; i < sizeof(k_small_primes)/sizeof(k_small_primes[0]); ++i) {
        uint64_t p = k_small_primes[i];
        if (n == p) return true;
        if (n % p == 0 && n != p) return false;
    }

    uint64_t d = n - 1;
    uint32_t s = 0;
    while ((d & 1ULL) == 0) {
        d >>= 1ULL;
        ++s;
    }

    static const uint64_t bases[] = {2ULL, 3ULL, 5ULL, 7ULL, 11ULL, 13ULL};
    for (size_t i = 0; i < sizeof(bases)/sizeof(bases[0]); ++i) {
        uint64_t a = bases[i] % n;
        if (a == 0) continue;
        uint64_t x = ttak_powmod_u64(a, d, n);
        if (x == 1ULL || x == n - 1) continue;
        bool witness = true;
        for (uint32_t r = 1; r < s; ++r) {
            x = ttak_mulmod_u64(x, x, n);
            if (x == n - 1) {
                witness = false;
                break;
            }
        }
        if (witness) return false;
    }
    return true;
}

static uint64_t ttak_pollard_rho_brent(uint64_t n, ttak_factor_ctx_t *ctx) {
    if ((n & 1ULL) == 0ULL) return 2ULL;
    uint64_t c = ttak_prng_next(&ctx->rng_state) % (n - 1ULL) + 1ULL;
    uint64_t y = ttak_prng_next(&ctx->rng_state) % (n - 1ULL) + 1ULL;
    uint64_t m = 128ULL;
    uint64_t g = 1ULL;
    uint64_t r = 1ULL;
    uint64_t q = 1ULL;
    uint64_t ys = 0ULL;

    while (g == 1ULL) {
        uint64_t x = y;
        for (uint64_t i = 0; i < r; ++i) {
            y = (ttak_mulmod_u64(y, y, n) + c) % n;
        }
        uint64_t k = 0ULL;
        while (k < r && g == 1ULL) {
            ys = y;
            uint64_t limit = (m < (r - k)) ? m : (r - k);
            for (uint64_t i = 0; i < limit; ++i) {
                y = (ttak_mulmod_u64(y, y, n) + c) % n;
                uint64_t diff = ttak_abs_diff_u64(x, y);
                if (diff == 0) continue;
                q = ttak_mulmod_u64(q, diff, n);
            }
            g = ttak_gcd_u64(q, n);
            k += limit;
        }
        r <<= 1ULL;
    }

    if (g == n) {
        do {
            uint64_t x = ys;
            ys = (ttak_mulmod_u64(ys, ys, n) + c) % n;
            g = ttak_gcd_u64(ttak_abs_diff_u64(x, ys), n);
        } while (g == 1ULL);
    }

    return g;
}

static uint64_t ttak_trial_fallback(uint64_t n) {
    if ((n & 1ULL) == 0ULL) return 2ULL;
    for (uint64_t i = 3; i <= n / i; i += 2) {
        if (n % i == 0) return i;
    }
    return n;
}

static int ttak_record_factor(uint64_t p, ttak_factor_ctx_t *ctx) {
    size_t idx = 0;
    while (idx < *ctx->count && (*ctx->factors)[idx].p < p) {
        idx++;
    }

    if (idx < *ctx->count && (*ctx->factors)[idx].p == p) {
        (*ctx->factors)[idx].a++;
        return 0;
    }

    if (*ctx->count == *ctx->capacity) {
        size_t new_cap = (*ctx->capacity == 0) ? 8 : (*ctx->capacity * 2);
        ttak_prime_factor_t *grown =
            ttak_mem_realloc_raw(*ctx->factors, new_cap * sizeof(ttak_prime_factor_t),
                             __TTAK_UNSAFE_MEM_FOREVER__, ctx->now);
        if (!grown) return -1;
        *ctx->factors = grown;
        *ctx->capacity = new_cap;
    }

    memmove(&(*ctx->factors)[idx + 1],
            &(*ctx->factors)[idx],
            (*ctx->count - idx) * sizeof(ttak_prime_factor_t));

    (*ctx->factors)[idx].p = p;
    (*ctx->factors)[idx].a = 1;
    (*ctx->count)++;
    return 0;
}

static int ttak_factor_recursive(uint64_t n, ttak_factor_ctx_t *ctx) {
    if (n == 1) return 0;
    if (ttak_miller_rabin_u64(n)) {
        return ttak_record_factor(n, ctx);
    }

    uint64_t d = 0;
    for (int attempt = 0; attempt < 32; ++attempt) {
        d = ttak_pollard_rho_brent(n, ctx);
        if (d > 1 && d < n) break;
    }

    if (d == 0 || d == n) {
        d = ttak_trial_fallback(n);
        if (d == n) {
            return ttak_record_factor(n, ctx);
        }
    }

    if (ttak_factor_recursive(d, ctx) != 0) return -1;
    if (ttak_factor_recursive(n / d, ctx) != 0) return -1;
    return 0;
}

int ttak_factor_u64(uint64_t n, ttak_prime_factor_t **factors_out, size_t *count_out, uint64_t now) {
    if (n <= 1) {
        *factors_out = NULL;
        *count_out = 0;
        return 0;
    }

    ttak_prime_factor_t *factors = NULL;
    size_t count = 0;
    size_t capacity = 0;

    ttak_factor_ctx_t ctx = {
        .factors = &factors,
        .count = &count,
        .capacity = &capacity,
        .rng_state = n ^ now ^ 0xA55AA55AA55AA55AULL,
        .now = now
    };

    for (size_t i = 0; i < sizeof(k_small_primes)/sizeof(k_small_primes[0]); ++i) {
        uint16_t p = k_small_primes[i];
        if ((uint64_t)p * (uint64_t)p > n) break;
        while (n % p == 0) {
            if (ttak_record_factor(p, &ctx) != 0) goto fail;
            n /= p;
        }
    }

    if (n > 1) {
        if (ttak_factor_recursive(n, &ctx) != 0) goto fail;
    }

    *factors_out = factors;
    *count_out = count;
    return 0;

fail:
    ttak_mem_free(factors);
    *factors_out = NULL;
    *count_out = 0;
    return -1;
}

/**
 * @brief Append or increment a big prime factor within the dynamic list.
 *
 * @param p         Factor to add.
 * @param factors   Dynamic array pointer.
 * @param count     Number of populated entries.
 * @param capacity  Allocated capacity.
 * @param now       Timestamp for allocations.
 * @return 0 on success, -1 on allocation failure.
 */
static int add_factor_big(const ttak_bigint_t *p, ttak_prime_factor_big_t **factors, size_t *count, size_t *capacity, uint64_t now) {
    for (size_t i = 0; i < *count; ++i) {
        if (ttak_bigint_cmp(&(*factors)[i].p, p) == 0) {
            (*factors)[i].a++;
            return 0;
        }
    }

    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0) ? 8 : *capacity * 2;
        ttak_prime_factor_big_t *new_factors = ttak_mem_realloc_raw(*factors, new_capacity * sizeof(ttak_prime_factor_big_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
        if (!new_factors) return -1;
        *factors = new_factors;
        *capacity = new_capacity;
    }

    ttak_bigint_init_copy(&(*factors)[*count].p, p, now);
    (*factors)[*count].a = 1;
    (*count)++;
    return 0;
}


// NOTE: This is a trial division implementation for big integers. It is correct but will be
// very slow for numbers with large prime factors. For a "fast" implementation as requested,
// this should be replaced or augmented with algorithms like Pollard's Rho or the
// Elliptic Curve Method (ECM), especially for factors beyond a certain bit size.
/**
 * @brief Factor a big integer via naive trial division.
 *
 * @param n           Value to factor (must be > 1).
 * @param factors_out Output array of big factors.
 * @param count_out   Number of factors produced.
 * @param now         Timestamp for allocations.
 * @return 0 on success, -1 when allocation fails.
 */
/* Hard stop so big-int trial division cannot stall forever on tough inputs. */
#define FACTOR_BIG_TRIAL_LIMIT 500000

int ttak_factor_big(const ttak_bigint_t *n, ttak_prime_factor_big_t **factors_out, size_t *count_out, uint64_t now) {
    if (ttak_bigint_is_zero(n) || ttak_bigint_cmp_u64(n, 1) <= 0) {
        *factors_out = NULL;
        *count_out = 0;
        return 0;
    }

    uint64_t small_value = 0;
    if (ttak_bigint_export_u64(n, &small_value)) {
        ttak_prime_factor_t *u64_factors = NULL;
        size_t u64_count = 0;
        if (ttak_factor_u64(small_value, &u64_factors, &u64_count, now) != 0) {
            *factors_out = NULL;
            *count_out = 0;
            return -1;
        }

        ttak_prime_factor_big_t *big_factors =
            ttak_mem_realloc_raw(NULL, u64_count * sizeof(ttak_prime_factor_big_t),
                             __TTAK_UNSAFE_MEM_FOREVER__, now);
        if (!big_factors && u64_count > 0) {
            ttak_mem_free(u64_factors);
            *factors_out = NULL;
            *count_out = 0;
            return -1;
        }

        for (size_t i = 0; i < u64_count; ++i) {
            ttak_bigint_init(&big_factors[i].p, now);
            if (!ttak_bigint_set_u64(&big_factors[i].p, u64_factors[i].p, now)) {
                for (size_t j = 0; j <= i; ++j) {
                    ttak_bigint_free(&big_factors[j].p, now);
                }
                ttak_mem_free(big_factors);
                ttak_mem_free(u64_factors);
                *factors_out = NULL;
                *count_out = 0;
                return -1;
            }
            big_factors[i].a = u64_factors[i].a;
        }

        ttak_mem_free(u64_factors);
        *factors_out = big_factors;
        *count_out = u64_count;
        return 0;
    }

    ttak_prime_factor_big_t *factors = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int ret = -1;

    ttak_bigint_t temp_n, rem, p;
    ttak_bigint_init_copy(&temp_n, n, now);
    ttak_bigint_init(&rem, now);
    ttak_bigint_init_u64(&p, 2, now);

    // Handle factor 2
    ttak_bigint_mod_u64(&rem, &temp_n, 2, now);
    while (ttak_bigint_is_zero(&rem)) {
        if (add_factor_big(&p, &factors, &count, &capacity, now) != 0) goto big_fail;
        ttak_bigint_div_u64(&temp_n, NULL, &temp_n, 2, now);
        ttak_bigint_mod_u64(&rem, &temp_n, 2, now);
    }

    // Handle odd factors
    ttak_bigint_set_u64(&p, 3, now);
    ttak_bigint_t p_squared;
    ttak_bigint_init(&p_squared, now);
    ttak_bigint_mul(&p_squared, &p, &p, now);

    size_t trial_steps = 0;
    while (ttak_bigint_cmp(&p_squared, &temp_n) <= 0) {
        if (trial_steps++ >= FACTOR_BIG_TRIAL_LIMIT) {
            goto big_fail_limit;
        }
        ttak_bigint_mod(&rem, &temp_n, &p, now);
        while (ttak_bigint_is_zero(&rem)) {
            if (add_factor_big(&p, &factors, &count, &capacity, now) != 0) goto big_fail;
            ttak_bigint_div(&temp_n, NULL, &temp_n, &p, now);
            ttak_bigint_mod(&rem, &temp_n, &p, now);
        }
        ttak_bigint_add_u64(&p, &p, 2, now);
        ttak_bigint_mul(&p_squared, &p, &p, now);
    }

    if (ttak_bigint_cmp_u64(&temp_n, 1) > 0) {
        if (add_factor_big(&temp_n, &factors, &count, &capacity, now) != 0) goto big_fail;
    }

    *factors_out = factors;
    *count_out = count;

    ttak_bigint_free(&temp_n, now);
    ttak_bigint_free(&rem, now);
    ttak_bigint_free(&p, now);
    ttak_bigint_free(&p_squared, now);
    return 0;

big_fail:
    ret = -1;
    goto cleanup;

big_fail_limit:
    ret = -2;
    goto cleanup;

cleanup:
    for(size_t i = 0; i < count; ++i) {
        ttak_bigint_free(&factors[i].p, now);
    }
    ttak_mem_free(factors);
    *factors_out = NULL;
    *count_out = 0;
    ttak_bigint_free(&temp_n, now);
    ttak_bigint_free(&rem, now);
    ttak_bigint_free(&p, now);
    ttak_bigint_free(&p_squared, now);
    return ret;
}
