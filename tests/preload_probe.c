// SPDX-License-Identifier: BSD-3-Clause
/*
 * A perfectly ordinary program: it uses libc's malloc family, knows nothing
 * about this project, and is built without it on purpose.  LD_PRELOAD is what
 * swaps the allocator underneath it, so if this prints the same checksum both
 * ways the interposition is honest.
 */

#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 4
#define SLOTS 256
#define OPS 60000

static unsigned long long total;
static pthread_mutex_t total_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned xorshift(unsigned *s)
{
	unsigned x = *s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

static void *worker(void *arg)
{
	int id = (int)(long)arg;
	char *slot[SLOTS] = { 0 };
	size_t size[SLOTS] = { 0 };
	unsigned seed = 1234u + (unsigned)id;
	unsigned long long sum = 0;
	int i, op;

	for (op = 0; op < OPS; op++) {
		unsigned k = xorshift(&seed) % SLOTS;
		unsigned what = xorshift(&seed) % 10;

		if (!slot[k]) {
			size[k] = 1 + xorshift(&seed) % 4000;
			if (what == 0) {
				slot[k] = calloc(1, size[k]);
				if (slot[k] && slot[k][0] != 0)
					sum += 0xdead;		/* calloc not zeroed */
			} else if (what == 1) {
				if (posix_memalign((void **)&slot[k], 256, size[k]) != 0)
					slot[k] = NULL;
				if (slot[k] && ((size_t)slot[k] & 255))
					sum += 0xbeef;		/* misaligned */
			} else {
				slot[k] = malloc(size[k]);
			}
			if (!slot[k]) {
				sum += 0xfa11;		/* allocation failed */
				continue;
			}
			memset(slot[k], (int)(k & 0xff), size[k]);
			if (malloc_usable_size(slot[k]) < size[k])
				sum += 0xbad;			/* lying usable size */
		} else if (what < 3) {
			size_t want = 1 + xorshift(&seed) % 4000;
			char *p = realloc(slot[k], want);

			if (!p) {
				sum += 0xfa11;		/* allocation failed */
			} else {
				size_t keep = want < size[k] ? want : size[k];
				size_t j;

				for (j = 0; j < keep; j++)
					if ((unsigned char)p[j] != (k & 0xff))
						sum += 1;	/* realloc lost data */
				slot[k] = p;
				size[k] = want;
				memset(p, (int)(k & 0xff), want);
			}
		} else {
			size_t j;

			for (j = 0; j < size[k]; j++)
				if ((unsigned char)slot[k][j] != (k & 0xff))
					sum += 1;		/* corruption */
			free(slot[k]);
			slot[k] = NULL;
		}
		sum += size[k] & 0xf;
	}
	for (i = 0; i < SLOTS; i++)
		free(slot[i]);

	pthread_mutex_lock(&total_lock);
	total += sum;
	pthread_mutex_unlock(&total_lock);
	return NULL;
}

int main(void)
{
	pthread_t th[THREADS];
	char *strings[1000];
	int i;

	/* libc's own allocations: strdup, asprintf, stdio buffers. */
	for (i = 0; i < 1000; i++) {
		char buf[64];

		snprintf(buf, sizeof(buf), "string number %d", i);
		strings[i] = strdup(buf);
		if (!strings[i])
			return 2;
	}
	for (i = 0; i < 1000; i++) {
		if (strncmp(strings[i], "string number", 13) != 0)
			return 3;
		free(strings[i]);
	}

	for (i = 0; i < THREADS; i++)
		pthread_create(&th[i], NULL, worker, (void *)(long)i);
	for (i = 0; i < THREADS; i++)
		pthread_join(th[i], NULL);

	printf("probe checksum %llu\n", total);
	return 0;
}
