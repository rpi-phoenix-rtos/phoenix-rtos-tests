/*
 * Phoenix-RTOS
 *
 * Phoenix-specific virtual-to-physical address query
 *
 * HEADER:
 *    - sys/mman.h
 *
 * TESTED:
 *    - va2pa()
 *
 * va2pa() is the only call in userspace reach that answers "is this page mapped
 * RIGHT NOW?". It is the va2pa syscall, i.e. pmap_resolve(), which returns 0 for
 * an invalid page-table descriptor, so 0 means "no translation" rather than
 * "physical page zero".
 *
 * These tests exist because libphoenix's allocator now DEPENDS on that contract.
 * On 2026-09-25 a malloc guard dereferenced a heap-tracking entry that named an
 * unmapped page and killed /bin/ntpclient with a Data Abort at far=0x5000; the
 * fix makes the allocator probe with va2pa() first. Nothing tested that contract,
 * so a change to it would silently reintroduce the fault rather than fail here.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdint.h>
#include <sys/mman.h>

#include <unity_fixture.h>


#define VA2PA_PAGE 4096


TEST_GROUP(va2pa);


TEST_SETUP(va2pa)
{
}


TEST_TEAR_DOWN(va2pa)
{
}


/* A page that has actually been written has a translation. The write matters:
 * an anonymous mapping may be demand-paged, so a freshly mmap()ed page that
 * nobody has touched is not required to resolve yet. Touch it first, then the
 * answer must be non-zero. */
TEST(va2pa, mapped_page_resolves)
{
	volatile char *p = mmap(NULL, VA2PA_PAGE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, (void *)p);
	p[0] = 'x';
	TEST_ASSERT_NOT_EQUAL_UINT64(0, (uint64_t)va2pa((void *)p));
	TEST_ASSERT_EQUAL_INT(0, munmap((void *)p, VA2PA_PAGE));
}


/* THE property the allocator relies on: once a region is unmapped, va2pa() on it
 * must report 0 rather than a stale translation. If this ever regresses, every
 * "is it still mapped?" probe in malloc_dl.c silently starts answering yes and
 * dereferences a dead page. */
TEST(va2pa, unmapped_page_resolves_to_zero)
{
	volatile char *p = mmap(NULL, VA2PA_PAGE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, (void *)p);
	p[0] = 'x';
	TEST_ASSERT_NOT_EQUAL_UINT64(0, (uint64_t)va2pa((void *)p));

	TEST_ASSERT_EQUAL_INT(0, munmap((void *)p, VA2PA_PAGE));
	TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)va2pa((void *)p));
}


/* NULL has no translation. Cheap, and it is the degenerate case every caller
 * passes sooner or later.
 *
 * This case is only safe to assert because page 0 really is unmapped for a user
 * process on this target -- if some port deliberately mapped it (a few do, to
 * catch NULL derefs), pmap_resolve() would walk a valid descriptor and return
 * non-zero, and this test would go red on a correct system. The evidence it is
 * not mapped here: a Data Abort with esr=0x92000047 far=0x0 is in the archive
 * (a write to NULL from /bin/ntpclient, 2026-09-11) -- a WRITE to address 0 took
 * a translation fault, which it could not do if page 0 were mapped writable. */
TEST(va2pa, null_resolves_to_zero)
{
	TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)va2pa(NULL));
}


/* The offset within the page is carried through untouched -- the syscall is
 * (pmap_resolve(va & ~0xfff) & ~0xfff) + (va & 0xfff). Two addresses in the same
 * page therefore differ in their result by exactly their virtual difference.
 * This is what makes it legitimate to probe with a page-aligned base and reason
 * about the whole page. */
TEST(va2pa, offset_within_page_is_preserved)
{
	volatile char *p = mmap(NULL, VA2PA_PAGE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	uint64_t base, off;

	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, (void *)p);
	p[0] = 'x';
	p[64] = 'y';

	base = (uint64_t)va2pa((void *)p);
	off = (uint64_t)va2pa((void *)(p + 64));
	TEST_ASSERT_NOT_EQUAL_UINT64(0, base);
	TEST_ASSERT_EQUAL_UINT64(base + 64u, off);

	/* ...and both name the same physical page. */
	TEST_ASSERT_EQUAL_UINT64(base & ~(uint64_t)(VA2PA_PAGE - 1),
			off & ~(uint64_t)(VA2PA_PAGE - 1));

	TEST_ASSERT_EQUAL_INT(0, munmap((void *)p, VA2PA_PAGE));
}


/* ⚠ THE FOOTGUN, pinned down by a test so nobody has to rediscover it.
 *
 * va2pa() is (pmap_resolve(va & ~0xfff) & ~0xfff) + (va & 0xfff). On an UNMAPPED
 * page pmap_resolve gives 0, so the result is 0 + the in-page offset -- which is
 * NON-ZERO for any address that is not page-aligned. A caller probing "is this
 * mapped?" with an unaligned pointer therefore gets a truthy answer for memory
 * that is not mapped at all, and only finds out by faulting on the next read.
 *
 * Every mapping probe must page-align first. libphoenix's allocator does, and
 * this test is what keeps that from silently regressing. */
TEST(va2pa, unmapped_unaligned_returns_the_offset_not_zero)
{
	volatile char *p = mmap(NULL, VA2PA_PAGE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, (void *)p);
	p[0] = 'x';
	TEST_ASSERT_EQUAL_INT(0, munmap((void *)p, VA2PA_PAGE));

	/* Page-aligned: honest 0. */
	TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)va2pa((void *)p));
	/* Same dead page, offset 64: returns 64, NOT 0. */
	TEST_ASSERT_EQUAL_UINT64(64u, (uint64_t)va2pa((void *)(p + 64)));
}


TEST_GROUP_RUNNER(va2pa)
{
	RUN_TEST_CASE(va2pa, mapped_page_resolves);
	RUN_TEST_CASE(va2pa, unmapped_page_resolves_to_zero);
	RUN_TEST_CASE(va2pa, null_resolves_to_zero);
	RUN_TEST_CASE(va2pa, offset_within_page_is_preserved);
	RUN_TEST_CASE(va2pa, unmapped_unaligned_returns_the_offset_not_zero);
}
