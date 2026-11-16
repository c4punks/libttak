#ifndef TTAK_NET_LATTICE_H
#define TTAK_NET_LATTICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ttak/mem/mem.h>
#include <ttak/sync/sync.h>
#include <ttak/atomic/atomic.h>

/**
 * @file lattice.h
 * @brief Lock-free parallel ingress buffer using a 2-D slot lattice.
 */

#define TTAK_LATTICE_SLOT_SIZE 16384
#define TTAK_LATTICE_MAX_DIM 16

/**
 * @brief A single data slot in the lattice.
 */
typedef struct ttak_net_lattice_slot {
    _Alignas(64) uint8_t  data[TTAK_LATTICE_SLOT_SIZE];
    uint64_t timestamp;
    uint32_t len;
    uint32_t seq;
    volatile uint64_t state;
    uint8_t padding[32]; /* Pad to ensure 64-byte multiple if needed */
} ttak_net_lattice_slot_t;

/**
 * @brief The lattice structure.
 */
typedef struct ttak_net_lattice {
    uint32_t dim;      /* Dimension of the square (must be power of 2) */
    uint32_t mask;     /* dim - 1 */
    uint32_t capacity; /* Total number of slots */
    ttak_net_lattice_slot_t *slots; /* dim * dim array */
    volatile uint64_t total_ingress;
    volatile uint64_t used_slots;
    _Atomic uint32_t compact_state;
    _Bool is_full;
    _Bool is_stub;
    struct ttak_net_lattice *prev;
    struct ttak_net_lattice *next; /* Grows when the lattice saturates */
    ttak_mutex_t expand_lock;
} ttak_net_lattice_t;

/**
 * @brief Initializes a net lattice with given dimension.
 */
ttak_net_lattice_t* ttak_net_lattice_create(uint32_t dim, uint64_t now);

/**
 * @brief Destroys the lattice.
 */
void ttak_net_lattice_destroy(ttak_net_lattice_t *lat, uint64_t now);

/**
 * @brief Lock-free deterministic write into the lattice.
 */
_Bool ttak_net_lattice_write(ttak_net_lattice_t *lat, uint32_t tid, const void *data, uint32_t len, uint64_t now);

/**
 * @brief Reads data from a designated slot in the lattice for a specific worker (tid).
 */
_Bool ttak_net_lattice_read(ttak_net_lattice_t *lat, uint32_t tid, void *dst, uint32_t *len_out, uint64_t now);

/**
 * @brief Returns the global default lattice.
 */
ttak_net_lattice_t* ttak_net_lattice_get_default(void);

/**
 * @brief Sets the worker ID for the current thread to be used in deterministic lattice selection.
 */
void ttak_net_lattice_set_worker_id(uint32_t tid);

/**
 * @brief Gets the worker ID for the current thread.
 */
uint32_t ttak_net_lattice_get_worker_id(void);

/**
 * @brief Reports aggregate worker load for automatic lattice activation.
 *
 * Automatic mode turns on at 75% utilization and stays on until utilization
 * falls below 50%. A zero capacity disables automatic mode. Explicit
 * TTAK_NET_ROLE_LATTICE_ACCEL endpoints are not affected by this setting.
 */
void ttak_net_lattice_set_load(uint32_t active, uint32_t capacity);

/**
 * @brief Returns whether automatic lattice acceleration is currently active.
 */
_Bool ttak_net_lattice_auto_enabled(void);

/**
 * @brief Ensures the current lattice has a successor allocated when needed.
 */
ttak_net_lattice_t* ttak_net_lattice_ensure_next(ttak_net_lattice_t *lat, uint64_t now);

/**
 * @brief Tracks that a slot has been acquired from @p lat and triggers prefetching.
 */
void ttak_net_lattice_mark_slot_acquired(ttak_net_lattice_t *lat, uint64_t now);

/**
 * @brief Tracks that a slot has been released back to @p lat.
 */
void ttak_net_lattice_mark_slot_released(ttak_net_lattice_t *lat);

#endif /* TTAK_NET_LATTICE_H */
