/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - wchar.h
 *    - wctype.h
 *
 * TESTED (libphoenix additions):
 *    - wcspbrk(), wcsspn(), wcscspn(), wcsstr(), wcstok()
 *    - wcstol(), wcstoul(), wcstoll(), wcstoull()
 *    - wcstod(), wcstof(), wcstold()
 *    - iswalpha/iswdigit/iswalnum/iswspace/iswupper/iswlower/iswpunct/
 *      iswxdigit/iswcntrl/iswblank/iswgraph/iswprint, towupper/towlower,
 *      wctype()/iswctype(), wctrans()/towctrans()
 *    - wcwidth(), wcswidth(), mbrlen(), wctob(), wmemchr(), wcsdup(), wcscoll()
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <wchar.h>
#include <wctype.h>
#include <stdio.h>
#include <string.h>
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


/* --- wctype.h classification (C locale; libphoenix wctype/ module) --- */

TEST(string_wchar, wctype_classify)
{
	TEST_ASSERT_TRUE(iswalpha(L'a'));
	TEST_ASSERT_TRUE(iswalpha(L'Z'));
	TEST_ASSERT_FALSE(iswalpha(L'5'));
	TEST_ASSERT_TRUE(iswdigit(L'7'));
	TEST_ASSERT_FALSE(iswdigit(L'a'));
	TEST_ASSERT_TRUE(iswalnum(L'a'));
	TEST_ASSERT_TRUE(iswalnum(L'5'));
	TEST_ASSERT_FALSE(iswalnum(L'.'));
	TEST_ASSERT_TRUE(iswspace(L' '));
	TEST_ASSERT_TRUE(iswspace(L'\t'));
	TEST_ASSERT_TRUE(iswspace(L'\n'));
	TEST_ASSERT_FALSE(iswspace(L'x'));
	TEST_ASSERT_TRUE(iswupper(L'A'));
	TEST_ASSERT_FALSE(iswupper(L'a'));
	TEST_ASSERT_TRUE(iswlower(L'a'));
	TEST_ASSERT_FALSE(iswlower(L'A'));
	TEST_ASSERT_TRUE(iswpunct(L'.'));
	TEST_ASSERT_TRUE(iswxdigit(L'f'));
	TEST_ASSERT_TRUE(iswxdigit(L'9'));
	TEST_ASSERT_FALSE(iswxdigit(L'g'));
	TEST_ASSERT_TRUE(iswcntrl(L'\n'));
	TEST_ASSERT_FALSE(iswcntrl(L'a'));
	TEST_ASSERT_TRUE(iswblank(L' '));
	TEST_ASSERT_TRUE(iswblank(L'\t'));
	TEST_ASSERT_FALSE(iswblank(L'\n'));
	TEST_ASSERT_TRUE(iswgraph(L'a'));
	TEST_ASSERT_FALSE(iswgraph(L' '));
	TEST_ASSERT_TRUE(iswprint(L' '));
	TEST_ASSERT_TRUE(iswprint(L'a'));
	TEST_ASSERT_FALSE(iswprint(L'\n'));
}


TEST(string_wchar, wctype_convert)
{
	TEST_ASSERT_EQUAL_INT(L'A', towupper(L'a'));
	TEST_ASSERT_EQUAL_INT(L'A', towupper(L'A')); /* idempotent */
	TEST_ASSERT_EQUAL_INT(L'5', towupper(L'5')); /* non-alpha unchanged */
	TEST_ASSERT_EQUAL_INT(L'z', towlower(L'Z'));
	TEST_ASSERT_EQUAL_INT(L'z', towlower(L'z'));
	TEST_ASSERT_EQUAL_INT(L'.', towlower(L'.'));

	/* wctype() + iswctype() — functional (avoid assuming the wctype_t repr) */
	TEST_ASSERT_TRUE(iswctype(L'a', wctype("alpha")));
	TEST_ASSERT_FALSE(iswctype(L'5', wctype("alpha")));
	TEST_ASSERT_TRUE(iswctype(L'5', wctype("digit")));
	TEST_ASSERT_TRUE(iswctype(L' ', wctype("space")));
	TEST_ASSERT_FALSE(iswctype(L'a', wctype("bogus"))); /* unknown class -> 0 */

	/* wctrans() + towctrans() */
	TEST_ASSERT_EQUAL_INT(L'A', towctrans(L'a', wctrans("toupper")));
	TEST_ASSERT_EQUAL_INT(L'a', towctrans(L'A', wctrans("tolower")));
}


/* --- wchar.h additions: width, single-byte, misc, collate --- */

TEST(string_wchar, wchar_width)
{
	TEST_ASSERT_EQUAL_INT(0, wcwidth(L'\0'));
	TEST_ASSERT_EQUAL_INT(1, wcwidth(L'a'));
	TEST_ASSERT_EQUAL_INT(1, wcwidth(L' '));
	TEST_ASSERT_EQUAL_INT(-1, wcwidth(L'\n')); /* control char -> not printable */

	TEST_ASSERT_EQUAL_INT(3, wcswidth(L"abc", 3));
	TEST_ASSERT_EQUAL_INT(2, wcswidth(L"abc", 2));       /* honors n */
	TEST_ASSERT_EQUAL_INT(-1, wcswidth(L"a\nb", 3));     /* control char -> -1 */
}


TEST(string_wchar, wchar_misc)
{
	mbstate_t st;
	const wchar_t s[] = L"abcd";
	wchar_t *dup;

	/* mbrlen (C locale, single-byte) */
	memset(&st, 0, sizeof(st));
	TEST_ASSERT_EQUAL_INT(1, (int)mbrlen("a", 1, &st));
	memset(&st, 0, sizeof(st));
	TEST_ASSERT_EQUAL_INT(0, (int)mbrlen("", 1, &st)); /* NUL -> 0 */

	/* wctob (ASCII single-byte roundtrip) */
	TEST_ASSERT_EQUAL_INT('A', wctob(L'A'));
	TEST_ASSERT_EQUAL_INT(EOF, wctob(0x100)); /* not a single byte */

	/* wmemchr */
	TEST_ASSERT_EQUAL_PTR(&s[2], wmemchr(s, L'c', 4));
	TEST_ASSERT_NULL(wmemchr(s, L'z', 4));

	/* wcsdup */
	dup = wcsdup(L"hello");
	TEST_ASSERT_NOT_NULL(dup);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(dup, L"hello"));
	free(dup);
}


TEST(string_wchar, wchar_coll)
{
	/* C locale: wcscoll orders like wcscmp */
	TEST_ASSERT_EQUAL_INT(0, wcscoll(L"abc", L"abc"));
	TEST_ASSERT_TRUE(wcscoll(L"abc", L"abd") < 0);
	TEST_ASSERT_TRUE(wcscoll(L"abd", L"abc") > 0);
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
	RUN_TEST_CASE(string_wchar, wctype_classify);
	RUN_TEST_CASE(string_wchar, wctype_convert);
	RUN_TEST_CASE(string_wchar, wchar_width);
	RUN_TEST_CASE(string_wchar, wchar_misc);
	RUN_TEST_CASE(string_wchar, wchar_coll);
}
