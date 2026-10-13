// SPDX-License-Identifier: BSD-3-Clause
/*
 * The free list.  Reuse, splitting, coalescing in both directions,
 * giving the trailing hole back to the arena's bump pointer, best fit, and a
 * long randomised run that must not fragment the arena away.
 */

#include "test_common.h"

/* Total block size the allocator uses for a request of n bytes. */
#define TOTAL(n)	(((n) + 16 + 15) / 16 * 16 < 32 ? 32 : ((n) + 16 + 15) / 16 * 16)
/* Request size that exactly fills a block of t bytes. */
#define PAYLOAD(t)	((t) - 16)

static void expect_empty(const char *what)
{
	struct osmem_stats st;

	/* Push through anything the allocator is holding back -- deferred
	 * cross-thread frees, and parked blocks when delayed coalescing is
	 * enabled -- so that "everything came back" is a fair question.
	 */
	osmem_drain_all();
	osmem_stats(&st);
	CHECK_STATF(st.bytes_used == 0, "%s: %zu bytes still in use", what, st.bytes_used);
	CHECKF(st.bytes_free == 0, "%s: %zu bytes left in the free lists", what, st.bytes_free);
	CHECKF(st.bytes_bump == 0, "%s: the arena frontier did not return to its start "
	       "(%zu bytes still handed out)", what, st.bytes_bump);
}

static void test_overhead(void)
{
	struct osmem_stats st;
	void *p[3];

	t_begin("the smallest block costs 32 bytes, header included");
	expect_empty("start");

	/* 16 bytes of header, so 1..16 bytes of payload fit the minimum block. */
	p[0] = os_malloc(1);
	osmem_stats(&st);
	CHECK_STATF(st.bytes_used == 32, "a 1-byte request costs %zu bytes", st.bytes_used);

	p[1] = os_malloc(16);
	osmem_stats(&st);
	CHECK_STATF(st.bytes_used == 64, "a 16-byte request pushed the total to %zu",
	       st.bytes_used);

	p[2] = os_malloc(17);		/* 17 + 16 -> 48 */
	osmem_stats(&st);
	CHECK_STATF(st.bytes_used == 112, "a 17-byte request pushed the total to %zu",
	       st.bytes_used);

	os_free(p[0]);
	os_free(p[1]);
	os_free(p[2]);
	expect_empty("after the overhead check");
	CHECK_SOUND();
}

static void test_foreign_pointer(void)
{
	struct osmem_stats before, after;
	static char fake[256];
	void *p;

	t_begin("a pointer the allocator never handed out is rejected");
	osmem_stats(&before);

	/* Memory that was never touched by this allocator, and memory with a
	 * plausible but wrong header.
	 */
	os_free(fake + 64);
	memset(fake, 0xa5, sizeof(fake));
	os_free(fake + 128);
	CHECK(os_realloc(fake + 128, 64) == NULL);

	osmem_stats(&after);
	CHECKF(after.foreign_ptrs == before.foreign_ptrs + 3,
	       "%zu of 3 foreign pointers were rejected",
	       after.foreign_ptrs - before.foreign_ptrs);

	/* And the allocator is untouched by it. */
	p = os_malloc(64);
	CHECK(p != NULL);
	os_free(p);
	CHECK_SOUND();
}

static void test_reuse(void)
{
	struct osmem_stats before, after;
	void *p, *q;

	t_begin("a freed block is handed out again without touching the kernel");
	expect_empty("start");
	os_free(os_malloc(8));			/* make sure an arena exists */
	osmem_stats(&before);
	p = os_malloc(100);
	os_free(p);
	q = os_malloc(100);
	osmem_stats(&after);
	CHECKF(q == p, "block was not recycled: %p then %p", p, q);
	CHECKF(after.mmap_calls == before.mmap_calls,
	       "recycling cost %zu mmap calls", after.mmap_calls - before.mmap_calls);
	os_free(q);
	CHECK_SOUND();
	expect_empty("after reuse");
}

static void test_split(void)
{
	struct osmem_stats st;
	char *big, *small, *guard;

	t_begin("a large hole is split, the remainder stays reusable");
	expect_empty("start");
	big = os_malloc(PAYLOAD(1024));
	guard = os_malloc(64);			/* keeps the hole off the frontier */
	os_free(big);
	osmem_drain_all();
	small = os_malloc(PAYLOAD(128));
	CHECKF(small == big, "the hole was not reused (%p vs %p)", (void *)small, (void *)big);
	osmem_stats(&st);
	CHECKF(st.bytes_free == 1024 - 128, "remainder after the split is %zu, expected 896",
	       st.bytes_free);
	CHECKF(st.largest_free == 1024 - 128, "largest hole is %zu, expected 896",
	       st.largest_free);
	CHECK_SOUND();
	os_free(small);
	os_free(guard);
	expect_empty("after split");
}

static void test_coalesce(void)
{
	struct osmem_stats st;
	char *b[5];
	void *p;
	int i;

	t_begin("adjacent holes merge forwards, backwards and both ways");
	expect_empty("start");

	/* Five consecutive 128-byte blocks carved from the bump pointer. */
	for (i = 0; i < 5; i++) {
		b[i] = os_malloc(PAYLOAD(128));
		CHECK(b[i] != NULL);
	}
	for (i = 1; i < 5; i++)
		CHECKF(b[i] == b[i - 1] + 128,
		       "bump allocation is not contiguous: %p then %p",
		       (void *)b[i - 1], (void *)b[i]);

	/* Forwards: free b[1], then b[2] merges into it. */
	os_free(b[1]);
	os_free(b[2]);
	osmem_drain_all();
	osmem_stats(&st);
	CHECKF(st.largest_free == 256, "forward merge produced a %zu-byte hole", st.largest_free);
	CHECK_STATF(st.blocks_free == 1, "forward merge left %zu holes", st.blocks_free);
	p = os_malloc(PAYLOAD(256));
	CHECKF(p == b[1], "the merged hole was not reused (%p vs %p)", p, (void *)b[1]);
	CHECK_SOUND();

	/* Backwards: free b[3] first, then p (which sits in front of it). */
	os_free(b[3]);
	os_free(p);
	osmem_drain_all();
	osmem_stats(&st);
	CHECKF(st.largest_free == 384, "backward merge produced a %zu-byte hole",
	       st.largest_free);
	CHECK_STATF(st.blocks_free == 1, "backward merge left %zu holes", st.blocks_free);

	/* Both ways: b[1..3] is one hole, b[0] and b[4] are in use.  Freeing
	 * b[0] must merge with the hole in front of it, and b[4] then merges
	 * everything and hands it back to the bump pointer.
	 */
	os_free(b[0]);
	osmem_drain_all();
	osmem_stats(&st);
	CHECK_STATF(st.blocks_free == 1 && st.largest_free == 512,
	       "merging both ways left %zu hole(s), largest %zu", st.blocks_free,
	       st.largest_free);
	CHECK_SOUND();
	os_free(b[4]);
	expect_empty("after merging everything");
	CHECK_SOUND();
}

static void test_bump_retract(void)
{
	struct osmem_stats st;
	void *p[64];
	int i;

	t_begin("freeing the trailing block gives the space back to the frontier");
	expect_empty("start");
	for (i = 0; i < 64; i++)
		p[i] = os_malloc(200);
	osmem_stats(&st);
	CHECKF(st.bytes_bump == 64 * TOTAL(200), "frontier advanced by %zu, expected %zu",
	       st.bytes_bump, (size_t)(64 * TOTAL(200)));
	for (i = 63; i >= 0; i--) {
		os_free(p[i]);
		osmem_drain_all();
		osmem_stats(&st);
		CHECKF(st.bytes_free == 0,
		       "freeing block %d in LIFO order left %zu bytes in a free list",
		       i, st.bytes_free);
	}
	expect_empty("after LIFO teardown");
	CHECK_SOUND();
}

static void test_best_fit(void)
{
	char *hole[3], *guard[3];
	size_t size[3] = { 512, 128, 1024 };
	void *p;
	int i;

	t_begin("the closest-fitting hole is picked");
	expect_empty("start");
	/* Three holes of different sizes, each kept apart by a guard block so
	 * that they cannot merge.
	 */
	for (i = 0; i < 3; i++) {
		hole[i] = os_malloc(PAYLOAD(size[i]));
		guard[i] = os_malloc(64);
	}
	for (i = 0; i < 3; i++)
		os_free(hole[i]);
	osmem_drain_all();		/* the holes must be in the free lists */

	p = os_malloc(PAYLOAD(128));
	CHECKF(p == hole[1], "a 112-byte request took the %s hole",
	       p == hole[0] ? "512-byte" : p == hole[2] ? "1024-byte" : "wrong");
	os_free(p);
	osmem_drain_all();

	p = os_malloc(PAYLOAD(512));
	CHECKF(p == hole[0], "a 496-byte request did not take the 512-byte hole%s", "");
	os_free(p);
	osmem_drain_all();

	p = os_malloc(PAYLOAD(256));		/* no exact match: 512 is closest */
	CHECKF(p == hole[0], "a 240-byte request did not take the closest (512) hole");
	os_free(p);
	CHECK_SOUND();

	for (i = 0; i < 3; i++)
		os_free(guard[i]);
	expect_empty("after best fit");
}

#define LIVE 2048
#define OPS 300000

static void test_stress(void)
{
	struct osmem_stats st;
	static void *ptr[LIVE];
	static size_t size[LIVE];
	struct rng rng;
	size_t peak_arenas = 0;
	int i, op;

	t_begin("randomised allocate/free/realloc run, 300k operations");
	expect_empty("start");
	rng_seed(&rng, 20261012);

	for (op = 0; op < OPS; op++) {
		int slot = (int)(rng_next(&rng) % LIVE);
		int action = (int)(rng_next(&rng) % 10);

		if (ptr[slot] && !pattern_ok(ptr[slot], size[slot], (uint64_t)slot + 1)) {
			CHECKF(0, "slot %d (%zu bytes) corrupted at operation %d",
			       slot, size[slot], op);
			ptr[slot] = NULL;
			break;
		}

		if (!ptr[slot]) {
			size[slot] = rng_size(&rng, 1, 3000);
			ptr[slot] = os_malloc(size[slot]);
			if (!ptr[slot]) {
				CHECKF(0, "os_malloc(%zu) failed at operation %d",
				       size[slot], op);
				break;
			}
			pattern_fill(ptr[slot], size[slot], (uint64_t)slot + 1);
		} else if (action < 3) {
			size_t old = size[slot];
			void *q;

			size[slot] = rng_size(&rng, 1, 3000);
			q = os_realloc(ptr[slot], size[slot]);
			if (!q) {
				CHECKF(0, "os_realloc(%zu) failed at operation %d",
				       size[slot], op);
				break;
			}
			ptr[slot] = q;
			if (size[slot] > old) {
				if (!pattern_ok(ptr[slot], old, (uint64_t)slot + 1))
					CHECKF(0, "realloc at operation %d lost data", op);
				pattern_fill(ptr[slot], size[slot], (uint64_t)slot + 1);
			} else {
				pattern_fill(ptr[slot], size[slot], (uint64_t)slot + 1);
			}
		} else {
			os_free(ptr[slot]);
			ptr[slot] = NULL;
		}

		if ((op & 0xffff) == 0) {
			osmem_stats(&st);
			if (st.arena_count > peak_arenas)
				peak_arenas = st.arena_count;
		}
	}

	CHECK_SOUND();
	for (i = 0; i < LIVE; i++) {
		if (ptr[i]) {
			CHECKF(pattern_ok(ptr[i], size[i], (uint64_t)i + 1),
			       "slot %d corrupted by the end of the run", i);
			os_free(ptr[i]);
			ptr[i] = NULL;
		}
	}
	osmem_stats(&st);
	CHECKF(st.arena_count <= 8, "the run needed %zu arenas for a <= 6 MiB live set",
	       st.arena_count);
	CHECKF(st.standalone_blocks == 0, "%zu standalone blocks leaked",
	       st.standalone_blocks);
	expect_empty("after the stress run");
	CHECK_SOUND();
	printf("      (peak arenas: %zu, %zu KiB mapped)\n", peak_arenas,
	       st.bytes_mapped / 1024);
}

int main(void)
{
	printf("== test_reuse ==\n");
	test_overhead();
	test_foreign_pointer();
	test_reuse();
	test_split();
	test_coalesce();
	test_bump_retract();
	test_best_fit();
	test_stress();
	return t_report("test_reuse");
}
