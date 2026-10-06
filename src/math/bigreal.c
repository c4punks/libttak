#include <ttak/math/bigreal.h>
#include <ttak/mem/mem.h>
#include <string.h>
#include <stdalign.h>

/**
 * Aligned-limb add/sub helper for bigreal operations.
 * Operates on mantissas after exponent alignment.
 */
static _Bool ttak_bigreal_op_aligned_addsub(ttak_bigreal_t *dst, const ttak_bigreal_t *lhs, const ttak_bigreal_t *rhs, _Bool is_sub, uint64_t now) {
    if (is_sub) {
        return ttak_bigint_sub(&dst->mantissa, &lhs->mantissa, &rhs->mantissa, now);
    } else {
        return ttak_bigint_add(&dst->mantissa, &lhs->mantissa, &rhs->mantissa, now);
    }
}

void ttak_bigreal_init(ttak_bigreal_t *br, uint64_t now) {
    ttak_bigint_init(&br->mantissa, now);
    br->exponent = 0;
}

void ttak_bigreal_init_u64(ttak_bigreal_t *br, uint64_t value, uint64_t now) {
    ttak_bigint_init_u64(&br->mantissa, value, now);
    br->exponent = 0;
}

/**
 * @brief Release the mantissa storage inside the big real.
 *
 * @param br  Big real to destroy.
 * @param now Timestamp forwarded to bigint free.
 */
void ttak_bigreal_free(ttak_bigreal_t *br, uint64_t now) {
    ttak_bigint_free(&br->mantissa, now);
}

void ttak_bigreal_normalize(ttak_bigreal_t *br, uint64_t now) {
    if (!br || ttak_bigint_is_zero(&br->mantissa)) {
        if (br) br->exponent = 0;
        return;
    }
    ttak_bigint_t q, r;
    ttak_bigint_init(&q, now);
    ttak_bigint_init(&r, now);
    while (true) {
        if (!ttak_bigint_div_u64(&q, &r, &br->mantissa, 10, now)) break;
        if (!ttak_bigint_is_zero(&r)) break; // Not divisible by 10
        ttak_bigint_copy(&br->mantissa, &q, now);
        br->exponent++;
    }
    ttak_bigint_free(&q, now);
    ttak_bigint_free(&r, now);
}

_Bool ttak_bigreal_copy(ttak_bigreal_t *dst, const ttak_bigreal_t *src, uint64_t now) {
    if (dst == src) return true;
    dst->exponent = src->exponent;
    return ttak_bigint_copy(&dst->mantissa, &src->mantissa, now);
}

_Bool ttak_bigreal_align(ttak_bigreal_t *a, ttak_bigreal_t *b, uint64_t now) {
    if (a->exponent == b->exponent) return true;
    
    ttak_bigint_t tmp;
    ttak_bigint_init(&tmp, now);

    if (a->exponent > b->exponent) {
        // Shift a left to match smaller exponent b
        uint64_t diff = a->exponent - b->exponent;
        if (diff > 60) {
            ttak_bigint_free(&tmp, now);
            return false;
        }
        
        // Correct base-10 alignment: multiply by 10^diff
        ttak_bigint_copy(&tmp, &a->mantissa, now);
        for (uint64_t i = 0; i < diff; i++) {
            ttak_bigint_mul_u64(&tmp, &tmp, 10, now);
        }
        ttak_bigint_copy(&a->mantissa, &tmp, now);
        a->exponent = b->exponent;
    } else {
        // Shift b left to match smaller exponent a
        uint64_t diff = b->exponent - a->exponent;
        if (diff > 60) {
            ttak_bigint_free(&tmp, now);
            return false;
        }
        
        // Correct base-10 alignment: multiply by 10^diff
        ttak_bigint_copy(&tmp, &b->mantissa, now);
        for (uint64_t i = 0; i < diff; i++) {
            ttak_bigint_mul_u64(&tmp, &tmp, 10, now);
        }
        ttak_bigint_copy(&b->mantissa, &tmp, now);
        b->exponent = a->exponent;
    }
    ttak_bigint_free(&tmp, now);
    return true;
}

/**
 * @brief Add two big reals with matching exponents.
 *
 * @param dst Destination big real.
 * @param lhs Left operand.
 * @param rhs Right operand.
 * @param now Timestamp for bigint arithmetic.
 * @return true on success, false if exponents differ.
 */
_Bool ttak_bigreal_add(ttak_bigreal_t *dst, const ttak_bigreal_t *lhs, const ttak_bigreal_t *rhs, uint64_t now) {
    ttak_bigreal_t l, r;
    ttak_bigreal_init(&l, now);
    ttak_bigreal_init(&r, now);
    ttak_bigreal_copy(&l, lhs, now);
    ttak_bigreal_copy(&r, rhs, now);
    
    if (!ttak_bigreal_align(&l, &r, now)) {
        ttak_bigreal_free(&l, now);
        ttak_bigreal_free(&r, now);
        return false;
    }
    
    dst->exponent = l.exponent;
    /* Aligned internal limb processing */
    _Bool ok = ttak_bigreal_op_aligned_addsub(dst, &l, &r, false, now);
    
    ttak_bigreal_free(&l, now);
    ttak_bigreal_free(&r, now);
    return ok;
}

_Bool ttak_bigreal_sub(ttak_bigreal_t *dst, const ttak_bigreal_t *lhs, const ttak_bigreal_t *rhs, uint64_t now) {
    ttak_bigreal_t l, r;
    ttak_bigreal_init(&l, now);
    ttak_bigreal_init(&r, now);
    ttak_bigreal_copy(&l, lhs, now);
    ttak_bigreal_copy(&r, rhs, now);
    
    if (!ttak_bigreal_align(&l, &r, now)) {
        ttak_bigreal_free(&l, now);
        ttak_bigreal_free(&r, now);
        return false;
    }
    
    dst->exponent = l.exponent;
    /* Aligned internal limb processing */
    _Bool ok = ttak_bigreal_op_aligned_addsub(dst, &l, &r, true, now);
    
    ttak_bigreal_free(&l, now);
    ttak_bigreal_free(&r, now);
    return ok;
}

_Bool ttak_bigreal_mul(ttak_bigreal_t *dst, const ttak_bigreal_t *lhs, const ttak_bigreal_t *rhs, uint64_t now) {
    dst->exponent = lhs->exponent + rhs->exponent;
    return ttak_bigint_mul(&dst->mantissa, &lhs->mantissa, &rhs->mantissa, now);
}

_Bool ttak_bigreal_div(ttak_bigreal_t *dst, const ttak_bigreal_t *lhs, const ttak_bigreal_t *rhs, uint64_t now) {
    // Improved division: shift left by 10^6 for 6 decimal places of precision
    ttak_bigint_t r_rem, lhs_shifted;
    ttak_bigint_init(&r_rem, now);
    ttak_bigint_init(&lhs_shifted, now);
    
    // Multiply lhs by 1,000,000
    ttak_bigint_mul_u64(&lhs_shifted, &lhs->mantissa, 1000000, now);
    
    dst->exponent = lhs->exponent - rhs->exponent - 6;
    _Bool ok = ttak_bigint_div(&dst->mantissa, &r_rem, &lhs_shifted, &rhs->mantissa, now);
    if (ok) {
        ttak_bigreal_normalize(dst, now);
    }
    
    ttak_bigint_free(&r_rem, now);
    ttak_bigint_free(&lhs_shifted, now);
    return ok;
}

int ttak_bigreal_cmp(const ttak_bigreal_t *lhs, const ttak_bigreal_t *rhs, uint64_t now) {
    ttak_bigreal_t l, r;
    ttak_bigreal_init(&l, now);
    ttak_bigreal_init(&r, now);
    ttak_bigreal_copy(&l, lhs, now);
    ttak_bigreal_copy(&r, rhs, now);
    
    if (!ttak_bigreal_align(&l, &r, now)) {
        // Fallback or error?
        ttak_bigreal_free(&l, now);
        ttak_bigreal_free(&r, now);
        return 0; 
    }
    
    int res = ttak_bigint_cmp(&l.mantissa, &r.mantissa);
    ttak_bigreal_free(&l, now);
    ttak_bigreal_free(&r, now);
    return res;
}
