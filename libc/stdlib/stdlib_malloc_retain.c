/*
 * Phoenix-RTOS
 *
 * libphoenix allocator: reuse of freed heaps
 * HEADER:
 *    - stdlib.h
 *    - malloc.h
 *
 * TESTED:
 *    - malloc(), realloc(), free() keeping and reusing an entirely free heap
 *    - malloc_trim()
 *
 * libphoenix (stdlib/malloc_dl.c) gives every large request a heap of its own.
 * A heap that becomes entirely free is now kept mapped, within a size limit,
 * and reused by the next request it can serve, instead of being unmapped and
 * mapped again (KNOWN-ISSUES P26). Whether a request reused a kept heap or
 * mapped a new one is visible in mallocInfo().mapsz, the bytes the allocator
 * has mapped: comparing pointers would not do, since mmap() may well hand the
 * same address back to an allocator that did unmap.
 *
 * Every test starts from malloc_trim(0), which releases all kept heaps, so the
 * heaps left behind by earlier tests do not move the numbers.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/wait.h>
#include <unity_fixture.h>


#define KIB (1024u)
#define MIB (1024u * 1024u)

/* A 1080p frame buffer, the P26 case */
#define FRAME (3u * MIB)

/* MALLOC_RETAIN_BYTES in libphoenix stdlib/malloc_dl.c */
#define RETAIN_LIMIT (16u * MIB)

/* The allocator keeps freed heaps only on targets with an MMU (NOMMU targets
 * release them at once), and these tests map tens of MiB */
#if defined(__phoenix__) && !defined(NOMMU)
#define RETAIN_TESTS
#endif


#ifdef RETAIN_TESTS

static size_t retain_mapsz(void)
{
	mallocInfo_t info;

	mallocInfo(&info);

	return info.mapsz;
}


/* A pattern that differs per block and per page, so a block that overlaps
 * another, or a page that was swapped for a fresh one, reads back wrong. */
static void retain_fill(unsigned char *p, size_t size, unsigned int tag)
{
	size_t i;

	for (i = 0; i < size; i++) {
		p[i] = (unsigned char)((i >> 12) * 31u + tag);
	}
}


static int retain_check(const unsigned char *p, size_t size, unsigned int tag)
{
	size_t i;

	for (i = 0; i < size; i++) {
		if (p[i] != (unsigned char)((i >> 12) * 31u + tag)) {
			return 0;
		}
	}

	return 1;
}

#endif


TEST_GROUP(stdlib_malloc_retain);


TEST_SETUP(stdlib_malloc_retain)
{
#ifdef RETAIN_TESTS
	(void)malloc_trim(0);
#endif
}


TEST_TEAR_DOWN(stdlib_malloc_retain)
{
#ifdef RETAIN_TESTS
	(void)malloc_trim(0);
#endif
}


/* Freeing a frame-sized block keeps its heap mapped, and the next request of the
 * same size is served from it without mapping anything. */
TEST(stdlib_malloc_retain, freed_heap_is_reused)
{
#ifdef RETAIN_TESTS
	size_t m0, m1;
	unsigned char *p, *q;

	m0 = retain_mapsz();
	p = malloc(FRAME);
	TEST_ASSERT_NOT_NULL(p);
	retain_fill(p, FRAME, 1);
	m1 = retain_mapsz();
	TEST_ASSERT_GREATER_OR_EQUAL_size_t(m0 + FRAME, m1);
	TEST_ASSERT_TRUE(retain_check(p, FRAME, 1));
	free(p);

	/* Kept, not unmapped */
	TEST_ASSERT_EQUAL_size_t(m1, retain_mapsz());

	q = malloc(FRAME);
	TEST_ASSERT_NOT_NULL(q);
	/* Reused, not mapped again */
	TEST_ASSERT_EQUAL_size_t(m1, retain_mapsz());
	retain_fill(q, FRAME, 2);
	TEST_ASSERT_TRUE(retain_check(q, FRAME, 2));
	free(q);
#else
	TEST_IGNORE_MESSAGE("libphoenix allocator on MMU targets only");
#endif
}


/* A kept heap serves a smaller request of a different size; a request larger than
 * any kept heap maps a new one, and the whole of it is usable. */
TEST(stdlib_malloc_retain, different_size_reuse)
{
#ifdef RETAIN_TESTS
	size_t m0, m1;
	unsigned char *p, *q, *r;

	m0 = retain_mapsz();
	p = malloc(FRAME);
	TEST_ASSERT_NOT_NULL(p);
	retain_fill(p, FRAME, 3);
	free(p);
	m1 = retain_mapsz();
	TEST_ASSERT_GREATER_THAN_size_t(m0, m1);

	q = malloc(FRAME - 256u * KIB);
	TEST_ASSERT_NOT_NULL(q);
	TEST_ASSERT_EQUAL_size_t(m1, retain_mapsz());
	retain_fill(q, FRAME - 256u * KIB, 4);

	/* Does not fit the kept heap, which now holds q anyway */
	r = malloc(FRAME + MIB);
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_GREATER_OR_EQUAL_size_t(m1 + FRAME + MIB, retain_mapsz());
	retain_fill(r, FRAME + MIB, 5);

	TEST_ASSERT_TRUE(retain_check(q, FRAME - 256u * KIB, 4));
	TEST_ASSERT_TRUE(retain_check(r, FRAME + MIB, 5));
	free(q);
	free(r);
#else
	TEST_IGNORE_MESSAGE("libphoenix allocator on MMU targets only");
#endif
}


/* realloc() that has to move a large block frees the old heap, which is kept;
 * the next request of the old size reuses it, and blocks in a reused heap
 * reallocate (move and shrink) like any other. */
TEST(stdlib_malloc_retain, realloc_across_kept_heaps)
{
#ifdef RETAIN_TESTS
	size_t m1, m2;
	unsigned char *p, *q;

	p = malloc(2u * MIB);
	TEST_ASSERT_NOT_NULL(p);
	retain_fill(p, 2u * MIB, 6);
	m1 = retain_mapsz();

	/* The heap was sized for 2 MiB, so this moves: a new heap of at least 4 MiB
	 * is mapped and the old one is kept, so the mapping grows by the whole new
	 * heap (unmapping the old one would make it grow by the difference only) */
	p = realloc(p, 4u * MIB);
	TEST_ASSERT_NOT_NULL(p);
	m2 = retain_mapsz();
	TEST_ASSERT_GREATER_OR_EQUAL_size_t(m1 + 4u * MIB, m2);
	TEST_ASSERT_TRUE(retain_check(p, 2u * MIB, 6));

	/* ... and the old size reuses it */
	q = malloc(2u * MIB);
	TEST_ASSERT_NOT_NULL(q);
	TEST_ASSERT_EQUAL_size_t(m2, retain_mapsz());
	retain_fill(q, 2u * MIB, 7);

	/* Grow the block in the reused heap: it moves, its heap is kept again */
	q = realloc(q, 6u * MIB);
	TEST_ASSERT_NOT_NULL(q);
	TEST_ASSERT_TRUE(retain_check(q, 2u * MIB, 7));
	TEST_ASSERT_TRUE(retain_check(p, 2u * MIB, 6));

	/* Shrink in place, then a request that fits the heap q left behind */
	p = realloc(p, MIB);
	TEST_ASSERT_NOT_NULL(p);
	TEST_ASSERT_TRUE(retain_check(p, MIB, 6));
	m2 = retain_mapsz();
	free(malloc(2u * MIB));
	TEST_ASSERT_EQUAL_size_t(m2, retain_mapsz());

	TEST_ASSERT_TRUE(retain_check(q, 2u * MIB, 7));
	free(p);
	free(q);
#else
	TEST_IGNORE_MESSAGE("libphoenix allocator on MMU targets only");
#endif
}


/* Freeing more heaps than the limit allows keeps some, but never more than the
 * limit; malloc_trim(0) then returns exactly to the starting point. */
TEST(stdlib_malloc_retain, kept_bytes_are_bounded)
{
#ifdef RETAIN_TESTS
	enum { N = 8 }; /* 8 x 3 MiB > RETAIN_LIMIT */
	unsigned char *p[N];
	size_t m0, kept;
	unsigned int i;

	m0 = retain_mapsz();
	for (i = 0; i < N; i++) {
		p[i] = malloc(FRAME);
		TEST_ASSERT_NOT_NULL(p[i]);
		retain_fill(p[i], FRAME, 10u + i);
	}
	for (i = 0; i < N; i++) {
		TEST_ASSERT_TRUE(retain_check(p[i], FRAME, 10u + i));
		free(p[i]);
	}

	kept = retain_mapsz() - m0;
	TEST_ASSERT_GREATER_OR_EQUAL_size_t(FRAME, kept);
	TEST_ASSERT_LESS_OR_EQUAL_size_t(RETAIN_LIMIT, kept);

	TEST_ASSERT_EQUAL_INT(1, malloc_trim(0));
	TEST_ASSERT_EQUAL_size_t(m0, retain_mapsz());
#else
	TEST_IGNORE_MESSAGE("libphoenix allocator on MMU targets only");
#endif
}


/* A heap that served a block after being kept must leave the kept list. Here a
 * kept heap is reused by calloc() and then the list is pushed over its limit
 * while that block is live: a heap still listed would be the oldest entry, the
 * first one evicted, and unmapped under its owner. calloc() also has to zero
 * memory that a reused heap hands back dirty. */
TEST(stdlib_malloc_retain, reused_heap_leaves_the_list)
{
#ifdef RETAIN_TESTS
	enum { N = 6 };
	unsigned char *p, *q, *other[N];
	size_t m0, i;
	unsigned int k;

	m0 = retain_mapsz();
	p = malloc(FRAME);
	TEST_ASSERT_NOT_NULL(p);
	memset(p, 0xa5, FRAME);
	free(p);

	q = calloc(1, FRAME);
	TEST_ASSERT_NOT_NULL(q);

	for (k = 0; k < N; k++) {
		other[k] = malloc(FRAME);
		TEST_ASSERT_NOT_NULL(other[k]);
	}
	for (k = 0; k < N; k++) {
		free(other[k]);
	}

	for (i = 0; i < FRAME; i++) {
		if (q[i] != 0u) {
			break;
		}
	}
	TEST_ASSERT_EQUAL_size_t(FRAME, i);

	free(q);
	(void)malloc_trim(0);
	TEST_ASSERT_EQUAL_size_t(m0, retain_mapsz());
#else
	TEST_IGNORE_MESSAGE("libphoenix allocator on MMU targets only");
#endif
}


/* Many sizes, large and small, alternating with a window of live blocks: every
 * block keeps its contents however its memory was obtained, and the mapping
 * never grows past what is live plus the kept-heap limit. */
TEST(stdlib_malloc_retain, alternating_sizes)
{
#ifdef RETAIN_TESTS
	enum { WINDOW = 4, ROUNDS = 120 };
	static const size_t sizes[] = {
		3u * MIB, 64u, 1u * MIB + 123u, 2048u, 5u * MIB / 2u, 300u * KIB, 40u, 7u * MIB, 200u * KIB + 8u, 4096u
	};
	unsigned char *p[WINDOW] = { NULL };
	size_t sz[WINDOW] = { 0 };
	size_t m0, mapped;
	unsigned int r, w;

	m0 = retain_mapsz();
	for (r = 0; r < ROUNDS; r++) {
		w = r % WINDOW;
		if (p[w] != NULL) {
			TEST_ASSERT_TRUE(retain_check(p[w], sz[w], r - WINDOW));
			free(p[w]);
		}
		sz[w] = sizes[(r * 7u) % (sizeof(sizes) / sizeof(sizes[0]))];
		p[w] = malloc(sz[w]);
		TEST_ASSERT_NOT_NULL(p[w]);
		retain_fill(p[w], sz[w], r);

		/* Every heap mapped here either holds one of the WINDOW live blocks or is
		 * kept. A heap is at most 8 MiB + a page for these sizes (7 MiB rounds up
		 * to its size class), so this holds unless kept heaps grow past the limit. */
		mapped = retain_mapsz() - m0;
		TEST_ASSERT_LESS_OR_EQUAL_size_t(WINDOW * (8u * MIB + 4u * KIB) + RETAIN_LIMIT, mapped);
	}

	for (w = 0; w < WINDOW; w++) {
		TEST_ASSERT_TRUE(retain_check(p[w], sz[w], ROUNDS - WINDOW + w));
		free(p[w]);
	}
	(void)malloc_trim(0);
	TEST_ASSERT_EQUAL_size_t(m0, retain_mapsz());
#else
	TEST_IGNORE_MESSAGE("libphoenix allocator on MMU targets only");
#endif
}


/* The batch pattern of small blocks: once one batch has been allocated and freed,
 * the next batch is served entirely from the kept heaps. A 2 KiB block gets a
 * one-page heap to itself, so BATCH heaps are kept: BATCH must not exceed
 * MALLOC_RETAIN_MAX (64) in libphoenix stdlib/malloc_dl.c. */
TEST(stdlib_malloc_retain, small_heap_batches_reuse)
{
#ifdef RETAIN_TESTS
	enum { BATCH = 64 };
	unsigned char *p[BATCH];
	size_t m1 = 0;
	unsigned int i, round;

	for (round = 0; round < 3u; round++) {
		for (i = 0; i < BATCH; i++) {
			p[i] = malloc(2048);
			TEST_ASSERT_NOT_NULL(p[i]);
			memset(p[i], (int)(i + round), 2048);
			if (round != 0u) {
				TEST_ASSERT_EQUAL_size_t(m1, retain_mapsz());
			}
		}
		for (i = 0; i < BATCH; i++) {
			TEST_ASSERT_EACH_EQUAL_HEX8((unsigned char)(i + round), p[i], 2048);
			free(p[i]);
		}
		if (round == 0u) {
			m1 = retain_mapsz();
		}
		TEST_ASSERT_EQUAL_size_t(m1, retain_mapsz());
	}
#else
	TEST_IGNORE_MESSAGE("libphoenix allocator on MMU targets only");
#endif
}


/* malloc_trim(pad) releases kept heaps, oldest first, until at most pad bytes of
 * them remain, and says whether it released anything. */
TEST(stdlib_malloc_retain, trim_releases_kept_heaps)
{
#ifdef RETAIN_TESTS
	unsigned char *a, *b;
	size_t m0, kept;

	m0 = retain_mapsz();
	TEST_ASSERT_EQUAL_INT(0, malloc_trim(0));

	/* Two live blocks of the same size: two heaps of the same size, kept */
	a = malloc(FRAME);
	b = malloc(FRAME);
	TEST_ASSERT_NOT_NULL(a);
	TEST_ASSERT_NOT_NULL(b);
	free(a);
	free(b);
	kept = retain_mapsz() - m0;
	TEST_ASSERT_GREATER_OR_EQUAL_size_t(2u * FRAME, kept);

	/* Room for one of the two */
	TEST_ASSERT_EQUAL_INT(1, malloc_trim(kept / 2u));
	TEST_ASSERT_EQUAL_size_t(kept / 2u, retain_mapsz() - m0);

	TEST_ASSERT_EQUAL_INT(1, malloc_trim(0));
	TEST_ASSERT_EQUAL_size_t(m0, retain_mapsz());
	TEST_ASSERT_EQUAL_INT(0, malloc_trim(0));
#else
	TEST_IGNORE_MESSAGE("libphoenix allocator on MMU targets only");
#endif
}


/* A child inherits the parent's kept heaps as its own copies: it reuses one,
 * and malloc_trim(0) there releases exactly what the child had mapped, while the
 * parent's kept heap is untouched. */
TEST(stdlib_malloc_retain, fork_child_reuses_kept_heap)
{
#ifdef RETAIN_TESTS
	unsigned char *p;
	size_t m1;
	pid_t pid;
	int status;

	p = malloc(FRAME);
	TEST_ASSERT_NOT_NULL(p);
	retain_fill(p, FRAME, 30);
	free(p);
	m1 = retain_mapsz();

	pid = fork();
	TEST_ASSERT_NOT_EQUAL_INT(-1, pid);
	if (pid == 0) {
		int ok;

		p = malloc(FRAME);
		ok = (p != NULL) && (retain_mapsz() == m1);
		if (ok != 0) {
			retain_fill(p, FRAME, 31);
			ok = retain_check(p, FRAME, 31);
			free(p);
			ok = ok && (malloc_trim(0) == 1) && (retain_mapsz() < m1);
		}
		_exit((ok != 0) ? 0 : 1);
	}

	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	TEST_ASSERT_TRUE(WIFEXITED(status));
	TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

	TEST_ASSERT_EQUAL_size_t(m1, retain_mapsz());
	p = malloc(FRAME);
	TEST_ASSERT_NOT_NULL(p);
	TEST_ASSERT_EQUAL_size_t(m1, retain_mapsz());
	retain_fill(p, FRAME, 32);
	TEST_ASSERT_TRUE(retain_check(p, FRAME, 32));
	free(p);
#else
	TEST_IGNORE_MESSAGE("libphoenix allocator on MMU targets only");
#endif
}


TEST_GROUP_RUNNER(stdlib_malloc_retain)
{
	RUN_TEST_CASE(stdlib_malloc_retain, freed_heap_is_reused);
	RUN_TEST_CASE(stdlib_malloc_retain, different_size_reuse);
	RUN_TEST_CASE(stdlib_malloc_retain, realloc_across_kept_heaps);
	RUN_TEST_CASE(stdlib_malloc_retain, kept_bytes_are_bounded);
	RUN_TEST_CASE(stdlib_malloc_retain, reused_heap_leaves_the_list);
	RUN_TEST_CASE(stdlib_malloc_retain, alternating_sizes);
	RUN_TEST_CASE(stdlib_malloc_retain, small_heap_batches_reuse);
	RUN_TEST_CASE(stdlib_malloc_retain, trim_releases_kept_heaps);
	RUN_TEST_CASE(stdlib_malloc_retain, fork_child_reuses_kept_heap);
}
