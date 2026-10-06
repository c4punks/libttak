#ifndef TTAK_MATH_BIGREAL_H
#define TTAK_MATH_BIGREAL_H

#include <ttak/math/bigint.h>

/**
 * @brief Arbitrary-precision real number engine.
 * Represents value = mantissa * 10^exponent.
 */
typedef struct ttak_bigreal {
    ttak_bigint_t mantissa;
    int64_t       exponent;
#ifndef _MSC_VER
    alignas(ttak_max_align_t) char padding[0];
#endif
} ttak_bigreal_t;

typedef ttak_bigreal_t tt_big_r_t;

void ttak_bigreal_init(ttak_bigreal_t *br, uint64_t now);
void ttak_bigreal_init_u64(ttak_bigreal_t *br, uint64_t value, uint64_t now);
void ttak_bigreal_free(ttak_bigreal_t *br, uint64_t now);
_Bool ttak_bigreal_copy(ttak_bigreal_t *dst, const ttak_bigreal_t *src, uint64_t now);

/**
 * @brief Normalizes a bigreal by trimming trailing decimal zeros from mantissa.
 */
void ttak_bigreal_normalize(ttak_bigreal_t *br, uint64_t now);

/**
 * @brief Aligns the exponents of two bigreals to the smaller one.
 */
_Bool ttak_bigreal_align(ttak_bigreal_t *a, ttak_bigreal_t *b, uint64_t now);

/**
 * @brief Deep copy a bigreal number.
 */
_Bool ttak_bigreal_copy(ttak_bigreal_t *dst, const ttak_bigreal_t *src, uint64_t now);

/**
 * @brief Adds two bigreal numbers with matching exponents.
 *
 * Returns false if exponents differ or on allocation failure.
 */
_Bool ttak_bigreal_add(ttak_bigreal_t *dst, const ttak_bigreal_t *lhs, const ttak_bigreal_t *rhs, uint64_t now);
_Bool ttak_bigreal_sub(ttak_bigreal_t *dst, const ttak_bigreal_t *lhs, const ttak_bigreal_t *rhs, uint64_t now);
_Bool ttak_bigreal_mul(ttak_bigreal_t *dst, const ttak_bigreal_t *lhs, const ttak_bigreal_t *rhs, uint64_t now);
_Bool ttak_bigreal_div(ttak_bigreal_t *dst, const ttak_bigreal_t *lhs, const ttak_bigreal_t *rhs, uint64_t now);

int ttak_bigreal_cmp(const ttak_bigreal_t *lhs, const ttak_bigreal_t *rhs, uint64_t now);

#endif // TTAK_MATH_BIGREAL_H
