/**
 * @file mem.h
 * @brief TTAK Unified Memory Subsystem with Lifecycle Management and Hardware Optimization.
 *
 * This header defines the "Fortress" memory allocation system, which provides:
 * - Tiered allocation (Thread-Local Pockets, Bare-Metal VMA, System Allocator)
 * - Automatic lifecycle management with tick-based expiration
 * - Security features (Magic numbers, Checksums, Canaries)
 * - Hardware optimizations (Cache-line alignment, Huge pages)
 */

#ifndef TTAK_MEM_H
#define TTAK_MEM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#if defined(_MSC_VER)
#include <windows.h>
#elif !defined(__TINYC__) && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#endif
#include <ttak/mem/epoch_gc.h>
#include <ttak/types/ttak_compiler.h>
#include <stdalign.h>
#include <pthread.h>

/* Owner handle used by ttak_mem_unuse(); full definition is in <ttak/mem/owner.h>. */
typedef struct ttak_owner ttak_owner_t;

#if defined(_MSC_VER)
#define TTAK_ATOMIC_FETCH_ADD_U64(ptr, val) InterlockedExchangeAdd64((volatile LONG64 *)(ptr), (LONG64)(val))
#elif defined(__TINYC__) || defined(__STDC_NO_ATOMICS__)
uint64_t ttak_atomic_fetch_add_u64_fallback(uint64_t *ptr, uint64_t val);
#define TTAK_ATOMIC_FETCH_ADD_U64(ptr, val) ttak_atomic_fetch_add_u64_fallback((uint64_t *)(ptr), (uint64_t)(val))
#else
#define TTAK_ATOMIC_FETCH_ADD_U64(ptr, val) atomic_fetch_add((_Atomic uint64_t *)(ptr), (val))
#endif

/**
 * @enum ttak_allocation_tier_t
 * @brief Defines the memory tier used for an allocation.
 */
typedef enum {
    TTAK_ALLOC_TIER_UNKNOWN = 0,    /**< Tier unknown or corrupted */
    TTAK_ALLOC_TIER_POCKET,         /**< Allocated from a Thread-Local Pocket (Small objects) */
    TTAK_ALLOC_TIER_VMA,            /**< Allocated from Bare-Metal VMA (Medium objects) */
    TTAK_ALLOC_TIER_SLAB,           /**< Allocated from a Slab allocator (Reserved) */
    TTAK_ALLOC_TIER_BUDDY,          /**< Allocated from the Buddy System (Embedded Mode) */
    TTAK_ALLOC_TIER_GENERAL,        /**< Allocated via general system allocator (Large objects) */
} ttak_allocation_tier_t;

/**
 * @brief Alignment for cache-line optimization (64-byte).
 */
#define TTAK_CACHE_LINE_SIZE 64

/**
 * @brief Macro to indicate that the allocated memory should persist forever.
 */
#define __TTAK_UNSAFE_MEM_FOREVER__ ((uint64_t)-1)

/**
 * @brief "Fortress" Magic Number for header validation.
 */
#define TTAK_MAGIC_NUMBER 0x5454414B

/**
 * @brief Sentinel for invalidated references.
 */
#define SAFE_NULL NULL

/**
 * @struct ttak_mem_header_t
 * @brief "Fortress" Memory Header stored before user data.
 *
 * 64-byte aligned to prevent False Sharing and ensure user pointer alignment.
 * The structure is padded to maintain alignment for the following user data.
 */
typedef struct ttak_mem_header_t {
    alignas(64) uint32_t magic;         /**< 0x5454414B */
    uint32_t checksum;                  /**< Metadata checksum to detect header corruption */
    uint64_t created_tick;              /**< Creation timestamp in ticks */
    uint64_t expires_tick;              /**< Expiration timestamp in ticks */
    uint64_t access_count;              /**< Atomic access audit counter */
    uint64_t pin_count;                 /**< Atomic reference count for pinning */
    size_t   size;                      /**< User-requested size in bytes */
    pthread_mutex_t lock;               /**< Per-header synchronization lock */
    uint8_t  freed;                     /**< True if the block has been deallocated */
    uint8_t  is_const;                  /**< Immutability hint */
    uint8_t  is_volatile;               /**< Volatility hint */
    uint8_t  allow_direct_access;       /**< Safety bypass flag for direct pointer access */
    uint8_t  is_huge;                   /**< True if mapped via hugepages */
    uint8_t  should_join;               /**< Indicates if associated resource needs joining */
    uint8_t  strict_check;              /**< Enable strict memory boundary (canary) checks */
    uint8_t  is_root;                   /**< Marks the allocation as a root node for the mem_tree */
    uint64_t canary_start;              /**< Magic number for start of user data (in strict mode) */
    uint64_t canary_end;                /**< Magic number for end of user data (in strict mode) */
    char     *tracking_log;             /**< Dynamic memory operation tracking log (JSON) */
    uint8_t  allocation_tier;           /**< Tier that performed the allocation */
    size_t   mapped_size;               /**< Total OS-mapped bytes (header + payload + canary) */
    char     reserved[2];               /**< Explicit padding for header alignment */
} ttak_mem_header_t;

/**
 * @enum ttak_mem_flags_t
 * @brief Memory allocation behavior flags.
 */
typedef enum {
    TTAK_MEM_DEFAULT = 0,               /**< Default allocation behavior */
    TTAK_MEM_HUGE_PAGES = (1 << 0),     /**< Try to use 2MB/1GB pages */
    TTAK_MEM_CACHE_ALIGNED = (1 << 1),  /**< Force 64-byte cache alignment */
    TTAK_MEM_STRICT_CHECK = (1 << 2),   /**< Enable strict boundary/canary checks */
    TTAK_MEM_LOW_PRIORITY = (1 << 3)    /**< Reject if under memory pressure/high friction */
} ttak_mem_flags_t;

/**
 * @brief Unified memory allocation with lifecycle management.
 * @param size Number of bytes requested.
 * @param lifetime_ticks Lifetime hint in ticks (__TTAK_UNSAFE_MEM_FOREVER__ for infinite).
 * @param now_tick Current timestamp in ticks.
 * @param is_const Marks the buffer as immutable.
 * @param is_volatile Indicates volatile access patterns.
 * @param allow_direct If false, direct access via ttak_mem_access is restricted.
 * @param is_root Marks the allocation as a root for garbage collection.
 * @param flags Allocation behavior flags.
 * @return Pointer to zeroed user memory, or NULL on failure.
 */
void *ttak_mem_alloc_safe(size_t size, uint64_t lifetime_ticks, uint64_t now_tick, bool is_const, bool is_volatile, bool allow_direct, bool is_root, ttak_mem_flags_t flags);
/* @brief Wrapper for easy allocation with auto GC registering */
void *ttak_fastalloc(ttak_epoch_gc_t *gc, size_t size, uint64_t lifetime_ticks, uint64_t now_tick);
/* @brief Wrapper for easy allocation and zero-fill with auto GC registering */
void * ttak_fastcalloc(ttak_epoch_gc_t *gc, size_t size, uint64_t lifetime_ticks, uint64_t now);
/**
 * @brief Reallocates memory with lifecycle management.
 * @param ptr Existing allocation pointer.
 * @param new_size Requested new size.
 * @param lifetime_ticks Updated lifetime hint.
 * @param now_tick Current timestamp in ticks.
 * @param is_root Whether the new allocation is a root node.
 * @param flags Allocation behavior flags.
 * @return Reallocated pointer, or NULL on failure.
 */
void *ttak_mem_realloc_safe(void *ptr, size_t new_size, uint64_t lifetime_ticks, uint64_t now_tick, bool is_root, ttak_mem_flags_t flags);
/**
 * @brief Primitive allocator for internal subsystem bootstrap.
 *
 * Bypasses all managed logic (Header, Map, Tree) to prevent recursive cycles
 * during early initialization of the epoch or memory subsystems.
 *
 * @param size Number of bytes requested.
 * @return Pointer to zeroed user memory (64-byte aligned), or NULL on failure.
 */

void *ttak_dangerous_alloc(size_t size);

/**
 * @brief Primitive calloc for internal subsystem bootstrap.
 *
 * @param nmemb Number of elements.
 * @param size Size of each element.
 * @return Pointer to zeroed user memory (64-byte aligned), or NULL on failure.
 */
void *ttak_dangerous_calloc(size_t nmemb, size_t size);

/**
 * @brief Primitive deallocator for internal subsystem cleanup.
 *
 * Directly returns memory to the underlying system allocator or buddy pool
 * without performing any managed header or checksum validations.
 *
 * @param ptr Pointer to memory allocated via ttak_dangerous_alloc.
 */
void ttak_dangerous_free(void *ptr);

/**
 * @brief Frees a memory block and updates usage statistics.
 * @param ptr Pointer to user memory.
 */
void ttak_mem_free(void *ptr);

/**
 * @brief Duplicates a memory block with lifecycle management.
 * @param src Source memory block.
 * @param size Number of bytes to copy.
 * @param lifetime_ticks Updated lifetime hint.
 * @param now_tick Current timestamp in ticks.
 * @param is_root Whether the new allocation is a root node.
 * @param flags Allocation behavior flags.
 * @return Duplicated pointer, or NULL on failure.
 */
void *ttak_mem_dup_safe(const void *src, size_t size, uint64_t lifetime_ticks, uint64_t now_tick, bool is_root, ttak_mem_flags_t flags);

/**
 * @brief Explicitly tells the GC that the caller no longer uses this pointer.
 *
 * If @p owner is non-NULL, the pointer is also removed from that owner's
 * resource map.  Pass TTAK_NO_OWNER (or NULL) for ownerless pointers.
 *
 * @param ptr   Pointer to user memory that is being released from use.
 * @param owner Owner context that held the pointer, or TTAK_NO_OWNER.
 */
void ttak_mem_unuse(void *ptr, ttak_owner_t *owner);

/**
 * @brief Frees a memory block and clears the caller's pointer.
 *
 * Convenience wrapper around ttak_mem_free() that also sets @p *ptr to NULL
 * so the caller cannot accidentally reuse a dangling pointer. Passing a NULL
 * pointer or a pointer to NULL is a no-op.
 *
 * @param ptr Pointer to the user pointer that will be freed and zeroed.
 */
void ttak_mem_freep(void **ptr);

/**
 * @brief Safe accessor implemented in the library for ABI-stable builds.
 *
 * Consumers that define TTAK_MEM_FORCE_ACCESS_BRIDGE will call into this
 * function instead of relying on the inline header logic. This is primarily
 * intended for scenarios where the library was compiled with a different
 * compiler (e.g., TinyCC) and the structure layout may not match the
 * including translation unit.
 */
void *ttak_mem_access_bridge(void *ptr, uint64_t now_tick);

/**
 * @brief Accesses a memory block, verifying its lifecycle and security.
 * @param ptr Pointer to user memory.
 * @param now_tick Current timestamp in ticks.
 * @return Validated pointer, or NULL if security check fails or block is expired.
 */
#if defined(TTAK_MEM_FORCE_ACCESS_BRIDGE)
static inline void *ttak_mem_access(void *ptr, uint64_t now_tick) {
    return ttak_mem_access_bridge(ptr, now_tick);
}
#else
#if defined(__linux__)
#include <sys/mman.h>
#endif
static inline void *ttak_mem_access(void *ptr, uint64_t now_tick) {
    if (!ptr) return NULL;
    ttak_mem_header_t *header = (ttak_mem_header_t *)ptr - 1;

#if defined(__linux__)
    /* Guard against use-after-free on pages that have been unmapped. */
    unsigned char vec = 0;
    void *page_base = (void *)(((uintptr_t)header) & ~((uintptr_t)4095));
    if (mincore(page_base, 4096, &vec) != 0) return NULL;
    if (!(vec & 1)) return NULL;
#endif

    if (header->magic != TTAK_MAGIC_NUMBER) return NULL;
    if (header->freed) return NULL;
    if (header->expires_tick != __TTAK_UNSAFE_MEM_FOREVER__ && now_tick > header->expires_tick) return NULL;
    if (!header->allow_direct_access) return NULL;

    TTAK_ATOMIC_FETCH_ADD_U64(&header->access_count, 1ULL);
    return ptr;
}
#endif

/**
 * @brief Inspects for "dirty" pointers (expired or over-accessed).
 * @param now_tick Current timestamp.
 * @param count_out Pointer to store the number of dirty pointers found.
 * @return Array of pointers (caller must free), or NULL.
 */
void **tt_inspect_dirty_pointers(uint64_t now_tick, size_t *count_out);

/**
 * @brief Automatically cleans up expired memory blocks.
 * @param now_tick Current timestamp.
 */
void tt_autoclean_dirty_pointers(uint64_t now_tick);

/**
 * @brief Configures background GC parameters.
 * @param min_interval_ns Minimum sweep interval.
 * @param max_interval_ns Maximum sweep interval.
 * @param pressure_threshold Memory pressure threshold to trigger damping.
 */
void ttak_mem_configure_gc(uint64_t min_interval_ns, uint64_t max_interval_ns, size_t pressure_threshold);

/**
 * @brief Sweeps and returns dirty pointers in a single pass.
 * @param now_tick Current timestamp.
 * @param count_out Pointer to store found count.
 * @return Array of dirty pointers.
 */
void **tt_autoclean_and_inspect(uint64_t now_tick, size_t *count_out);

/**
 * @brief Sets the global memory tracing flag.
 * @param enable Non-zero to enable JSON tracing to stderr.
 */
void ttak_mem_set_trace(int enable);

/**
 * @brief Checks if memory tracing is enabled.
 * @return Non-zero if enabled.
 */
int ttak_mem_is_trace_enabled(void);

/**
 * @brief Calculates a 32-bit checksum for the memory header.
 * @param h Pointer to the header.
 * @return Calculated checksum.
 */
static inline uint32_t ttak_calc_header_checksum(const ttak_mem_header_t *h) {
    uint32_t sum1 = h->magic;
    uint32_t sum2 = (uint32_t)h->created_tick;
    sum1 ^= (uint32_t)(h->created_tick >> 32);
    sum2 ^= (uint32_t)h->expires_tick;
    sum1 ^= (uint32_t)(h->expires_tick >> 32);
    sum2 ^= (uint32_t)h->size;
#if defined(__LP64__) || defined(_WIN64)
    sum1 ^= (uint32_t)(h->size >> 32);
#endif
    sum2 ^= (uint32_t)h->should_join;
    sum1 ^= (uint32_t)h->strict_check;
    sum2 ^= (uint32_t)h->is_root;
    sum1 ^= (uint32_t)h->canary_start;
    sum2 ^= (uint32_t)(h->canary_start >> 32);
    sum1 ^= (uint32_t)h->canary_end;
    sum2 ^= (uint32_t)(h->canary_end >> 32);
    sum1 ^= (uint32_t)h->allocation_tier;
    return sum1 ^ sum2;
}

/* Convenience type alias for lifecycle objects */
typedef void ttak_lifecycle_obj_t;

#if defined(__GNUC__) || defined(__clang__)
/**
 * @brief Cleanup handler for scoped ttak memory allocations.
 * @param p Pointer to the variable holding the allocation (void **).
 *
 * Invoked automatically when a variable declared with the cleanup attribute
 * goes out of scope.
 */
static inline void ttak_mem_cleanup(void *p) {
    void **pp = (void **)p;
    if (*pp) {
        ttak_mem_free(*pp);
    }
}
#endif

/**
 * @defgroup MemScopedMacros Scoped RAII Allocation Macros
 * @brief Scope-bound allocation macros that auto-free on scope exit (GCC/Clang).
 *
 * These macros declare a variable that is automatically freed when it goes
 * out of scope using the compiler's @c cleanup attribute. On compilers that
 * do not support this attribute they degrade to plain allocation.
 * @{
 */

/**
 * @def ttak_mem_alloc_scoped(var, cast, size, lifetime, now_tick)
 * @brief Scoped allocation with default flags.
 * @param var     Variable name to declare.
 * @param cast    Type to cast the resulting pointer to.
 * @param size    Number of bytes to allocate.
 * @param lifetime Lifetime hint in ticks.
 * @param now_tick Current timestamp in ticks.
 */
#define ttak_mem_alloc_scoped(var, cast, size, lifetime, now_tick) \
    void *_ttak_scoped_##var TTAK_ATTRIBUTE_CLEANUP(ttak_mem_cleanup) = ttak_mem_alloc_safe(size, lifetime, now_tick, false, false, true, false, TTAK_MEM_DEFAULT); \
    cast var = (cast)_ttak_scoped_##var

/**
 * @def ttak_root_alloc_scoped(var, cast, size, lifetime, now_tick)
 * @brief Scoped root allocation with default flags.
 * @param var     Variable name to declare.
 * @param cast    Type to cast the resulting pointer to.
 * @param size    Number of bytes to allocate.
 * @param lifetime Lifetime hint in ticks.
 * @param now_tick Current timestamp in ticks.
 */
#define ttak_root_alloc_scoped(var, cast, size, lifetime, now_tick) \
    void *_ttak_scoped_##var TTAK_ATTRIBUTE_CLEANUP(ttak_mem_cleanup) = ttak_mem_alloc_safe(size, lifetime, now_tick, true, true, true, true, TTAK_MEM_DEFAULT); \
    cast var = (cast)_ttak_scoped_##var

/**
 * @def ttak_mem_alloc_with_flags_scoped(var, cast, size, lifetime, now_tick, flags)
 * @brief Scoped allocation with custom flags.
 */
#define ttak_mem_alloc_with_flags_scoped(var, cast, size, lifetime, now_tick, flags) \
    void *_ttak_scoped_##var TTAK_ATTRIBUTE_CLEANUP(ttak_mem_cleanup) = ttak_mem_alloc_safe(size, lifetime, now_tick, false, false, true, false, flags); \
    cast var = (cast)_ttak_scoped_##var

/**
 * @def ttak_root_alloc_with_flags_scoped(var, cast, size, lifetime, now_tick, flags)
 * @brief Scoped root allocation with custom flags.
 */
#define ttak_root_alloc_with_flags_scoped(var, cast, size, lifetime, now_tick, flags) \
    void *_ttak_scoped_##var TTAK_ATTRIBUTE_CLEANUP(ttak_mem_cleanup) = ttak_mem_alloc_safe(size, lifetime, now_tick, true, true, true, true, flags); \
    cast var = (cast)_ttak_scoped_##var

/**
 * @def ttak_mem_realloc_scoped(var, cast, ptr, size, lifetime, now_tick)
 * @brief Scoped reallocation with default flags.
 */
#define ttak_mem_realloc_scoped(var, cast, ptr, size, lifetime, now_tick) \
    void *_ttak_scoped_##var TTAK_ATTRIBUTE_CLEANUP(ttak_mem_cleanup) = ttak_mem_realloc_safe(ptr, size, lifetime, now_tick, false, TTAK_MEM_DEFAULT); \
    cast var = (cast)_ttak_scoped_##var

/**
 * @def ttak_root_realloc_scoped(var, cast, ptr, size, lifetime, now_tick)
 * @brief Scoped root reallocation with default flags.
 */
#define ttak_root_realloc_scoped(var, cast, ptr, size, lifetime, now_tick) \
    void *_ttak_scoped_##var TTAK_ATTRIBUTE_CLEANUP(ttak_mem_cleanup) = ttak_mem_realloc_safe(ptr, size, lifetime, now_tick, true, TTAK_MEM_DEFAULT); \
    cast var = (cast)_ttak_scoped_##var

/**
 * @def ttak_mem_realloc_with_flags_scoped(var, cast, ptr, size, lifetime, now_tick, flags)
 * @brief Scoped reallocation with custom flags.
 */
#define ttak_mem_realloc_with_flags_scoped(var, cast, ptr, size, lifetime, now_tick, flags) \
    void *_ttak_scoped_##var TTAK_ATTRIBUTE_CLEANUP(ttak_mem_cleanup) = ttak_mem_realloc_safe(ptr, size, lifetime, now_tick, false, flags); \
    cast var = (cast)_ttak_scoped_##var

/**
 * @def ttak_root_realloc_with_flags_scoped(var, cast, ptr, size, lifetime, now_tick, flags)
 * @brief Scoped root reallocation with custom flags.
 */
#define ttak_root_realloc_with_flags_scoped(var, cast, ptr, size, lifetime, now_tick, flags) \
    void *_ttak_scoped_##var TTAK_ATTRIBUTE_CLEANUP(ttak_mem_cleanup) = ttak_mem_realloc_safe(ptr, size, lifetime, now_tick, true, flags); \
    cast var = (cast)_ttak_scoped_##var

/**
 * @def ttak_mem_dup_scoped(var, cast, src, size, lifetime, now_tick)
 * @brief Scoped duplication with default flags.
 */
#define ttak_mem_dup_scoped(var, cast, src, size, lifetime, now_tick) \
    void *_ttak_scoped_##var TTAK_ATTRIBUTE_CLEANUP(ttak_mem_cleanup) = ttak_mem_dup_safe(src, size, lifetime, now_tick, false, TTAK_MEM_DEFAULT); \
    cast var = (cast)_ttak_scoped_##var

/**
 * @def ttak_root_dup_scoped(var, cast, src, size, lifetime, now_tick)
 * @brief Scoped root duplication with default flags.
 */
#define ttak_root_dup_scoped(var, cast, src, size, lifetime, now_tick) \
    void *_ttak_scoped_##var TTAK_ATTRIBUTE_CLEANUP(ttak_mem_cleanup) = ttak_mem_dup_safe(src, size, lifetime, now_tick, true, TTAK_MEM_DEFAULT); \
    cast var = (cast)_ttak_scoped_##var

/**
 * @def ttak_mem_dup_with_flags_scoped(var, cast, src, size, lifetime, now_tick, flags)
 * @brief Scoped duplication with custom flags.
 */
#define ttak_mem_dup_with_flags_scoped(var, cast, src, size, lifetime, now_tick, flags) \
    void *_ttak_scoped_##var TTAK_ATTRIBUTE_CLEANUP(ttak_mem_cleanup) = ttak_mem_dup_safe(src, size, lifetime, now_tick, false, flags); \
    cast var = (cast)_ttak_scoped_##var

/**
 * @def ttak_root_dup_with_flags_scoped(var, cast, src, size, lifetime, now_tick, flags)
 * @brief Scoped root duplication with custom flags.
 */
#define ttak_root_dup_with_flags_scoped(var, cast, src, size, lifetime, now_tick, flags) \
    void *_ttak_scoped_##var TTAK_ATTRIBUTE_CLEANUP(ttak_mem_cleanup) = ttak_mem_dup_safe(src, size, lifetime, now_tick, true, flags); \
    cast var = (cast)_ttak_scoped_##var

/** @} */ /* end of MemScopedMacros */

/**
 * @defgroup MemConvenienceMacros Convenience Allocation Macros
 * @brief Simplified macros that return a plain pointer for caller-driven lifetime.
 *
 * These macros wrap the underlying @c _safe functions with sensible defaults
 * for everyday use. The caller is responsible for freeing the returned memory.
 * @{
 */

/** @brief Allocate memory with default flags. */
#define ttak_mem_alloc(size, lifetime, now_tick) ttak_mem_alloc_safe(size, lifetime, now_tick, false, false, true, false, TTAK_MEM_DEFAULT)
/** @brief Allocate root memory with default flags. */
#define ttak_root_alloc(size, lifetime, now_tick) ttak_mem_alloc_safe(size, lifetime, now_tick, true, true, true, true, TTAK_MEM_DEFAULT)
/** @brief Allocate memory with custom flags. */
#define ttak_mem_alloc_with_flags(size, lifetime, now_tick, flags) ttak_mem_alloc_safe(size, lifetime, now_tick, false, false, true, false, flags)
/** @brief Allocate root memory with custom flags. */
#define ttak_root_alloc_with_flags(size, lifetime, now_tick, flags) ttak_mem_alloc_safe(size, lifetime, now_tick, true, true, true, true, flags)
/** @brief Reallocate memory with default flags. */
#define ttak_mem_realloc(ptr, size, lifetime, now_tick) ttak_mem_realloc_safe(ptr, size, lifetime, now_tick, false, TTAK_MEM_DEFAULT)
/** @brief Reallocate root memory with default flags. */
#define ttak_root_realloc(ptr, size, lifetime, now_tick) ttak_mem_realloc_safe(ptr, size, lifetime, now_tick, true, TTAK_MEM_DEFAULT)
/** @brief Reallocate memory with custom flags. */
#define ttak_mem_realloc_with_flags(ptr, size, lifetime, now_tick, flags) ttak_mem_realloc_safe(ptr, size, lifetime, now_tick, false, flags)
/** @brief Reallocate root memory with custom flags. */
#define ttak_root_realloc_with_flags(ptr, size, lifetime, now_tick, flags) ttak_mem_realloc_safe(ptr, size, lifetime, now_tick, true, flags)
/** @brief Duplicate memory with default flags. */
#define ttak_mem_dup(src, size, lifetime, now_tick) ttak_mem_dup_safe(src, size, lifetime, now_tick, false, TTAK_MEM_DEFAULT)
/** @brief Duplicate root memory with default flags. */
#define ttak_root_dup(src, size, lifetime, now_tick) ttak_mem_dup_safe(src, size, lifetime, now_tick, true, TTAK_MEM_DEFAULT)
/** @brief Duplicate memory with custom flags. */
#define ttak_mem_dup_with_flags(src, size, lifetime, now_tick, flags) ttak_mem_dup_safe(src, size, lifetime, now_tick, false, flags)
/** @brief Duplicate root memory with custom flags. */
#define ttak_root_dup_with_flags(src, size, lifetime, now_tick, flags) ttak_mem_dup_safe(src, size, lifetime, now_tick, true, flags)

/** @} */ /* end of MemConvenienceMacros */

/**
 * @defgroup MemRawMacros Raw Allocation Macros
 * @brief Explicit aliases of the convenience macros for code that prefers the @_raw suffix.
 * @{
 */
#define ttak_mem_alloc_raw(size, lifetime, now_tick) ttak_mem_alloc_safe(size, lifetime, now_tick, false, false, true, false, TTAK_MEM_DEFAULT)
#define ttak_root_alloc_raw(size, lifetime, now_tick) ttak_mem_alloc_safe(size, lifetime, now_tick, true, true, true, true, TTAK_MEM_DEFAULT)
#define ttak_mem_alloc_with_flags_raw(size, lifetime, now_tick, flags) ttak_mem_alloc_safe(size, lifetime, now_tick, false, false, true, false, flags)
#define ttak_root_alloc_with_flags_raw(size, lifetime, now_tick, flags) ttak_mem_alloc_safe(size, lifetime, now_tick, true, true, true, true, flags)
#define ttak_mem_realloc_raw(ptr, size, lifetime, now_tick) ttak_mem_realloc_safe(ptr, size, lifetime, now_tick, false, TTAK_MEM_DEFAULT)
#define ttak_root_realloc_raw(ptr, size, lifetime, now_tick) ttak_mem_realloc_safe(ptr, size, lifetime, now_tick, true, TTAK_MEM_DEFAULT)
#define ttak_mem_realloc_with_flags_raw(ptr, size, lifetime, now_tick, flags) ttak_mem_realloc_safe(ptr, size, lifetime, now_tick, false, flags)
#define ttak_root_realloc_with_flags_raw(ptr, size, lifetime, now_tick, flags) ttak_mem_realloc_safe(ptr, size, lifetime, now_tick, true, flags)
#define ttak_mem_dup_raw(src, size, lifetime, now_tick) ttak_mem_dup_safe(src, size, lifetime, now_tick, false, TTAK_MEM_DEFAULT)
#define ttak_root_dup_raw(src, size, lifetime, now_tick) ttak_mem_dup_safe(src, size, lifetime, now_tick, true, TTAK_MEM_DEFAULT)
#define ttak_mem_dup_with_flags_raw(src, size, lifetime, now_tick, flags) ttak_mem_dup_safe(src, size, lifetime, now_tick, false, flags)
#define ttak_root_dup_with_flags_raw(src, size, lifetime, now_tick, flags) ttak_mem_dup_safe(src, size, lifetime, now_tick, true, flags)

/** @} */ /* end of MemRawMacros */

#ifndef EMBEDDED
#define EMBEDDED 0
#endif

#if EMBEDDED
void ttak_mem_buddy_init(void *pool_start, size_t pool_len, int embedded_mode);
void ttak_mem_buddy_set_pool(void *pool_start, size_t pool_len);
void ttak_mem_set_embedded_pool(void *pool_start, size_t pool_len);
#endif

#endif // TTAK_MEM_H
