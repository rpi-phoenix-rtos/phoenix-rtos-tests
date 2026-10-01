/*
 * Phoenix-RTOS
 *
 * C11 standard library tests
 *
 * HEADER:
 *    - stdint.h
 *
 * TESTED:
 *    - UINT8_MAX, UINT16_MAX, INT8_MAX, INT16_MAX (C11 7.20.2p1: the type of each
 *      limit macro is the type of the corresponding integer type after the integer
 *      promotions, i.e. int for every 8- and 16-bit type)
 *
 * UINT8_MAX was (0xffU) and UINT16_MAX (0xffffU): unsigned int. A signed operand
 * compared with them was converted to unsigned, so `int16_t t = -5; t > UINT8_MAX`
 * was TRUE. GCC 16 flagged 397 such comparisons in WebKit (SIMDe saturation,
 * JSC PropertyTable). The type checks are made by the compiler: on the old header
 * this file does not build.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdint.h>

#include <unity_fixture.h>


#define IS_INT(x) _Generic((x), int: 1, default: 0)

_Static_assert(IS_INT(UINT8_MAX), "UINT8_MAX must have type int");
_Static_assert(IS_INT(UINT16_MAX), "UINT16_MAX must have type int");
_Static_assert(IS_INT(INT8_MAX), "INT8_MAX must have type int");
_Static_assert(IS_INT(INT16_MAX), "INT16_MAX must have type int");
_Static_assert(IS_INT(UINT_LEAST8_MAX) && IS_INT(UINT_LEAST16_MAX), "UINT_LEASTn_MAX (n <= 16) must have type int");
_Static_assert(UINT8_MAX == 255 && UINT16_MAX == 65535, "values");


TEST_GROUP(stdint_limits);


TEST_SETUP(stdint_limits)
{
}


TEST_TEAR_DOWN(stdint_limits)
{
}


TEST(stdint_limits, signed_compare_stays_signed)
{
	volatile int16_t t = -5;
	volatile int8_t c = -1;

	TEST_ASSERT_FALSE(t > UINT8_MAX);
	TEST_ASSERT_FALSE(c > UINT16_MAX);
	TEST_ASSERT_TRUE(t < UINT8_MAX);
}


TEST_GROUP_RUNNER(stdint_limits)
{
	RUN_TEST_CASE(stdint_limits, signed_compare_stays_signed);
}
