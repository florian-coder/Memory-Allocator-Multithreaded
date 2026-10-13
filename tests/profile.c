// SPDX-License-Identifier: BSD-3-Clause
/*
 * Single-threaded profiling driver, for `make profile`.
 *
 * gprof's instrumentation is not thread safe, so this deliberately runs on one
 * thread: the question it answers is where the time goes on the same-thread
 * churn path, which is exactly the path that is slower than glibc's and the
 * one Phase 3.4 of the plan pays for.
 *
 * The workload is the same shape as the throughput benchmark -- a live set
 * churning through small blocks -- so the profile describes the numbers
 * `make bench` reports rather than some other program.
 */

#include "test_common.h"

#define SLOTS 128
#define OPS 20000000
#define MIN_SIZE 8
#define MAX_SIZE 512

int main(void)
{
	static void *slot[SLOTS];
	struct rng rng;
	uint64_t t0, t1;
	long ops = 0;
	int i, op;

	printf("== profile: %d operations, %d..%d byte blocks, one thread ==\n",
	       OPS, MIN_SIZE, MAX_SIZE);
	rng_seed(&rng, 1234);
	t0 = now_ns();

	for (op = 0; op < OPS; op++) {
		int k = (int)(rng_next(&rng) % SLOTS);

		if (slot[k]) {
			os_free(slot[k]);
			slot[k] = NULL;
		} else {
			size_t size = rng_size(&rng, MIN_SIZE, MAX_SIZE);

			slot[k] = os_malloc(size);
			if (!slot[k])
				return 1;
			((char *)slot[k])[0] = (char)k;
			((char *)slot[k])[size - 1] = (char)k;
		}
		ops++;
	}
	t1 = now_ns();

	for (i = 0; i < SLOTS; i++)
		os_free(slot[i]);

	printf("   %ld operations in %.2f s -- %.1f ns/op, %.2f Mops/s\n",
	       ops, (double)(t1 - t0) / 1e9, (double)(t1 - t0) / (double)ops,
	       (double)ops / ((double)(t1 - t0) / 1e9) / 1e6);
	printf("   (gmon.out written; `make profile` runs gprof over it)\n");
	return osmem_check();
}
