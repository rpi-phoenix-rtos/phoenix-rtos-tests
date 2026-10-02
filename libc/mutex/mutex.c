/*
 * Phoenix-RTOS
 *
 *    Mutexes and condition variables, as built on a user-space lock word
 *    TESTED:
 *    - mutual exclusion under 4-thread contention for normal, recursive and
 *      error-checking mutexes, and for a trylock-only locker
 *    - trylock, timedlock (timeout, free mutex, deadline already past)
 *    - recursive depth and owner checks, error-checking EDEADLK/EPERM
 *    - the default protocol is PTHREAD_PRIO_NONE; PTHREAD_PRIO_INHERIT still works
 *    - condition variables: ping-pong (a lost wake-up hangs it), a pool of
 *      consumers fed by signal, broadcast to many waiters, timed wait, the
 *      mutex is owned again when a wait returns, waits on a kernel (PI) mutex
 *    - fork(): the child owns what the forking thread held, mutexes another
 *      thread held stay locked, new mutexes and the heap work in the child
 *      even when another thread was allocating during the fork
 *    - (Phoenix) futexWait()/futexWake() themselves
 *
 * libphoenix used to make every lock and unlock a kernel mutex syscall. A
 * mutex is now a word in user memory changed with atomic operations, and the
 * kernel is entered only to sleep on contention (futexWait/futexWake). The
 * places such a lock goes wrong are a lost wake-up (a waiter sleeps through the
 * unlock that should wake it: the ping-pong and the pools hang, and the timed
 * waits used here turn the hang into a failure), a broken owner field
 * (recursive/error-checking results), and state that fork() copies.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include <unity_fixture.h>

#ifdef __phoenix__
#include <sys/threads.h>

/* Weak, so that the test also builds against a libphoenix from before the
 * user-space mutexes, to show which cases fail there; the futex cases are then
 * ignored. */
extern int futexWait(volatile unsigned int *addr, unsigned int val, time_t timeout, int clock) __attribute__((weak));
extern int futexWake(volatile unsigned int *addr, unsigned int count) __attribute__((weak));
#endif


#define NTHREADS 4
#define NITERS   20000

/* Upper bound for any single wait in these tests: reaching it means a lost wake-up */
#define HANG_MS 10000


static struct {
	pthread_mutex_t m;
	pthread_cond_t c;
	volatile unsigned long counter;
	volatile int flag;
	unsigned int iters;
} mtx_common;


static uint64_t mtx_nowMs(clockid_t clk)
{
	struct timespec ts;

	(void)clock_gettime(clk, &ts);

	return ((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u);
}


static void mtx_deadline(struct timespec *ts, clockid_t clk, unsigned int ms)
{
	(void)clock_gettime(clk, ts);
	ts->tv_sec += ms / 1000u;
	ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}


static void mtx_initType(pthread_mutex_t *m, int type, int protocol)
{
	pthread_mutexattr_t attr;

	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_settype(&attr, type));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_setprotocol(&attr, protocol));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(m, &attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_destroy(&attr));
}


/* A condition variable whose timed waits use CLOCK_MONOTONIC (POSIX default is
 * CLOCK_REALTIME, Phoenix's MONOTONIC; naming it keeps the test portable). */
static void mtx_initCond(pthread_cond_t *c)
{
	pthread_condattr_t attr;

	TEST_ASSERT_EQUAL_INT(0, pthread_condattr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
	TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(c, &attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_condattr_destroy(&attr));
}


/* Waits for `pred` under mtx_common.m; returns ETIMEDOUT after HANG_MS */
static int mtx_condWaitFor(pthread_cond_t *c, pthread_mutex_t *m, volatile int *pred, int value)
{
	struct timespec dl;
	int err = 0;

	mtx_deadline(&dl, CLOCK_MONOTONIC, HANG_MS);
	while ((*pred != value) && (err == 0)) {
		err = pthread_cond_timedwait(c, m, &dl);
	}

	return (*pred == value) ? 0 : err;
}


/*
 * Mutual exclusion
 */


static void *mtx_incrementer(void *arg)
{
	unsigned int i;
	(void)arg;

	for (i = 0; i < mtx_common.iters; i++) {
		if (pthread_mutex_lock(&mtx_common.m) != 0) {
			return (void *)1;
		}
		/* Read-modify-write in two steps, so overlap loses updates */
		unsigned long v = mtx_common.counter;
		mtx_common.counter = v + 1u;
		if (pthread_mutex_unlock(&mtx_common.m) != 0) {
			return (void *)1;
		}
	}

	return NULL;
}


static void *mtx_tryIncrementer(void *arg)
{
	unsigned int i;
	(void)arg;

	for (i = 0; i < mtx_common.iters; i++) {
		while (pthread_mutex_trylock(&mtx_common.m) != 0) {
			(void)sched_yield();
		}
		unsigned long v = mtx_common.counter;
		mtx_common.counter = v + 1u;
		if (pthread_mutex_unlock(&mtx_common.m) != 0) {
			return (void *)1;
		}
	}

	return NULL;
}


static void mtx_contend(void *(*fn)(void *), unsigned int iters)
{
	pthread_t th[NTHREADS];
	void *ret;
	int i;

	mtx_common.counter = 0;
	mtx_common.iters = iters;

	for (i = 0; i < NTHREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&th[i], NULL, fn, NULL));
	}
	for (i = 0; i < NTHREADS; i++) {
		ret = (void *)1;
		TEST_ASSERT_EQUAL_INT(0, pthread_join(th[i], &ret));
		TEST_ASSERT_NULL_MESSAGE(ret, "a lock or unlock call failed");
	}

	TEST_ASSERT_EQUAL_UINT32(NTHREADS * iters, mtx_common.counter);
}


TEST_GROUP(mutex_fast);


TEST_SETUP(mutex_fast)
{
}


TEST_TEAR_DOWN(mutex_fast)
{
}


TEST(mutex_fast, contention_normal)
{
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&mtx_common.m, NULL));
	mtx_contend(mtx_incrementer, NITERS);
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&mtx_common.m));
}


/* PTHREAD_MUTEX_INITIALIZER: initialized on first use, by racing threads */
TEST(mutex_fast, contention_static_initializer)
{
	static const pthread_mutex_t init = PTHREAD_MUTEX_INITIALIZER;

	memcpy(&mtx_common.m, &init, sizeof(init));
	mtx_contend(mtx_incrementer, NITERS);
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&mtx_common.m));
}


TEST(mutex_fast, contention_recursive)
{
	mtx_initType(&mtx_common.m, PTHREAD_MUTEX_RECURSIVE, PTHREAD_PRIO_NONE);
	mtx_contend(mtx_incrementer, NITERS);
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&mtx_common.m));
}


TEST(mutex_fast, contention_errorcheck)
{
	mtx_initType(&mtx_common.m, PTHREAD_MUTEX_ERRORCHECK, PTHREAD_PRIO_NONE);
	mtx_contend(mtx_incrementer, NITERS);
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&mtx_common.m));
}


TEST(mutex_fast, contention_trylock)
{
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&mtx_common.m, NULL));
	mtx_contend(mtx_tryIncrementer, NITERS / 4u);
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&mtx_common.m));
}


/* A priority-inheritance mutex is still a kernel mutex; it must still exclude */
TEST(mutex_fast, contention_prio_inherit)
{
	mtx_initType(&mtx_common.m, PTHREAD_MUTEX_NORMAL, PTHREAD_PRIO_INHERIT);
	mtx_contend(mtx_incrementer, NITERS / 4u);
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&mtx_common.m));
}


/*
 * Types, trylock, timedlock
 */


static void *mtx_holder(void *arg)
{
	pthread_mutex_t *m = arg;

	if (pthread_mutex_lock(m) != 0) {
		return (void *)1;
	}

	pthread_mutex_lock(&mtx_common.m);
	mtx_common.flag = 1;
	pthread_cond_broadcast(&mtx_common.c);
	while (mtx_common.flag != 2) {
		pthread_cond_wait(&mtx_common.c, &mtx_common.m);
	}
	pthread_mutex_unlock(&mtx_common.m);

	return (pthread_mutex_unlock(m) != 0) ? (void *)1 : NULL;
}


/* Starts a thread that locks `m` and keeps it until mtx_releaseHolder() */
static void mtx_startHolder(pthread_t *th, pthread_mutex_t *m)
{
	mtx_common.flag = 0;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(th, NULL, mtx_holder, m));

	pthread_mutex_lock(&mtx_common.m);
	TEST_ASSERT_EQUAL_INT(0, mtx_condWaitFor(&mtx_common.c, &mtx_common.m, &mtx_common.flag, 1));
	pthread_mutex_unlock(&mtx_common.m);
}


static void mtx_releaseHolder(pthread_t th)
{
	void *ret = (void *)1;

	pthread_mutex_lock(&mtx_common.m);
	mtx_common.flag = 2;
	pthread_cond_broadcast(&mtx_common.c);
	pthread_mutex_unlock(&mtx_common.m);

	TEST_ASSERT_EQUAL_INT(0, pthread_join(th, &ret));
	TEST_ASSERT_NULL(ret);
}


TEST_GROUP(mutex_semantics);


TEST_SETUP(mutex_semantics)
{
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&mtx_common.m, NULL));
	mtx_initCond(&mtx_common.c);
}


TEST_TEAR_DOWN(mutex_semantics)
{
	pthread_cond_destroy(&mtx_common.c);
	pthread_mutex_destroy(&mtx_common.m);
}


TEST(mutex_semantics, default_protocol_is_prio_none)
{
	pthread_mutexattr_t attr;
	int v = -1;

	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_getprotocol(&attr, &v));
	TEST_ASSERT_EQUAL_INT(PTHREAD_PRIO_NONE, v);
	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_destroy(&attr));
}


TEST(mutex_semantics, trylock)
{
	pthread_mutex_t m;
	pthread_t th;

	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&m, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_trylock(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&m));

	mtx_startHolder(&th, &m);
	TEST_ASSERT_EQUAL_INT(EBUSY, pthread_mutex_trylock(&m));
	mtx_releaseHolder(th);

	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_trylock(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&m));
}


TEST(mutex_semantics, timedlock)
{
	struct timespec dl;
	pthread_mutex_t m;
	pthread_t th;
	uint64_t t0, dt;

	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&m, NULL));

	/* A free mutex is taken whatever the deadline */
	mtx_deadline(&dl, CLOCK_REALTIME, 0);
	dl.tv_sec -= 1;
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_timedlock(&m, &dl));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&m));

	mtx_startHolder(&th, &m);

	/* Held, deadline already past: fails at once */
	TEST_ASSERT_EQUAL_INT(ETIMEDOUT, pthread_mutex_timedlock(&m, &dl));

	/* Held, deadline 100 ms ahead: fails after it */
	t0 = mtx_nowMs(CLOCK_MONOTONIC);
	mtx_deadline(&dl, CLOCK_REALTIME, 100);
	TEST_ASSERT_EQUAL_INT(ETIMEDOUT, pthread_mutex_timedlock(&m, &dl));
	dt = mtx_nowMs(CLOCK_MONOTONIC) - t0;
	TEST_ASSERT_GREATER_OR_EQUAL_UINT32(90u, (uint32_t)dt);
	TEST_ASSERT_LESS_THAN_UINT32(HANG_MS, (uint32_t)dt);

	mtx_releaseHolder(th);

	/* Released: a waiting timedlock gets it */
	mtx_deadline(&dl, CLOCK_REALTIME, HANG_MS);
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_timedlock(&m, &dl));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&m));
}


TEST(mutex_semantics, timedlock_recursive_held_elsewhere)
{
	struct timespec dl;
	pthread_mutex_t m;
	pthread_t th;

	mtx_initType(&m, PTHREAD_MUTEX_RECURSIVE, PTHREAD_PRIO_NONE);
	mtx_startHolder(&th, &m);

	mtx_deadline(&dl, CLOCK_REALTIME, 50);
	TEST_ASSERT_EQUAL_INT(ETIMEDOUT, pthread_mutex_timedlock(&m, &dl));
	TEST_ASSERT_EQUAL_INT(EBUSY, pthread_mutex_trylock(&m));
	/* Not the owner */
	TEST_ASSERT_EQUAL_INT(EPERM, pthread_mutex_unlock(&m));

	mtx_releaseHolder(th);
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&m));
}


TEST(mutex_semantics, recursive_depth)
{
	pthread_mutex_t m;
	pthread_t th;
	int i;

	mtx_initType(&m, PTHREAD_MUTEX_RECURSIVE, PTHREAD_PRIO_NONE);

	for (i = 0; i < 100; i++) {
		TEST_ASSERT_EQUAL_INT(0, (i % 2 == 0) ? pthread_mutex_lock(&m) : pthread_mutex_trylock(&m));
	}
	for (i = 0; i < 100; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&m));
	}
	TEST_ASSERT_EQUAL_INT(EPERM, pthread_mutex_unlock(&m));

	/* Fully released: another thread can take it */
	mtx_startHolder(&th, &m);
	mtx_releaseHolder(th);

	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&m));
}


TEST(mutex_semantics, errorcheck)
{
	pthread_mutex_t m;
	pthread_t th;

	mtx_initType(&m, PTHREAD_MUTEX_ERRORCHECK, PTHREAD_PRIO_NONE);

	TEST_ASSERT_EQUAL_INT(EPERM, pthread_mutex_unlock(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&m));
	TEST_ASSERT_EQUAL_INT(EDEADLK, pthread_mutex_lock(&m));
	TEST_ASSERT_EQUAL_INT(EBUSY, pthread_mutex_trylock(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&m));
	TEST_ASSERT_EQUAL_INT(EPERM, pthread_mutex_unlock(&m));

	mtx_startHolder(&th, &m);
	TEST_ASSERT_EQUAL_INT(EBUSY, pthread_mutex_trylock(&m));
	TEST_ASSERT_EQUAL_INT(EPERM, pthread_mutex_unlock(&m));
	mtx_releaseHolder(th);

	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&m));
}


/*
 * Condition variables
 */


static struct {
	pthread_mutex_t m;
	pthread_cond_t c;
	volatile int turn;
	volatile unsigned int queued, taken, waiting, released, stop;
	unsigned int rounds;
	int failed;
} cv;


static void *cv_ponger(void *arg)
{
	unsigned int i;
	(void)arg;

	pthread_mutex_lock(&cv.m);
	for (i = 0; i < cv.rounds; i++) {
		if (mtx_condWaitFor(&cv.c, &cv.m, &cv.turn, 1) != 0) {
			cv.failed = 1;
			break;
		}
		cv.turn = 0;
		pthread_cond_signal(&cv.c);
	}
	pthread_mutex_unlock(&cv.m);

	return NULL;
}


static void *cv_consumer(void *arg)
{
	struct timespec dl;
	int err = 0;
	(void)arg;

	pthread_mutex_lock(&cv.m);
	for (;;) {
		mtx_deadline(&dl, CLOCK_MONOTONIC, HANG_MS);
		while ((cv.queued == 0u) && (cv.stop == 0u) && (err == 0)) {
			err = pthread_cond_timedwait(&cv.c, &cv.m, &dl);
		}
		if (cv.queued != 0u) {
			cv.queued--;
			cv.taken++;
			err = 0;
			continue;
		}
		if (err != 0) {
			cv.failed = 1;
		}
		break;
	}
	pthread_mutex_unlock(&cv.m);

	return NULL;
}


static void *cv_broadcastWaiter(void *arg)
{
	struct timespec dl;
	int err = 0;
	(void)arg;

	pthread_mutex_lock(&cv.m);
	cv.waiting++;
	pthread_cond_broadcast(&cv.c); /* tell the main thread */
	mtx_deadline(&dl, CLOCK_MONOTONIC, HANG_MS);
	while ((cv.stop == 0u) && (err == 0)) {
		err = pthread_cond_timedwait(&cv.c, &cv.m, &dl);
	}
	if (cv.stop == 0u) {
		cv.failed = 1;
	}
	cv.released++;
	pthread_mutex_unlock(&cv.m);

	return NULL;
}


TEST_GROUP(cond_fast);


TEST_SETUP(cond_fast)
{
	memset(&cv, 0, sizeof(cv));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&cv.m, NULL));
	mtx_initCond(&cv.c);
}


TEST_TEAR_DOWN(cond_fast)
{
	pthread_cond_destroy(&cv.c);
	pthread_mutex_destroy(&cv.m);
}


/* Strict alternation: any lost wake-up stops the exchange (and times out) */
static void cv_pingPong(void)
{
	pthread_t th;
	unsigned int i;

	cv.rounds = 5000;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, cv_ponger, NULL));

	pthread_mutex_lock(&cv.m);
	for (i = 0; (i < cv.rounds) && (cv.failed == 0); i++) {
		cv.turn = 1;
		pthread_cond_signal(&cv.c);
		if (mtx_condWaitFor(&cv.c, &cv.m, &cv.turn, 0) != 0) {
			cv.failed = 1;
		}
	}
	pthread_mutex_unlock(&cv.m);

	TEST_ASSERT_EQUAL_INT(0, pthread_join(th, NULL));
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, cv.failed, "a wake-up was lost");
	TEST_ASSERT_EQUAL_UINT(cv.rounds, i);
}


TEST(cond_fast, ping_pong)
{
	cv_pingPong();
}


/* The same over a kernel (priority-inheritance) mutex */
TEST(cond_fast, ping_pong_prio_inherit_mutex)
{
	pthread_mutex_destroy(&cv.m);
	mtx_initType(&cv.m, PTHREAD_MUTEX_NORMAL, PTHREAD_PRIO_INHERIT);
	cv_pingPong();
}


/* ...and over a recursive mutex locked once */
TEST(cond_fast, ping_pong_recursive_mutex)
{
	pthread_mutex_destroy(&cv.m);
	mtx_initType(&cv.m, PTHREAD_MUTEX_RECURSIVE, PTHREAD_PRIO_NONE);
	cv_pingPong();
}


/* 8 consumers, 20000 items announced one signal each */
TEST(cond_fast, producer_consumers)
{
	enum { NCONS = 8, NITEMS = 20000 };
	pthread_t th[NCONS];
	unsigned int i;

	for (i = 0; i < NCONS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&th[i], NULL, cv_consumer, NULL));
	}

	for (i = 0; i < NITEMS; i++) {
		pthread_mutex_lock(&cv.m);
		cv.queued++;
		pthread_cond_signal(&cv.c);
		pthread_mutex_unlock(&cv.m);
		if ((i % 64u) == 0u) {
			(void)sched_yield();
		}
	}

	pthread_mutex_lock(&cv.m);
	cv.stop = 1;
	pthread_cond_broadcast(&cv.c);
	pthread_mutex_unlock(&cv.m);

	for (i = 0; i < NCONS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(th[i], NULL));
	}

	/* A consumer leaves only once the queue is empty and `stop` is set */
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, cv.failed, "a consumer timed out: a wake-up was lost");
	TEST_ASSERT_EQUAL_UINT(NITEMS, cv.taken + cv.queued);
	TEST_ASSERT_EQUAL_UINT(0, cv.queued);
}


/* One broadcast must release every waiter, also when signalled without the mutex */
TEST(cond_fast, broadcast_many)
{
	enum { NW = 12 };
	pthread_t th[NW];
	struct timespec dl;
	unsigned int i;
	int err = 0;

	for (i = 0; i < NW; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&th[i], NULL, cv_broadcastWaiter, NULL));
	}

	pthread_mutex_lock(&cv.m);
	mtx_deadline(&dl, CLOCK_MONOTONIC, HANG_MS);
	while ((cv.waiting != NW) && (err == 0)) {
		err = pthread_cond_timedwait(&cv.c, &cv.m, &dl);
	}
	cv.stop = 1;
	pthread_mutex_unlock(&cv.m);
	TEST_ASSERT_EQUAL_INT(0, pthread_cond_broadcast(&cv.c));

	for (i = 0; i < NW; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(th[i], NULL));
	}
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, cv.failed, "a waiter missed the broadcast");
	TEST_ASSERT_EQUAL_UINT(NW, cv.released);
}


/* A timed wait times out with the mutex owned again */
TEST(cond_fast, timedwait_timeout_reowns_mutex)
{
	struct timespec dl;
	pthread_mutex_t m;
	uint64_t t0, dt;

	mtx_initType(&m, PTHREAD_MUTEX_ERRORCHECK, PTHREAD_PRIO_NONE);

	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&m));
	t0 = mtx_nowMs(CLOCK_MONOTONIC);
	mtx_deadline(&dl, CLOCK_MONOTONIC, 100);
	TEST_ASSERT_EQUAL_INT(ETIMEDOUT, pthread_cond_timedwait(&cv.c, &m, &dl));
	dt = mtx_nowMs(CLOCK_MONOTONIC) - t0;
	TEST_ASSERT_GREATER_OR_EQUAL_UINT32(90u, (uint32_t)dt);
	TEST_ASSERT_LESS_THAN_UINT32(HANG_MS, (uint32_t)dt);

	/* Owned: relocking is EDEADLK, unlocking succeeds once */
	TEST_ASSERT_EQUAL_INT(EDEADLK, pthread_mutex_lock(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&m));
	TEST_ASSERT_EQUAL_INT(EPERM, pthread_mutex_unlock(&m));

	/* A wait on a mutex we do not hold is refused, not slept in */
	TEST_ASSERT_EQUAL_INT(EPERM, pthread_cond_timedwait(&cv.c, &m, &dl));

	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&m));
}


/* A signal with nobody waiting is not remembered (no stray wake-up later) */
TEST(cond_fast, signal_without_waiters)
{
	struct timespec dl;

	TEST_ASSERT_EQUAL_INT(0, pthread_cond_signal(&cv.c));
	TEST_ASSERT_EQUAL_INT(0, pthread_cond_broadcast(&cv.c));

	pthread_mutex_lock(&cv.m);
	mtx_deadline(&dl, CLOCK_MONOTONIC, 50);
	TEST_ASSERT_EQUAL_INT(ETIMEDOUT, pthread_cond_timedwait(&cv.c, &cv.m, &dl));
	pthread_mutex_unlock(&cv.m);
}


/*
 * fork()
 */


#define FORK_CHK_OWN_NORMAL   (1 << 0)
#define FORK_CHK_OWN_TYPED    (1 << 1)
#define FORK_CHK_OTHER_LOCKED (1 << 2)
#define FORK_CHK_NEW_MUTEX    (1 << 3)
#define FORK_CHK_HEAP         (1 << 4)
#define FORK_CHK_COND         (1 << 5)


static struct {
	pthread_mutex_t mine, mineRec, mineErr, others;
	volatile int stop;
} fk;


static void *fk_allocator(void *arg)
{
	void *volatile p;
	(void)arg;

	while (fk.stop == 0) {
		p = malloc(48);
		free(p);
	}

	return NULL;
}


static void *fk_holdOthers(void *arg)
{
	(void)arg;

	pthread_mutex_lock(&fk.others);
	while (fk.stop == 0) {
		usleep(1000);
	}
	pthread_mutex_unlock(&fk.others);

	return NULL;
}


/* In the child; returns the FORK_CHK_* bits of the checks that failed */
static int fk_child(void)
{
	pthread_mutex_t m;
	pthread_cond_t c;
	pthread_condattr_t cattr;
	struct timespec dl;
	int fail = 0, i;
	void *p[64];

	/* What the forking thread held, the child's thread holds (POSIX) */
	if (pthread_mutex_unlock(&fk.mine) != 0) {
		fail |= FORK_CHK_OWN_NORMAL;
	}
#ifdef __phoenix__
	/* glibc keys typed mutexes by tid, which the child does not share */
	if ((pthread_mutex_unlock(&fk.mineRec) != 0) || (pthread_mutex_unlock(&fk.mineRec) != 0) ||
			(pthread_mutex_unlock(&fk.mineErr) != 0)) {
		fail |= FORK_CHK_OWN_TYPED;
	}
#endif

	/* ...and what another thread held stays locked */
	if (pthread_mutex_trylock(&fk.others) != EBUSY) {
		fail |= FORK_CHK_OTHER_LOCKED;
	}

	if ((pthread_mutex_init(&m, NULL) != 0) || (pthread_mutex_lock(&m) != 0) || (pthread_mutex_trylock(&m) != EBUSY) ||
			(pthread_mutex_unlock(&m) != 0) || (pthread_mutex_destroy(&m) != 0)) {
		fail |= FORK_CHK_NEW_MUTEX;
	}

	/* The allocator thread was inside malloc()/free() during the fork */
	for (i = 0; i < 64; i++) {
		p[i] = malloc(16u + (unsigned int)i * 24u);
		if (p[i] == NULL) {
			fail |= FORK_CHK_HEAP;
		}
	}
	for (i = 0; i < 64; i++) {
		free(p[i]);
	}

	if ((pthread_mutex_init(&m, NULL) != 0) || (pthread_condattr_init(&cattr) != 0) ||
			(pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC) != 0) || (pthread_cond_init(&c, &cattr) != 0)) {
		fail |= FORK_CHK_COND;
	}
	else {
		pthread_mutex_lock(&m);
		mtx_deadline(&dl, CLOCK_MONOTONIC, 10);
		if (pthread_cond_timedwait(&c, &m, &dl) != ETIMEDOUT) {
			fail |= FORK_CHK_COND;
		}
		pthread_mutex_unlock(&m);
	}

	return fail;
}


TEST_GROUP(mutex_fork);


TEST_SETUP(mutex_fork)
{
}


TEST_TEAR_DOWN(mutex_fork)
{
}


TEST(mutex_fork, child_lock_state)
{
	pthread_t alloc[2], holder;
	int status = -1, i;
	pid_t pid;

	memset(&fk, 0, sizeof(fk));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&fk.mine, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&fk.others, NULL));
	mtx_initType(&fk.mineRec, PTHREAD_MUTEX_RECURSIVE, PTHREAD_PRIO_NONE);
	mtx_initType(&fk.mineErr, PTHREAD_MUTEX_ERRORCHECK, PTHREAD_PRIO_NONE);

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&holder, NULL, fk_holdOthers, NULL));
	for (i = 0; i < 2; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&alloc[i], NULL, fk_allocator, NULL));
	}

	/* let the holder take its mutex */
	while (pthread_mutex_trylock(&fk.others) == 0) {
		pthread_mutex_unlock(&fk.others);
		usleep(1000);
	}

	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&fk.mine));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&fk.mineRec));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&fk.mineRec));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&fk.mineErr));

	/* Several forks: the allocators are caught at different points */
	for (i = 0; i < 8; i++) {
		pid = fork();
		if (pid == 0) {
			_exit(fk_child());
		}
		TEST_ASSERT_GREATER_THAN_INT(0, pid);
		TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
		TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(status), "the child died");
		/* Non-zero: the FORK_CHK_* bits of the checks that failed */
		TEST_ASSERT_EQUAL_HEX8(0, WEXITSTATUS(status));
	}

	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&fk.mineErr));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&fk.mineRec));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&fk.mineRec));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&fk.mine));

	fk.stop = 1;
	TEST_ASSERT_EQUAL_INT(0, pthread_join(holder, NULL));
	for (i = 0; i < 2; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(alloc[i], NULL));
	}

	pthread_mutex_destroy(&fk.mineErr);
	pthread_mutex_destroy(&fk.mineRec);
	pthread_mutex_destroy(&fk.others);
	pthread_mutex_destroy(&fk.mine);
}


/*
 * futexWait()/futexWake()
 */


#ifdef __phoenix__

static struct {
	volatile unsigned int word;
	volatile unsigned int sleeping, woken;
} fx;


static void *fx_sleeper(void *arg)
{
	int err;
	(void)arg;

	__atomic_add_fetch(&fx.sleeping, 1u, __ATOMIC_SEQ_CST);
	do {
		err = futexWait(&fx.word, 0u, 0, PH_CLOCK_RELATIVE);
	} while ((__atomic_load_n(&fx.word, __ATOMIC_SEQ_CST) == 0u) && ((err == EOK) || (err == -EINTR)));
	__atomic_add_fetch(&fx.woken, 1u, __ATOMIC_SEQ_CST);

	return NULL;
}

#endif


TEST_GROUP(futex);


TEST_SETUP(futex)
{
}


TEST_TEAR_DOWN(futex)
{
}


TEST(futex, value_mismatch_and_timeout)
{
#ifdef __phoenix__
	volatile unsigned int w = 5;
	uint64_t t0, dt;

	if ((futexWait == NULL) || (futexWake == NULL)) {
		TEST_IGNORE_MESSAGE("this libphoenix has no futexWait()/futexWake()");
	}

	/* The value differs: returns at once */
	TEST_ASSERT_EQUAL_INT(-EAGAIN, futexWait(&w, 4u, 0, PH_CLOCK_RELATIVE));

	/* Equal and nobody wakes: the relative timeout expires */
	t0 = mtx_nowMs(CLOCK_MONOTONIC);
	TEST_ASSERT_EQUAL_INT(-ETIME, futexWait(&w, 5u, 50000, PH_CLOCK_RELATIVE));
	dt = mtx_nowMs(CLOCK_MONOTONIC) - t0;
	TEST_ASSERT_GREATER_OR_EQUAL_UINT32(45u, (uint32_t)dt);
	TEST_ASSERT_LESS_THAN_UINT32(HANG_MS, (uint32_t)dt);

	/* Nobody asleep */
	TEST_ASSERT_EQUAL_INT(0, futexWake(&w, 1u));

	/* Misaligned */
	TEST_ASSERT_EQUAL_INT(-EINVAL, futexWait((volatile unsigned int *)((uintptr_t)&w + 1u), 5u, 0, PH_CLOCK_RELATIVE));
#else
	TEST_IGNORE_MESSAGE("Phoenix futexWait()/futexWake()");
#endif
}


TEST(futex, wake_one_then_all)
{
#ifdef __phoenix__
	enum { NS = 4 };
	pthread_t th[NS];
	int i, woken = 0, tries;

	if ((futexWait == NULL) || (futexWake == NULL)) {
		TEST_IGNORE_MESSAGE("this libphoenix has no futexWait()/futexWake()");
	}

	memset((void *)&fx, 0, sizeof(fx));
	for (i = 0; i < NS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&th[i], NULL, fx_sleeper, NULL));
	}

	/* futexWake() returns how many it woke: poll until all four are asleep */
	for (tries = 0; (tries < 1000) && (__atomic_load_n(&fx.sleeping, __ATOMIC_SEQ_CST) != NS); tries++) {
		usleep(1000);
	}
	usleep(20000);

	/* One wake-up wakes one sleeper -- which goes back to sleep: the word is 0 */
	TEST_ASSERT_EQUAL_INT(1, futexWake(&fx.word, 1u));
	usleep(20000);
	TEST_ASSERT_EQUAL_UINT(0, fx.woken);

	__atomic_store_n(&fx.word, 1u, __ATOMIC_SEQ_CST);
	for (tries = 0; (tries < 100) && (__atomic_load_n(&fx.woken, __ATOMIC_SEQ_CST) != NS); tries++) {
		woken += futexWake(&fx.word, ~0u);
		usleep(1000);
	}
	TEST_ASSERT_LESS_OR_EQUAL_INT(NS, woken);

	for (i = 0; i < NS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(th[i], NULL));
	}
	TEST_ASSERT_EQUAL_UINT(NS, fx.woken);
#else
	TEST_IGNORE_MESSAGE("Phoenix futexWait()/futexWake()");
#endif
}


TEST_GROUP_RUNNER(mutex_fast)
{
	RUN_TEST_CASE(mutex_fast, contention_normal);
	RUN_TEST_CASE(mutex_fast, contention_static_initializer);
	RUN_TEST_CASE(mutex_fast, contention_recursive);
	RUN_TEST_CASE(mutex_fast, contention_errorcheck);
	RUN_TEST_CASE(mutex_fast, contention_trylock);
	RUN_TEST_CASE(mutex_fast, contention_prio_inherit);
}


/* The kernel's own NORMAL mutexes (what libphoenix uses internally): relocking
 * one you hold slept forever, or, with a timeout, returned EOK after the
 * timeout without the caller gaining anything (KNOWN-ISSUES C11). Both must
 * fail with EDEADLK. The timed call runs first: on the old kernel it returns
 * after 200 ms instead of hanging the suite. */
TEST(mutex_semantics, kernel_mutex_self_relock_edeadlk)
{
#ifdef __phoenix__
	handle_t h;

	TEST_ASSERT_EQUAL_INT(0, mutexCreate(&h));
	TEST_ASSERT_EQUAL_INT(0, mutexLock(h));
	TEST_ASSERT_EQUAL_INT(-EDEADLK, mutexLockClockWait(h, 200000, PH_CLOCK_RELATIVE));
	TEST_ASSERT_EQUAL_INT(-EDEADLK, mutexLock(h));
	/* still held exactly once: one unlock frees it for another locker */
	TEST_ASSERT_EQUAL_INT(0, mutexUnlock(h));
	TEST_ASSERT_EQUAL_INT(0, mutexTry(h));
	TEST_ASSERT_EQUAL_INT(0, mutexUnlock(h));
	TEST_ASSERT_EQUAL_INT(0, resourceDestroy(h));
#else
	TEST_IGNORE_MESSAGE("Phoenix kernel mutexes only");
#endif
}


TEST_GROUP_RUNNER(mutex_semantics)
{
	RUN_TEST_CASE(mutex_semantics, default_protocol_is_prio_none);
	RUN_TEST_CASE(mutex_semantics, trylock);
	RUN_TEST_CASE(mutex_semantics, timedlock);
	RUN_TEST_CASE(mutex_semantics, timedlock_recursive_held_elsewhere);
	RUN_TEST_CASE(mutex_semantics, recursive_depth);
	RUN_TEST_CASE(mutex_semantics, errorcheck);
	RUN_TEST_CASE(mutex_semantics, kernel_mutex_self_relock_edeadlk);
}


TEST_GROUP_RUNNER(cond_fast)
{
	RUN_TEST_CASE(cond_fast, ping_pong);
	RUN_TEST_CASE(cond_fast, ping_pong_prio_inherit_mutex);
	RUN_TEST_CASE(cond_fast, ping_pong_recursive_mutex);
	RUN_TEST_CASE(cond_fast, producer_consumers);
	RUN_TEST_CASE(cond_fast, broadcast_many);
	RUN_TEST_CASE(cond_fast, timedwait_timeout_reowns_mutex);
	RUN_TEST_CASE(cond_fast, signal_without_waiters);
}


TEST_GROUP_RUNNER(mutex_fork)
{
	RUN_TEST_CASE(mutex_fork, child_lock_state);
}


TEST_GROUP_RUNNER(futex)
{
	RUN_TEST_CASE(futex, value_mismatch_and_timeout);
	RUN_TEST_CASE(futex, wake_one_then_all);
}
