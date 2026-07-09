#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ttak/mem/mem.h>
#include <ttak/mem/epoch.h>
#include <ttak/mols_control.h>
#include <ttak/timing/timing.h>
#include <ttak/types/ttak_compiler.h>

#ifndef _MSC_VER
#include <stdatomic.h>
#else
#include <windows.h>
#endif

#include <pthread.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

/**
 * @file epoch.c
 * @brief Epoch-Based Reclamation (EBR) using a 16x16 OLS mapping.
 *
 * This implementation provides:
 * - Thread registration with a logical TID assignment.
 * - Per-thread epoch tracking for quiescent state detection.
 * - Retire lists stored in an OLS grid (buckets[x][y]) indexed by (tid, gen).
 * - A reclaim step that advances the global epoch and frees retired nodes when safe.
 *
 * Cross-platform strategy:
 * - On GCC/Clang: uses C11 <stdatomic.h>, _Thread_local, and GNU attributes.
 * - On MSVC: provides minimal atomics shims via Interlocked* and MemoryBarrier(),
 *   and disables GNU attribute decorations while keeping lazy init logic.
 */

/* --- Configuration --- */

#define OLS_ORDER 16
#define TTAK_EPOCH_NODE_POOL_LIMIT 16384U

/* --- Cross-compiler attributes and TLS --- */

#define TTAK_STRINGIFY_INNER(x) #x
#define TTAK_STRINGIFY(x) TTAK_STRINGIFY_INNER(x)

#if defined(_MSC_VER)
#define TTAK_VIS_DEFAULT
#define TTAK_CONSTRUCTOR(prio)
#elif defined(__TINYC__)
#define TTAK_VIS_DEFAULT __attribute__((visibility("default")))
#define TTAK_CONSTRUCTOR(prio)                                                      \
    __attribute__((constructor))                                                    \
    __attribute__((used))                                                           \
    __attribute__((section(".init_array.0" TTAK_STRINGIFY(prio))))
#else
#define TTAK_VIS_DEFAULT __attribute__((visibility("default")))
#define TTAK_CONSTRUCTOR(prio) __attribute__((constructor(prio)))
#endif

/* --- Minimal atomics abstraction --- */

#if defined(_MSC_VER)

#ifndef _Atomic
#define _Atomic volatile
#endif

static __forceinline void tt_atomic_fence(memory_order order) {
    (void)order;
    MemoryBarrier();
}

static __forceinline void *tt_atomic_load_ptr(void * _Atomic *obj, memory_order order) {
    (void)order;
    tt_atomic_fence(memory_order_seq_cst);
    return (void *)(*obj);
}

static __forceinline void tt_atomic_store_ptr(void * _Atomic *obj, void *val, memory_order order) {
    (void)order;
    tt_atomic_fence(memory_order_seq_cst);
    *obj = val;
    tt_atomic_fence(memory_order_seq_cst);
}

static __forceinline void *tt_atomic_exchange_ptr(void * _Atomic *obj, void *val, memory_order order) {
    (void)order;
    return InterlockedExchangePointer((void *volatile *)obj, val);
}

static __forceinline bool tt_atomic_cas_weak_ptr(void * _Atomic *obj, void **expected, void *desired,
                                                 memory_order success, memory_order failure) {
    (void)success;
    (void)failure;
    void *prev = InterlockedCompareExchangePointer((void *volatile *)obj, desired, *expected);
    if (prev == *expected) {
        return true;
    }
    *expected = prev;
    return false;
}

static __forceinline uint32_t tt_atomic_load_u32(uint32_t _Atomic *obj, memory_order order) {
    (void)order;
    tt_atomic_fence(memory_order_seq_cst);
    return (uint32_t)(*obj);
}

static __forceinline void tt_atomic_store_u32(uint32_t _Atomic *obj, uint32_t val, memory_order order) {
    (void)order;
    tt_atomic_fence(memory_order_seq_cst);
    *obj = val;
    tt_atomic_fence(memory_order_seq_cst);
}

static __forceinline uint32_t tt_atomic_fetch_add_u32(uint32_t _Atomic *obj, uint32_t val, memory_order order) {
    (void)order;
    return (uint32_t)InterlockedExchangeAdd((volatile long *)obj, (long)val);
}

static __forceinline bool tt_atomic_load_bool(bool _Atomic *obj, memory_order order) {
    (void)order;
    tt_atomic_fence(memory_order_seq_cst);
    return (*obj) ? true : false;
}

static __forceinline void tt_atomic_store_bool(bool _Atomic *obj, bool val, memory_order order) {
    (void)order;
    tt_atomic_fence(memory_order_seq_cst);
    *obj = val ? true : false;
    tt_atomic_fence(memory_order_seq_cst);
}

#define TT_ATOMIC_LOAD_PTR(p, order)            tt_atomic_load_ptr((void * _Atomic *)(p), (order))
#define TT_ATOMIC_STORE_PTR(p, v, order)        tt_atomic_store_ptr((void * _Atomic *)(p), (void *)(v), (order))
#define TT_ATOMIC_XCHG_PTR(p, v, order)         tt_atomic_exchange_ptr((void * _Atomic *)(p), (void *)(v), (order))
#define TT_ATOMIC_CAS_WEAK_PTR(p, e, d, so, fo) tt_atomic_cas_weak_ptr((void * _Atomic *)(p), (void **)(e), (void *)(d), (so), (fo))

#define TT_ATOMIC_LOAD_U32(p, order)            tt_atomic_load_u32((uint32_t _Atomic *)(p), (order))
#define TT_ATOMIC_STORE_U32(p, v, order)        tt_atomic_store_u32((uint32_t _Atomic *)(p), (uint32_t)(v), (order))
#define TT_ATOMIC_FETCH_ADD_U32(p, v, order)    tt_atomic_fetch_add_u32((uint32_t _Atomic *)(p), (uint32_t)(v), (order))

#define TT_ATOMIC_LOAD_BOOL(p, order)           tt_atomic_load_bool((bool _Atomic *)(p), (order))
#define TT_ATOMIC_STORE_BOOL(p, v, order)       tt_atomic_store_bool((bool _Atomic *)(p), (bool)(v), (order))

#define TT_ATOMIC_FENCE(order)                  tt_atomic_fence((order))

#else

#define TT_ATOMIC_LOAD_PTR(p, order)            atomic_load_explicit((p), (order))
#define TT_ATOMIC_STORE_PTR(p, v, order)        atomic_store_explicit((p), (v), (order))
#define TT_ATOMIC_XCHG_PTR(p, v, order)         atomic_exchange_explicit((p), (v), (order))
#define TT_ATOMIC_CAS_WEAK_PTR(p, e, d, so, fo) atomic_compare_exchange_weak_explicit((p), (e), (d), (so), (fo))

#define TT_ATOMIC_LOAD_U32(p, order)            atomic_load_explicit((p), (order))
#define TT_ATOMIC_STORE_U32(p, v, order)        atomic_store_explicit((p), (v), (order))
#define TT_ATOMIC_FETCH_ADD_U32(p, v, order)    atomic_fetch_add_explicit((p), (v), (order))

#define TT_ATOMIC_LOAD_BOOL(p, order)           atomic_load_explicit((p), (order))
#define TT_ATOMIC_STORE_BOOL(p, v, order)       atomic_store_explicit((p), (v), (order))

#define TT_ATOMIC_FENCE(order)                  atomic_thread_fence((order))

#endif

/* --- OLS plane --- */

/**
 * @brief 16x16 OLS mapping plane containing retired pointer lists.
 *
 * buckets[x][y] is a Treiber stack of ttak_retired_node_t, accessed atomically.
 * occupancy[] is initialized to zero and may be used for debugging/visibility.
 */
typedef struct {
    ttak_retired_node_t * _Atomic buckets[OLS_ORDER][OLS_ORDER];
    size_t _Atomic occupancy[OLS_ORDER];
} ttak_ols_plane_t;

/* --- Global state --- */

/**
 * @brief Global epoch manager state.
 *
 * Expected to contain at least an atomic global epoch counter. The exact definition
 * is provided by <ttak/mem/epoch.h>.
 */
TTAK_VIS_DEFAULT ttak_epoch_manager_t g_epoch_mgr = {0};

/**
 * @brief Lock-free cache of retired nodes to avoid alloc/free flapping.
 */
static ttak_retired_node_t * _Atomic g_retired_node_pool = NULL;
static uint32_t _Atomic g_retired_node_pool_count = 0;

/**
 * @brief Global OLS plane storage.
 *
 * Left implicitly zero-initialized because the first member is an array of
 * atomic pointers.  Some toolchains reject a brace-enclosed `{0}` initializer
 * for an _Atomic pointer array element.
 */
TTAK_VIS_DEFAULT ttak_ols_plane_t g_ols_static_plane;

/**
 * @brief Indicates whether the OLS plane and epoch subsystem have been initialized.
 */
TTAK_VIS_DEFAULT bool _Atomic g_epoch_init_ready = false;

/**
 * @brief Global lock protecting reclaim execution (prevents concurrent reclaimers).
 */
static pthread_mutex_t g_reclaim_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Linked list node used to track registered threads.
 */
typedef struct ttak_thread_node {
    ttak_thread_state_t *state;
    uint32_t logical_tid;
    struct ttak_thread_node *next;
} ttak_thread_node_t;

/**
 * @brief Per-thread state pointer.
 *
 * In non-TinyCC builds we use TLS storage. For TinyCC, a single global pointer is used.
 * That mode is not thread-safe but preserves buildability in constrained toolchains.
 */
/* Tiny C does not support _Thread_local */
#if defined(__TINYC__)
TTAK_VIS_DEFAULT ttak_thread_state_t *t_local_state_unused = NULL;
static pthread_key_t g_tls_key;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;
static void _ttak_tls_init(void) { pthread_key_create(&g_tls_key, NULL); }
ttak_thread_state_t *ttak_get_t_local_state(void) {
    pthread_once(&g_tls_once, _ttak_tls_init);
    return (ttak_thread_state_t *)pthread_getspecific(g_tls_key);
}
void ttak_set_t_local_state(ttak_thread_state_t *val) {
    pthread_once(&g_tls_once, _ttak_tls_init);
    pthread_setspecific(g_tls_key, val);
}
#else
TTAK_VIS_DEFAULT _Thread_local ttak_thread_state_t *t_local_state = NULL;
TTAK_VIS_DEFAULT ttak_thread_state_t *ttak_get_t_local_state(void) {
    return t_local_state;
}
TTAK_VIS_DEFAULT void ttak_set_t_local_state(ttak_thread_state_t *val) {
    t_local_state = val;
}
#endif

/**
 * @brief Head of the registered thread list (atomic pointer).
 */
TTAK_VIS_DEFAULT ttak_thread_node_t * _Atomic g_thread_list = NULL;

/**
 * @brief Logical thread ID counter (atomic).
 */
TTAK_VIS_DEFAULT uint32_t _Atomic g_tid_counter = 0;

/* --- Retired node cache helpers --- */

static inline ttak_retired_node_t *epoch_node_pool_acquire(void) {
    while (1) {
        ttak_retired_node_t *head =
            (ttak_retired_node_t *)TT_ATOMIC_LOAD_PTR((void * _Atomic *)&g_retired_node_pool,
                                                      memory_order_acquire);
        if (!head) {
            break;
        }
        ttak_retired_node_t *next = head->next;
        void *expected = head;
        if (TT_ATOMIC_CAS_WEAK_PTR((void * _Atomic *)&g_retired_node_pool, &expected, next,
                                   memory_order_acq_rel, memory_order_acquire)) {
            uint32_t prev = TT_ATOMIC_FETCH_ADD_U32(&g_retired_node_pool_count, (uint32_t)-1,
                                                    memory_order_relaxed);
            if (TTAK_UNLIKELY(prev == 0)) {
                TT_ATOMIC_FETCH_ADD_U32(&g_retired_node_pool_count, 1, memory_order_relaxed);
            }
            head->next = NULL;
            return head;
        }
    }
    return (ttak_retired_node_t *)ttak_dangerous_calloc(1, sizeof(ttak_retired_node_t));
}

static inline void epoch_node_pool_release(ttak_retired_node_t *node) {
    if (!node) {
        return;
    }
    node->ptr = NULL;
    node->cleanup = NULL;

    uint32_t cached = TT_ATOMIC_LOAD_U32(&g_retired_node_pool_count, memory_order_relaxed);
    if (cached >= TTAK_EPOCH_NODE_POOL_LIMIT) {
        ttak_dangerous_free(node);
        return;
    }

    while (1) {
        ttak_retired_node_t *head =
            (ttak_retired_node_t *)TT_ATOMIC_LOAD_PTR((void * _Atomic *)&g_retired_node_pool,
                                                      memory_order_acquire);
        node->next = head;
        void *expected = head;
        if (TT_ATOMIC_CAS_WEAK_PTR((void * _Atomic *)&g_retired_node_pool, &expected, node,
                                   memory_order_release, memory_order_acquire)) {
            TT_ATOMIC_FETCH_ADD_U32(&g_retired_node_pool_count, 1, memory_order_relaxed);
            return;
        }
    }
}

/* --- Internal helpers --- */

/**
 * @brief Compute OLS coordinates based on TID and generation value.
 *
 * @param tid Logical thread identifier.
 * @param gen Current generation (epoch).
 * @param x Output X coordinate.
 * @param y Output Y coordinate.
 */
static inline void ttak_get_ols_coords(uint32_t tid, uint32_t gen, int *x, int *y) {
    *x = (int)(tid % OLS_ORDER);
    *y = (int)((tid / OLS_ORDER + gen) % OLS_ORDER);
    const uint32_t row = (uint32_t)(*x) & (uint32_t)TTAK_MOLS_SYMBOL_MASK;
    const uint32_t col = (uint32_t)(*y) & (uint32_t)TTAK_MOLS_SYMBOL_MASK;
    const uint16_t node_id = (uint16_t)((row << TTAK_MOLS_COORD_SHIFT) | col);
    const uint32_t combined =
        (row << TTAK_MOLS_COORD_SHIFT) | col;
    const uint32_t adjusted = ttak_apply_mols_control(node_id, combined);
    *x = (int)((adjusted >> TTAK_MOLS_COORD_SHIFT) & (uint32_t)(OLS_ORDER - 1));
    *y = (int)(adjusted & (uint32_t)(OLS_ORDER - 1));
}

/* --- Initialization --- */

/**
 * @brief Initialize the OLS plane and mark the subsystem ready.
 *
 * On GCC/Clang, this function is also tagged as a constructor for early init.
 * On MSVC, constructor decoration is disabled; lazy init is performed by callers.
 */
TTAK_CONSTRUCTOR(101)
void ttak_epoch_subsystem_init(void) {
    if (TT_ATOMIC_LOAD_BOOL(&g_epoch_init_ready, memory_order_seq_cst)) {
        return;
    }

    for (int i = 0; i < OLS_ORDER; i++) {
        for (int j = 0; j < OLS_ORDER; j++) {
#if defined(_MSC_VER)
            TT_ATOMIC_STORE_PTR((void * _Atomic *)&g_ols_static_plane.buckets[i][j], NULL, memory_order_seq_cst);
#else
            atomic_init(&g_ols_static_plane.buckets[i][j], NULL);
#endif
        }
#if defined(_MSC_VER)
        g_ols_static_plane.occupancy[i] = 0;
#else
        atomic_init(&g_ols_static_plane.occupancy[i], 0);
#endif
    }

    TT_ATOMIC_STORE_BOOL(&g_epoch_init_ready, true, memory_order_seq_cst);
}

/* --- Thread registration --- */

/**
 * @brief Register the current thread with the epoch subsystem.
 *
 * Allocates a per-thread state and publishes it to the global thread list.
 * This function is idempotent for the current thread.
 */
void ttak_epoch_register_thread(void) {
#if defined(__TINYC__)
    ttak_thread_state_t *st = ttak_get_t_local_state();
    if (st) return;
#else
    if (t_local_state) return;
#endif

    if (!TT_ATOMIC_LOAD_BOOL(&g_epoch_init_ready, memory_order_seq_cst)) {
        ttak_epoch_subsystem_init();
    }

    ttak_thread_state_t *new_st = (ttak_thread_state_t *)ttak_dangerous_calloc(1, sizeof(ttak_thread_state_t));
    if (!new_st) {
        return;
    }

#if defined(__TINYC__)
    ttak_set_t_local_state(new_st);
#else
    t_local_state = new_st;
#endif

#if defined(_MSC_VER)
    new_st->local_epoch = 0;
    new_st->active = false;
#else
    atomic_init(&new_st->local_epoch, 0);
    atomic_init(&new_st->active, false);
#endif

    ttak_thread_node_t *node = (ttak_thread_node_t *)ttak_dangerous_calloc(1, sizeof(ttak_thread_node_t));
    if (!node) {
#if defined(__TINYC__)
        ttak_set_t_local_state(NULL);
#else
        t_local_state = NULL;
#endif
        return;
    }

    node->state = new_st;
    node->logical_tid = TT_ATOMIC_FETCH_ADD_U32(&g_tid_counter, 1, memory_order_relaxed);
    new_st->logical_tid = node->logical_tid;

    ttak_thread_node_t *old_head;
    do {
        old_head = (ttak_thread_node_t *)TT_ATOMIC_LOAD_PTR((void * _Atomic *)&g_thread_list, memory_order_acquire);
        node->next = old_head;

        void *expected = old_head;
        if (TT_ATOMIC_CAS_WEAK_PTR((void * _Atomic *)&g_thread_list, &expected, node,
                                  memory_order_release, memory_order_acquire)) {
            break;
        }
    } while (1);
}

/**
 * @brief Deregister the current thread from epoch tracking.
 *
 * This marks the thread inactive and drops the TLS pointer. The allocated state is
 * intentionally not freed to avoid races with concurrent reclaim operations.
 */
void ttak_epoch_deregister_thread(void) {
#if defined(__TINYC__)
    ttak_thread_state_t *st = ttak_get_t_local_state();
    if (!st) return;
    atomic_store_explicit(&st->active, false, memory_order_seq_cst);
    ttak_set_t_local_state(NULL);
#else
    if (!t_local_state) {
        return;
    }

#if defined(_MSC_VER)
    t_local_state->active = false;
#else
    atomic_store_explicit(&t_local_state->active, false, memory_order_seq_cst);
#endif

    t_local_state = NULL;
#endif
}

/* --- Epoch enter/exit is now inlined in include/ttak/mem/epoch.h --- */

/* --- Retirement --- */

/**
 * @brief Retire a pointer for deferred reclamation.
 *
 * The retired node is pushed into the OLS bucket selected by the current
 * thread TID and current global epoch value.
 *
 * @param ptr Pointer to retire.
 * @param cleanup Optional cleanup callback to run before node memory is freed.
 */
void ttak_epoch_retire(void *ptr, void (*cleanup)(void *)) {
    if (!ptr) {
        return;
    }
    if (!t_local_state) {
        ttak_epoch_register_thread();
    }
    if (!t_local_state) {
        return;
    }

    ttak_retired_node_t *node = epoch_node_pool_acquire();
    if (!node) {
        return;
    }

    node->ptr = ptr;
    node->cleanup = cleanup;

    uint32_t gen = TT_ATOMIC_LOAD_U32(&g_epoch_mgr.global_epoch, memory_order_acquire);
    uint32_t tid = t_local_state->logical_tid;

    int x, y;
    ttak_get_ols_coords(tid, gen, &x, &y);

    ttak_retired_node_t *old_head;
    do {
        old_head = (ttak_retired_node_t *)TT_ATOMIC_LOAD_PTR((void * _Atomic *)&g_ols_static_plane.buckets[x][y],
                                                             memory_order_acquire);
        node->next = old_head;

        void *expected = old_head;
        if (TT_ATOMIC_CAS_WEAK_PTR((void * _Atomic *)&g_ols_static_plane.buckets[x][y], &expected, node,
                                  memory_order_release, memory_order_acquire)) {
            break;
        }
    } while (1);
}

/* --- Reclamation --- */

/**
 * @brief Attempt to reclaim retired nodes if it is safe to advance the epoch.
 *
 * A reclamation is safe if all active threads have local_epoch >= current global_epoch.
 * If safe, the global epoch is incremented and all OLS buckets are drained.
 */
void ttak_epoch_reclaim(void) {
    if (!TT_ATOMIC_LOAD_BOOL(&g_epoch_init_ready, memory_order_seq_cst)) {
        return;
    }

    if (pthread_mutex_trylock(&g_reclaim_lock) != 0) {
        return;
    }

    uint32_t current = TT_ATOMIC_LOAD_U32(&g_epoch_mgr.global_epoch, memory_order_acquire);
    bool safe = true;

    TT_ATOMIC_FENCE(memory_order_seq_cst);

    ttak_thread_node_t *node = (ttak_thread_node_t *)TT_ATOMIC_LOAD_PTR((void * _Atomic *)&g_thread_list,
                                                                        memory_order_acquire);
    while (node) {
#if defined(_MSC_VER)
        bool active = node->state->active ? true : false;
        uint32_t local_epoch = (uint32_t)node->state->local_epoch;
#else
        bool active = atomic_load_explicit(&node->state->active, memory_order_acquire);
        uint32_t local_epoch = atomic_load_explicit(&node->state->local_epoch, memory_order_acquire);
#endif

        if (active) {
            if (local_epoch < current) {
                safe = false;
                break;
            }
        }
        node = node->next;
    }

    if (safe) {
        (void)TT_ATOMIC_FETCH_ADD_U32(&g_epoch_mgr.global_epoch, 1, memory_order_acq_rel);

        for (int i = 0; i < OLS_ORDER; i++) {
            for (int j = 0; j < OLS_ORDER; j++) {
                ttak_retired_node_t *to_free =
                    (ttak_retired_node_t *)TT_ATOMIC_XCHG_PTR((void * _Atomic *)&g_ols_static_plane.buckets[i][j],
                                                             NULL, memory_order_acq_rel);

                while (to_free) {
                    ttak_retired_node_t *next = to_free->next;
                    if (to_free->cleanup) {
                        to_free->cleanup(to_free->ptr);
                    }
                    epoch_node_pool_release(to_free);
                    to_free = next;
                }
            }
        }
    }

    pthread_mutex_unlock(&g_reclaim_lock);
}
