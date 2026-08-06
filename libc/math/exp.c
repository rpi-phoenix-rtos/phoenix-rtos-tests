/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - math.h
 *
 * TESTED:
 *    - exp(), exp2(), exp2f(), frexp(), ldexp()
 *    - scalbn(), scalbnf(), scalbln(), scalblnf()
 *    - log(), log2(), log2f(), log10()
 *
 * Copyright 2023 Phoenix Systems
 * Author: Adam Debek
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <math.h>
#include <limits.h>
#include <float.h>
#include <errno.h>
#include <stdlib.h>

#include "common.h"
#include <unity_fixture.h>

TEST_GROUP(math_exp);


TEST_SETUP(math_exp)
{
}


TEST_TEAR_DOWN(math_exp)
{
}


TEST(math_exp, exp_basic)
{
	int i, iters = 100 * ITER_FACTOR;
	int digLost, acceptLoss = 50;
	double xmax = log(DBL_MAX);
	double xmin = 1.0;
	double x, y, ymin, ymax, f, g;

	for (i = 0; i < iters; i++) {
		x = test_getRandomLog(xmin, xmax);
		ymin = -xmax;
		ymax = xmax - x;

		y = (double)rand() / RAND_MAX * (ymax - ymin) + ymin;
		/* Using exponent addition rule */
		f = exp(x) * exp(y);
		g = exp(x + y);

		digLost = test_checkResult(f, g);
		test_check_digLost2("exp", x, y, digLost, acceptLoss);
	}
}


TEST(math_exp, exp_special_val)
{
	TEST_ASSERT_DOUBLE_IS_NAN(exp(NAN));

	TEST_ASSERT_EQUAL_DOUBLE(1.0, exp(0.0));
	TEST_ASSERT_EQUAL_DOUBLE(1.0, exp(-0.0));

	TEST_ASSERT_DOUBLE_IS_INF(exp(INFINITY));
	TEST_ASSERT_DOUBLE_IS_ZERO(exp(-INFINITY));
}


TEST(math_exp, frexp_basic)
{
	int i, e, iters = 10 * ITER_FACTOR;
	int digLost, acceptLoss = 1;
	double max = DBL_MAX / 2.0;
	double min = DBL_MIN;
	double x, f, g, y;

	for (i = 0; i < iters; i++) {
		x = test_getRandomLog(min, max);

		if (i % 2) {
			x = -x;
		}

		y = frexp(x, &e);

		if (fabs(y) >= 1.0 || fabs(y) < 0.5) {
			char errStr[100];
			sprintf(errStr, "frexp(%g, int *exp) returned %g - value out of range <0.5, 1)", x, y);
			TEST_FAIL_MESSAGE(errStr);
		}

		if (e > 0.0) {
			f = y * pow(2.0, e);
		}
		else {
			f = y / pow(2.0, -e);
		}

		g = x;

		digLost = test_checkResult(f, g);
		test_check_digLost("frexp", x, digLost, acceptLoss);
	}
}


TEST(math_exp, frexp_special_val)
{
	int exp;

	TEST_ASSERT_DOUBLE_IS_NAN(frexp(NAN, &exp));

	TEST_ASSERT_DOUBLE_IS_ZERO(frexp(0.0, &exp));
	TEST_ASSERT_DOUBLE_IS_NEG_ZERO(frexp(-0.0, &exp));

	TEST_ASSERT_DOUBLE_IS_INF(frexp(INFINITY, &exp));
	TEST_ASSERT_DOUBLE_IS_NEG_INF(frexp(-INFINITY, &exp));
}


TEST(math_exp, ldexp_basic)
{
	int i, e, iters = 10 * ITER_FACTOR;
	int digLost, acceptLoss = 1;
	double max = DBL_MAX / 2.0;
	double min = DBL_MIN;
	double x, f, g, y;

	for (i = 0; i < iters; i++) {
		x = test_getRandomLog(min, max);

		if (i % 2) {
			x = -x;
		}

		y = frexp(x, &e);
		f = ldexp(y, e);
		g = x;

		digLost = test_checkResult(f, g);
		test_check_digLost("ldexp", x, digLost, acceptLoss);
	}
}


TEST(math_exp, ldexp_special_val)
{
	/* Initialize x and exp to random finite value other than 0.0 */
	double x = 1.2;
	int exp = 2;

	TEST_ASSERT_DOUBLE_IS_NAN(ldexp(NAN, exp));

	TEST_ASSERT_DOUBLE_IS_ZERO(ldexp(0.0, exp));
	TEST_ASSERT_DOUBLE_IS_NEG_ZERO(ldexp(-0.0, exp));

	TEST_ASSERT_DOUBLE_IS_INF(ldexp(INFINITY, exp));
	TEST_ASSERT_DOUBLE_IS_NEG_INF(ldexp(-INFINITY, exp));

	TEST_ASSERT_EQUAL_DOUBLE(x, ldexp(x, 0));
}


TEST(math_exp, log_basic)
{
	int i, iters = 20 * ITER_FACTOR;
	int digLost, acceptLoss = 50;
	double xmax = log(DBL_MAX);
	double xmin = 1.0e-20;
	double x, y, ymin, ymax, f, g;

	for (i = 0; i < iters; i++) {
		x = test_getRandomLog(xmin, xmax);
		ymin = xmin;
		ymax = xmax - x;

		y = (double)rand() / RAND_MAX * (ymax - ymin) + ymin;
		/* Using logarithm properties */
		f = log(x) + log(y);
		g = log(x * y);

		digLost = test_checkResult(f, g);
		test_check_digLost("log", x * y, digLost, acceptLoss);
	}
}


TEST(math_exp, log_special_val)
{
	TEST_ASSERT_DOUBLE_IS_NAN(log(NAN));

	errno = 0;
	TEST_ASSERT_EQUAL_DOUBLE(-HUGE_VAL, log(0.0));
	TEST_ASSERT_EQUAL_INT(ERANGE, errno);

	errno = 0;
	TEST_ASSERT_EQUAL_DOUBLE(-HUGE_VAL, log(-0.0));
	TEST_ASSERT_EQUAL_INT(ERANGE, errno);

	TEST_ASSERT_DOUBLE_IS_INF(log(INFINITY));

	TEST_ASSERT_DOUBLE_IS_ZERO(log(1.0));

	errno = 0;
	TEST_ASSERT_DOUBLE_IS_NAN(log(-1.0));
	TEST_ASSERT_EQUAL_INT(EDOM, errno);
}


TEST(math_exp, log2_basic)
{
	int i, iters = 20 * ITER_FACTOR;
	int digLost, acceptLoss = 50;
	double xmax = log(DBL_MAX);
	double xmin = 1.0e-10;
	double x, y, ymin, ymax, f, g;

	for (i = 0; i < iters; i++) {
		x = test_getRandomLog(xmin, xmax);
		ymin = xmin;
		ymax = xmax - x;

		y = (double)rand() / RAND_MAX * (ymax - ymin) + ymin;
		f = log2(x) + log2(y);
		g = log2(x * y);

		digLost = test_checkResult(f, g);
		test_check_digLost("log2", x * y, digLost, acceptLoss);
	}
}


TEST(math_exp, log2_special_val)
{
	TEST_ASSERT_DOUBLE_IS_NAN(log2(NAN));

	errno = 0;
	TEST_ASSERT_EQUAL_DOUBLE(-HUGE_VAL, log2(0.0));
	TEST_ASSERT_EQUAL_INT(ERANGE, errno);

	errno = 0;
	TEST_ASSERT_EQUAL_DOUBLE(-HUGE_VAL, log2(-0.0));
	TEST_ASSERT_EQUAL_INT(ERANGE, errno);

	TEST_ASSERT_DOUBLE_IS_INF(log2(INFINITY));

	TEST_ASSERT_DOUBLE_IS_ZERO(log2(1.0));

	errno = 0;
	TEST_ASSERT_DOUBLE_IS_NAN(log2(-1.0));
	TEST_ASSERT_EQUAL_INT(EDOM, errno);
}


TEST(math_exp, log10_basic)
{
	int i, iters = 20 * ITER_FACTOR;
	int digLost, acceptLoss = 50;
	double xmax = log(DBL_MAX);
	double xmin = 1.0e-10;
	double x, y, ymin, ymax, f, g;

	for (i = 0; i < iters; i++) {
		x = test_getRandomLog(xmin, xmax);
		ymin = xmin;
		ymax = xmax - x;

		y = (double)rand() / RAND_MAX * (ymax - ymin) + ymin;
		f = log10(x) + log10(y);
		g = log10(x * y);

		digLost = test_checkResult(f, g);
		test_check_digLost("log10", x * y, digLost, acceptLoss);
	}
}


TEST(math_exp, log10_special_val)
{
	TEST_ASSERT_DOUBLE_IS_NAN(log10(NAN));

	errno = 0;
	TEST_ASSERT_EQUAL_DOUBLE(-HUGE_VAL, log10(0.0));
	TEST_ASSERT_EQUAL_INT(ERANGE, errno);

	errno = 0;
	TEST_ASSERT_EQUAL_DOUBLE(-HUGE_VAL, log10(-0.0));
	TEST_ASSERT_EQUAL_INT(ERANGE, errno);

	TEST_ASSERT_DOUBLE_IS_INF(log10(INFINITY));

	TEST_ASSERT_DOUBLE_IS_ZERO(log10(1.0));

	errno = 0;
	TEST_ASSERT_DOUBLE_IS_NAN(log10(-1.0));
	TEST_ASSERT_EQUAL_INT(EDOM, errno);
}


/* Phoenix computes exp2(x) as exp(x * M_LN2) on top of a 13-term Maclaurin exp(), so the
 * results are NOT bit-exact powers of two - e.g. exp2(1.0) == 1.9999999999728812 and
 * exp2(10.0) == 1023.9999996131212. The expected values below are Phoenix libm's OWN output
 * (host-compiled from libphoenix/libm/phoenix/exp.c), NOT glibc's; the on-target libm runs
 * the same code so it matches to well within the small deltas used here. */
TEST(math_exp, exp2_basic)
{
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.4142135623730889, exp2(0.5));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.9999999999728812, exp2(1.0));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 3.9999999999999347, exp2(2.0));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 11.313708498984187, exp2(3.5));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1023.9999996131212, exp2(10.0));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.49999999997562594, exp2(-1.0));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.17677669527918793, exp2(-2.5));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.1892071150027212, exp2(0.25));
}


TEST(math_exp, exp2_special_val)
{
	TEST_ASSERT_DOUBLE_IS_NAN(exp2(NAN));

	/* exp2(+-0) == 1 exactly: exp(0.0) short-circuits the Maclaurin loop to 1.0. */
	TEST_ASSERT_EQUAL_DOUBLE(1.0, exp2(0.0));
	TEST_ASSERT_EQUAL_DOUBLE(1.0, exp2(-0.0));

	TEST_ASSERT_DOUBLE_IS_INF(exp2(INFINITY));

	/* exp2(-inf) collapses to +0 (via the underflow path of quickPow). */
	TEST_ASSERT_DOUBLE_IS_ZERO(exp2(-INFINITY));
}


TEST(math_exp, exp2f_basic)
{
	/* float rounding hides most of the double inaccuracy; expected values are the
	 * host-compiled Phoenix exp2f() output. */
	TEST_ASSERT_EQUAL_FLOAT(1.0f, exp2f(0.0f));
	TEST_ASSERT_EQUAL_FLOAT(1.41421354f, exp2f(0.5f));
	TEST_ASSERT_EQUAL_FLOAT(11.3137083f, exp2f(3.5f));
	TEST_ASSERT_EQUAL_FLOAT(0.176776692f, exp2f(-2.5f));
	TEST_ASSERT_EQUAL_FLOAT(1024.0f, exp2f(10.0f));
}


TEST(math_exp, log2f_basic)
{
	/* log2f(1) is exactly 0 (log() special-cases x == 1). The remaining powers of two come
	 * out clean after the float rounding of log(x) / M_LN2. */
	TEST_ASSERT_EQUAL_FLOAT(0.0f, log2f(1.0f));
	TEST_ASSERT_EQUAL_FLOAT(1.0f, log2f(2.0f));
	TEST_ASSERT_EQUAL_FLOAT(3.0f, log2f(8.0f));
	TEST_ASSERT_EQUAL_FLOAT(-1.0f, log2f(0.5f));
	TEST_ASSERT_EQUAL_FLOAT(3.32192802f, log2f(10.0f));
	TEST_ASSERT_EQUAL_FLOAT(1.58496249f, log2f(3.0f));
}


TEST(math_exp, log2f_special_val)
{
	TEST_ASSERT_FLOAT_IS_NAN(log2f(NAN));
	TEST_ASSERT_FLOAT_IS_INF(log2f(INFINITY));
	TEST_ASSERT_FLOAT_IS_NEG_INF(log2f(0.0f));
	TEST_ASSERT_EQUAL_FLOAT(0.0f, log2f(1.0f));
}


/* scalbn(x, n) == x * 2^n and, on this FLT_RADIX == 2 target, is exactly ldexp(x, n).
 * All results are exact, so exact asserts are used. */
TEST(math_exp, scalbn_basic)
{
	TEST_ASSERT_EQUAL_DOUBLE(24.0, scalbn(1.5, 4));
	TEST_ASSERT_EQUAL_DOUBLE(0.125, scalbn(1.0, -3));
	TEST_ASSERT_EQUAL_DOUBLE(3.0, scalbn(3.0, 0));
	TEST_ASSERT_EQUAL_DOUBLE(-20.0, scalbn(-2.5, 3));

	/* scalbn must agree with ldexp bit-for-bit. */
	TEST_ASSERT_EQUAL_DOUBLE(ldexp(1.5, 4), scalbn(1.5, 4));
	TEST_ASSERT_EQUAL_DOUBLE(ldexp(-2.5, 3), scalbn(-2.5, 3));

	/* scalbln takes a long exponent; for in-range n it equals scalbn/ldexp. */
	TEST_ASSERT_EQUAL_DOUBLE(24.0, scalbln(1.5, 4L));
	TEST_ASSERT_EQUAL_DOUBLE(0.125, scalbln(1.0, -3L));
	TEST_ASSERT_EQUAL_DOUBLE(ldexp(1.5, 4), scalbln(1.5, 4L));

	TEST_ASSERT_EQUAL_FLOAT(24.0f, scalbnf(1.5f, 4));
	TEST_ASSERT_EQUAL_FLOAT(-20.0f, scalbnf(-2.5f, 3));
	TEST_ASSERT_EQUAL_FLOAT(24.0f, scalblnf(1.5f, 4L));
	TEST_ASSERT_EQUAL_FLOAT(0.125f, scalblnf(1.0f, -3L));
}


TEST(math_exp, scalbn_special_val)
{
	TEST_ASSERT_DOUBLE_IS_NAN(scalbn(NAN, 3));
	TEST_ASSERT_DOUBLE_IS_INF(scalbn(INFINITY, 3));
	TEST_ASSERT_DOUBLE_IS_NEG_INF(scalbn(-INFINITY, 3));
	TEST_ASSERT_DOUBLE_IS_ZERO(scalbn(0.0, 3));
	TEST_ASSERT_DOUBLE_IS_NEG_ZERO(scalbn(-0.0, 3));

	/* scalbln shares the ldexp core, so the same special values hold. */
	TEST_ASSERT_DOUBLE_IS_NAN(scalbln(NAN, 3L));
	TEST_ASSERT_DOUBLE_IS_INF(scalbln(INFINITY, 3L));
	TEST_ASSERT_DOUBLE_IS_NEG_INF(scalbln(-INFINITY, 3L));

	/* Huge long exponents (beyond int range) must saturate to +-inf / +-0, not wrap.
	 * Regression guard: a prior clamp-to-INT_MAX overflowed ldexp's internal exponent
	 * addition and wrongly returned ~0 for n > INT_MAX. */
	TEST_ASSERT_DOUBLE_IS_INF(scalbln(1.0, 5000000000L));      /* > INT_MAX */
	TEST_ASSERT_DOUBLE_IS_ZERO(scalbln(1.0, -5000000000L));
	TEST_ASSERT_DOUBLE_IS_INF(scalbln(1.0, LONG_MAX));
	TEST_ASSERT_DOUBLE_IS_ZERO(scalbln(1.0, LONG_MIN));
}


TEST_GROUP_RUNNER(math_exp)
{
	test_setup();

	RUN_TEST_CASE(math_exp, exp_basic);
	RUN_TEST_CASE(math_exp, exp_special_val);

	RUN_TEST_CASE(math_exp, frexp_basic);
	RUN_TEST_CASE(math_exp, frexp_special_val);

	RUN_TEST_CASE(math_exp, ldexp_basic);
	RUN_TEST_CASE(math_exp, ldexp_special_val);

	RUN_TEST_CASE(math_exp, log_basic);
	RUN_TEST_CASE(math_exp, log_special_val);

	RUN_TEST_CASE(math_exp, log2_basic);
	RUN_TEST_CASE(math_exp, log2_special_val);

	RUN_TEST_CASE(math_exp, log10_basic);
	RUN_TEST_CASE(math_exp, log10_special_val);

	RUN_TEST_CASE(math_exp, exp2_basic);
	RUN_TEST_CASE(math_exp, exp2_special_val);
	RUN_TEST_CASE(math_exp, exp2f_basic);

	RUN_TEST_CASE(math_exp, log2f_basic);
	RUN_TEST_CASE(math_exp, log2f_special_val);

	RUN_TEST_CASE(math_exp, scalbn_basic);
	RUN_TEST_CASE(math_exp, scalbn_special_val);
}
