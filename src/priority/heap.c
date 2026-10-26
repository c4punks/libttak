#include <ttak/priority/heap.h>
#include <ttak/mem/mem.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Initialize a binary heap container.
 *
 * @param heap        Heap to initialize.
 * @param initial_cap Desired starting capacity.
 * @param cmp         Comparator returning positive when first arg has higher priority.
 */
void ttak_heap_tree_init(ttak_heap_tree_t *heap, size_t initial_cap, int (*cmp)(const void*, const void*)) {
    if (!heap) return;
    heap->cmp = cmp;
    heap->size = 0;
    heap->capacity = initial_cap > 0 ? initial_cap : 16;
    heap->data = (void **)ttak_mem_alloc_raw(sizeof(void *) * heap->capacity, __TTAK_UNSAFE_MEM_FOREVER__, 0);
}

static void swap(void **a, void **b) {
    void *temp = *a;
    *a = *b;
    *b = temp;
}

static void heapify_up(ttak_heap_tree_t *heap, size_t index) {
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        // If child violates heap property relative to parent (child > parent for max heap logic if cmp returns a-b)
        // standard comparator: a < b => negative.
        // Let's assume Min-Heap behavior for "priority queue" defaults usually, or Max.
        // If cmp(child, parent) > 0, we swap. This means "child has higher priority".
        if (heap->cmp(heap->data[index], heap->data[parent]) > 0) {
            swap(&heap->data[index], &heap->data[parent]);
            index = parent;
        } else {
            break;
        }
    }
}

static void heapify_down(ttak_heap_tree_t *heap, size_t index) {
    size_t left, right, largest;
    
    while (true) {
        left = 2 * index + 1;
        right = 2 * index + 2;
        largest = index;

        if (left < heap->size && heap->cmp(heap->data[left], heap->data[largest]) > 0) {
            largest = left;
        }

        if (right < heap->size && heap->cmp(heap->data[right], heap->data[largest]) > 0) {
            largest = right;
        }

        if (largest != index) {
            swap(&heap->data[index], &heap->data[largest]);
            index = largest;
        } else {
            break;
        }
    }
}

/**
 * @brief Insert an element into the heap.
 *
 * @param heap    Heap to update.
 * @param element Payload pointer to push.
 * @param now     Timestamp for allocator bookkeeping.
 * @return true on success, false if the heap could not grow.
 */
_Bool ttak_heap_tree_push(ttak_heap_tree_t *heap, void *element, uint64_t now) {
    if (!heap || !heap->data) return 0;

    if (heap->size >= heap->capacity) {
        size_t new_cap = heap->capacity * 2;
        void **new_data = (void **)ttak_mem_realloc_raw(heap->data, sizeof(void *) * new_cap, __TTAK_UNSAFE_MEM_FOREVER__, now);
        if (!new_data) return 0; // Allocation failed
        heap->data = new_data;
        heap->capacity = new_cap;
    }

    heap->data[heap->size] = element;
    heapify_up(heap, heap->size);
    heap->size++;
    return 1;
}

/**
 * @brief Remove and return the highest-priority element.
 *
 * @param heap Heap to pop from.
 * @param now  Timestamp (unused but kept for interface parity).
 * @return Pointer to the popped element, or NULL if empty.
 */
void *ttak_heap_tree_pop(ttak_heap_tree_t *heap, uint64_t now) {
    (void)now; // Access verification not strictly needed for array slots unless checking the array pointer itself
    if (!heap || !heap->data || heap->size == 0) return NULL;

    void *root = heap->data[0];
    heap->data[0] = heap->data[heap->size - 1];
    heap->size--;
    
    if (heap->size > 0) {
        heapify_down(heap, 0);
    }

    return root;
}

/**
 * @brief Access the top element without removing it.
 *
 * @param heap Heap to inspect.
 * @return Pointer to the element, or NULL if empty.
 */
void *ttak_heap_tree_peek(const ttak_heap_tree_t *heap) {
    if (!heap || !heap->data || heap->size == 0) return NULL;
    return heap->data[0];
}

/**
 * @brief Destroy the heap and release its storage.
 *
 * @param heap Heap to destroy.
 * @param now  Timestamp (unused).
 */
void ttak_heap_tree_destroy(ttak_heap_tree_t *heap, uint64_t now) {
    (void)now;
    if (!heap) return;
    if (heap->data) {
        ttak_mem_free(heap->data);
        heap->data = NULL;
    }
    heap->size = 0;
    heap->capacity = 0;
}
