// SPDX-License-Identifier: BSD-3-Clause
/*
 * osmem.c -- the public API.
 *
 * os_malloc(), os_calloc(), os_realloc(), os_free(), the allocation ladder of
 * Phase 2 that they all funnel through, and the blocks that are too large for
 * an arena and get a private mapping of their own.
 */

#define _GNU_SOURCE

#include "osmem_internal.h"

static void *alloc_common(size_t size, int want_zero);
static void free_inner(void *ptr);

/* ------------------------------------------------------ standalone blocks */

/* Blocks too big for an arena get their own mapping and are returned to the
 * kernel as soon as they are freed.
 */
static void *standalone_alloc(size_t size)
{
	size_t total = align_up(size + BLOCK_META_SIZE, g_page);
	struct block_meta *b;
	void *map;

	if (total < size || total > BLOCK_MAX_SIZE)
		return NULL;

	map = osmem_map(total);
	if (map == MAP_FAILED)
		return NULL;

	atomic_fetch_add_explicit(&g_mmap_calls, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_bytes_mapped, total, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_standalone_blocks, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_standalone_bytes, total, memory_order_relaxed);

	b = map;
	blk_set(b, total, BLOCK_STANDALONE);
	b->arena = NULL;
	return blk_payload(b);
}

static void standalone_free(struct block_meta *b)
{
	size_t total = blk_size(b);

	atomic_fetch_sub_explicit(&g_standalone_blocks, 1, memory_order_relaxed);
	atomic_fetch_sub_explicit(&g_standalone_bytes, total, memory_order_relaxed);
	atomic_fetch_sub_explicit(&g_bytes_mapped, total, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_munmap_calls, 1, memory_order_relaxed);
	munmap(b, total);
}

/* --------------------------------------------------- over-aligned blocks */

/*
 * A request for an alignment stricter than BLOCK_ALIGN is served out of an
 * ordinary, over-sized block: the payload handed back is moved forward to the
 * first aligned address, and a second header -- a "shim" -- is written in
 * front of it holding the distance back to the real one.  free(), realloc()
 * and usable_size() recognise it by BLOCK_SHIM and step back.
 *
 *   real header | ...slack... | shim | aligned payload ............ |
 *   ^b                          ^b2   ^returned pointer
 */
static size_t usable_from(struct block_meta *b, void *ptr)
{
	if (blk_flags(b) & BLOCK_SHIM) {
		struct block_meta *real =
			(struct block_meta *)((char *)b - blk_size(b));

		return (size_t)(((char *)real + blk_size(real)) - (char *)ptr);
	}
	return blk_size(b) - BLOCK_META_SIZE;
}

size_t os_usable_size(void *ptr)
{
	struct block_meta *b;

	if (!ptr)
		return 0;
	osmem_boot();
	b = blk_of(ptr);
	if (!blk_tagged(b)) {
		atomic_fetch_add_explicit(&g_foreign_ptrs, 1, memory_order_relaxed);
		return 0;
	}
	return usable_from(b, ptr);
}

void *os_memalign(size_t alignment, size_t size)
{
	struct block_meta *shim;
	char *raw, *aligned;
	size_t total;
	void *p;

	if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
		errno = EINVAL;
		return NULL;
	}
	if (size == 0)
		return NULL;
	if (alignment <= BLOCK_ALIGN)
		return os_malloc(size);		/* already guaranteed */

	/* Room for the worst-case shift plus the shim header. */
	if (size > MAX_ALLOC - alignment - BLOCK_META_SIZE) {
		errno = ENOMEM;
		return NULL;
	}
	total = size + alignment + BLOCK_META_SIZE;

	osmem_boot();
	WORLD_ENTER();
	p = alloc_common(total, 0);
	WORLD_LEAVE();
	if (!p)
		return NULL;

	raw = p;
	aligned = (char *)align_up((size_t)(uintptr_t)raw + BLOCK_META_SIZE, alignment);
	if (aligned == raw)
		return raw;			/* already aligned, no shim needed */

	shim = (struct block_meta *)(aligned - BLOCK_META_SIZE);
	blk_set(shim, (size_t)((char *)shim - (char *)blk_of(raw)), BLOCK_SHIM);
	shim->arena = NULL;
	return aligned;
}

int os_posix_memalign(void **out, size_t alignment, size_t size)
{
	void *p;

	if (!out)
		return EINVAL;
	if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
	    alignment % sizeof(void *) != 0)
		return EINVAL;

	p = os_memalign(alignment, size ? size : 1);
	if (!p)
		return ENOMEM;
	*out = p;
	return 0;
}

void *os_aligned_alloc(size_t alignment, size_t size)
{
	return os_memalign(alignment, size);
}

/* --------------------------------------------------------- the main paths */

static size_t need_for(size_t size)
{
	size_t need = align_up(size + BLOCK_META_SIZE, BLOCK_ALIGN);

	return need < BLOCK_MIN_SIZE ? BLOCK_MIN_SIZE : need;
}

/*
 * Phase 2: the ladder.  Private arena -> adopted orphan -> new private arena
 * -> shared pool -> private mapping as a last resort.
 */
static void *alloc_common(size_t size, int want_zero)
{
	unsigned long long me;
	struct arena *a;
	size_t need;
	int zeroed = 0;
	void *p;

	if (size > MAX_ALLOC) {
		errno = ENOMEM;
		return NULL;
	}

	need = need_for(size);
	if (need > g_standalone_min) {
		p = standalone_alloc(size);
		if (!p)
			errno = ENOMEM;
		return p;		/* fresh mapping: already zeroed */
	}

	tls_register();
	me = self_id();

	/* A: private arena, lock-free (Phase 2A). */
	a = tl_arena;
	if (a && atomic_load_explicit(&a->owner, memory_order_relaxed) == me) {
		p = arena_alloc(a, need, &zeroed);
		if (p)
			goto done;
	}

	/* B: adopt an arena left behind by a finished thread. */
	CHAOS();
	p = arena_adopt(me, need, &zeroed, &a);
	if (p) {
		tl_arena = a;
		goto done;
	}

	/* C: ask the kernel for a private arena, if the budget allows. */
	a = arena_create(me, ARENA_EXCLUSIVE);
	if (a) {
		p = arena_alloc(a, need, &zeroed);
		if (p) {
			tl_arena = a;
			goto done;
		}
	}

	/* C2: an idle arena from the shared pool can go private again. */
	p = arena_reclaim(me, need, &zeroed, &a);
	if (p) {
		tl_arena = a;
		goto done;
	}

	/* D: shared regime. */
	CHAOS();
	p = ffa_alloc(need, &zeroed);
	if (p)
		goto done;

	/* Pressure valve: every arena is full and A_max is spent.  Returning
	 * NULL while the kernel still has memory would be worse than one extra
	 * mapping, which is freed again as soon as the block is.
	 */
	p = standalone_alloc(size);
	if (!p) {
		errno = ENOMEM;
		return NULL;
	}
	return p;

done:
	if (want_zero && !zeroed)
		memset(p, 0, size);
	return p;
}

void *os_malloc(size_t size)
{
	void *p;

	if (size == 0)
		return NULL;
	osmem_boot();
	WORLD_ENTER();
	p = alloc_common(size, 0);
	WORLD_LEAVE();
	return p;
}

void *os_calloc(size_t nmemb, size_t size)
{
	size_t total;
	void *p;

	if (nmemb == 0 || size == 0)
		return NULL;
	if (nmemb > SIZE_MAX / size) {		/* overflow */
		errno = ENOMEM;
		return NULL;
	}
	osmem_boot();
	total = nmemb * size;
	WORLD_ENTER();
	p = alloc_common(total, 1);
	WORLD_LEAVE();
	return p;
}

/*
 * Phase 3: the header tells us which arena the block came from; arena_acquire()
 * picks the cheapest safe way in.
 */
static void free_inner(void *ptr)
{
	struct block_meta *b;
	struct arena *a;
	enum acquire_mode mode;

	b = blk_of(ptr);
	if (!blk_tagged(b)) {
		/* Not a pointer this allocator handed out.  Doing nothing is
		 * the only safe answer: acting on it would corrupt whatever
		 * really lives there.
		 */
		atomic_fetch_add_explicit(&g_foreign_ptrs, 1, memory_order_relaxed);
		return;
	}
	if (blk_flags(b) & BLOCK_SHIM)		/* over-aligned: step back to the real block */
		b = (struct block_meta *)((char *)b - blk_size(b));
	a = b->arena;
	if (!a) {
		standalone_free(b);
		return;
	}

	mode = arena_acquire(a, self_id());
	if (mode == ACQ_REMOTE) {
		remote_push(a, b);
		return;
	}
	if (mode == ACQ_POISONED)
		return;		/* unrepairable arena: the block is left behind */
	if (blk_flags(b) & BLOCK_FREE) {	/* best effort double-free guard */
		arena_release_acquired(a, mode);
		return;
	}
	arena_free_user(a, b);
	arena_release_acquired(a, mode);
}

void os_free(void *ptr)
{
	if (!ptr)
		return;
	osmem_boot();
	WORLD_ENTER();
	free_inner(ptr);
	WORLD_LEAVE();
}

static void *realloc_standalone(struct block_meta *b, void *ptr, size_t size)
{
	size_t old_total = blk_size(b);
	size_t new_total = align_up(size + BLOCK_META_SIZE, g_page);
	struct block_meta *nb;

	/* Shrunk enough to live inside an arena again? */
	if (need_for(size) <= g_standalone_min) {
		void *np = alloc_common(size, 0);

		if (!np)
			return NULL;
		memcpy(np, ptr, size < old_total - BLOCK_META_SIZE ? size : old_total - BLOCK_META_SIZE);
		standalone_free(b);
		return np;
	}

	if (new_total == old_total)
		return ptr;

	nb = osmem_remap(b, old_total, new_total);
	if (nb == MAP_FAILED) {
		void *np = alloc_common(size, 0);

		if (!np)
			return NULL;
		memcpy(np, ptr, old_total - BLOCK_META_SIZE);
		standalone_free(b);
		return np;
	}

	if (new_total > old_total)
		atomic_fetch_add_explicit(&g_bytes_mapped, new_total - old_total,
					  memory_order_relaxed);
	else
		atomic_fetch_sub_explicit(&g_bytes_mapped, old_total - new_total,
					  memory_order_relaxed);
	atomic_fetch_add_explicit(&g_standalone_bytes, new_total - old_total, memory_order_relaxed);

	blk_set(nb, new_total, BLOCK_STANDALONE);
	nb->arena = NULL;
	return blk_payload(nb);
}

static void *realloc_inner(void *ptr, size_t size)
{
	enum acquire_mode mode;
	struct block_meta *b;
	struct arena *a;
	size_t old, need;
	void *np;

	b = blk_of(ptr);
	if (!blk_tagged(b)) {
		atomic_fetch_add_explicit(&g_foreign_ptrs, 1, memory_order_relaxed);
		errno = EINVAL;
		return NULL;
	}
	if (blk_flags(b) & BLOCK_SHIM) {
		/* An over-aligned block cannot be grown in place without losing
		 * its alignment guarantee, so move it into an ordinary block --
		 * which is also what realloc() of a memalign'd pointer does
		 * everywhere else.
		 */
		size_t usable = usable_from(b, ptr);

		np = alloc_common(size, 0);
		if (!np)
			return NULL;
		memcpy(np, ptr, size < usable ? size : usable);
		free_inner(ptr);
		return np;
	}
	a = b->arena;
	if (!a)
		return realloc_standalone(b, ptr, size);

	need = need_for(size);
	mode = arena_acquire(a, self_id());
	old = blk_size(b);

	if (mode != ACQ_REMOTE && mode != ACQ_POISONED) {
		struct block_meta *next;

		if (need <= old) {
			blk_trim(a, b, old, need);
			arena_release_acquired(a, mode);
			return ptr;
		}

		/* Grow in place by eating the hole in front of us ... */
		next = blk_next(a, b, old);
		if (next && (blk_flags(next) & BLOCK_FREE) && old + blk_size(next) >= need) {
			size_t have = old + blk_size(next);

			bin_remove(a, next);
			STAT_ADD(&a->bytes_used, blk_size(next));
			blk_set(b, have, blk_flags(b) & BLOCK_PREV_FREE);
			blk_trim(a, b, have, need);
			arena_release_acquired(a, mode);
			return ptr;
		}
		/* ... or by pushing the arena's own frontier further out. */
		if (!next && (char *)b + old == a->bump &&
		    (size_t)(a->data_end - a->bump) >= need - old) {
			size_t grow = need - old;

			a->bump += grow;
			if (a->bump > a->hwm) {
				a->hwm = a->bump;
				STAT_SET(&a->touched, (size_t)(a->hwm - a->data_start));
			}
			atomic_store_explicit(&a->bump_room,
					      (size_t)(a->data_end - a->bump),
					      memory_order_relaxed);
			blk_set(b, need, blk_flags(b) & BLOCK_PREV_FREE);
			STAT_ADD(&a->bytes_used, grow);
			arena_release_acquired(a, mode);
			return ptr;
		}
		arena_release_acquired(a, mode);
	}

	/*
	 * Everything else: copy into a fresh block.  Note that this is taken
	 * even when the block is *shrinking*, if its arena belongs to another
	 * live thread (ACQ_REMOTE) or was torn by a fork (ACQ_POISONED).
	 * Trimming in place would mean inserting the tail into that arena's
	 * free list, and the concurrency contract forbids touching it at all --
	 * so a copy is not a missed optimisation here, it is the only legal
	 * move.
	 *
	 * A block's own size stays readable even in a quarantined arena: only
	 * the arena's free lists can be torn, never the header of a block that
	 * is handed out.
	 */
	np = alloc_common(size, 0);
	if (!np)
		return NULL;
	memcpy(np, ptr, size < old - BLOCK_META_SIZE ? size : old - BLOCK_META_SIZE);
	free_inner(ptr);
	return np;
}

size_t os_trim(void)
{
	size_t released = 0;
	unsigned long long me;
	int n, i;

	osmem_boot();
	WORLD_ENTER();
	me = self_id();
	n = arena_count();
	for (i = 0; i < n; i++) {
		struct arena *a = arena_at(i);
		enum acquire_mode mode;

		if (!a)
			continue;
		/* Only arenas we are allowed inside: one owned by another live
		 * thread is off limits, exactly as it is for free().
		 */
		mode = arena_acquire(a, me);
		if (mode == ACQ_REMOTE || mode == ACQ_POISONED)
			continue;
		released += arena_trim(a);
		arena_release_acquired(a, mode);
	}
	WORLD_LEAVE();
	return released;
}

void *os_realloc(void *ptr, size_t size)
{
	void *p;

	if (!ptr)
		return os_malloc(size);
	if (size == 0) {
		os_free(ptr);
		return NULL;
	}
	if (size > MAX_ALLOC) {
		errno = ENOMEM;
		return NULL;
	}
	osmem_boot();
	WORLD_ENTER();
	p = realloc_inner(ptr, size);
	WORLD_LEAVE();
	return p;
}
