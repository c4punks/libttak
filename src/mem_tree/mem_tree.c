#include <ttak/mem_tree/mem_tree.h>
#include <ttak/mem/mem.h> // For ttak_mem_free
#include <ttak/timing/timing.h> // For ttak_get_tick_count
#include "../../internal/app_types.h" // For TT_SECOND, etc.
#include <stdlib.h>
#include <string.h>
#include <stdio.h> // For debugging

#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0602
#  endif
#  include <windows.h>
#  include <stdint.h>
#endif

// Forward declaration for the cleanup thread function
static void *cleanup_thread_func(void *arg);

/**
 * @brief Initializes a new mem tree instance.
 *
 * This function sets up the initial state of the mem tree, including its mutex,
 * default cleanup interval, and manual cleanup flag. It also launches a background
 * thread responsible for automatic memory cleanup.
 *
 * @param tree Pointer to the ttak_mem_tree_t structure to initialize.
 */
void ttak_mem_tree_init(ttak_mem_tree_t *tree) {
    if (!tree) return;

    memset(tree, 0, sizeof(ttak_mem_tree_t));
    pthread_mutex_init(&tree->lock, NULL);
    pthread_cond_init(&tree->cond, NULL);
    atomic_store(&tree->min_cleanup_interval_ns, TT_MILLI_SECOND(500)); // Default min 500ms
    atomic_store(&tree->max_cleanup_interval_ns, TT_SECOND(10)); // Default max 10s
    atomic_store(&tree->garbage_pressure, 0);
    atomic_store(&tree->pressure_threshold, 1024 * 1024); // Default 1MB
    atomic_store(&tree->use_manual_cleanup, false);
    atomic_store(&tree->shutdown_requested, false);
    atomic_store(&tree->pending_hints, 0);
    atomic_store(&tree->cleanup_needed, false);

    // Launch the background cleanup thread
    if (pthread_create(&tree->cleanup_thread, NULL, cleanup_thread_func, tree) != 0) {
        fprintf(stderr, "[TTAK_MEM_TREE] Failed to create cleanup thread.\n");
    }
}

/**
 * @brief Destroys the mem tree, freeing all remaining allocated memory blocks and stopping the cleanup thread.
 *
 * This function signals the cleanup thread to terminate and waits for its completion.
 * It then iterates through any remaining mem nodes, frees their associated memory,
 * and destroys the mem tree's internal mutex.
 *
 * @param tree Pointer to the ttak_mem_tree_t structure to destroy.
 */
void ttak_mem_tree_destroy(ttak_mem_tree_t *tree) {
    if (!tree) return;

    // Signal cleanup thread to stop and join it
    atomic_store(&tree->shutdown_requested, true);
    pthread_mutex_lock(&tree->lock);
    pthread_cond_signal(&tree->cond);
    pthread_mutex_unlock(&tree->lock);

    if (tree->cleanup_thread) {
        pthread_join(tree->cleanup_thread, NULL);
    }

    pthread_mutex_lock(&tree->lock);
    ttak_mem_node_t *to_free_list = tree->head;
    tree->head = NULL;
    pthread_mutex_unlock(&tree->lock);

    ttak_mem_node_t *current = to_free_list;
    while (current) {
        ttak_mem_node_t *next = current->next;
        // Free the actual memory block if it hasn't been freed already
        if (current->ptr) {
            ttak_mem_free(current->ptr);
        }
        pthread_mutex_destroy(&current->lock);
        free(current); // Free the mem node itself
        current = next;
    }
    pthread_cond_destroy(&tree->cond);
    pthread_mutex_destroy(&tree->lock);
}

/**
 * @brief Adds a new memory block to be tracked by the mem tree.
 *
 * A new mem node is created to encapsulate the provided memory block's metadata.
 * This node is then added to the mem tree's internal list. The initial reference
 * count is set to 1.
 *
 * @param tree Pointer to the mem tree.
 * @param ptr Pointer to the allocated memory block.
 * @param size Size of the allocated memory block.
 * @param expires_tick Monotonic tick when this memory block should expire.
 * @param is_root True if this is a root node (externally referenced).
 * @return A pointer to the newly created mem node, or NULL on failure.
 */
ttak_mem_node_t *ttak_mem_tree_add(ttak_mem_tree_t *tree, void *ptr, size_t size, uint64_t expires_tick, _Bool is_root) {
    if (!tree || !ptr) return NULL;

    ttak_mem_node_t *new_node = (ttak_mem_node_t *)malloc(sizeof(ttak_mem_node_t));
    if (!new_node) {
        fprintf(stderr, "[TTAK_MEM_TREE] Failed to allocate mem node.\n");
        return NULL;
    }

    new_node->ptr = ptr;
    new_node->size = size;
    new_node->expires_tick = expires_tick;
    atomic_init(&new_node->ref_count, 1); // Initial ref count is 1
    new_node->is_root = is_root;
    new_node->tree = tree;
    pthread_mutex_init(&new_node->lock, NULL);

    pthread_mutex_lock(&tree->lock);
    new_node->next = tree->head;
    new_node->prev = NULL;
    if (tree->head) {
        tree->head->prev = new_node;
    }
    tree->head = new_node;
    pthread_mutex_unlock(&tree->lock);

    return new_node;
}

/**
 * @brief Removes a memory block from the mem tree.
 *
 * This function unlinks the node from the tree's bookkeeping list, but it does
 * not release the underlying allocation. Callers are responsible for freeing
 * the tracked pointer once it has been detached from the tree.
 *
 * @param tree Pointer to the mem tree.
 * @param node Pointer to the mem node to remove.
 */
void ttak_mem_tree_remove(ttak_mem_tree_t *tree, ttak_mem_node_t *node) {
    if (!tree || !node) return;

    pthread_mutex_lock(&tree->lock);
    
    if (node->prev) {
        node->prev->next = node->next;
    } else if (tree->head == node) {
        tree->head = node->next;
    }

    if (node->next) {
        node->next->prev = node->prev;
    }

    pthread_mutex_unlock(&tree->lock);

    pthread_mutex_destroy(&node->lock);
    free(node); // Free the mem node itself
}

/**
 * @brief Increments the reference count for a given mem node.
 *
 * This function atomically increments the reference count of a mem node,
 * indicating that another part of the system is now referencing this memory block.
 *
 * @param node Pointer to the mem node.
 */
void ttak_mem_node_acquire(ttak_mem_node_t *node) {
    if (!node) return;
    atomic_fetch_add(&node->ref_count, 1);
}

/**
 * @brief Decrements the reference count for a given mem node.
 *
 * This function atomically decrements the reference count. If the count drops
 * to zero, the node is considered unreferenced. If it's also expired, it becomes
 * a candidate for cleanup.
 *
 * @param node Pointer to the mem node.
 */
void ttak_mem_node_release(ttak_mem_node_t *node) {
    if (!node) return;
    if (atomic_fetch_sub(&node->ref_count, 1) == 1) {
        // Last reference released, report pressure to the tree if possible
        if (node->tree) {
            ttak_mem_tree_report_pressure(node->tree, node->size);
        }
    }
}

/**
 * @brief Sets the min and max intervals for automatic memory cleanup.
 *
 * The cleanup thread uses an adaptive interval based on memory pressure.
 * It will back off towards the max interval if no garbage is detected.
 *
 * @param tree Pointer to the mem tree.
 * @param min_ns The minimum cleanup interval in nanoseconds (e.g., 10s).
 * @param max_ns The maximum cleanup interval in nanoseconds (e.g., 120s).
 */
void ttak_mem_tree_set_cleaning_intervals(ttak_mem_tree_t *tree, uint64_t min_ns, uint64_t max_ns) {
    if (!tree) return;
    atomic_store(&tree->min_cleanup_interval_ns, min_ns);
    atomic_store(&tree->max_cleanup_interval_ns, max_ns);
}

/**
 * @brief Sets the pressure threshold that triggers immediate cleanup.
 * 
 * @param tree Pointer to the mem tree.
 * @param threshold_bytes Pressure threshold in bytes.
 */
void ttak_mem_tree_set_pressure_threshold(ttak_mem_tree_t *tree, size_t threshold_bytes) {
    if (!tree) return;
    atomic_store(&tree->pressure_threshold, threshold_bytes);
}

/**
 * @brief Reports potential garbage pressure to the mem tree.
 *
 * Increasing the pressure score signals the cleanup thread that there is work to do.
 * If pressure exceeds the threshold, the cleanup thread is signaled to wake up immediately.
 *
 * @param tree Pointer to the mem tree.
 * @param pressure_amount Amount to increase the pressure score by.
 */
void ttak_mem_tree_report_pressure(ttak_mem_tree_t *tree, size_t pressure_amount) {
    if (!tree) return;
    size_t new_pressure = atomic_fetch_add(&tree->garbage_pressure, pressure_amount) + pressure_amount;
    atomic_store(&tree->cleanup_needed, true);

    if (new_pressure >= atomic_load(&tree->pressure_threshold)) {
        pthread_mutex_lock(&tree->lock);
        pthread_cond_signal(&tree->cond);
        pthread_mutex_unlock(&tree->lock);
    }
}

/**
 * @brief Sets the manual cleanup flag.
 *
 * When set to true, automatic cleanup is disabled, and memory must be freed manually.
 * This function atomically updates the flag.
 *
 * @param tree Pointer to the mem tree.
 * @param manual_cleanup_enabled True to enable manual cleanup, false to enable automatic.
 */
void ttak_mem_tree_set_manual_cleanup(ttak_mem_tree_t *tree, _Bool manual_cleanup_enabled) {
    if (!tree) return;
    _Bool prev = atomic_exchange(&tree->use_manual_cleanup, manual_cleanup_enabled);
    if (prev == manual_cleanup_enabled) return;

    if (manual_cleanup_enabled) {
        // Transition to manual: stop the auto-cleanup thread.
        if (tree->cleanup_thread) {
            atomic_store(&tree->shutdown_requested, true);
            pthread_mutex_lock(&tree->lock);
            pthread_cond_signal(&tree->cond);
            pthread_mutex_unlock(&tree->lock);
            pthread_join(tree->cleanup_thread, NULL);
            tree->cleanup_thread = 0;
            atomic_store(&tree->shutdown_requested, false);
        }
    } else {
        // Transition to auto: start the auto-cleanup thread if not already running.
        if (!tree->cleanup_thread) {
            atomic_store(&tree->shutdown_requested, false);
            if (pthread_create(&tree->cleanup_thread, NULL, cleanup_thread_func, tree) != 0) {
                fprintf(stderr, "[TTAK_MEM_TREE] Failed to create cleanup thread.\n");
            } else {
                pthread_mutex_lock(&tree->lock);
                pthread_cond_signal(&tree->cond);
                pthread_mutex_unlock(&tree->lock);
            }
        }
    }
}

/**
 * @brief Performs a manual cleanup pass, freeing expired and unreferenced memory blocks.
 *
 * This function iterates through all tracked mem nodes. If a node's reference count
 * is zero and its expiration time has passed (or it's marked for immediate cleanup),
 * its associated memory is freed, and the node is removed from the tree.
 *
 * @param tree Pointer to the mem tree.
 * @param now Current monotonic tick.
 */
void ttak_mem_tree_perform_cleanup(ttak_mem_tree_t *tree, uint64_t now) {
    if (!tree) return;

    // Only walk the tree when there is a known candidate for release.
    if (atomic_load(&tree->garbage_pressure) == 0 && !atomic_load(&tree->cleanup_needed)) {
        return;
    }

    pthread_mutex_lock(&tree->lock);
    ttak_mem_node_t **indirect = &tree->head;
    ttak_mem_node_t *to_free_head = NULL;
    ttak_mem_node_t *to_free_tail = NULL;

    while (*indirect) {
        ttak_mem_node_t *node = *indirect;
        pthread_mutex_lock(&node->lock); // Lock node before checking its state

        _Bool should_free = false;
        if (atomic_load(&node->ref_count) == 0 && node->expires_tick != __TTAK_UNSAFE_MEM_FOREVER__ && now >= node->expires_tick) {
            should_free = true;
        }

        pthread_mutex_unlock(&node->lock); // Unlock node

        if (should_free) {
            *indirect = node->next; // Remove from main list
            if (node->next) {
                node->next->prev = node->prev;
            }
            
            // Add to temporary free list
            node->next = NULL;
            node->prev = NULL;
            if (!to_free_head) {
                to_free_head = node;
                to_free_tail = node;
            } else {
                to_free_tail->next = node;
                to_free_tail = node;
            }
        } else {
            indirect = &(*indirect)->next;
        }
    }
    pthread_mutex_unlock(&tree->lock);

    // Free collected nodes outside the tree lock
    size_t total_freed = 0;
    ttak_mem_node_t *current = to_free_head;
    while (current) {
        ttak_mem_node_t *next = current->next;
        total_freed += current->size;
        if (current->ptr) {
            ttak_mem_free(current->ptr);
            current->ptr = NULL;
        }
        pthread_mutex_destroy(&current->lock);
        free(current);
        current = next;
    }

    // Subtract freed amount from pressure
    if (total_freed > 0) {
        size_t old_pressure = atomic_load(&tree->garbage_pressure);
        while (old_pressure >= total_freed && !atomic_compare_exchange_weak(&tree->garbage_pressure, &old_pressure, old_pressure - total_freed));
        if (old_pressure < total_freed) {
            atomic_store(&tree->garbage_pressure, 0);
        }
    }

    // No remaining pressure means there is nothing left to clean up.
    if (atomic_load(&tree->garbage_pressure) == 0) {
        atomic_store(&tree->cleanup_needed, false);
    }
}

#ifdef _WIN32
/**
 * @brief Compatibility layer to support win32 while preserving clock_gettime_win behavior.
 * 
 * @param  A pointer to the struct timespec_t.
 * @return int timestamp in 100ns precision.
 */
static inline int clock_gettime_win(struct timespec *spec) {
    FILETIME ft;
    uint64_t tim;

    // Get precise time in 100ns unit
    GetSystemTimePreciseAsFileTime(&ft);

    tim = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
    #define WIN_EPOCH_DIFFERENCE 116444736000000000ULL
    tim -= WIN_EPOCH_DIFFERENCE;

    // convert timespec to 100ns unit
    spec->tv_sec = (time_t)(tim / 1000000000ULL);
    spec->tv_nsec = (long)((tim % 1000000000ULL) * 100);
    return 0;
}
#endif

/**
 * @brief Background thread function for automatic memory cleanup.
 *
 * This thread periodically wakes up, checks if automatic cleanup is enabled,
 * and if so, performs a cleanup pass on the mem tree. It respects the configured
 * cleanup interval and terminates when a shutdown request is received.
 *
 * @param arg A pointer to the ttak_mem_tree_t instance.
 * @return NULL upon termination.
 */
static void *cleanup_thread_func(void *arg) {
    ttak_mem_tree_t *tree = (ttak_mem_tree_t *)arg;
    if (!tree) return NULL;

    uint64_t current_sleep_ns = atomic_load(&tree->min_cleanup_interval_ns);

    while (!atomic_load(&tree->shutdown_requested)) {
        _Bool manual_cleanup_enabled = atomic_load(&tree->use_manual_cleanup);
        uint32_t hints = atomic_exchange(&tree->pending_hints, 0);

        if (hints & TTAK_MEM_TREE_HINT_SHUTDOWN) {
            atomic_store(&tree->shutdown_requested, true);
            break;
        }

        if (!manual_cleanup_enabled) {
            size_t pressure = atomic_load(&tree->garbage_pressure);

            if (pressure > 0 || (hints & (TTAK_MEM_TREE_HINT_ALLOC | TTAK_MEM_TREE_HINT_FREE | TTAK_MEM_TREE_HINT_PRESSURE))) {
                ttak_mem_tree_perform_cleanup(tree, ttak_get_tick_count());
                // Reset sleep interval to min after productive work.
                current_sleep_ns = atomic_load(&tree->min_cleanup_interval_ns);
            } else {
                // No work to do: gracefully back off.
                if (hints & TTAK_MEM_TREE_HINT_IDLE) {
                    current_sleep_ns += atomic_load(&tree->max_cleanup_interval_ns) / 4;
                } else {
                    current_sleep_ns += atomic_load(&tree->min_cleanup_interval_ns);
                }
                uint64_t max_ns = atomic_load(&tree->max_cleanup_interval_ns);
                if (current_sleep_ns > max_ns) {
                    current_sleep_ns = max_ns;
                }
            }
        } else {
            // Manual cleanup: sleep for max interval or until signaled.
            current_sleep_ns = atomic_load(&tree->max_cleanup_interval_ns);
        }

        pthread_mutex_lock(&tree->lock);
        if (!atomic_load(&tree->shutdown_requested)) {
            struct timespec ts;
#ifdef _WIN32
            clock_gettime_win(&ts);
#elif defined(EMBEDDED_BAREMETAL)
            uint64_t ns = ttak_get_tick_count_ns();
            ts.tv_sec = (long)(ns / 1000000000ULL);
            ts.tv_nsec = (long)(ns % 1000000000ULL);
#else
            clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
            uint64_t total_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec + current_sleep_ns;
            ts.tv_sec = (time_t)(total_ns / 1000000000ULL);
            ts.tv_nsec = (long)(total_ns % 1000000000ULL);
            pthread_cond_timedwait(&tree->cond, &tree->lock, &ts);
        }
        pthread_mutex_unlock(&tree->lock);
    }
    return NULL;
}

/**
 * @brief Finds a mem node associated with a given memory pointer.
 *
 * This function traverses the mem tree's internal list to find the node
 * that tracks the specified memory pointer. It ensures thread-safe access
 * to the tree structure.
 *
 * @param tree Pointer to the mem tree.
 * @param ptr The memory pointer to search for.
 * @return A pointer to the found mem node, or NULL if not found.
 */
ttak_mem_node_t *ttak_mem_tree_find_node(ttak_mem_tree_t *tree, void *ptr) {
    if (!tree || !ptr) return NULL;

    pthread_mutex_lock(&tree->lock);
    ttak_mem_node_t *current = tree->head;
    while (current) {
        if (current->ptr == ptr) {
            pthread_mutex_unlock(&tree->lock);
            return current;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&tree->lock);
    return NULL;
}

void ttak_mem_tree_hint(ttak_mem_tree_t *tree, ttak_mem_tree_hint_t hint) {
    if (!tree) return;
    atomic_fetch_or(&tree->pending_hints, (uint32_t)hint);
    // Wake the cleanup thread for urgent hints.
    if (hint == TTAK_MEM_TREE_HINT_SHUTDOWN || hint == TTAK_MEM_TREE_HINT_PRESSURE) {
        pthread_mutex_lock(&tree->lock);
        pthread_cond_signal(&tree->cond);
        pthread_mutex_unlock(&tree->lock);
    }
}
