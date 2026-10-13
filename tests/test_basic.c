// SPDX-License-Identifier: BSD-3-Clause
/*
 * API semantics plus the Phase 1 promises: nothing is allocated before the
 * first request, brk/sbrk is never touched, and memory arrives as arenas.
 */

#include <errno.h>
#include <unistd.h>

#include "test_common.h"

#define NPTR 256

static void test_lazy_start(void)
{
	struct osmem_stats st;
	void *p;

	t_begin("Phase 1.1: nothing is mapped before the first request");
	osmem_stats(&st);
	CHECKF(st.arena_count == 0, "arena_count is %zu before any allocation", st.arena_count);
	CHECKF(st.bytes_mapped == 0, "%zu bytes already mapped", st.bytes_mapped);
	CHECKF(st.mmap_calls == 0, "%zu mmap calls already made", st.mmap_calls);
	CHECKF(st.arena_max >= 2, "A_max is %zu, must leave room for a shared arena",
	       st.arena_max);

	p = os_malloc(1);
	CHECK(p != NULL);
	osmem_stats(&st);
	CHECKF(st.arena_count == 1, "first malloc created %zu arenas", st.arena_count);
	CHECKF(st.mmap_calls == 1, "first malloc made %zu mmap calls", st.mmap_calls);
	CHECKF(st.bytes_mapped == st.arena_size, "mapped %zu bytes for a %zu-byte arena",
	       st.bytes_mapped, st.arena_size);
	CHECKF(st.arena_exclusive == 1, "the first arena is not private (%zu private)",
	       st.arena_exclusive);
	CHECKF(osmem_thread_arena() != NULL, "no arena bound to the calling thread");
	CHECKF(osmem_arena_owner(osmem_thread_arena()) == osmem_thread_id(),
	       "arena owner does not match the calling thread");
	os_free(p);
	CHECK_SOUND();
}

static void test_no_brk(void)
{
	void *keep_libc_heap_busy = malloc(1 << 20);
	void *before, *after;
	void *ptr[NPTR];
	int i, round;

	t_begin("Phase 1.2: the program break never moves");
	CHECK(keep_libc_heap_busy != NULL);
	/* No libc allocation may happen between the two probes. */
	before = sbrk(0);
	for (round = 0; round < 20; round++) {
		for (i = 0; i < NPTR; i++)
			ptr[i] = os_malloc(17 + i * 13);
		for (i = 0; i < NPTR; i++)
			os_free(ptr[i]);
	}
	after = sbrk(0);
	CHECKF(before == after, "sbrk(0) moved from %p to %p", before, after);
	free(keep_libc_heap_busy);
	CHECK_SOUND();
}

static void test_alignment_and_integrity(void)
{
	void *ptr[NPTR];
	size_t size[NPTR];
	int i;

	t_begin("payload alignment, usable size and isolation between blocks");
	for (i = 0; i < NPTR; i++) {
		size[i] = (size_t)(i * i) + 1;
		ptr[i] = os_malloc(size[i]);
		CHECKF(ptr[i] != NULL, "os_malloc(%zu) failed", size[i]);
		CHECK_ALIGNED(ptr[i]);
		pattern_fill(ptr[i], size[i], (uint64_t)i + 1);
	}
	for (i = 0; i < NPTR; i++)
		CHECKF(pattern_ok(ptr[i], size[i], (uint64_t)i + 1),
		       "block %d (%zu bytes) was corrupted", i, size[i]);
	CHECK_SOUND();
	for (i = 0; i < NPTR; i += 2)
		os_free(ptr[i]);
	for (i = 1; i < NPTR; i += 2)
		CHECKF(pattern_ok(ptr[i], size[i], (uint64_t)i + 1),
		       "block %d was corrupted by its neighbours being freed", i);
	CHECK_SOUND();
	for (i = 1; i < NPTR; i += 2)
		os_free(ptr[i]);
	CHECK_SOUND();
}

static void test_edge_cases(void)
{
	void *p;

	t_begin("edge cases: zero sizes, NULL, overflow, absurd sizes");
	CHECK(os_malloc(0) == NULL);
	CHECK(os_calloc(0, 16) == NULL);
	CHECK(os_calloc(16, 0) == NULL);
	os_free(NULL);

	errno = 0;
	CHECK(os_calloc((size_t)1 << 62, (size_t)1 << 62) == NULL);
	CHECKF(errno == ENOMEM, "calloc overflow set errno to %d", errno);

	errno = 0;
	CHECK(os_malloc(SIZE_MAX) == NULL);
	CHECKF(errno == ENOMEM, "os_malloc(SIZE_MAX) set errno to %d", errno);

	errno = 0;
	CHECK(os_malloc(SIZE_MAX / 2 - 4096) == NULL);
	CHECKF(errno == ENOMEM, "a hopeless request set errno to %d", errno);

	/* The allocator must still work after a failed request. */
	p = os_malloc(64);
	CHECK(p != NULL);
	os_free(p);
	CHECK_SOUND();
}

static void test_calloc(void)
{
	unsigned char *p;
	size_t i;

	t_begin("os_calloc zeroes fresh and recycled memory");
	p = os_calloc(100, 8);
	CHECK(p != NULL);
	CHECK_ALIGNED(p);
	for (i = 0; i < 800; i++)
		if (p[i] != 0)
			break;
	CHECKF(i == 800, "fresh calloc memory is dirty at offset %zu", i);

	/* Dirty it, give it back, then make sure the recycled block is zeroed. */
	memset(p, 0xa5, 800);
	os_free(p);
	p = os_calloc(100, 8);
	CHECK(p != NULL);
	for (i = 0; i < 800; i++)
		if (p[i] != 0)
			break;
	CHECKF(i == 800, "recycled calloc memory is dirty at offset %zu", i);
	os_free(p);

	/* Large (own mapping) calloc. */
	p = os_calloc(1, 3u << 20);
	CHECK(p != NULL);
	for (i = 0; i < (3u << 20); i++)
		if (p[i] != 0)
			break;
	CHECKF(i == (3u << 20), "large calloc memory is dirty at offset %zu", i);
	os_free(p);
	CHECK_SOUND();
}

static void test_realloc(void)
{
	char *p, *q;

	t_begin("os_realloc semantics");
	CHECK(os_realloc(NULL, 0) == NULL);

	p = os_realloc(NULL, 128);
	CHECKF(p != NULL, "realloc(NULL, n) must behave like malloc");
	pattern_fill(p, 128, 7);

	q = os_realloc(p, 64);			/* shrink */
	CHECK(q != NULL);
	CHECKF(pattern_ok(q, 64, 7), "shrinking realloc lost data");

	q = os_realloc(q, 4096);		/* grow */
	CHECK(q != NULL);
	CHECK_ALIGNED(q);
	CHECKF(pattern_ok(q, 64, 7), "growing realloc lost data");
	pattern_fill(q, 4096, 9);

	q = os_realloc(q, 3u << 20);		/* grow past the arena threshold */
	CHECK(q != NULL);
	CHECKF(pattern_ok(q, 4096, 9), "realloc into a private mapping lost data");
	pattern_fill(q, 3u << 20, 11);

	q = os_realloc(q, 6u << 20);		/* grow a standalone mapping */
	CHECK(q != NULL);
	CHECK_ALIGNED(q);
	CHECKF(pattern_ok(q, 3u << 20, 11), "mremap-based realloc lost data");

	q = os_realloc(q, 777);			/* back into an arena */
	CHECK(q != NULL);
	CHECKF(pattern_ok(q, 777, 11), "realloc back into an arena lost data");

	CHECK(os_realloc(q, 0) == NULL);	/* realloc(p, 0) frees */
	CHECK_SOUND();
}

static void test_standalone(void)
{
	struct osmem_stats a, b, c;
	void *p;

	t_begin("blocks too large for an arena get (and give back) their own mapping");
	osmem_stats(&a);
	p = os_malloc(a.arena_size);
	CHECK(p != NULL);
	CHECK_ALIGNED(p);
	pattern_fill(p, a.arena_size, 3);
	osmem_stats(&b);
	CHECKF(b.standalone_blocks == a.standalone_blocks + 1,
	       "standalone blocks went from %zu to %zu", a.standalone_blocks,
	       b.standalone_blocks);
	CHECKF(b.arena_count == a.arena_count,
	       "a huge request created an arena (%zu -> %zu)", a.arena_count, b.arena_count);
	CHECKF(pattern_ok(p, a.arena_size, 3), "standalone block corrupted");

	os_free(p);
	osmem_stats(&c);
	CHECKF(c.standalone_blocks == a.standalone_blocks,
	       "standalone block was not released (%zu live)", c.standalone_blocks);
	CHECKF(c.munmap_calls == b.munmap_calls + 1,
	       "freeing a standalone block made %zu munmap calls",
	       c.munmap_calls - b.munmap_calls);
	CHECK_SOUND();
}

int main(void)
{
	printf("== test_basic ==\n");
	test_lazy_start();
	test_no_brk();
	test_alignment_and_integrity();
	test_edge_cases();
	test_calloc();
	test_realloc();
	test_standalone();
	return t_report("test_basic");
}
