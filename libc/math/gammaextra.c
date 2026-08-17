/*
 * Phoenix-RTOS
 *
 * math tests for the libphoenix libm additions:
 *   tgamma, lgamma, lgamma_r, exp10, remainder, drem, logb, ilogb,
 *   scalb, significand
 *
 * Reference values computed with glibc; phoenix libm accuracy ~1e-12.
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


TEST_GROUP(math_gammaextra);


TEST_SETUP(math_gammaextra)
{
}


TEST_TEAR_DOWN(math_gammaextra)
{
}


/* --- gamma function --- */

TEST(math_gammaextra, tgamma_basic)
{
	/* tgamma = Lanczos * pow * exp; it inherits phoenix libm's ~1e-7 transcendental
	 * accuracy, so tolerances are magnitude-scaled (not 1e-15 as with glibc). */
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.0, tgamma(1.0));
	TEST_ASSERT_DOUBLE_WITHIN(1e-4, 24.0, tgamma(5.0));      /* 4! */
	TEST_ASSERT_DOUBLE_WITHIN(1e-3, 120.0, tgamma(6.0));     /* 5! */
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.7724538509055159, tgamma(0.5));  /* sqrt(pi) */
	TEST_ASSERT_DOUBLE_WITHIN(1e-5, 3.3233509704478426, tgamma(3.5));
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, -3.5449077018110318, tgamma(-0.5)); /* -2 sqrt(pi) */
}


TEST(math_gammaextra, tgamma_special)
{
	TEST_ASSERT_EQUAL_DOUBLE(INFINITY, tgamma(0.0));   /* +0 -> +inf */
	TEST_ASSERT_DOUBLE_IS_NAN(tgamma(-3.0));           /* pole at negative integer */
	TEST_ASSERT_EQUAL_DOUBLE(INFINITY, tgamma(INFINITY));
	TEST_ASSERT_DOUBLE_IS_NAN(tgamma(-INFINITY));
	TEST_ASSERT_DOUBLE_IS_NAN(tgamma(NAN));
}


/* --- log-gamma --- */

TEST(math_gammaextra, lgamma_basic)
{
	TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, lgamma(1.0));  /* G(1)=1 -> ln=0 (Lanczos ~1e-15) */
	TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, lgamma(2.0));
	TEST_ASSERT_DOUBLE_WITHIN(1e-7, 3.1780538303479458, lgamma(5.0));   /* ln(24) */
	TEST_ASSERT_DOUBLE_WITHIN(1e-7, 0.57236494292470008, lgamma(0.5));  /* ln(sqrt(pi)) */
	TEST_ASSERT_DOUBLE_WITHIN(1e-5, 359.13420536957539, lgamma(100.0));
}


TEST(math_gammaextra, lgamma_r_sign)
{
	int sign;

	sign = 0;
	(void)lgamma_r(3.0, &sign);
	TEST_ASSERT_EQUAL_INT(1, sign);          /* G(3) = 2 > 0 */

	sign = 0;
	(void)lgamma_r(-0.5, &sign);
	TEST_ASSERT_EQUAL_INT(-1, sign);         /* G(-0.5) = -2 sqrt(pi) < 0 */

	TEST_ASSERT_EQUAL_DOUBLE(INFINITY, lgamma(0.0));   /* pole */
	TEST_ASSERT_EQUAL_DOUBLE(INFINITY, lgamma(-2.0));  /* pole */
}


/* --- exp10 --- */

TEST(math_gammaextra, exp10_basic)
{
	TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0, exp10(0.0));
	TEST_ASSERT_DOUBLE_WITHIN(1e-4, 1000.0, exp10(3.0));   /* pow(10,x): ~1e-7 rel */
	TEST_ASSERT_DOUBLE_WITHIN(1e-7, 0.1, exp10(-1.0));
	TEST_ASSERT_DOUBLE_WITHIN(1e-3, 316.22776601683796, exp10(2.5));
}


/* --- remainder / drem (IEEE, round-half-to-even) --- */

TEST(math_gammaextra, remainder_basic)
{
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, -1.0, remainder(5.0, 3.0));  /* 5/3->2, 5-6 */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0, remainder(5.0, 2.0));   /* 5/2->2 (even), 5-4 */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, -1.0, remainder(7.0, 2.0));  /* 7/2->4 (even), 7-8 */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, -0.7, remainder(5.3, 2.0));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.7, remainder(-5.3, 2.0));  /* odd function */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, -1.0, drem(5.0, 3.0));
	TEST_ASSERT_DOUBLE_IS_NAN(remainder(1.0, 0.0));
}


/* --- logb / ilogb --- */

TEST(math_gammaextra, logb_ilogb)
{
	TEST_ASSERT_EQUAL_DOUBLE(3.0, logb(8.0));
	TEST_ASSERT_EQUAL_DOUBLE(0.0, logb(1.0));
	TEST_ASSERT_EQUAL_DOUBLE(-1.0, logb(0.5));
	TEST_ASSERT_EQUAL_DOUBLE(10.0, logb(1024.0));
	TEST_ASSERT_EQUAL_DOUBLE(-INFINITY, logb(0.0));

	TEST_ASSERT_EQUAL_INT(3, ilogb(8.0));
	TEST_ASSERT_EQUAL_INT(0, ilogb(1.0));
	TEST_ASSERT_EQUAL_INT(-2, ilogb(0.25));
}


/* --- scalb / significand (obsolete BSD) --- */

TEST(math_gammaextra, scalb_significand)
{
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 48.0, scalb(3.0, 4.0));   /* 3 * 2^4 */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.75, scalb(3.0, -2.0));  /* 3 * 2^-2 */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.5, significand(12.0));  /* 12 = 1.5 * 2^3 */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.25, significand(5.0));  /* 5 = 1.25 * 2^2 */
}


TEST_GROUP_RUNNER(math_gammaextra)
{
	RUN_TEST_CASE(math_gammaextra, tgamma_basic);
	RUN_TEST_CASE(math_gammaextra, tgamma_special);
	RUN_TEST_CASE(math_gammaextra, lgamma_basic);
	RUN_TEST_CASE(math_gammaextra, lgamma_r_sign);
	RUN_TEST_CASE(math_gammaextra, exp10_basic);
	RUN_TEST_CASE(math_gammaextra, remainder_basic);
	RUN_TEST_CASE(math_gammaextra, logb_ilogb);
	RUN_TEST_CASE(math_gammaextra, scalb_significand);
}
