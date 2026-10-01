/*
 * Phoenix-RTOS
 *
 * POSIX.1-2008 standard library functions tests
 *
 * HEADER:
 *    - locale.h, ctype.h, wctype.h, string.h, strings.h, wchar.h, time.h,
 *      langinfo.h, stdlib.h
 *
 * TESTED:
 *    - newlocale(), duplocale(), freelocale(), uselocale()
 *    - the *_l() functions, and strto*_l() (BSD, glibc)
 *    - setlocale(LC_MESSAGES)
 *
 * libxslt sorts with newlocale() + strxfrm_l(), GLib converts numbers with
 * newlocale() + strtod_l(), and labwc's font code switches locales with
 * uselocale(). Phoenix has only the C locale, so a locale object must
 * behave exactly as the global C locale does.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#define _GNU_SOURCE /* strto*_l() on glibc (host-generic-pc) */

#include <ctype.h>
#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#ifdef __phoenix__
#include <arch.h> /* __LIBPHOENIX_ARCH_TLS_SUPPORTED */
#endif

#include <unity_fixture.h>


static locale_t locobj_c;


TEST_GROUP(locale_objects);


TEST_SETUP(locale_objects)
{
	locobj_c = newlocale(LC_ALL_MASK, "C", (locale_t)0);
	TEST_ASSERT_NOT_NULL(locobj_c);
}


TEST_TEAR_DOWN(locale_objects)
{
	(void)uselocale(LC_GLOBAL_LOCALE);
	freelocale(locobj_c);
}


TEST(locale_objects, newlocale_names)
{
	locale_t loc;

	loc = newlocale(LC_ALL_MASK, "POSIX", (locale_t)0);
	TEST_ASSERT_NOT_NULL(loc);
	freelocale(loc);

	/* "" is the locale the environment names, the C locale here */
	loc = newlocale(LC_CTYPE_MASK | LC_NUMERIC_MASK, "", (locale_t)0);
	TEST_ASSERT_NOT_NULL(loc);
	freelocale(loc);

	/* base may be reused for the result */
	loc = newlocale(LC_COLLATE_MASK, "C", duplocale(locobj_c));
	TEST_ASSERT_NOT_NULL(loc);
	freelocale(loc);
}


TEST(locale_objects, newlocale_errors)
{
	errno = 0;
	TEST_ASSERT_NULL(newlocale(LC_ALL_MASK, "xx_YY.NOSUCH", (locale_t)0));
	TEST_ASSERT_EQUAL_INT(ENOENT, errno);

	errno = 0;
	TEST_ASSERT_NULL(newlocale(LC_ALL_MASK, NULL, (locale_t)0));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);

	errno = 0;
	TEST_ASSERT_NULL(newlocale(1 << 30, "C", (locale_t)0));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}


TEST(locale_objects, duplocale_global)
{
	locale_t loc = duplocale(LC_GLOBAL_LOCALE);

	TEST_ASSERT_NOT_NULL(loc);
	TEST_ASSERT_TRUE(loc != LC_GLOBAL_LOCALE);
	TEST_ASSERT_EQUAL_INT(isalpha('a') != 0, isalpha_l('a', loc) != 0);
	freelocale(loc);
}


TEST(locale_objects, uselocale_switches_and_restores)
{
	TEST_ASSERT_TRUE(uselocale((locale_t)0) == LC_GLOBAL_LOCALE);
	TEST_ASSERT_TRUE(uselocale(locobj_c) == LC_GLOBAL_LOCALE);
	TEST_ASSERT_TRUE(uselocale((locale_t)0) == locobj_c);
	TEST_ASSERT_TRUE(uselocale(LC_GLOBAL_LOCALE) == locobj_c);
	TEST_ASSERT_TRUE(uselocale((locale_t)0) == LC_GLOBAL_LOCALE);
}


static void *locobj_threadLocale(void *arg)
{
	*(locale_t *)arg = uselocale((locale_t)0);
	return NULL;
}


/* The current locale belongs to the thread */
TEST(locale_objects, uselocale_is_per_thread)
{
#if !defined(__phoenix__) || defined(__LIBPHOENIX_ARCH_TLS_SUPPORTED)
	pthread_t thread;
	locale_t seen = (locale_t)0;

	TEST_ASSERT_TRUE(uselocale(locobj_c) == LC_GLOBAL_LOCALE);
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, locobj_threadLocale, &seen));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
	TEST_ASSERT_TRUE(seen == LC_GLOBAL_LOCALE);
	TEST_ASSERT_TRUE(uselocale((locale_t)0) == locobj_c);
#else
	TEST_IGNORE_MESSAGE("no thread-local storage on this architecture");
#endif
}


/* Every byte classifies and maps as in the global C locale */
TEST(locale_objects, ctype_l_matches_ctype)
{
	int c;

	for (c = -1; c < 256; c++) {
		TEST_ASSERT_EQUAL_INT(isalnum(c) != 0, isalnum_l(c, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(isalpha(c) != 0, isalpha_l(c, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(isblank(c) != 0, isblank_l(c, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(iscntrl(c) != 0, iscntrl_l(c, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(isdigit(c) != 0, isdigit_l(c, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(isgraph(c) != 0, isgraph_l(c, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(islower(c) != 0, islower_l(c, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(isprint(c) != 0, isprint_l(c, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(ispunct(c) != 0, ispunct_l(c, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(isspace(c) != 0, isspace_l(c, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(isupper(c) != 0, isupper_l(c, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(isxdigit(c) != 0, isxdigit_l(c, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(tolower(c), tolower_l(c, locobj_c));
		TEST_ASSERT_EQUAL_INT(toupper(c), toupper_l(c, locobj_c));
	}
}


TEST(locale_objects, wctype_l_matches_wctype)
{
	wint_t wc;

	for (wc = 0; wc < 256; wc++) {
		TEST_ASSERT_EQUAL_INT(iswalpha(wc) != 0, iswalpha_l(wc, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(iswdigit(wc) != 0, iswdigit_l(wc, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(iswspace(wc) != 0, iswspace_l(wc, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT(iswpunct(wc) != 0, iswpunct_l(wc, locobj_c) != 0);
		TEST_ASSERT_EQUAL_INT((int)towupper(wc), (int)towupper_l(wc, locobj_c));
		TEST_ASSERT_EQUAL_INT((int)towlower(wc), (int)towlower_l(wc, locobj_c));
		TEST_ASSERT_EQUAL_INT(iswctype(wc, wctype("alpha")) != 0, iswctype_l(wc, wctype_l("alpha", locobj_c), locobj_c) != 0);
	}
	TEST_ASSERT_EQUAL_INT((int)L'A', (int)towctrans_l(L'a', wctrans_l("toupper", locobj_c), locobj_c));
}


TEST(locale_objects, string_l)
{
	char buf[16];

	TEST_ASSERT_LESS_THAN_INT(0, strcoll_l("abc", "abd", locobj_c));
	TEST_ASSERT_EQUAL_INT(0, strcoll_l("abc", "abc", locobj_c));
	TEST_ASSERT_EQUAL_UINT(strxfrm(NULL, "hello", 0), strxfrm_l(NULL, "hello", 0, locobj_c));
	TEST_ASSERT_EQUAL_UINT(5u, strxfrm_l(buf, "hello", sizeof(buf), locobj_c));
	TEST_ASSERT_EQUAL_INT(0, strcmp(buf, "hello"));
	TEST_ASSERT_EQUAL_STRING(strerror(ENOENT), strerror_l(ENOENT, locobj_c));
	TEST_ASSERT_EQUAL_INT(0, strcasecmp_l("HeLLo", "hello", locobj_c));
	TEST_ASSERT_EQUAL_INT(0, strncasecmp_l("HeLLo!", "hello?", 5, locobj_c));
	TEST_ASSERT_LESS_THAN_INT(0, wcscoll_l(L"abc", L"abd", locobj_c));
}


TEST(locale_objects, numbers_l)
{
	char *end;

	TEST_ASSERT_EQUAL_DOUBLE(1.5, strtod_l("1.5x", &end, locobj_c));
	TEST_ASSERT_EQUAL_CHAR('x', *end);
	TEST_ASSERT_EQUAL_FLOAT(0.25f, strtof_l("0.25", NULL, locobj_c));
	TEST_ASSERT_TRUE(strtold_l("2.5", NULL, locobj_c) == 2.5L);
	TEST_ASSERT_EQUAL_INT64(31, strtol_l("0x1f", &end, 16, locobj_c));
	TEST_ASSERT_EQUAL_CHAR('\0', *end);
	TEST_ASSERT_EQUAL_UINT64(4294967296ull, strtoull_l("4294967296", NULL, 10, locobj_c));
	TEST_ASSERT_EQUAL_INT64(-42, strtoll_l("-42", NULL, 10, locobj_c));
	TEST_ASSERT_EQUAL_UINT64(255u, strtoul_l("377", NULL, 8, locobj_c));
}


TEST(locale_objects, time_and_langinfo_l)
{
	struct tm tm;
	time_t t = 86400 * 365;
	char a[64], b[64];

	TEST_ASSERT_NOT_NULL(gmtime_r(&t, &tm));
	TEST_ASSERT_EQUAL_UINT(strftime(a, sizeof(a), "%a %d %b %Y %H:%M:%S", &tm),
			strftime_l(b, sizeof(b), "%a %d %b %Y %H:%M:%S", &tm, locobj_c));
	TEST_ASSERT_EQUAL_STRING(a, b);
	TEST_ASSERT_EQUAL_STRING(nl_langinfo(CODESET), nl_langinfo_l(CODESET, locobj_c));
}


/* POSIX requires LC_MESSAGES: setlocale() and newlocale() take it */
TEST(locale_objects, setlocale_messages)
{
	locale_t loc;

	TEST_ASSERT_NOT_NULL(setlocale(LC_MESSAGES, "C"));
	TEST_ASSERT_NOT_NULL(setlocale(LC_MESSAGES, NULL));

	loc = newlocale(LC_MESSAGES_MASK, "C", (locale_t)0);
	TEST_ASSERT_NOT_NULL(loc);
	freelocale(loc);
}


TEST_GROUP_RUNNER(locale_objects)
{
	RUN_TEST_CASE(locale_objects, newlocale_names);
	RUN_TEST_CASE(locale_objects, newlocale_errors);
	RUN_TEST_CASE(locale_objects, duplocale_global);
	RUN_TEST_CASE(locale_objects, uselocale_switches_and_restores);
	RUN_TEST_CASE(locale_objects, uselocale_is_per_thread);
	RUN_TEST_CASE(locale_objects, ctype_l_matches_ctype);
	RUN_TEST_CASE(locale_objects, wctype_l_matches_wctype);
	RUN_TEST_CASE(locale_objects, string_l);
	RUN_TEST_CASE(locale_objects, numbers_l);
	RUN_TEST_CASE(locale_objects, time_and_langinfo_l);
	RUN_TEST_CASE(locale_objects, setlocale_messages);
}
