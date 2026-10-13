// SPDX-License-Identifier: BSD-3-Clause
/*
 * debug.c -- introspection and the consistency checker.
 *
 * None of this is on any hot path.  osmem_stats() summarises the allocator
 * without taking a lock; osmem_check() walks every block of every arena and
 * verifies the allocator's structural invariants, which is what lets
 * the test suite assert that the allocator is *correct* rather than merely
 * that it returned a non-NULL pointer.
 */

#define _GNU_SOURCE

#include <limits.h>
#include <stdio.h>

#include "osmem_internal.h"

void osmem_stats(struct osmem_stats *out)
{
	int n, i;

	memset(out, 0, sizeof(*out));
	osmem_boot();

	n = arena_count();
	out->arena_count = (size_t)n;
	out->arena_max = (size_t)g_arena_max;
	out->arena_size = g_arena_size;

	for (i = 0; i < n; i++) {
		struct arena *a = arena_at(i);
		int state;
		int j;

		if (!a)
			continue;
		state = atomic_load_explicit(&a->state, memory_order_acquire);
		if (state == ARENA_EXCLUSIVE)
			out->arena_exclusive++;
		else if (state == ARENA_ORPHAN)
			out->arena_orphan++;
		else if (state == ARENA_SHARED)
			out->arena_shared++;
		else
			out->arena_poisoned++;

		if (state == ARENA_POISONED)
			continue;	/* knowingly inconsistent, do not read it */

		out->bytes_used += cnt_get(&a->bytes_used);
		out->bytes_free += cnt_get(&a->bytes_free);
		out->blocks_used += cnt_get(&a->blocks_used);
		out->blocks_free += cnt_get(&a->blocks_free);
		out->bytes_bump += g_arena_payload - cnt_get(&a->bump_room);
		out->bytes_touched += cnt_get(&a->touched);
#ifdef OSMEM_QUARANTINE
		out->bytes_quarantined += cnt_get(&a->bytes_quarantined);
#endif
		j = bin_last(a);
		if (j >= 0 && bin_floor(j) > out->largest_free)
			out->largest_free = bin_floor(j);
	}

	out->bytes_mapped = cnt_get(&g_bytes_mapped);
	out->standalone_blocks = cnt_get(&g_standalone_blocks);
	out->standalone_bytes = cnt_get(&g_standalone_bytes);
	out->mmap_calls = cnt_get(&g_mmap_calls);
	out->munmap_calls = cnt_get(&g_munmap_calls);
	out->remote_frees = cnt_get(&g_remote_frees);
	out->ffa_allocs = cnt_get(&g_ffa_allocs);
	out->adoptions = cnt_get(&g_adoptions);
	out->reclaims = cnt_get(&g_reclaims);
	out->ffa_engaged = atomic_load_explicit(&g_ffa, memory_order_relaxed);
	out->map_denied = cnt_get(&g_map_denied);
	out->foreign_ptrs = cnt_get(&g_foreign_ptrs);
	out->bytes_released = cnt_get(&g_bytes_released);
#ifdef OSMEM_QUARANTINE
	out->has_quarantine = 1;
#endif
	out->has_world_lock = OSMEM_HAS_WORLD_LOCK;
	out->has_stats = OSMEM_HAS_STATS;
#ifdef OSMEM_CHAOS
	out->has_chaos = 1;
#else
	out->has_chaos = 0;
#endif
}

void osmem_fail_mmap_after(long n)
{
	osmem_boot();
	atomic_store_explicit(&g_map_budget, n < 0 ? LONG_MAX : n, memory_order_relaxed);
}

const void *osmem_thread_arena(void)
{
	return tl_arena;
}

int osmem_arena_list(const void **out, int max)
{
	int n, i, k = 0;

	osmem_boot();
	n = arena_count();
	for (i = 0; i < n && k < max; i++) {
		struct arena *a = arena_at(i);

		if (a)
			out[k++] = a;
	}
	return k;
}

unsigned long long osmem_thread_id(void)
{
	return tl_tid;
}

int osmem_arena_state(const void *arena)
{
	const struct arena *a = arena;

	return atomic_load_explicit(&a->state, memory_order_acquire);
}

unsigned long long osmem_arena_owner(const void *arena)
{
	const struct arena *a = arena;

	return atomic_load_explicit(&a->owner, memory_order_acquire);
}

void osmem_drain_all(void)
{
	unsigned long long me = tl_tid;
	int n, i;

	osmem_boot();
	WORLD_STOP();
	n = arena_count();
	for (i = 0; i < n; i++) {
		struct arena *a = arena_at(i);
		unsigned long long owner;

		if (!a || atomic_load_explicit(&a->state, memory_order_acquire) == ARENA_POISONED)
			continue;
		owner = atomic_load_explicit(&a->owner, memory_order_acquire);
		if (owner == me && me != 0) {
			remote_drain(a);
			arena_flush(a);
			continue;
		}
		pthread_mutex_lock(&a->lock);
		if (atomic_load_explicit(&a->owner, memory_order_relaxed) == 0) {
			remote_drain(a);
			arena_flush(a);
		}
		pthread_mutex_unlock(&a->lock);
	}
	WORLD_GO();
}

#define CHK(cond, ...)								\
	do {									\
		if (!(cond)) {							\
			errors++;						\
			if (errors <= 10) {					\
				fprintf(stderr, "osmem_check: arena %d: ", idx);	\
				fprintf(stderr, __VA_ARGS__);			\
				fprintf(stderr, "\n");				\
			}							\
		}								\
	} while (0)

static int check_arena(struct arena *a, int idx)
{
	size_t bytes_used = 0, bytes_free = 0, blocks_used = 0, blocks_free = 0;
	size_t binned_bytes = 0, binned_blocks = 0;
	struct block_meta *b, *prev = NULL;
	int errors = 0;
	char *p;
	int i;

	CHK(a->data_start <= a->bump && a->bump <= a->data_end, "bump out of range");
	CHK(a->hwm >= a->bump && a->hwm <= a->data_end, "high-water mark out of range");
	if (OSMEM_HAS_STATS)
		CHK(cnt_get(&a->touched) == (size_t)(a->hwm - a->data_start),
		    "stale footprint counter: %zu, actual %zu", cnt_get(&a->touched),
		    (size_t)(a->hwm - a->data_start));
	CHK(((uintptr_t)a->data_start & (g_page - 1)) == 0,
	    "the carvable region does not start on a page boundary");
	CHK(cnt_get(&a->bump_room) == (size_t)(a->data_end - a->bump), "stale bump_room");

	/* Walk the block chain. */
	for (p = a->data_start; p < a->bump; p += blk_size(b)) {
		unsigned int flags;
		size_t size;

		b = (struct block_meta *)p;
		size = blk_size(b);
		flags = blk_flags(b);

		CHK(size >= BLOCK_MIN_SIZE && size % BLOCK_ALIGN == 0,
		    "block %p bad size %zu", (void *)b, size);
		if (size < BLOCK_MIN_SIZE || size % BLOCK_ALIGN)
			return errors;	/* chain is unusable, stop here */
		CHK(p + size <= a->bump, "block %p overruns the frontier", (void *)b);
		CHK(b->arena == a, "block %p has a foreign arena pointer", (void *)b);
		CHK(blk_tagged(b), "block %p has no allocator tag", (void *)b);
		CHK(!(flags & BLOCK_STANDALONE), "block %p flagged standalone", (void *)b);

		if (flags & BLOCK_FREE) {
			CHK(!(flags & BLOCK_PREV_FREE),
			    "free block %p follows a free block", (void *)b);
			CHK(blk_prev_footer((struct block_meta *)(p + size)) == size,
			    "free block %p has a bad footer", (void *)b);
			bytes_free += size;
			blocks_free++;
		} else {
			bytes_used += size;
			blocks_used++;
		}
		if (prev) {
			int prev_free = (blk_flags(prev) & BLOCK_FREE) != 0;
			int claims_prev_free = (flags & BLOCK_PREV_FREE) != 0;

			CHK(prev_free == claims_prev_free,
			    "block %p PREV_FREE=%d but previous block is %s",
			    (void *)b, claims_prev_free, prev_free ? "free" : "in use");
		} else {
			CHK(!(flags & BLOCK_PREV_FREE),
			    "first block %p claims a free predecessor", (void *)b);
		}
		prev = b;
	}
	CHK(p == a->bump, "block chain ends at %p, frontier is %p", (void *)p, (void *)a->bump);
	CHK(!prev || !(blk_flags(prev) & BLOCK_FREE),
	    "the block below the frontier is free (should have been reclaimed)");

	/* Walk the bins. */
	for (i = 0; i < NBINS; i++) {
		int mapped = (int)((bin_word(a, i >> 6) >> (i & 63)) & 1);

		CHK(mapped == (a->bins[i] != NULL), "bin %d map bit %d but head %p",
		    i, mapped, (void *)a->bins[i]);
		for (b = a->bins[i]; b; b = blk_from_off(a, blk_node(b)->next)) {
			size_t size = blk_size(b);

			CHK((char *)b >= a->data_start && (char *)b + size <= a->bump,
			    "binned block %p outside the arena", (void *)b);
			CHK(blk_flags(b) & BLOCK_FREE, "binned block %p is not free", (void *)b);
			CHK(bin_index(size) == i, "block %p of size %zu lives in bin %d",
			    (void *)b, size, i);
			CHK(!blk_node(b)->next ||
			    blk_node(blk_from_off(a, blk_node(b)->next))->prev == blk_off(a, b),
			    "broken free-list links at %p", (void *)b);
			binned_bytes += size;
			binned_blocks++;
			if (binned_blocks > blocks_free + 1)
				break;		/* cycle guard */
		}
	}
	CHK(binned_blocks == blocks_free, "%zu free blocks in the chain, %zu in the bins",
	    blocks_free, binned_blocks);
	CHK(binned_bytes == bytes_free, "%zu free bytes in the chain, %zu in the bins",
	    bytes_free, binned_bytes);
	CHK(cnt_get(&a->bytes_free) == bytes_free, "bytes_free counter %zu, actual %zu",
	    cnt_get(&a->bytes_free), bytes_free);
	if (OSMEM_HAS_STATS) {
		CHK(cnt_get(&a->bytes_used) == bytes_used,
		    "bytes_used counter %zu, actual %zu", cnt_get(&a->bytes_used),
		    bytes_used);
		CHK(cnt_get(&a->blocks_free) == blocks_free,
		    "blocks_free counter %zu, actual %zu", cnt_get(&a->blocks_free),
		    blocks_free);
		CHK(cnt_get(&a->blocks_used) == blocks_used,
		    "blocks_used counter %zu, actual %zu", cnt_get(&a->blocks_used),
		    blocks_used);
	}

	/* The hint bitmaps must agree with the real state while nothing runs. */
	{
		int state = atomic_load_explicit(&a->state, memory_order_relaxed);

		CHK(mask_test(g_orphan_mask, a->index) == (state == ARENA_ORPHAN),
		    "orphan hint is %d but the state is %d",
		    mask_test(g_orphan_mask, a->index), state);
		CHK(mask_test(g_shared_mask, a->index) == (state == ARENA_SHARED),
		    "shared hint is %d but the state is %d",
		    mask_test(g_shared_mask, a->index), state);
		CHK(a->index == idx, "arena thinks it is slot %d, it is %d", a->index, idx);
	}

	/* Pending cross-thread frees must still look allocated. */
	for (b = atomic_load_explicit(&a->remote, memory_order_acquire); b;
	     b = *blk_remote_next(b)) {
		CHK(!(blk_flags(b) & BLOCK_FREE), "remote block %p already free", (void *)b);
		CHK(b->arena == a, "remote block %p has a foreign arena pointer", (void *)b);
	}

	return errors;
}

/*
 * With -DOSMEM_WORLD_LOCK this stops the world first, so it is safe to call
 * while other threads are allocating -- which is where it finds the most.
 * Without it, the caller must make sure no other thread is inside the
 * allocator.
 */
int osmem_check(void)
{
	int errors = 0;
	int n, i;

	osmem_boot();
	WORLD_STOP();
	n = arena_count();
	for (i = 0; i < n; i++) {
		struct arena *a = arena_at(i);

		/* A quarantined arena is known to be inconsistent by design. */
		if (!a || atomic_load_explicit(&a->state, memory_order_acquire) == ARENA_POISONED)
			continue;
		pthread_mutex_lock(&a->lock);
		errors += check_arena(a, i);
		pthread_mutex_unlock(&a->lock);
	}
	WORLD_GO();
	return errors;
}
