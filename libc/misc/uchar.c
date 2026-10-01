/*
 * Phoenix-RTOS
 *
 * C11 standard library functions tests
 *
 * HEADER:
 *    - uchar.h
 *
 * TESTED:
 *    - mbrtoc16(), c16rtomb(), mbrtoc32(), c32rtomb()
 *
 * ICU includes <uchar.h> for char16_t and the conversions. libphoenix's
 * multibyte layer is the C locale (one byte, one character), and these follow
 * it: a byte becomes the code point of the same value, and only code points up
 * to U+00FF can be written back. The common cases hold on glibc as well.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <string.h>
#include <uchar.h>

#include <unity_fixture.h>


static mbstate_t uchar_state;


TEST_GROUP(misc_uchar);


TEST_SETUP(misc_uchar)
{
	memset(&uchar_state, 0, sizeof(uchar_state));
}


TEST_TEAR_DOWN(misc_uchar)
{
}


TEST(misc_uchar, types)
{
	TEST_ASSERT_EQUAL_UINT(2u, sizeof(char16_t));
	TEST_ASSERT_EQUAL_UINT(4u, sizeof(char32_t));
	/* Unsigned, as C11 7.28 requires */
	TEST_ASSERT_TRUE((char16_t)-1 > 0);
	TEST_ASSERT_TRUE((char32_t)-1 > 0);
}


TEST(misc_uchar, ascii_round_trip)
{
	const char *text = "Phoenix ~ 0x7f";
	char32_t c32 = 0;
	char16_t c16 = 0;
	char out[4];
	size_t i;

	for (i = 0; text[i] != '\0'; i++) {
		TEST_ASSERT_EQUAL_UINT(1u, mbrtoc32(&c32, &text[i], 4, &uchar_state));
		TEST_ASSERT_EQUAL_UINT((unsigned char)text[i], c32);
		TEST_ASSERT_EQUAL_UINT(1u, c32rtomb(out, c32, &uchar_state));
		TEST_ASSERT_EQUAL_CHAR(text[i], out[0]);

		TEST_ASSERT_EQUAL_UINT(1u, mbrtoc16(&c16, &text[i], 4, &uchar_state));
		TEST_ASSERT_EQUAL_UINT((unsigned char)text[i], c16);
		TEST_ASSERT_EQUAL_UINT(1u, c16rtomb(out, c16, &uchar_state));
		TEST_ASSERT_EQUAL_CHAR(text[i], out[0]);
	}
}


TEST(misc_uchar, null_and_empty)
{
	char32_t c32 = 1;
	char16_t c16 = 1;
	char out[4];

	/* The null character converts to 0 and returns 0 */
	TEST_ASSERT_EQUAL_UINT(0u, mbrtoc32(&c32, "", 1, &uchar_state));
	TEST_ASSERT_EQUAL_UINT(0u, c32);
	TEST_ASSERT_EQUAL_UINT(0u, mbrtoc16(&c16, "", 1, &uchar_state));
	TEST_ASSERT_EQUAL_UINT(0u, c16);

	/* No bytes: an incomplete character */
	TEST_ASSERT_EQUAL_UINT((size_t)-2, mbrtoc32(&c32, "a", 0, &uchar_state));

	/* s == NULL resets the state */
	TEST_ASSERT_EQUAL_UINT(0u, mbrtoc32(NULL, NULL, 0, &uchar_state));
	TEST_ASSERT_EQUAL_UINT(1u, c32rtomb(NULL, U'x', &uchar_state));
	TEST_ASSERT_EQUAL_UINT(1u, c32rtomb(out, 0, &uchar_state));
	TEST_ASSERT_EQUAL_CHAR('\0', out[0]);
}


/* What the C locale can hold: bytes as U+0000-U+00FF, nothing beyond */
TEST(misc_uchar, c_locale_range)
{
#ifdef __phoenix__
	char32_t c32 = 0;
	char16_t c16 = 0;
	char out[4];

	TEST_ASSERT_EQUAL_UINT(1u, mbrtoc32(&c32, "\xe9", 1, &uchar_state));
	TEST_ASSERT_EQUAL_UINT(0xe9u, c32);
	TEST_ASSERT_EQUAL_UINT(1u, c32rtomb(out, c32, &uchar_state));
	TEST_ASSERT_EQUAL_HEX8(0xe9, (unsigned char)out[0]);
	TEST_ASSERT_EQUAL_UINT(1u, mbrtoc16(&c16, "\xff", 1, &uchar_state));
	TEST_ASSERT_EQUAL_UINT(0xffu, c16);
#endif

	errno = 0;
	TEST_ASSERT_EQUAL_UINT((size_t)-1, c32rtomb((char[4]) { 0 }, U'€', &uchar_state));
	TEST_ASSERT_EQUAL_INT(EILSEQ, errno);

	errno = 0;
	TEST_ASSERT_EQUAL_UINT((size_t)-1, c16rtomb((char[4]) { 0 }, (char16_t)0x0100, &uchar_state));
	TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
}


TEST_GROUP_RUNNER(misc_uchar)
{
	RUN_TEST_CASE(misc_uchar, types);
	RUN_TEST_CASE(misc_uchar, ascii_round_trip);
	RUN_TEST_CASE(misc_uchar, null_and_empty);
	RUN_TEST_CASE(misc_uchar, c_locale_range);
}
