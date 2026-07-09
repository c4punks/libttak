#include <ttak/mem/epoch_gc.h>
#include <ttak/mem/mem.h>
#include <ttak/mem/epoch.h>
#include <ttak/timing/timing.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
static inline void epoch_gc_clock_gettime(struct timespec *spec) {
    FILETIME ft;
    uint64_t tim;
    GetSystemTimePreciseAsFileTime(&ft);
    tim = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
    tim -= 116444736000000000ULL; /* Windows epoch diff */
    spec->tv_sec = (time_t)(tim / 1000000000ULL);
    spec->tv_nsec = (long)((tim % 1000000000ULL) * 100);
}
#elif defined(EMBEDDED_BAREMETAL)
static inline void epoch_gc_clock_gettime(struct timespec *spec) {
    uint64_t ns = ttak_get_tick_count_ns();
    spec->tv_sec = (long)(ns / 1000000000ULL);
    spec->tv_nsec = (long)(ns % 1000000000ULL);
}
#else
#  define epoch_gc_clock_gettime(spec) clock_gettime(CLOCK_MONOTONIC, (spec))
#endif

/* Relaxed cadence: 20 Hz min, 0.5 Hz max.  The memory manager provides
 * hints (alloc/free/realloc/idle) so the thread does not have to spin. */
#define TTAK_EPOCH_GC_MIN_ROTATE_NS TT_MILLI_SECOND(50)
#define TTAK_EPOCH_GC_MAX_ROTATE_NS TT_SECOND(2)

static void *epoch_gc_rotate_thread(void *arg) {
    ttak_epoch_gc_t *gc = (ttak_epoch_gc_t *)arg;
    if (!gc) return NULL;

    uint64_t sleep_ns = atomic_load(&gc->min_rotate_ns);

    while (!atomic_load(&gc->shutdown_requested)) {
        uint32_t hints = atomic_exchange(&gc->pending_hints, 0);
        _Bool manual = atomic_load(&gc->manual_rotation);

        if (!manual) {
            if (hints & TTAK_EPOCH_GC_HINT_COLLECT_NOW) {
                ttak_epoch_gc_rotate(gc);
                ttak_epoch_reclaim();
                sleep_ns = atomic_load(&gc->min_rotate_ns);
            } else if (hints & (TTAK_EPOCH_GC_HINT_ALLOC | TTAK_EPOCH_GC_HINT_FREE | TTAK_EPOCH_GC_HINT_REALLOC)) {
                ttak_epoch_gc_rotate(gc);
                ttak_epoch_reclaim();
                sleep_ns = atomic_load(&gc->min_rotate_ns);
            } else {
                // Idle: back off gracefully, but still rotate/reclaim at
                // the current (possibly elongated) interval.
                sleep_ns += atomic_load(&gc->min_rotate_ns);
                uint64_t max_ns = atomic_load(&gc->max_rotate_ns);
                if (sleep_ns > max_ns) sleep_ns = max_ns;

                ttak_epoch_gc_rotate(gc);
                ttak_epoch_reclaim();
            }
        } else {
            sleep_ns = atomic_load(&gc->max_rotate_ns);
        }

        pthread_mutex_lock(&gc->rotate_lock);
        if (!atomic_load(&gc->shutdown_requested)) {
            struct timespec ts;
            epoch_gc_clock_gettime(&ts);
            uint64_t total_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec + sleep_ns;
            ts.tv_sec = (time_t)(total_ns / 1000000000ULL);
            ts.tv_nsec = (long)(total_ns % 1000000000ULL);
            pthread_cond_timedwait(&gc->rotate_cond, &gc->rotate_lock, &ts);
        }
        pthread_mutex_unlock(&gc->rotate_lock);
    }

    return NULL;
}

/**
 * @brief Initializes the Epoch GC structure.
 * 
 * Sets up the underlying memory tree with manual cleanup mode enabled,
 * allowing the user to control the garbage collection cycles via ttak_epoch_gc_rotate.
 * 
 * @param gc Pointer to the GC context.
 */
void ttak_epoch_gc_init(ttak_epoch_gc_t *gc) {
    ttak_mem_tree_init(&gc->tree);
    // The epoch GC rotate thread drives cleanup manually via
    // ttak_mem_tree_perform_cleanup.  Suppress the mem_tree's own
    // background thread so we do not spawn two threads per context.
    ttak_mem_tree_set_manual_cleanup(&gc->tree, true);

    atomic_store(&gc->current_epoch, 0);
    atomic_store(&gc->last_cleanup_ts, ttak_get_tick_count());

    pthread_mutex_init(&gc->rotate_lock, NULL);
    pthread_cond_init(&gc->rotate_cond, NULL);
    atomic_store(&gc->shutdown_requested, false);
    atomic_store(&gc->manual_rotation, true);
    atomic_store(&gc->min_rotate_ns, TTAK_EPOCH_GC_MIN_ROTATE_NS);
    atomic_store(&gc->max_rotate_ns, TTAK_EPOCH_GC_MAX_ROTATE_NS);
    atomic_store(&gc->pending_hints, 0);
    /* Do not spawn a background rotate thread: it busy-waits in
     * ttak_epoch_reclaim and consumes whole cores.  Callers rotate/reclaim
     * explicitly. */
    gc->rotate_thread_started = false;
}

/**
 * @brief Destroys the GC context.
 * 
 * Frees all remaining memory blocks tracked by the tree.
 * 
 * @param gc Pointer to the GC context.
 */
void ttak_epoch_gc_destroy(ttak_epoch_gc_t *gc) {
    atomic_store(&gc->shutdown_requested, true);
    pthread_mutex_lock(&gc->rotate_lock);
    pthread_cond_signal(&gc->rotate_cond);
    pthread_mutex_unlock(&gc->rotate_lock);

    if (gc->rotate_thread_started) {
        pthread_join(gc->rotate_thread, NULL);
    }

    pthread_cond_destroy(&gc->rotate_cond);
    pthread_mutex_destroy(&gc->rotate_lock);

    ttak_mem_tree_destroy(&gc->tree);
}

/**
 * @brief Registers a memory block with the current epoch.
 * 
 * @param gc Pointer to the GC context.
 * @param ptr Pointer to the memory block.
 * @param size Size of the block.
 */
void ttak_epoch_gc_register(ttak_epoch_gc_t *gc, void *ptr, size_t size) {
    // Add to the underlying mem_tree.
    // The 'expires_tick' (4th arg) is 0 because we manage lifetime via epochs,
    // not strictly by clock time, though mem_tree supports both.
    // We treat these as "roots" for the current epoch.
    ttak_mem_tree_add(&gc->tree, ptr, size, 0, true);
}

/**
 * @brief Rotates the epoch and performs cleanup.
 * 
 * Increments the epoch counter and triggers the memory tree's cleanup logic.
 * This function iterates through the tree, identifying blocks that are no longer
 * referenced or valid, and frees them. It is designed to be called periodically.
 * 
 * @param gc Pointer to the GC context.
 */
void ttak_epoch_gc_rotate(ttak_epoch_gc_t *gc) {
    if (!gc) return;

    atomic_fetch_add(&gc->current_epoch, 1);

    uint64_t now = ttak_get_tick_count();
    // Perform cleanup using the current timestamp.
    // mem_tree_perform_cleanup iterates the tree and removes expired/unreferenced nodes.
    // By controlling when this is called, we avoid "stop-the-world" pauses at arbitrary times.
    ttak_mem_tree_perform_cleanup(&gc->tree, now);

    atomic_store(&gc->last_cleanup_ts, now);
}

void ttak_epoch_gc_manual_rotate(ttak_epoch_gc_t *gc, _Bool manual_mode) {
    if (!gc) return;
    atomic_store(&gc->manual_rotation, manual_mode);
    pthread_mutex_lock(&gc->rotate_lock);
    pthread_cond_signal(&gc->rotate_cond);
    pthread_mutex_unlock(&gc->rotate_lock);
}

void ttak_epoch_gc_hint(ttak_epoch_gc_t *gc, ttak_epoch_gc_hint_t hint) {
    if (!gc) return;
    atomic_fetch_or(&gc->pending_hints, (uint32_t)hint);
    if (hint == TTAK_EPOCH_GC_HINT_COLLECT_NOW) {
        pthread_mutex_lock(&gc->rotate_lock);
        pthread_cond_signal(&gc->rotate_cond);
        pthread_mutex_unlock(&gc->rotate_lock);
    }
}
