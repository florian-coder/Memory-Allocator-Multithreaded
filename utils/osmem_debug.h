/* SPDX-License-Identifier: BSD-3-Clause */

#pragma once

#include <stddef.h>

/*
 * Introspection API.
 *
 * None of this is needed to use the allocator; it exists so that the test
 * suite can assert *structural* properties (how many arenas exist, whether a
 * block was recycled instead of mmap()-ed, whether the arena cap is honoured)
 * instead of merely checking that malloc returned non-NULL.
 */

/* Ownership regime of an arena, mirrors enum arena_state in osmem_internal.h. */
#define OSMEM_ARENA_EXCLUSIVE	0	/* one thread owns it, lock-free path */
#define OSMEM_ARENA_ORPHAN	1	/* no owner, adoptable                */
#define OSMEM_ARENA_SHARED	2	/* free-for-all pool, mutex protected */
#define OSMEM_ARENA_POISONED	3	/* inherited torn through fork(), never touched again */

struct osmem_stats {
	size_t arena_count;		/* arenas created so far              */
	size_t arena_max;		/* A_max: hard cap on arenas          */
	size_t arena_size;		/* bytes per arena mapping            */
	size_t arena_exclusive;		/* arenas privately owned right now   */
	size_t arena_orphan;		/* arenas waiting to be adopted       */
	size_t arena_shared;		/* arenas in the FFA pool             */
	size_t bytes_mapped;		/* total virtual memory from the OS   */
	size_t bytes_bump;		/* arena space handed to the bump ptr */
	size_t bytes_touched;		/* arena pages we have actually faulted */
	size_t bytes_released;		/* pages handed back with MADV_DONTNEED */
	size_t bytes_used;		/* live blocks inside arenas          */
	size_t bytes_free;		/* bytes sitting in arena free lists  */
	size_t blocks_used;		/* live blocks inside arenas          */
	size_t blocks_free;
	size_t largest_free;		/* size class of the biggest reusable
					 * hole: exact below 1 KiB, a lower
					 * bound above                        */
	size_t standalone_blocks;	/* live blocks with a private mmap    */
	size_t standalone_bytes;
	size_t mmap_calls;		/* syscall counters                   */
	size_t munmap_calls;
	size_t remote_frees;		/* cross-thread frees deferred to the owner */
	size_t ffa_allocs;		/* allocations served by the FFA path */
	size_t adoptions;		/* orphaned arenas taken over (Phase 2B) */
	size_t reclaims;		/* arenas pulled back out of the FFA pool */
	size_t arena_poisoned;		/* arenas quarantined after a fork()  */
	size_t map_denied;		/* mappings refused (real or injected) */
	size_t foreign_ptrs;		/* pointers rejected by the header tag */
	int has_world_lock;		/* built with -DOSMEM_WORLD_LOCK       */
	int has_chaos;			/* built with -DOSMEM_CHAOS           */
	int has_quarantine;		/* built with -DOSMEM_QUARANTINE      */
	size_t bytes_quarantined;	/* freed but not yet coalesced        */
	int has_stats;			/* 0 if built with -DOSMEM_NO_STATS:
					 * bytes_used, blocks_* and
					 * bytes_touched then read 0          */
	int ffa_engaged;		/* 1 once the shared regime kicked in */
};

/* Snapshot of the allocator state.  Lock-free, so only loosely consistent
 * while other threads are allocating.
 */
void osmem_stats(struct osmem_stats *out);

/*
 * Full consistency check of every arena: block chain, boundary tags,
 * PREV_FREE bits, free-list/bin membership, bitmap and byte counters.
 * Returns the number of violations found (0 == healthy) and describes the
 * first few on stderr.  Must be called while no other thread is inside the
 * allocator.
 */
int osmem_check(void);

/*
 * Make the next `n` mappings succeed and every one after that fail, so the
 * out-of-memory paths can be tested.  A negative value restores the default
 * (no limit).  Also settable at start-up with OSMEM_FAIL_MMAP_AFTER.
 */
void osmem_fail_mmap_after(long n);

/*
 * Push every pending cross-thread free into the free list of its arena.
 * Only arenas without a live owner (orphaned or shared) and the arenas owned
 * by the calling thread can be drained; the rest are left to their owner.
 * Test-only helper used to make deferred reclamation observable.
 */
void osmem_drain_all(void);

/* Handles of all arenas, oldest first; returns how many were stored.  A handle
 * is the base address of the arena's mapping.
 */
int osmem_arena_list(const void **out, int max);

/* Opaque handle of the arena the calling thread currently allocates from,
 * NULL if it has none (it allocates from the shared pool, or not at all).
 */
const void *osmem_thread_arena(void);

/* Allocator-internal id of the calling thread (0 if it never allocated). */
unsigned long long osmem_thread_id(void);

/* State (OSMEM_ARENA_*) and owner id of an arena handle. */
int osmem_arena_state(const void *arena);
unsigned long long osmem_arena_owner(const void *arena);
