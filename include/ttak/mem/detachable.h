#ifndef TTAK_MEM_DETACHABLE_H
#define TTAK_MEM_DETACHABLE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <ttak/compat/baremetal.h>

/* Signal-driven shutdown hooks need POSIX signals: not on Win32 or bare metal. */
#if !defined(_WIN32) && !TTAK_PLATFORM_BAREMETAL
#define TTAK_DETACHABLE_HAVE_SIGNALS 1
#else
#define TTAK_DETACHABLE_HAVE_SIGNALS 0
#endif

#if TTAK_DETACHABLE_HAVE_SIGNALS
#include <signal.h>
#endif
#include <ttak/compat/pthread.h>
#include <ttak/compat/stdatomic.h>

#include <ttak/mem/epoch.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum size in bytes for cacheable detachable chunks.
 */
#define TTAK_DETACHABLE_CACHE_MAX_BYTES 16U

/**
 * @brief Default number of cache slots kept per detachable arena.
 */
#define TTAK_DETACHABLE_CACHE_SLOTS 8U

/**
 * @brief Maximum number of tracked generations per arena row.
 */
#define TTAK_DETACHABLE_GENERATIONS 4U

/**
 * @brief Maximum number of 2D rows in the detachable arena tracker.
 *
 * The design models a 2D array backed tree where each "row" models the
 * hierarchy and avoids pathological page-aligned fragmentation.
 */
#define TTAK_DETACHABLE_MATRIX_ROWS 8U
#define TTAK_DETACHABLE_FLIP_WINDOW_NS 5000000ULL
#define TTAK_DETACHABLE_FLIP_MAX_GAP_NS 100000ULL
#define TTAK_DETACHABLE_FLIP_EVENT_THRESHOLD 512U
#define TTAK_DETACHABLE_FLIP_SMALL_QUARANTINE_BYTES 64U
#define TTAK_DETACHABLE_QUARANTINE_BYTE_LIMIT (1U << 20)

/**
 * @brief Bit flags describing arena level traits.
 */
typedef enum ttak_detachable_context_flags {
    TTAK_ARENA_HAS_OWNER             = (1u << 0),
    TTAK_ARENA_HAS_EPOCH_RECLAMATION = (1u << 1),
    TTAK_ARENA_HAS_DEFAULT_EPOCH_GC  = (1u << 2),
    TTAK_ARENA_IS_URGENT_TASK        = (1u << 3),
    TTAK_ARENA_USE_LOCKED_ACCESS     = (1u << 4),
    TTAK_ARENA_IS_SINGLE_THREAD      = (1u << 5),
    TTAK_ARENA_USE_ASYNC_OPT         = (1u << 6)
} ttak_detachable_context_flags_t;

/**
 * @brief Bit flags stored inside the detachable status byte.
 */
typedef enum ttak_detach_state_flags {
    TTAK_DETACHABLE_UNKNOWN        = 0x00,
    TTAK_DETACHABLE_ATTACH         = 0x01,
    TTAK_DETACHABLE_DETACH_NOCHECK = 0x02,
    TTAK_DETACHABLE_PARTIAL_CACHE  = 0x04,
    TTAK_DETACHABLE_STATUS_KNOWN   = 0x80
} ttak_detach_state_flags_t;

/**
 * @brief Tracks the detach lifecycle of a detachable block.
 *
 * The status is stored as a single byte and converges towards UNKNOWN whenever
 * no explicit KNOWN flag is set. Padding keeps 8-byte alignment stable for the
 * struct returned to callers.
 */
typedef struct ttak_detach_status {
    uint8_t bits;
    _Bool pad[4];
} ttak_detach_status_t;

static inline void ttak_detach_status_reset(ttak_detach_status_t *status) {
    if (!status) return;
    status->bits = TTAK_DETACHABLE_UNKNOWN;
    for (size_t i = 0; i < sizeof(status->pad) / sizeof(status->pad[0]); ++i) {
        status->pad[i] = false;
    }
}

static inline void ttak_detach_status_mark_known(ttak_detach_status_t *status) {
    if (!status) return;
    status->bits |= TTAK_DETACHABLE_STATUS_KNOWN;
}

static inline void ttak_detach_status_converge(ttak_detach_status_t *status) {
    if (!status) return;
    if (!(status->bits & TTAK_DETACHABLE_STATUS_KNOWN)) {
        status->bits = TTAK_DETACHABLE_UNKNOWN;
    }
}

/**
 * @brief Per-context cache for tiny detachable chunks (<= 16 bytes).
 *
 * The cache implements an approximated LRU queue that is biased to favor the
 * active generation. Entries are zeroed before returning to the caller to keep
 * calloc semantics intact.
 */
typedef struct ttak_detachable_cache {
    size_t chunk_size;
    size_t capacity;
    size_t count;
    size_t head;
    size_t tail;
    void **slots;
    uint64_t hits;
    uint64_t misses;
    pthread_mutex_t lock;
} ttak_detachable_cache_t;

/**
 * @brief Fixed-width arena row descriptor.
 */
typedef struct ttak_detachable_generation_row {
    void **columns;
    size_t len;
    size_t cap;
} ttak_detachable_generation_row_t;

typedef struct ttak_detachable_quarantine_row {
    void **columns;
    size_t *sizes;
    size_t len;
    size_t cap;
    size_t bytes;
} ttak_detachable_quarantine_row_t;

/**
 * @brief State holder for detachable memory contexts.
 */
typedef struct ttak_detachable_context {
    uint8_t matrix_rows;
    uint8_t active_row;
    uint16_t epoch_delay;
    uint32_t flags;
    ttak_detach_status_t base_status;
    ttak_detachable_cache_t small_cache;
    ttak_detachable_generation_row_t rows[TTAK_DETACHABLE_MATRIX_ROWS];
    ttak_detachable_quarantine_row_t quarantine;
    pthread_rwlock_t arena_lock;
    atomic_uint_fast64_t global_epoch_hint;
    atomic_uint_fast64_t flip_window_start_ns;
    atomic_uint_fast64_t flip_last_event_ns;
    atomic_uint_fast64_t flip_event_count;
    uint64_t flip_window_ns;
    uint64_t flip_max_gap_ns;
    uint32_t flip_event_threshold;
    size_t flip_small_quarantine_bytes;
    size_t quarantine_byte_limit;
} ttak_detachable_context_t;

/**
 * @brief Description of the detachable allocation returned to callers.
 */
typedef struct ttak_detachable_allocation {
    void *data;
    size_t size;
    ttak_detach_status_t detach_status;
    ttak_detachable_cache_t *cache;
} ttak_detachable_allocation_t;

/**
 * @brief Initializes a detachable context with sane defaults.
 */
void ttak_detachable_context_init(ttak_detachable_context_t *ctx, uint32_t flags);

/**
 * @brief Destroys a detachable context and releases cached entries.
 */
void ttak_detachable_context_destroy(ttak_detachable_context_t *ctx);

/**
 * @brief Returns a lazily created global default context suitable for most users.
 */
ttak_detachable_context_t *ttak_detachable_context_default(void);

/**
 * @brief Initializes the per-context cache.
 */
void ttak_detachable_cache_init(ttak_detachable_cache_t *cache, size_t chunk_size, size_t capacity);

/**
 * @brief Releases any pending cache entries and tears down synchronization primitives.
 */
void ttak_detachable_cache_destroy(ttak_detachable_context_t *ctx, ttak_detachable_cache_t *cache);

/**
 * @brief Allocates detachable memory backed by calloc and epoch protection.
 */
ttak_detachable_allocation_t ttak_detachable_mem_alloc(ttak_detachable_context_t *ctx, size_t size, uint64_t epoch_hint);

/**
 * @brief Releases a detachable allocation, optionally routing it through the cache.
 */
void ttak_detachable_mem_free(ttak_detachable_context_t *ctx, ttak_detachable_allocation_t *alloc);

#if TTAK_DETACHABLE_HAVE_SIGNALS
/**
 * @brief Registers signal handlers that gracefully flush detachable arenas and exit.
 *
 * @param signals Bitmask of signals to intercept.
 * @param ret Optional pointer that stores the exit status to use.
 * @return 0 on success, -1 on failure.
 */
int ttak_hard_kill_graceful_exit(sigset_t signals, int *ret);

/**
 * @brief Registers signal handlers that immediately exit without flushing arenas.
 *
 * @param signals Bitmask of signals to intercept.
 * @param ret Optional pointer that stores the exit status to use.
 * @return 0 on success, -1 on failure.
 */
int ttak_hard_kill_exit(sigset_t signals, int *ret);
#endif /* TTAK_DETACHABLE_HAVE_SIGNALS */

#ifdef __cplusplus
}
#endif

#endif /* TTAK_MEM_DETACHABLE_H */
