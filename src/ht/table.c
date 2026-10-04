#include <ttak/ht/table.h>
#include <ttak/ht/hash.h>
#include <ttak/ht/wyhash.h>
#include <ttak/mem/mem.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PROBE 32

/**
 * @brief Default wyhash implementation for arbitrary byte keys.
 */
static uint64_t default_wyhash(const void *key, size_t len, uint64_t k0, uint64_t k1) {
    (void)k1;
    return ttak_wyhash(key, len, k0);
}

/**
 * @brief Initialize a hash table with optional callbacks.
 */
void ttak_table_init(ttak_table_t *table, size_t capacity,
                     uint64_t (*hash_func)(const void*, size_t, uint64_t, uint64_t),
                     int (*key_cmp)(const void*, const void*),
                     void (*key_free)(void*),
                     void (*val_free)(void*)) {
    if (!table) return;
    
    // Round up capacity to power of 2
    size_t cap = 16;
    while (cap < capacity) cap <<= 1;
    
    table->capacity = cap;
    table->size = 0;
    table->tombstones = 0;
    table->k0 = 0xa0761d6478bd642fULL;
    table->k1 = 0xe7037ed1a0b428dbULL;
    table->hash_func = hash_func ? hash_func : default_wyhash;
    table->key_cmp = key_cmp;
    table->key_free = key_free;
    table->val_free = val_free;

    size_t padded_cap = cap + MAX_PROBE;
    ttak_mem_flags_t flags = (padded_cap * (sizeof(uint8_t) + sizeof(void*) * 3) >= 2 * 1024 * 1024) ? TTAK_MEM_HUGE_PAGES : TTAK_MEM_DEFAULT;
    
    table->ctrls = ttak_mem_alloc_safe(padded_cap * sizeof(uint8_t), __TTAK_UNSAFE_MEM_FOREVER__, 0, false, false, true, true, flags);
    table->keys = ttak_mem_alloc_safe(padded_cap * sizeof(void*), __TTAK_UNSAFE_MEM_FOREVER__, 0, false, false, true, true, flags);
    table->key_lens = ttak_mem_alloc_safe(padded_cap * sizeof(size_t), __TTAK_UNSAFE_MEM_FOREVER__, 0, false, false, true, true, flags);
    table->values = ttak_mem_alloc_safe(padded_cap * sizeof(void*), __TTAK_UNSAFE_MEM_FOREVER__, 0, false, false, true, true, flags);

    if (table->ctrls) memset(table->ctrls, 0, padded_cap * sizeof(uint8_t));
}

/**
 * @brief Rebuild the table into fresh arrays of @p new_cap slots.
 *
 * Only live (OCCUPIED) entries survive, so all tombstones are cleared.
 * @p new_cap may equal the current capacity to rehash in place when the
 * table is full of tombstones rather than live entries.
 */
static void ttak_table_rehash(ttak_table_t *table, uint64_t now, size_t new_cap) {
    size_t old_cap = table->capacity;
    uint8_t *old_ctrls = table->ctrls;
    void **old_keys = table->keys;
    size_t *old_key_lens = table->key_lens;
    void **old_vals = table->values;

    ttak_table_t new_t;
    ttak_table_init(&new_t, new_cap, table->hash_func, table->key_cmp, NULL, NULL);
    new_t.k0 = table->k0;
    new_t.k1 = table->k1;

    for (size_t i = 0; i < old_cap + MAX_PROBE; i++) {
        if (old_ctrls[i] == OCCUPIED) {
            ttak_table_put(&new_t, old_keys[i], old_key_lens[i], old_vals[i], now);
        }
    }

    ttak_mem_free(old_ctrls);
    ttak_mem_free(old_keys);
    ttak_mem_free(old_key_lens);
    ttak_mem_free(old_vals);

    table->ctrls = new_t.ctrls;
    table->keys = new_t.keys;
    table->key_lens = new_t.key_lens;
    table->values = new_t.values;
    table->capacity = new_t.capacity;
    table->tombstones = 0;
}

void ttak_table_put(ttak_table_t *table, void *key, size_t key_len, void *value, uint64_t now) {
    if (!table || !table->ctrls) return;
    /* Tombstones degrade probe chains just like live entries, so they count
     * toward the load factor. If live entries alone are under 70% the table
     * is mostly tombstones: rehash at the same capacity to bound memory. */
    if ((table->size + table->tombstones) * 10 >= table->capacity * 7) {
        size_t new_cap = (table->size * 10 >= table->capacity * 7) ? table->capacity * 2 : table->capacity;
        ttak_table_rehash(table, now, new_cap);
    }

    uint64_t hash = table->hash_func(key, key_len, table->k0, table->k1);
    size_t idx = hash & (table->capacity - 1);
    size_t first_tomb = SIZE_MAX;

    while (table->ctrls[idx] != EMPTY) {
        if (table->ctrls[idx] == DELETED) {
            if (first_tomb == SIZE_MAX) first_tomb = idx;
        } else if (table->key_cmp(table->keys[idx], key) == 0) {
            if (table->val_free && table->values[idx]) table->val_free(table->values[idx]);
            table->values[idx] = value;
            table->key_lens[idx] = key_len;
            return;
        }
        idx++;
        if (idx >= table->capacity + MAX_PROBE - 1) idx = 0;
    }

    if (first_tomb != SIZE_MAX) {
        idx = first_tomb;
        table->tombstones--;
    }

    table->ctrls[idx] = OCCUPIED;
    table->keys[idx] = key;
    table->key_lens[idx] = key_len;
    table->values[idx] = value;
    table->size++;
}

void *ttak_table_get(ttak_table_t *table, const void *key, size_t key_len, uint64_t now) {
    if (!table || !table->ctrls) return NULL;
    (void)now;

    uint64_t hash = table->hash_func(key, key_len, table->k0, table->k1);
    size_t idx = hash & (table->capacity - 1);

    while (table->ctrls[idx] != EMPTY) {
        if (table->ctrls[idx] == OCCUPIED && table->key_cmp(table->keys[idx], key) == 0) {
            return table->values[idx];
        }
        idx++;
        if (idx >= table->capacity + MAX_PROBE - 1) idx = 0;
    }
    return NULL;
}

bool ttak_table_remove(ttak_table_t *table, const void *key, size_t key_len, uint64_t now) {
    if (!table || !table->ctrls) return false;
    (void)now;

    uint64_t hash = table->hash_func(key, key_len, table->k0, table->k1);
    size_t idx = hash & (table->capacity - 1);

    while (table->ctrls[idx] != EMPTY) {
        if (table->ctrls[idx] == OCCUPIED && table->key_cmp(table->keys[idx], key) == 0) {
            if (table->key_free && table->keys[idx]) table->key_free(table->keys[idx]);
            if (table->val_free && table->values[idx]) table->val_free(table->values[idx]);
            table->ctrls[idx] = DELETED;
            table->size--;
            table->tombstones++;
            return true;
        }
        idx++;
        if (idx >= table->capacity + MAX_PROBE - 1) idx = 0;
    }
    return false;
}

void ttak_table_destroy(ttak_table_t *table, uint64_t now) {
    if (!table || !table->ctrls) return;
    (void)now;

    for (size_t i = 0; i < table->capacity + MAX_PROBE; i++) {
        if (table->ctrls[i] == OCCUPIED) {
            if (table->key_free && table->keys[i]) table->key_free(table->keys[i]);
            if (table->val_free && table->values[i]) table->val_free(table->values[i]);
        }
    }
    ttak_mem_free(table->ctrls);
    ttak_mem_free(table->keys);
    ttak_mem_free(table->key_lens);
    ttak_mem_free(table->values);
    table->size = 0;
    table->tombstones = 0;
}
