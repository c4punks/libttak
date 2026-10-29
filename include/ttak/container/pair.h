#ifndef TTAK_CONTAINER_PAIR_H
#define TTAK_CONTAINER_PAIR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <ttak/compat/static_alloc.h>

/**
 * @brief Bytes of caller-provided storage required for a static pair.
 *   static uint8_t storage[TTAK_PAIR_STATIC_STORAGE_SIZE(n)];
 */
#define TTAK_PAIR_STATIC_STORAGE_SIZE(length) \
    TTAK_STATIC_ALIGN_UP((length) * sizeof(void *), TTAK_STATIC_STORAGE_ALIGN)

/**
 * @brief Generic Pair/Tuple structure.
 * Despite the name "Pair", it can hold 'length' number of generic elements.
 */
typedef struct ttak_pair {
    size_t length;
    void **elements;
} ttak_pair_t;

/**
 * @brief Initialize a pair (tuple) with a fixed length.
 * @param pair Pointer to the pair structure.
 * @param length Number of elements this pair will hold.
 */
void ttak_pair_init(ttak_pair_t *pair, size_t length, uint64_t now);

/**
 * @brief Initializes a pair in caller-provided storage (zero-allocation).
 *
 * @param pair         Pair structure to initialize (caller-owned).
 * @param length       Number of elements this pair will hold.
 * @param storage      Backing storage for the element pointer array; must
 *                     hold at least TTAK_PAIR_STATIC_STORAGE_SIZE(length)
 *                     bytes.
 * @param storage_size Size of @p storage in bytes.
 * @return true on success, false if arguments or storage size are invalid.
 */
bool ttak_pair_init_static(ttak_pair_t *pair, size_t length,
                           void *storage, size_t storage_size);

/**
 * @brief Set an element at a specific index.
 */
void ttak_pair_set(ttak_pair_t *pair, size_t index, void *element);

/**
 * @brief Get an element at a specific index.
 */
void *ttak_pair_get(const ttak_pair_t *pair, size_t index);

/**
 * @brief Destroy the pair structure.
 * @param free_elem Optional function to free elements.
 */
void ttak_pair_destroy(ttak_pair_t *pair, void (*free_elem)(void*), uint64_t now);

#endif // TTAK_CONTAINER_PAIR_H
