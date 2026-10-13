// SPDX-License-Identifier: BSD-3-Clause
/*
 * Phase 2 (private arenas, orphan adoption) and Phase 3.1-3.3 (a block freed
 * by a thread that did not allocate it).
 */

#include <pthread.h>
#include <unistd.h>

#include "test_common.h"

#define PRIVATE_THREADS 6
#define SEQ_THREADS 48
#define STRESS_THREADS 8

/* ----------------------------------------- Phase 2A: one arena per thread */

static pthread_barrier_t barrier;

struct private_result {
	const void *arena;
	unsigned long long owner;
	unsigned long long tid;
	int state;
	int ok;
};

static struct private_result results[PRIVATE_THREADS];

static void *private_worker(void *arg)
{
	struct private_result *r = arg;
	void *p[128];
	int i, round;

	for (round = 0; round < 50; round++) {
		for (i = 0; i < 128; i++) {
			p[i] = os_malloc(64 + i * 8);
			if (!p[i])
				return NULL;
			pattern_fill(p[i], 64 + i * 8, (uint64_t)i + 1);
		}
		for (i = 0; i < 128; i++) {
			if (!pattern_ok(p[i], 64 + i * 8, (uint64_t)i + 1))
				return NULL;
			os_free(p[i]);
		}
	}

	/* Hold on to one block so the arena cannot be mistaken for unused,
	 * and report the arena while every worker is still alive.
	 */
	p[0] = os_malloc(32);
	r->arena = osmem_thread_arena();
	r->tid = osmem_thread_id();
	r->owner = r->arena ? osmem_arena_owner(r->arena) : 0;
	r->state = r->arena ? osmem_arena_state(r->arena) : -1;
	r->ok = 1;
	pthread_barrier_wait(&barrier);
	os_free(p[0]);
	return NULL;
}

static void test_private_arenas(void)
{
	pthread_t th[PRIVATE_THREADS];
	struct osmem_stats before, after;
	int i, j;

	t_begin("Phase 2A: concurrent threads get private, lock-free arenas");
	osmem_stats(&before);
	pthread_barrier_init(&barrier, NULL, PRIVATE_THREADS);
	for (i = 0; i < PRIVATE_THREADS; i++)
		CHECK(pthread_create(&th[i], NULL, private_worker, &results[i]) == 0);
	for (i = 0; i < PRIVATE_THREADS; i++)
		pthread_join(th[i], NULL);
	pthread_barrier_destroy(&barrier);

	for (i = 0; i < PRIVATE_THREADS; i++) {
		CHECKF(results[i].ok, "worker %d failed", i);
		CHECKF(results[i].arena != NULL, "worker %d got no arena", i);
		CHECKF(results[i].owner == results[i].tid,
		       "worker %d allocated from an arena owned by %llu, not by itself (%llu)",
		       i, results[i].owner, results[i].tid);
		CHECKF(results[i].state == OSMEM_ARENA_EXCLUSIVE,
		       "worker %d allocated from a non-exclusive arena (state %d)",
		       i, results[i].state);
		for (j = 0; j < i; j++)
			CHECKF(results[i].arena != results[j].arena,
			       "workers %d and %d shared arena %p while both were alive",
			       j, i, results[i].arena);
	}

	osmem_stats(&after);
	CHECKF(after.ffa_engaged == 0,
	       "the shared regime engaged although only %d threads were running",
	       PRIVATE_THREADS);
	CHECKF(after.remote_frees == before.remote_frees,
	       "%zu frees were deferred although every thread freed its own blocks",
	       after.remote_frees - before.remote_frees);
	CHECKF(after.arena_count <= before.arena_count + PRIVATE_THREADS,
	       "%zu arenas for %d threads", after.arena_count, PRIVATE_THREADS);
	CHECK_SOUND();
}

/* ------------------------------------- Phase 2B: adopting orphaned arenas */

static const void *seq_arena[SEQ_THREADS];

static void *seq_worker(void *arg)
{
	int idx = (int)(long)arg;
	void *p[64];
	int i;

	for (i = 0; i < 64; i++) {
		p[i] = os_malloc(4096);
		if (p[i])
			pattern_fill(p[i], 4096, (uint64_t)i + 1);
	}
	seq_arena[idx] = osmem_thread_arena();
	for (i = 0; i < 64; i++) {
		if (p[i] && !pattern_ok(p[i], 4096, (uint64_t)i + 1))
			seq_arena[idx] = NULL;
		os_free(p[i]);
	}
	return NULL;
}

static void test_orphan_adoption(void)
{
	struct osmem_stats before, after;
	pthread_t th;
	int i;

	t_begin("Phase 2B: a new thread adopts the arena of a finished one");
	osmem_stats(&before);
	for (i = 0; i < SEQ_THREADS; i++) {
		CHECK(pthread_create(&th, NULL, seq_worker, (void *)(long)i) == 0);
		pthread_join(th, NULL);
	}
	osmem_stats(&after);

	CHECKF(after.arena_count <= before.arena_count + 1,
	       "%d threads run one after another needed %zu new arenas instead of 1",
	       SEQ_THREADS, after.arena_count - before.arena_count);
	CHECKF(after.mmap_calls <= before.mmap_calls + 1,
	       "%d sequential threads cost %zu mmap calls",
	       SEQ_THREADS, after.mmap_calls - before.mmap_calls);
	for (i = 1; i < SEQ_THREADS; i++)
		CHECKF(seq_arena[i] == seq_arena[0],
		       "thread %d did not reuse the arena left by thread 0 (%p vs %p)",
		       i, seq_arena[i], seq_arena[0]);
	CHECKF(after.arena_orphan >= 1, "no arena was left adoptable");
	CHECK_SOUND();
}

/* ------------------------- Phase 3.1-3.3: freeing somebody else's memory */

#define QUEUE_SIZE 256
#define HANDOVER 20000

struct queue {
	void *slot[QUEUE_SIZE];
	size_t size[QUEUE_SIZE];
	uint64_t seed[QUEUE_SIZE];
	int head, tail, done;
	pthread_mutex_t lock;
	pthread_cond_t not_empty, not_full;
};

static struct queue q;
static int handover_errors;

static void *producer(void *arg)
{
	struct rng rng;
	int i;

	(void)arg;
	rng_seed(&rng, 99);
	for (i = 0; i < HANDOVER; i++) {
		size_t size = rng_size(&rng, 1, 2000);
		void *p = os_malloc(size);

		if (!p) {
			handover_errors++;
			break;
		}
		pattern_fill(p, size, (uint64_t)i + 1);

		pthread_mutex_lock(&q.lock);
		while ((q.head + 1) % QUEUE_SIZE == q.tail)
			pthread_cond_wait(&q.not_full, &q.lock);
		q.slot[q.head] = p;
		q.size[q.head] = size;
		q.seed[q.head] = (uint64_t)i + 1;
		q.head = (q.head + 1) % QUEUE_SIZE;
		pthread_cond_signal(&q.not_empty);
		pthread_mutex_unlock(&q.lock);
	}
	pthread_mutex_lock(&q.lock);
	q.done = 1;
	pthread_cond_broadcast(&q.not_empty);
	pthread_mutex_unlock(&q.lock);
	return NULL;
}

static void *consumer(void *arg)
{
	(void)arg;
	for (;;) {
		void *p;
		size_t size;
		uint64_t seed;

		pthread_mutex_lock(&q.lock);
		while (q.head == q.tail && !q.done)
			pthread_cond_wait(&q.not_empty, &q.lock);
		if (q.head == q.tail && q.done) {
			pthread_mutex_unlock(&q.lock);
			return NULL;
		}
		p = q.slot[q.tail];
		size = q.size[q.tail];
		seed = q.seed[q.tail];
		q.tail = (q.tail + 1) % QUEUE_SIZE;
		pthread_cond_signal(&q.not_full);
		pthread_mutex_unlock(&q.lock);

		if (!pattern_ok(p, size, seed))
			handover_errors++;
		os_free(p);		/* freed by a thread that did not allocate it */
	}
}

static void test_cross_thread_free(void)
{
	struct osmem_stats before, after;
	pthread_t prod, cons;

	t_begin("Phase 3: blocks allocated by one thread, freed by another");
	osmem_stats(&before);
	pthread_mutex_init(&q.lock, NULL);
	pthread_cond_init(&q.not_empty, NULL);
	pthread_cond_init(&q.not_full, NULL);
	q.head = q.tail = q.done = 0;

	CHECK(pthread_create(&prod, NULL, producer, NULL) == 0);
	CHECK(pthread_create(&cons, NULL, consumer, NULL) == 0);
	pthread_join(prod, NULL);
	pthread_join(cons, NULL);

	CHECKF(handover_errors == 0, "%d blocks were corrupted in transit", handover_errors);
	osmem_stats(&after);
	CHECKF(after.remote_frees > before.remote_frees,
	       "no free was deferred to the owning thread, so the remote path was untested");
	CHECK_SOUND();

	/* The deferred blocks must come back once the arena has no live owner. */
	osmem_drain_all();
	osmem_stats(&after);
	CHECK_STATF(after.bytes_used == 0, "%zu bytes still in use after the handover test",
	       after.bytes_used);
	CHECK_SOUND();
	printf("      (%zu frees took the deferred path)\n",
	       after.remote_frees - before.remote_frees);
}

/* ------------------------------- concurrent stress with shared ownership */

#define DONATE_SLOTS 512
#define STRESS_OPS 60000

static struct {
	void *p;
	size_t size;
	uint64_t seed;
} donated[DONATE_SLOTS];
static pthread_mutex_t donate_lock = PTHREAD_MUTEX_INITIALIZER;
static int stress_errors;

static void *stress_worker(void *arg)
{
	int id = (int)(long)arg;
	struct rng rng;
	void *mine[256] = { 0 };
	size_t size[256] = { 0 };
	int i, op;

	rng_seed(&rng, 1000 + id);
	for (op = 0; op < STRESS_OPS; op++) {
		int slot = (int)(rng_next(&rng) % 256);
		int action = (int)(rng_next(&rng) % 10);

		if (mine[slot]) {
			if (!pattern_ok(mine[slot], size[slot], (uint64_t)slot + 1))
				stress_errors++;
			if (action < 2) {
				/* Hand the block to another thread. */
				int d = (int)(rng_next(&rng) % DONATE_SLOTS);
				void *steal = NULL;
				size_t steal_size = 0;
				uint64_t steal_seed = 0;

				pthread_mutex_lock(&donate_lock);
				steal = donated[d].p;
				steal_size = donated[d].size;
				steal_seed = donated[d].seed;
				donated[d].p = mine[slot];
				donated[d].size = size[slot];
				donated[d].seed = (uint64_t)slot + 1;
				pthread_mutex_unlock(&donate_lock);

				mine[slot] = NULL;
				if (steal) {
					if (!pattern_ok(steal, steal_size, steal_seed))
						stress_errors++;
					os_free(steal);
				}
			} else if (action < 4) {
				size_t want = rng_size(&rng, 1, 4000);
				void *np = os_realloc(mine[slot], want);

				if (!np) {
					stress_errors++;
				} else {
					mine[slot] = np;
					size[slot] = want;
					pattern_fill(np, want, (uint64_t)slot + 1);
				}
			} else {
				os_free(mine[slot]);
				mine[slot] = NULL;
			}
		} else {
			size[slot] = rng_size(&rng, 1, 4000);
			mine[slot] = (action < 2) ? os_calloc(1, size[slot])
						  : os_malloc(size[slot]);
			if (!mine[slot])
				stress_errors++;
			else
				pattern_fill(mine[slot], size[slot], (uint64_t)slot + 1);
		}
	}
	for (i = 0; i < 256; i++) {
		if (mine[i]) {
			if (!pattern_ok(mine[i], size[i], (uint64_t)i + 1))
				stress_errors++;
			os_free(mine[i]);
		}
	}
	return NULL;
}

static void test_concurrent_stress(void)
{
	pthread_t th[STRESS_THREADS];
	struct osmem_stats st;
	int i;

	t_begin("concurrent stress, 8 threads, blocks changing owner");
	for (i = 0; i < STRESS_THREADS; i++)
		CHECK(pthread_create(&th[i], NULL, stress_worker, (void *)(long)i) == 0);
	for (i = 0; i < STRESS_THREADS; i++)
		pthread_join(th[i], NULL);

	for (i = 0; i < DONATE_SLOTS; i++) {
		if (donated[i].p) {
			CHECKF(pattern_ok(donated[i].p, donated[i].size, donated[i].seed),
			       "donated block %d was corrupted", i);
			os_free(donated[i].p);
			donated[i].p = NULL;
		}
	}
	CHECKF(stress_errors == 0, "%d errors during the concurrent run", stress_errors);
	CHECK_SOUND();
	osmem_drain_all();
	osmem_stats(&st);
	CHECK_STATF(st.bytes_used == 0, "%zu bytes still in use after the concurrent run",
	       st.bytes_used);
	CHECKF(st.arena_count <= st.arena_max, "%zu arenas exceed A_max = %zu",
	       st.arena_count, st.arena_max);
	CHECK_SOUND();
	printf("      (%zu arenas, %zu KiB mapped, %zu deferred frees)\n",
	       st.arena_count, st.bytes_mapped / 1024, st.remote_frees);
}

/* ------------------------------------------- no two live blocks overlap */

#define OVERLAP_THREADS 8
#define OVERLAP_BLOCKS 400

struct record {
	char *p;
	size_t size;
};

static struct record rec[OVERLAP_THREADS * OVERLAP_BLOCKS];

static void *overlap_worker(void *arg)
{
	int id = (int)(long)arg;
	struct record *mine = &rec[id * OVERLAP_BLOCKS];
	struct rng rng;
	int i;

	rng_seed(&rng, 4242 + id);
	for (i = 0; i < OVERLAP_BLOCKS; i++) {
		mine[i].size = rng_size(&rng, 1, 1024);
		mine[i].p = os_malloc(mine[i].size);
		if (mine[i].p)
			pattern_fill(mine[i].p, mine[i].size, (uint64_t)(id * 1000 + i) + 1);
	}
	pthread_barrier_wait(&barrier);
	for (i = 0; i < OVERLAP_BLOCKS; i++) {
		if (mine[i].p && !pattern_ok(mine[i].p, mine[i].size,
					     (uint64_t)(id * 1000 + i) + 1))
			stress_errors++;
	}
	pthread_barrier_wait(&barrier);
	for (i = 0; i < OVERLAP_BLOCKS; i++)
		os_free(mine[i].p);
	return NULL;
}

static int by_address(const void *a, const void *b)
{
	const struct record *x = a, *y = b;

	return x->p < y->p ? -1 : x->p > y->p ? 1 : 0;
}

static void test_no_overlap(void)
{
	pthread_t th[OVERLAP_THREADS];
	int total = OVERLAP_THREADS * OVERLAP_BLOCKS;
	int i, overlaps = 0, nulls = 0;

	t_begin("blocks handed to different threads never overlap");
	stress_errors = 0;
	pthread_barrier_init(&barrier, NULL, OVERLAP_THREADS);
	for (i = 0; i < OVERLAP_THREADS; i++)
		CHECK(pthread_create(&th[i], NULL, overlap_worker, (void *)(long)i) == 0);
	for (i = 0; i < OVERLAP_THREADS; i++)
		pthread_join(th[i], NULL);
	pthread_barrier_destroy(&barrier);

	qsort(rec, (size_t)total, sizeof(rec[0]), by_address);
	for (i = 0; i < total; i++) {
		if (!rec[i].p) {
			nulls++;
			continue;
		}
		CHECK_ALIGNED(rec[i].p);
		if (i + 1 < total && rec[i + 1].p && rec[i].p + rec[i].size > rec[i + 1].p)
			overlaps++;
	}
	CHECKF(nulls == 0, "%d allocations failed", nulls);
	CHECKF(overlaps == 0, "%d pairs of live blocks overlapped", overlaps);
	CHECKF(stress_errors == 0, "%d blocks were corrupted while all were live",
	       stress_errors);
	CHECK_SOUND();
}

int main(void)
{
	printf("== test_threads ==\n");
	test_private_arenas();
	test_orphan_adoption();
	test_cross_thread_free();
	test_concurrent_stress();
	test_no_overlap();
	return t_report("test_threads");
}
