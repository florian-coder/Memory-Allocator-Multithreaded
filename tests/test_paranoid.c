// SPDX-License-Identifier: BSD-3-Clause
/*
 * Verifying every structural invariant *while* threads are hammering the
 * allocator, rather than only at join points.
 *
 * Needs the allocator built with -DOSMEM_WORLD_LOCK (use `make test-paranoid`),
 * which makes every public entry point hold a read lock for the whole
 * operation so that osmem_check() can stop the world.  Built without it the
 * test reports that it was skipped, so a plain `make test` stays green.
 *
 * Normally paired with -DOSMEM_CHAOS, which perturbs the interleaving at the
 * windows where two threads can race.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include "test_common.h"

#define WORKERS 8
#define SLOTS 128
#define DONATE 256
#define RUN_MS 4000
#define CHECK_INTERVAL_US 300

static _Atomic int running = 1;
static _Atomic long worker_ops;
static int worker_errors;

static struct {
	void *p;
	size_t size;
	uint64_t seed;
} donated[DONATE];
static pthread_mutex_t donate_lock = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg)
{
	int id = (int)(long)arg;
	void *mine[SLOTS] = { 0 };
	size_t size[SLOTS] = { 0 };
	struct rng rng;
	int i;

	rng_seed(&rng, 5000 + id);
	while (atomic_load_explicit(&running, memory_order_relaxed)) {
		int slot = (int)(rng_next(&rng) % SLOTS);
		int action = (int)(rng_next(&rng) % 10);

		if (!mine[slot]) {
			size[slot] = rng_size(&rng, 1, 3000);
			mine[slot] = (action < 2) ? os_calloc(1, size[slot])
						  : os_malloc(size[slot]);
			if (!mine[slot])
				worker_errors++;
			else
				pattern_fill(mine[slot], size[slot], (uint64_t)slot + 1);
		} else if (!pattern_ok(mine[slot], size[slot], (uint64_t)slot + 1)) {
			worker_errors++;
			mine[slot] = NULL;
		} else if (action < 2) {
			/* hand the block to another thread */
			int d = (int)(rng_next(&rng) % DONATE);
			void *steal;
			size_t steal_size;
			uint64_t steal_seed;

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
					worker_errors++;
				os_free(steal);
			}
		} else if (action < 4) {
			size_t want = rng_size(&rng, 1, 5000);
			void *np = os_realloc(mine[slot], want);

			if (!np) {
				worker_errors++;
			} else {
				mine[slot] = np;
				size[slot] = want;
				pattern_fill(np, want, (uint64_t)slot + 1);
			}
		} else {
			os_free(mine[slot]);
			mine[slot] = NULL;
		}
		atomic_fetch_add_explicit(&worker_ops, 1, memory_order_relaxed);
	}

	for (i = 0; i < SLOTS; i++) {
		if (mine[i]) {
			if (!pattern_ok(mine[i], size[i], (uint64_t)i + 1))
				worker_errors++;
			os_free(mine[i]);
		}
	}
	return NULL;
}

static long checks_done;
static long checks_failed;

static void *monitor(void *arg)
{
	(void)arg;
	while (atomic_load_explicit(&running, memory_order_relaxed)) {
		int errs = osmem_check();

		checks_done++;
		if (errs)
			checks_failed++;
		usleep(CHECK_INTERVAL_US);
	}
	return NULL;
}

int main(void)
{
	pthread_t th[WORKERS], mon;
	struct osmem_stats st;
	int i;

	printf("== test_paranoid ==\n");
	osmem_stats(&st);
	if (!st.has_world_lock) {
		printf("  - skipped: needs -DOSMEM_WORLD_LOCK (run `make test-paranoid`)\n");
		printf("SKIP test_paranoid\n");
		return 0;
	}
	t_begin("full consistency check while 8 threads allocate");
	printf("      (world lock: on, chaos: %s)\n", st.has_chaos ? "on" : "off");
	fflush(stdout);

	for (i = 0; i < WORKERS; i++)
		CHECK(pthread_create(&th[i], NULL, worker, (void *)(long)i) == 0);
	CHECK(pthread_create(&mon, NULL, monitor, NULL) == 0);

	usleep(RUN_MS * 1000);
	atomic_store_explicit(&running, 0, memory_order_relaxed);

	for (i = 0; i < WORKERS; i++)
		pthread_join(th[i], NULL);
	pthread_join(mon, NULL);

	for (i = 0; i < DONATE; i++) {
		if (donated[i].p) {
			CHECKF(pattern_ok(donated[i].p, donated[i].size, donated[i].seed),
			       "donated block %d was corrupted", i);
			os_free(donated[i].p);
		}
	}

	CHECKF(worker_errors == 0, "%d errors reported by the workers", worker_errors);
	CHECKF(checks_done > 100, "only %ld consistency checks ran", checks_done);
	CHECKF(checks_failed == 0, "%ld of %ld live consistency checks failed",
	       checks_failed, checks_done);
	osmem_drain_all();
	CHECK_SOUND();
	osmem_stats(&st);
	CHECK_STATF(st.bytes_used == 0, "%zu bytes still in use", st.bytes_used);
	printf("      (%ld operations, %ld live checks, %zu arenas)\n",
	       atomic_load(&worker_ops), checks_done, st.arena_count);
	return t_report("test_paranoid");
}
