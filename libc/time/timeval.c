/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - sys/time.h
 *
 * TESTED (libphoenix):
 *    - timercmp() macro — the `->` fix: its arguments are `struct timeval *`,
 *      and the previous `.` form silently broke every standard caller (e.g.
 *      readline). Only the strict < / > forms are reliable for the classic
 *      timercmp macro (the trailing `|| a->tv_sec CMP b->tv_sec` term makes
 *      ==/!= give false positives), so only those are exercised here.
 *    - timerisset()
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <sys/time.h>

#include <unity_fixture.h>


TEST_GROUP(time_timeval);


TEST_SETUP(time_timeval)
{
}


TEST_TEAR_DOWN(time_timeval)
{
}


TEST(time_timeval, timercmp_lt_gt)
{
	struct timeval a = { .tv_sec = 5, .tv_usec = 100 };
	struct timeval b = { .tv_sec = 5, .tv_usec = 200 };
	struct timeval c = { .tv_sec = 6, .tv_usec = 0 };
	struct timeval eq = { .tv_sec = 5, .tv_usec = 100 };

	/* same tv_sec, tv_usec differs */
	TEST_ASSERT_TRUE(timercmp(&a, &b, <));
	TEST_ASSERT_FALSE(timercmp(&a, &b, >));
	TEST_ASSERT_TRUE(timercmp(&b, &a, >));
	TEST_ASSERT_FALSE(timercmp(&b, &a, <));

	/* tv_sec differs (dominates) */
	TEST_ASSERT_TRUE(timercmp(&a, &c, <));
	TEST_ASSERT_TRUE(timercmp(&c, &a, >));
	TEST_ASSERT_FALSE(timercmp(&c, &a, <));
	TEST_ASSERT_FALSE(timercmp(&a, &c, >));

	/* equal values: neither strictly less nor greater */
	TEST_ASSERT_FALSE(timercmp(&a, &eq, <));
	TEST_ASSERT_FALSE(timercmp(&a, &eq, >));
}


TEST(time_timeval, timerisset_basic)
{
	struct timeval zero = { .tv_sec = 0, .tv_usec = 0 };
	struct timeval sec = { .tv_sec = 1, .tv_usec = 0 };
	struct timeval usec = { .tv_sec = 0, .tv_usec = 1 };

	TEST_ASSERT_FALSE(timerisset(&zero));
	TEST_ASSERT_TRUE(timerisset(&sec));
	TEST_ASSERT_TRUE(timerisset(&usec));
}


TEST(time_timeval, timerclear_basic)
{
	struct timeval t = { .tv_sec = 42, .tv_usec = 999 };

	timerclear(&t);
	TEST_ASSERT_EQUAL_INT(0, t.tv_sec);
	TEST_ASSERT_EQUAL_INT(0, t.tv_usec);
	TEST_ASSERT_FALSE(timerisset(&t));
}


TEST(time_timeval, timeradd_carry)
{
	struct timeval r;
	struct timeval c = { .tv_sec = 1, .tv_usec = 100000 };
	struct timeval d = { .tv_sec = 2, .tv_usec = 200000 };
	struct timeval a = { .tv_sec = 1, .tv_usec = 800000 };
	struct timeval b = { .tv_sec = 2, .tv_usec = 300000 };

	/* no carry */
	timeradd(&c, &d, &r);
	TEST_ASSERT_EQUAL_INT(3, r.tv_sec);
	TEST_ASSERT_EQUAL_INT(300000, r.tv_usec);

	/* tv_usec overflow carries into tv_sec (1.1e6 -> +1 sec, 0.1e6) */
	timeradd(&a, &b, &r);
	TEST_ASSERT_EQUAL_INT(4, r.tv_sec);
	TEST_ASSERT_EQUAL_INT(100000, r.tv_usec);
}


TEST(time_timeval, timersub_borrow)
{
	struct timeval r;
	struct timeval a = { .tv_sec = 5, .tv_usec = 500000 };
	struct timeval b = { .tv_sec = 2, .tv_usec = 200000 };
	struct timeval c = { .tv_sec = 5, .tv_usec = 100000 };
	struct timeval d = { .tv_sec = 2, .tv_usec = 300000 };

	/* no borrow */
	timersub(&a, &b, &r);
	TEST_ASSERT_EQUAL_INT(3, r.tv_sec);
	TEST_ASSERT_EQUAL_INT(300000, r.tv_usec);

	/* tv_usec underflow borrows from tv_sec (-0.2e6 -> -1 sec, 0.8e6) */
	timersub(&c, &d, &r);
	TEST_ASSERT_EQUAL_INT(2, r.tv_sec);
	TEST_ASSERT_EQUAL_INT(800000, r.tv_usec);
}


TEST_GROUP_RUNNER(time_timeval)
{
	RUN_TEST_CASE(time_timeval, timercmp_lt_gt);
	RUN_TEST_CASE(time_timeval, timerisset_basic);
	RUN_TEST_CASE(time_timeval, timerclear_basic);
	RUN_TEST_CASE(time_timeval, timeradd_carry);
	RUN_TEST_CASE(time_timeval, timersub_borrow);
}
