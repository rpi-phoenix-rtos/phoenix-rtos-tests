/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - math.h
 *
 * TESTED:
 *    - rint(), rintf(), nearbyint(), nearbyintf()
 *    - lrint(), llrint(), lround(), llround()
 *    - fdim(), fmax(), fmin(), copysign()
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <math.h>

#include "common.h"
#include <unity_fixture.h>


TEST_GROUP(math_round);


TEST_SETUP(math_round)
{
}


TEST_TEAR_DOWN(math_round)
{
}


/* rint()/nearbyint() round to nearest, ties to EVEN (default FE_TONEAREST) - this is what
 * distinguishes them from round(), which rounds halves away from zero. */
TEST(math_round, rint_basic)
{
	TEST_ASSERT_EQUAL_DOUBLE(2.0, rint(2.4));
	TEST_ASSERT_EQUAL_DOUBLE(3.0, rint(2.6));
	TEST_ASSERT_EQUAL_DOUBLE(-2.0, rint(-2.4));
	TEST_ASSERT_EQUAL_DOUBLE(-3.0, rint(-2.6));

	/* halves -> nearest even */
	TEST_ASSERT_EQUAL_DOUBLE(2.0, rint(2.5));
	TEST_ASSERT_EQUAL_DOUBLE(4.0, rint(3.5));
	TEST_ASSERT_EQUAL_DOUBLE(-2.0, rint(-2.5));
	TEST_ASSERT_EQUAL_DOUBLE(-4.0, rint(-3.5));
	TEST_ASSERT_DOUBLE_IS_ZERO(rint(0.5));
	TEST_ASSERT_DOUBLE_IS_NEG_ZERO(rint(-0.5));

	/* nearbyint() must agree with rint() */
	TEST_ASSERT_EQUAL_DOUBLE(2.0, nearbyint(2.5));
	TEST_ASSERT_EQUAL_DOUBLE(4.0, nearbyint(3.5));

	/* float variants */
	TEST_ASSERT_EQUAL_FLOAT(2.0f, rintf(2.5f));
	TEST_ASSERT_EQUAL_FLOAT(4.0f, rintf(3.5f));
	TEST_ASSERT_EQUAL_FLOAT(3.0f, nearbyintf(2.6f));
}


TEST(math_round, rint_special_val)
{
	TEST_ASSERT_DOUBLE_IS_NAN(rint(NAN));
	TEST_ASSERT_DOUBLE_IS_INF(rint(INFINITY));
	TEST_ASSERT_DOUBLE_IS_NEG_INF(rint(-INFINITY));
	TEST_ASSERT_DOUBLE_IS_ZERO(rint(0.0));
	TEST_ASSERT_DOUBLE_IS_NEG_ZERO(rint(-0.0));
}


/* lrint()/llrint() round to nearest-even and convert to integer;
 * lround()/llround() round halves away from zero. */
TEST(math_round, lrint_lround)
{
	TEST_ASSERT_EQUAL_INT64(2, lrint(2.5));  /* ties to even */
	TEST_ASSERT_EQUAL_INT64(4, lrint(3.5));
	TEST_ASSERT_EQUAL_INT64(-2, lrint(-2.5));
	TEST_ASSERT_EQUAL_INT64(3, lrint(2.6));
	TEST_ASSERT_EQUAL_INT64(2, llrint(2.4));
	TEST_ASSERT_EQUAL_INT64(4, llrint(3.5));

	TEST_ASSERT_EQUAL_INT64(3, lround(2.5));  /* ties away from zero */
	TEST_ASSERT_EQUAL_INT64(-3, lround(-2.5));
	TEST_ASSERT_EQUAL_INT64(1, lround(0.5));
	TEST_ASSERT_EQUAL_INT64(2, lround(2.4));
	TEST_ASSERT_EQUAL_INT64(4, llround(3.5));
	TEST_ASSERT_EQUAL_INT64(-4, llround(-3.5));
}


TEST(math_round, fdim_basic)
{
	TEST_ASSERT_EQUAL_DOUBLE(2.0, fdim(5.0, 3.0));
	TEST_ASSERT_DOUBLE_IS_ZERO(fdim(3.0, 5.0));
	TEST_ASSERT_DOUBLE_IS_ZERO(fdim(4.0, 4.0));
	TEST_ASSERT_EQUAL_DOUBLE(8.0, fdim(3.0, -5.0));
	TEST_ASSERT_DOUBLE_IS_NAN(fdim(NAN, 1.0));
	TEST_ASSERT_DOUBLE_IS_NAN(fdim(1.0, NAN));
	TEST_ASSERT_EQUAL_FLOAT(2.0f, fdimf(5.0f, 3.0f));
}


TEST(math_round, fmax_fmin)
{
	TEST_ASSERT_EQUAL_DOUBLE(5.0, fmax(3.0, 5.0));
	TEST_ASSERT_EQUAL_DOUBLE(5.0, fmax(5.0, 3.0));
	TEST_ASSERT_EQUAL_DOUBLE(3.0, fmin(3.0, 5.0));
	TEST_ASSERT_EQUAL_DOUBLE(3.0, fmin(5.0, 3.0));

	/* a NaN operand yields the other (C99 semantics) */
	TEST_ASSERT_EQUAL_DOUBLE(5.0, fmax(NAN, 5.0));
	TEST_ASSERT_EQUAL_DOUBLE(3.0, fmax(3.0, NAN));
	TEST_ASSERT_EQUAL_DOUBLE(5.0, fmin(NAN, 5.0));
	TEST_ASSERT_EQUAL_DOUBLE(3.0, fmin(3.0, NAN));
	TEST_ASSERT_DOUBLE_IS_NAN(fmax(NAN, NAN));
	TEST_ASSERT_DOUBLE_IS_NAN(fmin(NAN, NAN));

	TEST_ASSERT_EQUAL_FLOAT(5.0f, fmaxf(3.0f, 5.0f));
	TEST_ASSERT_EQUAL_FLOAT(3.0f, fminf(3.0f, 5.0f));
}


TEST(math_round, copysign_basic)
{
	TEST_ASSERT_EQUAL_DOUBLE(-3.0, copysign(3.0, -1.0));
	TEST_ASSERT_EQUAL_DOUBLE(3.0, copysign(-3.0, 1.0));
	TEST_ASSERT_EQUAL_DOUBLE(-3.0, copysign(3.0, -0.0));
	TEST_ASSERT_EQUAL_DOUBLE(3.0, copysign(3.0, 5.0));
	TEST_ASSERT_DOUBLE_IS_NEG_ZERO(copysign(0.0, -1.0));
	TEST_ASSERT_DOUBLE_IS_ZERO(copysign(0.0, 1.0));
	TEST_ASSERT_EQUAL_FLOAT(-3.0f, copysignf(3.0f, -1.0f));
}


TEST_GROUP_RUNNER(math_round)
{
	RUN_TEST_CASE(math_round, rint_basic);
	RUN_TEST_CASE(math_round, rint_special_val);
	RUN_TEST_CASE(math_round, lrint_lround);
	RUN_TEST_CASE(math_round, fdim_basic);
	RUN_TEST_CASE(math_round, fmax_fmin);
	RUN_TEST_CASE(math_round, copysign_basic);
}
