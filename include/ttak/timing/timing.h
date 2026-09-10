#ifndef TTAK_TIMING_H
#define TTAK_TIMING_H

#include <stdint.h>
#include <time.h>
#include <ttak/types/ttak_compiler.h>

#if defined(__x86_64__) || defined(_M_X64)
#  if defined(__TINYC__)
     static inline uint64_t __rdtsc(void) {
         uint32_t low, high;
         __asm__ volatile ("rdtsc" : "=a" (low), "=d" (high));
         return ((uint64_t)high << 32) | low;
     }
#  else
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

#if defined(EMBEDDED_BAREMETAL)
/* CPU clock in MHz, used below to convert the DWT cycle counter
 * (0xE0001004, CYCCNT) into real elapsed time. This has to match
 * whatever clock the target actually runs at -- it does not derive
 * that from anywhere itself (a generic engine has no board-specific
 * knowledge of SYSCLK), so each board's build must pass it explicitly
 * (see os/default/Makefile's libttak target, which passes
 * -DTTAK_DWT_MHZ=$(BOARD_CPU_MHZ), with BOARD_CPU_MHZ set in that
 * board's configs/<board> file to match its own hal_sys.c). Get this
 * wrong and the build still succeeds -- ttak_get_tick_count() just
 * silently reports the wrong elapsed time, scaled by the ratio of the
 * wrong clock to the real one. */
#ifndef TTAK_DWT_MHZ
#define TTAK_DWT_MHZ 16ULL
#endif

static inline void ttak_baremetal_dwt_init(void) {
    static int init = 0;
    if (!init) {
        *(volatile uint32_t *)0xE000EDFC |= (1U << 24);
        *(volatile uint32_t *)0xE0001000 |= (1U << 0);
        init = 1;
    }
}
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
        (__rdtsc() * __scale) >> 32; \
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
    return (__rdtsc() * scale) >> 32;
#elif defined(_WIN32)
    extern uint64_t ttak_get_tick_count_ns_win32(void);
    return ttak_get_tick_count_ns_win32();
#elif defined(EMBEDDED_BAREMETAL)
    ttak_baremetal_dwt_init();
    return ((uint64_t)(*(volatile uint32_t *)0xE0001004)) * 1000ULL / TTAK_DWT_MHZ;
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
#elif defined(EMBEDDED_BAREMETAL)
    ttak_baremetal_dwt_init();
    return ((uint64_t)(*(volatile uint32_t *)0xE0001004)) / (TTAK_DWT_MHZ * 1000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
#endif
}
#endif

#endif // TTAK_TIMING_H
