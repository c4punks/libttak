#include <ttak/container/set.h>
#include "../../internal/ttak/table_internal.h"
#include <stddef.h>

/**
 * @brief Initialize a set wrapper around the generic hash table.
 *
 * @param set       Set instance to initialize.
 * @param capacity  Desired starting capacity.
 * @param hash_func Function that hashes a key.
 * @param key_cmp   Comparator for keys.
 * @param key_free  Optional destructor for stored keys.
 */
void ttak_set_init(ttak_set_t *set, size_t capacity,
                   uint64_t (*hash_func)(const void*, size_t, uint64_t, uint64_t),
                   int (*key_cmp)(const void*, const void*),
                   void (*key_free)(void*)) {
    if (!set) return;
    ttak_table_init(&set->table, capacity, hash_func, key_cmp, key_free, NULL);
}

/**
 * @brief Insert a key into the set.
 *
 * Stores a placeholder value so lookups can distinguish between "missing" and
 * "present with NULL value". Duplicate adds are no-ops resolved with a single
 * hash-table probe.
 *
 * @param set     Set to update.
 * @param key     Key to store.
 * @param key_len Length of the key payload.
 * @param now     Timestamp needed by the memory tracker.
 */
void ttak_set_add(ttak_set_t *set, void *key, size_t key_len, uint64_t now) {
    if (!set) return;
    ttak_table_put_if_absent(&set->table, key, key_len, (void*)1, now);
}

/**
 * @brief Check whether a key exists in the set.
 *
 * @param set     Set instance to probe.
 * @param key     Key to query.
 * @param key_len Length of the key payload.
 * @param now     Timestamp for safe access.
 * @return true if key exists, false otherwise.
 */
bool ttak_set_contains(ttak_set_t *set, const void *key, size_t key_len, uint64_t now) {
    if (!set) return false;
    return ttak_table_get(&set->table, key, key_len, now) != NULL;
}

/**
 * @brief Remove the provided key from the set.
 *
 * @param set     Set container.
 * @param key     Key to delete.
 * @param key_len Length of the key payload.
 * @param now     Timestamp for memory validation.
 * @return true if the key was removed, false if it was absent.
 */
bool ttak_set_remove(ttak_set_t *set, const void *key, size_t key_len, uint64_t now) {
    if (!set) return false;
    return ttak_table_remove(&set->table, key, key_len, now);
}

/**
 * @brief Destroy the set and release any owned resources.
 *
 * @param set Set to tear down.
 * @param now Timestamp supplied to the underlying table destroy routine.
 */
void ttak_set_destroy(ttak_set_t *set, uint64_t now) {
    if (!set) return;
    ttak_table_destroy(&set->table, now);
}
