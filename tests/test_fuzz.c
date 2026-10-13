// SPDX-License-Identifier: BSD-3-Clause
/*
 * Fuzz harness.
 *
 * The input bytes are read as a little program -- allocate, free, reallocate,
 * align, trim, check -- so a fuzzer explores size and ordering combinations
 * that a hand-written stress test never would.  Every live block carries a
 * pattern derived from its slot, so any overlap or stale reuse shows up
 * immediately, and the structural checker runs along the way.
 *
 * It is written against the libFuzzer entry point, but also carries its own
 * deterministic driver so it is useful with a plain gcc build:
 *
 *   make test              -- runs OSMEM_FUZZ_RUNS (default 400) random programs
 *   ./tests/test_fuzz f1 f2 -- replays saved inputs, to reproduce a failure
 *
 * A failing program is written to /tmp/osmem_fuzz_fail_<n>.bin so it can be
 * replayed and shrunk.
 */

#include <unistd.h>

#include "test_common.h"

#define SLOTS 64
#define MAX_SIZE 8192

struct block {
	void *p;
	size_t size;		/* what the caller asked for */
	uint64_t seed;
	size_t align;
	int zeroed;		/* came from calloc and has not been written since */
};

static struct block slot[SLOTS];
static unsigned long ops_run;
static unsigned long checks_run;

static void fail(const char *what, int op)
{
	fprintf(stderr, "fuzz: %s (operation %d)\n", what, op);
	abort();
}

static void verify(int i, int op)
{
	if (!slot[i].p)
		return;
	if (((uintptr_t)slot[i].p & (slot[i].align - 1)) != 0)
		fail("alignment lost", op);
	if (os_usable_size(slot[i].p) < slot[i].size)
		fail("usable size below the request", op);
	if (slot[i].zeroed) {
		const unsigned char *q = slot[i].p;
		size_t k;

		for (k = 0; k < slot[i].size; k++)
			if (q[k])
				fail("calloc memory was not zero", op);
	} else if (!pattern_ok(slot[i].p, slot[i].size, slot[i].seed)) {
		fail("block contents changed", op);
	}
}

static void release(int i)
{
	os_free(slot[i].p);
	memset(&slot[i], 0, sizeof(slot[i]));
}

/* Runs one program.  Returns the number of operations executed. */
static unsigned long run_program(const unsigned char *in, size_t len)
{
	size_t at = 0;
	int op = 0;
	int i;

	while (at + 4 <= len) {
		unsigned char code = in[at++];
		int k = in[at++] % SLOTS;
		size_t arg = (size_t)in[at] | ((size_t)in[at + 1] << 8);

		at += 2;
		op++;
		ops_run++;
		verify(k, op);

		switch (code % 8) {
		case 0:				/* malloc */
		case 1:
			if (slot[k].p)
				release(k);
			slot[k].size = (arg % MAX_SIZE) + 1;
			slot[k].p = os_malloc(slot[k].size);
			slot[k].align = BLOCK_ALIGN_EXPECTED;
			slot[k].seed = (uint64_t)k * 7919 + op;
			if (slot[k].p)
				pattern_fill(slot[k].p, slot[k].size, slot[k].seed);
			else
				slot[k].size = 0;
			break;
		case 2: {			/* calloc */
			size_t n = (arg % 64) + 1;
			size_t s = (arg % 97) + 1;

			if (slot[k].p)
				release(k);
			slot[k].p = os_calloc(n, s);
			slot[k].size = slot[k].p ? n * s : 0;
			slot[k].align = BLOCK_ALIGN_EXPECTED;
			slot[k].zeroed = slot[k].p != NULL;
			break;
		}
		case 3: {			/* realloc */
			size_t want = (arg % MAX_SIZE) + 1;
			void *np;

			if (!slot[k].p) {
				slot[k].size = want;
				slot[k].seed = (uint64_t)k * 7919 + op;
				slot[k].align = BLOCK_ALIGN_EXPECTED;
				slot[k].p = os_malloc(want);
				if (slot[k].p)
					pattern_fill(slot[k].p, want, slot[k].seed);
				else
					slot[k].size = 0;
				break;
			}
			np = os_realloc(slot[k].p, want);
			if (!np)
				break;		/* the old block is still ours */
			slot[k].p = np;
			/* realloc() does not promise to keep a stricter alignment,
			 * here or in any other allocator.
			 */
			slot[k].align = BLOCK_ALIGN_EXPECTED;
			if (slot[k].zeroed) {
				/* only the old span is guaranteed zero */
				if (want < slot[k].size)
					slot[k].size = want;
			} else {
				size_t keep = want < slot[k].size ? want : slot[k].size;

				if (!pattern_ok(np, keep, slot[k].seed))
					fail("realloc lost data", op);
				slot[k].size = want;
				pattern_fill(np, want, slot[k].seed);
			}
			break;
		}
		case 4:				/* free */
			release(k);
			break;
		case 5: {			/* over-aligned allocation */
			size_t align = (size_t)1 << (4 + (arg % 10));	/* 16 .. 8192 */

			if (slot[k].p)
				release(k);
			slot[k].size = (arg % 1024) + 1;
			slot[k].p = os_memalign(align, slot[k].size);
			slot[k].align = align;
			slot[k].seed = (uint64_t)k * 7919 + op;
			if (slot[k].p)
				pattern_fill(slot[k].p, slot[k].size, slot[k].seed);
			else
				slot[k].size = 0;
			break;
		}
		case 6:				/* give pages back */
			os_trim();
			break;
		case 7:				/* structural check */
			if (osmem_check() != 0)
				fail("the allocator is inconsistent", op);
			checks_run++;
			break;
		}
	}

	for (i = 0; i < SLOTS; i++) {
		verify(i, op);
		release(i);
	}
	if (osmem_check() != 0)
		fail("the allocator is inconsistent at the end of the program", op);
	checks_run++;
	return (unsigned long)op;
}

/* libFuzzer entry point: build with clang -fsanitize=fuzzer. */
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
	run_program(data, size);
	return 0;
}

#ifndef OSMEM_LIBFUZZER

static int replay(const char *path)
{
	static unsigned char buf[1 << 16];
	FILE *f = fopen(path, "rb");
	size_t n;

	if (!f) {
		fprintf(stderr, "fuzz: cannot open %s\n", path);
		return 1;
	}
	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	printf("  - replaying %s (%zu bytes)\n", path, n);
	run_program(buf, n);
	return 0;
}

int main(int argc, char **argv)
{
	static unsigned char program[4096];
	struct osmem_stats st;
	unsigned long runs = 400;
	struct rng rng;
	const char *env;
	unsigned long i;
	int j;

	printf("== test_fuzz ==\n");
	if (argc > 1) {
		for (j = 1; j < argc; j++)
			if (replay(argv[j]))
				return 1;
		printf("PASS test_fuzz (replayed %d input(s))\n", argc - 1);
		return 0;
	}

	env = getenv("OSMEM_FUZZ_RUNS");
	if (env && *env)
		runs = strtoul(env, NULL, 10);
	env = getenv("OSMEM_FUZZ_SEED");
	rng_seed(&rng, env && *env ? strtoull(env, NULL, 10) : 20261012);

	t_begin("random programs of allocate / free / realloc / align / trim");
	for (i = 0; i < runs; i++) {
		size_t len = 64 + (size_t)(rng_next(&rng) % (sizeof(program) - 64));
		size_t k;

		for (k = 0; k < len; k++)
			program[k] = (unsigned char)rng_next(&rng);
		run_program(program, len);
	}

	osmem_drain_all();
	osmem_stats(&st);
	CHECK_STATF(st.bytes_used == 0, "%zu bytes still in use after %lu programs",
		    st.bytes_used, runs);
	CHECKF(st.standalone_blocks == 0, "%zu standalone blocks leaked",
	       st.standalone_blocks);
	CHECK_SOUND();
	printf("      (%lu programs, %lu operations, %lu structural checks)\n",
	       runs, ops_run, checks_run);
	return t_report("test_fuzz");
}

#endif /* OSMEM_LIBFUZZER */
