/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - pthread.h
 *
 * TESTED:
 *    - the clock of a condition variable: pthread_cond_timedwait(),
 *      pthread_cond_clockwait(), pthread_condattr_setclock()/getclock()
 *
 * POSIX: a condition variable initialised without attributes, or with
 * attributes whose clock was not set, measures pthread_cond_timedwait()'s
 * absolute deadline on CLOCK_REALTIME. Portable code therefore passes a
 * CLOCK_REALTIME deadline -- about 1.7e9 s -- to such a condition variable. A
 * library whose default is CLOCK_MONOTONIC reads that as a deadline some 56
 * years after the boot, and the wait lasts until it is signalled. Each timed
 * wait here runs in a thread watched by the test: one that is still waiting
 * long after its deadline is released and reported, so a broken library fails
 * the test instead of hanging the suite.
 *
 * The wall clock can be stepped (clock_settime()), on a board without an RTC by
 * decades when the time is first set during boot. The group
 * pthread_cond_clockstep checks that a step neither makes a wait longer than
 * the time to its deadline when it began, nor stops a forward step from ending
 * the wait at its next wake-up. It steps the system clock by
 * CONDCLOCK_STEP_S seconds and back, which other programs see, so it runs only
 * when the environment variable PH_TEST_CLOCKSTEP is set.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <unity_fixture.h>


#define CONDCLOCK_WAIT_MS     300  /* the timeout of the timed waits */
#define CONDCLOCK_SLACK_MS    1200 /* how late a timeout may still come */
#define CONDCLOCK_EARLY_MS    20   /* how early, for the clock's granularity */
#define CONDCLOCK_WATCHDOG_MS 3000 /* a wait still running then has hung */
#define CONDCLOCK_STEP_S      10   /* the wall-clock step */


typedef struct {
	pthread_cond_t *cond;
	pthread_mutex_t lock;
	int clockwait;        /* use pthread_cond_clockwait(clock), else timedwait() */
	clockid_t clock;      /* of the deadline, for pthread_cond_clockwait() */
	struct timespec deadline;
	int released;         /* the predicate: set by a signaller or the watchdog */
	int rc;               /* of the last wait */
	int64_t elapsedMs;    /* how long the waits took */
	int held;             /* the mutex was held on return */
	volatile int done;
} condclock_waiter_t;


static struct {
	pthread_cond_t cond;
	pthread_cond_t staticCond;
	pthread_condattr_t attr;
	int stepped;          /* the wall clock is off by this (s), for the tear-down */
} condclock_common = {
	.staticCond = PTHREAD_COND_INITIALIZER,
};


static int64_t condclock_ms(clockid_t clock)
{
	struct timespec ts;

	(void)clock_gettime(clock, &ts);
	return ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}


/* `ms` milliseconds from now on `clock` (negative: in the past) */
static struct timespec condclock_deadline(clockid_t clock, int64_t ms)
{
	struct timespec ts;
	int64_t ns;

	(void)clock_gettime(clock, &ts);
	ns = (int64_t)ts.tv_nsec + ((ms % 1000) * 1000000);
	ts.tv_sec += (time_t)(ms / 1000);
	if (ns >= 1000000000) {
		ns -= 1000000000;
		ts.tv_sec++;
	}
	else if (ns < 0) {
		ns += 1000000000;
		ts.tv_sec--;
	}
	ts.tv_nsec = (long)ns;
	return ts;
}


/* Steps the wall clock by `s` seconds; returns 0 on success */
static int condclock_step(int s)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
		return -1;
	}
	ts.tv_sec += s;
	if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
		return -1;
	}
	condclock_common.stepped += s;
	return 0;
}


static void *condclock_waitThread(void *arg)
{
	condclock_waiter_t *w = arg;
	int64_t start;

	(void)pthread_mutex_lock(&w->lock);
	start = condclock_ms(CLOCK_MONOTONIC);
	do {
		if (w->clockwait != 0) {
			w->rc = pthread_cond_clockwait(w->cond, &w->lock, w->clock, &w->deadline);
		}
		else {
			w->rc = pthread_cond_timedwait(w->cond, &w->lock, &w->deadline);
		}
	} while ((w->rc == 0) && (w->released == 0)); /* a spurious wake-up: wait on */
	w->elapsedMs = condclock_ms(CLOCK_MONOTONIC) - start;
	w->held = (pthread_mutex_trylock(&w->lock) == EBUSY) ? 1 : 0;
	(void)pthread_mutex_unlock(&w->lock);

	__atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
	return NULL;
}


static void condclock_prepare(condclock_waiter_t *w, pthread_cond_t *cond, int clockwait, clockid_t clock, int64_t ms)
{
	w->cond = cond;
	w->clockwait = clockwait;
	w->clock = clock;
	w->deadline = condclock_deadline(clock, ms);
	w->released = 0;
	w->rc = -1;
	w->elapsedMs = -1;
	w->held = 0;
	w->done = 0;
}


static void condclock_run(condclock_waiter_t *w, pthread_t *th)
{
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&w->lock, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(th, NULL, condclock_waitThread, w));
}


/* A thread waiting on `cond` until `ms` from now on `clock` */
static void condclock_start(condclock_waiter_t *w, pthread_t *th, pthread_cond_t *cond, int clockwait, clockid_t clock, int64_t ms)
{
	condclock_prepare(w, cond, clockwait, clock, ms);
	condclock_run(w, th);
}


/* Ends the wait as a signaller would: sets the predicate and wakes the waiter */
static void condclock_release(condclock_waiter_t *w)
{
	(void)pthread_mutex_lock(&w->lock);
	w->released = 1;
	(void)pthread_cond_broadcast(w->cond);
	(void)pthread_mutex_unlock(&w->lock);
}


/* Joins the waiter, releasing it first if it is still waiting after the
 * watchdog's time; returns 0 if it was released that way (it hung) */
static int condclock_join(condclock_waiter_t *w, pthread_t th)
{
	int64_t end = condclock_ms(CLOCK_MONOTONIC) + CONDCLOCK_WATCHDOG_MS;
	int finished;

	while (((finished = __atomic_load_n(&w->done, __ATOMIC_ACQUIRE)) == 0) && (condclock_ms(CLOCK_MONOTONIC) < end)) {
		usleep(10 * 1000);
	}
	if (finished == 0) {
		condclock_release(w);
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_join(th, NULL));
	(void)pthread_mutex_destroy(&w->lock);
	return finished;
}


/* A wait that should time out after about `ms` */
static void condclock_expectTimeout(condclock_waiter_t *w, pthread_t th, int64_t ms)
{
	if (condclock_join(w, th) == 0) {
		TEST_FAIL_MESSAGE("the timed wait was still waiting long after its deadline");
	}
	TEST_ASSERT_EQUAL_INT(ETIMEDOUT, w->rc);
	TEST_ASSERT_GREATER_OR_EQUAL_INT64(ms - CONDCLOCK_EARLY_MS, w->elapsedMs);
	TEST_ASSERT_LESS_OR_EQUAL_INT64(ms + CONDCLOCK_SLACK_MS, w->elapsedMs);
}


static void condclock_timeout(pthread_cond_t *cond, int clockwait, clockid_t clock)
{
	condclock_waiter_t w;
	pthread_t th;

	condclock_start(&w, &th, cond, clockwait, clock, CONDCLOCK_WAIT_MS);
	condclock_expectTimeout(&w, th, CONDCLOCK_WAIT_MS);
}


static void condclock_initClock(clockid_t clock)
{
	TEST_ASSERT_EQUAL_INT(0, pthread_condattr_init(&condclock_common.attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_condattr_setclock(&condclock_common.attr, clock));
	TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&condclock_common.cond, &condclock_common.attr));
	(void)pthread_condattr_destroy(&condclock_common.attr);
}


TEST_GROUP(pthread_cond_clock);


TEST_SETUP(pthread_cond_clock)
{
}


TEST_TEAR_DOWN(pthread_cond_clock)
{
	(void)pthread_cond_destroy(&condclock_common.cond);
}


TEST(pthread_cond_clock, condattr_default_clock_is_realtime)
{
	pthread_condattr_t attr;
	clockid_t clock = (clockid_t)-1;

	TEST_ASSERT_EQUAL_INT(0, pthread_condattr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_condattr_getclock(&attr, &clock));
	TEST_ASSERT_EQUAL_INT(CLOCK_REALTIME, clock);
	TEST_ASSERT_EQUAL_INT(0, pthread_condattr_destroy(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&condclock_common.cond, NULL));
}


/* The case of CPython's parking lot, SDL, libstdc++, Mesa's C11 threads... */
TEST(pthread_cond_clock, default_cond_realtime_deadline_times_out)
{
	TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&condclock_common.cond, NULL));
	condclock_timeout(&condclock_common.cond, 0, CLOCK_REALTIME);
}


TEST(pthread_cond_clock, static_cond_realtime_deadline_times_out)
{
	TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&condclock_common.cond, NULL)); /* for the tear-down */
	condclock_timeout(&condclock_common.staticCond, 0, CLOCK_REALTIME);
}


TEST(pthread_cond_clock, attr_without_clock_realtime_deadline_times_out)
{
	TEST_ASSERT_EQUAL_INT(0, pthread_condattr_init(&condclock_common.attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&condclock_common.cond, &condclock_common.attr));
	(void)pthread_condattr_destroy(&condclock_common.attr);
	condclock_timeout(&condclock_common.cond, 0, CLOCK_REALTIME);
}


TEST(pthread_cond_clock, realtime_cond_realtime_deadline_times_out)
{
	condclock_initClock(CLOCK_REALTIME);
	condclock_timeout(&condclock_common.cond, 0, CLOCK_REALTIME);
}


TEST(pthread_cond_clock, monotonic_cond_monotonic_deadline_times_out)
{
	condclock_initClock(CLOCK_MONOTONIC);
	condclock_timeout(&condclock_common.cond, 0, CLOCK_MONOTONIC);
}


TEST(pthread_cond_clock, clockwait_monotonic_on_default_cond)
{
	TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&condclock_common.cond, NULL));
	condclock_timeout(&condclock_common.cond, 1, CLOCK_MONOTONIC);
}


TEST(pthread_cond_clock, clockwait_realtime_on_monotonic_cond)
{
	condclock_initClock(CLOCK_MONOTONIC);
	condclock_timeout(&condclock_common.cond, 1, CLOCK_REALTIME);
}


TEST(pthread_cond_clock, clockwait_invalid_clock_einval)
{
	pthread_mutex_t lock;
	struct timespec ts = condclock_deadline(CLOCK_REALTIME, CONDCLOCK_WAIT_MS);

	TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&condclock_common.cond, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&lock, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&lock));
	TEST_ASSERT_EQUAL_INT(EINVAL, pthread_cond_clockwait(&condclock_common.cond, &lock, CLOCK_THREAD_CPUTIME_ID, &ts));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&lock));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&lock));
}


/* A deadline that has passed times out at once, the mutex held */
static void condclock_past(pthread_cond_t *cond, int clockwait, clockid_t clock, const struct timespec *deadline)
{
	condclock_waiter_t w;
	pthread_t th;

	condclock_prepare(&w, cond, clockwait, clock, 0);
	w.deadline = *deadline;
	condclock_run(&w, &th);
	if (condclock_join(&w, th) == 0) {
		TEST_FAIL_MESSAGE("a timed wait with a deadline in the past was still waiting");
	}
	TEST_ASSERT_EQUAL_INT(ETIMEDOUT, w.rc);
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, w.held, "the mutex was not held on return");
	TEST_ASSERT_LESS_THAN_INT64(100, w.elapsedMs);
}


TEST(pthread_cond_clock, past_deadline_times_out_at_once)
{
	struct timespec ts;

	TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&condclock_common.cond, NULL));

	ts = condclock_deadline(CLOCK_REALTIME, -1000);
	condclock_past(&condclock_common.cond, 0, CLOCK_REALTIME, &ts);

	/* 1970: what a deadline computed before the wall clock was set looks like */
	ts.tv_sec = 1;
	ts.tv_nsec = 0;
	condclock_past(&condclock_common.cond, 0, CLOCK_REALTIME, &ts);

	ts = condclock_deadline(CLOCK_MONOTONIC, -1000);
	condclock_past(&condclock_common.cond, 1, CLOCK_MONOTONIC, &ts);

	ts = condclock_deadline(CLOCK_REALTIME, -1000);
	condclock_past(&condclock_common.cond, 1, CLOCK_REALTIME, &ts);
}


TEST(pthread_cond_clock, signal_before_deadline_wakes)
{
	condclock_waiter_t w;
	pthread_t th;

	TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&condclock_common.cond, NULL));
	condclock_start(&w, &th, &condclock_common.cond, 0, CLOCK_REALTIME, 10 * 1000);
	usleep(100 * 1000);
	condclock_release(&w);
	TEST_ASSERT_EQUAL_INT(1, condclock_join(&w, th));
	TEST_ASSERT_EQUAL_INT(0, w.rc);
	TEST_ASSERT_LESS_THAN_INT64(2000, w.elapsedMs);
}


/* A deadline too far away to count in microseconds, as C++'s time_point::max()
 * gives, must block -- an overflow into the past would time out at once and
 * make the caller spin */
TEST(pthread_cond_clock, far_future_deadline_blocks)
{
	condclock_waiter_t w;
	pthread_t th;
	int clockwait, early;

	TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&condclock_common.cond, NULL));
	for (clockwait = 0; clockwait <= 1; clockwait++) {
		condclock_prepare(&w, &condclock_common.cond, clockwait, (clockwait != 0) ? CLOCK_MONOTONIC : CLOCK_REALTIME, 0);
		w.deadline.tv_sec = (time_t)LLONG_MAX; /* time_t is 64-bit on Phoenix */
		w.deadline.tv_nsec = 0;
		condclock_run(&w, &th);
		usleep(200 * 1000);
		early = __atomic_load_n(&w.done, __ATOMIC_ACQUIRE);
		condclock_release(&w);
		TEST_ASSERT_EQUAL_INT(1, condclock_join(&w, th));
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, early, "a far-future deadline timed out");
		TEST_ASSERT_EQUAL_INT(0, w.rc);
	}
}


TEST_GROUP_RUNNER(pthread_cond_clock)
{
	RUN_TEST_CASE(pthread_cond_clock, condattr_default_clock_is_realtime);
	RUN_TEST_CASE(pthread_cond_clock, default_cond_realtime_deadline_times_out);
	RUN_TEST_CASE(pthread_cond_clock, static_cond_realtime_deadline_times_out);
	RUN_TEST_CASE(pthread_cond_clock, attr_without_clock_realtime_deadline_times_out);
	RUN_TEST_CASE(pthread_cond_clock, realtime_cond_realtime_deadline_times_out);
	RUN_TEST_CASE(pthread_cond_clock, monotonic_cond_monotonic_deadline_times_out);
	RUN_TEST_CASE(pthread_cond_clock, clockwait_monotonic_on_default_cond);
	RUN_TEST_CASE(pthread_cond_clock, clockwait_realtime_on_monotonic_cond);
	RUN_TEST_CASE(pthread_cond_clock, clockwait_invalid_clock_einval);
	RUN_TEST_CASE(pthread_cond_clock, past_deadline_times_out_at_once);
	RUN_TEST_CASE(pthread_cond_clock, signal_before_deadline_wakes);
	RUN_TEST_CASE(pthread_cond_clock, far_future_deadline_blocks);
}


TEST_GROUP(pthread_cond_clockstep);


TEST_SETUP(pthread_cond_clockstep)
{
	condclock_common.stepped = 0;
	if (getenv("PH_TEST_CLOCKSTEP") == NULL) {
		TEST_IGNORE_MESSAGE("steps the system clock: set PH_TEST_CLOCKSTEP to run");
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&condclock_common.cond, NULL));
}


TEST_TEAR_DOWN(pthread_cond_clockstep)
{
	if (condclock_common.stepped != 0) {
		(void)condclock_step(-condclock_common.stepped);
	}
	(void)pthread_cond_destroy(&condclock_common.cond);
}


/* The wall clock stepping back during a wait does not make the wait longer
 * than the time to its deadline when it began */
TEST(pthread_cond_clockstep, backward_step_does_not_extend_wait)
{
	condclock_waiter_t w;
	pthread_t th;

	condclock_start(&w, &th, &condclock_common.cond, 0, CLOCK_REALTIME, CONDCLOCK_WAIT_MS);
	usleep(100 * 1000);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, condclock_step(-CONDCLOCK_STEP_S), "clock_settime() failed");
	condclock_expectTimeout(&w, th, CONDCLOCK_WAIT_MS);
}


/* Stepping forward past the deadline, with nothing to wake the waiter: the
 * wait still ends no later than its deadline as it was when it began */
TEST(pthread_cond_clockstep, forward_step_does_not_extend_wait)
{
	condclock_waiter_t w;
	pthread_t th;

	condclock_start(&w, &th, &condclock_common.cond, 0, CLOCK_REALTIME, CONDCLOCK_WAIT_MS);
	usleep(100 * 1000);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, condclock_step(CONDCLOCK_STEP_S), "clock_settime() failed");
	if (condclock_join(&w, th) == 0) {
		TEST_FAIL_MESSAGE("the timed wait was still waiting long after its deadline");
	}
	TEST_ASSERT_EQUAL_INT(ETIMEDOUT, w.rc);
	TEST_ASSERT_LESS_OR_EQUAL_INT64(CONDCLOCK_WAIT_MS + CONDCLOCK_SLACK_MS, w.elapsedMs);
}


/* Stepping forward past the deadline, then a wake-up that does not satisfy
 * the predicate: the waiter's next wait times out at once */
TEST(pthread_cond_clockstep, forward_step_past_deadline_ends_wait_at_wakeup)
{
	condclock_waiter_t w;
	pthread_t th;

	condclock_start(&w, &th, &condclock_common.cond, 0, CLOCK_REALTIME, 5 * 1000);
	usleep(100 * 1000);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, condclock_step(CONDCLOCK_STEP_S), "clock_settime() failed");
	(void)pthread_mutex_lock(&w.lock);
	(void)pthread_cond_broadcast(w.cond);
	(void)pthread_mutex_unlock(&w.lock);
	if (condclock_join(&w, th) == 0) {
		TEST_FAIL_MESSAGE("the timed wait was still waiting long after its deadline");
	}
	TEST_ASSERT_EQUAL_INT(ETIMEDOUT, w.rc);
	TEST_ASSERT_LESS_THAN_INT64(2000, w.elapsedMs);
}


TEST_GROUP_RUNNER(pthread_cond_clockstep)
{
	RUN_TEST_CASE(pthread_cond_clockstep, backward_step_does_not_extend_wait);
	RUN_TEST_CASE(pthread_cond_clockstep, forward_step_does_not_extend_wait);
	RUN_TEST_CASE(pthread_cond_clockstep, forward_step_past_deadline_ends_wait_at_wakeup);
}
