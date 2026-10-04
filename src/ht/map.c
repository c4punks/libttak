#include <ttak/ht/hash.h>
#include <ttak/ht/map.h>
#include <ttak/mem/mem.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PROBE 32

static size_t next_pow2(size_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1; n |= n >> 2; n |= n >> 4;
    n |= n >> 8; n |= n >> 16;
#if UINTPTR_MAX > 0xffffffff
    n |= n >> 32;
#endif
    return n + 1;
}

/* Internal destructor: release all three backing arrays regardless of NULL state. */
static void ttak_map_arrays_destroy(tt_map_t *map) {
    if (map->ctrls)  { ttak_mem_free(map->ctrls);  map->ctrls  = NULL; }
    if (map->keys)   { ttak_mem_free(map->keys);   map->keys   = NULL; }
    if (map->values) { ttak_mem_free(map->values); map->values = NULL; }
}

/* Internal constructor: allocate the three arrays and zero only the
 * control bytes. keys/values of non-OCCUPIED slots are never read, so
 * zero-initialising them is unnecessary work.
 * Returns 0 on success, -1 on any allocation failure (arrays freed on error). */
static int ttak_map_arrays_alloc(tt_map_t *map, size_t padded_cap, uint64_t now) {
    map->ctrls  = ttak_mem_alloc_raw(padded_cap * sizeof(uint8_t),   __TTAK_UNSAFE_MEM_FOREVER__, now);
    map->keys   = ttak_mem_alloc_raw(padded_cap * sizeof(uintptr_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
    map->values = ttak_mem_alloc_raw(padded_cap * sizeof(size_t),    __TTAK_UNSAFE_MEM_FOREVER__, now);

    if (map->ctrls == NULL || map->keys == NULL || map->values == NULL) {
        ttak_map_arrays_destroy(map);
        return -1;
    }

    memset(map->ctrls,  0, padded_cap * sizeof(uint8_t));
    return 0;
}

tt_map_t *ttak_create_map(size_t init_cap, uint64_t now) {
    tt_map_t *map = ttak_mem_alloc_raw(sizeof(tt_map_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!map) return NULL;

    map->cap  = next_pow2(init_cap);
    map->size = 0;
    map->seed = 0xa0761d6478bd642fULL;
    map->ctrls  = NULL;
    map->keys   = NULL;
    map->values = NULL;

    // Allocate with padding to allow branchless linear probing
    size_t padded_cap = map->cap + MAX_PROBE;
    if (ttak_map_arrays_alloc(map, padded_cap, now) != 0) {
        ttak_mem_free(map);
        return NULL;
    }

    return map;
}

static void ttak_resize_map(tt_map_t *map, uint64_t now) {
    size_t old_cap = map->cap;
    uint8_t *old_ctrls = map->ctrls;
    uintptr_t *old_keys = map->keys;
    size_t *old_vals = map->values;

    size_t new_cap = old_cap * 2;
    tt_map_t *new_m = ttak_create_map(new_cap, now);
    if (!new_m) return;

    for (size_t i = 0; i < old_cap; i++) {
        if (old_ctrls[i] == OCCUPIED) {
            ttak_insert_to_map(new_m, old_keys[i], old_vals[i], now);
        }
    }

    ttak_mem_free(old_ctrls);
    ttak_mem_free(old_keys);
    ttak_mem_free(old_vals);

    uint64_t s = map->seed;
    *map = *new_m;
    map->seed = s;
    ttak_mem_free(new_m);
}

void ttak_insert_to_map(tt_map_t *map, uintptr_t key, size_t val, uint64_t now) {
    if (!ttak_mem_access(map, now)) return;
    if (map->size * 10 >= map->cap * 7) ttak_resize_map(map, now);

    uint64_t h = gen_hash_wyhash(key, map->seed);
    size_t idx = h & (map->cap - 1);

    // Linear probing with padding - fewer branches
    while (map->ctrls[idx] == OCCUPIED) {
        if (map->keys[idx] == key) {
            map->values[idx] = val;
            return;
        }
        idx++;
        // If we hit the padding limit, we must wrap. 
        // But with MAX_PROBE and 70% load, this is rare.
        if (idx >= map->cap + MAX_PROBE - 1) {
            idx = 0;
        }
    }

    map->ctrls[idx] = OCCUPIED;
    map->keys[idx] = key;
    map->values[idx] = val;
    map->size++;
}

_Bool ttak_map_get_key(tt_map_t *map, uintptr_t key, size_t *out, uint64_t now) {
    if (!ttak_mem_access(map, now)) return 0;
    uint64_t h = gen_hash_wyhash(key, map->seed);
    size_t idx = h & (map->cap - 1);

    while (map->ctrls[idx] != EMPTY) {
        if (map->ctrls[idx] == OCCUPIED && map->keys[idx] == key) {
            if (out) *out = map->values[idx];
            return 1;
        }
        idx++;
        if (idx >= map->cap + MAX_PROBE - 1) idx = 0;
    }
    return 0;
}

void ttak_delete_from_map(tt_map_t *map, uintptr_t key, uint64_t now) {
    if (!ttak_mem_access(map, now)) return;
    uint64_t h = gen_hash_wyhash(key, map->seed);
    size_t idx = h & (map->cap - 1);

    while (map->ctrls[idx] != EMPTY) {
        if (map->ctrls[idx] == OCCUPIED && map->keys[idx] == key) {
            map->ctrls[idx] = DELETED;
            map->size--;
            return;
        }
        idx++;
        if (idx >= map->cap + MAX_PROBE - 1) idx = 0;
    }
}

void ttak_destroy_map(tt_map_t *map) {
    if (!map) return;
    ttak_map_arrays_destroy(map);
    ttak_mem_free(map);
}
