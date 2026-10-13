// SPDX-License-Identifier: BSD-3-Clause
/*
 * The shared regime: with the arena budget deliberately set to 3, many
 * more threads than arenas have to share.  Checks that the shared regime
 * engages, that the number of mappings the kernel sees stays inside A_max,
 * and that allocation never fails while the kernel still has memory.
 *
 * The allocator configures itself lazily on the first request, so setting the
 * environment at the top of main() is enough to pick the budget.
 */

#include <pthread.h>
#include <unistd.h>

#include "test_common.h"

#define ARENA_MAX 3
#define ARENA_SIZE (1024 * 1024)
#define MANY_THREADS 64

static int worker_errors;
static pthread_barrier_t barrier;

static void test_config(void)
{
	struct osmem_stats st;

	t_begin("the configured arena budget is in force");
	osmem_stats(&st);
	CHECKF(st.arena_max == ARENA_MAX, "A_max is %zu, expected %d", st.arena_max,
	       ARENA_MAX);
	CHECKF(st.arena_size == ARENA_SIZE, "arena size is %zu, expected %d",
	       st.arena_size, ARENA_SIZE);
	CHECKF(st.arena_count == 0, "%zu arenas exist before the first allocation",
	       st.arena_count);
}

/* ------------------------------------------------- many threads, 3 arenas */

static void *crowd_worker(void *arg)
{
	int id = (int)(long)arg;
	void *p[32];
	size_t size[32];
	struct rng rng;
	int i, round;

	rng_seed(&rng, 7000 + id);
	pthread_barrier_wait(&barrier);		/* maximise overlap */

	for (round = 0; round < 40; round++) {
		for (i = 0; i < 32; i++) {
			size[i] = rng_size(&rng, 8, 600);
			p[i] = os_malloc(size[i]);
			if (!p[i]) {
				worker_errors++;
				size[i] = 0;
				continue;
			}
			pattern_fill(p[i], size[i], (uint64_t)(id * 100 + i) + 1);
		}
		for (i = 0; i < 32; i++) {
			if (!p[i])
				continue;
			if (!pattern_ok(p[i], size[i], (uint64_t)(id * 100 + i) + 1))
				worker_errors++;
			os_free(p[i]);
		}
	}
	return NULL;
}

static void test_thread_explosion(void)
{
	pthread_t th[MANY_THREADS];
	struct osmem_stats st;
	const void *handle[ARENA_MAX + 4];
	unsigned long long distinct[ARENA_MAX + 4];
	int vmas_before, vmas_after;
	int i, j, n, ndistinct = 0;

	t_begin("64 threads share 3 arenas");
	vmas_before = count_vmas();
	pthread_barrier_init(&barrier, NULL, MANY_THREADS);
	for (i = 0; i < MANY_THREADS; i++)
		CHECK(pthread_create(&th[i], NULL, crowd_worker, (void *)(long)i) == 0);
	for (i = 0; i < MANY_THREADS; i++)
		pthread_join(th[i], NULL);
	pthread_barrier_destroy(&barrier);
	vmas_after = count_vmas();

	CHECKF(worker_errors == 0, "%d errors reported by the workers", worker_errors);
	osmem_stats(&st);
	CHECKF(st.arena_count <= ARENA_MAX, "%zu arenas created, A_max is %d",
	       st.arena_count, ARENA_MAX);
	CHECKF(st.ffa_engaged == 1, "the shared regime never engaged with %d threads",
	       MANY_THREADS);
	CHECKF(st.ffa_allocs > 0, "no allocation went through the shared path");
	CHECKF(st.arena_shared >= 1, "no arena ended up in the shared pool");
	CHECKF(st.standalone_blocks == 0, "%zu standalone blocks leaked",
	       st.standalone_blocks);
	CHECKF(st.bytes_mapped <= (size_t)ARENA_MAX * ARENA_SIZE,
	       "%zu bytes mapped for %d arenas of %d bytes", st.bytes_mapped,
	       ARENA_MAX, ARENA_SIZE);

	/* Every arena really is one anonymous read/write mapping,
	 * and the whole process never holds more than A_max of them.  (The raw
	 * mapping count also moves with pthread stacks, which glibc caches, and
	 * the kernel merges adjacent anonymous mappings, so the arenas are
	 * checked against their own addresses.)
	 */
	n = osmem_arena_list(handle, ARENA_MAX + 4);
	CHECKF(n == (int)st.arena_count, "listed %d arenas, stats report %zu", n,
	       st.arena_count);
	CHECKF(n <= ARENA_MAX, "%d arenas exist, A_max is %d", n, ARENA_MAX);
	for (i = 0; i < n; i++) {
		struct vma v;

		if (!CHECK_VMA(handle[i], &v))
			continue;
		CHECKF(v.hi - (unsigned long long)(uintptr_t)handle[i] >= ARENA_SIZE,
		       "arena %d is not fully inside its mapping", i);
		CHECKF(strncmp(v.perm, "rw-p", 4) == 0,
		       "arena %d mapping has permissions %s", i, v.perm);
		CHECKF(v.anonymous, "arena %d is not an anonymous mapping", i);
		for (j = 0; j < ndistinct; j++)
			if (distinct[j] == v.lo)
				break;
		if (j == ndistinct)
			distinct[ndistinct++] = v.lo;
	}
	CHECKF(ndistinct <= ARENA_MAX, "%d distinct mappings hold %d arenas", ndistinct, n);
	CHECKF(vmas_after >= vmas_before, "mapping count went backwards (%d -> %d)",
	       vmas_before, vmas_after);
	CHECK_SOUND();
	osmem_drain_all();
	osmem_stats(&st);
	CHECK_STATF(st.bytes_used == 0, "%zu bytes still in use", st.bytes_used);
	printf("      (%zu arenas: %zu private, %zu orphan, %zu shared; %zu shared allocations)\n",
	       st.arena_count, st.arena_exclusive, st.arena_orphan, st.arena_shared,
	       st.ffa_allocs);
	CHECK_SOUND();
}

/* ------------------------- the pool releases an arena once the load is gone */

static struct {
	const void *arena;
	int state;
	unsigned long long owner;
	unsigned long long tid;
} solo;

static void *solo_worker(void *arg)
{
	void *p = os_malloc(128);

	(void)arg;
	solo.arena = osmem_thread_arena();
	solo.tid = osmem_thread_id();
	solo.state = solo.arena ? osmem_arena_state(solo.arena) : -1;
	solo.owner = solo.arena ? osmem_arena_owner(solo.arena) : 0;
	os_free(p);
	return NULL;
}

/*
 * Once the storm is over every arena is idle, either orphaned or still in the
 * shared pool.  A thread arriving now must get a private, lock-free arena
 * again -- by adopting an orphan (arena_adopt) or by pulling an idle one out of
 * the pool (arena_reclaim) -- and it must not cost a new mapping.  Which of
 * the two routes a given run takes depends on what the storm left behind, so
 * the test pins down the outcome and the fact that exactly one arena left the
 * free pool, not the route.
 */
static void test_accordion(void)
{
	struct osmem_stats before, after;
	size_t adopted = 0, reclaimed = 0;
	pthread_t th;
	int round;

	t_begin("an idle arena goes back to private, lock-free use");
	osmem_drain_all();
	osmem_stats(&before);
	CHECK_STATF(before.bytes_used == 0, "%zu bytes still in use before the test",
	       before.bytes_used);
	CHECKF(before.arena_orphan + before.arena_shared == before.arena_count,
	       "%zu of %zu arenas are still owned by a live thread",
	       before.arena_count - before.arena_orphan - before.arena_shared,
	       before.arena_count);

	for (round = 0; round < 8; round++) {
		struct osmem_stats a, b;

		osmem_stats(&a);
		memset(&solo, 0, sizeof(solo));
		CHECK(pthread_create(&th, NULL, solo_worker, NULL) == 0);
		pthread_join(th, NULL);
		osmem_stats(&b);

		CHECKF(solo.arena != NULL,
		       "round %d: the thread got no private arena, so it was stuck "
		       "in the shared regime", round);
		CHECKF(solo.state == OSMEM_ARENA_EXCLUSIVE,
		       "round %d: the arena is in state %d, not exclusive", round,
		       solo.state);
		CHECKF(solo.owner == solo.tid,
		       "round %d: arena owner %llu, thread %llu", round, solo.owner,
		       solo.tid);
		CHECKF(b.arena_count == a.arena_count,
		       "round %d: going private again cost %zu new arenas", round,
		       b.arena_count - a.arena_count);
		CHECKF(b.adoptions + b.reclaims == a.adoptions + a.reclaims + 1,
		       "round %d: %zu arenas left the free pool instead of 1", round,
		       b.adoptions + b.reclaims - a.adoptions - a.reclaims);
		adopted += b.adoptions - a.adoptions;
		reclaimed += b.reclaims - a.reclaims;
		CHECK_SOUND();
	}

	osmem_stats(&after);
	CHECKF(after.mmap_calls == before.mmap_calls,
	       "8 rounds cost %zu mmap calls", after.mmap_calls - before.mmap_calls);
	CHECKF(after.arena_orphan >= 1, "no arena was left adoptable");
	printf("      (%zu rounds adopted an orphan, %zu reclaimed from the pool)\n",
	       adopted, reclaimed);
}

/* ------------------------------------- running out of arenas entirely */

#define BIG_BLOCKS 96
#define BIG_SIZE (64 * 1024)

static void test_exhaustion(void)
{
	static void *p[BIG_BLOCKS];
	struct osmem_stats st;
	int i, failures = 0;

	t_begin("when every arena is full, allocation still succeeds");
	/* 96 x 64 KiB = 6 MiB, twice what the three 1 MiB arenas can hold. */
	for (i = 0; i < BIG_BLOCKS; i++) {
		p[i] = os_malloc(BIG_SIZE);
		if (!p[i]) {
			failures++;
			continue;
		}
		pattern_fill(p[i], BIG_SIZE, (uint64_t)i + 1);
	}
	CHECKF(failures == 0, "%d of %d allocations failed although memory was available",
	       failures, BIG_BLOCKS);
	for (i = 0; i < BIG_BLOCKS; i++)
		if (p[i])
			CHECKF(pattern_ok(p[i], BIG_SIZE, (uint64_t)i + 1),
			       "block %d corrupted under memory pressure", i);

	osmem_stats(&st);
	CHECKF(st.arena_count <= ARENA_MAX, "%zu arenas created under pressure",
	       st.arena_count);
	CHECKF(st.standalone_blocks > 0,
	       "the pressure valve never triggered, so it was not tested");
	CHECK_SOUND();
	printf("      (%zu blocks inside arenas, %zu blocks with a private mapping)\n",
	       st.blocks_used, st.standalone_blocks);

	for (i = 0; i < BIG_BLOCKS; i++)
		os_free(p[i]);
	osmem_stats(&st);
	CHECKF(st.standalone_blocks == 0, "%zu standalone blocks survived the teardown",
	       st.standalone_blocks);
	CHECK_STATF(st.bytes_used == 0, "%zu bytes still in use", st.bytes_used);
	CHECKF(st.bytes_mapped <= (size_t)ARENA_MAX * ARENA_SIZE,
	       "%zu bytes still mapped after the teardown", st.bytes_mapped);
	CHECK_SOUND();
}

/* --------------------------- threads coming and going in the shared regime */

static void *churn_worker(void *arg)
{
	int id = (int)(long)arg;
	void *p[16];
	int i;

	for (i = 0; i < 16; i++) {
		p[i] = os_malloc(100 + i);
		if (!p[i])
			worker_errors++;
		else
			pattern_fill(p[i], 100 + i, (uint64_t)id + 1);
	}
	for (i = 0; i < 16; i++) {
		if (p[i] && !pattern_ok(p[i], 100 + i, (uint64_t)id + 1))
			worker_errors++;
		os_free(p[i]);
	}
	return NULL;
}

static void test_churn(void)
{
	struct osmem_stats before, after;
	pthread_t th[8];
	int round, i;

	t_begin("threads starting and finishing while the pool is shared");
	worker_errors = 0;
	osmem_stats(&before);
	for (round = 0; round < 20; round++) {
		for (i = 0; i < 8; i++)
			CHECK(pthread_create(&th[i], NULL, churn_worker, (void *)(long)i) == 0);
		for (i = 0; i < 8; i++)
			pthread_join(th[i], NULL);
	}
	osmem_stats(&after);
	CHECKF(worker_errors == 0, "%d errors during the churn", worker_errors);
	CHECKF(after.arena_count == before.arena_count,
	       "160 more threads created %zu extra arenas",
	       after.arena_count - before.arena_count);
	CHECKF(after.mmap_calls == before.mmap_calls,
	       "160 more threads cost %zu mmap calls", after.mmap_calls - before.mmap_calls);
	CHECK_SOUND();
}

int main(void)
{
	char max[16], size[32];

	snprintf(max, sizeof(max), "%d", ARENA_MAX);
	snprintf(size, sizeof(size), "%d", ARENA_SIZE);
	setenv("OSMEM_ARENA_MAX", max, 1);
	setenv("OSMEM_ARENA_SIZE", size, 1);

	printf("== test_ffa (A_max=%d, arena=%d KiB) ==\n", ARENA_MAX, ARENA_SIZE / 1024);
	test_config();
	test_thread_explosion();
	test_accordion();
	test_exhaustion();
	test_churn();
	return t_report("test_ffa");
}
