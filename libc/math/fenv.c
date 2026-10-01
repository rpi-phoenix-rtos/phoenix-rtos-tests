/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - fenv.h
 *
 * TESTED:
 *    - fegetround(), fesetround()
 *    - feclearexcept(), feraiseexcept(), fetestexcept()
 *    - fegetexceptflag(), fesetexceptflag()
 *    - fegetenv(), fesetenv(), feholdexcept(), feupdateenv(), FE_DFL_ENV
 *    - pthread_create(): a new thread starts with its creator's environment
 *
 * libphoenix shipped libmcs' placeholder <fenv.h>, which only #errors, so
 * nothing using the floating-point environment built (JavaScriptCore's number
 * conversions, SIMD emulation layers). Each rounding direction is checked on
 * real arithmetic, not only read back; operands and results are volatile so
 * the compiler can neither fold an operation nor move it to another rounding
 * direction.
 *
 * On an architecture without environment control <fenv.h> defines only
 * FE_TONEAREST and FE_ALL_EXCEPT == 0; the cases that need more are skipped.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <fenv.h>
#include <float.h>
#include <pthread.h>

#include <unity_fixture.h>



static volatile double fenv_one = 1.0;
static volatile double fenv_three = 3.0;
static volatile double fenv_zero = 0.0;


/* 1/3 and -1/3 under the current rounding direction. The result is stored
 * to a volatile: without it GCC moves the division past the next fesetround()
 * call, as it does not model the rounding mode (even with -frounding-math). */
static double fenv_third(void)
{
	volatile double r = fenv_one / fenv_three;

	return r;
}


static double fenv_minusThird(void)
{
	volatile double r = -fenv_one / fenv_three;

	return r;
}


TEST_GROUP(math_fenv);


TEST_SETUP(math_fenv)
{
	(void)fesetenv(FE_DFL_ENV);
}


TEST_TEAR_DOWN(math_fenv)
{
	(void)fesetenv(FE_DFL_ENV);
}


TEST(math_fenv, default_round_to_nearest)
{
	TEST_ASSERT_EQUAL_INT(FE_TONEAREST, fegetround());
	TEST_ASSERT_EQUAL_INT(0, fesetround(FE_TONEAREST));
	TEST_ASSERT_EQUAL_INT(FE_TONEAREST, fegetround());
}


TEST(math_fenv, rounding_directions_round)
{
#if defined(FE_UPWARD) && defined(FE_DOWNWARD) && defined(FE_TOWARDZERO)
	double nearest, up, down, zero, mnearest, mup, mdown, mzero;

	nearest = fenv_third();
	mnearest = fenv_minusThird();

	TEST_ASSERT_EQUAL_INT(0, fesetround(FE_UPWARD));
	TEST_ASSERT_EQUAL_INT(FE_UPWARD, fegetround());
	up = fenv_third();
	mup = fenv_minusThird();

	TEST_ASSERT_EQUAL_INT(0, fesetround(FE_DOWNWARD));
	TEST_ASSERT_EQUAL_INT(FE_DOWNWARD, fegetround());
	down = fenv_third();
	mdown = fenv_minusThird();

	TEST_ASSERT_EQUAL_INT(0, fesetround(FE_TOWARDZERO));
	TEST_ASSERT_EQUAL_INT(FE_TOWARDZERO, fegetround());
	zero = fenv_third();
	mzero = fenv_minusThird();

	TEST_ASSERT_EQUAL_INT(0, fesetround(FE_TONEAREST));

	/* 1/3 is inexact: up and down differ by one ulp, nearest is one of them */
	TEST_ASSERT_TRUE(up > down);
	TEST_ASSERT_TRUE((nearest == up) || (nearest == down));
	TEST_ASSERT_TRUE(mup > mdown);
	/* towards zero is down for a positive result, up for a negative one */
	TEST_ASSERT_TRUE(zero == down);
	TEST_ASSERT_TRUE(mzero == mup);
	TEST_ASSERT_TRUE((mnearest == mup) || (mnearest == mdown));
#else
	TEST_IGNORE_MESSAGE("only round-to-nearest on this architecture");
#endif
}


TEST(math_fenv, invalid_direction_is_refused)
{
	TEST_ASSERT_NOT_EQUAL(0, fesetround(0x12345));
	TEST_ASSERT_EQUAL_INT(FE_TONEAREST, fegetround());
}


TEST(math_fenv, arithmetic_raises_flags)
{
#if (FE_ALL_EXCEPT != 0) && defined(FE_DIVBYZERO) && defined(FE_INVALID) && defined(FE_OVERFLOW) && defined(FE_UNDERFLOW) && defined(FE_INEXACT)
	volatile double r, big = DBL_MAX, tiny = DBL_MIN;

	TEST_ASSERT_EQUAL_INT(0, feclearexcept(FE_ALL_EXCEPT));
	TEST_ASSERT_EQUAL_INT(0, fetestexcept(FE_ALL_EXCEPT));

	r = fenv_one / fenv_zero;
	TEST_ASSERT_EQUAL_INT(FE_DIVBYZERO, fetestexcept(FE_ALL_EXCEPT));

	TEST_ASSERT_EQUAL_INT(0, feclearexcept(FE_ALL_EXCEPT));
	r = fenv_zero / fenv_zero;
	TEST_ASSERT_EQUAL_INT(FE_INVALID, fetestexcept(FE_ALL_EXCEPT));

	TEST_ASSERT_EQUAL_INT(0, feclearexcept(FE_ALL_EXCEPT));
	r = big * 2.0;
	TEST_ASSERT_EQUAL_INT(FE_OVERFLOW | FE_INEXACT, fetestexcept(FE_ALL_EXCEPT));

	TEST_ASSERT_EQUAL_INT(0, feclearexcept(FE_ALL_EXCEPT));
	r = tiny / 3.0;
	TEST_ASSERT_EQUAL_INT(FE_UNDERFLOW | FE_INEXACT, fetestexcept(FE_ALL_EXCEPT));

	TEST_ASSERT_EQUAL_INT(0, feclearexcept(FE_ALL_EXCEPT));
	r = fenv_third();
	TEST_ASSERT_EQUAL_INT(FE_INEXACT, fetestexcept(FE_ALL_EXCEPT));

	/* Clearing one flag leaves the others */
	r = fenv_one / fenv_zero;
	TEST_ASSERT_EQUAL_INT(0, feclearexcept(FE_INEXACT));
	TEST_ASSERT_EQUAL_INT(FE_DIVBYZERO, fetestexcept(FE_ALL_EXCEPT));
	(void)r;
#else
	TEST_IGNORE_MESSAGE("no exception flags on this architecture");
#endif
}


TEST(math_fenv, raise_and_flag_round_trip)
{
#if (FE_ALL_EXCEPT != 0) && defined(FE_OVERFLOW) && defined(FE_INVALID)
	fexcept_t flags;

	TEST_ASSERT_EQUAL_INT(0, feclearexcept(FE_ALL_EXCEPT));
	TEST_ASSERT_EQUAL_INT(0, feraiseexcept(FE_OVERFLOW | FE_INVALID));
	TEST_ASSERT_EQUAL_INT(FE_OVERFLOW | FE_INVALID, fetestexcept(FE_ALL_EXCEPT));
	TEST_ASSERT_EQUAL_INT(FE_OVERFLOW, fetestexcept(FE_OVERFLOW));

	TEST_ASSERT_EQUAL_INT(0, fegetexceptflag(&flags, FE_ALL_EXCEPT));
	TEST_ASSERT_EQUAL_INT(0, feclearexcept(FE_ALL_EXCEPT));
	TEST_ASSERT_EQUAL_INT(0, fetestexcept(FE_ALL_EXCEPT));

	/* Only the flags named are restored */
	TEST_ASSERT_EQUAL_INT(0, fesetexceptflag(&flags, FE_OVERFLOW));
	TEST_ASSERT_EQUAL_INT(FE_OVERFLOW, fetestexcept(FE_ALL_EXCEPT));
#else
	TEST_IGNORE_MESSAGE("no exception flags on this architecture");
#endif
}


TEST(math_fenv, environment_save_restore)
{
#if (FE_ALL_EXCEPT != 0) && defined(FE_UPWARD) && defined(FE_INEXACT) && defined(FE_DIVBYZERO)
	fenv_t env;

	TEST_ASSERT_EQUAL_INT(0, feraiseexcept(FE_INEXACT));
	TEST_ASSERT_EQUAL_INT(0, fegetenv(&env));

	TEST_ASSERT_EQUAL_INT(0, fesetround(FE_UPWARD));
	TEST_ASSERT_EQUAL_INT(0, feclearexcept(FE_ALL_EXCEPT));
	TEST_ASSERT_EQUAL_INT(0, feraiseexcept(FE_DIVBYZERO));

	TEST_ASSERT_EQUAL_INT(0, fesetenv(&env));
	TEST_ASSERT_EQUAL_INT(FE_TONEAREST, fegetround());
	TEST_ASSERT_EQUAL_INT(FE_INEXACT, fetestexcept(FE_ALL_EXCEPT));

	/* The default environment: nearest, no flags */
	TEST_ASSERT_EQUAL_INT(0, fesetround(FE_UPWARD));
	TEST_ASSERT_EQUAL_INT(0, fesetenv(FE_DFL_ENV));
	TEST_ASSERT_EQUAL_INT(FE_TONEAREST, fegetround());
	TEST_ASSERT_EQUAL_INT(0, fetestexcept(FE_ALL_EXCEPT));
#else
	TEST_IGNORE_MESSAGE("no environment control on this architecture");
#endif
}


TEST(math_fenv, hold_and_update)
{
#if (FE_ALL_EXCEPT != 0) && defined(FE_UPWARD) && defined(FE_INEXACT) && defined(FE_OVERFLOW)
	fenv_t env;

	TEST_ASSERT_EQUAL_INT(0, fesetround(FE_UPWARD));
	TEST_ASSERT_EQUAL_INT(0, feraiseexcept(FE_INEXACT));

	/* hold: saves, then clears the flags */
	TEST_ASSERT_EQUAL_INT(0, feholdexcept(&env));
	TEST_ASSERT_EQUAL_INT(0, fetestexcept(FE_ALL_EXCEPT));
	TEST_ASSERT_EQUAL_INT(FE_UPWARD, fegetround());

	TEST_ASSERT_EQUAL_INT(0, fesetround(FE_TONEAREST));
	TEST_ASSERT_EQUAL_INT(0, feraiseexcept(FE_OVERFLOW));

	/* update: back to the saved environment, plus what was raised meanwhile */
	TEST_ASSERT_EQUAL_INT(0, feupdateenv(&env));
	TEST_ASSERT_EQUAL_INT(FE_UPWARD, fegetround());
	TEST_ASSERT_EQUAL_INT(FE_INEXACT | FE_OVERFLOW, fetestexcept(FE_ALL_EXCEPT));
#else
	TEST_IGNORE_MESSAGE("no environment control on this architecture");
#endif
}


#ifdef FE_UPWARD
static void *fenv_otherThread(void *arg)
{
	volatile int *round = arg;

	(void)fesetround(FE_DOWNWARD);
	*round = fegetround();

	return NULL;
}


static void *fenv_initialRound(void *arg)
{
	volatile int *round = arg;

	*round = fegetround();

	return NULL;
}
#endif


/* POSIX pthread_create(): the new thread inherits the creator's environment */
TEST(math_fenv, new_thread_inherits)
{
#ifdef FE_UPWARD
	pthread_t thread;
	volatile int round = -1;

	TEST_ASSERT_EQUAL_INT(0, fesetround(FE_UPWARD));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, fenv_initialRound, (void *)&round));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));

	TEST_ASSERT_EQUAL_INT(FE_UPWARD, round);
#else
	TEST_IGNORE_MESSAGE("only round-to-nearest on this architecture");
#endif
}


/* The environment belongs to the thread: another thread's change is not seen */
TEST(math_fenv, per_thread)
{
#ifdef FE_UPWARD
	pthread_t thread;
	volatile int round = -1;

	TEST_ASSERT_EQUAL_INT(0, fesetround(FE_UPWARD));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, fenv_otherThread, (void *)&round));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));

	TEST_ASSERT_EQUAL_INT(FE_DOWNWARD, round);
	TEST_ASSERT_EQUAL_INT(FE_UPWARD, fegetround());
#else
	TEST_IGNORE_MESSAGE("only round-to-nearest on this architecture");
#endif
}


TEST_GROUP_RUNNER(math_fenv)
{
	RUN_TEST_CASE(math_fenv, default_round_to_nearest);
	RUN_TEST_CASE(math_fenv, rounding_directions_round);
	RUN_TEST_CASE(math_fenv, invalid_direction_is_refused);
	RUN_TEST_CASE(math_fenv, arithmetic_raises_flags);
	RUN_TEST_CASE(math_fenv, raise_and_flag_round_trip);
	RUN_TEST_CASE(math_fenv, environment_save_restore);
	RUN_TEST_CASE(math_fenv, hold_and_update);
	RUN_TEST_CASE(math_fenv, per_thread);
	RUN_TEST_CASE(math_fenv, new_thread_inherits);
}
