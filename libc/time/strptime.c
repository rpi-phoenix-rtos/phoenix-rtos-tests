/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - time.h
 *
 * TESTED (libphoenix):
 *    - strptime() — was an unimplemented stub returning NULL (every parse
 *      failed). C-locale directives: %Y %y %m %d %e %H %M %S %j %a %A %b %B, the
 *      end-pointer return, and NULL on mismatch/out-of-range.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <time.h>
#include <string.h>

#include <unity_fixture.h>


TEST_GROUP(time_strptime);


TEST_SETUP(time_strptime)
{
}


TEST_TEAR_DOWN(time_strptime)
{
}


TEST(time_strptime, iso_date)
{
	struct tm tm;
	char *end;

	memset(&tm, 0, sizeof(tm));
	end = strptime("2026-08-21", "%Y-%m-%d", &tm);
	TEST_ASSERT_NOT_NULL(end);
	TEST_ASSERT_EQUAL_INT('\0', *end);
	TEST_ASSERT_EQUAL_INT(126, tm.tm_year); /* 2026 - 1900 */
	TEST_ASSERT_EQUAL_INT(7, tm.tm_mon);    /* August = index 7 */
	TEST_ASSERT_EQUAL_INT(21, tm.tm_mday);
}


TEST(time_strptime, datetime)
{
	struct tm tm;

	memset(&tm, 0, sizeof(tm));
	TEST_ASSERT_NOT_NULL(strptime("2026-08-21 13:45:07", "%Y-%m-%d %H:%M:%S", &tm));
	TEST_ASSERT_EQUAL_INT(13, tm.tm_hour);
	TEST_ASSERT_EQUAL_INT(45, tm.tm_min);
	TEST_ASSERT_EQUAL_INT(7, tm.tm_sec);
	TEST_ASSERT_EQUAL_INT(126, tm.tm_year);
}


TEST(time_strptime, names)
{
	struct tm tm;

	/* full weekday + full month */
	memset(&tm, 0, sizeof(tm));
	TEST_ASSERT_NOT_NULL(strptime("Friday August 2026", "%A %B %Y", &tm));
	TEST_ASSERT_EQUAL_INT(5, tm.tm_wday); /* Friday (Sun=0) */
	TEST_ASSERT_EQUAL_INT(7, tm.tm_mon);
	TEST_ASSERT_EQUAL_INT(126, tm.tm_year);

	/* abbreviated weekday + month */
	memset(&tm, 0, sizeof(tm));
	TEST_ASSERT_NOT_NULL(strptime("Mon Jan", "%a %b", &tm));
	TEST_ASSERT_EQUAL_INT(1, tm.tm_wday); /* Monday */
	TEST_ASSERT_EQUAL_INT(0, tm.tm_mon);  /* January */
}


TEST(time_strptime, two_digit_year_and_endptr)
{
	struct tm tm;
	char *end;

	/* %y 26 -> 2026 (tm_year 126); trailing text returned via the end pointer */
	memset(&tm, 0, sizeof(tm));
	end = strptime("08/21/26 rest", "%m/%d/%y", &tm);
	TEST_ASSERT_NOT_NULL(end);
	TEST_ASSERT_EQUAL_INT(126, tm.tm_year);
	TEST_ASSERT_EQUAL_INT(7, tm.tm_mon);
	TEST_ASSERT_EQUAL_INT(21, tm.tm_mday);
	TEST_ASSERT_EQUAL_INT(0, strncmp(end, " rest", 5));

	/* %y 70 -> 1970 (tm_year 70) */
	memset(&tm, 0, sizeof(tm));
	TEST_ASSERT_NOT_NULL(strptime("70", "%y", &tm));
	TEST_ASSERT_EQUAL_INT(70, tm.tm_year);
}


TEST(time_strptime, mismatch_returns_null)
{
	struct tm tm;

	memset(&tm, 0, sizeof(tm));
	TEST_ASSERT_NULL(strptime("2026/08/21", "%Y-%m-%d", &tm)); /* literal mismatch */
	memset(&tm, 0, sizeof(tm));
	TEST_ASSERT_NULL(strptime("13", "%m", &tm));               /* month out of range */
	memset(&tm, 0, sizeof(tm));
	TEST_ASSERT_NULL(strptime("xx", "%H", &tm));               /* non-numeric */
}


TEST_GROUP_RUNNER(time_strptime)
{
	RUN_TEST_CASE(time_strptime, iso_date);
	RUN_TEST_CASE(time_strptime, datetime);
	RUN_TEST_CASE(time_strptime, names);
	RUN_TEST_CASE(time_strptime, two_digit_year_and_endptr);
	RUN_TEST_CASE(time_strptime, mismatch_returns_null);
}
