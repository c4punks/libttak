/**
 * EBR grace period: a node retired in epoch E must survive until every
 * reader that could have seen it has left, i.e. until the global epoch has
 * moved TTAK_EPOCH_GRACE epochs past E. Reclamation used to free every
 * queued node as soon as no reader lagged behind the current epoch,
 * including nodes retired in that same epoch while a reader inside it could
 * still hold them.
 */
#include <ttak/mem/epoch.h>
#include <pthread.h>
#include <stdatomic.h>
#include "test_macros.h"

static atomic_int g_freed = 0;

static void count_free(void *p) {
    (void)p;
    atomic_fetch_add(&g_freed, 1);
}

static int g_dummy[4];

/* --- a reader inside the retire epoch keeps the node alive --------------- */

static atomic_int g_stage = 0;

static void *reader_thread(void *arg) {
    (void)arg;
    ttak_epoch_enter();
    atomic_store(&g_stage, 1); /* inside the current epoch */
    while (atomic_load(&g_stage) < 2) {
    }
    ttak_epoch_exit();
    atomic_store(&g_stage, 3);
    return NULL;
}

static void test_reader_blocks_reclaim(void) {
    atomic_store(&g_freed, 0);
    pthread_t t;
    ASSERT(pthread_create(&t, NULL, reader_thread, NULL) == 0);
    while (atomic_load(&g_stage) < 1) {
    }

    /* Retired in the reader's epoch: the reader may still hold it. */
    ttak_epoch_retire(&g_dummy[0], count_free);
    for (int i = 0; i < 8; i++) ttak_epoch_reclaim();
    ASSERT_MSG(atomic_load(&g_freed) == 0, "freed %d node(s) under an active reader",
               atomic_load(&g_freed));

    atomic_store(&g_stage, 2);
    while (atomic_load(&g_stage) < 3) {
    }
    pthread_join(t, NULL);
    for (int i = 0; i < (int)TTAK_EPOCH_GRACE; i++) ttak_epoch_reclaim();
    ASSERT(atomic_load(&g_freed) == 1);
}

/* --- no readers: TTAK_EPOCH_GRACE reclaims free everything ------------- */

static void test_grace_without_readers(void) {
    atomic_store(&g_freed, 0);
    ttak_epoch_retire(&g_dummy[1], count_free);
    ttak_epoch_retire(&g_dummy[2], count_free);
    ttak_epoch_reclaim();
    /* One epoch of age is not enough. */
    ASSERT(atomic_load(&g_freed) == 0);
    for (int i = 1; i < (int)TTAK_EPOCH_GRACE; i++) ttak_epoch_reclaim();
    ASSERT(atomic_load(&g_freed) == 2);
}

/* --- old nodes are freed, young ones in the same bucket kept ----------- */

static void test_mixed_ages(void) {
    atomic_store(&g_freed, 0);
    ttak_epoch_retire(&g_dummy[0], count_free); /* ages first */
    ttak_epoch_reclaim();
    ttak_epoch_retire(&g_dummy[1], count_free); /* one epoch younger */
    ttak_epoch_reclaim();
    ASSERT(atomic_load(&g_freed) == 1);
    ttak_epoch_reclaim();
    ASSERT(atomic_load(&g_freed) == 2);
}

int main(void) {
    RUN_TEST(test_reader_blocks_reclaim);
    RUN_TEST(test_grace_without_readers);
    RUN_TEST(test_mixed_ages);
    return 0;
}
