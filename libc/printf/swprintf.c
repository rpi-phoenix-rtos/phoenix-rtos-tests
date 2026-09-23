/*
 * Phoenix-RTOS
 *
 * libc tests
 *
 * HEADER:
 *    - wchar.h
 *
 * TESTED:
 *    - swprintf()/vswprintf(), added to libphoenix 2026-09-23.
 *
 * ⚠ The contract differs from snprintf and that is the point of half of these:
 * swprintf returns a NEGATIVE value when n or more wide characters are
 * requested (C99 7.24.2.7), it does NOT report the would-be length. Code ported
 * from the narrow family that grows a buffer on a large return would loop
 * forever, so the truncation cases below are the ones worth keeping.
 *
 * ⚠ %s takes a MULTIBYTE string and %ls a wide one, which is the reverse of what
 * some code assumes -- Irrlicht passes wchar_t * to %s, which is why
 * SuperTuxKart carries its own shim.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdarg.h>
#include <wchar.h>

#include <unity_fixture.h>


TEST_GROUP(wide_printf);


TEST_SETUP(wide_printf)
{
}


TEST_TEAR_DOWN(wide_printf)
{
}


static int wp_via_v(wchar_t *ws, size_t n, const wchar_t *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vswprintf(ws, n, fmt, ap);
	va_end(ap);

	return ret;
}


TEST(wide_printf, plain_text)
{
	wchar_t buf[32];

	TEST_ASSERT_EQUAL_INT(5, swprintf(buf, 32, L"hello"));
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"hello"));
}


TEST(wide_printf, percent_literal)
{
	wchar_t buf[32];

	TEST_ASSERT_EQUAL_INT(4, swprintf(buf, 32, L"100%%"));
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"100%"));
}


TEST(wide_printf, integers)
{
	wchar_t buf[64];

	TEST_ASSERT_EQUAL_INT(4, swprintf(buf, 64, L"[%d]", 42));
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[42]"));

	swprintf(buf, 64, L"[%d]", -42);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[-42]"));

	swprintf(buf, 64, L"[%8d]", 42);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[      42]"));

	swprintf(buf, 64, L"[%-8d]", 42);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[42      ]"));

	swprintf(buf, 64, L"[%08d]", 42);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[00000042]"));

	swprintf(buf, 64, L"[%x]", 0xabcu);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[abc]"));

	swprintf(buf, 64, L"[%#x]", 0xabcu);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[0xabc]"));
}


TEST(wide_printf, length_modifiers)
{
	wchar_t buf[64];

	swprintf(buf, 64, L"[%ld]", 1234567890L);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[1234567890]"));

	swprintf(buf, 64, L"[%lld]", 1234567890123LL);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[1234567890123]"));

	swprintf(buf, 64, L"[%zu]", (size_t)123456);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[123456]"));

	swprintf(buf, 64, L"[%hd]", -7);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[-7]"));
}


TEST(wide_printf, floats)
{
	wchar_t buf[64];

	swprintf(buf, 64, L"[%.2f]", 3.14159);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[3.14]"));

	swprintf(buf, 64, L"[%.0f]", 2.0);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[2]"));
}


TEST(wide_printf, chars_narrow_and_wide)
{
	wchar_t buf[32];

	swprintf(buf, 32, L"[%c]", 'Z');
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[Z]"));

	swprintf(buf, 32, L"[%lc]", (wint_t)L'Q');
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[Q]"));
}


/* %s is MULTIBYTE, %ls is wide. Getting these the wrong way round is the most
 * common wide-printf porting bug, so both directions are pinned. */
TEST(wide_printf, strings_multibyte_and_wide)
{
	wchar_t buf[64];

	swprintf(buf, 64, L"[%s]", "narrow");
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[narrow]"));

	swprintf(buf, 64, L"[%ls]", L"wide");
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[wide]"));

	swprintf(buf, 64, L"[%.3s]", "narrow");
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[nar]"));

	swprintf(buf, 64, L"[%.2ls]", L"wide");
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[wi]"));
}


TEST(wide_printf, star_width_and_precision)
{
	wchar_t buf[64];

	swprintf(buf, 64, L"[%*d]", 6, 42);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[    42]"));

	swprintf(buf, 64, L"[%.*f]", 3, 3.14159);
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"[3.142]"));
}


/* ★ The contract that differs from snprintf. */
TEST(wide_printf, truncation_returns_negative)
{
	wchar_t buf[16];

	/* exactly fits: 5 chars + null in 6 */
	TEST_ASSERT_EQUAL_INT(5, swprintf(buf, 6, L"12345"));
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"12345"));

	/* one short: NEGATIVE, not 5 */
	TEST_ASSERT_TRUE(swprintf(buf, 5, L"12345") < 0);

	/* a conversion that overflows: NEGATIVE, not the would-be length */
	TEST_ASSERT_TRUE(swprintf(buf, 4, L"[%d]", 12345) < 0);
}


TEST(wide_printf, zero_size_is_negative)
{
	wchar_t buf[8];

	buf[0] = L'x';
	TEST_ASSERT_TRUE(swprintf(buf, 0, L"abc") < 0);
	/* nothing written */
	TEST_ASSERT_EQUAL_INT((int)L'x', (int)buf[0]);
}


TEST(wide_printf, vswprintf_matches)
{
	wchar_t buf[64];

	TEST_ASSERT_EQUAL_INT(9, wp_via_v(buf, 64, L"%d/%s/%ls", 7, "ab", L"cdef"));
	TEST_ASSERT_EQUAL_INT(0, wcscmp(buf, L"7/ab/cdef"));
}


TEST_GROUP_RUNNER(wide_printf)
{
	RUN_TEST_CASE(wide_printf, plain_text);
	RUN_TEST_CASE(wide_printf, percent_literal);
	RUN_TEST_CASE(wide_printf, integers);
	RUN_TEST_CASE(wide_printf, length_modifiers);
	RUN_TEST_CASE(wide_printf, floats);
	RUN_TEST_CASE(wide_printf, chars_narrow_and_wide);
	RUN_TEST_CASE(wide_printf, strings_multibyte_and_wide);
	RUN_TEST_CASE(wide_printf, star_width_and_precision);
	RUN_TEST_CASE(wide_printf, truncation_returns_negative);
	RUN_TEST_CASE(wide_printf, zero_size_is_negative);
	RUN_TEST_CASE(wide_printf, vswprintf_matches);
}
