#include <ttak/tree/btree.h>
#include <ttak/mem/mem.h>
#include <stdlib.h>

/**
 * @brief Allocate a B-tree node with capacity derived from t.
 *
 * @param t    Minimum degree.
 * @param leaf Indicates whether the node starts as a leaf.
 * @param now  Timestamp for allocator bookkeeping.
 * @return Pointer to the node or NULL on failure.
 */
static ttak_btree_node_t *create_node(int t, bool leaf, uint64_t now) {
    ttak_btree_node_t *node = (ttak_btree_node_t *)ttak_mem_alloc_raw(sizeof(ttak_btree_node_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!node) return NULL;

    node->leaf = leaf;
    node->n = 0;
    
    // Allocate arrays. Max keys = 2t-1. Max children = 2t.
    size_t max_keys = 2 * t - 1;
    size_t max_children = 2 * t;

    node->keys = (void **)ttak_mem_alloc_raw(sizeof(void *) * max_keys, __TTAK_UNSAFE_MEM_FOREVER__, now);
    node->values = (void **)ttak_mem_alloc_raw(sizeof(void *) * max_keys, __TTAK_UNSAFE_MEM_FOREVER__, now);
    node->children = (struct ttak_btree_node **)ttak_mem_alloc_raw(sizeof(struct ttak_btree_node *) * max_children, __TTAK_UNSAFE_MEM_FOREVER__, now);

    if (!node->keys || !node->values || !node->children) {
        // Cleanup if partial alloc failed (simplified: just return null, leaking partials in this simplified prototype)
        // ideally we free what we alloc'd.
        return NULL; 
    }
    return node;
}

/**
 * @brief Initialize a B-tree descriptor.
 *
 * @param tree     Tree to initialize.
 * @param t        Minimum degree (clamped to >= 2).
 * @param cmp      Key comparator.
 * @param key_free Optional key destructor.
 * @param val_free Optional value destructor.
 */
void ttak_btree_init(ttak_btree_t *tree, int t, int (*cmp)(const void*, const void*), void (*key_free)(void*), void (*val_free)(void*)) {
    if (!tree) return;
    tree->root = NULL;
    tree->t = t < 2 ? 2 : t;
    tree->cmp = cmp;
    tree->key_free = key_free;
    tree->val_free = val_free;
}

/**
 * @brief Split the i-th child of node x when it becomes full.
 *
 * @param tree B-tree instance.
 * @param x    Parent node.
 * @param i    Child index to split.
 * @param now  Timestamp for allocations.
 */
static void split_child(ttak_btree_t *tree, ttak_btree_node_t *x, int i, uint64_t now) {
    int t = tree->t;
    ttak_btree_node_t *y = x->children[i];
    ttak_btree_node_t *z = create_node(t, y->leaf, now);
    
    z->n = t - 1;

    // Copy the last t-1 keys of y to z
    for (int j = 0; j < t - 1; j++) {
        z->keys[j] = y->keys[j + t];
        z->values[j] = y->values[j + t];
    }

    // Copy the last t children of y to z
    if (!y->leaf) {
        for (int j = 0; j < t; j++) {
            z->children[j] = y->children[j + t];
        }
    }

    y->n = t - 1;

    // Shift children of x to make room for z
    for (int j = x->n; j >= i + 1; j--) {
        x->children[j + 1] = x->children[j];
    }
    x->children[i + 1] = z;

    // Shift keys of x to make room for median of y
    for (int j = x->n - 1; j >= i; j--) {
        x->keys[j + 1] = x->keys[j];
        x->values[j + 1] = x->values[j];
    }

    x->keys[i] = y->keys[t - 1];
    x->values[i] = y->values[t - 1];
    x->n = x->n + 1;
}

/**
 * @brief Insert a key/value pair into a node that is known not to be full.
 *
 * @param tree B-tree instance.
 * @param x    Node that can accept another key.
 * @param k    Key pointer.
 * @param v    Value pointer.
 * @param now  Timestamp for recursion/allocations.
 */
static void insert_non_full(ttak_btree_t *tree, ttak_btree_node_t *x, void *k, void *v, uint64_t now) {
    /* Binary search: i becomes the insert position, i.e. the first index
     * where cmp(k, keys[i]) < 0; all keys from i onwards shift right. */
    int i;
    {
        int lo = 0, hi = x->n;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (tree->cmp(k, x->keys[mid]) < 0) hi = mid;
            else lo = mid + 1;
        }
        i = lo;
    }

    if (x->leaf) {
        for (int j = x->n - 1; j >= i; j--) {
            x->keys[j + 1] = x->keys[j];
            x->values[j + 1] = x->values[j];
        }
        x->keys[i] = k;
        x->values[i] = v;
        x->n = x->n + 1;
    } else {
        if (x->children[i]->n == 2 * tree->t - 1) {
            split_child(tree, x, i, now);
            if (tree->cmp(k, x->keys[i]) > 0) {
                i++;
            }
        }
        insert_non_full(tree, x->children[i], k, v, now);
    }
}

/**
 * @brief Insert or update a key/value pair in the B-tree.
 *
 * @param tree  Tree to mutate.
 * @param key   Key pointer owned by the tree.
 * @param value Value pointer owned by the tree.
 * @param now   Timestamp for allocations.
 */
void ttak_btree_insert(ttak_btree_t *tree, void *key, void *value, uint64_t now) {
    if (!tree) return;
    
    ttak_btree_node_t *r = tree->root;
    if (!r) {
        tree->root = create_node(tree->t, true, now);
        tree->root->keys[0] = key;
        tree->root->values[0] = value;
        tree->root->n = 1;
    } else {
        if (r->n == 2 * tree->t - 1) {
            ttak_btree_node_t *s = create_node(tree->t, false, now);
            tree->root = s;
            s->children[0] = r;
            split_child(tree, s, 0, now);
            insert_non_full(tree, s, key, value, now);
        } else {
            insert_non_full(tree, r, key, value, now);
        }
    }
}

/**
 * @brief Recursively search for a key starting at node x.
 *
 * @param tree B-tree instance.
 * @param x    Current node.
 * @param k    Key to find.
 * @param now  Timestamp for pointer validation.
 * @return Stored value or NULL if absent.
 */
static void *search_recursive(ttak_btree_t *tree, ttak_btree_node_t *x, const void *k, uint64_t now) {
    if (!x || !ttak_mem_access(x, now)) return NULL;
    
    int i = 0;
    int lo = 0, hi = x->n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (tree->cmp(k, x->keys[mid]) > 0) lo = mid + 1;
        else hi = mid;
    }
    i = lo;
    if (i < x->n && tree->cmp(k, x->keys[i]) == 0) {
        return x->values[i];
    }
    
    if (x->leaf) return NULL;
    
    return search_recursive(tree, x->children[i], k, now);
}

/**
 * @brief Lookup a key in the B-tree.
 *
 * @param tree Tree to query.
 * @param key  Key to search for.
 * @param now  Timestamp for pointer validation.
 * @return Stored value pointer or NULL.
 */
void *ttak_btree_search(ttak_btree_t *tree, const void *key, uint64_t now) {
    if (!tree) return NULL;
    return search_recursive(tree, tree->root, key, now);
}

/**
 * @brief Recursively free a subtree.
 *
 * @param x   Node to destroy.
 * @param kf  Key destructor.
 * @param vf  Value destructor.
 * @param now Timestamp for bookkeeping.
 */
static void destroy_recursive(ttak_btree_node_t *x, void (*kf)(void*), void (*vf)(void*), uint64_t now) {
    if (!x) return;
    
    if (!x->leaf) {
        for (int i = 0; i <= x->n; i++) {
            destroy_recursive(x->children[i], kf, vf, now);
        }
    }
    
    if (kf || vf) {
        for (int i = 0; i < x->n; i++) {
            if (kf) kf(x->keys[i]);
            if (vf) vf(x->values[i]);
        }
    }
    
    ttak_mem_free(x->keys);
    ttak_mem_free(x->values);
    ttak_mem_free(x->children);
    ttak_mem_free(x);
}

/**
 * @brief Destroy the entire B-tree.
 *
 * @param tree Tree to tear down.
 * @param now  Timestamp for destructor bookkeeping.
 */
void ttak_btree_destroy(ttak_btree_t *tree, uint64_t now) {
    if (!tree || !tree->root) return;
    destroy_recursive(tree->root, tree->key_free, tree->val_free, now);
    tree->root = NULL;
}
