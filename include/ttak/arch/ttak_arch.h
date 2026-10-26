/**
 * @file ttak_arch.h
 * @brief Architecture / ISA / compiler capability detection and inline helpers.
 *
 * Provides a single place for arch-specific intrinsics and inline assembly.
 * TinyCC gets arch-specific inline assembly where available, otherwise it
 * falls back to portable C implementations.
 */

#ifndef TTAK_ARCH_H
#define TTAK_ARCH_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <ttak/compat/baremetal.h>

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Architecture detection
 * ============================================================================ */

#if defined(__x86_64__) || defined(_M_X64)
#  define TTAK_ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define TTAK_ARCH_AARCH64 1
#elif defined(__riscv) && (__riscv_xlen == 64)
#  define TTAK_ARCH_RISCV64 1
#elif defined(__PPC64__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#  define TTAK_ARCH_PPC64LE 1
#elif defined(__mips__) && defined(__LP64__)
#  define TTAK_ARCH_MIPS64 1
#endif

/* ============================================================================
 * Compiler detection
 * ============================================================================ */

#if defined(__TINYC__)
#  define TTAK_COMPILER_TCC 1
#elif defined(__clang__)
#  define TTAK_COMPILER_CLANG 1
#elif defined(__GNUC__)
#  define TTAK_COMPILER_GCC 1
#elif defined(_MSC_VER)
#  define TTAK_COMPILER_MSVC 1
#endif

/* ============================================================================
 * SIMD capability detection (disabled for TCC)
 * ============================================================================ */

#if !defined(TTAK_COMPILER_TCC)
#  if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86) && _M_IX86_FP >= 2)
#    define TTAK_HAS_SSE2 1
#  endif
#  if defined(__AVX2__)
#    define TTAK_HAS_AVX2 1
#  endif
#  if defined(__AVX512F__)
#    define TTAK_HAS_AVX512F 1
#  endif
#  if defined(__ARM_NEON) || defined(__ARM_NEON__)
#    define TTAK_HAS_NEON 1
#  endif
#  if defined(__riscv_vector)
#    define TTAK_HAS_RVV 1
#  endif
#endif

/* ============================================================================
 * Force-inline helper
 * ============================================================================ */

#if defined(__GNUC__) || defined(__clang__)
#  define TTAK_ARCH_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#  define TTAK_ARCH_INLINE static __forceinline
#else
#  define TTAK_ARCH_INLINE static inline
#endif

/* ============================================================================
 * CPU pause / spin-hint
 * ============================================================================ */

TTAK_ARCH_INLINE void ttak_arch_pause(void) {
#if defined(TTAK_ARCH_X86_64)
#  if defined(TTAK_COMPILER_TCC)
    __asm__ volatile ("pause" ::: "memory");
#  elif defined(TTAK_COMPILER_MSVC)
    _mm_pause();
#  else
    __builtin_ia32_pause();
#  endif
#elif defined(TTAK_ARCH_AARCH64)
#  if defined(TTAK_COMPILER_TCC)
    __asm__ volatile ("yield" ::: "memory");
#  else
    __asm__ volatile ("yield" ::: "memory");
#  endif
#elif defined(TTAK_ARCH_RISCV64) && defined(TTAK_COMPILER_TCC)
    /* RISC-V has no standard pause; use a light compiler barrier. */
    __asm__ volatile ("" ::: "memory");
#elif defined(TTAK_ARCH_PPC64LE) && defined(TTAK_COMPILER_TCC)
    /* POWER does not have a portable yield/pause in user space. */
    __asm__ volatile ("" ::: "memory");
#elif defined(TTAK_ARCH_MIPS64) && defined(TTAK_COMPILER_TCC)
    /* MIPS32R2/MIPS64R2 introduced the 'pause' hint instruction. */
    __asm__ volatile ("pause" ::: "memory");
#else
#  if defined(__GNUC__) || defined(__clang__) || defined(TTAK_COMPILER_TCC)
    __asm__ volatile ("" ::: "memory");
#  endif
#endif
}

/* ============================================================================
 * Timestamp counter
 * ============================================================================ */

TTAK_ARCH_INLINE uint64_t ttak_arch_rdtsc(void) {
#if defined(TTAK_ARCH_X86_64)
#  if defined(TTAK_COMPILER_TCC)
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
#  elif defined(TTAK_COMPILER_MSVC)
    return __rdtsc();
#  else
    return __builtin_ia32_rdtsc();
#  endif
#elif defined(TTAK_ARCH_AARCH64) && defined(TTAK_COMPILER_TCC)
    uint64_t cnt;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(cnt));
    return cnt;
#elif defined(TTAK_ARCH_RISCV64) && defined(TTAK_COMPILER_TCC)
    uint64_t time;
    __asm__ volatile ("rdtime %0" : "=r"(time));
    return time;
#elif defined(TTAK_ARCH_PPC64LE) && defined(TTAK_COMPILER_TCC)
    uint64_t tb;
    __asm__ volatile ("mftb %0" : "=r"(tb));
    return tb;
#elif defined(TTAK_ARCH_MIPS64) && defined(TTAK_COMPILER_TCC)
    uint64_t cc;
    __asm__ volatile ("rdhwr %0, $2" : "=r"(cc));
    return cc;
#else
    /* Fallback: monotonic nanoseconds are portable but slower.
     * Implemented locally to avoid a linkage/declaration cycle with
     * ttak_get_tick_count_ns() defined in timing.h. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
#endif
}

/* ============================================================================
 * Count leading zeros
 * ============================================================================ */

TTAK_ARCH_INLINE int ttak_arch_clz32(uint32_t v) {
    if (v == 0) return 32;
#if defined(TTAK_COMPILER_TCC)
#  if defined(TTAK_ARCH_X86_64)
    uint32_t r;
    __asm__ ("bsrl %1, %0" : "=r"(r) : "r"(v));
    return 31 ^ (int)r;
#  elif defined(TTAK_ARCH_AARCH64)
    uint32_t r;
    __asm__ ("clz %w0, %w1" : "=r"(r) : "r"(v));
    return (int)r;
#  elif defined(TTAK_ARCH_PPC64LE)
    uint32_t r;
    __asm__ ("cntlzw %0, %1" : "=r"(r) : "r"(v));
    return (int)r;
#  elif defined(TTAK_ARCH_MIPS64)
    uint32_t r;
    __asm__ ("clz %0, %1" : "=r"(r) : "r"(v));
    return (int)r;
#  else
    int r = 0;
    for (int i = 31; i >= 0; i--) {
        if (v & (1U << i)) break;
        r++;
    }
    return r;
#  endif
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_clz(v);
#elif defined(_MSC_VER)
    unsigned long idx;
    _BitScanReverse(&idx, (unsigned long)v);
    return 31 - (int)idx;
#else
    int r = 0;
    for (int i = 31; i >= 0; i--) {
        if (v & (1U << i)) break;
        r++;
    }
    return r;
#endif
}

TTAK_ARCH_INLINE int ttak_arch_clz64(uint64_t v) {
    if (v == 0) return 64;
#if defined(TTAK_COMPILER_TCC)
#  if defined(TTAK_ARCH_X86_64)
    uint64_t r;
    __asm__ ("bsrq %1, %0" : "=r"(r) : "r"(v));
    return 63 ^ (int)r;
#  elif defined(TTAK_ARCH_AARCH64)
    uint64_t r;
    __asm__ ("clz %x0, %x1" : "=r"(r) : "r"(v));
    return (int)r;
#  elif defined(TTAK_ARCH_PPC64LE)
    uint64_t r;
    __asm__ ("cntlzd %0, %1" : "=r"(r) : "r"(v));
    return (int)r;
#  elif defined(TTAK_ARCH_MIPS64)
    uint64_t r;
    __asm__ ("dclz %0, %1" : "=r"(r) : "r"(v));
    return (int)r;
#  else
    int r = 0;
    for (int i = 63; i >= 0; i--) {
        if (v & (1ULL << i)) break;
        r++;
    }
    return r;
#  endif
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(v);
#elif defined(_MSC_VER)
    unsigned long idx;
#  if defined(_WIN64)
    _BitScanReverse64(&idx, v);
    return 63 - (int)idx;
#  else
    if (_BitScanReverse(&idx, (unsigned long)(v >> 32))) {
        return 31 - (int)idx;
    }
    _BitScanReverse(&idx, (unsigned long)v);
    return 63 - (int)idx;
#  endif
#else
    int r = 0;
    for (int i = 63; i >= 0; i--) {
        if (v & (1ULL << i)) break;
        r++;
    }
    return r;
#endif
}

/* ============================================================================
 * Count trailing zeros
 * ============================================================================ */

TTAK_ARCH_INLINE int ttak_arch_ctz32(uint32_t v) {
    if (v == 0) return 32;
#if defined(TTAK_COMPILER_TCC)
#  if defined(TTAK_ARCH_X86_64)
    uint32_t r;
    __asm__ ("bsfl %1, %0" : "=r"(r) : "r"(v));
    return (int)r;
#  elif defined(TTAK_ARCH_AARCH64)
    uint32_t r;
    __asm__ ("rbit %w0, %w1\n\t"
             "clz %w0, %w0" : "=r"(r) : "r"(v));
    return (int)r;
#  elif defined(TTAK_ARCH_PPC64LE)
    uint32_t r;
    __asm__ ("cnttzw %0, %1" : "=r"(r) : "r"(v));
    return (int)r;
#  else
    int r = 0;
    while (!(v & 1U)) { v >>= 1; r++; }
    return r;
#  endif
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_ctz(v);
#elif defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward(&idx, (unsigned long)v);
    return (int)idx;
#else
    int r = 0;
    while (!(v & 1U)) { v >>= 1; r++; }
    return r;
#endif
}

TTAK_ARCH_INLINE int ttak_arch_ctz64(uint64_t v) {
    if (v == 0) return 64;
#if defined(TTAK_COMPILER_TCC)
#  if defined(TTAK_ARCH_X86_64)
    uint64_t r;
    __asm__ ("bsfq %1, %0" : "=r"(r) : "r"(v));
    return (int)r;
#  elif defined(TTAK_ARCH_AARCH64)
    uint64_t r;
    __asm__ ("rbit %x0, %x1\n\t"
             "clz %x0, %x0" : "=r"(r) : "r"(v));
    return (int)r;
#  elif defined(TTAK_ARCH_PPC64LE)
    uint64_t r;
    __asm__ ("cnttzd %0, %1" : "=r"(r) : "r"(v));
    return (int)r;
#  else
    int r = 0;
    while (!(v & 1ULL)) { v >>= 1; r++; }
    return r;
#  endif
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(v);
#elif defined(_MSC_VER)
    unsigned long idx;
#  if defined(_WIN64)
    _BitScanForward64(&idx, v);
    return (int)idx;
#  else
    if (_BitScanForward(&idx, (unsigned long)v)) {
        return (int)idx;
    }
    _BitScanForward(&idx, (unsigned long)(v >> 32));
    return 32 + (int)idx;
#  endif
#else
    int r = 0;
    while (!(v & 1ULL)) { v >>= 1; r++; }
    return r;
#endif
}

/* ============================================================================
 * Byte swap
 * ============================================================================ */

TTAK_ARCH_INLINE uint32_t ttak_arch_bswap32(uint32_t v) {
#if defined(TTAK_COMPILER_TCC)
#  if defined(TTAK_ARCH_X86_64)
    __asm__ ("bswapl %0" : "=r"(v) : "0"(v));
    return v;
#  elif defined(TTAK_ARCH_AARCH64)
    uint32_t r;
    __asm__ ("rev %w0, %w1" : "=r"(r) : "r"(v));
    return r;
#  elif defined(TTAK_ARCH_PPC64LE)
    uint32_t r;
    __asm__ ("brw %0, %1" : "=r"(r) : "r"(v));
    return r;
#  else
    return ((v & 0xFF000000U) >> 24) |
           ((v & 0x00FF0000U) >>  8) |
           ((v & 0x0000FF00U) <<  8) |
           ((v & 0x000000FFU) << 24);
#  endif
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(v);
#elif defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return ((v & 0xFF000000U) >> 24) |
           ((v & 0x00FF0000U) >>  8) |
           ((v & 0x0000FF00U) <<  8) |
           ((v & 0x000000FFU) << 24);
#endif
}

TTAK_ARCH_INLINE uint64_t ttak_arch_bswap64(uint64_t v) {
#if defined(TTAK_COMPILER_TCC)
#  if defined(TTAK_ARCH_X86_64)
    __asm__ ("bswapq %0" : "=r"(v) : "0"(v));
    return v;
#  elif defined(TTAK_ARCH_AARCH64)
    uint64_t r;
    __asm__ ("rev %x0, %x1" : "=r"(r) : "r"(v));
    return r;
#  elif defined(TTAK_ARCH_PPC64LE)
    uint64_t r;
    __asm__ ("brd %0, %1" : "=r"(r) : "r"(v));
    return r;
#  else
    return ((v & 0xFF00000000000000ULL) >> 56) |
           ((v & 0x00FF000000000000ULL) >> 40) |
           ((v & 0x0000FF0000000000ULL) >> 24) |
           ((v & 0x000000FF00000000ULL) >>  8) |
           ((v & 0x00000000FF000000ULL) <<  8) |
           ((v & 0x0000000000FF0000ULL) << 24) |
           ((v & 0x000000000000FF00ULL) << 40) |
           ((v & 0x00000000000000FFULL) << 56);
#  endif
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(v);
#elif defined(_MSC_VER)
    return _byteswap_uint64(v);
#else
    return ((v & 0xFF00000000000000ULL) >> 56) |
           ((v & 0x00FF000000000000ULL) >> 40) |
           ((v & 0x0000FF0000000000ULL) >> 24) |
           ((v & 0x000000FF00000000ULL) >>  8) |
           ((v & 0x00000000FF000000ULL) <<  8) |
           ((v & 0x0000000000FF0000ULL) << 24) |
           ((v & 0x000000000000FF00ULL) << 40) |
           ((v & 0x00000000000000FFULL) << 56);
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* TTAK_ARCH_H */
