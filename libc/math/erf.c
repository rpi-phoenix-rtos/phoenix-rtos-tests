/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - math.h
 *
 * TESTED:
 *    - erf(), erff()
 *    - erfc(), erfcf()
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


TEST_GROUP(math_erf);


TEST_SETUP(math_erf)
{
}


TEST_TEAR_DOWN(math_erf)
{
}


/* Expected values are the host-compiled output of Phoenix libm's OWN erf() family
 * (libphoenix/libm/phoenix/erf.c, an fdlibm-derived implementation), NOT glibc's. The
 * on-target libm runs the identical code, so the values match within the small deltas.
 * Inputs deliberately straddle the algorithm's |x| branch cut-offs (0.84375, 1.25,
 * 1/0.35 ~ 2.857, 6) for coverage. */
TEST(math_erf, erf_basic)
{
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.27632639016823696, erf(0.25));  /* |x| < 0.84375 */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.52049987781304652, erf(0.5));   /* |x| < 0.84375 */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.84270079294971489, erf(1.0));   /* [0.84375, 1.25) */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.99532226501896925, erf(2.0));   /* [1.25, 2.857) */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.99997790950300147, erf(3.0));   /* [2.857, 6) */

	/* erf is odd: erf(-x) == -erf(x). */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, -0.84270079294971489, erf(-1.0));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, -0.7111556336535152, erf(-0.75));
}


TEST(math_erf, erf_special_val)
{
	TEST_ASSERT_DOUBLE_IS_NAN(erf(NAN));

	/* erf(+-0) == +-0 (sign preserved). */
	TEST_ASSERT_DOUBLE_IS_ZERO(erf(0.0));
	TEST_ASSERT_DOUBLE_IS_NEG_ZERO(erf(-0.0));

	/* erf(+-inf) == +-1 exactly. */
	TEST_ASSERT_EQUAL_DOUBLE(1.0, erf(INFINITY));
	TEST_ASSERT_EQUAL_DOUBLE(-1.0, erf(-INFINITY));

	/* |x| >= 6 saturates to +-1 exactly (distinct code branch from the inf case). */
	TEST_ASSERT_EQUAL_DOUBLE(1.0, erf(7.0));
	TEST_ASSERT_EQUAL_DOUBLE(-1.0, erf(-7.0));
}


TEST(math_erf, erfc_basic)
{
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.7236736098317631, erfc(0.25));       /* |x| < 0.84375 */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.47950012218695348, erfc(0.5));       /* |x| < 0.84375 */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.15729920705028513, erfc(1.0));       /* [0.84375, 1.25) */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.0046777349810307838, erfc(2.0));     /* [1.25, 2.857) */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 2.2090496998507612e-05, erfc(3.0));    /* [2.857, 28) */

	/* erfc(-x) == 2 - erfc(x). */
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.842700792949715, erfc(-1.0));
	TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.7111556336535152, erfc(-0.75));
}


TEST(math_erf, erfc_special_val)
{
	TEST_ASSERT_DOUBLE_IS_NAN(erfc(NAN));

	/* erfc(0) == 1, erfc(+inf) == 0, erfc(-inf) == 2, all exact. */
	TEST_ASSERT_EQUAL_DOUBLE(1.0, erfc(0.0));
	TEST_ASSERT_EQUAL_DOUBLE(1.0, erfc(-0.0));
	TEST_ASSERT_DOUBLE_IS_ZERO(erfc(INFINITY));
	TEST_ASSERT_EQUAL_DOUBLE(2.0, erfc(-INFINITY));
}


TEST(math_erf, erff_erfcf_basic)
{
	/* erff/erfcf are (float)erf((double)x) / (float)erfc((double)x); expected values are
	 * the host-compiled Phoenix float output. */
	TEST_ASSERT_EQUAL_FLOAT(0.520499885f, erff(0.5f));
	TEST_ASSERT_EQUAL_FLOAT(0.842700779f, erff(1.0f));
	TEST_ASSERT_EQUAL_FLOAT(0.995322287f, erff(2.0f));
	TEST_ASSERT_EQUAL_FLOAT(-0.842700779f, erff(-1.0f));

	TEST_ASSERT_EQUAL_FLOAT(0.479500115f, erfcf(0.5f));
	TEST_ASSERT_EQUAL_FLOAT(0.157299206f, erfcf(1.0f));
	TEST_ASSERT_EQUAL_FLOAT(0.0046777348f, erfcf(2.0f));
}


TEST(math_erf, erff_erfcf_special_val)
{
	TEST_ASSERT_FLOAT_IS_NAN(erff(NAN));
	TEST_ASSERT_FLOAT_IS_NAN(erfcf(NAN));

	TEST_ASSERT_EQUAL_FLOAT(0.0f, erff(0.0f));
	TEST_ASSERT_EQUAL_FLOAT(1.0f, erfcf(0.0f));

	TEST_ASSERT_EQUAL_FLOAT(1.0f, erff(INFINITY));
	TEST_ASSERT_EQUAL_FLOAT(-1.0f, erff(-INFINITY));
	TEST_ASSERT_EQUAL_FLOAT(0.0f, erfcf(INFINITY));
	TEST_ASSERT_EQUAL_FLOAT(2.0f, erfcf(-INFINITY));
}


TEST_GROUP_RUNNER(math_erf)
{
	RUN_TEST_CASE(math_erf, erf_basic);
	RUN_TEST_CASE(math_erf, erf_special_val);

	RUN_TEST_CASE(math_erf, erfc_basic);
	RUN_TEST_CASE(math_erf, erfc_special_val);

	RUN_TEST_CASE(math_erf, erff_erfcf_basic);
	RUN_TEST_CASE(math_erf, erff_erfcf_special_val);
}
