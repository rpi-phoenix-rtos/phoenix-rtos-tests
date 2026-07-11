/*
 * Phoenix-RTOS
 *
 *    libc-tests
 *    HEADER:
 *    - langinfo.h
 *    TESTED:
 *    - nl_langinfo()
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <langinfo.h>
#include <string.h>
#include <unity_fixture.h>


TEST_GROUP(langinfo);


TEST_SETUP(langinfo)
{
}


TEST_TEAR_DOWN(langinfo)
{
}


TEST(langinfo, codeset)
{
	/* C/POSIX locale: ASCII, since the multibyte layer has no UTF-8 decoder. */
	TEST_ASSERT_EQUAL_STRING("ANSI_X3.4-1968", nl_langinfo(CODESET));
}


TEST(langinfo, time_names)
{
	TEST_ASSERT_EQUAL_STRING("Sunday", nl_langinfo(DAY_1));
	TEST_ASSERT_EQUAL_STRING("Saturday", nl_langinfo(DAY_7));
	TEST_ASSERT_EQUAL_STRING("Sun", nl_langinfo(ABDAY_1));
	TEST_ASSERT_EQUAL_STRING("January", nl_langinfo(MON_1));
	TEST_ASSERT_EQUAL_STRING("December", nl_langinfo(MON_12));
	TEST_ASSERT_EQUAL_STRING("Dec", nl_langinfo(ABMON_12));
	TEST_ASSERT_EQUAL_STRING("AM", nl_langinfo(AM_STR));
	TEST_ASSERT_EQUAL_STRING("PM", nl_langinfo(PM_STR));
}


TEST(langinfo, numeric)
{
	/* consistent with localeconv(): decimal point ".", no thousands sep */
	TEST_ASSERT_EQUAL_STRING(".", nl_langinfo(RADIXCHAR));
	TEST_ASSERT_EQUAL_STRING("", nl_langinfo(THOUSEP));
}


TEST(langinfo, invalid_item)
{
	/* POSIX: an unknown item yields a pointer to an empty string, not NULL. */
	TEST_ASSERT_NOT_NULL(nl_langinfo(-1));
	TEST_ASSERT_EQUAL_STRING("", nl_langinfo(-1));
	TEST_ASSERT_EQUAL_STRING("", nl_langinfo(99999));
}


TEST_GROUP_RUNNER(langinfo)
{
	RUN_TEST_CASE(langinfo, codeset);
	RUN_TEST_CASE(langinfo, time_names);
	RUN_TEST_CASE(langinfo, numeric);
	RUN_TEST_CASE(langinfo, invalid_item);
}
