/*
 * Phoenix-RTOS
 *
 *    libc-tests
 *    HEADER:
 *    - stdlib.h
 *    TESTED:
 *    - getsubopt()
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdlib.h>
#include <string.h>
#include <unity_fixture.h>


static char *const tokens[] = { "ro", "rw", "data", NULL };


TEST_GROUP(stdlib_getsubopt);


TEST_SETUP(stdlib_getsubopt)
{
}


TEST_TEAR_DOWN(stdlib_getsubopt)
{
}


TEST(stdlib_getsubopt, sequence)
{
	char opts[] = "ro,rw=1,data=/x,foo";
	char *p = opts;
	char *value;

	TEST_ASSERT_EQUAL_INT(0, getsubopt(&p, tokens, &value));
	TEST_ASSERT_NULL(value);

	TEST_ASSERT_EQUAL_INT(1, getsubopt(&p, tokens, &value));
	TEST_ASSERT_NOT_NULL(value);
	TEST_ASSERT_EQUAL_STRING("1", value);

	TEST_ASSERT_EQUAL_INT(2, getsubopt(&p, tokens, &value));
	TEST_ASSERT_NOT_NULL(value);
	TEST_ASSERT_EQUAL_STRING("/x", value);

	/* unrecognised token: -1, value points at the whole token */
	TEST_ASSERT_EQUAL_INT(-1, getsubopt(&p, tokens, &value));
	TEST_ASSERT_NOT_NULL(value);
	TEST_ASSERT_EQUAL_STRING("foo", value);

	/* exhausted */
	TEST_ASSERT_EQUAL_INT(-1, getsubopt(&p, tokens, &value));
	TEST_ASSERT_NULL(value);
}


TEST(stdlib_getsubopt, no_prefix_false_match)
{
	/* "rwx" must NOT match the "rw" token by prefix. */
	char opts[] = "rwx";
	char *p = opts;
	char *value;

	TEST_ASSERT_EQUAL_INT(-1, getsubopt(&p, tokens, &value));
	TEST_ASSERT_NOT_NULL(value);
	TEST_ASSERT_EQUAL_STRING("rwx", value);
}


TEST(stdlib_getsubopt, empty)
{
	char opts[] = "";
	char *p = opts;
	char *value;

	TEST_ASSERT_EQUAL_INT(-1, getsubopt(&p, tokens, &value));
	TEST_ASSERT_NULL(value);
}


TEST_GROUP_RUNNER(stdlib_getsubopt)
{
	RUN_TEST_CASE(stdlib_getsubopt, sequence);
	RUN_TEST_CASE(stdlib_getsubopt, no_prefix_false_match);
	RUN_TEST_CASE(stdlib_getsubopt, empty);
}
