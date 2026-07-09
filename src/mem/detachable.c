/**
 * @file detachable.c
 * @brief Detachable arena — generation-tracked, cache-ring backed allocation.
 *
 * Detachable allocations can be handed off across ownership boundaries
 * by transferring a generation ticket.  A per-CPU cache ring minimises
 * lock contention on the hot path.
 */

#include <ttak/mem/detachable.h>
#include <ttak/mem/fastpath.h>
#include <ttak/timing/timing.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32) && !defined(EMBEDDED_BAREMETAL)
#include <signal.h>
#include <unistd.h>
#endif

#ifdef __linux__
    #include <sys/mman.h>
#endif

#ifndef _WIN32
#ifndef NSIG
#ifdef _NSIG
#define NSIG _NSIG
#else
/* 64 real-time signals + 1; covers Linux and most BSD defaults */
#define NSIG 65
#endif
#endif
#endif

#ifndef _WIN32
typedef struct {
    atomic_bool triggered;
    _Bool graceful;
    int exit_code;
} ttak_signal_guard_t;
#endif

static pthread_once_t g_default_ctx_once = PTHREAD_ONCE_INIT;
static ttak_detachable_context_t g_default_ctx;

static pthread_mutex_t g_ctx_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static ttak_detachable_context_t **g_ctx_registry = NULL;
static size_t g_ctx_registry_len = 0;
static size_t g_ctx_registry_cap = 0;

#ifndef _WIN32
static ttak_signal_guard_t g_signal_guard = {
    .triggered = false,
    .graceful = true,
    .exit_code = -1
};
#endif

static void ttak_detachable_register_context(ttak_detachable_context_t *ctx);
static void ttak_detachable_unregister_context(ttak_detachable_context_t *ctx);
static void ttak_detachable_context_once(void);
static void ttak_detachable_row_flush(ttak_detachable_context_t *ctx, ttak_detachable_generation_row_t *row);
static void ttak_detachable_quarantine_flush(ttak_detachable_context_t *ctx);
static bool ttak_detachable_quarantine_push(ttak_detachable_context_t *ctx, void *ptr, size_t size);
static void ttak_detachable_track_pointer(ttak_detachable_context_t *ctx, void *ptr);
static bool ttak_detachable_untrack_pointer(ttak_detachable_context_t *ctx, void *ptr);
static bool ttak_detachable_cache_store(ttak_detachable_context_t *ctx, ttak_detachable_cache_t *cache, void *ptr, size_t size);
static void *ttak_detachable_cache_take(ttak_detachable_cache_t *cache, size_t requested);
static void ttak_detachable_cache_drain(ttak_detachable_context_t *ctx, ttak_detachable_cache_t *cache, bool release_storage);
typedef enum {
    TTAK_DETACHABLE_MODE_STANDARD = 0,
    TTAK_DETACHABLE_MODE_DETACHED = 1
} ttak_detachable_mode_t;
static ttak_detachable_mode_t ttak_detachable_mode_for_status(const ttak_detach_status_t *status);
static bool ttak_detachable_validate_status(ttak_detachable_context_t *ctx,
                                            ttak_detach_status_t *status,
                                            ttak_detachable_mode_t *mode_out);
static uint64_t ttak_detachable_now_ns(void);
static bool ttak_detachable_flip_should_quarantine(ttak_detachable_context_t *ctx, size_t size);
#ifndef _WIN32
static void ttak_detachable_signal_handler(int signo);
static int ttak_hard_kill_configure(sigset_t signals, int *ret, _Bool graceful);
#endif

static inline _Bool ttak_detachable_need_lock(const ttak_detachable_context_t *ctx) {
    return (ctx->flags & TTAK_ARENA_USE_LOCKED_ACCESS) && !(ctx->flags & TTAK_ARENA_IS_SINGLE_THREAD);
}

static inline void ttak_detachable_wrlock(ttak_detachable_context_t *ctx) {
    if (ttak_detachable_need_lock(ctx)) {
        pthread_rwlock_wrlock(&ctx->arena_lock);
    }
}

static inline void ttak_detachable_unlock(ttak_detachable_context_t *ctx) {
    if (ttak_detachable_need_lock(ctx)) {
        pthread_rwlock_unlock(&ctx->arena_lock);
    }
}

static void ttak_detachable_context_once(void) {
    ttak_detachable_context_init(&g_default_ctx, TTAK_ARENA_HAS_EPOCH_RECLAMATION | TTAK_ARENA_HAS_DEFAULT_EPOCH_GC);
}

ttak_detachable_context_t *ttak_detachable_context_default(void) {
    pthread_once(&g_default_ctx_once, ttak_detachable_context_once);
    return &g_default_ctx;
}

void ttak_detachable_context_init(ttak_detachable_context_t *ctx, uint32_t flags) {
    if (!ctx) return;

    ttak_mem_stream_zero(ctx, sizeof(*ctx));
    ctx->flags = flags;
    ctx->matrix_rows = TTAK_DETACHABLE_MATRIX_ROWS;
    ctx->active_row = 0;
    ctx->epoch_delay = (flags & TTAK_ARENA_HAS_DEFAULT_EPOCH_GC) ? 2 : 1;
    ttak_detach_status_reset(&ctx->base_status);
    atomic_init(&ctx->global_epoch_hint, 0);

    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
#ifdef PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
    pthread_rwlock_init(&ctx->arena_lock, &attr);
    pthread_rwlockattr_destroy(&attr);

    ttak_detachable_cache_init(&ctx->small_cache, TTAK_DETACHABLE_CACHE_MAX_BYTES, TTAK_DETACHABLE_CACHE_SLOTS);
    ctx->quarantine.columns = NULL;
    ctx->quarantine.sizes = NULL;
    ctx->quarantine.len = 0;
    ctx->quarantine.cap = 0;
    ctx->quarantine.bytes = 0;
    ctx->flip_window_ns = TTAK_DETACHABLE_FLIP_WINDOW_NS;
    ctx->flip_max_gap_ns = TTAK_DETACHABLE_FLIP_MAX_GAP_NS;
    ctx->flip_event_threshold = TTAK_DETACHABLE_FLIP_EVENT_THRESHOLD;
    ctx->flip_small_quarantine_bytes = TTAK_DETACHABLE_FLIP_SMALL_QUARANTINE_BYTES;
    ctx->quarantine_byte_limit = TTAK_DETACHABLE_QUARANTINE_BYTE_LIMIT;
    atomic_init(&ctx->flip_window_start_ns, 0);
    atomic_init(&ctx->flip_last_event_ns, 0);
    atomic_init(&ctx->flip_event_count, 0);

    for (size_t i = 0; i < ctx->matrix_rows; ++i) {
        ctx->rows[i].columns = NULL;
        ctx->rows[i].len = 0;
        ctx->rows[i].cap = 0;
    }

    ttak_detachable_register_context(ctx);
}

void ttak_detachable_context_destroy(ttak_detachable_context_t *ctx) {
    if (!ctx) return;

    ttak_detachable_unregister_context(ctx);

    for (size_t i = 0; i < ctx->matrix_rows; ++i) {
        ttak_detachable_row_flush(ctx, &ctx->rows[i]);
        free(ctx->rows[i].columns);
        ctx->rows[i].columns = NULL;
        ctx->rows[i].cap = 0;
        ctx->rows[i].len = 0;
    }

    ttak_detachable_cache_destroy(ctx, &ctx->small_cache);
    ttak_detachable_quarantine_flush(ctx);
    free(ctx->quarantine.columns);
    free(ctx->quarantine.sizes);
    ctx->quarantine.columns = NULL;
    ctx->quarantine.sizes = NULL;
    ctx->quarantine.len = 0;
    ctx->quarantine.cap = 0;
    ctx->quarantine.bytes = 0;
    pthread_rwlock_destroy(&ctx->arena_lock);
}

void ttak_detachable_cache_init(ttak_detachable_cache_t *cache, size_t chunk_size, size_t capacity) {
    if (!cache) return;

    ttak_mem_stream_zero(cache, sizeof(*cache));
    cache->chunk_size = (chunk_size == 0 || chunk_size > TTAK_DETACHABLE_CACHE_MAX_BYTES)
                            ? TTAK_DETACHABLE_CACHE_MAX_BYTES
                            : chunk_size;
    cache->capacity = (capacity == 0) ? TTAK_DETACHABLE_CACHE_SLOTS : capacity;
    cache->slots = calloc(cache->capacity, sizeof(void *));
    if (cache->slots == NULL) {
        cache->capacity = 0;
    }
    pthread_mutex_init(&cache->lock, NULL);
}

void ttak_detachable_cache_destroy(ttak_detachable_context_t *ctx, ttak_detachable_cache_t *cache) {
    if (!cache) return;
    ttak_detachable_cache_drain(ctx, cache, true);
    pthread_mutex_destroy(&cache->lock);
}

ttak_detachable_allocation_t ttak_detachable_mem_alloc(ttak_detachable_context_t *ctx, size_t size, uint64_t epoch_hint) {
    ttak_detachable_allocation_t result = {0};
    if (!ctx) ctx = ttak_detachable_context_default();

    size_t actual_size = size == 0 ? 1 : size;
    ttak_detach_status_t status = ctx->base_status;
    ttak_detach_status_converge(&status);
    ttak_detachable_mode_t mode = TTAK_DETACHABLE_MODE_STANDARD;
    if (!ttak_detachable_validate_status(ctx, &status, &mode)) {
        ttak_detach_status_reset(&status);
        status.bits |= TTAK_DETACHABLE_ATTACH;
    }

    void *data = NULL;
    bool from_cache = false;

    if (actual_size <= ctx->small_cache.chunk_size) {
        data = ttak_detachable_cache_take(&ctx->small_cache, actual_size);
        if (data) {
            from_cache = true;
        }
    }

    if (!data) {
        if (ctx->flags & TTAK_ARENA_HAS_EPOCH_RECLAMATION) {
            ttak_epoch_enter();
        }

        data = calloc(1, actual_size);

        if (ctx->flags & TTAK_ARENA_HAS_EPOCH_RECLAMATION) {
            atomic_store_explicit(&ctx->global_epoch_hint, epoch_hint, memory_order_relaxed);
            ttak_epoch_exit();
        }

        if (!data) {
            return result;
        }

#ifdef __linux__
        if (ctx->flags & TTAK_ARENA_USE_ASYNC_OPT) {
            madvise(data, actual_size, MADV_DONTFORK);
        }
#endif
    }

    if (mode == TTAK_DETACHABLE_MODE_STANDARD) {
        status.bits |= TTAK_DETACHABLE_ATTACH;
    }
    if (from_cache) status.bits |= TTAK_DETACHABLE_PARTIAL_CACHE;
    ttak_detach_status_mark_known(&status);

    ttak_detachable_wrlock(ctx);
    ttak_detachable_track_pointer(ctx, data);
    ttak_detachable_unlock(ctx);

    result.data = data;
    result.size = actual_size;
    result.detach_status = status;
    result.cache = &ctx->small_cache;
    return result;
}

void ttak_detachable_mem_free(ttak_detachable_context_t *ctx, ttak_detachable_allocation_t *alloc) {
    if (!alloc || !alloc->data) return;
    if (!ctx) ctx = ttak_detachable_context_default();

    ttak_detachable_mode_t mode = TTAK_DETACHABLE_MODE_STANDARD;
    bool status_valid = ttak_detachable_validate_status(ctx, &alloc->detach_status, &mode);
    if (!status_valid) {
        mode = TTAK_DETACHABLE_MODE_STANDARD;
    }

    bool flip_hot = ttak_detachable_flip_should_quarantine(ctx, alloc->size);
    bool stored = false;
    bool quarantined = false;

    if (flip_hot) {
        ttak_detachable_wrlock(ctx);
        quarantined = ttak_detachable_quarantine_push(ctx, alloc->data, alloc->size);
        ttak_detachable_unlock(ctx);
    }

    if (!quarantined && 
        alloc->size <= ctx->small_cache.chunk_size &&
        !flip_hot &&
        mode == TTAK_DETACHABLE_MODE_STANDARD) {
        stored = ttak_detachable_cache_store(ctx, &ctx->small_cache, alloc->data, alloc->size);
        if (stored) {
            alloc->detach_status.bits |= TTAK_DETACHABLE_PARTIAL_CACHE;
        }
    }

    ttak_detachable_wrlock(ctx);
    bool was_tracked = ttak_detachable_untrack_pointer(ctx, alloc->data);
    ttak_detachable_unlock(ctx);
    bool skip_retire = (!was_tracked) && (ctx->flags & TTAK_ARENA_HAS_EPOCH_RECLAMATION);

    if (!stored && !quarantined && !skip_retire) {
        if (ctx->flags & TTAK_ARENA_HAS_EPOCH_RECLAMATION) {
            ttak_epoch_enter();
            ttak_epoch_retire(alloc->data, free);
            ttak_epoch_exit();
        } else {
            free(alloc->data);
        }
    }

    alloc->data = NULL;
    alloc->size = 0;
    alloc->cache = NULL;
    ttak_detach_status_reset(&alloc->detach_status);
}

static uint64_t ttak_detachable_now_ns(void) {
#if defined(_WIN32)
    return 0;
#elif defined(EMBEDDED_BAREMETAL)
    return ttak_get_tick_count_ns();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
#endif
}

static bool ttak_detachable_flip_should_quarantine(ttak_detachable_context_t *ctx, size_t size) {
    if (!ctx || size == 0) {
        return false;
    }
    if (ctx->flip_event_threshold == 0 || ctx->flip_window_ns == 0 || ctx->flip_max_gap_ns == 0) {
        return false;
    }
    if (size > ctx->flip_small_quarantine_bytes) {
        return false;
    }
    if (ctx->quarantine_byte_limit > 0 && ctx->quarantine.bytes >= ctx->quarantine_byte_limit) {
        return false;
    }

    uint64_t now_ns = ttak_detachable_now_ns();
    uint64_t window_start = atomic_load_explicit(&ctx->flip_window_start_ns, memory_order_relaxed);
    uint64_t last_event_ns = atomic_load_explicit(&ctx->flip_last_event_ns, memory_order_relaxed);

    if (window_start == 0 || last_event_ns == 0) {
        atomic_store_explicit(&ctx->flip_window_start_ns, now_ns, memory_order_relaxed);
        atomic_store_explicit(&ctx->flip_last_event_ns, now_ns, memory_order_relaxed);
        atomic_store_explicit(&ctx->flip_event_count, 1, memory_order_relaxed);
        return false;
    }

    if ((now_ns > window_start && (now_ns - window_start) > ctx->flip_window_ns) ||
        (now_ns > last_event_ns && (now_ns - last_event_ns) > ctx->flip_max_gap_ns)) {
        atomic_store_explicit(&ctx->flip_window_start_ns, now_ns, memory_order_relaxed);
        atomic_store_explicit(&ctx->flip_last_event_ns, now_ns, memory_order_relaxed);
        atomic_store_explicit(&ctx->flip_event_count, 1, memory_order_relaxed);
        return false;
    }

    atomic_store_explicit(&ctx->flip_last_event_ns, now_ns, memory_order_relaxed);
    uint64_t count = atomic_fetch_add_explicit(&ctx->flip_event_count, 1, memory_order_relaxed) + 1;
    return count >= ctx->flip_event_threshold;
}

static ttak_detachable_mode_t ttak_detachable_mode_for_status(const ttak_detach_status_t *status) {
    if (!status) return TTAK_DETACHABLE_MODE_STANDARD;
    return (status->bits & TTAK_DETACHABLE_DETACH_NOCHECK)
               ? TTAK_DETACHABLE_MODE_DETACHED
               : TTAK_DETACHABLE_MODE_STANDARD;
}

static bool ttak_detachable_validate_status(ttak_detachable_context_t *ctx,
                                            ttak_detach_status_t *status,
                                            ttak_detachable_mode_t *mode_out) {
    if (!ctx || !status || !mode_out) {
        return false;
    }

    if (!(status->bits & TTAK_DETACHABLE_STATUS_KNOWN)) {
        return false;
    }

    ttak_detachable_mode_t mode = ttak_detachable_mode_for_status(status);
    if (mode == TTAK_DETACHABLE_MODE_STANDARD) {
        if (!(status->bits & TTAK_DETACHABLE_ATTACH)) {
            return false;
        }
    } else {
        if ((status->bits & TTAK_DETACHABLE_ATTACH)) {
            return false;
        }
        if (!(ctx->flags & TTAK_ARENA_HAS_EPOCH_RECLAMATION)) {
            return false;
        }
    }

    *mode_out = mode;
    return true;
}

#ifndef _WIN32
int ttak_hard_kill_graceful_exit(sigset_t signals, int *ret) {
    return ttak_hard_kill_configure(signals, ret, true);
}

int ttak_hard_kill_exit(sigset_t signals, int *ret) {
    return ttak_hard_kill_configure(signals, ret, false);
}
#endif

static void ttak_detachable_register_context(ttak_detachable_context_t *ctx) {
    pthread_mutex_lock(&g_ctx_registry_lock);
    if (g_ctx_registry_len == g_ctx_registry_cap) {
        size_t new_cap = g_ctx_registry_cap ? g_ctx_registry_cap * 2 : 8;
        ttak_detachable_context_t **tmp = realloc(g_ctx_registry, new_cap * sizeof(*tmp));
        if (!tmp) {
            pthread_mutex_unlock(&g_ctx_registry_lock);
            return;
        }
        g_ctx_registry = tmp;
        g_ctx_registry_cap = new_cap;
    }
    g_ctx_registry[g_ctx_registry_len++] = ctx;
    pthread_mutex_unlock(&g_ctx_registry_lock);
}

static void ttak_detachable_unregister_context(ttak_detachable_context_t *ctx) {
    pthread_mutex_lock(&g_ctx_registry_lock);
    for (size_t i = 0; i < g_ctx_registry_len; ++i) {
        if (g_ctx_registry[i] == ctx) {
            g_ctx_registry[i] = g_ctx_registry[g_ctx_registry_len - 1];
            g_ctx_registry_len--;
            break;
        }
    }
    pthread_mutex_unlock(&g_ctx_registry_lock);
}

static void ttak_detachable_row_alloc(ttak_detachable_generation_row_t *row) {
    if (!row->columns) {
        row->cap = TTAK_DETACHABLE_GENERATIONS;
        row->columns = calloc(row->cap, sizeof(void *));
        if (row->columns == NULL) {
            row->cap = 0;
        }
        row->len = 0;
    }
}

static void ttak_detachable_row_flush(ttak_detachable_context_t *ctx, ttak_detachable_generation_row_t *row) {
    if (!row->columns) return;
    for (size_t i = 0; i < row->len; ++i) {
        void *ptr = row->columns[i];
        if (!ptr) continue;
        if (ctx->flags & TTAK_ARENA_HAS_EPOCH_RECLAMATION) {
            ttak_epoch_retire(ptr, free);
        } else {
            free(ptr);
        }
        row->columns[i] = NULL;
    }
    row->len = 0;
}

static void ttak_detachable_quarantine_flush(ttak_detachable_context_t *ctx) {
    if (!ctx || !ctx->quarantine.columns || !ctx->quarantine.sizes) return;
    for (size_t i = 0; i < ctx->quarantine.len; ++i) {
        void *ptr = ctx->quarantine.columns[i];
        if (!ptr) continue;
        if (ctx->flags & TTAK_ARENA_HAS_EPOCH_RECLAMATION) {
            ttak_epoch_retire(ptr, free);
        } else {
            free(ptr);
        }
        ctx->quarantine.columns[i] = NULL;
        ctx->quarantine.sizes[i] = 0;
    }
    ctx->quarantine.len = 0;
    ctx->quarantine.bytes = 0;
}

static bool ttak_detachable_quarantine_push(ttak_detachable_context_t *ctx, void *ptr, size_t size) {
    if (!ctx || !ptr || size == 0) return false;

    if (ctx->quarantine.cap == 0) {
        size_t initial_cap = 64;
        ctx->quarantine.columns = calloc(initial_cap, sizeof(void *));
        ctx->quarantine.sizes = calloc(initial_cap, sizeof(size_t));
        if (!ctx->quarantine.columns || !ctx->quarantine.sizes) {
            free(ctx->quarantine.columns);
            free(ctx->quarantine.sizes);
            ctx->quarantine.columns = NULL;
            ctx->quarantine.sizes = NULL;
            ctx->quarantine.cap = 0;
            return false;
        }
        ctx->quarantine.cap = initial_cap;
    } else if (ctx->quarantine.len >= ctx->quarantine.cap) {
        size_t new_cap = ctx->quarantine.cap * 2;
        void **new_columns = realloc(ctx->quarantine.columns, new_cap * sizeof(void *));
        size_t *new_sizes = realloc(ctx->quarantine.sizes, new_cap * sizeof(size_t));
        if (!new_columns || !new_sizes) {
            if (new_columns) ctx->quarantine.columns = new_columns;
            if (new_sizes) ctx->quarantine.sizes = new_sizes;
            return false;
        }
        memset(new_columns + ctx->quarantine.cap, 0, (new_cap - ctx->quarantine.cap) * sizeof(void *));
        memset(new_sizes + ctx->quarantine.cap, 0, (new_cap - ctx->quarantine.cap) * sizeof(size_t));
        ctx->quarantine.columns = new_columns;
        ctx->quarantine.sizes = new_sizes;
        ctx->quarantine.cap = new_cap;
    }

    if (ctx->quarantine_byte_limit > 0 &&
        ctx->quarantine.bytes + size > ctx->quarantine_byte_limit) {
        ttak_detachable_quarantine_flush(ctx);
    }

    ctx->quarantine.columns[ctx->quarantine.len] = ptr;
    ctx->quarantine.sizes[ctx->quarantine.len] = size;
    ctx->quarantine.len++;
    ctx->quarantine.bytes += size;
    return true;
}

static void ttak_detachable_track_pointer(ttak_detachable_context_t *ctx, void *ptr) {
    if (!ptr) return;
    ttak_detachable_generation_row_t *row = &ctx->rows[ctx->active_row];
    ttak_detachable_row_alloc(row);

    if (row->len >= row->cap) {
        ttak_detachable_row_flush(ctx, row);
        size_t advance = ctx->epoch_delay ? ctx->epoch_delay : 1;
        ctx->active_row = (ctx->active_row + advance) % ctx->matrix_rows;
        row = &ctx->rows[ctx->active_row];
        ttak_detachable_row_alloc(row);
    }

    if (row->columns == NULL) return;
    row->columns[row->len++] = ptr;
}

static bool ttak_detachable_untrack_pointer(ttak_detachable_context_t *ctx, void *ptr) {
    if (!ptr) return false;
    for (size_t r = 0; r < ctx->matrix_rows; ++r) {
        ttak_detachable_generation_row_t *row = &ctx->rows[r];
        if (!row->columns) continue;
        for (size_t c = 0; c < row->len; ++c) {
            if (row->columns[c] == ptr) {
                row->columns[c] = NULL;
                return true;
            }
        }
    }
    return false;
}

static bool ttak_detachable_cache_store(ttak_detachable_context_t *ctx, ttak_detachable_cache_t *cache, void *ptr, size_t size) {
    if (!cache || !ptr) return false;
    pthread_mutex_lock(&cache->lock);
    if (!cache->slots || size > cache->chunk_size) {
        pthread_mutex_unlock(&cache->lock);
        return false;
    }

    if (cache->count == cache->capacity) {
        if (ctx->flags & TTAK_ARENA_IS_URGENT_TASK) {
            void *victim = cache->slots[cache->head];
            if (victim) {
                if (ctx->flags & TTAK_ARENA_HAS_EPOCH_RECLAMATION) {
                    ttak_epoch_retire(victim, free);
                } else {
                    free(victim);
                }
            }
            cache->head = (cache->head + 1) % cache->capacity;
            cache->count--;
        } else {
            cache->misses++;
            pthread_mutex_unlock(&cache->lock);
            return false;
        }
    } else {
        cache->hits++;
    }

    cache->slots[cache->tail] = ptr;
    cache->tail = (cache->tail + 1) % cache->capacity;
    cache->count++;
    pthread_mutex_unlock(&cache->lock);
    return true;
}

static void *ttak_detachable_cache_take(ttak_detachable_cache_t *cache, size_t requested) {
    if (!cache) return NULL;
    pthread_mutex_lock(&cache->lock);
    if (!cache->slots || cache->count == 0 || requested > cache->chunk_size) {
        cache->misses++;
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }

    void *ptr = cache->slots[cache->head];
    cache->slots[cache->head] = NULL;
    cache->head = (cache->head + 1) % cache->capacity;
    cache->count--;
    cache->hits++;
    pthread_mutex_unlock(&cache->lock);

    if (ptr) {
        ttak_mem_stream_zero(ptr, cache->chunk_size);
    }
    return ptr;
}

static void ttak_detachable_cache_drain(ttak_detachable_context_t *ctx, ttak_detachable_cache_t *cache, bool release_storage) {
    if (!cache) return;
    pthread_mutex_lock(&cache->lock);
    void **slots = cache->slots;
    size_t cap = cache->capacity;
    if (slots) {
        for (size_t i = 0; i < cap; ++i) {
            void *ptr = slots[i];
            if (!ptr) continue;
            if (ctx && (ctx->flags & TTAK_ARENA_HAS_EPOCH_RECLAMATION)) {
                ttak_epoch_retire(ptr, free);
            } else {
                free(ptr);
            }
            slots[i] = NULL;
        }
    }
    cache->head = cache->tail = cache->count = 0;
    if (release_storage) {
        cache->slots = NULL;
        cache->capacity = 0;
    }
    pthread_mutex_unlock(&cache->lock);

    if (release_storage) {
        free(slots);
    }
}

#if !defined(_WIN32)
static void ttak_detachable_global_shutdown(_Bool flush_rows) {
    pthread_mutex_lock(&g_ctx_registry_lock);
    for (size_t i = 0; i < g_ctx_registry_len; ++i) {
        ttak_detachable_context_t *ctx = g_ctx_registry[i];
        if (!ctx) continue;
        ttak_detachable_cache_drain(ctx, &ctx->small_cache, false);
        if (flush_rows) {
            for (size_t r = 0; r < ctx->matrix_rows; ++r) {
                ttak_detachable_row_flush(ctx, &ctx->rows[r]);
            }
        }
    }
    pthread_mutex_unlock(&g_ctx_registry_lock);
}
#endif

#if !defined(_WIN32) && !defined(EMBEDDED_BAREMETAL)
static void ttak_detachable_signal_handler(int signo) {
    bool expected = false;
    if (!atomic_compare_exchange_weak(&g_signal_guard.triggered, &expected, true)) {
        _exit(g_signal_guard.exit_code >= 0 ? g_signal_guard.exit_code : signo);
    }

    if (g_signal_guard.graceful) {
        ttak_detachable_global_shutdown(true);
    } else {
        ttak_detachable_global_shutdown(false);
    }

    int code = (g_signal_guard.exit_code >= 0) ? g_signal_guard.exit_code : signo;
    _exit(code);
}

static int ttak_hard_kill_configure(sigset_t signals, int *ret, _Bool graceful) {
    g_signal_guard.graceful = graceful;
    g_signal_guard.exit_code = ret ? *ret : -1;
    atomic_store(&g_signal_guard.triggered, false);

    struct sigaction sa;
    ttak_mem_stream_zero(&sa, sizeof(sa));
    sa.sa_handler = ttak_detachable_signal_handler;
    sigemptyset(&sa.sa_mask);
#ifdef SA_RESTART
    sa.sa_flags = SA_RESTART;
#endif

    for (int sig = 1; sig < NSIG; ++sig) {
        if (sigismember(&signals, sig) == 1) {
            if (sigaction(sig, &sa, NULL) != 0) {
                return -1;
            }
        }
    }

    return 0;
}
#endif
