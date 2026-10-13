// SPDX-License-Identifier: BSD-3-Clause
/*
 * Walks the arena state machine deterministically, to cover the one transition
 * that the random thread storm in test_ffa only reaches by luck: an arena
 * leaving the shared pool and going back to private, lock-free use.
 *
 * With A_max = 4 the budget is: 2 private arenas, 2 reserved for the pool.
 * The test drives exactly this sequence:
 *
 *   1. a worker takes private arena #0 and stays alive, holding it;
 *   2. main takes private arena #1 and stays alive, holding it
 *      -- the private budget is now spent;
 *   3. a second worker, which therefore cannot get a private arena, fills the
 *      shared pool until it holds two arenas (#2 and #3), then frees
 *      everything and exits without ever owning an arena;
 *   4. a third worker arrives.  There is no orphan to adopt (both private
 *      arenas still have live owners) and no budget left to create one, so the
 *      only way for it to get a private arena is to reclaim an idle one out of
 *      the pool.
 */

#include <pthread.h>

#include "test_common.h"

#define ARENA_MAX 4
#define ARENA_SIZE (1024 * 1024)
#define FILL_BLOCK (64 * 1024)
#define FILL_MAX 64

static pthread_barrier_t hold;
static int worker_errors;

struct view {
	const void *arena;
	int state;
	unsigned long long owner;
	unsigned long long tid;
};

/* Step 1: take a private arena and keep it until the test is over. */
static void *holder(void *arg)
{
	struct view *v = arg;
	void *p = os_malloc(256);

	if (!p)
		worker_errors++;
	v->arena = osmem_thread_arena();
	v->tid = osmem_thread_id();
	v->state = v->arena ? osmem_arena_state(v->arena) : -1;
	v->owner = v->arena ? osmem_arena_owner(v->arena) : 0;

	pthread_barrier_wait(&hold);	/* stay alive while the rest runs */
	pthread_barrier_wait(&hold);
	os_free(p);
	return NULL;
}

/* Step 3: grow the shared pool to two arenas, then empty it again. */
static void *pool_filler(void *arg)
{
	static void *block[FILL_MAX];
	struct osmem_stats st;
	int i, n = 0;

	(void)arg;
	for (i = 0; i < FILL_MAX; i++) {
		osmem_stats(&st);
		if (st.arena_shared >= 2)
			break;
		block[n] = os_malloc(FILL_BLOCK);
		if (!block[n]) {
			worker_errors++;
			break;
		}
		pattern_fill(block[n], FILL_BLOCK, (uint64_t)n + 1);
		n++;
	}

	osmem_stats(&st);
	if (st.arena_shared < 2)
		worker_errors++;
	if (osmem_thread_arena() != NULL)
		worker_errors++;		/* it must never own an arena */

	for (i = 0; i < n; i++) {
		if (!pattern_ok(block[i], FILL_BLOCK, (uint64_t)i + 1))
			worker_errors++;
		os_free(block[i]);
	}
	return NULL;
}

/* Step 4: the thread that can only succeed by reclaiming from the pool. */
static struct view late;

static void *late_worker(void *arg)
{
	void *p = os_malloc(128);

	(void)arg;
	if (!p)
		worker_errors++;
	late.arena = osmem_thread_arena();
	late.tid = osmem_thread_id();
	late.state = late.arena ? osmem_arena_state(late.arena) : -1;
	late.owner = late.arena ? osmem_arena_owner(late.arena) : 0;
	pattern_fill(p, 128, 5);
	if (!pattern_ok(p, 128, 5))
		worker_errors++;
	os_free(p);
	return NULL;
}

int main(void)
{
	struct osmem_stats before, mid, after;
	struct view first = { 0 };
	pthread_t keeper, filler, late_th;
	void *mine;
	char buf[16];

	snprintf(buf, sizeof(buf), "%d", ARENA_MAX);
	setenv("OSMEM_ARENA_MAX", buf, 1);
	snprintf(buf, sizeof(buf), "%d", ARENA_SIZE);
	setenv("OSMEM_ARENA_SIZE", buf, 1);

	printf("== test_reclaim (A_max=%d: 2 private + 2 pool slots) ==\n", ARENA_MAX);

	t_begin("the private budget leaves two slots for the shared pool");
	pthread_barrier_init(&hold, NULL, 2);
	CHECK(pthread_create(&keeper, NULL, holder, &first) == 0);
	pthread_barrier_wait(&hold);		/* keeper now holds arena #0 */

	CHECKF(first.arena != NULL, "the first thread got no private arena");
	CHECKF(first.state == OSMEM_ARENA_EXCLUSIVE, "arena #0 is in state %d",
	       first.state);

	mine = os_malloc(256);			/* main takes the second private arena */
	CHECK(mine != NULL);
	CHECKF(osmem_thread_arena() != NULL, "main got no private arena");
	CHECKF(osmem_thread_arena() != first.arena,
	       "main and the first thread share an arena while both are alive");
	osmem_stats(&before);
	CHECKF(before.arena_count == 2, "%zu arenas after two private ones",
	       before.arena_count);
	CHECKF(before.arena_exclusive == 2, "%zu private arenas, expected 2",
	       before.arena_exclusive);
	CHECK_SOUND();

	t_begin("a thread beyond the private budget grows the shared pool");
	CHECK(pthread_create(&filler, NULL, pool_filler, NULL) == 0);
	pthread_join(filler, NULL);
	osmem_stats(&mid);
	CHECKF(worker_errors == 0, "%d errors while growing the pool", worker_errors);
	CHECKF(mid.arena_count == ARENA_MAX, "the pool grew to %zu arenas, expected %d",
	       mid.arena_count, ARENA_MAX);
	CHECKF(mid.arena_shared == 2, "%zu arenas in the pool, expected 2",
	       mid.arena_shared);
	CHECKF(mid.arena_orphan == 0, "%zu orphaned arenas, expected none",
	       mid.arena_orphan);
	CHECKF(mid.ffa_engaged == 1, "the shared regime did not engage");
	CHECK_SOUND();

	t_begin("the next thread can only get a private arena by reclaiming one");
	CHECK(pthread_create(&late_th, NULL, late_worker, NULL) == 0);
	pthread_join(late_th, NULL);
	osmem_stats(&after);

	CHECKF(worker_errors == 0, "%d errors in the late worker", worker_errors);
	CHECKF(late.arena != NULL,
	       "the late thread stayed in the shared regime instead of reclaiming");
	CHECKF(late.state == OSMEM_ARENA_EXCLUSIVE,
	       "the reclaimed arena is in state %d, not exclusive", late.state);
	CHECKF(late.owner == late.tid, "reclaimed arena owner %llu, thread %llu",
	       late.owner, late.tid);
	CHECKF(late.arena != first.arena && late.arena != osmem_thread_arena(),
	       "the late thread took an arena that was still owned");
	CHECKF(after.reclaims == mid.reclaims + 1,
	       "%zu arenas were reclaimed, expected exactly 1",
	       after.reclaims - mid.reclaims);
	CHECKF(after.adoptions == mid.adoptions,
	       "%zu arenas were adopted, expected none to be available",
	       after.adoptions - mid.adoptions);
	CHECKF(after.arena_count == mid.arena_count,
	       "reclaiming cost %zu new arenas", after.arena_count - mid.arena_count);
	CHECKF(after.mmap_calls == mid.mmap_calls, "reclaiming cost %zu mmap calls",
	       after.mmap_calls - mid.mmap_calls);
	CHECKF(after.arena_shared == 1, "%zu arenas left in the pool, expected 1",
	       after.arena_shared);
	CHECK_SOUND();

	pthread_barrier_wait(&hold);		/* release the keeper */
	pthread_join(keeper, NULL);
	pthread_barrier_destroy(&hold);
	os_free(mine);
	CHECK_SOUND();
	return t_report("test_reclaim");
}
