#include <ttak/ht/hash.h>
#include <ttak/ht/map.h>
#include <ttak/mem/mem.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PROBE TTAK_HT_MAX_PROBE

/* Dynamic maps are validated through the memory tracker; static maps
 * (caller-provided storage) bypass it, and under TTAK_NO_MALLOC it is not
 * even linked. */
#ifdef TTAK_NO_MALLOC
#define ttak_map_mem_access(map, ptr, now) ((void)(now), 1)
#else
#define ttak_map_mem_access(map, ptr, now) \
    ((map) != NULL && ((map)->is_static || ttak_mem_access((ptr), (now))))
#endif

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

/* Internal destructor: release both backing arrays regardless of NULL state. */
#ifndef TTAK_NO_MALLOC
static void ttak_map_arrays_destroy(tt_map_t *map) {
    if (map->ctrls)   { ttak_mem_free(map->ctrls);   map->ctrls   = NULL; }
    if (map->entries) { ttak_mem_free(map->entries); map->entries = NULL; }
}
#endif /* TTAK_NO_MALLOC */

/* Internal constructor: allocate the two arrays and zero only the
 * control bytes. Entries of non-OCCUPIED slots are never read, so
 * zero-initialising them is unnecessary work.
 * Returns 0 on success, -1 on any allocation failure (arrays freed on error). */
#ifndef TTAK_NO_MALLOC
static int ttak_map_arrays_alloc(tt_map_t *map, size_t padded_cap, uint64_t now) {
    map->ctrls   = ttak_mem_alloc_raw(padded_cap * sizeof(uint8_t),          __TTAK_UNSAFE_MEM_FOREVER__, now);
    map->entries = ttak_mem_alloc_raw(padded_cap * sizeof(ttak_map_entry_t), __TTAK_UNSAFE_MEM_FOREVER__, now);

    if (map->ctrls == NULL || map->entries == NULL) {
        ttak_map_arrays_destroy(map);
        return -1;
    }

    memset(map->ctrls,  0, padded_cap * sizeof(uint8_t));
    return 0;
}
#endif /* TTAK_NO_MALLOC */

tt_map_t *ttak_map_init_static(tt_map_t *map, size_t init_cap,
                               void *storage, size_t storage_size) {
    if (!map || !init_cap || !storage) return NULL;
    if (init_cap > SIZE_MAX / 2) return NULL;

    size_t cap = next_pow2(init_cap);
    if (cap < init_cap) return NULL;
    size_t padded_cap = cap + MAX_PROBE;
    if (padded_cap < cap) return NULL;

    size_t ctrl_bytes = TTAK_STATIC_ALIGN_UP(padded_cap, TTAK_STATIC_STORAGE_ALIGN);
    size_t needed = ctrl_bytes + padded_cap * sizeof(ttak_map_entry_t);
    if (storage_size < needed) return NULL;

    char *base = (char *)TTAK_STATIC_ALIGN_UP((uintptr_t)storage, TTAK_STATIC_STORAGE_ALIGN);
    uint8_t *ctrls = (uint8_t *)base;
    ttak_map_entry_t *entries = (ttak_map_entry_t *)(base + ctrl_bytes);

    map->ctrls = ctrls;
    map->entries = entries;
    map->cap = cap;
    map->size = 0;
    map->tombstones = 0;
    map->seed = 0xa0761d6478bd642fULL;
    map->is_static = 1;
    memset(ctrls, 0, padded_cap * sizeof(uint8_t));
    return map;
}

#ifndef TTAK_NO_MALLOC
tt_map_t *ttak_create_map(size_t init_cap, uint64_t now) {
    tt_map_t *map = ttak_mem_alloc_raw(sizeof(tt_map_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!map) return NULL;

    map->cap  = next_pow2(init_cap);
    map->size = 0;
    map->tombstones = 0;
    map->seed = 0xa0761d6478bd642fULL;
    map->ctrls   = NULL;
    map->entries = NULL;
    map->is_static = 0;

    // Allocate with padding to allow branchless linear probing
    size_t padded_cap = map->cap + MAX_PROBE;
    if (ttak_map_arrays_alloc(map, padded_cap, now) != 0) {
        ttak_mem_free(map);
        return NULL;
    }

    return map;
}

/* Rebuild the map into fresh arrays of @p new_cap slots. Only live entries
 * survive, so tombstones are cleared. @p new_cap may equal the current
 * capacity to rehash in place. */
static void ttak_resize_map_to(tt_map_t *map, uint64_t now, size_t new_cap) {
    size_t old_cap = map->cap;
    uint8_t *old_ctrls = map->ctrls;
    ttak_map_entry_t *old_entries = map->entries;

    tt_map_t *new_m = ttak_create_map(new_cap, now);
    if (!new_m) return;

    for (size_t i = 0; i < old_cap + MAX_PROBE; i++) {
        if (old_ctrls[i] == OCCUPIED) {
            ttak_insert_to_map(new_m, old_entries[i].key, old_entries[i].value, now);
        }
    }

    ttak_mem_free(old_ctrls);
    ttak_mem_free(old_entries);

    uint64_t s = map->seed;
    *map = *new_m;
    map->seed = s;
    ttak_mem_free(new_m);
}
#endif /* TTAK_NO_MALLOC */

void ttak_insert_to_map(tt_map_t *map, uintptr_t key, size_t val, uint64_t now) {
    if (!ttak_map_mem_access(map, map, now)) return;
    /* Static maps never grow: sizing must guarantee headroom below the
     * load-factor trigger. Dynamic maps resize as before. */
#ifndef TTAK_NO_MALLOC
    if (!map->is_static) {
        /* Tombstones count toward the load factor: they lengthen probe chains
         * just like live entries. If live entries alone stay under 70%, rehash
         * at the same capacity instead of growing. */
        if ((map->size + map->tombstones) * 10 >= map->cap * 7) {
            size_t new_cap = (map->size * 10 >= map->cap * 7) ? map->cap * 2 : map->cap;
            ttak_resize_map_to(map, now, new_cap);
        }
    }
#endif

    uint64_t h = gen_hash_wyhash(key, map->seed);
    size_t idx = h & (map->cap - 1);
    size_t first_tomb = SIZE_MAX;

    // Linear probing with padding - fewer branches
    while (map->ctrls[idx] != EMPTY) {
        if (map->ctrls[idx] == DELETED) {
            if (first_tomb == SIZE_MAX) first_tomb = idx;
        } else if (map->entries[idx].key == key) {
            map->entries[idx].value = val;
            return;
        }
        idx++;
        // If we hit the padding limit, we must wrap.
        // But with MAX_PROBE and 70% load, this is rare.
        if (idx >= map->cap + MAX_PROBE - 1) {
            idx = 0;
        }
    }

    if (first_tomb != SIZE_MAX) {
        idx = first_tomb;
        map->tombstones--;
    }

    map->ctrls[idx] = OCCUPIED;
    map->entries[idx].key = key;
    map->entries[idx].value = val;
    map->size++;
}

_Bool ttak_map_get_key(tt_map_t *map, uintptr_t key, size_t *out, uint64_t now) {
    if (!ttak_map_mem_access(map, map, now)) return 0;
    uint64_t h = gen_hash_wyhash(key, map->seed);
    size_t idx = h & (map->cap - 1);

    while (map->ctrls[idx] != EMPTY) {
        if (map->ctrls[idx] == OCCUPIED && map->entries[idx].key == key) {
            if (out) *out = map->entries[idx].value;
            return 1;
        }
        idx++;
        if (idx >= map->cap + MAX_PROBE - 1) idx = 0;
    }
    return 0;
}

void ttak_delete_from_map(tt_map_t *map, uintptr_t key, uint64_t now) {
    if (!ttak_map_mem_access(map, map, now)) return;
    uint64_t h = gen_hash_wyhash(key, map->seed);
    size_t idx = h & (map->cap - 1);

    while (map->ctrls[idx] != EMPTY) {
        if (map->ctrls[idx] == OCCUPIED && map->entries[idx].key == key) {
            map->ctrls[idx] = DELETED;
            map->size--;
            map->tombstones++;
            return;
        }
        idx++;
        if (idx >= map->cap + MAX_PROBE - 1) idx = 0;
    }
}

void ttak_destroy_map(tt_map_t *map) {
    if (!map) return;
    if (map->is_static) {
        map->ctrls = NULL;
        map->entries = NULL;
        map->size = 0;
        map->tombstones = 0;
        return;
    }
#ifndef TTAK_NO_MALLOC
    ttak_map_arrays_destroy(map);
    ttak_mem_free(map);
#endif
}
