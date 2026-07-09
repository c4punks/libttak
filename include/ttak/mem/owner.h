#ifndef TTAK_MEM_OWNER_H
#define TTAK_MEM_OWNER_H

#include <ttak/ht/map.h>
#include <ttak/sync/sync.h>
#include <stdbool.h>
#include <stdint.h>

/* Forward declaration; full typedef lives in <ttak/mem/mem.h> to avoid cycles. */
struct ttak_owner;

/**
 * @brief Sentinel indicating that a memory pointer has no owning context.
 */
#define TTAK_NO_OWNER ((struct ttak_owner *)0)

/**
 * @brief Function pointer type for tasks executed within the owner's context.
 * 
 * @param ctx Pointer to the user-provided context data.
 * @param args Arguments passed during the execution call.
 */
typedef void (*ttak_owner_func_t)(void *ctx, void *args);

/**
 * @brief Configuration flags for the owner's safety policy.
 */
typedef enum {
    TTAK_OWNER_SAFE_DEFAULT = 0,
    TTAK_OWNER_DENY_DANGEROUS_MEM = (1 << 0), /**< Prevent access to memory marked as unsafe/volatile. */
    TTAK_OWNER_DENY_THREADING = (1 << 1),     /**< Prevent spawning threads or async tasks within this context. */
    TTAK_OWNER_STRICT_ISOLATION = (1 << 2)    /**< Enforce strict data isolation (no external pointer access). */
} ttak_owner_policy_t;

/**
 * @brief The Owner structure.
 * 
 * Acts as a sandbox/container for resources and functions.
 * Ensures isolation and enforces safety policies.
 */
struct ttak_owner {
    uint32_t id;                /**< Unique ID for bitmap tracking. */
    ttak_map_t *resources;      /**< Map storing owned resources (isolated variables). */
    ttak_map_t *functions;      /**< Map storing registered functions. */
    ttak_rwlock_t lock;         /**< RWLock for thread-safe access to the owner's state. */
    uint64_t creation_ts;       /**< Timestamp when this owner was created. */
    uint32_t policy_flags;      /**< Safety policy bitmask. */
};

typedef struct ttak_owner ttak_owner_t;
typedef ttak_owner_t tt_owner_t;

/**
 * @brief Creates a new Owner context.
 * 
 * @param policy Safety policy flags.
 * @return Pointer to the initialized owner, or NULL on failure.
 */
ttak_owner_t *ttak_owner_create(uint32_t policy);

/**
 * @brief Destroys the Owner and releases all registered resources.
 * 
 * @param owner The owner to destroy.
 */
void ttak_owner_destroy(ttak_owner_t *owner);

/**
 * @brief Registers a function into the owner's isolated context.
 * 
 * The function is "copied" (logically) and can only be invoked via ttak_owner_execute.
 * 
 * @param owner The owner context.
 * @param name Unique name for the function.
 * @param func Function pointer.
 * @return true if registered successfully, false otherwise.
 */
bool ttak_owner_register_func(ttak_owner_t *owner, const char *name, ttak_owner_func_t func);

/**
 * @brief Registers a resource (variable) into the owner's isolated context.
 * 
 * @param owner The owner context.
 * @param name Unique name for the resource.
 * @param data Pointer to the data.
 * @return true if registered successfully.
 */
bool ttak_owner_register_resource(ttak_owner_t *owner, const char *name, void *data);

/**
 * @brief Transfers a resource from one owner to another.
 */
bool ttak_owner_transfer_resource(ttak_owner_t *from, ttak_owner_t *to, const char *name);

/**
 * @brief Executes a registered function within the owner's safety context.
 * 
 * @param owner The owner context.
 * @param func_name Name of the function to execute.
 * @param resource_name Name of the registered resource to pass as 'ctx' (optional, can be NULL).
 * @param args runtime arguments to pass to the function.
 * @return true if execution was permitted and successful.
 */
bool ttak_owner_execute(ttak_owner_t *owner, const char *func_name, const char *resource_name, void *args);

#endif // TTAK_MEM_OWNER_H
