/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 / C11 standard library functions tests
 *
 * HEADER:
 *    - stdlib.h, malloc.h
 *
 * TESTED:
 *    - posix_memalign()
 *    - aligned_alloc()
 *    - memalign()
 *
 * None of the three existed. Worse, meson's probe for posix_memalign answered
 * YES anyway (it is a gcc builtin, so __has_builtin passes), so a port that
 * trusted the probe failed at link time. They are now backed by the allocator
 * itself (the leading slack becomes a free chunk), so the result can be passed
 * to free() and realloc() like any other block. The cases check alignment,
 * usable size, the POSIX error contract, and that the carved blocks leave the
 * heap coherent for ordinary malloc/free afterwards.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <unity_fixture.h>


#define MEMALIGN_BLOCKS 64


static int memalign_isAligned(const void *p, size_t alignment)
{
	return (((uintptr_t)p & (alignment - 1u)) == 0u) ? 1 : 0;
}


static void memalign_fill(unsigned char *p, size_t n, unsigned char tag)
{
	size_t i;

	for (i = 0; i < n; i++) {
		p[i] = (unsigned char)(tag ^ (unsigned char)(i * 31u));
	}
}


static int memalign_check(const unsigned char *p, size_t n, unsigned char tag)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (p[i] != (unsigned char)(tag ^ (unsigned char)(i * 31u))) {
			return 0;
		}
	}
	return 1;
}


TEST_GROUP(stdlib_memalign);


TEST_SETUP(stdlib_memalign)
{
}


TEST_TEAR_DOWN(stdlib_memalign)
{
}


TEST(stdlib_memalign, posix_memalign_alignments)
{
	static const size_t sizes[] = { 0, 1, 7, 8, 24, 100, 240, 241, 4000, 4096, 70000 };
	size_t alignment, i;

	for (alignment = sizeof(void *); alignment <= 65536u; alignment <<= 1) {
		for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
			void *p = NULL;

			TEST_ASSERT_EQUAL_INT(0, posix_memalign(&p, alignment, sizes[i]));
			TEST_ASSERT_NOT_NULL(p);
			TEST_ASSERT_TRUE_MESSAGE(memalign_isAligned(p, alignment), "misaligned result");
			TEST_ASSERT_TRUE(malloc_usable_size(p) >= sizes[i]);
			memset(p, 0xa5, sizes[i]); /* the whole block must be writable */
			free(p);
		}
	}
}


TEST(stdlib_memalign, posix_memalign_einval)
{
	static const size_t bad[] = { 0, 1, 2, 3, 12, 24, 48, 100, sizeof(void *) + 1 };
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		void *p = (void *)&p;

		errno = 1234;
		TEST_ASSERT_EQUAL_INT(EINVAL, posix_memalign(&p, bad[i], 16));
		/* reported through the return value: errno and *memptr untouched */
		TEST_ASSERT_EQUAL_INT(1234, errno);
		TEST_ASSERT_TRUE(p == (void *)&p);
	}

	/* A power of two below sizeof(void *) is not allowed either. */
	if (sizeof(void *) > 4u) {
		void *p = NULL;
		TEST_ASSERT_EQUAL_INT(EINVAL, posix_memalign(&p, 4, 16));
	}
}


TEST(stdlib_memalign, posix_memalign_enomem)
{
	void *p = (void *)&p;

	TEST_ASSERT_EQUAL_INT(ENOMEM, posix_memalign(&p, 64, SIZE_MAX - 4096u));
	TEST_ASSERT_TRUE(p == (void *)&p);
	TEST_ASSERT_EQUAL_INT(ENOMEM, posix_memalign(&p, (size_t)1 << 20, SIZE_MAX / 2u));
	TEST_ASSERT_TRUE(p == (void *)&p);
}


TEST(stdlib_memalign, aligned_alloc_and_memalign)
{
	size_t alignment;

	for (alignment = 1; alignment <= 16384u; alignment <<= 1) {
		unsigned char *a = aligned_alloc(alignment, 3u * alignment);
		unsigned char *m = memalign(alignment, 100);

		TEST_ASSERT_NOT_NULL(a);
		TEST_ASSERT_NOT_NULL(m);
		TEST_ASSERT_TRUE(memalign_isAligned(a, alignment));
		TEST_ASSERT_TRUE(memalign_isAligned(m, alignment));
		memset(a, 1, 3u * alignment);
		memset(m, 2, 100);
		free(m);
		free(a);
	}

	/* not a power of two */
	errno = 0;
	TEST_ASSERT_NULL(aligned_alloc(24, 48));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
	errno = 0;
	TEST_ASSERT_NULL(aligned_alloc(0, 16));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}


TEST(stdlib_memalign, realloc_keeps_contents)
{
	unsigned char *p;
	void *v = NULL;

	TEST_ASSERT_EQUAL_INT(0, posix_memalign(&v, 4096, 300));
	p = v;
	memalign_fill(p, 300, 0x3c);

	/* grow (may move, alignment then no longer promised) and shrink */
	p = realloc(p, 20000);
	TEST_ASSERT_NOT_NULL(p);
	TEST_ASSERT_TRUE(memalign_check(p, 300, 0x3c));
	p = realloc(p, 100);
	TEST_ASSERT_NOT_NULL(p);
	TEST_ASSERT_TRUE(memalign_check(p, 100, 0x3c));
	free(p);
}


/* Interleave aligned and plain blocks, free them in an order that makes the
 * carved leading chunks coalesce with their neighbours, and check every
 * surviving block after each step: a bad header or footer written by the
 * aligned path shows up as corrupted data in a neighbour. */
TEST(stdlib_memalign, mixed_with_malloc_heap_stays_coherent)
{
	unsigned char *blk[MEMALIGN_BLOCKS];
	size_t len[MEMALIGN_BLOCKS];
	int i, j, pass;

	for (pass = 0; pass < 3; pass++) {
		for (i = 0; i < MEMALIGN_BLOCKS; i++) {
			len[i] = 16u + (size_t)((i * 37 + pass * 101) % 900);
			if ((i % 2) == 0) {
				size_t alignment = (size_t)16 << (i % 9); /* 16 .. 4096 */
				void *v = NULL;
				TEST_ASSERT_EQUAL_INT(0, posix_memalign(&v, alignment, len[i]));
				blk[i] = v;
				TEST_ASSERT_TRUE(memalign_isAligned(blk[i], alignment));
			}
			else {
				blk[i] = malloc(len[i]);
				TEST_ASSERT_NOT_NULL(blk[i]);
			}
			memalign_fill(blk[i], len[i], (unsigned char)(i + pass));
		}

		/* free every third, then the rest, checking survivors each time */
		for (j = 0; j < 3; j++) {
			for (i = j; i < MEMALIGN_BLOCKS; i += 3) {
				TEST_ASSERT_TRUE_MESSAGE(memalign_check(blk[i], len[i], (unsigned char)(i + pass)), "block corrupted");
				free(blk[i]);
				blk[i] = NULL;
			}
			for (i = 0; i < MEMALIGN_BLOCKS; i++) {
				if (blk[i] != NULL) {
					TEST_ASSERT_TRUE_MESSAGE(memalign_check(blk[i], len[i], (unsigned char)(i + pass)), "survivor corrupted");
				}
			}
		}
	}
}


TEST_GROUP_RUNNER(stdlib_memalign)
{
	RUN_TEST_CASE(stdlib_memalign, posix_memalign_alignments);
	RUN_TEST_CASE(stdlib_memalign, posix_memalign_einval);
	RUN_TEST_CASE(stdlib_memalign, posix_memalign_enomem);
	RUN_TEST_CASE(stdlib_memalign, aligned_alloc_and_memalign);
	RUN_TEST_CASE(stdlib_memalign, realloc_keeps_contents);
	RUN_TEST_CASE(stdlib_memalign, mixed_with_malloc_heap_stays_coherent);
}
