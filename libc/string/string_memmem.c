/*
 * Phoenix-RTOS
 *
 *    libc-tests
 *    HEADER:
 *    - string.h
 *    TESTED:
 *    - memmem()
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <string.h>
#include <unity_fixture.h>


TEST_GROUP(string_memmem);


TEST_SETUP(string_memmem)
{
}


TEST_TEAR_DOWN(string_memmem)
{
}


TEST(string_memmem, found)
{
	const char hs[] = "hello world";

	TEST_ASSERT_EQUAL_PTR(hs + 6, memmem(hs, sizeof(hs) - 1, "world", 5));
	TEST_ASSERT_EQUAL_PTR(hs + 0, memmem(hs, sizeof(hs) - 1, "hello", 5));
	TEST_ASSERT_EQUAL_PTR(hs + 10, memmem(hs, sizeof(hs) - 1, "d", 1));
}


TEST(string_memmem, not_found)
{
	const char hs[] = "hello world";

	TEST_ASSERT_NULL(memmem(hs, sizeof(hs) - 1, "xyz", 3));
	/* needle longer than haystack must not match */
	TEST_ASSERT_NULL(memmem("abc", 3, "abcd", 4));
}


TEST(string_memmem, empty_needle)
{
	/* A zero-length needle matches at the start of the haystack. */
	const char hs[] = "abc";

	TEST_ASSERT_EQUAL_PTR(hs, memmem(hs, 3, "", 0));
}


TEST(string_memmem, first_occurrence)
{
	const char hs[] = "abcabc";

	TEST_ASSERT_EQUAL_PTR(hs + 1, memmem(hs, 6, "bc", 2));
	TEST_ASSERT_EQUAL_PTR(hs + 2, memmem(hs, 6, "cab", 3));
}


TEST(string_memmem, embedded_nul)
{
	/* memmem is length-bounded, so it searches past embedded NULs. */
	const char hs[] = { 'a', '\0', 'b', 'c' };
	const char nd[] = { '\0', 'b' };

	TEST_ASSERT_EQUAL_PTR(hs + 1, memmem(hs, sizeof(hs), nd, sizeof(nd)));
}


TEST_GROUP_RUNNER(string_memmem)
{
	RUN_TEST_CASE(string_memmem, found);
	RUN_TEST_CASE(string_memmem, not_found);
	RUN_TEST_CASE(string_memmem, empty_needle);
	RUN_TEST_CASE(string_memmem, first_occurrence);
	RUN_TEST_CASE(string_memmem, embedded_nul);
}
