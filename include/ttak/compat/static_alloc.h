/**
 * @file static_alloc.h
 * @brief Shared helpers for zero-allocation (static storage) initialization.
 *
 * These macros back the TTAK_<MODULE>_STATIC_STORAGE_SIZE sizing helpers and
 * the ttak_<module>_init_static() family of functions. They are pure
 * compile-time constant expressions and require no runtime support, so they
 * are usable in bare-metal and safety-critical profiles where the heap is
 * prohibited (see include/ttak/compat/no_malloc.h).
 */

#ifndef TTAK_COMPAT_STATIC_ALLOC_H
#define TTAK_COMPAT_STATIC_ALLOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Alignment applied to every region carved out of caller storage. */
#define TTAK_STATIC_STORAGE_ALIGN ((size_t)16U)

/** @brief Round @p v up to a multiple of @p a (both must be positive). */
#define TTAK_STATIC_ALIGN_UP(v, a) \
    (((size_t)(v) + (size_t)(a) - 1U) & ~((size_t)(a) - 1U))

/* Shift-or fill helpers for TTAK_STATIC_POW2. */
#define TTAK_STATIC_POW2_FILL1(x)  ((x) | ((x) >> 1))
#define TTAK_STATIC_POW2_FILL2(x)  (TTAK_STATIC_POW2_FILL1(x) | (TTAK_STATIC_POW2_FILL1(x) >> 2))
#define TTAK_STATIC_POW2_FILL4(x)  (TTAK_STATIC_POW2_FILL2(x) | (TTAK_STATIC_POW2_FILL2(x) >> 4))
#define TTAK_STATIC_POW2_FILL8(x)  (TTAK_STATIC_POW2_FILL4(x) | (TTAK_STATIC_POW2_FILL4(x) >> 8))
#define TTAK_STATIC_POW2_FILL16(x) (TTAK_STATIC_POW2_FILL8(x) | (TTAK_STATIC_POW2_FILL8(x) >> 16))
#define TTAK_STATIC_POW2_FILL32(x) \
    ((SIZE_MAX > 0xFFFFFFFFULL) \
        ? (TTAK_STATIC_POW2_FILL16(x) | (TTAK_STATIC_POW2_FILL16(x) >> 32)) \
        : TTAK_STATIC_POW2_FILL16(x))

/**
 * @brief Round @p n up to the next power of two (0 and 1 map to 1).
 *
 * Mirrors the runtime next_pow2()/next_power_of_two() helpers used by the
 * dynamic constructors so sizing macros and init functions agree.
 */
#define TTAK_STATIC_POW2(n) \
    (((size_t)(n) <= 1U) ? 1U : (TTAK_STATIC_POW2_FILL32((size_t)(n) - 1U) + 1U))

#ifdef __cplusplus
}
#endif

#endif /* TTAK_COMPAT_STATIC_ALLOC_H */
