/**
 * @file test_static_init.c
 * @brief Exercises every zero-allocation (static storage) init API.
 *
 * Each test initializes the module entirely from caller-provided storage
 * (file-scope static buffers and stack buffers), runs a full
 * push/insert/pop lifecycle, and verifies that the sizing macros are
 * sufficient. When the suite is built with -DTTAK_NO_MALLOC the same
 * sources are recompiled so any heap use fails the build (see the
 * zero-alloc-test Makefile target and the CI job in unit_tests.yml).
 */

#include "test_macros.h"

#include <ttak/container/ringbuf.h>
#include <ttak/container/pool.h>
#include <ttak/container/pair.h>
#include <ttak/container/set.h>
#include <ttak/ht/map.h>
#include <ttak/ht/table.h>
#include <ttak/sync/sync.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* ringbuf                                                             */
/* ------------------------------------------------------------------ */

#define RB_CAPACITY 16
#define RB_ITEM_SIZE sizeof(uint64_t)

static uint8_t g_rb_storage[TTAK_RINGBUF_STATIC_STORAGE_SIZE(RB_CAPACITY, RB_ITEM_SIZE)];

static void test_ringbuf_static(void) {
    ttak_ringbuf_t rb;

    /* Sizing macro must be sufficient and the API must reject short storage. */
    ASSERT(TTAK_RINGBUF_STATIC_STORAGE_SIZE(RB_CAPACITY, RB_ITEM_SIZE) >=
           RB_CAPACITY * RB_ITEM_SIZE);
    ASSERT(!ttak_ringbuf_init_static(&rb, RB_CAPACITY, RB_ITEM_SIZE, g_rb_storage,
                                     RB_CAPACITY * RB_ITEM_SIZE - 1));
    ASSERT(ttak_ringbuf_init_static(&rb, RB_CAPACITY, RB_ITEM_SIZE, g_rb_storage,
                                    sizeof(g_rb_storage)));
    ASSERT(ttak_ringbuf_is_empty(&rb));
    ASSERT(!ttak_ringbuf_is_full(&rb));

    /* Fill to capacity, verify full, drain completely. */
    for (uint64_t i = 0; i < RB_CAPACITY; i++) {
        ASSERT(ttak_ringbuf_push(&rb, &i));
        ASSERT(ttak_ringbuf_count(&rb) == i + 1);
    }
    ASSERT(ttak_ringbuf_is_full(&rb));
    ASSERT(!ttak_ringbuf_push(&rb, &(uint64_t){99}));

    for (uint64_t i = 0; i < RB_CAPACITY; i++) {
        uint64_t out = 0;
        ASSERT(ttak_ringbuf_pop(&rb, &out));
        ASSERT(out == i);
    }
    ASSERT(ttak_ringbuf_is_empty(&rb));
    ASSERT(!ttak_ringbuf_pop(&rb, &(uint64_t){0}));
    ASSERT(ttak_ringbuf_count(&rb) == 0);

    /* Non-power-of-two capacity also works. */
    ttak_ringbuf_t rb2;
    static uint8_t storage2[TTAK_RINGBUF_STATIC_STORAGE_SIZE(10, sizeof(int))];
    ASSERT(ttak_ringbuf_init_static(&rb2, 10, sizeof(int), storage2, sizeof(storage2)));
    for (int i = 0; i < 10; i++) ASSERT(ttak_ringbuf_push(&rb2, &i));
    for (int i = 0; i < 10; i++) {
        int out = -1;
        ASSERT(ttak_ringbuf_pop(&rb2, &out));
        ASSERT(out == i);
    }

    ttak_rwlock_destroy(&rb.lock);
    ttak_rwlock_destroy(&rb2.lock);
}

/* ------------------------------------------------------------------ */
/* object pool                                                         */
/* ------------------------------------------------------------------ */

typedef struct { uint64_t a, b, c; } pool_item_t;

#define POOL_CAPACITY 32

static uint8_t g_pool_storage[TTAK_POOL_STATIC_STORAGE_SIZE(POOL_CAPACITY, sizeof(pool_item_t))];

static void test_pool_static(void) {
    ttak_object_pool_t pool;

    ASSERT(!ttak_object_pool_init_static(&pool, POOL_CAPACITY, sizeof(pool_item_t),
                                         g_pool_storage, 1));
    ASSERT(ttak_object_pool_init_static(&pool, POOL_CAPACITY, sizeof(pool_item_t),
                                        g_pool_storage, sizeof(g_pool_storage)));

    pool_item_t *items[POOL_CAPACITY];
    for (size_t i = 0; i < POOL_CAPACITY; i++) {
        items[i] = ttak_object_pool_alloc(&pool);
        ASSERT(items[i] != NULL);
        items[i]->a = i;
        items[i]->b = i * 2;
        items[i]->c = i * 3;
    }
    /* Pool exhausted: no allocation from nowhere. */
    ASSERT(ttak_object_pool_alloc(&pool) == NULL);

    for (size_t i = 0; i < POOL_CAPACITY; i++) {
        ASSERT(items[i]->a == i && items[i]->b == i * 2 && items[i]->c == i * 3);
        ttak_object_pool_free(&pool, items[i]);
    }
    /* After free-all, full capacity is available again. */
    for (size_t i = 0; i < POOL_CAPACITY; i++) {
        ASSERT(ttak_object_pool_alloc(&pool) != NULL);
    }
    ASSERT(ttak_object_pool_alloc(&pool) == NULL);
}

/* ------------------------------------------------------------------ */
/* hash map                                                            */
/* ------------------------------------------------------------------ */

#define MAP_CAPACITY 64
#define MAP_ITEMS 32 /* stay below the 70% load-factor trigger */

static uint8_t g_map_storage[TTAK_MAP_STATIC_STORAGE_SIZE(MAP_CAPACITY)];

static void test_map_static(void) {
    tt_map_t map;

    ASSERT(!ttak_map_init_static(&map, MAP_CAPACITY, g_map_storage,
                                 TTAK_MAP_STATIC_STORAGE_SIZE(MAP_CAPACITY) - 1));
    ASSERT(ttak_map_init_static(&map, MAP_CAPACITY, g_map_storage,
                                sizeof(g_map_storage)) == &map);

    for (uintptr_t k = 1; k <= MAP_ITEMS; k++) {
        ttak_insert_to_map(&map, k, (size_t)(k * 10), 0);
    }
    ASSERT(map.size == MAP_ITEMS);
    for (uintptr_t k = 1; k <= MAP_ITEMS; k++) {
        size_t out = 0;
        ASSERT(ttak_map_get_key(&map, k, &out, 0));
        ASSERT(out == (size_t)(k * 10));
    }
    /* Overwrite existing key. */
    ttak_insert_to_map(&map, 7, 777, 0);
    size_t out = 0;
    ASSERT(ttak_map_get_key(&map, 7, &out, 0) && out == 777);
    ASSERT(map.size == MAP_ITEMS);

    /* Delete half, verify, re-insert. */
    for (uintptr_t k = 1; k <= MAP_ITEMS; k += 2) {
        ttak_delete_from_map(&map, k, 0);
    }
    ASSERT(map.size == MAP_ITEMS / 2);
    for (uintptr_t k = 1; k <= MAP_ITEMS; k++) {
        bool found = ttak_map_get_key(&map, k, &out, 0);
        if (k % 2 == 0) {
            ASSERT(found && out == (size_t)(k * 10));
        } else {
            ASSERT(!found);
        }
    }
    for (uintptr_t k = 1; k <= MAP_ITEMS; k += 2) {
        ttak_insert_to_map(&map, k, (size_t)(k * 10), 0);
    }
    ASSERT(map.size == MAP_ITEMS);

    /* Destroy must not free caller storage; storage must be reusable. */
    ttak_destroy_map(&map);
    ASSERT(ttak_map_init_static(&map, MAP_CAPACITY, g_map_storage,
                                sizeof(g_map_storage)) == &map);
    ASSERT(map.size == 0);
}

/* ------------------------------------------------------------------ */
/* hash table                                                          */
/* ------------------------------------------------------------------ */

#define TABLE_CAPACITY 64
#define TABLE_ITEMS 32

static uint8_t g_table_storage[TTAK_TABLE_STATIC_STORAGE_SIZE(TABLE_CAPACITY)];
static uint8_t g_set_storage[TTAK_SET_STATIC_STORAGE_SIZE(TABLE_CAPACITY)];

static int u64_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void test_table_static(void) {
    ttak_table_t table;

    ASSERT(!ttak_table_init_static(&table, TABLE_CAPACITY, g_table_storage, 1,
                                   NULL, u64_cmp, NULL, NULL));
    ASSERT(ttak_table_init_static(&table, TABLE_CAPACITY, g_table_storage,
                                  sizeof(g_table_storage), NULL, u64_cmp, NULL, NULL));

    uint64_t keys[TABLE_ITEMS];
    for (size_t i = 0; i < TABLE_ITEMS; i++) {
        keys[i] = i * 100 + 7;
        char *value = (char *)(uintptr_t)(i + 1); /* opaque tag value */
        ttak_table_put(&table, &keys[i], sizeof(keys[i]), value, 0);
    }
    ASSERT(table.size == TABLE_ITEMS);
    for (size_t i = 0; i < TABLE_ITEMS; i++) {
        void *v = ttak_table_get(&table, &keys[i], sizeof(keys[i]), 0);
        ASSERT(v == (void *)(uintptr_t)(i + 1));
    }

    /* Replace an existing value. */
    ttak_table_put(&table, &keys[5], sizeof(keys[5]), (void *)(uintptr_t)555, 0);
    ASSERT(table.size == TABLE_ITEMS);
    ASSERT(ttak_table_get(&table, &keys[5], sizeof(keys[5]), 0) == (void *)(uintptr_t)555);

    /* Remove every other entry. */
    for (size_t i = 0; i < TABLE_ITEMS; i += 2) {
        ASSERT(ttak_table_remove(&table, &keys[i], sizeof(keys[i]), 0));
    }
    ASSERT(table.size == TABLE_ITEMS / 2);
    for (size_t i = 0; i < TABLE_ITEMS; i++) {
        void *v = ttak_table_get(&table, &keys[i], sizeof(keys[i]), 0);
        if (i % 2 == 0) ASSERT(v == NULL);
        else ASSERT(v == (void *)(uintptr_t)(i == 5 ? 555 : i + 1));
    }

    ttak_table_destroy(&table, 0);
    ASSERT(table.ctrls == NULL && table.entries == NULL && table.size == 0);
}

/* ------------------------------------------------------------------ */
/* set                                                                 */
/* ------------------------------------------------------------------ */

static void test_set_static(void) {
    ttak_set_t set;

    ASSERT(!ttak_set_init_static(&set, TABLE_CAPACITY, g_set_storage, 1,
                                 NULL, u64_cmp, NULL));
    ASSERT(ttak_set_init_static(&set, TABLE_CAPACITY, g_set_storage,
                                sizeof(g_set_storage), NULL, u64_cmp, NULL));

    uint64_t keys[TABLE_ITEMS];
    for (size_t i = 0; i < TABLE_ITEMS; i++) {
        keys[i] = i + 1;
        ttak_set_add(&set, &keys[i], sizeof(keys[i]), 0);
    }
    for (size_t i = 0; i < TABLE_ITEMS; i++) {
        ASSERT(ttak_set_contains(&set, &keys[i], sizeof(keys[i]), 0));
    }
    uint64_t missing = 9999;
    ASSERT(!ttak_set_contains(&set, &missing, sizeof(missing), 0));

    /* Duplicates are no-ops. */
    ttak_set_add(&set, &keys[3], sizeof(keys[3]), 0);

    for (size_t i = 0; i < TABLE_ITEMS; i += 3) {
        ASSERT(ttak_set_remove(&set, &keys[i], sizeof(keys[i]), 0));
    }
    for (size_t i = 0; i < TABLE_ITEMS; i++) {
        bool has = ttak_set_contains(&set, &keys[i], sizeof(keys[i]), 0);
        if (i % 3 == 0) ASSERT(!has);
        else ASSERT(has);
    }

    ttak_set_destroy(&set, 0);
    ASSERT(set.table.ctrls == NULL);
}

/* ------------------------------------------------------------------ */
/* pair                                                                */
/* ------------------------------------------------------------------ */

#define PAIR_LENGTH 4

static uint8_t g_pair_storage[TTAK_PAIR_STATIC_STORAGE_SIZE(PAIR_LENGTH)];

static void test_pair_static(void) {
    ttak_pair_t pair;

    ASSERT(ttak_pair_init_static(&pair, PAIR_LENGTH, g_pair_storage,
                                 sizeof(g_pair_storage)));
    ASSERT(pair.length == PAIR_LENGTH);

    int a = 1, b = 2, c = 3, d = 4;
    ttak_pair_set(&pair, 0, &a);
    ttak_pair_set(&pair, 1, &b);
    ttak_pair_set(&pair, 2, &c);
    ttak_pair_set(&pair, 3, &d);
    ASSERT(*(int *)ttak_pair_get(&pair, 0) == 1);
    ASSERT(*(int *)ttak_pair_get(&pair, 3) == 4);
    ASSERT(ttak_pair_get(&pair, PAIR_LENGTH) == NULL);

    ttak_pair_destroy(&pair, NULL, 0);
    ASSERT(pair.elements == NULL && pair.length == 0);
}

/* ------------------------------------------------------------------ */
/* sync primitives (already static-init friendly)                      */
/* ------------------------------------------------------------------ */

static ttak_mutex_t g_static_mutex = TTAK_MUTEX_INITIALIZER;
static ttak_rwlock_t g_static_rwlock = TTAK_RWLOCK_INITIALIZER;

static void test_sync_static(void) {
    ASSERT(ttak_mutex_lock(&g_static_mutex) == 0);
    ASSERT(ttak_mutex_unlock(&g_static_mutex) == 0);
    ASSERT(ttak_mutex_destroy(&g_static_mutex) == 0);

    ASSERT(ttak_rwlock_wrlock(&g_static_rwlock) == 0);
    ASSERT(ttak_rwlock_unlock(&g_static_rwlock) == 0);
    ASSERT(ttak_rwlock_destroy(&g_static_rwlock) == 0);

    /* Runtime init on stack storage also requires no allocation. */
    ttak_mutex_t m;
    ASSERT(ttak_mutex_init(&m) == 0);
    ASSERT(ttak_mutex_lock(&m) == 0);
    ASSERT(ttak_mutex_unlock(&m) == 0);
    ASSERT(ttak_mutex_destroy(&m) == 0);
}

/* ------------------------------------------------------------------ */

int main(void) {
    RUN_TEST(test_ringbuf_static);
    RUN_TEST(test_pool_static);
    RUN_TEST(test_map_static);
    RUN_TEST(test_table_static);
    RUN_TEST(test_set_static);
    RUN_TEST(test_pair_static);
    RUN_TEST(test_sync_static);
    printf("All static-init tests passed.\n");
    return 0;
}
