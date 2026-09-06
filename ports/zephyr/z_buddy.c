/*
 * Copyright (c) 2024 LibTTAK Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/z_buddy.h>
#include <zephyr/sys/util.h>
#include <string.h>

static inline size_t order_size(uint8_t order)
{
	return (size_t)1 << order;
}

static uint8_t order_for_size(size_t bytes)
{
	if (bytes <= (1ULL << Z_BUDDY_MIN_ORDER)) {
		return Z_BUDDY_MIN_ORDER;
	}
#if defined(__LP64__) || defined(_WIN64)
	return (uint8_t)(64U - __builtin_clzll(bytes - 1ULL));
#else
	return (uint8_t)(32U - __builtin_clz(bytes - 1U));
#endif
}

static inline void *block_to_ptr(z_buddy_block_t *block)
{
	return (void *)(block + 1);
}

static inline z_buddy_block_t *ptr_to_block(void *ptr)
{
	return ((z_buddy_block_t *)ptr) - 1;
}

static z_buddy_block_t *buddy_pair(struct z_buddy_heap *h, z_buddy_block_t *block, uint8_t order)
{
	uintptr_t offset = (uintptr_t)block - (uintptr_t)h->pool_start;
	uintptr_t pair_offset = offset ^ order_size(order);

	if (pair_offset >= h->pool_len) {
		return NULL;
	}
	return (z_buddy_block_t *)((uintptr_t)h->pool_start + pair_offset);
}

static void mark_order_nonempty(struct z_buddy_heap *h, uint8_t order)
{
	h->free_mask |= (1U << order);
}

static void mark_order_empty(struct z_buddy_heap *h, uint8_t order)
{
	h->free_mask &= ~(1U << order);
}

static void list_push(struct z_buddy_heap *h, uint8_t order, z_buddy_block_t *block)
{
	block->next = h->free_lists[order];
	h->free_lists[order] = block;
	block->order = order;
	block->in_use = 0;
	mark_order_nonempty(h, order);
}

static z_buddy_block_t *list_pop_head(struct z_buddy_heap *h, uint8_t order)
{
	z_buddy_block_t *block = h->free_lists[order];

	if (block) {
		h->free_lists[order] = block->next;
		if (!h->free_lists[order]) {
			mark_order_empty(h, order);
		}
	}
	return block;
}

static void list_remove(struct z_buddy_heap *h, uint8_t order, z_buddy_block_t *target)
{
	z_buddy_block_t **cur = &h->free_lists[order];

	while (*cur) {
		if (*cur == target) {
			*cur = target->next;
			if (!h->free_lists[order]) {
				mark_order_empty(h, order);
			}
			return;
		}
		cur = &((*cur)->next);
	}
}

void z_buddy_init(struct z_buddy_heap *h, void *mem, size_t bytes)
{
	memset(h, 0, sizeof(*h));
	h->pool_start = mem;
	h->pool_len = bytes;

	/* Initialize by adding the largest possible power-of-two blocks */
	uint8_t order = Z_BUDDY_MAX_ORDER;
	uintptr_t ptr = (uintptr_t)mem;
	size_t remaining = bytes;

	while (remaining >= (1U << Z_BUDDY_MIN_ORDER)) {
		size_t sz = order_size(order);
		if (sz <= remaining && (ptr % sz == 0)) {
			list_push(h, order, (z_buddy_block_t *)ptr);
			ptr += sz;
			remaining -= sz;
			if (order > h->max_order) {
				h->max_order = order;
			}
		} else {
			order--;
		}
	}
}

/* Bitmask-based block selection for the buddy system. */
static z_buddy_block_t *select_block(struct z_buddy_heap *h, uint8_t desired)
{
	if (desired > h->max_order) {
		return NULL;
	}

	uint32_t mask = h->free_mask & (~0U << desired);
	if (!mask) {
		return NULL;
	}

	/* BEST_FIT via CTZ */
	uint8_t order = (uint8_t)__builtin_ctz(mask);
	return list_pop_head(h, order);
}

static void split_block(struct z_buddy_heap *h, uint8_t target_order, z_buddy_block_t *block)
{
	while (block->order > target_order) {
		uint8_t new_order = block->order - 1;
		size_t size = order_size(new_order);
		z_buddy_block_t *buddy = (z_buddy_block_t *)((uintptr_t)block + size);
		
		list_push(h, new_order, buddy);
		block->order = new_order;
	}
	block->in_use = 1;
}

void *z_buddy_alloc(struct z_buddy_heap *h, size_t bytes)
{
	if (bytes == 0) {
		return NULL;
	}

	size_t needed = bytes + sizeof(z_buddy_block_t);
	uint8_t order = order_for_size(needed);

	k_spinlock_key_t key = k_spin_lock(&h->lock);

	z_buddy_block_t *block = select_block(h, order);
	if (!block) {
		k_spin_unlock(&h->lock, key);
		return NULL;
	}

	split_block(h, order, block);

	k_spin_unlock(&h->lock, key);
	return block_to_ptr(block);
}

void z_buddy_free(struct z_buddy_heap *h, void *mem)
{
	if (!mem) {
		return;
	}

	z_buddy_block_t *block = ptr_to_block(mem);
	
	k_spinlock_key_t key = k_spin_lock(&h->lock);

	while (block->order < h->max_order) {
		uint8_t order = block->order;
		z_buddy_block_t *pair = buddy_pair(h, block, order);

		if (!pair || pair->in_use || pair->order != order) {
			break;
		}

		list_remove(h, order, pair);
		block = (block < pair) ? block : pair;
		block->order = order + 1;
	}

	list_push(h, block->order, block);

	k_spin_unlock(&h->lock, key);
}
