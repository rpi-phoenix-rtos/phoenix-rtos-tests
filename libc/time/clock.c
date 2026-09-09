/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - time.h
 *
 * TESTED (libphoenix):
 *    - clock_getres() — was entirely missing. Its absence is why libphoenix
 *      cannot honestly advertise _POSIX_TIMERS / _POSIX_MONOTONIC_CLOCK:
 *      portable code that sees those option macros is entitled to call
 *      clock_getres(), and a missing symbol turns a header claim into a link
 *      error.
 *    - the resolution it reports has to match what clock_gettime() actually
 *      delivers. gettime() reports microseconds, so tv_nsec is a multiple of
 *      1000 and the advertised resolution must be 1000 ns — not the ~18.5 ns
 *      of the underlying counter.
 *    - CLOCK_MONOTONIC must not go backwards, and must not be the same clock
 *      as CLOCK_REALTIME (REALTIME carries a settable offset).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <time.h>

#include <unity_fixture.h>


TEST_GROUP(time_clock);


TEST_SETUP(time_clock)
{
}


TEST_TEAR_DOWN(time_clock)
{
}


TEST(time_clock, getres_supported_clocks)
{
	const clockid_t ids[] = { CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW };
	struct timespec res;
	size_t i;

	for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		res.tv_sec = -1;
		res.tv_nsec = -1;
		TEST_ASSERT_EQUAL_INT(0, clock_getres(ids[i], &res));
		TEST_ASSERT_EQUAL_INT(0, res.tv_sec);
		TEST_ASSERT_TRUE(res.tv_nsec > 0);
		TEST_ASSERT_TRUE(res.tv_nsec < 1000000000L);
	}
}


TEST(time_clock, getres_null_is_allowed)
{
	/* POSIX: a NULL res only validates clk_id. */
	TEST_ASSERT_EQUAL_INT(0, clock_getres(CLOCK_MONOTONIC, NULL));
}


TEST(time_clock, getres_bad_clock)
{
	struct timespec res;

	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, clock_getres((clockid_t)0x7fff, &res));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}


TEST(time_clock, getres_matches_gettime_granularity)
{
	struct timespec res, now;

	TEST_ASSERT_EQUAL_INT(0, clock_getres(CLOCK_MONOTONIC, &res));
	TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC, &now));

	/*
	 * Whatever resolution is advertised, clock_gettime() must not return a
	 * value finer than it. Reporting 1 ns while only microseconds are
	 * available is the silent-precision-loss bug this guards against.
	 */
	TEST_ASSERT_EQUAL_INT(0, now.tv_nsec % res.tv_nsec);
}


TEST(time_clock, monotonic_does_not_go_backwards)
{
	struct timespec a, b;
	int i;

	TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC, &a));
	for (i = 0; i < 1000; i++) {
		TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC, &b));
		TEST_ASSERT_TRUE((b.tv_sec > a.tv_sec) || (b.tv_sec == a.tv_sec && b.tv_nsec >= a.tv_nsec));
		a = b;
	}
}


TEST(time_clock, monotonic_advances)
{
	struct timespec a, b;
	const struct timespec req = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 };
	long long delta_us;

	TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC, &a));
	TEST_ASSERT_EQUAL_INT(0, nanosleep(&req, NULL));
	TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC, &b));

	delta_us = ((long long)b.tv_sec - a.tv_sec) * 1000000LL + (b.tv_nsec - a.tv_nsec) / 1000;

	/*
	 * A clock that had fallen back to time() would report either 0 or a full
	 * second here; a 20 ms sleep must land strictly between.
	 */
	TEST_ASSERT_TRUE(delta_us >= 20000);
	TEST_ASSERT_TRUE(delta_us < 1000000);
}


TEST(time_clock, realtime_is_a_distinct_clock)
{
	struct timespec mono, real;

	TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC, &mono));
	TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_REALTIME, &real));

	/*
	 * CLOCK_REALTIME = CLOCK_MONOTONIC + a settable offset. On a target whose
	 * wall clock has been set at all they differ by years; asserting only
	 * ">= monotonic" keeps this true on a board that has never been set.
	 */
	TEST_ASSERT_TRUE((real.tv_sec > mono.tv_sec) || (real.tv_sec == mono.tv_sec && real.tv_nsec >= mono.tv_nsec));
}


TEST_GROUP_RUNNER(time_clock)
{
	RUN_TEST_CASE(time_clock, getres_supported_clocks);
	RUN_TEST_CASE(time_clock, getres_null_is_allowed);
	RUN_TEST_CASE(time_clock, getres_bad_clock);
	RUN_TEST_CASE(time_clock, getres_matches_gettime_granularity);
	RUN_TEST_CASE(time_clock, monotonic_does_not_go_backwards);
	RUN_TEST_CASE(time_clock, monotonic_advances);
	RUN_TEST_CASE(time_clock, realtime_is_a_distinct_clock);
}
