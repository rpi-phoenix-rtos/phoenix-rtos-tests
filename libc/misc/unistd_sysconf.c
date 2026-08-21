/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - unistd.h (sysconf)
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <unistd.h>

#include <unity_fixture.h>


TEST_GROUP(unistd_sysconf);


TEST_SETUP(unistd_sysconf)
{
}


TEST_TEAR_DOWN(unistd_sysconf)
{
}


TEST(unistd_sysconf, supported_names)
{
	/* _SC_CLK_TCK: libphoenix returns the conventional fixed 100 (was -1/EINVAL);
	 * software such as CPython's _Py_GetTicksPerSecond needs it > 0. */
	TEST_ASSERT_EQUAL_INT(100, (int)sysconf(_SC_CLK_TCK));

	TEST_ASSERT_GREATER_THAN_INT(0, sysconf(_SC_PAGESIZE));
	TEST_ASSERT_GREATER_THAN_INT(0, sysconf(_SC_OPEN_MAX));
}


TEST(unistd_sysconf, nprocessors)
{
	long onln = sysconf(_SC_NPROCESSORS_ONLN);
	long conf = sysconf(_SC_NPROCESSORS_CONF);

	/* Was unimplemented (-1/EINVAL). Now returns the kernel SMP core count via
	 * platformctl(pctl_cpucount). Must be a positive, sane count. */
	TEST_ASSERT_GREATER_THAN_INT(0, onln);
	TEST_ASSERT_GREATER_THAN_INT(0, conf);
	TEST_ASSERT_TRUE(onln <= conf);
	/* RPi4 (BCM2711) is a 4-core Cortex-A72; Phoenix runs 4-core SMP. */
	TEST_ASSERT_EQUAL_INT(4, (int)onln);
}


TEST_GROUP_RUNNER(unistd_sysconf)
{
	RUN_TEST_CASE(unistd_sysconf, supported_names);
	RUN_TEST_CASE(unistd_sysconf, nprocessors);
}
