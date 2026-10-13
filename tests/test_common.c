// SPDX-License-Identifier: BSD-3-Clause

#include <time.h>
#include <inttypes.h>

#include "test_common.h"

int t_checks;
int t_failures;
const char *t_section = "init";

int t_stats_available(void)
{
	struct osmem_stats st;

	osmem_stats(&st);
	return st.has_stats;
}

void t_begin(const char *section)
{
	t_section = section;
	printf("  - %s\n", section);
	fflush(stdout);
}

int t_report(const char *suite)
{
	if (t_failures == 0) {
		printf("PASS %s (%d checks)\n", suite, t_checks);
		return 0;
	}
	printf("FAIL %s (%d/%d checks failed)\n", suite, t_failures, t_checks);
	return 1;
}

void pattern_fill(void *p, size_t n, uint64_t seed)
{
	unsigned char *q = p;
	size_t i;

	for (i = 0; i < n; i++)
		q[i] = (unsigned char)(seed * 31 + i * 7 + (i >> 8));
}

int pattern_ok(const void *p, size_t n, uint64_t seed)
{
	const unsigned char *q = p;
	size_t i;

	for (i = 0; i < n; i++)
		if (q[i] != (unsigned char)(seed * 31 + i * 7 + (i >> 8)))
			return 0;
	return 1;
}

int count_vmas(void)
{
	char buf[4096];
	FILE *f = fopen("/proc/self/maps", "r");
	int n = 0;

	if (!f)
		return -1;
	while (fgets(buf, sizeof(buf), f))
		n++;
	fclose(f);
	return n;
}

int vma_lookup(const void *addr, struct vma *out)
{
	unsigned long long want = (unsigned long long)(uintptr_t)addr;
	char buf[4096];
	FILE *f = fopen("/proc/self/maps", "r");
	int found = 0;

	if (!f)
		return 0;
	while (fgets(buf, sizeof(buf), f)) {
		unsigned long long lo, hi;
		char perm[8];

		if (sscanf(buf, "%llx-%llx %7s", &lo, &hi, perm) != 3)
			continue;
		if (want < lo || want >= hi)
			continue;
		out->lo = lo;
		out->hi = hi;
		memcpy(out->perm, perm, sizeof(out->perm));
		out->anonymous = strchr(buf, '/') == NULL;
		found = 1;
		break;
	}
	fclose(f);
	return found;
}

int vma_require(const void *addr, struct vma *out, const char *file, int line)
{
	t_checks++;
	if (vma_lookup(addr, out))
		return 1;
	t_failures++;
	fprintf(stderr, "  FAIL [%s] %s:%d: no mapping contains %p\n",
		t_section, file, line, addr);
	return 0;
}

size_t rss_kb(void)
{
	FILE *f = fopen("/proc/self/statm", "r");
	unsigned long total = 0, resident = 0;
	size_t page = 4096;

	if (!f)
		return 0;
	if (fscanf(f, "%lu %lu", &total, &resident) != 2)
		resident = 0;
	fclose(f);
	return (size_t)resident * page / 1024;
}

uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
