/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - pthread.h
 *
 * TESTED:
 *    - pthread_setcanceltype()
 *    - pthread_setcancelstate() with an asynchronous-type thread
 *
 * libphoenix had pthread_setcancelstate() but no pthread_setcanceltype(), so
 * Mesa's VK_KHR_display hotplug thread (wsi_common_display.c), which switches
 * itself to PTHREAD_CANCEL_ASYNCHRONOUS so it can be cancelled while blocked,
 * did not build. libphoenix delivers a cancellation immediately whenever it is
 * enabled -- asynchronous behaviour -- so ASYNCHRONOUS is honoured for real;
 * the cases below cancel threads that never reach a cancellation point.
 * DEFERRED is recorded but not honoured (see the note in <pthread.h>), which is
 * why no case here depends on deferral.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <unity_fixture.h>


/* Upper bound on any wait in this group, so a failure cannot hang the suite. */
#define CANCELTYPE_TIMEOUT_MS 5000

static volatile int canceltype_ready;
static volatile int canceltype_requested;
static volatile int canceltype_survived;


static long canceltype_nowMs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}


static int canceltype_waitFor(volatile int *flag)
{
	long deadline = canceltype_nowMs() + CANCELTYPE_TIMEOUT_MS;

	while (*flag == 0) {
		if (canceltype_nowMs() > deadline) {
			return -1;
		}
		usleep(1000);
	}
	return 0;
}


/* Switches itself to asynchronous cancellation, then spins without calling
 * any function that could be a cancellation point. Only an asynchronous
 * cancel can stop it before the deadline. */
static void *canceltype_spinner(void *arg)
{
	volatile unsigned long spins = 0;
	long deadline;

	(void)arg;
	if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL) != 0) {
		return (void *)2;
	}
	deadline = canceltype_nowMs() + CANCELTYPE_TIMEOUT_MS;
	canceltype_ready = 1;

	for (;;) {
		spins++;
		if (((spins & 0xfffffUL) == 0UL) && (canceltype_nowMs() > deadline)) {
			break;
		}
	}
	canceltype_survived = 1;
	return (void *)1;
}


/* Holds a cancellation request pending with cancellation disabled, then
 * re-enables it: with the asynchronous type the request must be acted on
 * right there, inside pthread_setcancelstate(). */
static void *canceltype_pending(void *arg)
{
	(void)arg;
	if ((pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL) != 0) ||
			(pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL) != 0)) {
		return (void *)2;
	}
	canceltype_ready = 1;

	if (canceltype_waitFor(&canceltype_requested) != 0) {
		return (void *)3;
	}
	(void)pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

	canceltype_survived = 1;
	return (void *)1;
}


TEST_GROUP(pthread_canceltype);


TEST_SETUP(pthread_canceltype)
{
	canceltype_ready = 0;
	canceltype_requested = 0;
	canceltype_survived = 0;
}


TEST_TEAR_DOWN(pthread_canceltype)
{
}


TEST(pthread_canceltype, get_set_and_default)
{
	int old = -1;

	/* the default is DEFERRED (POSIX) */
	TEST_ASSERT_EQUAL_INT(0, pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &old));
	TEST_ASSERT_EQUAL_INT(PTHREAD_CANCEL_DEFERRED, old);

	TEST_ASSERT_EQUAL_INT(0, pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &old));
	TEST_ASSERT_EQUAL_INT(PTHREAD_CANCEL_ASYNCHRONOUS, old);

	/* oldtype may be NULL */
	TEST_ASSERT_EQUAL_INT(0, pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL));

	TEST_ASSERT_NOT_EQUAL_INT(PTHREAD_CANCEL_DEFERRED, PTHREAD_CANCEL_ASYNCHRONOUS);
}


TEST(pthread_canceltype, invalid_type)
{
	int old = 12345;

	TEST_ASSERT_EQUAL_INT(EINVAL, pthread_setcanceltype(-1, &old));
	TEST_ASSERT_EQUAL_INT(EINVAL, pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED + PTHREAD_CANCEL_ASYNCHRONOUS + 7, &old));
	TEST_ASSERT_EQUAL_INT(12345, old);

	/* the type is unchanged by a rejected call */
	TEST_ASSERT_EQUAL_INT(0, pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &old));
	TEST_ASSERT_EQUAL_INT(PTHREAD_CANCEL_DEFERRED, old);
}


TEST(pthread_canceltype, type_is_per_thread)
{
	pthread_t t;
	void *ret = NULL;
	int old = -1;

	/* a thread that switches itself to async must not change ours */
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, canceltype_spinner, NULL));
	TEST_ASSERT_EQUAL_INT(0, canceltype_waitFor(&canceltype_ready));

	TEST_ASSERT_EQUAL_INT(0, pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &old));
	TEST_ASSERT_EQUAL_INT(PTHREAD_CANCEL_DEFERRED, old);

	TEST_ASSERT_EQUAL_INT(0, pthread_cancel(t));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(t, &ret));
	TEST_ASSERT_TRUE(ret == (void *)PTHREAD_CANCELED);
}


TEST(pthread_canceltype, async_cancels_a_thread_outside_cancellation_points)
{
	pthread_t t;
	void *ret = NULL;

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, canceltype_spinner, NULL));
	TEST_ASSERT_EQUAL_INT(0, canceltype_waitFor(&canceltype_ready));

	TEST_ASSERT_EQUAL_INT(0, pthread_cancel(t));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(t, &ret));

	TEST_ASSERT_TRUE_MESSAGE(ret == (void *)PTHREAD_CANCELED, "spinning thread was not cancelled");
	TEST_ASSERT_EQUAL_INT(0, canceltype_survived);
}


TEST(pthread_canceltype, async_pending_request_fires_on_enable)
{
	pthread_t t;
	void *ret = NULL;

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, canceltype_pending, NULL));
	TEST_ASSERT_EQUAL_INT(0, canceltype_waitFor(&canceltype_ready));

	/* cancellation is disabled in the target: the request stays pending */
	TEST_ASSERT_EQUAL_INT(0, pthread_cancel(t));
	canceltype_requested = 1;

	TEST_ASSERT_EQUAL_INT(0, pthread_join(t, &ret));
	TEST_ASSERT_TRUE_MESSAGE(ret == (void *)PTHREAD_CANCELED, "pending request not acted on at re-enable");
	TEST_ASSERT_EQUAL_INT(0, canceltype_survived);
}


TEST_GROUP_RUNNER(pthread_canceltype)
{
	RUN_TEST_CASE(pthread_canceltype, get_set_and_default);
	RUN_TEST_CASE(pthread_canceltype, invalid_type);
	RUN_TEST_CASE(pthread_canceltype, type_is_per_thread);
	RUN_TEST_CASE(pthread_canceltype, async_cancels_a_thread_outside_cancellation_points);
	RUN_TEST_CASE(pthread_canceltype, async_pending_request_fires_on_enable);
}
