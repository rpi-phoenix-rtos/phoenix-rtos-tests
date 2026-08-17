/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - math.h
 *
 * TESTED:
 *    - log1p(), expm1(), asinh(), acosh(), atanh()
 *    - floorl(), ceill(), llroundl()
 *    - nextafter(), nexttoward()
 *  (libphoenix additions; reference values from glibc)
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <math.h>
#include <float.h>

#include "common.h"
#include <unity_fixture.h>


TEST_GROUP(math_c99extra);


TEST_SETUP(math_c99extra)
{
}


TEST_TEAR_DOWN(math_c99extra)
{
}


TEST(math_c99extra, log1p_basic)
{
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.40546510810816438, log1p(0.5));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 9.9999999999949996e-13, log1p(1e-12)); /* tiny x: ~x */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, -0.69314718055994531, log1p(-0.5));
}


TEST(math_c99extra, log1p_special_val)
{
	TEST_ASSERT_DOUBLE_IS_ZERO(log1p(0.0));
	TEST_ASSERT_DOUBLE_IS_NEG_ZERO(log1p(-0.0));
	TEST_ASSERT_DOUBLE_IS_NAN(log1p(NAN));
	TEST_ASSERT_EQUAL_DOUBLE(-INFINITY, log1p(-1.0)); /* pole */
	TEST_ASSERT_DOUBLE_IS_NAN(log1p(-2.0));           /* domain */
	TEST_ASSERT_EQUAL_DOUBLE(INFINITY, log1p(INFINITY));
}


TEST(math_c99extra, expm1_basic)
{
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.64872127070012819, expm1(0.5));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0000000000005e-12, expm1(1e-12)); /* tiny x: ~x */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, -0.39346934028736658, expm1(-0.5));
}


TEST(math_c99extra, expm1_special_val)
{
	TEST_ASSERT_DOUBLE_IS_ZERO(expm1(0.0));
	TEST_ASSERT_DOUBLE_IS_NEG_ZERO(expm1(-0.0));
	TEST_ASSERT_DOUBLE_IS_NAN(expm1(NAN));
	TEST_ASSERT_EQUAL_DOUBLE(INFINITY, expm1(INFINITY));
	TEST_ASSERT_EQUAL_DOUBLE(-1.0, expm1(-INFINITY));
}


TEST(math_c99extra, asinh_basic)
{
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.88137358701954305, asinh(1.0));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, -1.8184464592320668, asinh(-3.0));
	/* inverse of sinh */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 2.0, asinh(sinh(2.0)));
}


TEST(math_c99extra, asinh_special_val)
{
	TEST_ASSERT_DOUBLE_IS_ZERO(asinh(0.0));
	TEST_ASSERT_DOUBLE_IS_NEG_ZERO(asinh(-0.0));
	TEST_ASSERT_DOUBLE_IS_NAN(asinh(NAN));
	TEST_ASSERT_EQUAL_DOUBLE(INFINITY, asinh(INFINITY));
	TEST_ASSERT_EQUAL_DOUBLE(-INFINITY, asinh(-INFINITY));
}


TEST(math_c99extra, acosh_basic)
{
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.3169578969248168, acosh(2.0));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 3.0, acosh(cosh(3.0)));
}


TEST(math_c99extra, acosh_special_val)
{
	TEST_ASSERT_DOUBLE_IS_ZERO(acosh(1.0));
	TEST_ASSERT_DOUBLE_IS_NAN(acosh(NAN));
	TEST_ASSERT_DOUBLE_IS_NAN(acosh(0.5)); /* domain x < 1 */
	TEST_ASSERT_EQUAL_DOUBLE(INFINITY, acosh(INFINITY));
}


TEST(math_c99extra, atanh_basic)
{
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.54930614433405489, atanh(0.5));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, -1.4722194895832204, atanh(-0.9));
}


TEST(math_c99extra, atanh_special_val)
{
	TEST_ASSERT_DOUBLE_IS_ZERO(atanh(0.0));
	TEST_ASSERT_DOUBLE_IS_NEG_ZERO(atanh(-0.0));
	TEST_ASSERT_DOUBLE_IS_NAN(atanh(NAN));
	TEST_ASSERT_EQUAL_DOUBLE(INFINITY, atanh(1.0));   /* pole */
	TEST_ASSERT_EQUAL_DOUBLE(-INFINITY, atanh(-1.0)); /* pole */
	TEST_ASSERT_DOUBLE_IS_NAN(atanh(2.0));            /* domain |x| > 1 */
}


TEST(math_c99extra, floorl_ceill_llroundl)
{
	/* long double (128-bit) rounding; exact integer results */
	TEST_ASSERT_TRUE(floorl(2.7L) == 2.0L);
	TEST_ASSERT_TRUE(floorl(-2.5L) == -3.0L);
	TEST_ASSERT_TRUE(ceill(2.3L) == 3.0L);
	TEST_ASSERT_TRUE(ceill(-2.5L) == -2.0L);
	TEST_ASSERT_TRUE(floorl(5.0L) == 5.0L); /* already integral */
	TEST_ASSERT_EQUAL_INT64(3, llroundl(2.5L));   /* round half away from zero */
	TEST_ASSERT_EQUAL_INT64(-3, llroundl(-2.5L));
	TEST_ASSERT_EQUAL_INT64(2, llroundl(2.4L));
	TEST_ASSERT_EQUAL_INT64(0, llroundl(0.0L));
}


TEST(math_c99extra, nextafter_nexttoward)
{
	/* one ULP steps: 1 + 2^-52 = 0x1.0000000000001p0 */
	TEST_ASSERT_TRUE(nextafter(1.0, 2.0) == 0x1.0000000000001p0);
	TEST_ASSERT_TRUE(nextafter(1.0, 0.0) == 0x1.fffffffffffffp-1); /* 1 - 2^-53 */
	TEST_ASSERT_EQUAL_DOUBLE(2.0, nextafter(2.0, 2.0)); /* x == y -> y */
	TEST_ASSERT_DOUBLE_IS_NAN(nextafter(NAN, 1.0));
	TEST_ASSERT_TRUE(nexttoward(1.0, 2.0L) == 0x1.0000000000001p0);
	TEST_ASSERT_EQUAL_DOUBLE(5.0, nexttoward(5.0, 5.0L));
}


TEST_GROUP_RUNNER(math_c99extra)
{
	RUN_TEST_CASE(math_c99extra, log1p_basic);
	RUN_TEST_CASE(math_c99extra, log1p_special_val);
	RUN_TEST_CASE(math_c99extra, expm1_basic);
	RUN_TEST_CASE(math_c99extra, expm1_special_val);
	RUN_TEST_CASE(math_c99extra, asinh_basic);
	RUN_TEST_CASE(math_c99extra, asinh_special_val);
	RUN_TEST_CASE(math_c99extra, acosh_basic);
	RUN_TEST_CASE(math_c99extra, acosh_special_val);
	RUN_TEST_CASE(math_c99extra, atanh_basic);
	RUN_TEST_CASE(math_c99extra, atanh_special_val);
	RUN_TEST_CASE(math_c99extra, floorl_ceill_llroundl);
	RUN_TEST_CASE(math_c99extra, nextafter_nexttoward);
}
