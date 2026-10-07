/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - semaphore.h
 *
 * TESTED:
 *    - sem_init(), sem_destroy(), sem_post(), sem_wait(), sem_trywait(),
 *      sem_timedwait(), sem_getvalue()
 *
 * Unnamed semaphores: foot's render workers use them, and WebKit's thread
 * suspension posts one from a signal handler (sem_post() is
 * async-signal-safe). The waits are bounded so a lost wakeup fails a case
 * instead of hanging the suite.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <unity_fixture.h>


#define PSEM_TIMEOUT_MS 5000
#define PSEM_THREADS    4
#define PSEM_ITEMS      2000


static sem_t psem;
static volatile int psem_done;


static long psem_nowMs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}


/* sem_timedwait() with a deadline ms from now, so nothing waits forever */
static int psem_waitFor(sem_t *sem, long ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}

	return sem_timedwait(sem, &ts);
}


static int psem_value(void)
{
	int v = -1;

	TEST_ASSERT_EQUAL_INT(0, sem_getvalue(&psem, &v));
	return v;
}


static void *psem_waiter(void *arg)
{
	(void)arg;

	if (psem_waitFor(&psem, PSEM_TIMEOUT_MS) == 0) {
		__atomic_add_fetch(&psem_done, 1, __ATOMIC_SEQ_CST);
	}

	return NULL;
}


static void *psem_consumer(void *arg)
{
	int *count = arg;

	while (psem_waitFor(&psem, PSEM_TIMEOUT_MS) == 0) {
		if (__atomic_add_fetch(&psem_done, 1, __ATOMIC_SEQ_CST) > PSEM_ITEMS) {
			break;
		}
		(*count)++;
	}

	return NULL;
}


TEST_GROUP(posix_semaphore);


TEST_SETUP(posix_semaphore)
{
	psem_done = 0;
}


TEST_TEAR_DOWN(posix_semaphore)
{
}


TEST(posix_semaphore, count_and_trywait)
{
	TEST_ASSERT_EQUAL_INT(0, sem_init(&psem, 0, 2));
	TEST_ASSERT_EQUAL_INT(2, psem_value());

	TEST_ASSERT_EQUAL_INT(0, sem_trywait(&psem));
	TEST_ASSERT_EQUAL_INT(0, sem_wait(&psem));
	TEST_ASSERT_EQUAL_INT(0, psem_value());

	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, sem_trywait(&psem));
	TEST_ASSERT_EQUAL_INT(EAGAIN, errno);

	TEST_ASSERT_EQUAL_INT(0, sem_post(&psem));
	TEST_ASSERT_EQUAL_INT(0, sem_post(&psem));
	TEST_ASSERT_EQUAL_INT(2, psem_value());
	TEST_ASSERT_EQUAL_INT(0, sem_destroy(&psem));
}


/* Each post releases one blocked waiter, none is lost */
TEST(posix_semaphore, posts_release_waiters)
{
	pthread_t threads[PSEM_THREADS];
	long deadline;
	int i;

	TEST_ASSERT_EQUAL_INT(0, sem_init(&psem, 0, 0));
	for (i = 0; i < PSEM_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[i], NULL, psem_waiter, NULL));
	}
	usleep(50000); /* let them block */
	TEST_ASSERT_EQUAL_INT(0, psem_done);

	for (i = 0; i < PSEM_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, sem_post(&psem));
	}

	deadline = psem_nowMs() + PSEM_TIMEOUT_MS;
	while ((psem_done < PSEM_THREADS) && (psem_nowMs() < deadline)) {
		usleep(1000);
	}
	for (i = 0; i < PSEM_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(threads[i], NULL));
	}

	TEST_ASSERT_EQUAL_INT(PSEM_THREADS, psem_done);
	TEST_ASSERT_EQUAL_INT(0, psem_value());
	TEST_ASSERT_EQUAL_INT(0, sem_destroy(&psem));
}


/* Producer and consumers racing: every unit is consumed exactly once */
TEST(posix_semaphore, producer_consumers)
{
	pthread_t threads[2];
	int counts[2] = { 0, 0 }, i;
	long deadline;

	TEST_ASSERT_EQUAL_INT(0, sem_init(&psem, 0, 0));
	for (i = 0; i < 2; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[i], NULL, psem_consumer, &counts[i]));
	}
	for (i = 0; i < PSEM_ITEMS; i++) {
		TEST_ASSERT_EQUAL_INT(0, sem_post(&psem));
		if ((i % 64) == 0) {
			usleep(100);
		}
	}
	/* Let the consumers drain, then one more unit each to stop them */
	deadline = psem_nowMs() + PSEM_TIMEOUT_MS;
	while ((psem_done < PSEM_ITEMS) && (psem_nowMs() < deadline)) {
		usleep(1000);
	}
	TEST_ASSERT_EQUAL_INT(0, sem_post(&psem));
	TEST_ASSERT_EQUAL_INT(0, sem_post(&psem));
	for (i = 0; i < 2; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(threads[i], NULL));
	}

	TEST_ASSERT_EQUAL_INT(PSEM_ITEMS, counts[0] + counts[1]);
	TEST_ASSERT_EQUAL_INT(0, sem_destroy(&psem));
}


/* A wait that times out leaves the count as it was */
TEST(posix_semaphore, timedwait_times_out)
{
	long start, elapsed;

	TEST_ASSERT_EQUAL_INT(0, sem_init(&psem, 0, 0));

	start = psem_nowMs();
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, psem_waitFor(&psem, 100));
	elapsed = psem_nowMs() - start;
	TEST_ASSERT_EQUAL_INT(ETIMEDOUT, errno);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(90, (int)elapsed);
	TEST_ASSERT_LESS_THAN_INT(PSEM_TIMEOUT_MS, (int)elapsed);

	TEST_ASSERT_EQUAL_INT(0, psem_value());
	TEST_ASSERT_EQUAL_INT(0, sem_post(&psem));
	TEST_ASSERT_EQUAL_INT(1, psem_value());
	TEST_ASSERT_EQUAL_INT(0, sem_trywait(&psem));
	TEST_ASSERT_EQUAL_INT(0, sem_destroy(&psem));
}


struct psem_timed {
	int rc;
	int err;
	long ms;
};


static void *psem_timedWaiter(void *arg)
{
	struct psem_timed *t = arg;
	long start = psem_nowMs();

	t->rc = psem_waitFor(&psem, 300);
	t->err = errno;
	t->ms = psem_nowMs() - start;
	return NULL;
}


/* Two timed waiters, one post: the waiter that does not get the unit must still
 * time out at its deadline. Both see the token's pipe readable; the one that
 * loses the read used to block in read() until the next post (here 1500 ms). */
TEST(posix_semaphore, timedwait_loser_keeps_deadline)
{
	struct psem_timed t[2] = { 0 };
	pthread_t th[2];
	int i, won = 0, lost = -1;

	TEST_ASSERT_EQUAL_INT(0, sem_init(&psem, 0, 0));
	for (i = 0; i < 2; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&th[i], NULL, psem_timedWaiter, &t[i]));
	}
	usleep(50000);
	TEST_ASSERT_EQUAL_INT(0, sem_post(&psem));

	/* A stuck loser is released by a second post, so the suite never hangs */
	usleep(1500000);
	(void)sem_post(&psem);

	for (i = 0; i < 2; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(th[i], NULL));
	}
	for (i = 0; i < 2; i++) {
		if (t[i].rc == 0) {
			won++;
		}
		else {
			lost = i;
		}
	}
	printf("PSEM loser_keeps_deadline won=%d ms=%ld,%ld\n", won, t[0].ms, t[1].ms);

	TEST_ASSERT_EQUAL_INT(1, won);
	TEST_ASSERT_EQUAL_INT(ETIMEDOUT, t[lost].err);
	TEST_ASSERT_LESS_THAN_INT(800, (int)t[lost].ms);
	TEST_ASSERT_EQUAL_INT(0, sem_destroy(&psem));
}


TEST(posix_semaphore, timedwait_bad_time)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000000L };

	/* Needed when the call would block (glibc checks it always) */
	TEST_ASSERT_EQUAL_INT(0, sem_init(&psem, 0, 0));
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, sem_timedwait(&psem, &ts));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);

	/* and the failed wait left no trace on the count */
	TEST_ASSERT_EQUAL_INT(0, psem_value());
	TEST_ASSERT_EQUAL_INT(0, sem_post(&psem));
	TEST_ASSERT_EQUAL_INT(0, sem_trywait(&psem));
	TEST_ASSERT_EQUAL_INT(0, sem_destroy(&psem));
}


static void psem_handler(int sig)
{
	(void)sig;
	(void)sem_post(&psem);
}


static void *psem_signaller(void *arg)
{
	usleep(50000);
	(void)pthread_kill(*(pthread_t *)arg, SIGUSR1);
	return NULL;
}


/* The classic use: a handler posts to wake the thread it interrupted */
TEST(posix_semaphore, post_from_signal_handler)
{
	struct sigaction sa;
	pthread_t self = pthread_self(), thread;
	int rc, err;

	TEST_ASSERT_EQUAL_INT(0, sem_init(&psem, 0, 0));
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = psem_handler;
	sigemptyset(&sa.sa_mask);
	TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &sa, NULL));

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, psem_signaller, &self));
	rc = psem_waitFor(&psem, PSEM_TIMEOUT_MS);
	err = errno;
	TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));

	sa.sa_handler = SIG_DFL;
	(void)sigaction(SIGUSR1, &sa, NULL);

	if (rc != 0) {
		/* Interrupted (allowed): then the handler's unit must still be there */
		TEST_ASSERT_EQUAL_INT(EINTR, err);
		TEST_ASSERT_EQUAL_INT(0, sem_trywait(&psem));
	}
	TEST_ASSERT_EQUAL_INT(0, psem_value());
	TEST_ASSERT_EQUAL_INT(0, sem_destroy(&psem));
}


TEST(posix_semaphore, overflow)
{
	TEST_ASSERT_EQUAL_INT(0, sem_init(&psem, 0, SEM_VALUE_MAX));
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, sem_post(&psem));
	TEST_ASSERT_EQUAL_INT(EOVERFLOW, errno);
	TEST_ASSERT_EQUAL_INT(0, sem_destroy(&psem));
}


TEST(posix_semaphore, process_shared_unsupported)
{
#ifdef __phoenix__
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, sem_init(&psem, 1, 0));
	TEST_ASSERT_EQUAL_INT(ENOSYS, errno);
#else
	TEST_IGNORE_MESSAGE("glibc supports process-shared semaphores");
#endif
}


TEST_GROUP_RUNNER(posix_semaphore)
{
	RUN_TEST_CASE(posix_semaphore, count_and_trywait);
	RUN_TEST_CASE(posix_semaphore, posts_release_waiters);
	RUN_TEST_CASE(posix_semaphore, producer_consumers);
	RUN_TEST_CASE(posix_semaphore, timedwait_times_out);
	RUN_TEST_CASE(posix_semaphore, timedwait_loser_keeps_deadline);
	RUN_TEST_CASE(posix_semaphore, timedwait_bad_time);
	RUN_TEST_CASE(posix_semaphore, post_from_signal_handler);
	RUN_TEST_CASE(posix_semaphore, overflow);
	RUN_TEST_CASE(posix_semaphore, process_shared_unsupported);
}
