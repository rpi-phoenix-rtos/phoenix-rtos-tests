/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - sys/resource.h
 *
 * TESTED:
 *    - getrlimit(), setrlimit() + the RLIMIT_* identifiers
 *
 * Phoenix does not enforce per-process resource limits, so getrlimit() reports
 * every resource as RLIM_INFINITY (and must initialise *rlp, not leave it
 * garbage while returning success).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <sys/resource.h>

#include <unity_fixture.h>


TEST_GROUP(resource_limits);


TEST_SETUP(resource_limits)
{
}


TEST_TEAR_DOWN(resource_limits)
{
}


TEST(resource_limits, getrlimit_reports_unlimited)
{
	struct rlimit rl;

	/* poison so a stub that returns 0 without writing is caught */
	rl.rlim_cur = 12345;
	rl.rlim_max = 12345;

	TEST_ASSERT_EQUAL_INT(0, getrlimit(RLIMIT_DATA, &rl));
	TEST_ASSERT_EQUAL_INT(RLIM_INFINITY, rl.rlim_cur);
	TEST_ASSERT_EQUAL_INT(RLIM_INFINITY, rl.rlim_max);
}


TEST(resource_limits, getrlimit_all_resources)
{
	struct rlimit rl;
	int res[] = { RLIMIT_CORE, RLIMIT_STACK, RLIMIT_NOFILE, RLIMIT_DATA,
		RLIMIT_AS, RLIMIT_FSIZE, RLIMIT_CPU, RLIMIT_RSS, RLIMIT_NPROC,
		RLIMIT_MEMLOCK };
	size_t i;

	for (i = 0; i < sizeof(res) / sizeof(res[0]); i++) {
		rl.rlim_cur = 7;
		TEST_ASSERT_EQUAL_INT(0, getrlimit(res[i], &rl));
		TEST_ASSERT_EQUAL_INT(RLIM_INFINITY, rl.rlim_cur);
	}
}


TEST(resource_limits, setrlimit_accepts)
{
	struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };

	TEST_ASSERT_EQUAL_INT(0, setrlimit(RLIMIT_DATA, &rl));
}


TEST_GROUP_RUNNER(resource_limits)
{
	RUN_TEST_CASE(resource_limits, getrlimit_reports_unlimited);
	RUN_TEST_CASE(resource_limits, getrlimit_all_resources);
	RUN_TEST_CASE(resource_limits, setrlimit_accepts);
}
