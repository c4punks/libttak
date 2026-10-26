#ifndef TTAK_PRIORITY_HEAP_H
#define TTAK_PRIORITY_HEAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Generic Binary Heap (Tree).
 * Implemented as a dynamic array.
 */
typedef struct ttak_heap_tree {
    void **data;
    size_t size;
    size_t capacity;
    
    // Comparator: returns < 0 if a < b, > 0 if a > b, 0 if equal.
    // Max-heap: a > b -> parent > child.
    int (*cmp)(const void *a, const void *b);
} ttak_heap_tree_t;

/**
 * @brief Initialize the heap.
 * @param initial_cap Initial capacity.
 * @param cmp Comparator function.
 */
void ttak_heap_tree_init(ttak_heap_tree_t *heap, size_t initial_cap, int (*cmp)(const void*, const void*));

/**
 * @brief Push an element into the heap.
 * @return true on success, false if the heap could not grow.
 */
_Bool ttak_heap_tree_push(ttak_heap_tree_t *heap, void *element, uint64_t now);

/**
 * @brief Pop the root element (min or max depending on comparator).
 */
void *ttak_heap_tree_pop(ttak_heap_tree_t *heap, uint64_t now);

/**
 * @brief Peek at the root element.
 */
void *ttak_heap_tree_peek(const ttak_heap_tree_t *heap);

/**
 * @brief Destroy the heap internal storage. Does not free the elements themselves.
 */
void ttak_heap_tree_destroy(ttak_heap_tree_t *heap, uint64_t now);

#endif // TTAK_PRIORITY_HEAP_H
