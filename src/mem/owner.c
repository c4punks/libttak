#include <ttak/mem/owner.h>
#include <ttak/ht/hash.h>
#include <ttak/mem/mem.h>
#include <ttak/timing/timing.h>
#include <ttak/compat/stdatomic.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static _Atomic uint32_t g_owner_id_counter = 1;

/**
 * @brief Hashing helper for string keys.
 */
static inline uintptr_t _hash_str(const char *str) {
    // Simple wrapper assuming string keys are cast to uintptr_t or hashed
    // For this implementation, we use a basic DJB2-like hash or cast if the map supports it.
    // Assuming ttak_map handles uintptr_t keys. We hash the string to uintptr_t.
    uintptr_t hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

ttak_owner_t *ttak_owner_create(uint32_t policy) {
    uint64_t now = ttak_get_tick_count();
    ttak_owner_t *owner = ttak_mem_alloc_raw(sizeof(ttak_owner_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!owner) return NULL;

    owner->id = atomic_fetch_add(&g_owner_id_counter, 1);
    
    // Initialize resource and function maps
    owner->resources = ttak_create_map(32, now);
    owner->functions = ttak_create_map(32, now);
    
    ttak_rwlock_init(&owner->lock);
    owner->creation_ts = now;
    owner->policy_flags = policy;

    return owner;
}

void ttak_owner_destroy(ttak_owner_t *owner) {
    if (!owner) return;

    ttak_rwlock_wrlock(&owner->lock);
    
    // Note: We do not deep-free resources here as we don't know their destructors.
    // In a full system, we might need a resource wrapper with a dtor.
    // ttak_map_destroy(owner->resources); // Assuming generic map destroy exists or we just leak for this snippet
    // ttak_map_destroy(owner->functions);
    
    ttak_rwlock_unlock(&owner->lock);
    ttak_rwlock_destroy(&owner->lock);
    ttak_mem_free(owner);
}

bool ttak_owner_register_func(ttak_owner_t *owner, const char *name, ttak_owner_func_t func) {
    if (!owner || !name || !func) return false;

    ttak_rwlock_wrlock(&owner->lock);
    uintptr_t key = _hash_str(name);
    
    // Check if strict policy prevents overwriting (simplified: just insert)
    ttak_insert_to_map(owner->functions, key, (size_t)func, ttak_get_tick_count());
    
    ttak_rwlock_unlock(&owner->lock);
    return true;
}

bool ttak_owner_register_resource(ttak_owner_t *owner, const char *name, void *data) {
    if (!owner || !name) return false;

    ttak_rwlock_wrlock(&owner->lock);
    uintptr_t key = _hash_str(name);
    uint64_t now = ttak_get_tick_count();

    /* Each registration also stores a reverse entry keyed by the pointer
     * whose value is the resource key, so ttak_mem_unuse can find and remove
     * the binding with a direct keyed lookup instead of scanning the map. */
    size_t old_val = 0;
    if (ttak_map_get_key(owner->resources, key, &old_val, now) && (void *)old_val != data)
        ttak_delete_from_map(owner->resources, (uintptr_t)old_val, now);
    size_t old_key = 0;
    if (ttak_map_get_key(owner->resources, (uintptr_t)data, &old_key, now))
        ttak_delete_from_map(owner->resources, (uintptr_t)old_key, now);

    ttak_insert_to_map(owner->resources, key, (size_t)data, now);
    ttak_insert_to_map(owner->resources, (uintptr_t)data, (size_t)key, now);
    
    if (ttak_mem_is_trace_enabled()) {
        fprintf(stderr, "[MEM_TRACK] {\"event\":\"register\",\"ptr\":\"%p\",\"owner\":\"%p\",\"name\":\"%s\",\"ts\":%" PRIu64 "}\n", 
                data, (void*)owner, name, ttak_get_tick_count());
    }

    ttak_rwlock_unlock(&owner->lock);
    return true;
}

bool ttak_owner_transfer_resource(ttak_owner_t *from, ttak_owner_t *to, const char *name) {
    if (!from || !to || !name) return false;

    uintptr_t key = _hash_str(name);
    size_t data_val = 0;

    ttak_rwlock_wrlock(&from->lock);
    if (!ttak_map_get_key(from->resources, key, &data_val, ttak_get_tick_count())) {
        ttak_rwlock_unlock(&from->lock);
        return false;
    }
    ttak_delete_from_map(from->resources, key, 0);
    /* Move the reverse (pointer-keyed) entry along with the resource. */
    ttak_delete_from_map(from->resources, (uintptr_t)data_val, 0);
    ttak_rwlock_unlock(&from->lock);

    ttak_rwlock_wrlock(&to->lock);
    size_t stale_key = 0;
    if (ttak_map_get_key(to->resources, (uintptr_t)data_val, &stale_key, ttak_get_tick_count()))
        ttak_delete_from_map(to->resources, (uintptr_t)stale_key, ttak_get_tick_count());
    ttak_insert_to_map(to->resources, key, data_val, ttak_get_tick_count());
    ttak_insert_to_map(to->resources, (uintptr_t)data_val, (size_t)key, ttak_get_tick_count());
    
    if (ttak_mem_is_trace_enabled()) {
        fprintf(stderr, "[MEM_TRACK] {\"event\":\"transfer\",\"ptr\":\"%p\",\"from\":\"%p\",\"to\":\"%p\",\"name\":\"%s\",\"ts\":%" PRIu64 "}\n", 
                (void*)data_val, (void*)from, (void*)to, name, ttak_get_tick_count());
    }

    ttak_rwlock_unlock(&to->lock);
    return true;
}

bool ttak_owner_execute(ttak_owner_t *owner, const char *func_name, const char *resource_name, void *args) {
    if (!owner || !func_name) return false;

    ttak_rwlock_rdlock(&owner->lock);

    // 1. Safety Check: Verify policy
    
    uintptr_t func_key = _hash_str(func_name);
    if(func_key == (uintptr_t) NULL) return false;
    size_t func_ptr_val = 0;
    
    // 2. Retrieve Function
    if (!ttak_map_get_key(owner->functions, func_key, &func_ptr_val, ttak_get_tick_count())) {
        ttak_rwlock_unlock(&owner->lock);
        return false; // Function not found
    }

    // 3. Retrieve Context Resource (if specified)
    void *ctx = NULL;
    if (resource_name) {
        uintptr_t res_key = _hash_str(resource_name);
        size_t res_ptr_val = 0;
        if (ttak_map_get_key(owner->resources, res_key, &res_ptr_val, ttak_get_tick_count())) {
            ctx = (void *)res_ptr_val;
        }
    }

    ttak_owner_func_t func = (ttak_owner_func_t)func_ptr_val;

    ttak_rwlock_unlock(&owner->lock);

    // 4. Execution Boundary
    // Ideally, we would set thread-local storage here to indicate we are inside this owner
    // so that mem_alloc calls check the owner's policy.

    func(ctx, args);

    return true;
}
