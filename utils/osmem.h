/* SPDX-License-Identifier: BSD-3-Clause */

#pragma once

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include "printf.h"

void *os_malloc(size_t size);
void os_free(void *ptr);
void *os_calloc(size_t nmemb, size_t size);
void *os_realloc(void *ptr, size_t size);

/*
 * Give the kernel back the physical pages the allocator is holding but not
 * using, without releasing any of its mappings -- so the number of mappings
 * stays inside the arena budget while the resident footprint drops.  Returns
 * the number of bytes released.
 *
 * Arenas that fall completely idle are trimmed automatically once their
 * footprint passes OSMEM_TRIM_THRESHOLD (256 KiB by default); call this when
 * the process knows it has just finished a burst.
 */
size_t os_trim(void);

/*
 * Allocation with a stricter alignment than the 16 bytes os_malloc() always
 * gives (cache lines, pages, DMA buffers).  `alignment` must be a power of
 * two; os_posix_memalign() additionally requires a multiple of sizeof(void *)
 * and reports errors through its return value instead of errno.
 */
void *os_memalign(size_t alignment, size_t size);
int os_posix_memalign(void **out, size_t alignment, size_t size);
void *os_aligned_alloc(size_t alignment, size_t size);

/*
 * How many bytes are actually usable at `ptr` -- at least what was asked for,
 * and often more, because requests are rounded up to the block grid.  Returns
 * 0 for NULL or for a pointer this allocator did not hand out.
 */
size_t os_usable_size(void *ptr);
