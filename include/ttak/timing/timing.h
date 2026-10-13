#ifndef TTAK_TIMING_H
#define TTAK_TIMING_H

#include <stdint.h>
#include <time.h>
#include <ttak/types/ttak_compiler.h>
#include <ttak/arch/ttak_arch.h>

#if defined(__x86_64__) || defined(_M_X64)
#  if !defined(__TINYC__)
#    if defined(_MSC_VER)
#      include <immintrin.h>
#    else
#      include <x86intrin.h>
#    endif
#  endif
   extern uint64_t g_tsc_freq_ghz;
   extern uint64_t g_tsc_scale;
   void calibrate_tsc(void);
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

#if defined(__TINYC__)
#  if defined(__x86_64__) || defined(_M_X64)
#    define ttak_get_tick_count_ns() ({ \
        uint64_t __scale = g_tsc_scale; \
        if (TTAK_UNLIKELY(__scale == 0)) { calibrate_tsc(); __scale = g_tsc_scale; if (__scale == 0) __scale = (1ULL << 32) / 2; } \
        uint64_t __tsc = ttak_arch_rdtsc(); \
        ((__tsc >> 32) * __scale + (((__tsc & 0xFFFFFFFFULL) * __scale) >> 32)); \
    })

#    define ttak_get_tick_count() (ttak_get_tick_count_ns() / 1000000ULL)
#  elif defined(_WIN32)
extern uint64_t ttak_get_tick_count_ns_win32(void);
static inline uint64_t ttak_get_tick_count_ns(void) { return ttak_get_tick_count_ns_win32(); }
static inline uint64_t ttak_get_tick_count(void) { return ttak_get_tick_count_ns() / 1000000ULL; }
#  else
static inline uint64_t ttak_get_tick_count_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static inline uint64_t ttak_get_tick_count(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}
#  endif
#else
/**
 * @brief Returns the current tick count in nanoseconds.
 */
TTAK_FORCE_INLINE uint64_t ttak_get_tick_count_ns(void) {
#if defined(__x86_64__) || defined(_M_X64)
    uint64_t scale = g_tsc_scale;
    if (TTAK_UNLIKELY(scale == 0)) {
        calibrate_tsc();
        scale = g_tsc_scale;
        if (scale == 0) scale = (1ULL << 32) / 2; 
    }
    uint64_t tsc = ttak_arch_rdtsc();
    return (tsc >> 32) * scale + (((tsc & 0xFFFFFFFFULL) * scale) >> 32);
#elif defined(_WIN32)
    extern uint64_t ttak_get_tick_count_ns_win32(void);
    return ttak_get_tick_count_ns_win32();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
#endif
}

/**
 * @brief Returns the current tick count in milliseconds.
 */
TTAK_FORCE_INLINE uint64_t ttak_get_tick_count(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return ttak_get_tick_count_ns() / 1000000ULL;
#elif defined(_WIN32)
    return ttak_get_tick_count_ns() / 1000000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
#endif
}
#endif

#endif // TTAK_TIMING_H
