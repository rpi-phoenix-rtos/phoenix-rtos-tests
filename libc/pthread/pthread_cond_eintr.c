/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - pthread.h
 *
 * TESTED:
 *    - pthread_cond_wait(), pthread_cond_timedwait() interrupted by a signal
 *
 * A signal handler runs inside the wait. POSIX: the function then either
 * resumes waiting or returns zero as a spurious wakeup, never EINTR, and the
 * mutex is held again either way. Resuming is only safe if nothing can have
 * changed meanwhile; but while the handler runs no thread is waiting, so a
 * pthread_cond_signal() from another thread finds nobody and is lost, and a
 * resumed wait sleeps on with its predicate true. libphoenix resumed. Here the
 * predicate is set by the handler itself, which makes that window
 * deterministic: a wait that resumes never sees it.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <unity_fixture.h>


#define CONDEINTR_TIMEOUT_MS 3000


static pthread_mutex_t condeintr_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condeintr_cond = PTHREAD_COND_INITIALIZER;
static volatile sig_atomic_t condeintr_flag;
static volatile int condeintr_waiting, condeintr_done, condeintr_rc, condeintr_unlockRc, condeintr_timed;


static long condeintr_nowMs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}


static void condeintr_handler(int sig)
{
	(void)sig;
	condeintr_flag = 1;
}


static void *condeintr_waiter(void *arg)
{
	struct timespec ts;
	int rc = 0;

	(void)arg;

	pthread_mutex_lock(&condeintr_mutex);
	condeintr_waiting = 1;
	while ((condeintr_flag == 0) && ((rc == 0) || (rc == ETIMEDOUT))) {
		if (condeintr_timed != 0) {
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 60;
			rc = pthread_cond_timedwait(&condeintr_cond, &condeintr_mutex, &ts);
		}
		else {
			rc = pthread_cond_wait(&condeintr_cond, &condeintr_mutex);
		}
	}
	condeintr_rc = rc;
	/* The mutex must be held again: unlocking it succeeds */
	condeintr_unlockRc = pthread_mutex_unlock(&condeintr_mutex);
	condeintr_done = 1;

	return NULL;
}


static void condeintr_run(int timed)
{
	struct sigaction sa;
	pthread_t thread;
	long deadline;

	condeintr_flag = 0;
	condeintr_waiting = 0;
	condeintr_done = 0;
	condeintr_rc = -1;
	condeintr_unlockRc = -1;
	condeintr_timed = timed;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = condeintr_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &sa, NULL));

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, condeintr_waiter, NULL));

	/* Once the waiter has set the flag and the mutex is free again, it waits */
	while (condeintr_waiting == 0) {
		usleep(1000);
	}
	pthread_mutex_lock(&condeintr_mutex);
	pthread_mutex_unlock(&condeintr_mutex);
	usleep(20000);

	TEST_ASSERT_EQUAL_INT(0, pthread_kill(thread, SIGUSR1));

	deadline = condeintr_nowMs() + CONDEINTR_TIMEOUT_MS;
	while ((condeintr_done == 0) && (condeintr_nowMs() < deadline)) {
		usleep(10000);
	}

	if (condeintr_done == 0) {
		/* Rescue the waiter so the suite can go on */
		pthread_mutex_lock(&condeintr_mutex);
		pthread_cond_broadcast(&condeintr_cond);
		pthread_mutex_unlock(&condeintr_mutex);
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));

	sa.sa_handler = SIG_DFL;
	(void)sigaction(SIGUSR1, &sa, NULL);

	TEST_ASSERT_EQUAL_INT_MESSAGE(1, condeintr_done, "the wait did not end after the handler changed the predicate");
	TEST_ASSERT_EQUAL_INT(0, condeintr_rc);
	TEST_ASSERT_EQUAL_INT(0, condeintr_unlockRc);
}


TEST_GROUP(pthread_cond_eintr);


TEST_SETUP(pthread_cond_eintr)
{
}


TEST_TEAR_DOWN(pthread_cond_eintr)
{
}


TEST(pthread_cond_eintr, wait_ends_after_handler)
{
	condeintr_run(0);
}


TEST(pthread_cond_eintr, timedwait_ends_after_handler)
{
	condeintr_run(1);
}


TEST_GROUP_RUNNER(pthread_cond_eintr)
{
	RUN_TEST_CASE(pthread_cond_eintr, wait_ends_after_handler);
	RUN_TEST_CASE(pthread_cond_eintr, timedwait_ends_after_handler);
}
