#ifndef TTAK_MEM_EPOCH_H
#define TTAK_MEM_EPOCH_H

#include <stdbool.h>
#include <stdint.h>
#include <ttak/compat/stdatomic.h>
#include <ttak/types/ttak_compiler.h>

#define TTAK_EPOCH_SESSIONS 3

typedef struct ttak_retired_node {
    void *ptr;
    void (*cleanup)(void *);
    struct ttak_retired_node *next;
    unsigned int epoch; /**< Global epoch when the node was retired. */
} ttak_retired_node_t;

/**
 * Epochs a retired node must age before reclamation may free it. A reader
 * that entered in epoch E may still hold a pointer unlinked and retired
 * during E, so a node retired in E is freed only once the global epoch has
 * reached E + TTAK_EPOCH_GRACE (every active reader has then entered after
 * the node was unlinked).
 */
#define TTAK_EPOCH_GRACE 2u

typedef struct {
    unsigned int _Atomic global_epoch;
    ttak_retired_node_t * _Atomic retired_queues[TTAK_EPOCH_SESSIONS];
} ttak_epoch_manager_t;

typedef struct {
    unsigned int _Atomic local_epoch;
    bool _Atomic active;
    uint32_t logical_tid;
} ttak_thread_state_t;

extern ttak_epoch_manager_t g_epoch_mgr;

/**
 * @brief Registers the current thread with the epoch manager.
 */
void ttak_epoch_register_thread(void);

/**
 * @brief Deregisters the current thread.
 */
void ttak_epoch_deregister_thread(void);

#if TTAK_NO_NATIVE_TLS || defined(TTAK_EPOCH_FORCE_TLS_SHIM)
extern ttak_thread_state_t *ttak_get_t_local_state(void);
extern void ttak_set_t_local_state(ttak_thread_state_t *val);
#define t_local_state ttak_get_t_local_state()

#define ttak_epoch_enter() do { \
    ttak_thread_state_t *__st = ttak_get_t_local_state(); \
    if (TTAK_UNLIKELY(!__st)) { ttak_epoch_register_thread(); __st = ttak_get_t_local_state(); } \
    if (TTAK_LIKELY(__st)) { \
        unsigned int __current = atomic_load_explicit(&g_epoch_mgr.global_epoch, memory_order_acquire); \
        atomic_store_explicit(&__st->local_epoch, __current, memory_order_relaxed); \
        atomic_store_explicit(&__st->active, true, memory_order_seq_cst); \
    } \
} while(0)

#define ttak_epoch_exit() do { \
    ttak_thread_state_t *__st = ttak_get_t_local_state(); \
    if (TTAK_LIKELY(__st)) { \
        atomic_store_explicit(&__st->active, false, memory_order_release); \
    } \
} while(0)
#else
extern _Thread_local ttak_thread_state_t *t_local_state;

/**
 * @brief Enters a critical section protected by EBR.
 */
TTAK_FORCE_INLINE void ttak_epoch_enter(void) {
    if (TTAK_UNLIKELY(!t_local_state)) {
        ttak_epoch_register_thread();
    }
    if (TTAK_UNLIKELY(!t_local_state)) return;

    unsigned int current = atomic_load_explicit(&g_epoch_mgr.global_epoch, memory_order_acquire);
    atomic_store_explicit(&t_local_state->local_epoch, current, memory_order_relaxed);
    atomic_store_explicit(&t_local_state->active, true, memory_order_seq_cst);
}

/**
 * @brief Exits the EBR critical section.
 */
TTAK_FORCE_INLINE void ttak_epoch_exit(void) {
    if (TTAK_UNLIKELY(!t_local_state)) return;
    atomic_store_explicit(&t_local_state->active, false, memory_order_release);
}
#endif

/**
 * @brief Retires a pointer for deferred reclamation.
 * @param ptr The pointer to retire.
 * @param cleanup Optional cleanup function (e.g., free).
 */
void ttak_epoch_retire(void *ptr, void (*cleanup)(void *));

/**
 * @brief Attempts to reclaim memory from safe epochs.
 *
 * If no active thread is behind the global epoch, advances it by one and
 * frees every retired node that is at least TTAK_EPOCH_GRACE epochs old;
 * younger nodes stay queued for a later call. With no thread inside an
 * epoch, TTAK_EPOCH_GRACE consecutive calls free everything retired before
 * the first one.
 */
void ttak_epoch_reclaim(void);

#endif // TTAK_MEM_EPOCH_H
