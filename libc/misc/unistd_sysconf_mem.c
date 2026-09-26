/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests (common extension)
 *
 * HEADER:
 *    - unistd.h (sysconf)
 *
 * TESTED:
 *    - sysconf(_SC_PHYS_PAGES), sysconf(_SC_AVPHYS_PAGES)
 *
 * Both were missing, so Mesa's os_get_total_physical_memory() could not size
 * anything by RAM (the old GPU lane's compat header defined the NAME and let
 * the call fail). libphoenix now answers from the kernel page allocator via
 * meminfo(). The cases check the values are real rather than merely positive:
 * consistent with each other and with the page size, and -- on Phoenix, where
 * the system is quiet enough to measure -- the free count drops when this
 * process touches fresh memory.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <unity_fixture.h>


TEST_GROUP(unistd_sysconf_mem);


TEST_SETUP(unistd_sysconf_mem)
{
}


TEST_TEAR_DOWN(unistd_sysconf_mem)
{
}


TEST(unistd_sysconf_mem, values_are_sane)
{
	long phys, avphys, pagesz;
	char msg[96];

	errno = 0;
	phys = sysconf(_SC_PHYS_PAGES);
	TEST_ASSERT_EQUAL_INT(0, errno);
	avphys = sysconf(_SC_AVPHYS_PAGES);
	TEST_ASSERT_EQUAL_INT(0, errno);
	pagesz = sysconf(_SC_PAGESIZE);

	snprintf(msg, sizeof(msg), "phys=%ld avphys=%ld pages of %ld bytes", phys, avphys, pagesz);
	TEST_MESSAGE(msg);

	TEST_ASSERT_GREATER_THAN_INT(0, pagesz);
	TEST_ASSERT_GREATER_THAN_INT(0, phys);
	TEST_ASSERT_GREATER_THAN_INT(0, avphys);
	TEST_ASSERT_TRUE(avphys <= phys);

	/* Pages, not bytes or KiB: a system able to run this test has at least
	 * 1 MiB of RAM, and far less than 2^40 pages. */
	TEST_ASSERT_TRUE((unsigned long long)phys * (unsigned long long)pagesz >= (1ULL << 20));
	TEST_ASSERT_TRUE((unsigned long long)phys < (1ULL << 40));
}


TEST(unistd_sysconf_mem, total_is_stable)
{
	/* The amount of RAM does not change while we run. */
	TEST_ASSERT_TRUE(sysconf(_SC_PHYS_PAGES) == sysconf(_SC_PHYS_PAGES));
}


TEST(unistd_sysconf_mem, free_count_tracks_allocation)
{
#ifdef __phoenix__
	const size_t len = 16u << 20;
	long pagesz = sysconf(_SC_PAGESIZE);
	long before, after;
	unsigned char *p;
	size_t i;

	before = sysconf(_SC_AVPHYS_PAGES);
	if ((unsigned long long)before * (unsigned long long)pagesz < 4ULL * len) {
		TEST_IGNORE_MESSAGE("not enough free RAM to measure");
	}

	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	TEST_ASSERT_TRUE(p != MAP_FAILED);
	for (i = 0; i < len; i += (size_t)pagesz) {
		p[i] = 1; /* make every page resident */
	}

	after = sysconf(_SC_AVPHYS_PAGES);
	munmap(p, len);

	/* 16 MiB just became resident: the free count must fall by at least half
	 * of that even if something else freed memory meanwhile. A constant or
	 * made-up value fails here. */
	TEST_ASSERT_TRUE(before - after >= (long)((len / 2u) / (size_t)pagesz));
#else
	TEST_IGNORE_MESSAGE("free-page accounting is too noisy to measure on a shared host");
#endif
}


TEST_GROUP_RUNNER(unistd_sysconf_mem)
{
	RUN_TEST_CASE(unistd_sysconf_mem, values_are_sane);
	RUN_TEST_CASE(unistd_sysconf_mem, total_is_stable);
	RUN_TEST_CASE(unistd_sysconf_mem, free_count_tracks_allocation);
}
