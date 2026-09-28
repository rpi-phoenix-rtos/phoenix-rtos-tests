/*
 * Phoenix-RTOS
 *
 * test-libc-time
 *
 * Tests of POSIX TZ strings (POSIX.1-2017, XBD 8.3): tzset(), localtime_r(),
 * mktime(), ctime_r() and strftime() %Z %z.
 * Expected values from Python's zoneinfo (Europe/Warsaw, Australia/Sydney,
 * America/New_York, which follow these rules in 2026) and glibc.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "unity_fixture.h"


#define TZ_CET "CET-1CEST,M3.5.0,M10.5.0/3"


static void setTZ(const char *tz)
{
	TEST_ASSERT_EQUAL_INT(0, setenv("TZ", tz, 1));
	tzset();
}


static void assertLocal(time_t t, int year, int mon, int mday, int hour, int min, int sec, int isdst)
{
	struct tm tm;

	TEST_ASSERT_NOT_NULL(localtime_r(&t, &tm));
	TEST_ASSERT_EQUAL_INT(year - 1900, tm.tm_year);
	TEST_ASSERT_EQUAL_INT(mon - 1, tm.tm_mon);
	TEST_ASSERT_EQUAL_INT(mday, tm.tm_mday);
	TEST_ASSERT_EQUAL_INT(hour, tm.tm_hour);
	TEST_ASSERT_EQUAL_INT(min, tm.tm_min);
	TEST_ASSERT_EQUAL_INT(sec, tm.tm_sec);
	TEST_ASSERT_EQUAL_INT(isdst, tm.tm_isdst);
}


static time_t makeLocal(struct tm *tm, int year, int mon, int mday, int hour, int min, int isdst)
{
	memset(tm, 0, sizeof(*tm));
	tm->tm_year = year - 1900;
	tm->tm_mon = mon - 1;
	tm->tm_mday = mday;
	tm->tm_hour = hour;
	tm->tm_min = min;
	tm->tm_isdst = isdst;

	return mktime(tm);
}


static void assertZone(time_t t, const char *expected)
{
	struct tm tm;
	char buf[32];

	TEST_ASSERT_NOT_NULL(localtime_r(&t, &tm));
	TEST_ASSERT_EQUAL_size_t(strlen(expected), strftime(buf, sizeof(buf), "%Z %z", &tm));
	TEST_ASSERT_EQUAL_STRING(expected, buf);
}


TEST_GROUP(time_tz);


TEST_SETUP(time_tz)
{
}


TEST_TEAR_DOWN(time_tz)
{
	/* the other groups expect UTC */
	unsetenv("TZ");
	tzset();
}


TEST(time_tz, tzset_dst)
{
	setTZ(TZ_CET);
	TEST_ASSERT_EQUAL_STRING("CET", tzname[0]);
	TEST_ASSERT_EQUAL_STRING("CEST", tzname[1]);
	TEST_ASSERT_EQUAL_INT(-3600, timezone);
	TEST_ASSERT_NOT_EQUAL(0, daylight);

	setTZ("EST5EDT");
	TEST_ASSERT_EQUAL_STRING("EST", tzname[0]);
	TEST_ASSERT_EQUAL_STRING("EDT", tzname[1]);
	TEST_ASSERT_EQUAL_INT(5 * 3600, timezone);
	TEST_ASSERT_NOT_EQUAL(0, daylight);
}


TEST(time_tz, tzset_quoted_no_dst)
{
	setTZ("<+0330>-3:30");
	TEST_ASSERT_EQUAL_STRING("+0330", tzname[0]);
	TEST_ASSERT_EQUAL_INT(-(3 * 3600 + 30 * 60), timezone);
	TEST_ASSERT_EQUAL_INT(0, daylight);

	setTZ("<-03>3");
	TEST_ASSERT_EQUAL_STRING("-03", tzname[0]);
	TEST_ASSERT_EQUAL_INT(3 * 3600, timezone);
	TEST_ASSERT_EQUAL_INT(0, daylight);
}


/* No time zone database: a zone name, a ':' value and an unset TZ are UTC */
TEST(time_tz, tzset_fallback_utc)
{
	static const char *const tzs[] = { "Europe/Warsaw", ":Europe/Warsaw", ":UTC", "" };
	unsigned int i;

	for (i = 0; i < sizeof(tzs) / sizeof(tzs[0]); i++) {
		setTZ(tzs[i]);
		TEST_ASSERT_EQUAL_STRING_MESSAGE("UTC", tzname[0], tzs[i]);
		TEST_ASSERT_EQUAL_STRING_MESSAGE("UTC", tzname[1], tzs[i]);
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, timezone, tzs[i]);
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, daylight, tzs[i]);
		assertLocal(1782900000, 2026, 7, 1, 10, 0, 0, 0);
	}

	unsetenv("TZ");
	tzset();
	TEST_ASSERT_EQUAL_STRING("UTC", tzname[0]);
	TEST_ASSERT_EQUAL_INT(0, timezone);
}


TEST(time_tz, localtime_cet)
{
	setTZ(TZ_CET);
	assertLocal(1782900000, 2026, 7, 1, 12, 0, 0, 1);
	assertLocal(1767225600, 2026, 1, 1, 1, 0, 0, 0);
}


/* The last Sunday of March and of October, each second either side */
TEST(time_tz, localtime_cet_changes)
{
	setTZ(TZ_CET);
	assertLocal(1774745999, 2026, 3, 29, 1, 59, 59, 0);
	assertLocal(1774746000, 2026, 3, 29, 3, 0, 0, 1);
	assertLocal(1792889999, 2026, 10, 25, 2, 59, 59, 1);
	assertLocal(1792890000, 2026, 10, 25, 2, 0, 0, 0);
}


/* DST spans the new year */
TEST(time_tz, localtime_southern)
{
	setTZ("AEST-10AEDT,M10.1.0,M4.1.0/3");
	assertLocal(1768435200, 2026, 1, 15, 11, 0, 0, 1);
	assertLocal(1782900000, 2026, 7, 1, 20, 0, 0, 0);
}


/* Without rules the US ones apply: second Sunday of March, first of November */
TEST(time_tz, localtime_default_rules)
{
	setTZ("EST5EDT");
	assertLocal(1782907200, 2026, 7, 1, 8, 0, 0, 1);
	assertLocal(1772953199, 2026, 3, 8, 1, 59, 59, 0);
	assertLocal(1772953200, 2026, 3, 8, 3, 0, 0, 1);
	assertLocal(1793512799, 2026, 11, 1, 1, 59, 59, 1);
	assertLocal(1793512800, 2026, 11, 1, 1, 0, 0, 0);
}


TEST(time_tz, mktime_roundtrip)
{
	struct tm tm;

	setTZ(TZ_CET);
	TEST_ASSERT_EQUAL_INT64(1782900000, makeLocal(&tm, 2026, 7, 1, 12, 0, -1));
	TEST_ASSERT_EQUAL_INT(1, tm.tm_isdst);
	TEST_ASSERT_EQUAL_INT64(1767225600, makeLocal(&tm, 2026, 1, 1, 1, 0, -1));
	TEST_ASSERT_EQUAL_INT(0, tm.tm_isdst);

	/* tm_isdst = 0 in summer: taken as standard time, normalised to DST */
	TEST_ASSERT_EQUAL_INT64(1782903600, makeLocal(&tm, 2026, 7, 1, 12, 0, 0));
	TEST_ASSERT_EQUAL_INT(13, tm.tm_hour);
	TEST_ASSERT_EQUAL_INT(1, tm.tm_isdst);
}


/* 02:30 does not exist on 2026-03-29: taken as standard time (glibc does the same) */
TEST(time_tz, mktime_skipped_hour)
{
	struct tm tm;

	setTZ(TZ_CET);
	TEST_ASSERT_EQUAL_INT64(1774747800, makeLocal(&tm, 2026, 3, 29, 2, 30, -1));
	TEST_ASSERT_EQUAL_INT(3, tm.tm_hour);
	TEST_ASSERT_EQUAL_INT(30, tm.tm_min);
	TEST_ASSERT_EQUAL_INT(1, tm.tm_isdst);
}


/* 02:30 happens twice on 2026-10-25 */
TEST(time_tz, mktime_repeated_hour)
{
	struct tm tm;

	setTZ(TZ_CET);
	TEST_ASSERT_EQUAL_INT64(1792888200, makeLocal(&tm, 2026, 10, 25, 2, 30, 1));
	TEST_ASSERT_EQUAL_INT(1, tm.tm_isdst);
	TEST_ASSERT_EQUAL_INT64(1792891800, makeLocal(&tm, 2026, 10, 25, 2, 30, 0));
	TEST_ASSERT_EQUAL_INT(0, tm.tm_isdst);

#ifdef __phoenix__
	/* libphoenix takes the earlier; glibc's choice depends on its previous call */
	TEST_ASSERT_EQUAL_INT64(1792888200, makeLocal(&tm, 2026, 10, 25, 2, 30, -1));
	TEST_ASSERT_EQUAL_INT(1, tm.tm_isdst);
#endif
}


TEST(time_tz, strftime_zone)
{
	struct tm tm;
	char buf[32];

	setTZ(TZ_CET);
	assertZone(1782900000, "CEST +0200");
	assertZone(1767225600, "CET +0100");

	setTZ("NPT-5:45");
	assertZone(0, "NPT +0545");

	setTZ("<-03>3");
	assertZone(0, "-03 -0300");

	/* tm_isdst < 0: no zone information */
	memset(&tm, 0, sizeof(tm));
	tm.tm_mday = 1;
	tm.tm_isdst = -1;
	TEST_ASSERT_EQUAL_size_t(2, strftime(buf, sizeof(buf), "[%Z%z]", &tm));
	TEST_ASSERT_EQUAL_STRING("[]", buf);
}


TEST(time_tz, ctime_local)
{
	time_t t = 1782900000;
	char buf[32];

	setTZ(TZ_CET);
	TEST_ASSERT_NOT_NULL(ctime_r(&t, buf));
	TEST_ASSERT_EQUAL_STRING("Wed Jul  1 12:00:00 2026\n", buf);
}


TEST_GROUP_RUNNER(time_tz)
{
	RUN_TEST_CASE(time_tz, tzset_dst);
	RUN_TEST_CASE(time_tz, tzset_quoted_no_dst);
	RUN_TEST_CASE(time_tz, tzset_fallback_utc);
	RUN_TEST_CASE(time_tz, localtime_cet);
	RUN_TEST_CASE(time_tz, localtime_cet_changes);
	RUN_TEST_CASE(time_tz, localtime_southern);
	RUN_TEST_CASE(time_tz, localtime_default_rules);
	RUN_TEST_CASE(time_tz, mktime_roundtrip);
	RUN_TEST_CASE(time_tz, mktime_skipped_hour);
	RUN_TEST_CASE(time_tz, mktime_repeated_hour);
	RUN_TEST_CASE(time_tz, strftime_zone);
	RUN_TEST_CASE(time_tz, ctime_local);
}
