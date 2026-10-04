#ifndef TTAK_HT_TABLE_H
#define TTAK_HT_TABLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Generic Table Entry.
 */
typedef struct ttak_table_entry {
    void *key;
    void *value;
    struct ttak_table_entry *next; // Chaining for collisions
} ttak_table_entry_t;

/**
 * @brief Generic SipHash Table (Refactored to Open Addressing SoA).
 */
typedef struct ttak_table {
    uint8_t  *ctrls;    /**< Control bytes */
    void     **keys;    /**< Keys array */
    size_t   *key_lens; /**< Key lengths array */
    void     **values;  /**< Values array */
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

void ttak_table_put(ttak_table_t *table, void *key, size_t key_len, void *value, uint64_t now);
void *ttak_table_get(ttak_table_t *table, const void *key, size_t key_len, uint64_t now);
bool ttak_table_remove(ttak_table_t *table, const void *key, size_t key_len, uint64_t now);
void ttak_table_destroy(ttak_table_t *table, uint64_t now);

#endif // TTAK_HT_TABLE_H
