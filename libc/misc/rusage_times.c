/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - sys/resource.h (getrusage)
 *    - sys/times.h (times)
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/times.h>

#include <unity_fixture.h>


TEST_GROUP(misc_rusage_times);


TEST_SETUP(misc_rusage_times)
{
}


TEST_TEAR_DOWN(misc_rusage_times)
{
}


TEST(misc_rusage_times, getrusage_defines_out_param)
{
	struct rusage ru;

	/* Poison the struct: getrusage must overwrite it with defined (zeroed)
	 * values, not leave the caller's stack garbage (was `return 0` untouched). */
	memset(&ru, 0xaa, sizeof(ru));
	TEST_ASSERT_EQUAL_INT(0, getrusage(RUSAGE_SELF, &ru));
	TEST_ASSERT_EQUAL_INT(0, (int)ru.ru_utime.tv_sec);
	TEST_ASSERT_EQUAL_INT(0, (int)ru.ru_utime.tv_usec);
}


TEST(misc_rusage_times, getrusage_null_efault)
{
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, getrusage(RUSAGE_SELF, NULL));
	TEST_ASSERT_EQUAL_INT(EFAULT, errno);
}


TEST(misc_rusage_times, times_returns_defined)
{
	struct tms tb;
	clock_t t1, t2;

	/* Poison; times() must zero the CPU breakdown (was left undefined). */
	memset(&tb, 0xaa, sizeof(tb));
	t1 = times(&tb);
	TEST_ASSERT_NOT_EQUAL_INT((clock_t)-1, t1);
	/* Pins the headline of the fix: real elapsed ticks, not the old `return 0`
	 * stub. By the time this runs, monotonic uptime is tens of seconds (>>0). */
	TEST_ASSERT_GREATER_THAN_INT(0, (int)t1);
	TEST_ASSERT_EQUAL_INT(0, (int)tb.tms_utime);
	TEST_ASSERT_EQUAL_INT(0, (int)tb.tms_stime);
	TEST_ASSERT_EQUAL_INT(0, (int)tb.tms_cutime);
	TEST_ASSERT_EQUAL_INT(0, (int)tb.tms_cstime);

	/* Elapsed real time is monotonic non-decreasing (was a stub returning 0,
	 * so every elapsed measurement read 0). */
	t2 = times(&tb);
	TEST_ASSERT_NOT_EQUAL_INT((clock_t)-1, t2);
	TEST_ASSERT_TRUE(t2 >= t1);

	/* NULL buffer is valid for times(): it still returns the tick count. */
	TEST_ASSERT_NOT_EQUAL_INT((clock_t)-1, times(NULL));
}


TEST_GROUP_RUNNER(misc_rusage_times)
{
	RUN_TEST_CASE(misc_rusage_times, getrusage_defines_out_param);
	RUN_TEST_CASE(misc_rusage_times, getrusage_null_efault);
	RUN_TEST_CASE(misc_rusage_times, times_returns_defined);
}
