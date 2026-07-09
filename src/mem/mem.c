/**
 * @file mem.c
 * @brief Implementation of the TTAK Unified Memory Subsystem.
 *
 * Implements tiered memory allocation, lifecycle tracking, and friction-based
 * pressure sensing for high-performance concurrent applications.
 */

#include <ttak/mem/mem.h>
#include <ttak/mem/owner.h>
#include <ttak/atomic/atomic.h>
#include <ttak/ht/map.h>
#include <ttak/timing/timing.h>
#include <ttak/mem_tree/mem_tree.h>
#include "../../internal/app_types.h"
#include "../../internal/ttak/mem_internal.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <limits.h>
#include <errno.h>

#ifdef _WIN32
    #define _CRT_NONSTDC_NO_DEPRECATE 1
    #include <windows.h>
    #include <io.h>
    #define fsync(fd) _commit(fd)
    #define unlink _unlink
    typedef SSIZE_T ssize_t;  /* POSIX ssize_t for MSVC */
    /* Map POSIX CRT names to their MSVC underscore equivalents */
    #ifndef open
    #  define open  _open
    #endif
    #ifndef write
    #  define write(fd, buf, n) _write((fd), (buf), (unsigned int)(n))
    #endif
    #ifndef close
    #  define close _close
    #endif
    #ifndef read
    #  define read  _read
    #endif
#else
    #include <unistd.h>
    #if EMBEDDED && !defined(EMBEDDED_BAREMETAL)
        #include <sys/mman.h>
    #endif
#endif

#ifdef _WIN32
static int posix_memalign(void **memptr, size_t alignment, size_t size) {
    *memptr = _aligned_malloc(size, alignment);
    return (*memptr) ? 0 : 1;
}
#ifndef MAP_FAILED
    #define MAP_FAILED ((void *)-1)
#endif
#define posix_memfree(ptr) _aligned_free(ptr)
#else
#define posix_memfree(ptr) free(ptr)
#endif

#include <ttak/mem/fastpath.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdio.h>
#include <fcntl.h>

/**
 * @brief Internal canary magic numbers for boundary check validation.
 */
#define TTAK_CANARY_START_MAGIC 0xDEADBEEFDEADBEEFULL
#define TTAK_CANARY_END_MAGIC   0xBEEFDEADBEEFDEADULL

static volatile uint64_t global_mem_usage = 0;           /**< Atomic counter for total libttak usage */
static pthread_mutex_t global_map_lock = PTHREAD_MUTEX_INITIALIZER; /**< Global lock for pointer map */
static ttak_mem_tree_t global_mem_tree;                 /**< Global root-tracking mem_tree */
static int global_trace_enabled = 0;                    /**< Flag for JSON tracing */

/**
 * @brief Initialize the global friction matrix for Damping.
 */
ttak_mem_friction_matrix_t global_friction_matrix = {
    .values = { TTAK_FP_ONE, TTAK_FP_ONE, TTAK_FP_ONE, TTAK_FP_ONE },
    .global_friction = TTAK_FP_ONE,
    .pressure_threshold = TTAK_FP_FROM_INT(1)
};

#if defined(__TINYC__) || defined(__STDC_NO_ATOMICS__)
uint64_t ttak_atomic_fetch_add_u64_fallback(uint64_t *ptr, uint64_t val) {
    uint64_t expected;
    uint64_t desired;
    do {
        expected = *ptr;
        desired = expected + val;
    } while (__sync_val_compare_and_swap(ptr, expected, desired) != expected);
    return expected;
}
#endif

/**
 * @brief Internal validation macro for header integrity and canaries.
 */
#if defined(__TINYC__)
#define V_HEADER(ptr) ((void)0)
#else
#define V_HEADER(ptr) do { \
    if (!ptr) break; \
    ttak_mem_header_t *_h = (ttak_mem_header_t *)(ptr) - 1; \
    if (_h->magic != TTAK_MAGIC_NUMBER || _h->checksum != ttak_calc_header_checksum(_h)) { \
        fprintf(stderr, "[FATAL] TTAK Memory Corruption detected at %p (Header corrupted)\n", (void*)ptr); \
        abort(); \
    } \
    if (_h->strict_check) { \
        if (_h->canary_start != TTAK_CANARY_START_MAGIC) { \
            fprintf(stderr, "[FATAL] TTAK Memory Corruption detected at %p (Start canary corrupted in header)\n", (void*)ptr); \
            abort(); \
        } \
        uint64_t *canary_end_ptr = (uint64_t *)((char *)ptr + _h->size); \
        if (*canary_end_ptr != TTAK_CANARY_END_MAGIC) { \
            fprintf(stderr, "[FATAL] TTAK Memory Corruption detected at %p (End canary corrupted)\n", (void*)ptr); \
            abort(); \
        } \
    } \
} while (0)
#endif


#define GET_HEADER(ptr) ((ttak_mem_header_t *)(ptr) - 1)
#define GET_USER_PTR(header) ((void *)((ttak_mem_header_t *)(header) + 1))

static volatile tt_map_t *global_ptr_map = NULL;
static pthread_mutex_t global_init_lock = PTHREAD_MUTEX_INITIALIZER;

#if EMBEDDED
#include <ttak/phys/mem/buddy.h>
#ifndef TTAK_EMBEDDED_POOL_ORDER
#if defined(EMBEDDED_BAREMETAL)
#define TTAK_EMBEDDED_POOL_ORDER 15  /* 32KB */
#elif defined(__LP64__) || defined(_WIN64)
#define TTAK_EMBEDDED_POOL_ORDER 28
#else
#define TTAK_EMBEDDED_POOL_ORDER 24
#endif
#endif
#define TTAK_EMBEDDED_POOL_SIZE (1ULL << TTAK_EMBEDDED_POOL_ORDER)
#if TTAK_EMBEDDED_POOL_ORDER >= 63
#error "TTAK_EMBEDDED_POOL_ORDER must be less than 63"
#endif
static _Alignas(64) uint8_t buddy_pool[TTAK_EMBEDDED_POOL_SIZE];
static pthread_once_t buddy_once = PTHREAD_ONCE_INIT;
static void *embedded_pool_start = buddy_pool;
static size_t embedded_pool_len = sizeof(buddy_pool);

#ifndef TTAK_BUDDY_DEFAULT_EMBEDDED_MODE
#define TTAK_BUDDY_DEFAULT_EMBEDDED_MODE 0 /* override to 1 for strict embedded builds */
#endif

static int ttak_detect_embedded_pool_mode(void) {
#if defined(EMBEDDED_BAREMETAL)
    return TTAK_BUDDY_DEFAULT_EMBEDDED_MODE;
#else
    const char *env = getenv("TTAK_MEM_EMBEDDED_POOL");
    if (!env || !*env) {
        return TTAK_BUDDY_DEFAULT_EMBEDDED_MODE;
    }
    switch (env[0]) {
        case '0':
        case 'n':
        case 'N':
        case 'f':
        case 'F':
            return 0;
        default:
            return 1;
    }
#endif
}

/**
 * @brief Lazy-init for the buddy system in embedded builds.
 */
static void buddy_bootstrap(void) {
    ttak_mem_buddy_init(buddy_pool, sizeof(buddy_pool), ttak_detect_embedded_pool_mode());
    embedded_pool_start = buddy_pool;
    embedded_pool_len = sizeof(buddy_pool);
}

static bool ttak_embedded_ptr_in_pool(const void *ptr) {
    const uintptr_t addr = (uintptr_t)ptr;
    const uintptr_t start = (uintptr_t)embedded_pool_start;
    const uintptr_t end = start + embedded_pool_len;
    return embedded_pool_start && embedded_pool_len && addr >= start && addr < end;
}

static void *ttak_embedded_os_alloc(size_t size) {
#if defined(_WIN32)
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__unix__)
    void *os_ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (os_ptr == MAP_FAILED) os_ptr = NULL;
    return os_ptr;
#else
    (void)size;
    return NULL;
#endif
}

static void ttak_embedded_os_free(void *ptr, size_t size) {
    if (!ptr || size == 0) return;
#if defined(_WIN32)
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__unix__)
    munmap(ptr, size);
#else
    (void)ptr;
    (void)size;
#endif
}
#endif

#if defined(__TINYC__)
static pthread_once_t ttak_tls_once = PTHREAD_ONCE_INIT;
static pthread_key_t ttak_tls_guard_key;
static pthread_key_t ttak_tls_memop_key;
static pthread_key_t ttak_tls_meminit_key;
static pthread_key_t ttak_tls_retrying_key;

static void ttak_tls_init_keys(void) {
    pthread_key_create(&ttak_tls_guard_key, free);
    pthread_key_create(&ttak_tls_memop_key, free);
    pthread_key_create(&ttak_tls_meminit_key, free);
    pthread_key_create(&ttak_tls_retrying_key, free);
}

static void *ttak_tls_get_slot(pthread_key_t key, size_t slot_size, void *fallback) {
    pthread_once(&ttak_tls_once, ttak_tls_init_keys);
    void *slot = pthread_getspecific(key);
    if (!slot) {
        slot = calloc(1, slot_size);
        if (slot && pthread_setspecific(key, slot) != 0) {
            free(slot);
            slot = NULL;
        }
    }
    return slot ? slot : fallback;
}

bool *ttak_tls_get_reentrancy_guard(void) {
    static bool global_fallback = false;
    return (bool *)ttak_tls_get_slot(ttak_tls_guard_key, sizeof(bool), &global_fallback);
}

static int *ttak_tls_get_in_mem_op(void) {
    static int fallback = 0;
    return (int *)ttak_tls_get_slot(ttak_tls_memop_key, sizeof(int), &fallback);
}

static int *ttak_tls_get_in_mem_init(void) {
    static int fallback = 0;
    return (int *)ttak_tls_get_slot(ttak_tls_meminit_key, sizeof(int), &fallback);
}

static int *ttak_tls_get_retrying_counter(void) {
    static int fallback = 0;
    return (int *)ttak_tls_get_slot(ttak_tls_retrying_key, sizeof(int), &fallback);
}

#define in_mem_op        (*ttak_tls_get_in_mem_op())
#define in_mem_init      (*ttak_tls_get_in_mem_init())
#else
TTAK_THREAD_LOCAL bool t_reentrancy_guard = false;  /**< Thread-local guard against recursive allocation */
TTAK_THREAD_LOCAL int in_mem_op = 0;               /**< Guard for pointer-map operations */
TTAK_THREAD_LOCAL int in_mem_init = 0;            /**< Guard for subsystem initialization */
#endif
static volatile int global_init_done = 0;         /**< Subsystem initialization ready flag */

/**
 * @brief Primitive allocator for internal subsystem bootstrap.
 *
 * Bypasses all managed logic (Header, Map, Tree) to prevent recursive cycles
 * during early initialization of the epoch or memory subsystems.
 */
void *ttak_dangerous_alloc(size_t size) {
    if (size == 0) return NULL;
    void *ptr = NULL;
#if EMBEDDED
    pthread_once(&buddy_once, buddy_bootstrap);
    ttak_mem_req_t req = { .size_bytes = size, .priority = 0, .owner_tag = 0, .call_safety = 0, .flags = 0 };
    ptr = ttak_mem_buddy_alloc(&req);
    if (!ptr) {
        size_t mapped_size;
        void *os_ptr;
        if (size > SIZE_MAX - sizeof(size_t)) return NULL;
        mapped_size = size + sizeof(size_t);
        os_ptr = ttak_embedded_os_alloc(mapped_size);
        if (!os_ptr) return NULL;
        *((size_t *)os_ptr) = mapped_size;
        ptr = (char *)os_ptr + sizeof(size_t);
    }
    ttak_mem_stream_zero(ptr, size);
#else
    if (posix_memalign(&ptr, 64, size) != 0) return NULL;
    ttak_mem_stream_zero(ptr, size);
#endif
    return ptr;
}

void *ttak_dangerous_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (nmemb && size && total / nmemb != size) return NULL; /* Overflow check */
    return ttak_dangerous_alloc(total);
}

/**
 * @brief Primitive deallocator for internal subsystem cleanup.
 *
 * Directly returns memory to the underlying system allocator or buddy pool
 * without performing any managed header or checksum validations.
 */
void ttak_dangerous_free(void *ptr) {
    if (!ptr) return;
#if EMBEDDED
    if (ttak_embedded_ptr_in_pool(ptr)) {
        ttak_mem_buddy_free(ptr);
    } else {
        void *os_ptr = (char *)ptr - sizeof(size_t);
        size_t mapped_size = *((size_t *)os_ptr);
        ttak_embedded_os_free(os_ptr, mapped_size);
    }
#else
    posix_memfree(ptr);
#endif
}

/**
 * @brief Recalculates the global friction as the product of all class values.
 */
static ttak_fixed_16_16_t ttak_mem_calculate_global_friction(void) TTAK_MAYBE_UNUSED;
static ttak_fixed_16_16_t ttak_mem_calculate_global_friction(void) {
    ttak_fixed_16_16_t friction_product = TTAK_FP_ONE;
    for (int i = 0; i < 4; ++i) {
        friction_product = TTAK_FP_MUL(friction_product, atomic_load(&global_friction_matrix.values[i]));
    }
    atomic_store(&global_friction_matrix.global_friction, friction_product);
    return friction_product;
}

/**
 * @brief Ensures the global pointer map and mem_tree are initialized.
 */
static void ensure_global_map(uint64_t now) {
    if (global_init_done || in_mem_init) return;
    pthread_mutex_lock(&global_init_lock);
    if (!global_ptr_map && !global_init_done) {
        in_mem_init = true;
        global_ptr_map = ttak_create_map(8192, now);
        ttak_mem_tree_init(&global_mem_tree);
        global_init_done = true;
        in_mem_init = false;
    }
    pthread_mutex_unlock(&global_init_lock);
}

// See public interfaces in include/ttak/mem/mem.h for full documentation

void TTAK_HOT_PATH *ttak_mem_alloc_safe(size_t size, uint64_t lifetime_ticks, uint64_t now, _Bool is_const, _Bool is_volatile, _Bool allow_direct, _Bool is_root, ttak_mem_flags_t flags) {
    size_t header_size = sizeof(ttak_mem_header_t);
    bool strict_check_enabled = (flags & TTAK_MEM_STRICT_CHECK);
    ttak_mem_header_t *header = NULL;
    ttak_allocation_tier_t allocated_tier = TTAK_ALLOC_TIER_UNKNOWN;
    void *user_ptr = NULL;

    size_t guard_extra = header_size;
    if (strict_check_enabled) {
        guard_extra += sizeof(uint64_t);
    }
    size_t align_overhead = (TTAK_VMA_ALIGNMENT > 0) ? (TTAK_VMA_ALIGNMENT - 1) : 0;
    if (guard_extra > SIZE_MAX - align_overhead) {
        return NULL;
    }
    guard_extra += align_overhead;
    if (size > SIZE_MAX - guard_extra) {
        return NULL;
    }

    if (t_reentrancy_guard) {
        /* Returning a raw malloc pointer here would create a use-after-free risk
         * because callers may later pass it to ttak_mem_free(), which reads the
         * TTAK header that precedes managed allocations.  Return NULL so the
         * caller can handle the failure safely. */
        return NULL;
    }
    t_reentrancy_guard = true;

    // --- Tier 1: Pockets (small objects) ---
    if (size > 0 && size <= 512 && lifetime_ticks < TT_SECOND(1)) {
        header = ttak_mem_pocket_alloc_internal(size);
        if (header) allocated_tier = TTAK_ALLOC_TIER_POCKET;
    }

    // --- Tier 2: VMA medium region ---
    if (!header && size > 0 && size < (2 * 1024 * 1024)) {
        size_t vma_size = size;
        if (strict_check_enabled) {
            vma_size += sizeof(uint64_t);
        }
        header = ttak_mem_vma_alloc_internal(vma_size);
        if (header) {
            allocated_tier = TTAK_ALLOC_TIER_VMA;
        }
#if EMBEDDED
        else {
            size_t mapped_size = size + sizeof(ttak_mem_header_t);
            header = ttak_embedded_os_alloc(mapped_size);
            if (header) {
                allocated_tier = TTAK_ALLOC_TIER_BUDDY;
                strict_check_enabled = false;
            }
        }
#endif
    }

    // --- Tier 3: Dedicated large region / buddy fallback ---
    if (!header) {
        size_t tier3_size = size;
        if (strict_check_enabled) {
            if (tier3_size > SIZE_MAX - sizeof(uint64_t)) {
                t_reentrancy_guard = false;
                return NULL;
            }
            tier3_size += sizeof(uint64_t);
        }
#if EMBEDDED
        if ((flags & TTAK_MEM_LOW_PRIORITY) &&
            atomic_load(&global_friction_matrix.global_friction) > atomic_load(&global_friction_matrix.pressure_threshold)) {
            t_reentrancy_guard = false;
            return NULL;
        }
        pthread_once(&buddy_once, buddy_bootstrap);
        ttak_mem_req_t req = { .size_bytes = tier3_size + sizeof(ttak_mem_header_t), .priority = 1, .owner_tag = 0, .call_safety = 0, .flags = 0 };
        header = ttak_mem_buddy_alloc(&req);
        if (header) {
            allocated_tier = TTAK_ALLOC_TIER_BUDDY;
            strict_check_enabled = false;
        } else {
            size_t mapped_size = tier3_size + sizeof(ttak_mem_header_t);
            header = ttak_embedded_os_alloc(mapped_size);
            if (header) {
                allocated_tier = TTAK_ALLOC_TIER_BUDDY;
                strict_check_enabled = false;
            }
        }
#else
        header = ttak_mem_large_alloc_internal(tier3_size);
        if (header) allocated_tier = TTAK_ALLOC_TIER_GENERAL;
#endif
    }

    if (!header) { t_reentrancy_guard = false; return NULL; }

    header->magic = TTAK_MAGIC_NUMBER;
    header->created_tick = now;
    header->expires_tick = (lifetime_ticks == __TTAK_UNSAFE_MEM_FOREVER__) ? (uint64_t)-1 : now + lifetime_ticks;
    header->access_count = 0;
    header->pin_count = 0;
    header->size = size;
    header->freed = false;
    header->is_const = is_const;
    header->is_volatile = is_volatile;
    header->allow_direct_access = allow_direct;
    header->is_huge = false;
    header->should_join = false;
    header->strict_check = strict_check_enabled;
    header->is_root = is_root;
    header->canary_start = strict_check_enabled ? TTAK_CANARY_START_MAGIC : 0;
    header->canary_end = strict_check_enabled ? TTAK_CANARY_END_MAGIC : 0;
    pthread_mutex_init(&header->lock, NULL);
    header->allocation_tier = allocated_tier;

    size_t actual_total_alloc_size;
    if (allocated_tier == TTAK_ALLOC_TIER_POCKET) actual_total_alloc_size = get_total_block_size_for_freelist(get_pocket_size_class_idx(header_size + size));
    else if (allocated_tier == TTAK_ALLOC_TIER_VMA) actual_total_alloc_size = (header_size + size + (strict_check_enabled ? sizeof(uint64_t) : 0) + TTAK_VMA_ALIGNMENT - 1) & ~((size_t)TTAK_VMA_ALIGNMENT - 1);
    else if (allocated_tier == TTAK_ALLOC_TIER_GENERAL) {
        size_t raw = header_size + size + (strict_check_enabled ? sizeof(uint64_t) : 0);
        actual_total_alloc_size = (raw + TTAK_VMA_ALIGNMENT - 1) & ~((size_t)TTAK_VMA_ALIGNMENT - 1);
    } else actual_total_alloc_size = header_size + size + (strict_check_enabled ? sizeof(uint64_t) : 0);
    header->mapped_size = actual_total_alloc_size;
    header->checksum = ttak_calc_header_checksum(header);

    ttak_atomic_add64(&global_mem_usage, actual_total_alloc_size);
    user_ptr = (char *)header + header_size;
    ttak_mem_stream_zero(user_ptr, size);
    if (strict_check_enabled) *((uint64_t *)((char *)user_ptr + size)) = TTAK_CANARY_END_MAGIC;

    if (global_trace_enabled) {
        header->tracking_log = malloc(1024);
        if (header->tracking_log) {
            snprintf(header->tracking_log, 1024, "{\"event\":\"alloc\",\"ptr\":\"%p\",\"size\":%zu,\"ts\":%" PRIu64 ",\"root\":%d,\"tier\":%d}", user_ptr, size, now, (int)is_root, (int)allocated_tier);
            fprintf(stderr, "[MEM_TRACK] %s\n", header->tracking_log);
        }
    } else header->tracking_log = NULL;

    if (is_root) {
        ensure_global_map(now);
        tt_map_t *map_handle = (tt_map_t *)global_ptr_map;
        if (global_init_done && !in_mem_init && !in_mem_op && map_handle) {
            /* Clear the reentrancy guard before map/tree operations so that
             * internal map resize allocations (non-root) can succeed normally
             * instead of hitting the NULL fallback path. */
            t_reentrancy_guard = false;
            pthread_mutex_lock(&global_map_lock); in_mem_op = true;
            ttak_insert_to_map(map_handle, (uintptr_t)user_ptr, (size_t)header, now);
            ttak_mem_tree_add(&global_mem_tree, user_ptr, size, header->expires_tick, is_root);
            in_mem_op = false; pthread_mutex_unlock(&global_map_lock);
        }
    }

    t_reentrancy_guard = false;
    return user_ptr;
}

void * ttak_fastalloc(ttak_epoch_gc_t *gc, size_t size, uint64_t lifetime_ticks, uint64_t now) {
    void *ptr = ttak_mem_alloc_raw(size, lifetime_ticks, now);
    ttak_epoch_gc_register(gc, ptr, size);
    return ptr;
}

void * ttak_fastcalloc(ttak_epoch_gc_t *gc, size_t size, uint64_t lifetime_ticks, uint64_t now) {
    void *ptr = ttak_fastalloc(gc, size, lifetime_ticks, now);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}


void TTAK_HOT_PATH *ttak_mem_realloc_safe(void *ptr, size_t new_size, uint64_t lifetime_ticks, uint64_t now, _Bool is_root, ttak_mem_flags_t flags) {
    V_HEADER(ptr);
    if (!ptr) return ttak_mem_alloc_safe(new_size, lifetime_ticks, now, false, false, true, is_root, flags);

    ttak_mem_header_t *old_header = GET_HEADER(ptr);
    pthread_mutex_lock(&old_header->lock);
    bool is_const = old_header->is_const, is_volatile = old_header->is_volatile, allow_direct = old_header->allow_direct_access, old_strict = old_header->strict_check;
    size_t old_size = old_header->size;
    pthread_mutex_unlock(&old_header->lock);

    ttak_mem_flags_t new_flags = flags;
    if (old_strict) new_flags |= TTAK_MEM_STRICT_CHECK; else new_flags &= ~TTAK_MEM_STRICT_CHECK;

    void *new_ptr = ttak_mem_alloc_safe(new_size, lifetime_ticks, now, is_const, is_volatile, allow_direct, is_root, new_flags);
    if (!new_ptr) return NULL;
    ttak_mem_stream_copy(new_ptr, ptr, (old_size < new_size) ? old_size : new_size);
    ttak_mem_free(ptr);
    return new_ptr;
}

void TTAK_HOT_PATH *ttak_mem_dup_safe(const void *src, size_t size, uint64_t lifetime_ticks, uint64_t now, _Bool is_root, ttak_mem_flags_t flags) {
    if (!src) return NULL;
    ttak_mem_header_t *h_src = (ttak_mem_header_t *)src - 1;
    bool is_const = false, is_volatile = false, allow_direct = true;
    ttak_mem_flags_t final_flags = flags;
    if (h_src->magic == TTAK_MAGIC_NUMBER) {
        is_const = h_src->is_const; is_volatile = h_src->is_volatile; allow_direct = h_src->allow_direct_access;
        if (h_src->strict_check) final_flags |= TTAK_MEM_STRICT_CHECK;
    }
    void *new_ptr = ttak_mem_alloc_safe(size, lifetime_ticks, now, is_const, is_volatile, allow_direct, is_root, final_flags);
    if (!new_ptr) return NULL;
    ttak_mem_stream_copy(new_ptr, src, size);
    return new_ptr;
}

void TTAK_HOT_PATH ttak_mem_free(void *ptr) {
    if (!ptr) return;
    void *stable_ptr = ptr;
    ttak_mem_header_t *header = GET_HEADER(stable_ptr);

    pthread_mutex_lock(&header->lock);
    if (header->freed) { pthread_mutex_unlock(&header->lock); return; }
    header->freed = true;
    pthread_mutex_unlock(&header->lock);

    V_HEADER(stable_ptr);

    if (global_trace_enabled && header->tracking_log) {
        snprintf(header->tracking_log, 1024, "{\"event\":\"free\",\"ptr\":\"%p\",\"ts\":%" PRIu64 ",\"tier\":%d}", stable_ptr, ttak_get_tick_count(), (int)header->allocation_tier);
        fprintf(stderr, "[MEM_TRACK] %s\n", header->tracking_log);
        free(header->tracking_log); header->tracking_log = NULL;
    }

    size_t actual_total_alloc_size = header->mapped_size;

    if (header->is_root && (header->allocation_tier == TTAK_ALLOC_TIER_GENERAL || header->allocation_tier == TTAK_ALLOC_TIER_BUDDY)) {
        pthread_mutex_lock(&global_map_lock); in_mem_op = 1;
        ttak_delete_from_map((tt_map_t*)global_ptr_map, (uintptr_t)stable_ptr, 0);
        ttak_mem_node_t *node = ttak_mem_tree_find_node(&global_mem_tree, stable_ptr);
        if (node) ttak_mem_tree_remove(&global_mem_tree, node);
        in_mem_op = 0; pthread_mutex_unlock(&global_map_lock);
    }

    ttak_atomic_sub64(&global_mem_usage, actual_total_alloc_size);

    switch (header->allocation_tier) {
        case TTAK_ALLOC_TIER_POCKET: _pocket_free_internal(header); break;
        case TTAK_ALLOC_TIER_VMA: _vma_free_internal(header); break;
        case TTAK_ALLOC_TIER_BUDDY:
#if EMBEDDED
            if (ttak_embedded_ptr_in_pool(header)) {
                ttak_mem_buddy_free(header);
            } else {
                pthread_mutex_destroy(&header->lock);
                ttak_embedded_os_free(header, header->mapped_size);
            }
#else
            _large_free_internal(header);
#endif
            break;
        case TTAK_ALLOC_TIER_GENERAL:
            _large_free_internal(header);
            break;
        default:
            pthread_mutex_destroy(&header->lock);
#if EMBEDDED
            ttak_mem_buddy_free(header);
#else
            _large_free_internal(header);
#endif
            break;
    }
}

void ttak_mem_unuse(void *ptr, ttak_owner_t *owner) {
    if (!ptr) return;
    void *stable_ptr = ptr;
    ttak_mem_header_t *header = GET_HEADER(stable_ptr);
    V_HEADER(stable_ptr);

    /* Remove the pointer from the owner's resource map when an owner is given. */
    if (owner != TTAK_NO_OWNER && owner != NULL && owner->resources) {
        ttak_rwlock_wrlock(&owner->lock);
        ttak_map_t *res = (ttak_map_t *)owner->resources;
        for (size_t i = 0; i < res->cap; ++i) {
            if (res->ctrls[i] == OCCUPIED && (void *)res->values[i] == stable_ptr) {
                ttak_delete_from_map(res, res->keys[i], ttak_get_tick_count());
                break;
            }
        }
        ttak_rwlock_unlock(&owner->lock);
    }

    /* Release one GC reference so the mem tree can collect the block when expired. */
    if (header->is_root && (header->allocation_tier == TTAK_ALLOC_TIER_GENERAL ||
                            header->allocation_tier == TTAK_ALLOC_TIER_BUDDY)) {
        pthread_mutex_lock(&global_map_lock); in_mem_op = 1;
        ttak_mem_node_t *node = ttak_mem_tree_find_node(&global_mem_tree, stable_ptr);
        if (node) ttak_mem_node_release(node);
        in_mem_op = 0; pthread_mutex_unlock(&global_map_lock);
    }
}

void ttak_mem_freep(void **ptr) {
    if (ptr && *ptr) {
        ttak_mem_free(*ptr);
        *ptr = NULL;
    }
}

void *ttak_mem_access_bridge(void *ptr, uint64_t now_tick) {
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

void ttak_mem_set_trace(int enable) {
    global_trace_enabled = enable;
    if (!global_init_done) return;
    pthread_mutex_lock(&global_map_lock);
    tt_map_t *map_handle = (tt_map_t *)global_ptr_map;
    if (map_handle) {
        for (size_t i = 0; i < map_handle->cap; i++) {
            if (map_handle->ctrls[i] == OCCUPIED) {
                ttak_mem_header_t *h = (ttak_mem_header_t *)map_handle->values[i];
                pthread_mutex_lock(&h->lock);
                if (enable && !h->tracking_log) {
                    h->tracking_log = malloc(1024);
                    if (h->tracking_log) snprintf(h->tracking_log, 1024, "{\"event\":\"trace_enabled\",\"ts\":%" PRIu64 "}", ttak_get_tick_count());
                } else if (!enable && h->tracking_log) { free(h->tracking_log); h->tracking_log = NULL; }
                pthread_mutex_unlock(&h->lock);
            }
        }
    }
    pthread_mutex_unlock(&global_map_lock);
}

int ttak_mem_is_trace_enabled(void) { return global_trace_enabled; }

void ttak_mem_configure_gc(uint64_t min_interval_ns, uint64_t max_interval_ns, size_t pressure_threshold) {
    uint64_t now = ttak_get_tick_count();
    ensure_global_map(now);
    ttak_mem_tree_set_cleaning_intervals(&global_mem_tree, min_interval_ns, max_interval_ns);
    ttak_mem_tree_set_pressure_threshold(&global_mem_tree, pressure_threshold);
    /* The global mem_tree cleanup thread spins on ttak_epoch_reclaim and
     * consumes whole cores.  Disable it; callers rotate/reclaim explicitly. */
    ttak_mem_tree_set_manual_cleanup(&global_mem_tree, true);
}

void TTAK_COLD_PATH tt_autoclean_dirty_pointers(uint64_t now) {
    size_t count = 0; void **dirty = tt_inspect_dirty_pointers(now, &count);
    if (!dirty) return;
    for (size_t i = 0; i < count; i++) ttak_mem_free(dirty[i]);
    free(dirty);
}

void TTAK_COLD_PATH **tt_inspect_dirty_pointers(uint64_t now, size_t *count_out) {
    tt_map_t *map_handle = (tt_map_t *)global_ptr_map;
    if (!count_out || !map_handle) return NULL;
    pthread_mutex_lock(&global_map_lock);
    void **dirty = malloc(sizeof(void *) * map_handle->size);
    if (!dirty) { pthread_mutex_unlock(&global_map_lock); return NULL; }
    size_t found = 0;
    for (size_t i = 0; i < map_handle->cap; i++) {
        if (map_handle->ctrls[i] == OCCUPIED) {
            ttak_mem_header_t *h = (ttak_mem_header_t *)map_handle->values[i];
            if ((h->expires_tick != (uint64_t)-1 && now > h->expires_tick) || ttak_atomic_read64(&h->access_count) > 1000000)
                dirty[found++] = (void*)map_handle->keys[i];
        }
    }
    pthread_mutex_unlock(&global_map_lock); *count_out = found; return dirty;
}

void **tt_autoclean_and_inspect(uint64_t now, size_t *count_out) {
    tt_autoclean_dirty_pointers(now); return tt_inspect_dirty_pointers(now, count_out);
}

_Bool ttak_mem_is_pressure_high(void) { return ttak_atomic_read64(&global_mem_usage) > TTAK_MEM_HIGH_WATERMARK; }

void save_current_progress(const char *filename, const void *data, size_t size) {
    char temp_name[256]; snprintf(temp_name, sizeof(temp_name), "%s.tmp", filename);
#if !defined(EMBEDDED_BAREMETAL)
    int fd = open(temp_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (write(fd, data, size) != (ssize_t)size) { close(fd); unlink(temp_name); return; }
    if (fsync(fd) != 0) { close(fd); unlink(temp_name); return; }
    close(fd); if (rename(temp_name, filename) != 0) unlink(temp_name);
#ifndef _WIN32
    int dfd = open(".", O_RDONLY | O_DIRECTORY); if (dfd >= 0) { fsync(dfd); close(dfd); }
#endif
#endif
}

#if EMBEDDED
void ttak_mem_set_embedded_pool(void *pool_start, size_t pool_len) {
    if (!pool_start || pool_len == 0) return;
    pthread_once(&buddy_once, buddy_bootstrap);
    embedded_pool_start = pool_start;
    embedded_pool_len = pool_len;
    ttak_mem_buddy_set_pool(pool_start, pool_len);
}
#endif
