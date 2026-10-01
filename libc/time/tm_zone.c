/*
 * Phoenix-RTOS
 *
 * test-libc-time
 *
 * struct tm's tm_gmtoff and tm_zone (BSD, glibc): filled by gmtime_r(),
 * localtime_r() and mktime(). nghttp2 probes for them and WebKit's date code
 * uses them when they exist.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#define _DEFAULT_SOURCE /* tm_gmtoff and tm_zone on glibc (host-generic-pc) */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "unity_fixture.h"


#define TM_ZONE_CET "CET-1CEST,M3.5.0,M10.5.0/3"

/* 2026-01-15 12:00:00 and 2026-07-15 12:00:00 UTC */
#define TM_ZONE_WINTER ((time_t)1768478400)
#define TM_ZONE_SUMMER ((time_t)1784116800)


static void tmzone_setTZ(const char *tz)
{
	TEST_ASSERT_EQUAL_INT(0, setenv("TZ", tz, 1));
	tzset();
}


static void tmzone_assertLocal(time_t t, long gmtoff, const char *zone)
{
	struct tm tm;

	memset(&tm, 0xa5, sizeof(tm));
	TEST_ASSERT_NOT_NULL(localtime_r(&t, &tm));
	TEST_ASSERT_EQUAL_INT32(gmtoff, tm.tm_gmtoff);
	TEST_ASSERT_NOT_NULL(tm.tm_zone);
	TEST_ASSERT_EQUAL_STRING(zone, tm.tm_zone);
}


TEST_GROUP(time_tm_zone);


TEST_SETUP(time_tm_zone)
{
}


TEST_TEAR_DOWN(time_tm_zone)
{
	/* the other groups expect UTC */
	unsetenv("TZ");
	tzset();
}


TEST(time_tm_zone, gmtime)
{
	struct tm tm;
	time_t t = TM_ZONE_SUMMER;

	tmzone_setTZ(TM_ZONE_CET);
	memset(&tm, 0xa5, sizeof(tm));
	TEST_ASSERT_NOT_NULL(gmtime_r(&t, &tm));
	TEST_ASSERT_EQUAL_INT32(0, tm.tm_gmtoff);
	TEST_ASSERT_NOT_NULL(tm.tm_zone);
	TEST_ASSERT_EQUAL_STRING("GMT", tm.tm_zone);
}


TEST(time_tm_zone, localtime_standard_and_dst)
{
	tmzone_setTZ(TM_ZONE_CET);
	tmzone_assertLocal(TM_ZONE_WINTER, 3600, "CET");
	tmzone_assertLocal(TM_ZONE_SUMMER, 7200, "CEST");

	/* West of UTC, and a quoted name */
	tmzone_setTZ("<-03>3");
	tmzone_assertLocal(TM_ZONE_WINTER, -3 * 3600, "-03");

	tmzone_setTZ("IST-5:30");
	tmzone_assertLocal(TM_ZONE_WINTER, 5 * 3600 + 1800, "IST");
}


TEST(time_tm_zone, mktime_fills_them)
{
	struct tm tm;

	tmzone_setTZ(TM_ZONE_CET);
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = 2026 - 1900;
	tm.tm_mon = 6;
	tm.tm_mday = 15;
	tm.tm_hour = 14;
	tm.tm_isdst = -1;
	tm.tm_zone = NULL;

	TEST_ASSERT_EQUAL_INT64((long long)TM_ZONE_SUMMER, (long long)mktime(&tm));
	TEST_ASSERT_EQUAL_INT32(7200, tm.tm_gmtoff);
	TEST_ASSERT_NOT_NULL(tm.tm_zone);
	TEST_ASSERT_EQUAL_STRING("CEST", tm.tm_zone);
}


/* The offset is what the broken-down time says: timegm(tm) - t */
TEST(time_tm_zone, gmtoff_matches_broken_down_time)
{
	static const char *const zones[] = { TM_ZONE_CET, "EST5EDT,M3.2.0,M11.1.0", "AEST-10AEDT,M10.1.0,M4.1.0/3", "UTC0" };
	struct tm tm, copy;
	time_t t;
	size_t i;

	for (i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
		tmzone_setTZ(zones[i]);
		for (t = TM_ZONE_WINTER; t < TM_ZONE_WINTER + 366 * 86400; t += 86400 + 3607) {
			TEST_ASSERT_NOT_NULL(localtime_r(&t, &tm));
			copy = tm; /* timegm() normalises its argument */
			TEST_ASSERT_EQUAL_INT64((long long)tm.tm_gmtoff, (long long)(timegm(&copy) - t));
		}
	}
}


/* tm_zone stays valid, and keeps its text, when TZ changes */
TEST(time_tm_zone, zone_outlives_tz_change)
{
	struct tm cet, est;
	time_t t = TM_ZONE_WINTER;

	tmzone_setTZ(TM_ZONE_CET);
	TEST_ASSERT_NOT_NULL(localtime_r(&t, &cet));

	tmzone_setTZ("EST5");
	TEST_ASSERT_NOT_NULL(localtime_r(&t, &est));

	TEST_ASSERT_EQUAL_STRING("CET", cet.tm_zone);
	TEST_ASSERT_EQUAL_STRING("EST", est.tm_zone);
	TEST_ASSERT_EQUAL_INT32(-5 * 3600, est.tm_gmtoff);
}


TEST_GROUP_RUNNER(time_tm_zone)
{
	RUN_TEST_CASE(time_tm_zone, gmtime);
	RUN_TEST_CASE(time_tm_zone, localtime_standard_and_dst);
	RUN_TEST_CASE(time_tm_zone, mktime_fills_them);
	RUN_TEST_CASE(time_tm_zone, gmtoff_matches_broken_down_time);
	RUN_TEST_CASE(time_tm_zone, zone_outlives_tz_change);
}
