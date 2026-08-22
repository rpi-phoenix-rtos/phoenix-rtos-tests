/*
 * Phoenix-RTOS
 *
 * phoenix-rtos-tests
 *
 * test/libc/semaphore - libphoenix counting semaphore (sys/threads.h)
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <pthread.h>
#include <sys/threads.h>
#include <unistd.h>

#include "unity_fixture.h"

/*
 * Regression coverage for the counting-semaphore wakeup contract.
 *
 * semaphoreUp() must wake a parked waiter for EVERY up, not only for the
 * 0->1 transition. A rewrite once signalled the condvar only when the counter
 * left zero; that loses wakeups whenever several units become available while
 * more than one thread is parked in semaphoreDown() - only one waiter woke and
 * the rest slept forever despite v > 0. It deadlocked multi-consumer pools
 * (found via vkQuake's task workers). multi_waiter_burst reproduces exactly
 * that shape and hangs->fails without the fix.
 */

#define NWAIT       8
#define WAKE_TO_MS  5000 /* generous - a correct impl wakes in well under this */
#define PARK_TO_MS  3000

static semaphore_t g_sem;
static handle_t    g_lock;
static int         g_ready;
static int         g_woke;


static void bump(int *p)
{
	mutexLock(g_lock);
	(*p)++;
	mutexUnlock(g_lock);
}


static int get(int *p)
{
	mutexLock(g_lock);
	int v = *p;
	mutexUnlock(g_lock);
	return v;
}


/* Announce readiness, then block until the main thread signals the semaphore. */
static void *waiter_fn(void *arg)
{
	(void)arg;
	bump(&g_ready);
	(void)semaphoreDown(&g_sem, 0); /* 0 == block forever */
	bump(&g_woke);
	return NULL;
}


static int spawn_detached(pthread_t *t, void *(*fn)(void *), void *arg)
{
	int err = pthread_create(t, NULL, fn, arg);
	if (err == 0) {
		pthread_detach(*t);
	}
	return err;
}


/* Poll a counter up to timeout_ms; returns 1 if it reached target in time. */
static int wait_until(int *counter, int target, int timeout_ms)
{
	for (int elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
		if (get(counter) >= target) {
			return 1;
		}
		usleep(10000);
	}
	return get(counter) >= target;
}


/* Run one burst round: NWAIT waiters park, then NWAIT ups must wake all NWAIT. */
static void run_burst_round(void)
{
	pthread_t th[NWAIT];

	mutexLock(g_lock);
	g_ready = 0;
	g_woke = 0;
	mutexUnlock(g_lock);

	for (int i = 0; i < NWAIT; ++i) {
		TEST_ASSERT_EQUAL_INT(0, spawn_detached(&th[i], waiter_fn, NULL));
	}

	/* Wait until every waiter has announced, then give them a moment to reach
	 * the condWait inside semaphoreDown() so the ups race parked waiters. */
	TEST_ASSERT_TRUE_MESSAGE(wait_until(&g_ready, NWAIT, PARK_TO_MS), "waiters never started");
	usleep(200000);

	/* Burst: make NWAIT units available while NWAIT threads are parked. */
	for (int i = 0; i < NWAIT; ++i) {
		semaphoreUp(&g_sem);
	}

	TEST_ASSERT_TRUE_MESSAGE(wait_until(&g_woke, NWAIT, WAKE_TO_MS),
		"not all waiters woke - counting-semaphore lost-wakeup regression");
	TEST_ASSERT_EQUAL_INT(NWAIT, get(&g_woke));
}


TEST_GROUP(test_semaphore);


TEST_SETUP(test_semaphore)
{
	TEST_ASSERT_EQUAL_INT(0, mutexCreate(&g_lock));
	TEST_ASSERT_EQUAL_INT(0, semaphoreCreate(&g_sem, 0));
	g_ready = 0;
	g_woke = 0;
}


TEST_TEAR_DOWN(test_semaphore)
{
	semaphoreDone(&g_sem);
	resourceDestroy(g_lock);
}


/* Sanity: count semantics and that a timed down on an empty semaphore returns
 * -ETIME promptly rather than blocking forever (the trywait building block). */
TEST(test_semaphore, basic_count_and_timeout)
{
	/* Empty: a tiny (1 us) and a small (10 ms) timeout must both time out. */
	TEST_ASSERT_EQUAL_INT(-ETIME, semaphoreDown(&g_sem, 1));
	TEST_ASSERT_EQUAL_INT(-ETIME, semaphoreDown(&g_sem, 10000));

	/* One up -> exactly one down succeeds, the next times out. */
	semaphoreUp(&g_sem);
	TEST_ASSERT_EQUAL_INT(0, semaphoreDown(&g_sem, 0));
	TEST_ASSERT_EQUAL_INT(-ETIME, semaphoreDown(&g_sem, 10000));

	/* Counts accumulate: two ups -> two downs. */
	semaphoreUp(&g_sem);
	semaphoreUp(&g_sem);
	TEST_ASSERT_EQUAL_INT(0, semaphoreDown(&g_sem, 0));
	TEST_ASSERT_EQUAL_INT(0, semaphoreDown(&g_sem, 0));
	TEST_ASSERT_EQUAL_INT(-ETIME, semaphoreDown(&g_sem, 10000));
}


/* The direct regression: N parked waiters, a burst of N ups, all must wake. */
TEST(test_semaphore, multi_waiter_burst)
{
	run_burst_round();
}


/* Same shape repeated, to catch timing-dependent lost wakeups across rounds. */
TEST(test_semaphore, multi_waiter_burst_repeated)
{
	for (int round = 0; round < 5; ++round) {
		run_burst_round();
	}
}


TEST_GROUP_RUNNER(test_semaphore)
{
	RUN_TEST_CASE(test_semaphore, basic_count_and_timeout);
	RUN_TEST_CASE(test_semaphore, multi_waiter_burst);
	RUN_TEST_CASE(test_semaphore, multi_waiter_burst_repeated);
}
