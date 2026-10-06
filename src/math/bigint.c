#include <ttak/math/bigint.h>
#include <ttak/math/bigint_accel.h>
#include <ttak/mem/mem.h>
#include "../../internal/app_types.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/**
 * @brief Retrieve a mutable pointer to the limb storage.
 *
 * Automatically selects between the SSO buffer and the heap buffer.
 *
 * @param bi Big integer container.
 * @return Pointer to the writable limb array.
 */
static limb_t *get_limbs(ttak_bigint_t *bi) {
    return bi->is_dynamic ? bi->data.dyn_ptr : bi->data.sso_buf;
}

/**
 * @brief Retrieve a read-only pointer to the limb storage.
 *
 * @param bi Big integer container.
 * @return Pointer to the immutable limb array.
 */
static const limb_t *get_const_limbs(const ttak_bigint_t *bi) {
    return bi->is_dynamic ? bi->data.dyn_ptr : bi->data.sso_buf;
}

/**
 * @brief Grow the limb buffer so it can hold the requested number of limbs.
 *
 * @param bi        Big integer to reallocate.
 * @param required  Minimum limb capacity.
 * @param now       Timestamp for allocator bookkeeping.
 * @return true if capacity is guaranteed, false on allocation failure.
 */
static _Bool ensure_capacity(ttak_bigint_t *bi, size_t required, uint64_t now) {
    if (required <= bi->capacity) return true;
    if (required > TTAK_MAX_LIMB_LIMIT) return false;

    size_t old_capacity = bi->capacity;
    size_t new_capacity = old_capacity ? old_capacity : TTAK_BIGINT_SSO_LIMIT;
    while (new_capacity < required && new_capacity < TTAK_MAX_LIMB_LIMIT) {
        size_t next = new_capacity * 2;
        if (next <= new_capacity) break;
        new_capacity = next;
    }
    if (new_capacity < required) new_capacity = required;
    if (new_capacity > TTAK_MAX_LIMB_LIMIT) return false;

    size_t new_size = new_capacity * sizeof(limb_t);
    limb_t *new_buf = NULL;

    if (bi->is_dynamic) {
        new_buf = ttak_mem_realloc_raw(bi->data.dyn_ptr, new_size, __TTAK_UNSAFE_MEM_FOREVER__, now);
    } else {
        new_buf = ttak_mem_dup_raw(bi->data.sso_buf, bi->used * sizeof(limb_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
        if (new_buf) {
            /* If we duped but requested more than was in SSO, zero the rest */
            if (new_capacity > bi->used) {
                 memset(new_buf + bi->used, 0, (new_capacity - bi->used) * sizeof(limb_t));
            }
        }
    }

    if (!new_buf) return false;
    if (new_capacity > old_capacity) {
        memset(new_buf + old_capacity, 0, (new_capacity - old_capacity) * sizeof(limb_t));
    }

    bi->data.dyn_ptr = new_buf;
    bi->is_dynamic = true;
    bi->capacity = new_capacity;
    return true;
}

/**
 * @brief Remove leading zero limbs and normalize the sign flag.
 *
 * @param bi Big integer to normalize.
 */
static void trim_unused(ttak_bigint_t *bi) {
    limb_t *limbs = get_limbs(bi);
    while (bi->used > 0 && limbs[bi->used - 1] == 0) {
        bi->used--;
    }
    if (bi->used == 0) {
        bi->is_negative = false;
    }
}

/**
 * @brief Initialize a big integer with zero value using SSO storage.
 *
 * @param bi  Big integer to initialize.
 * @param now Unused timestamp parameter (kept for API parity).
 */
void ttak_bigint_init(ttak_bigint_t *bi, uint64_t now) {
    (void)now;
    bi->capacity = TTAK_BIGINT_SSO_LIMIT;
    bi->used = 0;
    bi->is_negative = false;
    bi->is_dynamic = false;
    memset(bi->data.sso_buf, 0, sizeof(bi->data.sso_buf));
}

/**
 * @brief Initialize a big integer from an unsigned 64-bit value.
 *
 * @param bi    Destination big integer.
 * @param value Immediate value to store.
 * @param now   Current timestamp for downstream allocations.
 */
void ttak_bigint_init_u64(ttak_bigint_t *bi, uint64_t value, uint64_t now) {
    ttak_bigint_init(bi, now);
    ttak_bigint_set_u64(bi, value, now);
}

/**
 * @brief Initialize a big integer by copying another instance.
 *
 * @param dst Destination instance to initialize.
 * @param src Source big integer to duplicate.
 * @param now Current timestamp for allocations.
 */
void ttak_bigint_init_copy(ttak_bigint_t *dst, const ttak_bigint_t *src, uint64_t now) {
    ttak_bigint_init(dst, now);
    ttak_bigint_copy(dst, src, now);
}

/**
 * @brief Release any dynamic storage associated with the big integer.
 *
 * @param bi  Instance to destroy.
 * @param now Timestamp (unused, for API symmetry).
 */
void ttak_bigint_free(ttak_bigint_t *bi, uint64_t now) {
    (void)now;
    if (bi && bi->is_dynamic && bi->data.dyn_ptr) {
        ttak_mem_free(bi->data.dyn_ptr);
    }
    if (bi) {
      memset(bi, 0, sizeof(*bi));
    }
}

/**
 * @brief Assign an unsigned 64-bit value to the big integer.
 *
 * @param bi    Destination integer.
 * @param value Value to copy.
 * @param now   Timestamp for potential reallocations.
 * @return true on success, false on allocation failure.
 */
_Bool ttak_bigint_set_u64(ttak_bigint_t *bi, uint64_t value, uint64_t now) {
    bi->is_negative = false;
    if (value == 0) {
        bi->used = 0;
        return true;
    }

    size_t needed_limbs = (value > 0xFFFFFFFFu) ? 2 : 1;
    if (!ensure_capacity(bi, needed_limbs, now)) {
        return false;
    }

    limb_t *limbs = get_limbs(bi);
    if (needed_limbs == 1) {
        limbs[0] = (limb_t)value;
        bi->used = 1;
    } else {
        limbs[0] = (limb_t)(value & 0xFFFFFFFFu);
        limbs[1] = (limb_t)(value >> 32);
        bi->used = 2;
    }
    return true;
}

_Bool ttak_bigint_set_u128(ttak_bigint_t *bi, ttak_u128_t value, uint64_t now) {
    bi->is_negative = false;
    if (ttak_u128_is_zero(value)) {
        bi->used = 0;
        return true;
    }

    size_t needed_limbs = 0;
    if (value.hi != 0) {
        needed_limbs = 4;
    } else if (value.lo > 0xFFFFFFFFu) {
        needed_limbs = 2;
    } else {
        needed_limbs = 1;
    }

    if (!ensure_capacity(bi, needed_limbs, now)) {
        return false;
    }

    limb_t *limbs = get_limbs(bi);
    limbs[0] = (limb_t)(value.lo & 0xFFFFFFFFu);
    if (needed_limbs > 1) limbs[1] = (limb_t)(value.lo >> 32);
    if (needed_limbs > 2) limbs[2] = (limb_t)(value.hi & 0xFFFFFFFFu);
    if (needed_limbs > 3) limbs[3] = (limb_t)(value.hi >> 32);
    bi->used = needed_limbs;
    return true;
}

_Bool ttak_bigint_set_u256(ttak_bigint_t *bi, ttak_u256_t value, uint64_t now) {
    bi->is_negative = false;
    limb_t words[8];
    for (int i = 0; i < 4; ++i) {
        uint64_t chunk = value.limb[i];
        words[i * 2] = (limb_t)(chunk & 0xFFFFFFFFu);
        words[i * 2 + 1] = (limb_t)(chunk >> 32);
    }
    size_t needed_limbs = 8;
    while (needed_limbs > 0 && words[needed_limbs - 1] == 0) {
        needed_limbs--;
    }
    if (needed_limbs == 0) {
        bi->used = 0;
        return true;
    }
    if (!ensure_capacity(bi, needed_limbs, now)) {
        return false;
    }
    limb_t *limbs = get_limbs(bi);
    memcpy(limbs, words, needed_limbs * sizeof(limb_t));
    bi->used = needed_limbs;
    return true;
}

/**
 * @brief Copy an existing big integer into another.
 *
 * @param dst Destination big integer.
 * @param src Source to duplicate.
 * @param now Timestamp used for reallocations.
 * @return true on success, false otherwise.
 */
_Bool ttak_bigint_copy(ttak_bigint_t *dst, const ttak_bigint_t *src, uint64_t now) {
    if (dst == src) return true;
    if (!ensure_capacity(dst, src->used, now)) return false;
    
    dst->used = src->used;
    dst->is_negative = src->is_negative;
    memcpy(get_limbs(dst), get_const_limbs(src), src->used * sizeof(limb_t));
    return true;
}

/**
 * @brief Compare two big integers.
 *
 * @param lhs Left-hand operand.
 * @param rhs Right-hand operand.
 * @return -1, 0, or 1 following the standard ordering semantics.
 */
int ttak_bigint_cmp(const ttak_bigint_t *lhs, const ttak_bigint_t *rhs) {
    size_t l_used = lhs->used;
    const limb_t *l_limbs = get_const_limbs(lhs);
    while (l_used > 0 && l_limbs[l_used-1] == 0) l_used--;

    size_t r_used = rhs->used;
    const limb_t *r_limbs = get_const_limbs(rhs);
    while (r_used > 0 && r_limbs[r_used-1] == 0) r_used--;

    bool l_zero = (l_used == 0);
    bool r_zero = (r_used == 0);

    if (l_zero && r_zero) return 0;
    if (l_zero) return rhs->is_negative ? 1 : -1;
    if (r_zero) return lhs->is_negative ? -1 : 1;

    if (lhs->is_negative != rhs->is_negative) {
        return lhs->is_negative ? -1 : 1;
    }
    
    int sign = lhs->is_negative ? -1 : 1;

    if (l_used != r_used) {
        return (l_used > r_used) ? sign : -sign;
    }

    for (size_t i = l_used; i > 0; --i) {
        if (l_limbs[i-1] != r_limbs[i-1]) {
            return (l_limbs[i-1] > r_limbs[i-1]) ? sign : -sign;
        }
    }
    return 0;
}

/**
 * @brief Compare a big integer with a 64-bit value.
 *
 * @param lhs Big integer operand.
 * @param rhs Unsigned 64-bit value.
 * @return Comparison result using the same convention as ttak_bigint_cmp.
 */
int ttak_bigint_cmp_u64(const ttak_bigint_t *lhs, uint64_t rhs) {
    ttak_bigint_t rhs_bi;
    ttak_bigint_init_u64(&rhs_bi, rhs, 0);
    int result = ttak_bigint_cmp(lhs, &rhs_bi);
    ttak_bigint_free(&rhs_bi, 0);
    return result;
}

/**
 * @brief Test whether the big integer equals zero.
 *
 * @param bi Big integer to inspect.
 * @return true if the integer is zero, false otherwise.
 */
bool ttak_bigint_is_zero(const ttak_bigint_t *bi) {
    if (!bi || bi->used == 0) return true;
    const limb_t *limbs = get_const_limbs(bi);
    for (size_t i = 0; i < bi->used; i++) {
        if (limbs[i] != 0) return false;
    }
    return true;
}

/**
 * @brief Add two big integers.
 *
 * @param dst Destination for the result.
 * @param lhs Left operand.
 * @param rhs Right operand.
 * @param now Timestamp for potential allocations.
 * @return true on success, false on allocation failure.
 */
_Bool ttak_bigint_add(ttak_bigint_t *dst, const ttak_bigint_t *lhs, const ttak_bigint_t *rhs, uint64_t now) {
    if (lhs->is_negative != rhs->is_negative) {
        // Subtraction case: create temporary/view or handle with magnitude
        ttak_bigint_t temp;
        if (lhs->is_negative) { // (-a) + b == b - a (where a > 0, b > 0)
            ttak_bigint_init(&temp, now);
            ttak_bigint_copy(&temp, lhs, now);
            temp.is_negative = false;
            _Bool ok = ttak_bigint_sub(dst, rhs, &temp, now);
            ttak_bigint_free(&temp, now);
            return ok;
        } else { // a + (-b) == a - b (where a > 0, b > 0)
            ttak_bigint_init(&temp, now);
            ttak_bigint_copy(&temp, rhs, now);
            temp.is_negative = false;
            _Bool ok = ttak_bigint_sub(dst, lhs, &temp, now);
            ttak_bigint_free(&temp, now);
            return ok;
        }
    }

    size_t max_used = lhs->used > rhs->used ? lhs->used : rhs->used;
    if (!ensure_capacity(dst, max_used + 1, now)) return false;
    const limb_t *l = get_const_limbs(lhs);
    const limb_t *r = get_const_limbs(rhs);
    limb_t *d = get_limbs(dst);

    if (ttak_bigint_accel_available() && max_used >= ttak_bigint_accel_min_limbs()) {
        size_t out_used = 0;
        if (ttak_bigint_accel_add_raw(d, dst->capacity, &out_used,
                                      l, lhs->used, r, rhs->used)) {
            dst->used = out_used;
            dst->is_negative = lhs->is_negative;
            trim_unused(dst);
            return true;
        }
    }

    uint64_t carry = 0;
    size_t i = 0;
    for (; i < max_used; ++i) {
        uint64_t sum = carry;
        if (i < lhs->used) sum += l[i];
        if (i < rhs->used) sum += r[i];
        d[i] = (limb_t)sum;
        carry = sum >> 32;
    }
    if (carry) {
        d[i++] = (limb_t)carry;
    }
    dst->used = i;
    dst->is_negative = lhs->is_negative;
    trim_unused(dst);
    return true;
}

/**
 * @brief Subtract rhs from lhs and store in dst.
 *
 * @param dst Destination big integer.
 * @param lhs Minuend.
 * @param rhs Subtrahend.
 * @param now Timestamp for allocations.
 * @return true on success, false otherwise.
 */
_Bool ttak_bigint_sub(ttak_bigint_t *dst, const ttak_bigint_t *lhs, const ttak_bigint_t *rhs, uint64_t now) {
    if (lhs->is_negative != rhs->is_negative) {
        // Addition case: (-a) - b == -(a + b), a - (-b) == a + b (where a, b > 0)
        ttak_bigint_t temp;
        if (lhs->is_negative) { // (-a) - b == -(a + b)
            ttak_bigint_init(&temp, now);
            ttak_bigint_copy(&temp, lhs, now);
            temp.is_negative = false;
            _Bool ok = ttak_bigint_add(dst, &temp, rhs, now);
            if (ok) dst->is_negative = true;
            ttak_bigint_free(&temp, now);
            return ok;
        } else { // a - (-b) == a + b
            ttak_bigint_init(&temp, now);
            ttak_bigint_copy(&temp, rhs, now);
            temp.is_negative = false;
            _Bool ok = ttak_bigint_add(dst, lhs, &temp, now);
            ttak_bigint_free(&temp, now);
            return ok;
        }
    }

    // Same signs: a - b or (-a) - (-b) == b - a
    const ttak_bigint_t *a = lhs, *b = rhs;
    if (lhs->is_negative) { // (-a) - (-b) == b - a
        a = rhs; b = lhs;
    }

    // Compare magnitudes
    size_t a_used = a->used;
    const limb_t *a_limbs = get_const_limbs(a);
    while (a_used > 0 && a_limbs[a_used - 1] == 0) a_used--;

    size_t b_used = b->used;
    const limb_t *b_limbs = get_const_limbs(b);
    while (b_used > 0 && b_limbs[b_used - 1] == 0) b_used--;

    int mag_cmp = 0;
    if (a_used != b_used) {
        mag_cmp = (a_used > b_used) ? 1 : -1;
    } else {
        for (size_t i = a_used; i > 0; --i) {
            if (a_limbs[i - 1] != b_limbs[i - 1]) {
                mag_cmp = (a_limbs[i - 1] > b_limbs[i - 1]) ? 1 : -1;
                break;
            }
        }
    }

    if (mag_cmp == 0) {
        return ttak_bigint_set_u64(dst, 0, now);
    }

    bool result_is_negative = false;
    const ttak_bigint_t *larger = a;
    const ttak_bigint_t *smaller = b;
    if (mag_cmp < 0) {
        result_is_negative = true;
        larger = b;
        smaller = a;
    }

    if (!ensure_capacity(dst, larger->used, now)) return false;

    limb_t *d = get_limbs(dst);
    const limb_t *l = get_const_limbs(larger);
    const limb_t *r = get_const_limbs(smaller);

    uint64_t borrow = 0;
    size_t i = 0;
    for (; i < larger->used; ++i) {
        uint64_t diff = (uint64_t)l[i] - borrow;
        if (i < smaller->used) diff -= r[i];
        d[i] = (limb_t)diff;
        borrow = (diff >> 32) & 1;
    }
    dst->used = i;
    dst->is_negative = result_is_negative;
    trim_unused(dst);
    return true;
}

/**
 * @brief Multiply two big integers.
 *
 * @param dst Destination for the product.
 * @param lhs Left operand.
 * @param rhs Right operand.
 * @param now Timestamp for scratch allocations.
 * @return true when multiplication succeeds, false when memory is insufficient.
 */
_Bool ttak_bigint_mul(ttak_bigint_t *dst, const ttak_bigint_t *lhs, const ttak_bigint_t *rhs, uint64_t now) {
    if (ttak_bigint_is_zero(lhs) || ttak_bigint_is_zero(rhs)) {
        return ttak_bigint_set_u64(dst, 0, now);
    }

    size_t needed = lhs->used + rhs->used;
    ttak_bigint_t tmp;
    ttak_bigint_init(&tmp, now);
    if (!ensure_capacity(&tmp, needed, now)) {
        ttak_bigint_free(&tmp, now);
        return false;
    }

    limb_t *t = get_limbs(&tmp);
    const limb_t *l = get_const_limbs(lhs);
    const limb_t *r = get_const_limbs(rhs);

    bool attempted_accel = false;
    if (ttak_bigint_accel_available()) {
        size_t threshold = ttak_bigint_accel_min_limbs();
        if (lhs->used >= threshold || rhs->used >= threshold || needed >= threshold) {
            size_t out_used = 0;
            if (ttak_bigint_accel_mul_raw(t, tmp.capacity, &out_used, l, lhs->used, r, rhs->used)) {
                tmp.used = out_used;
                tmp.is_negative = lhs->is_negative != rhs->is_negative;
                trim_unused(&tmp);
                _Bool ok = ttak_bigint_copy(dst, &tmp, now);
                ttak_bigint_free(&tmp, now);
                return ok;
            }
            attempted_accel = true;
        }
    }

    (void)attempted_accel;
    memset(t, 0, needed * sizeof(limb_t));
    for (size_t i = 0; i < lhs->used; ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < rhs->used; ++j) {
            uint64_t prod = (uint64_t)l[i] * r[j] + t[i+j] + carry;
            t[i+j] = (limb_t)prod;
            carry = prod >> 32;
        }
        size_t k = i + rhs->used;
        while (carry && k < needed) {
            uint64_t sum = (uint64_t)t[k] + carry;
            t[k] = (limb_t)sum;
            carry = sum >> 32;
            k++;
        }
    }

    tmp.used = needed;
    tmp.is_negative = lhs->is_negative != rhs->is_negative;
    trim_unused(&tmp);
    
    _Bool ok = ttak_bigint_copy(dst, &tmp, now);
    ttak_bigint_free(&tmp, now);
    return ok;
}

/**
 * @brief Divide a big integer by a 64-bit value.
 *
 * @param q   Optional quotient output.
 * @param r   Optional remainder output.
 * @param n   Dividend.
 * @param d   Unsigned 64-bit divisor.
 * @param now Timestamp for temporary allocations.
 * @return true on success, false when dividing by zero or allocation fails.
 */
_Bool ttak_bigint_div_u64(ttak_bigint_t *q, ttak_bigint_t *r, const ttak_bigint_t *n, uint64_t d, uint64_t now) {
    if (d == 0) return false; // Division by zero

    // Handle zero dividend
    if (ttak_bigint_is_zero(n)) {
        if (q) ttak_bigint_set_u64(q, 0, now);
        if (r) ttak_bigint_set_u64(r, 0, now);
        return true;
    }

    // Handle division by 1
    if (d == 1) {
        if (q) ttak_bigint_copy(q, n, now);
        if (r) ttak_bigint_set_u64(r, 0, now);
        return true;
    }

    // Determine signs
    bool q_neg = n->is_negative; // d is always positive (uint64_t)
    bool r_neg = n->is_negative;

    // Use absolute value of n for calculation
    ttak_bigint_t n_abs;
    ttak_bigint_init_copy(&n_abs, n, now);
    n_abs.is_negative = false;

    // If dividend is smaller than divisor, quotient is 0, remainder is dividend
    if (ttak_bigint_cmp_u64(&n_abs, d) < 0) {
        if (q) ttak_bigint_set_u64(q, 0, now);
        if (r) ttak_bigint_copy(r, n, now); // Remainder keeps original sign
        ttak_bigint_free(&n_abs, now);
        return true;
    }

    // Prepare quotient (q)
    if (q) {
        if (!ensure_capacity(q, n_abs.used, now)) {
            ttak_bigint_free(&n_abs, now);
            return false;
        }
        q->used = n_abs.used;
        memset(get_limbs(q), 0, q->capacity * sizeof(limb_t));
    }
    limb_t *q_limbs = q ? get_limbs(q) : NULL;
    const limb_t *n_limbs = get_const_limbs(&n_abs);

    uint64_t remainder = 0; // Current remainder for long division
    for (size_t i = n_abs.used; i > 0; --i) {
        remainder = (remainder << 32) | n_limbs[i-1];
        if (q_limbs) {
            q_limbs[i-1] = (limb_t)(remainder / d);
        }
        remainder %= d;
    }

    // Set quotient and remainder
    if (q) {
        q->is_negative = q_neg;
        trim_unused(q);
    }
    if (r) {
        ttak_bigint_set_u64(r, remainder, now);
        r->is_negative = r_neg;
    }
    
ttak_bigint_free(&n_abs, now);
    return true;
}

/**
 * @brief Compute n mod d where d is a 64-bit value.
 *
 * @param r   Destination for the remainder.
 * @param n   Dividend.
 * @param d   Unsigned divisor.
 * @param now Timestamp for helper allocations.
 * @return true on success, false otherwise.
 */
_Bool ttak_bigint_mod_u64(ttak_bigint_t *r, const ttak_bigint_t *n, uint64_t d, uint64_t now) {
    return ttak_bigint_div_u64(NULL, r, n, d, now);
}

/**
 * @brief Add a 64-bit value to a big integer.
 *
 * @param dst Destination for the sum.
 * @param lhs Left operand.
 * @param rhs Unsigned value to add.
 * @param now Timestamp for allocations.
 * @return true on success, false otherwise.
 */
_Bool ttak_bigint_add_u64(ttak_bigint_t *dst, const ttak_bigint_t *lhs, uint64_t rhs, uint64_t now) {
    ttak_bigint_t rhs_bi;
    ttak_bigint_init_u64(&rhs_bi, rhs, now);
    _Bool ok = ttak_bigint_add(dst, lhs, &rhs_bi, now);
    ttak_bigint_free(&rhs_bi, now);
    return ok;
}

/**
 * @brief Multiply a big integer by a 64-bit value.
 *
 * @param dst Destination for the product.
 * @param lhs Multiplicand.
 * @param rhs 64-bit multiplier.
 * @param now Timestamp for allocations.
 * @return true on success, false if allocation fails.
 */
_Bool ttak_bigint_mul_u64(ttak_bigint_t *dst, const ttak_bigint_t *lhs, uint64_t rhs, uint64_t now) {
    if (rhs == 0 || ttak_bigint_is_zero(lhs)) {
        return ttak_bigint_set_u64(dst, 0, now);
    }
    if (rhs == 1) {
        return ttak_bigint_copy(dst, lhs, now);
    }

    size_t rhs_used = (rhs > 0xFFFFFFFFu) ? 2 : 1;
    size_t needed = lhs->used + rhs_used;
    
    // Use a temporary limb buffer to handle in-place multiplication safely
    limb_t *d = ttak_mem_alloc_raw(needed * sizeof(limb_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!d) return false;
    memset(d, 0, needed * sizeof(limb_t));

    const limb_t *l = get_const_limbs(lhs);
    limb_t rhs_limbs[2] = {(limb_t)rhs, (limb_t)(rhs >> 32)};

    for (size_t i = 0; i < lhs->used; ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < rhs_used; ++j) {
            uint64_t prod = (uint64_t)l[i] * rhs_limbs[j] + d[i+j] + carry;
            d[i+j] = (limb_t)prod;
            carry = prod >> 32;
        }
        size_t k = i + rhs_used;
        while (carry && k < needed) {
            uint64_t sum = (uint64_t)d[k] + carry;
            d[k] = (limb_t)sum;
            carry = sum >> 32;
            k++;
        }
    }

    if (!ensure_capacity(dst, needed, now)) {
        ttak_mem_free(d);
        return false;
    }
    
    limb_t *dst_limbs = get_limbs(dst);
    memcpy(dst_limbs, d, needed * sizeof(limb_t));
    dst->used = needed;
    dst->is_negative = lhs->is_negative; // rhs is not negative
    trim_unused(dst);
    
    ttak_mem_free(d);
    return true;
}

/**
 * @brief Compute the number of significant bits in the big integer.
 *
 * @param bi Big integer to inspect.
 * @return Bit length, or 0 when the value is zero.
 */
size_t ttak_bigint_get_bit_length(const ttak_bigint_t *bi) {
    if (ttak_bigint_is_zero(bi)) return 0;
    const limb_t *limbs = get_const_limbs(bi);
    size_t top_limb_idx = bi->used - 1;
    limb_t top_limb = limbs[top_limb_idx];
    size_t bits = top_limb_idx * 32;
    
    unsigned long msb_pos;
    #if defined(__GNUC__) || defined(__clang__)
        msb_pos = 31 - __builtin_clz(top_limb);
    #else
        msb_pos = 0;
        while ((1UL << msb_pos) <= top_limb && msb_pos < 32) {
            msb_pos++;
        }
        msb_pos--;
    #endif
    
    return bits + msb_pos + 1;
}

#define TTAK_BIGINT_BASE_BITS 32
#define TTAK_BIGINT_BASE (1ULL << TTAK_BIGINT_BASE_BITS)

// Helper for Knuth division: u -= v
/**
 * @brief Subtract two limb arrays in-place (u -= v).
 *
 * @param u Destination/minuend array.
 * @param v Subtrahend array.
 * @param n Number of limbs to process.
 * @return Final borrow flag (0 or 1).
 */
limb_t sub_limbs(limb_t *u, const limb_t *v, size_t n) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < n; ++i) {
        uint64_t diff = (uint64_t)u[i] - v[i] - borrow;
        u[i] = (limb_t)diff;
        borrow = (diff >> TTAK_BIGINT_BASE_BITS) & 1;
    }
    return (limb_t)borrow;
}

// Helper for Knuth division: u += v
/**
 * @brief Add two limb arrays in-place (u += v).
 *
 * @param u Destination/addend array.
 * @param v Second addend array.
 * @param n Number of limbs to sum.
 * @return Final carry flag.
 */
limb_t add_limbs(limb_t *u, const limb_t *v, size_t n) {
    uint64_t carry = 0;
    for (size_t i = 0; i < n; ++i) {
        uint64_t sum = (uint64_t)u[i] + v[i] + carry;
        u[i] = (limb_t)sum;
        carry = sum >> TTAK_BIGINT_BASE_BITS;
    }
    return (limb_t)carry;
}

// Helper for Knuth division: left shift
/**
 * @brief Left shift a limb array by a small number of bits.
 *
 * @param num        Buffer to shift.
 * @param len        Number of limbs.
 * @param shift_bits Bits to shift (0 < shift_bits < base bits).
 * @return Carry produced by the shift.
 */
static limb_t lshift_limbs(limb_t *num, size_t len, int shift_bits) {
    if (shift_bits == 0) return 0;
    limb_t carry = 0;
    for (size_t i = 0; i < len; ++i) {
        limb_t next_carry = num[i] >> (TTAK_BIGINT_BASE_BITS - shift_bits);
        num[i] = (num[i] << shift_bits) | carry;
        carry = next_carry;
    }
    return carry;
}

// Helper for Knuth division: right shift
/**
 * @brief Right shift a limb array by a small number of bits.
 *
 * @param num        Buffer to shift.
 * @param len        Number of limbs.
 * @param shift_bits Bits to shift (0 < shift_bits < base bits).
 */
static void rshift_limbs(limb_t *num, size_t len, int shift_bits) {
    if (shift_bits == 0) return;
    limb_t carry = 0;
    for (size_t i = len; i > 0; --i) {
        limb_t next_carry = num[i-1] << (TTAK_BIGINT_BASE_BITS - shift_bits);
        num[i-1] = (num[i-1] >> shift_bits) | carry;
        carry = next_carry;
    }
}

// Knuth's Algorithm D for division.
// q_out will have m + 1 limbs, r_out will have n limbs.
// u is the dividend (m+n limbs), v is the divisor (n limbs).
/**
 * @brief Execute Knuth's Algorithm D for limb arrays.
 *
 * @param q_out Quotient result (m + 1 limbs).
 * @param r_out Remainder result (n limbs).
 * @param u     Dividend limbs (m + n limbs).
 * @param m     Difference in limb counts between dividend and divisor.
 * @param v     Divisor limbs (n limbs).
 * @param n     Limb count of the divisor.
 * @param now   Timestamp for transient allocations.
 * @return true on success, false if allocation fails or divisor is invalid.
 */
static _Bool knuth_div_limbs(limb_t *q_out, limb_t *r_out,
                             const limb_t *u, size_t m,
                             const limb_t *v, size_t n,
                             uint64_t now) {
    // D1. Normalize.
    int d = 0;
    if (n > 0) {
        limb_t vn_1 = v[n - 1];
        if (vn_1 == 0) return false; // Should not happen if d is normalized
        while (vn_1 < (limb_t)(TTAK_BIGINT_BASE / 2)) {
            vn_1 <<= 1;
            d++;
        }
    }

    limb_t *u_norm = ttak_mem_alloc_raw((m + n + 1) * sizeof(limb_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
    limb_t *v_norm = ttak_mem_alloc_raw(n * sizeof(limb_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!u_norm || !v_norm) {
        ttak_mem_free(u_norm);
        ttak_mem_free(v_norm);
        return false;
    }
    memset(u_norm, 0, (m + n + 1) * sizeof(limb_t));

    memcpy(u_norm, u, (m + n) * sizeof(limb_t));
    if (d > 0) u_norm[m + n] = lshift_limbs(u_norm, m + n, d);

    memcpy(v_norm, v, n * sizeof(limb_t));
    if (d > 0) lshift_limbs(v_norm, n, d);

    // D2. Initialize j.
    for (int j = m; j >= 0; --j) {
        // D3. Calculate q_hat.
        uint64_t u_hat = ((uint64_t)u_norm[j + n] << TTAK_BIGINT_BASE_BITS) | u_norm[j + n - 1];
        limb_t q_hat;

        if (u_norm[j + n] == v_norm[n - 1]) {
            q_hat = (limb_t)-1;
        } else {
            q_hat = u_hat / v_norm[n - 1];
        }

        if (n > 1) {
            while ((uint64_t)q_hat * v_norm[n - 2] >
                   (u_hat - (uint64_t)q_hat * v_norm[n - 1]) * TTAK_BIGINT_BASE + u_norm[j + n - 2]) {
                q_hat--;
            }
        }

        // D4. Multiply and subtract.
        limb_t *u_norm_slice = &u_norm[j];
        uint64_t borrow = 0;
        uint64_t carry_prod = 0;
        for (size_t i = 0; i < n; ++i) {
            uint64_t prod = (uint64_t)v_norm[i] * q_hat + carry_prod;
            carry_prod = prod >> TTAK_BIGINT_BASE_BITS;
            uint64_t diff = (uint64_t)u_norm_slice[i] - (limb_t)prod - borrow;
            u_norm_slice[i] = (limb_t)diff;
            borrow = (diff >> TTAK_BIGINT_BASE_BITS) & 1;
        }
        uint64_t diff = (uint64_t)u_norm_slice[n] - carry_prod - borrow;
        u_norm_slice[n] = (limb_t)diff;
        borrow = (diff >> TTAK_BIGINT_BASE_BITS) & 1;

        // D5. Add back if remainder is negative.
        if (borrow > 0) {
            q_hat--;
            u_norm_slice[n] += add_limbs(u_norm_slice, v_norm, n);
        }

        // D6. Set quotient digit.
        if (q_out) q_out[j] = q_hat;
    }

    // D7. Unnormalize remainder.
    if (r_out) {
        memcpy(r_out, u_norm, n * sizeof(limb_t));
        if (d > 0) rshift_limbs(r_out, n, d);
    }

    ttak_mem_free(u_norm);
    ttak_mem_free(v_norm);
    return true;
}

/**
 * @brief Divide two arbitrary-precision integers.
 *
 * @param q   Optional quotient output.
 * @param r   Optional remainder output.
 * @param n   Dividend.
 * @param d   Divisor (must be non-zero).
 * @param now Timestamp for scratch allocations.
 * @return true on success, false on division by zero or allocation failure.
 */
_Bool ttak_bigint_div(ttak_bigint_t *q, ttak_bigint_t *r, const ttak_bigint_t *n, const ttak_bigint_t *d, uint64_t now) {
    if (ttak_bigint_is_zero(d)) return false;

    // Use absolute values for comparison and division
    ttak_bigint_t n_abs, d_abs;
    ttak_bigint_init_copy(&n_abs, n, now);
    ttak_bigint_init_copy(&d_abs, d, now);
    n_abs.is_negative = false;
    d_abs.is_negative = false;

    int cmp = ttak_bigint_cmp(&n_abs, &d_abs);

    ttak_bigint_free(&n_abs, now);
    ttak_bigint_free(&d_abs, now);

    if (cmp < 0) {
        if (q) ttak_bigint_set_u64(q, 0, now);
        if (r) ttak_bigint_copy(r, n, now);
        return true;
    }
    if (cmp == 0) {
        if (q) {
            ttak_bigint_set_u64(q, 1, now);
            q->is_negative = n->is_negative != d->is_negative;
        }
        if (r) ttak_bigint_set_u64(r, 0, now);
        return true;
    }

    ttak_bigint_t n_tmp;
    ttak_bigint_init_copy(&n_tmp, n, now);
    n_tmp.is_negative = false;

    ttak_bigint_t d_tmp;
    ttak_bigint_init_copy(&d_tmp, d, now);
    d_tmp.is_negative = false;

    const limb_t *n_limbs = get_const_limbs(&n_tmp);
    const limb_t *d_limbs = get_const_limbs(&d_tmp);
    size_t n_used = n_tmp.used;
    size_t d_used = d_tmp.used;

    if (d_used == 1) {
        _Bool ok = ttak_bigint_div_u64(q, r, n, (uint64_t)d_limbs[0], now);
        if (ok && q) {
            q->is_negative = n->is_negative != d->is_negative;
        }
        ttak_bigint_free(&n_tmp, now);
        ttak_bigint_free(&d_tmp, now);
        return ok;
    }

    size_t m = n_used - d_used;
    size_t q_limbs_len = m + 1;

    limb_t *q_limbs = NULL;
    if (q) {
        if (!ensure_capacity(q, q_limbs_len, now)) {
             ttak_bigint_free(&n_tmp, now);
             ttak_bigint_free(&d_tmp, now);
             return false;
        }
        q_limbs = get_limbs(q);
        memset(q_limbs, 0, q->capacity * sizeof(limb_t));
    }

    limb_t *r_limbs = NULL;
    if (r) {
        if (!ensure_capacity(r, d_used, now)) {
            ttak_bigint_free(&n_tmp, now);
            ttak_bigint_free(&d_tmp, now);
            return false;
        }
        r_limbs = get_limbs(r);
        memset(r_limbs, 0, r->capacity * sizeof(limb_t));
    }

    _Bool ok = knuth_div_limbs(q_limbs, r_limbs, n_limbs, m, d_limbs, d_used, now);

    if (ok) {
        if (q) {
            q->used = q_limbs_len;
            q->is_negative = n->is_negative != d->is_negative;
            trim_unused(q);
        }
        if (r) {
            r->used = d_used;
            r->is_negative = n->is_negative;
            trim_unused(r);
        }
    }

    ttak_bigint_free(&n_tmp, now);
    ttak_bigint_free(&d_tmp, now);

    return ok;
}

/**
 * @brief Compute the modulus n mod d for arbitrary-precision operands.
 *
 * @param r   Destination for the remainder.
 * @param n   Dividend.
 * @param d   Divisor.
 * @param now Timestamp for scratch allocations.
 * @return true on success, false on division errors.
 */
_Bool ttak_bigint_mod(ttak_bigint_t *r, const ttak_bigint_t *n, const ttak_bigint_t *d, uint64_t now) {
    return ttak_bigint_div(NULL, r, n, d, now);
}

/**
 * @brief Convert a big integer to a decimal string.
 *
 * Caller assumes ownership of the returned buffer.
 *
 * @param bi  Big integer to format.
 * @param now Timestamp for allocations.
 * @return Null-terminated decimal string, or NULL on failure.
 */
char* ttak_bigint_to_string(const ttak_bigint_t *bi, uint64_t now) {
    if (ttak_bigint_is_zero(bi)) {
        char *s = ttak_mem_alloc_raw(2, __TTAK_UNSAFE_MEM_FOREVER__, now);
        if(s) strcpy(s, "0");
        return s;
    }

    // Estimate buffer size: log10(2^N) = N * log10(2) ~= N * 0.301
    size_t num_bits = ttak_bigint_get_bit_length(bi);
    size_t estimated_len = (size_t)(num_bits * 0.30103) + 2; // + sign and null
    if (bi->is_negative) estimated_len++;

    char *s = ttak_mem_alloc_raw(estimated_len, __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!s) return NULL;

    char *p = s;
    
    ttak_bigint_t tmp, rem;
    ttak_bigint_init_copy(&tmp, bi, now);
    ttak_bigint_init(&rem, now);
    tmp.is_negative = false;

    while(!ttak_bigint_is_zero(&tmp)) {
        ttak_bigint_div_u64(&tmp, &rem, &tmp, 10, now);
        limb_t *rem_limbs = get_limbs(&rem);
        *p++ = "0123456789"[rem_limbs[0]];
    }

    if (bi->is_negative) {
        *p++ = '-';
    }
    *p = '\0';

    // Reverse the string
    char *start = s;
    char *end = p - 1;
    while(start < end) {
        char temp = *start;
        *start++ = *end;
        *end-- = temp;
    }
    
    ttak_bigint_free(&tmp, now);
    ttak_bigint_free(&rem, now);

    return s;
}

/**
 * @brief Reduce a big integer modulo the Mersenne number 2^p - 1.
 *
 * @param bi  Value to reduce in-place.
 * @param p   Exponent that defines the modulus.
 * @param now Timestamp for helper allocations.
 */
void ttak_bigint_mersenne_mod(ttak_bigint_t *bi, int p, uint64_t now) {
    (void)now;
    size_t required_limbs = ((size_t)p + 31) / 32;
    if (required_limbs > TTAK_MAX_LIMB_LIMIT) {
        return;
    }

    while (ttak_bigint_get_bit_length(bi) > (size_t)p) {
        ttak_bigint_t high, low;
        ttak_bigint_init(&high, now);
        ttak_bigint_init(&low, now);

        ensure_capacity(&low, required_limbs, now);
        
        limb_t* bi_limbs = get_limbs(bi);
        limb_t* low_limbs = get_limbs(&low);

        size_t p_limb_idx = p / 32;
        size_t p_bit_off = p % 32;

        // Low part
        memcpy(low_limbs, bi_limbs, p_limb_idx * sizeof(limb_t));
        limb_t mask = (1U << p_bit_off) - 1;
        if (p_bit_off > 0) {
            low_limbs[p_limb_idx] = bi_limbs[p_limb_idx] & mask;
        }
        low.used = required_limbs;
        trim_unused(&low);

        // High part (right shift by p)
        size_t high_limbs_needed = bi->used > p_limb_idx ? bi->used - p_limb_idx : 0;
        ensure_capacity(&high, high_limbs_needed, now);
        limb_t* high_limbs = get_limbs(&high);
        
        for(size_t i = 0; i < high_limbs_needed; ++i) {
            limb_t val = bi_limbs[p_limb_idx + i] >> p_bit_off;
            if (p_bit_off > 0 && (p_limb_idx + i + 1) < bi->used) {
                val |= bi_limbs[p_limb_idx + i + 1] << (32 - p_bit_off);
            }
            high_limbs[i] = val;
        }
        high.used = high_limbs_needed;
        trim_unused(&high);

        ttak_bigint_add(bi, &low, &high, now);

        ttak_bigint_free(&high, now);
        ttak_bigint_free(&low, now);
    }

    // Final check: if bi == 2^p - 1, it should be 0
    ttak_bigint_t mersenne_prime;
    ttak_bigint_init(&mersenne_prime, now);
    ttak_bigint_set_u64(&mersenne_prime, 1, now);
    // left shift by p
    size_t limb_shift = p / 32;
    size_t bit_shift = p % 32;
    ensure_capacity(&mersenne_prime, limb_shift + 1, now);
    get_limbs(&mersenne_prime)[limb_shift] = 1UL << bit_shift;
    mersenne_prime.used = limb_shift + 1;
    
    ttak_bigint_t one;
    ttak_bigint_init_u64(&one, 1, now);
    ttak_bigint_sub(&mersenne_prime, &mersenne_prime, &one, now);

    if (ttak_bigint_cmp(bi, &mersenne_prime) == 0) {
        ttak_bigint_set_u64(bi, 0, now);
    }

    ttak_bigint_free(&mersenne_prime, now);
    ttak_bigint_free(&one, now);
}

/**
 * @brief Export the big integer into an unsigned 64-bit value if possible.
 *
 * @param bi        Big integer to convert.
 * @param value_out Output pointer that receives the value.
 * @return true if the value fits in 64 bits, false otherwise.
 */
bool ttak_bigint_export_u64(const ttak_bigint_t *bi, uint64_t *value_out) {
    if (!bi || bi->is_negative || bi->used > 2) {
        if (value_out) *value_out = 0;
        return false;
    }
    const limb_t *limbs = get_const_limbs(bi);
    uint64_t value = 0;
    if (bi->used > 0) value |= limbs[0];
    if (bi->used > 1) value |= (uint64_t)limbs[1] << 32;
    if (value_out) *value_out = value;
    return true;
}

bool ttak_bigint_export_u128(const ttak_bigint_t *bi, ttak_u128_t *value_out) {
    if (!bi || !value_out || bi->is_negative || bi->used > 4) {
        if (value_out) *value_out = ttak_u128_zero();
        return false;
    }
    const limb_t *limbs = get_const_limbs(bi);
    uint64_t lo = 0, hi = 0;
    if (bi->used > 0) lo |= (uint64_t)limbs[0];
    if (bi->used > 1) lo |= (uint64_t)limbs[1] << 32;
    if (bi->used > 2) hi |= (uint64_t)limbs[2];
    if (bi->used > 3) hi |= (uint64_t)limbs[3] << 32;
    *value_out = ttak_u128_make(hi, lo);
    return true;
}

bool ttak_bigint_export_u256(const ttak_bigint_t *bi, ttak_u256_t *value_out) {
    if (!bi || !value_out || bi->is_negative || bi->used > 8) {
        if (value_out) *value_out = ttak_u256_zero();
        return false;
    }
    uint64_t chunks[4] = {0, 0, 0, 0};
    const limb_t *limbs = get_const_limbs(bi);
    for (int pair = 0; pair < 4; ++pair) {
        size_t idx = pair * 2;
        if (bi->used > idx) chunks[pair] |= (uint64_t)limbs[idx];
        if (bi->used > idx + 1) chunks[pair] |= (uint64_t)limbs[idx + 1] << 32;
    }
    *value_out = ttak_u256_from_limbs(chunks[3], chunks[2], chunks[1], chunks[0]);
    return true;
}

#include <ttak/security/sha256.h>

/**
 * @brief Hash the big integer with SHA-256 and emit a hexadecimal digest.
 *
 * @param bi  Input big integer.
 * @param out Buffer of at least 65 chars to store the ASCII hash and null terminator.
 */
void ttak_bigint_to_hex_hash(const ttak_bigint_t *bi, char out[65]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    if (bi && bi->used > 0) {
        const limb_t *limbs = get_const_limbs(bi);
        sha256_update(&ctx, (const uint8_t *)limbs, bi->used * sizeof(limb_t));
    }
    uint8_t digest[32];
    sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out[64] = '\0';
}

/**
 * @brief Produce a short prefix string describing the value.
 *
 * @param bi       Big integer to summarize.
 * @param dest     Destination buffer.
 * @param dest_cap Size of the destination buffer.
 */
void ttak_bigint_format_prefix(const ttak_bigint_t *bi, char *dest, size_t dest_cap) {
    if (!dest || dest_cap == 0) {
        if (dest) dest[0] = '\0';
        return;
    }
    
    char* s = ttak_bigint_to_string(bi, 0);
    if (!s) {
        dest[0] = '\0';
        return;
    }

    strncpy(dest, s, dest_cap - 1);
    dest[dest_cap - 1] = '\0';
    ttak_mem_free(s);
}

/**
 * @brief Compute the SHA-256 digest of the big integer limbs.
 *
 * @param bi  Big integer to hash.
 * @param out Output buffer for the 32-byte digest.
 */
void ttak_bigint_hash(const ttak_bigint_t *bi, uint8_t out[32]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    if (bi && bi->used > 0) {
        const limb_t *limbs = get_const_limbs(bi);
        sha256_update(&ctx, (const uint8_t *)limbs, bi->used * sizeof(limb_t));
    }
    sha256_final(&ctx, out);
}
