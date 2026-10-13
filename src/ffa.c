// SPDX-License-Identifier: BSD-3-Clause
/*
 * ffa.c -- the shared "free-for-all" regime.
 *
 * Reached once no private arena is available any more.  Scan the pool with
 * trylock so a busy arena is skipped rather than queued on, grow the pool if
 * the A_max budget still allows it or an orphan can be promoted into it, and
 * only as a last resort wait on the arena with the most room.
 */

#define _GNU_SOURCE

#include "osmem_internal.h"

/* Allocate from an arena we already hold the lock for. */
static void *ffa_try(struct arena *a, size_t need, int *zeroed)
{
	if (atomic_load_explicit(&a->owner, memory_order_relaxed) != 0)
		return NULL;
	return arena_alloc(a, need, zeroed);
}

/* Grow the shared pool: a brand new arena if A_max still allows one, else an
 * orphan promoted into the pool.
 */
static void *ffa_grow(size_t need, int *zeroed)
{
	struct arena *a = arena_create(0, ARENA_SHARED);
	int i;

	if (a) {
		void *p;

		/* arena_create() publishes the arena before we get here, so by
		 * now another thread may have reclaimed it out of the pool and
		 * be inside it lock-free: ffa_try() re-checks the owner.
		 */
		CHAOS();
		pthread_mutex_lock(&a->lock);
		p = ffa_try(a, need, zeroed);
		pthread_mutex_unlock(&a->lock);
		if (p)
			return p;
	}

	for (i = mask_next(g_orphan_mask, 0); i >= 0; i = mask_next(g_orphan_mask, i + 1)) {
		void *p;

		a = arena_at(i);
		if (!a || atomic_load_explicit(&a->state, memory_order_acquire) != ARENA_ORPHAN)
			continue;
		pthread_mutex_lock(&a->lock);
		if (atomic_load_explicit(&a->state, memory_order_relaxed) != ARENA_ORPHAN) {
			pthread_mutex_unlock(&a->lock);
			continue;
		}
		p = arena_alloc(a, need, zeroed);
		if (p) {
			atomic_store_explicit(&a->state, ARENA_SHARED, memory_order_release);
			mask_clear(g_orphan_mask, a->index);
			mask_set(g_shared_mask, a->index);
		}
		pthread_mutex_unlock(&a->lock);
		if (p)
			return p;
	}
	return NULL;
}

/*
 * No private arena is available any more, so share: scan the pool
 * with trylock and never wait behind a busy arena; only if every shared arena
 * is contended do we queue up, and then on the roomiest one.
 */
OSMEM_INTERNAL void *ffa_alloc(size_t need, int *zeroed)
{
	struct arena *best = NULL;
	size_t best_room = 0;
	void *p;
	int i;

	atomic_store_explicit(&g_ffa, 1, memory_order_relaxed);

	for (i = mask_next(g_shared_mask, 0); i >= 0; i = mask_next(g_shared_mask, i + 1)) {
		struct arena *a = arena_at(i);

		if (!a || atomic_load_explicit(&a->state, memory_order_acquire) != ARENA_SHARED)
			continue;
		if (pthread_mutex_trylock(&a->lock) != 0)
			continue;
		CHAOS();
		p = ffa_try(a, need, zeroed);
		pthread_mutex_unlock(&a->lock);
		if (p) {
			atomic_fetch_add_explicit(&g_ffa_allocs, 1, memory_order_relaxed);
			return p;
		}
	}

	p = ffa_grow(need, zeroed);
	if (p) {
		atomic_fetch_add_explicit(&g_ffa_allocs, 1, memory_order_relaxed);
		return p;
	}

	for (i = mask_next(g_shared_mask, 0); i >= 0; i = mask_next(g_shared_mask, i + 1)) {
		struct arena *a = arena_at(i);
		size_t room;

		if (!a || atomic_load_explicit(&a->state, memory_order_acquire) != ARENA_SHARED)
			continue;
		room = cnt_get(&a->bytes_free) + cnt_get(&a->bump_room);
		if (room >= need && room > best_room) {
			best_room = room;
			best = a;
		}
	}
	if (!best)
		return NULL;

	pthread_mutex_lock(&best->lock);
	p = ffa_try(best, need, zeroed);
	pthread_mutex_unlock(&best->lock);
	if (p)
		atomic_fetch_add_explicit(&g_ffa_allocs, 1, memory_order_relaxed);
	return p;
}
