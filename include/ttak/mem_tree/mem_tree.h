#ifndef TTAK_MEM_TREE_H
#define TTAK_MEM_TREE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

typedef struct ttak_mem_tree ttak_mem_tree_t;

/**
 * @brief Represents a node in the generic heap tree, tracking a dynamically allocated memory block.
 *
 * Each node stores metadata about a memory allocation, including its pointer, size,
 * expiration time, and reference count. It also includes flags for managing its
 * lifecycle within the heap tree and a mutex for thread-safe access.
 */
typedef struct ttak_mem_node {
    void *ptr;                      /**< Pointer to the actual memory block. */
    size_t size;                    /**< Size of the allocated memory block. */
    uint64_t expires_tick;          /**< Monotonic tick when this memory block should expire. */
    _Atomic uint32_t ref_count;     /**< Atomic count of references to this node. */
    _Bool is_root;                  /**< True if this node is referenced externally (not by another heap node). */
    pthread_mutex_t lock;           /**< Mutex for thread-safe access to this node's metadata. */
    struct ttak_mem_node *next;    /**< Pointer to the next node in the mem tree's internal list. */
    struct ttak_mem_node *prev;    /**< Pointer to the previous node in the mem tree's internal list. */
    ttak_mem_tree_t *tree;         /**< Pointer back to the parent mem tree. */
} ttak_mem_node_t;

/**
 * @brief Manages the collection of dynamically allocated memory blocks as a mem tree.
 *
 * This structure provides a centralized mechanism for tracking memory allocations,
 * their interdependencies (via reference counts), and their lifetimes. It supports
 * automatic cleanup of expired or unreferenced blocks and allows for manual control
 * over the cleanup process.
 */
struct ttak_mem_tree {
    ttak_mem_node_t *head;             /**< Head of the linked list of all tracked mem nodes. */
    pthread_mutex_t lock;               /**< Mutex for thread-safe access to the mem tree structure. */
    pthread_cond_t cond;                /**< Condition variable for immediate cleanup wakeup. */
    _Atomic uint64_t max_cleanup_interval_ns; /**< Maximum interval in nanoseconds for automatic cleanup (default 120s). */
    _Atomic uint64_t min_cleanup_interval_ns; /**< Minimum interval in nanoseconds for automatic cleanup (default 10s). */
    _Atomic size_t garbage_pressure;    /**< Score representing the amount of potential garbage/missing memory. */
    _Atomic size_t pressure_threshold;  /**< Threshold to trigger immediate cleanup (default 1MB). */
    _Atomic _Bool use_manual_cleanup;   /**< Flag to disable automatic cleanup (1 for manual, 0 for auto). */
    pthread_t cleanup_thread;           /**< Thread ID for the background automatic cleanup process. */
    _Atomic _Bool shutdown_requested;   /**< Flag to signal the cleanup thread to terminate. */
    _Atomic uint32_t pending_hints;     /**< Bitmask of pending hints from the memory manager. */
    _Atomic _Bool cleanup_needed;       /**< True when there is a known candidate for release; avoids walking the tree when empty. */
};

/**
 * @brief Initializes a new mem tree instance.
 *
 * @param tree Pointer to the ttak_mem_tree_t structure to initialize.
 */
void ttak_mem_tree_init(ttak_mem_tree_t *tree);

/**
 * @brief Destroys the mem tree, freeing all remaining allocated memory blocks and stopping the cleanup thread.
 *
 * @param tree Pointer to the ttak_mem_tree_t structure to destroy.
 */
void ttak_mem_tree_destroy(ttak_mem_tree_t *tree);

/**
 * @brief Adds a new memory block to be tracked by the mem tree.
 *
 * @param tree Pointer to the mem tree.
 * @param ptr Pointer to the allocated memory block.
 * @param size Size of the allocated memory block.
 * @param expires_tick Monotonic tick when this memory block should expire.
 * @param is_root True if this is a root node (externally referenced).
 * @return A pointer to the newly created mem node, or NULL on failure.
 */
ttak_mem_node_t *ttak_mem_tree_add(ttak_mem_tree_t *tree, void *ptr, size_t size, uint64_t expires_tick, _Bool is_root);

/**
 * @brief Removes a memory block from the mem tree bookkeeping.
 *
 * Callers are responsible for freeing the actual allocation once the node has
 * been detached from the tree.
 *
 * @param tree Pointer to the mem tree.
 * @param node Pointer to the mem node to remove.
 */
void ttak_mem_tree_remove(ttak_mem_tree_t *tree, ttak_mem_node_t *node);

/**
 * @brief Increments the reference count for a given mem node.
 *
 * @param node Pointer to the mem node.
 */
void ttak_mem_node_acquire(ttak_mem_node_t *node);

/**
 * @brief Decrements the reference count for a given mem node.
 *
 * If the reference count drops to zero and the node has expired, it may be
 * marked for cleanup or immediately freed depending on the mem tree's policy.
 *
 * @param node Pointer to the mem node.
 */
void ttak_mem_node_release(ttak_mem_node_t *node);

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
void ttak_mem_tree_set_cleaning_intervals(ttak_mem_tree_t *tree, uint64_t min_ns, uint64_t max_ns);

/**
 * @brief Sets the pressure threshold that triggers immediate cleanup.
 * 
 * @param tree Pointer to the mem tree.
 * @param threshold_bytes Pressure threshold in bytes.
 */
void ttak_mem_tree_set_pressure_threshold(ttak_mem_tree_t *tree, size_t threshold_bytes);

/**
 * @brief Reports potential garbage pressure to the mem tree.
 *
 * Increasing the pressure score signals the cleanup thread that there is work to do.
 * If pressure is zero, the cleanup thread may perform an early return or sleep longer.
 *
 * @param tree Pointer to the mem tree.
 * @param pressure_amount Amount to increase the pressure score by.
 */
void ttak_mem_tree_report_pressure(ttak_mem_tree_t *tree, size_t pressure_amount);

/**
 * @brief Sets the manual cleanup flag.
 *
 * When set to true, automatic cleanup is disabled, and memory must be freed manually.
 *
 * @param tree Pointer to the mem tree.
 * @param manual_cleanup_enabled True to enable manual cleanup, false to enable automatic.
 */
void ttak_mem_tree_set_manual_cleanup(ttak_mem_tree_t *tree, _Bool manual_cleanup_enabled);

/**
 * @brief Performs a manual cleanup pass, freeing expired and unreferenced memory blocks.
 *
 * This function is typically called when automatic cleanup is disabled or when
 * an immediate cleanup is desired.
 *
 * @param tree Pointer to the mem tree.
 * @param now Current monotonic tick.
 */
void ttak_mem_tree_perform_cleanup(ttak_mem_tree_t *tree, uint64_t now);

/**
 * @brief Finds a mem node associated with a given memory pointer.
 *
 * @param tree Pointer to the mem tree.
 * @param ptr The memory pointer to search for.
 * @return A pointer to the found mem node, or NULL if not found.
 */
ttak_mem_node_t *ttak_mem_tree_find_node(ttak_mem_tree_t *tree, void *ptr);

/**
 * @brief Hints for the mem tree cleanup thread to adapt its behaviour.
 */
typedef enum {
    TTAK_MEM_TREE_HINT_ALLOC      = (1U << 0), /**< A new allocation was made. */
    TTAK_MEM_TREE_HINT_FREE       = (1U << 1), /**< A block was freed or released. */
    TTAK_MEM_TREE_HINT_PRESSURE   = (1U << 2), /**< A pressure burst was reported. */
    TTAK_MEM_TREE_HINT_IDLE       = (1U << 3), /**< The system is idle; back off aggressively. */
    TTAK_MEM_TREE_HINT_SHUTDOWN   = (1U << 4), /**< Shutdown is imminent; clean up eagerly. */
} ttak_mem_tree_hint_t;

/**
 * @brief Deliver a hint to the mem tree's background cleanup thread.
 *
 * The thread uses hints to decide whether to stay awake, batch work, or
 * back off to longer sleep intervals.  This avoids CPU-busy polling.
 *
 * @param tree Pointer to the mem tree.
 * @param hint One of @c ttak_mem_tree_hint_t values.
 */
void ttak_mem_tree_hint(ttak_mem_tree_t *tree, ttak_mem_tree_hint_t hint);

#endif // TTAK_HEAP_TREE_H
