// SPDX-License-Identifier: BSD-3-Clause
/*
 * Out-of-memory behaviour.
 *
 * Every mapping goes through osmem_map()/osmem_remap(), so osmem_fail_mmap_after()
 * can starve the allocator at an exact point and the failure paths -- which a
 * normal run never reaches -- become testable: a failed request must return
 * NULL with ENOMEM, must not lose the caller's data, must leave the allocator
 * consistent, and the allocator must recover once memory is available again.
 */

#include <errno.h>

#include "test_common.h"

#define ARENA_MAX 2
#define ARENA_SIZE (128 * 1024)
#define BIG 2048
#define MANY 512

static void test_first_mapping_fails(void)
{
	struct osmem_stats st;
	void *p;

	t_begin("the very first request fails when the kernel gives nothing");
	osmem_fail_mmap_after(0);
	errno = 0;
	p = os_malloc(64);
	CHECKF(p == NULL, "os_malloc succeeded with no memory available");
	CHECKF(errno == ENOMEM, "errno is %d, expected ENOMEM", errno);

	osmem_stats(&st);
	CHECKF(st.arena_count == 0, "%zu arenas exist after a refused mapping",
	       st.arena_count);
	CHECKF(st.map_denied >= 1, "the refusal was not counted (%zu)", st.map_denied);

	/* calloc and realloc must fail the same way. */
	errno = 0;
	CHECK(os_calloc(8, 8) == NULL);
	CHECKF(errno == ENOMEM, "calloc set errno to %d", errno);
	errno = 0;
	CHECK(os_realloc(NULL, 64) == NULL);
	CHECKF(errno == ENOMEM, "realloc(NULL) set errno to %d", errno);

	/* ... and the allocator must come back to life. */
	osmem_fail_mmap_after(-1);
	p = os_malloc(64);
	CHECKF(p != NULL, "the allocator did not recover");
	os_free(p);
	CHECK_SOUND();
}

static void test_arenas_full_and_no_more_memory(void)
{
	static void *live[MANY];
	struct osmem_stats st;
	int n = 0, i;
	void *p;

	t_begin("every arena full and no mapping available");
	/* Fill the two arenas, then cut off the kernel. */
	for (i = 0; i < MANY; i++) {
		live[n] = os_malloc(BIG);
		if (!live[n])
			break;
		pattern_fill(live[n], BIG, (uint64_t)n + 1);
		n++;
		osmem_stats(&st);
		if (st.standalone_blocks > 0)	/* arenas are full, valve is open */
			break;
	}
	CHECKF(n > 0, "could not fill the arenas at all");
	osmem_stats(&st);
	CHECKF(st.arena_count == ARENA_MAX, "%zu arenas, expected %d", st.arena_count,
	       ARENA_MAX);

	osmem_fail_mmap_after(0);
	errno = 0;
	p = os_malloc(BIG);
	CHECKF(p == NULL, "allocation succeeded with full arenas and no memory");
	CHECKF(errno == ENOMEM, "errno is %d, expected ENOMEM", errno);

	/*
	 * Freeing must keep working while memory is exhausted, but *where* the
	 * block came from decides whether its space is reusable.  The last
	 * block came from the pressure valve, so freeing it hands the memory
	 * straight back to the kernel and does not help at all ...
	 */
	CHECKF(st.standalone_blocks == 1, "expected one block from the valve, got %zu",
	       st.standalone_blocks);
	os_free(live[n - 1]);
	live[n - 1] = NULL;
	osmem_stats(&st);
	CHECKF(st.standalone_blocks == 0, "the valve block was not released");
	p = os_malloc(BIG);
	CHECKF(p == NULL,
	       "releasing a block that owned its mapping should not create arena space");

	/* ... while freeing a block that lives in an arena does, with no
	 * syscall at all.
	 */
	os_free(live[0]);
	live[0] = NULL;
	p = os_malloc(BIG);
	CHECKF(p != NULL, "the arena space just freed was not reusable");
	if (p) {
		pattern_fill(p, BIG, 999);
		CHECK(pattern_ok(p, BIG, 999));
		os_free(p);
	}

	osmem_fail_mmap_after(-1);
	for (i = 0; i < n; i++) {
		if (!live[i])
			continue;
		CHECKF(pattern_ok(live[i], BIG, (uint64_t)i + 1),
		       "block %d was corrupted while memory was exhausted", i);
		os_free(live[i]);
		live[i] = NULL;
	}
	CHECK_SOUND();
}

static void test_failed_realloc_keeps_the_block(void)
{
	char *p, *q;

	t_begin("a realloc that cannot get memory must not lose the old block");
	/* Arena-backed block. */
	p = os_malloc(BIG);
	CHECK(p != NULL);
	pattern_fill(p, BIG, 21);

	osmem_fail_mmap_after(0);
	q = os_realloc(p, 64 * 1024);		/* needs a new arena or a mapping */
	if (q == NULL) {
		CHECKF(pattern_ok(p, BIG, 21),
		       "the original block was damaged by the failed realloc");
	} else {
		/* Served from existing space: that is fine, just check the data. */
		CHECKF(pattern_ok(q, BIG, 21), "realloc lost data");
		p = q;
	}
	osmem_fail_mmap_after(-1);
	os_free(p);
	CHECK_SOUND();
}

static void test_failed_mremap_keeps_the_block(void)
{
	char *p, *q;

	t_begin("a standalone block survives a failed mremap");
	p = os_malloc(100 * 1024);		/* above the arena threshold */
	CHECK(p != NULL);
	pattern_fill(p, 100 * 1024, 33);

	osmem_fail_mmap_after(0);
	errno = 0;
	q = os_realloc(p, 400 * 1024);
	CHECKF(q == NULL, "mremap succeeded although no memory was available");
	CHECKF(pattern_ok(p, 100 * 1024, 33),
	       "the standalone block was damaged by the failed realloc");

	/* Shrinking does not need new memory, so it must still work. */
	q = os_realloc(p, 80 * 1024);
	CHECKF(q != NULL, "shrinking a standalone block needed new memory");
	if (q) {
		CHECKF(pattern_ok(q, 80 * 1024, 33), "shrinking lost data");
		p = q;
	}
	osmem_fail_mmap_after(-1);
	os_free(p);
	CHECK_SOUND();
}

static void test_recovery(void)
{
	struct osmem_stats st;
	void *p[64];
	int i;

	t_begin("full recovery after a starvation episode");
	for (i = 0; i < 64; i++) {
		p[i] = os_malloc(512);
		CHECK(p[i] != NULL);
		pattern_fill(p[i], 512, (uint64_t)i + 1);
	}
	for (i = 0; i < 64; i++) {
		CHECKF(pattern_ok(p[i], 512, (uint64_t)i + 1), "block %d corrupted", i);
		os_free(p[i]);
	}
	osmem_drain_all();
	osmem_stats(&st);
	CHECK_STATF(st.bytes_used == 0, "%zu bytes still in use", st.bytes_used);
	CHECKF(st.standalone_blocks == 0, "%zu standalone blocks leaked",
	       st.standalone_blocks);
	CHECKF(st.arena_count <= ARENA_MAX, "%zu arenas", st.arena_count);
	CHECK_SOUND();
	printf("      (%zu mappings refused in total)\n", st.map_denied);
}

int main(void)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%d", ARENA_MAX);
	setenv("OSMEM_ARENA_MAX", buf, 1);
	snprintf(buf, sizeof(buf), "%d", ARENA_SIZE);
	setenv("OSMEM_ARENA_SIZE", buf, 1);

	printf("== test_faults (A_max=%d, arena=%d KiB) ==\n", ARENA_MAX, ARENA_SIZE / 1024);
	test_first_mapping_fails();
	test_arenas_full_and_no_more_memory();
	test_failed_realloc_keeps_the_block();
	test_failed_mremap_keeps_the_block();
	test_recovery();
	return t_report("test_faults");
}
