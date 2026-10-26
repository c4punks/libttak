#include <ttak/math/sum_divisors.h>
#include <ttak/math/factor.h>
#include <ttak/mem/mem.h>
#include <ttak/types/fixed.h>

#if TTAK_NO_NATIVE_TLS
#define SUMDIV_THREAD_LOCAL
#else
#define SUMDIV_THREAD_LOCAL _Thread_local
#endif

#define SUMDIV_SAFE_WINDOW        32U
#define SUMDIV_FACTOR_COOLDOWN    8U

typedef struct {
    uint32_t safe_mode_budget;
    uint32_t factor_cooldown;
    size_t   factor_backoff_bitlen;
    bool     needs_fresh_target;
} sumdiv_policy_state_t;

static SUMDIV_THREAD_LOCAL ttak_sumdiv_big_error_t g_sumdiv_big_last_error = TTAK_SUMDIV_BIG_ERROR_NONE;
static sumdiv_policy_state_t g_sumdiv_policy = {0, 0, 0, false};
static ttak_logger_t *g_sumdiv_logger = NULL;
static ttak_sumdiv_limits_t g_sumdiv_limits = { .max_input_bits = 0 };

static bool sumdiv_policy_force_safe_mode(void) {
    return g_sumdiv_policy.safe_mode_budget > 0;
}

static void sumdiv_policy_note_success(void) {
    if (g_sumdiv_policy.safe_mode_budget > 0) g_sumdiv_policy.safe_mode_budget--;
    if (g_sumdiv_policy.factor_cooldown > 0) g_sumdiv_policy.factor_cooldown--;
    if (g_sumdiv_policy.safe_mode_budget == 0) g_sumdiv_policy.needs_fresh_target = false;
}

static void sumdiv_policy_log_stage(const char *stage, ttak_sumdiv_big_error_t err,
                                    size_t bitlen, bool safe_mode, uint32_t gauge) {
    if (!stage || !g_sumdiv_logger) return;
    const char *err_name = ttak_sum_proper_divisors_big_error_name(err);
    ttak_logger_log(g_sumdiv_logger, TTAK_LOG_INFO,
                    "[SUMDIV][AUTO] stage=%s err=%s bits=%zu safe=%u gauge=%u",
                    stage,
                    err_name ? err_name : "unknown",
                    bitlen,
                    safe_mode ? 1U : 0U,
                    gauge);
}

static void sumdiv_policy_reset_target(ttak_bigint_t *target, uint64_t now) {
    if (!target || !g_sumdiv_policy.needs_fresh_target) return;
    ttak_bigint_free(target, now);
    ttak_bigint_init(target, now);
    g_sumdiv_policy.needs_fresh_target = false;
    sumdiv_policy_log_stage("target-reset", g_sumdiv_big_last_error, 0, false, 0);
}

static bool sumdiv_policy_handle_failure(ttak_sumdiv_big_error_t err, size_t bitlen, bool already_safe) {
    switch (err) {
        case TTAK_SUMDIV_BIG_ERROR_SET_VALUE:
            g_sumdiv_policy.needs_fresh_target = true;
            /* fallthrough */
        case TTAK_SUMDIV_BIG_ERROR_EXPORT:
        case TTAK_SUMDIV_BIG_ERROR_ARITHMETIC:
            if (!already_safe) {
                g_sumdiv_policy.safe_mode_budget = SUMDIV_SAFE_WINDOW;
                sumdiv_policy_log_stage("safe-entry", err, bitlen, false, g_sumdiv_policy.safe_mode_budget);
                return true;
            }
            break;
        case TTAK_SUMDIV_BIG_ERROR_GENERIC:
            if (!already_safe) {
                uint32_t window = SUMDIV_SAFE_WINDOW / 2U;
                if (window == 0) window = 1;
                g_sumdiv_policy.safe_mode_budget = window;
                g_sumdiv_policy.needs_fresh_target = true;
                sumdiv_policy_log_stage("safe-generic", err, bitlen, false, g_sumdiv_policy.safe_mode_budget);
                return true;
            }
            break;
        case TTAK_SUMDIV_BIG_ERROR_FACTOR:
            g_sumdiv_policy.factor_cooldown = SUMDIV_FACTOR_COOLDOWN;
            if (bitlen > g_sumdiv_policy.factor_backoff_bitlen) {
                g_sumdiv_policy.factor_backoff_bitlen = bitlen;
            }
            sumdiv_policy_log_stage("factor-backoff", err, bitlen, already_safe, g_sumdiv_policy.factor_cooldown);
            break;
        case TTAK_SUMDIV_BIG_ERROR_INPUT_TOO_LARGE:
            /* deterministic fast-fail path; nothing to auto-heal */
            break;
        /* fall through */
        case TTAK_SUMDIV_BIG_ERROR_NONE:
        default:
            break;
    }
    return false;
}

static bool sum_proper_divisors_big_impl(const ttak_bigint_t *n, ttak_bigint_t *result_out,
                                         uint64_t now, bool safe_mode, size_t bitlen);

static bool u128_mul_u64_checked(ttak_u128_t value, uint64_t factor, ttak_u128_t *out) {
    bool overflow = false;
    ttak_u128_t tmp = ttak_u128_mul_u64_wide(value, factor, &overflow);
    if (overflow) return false;
    if (out) *out = tmp;
    return true;
}

static bool u256_add_checked(ttak_u256_t a, ttak_u256_t b, ttak_u256_t *out) {
    ttak_u256_t res = ttak_u256_zero();
    uint64_t carry = 0;
    for (int i = 0; i < 4; ++i) {
        uint64_t sum = a.limb[i] + b.limb[i];
        uint64_t carry_from_pair = (sum < a.limb[i]) ? 1 : 0;
        sum += carry;
        uint64_t carry_from_carry = (sum < carry) ? 1 : 0;
        res.limb[i] = sum;
        carry = (carry_from_pair | carry_from_carry);
    }
    if (carry) return false;
    if (out) *out = res;
    return true;
}

static bool u256_sub_checked(ttak_u256_t a, ttak_u256_t b, ttak_u256_t *out) {
    ttak_u256_t res = ttak_u256_zero();
    uint64_t borrow = 0;
    for (int i = 0; i < 4; ++i) {
        uint64_t temp = a.limb[i] - b.limb[i];
        uint64_t borrow_from_pair = (a.limb[i] < b.limb[i]) ? 1 : 0;
        if (borrow) {
            uint64_t new_temp = temp - 1;
            uint64_t borrow_from_borrow = (temp == 0) ? 1 : 0;
            temp = new_temp;
            borrow = (borrow_from_pair | borrow_from_borrow);
        } else {
            borrow = borrow_from_pair;
        }
        res.limb[i] = temp;
    }
    if (borrow) return false;
    if (out) *out = res;
    return true;
}

static bool u256_mul_checked(ttak_u256_t a, ttak_u256_t b, ttak_u256_t *out) {
    uint64_t accum[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            uint64_t hi, lo;
            ttak_mul_64(a.limb[i], b.limb[j], &hi, &lo);

            size_t idx = (size_t)i + (size_t)j;
            uint64_t sum = accum[idx] + lo;
            uint64_t carry = (sum < accum[idx]) ? 1 : 0;
            accum[idx] = sum;
            hi += carry;

            size_t k = idx + 1;
            while (hi && k < 8) {
                uint64_t next = accum[k] + hi;
                uint64_t next_carry = (next < accum[k]) ? 1 : 0;
                accum[k] = next;
                hi = next_carry;
                ++k;
            }
        }
    }

    if (accum[4] || accum[5] || accum[6] || accum[7]) {
        return false;
    }

    if (out) {
        for (int i = 0; i < 4; ++i) {
            out->limb[i] = accum[i];
        }
    }
    return true;
}

static ttak_u256_t u256_from_u64(uint64_t value) {
    return ttak_u256_from_u128(ttak_u128_from_u64(value));
}

static bool compute_sigma_u64_lane(const ttak_prime_factor_t *factors, size_t count, ttak_u128_t *sigma_out) {
    ttak_u128_t sigma = ttak_u128_from_u64(1);
    for (size_t i = 0; i < count; ++i) {
        const uint64_t prime = factors[i].p;
        const uint32_t exponent = factors[i].a;
        ttak_u128_t term = ttak_u128_from_u64(1);
        ttak_u128_t power = ttak_u128_from_u64(1);

        for (uint32_t e = 0; e < exponent; ++e) {
            if (!u128_mul_u64_checked(power, prime, &power)) {
                return false;
            }
            if (ttak_u128_add_overflow(term, power, &term)) {
                return false;
            }
        }

        ttak_u256_t wide = ttak_u128_mul_u128(sigma, term);
        if (wide.limb[2] || wide.limb[3]) {
            return false;
        }
        sigma = ttak_u256_extract_low(wide);
    }
    if (sigma_out) *sigma_out = sigma;
    return true;
}

static bool compute_sigma_u128_lane(const ttak_prime_factor_big_t *factors, size_t count, ttak_u256_t *sigma_out) {
    ttak_u256_t sigma = u256_from_u64(1);
    for (size_t i = 0; i < count; ++i) {
        ttak_u128_t prime128;
        if (!ttak_bigint_export_u128(&factors[i].p, &prime128)) {
            return false;
        }
        ttak_u256_t prime256 = ttak_u256_from_u128(prime128);
        ttak_u256_t term = u256_from_u64(1);
        ttak_u256_t power = u256_from_u64(1);

        for (uint32_t e = 0; e < factors[i].a; ++e) {
            if (!u256_mul_checked(power, prime256, &power)) {
                return false;
            }
            if (!u256_add_checked(term, power, &term)) {
                return false;
            }
        }

        if (!u256_mul_checked(sigma, term, &sigma)) {
            return false;
        }
    }
    if (sigma_out) *sigma_out = sigma;
    return true;
}

static void free_big_factors(ttak_prime_factor_big_t *factors, size_t count, uint64_t now) {
    if (!factors) return;
    for (size_t i = 0; i < count; ++i) {
        ttak_bigint_free(&factors[i].p, now);
    }
    ttak_mem_free(factors);
}

static bool sum_proper_divisors_big_generic(const ttak_bigint_t *n,
                                            ttak_prime_factor_big_t *factors,
                                            size_t count,
                                            ttak_bigint_t *result_out,
                                            uint64_t now) {
    ttak_bigint_t sum_divs, term_num, term_den, temp;
    ttak_bigint_init_u64(&sum_divs, 1, now);
    ttak_bigint_init(&term_num, now);
    ttak_bigint_init(&term_den, now);
    ttak_bigint_init(&temp, now);

    bool ok = true;

    for (size_t i = 0; i < count; ++i) {
        ttak_bigint_t *p = &factors[i].p;
        uint32_t a = factors[i].a;

        ttak_bigint_set_u64(&term_num, 1, now);
        for (uint32_t j = 0; j < a + 1; ++j) {
            if (!ttak_bigint_mul(&term_num, &term_num, p, now)) { ok = false; break; }
        }
        if (!ok) break;

        ttak_bigint_t one;
        ttak_bigint_init_u64(&one, 1, now);
        if (!ttak_bigint_sub(&term_num, &term_num, &one, now)) { ok = false; ttak_bigint_free(&one, now); break; }
        if (!ttak_bigint_sub(&term_den, p, &one, now)) { ok = false; ttak_bigint_free(&one, now); break; }
        ttak_bigint_free(&one, now);

        if (!ttak_bigint_div(&temp, NULL, &term_num, &term_den, now)) { ok = false; break; }
        if (!ttak_bigint_mul(&sum_divs, &sum_divs, &temp, now)) { ok = false; break; }
    }

    ttak_bigint_free(&term_num, now);
    ttak_bigint_free(&term_den, now);
    ttak_bigint_free(&temp, now);

    if (!ok) {
        ttak_bigint_free(&sum_divs, now);
        return false;
    }

    if (!ttak_bigint_sub(result_out, &sum_divs, n, now)) {
        ttak_bigint_free(&sum_divs, now);
        return false;
    }

    ttak_bigint_free(&sum_divs, now);
    return true;
}

/**
 * @brief Compute the sum of proper divisors for a 64-bit integer.
 *
 * Implements σ(n) - n where σ uses the prime factorization formula.
 *
 * @param n          Input value.
 * @param result_out Output pointer for the sum.
 * @return true on success, false if factorization fails or overflow occurs.
 */
bool ttak_sum_proper_divisors_u64(uint64_t n, uint64_t *result_out) {
    if (!result_out) return false;
    if (n <= 1) {
        *result_out = 0;
        return true;
    }

    uint64_t now = 0;
    ttak_prime_factor_t *factors = NULL;
    size_t count = 0;

    if (ttak_factor_u64(n, &factors, &count, now) != 0) {
        return false;
    }

    ttak_u128_t sigma128;
    bool ok = compute_sigma_u64_lane(factors, count, &sigma128);
    ttak_mem_free(factors);
    if (!ok) return false;

    ttak_u128_t n128 = ttak_u128_from_u64(n);
    if (ttak_u128_cmp(sigma128, n128) < 0) return false;
    ttak_u128_t proper = ttak_u128_sub(sigma128, n128);
    if (proper.hi != 0) return false;
    *result_out = proper.lo;
    return true;
}

static bool sum_proper_divisors_big_impl(const ttak_bigint_t *n, ttak_bigint_t *result_out,
                                         uint64_t now, bool safe_mode, size_t bitlen) {
    if (!safe_mode &&
        g_sumdiv_policy.factor_cooldown > 0 &&
        g_sumdiv_policy.factor_backoff_bitlen > 0 &&
        bitlen >= g_sumdiv_policy.factor_backoff_bitlen) {
        g_sumdiv_big_last_error = TTAK_SUMDIV_BIG_ERROR_FACTOR;
        sumdiv_policy_log_stage("factor-cooldown", g_sumdiv_big_last_error, bitlen, false, g_sumdiv_policy.factor_cooldown);
        return false;
    }

    ttak_prime_factor_big_t *factors = NULL;
    size_t count = 0;

    if (ttak_factor_big(n, &factors, &count, now) != 0) {
        g_sumdiv_big_last_error = TTAK_SUMDIV_BIG_ERROR_FACTOR;
        return false; // Factorization failed
    }

    bool ok = false;

    if (!safe_mode && bitlen <= 64) {
        uint64_t n64 = 0;
        uint64_t proper = 0;
        if (ttak_bigint_export_u64(n, &n64)) {
            if (ttak_sum_proper_divisors_u64(n64, &proper)) {
                ok = ttak_bigint_set_u64(result_out, proper, now);
                if (!ok) g_sumdiv_big_last_error = TTAK_SUMDIV_BIG_ERROR_SET_VALUE;
            } else {
                g_sumdiv_big_last_error = TTAK_SUMDIV_BIG_ERROR_ARITHMETIC;
            }
        } else {
            g_sumdiv_big_last_error = TTAK_SUMDIV_BIG_ERROR_EXPORT;
        }
    } else if (!safe_mode && bitlen <= 128) {
        ttak_u256_t sigma256;
        ttak_u256_t n256;
        if (ttak_bigint_export_u256(n, &n256)) {
            if (compute_sigma_u128_lane(factors, count, &sigma256)) {
                ttak_u256_t proper256;
                if (u256_sub_checked(sigma256, n256, &proper256)) {
                    ok = ttak_bigint_set_u256(result_out, proper256, now);
                    if (!ok) g_sumdiv_big_last_error = TTAK_SUMDIV_BIG_ERROR_SET_VALUE;
                } else {
                    g_sumdiv_big_last_error = TTAK_SUMDIV_BIG_ERROR_ARITHMETIC;
                }
            } else {
                g_sumdiv_big_last_error = TTAK_SUMDIV_BIG_ERROR_ARITHMETIC;
            }
        } else {
            g_sumdiv_big_last_error = TTAK_SUMDIV_BIG_ERROR_EXPORT;
        }
    } else {
        ok = sum_proper_divisors_big_generic(n, factors, count, result_out, now);
        if (!ok) g_sumdiv_big_last_error = TTAK_SUMDIV_BIG_ERROR_GENERIC;
    }

    free_big_factors(factors, count, now);
    return ok;
}

/**
 * @brief Compute the sum of proper divisors for a big integer.
 *
 * @param n          Input value.
 * @param result_out Destination for σ(n) - n.
 * @param now        Timestamp for allocations.
 * @return true on success, false on factorization or arithmetic failure.
 */
bool ttak_sum_proper_divisors_big(const ttak_bigint_t *n, ttak_bigint_t *result_out, uint64_t now) {
    if (ttak_bigint_is_zero(n) || ttak_bigint_cmp_u64(n, 1) <= 0) {
        ttak_bigint_set_u64(result_out, 0, now);
        return true;
    }

    size_t bitlen = ttak_bigint_get_bit_length(n);
    size_t max_bits = g_sumdiv_limits.max_input_bits;
    if (max_bits > 0 && bitlen > max_bits) {
        g_sumdiv_big_last_error = TTAK_SUMDIV_BIG_ERROR_INPUT_TOO_LARGE;
        return false;
    }
    bool safe_mode = sumdiv_policy_force_safe_mode();
    g_sumdiv_big_last_error = TTAK_SUMDIV_BIG_ERROR_NONE;

    bool ok = sum_proper_divisors_big_impl(n, result_out, now, safe_mode, bitlen);
    if (!ok) {
        ttak_sumdiv_big_error_t err = g_sumdiv_big_last_error;
        if (sumdiv_policy_handle_failure(err, bitlen, safe_mode)) {
            sumdiv_policy_reset_target(result_out, now);
            g_sumdiv_big_last_error = TTAK_SUMDIV_BIG_ERROR_NONE;
            sumdiv_policy_log_stage("safe-retry", err, bitlen, true, g_sumdiv_policy.safe_mode_budget);
            ok = sum_proper_divisors_big_impl(n, result_out, now, true, bitlen);
            if (!ok) {
                return false;
            }
        } else {
            return false;
        }
    }

    sumdiv_policy_note_success();
    return true;
}

ttak_sumdiv_big_error_t ttak_sum_proper_divisors_big_last_error(void) {
    return g_sumdiv_big_last_error;
}

const char *ttak_sum_proper_divisors_big_error_name(ttak_sumdiv_big_error_t err) {
    switch (err) {
        case TTAK_SUMDIV_BIG_ERROR_NONE: return "none";
        case TTAK_SUMDIV_BIG_ERROR_FACTOR: return "factor";
        case TTAK_SUMDIV_BIG_ERROR_EXPORT: return "export";
        case TTAK_SUMDIV_BIG_ERROR_SET_VALUE: return "assign";
        case TTAK_SUMDIV_BIG_ERROR_ARITHMETIC: return "arith";
        case TTAK_SUMDIV_BIG_ERROR_GENERIC: return "generic";
        case TTAK_SUMDIV_BIG_ERROR_INPUT_TOO_LARGE: return "input-too-large";
        default: return "unknown";
    }
}

void ttak_sum_divisors_attach_logger(ttak_logger_t *logger) {
    g_sumdiv_logger = logger;
}

void ttak_sum_divisors_set_limits(const ttak_sumdiv_limits_t *limits) {
    if (!limits) {
        g_sumdiv_limits.max_input_bits = 0;
        return;
    }
    g_sumdiv_limits.max_input_bits = limits->max_input_bits;
}

void ttak_sum_divisors_get_limits(ttak_sumdiv_limits_t *limits_out) {
    if (!limits_out) return;
    *limits_out = g_sumdiv_limits;
}
