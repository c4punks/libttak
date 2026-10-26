#ifndef TTAK_INTERNAL_APP_TYPES_H
#define TTAK_INTERNAL_APP_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdalign.h>
#include <ttak/types/ttak_compiler.h>
#include <ttak/compat/pthread.h>
#include <ttak/mem/mem.h>

/**
 * @brief Safety limit for mathematical operations to prevent OOM/Overflow.
 * 16 million limbs (approx. 64MB per bigint).
 */
#define TTAK_MAX_LIMB_LIMIT 0x1000000 

/**
 * @brief High Watermark for memory pressure backpressure (512MB).
 */
#define TTAK_MEM_HIGH_WATERMARK (512ULL * 1024ULL * 1024ULL)

/**
 * @brief Internal error codes mapping.
 */
#define ERR_TTAK_MATH_ERR   -206
#define ERR_TTAK_INV_ACC    -205

#if defined(__GNUC__) || defined(__clang__)
#define TTAK_HOT_PATH __attribute__((hot))
#define TTAK_COLD_PATH __attribute__((cold))
#else
#define TTAK_HOT_PATH
#define TTAK_COLD_PATH
#endif

#ifndef TTAK_THREAD_LOCAL
#if TTAK_NO_NATIVE_TLS
#define TTAK_THREAD_LOCAL
#else
#define TTAK_THREAD_LOCAL _Thread_local
#endif
#endif

/**
 * @brief Time unit macros for converting to nanoseconds.
 */
#define TT_NANO_SECOND(n)   ((uint64_t)(n))
#define TT_MICRO_SECOND(n)  ((uint64_t)(n) * 1000ULL)
#define TT_MILLI_SECOND(n)  ((uint64_t)(n) * 1000ULL * 1000ULL)
#define TT_SECOND(n)        ((uint64_t)(n) * 1000ULL * 1000ULL * 1000ULL)
#define TT_MINUTE(n)        ((uint64_t)(n) * 60ULL * 1000ULL * 1000ULL * 1000ULL)
#define TT_HOUR(n)          ((uint64_t)(n) * 60ULL * 60ULL * 1000ULL * 1000ULL * 1000ULL)

/**
 * @brief Sentinel for invalidated references.
 */
#define SAFE_NULL NULL

/**
 * @brief Recommended pattern for resource-managing structures:
 * All structures that manage dynamic resources (e.g., memory, threads, file handles)
 * should follow an _init and _destroy pattern.
 * - `void ttak_struct_init(ttak_struct_t *s, ...)`: Initializes the structure,
 *   allocating necessary resources.
 * - `void ttak_struct_destroy(ttak_struct_t *s, ...)`: Frees all resources
 *   held by the structure.
 * This ensures strict lifetime management and proper cleanup.
 */

#endif // TTAK_INTERNAL_APP_TYPES_H
