// SPDX-License-Identifier: BSD-3-Clause
/*
 * block.c -- everything that happens inside a single arena.
 *
 * The size-bucketed free lists (61 exact-size bins below 1 KiB, power-of-two
 * bins above, with a summary bitmap), splitting, immediate coalescing
 * including handing a trailing hole back to the bump pointer, the
 * lock-free stack that carries blocks freed by a thread which was not allowed
 * inside, and arena_alloc(), which carves a block out.
 *
 * Every function here assumes the caller already has exclusive access to the
 * arena -- as its owner, or holding its lock with no owner present.
 */

#define _GNU_SOURCE

#include "osmem_internal.h"

/* Lowest non-empty bin with index >= from, or -1. */
static int bin_first_from(struct arena *a, int from)
{
	int w = from >> 6;
	uint64_t m = bin_word(a, w) & (~0ULL << (from & 63));

	for (;;) {
		if (m)
			return (w << 6) + __builtin_ctzll(m);
		if (++w >= BIN_WORDS)
			return -1;
		m = bin_word(a, w);
	}
}

/* Highest non-empty bin, or -1. */
OSMEM_INTERNAL int bin_last(struct arena *a)
{
	int w;

	for (w = BIN_WORDS - 1; w >= 0; w--) {
		uint64_t m = bin_word(a, w);

		if (m)
			return (w << 6) + 63 - __builtin_clzll(m);
	}
	return -1;
}

static void bin_insert(struct arena *a, struct block_meta *b)
{
	size_t size = blk_size(b);
	int i = bin_index(size);
	struct block_meta *head = a->bins[i];

	blk_node(b)->prev = 0;
	blk_node(b)->next = blk_off(a, head);
	if (head)
		blk_node(head)->prev = blk_off(a, b);
	a->bins[i] = b;
	bin_word_set(a, i);

	cnt_add(&a->bytes_free, size);
	STAT_ADD(&a->blocks_free, 1);
}

OSMEM_INTERNAL void bin_remove(struct arena *a, struct block_meta *b)
{
	size_t size = blk_size(b);
	int i = bin_index(size);
	struct block_meta *prev = blk_from_off(a, blk_node(b)->prev);
	struct block_meta *next = blk_from_off(a, blk_node(b)->next);

	if (prev)
		blk_node(prev)->next = blk_off(a, next);
	else
		a->bins[i] = next;
	if (next)
		blk_node(next)->prev = blk_off(a, prev);
	if (!a->bins[i])
		bin_word_clear(a, i);

	cnt_sub(&a->bytes_free, size);
	STAT_SUB(&a->blocks_free, 1);
}

/* Best fit inside one bin.  Exact-size bins answer in O(1). */
static struct block_meta *bin_best(struct arena *a, int i, size_t need)
{
	struct block_meta *b = a->bins[i];
	struct block_meta *best = NULL;
	size_t best_size = 0;
	int budget = BIN_SCAN_BUDGET;

	if (i < SMALL_BINS)
		return b;

	for (; b && budget > 0; b = blk_from_off(a, blk_node(b)->next), budget--) {
		size_t size = blk_size(b);

		if (size < need)
			continue;
		if (!best || size < best_size) {
			best = b;
			best_size = size;
			if (size == need)
				break;
		}
	}
	return best;
}

static struct block_meta *bin_find(struct arena *a, size_t need)
{
	int want = bin_index(need);
	int i = want;

	while (i >= 0 && i < NBINS) {
		struct block_meta *b;

		i = bin_first_from(a, i);
		if (i < 0)
			return NULL;
		/* Anything in an exact-size bin >= want fits, and so does
		 * anything in a range bin strictly above the requested one.
		 */
		if (i < SMALL_BINS || i > want)
			return bin_best(a, i, need);
		b = bin_best(a, i, need);
		if (b)
			return b;
		i++;
	}
	return NULL;
}

/* ----------------------------------------------------- returning pages */

/*
 * Hand a page range back to the kernel without giving up the mapping, so the
 * arena budget (A_max) is untouched but the physical footprint drops.
 * MADV_DONTNEED on a private anonymous mapping also means the range reads as
 * zero again, which is why the caller may push the arena's high-water mark
 * back: the calloc() fast path depends on that invariant.
 */
static size_t arena_release_pages(struct arena *a, char *from, char *to)
{
	char *lo = (char *)align_up((size_t)(uintptr_t)from, g_page);
	char *hi = (char *)((size_t)(uintptr_t)to & ~(g_page - 1));

	if (hi <= lo)
		return 0;
	if (madvise(lo, (size_t)(hi - lo), MADV_DONTNEED) != 0)
		return 0;
	atomic_fetch_add_explicit(&g_bytes_released, (size_t)(hi - lo),
				  memory_order_relaxed);
	(void)a;
	return (size_t)(hi - lo);
}

/*
 * Release what this arena is physically holding.  Called on an arena the
 * caller has exclusive access to.  An empty arena is released wholesale;
 * otherwise only the interior pages of the free blocks go back, leaving each
 * block's header, free-list links and footer resident.
 */
OSMEM_INTERNAL size_t arena_trim(struct arena *a)
{
	size_t released = 0;
	int i;

	if (atomic_load_explicit(&a->remote, memory_order_relaxed))
		remote_drain(a);
	arena_flush(a);

	if (a->bump == a->data_start) {
		/* Round the end *up*: everything above the high-water mark is
		 * untouched anyway, so releasing it as well costs nothing and
		 * leaves the whole region genuinely zero -- which is what makes
		 * resetting the high-water mark sound.
		 */
		char *end = (char *)align_up((size_t)(uintptr_t)a->hwm, g_page);

		released = arena_release_pages(a, a->data_start, end);
		if (released) {
			a->hwm = a->data_start;
			STAT_SET(&a->touched, 0);
		}
		return released;
	}

	for (i = 0; i < NBINS; i++) {
		struct block_meta *b;

		for (b = a->bins[i]; b; b = blk_from_off(a, blk_node(b)->next)) {
			size_t size = blk_size(b);

			if (size < 2 * g_page)
				continue;
			released += arena_release_pages(a,
					(char *)b + BLOCK_META_SIZE + sizeof(struct free_node),
					(char *)b + size - BLOCK_FOOTER_SIZE);
		}
	}
	return released;
}

OSMEM_INTERNAL void blk_trim(struct arena *a, struct block_meta *b, size_t have, size_t need)
{
	unsigned int keep_flags = blk_flags(b) & BLOCK_PREV_FREE;

	if (have - need >= BLOCK_MIN_SIZE) {
		struct block_meta *tail = (struct block_meta *)((char *)b + need);

		blk_set(b, need, keep_flags);
		blk_set(tail, have - need, 0);
		tail->arena = a;
		/* b was accounted for its full size, so arena_free_insert()
		 * subtracting the tail is exactly the correction needed; only
		 * the block count has to be balanced beforehand.
		 */
		STAT_ADD(&a->blocks_used, 1);
		arena_free_insert(a, tail);
	} else {
		struct block_meta *next;

		blk_set(b, have, keep_flags);
		next = blk_next(a, b, have);
		if (next)
			blk_flag_clear(next, BLOCK_PREV_FREE);
	}
}

/*
 * Put a block back into its arena, merging it with the physically
 * adjacent holes, and give the space back to the bump pointer when the block
 * ends up sitting at the frontier.
 */
OSMEM_INTERNAL void arena_free_insert(struct arena *a, struct block_meta *b)
{
	size_t size = blk_size(b);
	struct block_meta *next;

	STAT_SUB(&a->bytes_used, size);
	STAT_SUB(&a->blocks_used, 1);

	if (blk_flags(b) & BLOCK_PREV_FREE) {
		size_t prev_size = blk_prev_footer(b);
		struct block_meta *prev = (struct block_meta *)((char *)b - prev_size);

		bin_remove(a, prev);
		size += prev_size;
		b = prev;
	}

	next = blk_next(a, b, size);
	if (next && (blk_flags(next) & BLOCK_FREE)) {
		bin_remove(a, next);
		size += blk_size(next);
		next = blk_next(a, b, size);
	}

	if ((char *)b + size == a->bump) {
		/* Trailing hole: retract the local program break instead of
		 * keeping a free block around.  The block below the frontier
		 * is in use again, because we just merged backwards.
		 */
		a->bump = (char *)b;
		atomic_store_explicit(&a->bump_room, (size_t)(a->data_end - a->bump),
				      memory_order_relaxed);
		/* The arena just fell completely idle.  If it had grown a real
		 * footprint, give the pages back: this is the one point where
		 * the allocator can do it without guessing, and it is exactly
		 * the case that keeps a process at its peak RSS after a burst.
		 */
		if (a->bump == a->data_start &&
		    (size_t)(a->hwm - a->data_start) >= g_trim_threshold)
			arena_trim(a);
		return;
	}

	blk_set(b, size, BLOCK_FREE);
	b->arena = a;
	blk_put_footer(b, size);
	bin_insert(a, b);
	/* Writing a flag into the header of a block another thread may be
	 * holding: the window that the split size/flags layout makes safe.
	 */
	CHAOS();
	if (next)
		blk_flag_set(next, BLOCK_PREV_FREE);
}

OSMEM_INTERNAL void remote_push(struct arena *a, struct block_meta *b)
{
	struct block_meta *head = atomic_load_explicit(&a->remote, memory_order_relaxed);

	do {
		*blk_remote_next(b) = head;
		CHAOS();
	} while (!atomic_compare_exchange_weak_explicit(&a->remote, &head, b,
							memory_order_release,
							memory_order_relaxed));
	atomic_fetch_add_explicit(&g_remote_frees, 1, memory_order_relaxed);
}

/* Caller has exclusive access to the arena. */
OSMEM_INTERNAL void remote_drain(struct arena *a)
{
	struct block_meta *b = atomic_exchange_explicit(&a->remote, NULL, memory_order_acquire);

	CHAOS();
	while (b) {
		struct block_meta *next = *blk_remote_next(b);

		arena_free_insert(a, b);
		b = next;
	}
}

/*
 * Carve need bytes out of a.  Caller has exclusive access.
 * *zeroed is set when the payload is known to be untouched kernel memory.
 */
/* ------------------------------------------------- delayed coalescing */

#ifdef OSMEM_QUARANTINE

static void quarantine_flush_class(struct arena *a, int i)
{
	uint32_t off = a->quarantine[i];

	a->quarantine[i] = 0;
	a->qcount[i] = 0;
	while (off) {
		struct block_meta *b = blk_from_off(a, off);

		off = blk_node(b)->next;
		cnt_sub(&a->bytes_quarantined, blk_size(b));
		arena_free_insert(a, b);
	}
}

/* 1 if the block was parked, 0 if it has to go into the free list. */
static int quarantine_push(struct arena *a, struct block_meta *b)
{
	size_t size = blk_size(b);
	int i;

	if (size >= SMALL_BIN_LIMIT)
		return 0;
	i = (int)(size / BLOCK_ALIGN);
	if (a->qcount[i] >= QUARANTINE_MAX)
		quarantine_flush_class(a, i);
	blk_node(b)->next = a->quarantine[i];
	a->quarantine[i] = blk_off(a, b);
	a->qcount[i]++;
	cnt_add(&a->bytes_quarantined, size);
	return 1;
}

static void *quarantine_pop(struct arena *a, size_t need)
{
	struct block_meta *b;
	int i;

	if (need >= SMALL_BIN_LIMIT)
		return NULL;
	i = (int)(need / BLOCK_ALIGN);
	if (!a->quarantine[i])
		return NULL;
	b = blk_from_off(a, a->quarantine[i]);
	a->quarantine[i] = blk_node(b)->next;
	a->qcount[i]--;
	cnt_sub(&a->bytes_quarantined, need);
	/* Still flagged in use and still exactly the right size: there is
	 * nothing at all to do but hand it back.
	 */
	return blk_payload(b);
}

static int quarantine_empty(struct arena *a)
{
	return cnt_get(&a->bytes_quarantined) == 0;
}

OSMEM_INTERNAL void arena_flush(struct arena *a)
{
	int i;

	for (i = 0; i < QUARANTINE_CLASSES; i++)
		if (a->quarantine[i])
			quarantine_flush_class(a, i);
}

#else

static inline void *quarantine_pop(struct arena *a, size_t need)
{
	(void)a; (void)need;
	return NULL;
}

static inline int quarantine_push(struct arena *a, struct block_meta *b)
{
	(void)a; (void)b;
	return 0;
}

static inline int quarantine_empty(struct arena *a)
{
	(void)a;
	return 1;
}

OSMEM_INTERNAL void arena_flush(struct arena *a)
{
	(void)a;
}

#endif

OSMEM_INTERNAL void arena_free_user(struct arena *a, struct block_meta *b)
{
	if (quarantine_push(a, b))
		return;
	arena_free_insert(a, b);
}

/* ------------------------------------------------------- arena allocation */

static void *arena_carve(struct arena *a, size_t need, int *zeroed)
{
	struct block_meta *b;
	size_t have;

	b = bin_find(a, need);
	if (b) {
		have = blk_size(b);
		bin_remove(a, b);
		blk_set(b, have, 0);	/* in use; a binned block never has PREV_FREE */
		b->arena = a;
		STAT_ADD(&a->bytes_used, have);
		STAT_ADD(&a->blocks_used, 1);
		blk_trim(a, b, have, need);
		return blk_payload(b);
	}

	if ((size_t)(a->data_end - a->bump) >= need) {
		b = (struct block_meta *)a->bump;
		/* Fresh mmap() memory is zero filled, so a block carved past
		 * the high-water mark needs no memset in calloc().
		 */
		*zeroed = (char *)blk_payload(b) >= a->hwm;
		blk_set(b, need, 0);
		b->arena = a;
		a->bump += need;
		if (a->bump > a->hwm) {
			a->hwm = a->bump;
			STAT_SET(&a->touched, (size_t)(a->hwm - a->data_start));
		}
		atomic_store_explicit(&a->bump_room, (size_t)(a->data_end - a->bump),
				      memory_order_relaxed);
		STAT_ADD(&a->bytes_used, need);
		STAT_ADD(&a->blocks_used, 1);
		return blk_payload(b);
	}

	return NULL;
}

/*
 * Carve need bytes out of a.  Caller has exclusive access.
 * *zeroed is set when the payload is known to be untouched kernel memory.
 */
OSMEM_INTERNAL void *arena_alloc(struct arena *a, size_t need, int *zeroed)
{
	void *p;

	if (atomic_load_explicit(&a->remote, memory_order_relaxed))
		remote_drain(a);

	*zeroed = 0;

	/* A parked block of exactly this size needs no bookkeeping at all. */
	p = quarantine_pop(a, need);
	if (p)
		return p;

	p = arena_carve(a, need, zeroed);
	if (p)
		return p;

	/* Out of room as things stand: coalesce what is parked and retry once
	 * before telling the caller the arena is full.
	 */
	if (!quarantine_empty(a)) {
		arena_flush(a);
		p = arena_carve(a, need, zeroed);
	}
	return p;
}
