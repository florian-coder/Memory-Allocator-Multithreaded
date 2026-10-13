/* SPDX-License-Identifier: BSD-3-Clause */

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "osmem.h"
#include "osmem_debug.h"

/* osmem.h pulls in the heap-free printf of the framework, which hijacks the
 * printf family through macros.  The tests want the real stdio.
 */
#undef printf
#undef sprintf
#undef snprintf
#undef vsnprintf
#undef vprintf

/* The alignment os_malloc() guarantees for every allocation. */
#define BLOCK_ALIGN_EXPECTED	16

extern int t_checks;
extern int t_failures;
extern const char *t_section;

void t_begin(const char *section);
int t_report(const char *suite);

#define CHECKF(cond, ...)							\
	do {									\
		t_checks++;							\
		if (!(cond)) {							\
			t_failures++;						\
			fprintf(stderr, "  FAIL [%s] %s:%d: %s\n",		\
				t_section, __FILE__, __LINE__, #cond);		\
			fprintf(stderr, "        ");				\
			fprintf(stderr, __VA_ARGS__);				\
			fprintf(stderr, "\n");					\
		}								\
	} while (0)

#define CHECK(cond)	CHECKF(cond, "(no detail)")

/*
 * Some counters (bytes_used, blocks_*, bytes_touched) are informational and
 * are compiled out by -DOSMEM_NO_STATS.  Assertions about them are skipped in
 * that build so the suite stays meaningful in both.
 */
int t_stats_available(void);

#define CHECK_STATF(cond, ...)							\
	do {									\
		if (t_stats_available())					\
			CHECKF(cond, __VA_ARGS__);				\
	} while (0)

/* The allocator must stay structurally sound after every phase. */
#define CHECK_SOUND()								\
	do {									\
		int errs = osmem_check();					\
									\
		CHECKF(errs == 0, "osmem_check() reported %d violation(s)", errs);\
	} while (0)

#define CHECK_ALIGNED(p)							\
	CHECKF(((uintptr_t)(p) & 15) == 0, "pointer %p is not 16-byte aligned", (p))

/* Deterministic PRNG so failures reproduce. */
struct rng {
	uint64_t s;
};

static inline void rng_seed(struct rng *r, uint64_t seed)
{
	r->s = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

static inline uint64_t rng_next(struct rng *r)
{
	uint64_t x = r->s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	r->s = x;
	return x;
}

static inline size_t rng_size(struct rng *r, size_t lo, size_t hi)
{
	return lo + (size_t)(rng_next(r) % (hi - lo + 1));
}

/* Write a pointer-and-size specific pattern, so that any overlap between two
 * live blocks shows up as a mismatch.
 */
void pattern_fill(void *p, size_t n, uint64_t seed);
int pattern_ok(const void *p, size_t n, uint64_t seed);

/* Number of mappings the process currently holds (one line of /proc/self/maps). */
int count_vmas(void);

/*
 * Look up the mapping that contains addr in /proc/self/maps.  Returns 0 when
 * there is none.  The kernel is free to merge adjacent anonymous mappings, so
 * tests count the distinct ranges rather than one per arena.
 */
struct vma {
	unsigned long long lo, hi;
	char perm[8];
	int anonymous;
};

int vma_lookup(const void *addr, struct vma *out);

/* Counting variant: records a check and reports the failure itself. */
int vma_require(const void *addr, struct vma *out, const char *file, int line);

#define CHECK_VMA(addr, v)	vma_require((addr), (v), __FILE__, __LINE__)

uint64_t now_ns(void);

/* Resident set size of this process, in KiB (0 if unavailable). */
size_t rss_kb(void);
