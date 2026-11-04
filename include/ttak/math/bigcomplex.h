#ifndef TTAK_MATH_BIGCOMPLEX_H
#define TTAK_MATH_BIGCOMPLEX_H

#include <ttak/math/bigreal.h>

/**
 * @brief Arbitrary-precision complex number engine.
 */
typedef struct ttak_bigcomplex {
    ttak_bigreal_t real;
    ttak_bigreal_t imag;
#ifndef _MSC_VER
    alignas(alignof(ttak_max_align_t)) char padding[0];
#endif
} ttak_bigcomplex_t;

typedef ttak_bigcomplex_t tt_big_c_t;

void ttak_bigcomplex_init(ttak_bigcomplex_t *bc, uint64_t now);
void ttak_bigcomplex_free(ttak_bigcomplex_t *bc, uint64_t now);

/**
 * @brief Adds two complex numbers component-wise.
 *
 * Returns false if underlying real additions fail.
 */
_Bool ttak_bigcomplex_add(ttak_bigcomplex_t *dst, const ttak_bigcomplex_t *lhs, const ttak_bigcomplex_t *rhs, uint64_t now);

#endif // TTAK_MATH_BIGCOMPLEX_H
