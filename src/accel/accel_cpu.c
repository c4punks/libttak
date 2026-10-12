#include "ttak/ttak_accelerator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ttak/compat/stdatomic.h>
#include <string.h>

#if !defined(__SIZEOF_INT128__) && !defined(_MSC_VER)
#error "libttak acceleration CPU backend requires __int128 support."
#endif

#define TTAK_ACCEL_FACTOR_MAX 64

typedef struct {
    uint64_t prime;
    uint32_t exponent;
    uint32_t reserved;
} ttak_accel_factor_slot_t;

typedef struct {
    uint64_t value;
    uint32_t factor_count;
    uint32_t checksum;
    uint32_t reserved;
    ttak_accel_factor_slot_t slots[TTAK_ACCEL_FACTOR_MAX];
} ttak_accel_factor_record_t;

typedef struct {
    uint32_t guard;
    uint32_t record_count;
    uint32_t payload_checksum;
    uint32_t reserved;
} ttak_accel_record_prefix_t;

typedef struct {
    uint64_t state;
} ttak_factor_rng_t;

typedef struct {
    uint64_t modulus;
    uint64_t modulus_inv;
    uint64_t r2;
} ttak_monty64_t;

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

static inline uint32_t ttak_guard_word(const ttak_accel_config_t *config,
                                       const ttak_accel_batch_item_t *item) {
    uint32_t guard = config->integrity_mask ^ item->mask_seed;
    guard |= 0x01010101u;
    return guard;
}

static inline uint32_t ttak_checksum_seed(const ttak_accel_batch_item_t *item) {
    return (item->checksum_salt == 0u) ? 2166136261u : item->checksum_salt;
}

static inline uint32_t ttak_fnv1a32(const void *data, size_t len, uint32_t seed) {
    const uint8_t *ptr = (const uint8_t *)data;
    uint32_t hash = (seed == 0u) ? 2166136261u : seed;
    for (size_t i = 0; i < len; ++i) {
        hash ^= ptr[i];
        hash *= 16777619u;
    }
    return hash;
}

static inline void ttak_mask_payload(uint8_t *buf, size_t len, uint32_t guard) {
    const uint8_t lanes[4] = {
        (uint8_t)(guard & 0xFFu),
        (uint8_t)((guard >> 8) & 0xFFu),
        (uint8_t)((guard >> 16) & 0xFFu),
        (uint8_t)((guard >> 24) & 0xFFu)
    };
    for (size_t i = 0; i < len; ++i) {
        buf[i] ^= lanes[i & 0x3u];
    }
}

static inline uint64_t ttak_abs_diff_u64(uint64_t a, uint64_t b) {
    return (a > b) ? (a - b) : (b - a);
}

static inline uint64_t ttak_gcd_u64(uint64_t a, uint64_t b) {
    while (b != 0) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static inline uint64_t ttak_rng_next(ttak_factor_rng_t *rng) {
    uint64_t z = (rng->state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline uint64_t ttak_rng_between(ttak_factor_rng_t *rng,
                                        uint64_t min_inclusive,
                                        uint64_t max_inclusive) {
    if (max_inclusive <= min_inclusive) return min_inclusive;
    uint64_t span = max_inclusive - min_inclusive + 1ULL;
    return min_inclusive + (ttak_rng_next(rng) % span);
}

static uint64_t ttak_monty_compute_inverse(uint64_t n) {
    uint64_t inv = 1;
    for (size_t i = 0; i < 6; ++i) {
        inv *= 2 - n * inv;
    }
    return ~inv + 1ULL;
}

/* ---- Compiler-specific 128-bit modular arithmetic ----------------------
 * MSVC has no __int128; use _umul128/_udiv128 intrinsics instead.
 * GCC/Clang use __uint128_t natively.                                   */
#ifdef _MSC_VER
#include <intrin.h>

static inline uint64_t ttak_mulmod_u64(uint64_t a, uint64_t b, uint64_t mod) {
    uint64_t hi;
    uint64_t lo = _umul128(a, b, &hi);
    if (!hi) return lo % mod;
    uint64_t rem;
    _udiv128(hi, lo, mod, &rem);
    return rem;
}

static void ttak_monty_init(ttak_monty64_t *ctx, uint64_t n) {
    ctx->modulus = n;
    ctx->modulus_inv = ttak_monty_compute_inverse(n);
    /* r  = 2^64 mod n: dividend is hi=1,lo=0; remainder stored in r via &r */
    uint64_t r;
    (void)_udiv128(1ULL, 0ULL, n, &r);
    /* r2 = (r * 2^64) mod n: dividend is hi=r,lo=0 */
    uint64_t r2;
    (void)_udiv128(r, 0ULL, n, &r2);
    ctx->r2 = r2;
}

static inline uint64_t ttak_monty_reduce(const ttak_monty64_t *ctx,
                                         uint64_t t_lo, uint64_t t_hi) {
    uint64_t m = t_lo * ctx->modulus_inv;
    uint64_t prod_hi;
    uint64_t prod_lo = _umul128(m, ctx->modulus, &prod_hi);
    uint64_t u_lo    = t_lo + prod_lo;
    uint64_t carry   = (u_lo < t_lo) ? 1ULL : 0ULL;
    uint64_t u_hi    = t_hi + prod_hi + carry;
    if (u_hi >= ctx->modulus) u_hi -= ctx->modulus;
    return u_hi;
}

static inline uint64_t ttak_monty_to(const ttak_monty64_t *ctx, uint64_t x) {
    uint64_t hi;
    uint64_t lo = _umul128(x, ctx->r2, &hi);
    return ttak_monty_reduce(ctx, lo, hi);
}

static inline uint64_t ttak_monty_from(const ttak_monty64_t *ctx, uint64_t x) {
    return ttak_monty_reduce(ctx, x, 0ULL);
}

static inline uint64_t ttak_monty_mul(const ttak_monty64_t *ctx,
                                      uint64_t a, uint64_t b) {
    uint64_t hi;
    uint64_t lo = _umul128(a, b, &hi);
    return ttak_monty_reduce(ctx, lo, hi);
}

#else /* __SIZEOF_INT128__ available */

static inline uint64_t ttak_mulmod_u64(uint64_t a, uint64_t b, uint64_t mod) {
    return (uint64_t)(((__uint128_t)a * (__uint128_t)b) % mod);
}

static void ttak_monty_init(ttak_monty64_t *ctx, uint64_t n) {
    ctx->modulus = n;
    ctx->modulus_inv = ttak_monty_compute_inverse(n);
    __uint128_t r = (((__uint128_t)1) << 64) % n;
    r = (r * (((__uint128_t)1) << 64)) % n;
    ctx->r2 = (uint64_t)r;
}

static inline uint64_t ttak_monty_reduce(const ttak_monty64_t *ctx,
                                         __uint128_t t) {
    uint64_t m = (uint64_t)t * ctx->modulus_inv;
    __uint128_t u = t + (__uint128_t)m * ctx->modulus;
    uint64_t res = (uint64_t)(u >> 64);
    if (res >= ctx->modulus) {
        res -= ctx->modulus;
    }
    return res;
}

static inline uint64_t ttak_monty_to(const ttak_monty64_t *ctx, uint64_t x) {
    return ttak_monty_reduce(ctx, (__uint128_t)x * ctx->r2);
}

static inline uint64_t ttak_monty_from(const ttak_monty64_t *ctx, uint64_t x) {
    return ttak_monty_reduce(ctx, (__uint128_t)x);
}

static inline uint64_t ttak_monty_mul(const ttak_monty64_t *ctx,
                                      uint64_t a,
                                      uint64_t b) {
    return ttak_monty_reduce(ctx, (__uint128_t)a * (__uint128_t)b);
}

#endif /* _MSC_VER */

static uint64_t ttak_monty_pow(uint64_t base,
                               uint64_t exponent,
                               const ttak_monty64_t *ctx) {
    uint64_t result = ttak_monty_to(ctx, 1ULL);
    uint64_t x = ttak_monty_to(ctx, base % ctx->modulus);
    while (exponent > 0) {
        if (exponent & 1ULL) {
            result = ttak_monty_mul(ctx, result, x);
        }
        x = ttak_monty_mul(ctx, x, x);
        exponent >>= 1ULL;
    }
    return ttak_monty_from(ctx, result);
}

static bool ttak_miller_rabin_u64(uint64_t n) {
    if (n < 2) return false;
    for (size_t i = 0; i < sizeof(k_small_primes) / sizeof(k_small_primes[0]); ++i) {
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

    ttak_monty64_t monty;
    ttak_monty_init(&monty, n);
    static const uint64_t bases[] = {2ULL, 3ULL, 5ULL, 7ULL, 11ULL, 13ULL};
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); ++i) {
        uint64_t a = bases[i] % n;
        if (a == 0ULL) continue;
        uint64_t x = ttak_monty_pow(a, d, &monty);
        if (x == 1ULL || x == n - 1ULL) continue;
        bool witness = true;
        for (uint32_t r = 1; r < s; ++r) {
            x = ttak_mulmod_u64(x, x, n);
            if (x == n - 1ULL) {
                witness = false;
                break;
            }
        }
        if (witness) {
            return false;
        }
    }
    return true;
}

static uint64_t ttak_pollard_rho_brent(uint64_t n, ttak_factor_rng_t *rng) {
    if ((n & 1ULL) == 0ULL) return 2ULL;
    uint64_t c = ttak_rng_between(rng, 1ULL, n - 1ULL);
    uint64_t y = ttak_rng_between(rng, 1ULL, n - 1ULL);
    uint64_t m = 128ULL;
    uint64_t g = 1ULL;
    uint64_t r = 1ULL;
    uint64_t q = 1ULL;
    uint64_t ys = 0ULL;
    uint64_t x = 0ULL;

    while (g == 1ULL) {
        x = y;
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
                if (diff == 0ULL) continue;
                q = ttak_mulmod_u64(q, diff, n);
            }
            g = ttak_gcd_u64(q, n);
            k += limit;
        }
        r <<= 1ULL;
    }

    if (g == n) {
        do {
            ys = (ttak_mulmod_u64(ys, ys, n) + c) % n;
            uint64_t diff = ttak_abs_diff_u64(x, ys);
            g = ttak_gcd_u64(diff, n);
        } while (g == 1ULL);
    }

    return g;
}

static bool ttak_add_factor_slot(uint64_t prime,
                                 ttak_accel_factor_record_t *record) {
    size_t count = record->factor_count;
    for (size_t i = 0; i < count; ++i) {
        if (record->slots[i].prime == prime) {
            record->slots[i].exponent++;
            return true;
        }
        if (record->slots[i].prime > prime) {
            if (count >= TTAK_ACCEL_FACTOR_MAX) return false;
            memmove(&record->slots[i + 1],
                    &record->slots[i],
                    (count - i) * sizeof(ttak_accel_factor_slot_t));
            record->slots[i].prime = prime;
            record->slots[i].exponent = 1;
            record->factor_count++;
            return true;
        }
    }

    if (count >= TTAK_ACCEL_FACTOR_MAX) return false;
    record->slots[count].prime = prime;
    record->slots[count].exponent = 1;
    record->factor_count++;
    return true;
}

static bool ttak_factor_recursive(uint64_t n,
                                  ttak_factor_rng_t *rng,
                                  ttak_accel_factor_record_t *record) {
    if (n == 1ULL) return true;
    if (ttak_miller_rabin_u64(n)) {
        return ttak_add_factor_slot(n, record);
    }

    for (int attempt = 0; attempt < 32; ++attempt) {
        uint64_t factor = ttak_pollard_rho_brent(n, rng);
        if (factor > 1ULL && factor < n) {
            if (!ttak_factor_recursive(factor, rng, record)) return false;
            if (!ttak_factor_recursive(n / factor, rng, record)) return false;
            return true;
        }
    }

    return ttak_add_factor_slot(n, record);
}

static bool ttak_factor_number(uint64_t value,
                               ttak_factor_rng_t *rng,
                               ttak_accel_factor_record_t *record) {
    record->factor_count = 0;
    memset(record->slots, 0, sizeof(record->slots));
    if (value <= 1ULL) {
        return true;
    }

    uint64_t n = value;
    for (size_t i = 0; i < sizeof(k_small_primes) / sizeof(k_small_primes[0]); ++i) {
        uint64_t p = k_small_primes[i];
        if (p * p > n) break;
        while (n % p == 0ULL) {
            if (!ttak_add_factor_slot(p, record)) return false;
            n /= p;
        }
    }

    if (n == 1ULL) {
        return true;
    }

    return ttak_factor_recursive(n, rng, record);
}

static size_t ttak_accel_record_active_size(const ttak_accel_factor_record_t *record) {
    size_t used_slots = record->factor_count;
    if (used_slots > TTAK_ACCEL_FACTOR_MAX) used_slots = TTAK_ACCEL_FACTOR_MAX;
    return sizeof(record->value) + sizeof(record->factor_count) +
           sizeof(record->checksum) +
           used_slots * sizeof(record->slots[0]);
}

static void ttak_finalize_record(ttak_accel_factor_record_t *record,
                                 uint32_t checksum_seed,
                                 uint32_t ordinal) {
    record->reserved = 0;
    uint32_t seed = checksum_seed ^ (ordinal * 0x9E3779B1u);
    uint32_t hash = ttak_fnv1a32(&record->value,
                                 sizeof(record->value) + sizeof(record->factor_count) + sizeof(record->reserved),
                                 seed);
    size_t used_slots = record->factor_count;
    if (used_slots > TTAK_ACCEL_FACTOR_MAX) used_slots = TTAK_ACCEL_FACTOR_MAX;
    hash = ttak_fnv1a32(record->slots, used_slots * sizeof(record->slots[0]), hash);
    record->checksum = hash;
}

static ttak_result_t ttak_finalize_output(const ttak_accel_batch_item_t *item,
                                          uint32_t guard,
                                          size_t record_count,
                                          uint32_t checksum_seed) {
    size_t payload_offset = sizeof(ttak_accel_record_prefix_t);
    uint8_t *payload = item->output + payload_offset;
    uint32_t payload_checksum = checksum_seed;
    for (size_t i = 0; i < record_count; ++i) {
        uint8_t *record_bytes = payload + i * sizeof(ttak_accel_factor_record_t);
        const ttak_accel_factor_record_t *record = (const ttak_accel_factor_record_t *)record_bytes;
        payload_checksum = ttak_fnv1a32(record_bytes,
                                        ttak_accel_record_active_size(record),
                                        payload_checksum);
    }

    ttak_accel_record_prefix_t prefix = {
        .guard = guard,
        .record_count = (uint32_t)record_count,
        .payload_checksum = payload_checksum,
        .reserved = sizeof(ttak_accel_factor_record_t)
    };
    memcpy(item->output, &prefix, sizeof(prefix));
    for (size_t i = 0; i < record_count; ++i) {
        uint8_t *record_bytes = payload + i * sizeof(ttak_accel_factor_record_t);
        const ttak_accel_factor_record_t *record = (const ttak_accel_factor_record_t *)record_bytes;
        ttak_mask_payload(record_bytes, ttak_accel_record_active_size(record), guard);
    }
    if (item->checksum_out != NULL) {
        *(item->checksum_out) = payload_checksum;
    }
    return TTAK_RESULT_OK;
}

static ttak_result_t ttak_process_item(const ttak_accel_batch_item_t *item,
                                       const ttak_accel_config_t *config) {
    if (item->input == NULL || item->output == NULL) {
        return TTAK_RESULT_ERR_ARGUMENT;
    }
    if (item->input_len == 0 || (item->input_len % sizeof(uint64_t)) != 0) {
        return TTAK_RESULT_ERR_ARGUMENT;
    }

    size_t record_count = item->input_len / sizeof(uint64_t);
    if (record_count == 0 || record_count > UINT32_MAX) {
        return TTAK_RESULT_ERR_ARGUMENT;
    }

    if (record_count > (SIZE_MAX - sizeof(ttak_accel_record_prefix_t)) /
                           sizeof(ttak_accel_factor_record_t)) {
        return TTAK_RESULT_ERR_ARGUMENT;
    }

    size_t needed = sizeof(ttak_accel_record_prefix_t) +
                    record_count * sizeof(ttak_accel_factor_record_t);
    if (item->output_len < needed) {
        return TTAK_RESULT_ERR_ARGUMENT;
    }

    uint32_t guard = ttak_guard_word(config, item);
    uint32_t checksum_seed = ttak_checksum_seed(item);
    uint8_t *payload = item->output + sizeof(ttak_accel_record_prefix_t);

    ttak_factor_rng_t rng = {
        .state = ((uint64_t)guard << 32) ^ (uint64_t)record_count ^ (uint64_t)(uintptr_t)item->input
    };

    for (size_t idx = 0; idx < record_count; ++idx) {
        uint64_t value = 0;
        memcpy(&value, item->input + idx * sizeof(uint64_t), sizeof(uint64_t));

        ttak_accel_factor_record_t record;
        memset(&record, 0, sizeof(record));
        record.value = value;
        if (!ttak_factor_number(value, &rng, &record)) {
            return TTAK_RESULT_ERR_EXECUTION;
        }
        ttak_finalize_record(&record, checksum_seed, (uint32_t)idx);
        memcpy(payload + idx * sizeof(ttak_accel_factor_record_t),
               &record,
               sizeof(ttak_accel_factor_record_t));
    }

    return ttak_finalize_output(item, guard, record_count, checksum_seed);
}

ttak_result_t ttak_accel_run_cpu(
    const ttak_accel_batch_item_t *items,
    size_t item_count,
    const ttak_accel_config_t *config) {
    if (items == NULL || config == NULL) {
        return TTAK_RESULT_ERR_ARGUMENT;
    }

    atomic_size_t cursor = ATOMIC_VAR_INIT(0);
    size_t tile = config->max_tiles;
    if (tile == 0 || tile > item_count) {
        tile = item_count;
    }

    while (true) {
        size_t start = atomic_fetch_add_explicit(&cursor, tile, memory_order_relaxed);
        if (start >= item_count) {
            break;
        }
        size_t end = start + tile;
        if (end > item_count) end = item_count;
        for (size_t idx = start; idx < end; ++idx) {
            ttak_result_t status = ttak_process_item(&items[idx], config);
            if (status != TTAK_RESULT_OK) {
                return status;
            }
        }
    }

    return TTAK_RESULT_OK;
}
