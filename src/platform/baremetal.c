/**
 * @file baremetal.c
 * @brief Weak clock/scheduler shims for OS-less targets.
 *
 * See <ttak/compat/baremetal.h>. Every symbol here is weak so that the
 * application (or a board support package) can replace it.
 */

#include <ttak/compat/baremetal.h>

#if TTAK_PLATFORM_BAREMETAL

#include <errno.h>
#include <stdbool.h>

#if defined(__ARM_ARCH_PROFILE) && __ARM_ARCH_PROFILE == 'M'
#define TTAK_BM_ARM_M 1
#else
#define TTAK_BM_ARM_M 0
#endif

#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || \
    defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__)
#define TTAK_BM_HAVE_DWT 1
#else
#define TTAK_BM_HAVE_DWT 0
#endif

#define TTAK_WEAK __attribute__((weak))

#if TTAK_BM_ARM_M
/* Mask interrupts on a single-core MCU; nests by restoring the old state. */
static inline uint32_t ttak_bm_irq_save(void) {
    uint32_t primask;
    __asm__ volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask) :: "memory");
    return primask;
}

static inline void ttak_bm_irq_restore(uint32_t primask) {
    __asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");
}
#endif

TTAK_WEAK uint32_t ttak_platform_cpu_hz(void) {
    return 16000000u;
}

#if TTAK_BM_HAVE_DWT

#define TTAK_BM_DEMCR      (*(volatile uint32_t *)0xE000EDFCu)
#define TTAK_BM_DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)
#define TTAK_BM_DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define TTAK_BM_DWT_LAR    (*(volatile uint32_t *)0xE0001FB0u)

static uint64_t g_cycles_hi;
static uint32_t g_cycles_last;
static int g_dwt_ready;

/* CYCCNT is 32 bits wide and wraps every ~20 s at 216 MHz. Each call
 * extends it to 64 bits, so callers must sample at least once per wrap
 * period; long-running idle loops should override ttak_platform_now_ns. */
static uint64_t ttak_bm_cycles(void) {
    uint32_t key = ttak_bm_irq_save();
    if (!g_dwt_ready) {
        TTAK_BM_DEMCR |= (1u << 24);          /* TRCENA */
        TTAK_BM_DWT_LAR = 0xC5ACCE55u;        /* unlock (Cortex-M7) */
        TTAK_BM_DWT_CYCCNT = 0;
        TTAK_BM_DWT_CTRL |= 1u;               /* CYCCNTENA */
        g_dwt_ready = 1;
    }
    uint32_t now = TTAK_BM_DWT_CYCCNT;
    if (now < g_cycles_last) g_cycles_hi += (1ull << 32);
    g_cycles_last = now;
    uint64_t total = g_cycles_hi | now;
    ttak_bm_irq_restore(key);
    return total;
}

TTAK_WEAK uint64_t ttak_platform_now_ns(void) {
    uint64_t cycles = ttak_bm_cycles();
    uint64_t hz = ttak_platform_cpu_hz();
    if (hz == 0) hz = 16000000u;
    /* Split to keep cycles * 1e9 from overflowing 64 bits. */
    return (cycles / hz) * 1000000000ull +
           ((cycles % hz) * 1000000000ull) / hz;
}

#else /* !TTAK_BM_HAVE_DWT */

/* No cycle counter: advance 1 us per call so timeouts still expire.
 * Real applications are expected to override this. */
TTAK_WEAK uint64_t ttak_platform_now_ns(void) {
    static uint64_t fake_ns;
    fake_ns += 1000u;
    return fake_ns;
}

#endif /* TTAK_BM_HAVE_DWT */

TTAK_WEAK int clock_gettime(clockid_t clock_id, struct timespec *tp) {
    (void)clock_id;
    if (!tp) {
        errno = EINVAL;
        return -1;
    }
    uint64_t ns = ttak_platform_now_ns();
    tp->tv_sec = (time_t)(ns / 1000000000ull);
    tp->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}

TTAK_WEAK int nanosleep(const struct timespec *rqtp, struct timespec *rmtp) {
    if (!rqtp || rqtp->tv_nsec < 0 || rqtp->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    uint64_t wait = (uint64_t)rqtp->tv_sec * 1000000000ull + (uint64_t)rqtp->tv_nsec;
    uint64_t start = ttak_platform_now_ns();
    while (ttak_platform_now_ns() - start < wait) {
    }
    if (rmtp) {
        rmtp->tv_sec = 0;
        rmtp->tv_nsec = 0;
    }
    return 0;
}

TTAK_WEAK int usleep(useconds_t usec) {
    struct timespec ts = { (time_t)(usec / 1000000u), (long)(usec % 1000000u) * 1000L };
    return nanosleep(&ts, NULL);
}

TTAK_WEAK int sched_yield(void) {
    return 0;
}

#if TTAK_BM_ARM_M
/* --------------------------------------------------------------------------
 * libatomic replacements
 *
 * M-profile cores have no 64-bit exclusives (and ARMv6-M has none at all),
 * so GCC lowers those C11 atomics to __atomic_* library calls. Toolchains
 * for bare metal ship no libatomic; these weak versions serialise through
 * a PRIMASK critical section, which is atomic on a single-core MCU against
 * both other code and interrupt handlers. The asm labels keep the C names
 * distinct from GCC's built-in prototypes. The compare-exchange libcall
 * takes no 'weak' argument: GCC drops it when it falls back to a call.
 * ------------------------------------------------------------------------ */

#define TTAK_BM_ATOMIC_FETCH_OP(N, T, NAME, EXPR)                                  \
    TTAK_WEAK T ttak_bm_atomic_fetch_##NAME##_##N(volatile void *mem, T val,       \
                                                  int model)                       \
        __asm__("__atomic_fetch_" #NAME "_" #N);                                   \
    TTAK_WEAK T ttak_bm_atomic_fetch_##NAME##_##N(volatile void *mem, T val,       \
                                                  int model) {                     \
        (void)model;                                                               \
        volatile T *p = (volatile T *)mem;                                         \
        uint32_t key = ttak_bm_irq_save();                                         \
        T old = *p;                                                                \
        *p = (T)(EXPR);                                                            \
        ttak_bm_irq_restore(key);                                                  \
        return old;                                                                \
    }                                                                              \
    TTAK_WEAK T ttak_bm_atomic_##NAME##_fetch_##N(volatile void *mem, T val,       \
                                                  int model)                       \
        __asm__("__atomic_" #NAME "_fetch_" #N);                                   \
    TTAK_WEAK T ttak_bm_atomic_##NAME##_fetch_##N(volatile void *mem, T val,       \
                                                  int model) {                     \
        (void)model;                                                               \
        volatile T *p = (volatile T *)mem;                                         \
        uint32_t key = ttak_bm_irq_save();                                         \
        T old = *p;                                                                \
        T now = (T)(EXPR);                                                         \
        *p = now;                                                                  \
        ttak_bm_irq_restore(key);                                                  \
        return now;                                                                \
    }

#define TTAK_BM_ATOMIC_SIZE(N, T)                                                  \
    TTAK_WEAK T ttak_bm_atomic_load_##N(const volatile void *mem, int model)      \
        __asm__("__atomic_load_" #N);                                              \
    TTAK_WEAK T ttak_bm_atomic_load_##N(const volatile void *mem, int model) {    \
        (void)model;                                                               \
        uint32_t key = ttak_bm_irq_save();                                         \
        T val = *(const volatile T *)mem;                                          \
        ttak_bm_irq_restore(key);                                                  \
        return val;                                                                \
    }                                                                              \
    TTAK_WEAK void ttak_bm_atomic_store_##N(volatile void *mem, T val, int model) \
        __asm__("__atomic_store_" #N);                                             \
    TTAK_WEAK void ttak_bm_atomic_store_##N(volatile void *mem, T val, int model) {\
        (void)model;                                                               \
        uint32_t key = ttak_bm_irq_save();                                         \
        *(volatile T *)mem = val;                                                  \
        ttak_bm_irq_restore(key);                                                  \
    }                                                                              \
    TTAK_WEAK T ttak_bm_atomic_exchange_##N(volatile void *mem, T val, int model) \
        __asm__("__atomic_exchange_" #N);                                          \
    TTAK_WEAK T ttak_bm_atomic_exchange_##N(volatile void *mem, T val, int model) {\
        (void)model;                                                               \
        volatile T *p = (volatile T *)mem;                                         \
        uint32_t key = ttak_bm_irq_save();                                         \
        T old = *p;                                                                \
        *p = val;                                                                  \
        ttak_bm_irq_restore(key);                                                  \
        return old;                                                                \
    }                                                                              \
    TTAK_WEAK bool ttak_bm_atomic_compare_exchange_##N(volatile void *mem,         \
                                                       void *expected, T desired,  \
                                                       int success, int failure)   \
        __asm__("__atomic_compare_exchange_" #N);                                  \
    TTAK_WEAK bool ttak_bm_atomic_compare_exchange_##N(volatile void *mem,         \
                                                       void *expected, T desired,  \
                                                       int success, int failure) { \
        (void)success;                                                             \
        (void)failure;                                                             \
        volatile T *p = (volatile T *)mem;                                         \
        T *e = (T *)expected;                                                      \
        uint32_t key = ttak_bm_irq_save();                                         \
        T cur = *p;                                                                \
        bool ok = (cur == *e);                                                     \
        if (ok) *p = desired;                                                      \
        ttak_bm_irq_restore(key);                                                  \
        if (!ok) *e = cur;                                                         \
        return ok;                                                                 \
    }                                                                              \
    TTAK_BM_ATOMIC_FETCH_OP(N, T, add, old + val)                                  \
    TTAK_BM_ATOMIC_FETCH_OP(N, T, sub, old - val)                                  \
    TTAK_BM_ATOMIC_FETCH_OP(N, T, and, old & val)                                  \
    TTAK_BM_ATOMIC_FETCH_OP(N, T, or, old | val)                                   \
    TTAK_BM_ATOMIC_FETCH_OP(N, T, xor, old ^ val)                                  \
    TTAK_BM_ATOMIC_FETCH_OP(N, T, nand, ~(old & val))

TTAK_BM_ATOMIC_SIZE(8, uint64_t)

#if defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_8M_BASE__)
/* No LDREX/STREX for narrower types either on these baseline cores. */
TTAK_BM_ATOMIC_SIZE(1, uint8_t)
TTAK_BM_ATOMIC_SIZE(2, uint16_t)
TTAK_BM_ATOMIC_SIZE(4, uint32_t)
#endif

#endif /* TTAK_BM_ARM_M */

#endif /* TTAK_PLATFORM_BAREMETAL */
