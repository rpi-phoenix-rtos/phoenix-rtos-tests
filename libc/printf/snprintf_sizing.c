/*
 * Phoenix-RTOS
 *
 * libc tests
 *
 * HEADER:
 *    - stdio.h
 *
 * TESTED:
 *    - snprintf()/vsnprintf() return value = number of characters that WOULD be
 *      written (C99), including the s==NULL / n==0 "measure only" mode used by
 *      the vasprintf idiom (measure, malloc, format). A past libphoenix bug sized
 *      vsnprintf against a fixed 1024-byte buffer and overflowed for longer
 *      output; these guard the exact-sizing contract, especially past 1024 bytes.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <unity_fixture.h>


TEST_GROUP(stdio_printf_sizing);


TEST_SETUP(stdio_printf_sizing)
{
}


TEST_TEAR_DOWN(stdio_printf_sizing)
{
}


static int measure(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	return n;
}


/* snprintf(NULL, 0, ...) returns the would-be length and writes nothing */
TEST(stdio_printf_sizing, snprintf_null_zero_returns_len)
{
	TEST_ASSERT_EQUAL_INT(5, snprintf(NULL, 0, "%d", 12345));
	TEST_ASSERT_EQUAL_INT(5, snprintf(NULL, 0, "%s", "hello"));
	TEST_ASSERT_EQUAL_INT(0, snprintf(NULL, 0, "%s", ""));
	TEST_ASSERT_EQUAL_INT(8, snprintf(NULL, 0, "%08x", 0xabcdU));
	TEST_ASSERT_EQUAL_INT(10, snprintf(NULL, 0, "%s", "the quick "));
}


/* the vsnprintf variant used by vasprintf() */
TEST(stdio_printf_sizing, vsnprintf_null_zero_returns_len)
{
	TEST_ASSERT_EQUAL_INT(5, measure("%d", 12345));
	TEST_ASSERT_EQUAL_INT(6, measure("%c%c%c%c%c%c", 'a', 'b', 'c', 'd', 'e', 'f'));
	TEST_ASSERT_EQUAL_INT(7, measure("%d-%d", 12, 3456));
}


/* measure, allocate exactly, then fill — the vasprintf idiom must round-trip */
TEST(stdio_printf_sizing, measure_then_fill)
{
	const char *s = "the quick brown fox";
	int n = snprintf(NULL, 0, "[%s]", s);
	char *buf;
	int m;

	TEST_ASSERT_EQUAL_INT(21, n); /* '[' + 19 + ']' */
	buf = malloc((size_t)n + 1);
	TEST_ASSERT_NOT_NULL(buf);
	m = snprintf(buf, (size_t)n + 1, "[%s]", s);
	TEST_ASSERT_EQUAL_INT(21, m);
	TEST_ASSERT_EQUAL_STRING("[the quick brown fox]", buf);
	free(buf);
}


/* a too-small buffer truncates + NUL-terminates but returns the FULL length */
TEST(stdio_printf_sizing, truncation_returns_full_len)
{
	char buf[4];
	int n = snprintf(buf, sizeof(buf), "%d", 12345);

	TEST_ASSERT_EQUAL_INT(5, n);
	TEST_ASSERT_EQUAL_STRING("123", buf);
}


/* the regression that motivated the fix: correct sizing well past 1024 bytes */
TEST(stdio_printf_sizing, large_size_over_1024)
{
	static char big[2048];
	int n;

	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	n = snprintf(NULL, 0, "%s", big);
	TEST_ASSERT_EQUAL_INT((int)(sizeof(big) - 1), n); /* 2047, not capped at ~1024 */
}


TEST_GROUP_RUNNER(stdio_printf_sizing)
{
	RUN_TEST_CASE(stdio_printf_sizing, snprintf_null_zero_returns_len);
	RUN_TEST_CASE(stdio_printf_sizing, vsnprintf_null_zero_returns_len);
	RUN_TEST_CASE(stdio_printf_sizing, measure_then_fill);
	RUN_TEST_CASE(stdio_printf_sizing, truncation_returns_full_len);
	RUN_TEST_CASE(stdio_printf_sizing, large_size_over_1024);
}
