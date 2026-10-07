/*
 * Phoenix-RTOS
 *
 * phoenix-rtos-tests
 *
 * pthread thread lifetime: when the library's record of a thread, and the
 * stack it allocated for it, are given back.
 *
 * A joinable thread's record goes in pthread_join(). A detached thread cannot
 * free the stack it is still running on, so it parks it for a live thread to
 * reclaim in a later pthread_create()/pthread_join(). These cases cover the
 * paths between those: joins and detached exits interleaved on several cores,
 * a detached thread cancelled instead of exiting, and a cancel racing the
 * target's own exit. Most failures here are a crash, a hang or a sanitizer
 * report rather than a failed assertion -- run the host harness
 * (phoenix-rpi tools/pthread-key-hosttest) under ThreadSanitizer and
 * AddressSanitizer as well.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity_fixture.h"


TEST_GROUP(pthread_lifetime);


TEST_SETUP(pthread_lifetime)
{
}


TEST_TEAR_DOWN(pthread_lifetime)
{
}


static void *life_noop(void *arg)
{
	return arg;
}


/* Create and join a thread that does nothing. Besides its own record, this
 * reclaims everything detached threads have parked so far. */
static int life_createJoin(void)
{
	pthread_t th;
	void *ret = NULL;
	int err = pthread_create(&th, NULL, life_noop, (void *)0x1234);

	if (err == 0) {
		err = pthread_join(th, &ret);
	}
	if ((err == 0) && (ret != (void *)0x1234)) {
		err = -1;
	}
	return err;
}


/* ---- joins and detached exits, interleaved ---- */

#define LIFE_WORKERS    4
#define LIFE_ITERATIONS 200

static volatile int life_failures;


static void *life_joiner(void *arg)
{
	(void)arg;
	for (int i = 0; i < LIFE_ITERATIONS; i++) {
		if (life_createJoin() != 0) {
			__atomic_fetch_add(&life_failures, 1, __ATOMIC_RELAXED);
		}
	}
	return NULL;
}


/* Runs until its creator has detached it: a thread that ends while still
 * joinable and is detached only afterwards is never released (a separate
 * defect, not exercised here). */
static void *life_gated(void *arg)
{
	volatile int *gate = arg;

	while (__atomic_load_n(gate, __ATOMIC_ACQUIRE) == 0) {
		sched_yield();
	}
	return NULL;
}


static int life_createDetached(volatile int *gate)
{
	pthread_t th;
	int err;

	*gate = 0;
	err = pthread_create(&th, NULL, life_gated, (void *)gate);
	if (err == 0) {
		err = pthread_detach(th);
		__atomic_store_n(gate, 1, __ATOMIC_RELEASE);
	}
	return err;
}


static volatile int life_gates[LIFE_WORKERS][LIFE_ITERATIONS];


static void *life_detacher(void *arg)
{
	volatile int *gates = life_gates[(intptr_t)arg];

	for (int i = 0; i < LIFE_ITERATIONS; i++) {
		if (life_createDetached(&gates[i]) != 0) {
			__atomic_fetch_add(&life_failures, 1, __ATOMIC_RELAXED);
		}
	}
	return NULL;
}


/*
 * pthread_join() released the thread list lock once in its release path and
 * then a second time on its way out. The second release is a no-op while
 * nobody holds the lock, and releases SOMEBODY ELSE'S hold when another thread
 * is inside the list (the kernel does not check the owner of a plain lock):
 * two threads then edit the list at once. Several threads joining and creating
 * at the same time is all it takes. A DEBUG_THREADS kernel reports every such
 * release as `unlock on not locked lock`; the host harness aborts on it.
 */
TEST(pthread_lifetime, concurrent_join_and_detach)
{
	pthread_t workers[LIFE_WORKERS];

	life_failures = 0;
	for (int i = 0; i < LIFE_WORKERS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&workers[i], NULL, ((i % 2) == 0) ? life_joiner : life_detacher, (void *)(intptr_t)i));
	}
	for (int i = 0; i < LIFE_WORKERS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(workers[i], NULL));
	}
	TEST_ASSERT_EQUAL_INT(0, life_failures);
	TEST_ASSERT_EQUAL_INT(0, life_createJoin());
}


/* ---- detached exits racing the reclaim pass ---- */

#define LIFE_BURST_THREADS 16
#define LIFE_BURST_ROUNDS  40

static volatile int life_burstGates[2][LIFE_BURST_ROUNDS][LIFE_BURST_THREADS];


static void *life_burst(void *arg)
{
	intptr_t id = (intptr_t)arg;

	for (int round = 0; round < LIFE_BURST_ROUNDS; round++) {
		for (int i = 0; i < LIFE_BURST_THREADS; i++) {
			if (life_createDetached(&life_burstGates[id][round][i]) != 0) {
				__atomic_fetch_add(&life_failures, 1, __ATOMIC_RELAXED);
			}
		}
	}
	return NULL;
}


/*
 * An exiting detached thread put its stack on the list of stacks to reclaim
 * AFTER it had released the lock that guards that list, while a thread in
 * pthread_create()/pthread_join() takes the whole list under the lock. A
 * reclaimer emptying the list in between had its node linked back in behind
 * it: freed and unmapped twice, or lost -- with the second unmap hitting the
 * stack of a thread created since. Here detached threads exit in bursts on
 * every core while two other threads keep reclaiming.
 */
TEST(pthread_lifetime, detached_exits_race_reclaim)
{
	pthread_t bursts[2], joiners[2];

	life_failures = 0;
	for (int i = 0; i < 2; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&bursts[i], NULL, life_burst, (void *)(intptr_t)i));
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&joiners[i], NULL, life_joiner, NULL));
	}
	for (int i = 0; i < 2; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(bursts[i], NULL));
		TEST_ASSERT_EQUAL_INT(0, pthread_join(joiners[i], NULL));
	}
	TEST_ASSERT_EQUAL_INT(0, life_failures);
	TEST_ASSERT_EQUAL_INT(0, life_createJoin());
}


/* ---- cancelling a detached thread ---- */

#define LIFE_CANCEL_THREADS 8

static volatile int life_go;


/* Never returns: it is only ever cancelled */
static void *life_wait(void *arg)
{
	(void)arg;
	for (;;) {
		usleep(1000);
	}
	return NULL;
}


/*
 * A detached thread that is cancelled never reaches its own exit path, so the
 * cancel must give its record and stack back on its behalf. It did not: the
 * record stayed on the thread list for good -- which a stale handle shows, as
 * pthread_detach() still finds it (EINVAL, "already detached") instead of
 * reporting it gone (ESRCH) -- and the stack with it.
 */
TEST(pthread_lifetime, cancelled_detached_is_released)
{
	pthread_t th[LIFE_CANCEL_THREADS];

	for (int i = 0; i < LIFE_CANCEL_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&th[i], NULL, life_wait, NULL));
		TEST_ASSERT_EQUAL_INT(0, pthread_detach(th[i]));
	}
	usleep(10000);

	for (int i = 0; i < LIFE_CANCEL_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_cancel(th[i]));
	}
	/* Reclaim what the cancels parked */
	TEST_ASSERT_EQUAL_INT(0, life_createJoin());

	for (int i = 0; i < LIFE_CANCEL_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(ESRCH, pthread_detach(th[i]));
	}
}


static pthread_key_t life_key;
static volatile int life_readerReady;


static void *life_reader(void *arg)
{
	(void)arg;
	(void)pthread_setspecific(life_key, (void *)0x77);
	__atomic_store_n(&life_readerReady, 1, __ATOMIC_RELEASE);

	/* pthread_getspecific() reads this thread's record without a lock, until
	 * the SIGCANCEL sent by pthread_cancel() actually stops it. The record
	 * must not be freed before that. */
	for (;;) {
		(void)pthread_getspecific(life_key);
	}
	return NULL;
}


/* The release must not run ahead of the thread: a cancelled thread still runs
 * for a while after pthread_cancel() returns, reading its own record. Reclaim
 * passes are forced right behind each cancel (AddressSanitizer on the host
 * harness reports a premature free). */
TEST(pthread_lifetime, cancelled_detached_reader_outlives_cancel)
{
	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&life_key, NULL));

	for (int i = 0; i < LIFE_CANCEL_THREADS; i++) {
		pthread_t th;

		life_readerReady = 0;
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, life_reader, NULL));
		TEST_ASSERT_EQUAL_INT(0, pthread_detach(th));
		while (__atomic_load_n(&life_readerReady, __ATOMIC_ACQUIRE) == 0) {
			usleep(1000);
		}
		TEST_ASSERT_EQUAL_INT(0, pthread_cancel(th));
		TEST_ASSERT_EQUAL_INT(0, life_createJoin());
		TEST_ASSERT_EQUAL_INT(0, life_createJoin());
	}

	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(life_key));
}


#define LIFE_RACE_ROUNDS 200

/* Exits as soon as it is let go */
static void *life_goThenExit(void *arg)
{
	(void)arg;
	while (__atomic_load_n(&life_go, __ATOMIC_ACQUIRE) == 0) {
		sched_yield();
	}
	return NULL;
}


/*
 * A cancel that meets the target on its way out: exactly one of the two may
 * give the record back. The target was let go a moment before, at varying
 * distances, so either may get there first. No reclaim pass runs between the
 * go and the cancel, so the handle stays valid for pthread_cancel() even when
 * the target has already retired itself -- which then reports ESRCH.
 */
TEST(pthread_lifetime, cancel_races_detached_exit)
{
	int cancelled = 0, gone = 0;

	for (int round = 0; round < LIFE_RACE_ROUNDS; round++) {
		pthread_t th;
		int err;

		__atomic_store_n(&life_go, 0, __ATOMIC_RELAXED);
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, life_goThenExit, NULL));
		TEST_ASSERT_EQUAL_INT(0, pthread_detach(th));

		__atomic_store_n(&life_go, 1, __ATOMIC_RELEASE);
		for (volatile int spin = 0; spin < (round % 40) * 500; spin++) {
		}

		err = pthread_cancel(th);
		if (err == 0) {
			cancelled++;
		}
		else {
			TEST_ASSERT_EQUAL_INT(ESRCH, err);
			gone++;
		}
	}

	TEST_ASSERT_EQUAL_INT(0, life_createJoin());
	printf("LIFE cancel_races_detached_exit: cancelled=%d gone=%d\n", cancelled, gone);
}


TEST_GROUP_RUNNER(pthread_lifetime)
{
	RUN_TEST_CASE(pthread_lifetime, concurrent_join_and_detach);
	RUN_TEST_CASE(pthread_lifetime, detached_exits_race_reclaim);
	RUN_TEST_CASE(pthread_lifetime, cancelled_detached_is_released);
	RUN_TEST_CASE(pthread_lifetime, cancelled_detached_reader_outlives_cancel);
	RUN_TEST_CASE(pthread_lifetime, cancel_races_detached_exit);
}


void runner(void)
{
	RUN_TEST_GROUP(pthread_lifetime);
}


int main(int argc, char *argv[])
{
	return UnityMain(argc, (const char **)argv, runner) == 0 ? 0 : 1;
}
