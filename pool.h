/*
 * pool.h - reader for the file pool attached by the GRUB "wrap" module
 * (https://github.com/end0832/grub-wrap)
 *
 * Copyright (c) 2026 End0832_
 * SPDX-License-Identifier: MIT (text in LICENSES/MIT.txt)
 *
 * A single function, get_pool_info(), decodes the pool placed by "wrap" right
 * above the root task image and returns a PoolInfo describing every attached
 * file.  Nothing is copied: names and contents point into the pool itself.
 * The header is freestanding: it needs no C library, only <stdbool.h>,
 * <stddef.h>, <stdint.h> and the linker-provided symbol _end.
 *
 * Limits (pool format version 1):
 *   - a file is at most 4 GiB - 1 bytes
 *   - at most 64 files
 *   - a file name is at most 255 bytes
 *
 * Example:
 *
 *     #include "pool.h"
 *
 *     PoolInfo pool = get_pool_info();
 *     if (!pool.ok) {
 *         printf("pool error: %s\n", pool.error);
 *     } else {
 *         for (int i = 0; i < pool.nb; i++)
 *             printf("%d: %s (%u bytes at %p)\n", i, pool.name[i],
 *                    (unsigned) pool.size[i], (const void *) pool.start[i]);
 *     }
 * For more informations, please check the README.MD file.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POOL_MAX 64

typedef struct PoolInfo {
	uint8_t nb;                      /* number of files */
	bool ok;                         /* false if the pool could not be decoded */
	const char *error;               /* reason when ok is false */
	uint32_t size[POOL_MAX];         /* file sizes in bytes */
	const uint8_t *start[POOL_MAX];  /* first byte of each file */
	const char *name[POOL_MAX];      /* NUL-terminated names, as given to wrap */
} PoolInfo;

#undef POOL_MAX

static inline PoolInfo get_pool_info(void)
{
	extern char _end;                /* end of the root task image */
	const size_t name_max = 255;
	PoolInfo pi = {0};
	const uint8_t *root, *p;
	uint8_t width;

#define POOL_FAIL(msg) do { \
		pi.ok = false; \
		pi.error = (msg); \
		pi.nb = 0; \
		return pi; \
	} while (0)

	pi.ok = true;

	/* The pool starts on the first page boundary after the image. */
	root = (const uint8_t *)(((uintptr_t)&_end + 4095) & ~(uintptr_t)4095);

	/* Header: "POOL", version, size width, file count. */
	if (root[0] != 'P' || root[1] != 'O' || root[2] != 'O' || root[3] != 'L')
		POOL_FAIL("pool signature not found");
	if (root[4] != 1)
		POOL_FAIL("unsupported pool version");
	width = root[5];
	if (width < 1 || width > 4)
		POOL_FAIL("invalid size width");
	pi.nb = root[6];
	if (pi.nb > sizeof pi.size / sizeof pi.size[0])
		POOL_FAIL("too many files");

	/* Table of contents: for each file, a big-endian size then a name. */
	p = root + 7;
	for (uint8_t i = 0; i < pi.nb; i++) {
		uint32_t n = 0;
		size_t len = 0;

		for (uint8_t j = 0; j < width; j++)
			n = (n << 8) | p[j];
		p += width;

		/* The name ends at a NUL within name_max bytes. */
		while (len <= name_max && p[len] != 0)
			len++;
		if (len > name_max)
			POOL_FAIL("unterminated or oversized file name");

		pi.size[i] = n;
		pi.name[i] = (const char *)p;
		p += len + 1;
	}

	/* File contents follow the table, back to back. */
	for (uint8_t i = 0; i < pi.nb; i++) {
		pi.start[i] = p;
		p += pi.size[i];
	}

#undef POOL_FAIL
	return pi;
}