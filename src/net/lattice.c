/**
 * @file lattice.c
 * @brief Lock-free parallel ingress buffer using a 2-D slot lattice.
 *
 * Implements a 2-D slot lattice for zero-copy parallel packet ingress.
 * Contains architecture-specific inline assembly (x86-64, AArch64, RISC-V, PPC64)
 * for the TinyCC fast-copy path.
 * @warning Do not modify inline assembly blocks without testing all arches.
 */

#include <ttak/net/lattice.h>
#include <ttak/mem/mem.h>
#include <ttak/mols_control.h>
#include <ttak/atomic/atomic.h>
#include <ttak/timing/timing.h>
#include <ttak/types/ttak_compiler.h>
#include <string.h>
#include <stdlib.h>
#include <ttak/compat/pthread.h>
#include <ttak/compat/stdatomic.h>

#if defined(__TINYC__) && !TTAK_TINYCC_NEEDS_PORTABLE_FALLBACK
static inline void ttak_net_lattice_copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t len) {
    if (!dst || !src || len == 0) {
        return;
    }
#if defined(__x86_64__) || defined(_M_X64)
    size_t cnt = len;
    __asm__ __volatile__(
        "rep movsb"
        : "+D"(dst), "+S"(src), "+c"(cnt)
        :
        : "memory");
#elif defined(__aarch64__)
    register uint8_t *d = dst;
    register const uint8_t *s = src;
    register uint64_t n = len;
    uint64_t tmp;
    __asm__ __volatile__(
        "cbz %w2, 2f\n"
        "1:\n"
        "ldrb %w3, [%1], #1\n"
        "strb %w3, [%0], #1\n"
        "subs %w2, %w2, #1\n"
        "b.ne 1b\n"
        "2:\n"
        : "+r"(d), "+r"(s), "+r"(n), "=&r"(tmp)
        :
        : "memory");
#elif defined(__riscv) && (__riscv_xlen == 64)
    register uint8_t *d = dst;
    register const uint8_t *s = src;
    register uint64_t n = len;
    unsigned long tmp;
    __asm__ __volatile__(
        "beqz %2, 2f\n"
        "1:\n"
        "lb %3, 0(%1)\n"
        "addi %1, %1, 1\n"
        "sb %3, 0(%0)\n"
        "addi %0, %0, 1\n"
        "addi %2, %2, -1\n"
        "bnez %2, 1b\n"
        "2:\n"
        : "+r"(d), "+r"(s), "+r"(n), "=&r"(tmp)
        :
        : "memory");
#elif defined(__powerpc64__) || defined(__ppc64__)
    register uint8_t *d = dst;
    register const uint8_t *s = src;
    register uint64_t n = len;
    unsigned long tmp;
    __asm__ __volatile__(
        "cmpdi %2, 0\n"
        "beq 2f\n"
        "1:\n"
        "lbz %3, 0(%1)\n"
        "addi %1, %1, 1\n"
        "stb %3, 0(%0)\n"
        "addi %0, %0, 1\n"
        "addi %2, %2, -1\n"
        "cmpdi %2, 0\n"
        "bne 1b\n"
        "2:\n"
        : "+r"(d), "+r"(s), "+r"(n), "=&r"(tmp)
        :
        : "memory");
#elif defined(__mips64) || defined(__mips64__) || (defined(__mips) && (__mips == 64))
    register uint8_t *d = dst;
    register const uint8_t *s = src;
    register uint64_t n = len;
    unsigned long tmp;
    __asm__ __volatile__(
        "beqz %2, 2f\n"
        "1:\n"
        "lbu %3, 0(%1)\n"
        "daddi %1, %1, 1\n"
        "sb %3, 0(%0)\n"
        "daddi %0, %0, 1\n"
        "daddi %2, %2, -1\n"
        "bnez %2, 1b\n"
        "2:\n"
        : "+r"(d), "+r"(s), "+r"(n), "=&r"(tmp)
        :
        : "memory");
#else
    for (uint32_t i = 0; i < len; ++i) {
        dst[i] = src[i];
    }
#endif
}
#elif defined(__TINYC__) && TTAK_TINYCC_NEEDS_PORTABLE_FALLBACK
static inline void ttak_net_lattice_copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t len) {
    if (!dst || !src || len == 0) {
        return;
    }
    memcpy(dst, src, len);
}
#else
static inline void ttak_net_lattice_copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t len) {
    if (!dst || !src || len == 0) {
        return;
    }
    memcpy(dst, src, len);
}
#endif

#if TTAK_NO_NATIVE_TLS
static uint32_t tls_worker_id = 0;
#else
static _Thread_local uint32_t tls_worker_id = 0;
#endif

#define TTAK_LATTICE_COMPACT_MIN_FREE_PCT 80U
#define TTAK_LATTICE_COMPACT_THROTTLE_MASK 0xFFU
#define TTAK_LATTICE_COMPACT_TOKEN 0x1U
#define TTAK_LATTICE_NODE_IS_BUSY(node) \
    (atomic_load_explicit(&(node)->compact_state, memory_order_acquire) != 0U)

static _Atomic uint32_t g_lattice_compact_counter = 0;

static ttak_net_lattice_t *global_default_lattice = NULL;
static pthread_once_t lattice_once = PTHREAD_ONCE_INIT;

static void lattice_init_default(void) {
    /* Auto-detect CPU count or use a safe default (4) for lattice dimension */
    uint32_t dim = 4;
    global_default_lattice = ttak_net_lattice_create(dim, ttak_get_tick_count());
}

static ttak_net_lattice_t *ttak_net_lattice_head(ttak_net_lattice_t *lat) {
    while (lat && lat->prev) {
        lat = lat->prev;
    }
    return lat;
}

static inline _Bool ttak_net_lattice_is_real(const ttak_net_lattice_t *lat) {
    return lat && !lat->is_stub && lat->dim != 0 && lat->slots;
}

static inline _Bool ttak_net_lattice_claim_compaction(ttak_net_lattice_t *lat) {
    if (!lat) {
        return false;
    }
    uint32_t expected = 0U;
    return atomic_compare_exchange_strong_explicit(
        &lat->compact_state,
        &expected,
        TTAK_LATTICE_COMPACT_TOKEN,
        memory_order_acq_rel,
        memory_order_acquire
    );
}

static inline void ttak_net_lattice_release_compaction(ttak_net_lattice_t *lat) {
    if (lat) {
        atomic_store_explicit(&lat->compact_state, 0U, memory_order_release);
    }
}

static _Bool ttak_net_lattice_slots_idle(const ttak_net_lattice_t *lat) {
    if (!ttak_net_lattice_is_real(lat)) {
        return false;
    }
    size_t slots_count = (size_t)lat->dim * (size_t)lat->dim;
    for (size_t i = 0; i < slots_count; ++i) {
        if (ttak_atomic_read64(&lat->slots[i].state) != 0) {
            return false;
        }
    }
    return true;
}

static void ttak_net_lattice_mark_stub(ttak_net_lattice_t *lat) {
    if (!lat || lat->is_stub) {
        return;
    }
    if (lat->slots) {
        ttak_mem_free(lat->slots);
    }
    lat->slots = NULL;
    lat->dim = 0;
    lat->mask = 0;
    lat->capacity = 0;
    lat->is_stub = true;
    ttak_atomic_write64(&lat->used_slots, 0);
    ttak_atomic_write64(&lat->total_ingress, 0);
    lat->is_full = false;
    atomic_store_explicit(&lat->compact_state, 0U, memory_order_release);
}

static _Bool ttak_net_lattice_rehydrate(ttak_net_lattice_t *lat, uint32_t dim, uint64_t now) {
    if (!lat || dim == 0) {
        return false;
    }

    size_t slots_count = (size_t)dim * (size_t)dim;
    ttak_net_lattice_slot_t *slots = ttak_mem_alloc_raw(slots_count * sizeof(ttak_net_lattice_slot_t),
                                                    __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!slots) {
        return false;
    }
    memset(slots, 0, slots_count * sizeof(ttak_net_lattice_slot_t));

    lat->slots = slots;
    lat->dim = dim;
    lat->mask = dim - 1U;
    lat->capacity = (uint32_t)slots_count;
    lat->is_stub = false;
    ttak_atomic_write64(&lat->used_slots, 0);
    ttak_atomic_write64(&lat->total_ingress, 0);
    lat->is_full = false;
    atomic_store_explicit(&lat->compact_state, 0U, memory_order_release);
    return true;
}

static void ttak_net_lattice_try_compact(ttak_net_lattice_t *lat) {
    if (!lat) {
        return;
    }

    uint32_t ticket = atomic_fetch_add_explicit(&g_lattice_compact_counter, 1U, memory_order_relaxed);
    if ((ticket & TTAK_LATTICE_COMPACT_THROTTLE_MASK) != 0U) {
        return;
    }

    ttak_net_lattice_t *head = ttak_net_lattice_head(lat);
    uint64_t total_capacity = 0;
    uint64_t total_used = 0;
    size_t real_nodes = 0;
    ttak_net_lattice_t *prev_empty = NULL;
    ttak_net_lattice_t *first = NULL;
    ttak_net_lattice_t *second = NULL;

    for (ttak_net_lattice_t *node = head; node; node = node->next) {
        if (!ttak_net_lattice_is_real(node)) {
            continue;
        }

        real_nodes++;
        total_capacity += node->capacity;
        uint64_t used = ttak_atomic_read64(&node->used_slots);
        total_used += used;

        if (used == 0) {
            if (prev_empty) {
                first = prev_empty;
                second = node;
            }
            prev_empty = node;
        } else {
            prev_empty = NULL;
        }
    }

    if (real_nodes < 2 || total_capacity == 0) {
        return;
    }

    uint64_t free_pct = ((total_capacity - total_used) * 100ULL) / total_capacity;
    if (free_pct < TTAK_LATTICE_COMPACT_MIN_FREE_PCT || !first || !second) {
        return;
    }

    _Bool claimed_first = ttak_net_lattice_claim_compaction(first);
    if (!claimed_first) {
        return;
    }

    _Bool claimed_second = ttak_net_lattice_claim_compaction(second);
    if (!claimed_second) {
        ttak_net_lattice_release_compaction(first);
        return;
    }

    do {
        if (!ttak_net_lattice_is_real(first) || !ttak_net_lattice_is_real(second) ||
            ttak_atomic_read64(&first->used_slots) != 0 ||
            ttak_atomic_read64(&second->used_slots) != 0 ||
            first->dim != second->dim) {
            break;
        }

        if (!ttak_net_lattice_slots_idle(first) || !ttak_net_lattice_slots_idle(second)) {
            break;
        }

        size_t slots_bytes = (size_t)first->capacity * sizeof(ttak_net_lattice_slot_t);
        memset(first->slots, 0, slots_bytes);
        ttak_net_lattice_mark_stub(second);
    } while (0);

    ttak_net_lattice_release_compaction(second);
    ttak_net_lattice_release_compaction(first);
}

ttak_net_lattice_t* ttak_net_lattice_get_default(void) {
    pthread_once(&lattice_once, lattice_init_default);
    return global_default_lattice;
}

void ttak_net_lattice_set_worker_id(uint32_t tid) {
    tls_worker_id = tid;
}

uint32_t ttak_net_lattice_get_worker_id(void) {
    return tls_worker_id;
}

ttak_net_lattice_t* ttak_net_lattice_create(uint32_t dim, uint64_t now) {
    /* Ensure dim is power of 2 for masking */
    if (dim == 0 || (dim & (dim - 1)) != 0 || dim > TTAK_LATTICE_MAX_DIM) return NULL;

    ttak_net_lattice_t *lat = ttak_mem_alloc_raw(sizeof(ttak_net_lattice_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!lat) return NULL;

    size_t slots_count = (size_t)dim * (size_t)dim;
    lat->slots = ttak_mem_alloc_raw(slots_count * sizeof(ttak_net_lattice_slot_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!lat->slots) {
        ttak_mem_free(lat);
        return NULL;
    }

    lat->dim = dim;
    lat->mask = dim - 1;
    lat->capacity = (uint32_t)slots_count;
    lat->total_ingress = 0;
    lat->used_slots = 0;
    atomic_store_explicit(&lat->compact_state, 0U, memory_order_relaxed);
    lat->is_full = false;
    lat->is_stub = false;
    lat->prev = NULL;
    lat->next = NULL;
    if (ttak_mutex_init(&lat->expand_lock) != 0) {
        ttak_mem_free(lat->slots);
        ttak_mem_free(lat);
        return NULL;
    }

    memset(lat->slots, 0, slots_count * sizeof(ttak_net_lattice_slot_t));
    (void)now;
    return lat;
}

void ttak_net_lattice_destroy(ttak_net_lattice_t *lat, uint64_t now) {
    (void)now;
    while (lat) {
        ttak_net_lattice_t *next = lat->next;
        if (lat->slots) {
            ttak_mem_free(lat->slots);
        }
        ttak_mutex_destroy(&lat->expand_lock);
        ttak_mem_free(lat);
        lat = next;
    }
}

/**
 * @brief Lock-free deterministic write into the lattice.
 */
_Bool ttak_net_lattice_write(ttak_net_lattice_t *lat, uint32_t tid, const void *data, uint32_t len, uint64_t now) {
    if (!lat || !data || len > TTAK_LATTICE_SLOT_SIZE) return false;

    ttak_net_lattice_t *head = ttak_net_lattice_head(lat);
    ttak_net_lattice_t *mask_node = head;
    while (mask_node && !ttak_net_lattice_is_real(mask_node)) {
        mask_node = mask_node->next;
    }
    if (!mask_node) {
        return false;
    }

    uint32_t mask = mask_node->mask;
    uint32_t my_tid = tid & mask;
    
    for (ttak_net_lattice_t *node = head; node; ) {
        if (!ttak_net_lattice_is_real(node) || TTAK_LATTICE_NODE_IS_BUSY(node)) {
            node = node->next;
            continue;
        }

        uint32_t dim = node->dim;
        /* Sweep every lattice coordinate in deterministic order */
        for (uint32_t r = 0; r < dim; r++) {
            for (uint32_t c = 0; c < dim; c++) {
                const uint16_t node_id =
                    (uint16_t)(((r & (uint32_t)TTAK_MOLS_SYMBOL_MASK) << TTAK_MOLS_COORD_SHIFT) |
                               (c & (uint32_t)TTAK_MOLS_SYMBOL_MASK));
                uint32_t lane = ttak_apply_mols_control(node_id, my_tid) & mask;
                /* Enforce orthogonality so each worker selects disjoint slots. */
                if (((r + c) & mask) == lane) {
                    ttak_net_lattice_slot_t *slot = &node->slots[r * dim + c];
                    
                    if (ttak_atomic_read64(&slot->state) == 0) {
                        ttak_atomic_write64(&slot->state, 1); /* Entering writing state */
                        
                        ttak_net_lattice_copy_bytes(slot->data, (const uint8_t *)data, len);
                        slot->len = len;
                        slot->timestamp = now;
                        slot->seq++;
                        
                        ttak_atomic_write64(&slot->state, 2); /* Ready state */
                        ttak_atomic_add64(&node->total_ingress, 1);
                        ttak_net_lattice_mark_slot_acquired(node, now);
                        return true;
                    }
                }
            }
        }

        ttak_net_lattice_t *next = node->next;
        if (!next) {
            next = ttak_net_lattice_ensure_next(node, now);
        }
        node = next;
    }
    
    return false;
}

_Bool ttak_net_lattice_read(ttak_net_lattice_t *lat, uint32_t tid, void *dst, uint32_t *len_out, uint64_t now) {
    if (!lat || !dst || !len_out) return false;
    (void)now;
    
    ttak_net_lattice_t *head = ttak_net_lattice_head(lat);
    ttak_net_lattice_t *mask_node = head;
    while (mask_node && !ttak_net_lattice_is_real(mask_node)) {
        mask_node = mask_node->next;
    }
    if (!mask_node) {
        return false;
    }

    uint32_t mask = mask_node->mask;
    uint32_t my_tid = tid & mask;
    
    for (ttak_net_lattice_t *node = head; node; node = node->next) {
        if (!ttak_net_lattice_is_real(node) || TTAK_LATTICE_NODE_IS_BUSY(node)) {
            continue;
        }

        uint32_t dim = node->dim;
        /* High-dimensional coordinate sweep to locate an available slot */
        for (uint32_t r = 0; r < dim; r++) {
            for (uint32_t c = 0; c < dim; c++) {
                const uint16_t node_id =
                    (uint16_t)(((r & (uint32_t)TTAK_MOLS_SYMBOL_MASK) << TTAK_MOLS_COORD_SHIFT) |
                               (c & (uint32_t)TTAK_MOLS_SYMBOL_MASK));
                uint32_t lane = ttak_apply_mols_control(node_id, my_tid) & mask;
                if (((r + c) & mask) == lane) {
                    ttak_net_lattice_slot_t *slot = &node->slots[r * dim + c];
                    
                    if (ttak_atomic_read64(&slot->state) == 2) {
                        ttak_atomic_write64(&slot->state, 3); /* Mark as reading/processing */
                        
                        uint32_t len = slot->len;
                        ttak_net_lattice_copy_bytes((uint8_t *)dst, slot->data, len);
                        *len_out = len;
                        
                        ttak_atomic_write64(&slot->state, 0); /* Reset to empty */
                        ttak_net_lattice_mark_slot_released(node);
                        return true;
                    }
                }
            }
        }
    }
    
    return false;
}

ttak_net_lattice_t* ttak_net_lattice_ensure_next(ttak_net_lattice_t *lat, uint64_t now) {
    if (!lat) return NULL;
    if (lat->next && !lat->next->is_stub) return lat->next;

    ttak_mutex_lock(&lat->expand_lock);
    ttak_net_lattice_t *next = lat->next;
    if (!next) {
        next = ttak_net_lattice_create(lat->dim, now);
        if (next) {
            next->prev = lat;
            lat->next = next;
        }
    } else if (next->is_stub) {
        if (!ttak_net_lattice_rehydrate(next, lat->dim, now)) {
            next = NULL;
        }
    }
    ttak_mutex_unlock(&lat->expand_lock);
    return next;
}

void ttak_net_lattice_mark_slot_acquired(ttak_net_lattice_t *lat, uint64_t now) {
    if (!lat || lat->capacity == 0) return;
    uint64_t used = ttak_atomic_add64(&lat->used_slots, 1);
    if (used >= lat->capacity) {
        lat->is_full = true;
    }
    uint64_t scaled = used * 100;
    uint64_t threshold = (uint64_t)lat->capacity * 80;
    if (scaled >= threshold) {
        ttak_net_lattice_ensure_next(lat, now);
    }
}

void ttak_net_lattice_mark_slot_released(ttak_net_lattice_t *lat) {
    if (!lat || lat->capacity == 0) return;
    uint64_t remaining = ttak_atomic_sub64(&lat->used_slots, 1);
    if (remaining < lat->capacity) {
        lat->is_full = false;
    }
    if (remaining == 0) {
        ttak_net_lattice_try_compact(lat);
    }
}
