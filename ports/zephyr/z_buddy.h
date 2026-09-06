/*
 * Copyright (c) 2024 LibTTAK Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_SYS_Z_BUDDY_H_
#define ZEPHYR_INCLUDE_SYS_Z_BUDDY_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
/* struct k_spinlock is used below (embedded as struct z_buddy_heap::lock);
 * pulling it in only transitively via <zephyr/kernel.h> is not reliable,
 * because this header is itself reachable from inside kernel.h's own
 * include chain (kernel.h -> kernel_includes.h -> kernel_structs.h ->
 * sys_heap.h -> here). On that path kernel.h's include guard is already
 * active by the time we get here, so the transitive re-include below is a
 * no-op and struct k_spinlock has not been defined yet -- "incomplete
 * type" at the point of use. Including spinlock.h directly avoids
 * depending on include order. */
#include <zephyr/spinlock.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

#define Z_BUDDY_MIN_ORDER 6U   /* 64 bytes */
#define Z_BUDDY_MAX_ORDER 31U  /* 2 GB for 32-bit, or more for 64-bit */

typedef struct z_buddy_block {
	struct z_buddy_block *next;
	uint8_t order;
	uint8_t in_use;
} z_buddy_block_t;

struct z_buddy_heap {
	z_buddy_block_t *free_lists[Z_BUDDY_MAX_ORDER + 1];
	uint32_t free_mask; /* Bitmask of available orders */
	void *pool_start;
	size_t pool_len;
	uint8_t max_order;
	struct k_spinlock lock;
};

void z_buddy_init(struct z_buddy_heap *h, void *mem, size_t bytes);
void *z_buddy_alloc(struct z_buddy_heap *h, size_t bytes);
void z_buddy_free(struct z_buddy_heap *h, void *mem);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_SYS_Z_BUDDY_H_ */
