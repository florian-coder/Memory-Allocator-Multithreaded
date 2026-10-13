// SPDX-License-Identifier: BSD-3-Clause
/*
 * arena.c -- global allocator state and the life cycle of an arena.
 *
 * Start-up (lazy, so that a program which never allocates never maps a page),
 * per-thread identity, the table of arenas, and the four ownership
 * transitions: create, adopt an orphan (Phase 2B), reclaim one out of the
 * shared pool, and release on thread exit.  arena_acquire() at the bottom is
 * the single implementation of the concurrency contract in osmem_internal.h.
 */

#define _GNU_SOURCE

#include <sched.h>

#include "osmem_internal.h"

/* Shared with the other modules; see osmem_internal.h. */
_Atomic(struct arena *) g_arenas[ARENA_HARD_MAX];
_Atomic int g_arena_count;
_Atomic uint64_t g_orphan_mask[(ARENA_HARD_MAX + 63) / 64];
_Atomic uint64_t g_shared_mask[(ARENA_HARD_MAX + 63) / 64];

pthread_once_t g_once = PTHREAD_ONCE_INIT;
_Atomic int g_ready;
_Atomic int g_key_ready;
pthread_key_t g_tls_key;

size_t g_page = 4096;
size_t g_arena_size = ARENA_SIZE_DEFAULT;
size_t g_arena_payload;			/* carvable bytes per arena              */
size_t g_standalone_min;		/* above this, a block gets its own mmap */
int g_arena_max;

_Atomic unsigned long long g_next_tid = 1;
_Atomic int g_ffa;
_Atomic size_t g_mmap_calls;
_Atomic size_t g_munmap_calls;
_Atomic size_t g_bytes_mapped;
_Atomic size_t g_standalone_blocks;
_Atomic size_t g_standalone_bytes;
_Atomic size_t g_remote_frees;
_Atomic size_t g_ffa_allocs;
_Atomic size_t g_adoptions;
_Atomic size_t g_reclaims;
_Atomic size_t g_poisoned;
_Atomic long g_map_budget = LONG_MAX;
_Atomic size_t g_map_denied;
_Atomic size_t g_foreign_ptrs;
_Atomic size_t g_bytes_released;
size_t g_trim_threshold = 256 * 1024;
#ifdef OSMEM_WORLD_LOCK
pthread_rwlock_t g_world = PTHREAD_RWLOCK_INITIALIZER;
#endif

__thread struct arena *tl_arena;
__thread unsigned long long tl_tid;
__thread int tl_registered;

/* Private to this file. */
static pthread_mutex_t g_create_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------ kernel memory */

/*
 * The budget starts at LONG_MAX, so the check costs one relaxed decrement and
 * never trips in normal use; osmem_fail_mmap_after() lowers it so the tests
 * can walk the failure paths.
 */
OSMEM_INTERNAL void *osmem_map(size_t len)
{
	void *p;

	if (atomic_fetch_sub_explicit(&g_map_budget, 1, memory_order_relaxed) <= 0) {
		atomic_fetch_add_explicit(&g_map_denied, 1, memory_order_relaxed);
		errno = ENOMEM;
		return MAP_FAILED;
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		atomic_fetch_add_explicit(&g_map_denied, 1, memory_order_relaxed);
	return p;
}

OSMEM_INTERNAL void *osmem_remap(void *addr, size_t old_len, size_t new_len)
{
	void *p;

	if (new_len > old_len &&
	    atomic_fetch_sub_explicit(&g_map_budget, 1, memory_order_relaxed) <= 0) {
		atomic_fetch_add_explicit(&g_map_denied, 1, memory_order_relaxed);
		errno = ENOMEM;
		return MAP_FAILED;
	}
	p = mremap(addr, old_len, new_len, MREMAP_MAYMOVE);
	if (p == MAP_FAILED)
		atomic_fetch_add_explicit(&g_map_denied, 1, memory_order_relaxed);
	return p;
}

#ifdef OSMEM_WORLD_LOCK
OSMEM_INTERNAL void osmem_world_init(void)
{
	pthread_rwlockattr_t attr;

	/* Writer-preferring, or a steady stream of allocations would starve
	 * osmem_check() forever.
	 */
	pthread_rwlockattr_init(&attr);
	pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
	pthread_rwlock_init(&g_world, &attr);
	pthread_rwlockattr_destroy(&attr);
}
#endif

#ifdef OSMEM_CHAOS
/*
 * Deliberately perturb the interleaving.  Must not allocate, so it is a
 * thread-local xorshift plus sched_yield() or a short spin.
 */
static unsigned g_chaos_rate = 4;

OSMEM_INTERNAL void osmem_chaos_point(void)
{
	static __thread uint64_t seed;
	uint64_t x;

	x = seed;
	if (!x)
		x = 0x9e3779b97f4a7c15ULL ^ (uint64_t)(uintptr_t)&seed;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	seed = x;

	switch ((unsigned)(x % g_chaos_rate)) {
	case 0:
		sched_yield();
		break;
	case 1: {
		volatile int i;

		for (i = 0; i < 256; i++)
			;
		break;
	}
	default:
		break;
	}
}
#endif

static void tls_destructor(void *unused);
static void atfork_prepare(void);
static void atfork_parent(void);
static void atfork_child(void);

/*
 * Number of CPUs we are allowed to run on, without calling anything that might
 * allocate.  sysconf(_SC_NPROCESSORS_ONLN) can fall back to reading /sys
 * through stdio, which allocates -- harmless normally, but fatal when this
 * allocator is interposed over libc's malloc, because that allocation would
 * re-enter initialisation and deadlock on a non-reentrant pthread_once().
 * sched_getaffinity() works on a fixed-size bitmap instead.
 */
static int cpu_count(void)
{
	cpu_set_t set;

	if (sched_getaffinity(0, sizeof(set), &set) == 0) {
		int n = CPU_COUNT(&set);

		if (n > 0)
			return n;
	}
	return 1;
}

OSMEM_INTERNAL void osmem_init(void)
{
	long page = sysconf(_SC_PAGESIZE);	/* from auxv, never allocates */
	const char *env;
	size_t size;
	int amax;

	if (page > 0)
		g_page = (size_t)page;

	/* A_max: the kernel-facing budget from Phase 1.4. */
	amax = 8 * cpu_count();
	if (amax > ARENA_MAX_DEFAULT_CAP)
		amax = ARENA_MAX_DEFAULT_CAP;
	env = getenv("OSMEM_ARENA_MAX");
	if (env && *env)
		amax = atoi(env);
	if (amax < 2)
		amax = 2;		/* >= 1 private + >= 1 shareable arena */
	if (amax > ARENA_HARD_MAX)
		amax = ARENA_HARD_MAX;
	g_arena_max = amax;

	size = ARENA_SIZE_DEFAULT;
	env = getenv("OSMEM_ARENA_SIZE");
	if (env && *env) {
		long v = atol(env);

		if (v > 0)
			size = (size_t)v;
	}
	if (size < ARENA_SIZE_MIN)
		size = ARENA_SIZE_MIN;
	g_arena_size = align_up(size, g_page);
	/*
	 * The carvable region starts on a page boundary, not just on a 16-byte
	 * one.  It costs under a page of address space per arena and it is what
	 * makes trimming exact: MADV_DONTNEED works on whole pages, so a region
	 * that does not start on a page boundary can never be fully released,
	 * and the "untouched memory reads as zero" invariant behind os_calloc()
	 * would not survive a trim.
	 */
	g_arena_payload = g_arena_size - align_up(sizeof(struct arena), g_page);

	/* Requests that would eat half an arena get their own mapping instead:
	 * they would fragment an arena beyond repair, and they can be returned
	 * to the kernel immediately on free.
	 */
	g_standalone_min = g_arena_payload / 2;

	env = getenv("OSMEM_TRIM_THRESHOLD");
	if (env && *env) {
		long v = atol(env);

		if (v >= 0)
			g_trim_threshold = (size_t)v;
	}

	env = getenv("OSMEM_FAIL_MMAP_AFTER");
	if (env && *env)
		atomic_store_explicit(&g_map_budget, atol(env), memory_order_relaxed);
#ifdef OSMEM_CHAOS
	env = getenv("OSMEM_CHAOS_RATE");
	if (env && *env) {
		long v = atol(env);

		if (v >= 2)
			g_chaos_rate = (unsigned)v;
	}
#endif
#ifdef OSMEM_WORLD_LOCK
	osmem_world_init();
#endif

	/*
	 * Publish the configuration *before* calling into pthread.  Both calls
	 * below may allocate inside libc, and when this allocator is the
	 * process allocator that allocation lands back here: it must find the
	 * configuration finished and skip pthread_once(), which is not
	 * reentrant and would otherwise deadlock against this very call.
	 */
	atomic_store_explicit(&g_ready, 1, memory_order_release);

	pthread_key_create(&g_tls_key, tls_destructor);
	atomic_store_explicit(&g_key_ready, 1, memory_order_release);
	pthread_atfork(atfork_prepare, atfork_parent, atfork_child);
}

/* ------------------------------------------------------------------- fork */

/*
 * fork() keeps only the calling thread, but the mutexes of every arena stay
 * exactly as they were -- including the ones another thread was holding at the
 * moment of the fork.  In the child those are locked forever, so the first
 * allocation that needs one hangs.  The classic fix: take every lock before
 * the fork so the child inherits a quiescent allocator, then in the child
 * reset the locks and orphan the arenas whose owners no longer exist.
 *
 * Lock order is the same as everywhere else (g_create_lock, then arena locks
 * by index), so this cannot deadlock against a running thread.
 */
static void atfork_prepare(void)
{
	int n, i;

	/* With the world lock compiled in, this also waits for the lock-free
	 * owners, so the child inherits a fully consistent allocator and
	 * nothing has to be quarantined.
	 */
	WORLD_STOP();
	pthread_mutex_lock(&g_create_lock);
	n = atomic_load_explicit(&g_arena_count, memory_order_acquire);
	for (i = 0; i < n; i++) {
		struct arena *a = arena_at(i);

		if (a)
			pthread_mutex_lock(&a->lock);
	}
}

static void atfork_parent(void)
{
	int n = atomic_load_explicit(&g_arena_count, memory_order_acquire);
	int i;

	for (i = n - 1; i >= 0; i--) {
		struct arena *a = arena_at(i);

		if (a)
			pthread_mutex_unlock(&a->lock);
	}
	pthread_mutex_unlock(&g_create_lock);
	/* Unlock, never re-initialise: this process still has the threads that
	 * queued up behind the write lock, and re-initialising would zero the
	 * lock out from under them and leave them blocked for good.
	 */
	WORLD_GO();
}

static void atfork_child(void)
{
	unsigned long long me = tl_tid;
	int n = atomic_load_explicit(&g_arena_count, memory_order_acquire);
	int i;

	/*
	 * The thread that called fork() is the one that ran the prepare handler,
	 * so in this process it holds every lock: releasing them is both legal
	 * and better than re-initialising them, which would throw away the
	 * happens-before chain that tools like ThreadSanitizer follow.
	 *
	 * Note what this does *not* fix: holding an arena's lock never excluded
	 * its owner, because the private fast path is lock-free by design.  An
	 * arena owned by another thread may have been caught mid-update and is
	 * unrepairable, so it is quarantined rather than reused.
	 */
	for (i = 0; i < n; i++) {
		struct arena *a = arena_at(i);
		unsigned long long owner;

		if (!a)
			continue;
		pthread_mutex_unlock(&a->lock);

		owner = atomic_load_explicit(&a->owner, memory_order_relaxed);
		if (owner == 0 || owner == me)
			continue;		/* pool arena, or still ours: consistent */
		if (OSMEM_HAS_WORLD_LOCK) {
			/* Quiesced by the prepare handler: consistent, just ownerless. */
			atomic_store_explicit(&a->owner, 0, memory_order_relaxed);
			atomic_store_explicit(&a->state, ARENA_ORPHAN, memory_order_release);
			mask_set(g_orphan_mask, a->index);
			continue;
		}

		atomic_store_explicit(&a->owner, 0, memory_order_relaxed);
		atomic_store_explicit(&a->state, ARENA_POISONED, memory_order_release);
		mask_clear(g_orphan_mask, a->index);
		mask_clear(g_shared_mask, a->index);
		atomic_fetch_add_explicit(&g_poisoned, 1, memory_order_relaxed);
	}
	pthread_mutex_unlock(&g_create_lock);
	/*
	 * The arena mutexes could simply be unlocked above because glibc's
	 * default mutex does not record an owner.  A read/write lock does: it
	 * compares the writer's thread id, and this process got a new one from
	 * the fork, so unlocking it would fail with EPERM and leave the world
	 * stopped forever.  Re-initialising is safe here and only here: this
	 * process has exactly one thread, and the prepare handler guaranteed
	 * that nobody was inside.
	 */
	WORLD_REINIT();
}

/*
 * One mmap() per arena (Phase 1.3).  Private arenas may not spend the whole
 * A_max budget: two slots are kept in reserve for the shared pool, so that a
 * thread which cannot get a private arena always has somewhere to go
 * (Phase 2C) without having to steal an arena from a thread that is still
 * running, and so that the pool can grow to two arenas -- halving contention
 * in the shared regime -- without having to wait for an arena to be orphaned.
 */
OSMEM_INTERNAL struct arena *arena_create(unsigned long long owner, enum arena_state state)
{
	int budget = g_arena_max;

	if (state == ARENA_EXCLUSIVE)
		budget = g_arena_max > 2 ? g_arena_max - 2 : 1;
	struct arena *a;
	void *map;
	int n;

	pthread_mutex_lock(&g_create_lock);
	n = atomic_load_explicit(&g_arena_count, memory_order_relaxed);
	/* Arenas quarantined by a fork() are not usable capacity, so they do not
	 * spend the budget; the table is still hard-bounded.
	 */
	if (n - (int)cnt_get(&g_poisoned) >= budget || n >= ARENA_HARD_MAX) {
		pthread_mutex_unlock(&g_create_lock);
		return NULL;
	}

	map = osmem_map(g_arena_size);
	if (map == MAP_FAILED) {
		pthread_mutex_unlock(&g_create_lock);
		return NULL;
	}
	atomic_fetch_add_explicit(&g_mmap_calls, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_bytes_mapped, g_arena_size, memory_order_relaxed);

	a = map;
	/* The mapping is zero filled, so only the non-zero fields are set. */
	pthread_mutex_init(&a->lock, NULL);
	a->data_start = (char *)map + align_up(sizeof(struct arena), g_page);
	a->data_end = (char *)map + g_arena_size;
	a->bump = a->data_start;
	a->hwm = a->data_start;
	a->map_size = g_arena_size;
	a->index = n;
	atomic_store_explicit(&a->bump_room, (size_t)(a->data_end - a->bump),
			      memory_order_relaxed);
	atomic_store_explicit(&a->owner, owner, memory_order_relaxed);
	atomic_store_explicit(&a->state, state, memory_order_relaxed);

	if (state == ARENA_SHARED)
		mask_set(g_shared_mask, n);
	atomic_store_explicit(&g_arenas[n], a, memory_order_release);
	atomic_store_explicit(&g_arena_count, n + 1, memory_order_release);
	pthread_mutex_unlock(&g_create_lock);
	/* The arena is visible to everybody now, and its creator has not locked
	 * it yet: this is the window that let another thread reclaim it.
	 */
	CHAOS();
	return a;
}

/*
 * Phase 2B: take over an arena whose thread is gone.  Ownership is only taken
 * when the arena can actually serve the request, so a thread never ends up
 * owning an arena it cannot use.
 */
OSMEM_INTERNAL void *arena_adopt(unsigned long long me, size_t need, int *zeroed, struct arena **out)
{
	int n = arena_count();
	int i;

	for (i = 0; i < n; i++) {
		struct arena *a = arena_at(i);
		void *p;

		if (!a || atomic_load_explicit(&a->state, memory_order_acquire) != ARENA_ORPHAN)
			continue;

		CHAOS();
		pthread_mutex_lock(&a->lock);
		if (atomic_load_explicit(&a->state, memory_order_relaxed) != ARENA_ORPHAN) {
			pthread_mutex_unlock(&a->lock);
			continue;
		}
		p = arena_alloc(a, need, zeroed);
		if (!p) {
			pthread_mutex_unlock(&a->lock);
			continue;
		}
		atomic_store_explicit(&a->owner, me, memory_order_relaxed);
		atomic_store_explicit(&a->state, ARENA_EXCLUSIVE, memory_order_release);
		mask_clear(g_orphan_mask, a->index);
		pthread_mutex_unlock(&a->lock);
		atomic_fetch_add_explicit(&g_adoptions, 1, memory_order_relaxed);
		*out = a;
		return p;
	}
	return NULL;
}

/*
 * The accordion in reverse: once the thread spike is over, a completely empty
 * arena may leave the shared pool and go back to private, lock-free use.  This
 * is safe without any hand-over protocol because every other entry point
 * re-checks arena->owner under the lock (ffa_try(), arena_acquire(),
 * osmem_drain_all()) and backs off as soon as an owner appears.  One arena is
 * always left in the pool so that a thread which cannot get a private arena
 * still has somewhere to allocate from.
 */
OSMEM_INTERNAL void *arena_reclaim(unsigned long long me, size_t need, int *zeroed, struct arena **out)
{
	int i;

	/* Whether the pool can spare an arena is a decision about global state,
	 * so it is taken under the same lock that hands out arenas: two threads
	 * each reclaiming one of the last two shared arenas would otherwise
	 * leave the pool empty.
	 */
	pthread_mutex_lock(&g_create_lock);
	if (mask_count(g_shared_mask) < 2) {
		pthread_mutex_unlock(&g_create_lock);
		return NULL;
	}

	for (i = mask_next(g_shared_mask, 0); i >= 0; i = mask_next(g_shared_mask, i + 1)) {
		struct arena *a = arena_at(i);
		void *p;

		if (!a || atomic_load_explicit(&a->state, memory_order_acquire) != ARENA_SHARED)
			continue;
		/* Empty means the bump pointer is back where it started; that
		 * does not depend on any counter that can be compiled out.
		 */
		if (cnt_get(&a->bump_room) != g_arena_payload)
			continue;
		CHAOS();
		if (pthread_mutex_trylock(&a->lock) != 0)
			continue;
		/* Re-check everything now that nobody else can be inside. */
		if (atomic_load_explicit(&a->state, memory_order_relaxed) != ARENA_SHARED ||
		    cnt_get(&a->bump_room) != g_arena_payload ||
		    atomic_load_explicit(&a->remote, memory_order_relaxed)) {
			pthread_mutex_unlock(&a->lock);
			continue;
		}
		p = arena_alloc(a, need, zeroed);
		if (!p) {
			pthread_mutex_unlock(&a->lock);
			continue;
		}
		atomic_store_explicit(&a->owner, me, memory_order_relaxed);
		atomic_store_explicit(&a->state, ARENA_EXCLUSIVE, memory_order_release);
		mask_clear(g_shared_mask, a->index);
		pthread_mutex_unlock(&a->lock);
		pthread_mutex_unlock(&g_create_lock);
		atomic_fetch_add_explicit(&g_reclaims, 1, memory_order_relaxed);
		*out = a;
		return p;
	}
	pthread_mutex_unlock(&g_create_lock);
	return NULL;
}

/* Hand an arena back so that another thread can adopt it (thread exit). */
static void arena_release(struct arena *a)
{
	pthread_mutex_lock(&a->lock);
	remote_drain(a);
	arena_flush(a);		/* the next owner must see the real free space */
	atomic_store_explicit(&a->owner, 0, memory_order_relaxed);
	CHAOS();
	atomic_store_explicit(&a->state, ARENA_ORPHAN, memory_order_release);
	mask_set(g_orphan_mask, a->index);
	pthread_mutex_unlock(&a->lock);
}

static void tls_destructor(void *unused)
{
	unsigned long long me = tl_tid;
	int n = arena_count();
	int i;

	(void)unused;

	for (i = 0; i < n && me; i++) {
		struct arena *a = arena_at(i);

		if (a && atomic_load_explicit(&a->owner, memory_order_relaxed) == me)
			arena_release(a);
	}
	tl_arena = NULL;
	/* Allow re-registration in case a later destructor allocates again. */
	tl_registered = 0;
}

/*
 * Acquire the right to mutate an arena, as cheaply as the ownership regime
 * allows.  See the concurrency contract at the top of the file.
 */
OSMEM_INTERNAL enum acquire_mode arena_acquire(struct arena *a, unsigned long long me)
{
	if (atomic_load_explicit(&a->owner, memory_order_acquire) == me)
		return ACQ_OWNER;
	if (atomic_load_explicit(&a->state, memory_order_acquire) == ARENA_POISONED)
		return ACQ_POISONED;

	CHAOS();
	pthread_mutex_lock(&a->lock);
	/* Re-check under the lock: the arena may have been adopted meanwhile. */
	if (atomic_load_explicit(&a->owner, memory_order_relaxed) == 0) {
		remote_drain(a);
		return ACQ_LOCKED;
	}
	pthread_mutex_unlock(&a->lock);
	return ACQ_REMOTE;
}
