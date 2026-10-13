// SPDX-License-Identifier: BSD-3-Clause
/*
 * Over-aligned allocation and os_usable_size().
 *
 * os_malloc() always returns 16-byte aligned memory, which covers every C
 * type; cache lines, pages and DMA buffers need more.  Those requests are
 * served out of an over-sized ordinary block with a second "shim" header in
 * front of the aligned payload, so the interesting part of these tests is that
 * free(), realloc() and usable_size() all still find their way back to the
 * real block, and that nothing leaks.
 */

#include "test_common.h"

static const size_t alignments[] = { 32, 64, 128, 256, 512, 4096, 16384, 65536 };
#define NALIGN (sizeof(alignments) / sizeof(alignments[0]))

static void test_alignment_is_honoured(void)
{
	size_t sizes[] = { 1, 7, 16, 17, 100, 1000, 4096, 100000 };
	size_t i, j;

	t_begin("every requested alignment is honoured, for every size");
	for (i = 0; i < NALIGN; i++) {
		for (j = 0; j < sizeof(sizes) / sizeof(sizes[0]); j++) {
			void *p = os_memalign(alignments[i], sizes[j]);

			CHECKF(p != NULL, "os_memalign(%zu, %zu) failed", alignments[i],
			       sizes[j]);
			if (!p)
				continue;
			CHECKF(((uintptr_t)p & (alignments[i] - 1)) == 0,
			       "os_memalign(%zu, %zu) returned %p", alignments[i],
			       sizes[j], p);
			CHECKF(os_usable_size(p) >= sizes[j],
			       "usable size %zu is below the %zu bytes requested",
			       os_usable_size(p), sizes[j]);
			pattern_fill(p, sizes[j], (uint64_t)(i * 100 + j) + 1);
			CHECKF(pattern_ok(p, sizes[j], (uint64_t)(i * 100 + j) + 1),
			       "aligned block (%zu, %zu) corrupted", alignments[i],
			       sizes[j]);
			os_free(p);
		}
	}
	CHECK_SOUND();
}

static void test_no_leak(void)
{
	struct osmem_stats before, after;
	void *p[256];
	size_t i;

	t_begin("over-aligned blocks are fully reclaimed");
	osmem_drain_all();
	osmem_stats(&before);
	for (i = 0; i < 256; i++) {
		p[i] = os_memalign(alignments[i % NALIGN], 64 + i);
		CHECK(p[i] != NULL);
		if (p[i])
			pattern_fill(p[i], 64 + i, (uint64_t)i + 1);
	}
	for (i = 0; i < 256; i++) {
		if (!p[i])
			continue;
		CHECKF(pattern_ok(p[i], 64 + i, (uint64_t)i + 1), "block %zu corrupted", i);
		os_free(p[i]);
	}
	osmem_drain_all();
	osmem_stats(&after);
	CHECK_STATF(after.bytes_used == before.bytes_used,
	       "%zu bytes leaked by over-aligned allocations",
	       after.bytes_used - before.bytes_used);
	CHECKF(after.standalone_blocks == before.standalone_blocks,
	       "%zu standalone blocks leaked",
	       after.standalone_blocks - before.standalone_blocks);
	CHECK_SOUND();
}

static void test_posix_memalign(void)
{
	void *p = NULL;

	t_begin("os_posix_memalign reports errors the POSIX way");
	CHECKF(os_posix_memalign(&p, 64, 100) == 0, "a valid request failed");
	CHECKF(p != NULL && ((uintptr_t)p & 63) == 0, "returned %p for alignment 64", p);
	os_free(p);

	CHECKF(os_posix_memalign(&p, 0, 100) == EINVAL, "alignment 0 was accepted");
	CHECKF(os_posix_memalign(&p, 24, 100) == EINVAL,
	       "a non-power-of-two alignment was accepted");
	CHECKF(os_posix_memalign(&p, 4, 100) == EINVAL,
	       "an alignment below sizeof(void *) was accepted");
	CHECKF(os_posix_memalign(NULL, 64, 100) == EINVAL, "a NULL result pointer was accepted");

	/* os_memalign uses errno instead. */
	errno = 0;
	CHECK(os_memalign(24, 100) == NULL);
	CHECKF(errno == EINVAL, "os_memalign set errno to %d", errno);

	p = os_aligned_alloc(128, 256);
	CHECKF(p != NULL && ((uintptr_t)p & 127) == 0, "os_aligned_alloc returned %p", p);
	os_free(p);
	CHECK_SOUND();
}

static void test_realloc_of_aligned_block(void)
{
	char *p, *q;

	t_begin("realloc of an over-aligned block keeps the data");
	p = os_memalign(4096, 1000);
	CHECK(p != NULL);
	CHECK(((uintptr_t)p & 4095) == 0);
	pattern_fill(p, 1000, 77);

	q = os_realloc(p, 5000);
	CHECKF(q != NULL, "realloc of an aligned block failed");
	CHECKF(pattern_ok(q, 1000, 77), "realloc of an aligned block lost data");
	CHECKF(os_usable_size(q) >= 5000, "usable size %zu after growing to 5000",
	       os_usable_size(q));

	q = os_realloc(q, 100);
	CHECKF(q != NULL, "shrinking failed");
	CHECKF(pattern_ok(q, 100, 77), "shrinking lost data");
	os_free(q);
	CHECK_SOUND();
}

static void test_usable_size(void)
{
	void *p;

	t_begin("os_usable_size covers every kind of block");
	CHECKF(os_usable_size(NULL) == 0, "NULL reported a usable size");

	p = os_malloc(1);
	CHECKF(os_usable_size(p) == 16, "a minimum block reports %zu usable bytes",
	       os_usable_size(p));
	os_free(p);

	p = os_malloc(1000);			/* rounded up to 1008 + 16 */
	CHECKF(os_usable_size(p) >= 1000 && os_usable_size(p) < 1000 + 32,
	       "a 1000-byte request reports %zu usable bytes", os_usable_size(p));
	os_free(p);

	p = os_malloc(4u << 20);		/* own mapping */
	CHECKF(os_usable_size(p) >= (4u << 20), "a huge block reports %zu usable bytes",
	       os_usable_size(p));
	os_free(p);

	p = os_memalign(4096, 500);
	CHECKF(os_usable_size(p) >= 500, "an aligned block reports %zu usable bytes",
	       os_usable_size(p));
	os_free(p);
	CHECK_SOUND();
}

int main(void)
{
	printf("== test_align ==\n");
	test_alignment_is_honoured();
	test_no_leak();
	test_posix_memalign();
	test_realloc_of_aligned_block();
	test_usable_size();
	return t_report("test_align");
}
