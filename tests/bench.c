// SPDX-License-Identifier: BSD-3-Clause
/*
 * Throughput comparison against glibc malloc, for the two regimes the
 * allocator is built for: few threads (private, lock-free arenas) and many
 * threads (shared pool).  Informational -- it asserts only that nothing fails.
 */

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <unistd.h>

#include "test_common.h"

#define SLOTS 128
#define OPS_PER_THREAD 1000000
#define MAX_THREADS 32

struct allocator {
	const char *name;
	void *(*alloc)(size_t);
	void (*release)(void *);
};

static void *libc_alloc(size_t n)
{
	return malloc(n);
}

static void libc_free(void *p)
{
	free(p);
}

static const struct allocator allocators[] = {
	{ "osmem", os_malloc, os_free },
	{ "glibc", libc_alloc, libc_free },
};

struct job {
	const struct allocator *a;
	int id;
	int ops;
	int failures;
};

static pthread_barrier_t start;

static void *run(void *arg)
{
	struct job *j = arg;
	void *slot[SLOTS] = { 0 };
	struct rng rng;
	int i, op;

	rng_seed(&rng, 777 + j->id);
	pthread_barrier_wait(&start);

	for (op = 0; op < j->ops; op++) {
		int k = (int)(rng_next(&rng) % SLOTS);

		if (slot[k]) {
			j->a->release(slot[k]);
			slot[k] = NULL;
		} else {
			size_t size = rng_size(&rng, 8, 512);

			slot[k] = j->a->alloc(size);
			if (!slot[k]) {
				j->failures++;
				continue;
			}
			((char *)slot[k])[0] = (char)k;
			((char *)slot[k])[size - 1] = (char)k;
		}
	}
	for (i = 0; i < SLOTS; i++)
		if (slot[i])
			j->a->release(slot[i]);
	return NULL;
}

static double measure(const struct allocator *a, int threads, int *failures)
{
	pthread_t th[MAX_THREADS];
	struct job job[MAX_THREADS];
	uint64_t t0, t1;
	double total_ops;
	int i;

	pthread_barrier_init(&start, NULL, (unsigned)threads + 1);
	for (i = 0; i < threads; i++) {
		job[i].a = a;
		job[i].id = i;
		job[i].ops = OPS_PER_THREAD;
		job[i].failures = 0;
		pthread_create(&th[i], NULL, run, &job[i]);
	}
	pthread_barrier_wait(&start);
	t0 = now_ns();
	for (i = 0; i < threads; i++)
		pthread_join(th[i], NULL);
	t1 = now_ns();
	pthread_barrier_destroy(&start);

	*failures = 0;
	for (i = 0; i < threads; i++)
		*failures += job[i].failures;

	total_ops = (double)threads * OPS_PER_THREAD;
	return total_ops / ((double)(t1 - t0) / 1e9) / 1e6;	/* million ops/s */
}

/* ---------------------------------------------- producer / consumer handover
 *
 * Half the threads allocate, the other half free what they get.  This is the
 * case the remote-free stack exists for: the freeing thread never enters the
 * owner's arena, it just pushes the block on a lock-free stack.
 */

#define RING 4096
#define HANDOVER_OPS 2000000

struct ring {
	void *slot[RING];
	_Atomic unsigned head;
	_Atomic unsigned tail;
	const struct allocator *a;
};

static struct ring ring;

static void *handover_producer(void *arg)
{
	struct rng rng;
	int i;

	(void)arg;
	rng_seed(&rng, 31337);
	pthread_barrier_wait(&start);
	for (i = 0; i < HANDOVER_OPS; i++) {
		size_t size = rng_size(&rng, 8, 512);
		void *p = ring.a->alloc(size);
		unsigned head;

		if (!p)
			continue;
		((char *)p)[0] = 1;
		head = atomic_load_explicit(&ring.head, memory_order_relaxed);
		while (head - atomic_load_explicit(&ring.tail, memory_order_acquire) >= RING)
			sched_yield();
		ring.slot[head % RING] = p;
		atomic_store_explicit(&ring.head, head + 1, memory_order_release);
	}
	return NULL;
}

static void *handover_consumer(void *arg)
{
	int i;

	(void)arg;
	pthread_barrier_wait(&start);
	for (i = 0; i < HANDOVER_OPS; i++) {
		unsigned tail = atomic_load_explicit(&ring.tail, memory_order_relaxed);

		while (atomic_load_explicit(&ring.head, memory_order_acquire) == tail)
			sched_yield();
		ring.a->release(ring.slot[tail % RING]);
		atomic_store_explicit(&ring.tail, tail + 1, memory_order_release);
	}
	return NULL;
}

static double measure_handover(const struct allocator *a, int pairs)
{
	pthread_t th[MAX_THREADS];
	uint64_t t0, t1;
	int i;

	ring.a = a;
	atomic_store(&ring.head, 0);
	atomic_store(&ring.tail, 0);

	pthread_barrier_init(&start, NULL, (unsigned)(2 * pairs) + 1);
	for (i = 0; i < pairs; i++) {
		pthread_create(&th[2 * i], NULL, handover_producer, NULL);
		pthread_create(&th[2 * i + 1], NULL, handover_consumer, NULL);
	}
	pthread_barrier_wait(&start);
	t0 = now_ns();
	for (i = 0; i < 2 * pairs; i++)
		pthread_join(th[i], NULL);
	t1 = now_ns();
	pthread_barrier_destroy(&start);

	/* One malloc plus one free per iteration. */
	return 2.0 * HANDOVER_OPS * pairs / ((double)(t1 - t0) / 1e9) / 1e6;
}

static void bench_handover(void)
{
	double mine, libc;

	printf("\n== bench: one thread allocates, another frees ==\n\n");
	printf("  pairs       osmem Mops/s     glibc Mops/s      ratio\n");
	printf("  -----       ------------     ------------      -----\n");
	mine = measure_handover(&allocators[0], 1);
	libc = measure_handover(&allocators[1], 1);
	printf("  %5d       %12.2f     %12.2f      %4.2fx\n", 1, mine, libc, mine / libc);
	CHECKF(osmem_check() == 0, "the allocator is inconsistent after the handover run");
}

/* ------------------------------------------------- what it costs in memory
 *
 * Throughput on its own says nothing about an allocator: the cheapest way to
 * be fast is to waste memory.  This measures the other half -- how many pages
 * the allocator actually makes the kernel commit, against the bytes the
 * program asked for -- over a churning live set, which is where fragmentation
 * shows up.
 */

#define FRAG_LIVE 2048
#define FRAG_OPS 400000
#define FRAG_MAX 3000

static void bench_footprint(void)
{
	static void *live[FRAG_LIVE];
	static size_t size[FRAG_LIVE];
	struct osmem_stats st;
	size_t requested = 0, peak_touched = 0, peak_requested = 0;
	size_t base_touched, touched;
	struct rng rng;
	int i, op;

	printf("\n== bench: memory footprint over a churning live set ==\n\n");
	rng_seed(&rng, 4242);

	/*
	 * The throughput runs above left pages committed in up to 32 arenas.
	 * Give back what can be given back, then measure only the growth from
	 * here: bytes_touched is a process-wide total, so an absolute reading
	 * would be answering a different question.
	 */
	os_trim();
	osmem_stats(&st);
	base_touched = st.bytes_touched;

	for (op = 0; op < FRAG_OPS; op++) {
		int k = (int)(rng_next(&rng) % FRAG_LIVE);

		if (live[k]) {
			requested -= size[k];
			os_free(live[k]);
			live[k] = NULL;
		} else {
			size[k] = rng_size(&rng, 1, FRAG_MAX);
			live[k] = os_malloc(size[k]);
			if (!live[k]) {
				CHECKF(0, "allocation failed at operation %d", op);
				break;
			}
			requested += size[k];
		}
		if ((op & 0x3fff) == 0) {
			osmem_stats(&st);
			touched = st.bytes_touched - base_touched;
			if (touched > peak_touched)
				peak_touched = touched;
			if (requested > peak_requested)
				peak_requested = requested;
		}
	}

	osmem_stats(&st);
	touched = st.bytes_touched - base_touched;
	printf("  live payload requested      %8zu KiB\n", requested / 1024);
	printf("  blocks in use               %8zu KiB  (headers and rounding)\n",
	       st.bytes_used / 1024);
	printf("  free, held for reuse        %8zu KiB\n", st.bytes_free / 1024);
	if (st.has_quarantine)
		printf("  parked, not yet coalesced   %8zu KiB\n",
		       st.bytes_quarantined / 1024);
	printf("  pages newly committed       %8zu KiB\n", touched / 1024);
	if (requested)
		printf("  footprint / requested          %6.2fx   (peak %.2fx)\n",
		       (double)touched / (double)requested,
		       peak_requested ? (double)peak_touched / (double)peak_requested : 0.0);

	for (i = 0; i < FRAG_LIVE; i++) {
		os_free(live[i]);
		live[i] = NULL;
	}
	osmem_drain_all();
	osmem_stats(&st);
	printf("  after freeing everything    %8zu KiB still committed here, %zu KiB in free lists\n",
	       (st.bytes_touched - base_touched) / 1024, st.bytes_free / 1024);
	CHECK_STATF(st.bytes_used == 0, "%zu bytes still in use", st.bytes_used);
	CHECKF(osmem_check() == 0, "the allocator is inconsistent after the run");
}

int main(void)
{
	int counts[] = { 1, 2, 4, 8, 16, 32 };
	struct osmem_stats st;
	size_t i;

	printf("== bench: %d operations per thread, 8..512 byte blocks ==\n\n",
	       OPS_PER_THREAD);
	printf("  threads     osmem Mops/s     glibc Mops/s      ratio\n");
	printf("  -------     ------------     ------------      -----\n");

	/* Warm both allocators up so the first row is not paying for setup. */
	for (i = 0; i < 2; i++) {
		int dropped;

		measure(&allocators[i], 1, &dropped);
	}

	for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
		int f1 = 0, f2 = 0;
		double mine, libc;

		mine = measure(&allocators[0], counts[i], &f1);
		libc = measure(&allocators[1], counts[i], &f2);
		printf("  %7d     %12.2f     %12.2f      %4.2fx\n",
		       counts[i], mine, libc, mine / libc);
		CHECKF(f1 == 0, "%d osmem allocations failed at %d threads", f1, counts[i]);
		CHECKF(f2 == 0, "%d glibc allocations failed at %d threads", f2, counts[i]);
	}

	bench_handover();
	bench_footprint();

	osmem_stats(&st);
	printf("\n  osmem: %zu arenas (A_max %zu), %zu KiB mapped, %zu mmap / %zu munmap calls\n",
	       st.arena_count, st.arena_max, st.bytes_mapped / 1024, st.mmap_calls,
	       st.munmap_calls);
	printf("  osmem: %zu shared-pool allocations, %zu deferred frees, %d violations\n",
	       st.ffa_allocs, st.remote_frees, osmem_check());
	printf("  osmem: build = %s coalescing, %s counters%s%s\n\n",
	       st.has_quarantine ? "delayed" : "immediate",
	       st.has_stats ? "full" : "minimal",
	       st.has_world_lock ? ", world lock" : "",
	       st.has_chaos ? ", chaos" : "");
	return t_report("bench");
}
