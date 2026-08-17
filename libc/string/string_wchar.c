/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - wchar.h
 *
 * TESTED (libphoenix additions):
 *    - wcspbrk(), wcsspn(), wcscspn(), wcsstr(), wcstok()
 *    - wcstol(), wcstoul(), wcstoll(), wcstoull()
 *    - wcstod(), wcstof(), wcstold()
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <wchar.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

#include <unity_fixture.h>


TEST_GROUP(string_wchar);


TEST_SETUP(string_wchar)
{
}


TEST_TEAR_DOWN(string_wchar)
{
}


/* --- search / span --- */

TEST(string_wchar, wcsspn_wcscspn)
{
	TEST_ASSERT_EQUAL_UINT(5, wcsspn(L"aabbcXY", L"abc"));
	TEST_ASSERT_EQUAL_UINT(0, wcsspn(L"Xabc", L"abc")); /* first char not in set */
	TEST_ASSERT_EQUAL_UINT(2, wcscspn(L"XYabc", L"abc"));
	TEST_ASSERT_EQUAL_UINT(8, wcscspn(L"nonehere", L"xyz")); /* no char in set -> full length 8 */
}


TEST(string_wchar, wcspbrk)
{
	const wchar_t *s = L"hello,world;foo";
	TEST_ASSERT_EQUAL_PTR(&s[5], wcspbrk(s, L";,"));  /* first ',' */
	TEST_ASSERT_NULL(wcspbrk(s, L"XYZ"));             /* none present */
}


TEST(string_wchar, wcsstr)
{
	const wchar_t *hay = L"hello world";
	TEST_ASSERT_EQUAL_PTR(&hay[6], wcsstr(hay, L"world"));
	TEST_ASSERT_EQUAL_PTR(hay, wcsstr(hay, L""));   /* empty needle -> haystack */
	TEST_ASSERT_NULL(wcsstr(hay, L"xyz"));
}


TEST(string_wchar, wcstok)
{
	wchar_t str[] = L"a,b;c d";
	wchar_t *save, *t;

	t = wcstok(str, L",; ", &save);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(L"a", t));
	t = wcstok(NULL, L",; ", &save);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(L"b", t));
	t = wcstok(NULL, L",; ", &save);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(L"c", t));
	t = wcstok(NULL, L",; ", &save);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(L"d", t));
	t = wcstok(NULL, L",; ", &save);
	TEST_ASSERT_NULL(t);
}


/* --- integer conversions --- */

TEST(string_wchar, wcstol)
{
	wchar_t *end;

	TEST_ASSERT_EQUAL_INT(12345, wcstol(L"12345", NULL, 10));
	TEST_ASSERT_EQUAL_INT(-42, wcstol(L"  -42", NULL, 10)); /* leading ws + sign */
	TEST_ASSERT_EQUAL_INT(26, wcstol(L"0x1A", NULL, 0));    /* base 0 -> hex */
	TEST_ASSERT_EQUAL_INT(255, wcstol(L"ff", NULL, 16));
	TEST_ASSERT_EQUAL_INT(5, wcstol(L"101", NULL, 2));      /* binary */

	/* endptr points past the consumed digits */
	TEST_ASSERT_EQUAL_INT(123, wcstol(L"123abc", &end, 10));
	TEST_ASSERT_EQUAL_INT(0, wcscmp(L"abc", end));
}


TEST(string_wchar, wcstoul_ll_ull)
{
	TEST_ASSERT_EQUAL_UINT(4000000000u, wcstoul(L"4000000000", NULL, 10));
	TEST_ASSERT_TRUE(wcstoll(L"9000000000", NULL, 10) == 9000000000LL);
	TEST_ASSERT_TRUE(wcstoull(L"18446744073709551615", NULL, 10) == ULLONG_MAX);
	TEST_ASSERT_TRUE(wcstoll(L"-9000000000", NULL, 10) == -9000000000LL);
}


/* --- floating conversions --- */

TEST(string_wchar, wcstod_f_ld)
{
	wchar_t *end;

	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 3.14159, wcstod(L"3.14159", NULL));
	TEST_ASSERT_DOUBLE_WITHIN(1e-9, -2500.0, wcstod(L"-2.5e3", NULL));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.5f, wcstof(L"1.5", NULL));
	TEST_ASSERT_TRUE(wcstold(L"2.25", NULL) == 2.25L);

	/* endptr */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.5, wcstod(L"0.5xyz", &end));
	TEST_ASSERT_EQUAL_INT(0, wcscmp(L"xyz", end));
}


TEST_GROUP_RUNNER(string_wchar)
{
	RUN_TEST_CASE(string_wchar, wcsspn_wcscspn);
	RUN_TEST_CASE(string_wchar, wcspbrk);
	RUN_TEST_CASE(string_wchar, wcsstr);
	RUN_TEST_CASE(string_wchar, wcstok);
	RUN_TEST_CASE(string_wchar, wcstol);
	RUN_TEST_CASE(string_wchar, wcstoul_ll_ull);
	RUN_TEST_CASE(string_wchar, wcstod_f_ld);
}
