/* SPDX-License-Identifier: BSD-3-Clause */

#pragma once

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "printf.h"

#define DIE(assertion, call_description)									\
	do {													\
		if (assertion) {										\
			fprintf(stderr, "(%s, %d): ", __FILE__, __LINE__);					\
			perror(call_description);								\
			exit(errno);										\
		}												\
	} while (0)

struct arena;

/*
 * Per-allocation metadata -- the "hidden header".
 * It sits immediately before the pointer handed back to the caller, so that
 * os_free() can recover both the size of the block and the arena it was carved
 * from, no matter which thread does the freeing.
 *
 * Layout (16 bytes, so a 16-byte aligned header yields a 16-byte aligned
 * payload):
 *
 *   units : total block size, header included, in BLOCK_ALIGN units.  32 bits
 *           are enough for blocks up to 64 GiB.  Never written while the block
 *           is in use, so its owner may read it without acquiring anything.
 *   flags : the bits below, plus a tag.  This word *is* shared: coalescing a
 *           block means flipping BLOCK_PREV_FREE in the header of the *next*
 *           block, which may well be in use by another thread -- and that
 *           thread reads the same word to check the tag before it knows which
 *           arena to acquire.  Hence _Atomic, with relaxed accesses: the flag
 *           updates become a single read-modify-write, and the size bits in
 *           the neighbouring field are untouched by them.
 *   arena : arena the block belongs to, NULL for standalone mappings.
 *           Written once at carve time and never mutated afterwards, which is
 *           what lets any thread find the owner of a block it is freeing.
 */
struct block_meta {
	uint32_t units;
	_Atomic uint32_t flags;
	struct arena *arena;
};

/* Payload alignment and the resulting header size. */
#define BLOCK_ALIGN		16UL
#define BLOCK_META_SIZE		(sizeof(struct block_meta))

/* The largest block the header can describe. */
#define BLOCK_MAX_SIZE		(((size_t)UINT32_MAX - 1) * BLOCK_ALIGN)

#define BLOCK_FREE		0x1U	/* this block sits in a free list      */
#define BLOCK_PREV_FREE		0x2U	/* the physically previous one is free */
#define BLOCK_STANDALONE	0x4U	/* block owns a private mmap()         */
#define BLOCK_SHIM		0x8U	/* over-aligned redirect, see os_memalign */

/*
 * Only the low half of the flags word carries state; the upper half holds a
 * fixed tag.  Checking it costs a shift and a compare on a word the free path
 * already touches, and it is what lets os_free() recognise a pointer that
 * this allocator never handed out -- which matters most when the library is
 * interposed over libc's malloc and is handed foreign pointers.
 */
#define BLOCK_FLAG_BITS		0x0000ffffU
#define BLOCK_TAG		0x05ae0000U

/*
 * Free blocks carry two extra pieces of bookkeeping inside their payload:
 *   - a struct free_node at the very beginning (free-list links),
 *   - a size footer in the last 4 bytes (boundary tag, used to find the
 *     physically previous block when coalescing backwards).
 *
 * Both are 32-bit: the links are offsets from the base of the owning arena
 * rather than pointers (an arena is far smaller than 4 GiB, and offset 0 can
 * never be a block because the arena header sits there, so 0 doubles as NULL),
 * and the footer stores the size in BLOCK_ALIGN units.  That is what brings
 * the smallest block down to 32 bytes; with 64-bit links and an 8-byte footer
 * it would be 48, i.e. 50% more overhead on every small allocation.
 */
struct free_node {
	uint32_t prev;
	uint32_t next;
};

#define BLOCK_FOOTER_SIZE	(sizeof(uint32_t))
#define BLOCK_MIN_SIZE		32UL	/* 16 header + 8 links + 4 footer -> 16-aligned */
