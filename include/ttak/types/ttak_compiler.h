/**
 * @file ttak_compiler.h
 * @brief Compiler-specific macros and compatibility layer.
 */

#ifndef TTAK_COMPILER_H
#define TTAK_COMPILER_H

/**
 * @brief Low-level CAS implementation for TinyCC.
 *
 * Injects raw assembly based on the target architecture to prevent undefined
 * symbol errors (__builtin_cas) and ensure atomic integrity.
 */
#if defined(__TINYC__)
#if defined(__x86_64__) || defined(__i386__)
#  define __tcc_arch_cas(ptr, old, new) ({ \
     __typeof__(*(ptr)) __ret; \
     __asm__ __volatile__ ( \
       "lock; cmpxchg %2, %1" \
       : "=a"(__ret), "+m"(*(ptr)) \
       : "r"(new), "a"(old) \
       : "memory" \
     ); \
     __ret; \
   })
#  define __tcc_arch_xchg(ptr, val) ({ \
     __typeof__(*(ptr)) __ret; \
     __asm__ __volatile__ ( \
       "xchg %0, %1" \
       : "=r"(__ret), "+m"(*(ptr)) \
       : "0"(val) \
       : "memory" \
     ); \
     __ret; \
   })
#elif defined(__aarch64__)
#  define __tcc_arch_cas(ptr, old, new) ({ \
     __typeof__(*(ptr)) __oldval; int __res; \
     __asm__ __volatile__ ( \
       "1: ldaxr %0, [%2]\n" \
       "   cmp %0, %3\n" \
       "   b.ne 2f\n" \
       "   stlxr %w1, %4, [%2]\n" \
       "   cbnz %w1, 1b\n" \
       "2:" \
       : "=&r"(__oldval), "=&r"(__res) \
       : "r"(ptr), "r"(old), "r"(new) \
       : "memory" \
     ); \
     __oldval; \
   })
#  define __tcc_arch_xchg(ptr, val) ({ \
     __typeof__(*(ptr)) __oldval; int __res; \
     __asm__ __volatile__ ( \
       "1: ldaxr %0, [%2]\n" \
       "   stlxr %w1, %3, [%2]\n" \
       "   cbnz %w1, 1b\n" \
       : "=&r"(__oldval), "=&r"(__res) \
       : "r"(ptr), "r"(val) \
       : "memory" \
     ); \
     __oldval; \
   })
#elif defined(__mips__) && defined(__LP64__)
#  define __tcc_arch_cas(ptr, old, new) ({ \
     __typeof__(*(ptr)) __ret, __tmp; \
     __asm__ __volatile__ ( \
       ".set push\n.set mips3\n" \
       "1: lld %0, %1\n" \
       "   bne %0, %3, 2f\n" \
       "   move %2, %4\n" \
       "   scd %2, %1\n" \
       "   beqz %2, 1b\n" \
       "2:\n.set pop" \
       : "=&r"(__ret), "+m"(*(ptr)), "=&r"(__tmp) \
       : "r"(old), "r"(new) \
       : "memory" \
     ); \
     __ret; \
   })
#  define __tcc_arch_xchg(ptr, val) ({ \
     __typeof__(*(ptr)) __ret, __tmp; \
     __asm__ __volatile__ ( \
       ".set push\n.set mips3\n" \
       "1: lld %0, %1\n" \
       "   move %2, %3\n" \
       "   scd %2, %1\n" \
       "   beqz %2, 1b\n" \
       ".set pop" \
       : "=&r"(__ret), "+m"(*(ptr)), "=&r"(__tmp) \
       : "r"(val) \
       : "memory" \
     ); \
     __ret; \
   })
#elif defined(__PPC64__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#  define __tcc_arch_cas(ptr, old, new) ({ \
     __typeof__(*(ptr)) __ret; \
     __asm__ __volatile__ ( \
       "1: ldarx %0, 0, %2\n" \
       "   cmpd %0, %3\n" \
       "   bne 2f\n" \
       "   stdcx. %4, 0, %2\n" \
       "   bne 1b\n" \
       "2:" \
       : "=&r"(__ret), "=m"(*(ptr)) \
       : "r"(ptr), "r"(old), "r"(new) \
       : "cr0", "memory" \
     ); \
     __ret; \
   })
#  define __tcc_arch_xchg(ptr, val) ({ \
     __typeof__(*(ptr)) __ret; \
     __asm__ __volatile__ ( \
       "1: ldarx %0, 0, %2\n" \
       "   stdcx. %3, 0, %2\n" \
       "   bne 1b" \
       : "=&r"(__ret), "=m"(*(ptr)) \
       : "r"(ptr), "r"(val) \
       : "cr0", "memory" \
     ); \
     __ret; \
   })
#elif defined(__riscv) && (__riscv_xlen == 64)
#  define __tcc_arch_cas(ptr, old, new) ({ \
     __typeof__(*(ptr)) __ret; int __tmp; \
     __asm__ __volatile__ ( \
       "1: lr.d.aqrl %0, (%2)\n" \
       "   bne %0, %3, 2f\n" \
       "   sc.d.aqrl %1, %4, (%2)\n" \
       "   bnez %1, 1b\n" \
       "2:" \
       : "=&r"(__ret), "=&r"(__tmp) \
       : "r"(ptr), "r"(old), "r"(new) \
       : "memory" \
     ); \
     __ret; \
   })
#  define __tcc_arch_xchg(ptr, val) ({ \
     __typeof__(*(ptr)) __ret; \
     __asm__ __volatile__ ( \
       "amoswap.d.aqrl %0, %2, (%1)" \
       : "=&r"(__ret) \
       : "r"(ptr), "r"(val) \
       : "memory" \
     ); \
     __ret; \
   })
#else
#  error "TTAK: Target architecture not supported by TinyCC atomic shim."
#endif /* arch */
#endif /* __TINYC__ */

/* Intercept standard atomic symbols to prevent linker errors */
#if defined(__TINYC__)
#undef __sync_val_compare_and_swap
#define __sync_val_compare_and_swap(ptr, old, new) __tcc_arch_cas(ptr, old, new)

#undef __sync_bool_compare_and_swap
#define __sync_bool_compare_and_swap(ptr, old, new) (__tcc_arch_cas(ptr, old, new) == (old))

#undef __atomic_compare_exchange_n
#define __atomic_compare_exchange_n(ptr, p_old, new, weak, success, fail) ({ \
  __typeof__(*(ptr)) __exp = *(p_old); \
  __typeof__(*(ptr)) __act = __tcc_arch_cas(ptr, __exp, new); \
  bool __ok = (__act == __exp); \
  if (!__ok) *(p_old) = __act; \
  __ok; \
})

#undef __atomic_exchange_n
#define __atomic_exchange_n(ptr, val, mem) __tcc_arch_xchg(ptr, val)

#undef __sync_lock_test_and_set
#define __sync_lock_test_and_set(ptr, val) __tcc_arch_xchg(ptr, val)
#endif /* __TINYC__ intercept */

/** @brief 1 if TinyCC is targeting a non-x86 platform and needs portable fallbacks. */
#if !defined(TTAK_TINYCC_NEEDS_PORTABLE_FALLBACK)
#  if defined(__TINYC__) && !defined(__x86_64__) && !defined(__i386__)
#    define TTAK_TINYCC_NEEDS_PORTABLE_FALLBACK 1
#  else
#    define TTAK_TINYCC_NEEDS_PORTABLE_FALLBACK 0
#  endif
#endif

/** @brief Branch prediction hints for GCC/Clang; no-op on MSVC. */
#if defined(__GNUC__) || defined(__clang__)
#  define TTAK_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define TTAK_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define TTAK_LIKELY(x)   (x)
#  define TTAK_UNLIKELY(x) (x)
#endif

/** @brief Forces inlining on GCC/Clang/MSVC; falls back to plain inline. */
#if defined(__GNUC__) || defined(__clang__)
#  define TTAK_FORCE_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#  define TTAK_FORCE_INLINE static __forceinline
#else
#  define TTAK_FORCE_INLINE static inline
#endif

/** @brief Suppresses unused-variable warnings portably. */
#if defined(__GNUC__) || defined(__clang__)
#  define TTAK_MAYBE_UNUSED __attribute__((unused))
#else
#  define TTAK_MAYBE_UNUSED
#endif

#if !defined(__GNUC__) && !defined(__clang__)
static inline int __ttak_ctzll(unsigned long long v) {
    int r = 0;
    if (!v) return 64;
    while (!(v & 1)) { v >>= 1; r++; }
    return r;
}
static inline int __ttak_clzll(unsigned long long v) {
    int r = 0;
    if (!v) return 64;
    for (int i = 63; i >= 0; i--) {
        if (v & (1ULL << i)) break;
        r++;
    }
    return r;
}
#define __builtin_ctzll(x) __ttak_ctzll(x)
#define __builtin_clzll(x) __ttak_clzll(x)
#endif

#if defined(__TINYC__) && defined(__x86_64__)
/**
 * Optimized atomic operations for TinyC Compiler on x86_64.
 *
 * Provides inline assembly implementations to bypass standard library
 * overhead for common atomic patterns.
 */
#define TTAK_FAST_ATOMIC_ADD_U64(ptr, val) \
    __extension__ ({ \
        uint64_t __v = (val); \
        __asm__ volatile ("lock; xaddq %0, %1" : "+r"(__v), "+m"(*(volatile uint64_t *)(ptr)) : : "memory", "cc"); \
        __v; \
    })

#define TTAK_FAST_ATOMIC_ADD_U32(ptr, val) \
    __extension__ ({ \
        uint32_t __v = (val); \
        __asm__ volatile ("lock; xaddl %0, %1" : "+r"(__v), "+m"(*(volatile uint32_t *)(ptr)) : : "memory", "cc"); \
        __v; \
    })

#define TTAK_FAST_ATOMIC_LOAD_U64(ptr) \
    __extension__ ({ \
        uint64_t __v; \
        __asm__ volatile ("movq %1, %0" : "=r"(__v) : "m"(*(volatile uint64_t *)(ptr)) : "memory"); \
        __v; \
    })

#define TTAK_FAST_ATOMIC_STORE_U32(ptr, val) \
    __extension__ ({ \
        uint32_t __v = (val); \
        __asm__ volatile ("movl %1, %0" : "=m"(*(volatile uint32_t *)(ptr)) : "r"(__v) : "memory"); \
    })

#define TTAK_FAST_ATOMIC_STORE_BOOL(ptr, val) \
    __extension__ ({ \
        uint8_t __v = (val); \
        __asm__ volatile ("movb %1, %0" : "=m"(*(volatile uint8_t *)(ptr)) : "r"(__v) : "memory"); \
    })

#define TTAK_FAST_ATOMIC_XCHG_U64(ptr, val) \
    __extension__ ({ \
        uint64_t __v = (val); \
        __asm__ volatile ("lock; xchgq %0, %1" : "+r"(__v), "+m"(*(volatile uint64_t *)(ptr)) : : "memory"); \
        __v; \
    })

#elif defined(__TINYC__)
#include <stdatomic.h>
#define TTAK_FAST_ATOMIC_ADD_U64(ptr, val) \
    atomic_fetch_add_explicit((_Atomic uint64_t *)(ptr), (uint64_t)(val), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_ADD_U32(ptr, val) \
    atomic_fetch_add_explicit((_Atomic uint32_t *)(ptr), (uint32_t)(val), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_LOAD_U64(ptr) \
    atomic_load_explicit((_Atomic uint64_t *)(ptr), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_STORE_U32(ptr, val) \
    atomic_store_explicit((_Atomic uint32_t *)(ptr), (uint32_t)(val), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_STORE_BOOL(ptr, val) \
    atomic_store_explicit((_Atomic uint64_t *)(ptr), (uint64_t)((val) ? 1 : 0), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_XCHG_U64(ptr, val) \
    atomic_exchange_explicit((_Atomic uint64_t *)(ptr), (uint64_t)(val), memory_order_relaxed)

#elif defined(_MSC_VER) && !defined(__clang__)
/* MSVC: use the portable atomic_* wrappers supplied by include/stdatomic.h.
 * MSVC does not provide __atomic_* compiler built-ins or the __ATOMIC_*
 * memory-order constants, so the GCC/Clang #else branch below would produce
 * undefined-identifier errors when these macros are expanded.              */
#include <stdatomic.h>
#define TTAK_FAST_ATOMIC_ADD_U64(ptr, val) \
    atomic_fetch_add_explicit((_Atomic uint64_t *)(ptr), (uint64_t)(val), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_ADD_U32(ptr, val) \
    atomic_fetch_add_explicit((_Atomic uint32_t *)(ptr), (uint32_t)(val), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_LOAD_U64(ptr) \
    atomic_load_explicit((_Atomic uint64_t *)(ptr), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_STORE_U32(ptr, val) \
    atomic_store_explicit((_Atomic uint32_t *)(ptr), (uint32_t)(val), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_STORE_BOOL(ptr, val) \
    atomic_store_explicit((_Atomic _Bool *)(ptr), (_Bool)((val) ? 1 : 0), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_XCHG_U64(ptr, val) \
    atomic_exchange_explicit((_Atomic uint64_t *)(ptr), (uint64_t)(val), memory_order_relaxed)

#else
#include <stdatomic.h>
#define TTAK_FAST_ATOMIC_ADD_U64(ptr, val) \
    atomic_fetch_add_explicit((_Atomic uint64_t *)(ptr), (uint64_t)(val), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_ADD_U32(ptr, val) \
    atomic_fetch_add_explicit((_Atomic uint32_t *)(ptr), (uint32_t)(val), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_LOAD_U64(ptr) \
    atomic_load_explicit((_Atomic uint64_t *)(ptr), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_STORE_U32(ptr, val) \
    atomic_store_explicit((_Atomic uint32_t *)(ptr), (uint32_t)(val), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_STORE_BOOL(ptr, val) \
    atomic_store_explicit((_Atomic uint64_t *)(ptr), (uint64_t)((val) ? 1 : 0), memory_order_relaxed)
#define TTAK_FAST_ATOMIC_XCHG_U64(ptr, val) \
    atomic_exchange_explicit((_Atomic uint64_t *)(ptr), (uint64_t)(val), memory_order_relaxed)
#endif

/** @brief Variable cleanup attribute for GCC/Clang; no-op on others. */
#if defined(__GNUC__) || defined(__clang__)
#  define TTAK_ATTRIBUTE_CLEANUP(func) __attribute__((cleanup(func)))
#else
#  define TTAK_ATTRIBUTE_CLEANUP(func)
#endif

#endif /* TTAK_COMPILER_H */
