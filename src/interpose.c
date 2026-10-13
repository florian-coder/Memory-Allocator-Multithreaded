// SPDX-License-Identifier: BSD-3-Clause
/*
 * interpose.c -- the libc malloc family, implemented on top of this allocator.
 *
 * Built into a separate libosmem_preload.so, so that linking libosmem.so does
 * not silently replace a program's allocator.  With
 *
 *     LD_PRELOAD=./libosmem_preload.so some_program
 *
 * every allocation the program and libc make goes through the arenas.
 *
 * Three details matter for a program that does not know it is being
 * experimented on:
 *
 *   - malloc(0) must return a pointer that can be freed, not NULL: a lot of
 *     code treats NULL as failure.  os_malloc(0) returns NULL by contract, so
 *     the wrapper asks for one byte instead.
 *   - free() can be handed a pointer this allocator never produced.  It
 *     happens for memory the dynamic loader allocated before anything was
 *     interposed, and os_free() recognises it by the tag in the block header
 *     and ignores it, which leaks a little rather than corrupting memory.
 *   - malloc_usable_size() has to be right, because callers size their writes
 *     with it.
 */

#define _GNU_SOURCE

#include <malloc.h>	/* so the compiler checks these against libc's prototypes */
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "osmem.h"

#define EXPORT __attribute__((visibility("default")))

static size_t page_size(void)
{
	static size_t cached;

	if (!cached) {
		long p = sysconf(_SC_PAGESIZE);

		cached = p > 0 ? (size_t)p : 4096;
	}
	return cached;
}

EXPORT void *malloc(size_t size)
{
	return os_malloc(size ? size : 1);
}

EXPORT void free(void *ptr)
{
	os_free(ptr);
}

EXPORT void *calloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
		return os_malloc(1);
	return os_calloc(nmemb, size);
}

EXPORT void *realloc(void *ptr, size_t size)
{
	if (!ptr)
		return os_malloc(size ? size : 1);
	return os_realloc(ptr, size);	/* size 0 frees and returns NULL */
}

EXPORT void *reallocarray(void *ptr, size_t nmemb, size_t size)
{
	if (nmemb && size > SIZE_MAX / nmemb)
		return NULL;
	return realloc(ptr, nmemb * size);
}

EXPORT void *memalign(size_t alignment, size_t size)
{
	return os_memalign(alignment, size ? size : 1);
}

EXPORT int posix_memalign(void **out, size_t alignment, size_t size)
{
	return os_posix_memalign(out, alignment, size);
}

EXPORT void *aligned_alloc(size_t alignment, size_t size)
{
	return os_aligned_alloc(alignment, size ? size : 1);
}

EXPORT void *valloc(size_t size)
{
	return os_memalign(page_size(), size ? size : 1);
}

EXPORT void *pvalloc(size_t size)
{
	size_t page = page_size();
	size_t rounded = (size + page - 1) & ~(page - 1);

	return os_memalign(page, rounded ? rounded : page);
}

EXPORT size_t malloc_usable_size(void *ptr)
{
	return os_usable_size(ptr);
}

EXPORT int malloc_trim(size_t pad)
{
	(void)pad;
	return os_trim() > 0;
}
