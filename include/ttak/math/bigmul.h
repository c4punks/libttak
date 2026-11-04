#ifndef TTAK_MATH_BIGMUL_H
#define TTAK_MATH_BIGMUL_H

#include <ttak/math/bigint.h>

/**
 * @brief Helper container for bigint multiplication workflows.
 */
typedef struct ttak_bigmul {
    ttak_bigint_t lhs;
    ttak_bigint_t rhs;
    ttak_bigint_t product;
#ifndef _MSC_VER
    alignas(alignof(ttak_max_align_t)) char padding[0];
#endif
} ttak_bigmul_t;

typedef ttak_bigmul_t tt_big_mul_t;

void ttak_bigmul_init(ttak_bigmul_t *bm, uint64_t now);
void ttak_bigmul_free(ttak_bigmul_t *bm, uint64_t now);

#endif // TTAK_MATH_BIGMUL_H
