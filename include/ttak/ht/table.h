#ifndef TTAK_HT_TABLE_H
#define TTAK_HT_TABLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ttak/ht/hash.h>
#include <ttak/compat/static_alloc.h>

/**
 * @brief Minimum capacity of a table (capacities round up to power-of-two,
 *        never below this floor).
 */
#define TTAK_TABLE_MIN_CAPACITY ((size_t)16U)

/**
 * @brief Bytes of caller-provided storage required for a static table.
 *
 * Covers the ctrl bytes array and the fused key/value/key_len slots,
 * including the TTAK_HT_MAX_PROBE probe padding and inter-region alignment:
 *   static uint8_t storage[TTAK_TABLE_STATIC_STORAGE_SIZE(64)];
 */
#define TTAK_TABLE_STATIC_STORAGE_SIZE(capacity) \
    TTAK_STATIC_ALIGN_UP( \
        TTAK_STATIC_ALIGN_UP( \
            (TTAK_STATIC_POW2(capacity) < TTAK_TABLE_MIN_CAPACITY \
                ? TTAK_TABLE_MIN_CAPACITY : TTAK_STATIC_POW2(capacity)) + TTAK_HT_MAX_PROBE, \
            TTAK_STATIC_STORAGE_ALIGN) + \
        ((TTAK_STATIC_POW2(capacity) < TTAK_TABLE_MIN_CAPACITY \
            ? TTAK_TABLE_MIN_CAPACITY : TTAK_STATIC_POW2(capacity)) + TTAK_HT_MAX_PROBE) * \
            sizeof(ttak_table_entry_slot_t), \
        TTAK_STATIC_STORAGE_ALIGN)

/**
 * @brief Generic Table Entry.
 */
typedef struct ttak_table_entry {
    void *key;
    void *value;
    struct ttak_table_entry *next; // Chaining for collisions
} ttak_table_entry_t;

/**
 * @brief Fused key/value slot for the open-addressing table (AoS layout).
 *
 * Keeping the key next to its value (and its length) means a successful
 * lookup touches one entry cache line instead of separate keys/values lines.
 */
typedef struct ttak_table_entry_slot {
    void   *key;     /**< Slot key (only meaningful when ctrl is OCCUPIED). */
    void   *value;   /**< Slot value associated with @c key. */
    size_t key_len;  /**< Length of the @c key. */
} ttak_table_entry_slot_t;

/**
 * @brief Generic Table (open addressing, control bytes + fused entries).
 */
typedef struct ttak_table {
    uint8_t  *ctrls;   /**< Control bytes */
    ttak_table_entry_slot_t *entries; /**< Fused key/value/key_len slots */
    size_t   capacity;
    size_t   size;
    size_t   tombstones; /**< Slots marked DELETED; count toward the resize trigger. */
    uint64_t k0;
    uint64_t k1;
    
    // Function pointers for generic behavior
    uint64_t (*hash_func)(const void *key, size_t key_len, uint64_t k0, uint64_t k1);
    int (*key_cmp)(const void *k1, const void *k2);
    void (*key_free)(void *key);
    void (*val_free)(void *val);

    /**
     * @brief Non-zero when the backing arrays live in caller-provided
     * storage (see ttak_table_init_static()): rehashing and array freeing
     * are disabled regardless of build flags.
     */
    int is_static;
} ttak_table_t;

/**
 * @brief Initialize a generic table.
 * @param hash_func Custom hash function (or NULL for default SipHash on generic bytes).
 * @param key_cmp Key comparison function (returns 0 if equal).
 */
void ttak_table_init(ttak_table_t *table, size_t capacity, 
                     uint64_t (*hash_func)(const void*, size_t, uint64_t, uint64_t),
                     int (*key_cmp)(const void*, const void*),
                     void (*key_free)(void*),
                     void (*val_free)(void*));

/**
 * @brief Initialize a generic table in caller-provided storage (zero-allocation).
 *
 * The storage must hold at least TTAK_TABLE_STATIC_STORAGE_SIZE(capacity)
 * bytes. Capacity is fixed (rounded up to a power of two, minimum 16):
 * unlike tables from ttak_table_init(), a static table never rehashes and
 * ttak_table_destroy() never frees its backing arrays.
 *
 * @return true on success, false if arguments or storage size are invalid.
 */
bool ttak_table_init_static(ttak_table_t *table, size_t capacity, void *storage, size_t storage_size,
                            uint64_t (*hash_func)(const void*, size_t, uint64_t, uint64_t),
                            int (*key_cmp)(const void*, const void*),
                            void (*key_free)(void*),
                            void (*val_free)(void*));

void ttak_table_put(ttak_table_t *table, void *key, size_t key_len, void *value, uint64_t now);
void *ttak_table_get(ttak_table_t *table, const void *key, size_t key_len, uint64_t now);
bool ttak_table_remove(ttak_table_t *table, const void *key, size_t key_len, uint64_t now);
void ttak_table_destroy(ttak_table_t *table, uint64_t now);

#endif // TTAK_HT_TABLE_H
