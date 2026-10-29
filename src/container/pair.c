#include <ttak/container/pair.h>
#include <ttak/mem/mem.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Initialize a pair in caller-provided storage (zero-allocation).
 *
 * @param pair         Pair structure to configure.
 * @param length       Number of entries the pair should manage.
 * @param storage      Backing storage for the element pointer array; must
 *                     hold at least TTAK_PAIR_STATIC_STORAGE_SIZE(length)
 *                     bytes.
 * @param storage_size Size of @p storage in bytes.
 * @return true on success, false if arguments or storage size are invalid.
 */
bool ttak_pair_init_static(ttak_pair_t *pair, size_t length,
                           void *storage, size_t storage_size) {
    if (!pair) return false;
    size_t needed = TTAK_STATIC_ALIGN_UP(length * sizeof(void *), TTAK_STATIC_STORAGE_ALIGN);
    if (length > 0 && (!storage || storage_size < needed)) return false;

    pair->length = length;
    pair->elements = length > 0 ? (void **)storage : NULL;
    if (length > 0) memset(pair->elements, 0, length * sizeof(void *));
    return true;
}

#ifndef TTAK_NO_MALLOC
/**
 * @brief Initialize a fixed-length pair container.
 *
 * @param pair   Pair structure to configure.
 * @param length Number of entries the pair should manage.
 * @param now    Current timestamp for memory accounting.
 */
void ttak_pair_init(ttak_pair_t *pair, size_t length, uint64_t now) {
    if (!pair) return;
    pair->length = length;
    if (length > 0) {
        pair->elements = (void **)ttak_mem_alloc_raw(sizeof(void *) * length, __TTAK_UNSAFE_MEM_FOREVER__, now);
    } else {
        pair->elements = NULL;
    }
}
#endif /* TTAK_NO_MALLOC */

/**
 * @brief Assign an element to the given index.
 *
 * @param pair    Pair container holding the slots.
 * @param index   Index to update.
 * @param element Value to store.
 */
void ttak_pair_set(ttak_pair_t *pair, size_t index, void *element) {
    if (!pair || !pair->elements || index >= pair->length) return;
    // Access check not strictly required for the array pointer itself if we own it,
    // but good practice if referencing potentially stale pair.
    pair->elements[index] = element;
}

/**
 * @brief Retrieve an element from the pair.
 *
 * @param pair  Pair container to inspect.
 * @param index Slot to read.
 * @return Stored pointer or NULL if out of range.
 */
void *ttak_pair_get(const ttak_pair_t *pair, size_t index) {
    if (!pair || !pair->elements || index >= pair->length) return NULL;
    return pair->elements[index];
}

/**
 * @brief Release the pair container and optionally free stored items.
 *
 * @param pair      Container to tear down.
 * @param free_elem Callback invoked for each non-NULL element (optional).
 * @param now       Timestamp for memory validation.
 */
void ttak_pair_destroy(ttak_pair_t *pair, void (*free_elem)(void*), uint64_t now) {
    if (!pair) return;
    (void)now;
    
    if (pair->elements) {
#ifndef TTAK_NO_MALLOC
        if (ttak_mem_access(pair->elements, now)) {
            if (free_elem) {
                for (size_t i = 0; i < pair->length; i++) {
                    if (pair->elements[i]) {
                        free_elem(pair->elements[i]);
                    }
                }
            }
            ttak_mem_free(pair->elements);
        }
#else
        if (free_elem) {
            for (size_t i = 0; i < pair->length; i++) {
                if (pair->elements[i]) {
                    free_elem(pair->elements[i]);
                }
            }
        }
#endif
    }
    pair->elements = NULL;
    pair->length = 0;
}
