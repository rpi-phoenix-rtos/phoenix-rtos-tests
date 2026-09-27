/*
 * Phoenix-RTOS
 *
 * C11 standard library tests
 *
 * HEADER:
 *    - assert.h
 *
 * TESTED:
 *    - static_assert (C11 7.2p3: a macro expanding to _Static_assert)
 *
 * <assert.h> lacked the macro, so every C11 translation unit that used the
 * spelling the standard gives it failed to compile: Mesa alone reported 657+193
 * "implicit declaration of 'static_assert'" errors. Most of this group is
 * checked by the COMPILER -- if the macro is missing, this file does not build,
 * which is the failure that matters. The runtime cases pin the parts a compile
 * cannot: that the macro really is a macro in C11/C17 (and a keyword, not a
 * macro, in C23), and that NDEBUG does not switch it off.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <assert.h>
#include <limits.h>
#include <stddef.h>

#include <unity_fixture.h>


/* File scope: static_assert is a declaration, so this is legal at file scope. */
static_assert(CHAR_BIT == 8, "a byte is 8 bits");
static_assert(sizeof(char) == 1, "sizeof(char) is 1 by definition");


struct assert_static_probe {
	char c;
	int i;
};


TEST_GROUP(assert_static);


TEST_SETUP(assert_static)
{
}


TEST_TEAR_DOWN(assert_static)
{
}


TEST(assert_static, is_a_macro_before_c23)
{
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && (__STDC_VERSION__ < 202311L)
#ifndef static_assert
	TEST_FAIL_MESSAGE("C11/C17: <assert.h> must define static_assert as a macro");
#endif
#endif
	TEST_PASS();
}


TEST(assert_static, block_scope)
{
	/* ...and inside a function body, where it is a declaration too. */
	static_assert(offsetof(struct assert_static_probe, c) == 0, "first member at offset 0");
	static_assert(sizeof(struct assert_static_probe) >= sizeof(int) + 1, "struct holds both members");

	TEST_PASS();
}


/* Re-including <assert.h> with NDEBUG must disable assert() but NOT
 * static_assert (C11 7.2p1 makes only assert depend on NDEBUG). A release
 * build may already pass -DNDEBUG, so define it only when it is not set. */
#ifndef NDEBUG
#define NDEBUG
#endif
#include <assert.h>

static_assert(1, "still available after re-inclusion with NDEBUG");


TEST(assert_static, survives_ndebug)
{
	int evaluated = 0;

	static_assert(sizeof(int) >= 2, "int is at least 16 bits");

	/* With NDEBUG, assert() must not evaluate its argument. */
	assert((evaluated = 1) != 0);
	TEST_ASSERT_EQUAL_INT(0, evaluated);
}


TEST_GROUP_RUNNER(assert_static)
{
	RUN_TEST_CASE(assert_static, is_a_macro_before_c23);
	RUN_TEST_CASE(assert_static, block_scope);
	RUN_TEST_CASE(assert_static, survives_ndebug);
}
