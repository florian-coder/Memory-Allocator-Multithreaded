// SPDX-License-Identifier: BSD-3-Clause
/*
 * Giving physical pages back to the kernel.
 *
 * By design, arena *mappings* are never released -- that is what
 * keeps the mapping count inside A_max.  But holding the mapping does not
 * require holding the pages: MADV_DONTNEED drops the physical memory while the
 * mapping stays, so a process that had one burst does not sit at its peak RSS
 * forever.  These tests check that the resident footprint really drops, that
 * nothing live is damaged by it, and that the "untouched memory reads as zero"
 * invariant that os_calloc() depends on survives a trim.
 */

#include <unistd.h>

#include "test_common.h"

/*
 * A sanitizer keeps shadow memory proportional to what the program touches, and
 * that shadow is not ours to release, so the process-wide RSS ratio stops being
 * a fair measure under one.  The primary assertion is therefore on the bytes
 * the allocator itself reports releasing; the RSS check corroborates it in a
 * clean build.
 */
#ifndef __has_feature
#define __has_feature(x) 0
#endif

/* gcc defines __SANITIZE_*__, clang only answers __has_feature(). */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
	__has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#else
#define SANITIZED 0
#endif

#define BLOCK (16 * 1024)
#define COUNT 400

static void *live[COUNT];

static void test_automatic_trim_when_idle(void)
{
	struct osmem_stats before, peak, after;
	size_t rss_start, rss_peak, rss_end;
	size_t grew, gave_back, released;
	int i;

	t_begin("an arena that falls idle gives its pages back by itself");
	osmem_stats(&before);
	rss_start = rss_kb();

	for (i = 0; i < COUNT; i++) {
		live[i] = os_malloc(BLOCK);
		CHECK(live[i] != NULL);
		pattern_fill(live[i], BLOCK, (uint64_t)i + 1);
	}
	osmem_stats(&peak);
	rss_peak = rss_kb();
	CHECK_STATF(peak.bytes_touched >= (size_t)COUNT * BLOCK,
	       "only %zu bytes were faulted in for %d KiB of blocks",
	       peak.bytes_touched, COUNT * BLOCK / 1024);

	for (i = 0; i < COUNT; i++) {
		CHECKF(pattern_ok(live[i], BLOCK, (uint64_t)i + 1), "block %d corrupted", i);
		os_free(live[i]);
		live[i] = NULL;
	}
	osmem_drain_all();		/* nothing may be held back for this question */
	osmem_stats(&after);
	rss_end = rss_kb();

	released = after.bytes_released - before.bytes_released;
	CHECKF(released > 0, "no pages were released when the arenas fell idle");
	CHECKF(released * 10 >= (size_t)COUNT * BLOCK * 9,
	       "only %zu KiB of the %d KiB touched came back", released / 1024,
	       COUNT * BLOCK / 1024);
	CHECK_STATF(after.bytes_touched == 0,
	       "%zu bytes still counted as faulted in", after.bytes_touched);
	CHECKF(after.arena_count == peak.arena_count,
	       "trimming changed the arena count (%zu -> %zu)", peak.arena_count,
	       after.arena_count);
	CHECKF(after.bytes_mapped == peak.bytes_mapped,
	       "trimming released a mapping (%zu -> %zu bytes mapped)",
	       peak.bytes_mapped, after.bytes_mapped);

	grew = rss_peak > rss_start ? rss_peak - rss_start : 0;
	gave_back = rss_peak > rss_end ? rss_peak - rss_end : 0;
	if (!SANITIZED) {
		CHECKF(grew > 1024,
		       "RSS only grew by %zu KiB, the test is not measuring anything",
		       grew);
		CHECKF(gave_back * 100 >= grew * 60,
		       "RSS grew by %zu KiB and only %zu KiB came back", grew,
		       gave_back);
	}
	printf("      (RSS %zu -> %zu -> %zu KiB; %zu KiB released%s)\n",
	       rss_start, rss_peak, rss_end, released / 1024,
	       SANITIZED ? ", RSS ratio not checked under a sanitizer" : "");
	CHECK_SOUND();
}

static void test_explicit_trim_keeps_live_data(void)
{
	struct osmem_stats before, after;
	size_t released;
	int i;

	t_begin("os_trim() releases the holes and leaves live blocks untouched");
	for (i = 0; i < COUNT; i++) {
		live[i] = os_malloc(BLOCK);
		CHECK(live[i] != NULL);
		pattern_fill(live[i], BLOCK, (uint64_t)i + 1);
	}
	/* Free every other block, so the holes cannot coalesce and the arena
	 * stays half full: the whole-arena shortcut does not apply.
	 */
	for (i = 0; i < COUNT; i += 2) {
		os_free(live[i]);
		live[i] = NULL;
	}

	osmem_stats(&before);
	released = os_trim();
	osmem_stats(&after);

	CHECKF(released > 0, "os_trim() released nothing although %zu bytes are free",
	       before.bytes_free);
	CHECKF(after.bytes_free == before.bytes_free,
	       "trimming changed the free lists (%zu -> %zu)", before.bytes_free,
	       after.bytes_free);
	CHECK_STATF(after.bytes_used == before.bytes_used,
	       "trimming changed the live set (%zu -> %zu)", before.bytes_used,
	       after.bytes_used);
	CHECK_SOUND();

	/* The crucial part: nothing live may have been released. */
	for (i = 1; i < COUNT; i += 2)
		CHECKF(pattern_ok(live[i], BLOCK, (uint64_t)i + 1),
		       "live block %d was damaged by os_trim()", i);

	/* And the holes are still usable. */
	for (i = 0; i < COUNT; i += 2) {
		live[i] = os_malloc(BLOCK);
		CHECKF(live[i] != NULL, "could not reuse hole %d after trimming", i);
		if (live[i])
			pattern_fill(live[i], BLOCK, (uint64_t)i + 1);
	}
	for (i = 0; i < COUNT; i++) {
		if (live[i]) {
			CHECKF(pattern_ok(live[i], BLOCK, (uint64_t)i + 1),
			       "block %d corrupted after reuse", i);
			os_free(live[i]);
			live[i] = NULL;
		}
	}
	printf("      (%zu KiB released from a half-full arena)\n", released / 1024);
	CHECK_SOUND();
}

static void test_calloc_after_trim(void)
{
	unsigned char *p;
	size_t i, n = 512 * 1024;

	t_begin("os_calloc() still returns zeroed memory after a trim");
	/* Dirty a large span, free it (which trims), then ask for it back as
	 * zeroed memory: the fast path assumes untouched arena memory reads as
	 * zero, and MADV_DONTNEED is what has to make that true again.
	 */
	p = os_malloc(n);
	CHECK(p != NULL);
	memset(p, 0xa5, n);
	os_free(p);
	os_trim();			/* flushes anything parked, then releases */

	p = os_calloc(1, n);
	CHECK(p != NULL);
	for (i = 0; i < n; i++)
		if (p[i] != 0)
			break;
	CHECKF(i == n, "calloc returned dirty memory at offset %zu after a trim", i);
	os_free(p);
	CHECK_SOUND();
}

static void test_trim_is_idempotent(void)
{
	size_t first, second;
	void *p;

	t_begin("trimming twice releases nothing the second time");
	p = os_malloc(512 * 1024);
	CHECK(p != NULL);
	memset(p, 1, 512 * 1024);
	os_free(p);
	first = os_trim();
	second = os_trim();
	CHECKF(second == 0, "the second trim released another %zu bytes", second);
	(void)first;
	CHECK_SOUND();
}

int main(void)
{
	printf("== test_trim ==\n");
	test_automatic_trim_when_idle();
	test_explicit_trim_keeps_live_data();
	test_calloc_after_trim();
	test_trim_is_idempotent();
	return t_report("test_trim");
}
