#ifndef TTAK_SYNC_H
#define TTAK_SYNC_H

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <ttak/compat/pthread.h>
#include <stdint.h>
#include <ttak/compat/stdatomic.h>

typedef pthread_mutex_t ttak_mutex_t;
typedef pthread_mutex_t tt_mutex_t;

typedef pthread_cond_t ttak_cond_t;
typedef pthread_cond_t tt_cond_t;

typedef pthread_rwlock_t ttak_rwlock_t;
typedef pthread_rwlock_t tt_rwlock_t;

/*
 * Static (zero-allocation) initialization
 * ---------------------------------------
 * The sync primitives are plain pthread objects, so they need no storage
 * sizing macros and no init_static variants: they can be initialized at
 * compile time or placed in static storage with no heap involvement.
 *
 *   static ttak_mutex_t  mutex  = TTAK_MUTEX_INITIALIZER;
 *   static ttak_rwlock_t rwlock = TTAK_RWLOCK_INITIALIZER;
 *   ttak_mutex_lock(&mutex); ... ttak_mutex_destroy(&mutex);
 *
 * The *_init() functions below are equivalent for runtime-only init.
 */
/** @brief Static initializer for ttak_mutex_t (PTHREAD_MUTEX_INITIALIZER). */
#define TTAK_MUTEX_INITIALIZER  PTHREAD_MUTEX_INITIALIZER
/** @brief Static initializer for ttak_rwlock_t (PTHREAD_RWLOCK_INITIALIZER). */
#define TTAK_RWLOCK_INITIALIZER PTHREAD_RWLOCK_INITIALIZER

/**
 * @brief Generic shard structure.
 */
typedef struct ttak_shard {
    void            *data;
    ttak_rwlock_t   lock;
} ttak_shard_t;

typedef ttak_shard_t tt_shard_t;

/**
 * @brief Shared resource structure.
 */
typedef struct tt_type_shared {
    void            *data;
    int             size;
    pthread_mutex_t mutex;
    uint64_t        ts;    /** Inherited timestamp (tick count) */
} tt_type_shared_t;

typedef tt_type_shared_t ttak_type_shared_t;

// Mutex
static inline int ttak_mutex_init(ttak_mutex_t *mutex) {
    return pthread_mutex_init(mutex, NULL);
}

static inline int ttak_mutex_lock(ttak_mutex_t *mutex) {
    return pthread_mutex_lock(mutex);
}

static inline int ttak_mutex_unlock(ttak_mutex_t *mutex) {
    return pthread_mutex_unlock(mutex);
}

static inline int ttak_mutex_destroy(ttak_mutex_t *mutex) {
    return pthread_mutex_destroy(mutex);
}

// RWLock
static inline int ttak_rwlock_init(ttak_rwlock_t *rwlock) {
    return pthread_rwlock_init(rwlock, NULL);
}

static inline int ttak_rwlock_rdlock(ttak_rwlock_t *rwlock) {
    return pthread_rwlock_rdlock(rwlock);
}

static inline int ttak_rwlock_wrlock(ttak_rwlock_t *rwlock) {
    return pthread_rwlock_wrlock(rwlock);
}

static inline int ttak_rwlock_unlock(ttak_rwlock_t *rwlock) {
    return pthread_rwlock_unlock(rwlock);
}

static inline int ttak_rwlock_destroy(ttak_rwlock_t *rwlock) {
    return pthread_rwlock_destroy(rwlock);
}

// Shard
static inline int ttak_shard_init(ttak_shard_t *shard, void *data) {
    shard->data = data;
    return ttak_rwlock_init(&shard->lock);
}

static inline int ttak_shard_destroy(ttak_shard_t *shard) {
    return ttak_rwlock_destroy(&shard->lock);
}

// Condition Variable
static inline int ttak_cond_init(ttak_cond_t *cond) {
    return pthread_cond_init(cond, NULL);
}

static inline int ttak_cond_wait(ttak_cond_t *cond, ttak_mutex_t *mutex) {
    return pthread_cond_wait(cond, mutex);
}

static inline int ttak_cond_signal(ttak_cond_t *cond) {
    return pthread_cond_signal(cond);
}

static inline int ttak_cond_broadcast(ttak_cond_t *cond) {
    return pthread_cond_broadcast(cond);
}

static inline int ttak_cond_destroy(ttak_cond_t *cond) {
    return pthread_cond_destroy(cond);
}

#endif // TTAK_SYNC_H
