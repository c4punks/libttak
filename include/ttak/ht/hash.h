/**
 * @file hash.h
 * @brief Low-level open-addressing hash map (SoA layout) with SipHash/wyhash.
 *
 * @c ttak_map_t is the inner SoA table used by the higher-level map and
 * table APIs.  Control bytes follow the Swiss-table convention:
 * @c EMPTY (0x00), @c DELETED (0xDE), @c OCCUPIED (0x0C).
 */

#ifndef __TTAK_HASH_H__
#define __TTAK_HASH_H__

#include <stdint.h>
#include <stddef.h>
#include <stdalign.h>
#include <ttak/types/ttak_align.h>

/** @brief Slot is empty and was never used. */
#define EMPTY    0x00
/** @brief Slot was occupied but has been deleted (tombstone). */
#define DELETED  0xDE
/** @brief Slot holds a live key/value pair. */
#define OCCUPIED 0x0C

/**
 * @brief Fused key/value slot for the open-addressing map (AoS layout).
 *
 * Storing the key next to its value halves the cache misses per successful
 * lookup compared to separate keys/values arrays.
 */
typedef struct {
    uintptr_t key;   /**< Slot key (only meaningful when ctrl is OCCUPIED). */
    size_t    value; /**< Slot value associated with @c key. */
} ttak_map_entry_t;

/**
 * @brief Map structure using control bytes plus a fused entries array.
 */
typedef struct {
    uint8_t          *ctrls;   /**< Control bytes (OCCUPIED, EMPTY, DELETED) */
    ttak_map_entry_t *entries; /**< Fused key/value slots */
    size_t    cap;       /**< Capacity (must be power of two) */
    size_t    size;      /**< Number of live key/value pairs */
    size_t    tombstones;/**< Slots marked DELETED; count toward the resize trigger */
    uint64_t  seed;      /**< Seed for wyhash */
#ifndef _MSC_VER
    alignas(ttak_max_align_t) char padding[0];
#endif
} ttak_map_t;

typedef ttak_map_t tt_map_t;

/**
 * @brief Computes a 64-bit SipHash-2-4 digest of a single uintptr_t key.
 *
 * @param key  Key to hash.
 * @param k0   First half of the 128-bit SipHash seed.
 * @param k1   Second half of the 128-bit SipHash seed.
 * @return     64-bit digest.
 */
uint64_t gen_hash_sip24(uintptr_t key, uint64_t k0, uint64_t k1);

/**
 * @brief Computes a 64-bit wyhash digest of a single uintptr_t key.
 *
 * @param key  Key to hash.
 * @param seed wyhash seed.
 * @return     64-bit digest.
 */
uint64_t gen_hash_wyhash(uintptr_t key, uint64_t seed);

#endif // __TTAK_HASH_H__
