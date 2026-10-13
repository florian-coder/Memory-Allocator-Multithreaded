/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * osmem_internal.h -- shared internals of the adaptive arena allocator.
 *
 * Design overview:
 *
 *  Memory   Lazy start, no brk/sbrk at all.  Memory comes from the kernel as
 *           "arenas": one mmap() each, carved in user space by a local bump
 *           pointer plus a local free list.  A hard cap (A_max) limits how
 *           many such mappings the process may ever ask for.
 *
 *  Alloc    malloc() climbs a ladder:
 *             A. the calling thread's *private* arena, with no locking at all;
 *             B. adoption of an arena orphaned by a thread that has exited;
 *             C. a new private arena, while the A_max budget allows it;
 *            C2. an idle arena pulled back out of the shared pool;
 *             D. the shared free-for-all pool: trylock scan over the shared
 *                arenas, then a blocking lock on the roomiest one.
 *
 *  Free     free() reads the hidden header to find the arena the block was
 *           carved from, takes the cheapest safe route into that arena's free
 *           list, and coalesces with the physical neighbours straight away.
 *           Arena memory is never handed back to the kernel; only blocks that
 *           own a private mapping (too large for an arena) are munmap()-ed.
 *
 * Where everything lives
 * ----------------------
 *   osmem_internal.h  this file: tunables, the arena and acquire types, the
 *                     global state, and every hot inline helper
 *   arena.c           global state, start-up, thread identity, and the life
 *                     cycle of an arena (create, adopt, reclaim, release,
 *                     acquire)
 *   block.c           everything *inside* one arena: size-bucketed free
 *                     lists, splitting, coalescing, the cross-thread free
 *                     stack, and carving a block out
 *   ffa.c             the shared (free-for-all) regime
 *   osmem.c           the public API, plus blocks that own a private mapping
 *   debug.c           introspection and the consistency checker
 *
 * Concurrency contract (the one invariant everything else rests on)
 * -----------------------------------------------------------------
 * The interior of an arena (bump pointer, bins, bitmap, block headers) may be
 * touched only by
 *      (a) the thread recorded in arena->owner, or
 *      (b) a thread holding arena->lock, and only while arena->owner == 0.
 *
 * (a) is what makes the private fast path lock-free: an EXCLUSIVE arena has an
 * owner, so no one else may enter it -- not even to free.  A thread freeing a
 * block that belongs to somebody else's private arena therefore does *not*
 * touch the arena; it pushes the block on a lock-free stack (arena->remote)
 * which the owner drains on its next allocation.  The obvious alternative,
 * letting the freeing thread take the owner's mutex, would put a lock back
 * on the private fast path.
 *
 * Every path that mutates an arena under the lock must re-check the owner
 * afterwards, including the ones that look like they cannot need it --
 * arena_create() publishes an arena before its creator locks it, so by then
 * somebody else may legitimately own it.  arena_acquire() and ffa_try() are
 * the two places that do the re-checking.
 *
 * Arena structs live inside their own mapping and are never unmapped, so a
 * block header's arena pointer stays valid forever -- no use-after-free is
 * possible when a cross-thread free races with the owner's exit.
 */

#pragma once

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "block_meta.h"
#include "osmem.h"
#include "osmem_debug.h"

/*
 * Internals are shared between the modules above but must not leak out of the
 * shared library, where a name like bin_remove() could be interposed by the
 * program using it.  Hidden visibility keeps them link-time visible to each
 * other and invisible to everybody else.
 */
#define OSMEM_INTERNAL __attribute__((visibility("hidden")))

/*
 * Test hooks.  All three are no-ops unless explicitly enabled, so a release
 * build pays nothing for them.
 *
 *   CHAOS()        -- with -DOSMEM_CHAOS, randomly yields or spins at the
 *                     exact windows where two threads can interleave, so a
 *                     rare interleaving becomes a likely one.  It widens
 *                     windows; it cannot make an unreachable state reachable.
 *                     (Measured against the ffa_grow() race that corrupted
 *                     block headers: 4/25 runs failed with chaos and 5/25
 *                     without, because what hid that bug was reachability --
 *                     nothing ever reclaimed an arena out of the pool -- not
 *                     the width of the window.)
 *   WORLD_ENTER()  -- with -DOSMEM_WORLD_LOCK, every public entry point holds
 *                     a read lock for the whole operation, so osmem_check()
 *                     can stop the world and verify every invariant *while*
 *                     threads are hammering the allocator, instead of only at
 *                     join points.  It also makes fork() fully safe, because
 *                     the prepare handler can then quiesce the lock-free
 *                     owners too.
 *   osmem_map()    -- every mapping goes through it, so a test can make the
 *                     Nth mmap fail and exercise the out-of-memory paths.
 */
#ifdef OSMEM_CHAOS
OSMEM_INTERNAL void osmem_chaos_point(void);
#define CHAOS()		osmem_chaos_point()
#else
#define CHAOS()		((void)0)
#endif

/*
 * Counters that exist only to be reported.  With -DOSMEM_NO_STATS they are
 * compiled out of the hot path; the two the allocator actually makes decisions
 * with (bytes_free for the free-for-all scan, bump_room for everything else)
 * always stay.  osmem_check() skips the counter invariants in that build and
 * osmem_stats() reports zero for them.
 */
#ifdef OSMEM_NO_STATS
#define STAT_ADD(c, v)	((void)0)
#define STAT_SUB(c, v)	((void)0)
#define STAT_SET(c, v)	((void)0)
#define OSMEM_HAS_STATS	0
#else
#define STAT_ADD(c, v)	cnt_add((c), (v))
#define STAT_SUB(c, v)	cnt_sub((c), (v))
#define STAT_SET(c, v)	atomic_store_explicit((c), (v), memory_order_relaxed)
#define OSMEM_HAS_STATS	1
#endif

#ifdef OSMEM_WORLD_LOCK
extern OSMEM_INTERNAL pthread_rwlock_t g_world;
#define WORLD_ENTER()	pthread_rwlock_rdlock(&g_world)
#define WORLD_LEAVE()	pthread_rwlock_unlock(&g_world)
#define WORLD_STOP()	pthread_rwlock_wrlock(&g_world)
#define WORLD_GO()	pthread_rwlock_unlock(&g_world)
#define WORLD_REINIT()	osmem_world_init()
#define OSMEM_HAS_WORLD_LOCK 1
OSMEM_INTERNAL void osmem_world_init(void);
#else
#define WORLD_ENTER()	((void)0)
#define WORLD_LEAVE()	((void)0)
#define WORLD_STOP()	((void)0)
#define WORLD_GO()	((void)0)
#define WORLD_REINIT()	((void)0)
#define OSMEM_HAS_WORLD_LOCK 0
#endif

/* ------------------------------------------------------------------ tuning */

#define ARENA_SIZE_DEFAULT	(4UL << 20)	/* 4 MiB per mmap */
#define ARENA_SIZE_MIN		(64UL << 10)
#define ARENA_HARD_MAX		128		/* ceiling for A_max            */
#define ARENA_MAX_DEFAULT_CAP	64

/* Free-list bins: 0..63 hold one exact size each (16-byte granularity, so
 * small requests get a perfect fit in O(1)); 64..79 are power-of-two ranges.
 */
#define NBINS			80
#define SMALL_BINS		64
#define SMALL_BIN_LIMIT		(SMALL_BINS * BLOCK_ALIGN)	/* 1024 */
#define BIN_SCAN_BUDGET		64		/* best-fit scan cap in a range bin */

/*
 * Optional delayed coalescing (-DOSMEM_QUARANTINE).
 *
 * The default build coalesces every free with its physical neighbours
 * immediately.  That is what costs about half the single-threaded throughput
 * next to glibc, whose tcache pushes a small block on a per-thread list and
 * skips the boundary tags, the neighbour flags and the bin bookkeeping
 * entirely.
 *
 * This flag parks a *bounded* number of small freed blocks per exact size
 * class instead of inserting them, and hands them straight back on the next
 * request for the same size.  Parked blocks stay flagged as allocated, so no
 * invariant changes -- a neighbour simply cannot coalesce with them yet -- and
 * the bound keeps the fragmentation it costs measurable.
 */
#define QUARANTINE_CLASSES	SMALL_BINS
#define QUARANTINE_MAX		8	/* blocks parked per size class */

#define MAX_ALLOC		BLOCK_MAX_SIZE

/* ------------------------------------------------------------------- types */

enum arena_state {
	ARENA_EXCLUSIVE = OSMEM_ARENA_EXCLUSIVE,
	ARENA_ORPHAN	= OSMEM_ARENA_ORPHAN,
	ARENA_SHARED	= OSMEM_ARENA_SHARED,
	/*
	 * Inherited from a fork() while another thread was allocating from it
	 * without a lock.  Its free lists, bitmap and bump pointer may be torn
	 * and cannot be repaired, so the arena is never entered again: blocks
	 * already handed out of it stay readable (their headers are stable),
	 * but nothing is ever allocated from it or returned to it.
	 */
	ARENA_POISONED	= OSMEM_ARENA_POISONED,
};

/*
 * Arena administration block.  Lives at the base of the arena's own mapping,
 * the carvable payload follows it.
 */
struct arena {
	/* Ownership.  Read by every thread, so atomic. */
	_Atomic unsigned long long owner;	/* 0 == no exclusive owner */
	_Atomic int state;
	pthread_mutex_t lock;

	/* Local "program break" over [data_start, data_end). */
	char *data_start;
	char *data_end;
	char *bump;
	char *hwm;		/* [hwm, data_end) was never used: still zeroed */

	/* Free list, bucketed by size, with a summary bitmap.  The bitmap is
	 * atomic so that osmem_stats() can describe the biggest reusable hole
	 * without locking and without chasing a list that may be changing.
	 */
	struct block_meta *bins[NBINS];
	_Atomic uint64_t bin_map[(NBINS + 63) / 64];

	/* Blocks freed by a thread that was not allowed inside this arena. */
	_Atomic(struct block_meta *) remote;

#ifdef OSMEM_QUARANTINE
	/* Small blocks freed but not yet coalesced, by exact size class. */
	uint32_t quarantine[QUARANTINE_CLASSES];
	uint8_t qcount[QUARANTINE_CLASSES];
	_Atomic size_t bytes_quarantined;
#endif

	/* Counters.  Atomic because the FFA scan and osmem_stats() read them
	 * without holding the lock; updated with plain relaxed load/store
	 * (never a locked RMW) since writers always have exclusive access.
	 */
	_Atomic size_t bytes_free;
	_Atomic size_t bytes_used;
	_Atomic size_t bump_room;	/* data_end - bump */
	_Atomic size_t touched;		/* hwm - data_start: our physical footprint */
	_Atomic size_t blocks_free;
	_Atomic size_t blocks_used;

	size_t map_size;
	int index;		/* slot in g_arenas, for the hint bitmaps */
};

/* How a thread got hold of an arena it wants to mutate. */
enum acquire_mode {
	ACQ_OWNER,	/* we own it: nothing to lock, nothing to release */
	ACQ_LOCKED,	/* ownerless arena, we hold arena->lock           */
	ACQ_REMOTE,	/* somebody else's private arena: hands off       */
	ACQ_POISONED,	/* torn by a fork(): must not be touched at all   */
};

/* ------------------------------------------------------------------ globals */

extern OSMEM_INTERNAL _Atomic(struct arena *) g_arenas[ARENA_HARD_MAX];
extern OSMEM_INTERNAL _Atomic int g_arena_count;

extern OSMEM_INTERNAL pthread_once_t g_once;
extern OSMEM_INTERNAL _Atomic int g_ready;
extern OSMEM_INTERNAL _Atomic int g_key_ready;
extern OSMEM_INTERNAL pthread_key_t g_tls_key;

extern OSMEM_INTERNAL size_t g_page;
extern OSMEM_INTERNAL size_t g_arena_size;
extern OSMEM_INTERNAL size_t g_arena_payload;
extern OSMEM_INTERNAL size_t g_standalone_min;
extern OSMEM_INTERNAL int g_arena_max;

extern OSMEM_INTERNAL _Atomic unsigned long long g_next_tid;
extern OSMEM_INTERNAL _Atomic int g_ffa;
extern OSMEM_INTERNAL _Atomic size_t g_mmap_calls;
extern OSMEM_INTERNAL _Atomic size_t g_munmap_calls;
extern OSMEM_INTERNAL _Atomic size_t g_bytes_mapped;
extern OSMEM_INTERNAL _Atomic size_t g_standalone_blocks;
extern OSMEM_INTERNAL _Atomic size_t g_standalone_bytes;
extern OSMEM_INTERNAL _Atomic size_t g_remote_frees;
extern OSMEM_INTERNAL _Atomic size_t g_ffa_allocs;
extern OSMEM_INTERNAL _Atomic size_t g_adoptions;
extern OSMEM_INTERNAL _Atomic size_t g_reclaims;
extern OSMEM_INTERNAL _Atomic size_t g_poisoned;
extern OSMEM_INTERNAL _Atomic long g_map_budget;	/* LONG_MAX == unlimited */
extern OSMEM_INTERNAL _Atomic size_t g_map_denied;
extern OSMEM_INTERNAL _Atomic size_t g_foreign_ptrs;
extern OSMEM_INTERNAL _Atomic size_t g_bytes_released;
/*
 * Hints, not truth: a set bit means "this arena was orphaned / shared the last
 * time anybody looked".  They turn the scans that look for an arena to adopt,
 * reclaim or share into a couple of popcounts instead of a walk over all
 * A_max slots.  Every consumer still re-checks the real state under the
 * arena's lock, so a stale bit only ever costs a wasted lock attempt.
 */
extern OSMEM_INTERNAL _Atomic uint64_t g_orphan_mask[(ARENA_HARD_MAX + 63) / 64];
extern OSMEM_INTERNAL _Atomic uint64_t g_shared_mask[(ARENA_HARD_MAX + 63) / 64];
extern OSMEM_INTERNAL size_t g_trim_threshold;

extern OSMEM_INTERNAL __thread struct arena *tl_arena;
extern OSMEM_INTERNAL __thread unsigned long long tl_tid;
extern OSMEM_INTERNAL __thread int tl_registered;

/* ---------------------------------------------------------------- arena.c */

OSMEM_INTERNAL void osmem_init(void);
/* Every mapping and remapping goes through these, so that a test can starve
 * the allocator of memory on demand.
 */
OSMEM_INTERNAL void *osmem_map(size_t len);
OSMEM_INTERNAL void *osmem_remap(void *addr, size_t old_len, size_t new_len);
OSMEM_INTERNAL struct arena *arena_create(unsigned long long owner, enum arena_state state);
OSMEM_INTERNAL void *arena_adopt(unsigned long long me, size_t need, int *zeroed,
				 struct arena **out);
OSMEM_INTERNAL void *arena_reclaim(unsigned long long me, size_t need, int *zeroed,
				   struct arena **out);
OSMEM_INTERNAL enum acquire_mode arena_acquire(struct arena *a, unsigned long long me);

/* ---------------------------------------------------------------- block.c */

OSMEM_INTERNAL void *arena_alloc(struct arena *a, size_t need, int *zeroed);
OSMEM_INTERNAL void arena_free_insert(struct arena *a, struct block_meta *b);
/* The route a block freed by the caller takes: straight into the free list, or
 * into the delayed-coalescing cache when it is enabled.
 */
OSMEM_INTERNAL void arena_free_user(struct arena *a, struct block_meta *b);
/* Push everything the delayed-coalescing cache is holding into the free list.
 * A no-op unless -DOSMEM_QUARANTINE.
 */
OSMEM_INTERNAL void arena_flush(struct arena *a);
OSMEM_INTERNAL void blk_trim(struct arena *a, struct block_meta *b, size_t have, size_t need);
OSMEM_INTERNAL void bin_remove(struct arena *a, struct block_meta *b);
OSMEM_INTERNAL int bin_last(struct arena *a);
OSMEM_INTERNAL size_t arena_trim(struct arena *a);
OSMEM_INTERNAL void remote_push(struct arena *a, struct block_meta *b);
OSMEM_INTERNAL void remote_drain(struct arena *a);

/* ------------------------------------------------------------------ ffa.c */

OSMEM_INTERNAL void *ffa_alloc(size_t need, int *zeroed);

/* ---------------------------------------------------------- hint bitmaps */

#define MASK_WORDS	((int)((ARENA_HARD_MAX + 63) / 64))

static inline void mask_set(_Atomic uint64_t *m, int i)
{
	atomic_fetch_or_explicit(&m[i >> 6], 1ULL << (i & 63), memory_order_relaxed);
}

static inline void mask_clear(_Atomic uint64_t *m, int i)
{
	atomic_fetch_and_explicit(&m[i >> 6], ~(1ULL << (i & 63)), memory_order_relaxed);
}

static inline int mask_test(_Atomic uint64_t *m, int i)
{
	return (atomic_load_explicit(&m[i >> 6], memory_order_relaxed) >> (i & 63)) & 1;
}

static inline int mask_count(_Atomic uint64_t *m)
{
	int w, n = 0;

	for (w = 0; w < MASK_WORDS; w++)
		n += __builtin_popcountll(atomic_load_explicit(&m[w], memory_order_relaxed));
	return n;
}

/* Index of the next set bit at or after `from`, or -1. */
static inline int mask_next(_Atomic uint64_t *m, int from)
{
	int w = from >> 6;
	uint64_t x;

	if (from >= ARENA_HARD_MAX)
		return -1;
	x = atomic_load_explicit(&m[w], memory_order_relaxed) & (~0ULL << (from & 63));
	for (;;) {
		if (x)
			return (w << 6) + __builtin_ctzll(x);
		if (++w >= MASK_WORDS)
			return -1;
		x = atomic_load_explicit(&m[w], memory_order_relaxed);
	}
}

/* ------------------------------------------------------------ small helpers */

/* ------------------------------------------------------------ small helpers */

static inline size_t align_up(size_t v, size_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static inline void cnt_add(_Atomic size_t *c, size_t v)
{
	atomic_store_explicit(c, atomic_load_explicit(c, memory_order_relaxed) + v,
			      memory_order_relaxed);
}

static inline void cnt_sub(_Atomic size_t *c, size_t v)
{
	atomic_store_explicit(c, atomic_load_explicit(c, memory_order_relaxed) - v,
			      memory_order_relaxed);
}

static inline size_t cnt_get(const _Atomic size_t *c)
{
	return atomic_load_explicit(c, memory_order_relaxed);
}

/* --- block header accessors ------------------------------------------- */
/*
 * All of these require exclusive access to the containing arena (owner, or
 * lock held), with three deliberate exceptions that make cross-thread free
 * work:
 *   - b->arena is immutable once the block is carved;
 *   - b->units is never written while the block is in use, so the thread
 *     holding the block may read its own size before acquiring anything;
 *   - b->flags is atomic, because the owner of a neighbouring block flips
 *     BLOCK_PREV_FREE in it while its holder reads the tag out of it.
 */

static inline size_t blk_size(const struct block_meta *b)
{
	return (size_t)b->units * BLOCK_ALIGN;
}

static inline unsigned int blk_flags(const struct block_meta *b)
{
	return atomic_load_explicit(&b->flags, memory_order_relaxed) & BLOCK_FLAG_BITS;
}

static inline void blk_set(struct block_meta *b, size_t size, unsigned int flags)
{
	b->units = (uint32_t)(size / BLOCK_ALIGN);
	atomic_store_explicit(&b->flags, BLOCK_TAG | flags, memory_order_relaxed);
}

/*
 * Does this look like a header this allocator wrote?  This is the one field a
 * thread may read before it has acquired anything -- including on a pointer
 * that turns out not to be ours at all -- so it has to be the atomic one.
 */
static inline int blk_tagged(const struct block_meta *b)
{
	return (atomic_load_explicit(&b->flags, memory_order_relaxed) &
		~BLOCK_FLAG_BITS) == BLOCK_TAG;
}

/*
 * Flipping a flag in the header of a block that may be in use by another
 * thread: a single relaxed read-modify-write, so the thread reading the same
 * word for its tag never sees a torn value.
 */
static inline void blk_flag_set(struct block_meta *b, unsigned int flag)
{
	atomic_fetch_or_explicit(&b->flags, flag, memory_order_relaxed);
}

static inline void blk_flag_clear(struct block_meta *b, unsigned int flag)
{
	atomic_fetch_and_explicit(&b->flags, ~flag, memory_order_relaxed);
}

static inline void *blk_payload(struct block_meta *b)
{
	return (char *)b + BLOCK_META_SIZE;
}

static inline struct block_meta *blk_of(void *payload)
{
	return (struct block_meta *)((char *)payload - BLOCK_META_SIZE);
}

static inline struct free_node *blk_node(struct block_meta *b)
{
	return (struct free_node *)blk_payload(b);
}

/* Boundary tag at the end of a free block, used for backward coalescing. */
static inline void blk_put_footer(struct block_meta *b, size_t size)
{
	*(uint32_t *)((char *)b + size - BLOCK_FOOTER_SIZE) =
		(uint32_t)(size / BLOCK_ALIGN);
}

static inline size_t blk_prev_footer(struct block_meta *b)
{
	return (size_t)(*(uint32_t *)((char *)b - BLOCK_FOOTER_SIZE)) * BLOCK_ALIGN;
}

/* Free-list links are arena-relative offsets; 0 means "none". */
static inline uint32_t blk_off(struct arena *a, struct block_meta *b)
{
	return b ? (uint32_t)((char *)b - (char *)a) : 0;
}

static inline struct block_meta *blk_from_off(struct arena *a, uint32_t off)
{
	return off ? (struct block_meta *)((char *)a + off) : NULL;
}

/* Link used while a block waits on the lock-free remote stack. */
static inline struct block_meta **blk_remote_next(struct block_meta *b)
{
	return (struct block_meta **)blk_payload(b);
}

/* Physically next block inside the arena, or NULL at the bump frontier. */
static inline struct block_meta *blk_next(struct arena *a, struct block_meta *b, size_t size)
{
	char *n = (char *)b + size;

	return n < a->bump ? (struct block_meta *)n : NULL;
}

/* ----------------------------------------------------------- bootstrapping */

static inline void osmem_boot(void)
{
	/* pthread_once() publishes the configuration; the flag only lets the
	 * steady state skip the call.
	 */
	if (__builtin_expect(!atomic_load_explicit(&g_ready, memory_order_acquire), 0))
		pthread_once(&g_once, osmem_init);
}

static inline unsigned long long self_id(void)
{
	if (!tl_tid)
		tl_tid = atomic_fetch_add_explicit(&g_next_tid, 1, memory_order_relaxed);
	return tl_tid;
}

/*
 * Make sure tls_destructor() runs for this thread when it exits.  The key may
 * not exist yet if an allocation re-entered initialisation; in that case we
 * stay unregistered and the next allocation tries again.
 */
static inline void tls_register(void)
{
	if (!tl_registered &&
	    atomic_load_explicit(&g_key_ready, memory_order_acquire)) {
		tl_registered = 1;
		pthread_setspecific(g_tls_key, (void *)1);
	}
}

static inline int arena_count(void)
{
	return atomic_load_explicit(&g_arena_count, memory_order_acquire);
}

static inline struct arena *arena_at(int i)
{
	return atomic_load_explicit(&g_arenas[i], memory_order_acquire);
}

/* -------------------------------------------------------- free-list bins */

static inline int bin_index(size_t size)
{
	int k, i;

	if (size < SMALL_BIN_LIMIT)
		return (int)(size / BLOCK_ALIGN);

	k = 63 - __builtin_clzll((unsigned long long)size);
	i = 54 + k;		/* size in [1Ki, 2Ki) -> 64, [2Ki, 4Ki) -> 65, ... */
	return i < NBINS ? i : NBINS - 1;
}

#define BIN_WORDS	((int)(sizeof(((struct arena *)0)->bin_map) / sizeof(uint64_t)))

static inline uint64_t bin_word(struct arena *a, int w)
{
	return atomic_load_explicit(&a->bin_map[w], memory_order_relaxed);
}

static inline void bin_word_set(struct arena *a, int i)
{
	int w = i >> 6;

	atomic_store_explicit(&a->bin_map[w], bin_word(a, w) | (1ULL << (i & 63)),
			      memory_order_relaxed);
}

static inline void bin_word_clear(struct arena *a, int i)
{
	int w = i >> 6;

	atomic_store_explicit(&a->bin_map[w], bin_word(a, w) & ~(1ULL << (i & 63)),
			      memory_order_relaxed);
}

/* Smallest block size that lands in bin i. */
static inline size_t bin_floor(int i)
{
	return i < SMALL_BINS ? (size_t)i * BLOCK_ALIGN : (size_t)1 << (i - 54);
}

/* ------------------------------------------------------------- ownership */

static inline void arena_release_acquired(struct arena *a, enum acquire_mode mode)
{
	if (mode == ACQ_LOCKED)
		pthread_mutex_unlock(&a->lock);
}
