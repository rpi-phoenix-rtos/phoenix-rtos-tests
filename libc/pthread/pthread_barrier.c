/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - pthread.h
 *
 * TESTED:
 *    - pthread_barrier_init(), pthread_barrier_wait(), pthread_barrier_destroy()
 *    - pthread_barrierattr_*()
 *
 * libphoenix had no barriers; the old GPU lane stubbed them so that wait
 * never blocks, which is not a barrier at all. The core case here fails
 * against such a stub: every thread checks, after the wait, that all threads
 * of the round have arrived.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include <unity_fixture.h>


#define BARRIER_THREADS 4
#define BARRIER_ROUNDS  500


static pthread_barrier_t barrier_b;
static volatile unsigned int barrier_arrived[BARRIER_ROUNDS];
static volatile unsigned int barrier_serial[BARRIER_ROUNDS];
static volatile unsigned int barrier_early;


static void *barrier_worker(void *arg)
{
	int r, rc;

	(void)arg;
	for (r = 0; r < BARRIER_ROUNDS; r++) {
		__atomic_add_fetch(&barrier_arrived[r], 1u, __ATOMIC_SEQ_CST);
		rc = pthread_barrier_wait(&barrier_b);
		if (rc == PTHREAD_BARRIER_SERIAL_THREAD) {
			__atomic_add_fetch(&barrier_serial[r], 1u, __ATOMIC_SEQ_CST);
		}
		else if (rc != 0) {
			__atomic_add_fetch(&barrier_early, 1u, __ATOMIC_SEQ_CST);
		}
		/* released: everybody must have arrived in this round */
		if (__atomic_load_n(&barrier_arrived[r], __ATOMIC_SEQ_CST) != BARRIER_THREADS) {
			__atomic_add_fetch(&barrier_early, 1u, __ATOMIC_SEQ_CST);
		}
	}
	return NULL;
}


static void *barrier_waitOnce(void *arg)
{
	(void)pthread_barrier_wait((pthread_barrier_t *)arg);
	return NULL;
}


TEST_GROUP(pthread_barrier);


TEST_SETUP(pthread_barrier)
{
	memset((void *)barrier_arrived, 0, sizeof(barrier_arrived));
	memset((void *)barrier_serial, 0, sizeof(barrier_serial));
	barrier_early = 0;
}


TEST_TEAR_DOWN(pthread_barrier)
{
}


TEST(pthread_barrier, attr)
{
	pthread_barrierattr_t attr;
	int pshared = -1;

	TEST_ASSERT_EQUAL_INT(0, pthread_barrierattr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_barrierattr_getpshared(&attr, &pshared));
	TEST_ASSERT_EQUAL_INT(PTHREAD_PROCESS_PRIVATE, pshared);
	TEST_ASSERT_EQUAL_INT(0, pthread_barrierattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE));
	TEST_ASSERT_EQUAL_INT(EINVAL, pthread_barrierattr_setpshared(&attr, 12345));
#ifdef __phoenix__
	/* OS limitation, as for mutexes */
	TEST_ASSERT_EQUAL_INT(ENOTSUP, pthread_barrierattr_setpshared(&attr, PTHREAD_PROCESS_SHARED));
#endif
	TEST_ASSERT_EQUAL_INT(0, pthread_barrierattr_destroy(&attr));
}


TEST(pthread_barrier, count_zero_is_invalid)
{
	pthread_barrier_t b;

	TEST_ASSERT_EQUAL_INT(EINVAL, pthread_barrier_init(&b, NULL, 0));
}


TEST(pthread_barrier, count_one_never_blocks)
{
	pthread_barrier_t b;
	int i;

	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_init(&b, NULL, 1));
	for (i = 0; i < 10; i++) {
		TEST_ASSERT_EQUAL_INT(PTHREAD_BARRIER_SERIAL_THREAD, pthread_barrier_wait(&b));
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_destroy(&b));
}


TEST(pthread_barrier, all_arrive_before_any_leaves)
{
	pthread_t t[BARRIER_THREADS];
	int i, r;

	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_init(&barrier_b, NULL, BARRIER_THREADS));
	for (i = 0; i < BARRIER_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&t[i], NULL, barrier_worker, NULL));
	}
	for (i = 0; i < BARRIER_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(t[i], NULL));
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_destroy(&barrier_b));

	TEST_ASSERT_EQUAL_INT_MESSAGE(0, barrier_early, "a thread left the barrier early");
	for (r = 0; r < BARRIER_ROUNDS; r++) {
		/* exactly one thread per round gets PTHREAD_BARRIER_SERIAL_THREAD */
		TEST_ASSERT_EQUAL_INT(1, barrier_serial[r]);
	}
}


/* Destroying the barrier as soon as one's own wait returns is legal (every
 * thread has been released) even though the others may still be on their
 * way out; it must neither fail nor pull the mutex out from under them. */
TEST(pthread_barrier, destroy_right_after_release)
{
	pthread_barrier_t b;
	pthread_t t[2];
	int i, k;

	for (k = 0; k < 100; k++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_barrier_init(&b, NULL, 3));
		for (i = 0; i < 2; i++) {
			TEST_ASSERT_EQUAL_INT(0, pthread_create(&t[i], NULL, barrier_waitOnce, &b));
		}
		(void)pthread_barrier_wait(&b);
		TEST_ASSERT_EQUAL_INT(0, pthread_barrier_destroy(&b));
		for (i = 0; i < 2; i++) {
			TEST_ASSERT_EQUAL_INT(0, pthread_join(t[i], NULL));
		}
	}
}


TEST(pthread_barrier, destroy_while_waiting_is_busy)
{
#ifdef __phoenix__
	pthread_barrier_t b;
	pthread_t t;
	int tries;

	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_init(&b, NULL, 2));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, barrier_waitOnce, &b));
	/* wait (bounded) until the helper is blocked in the barrier; reading the
	 * libphoenix field directly, which only this Phoenix-only case may do */
	for (tries = 0; (tries < 5000) && (__atomic_load_n(&b.waiting, __ATOMIC_SEQ_CST) == 0u); tries++) {
		usleep(1000);
	}
	TEST_ASSERT_EQUAL_INT(1, (int)b.waiting);
	TEST_ASSERT_EQUAL_INT(EBUSY, pthread_barrier_destroy(&b));

	(void)pthread_barrier_wait(&b); /* release the helper */
	TEST_ASSERT_EQUAL_INT(0, pthread_join(t, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_destroy(&b));
#else
	TEST_IGNORE_MESSAGE("destroying a barrier in use is undefined; EBUSY is libphoenix's choice");
#endif
}


TEST_GROUP_RUNNER(pthread_barrier)
{
	RUN_TEST_CASE(pthread_barrier, attr);
	RUN_TEST_CASE(pthread_barrier, count_zero_is_invalid);
	RUN_TEST_CASE(pthread_barrier, count_one_never_blocks);
	RUN_TEST_CASE(pthread_barrier, all_arrive_before_any_leaves);
	RUN_TEST_CASE(pthread_barrier, destroy_right_after_release);
	RUN_TEST_CASE(pthread_barrier, destroy_while_waiting_is_busy);
}
