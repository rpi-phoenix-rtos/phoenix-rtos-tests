/*
 * Phoenix-RTOS
 *
 * phoenix-rtos-tests
 *
 * test/libc/pthread
 *
 * Copyright 2022 Phoenix Systems
 * Author: Lukasz Leczkowski, Damian Loewnau
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <pthread.h>
#include <stdio.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#include "unity_fixture.h"
#include "pthread_cond_test_functions.h"


static void test_cleanupHandler1(void *arg)
{
	int *val = (int *)arg;
	(*val) *= 2;
}


static void test_cleanupHandler2(void *arg)
{
	int *val = (int *)arg;
	(*val) *= 3;
}

/* NOTE: Do not remove matching push/pop calls - even if they are not executed:
 * POSIX permits pthread_cleanup_push/pop() to be implemented as macros that expandto text containing '{' and '}',
 * respectively. For this reason, the caller must ensure that calls to these functions are paired within the same function,
 * and at the same lexical nesting level.
 */


static void *test_threadCleanup1(void *arg)
{
	int *val = (int *)arg;
	pthread_cleanup_push(test_cleanupHandler1, val);
	pthread_cleanup_push(test_cleanupHandler2, val);

	pthread_exit(NULL);

	pthread_cleanup_pop(0);
	pthread_cleanup_pop(0);

	return NULL;
}


static void *test_threadCleanup2(void *arg)
{
	int *val = (int *)arg;
	pthread_cleanup_push(test_cleanupHandler1, val);
	pthread_cleanup_push(test_cleanupHandler2, val);

	pthread_cleanup_pop(0);
	pthread_cleanup_pop(0);

	pthread_exit(NULL);

	return NULL;
}


static void *test_threadCleanup3(void *arg)
{
	int *val = (int *)arg;
	pthread_cleanup_push(test_cleanupHandler1, &val[0]);
	pthread_cleanup_push(test_cleanupHandler2, &val[0]);

	pthread_cleanup_pop(1);
	val[1] = val[0];
	pthread_cleanup_pop(1);

	pthread_exit(NULL);

	return NULL;
}


static void *test_threadCleanup4(void *arg)
{
	int *val = (int *)arg;
	pthread_cleanup_push(test_cleanupHandler1, &val[0]);
	pthread_cleanup_push(test_cleanupHandler2, &val[0]);

	pthread_cleanup_pop(1);
	val[1] = val[0];

	pthread_exit(NULL);

	pthread_cleanup_pop(0);

	return NULL;
}


TEST_GROUP(test_pthread_cond);
TEST_GROUP(test_pthread_cleanup);


TEST_SETUP(test_pthread_cond)
{
}


TEST_TEAR_DOWN(test_pthread_cond)
{
}


TEST(test_pthread_cond, pthread_condattr_setclock)
{
	pthread_condattr_t attr;
	TEST_ASSERT_EQUAL(0, pthread_condattr_init(&attr));

	clockid_t clock;

	TEST_ASSERT_EQUAL(0, pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
	TEST_ASSERT_EQUAL(0, pthread_condattr_getclock(&attr, &clock));
	TEST_ASSERT_EQUAL(CLOCK_MONOTONIC, clock);

	/* glibc don't want to use CLOCK_MONOTONIC_RAW */
#ifdef __phoenix__
	TEST_ASSERT_EQUAL(0, pthread_condattr_setclock(&attr, CLOCK_MONOTONIC_RAW));
	TEST_ASSERT_EQUAL(0, pthread_condattr_getclock(&attr, &clock));
	TEST_ASSERT_EQUAL(CLOCK_MONOTONIC_RAW, clock);
#endif

	TEST_ASSERT_EQUAL(0, pthread_condattr_setclock(&attr, CLOCK_REALTIME));
	TEST_ASSERT_EQUAL(0, pthread_condattr_getclock(&attr, &clock));
	TEST_ASSERT_EQUAL(CLOCK_REALTIME, clock);
}


TEST(test_pthread_cond, pthread_condattr_setpshared)
{
	pthread_condattr_t attr;
	TEST_ASSERT_EQUAL(0, pthread_condattr_init(&attr));
	TEST_ASSERT_EQUAL(0, pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE));

	int pshared;
	/* Only 'PTHREAD_PROCESS_PRIVATE' supported on Phoenix-RTOS */
	TEST_ASSERT_EQUAL(0, pthread_condattr_getpshared(&attr, &pshared));
	TEST_ASSERT_EQUAL(PTHREAD_PROCESS_PRIVATE, pshared);
#ifdef __phoenix__
	TEST_ASSERT_EQUAL(EINVAL, pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED));
#else
	TEST_ASSERT_EQUAL(0, pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED));
	TEST_ASSERT_EQUAL(0, pthread_condattr_getpshared(&attr, &pshared));
	TEST_ASSERT_EQUAL(PTHREAD_PROCESS_SHARED, pshared);
#endif
}


TEST(test_pthread_cond, pthread_cond_init)
{
	pthread_cond_t cond;
	TEST_ASSERT_EQUAL(0, pthread_cond_init(&cond, NULL));
}


TEST(test_pthread_cond, pthread_cond_wait_signal)
{
	pthread_t first, second;
	thread_args.count = 0;
	thread_err_t err_first, err_second;

	TEST_ASSERT_EQUAL(0, pthread_mutex_init(&thread_args.count_lock, NULL));
	TEST_ASSERT_EQUAL(0, pthread_cond_init(&thread_args.count_nonzero, NULL));
	TEST_ASSERT_EQUAL(0, pthread_create(&first, NULL, decrement_count_wait, &err_first));
	TEST_ASSERT_EQUAL(0, pthread_create(&second, NULL, increment_count_signal, &err_second));
	TEST_ASSERT_EQUAL(0, pthread_join(first, NULL));
	TEST_ASSERT_EQUAL(0, pthread_join(second, NULL));

	TEST_ASSERT_EQUAL(0, err_first.err1);
	TEST_ASSERT_EQUAL(0, err_first.err2);
	TEST_ASSERT_EQUAL(0, err_first.err3);
	TEST_ASSERT_EQUAL(0, err_second.err1);
	TEST_ASSERT_EQUAL(0, err_second.err2);
	TEST_ASSERT_EQUAL(0, err_second.err3);
}


TEST(test_pthread_cond, pthread_cond_wait_broadcast)
{
	pthread_t first, second, third;
	thread_args.count = 0;
	thread_err_t err_first, err_second, err_third;

	TEST_ASSERT_EQUAL(0, pthread_mutex_init(&thread_args.count_lock, NULL));
	TEST_ASSERT_EQUAL(0, pthread_cond_init(&thread_args.count_nonzero, NULL));
	TEST_ASSERT_EQUAL(0, pthread_create(&first, NULL, decrement_count_wait, &err_first));
	TEST_ASSERT_EQUAL(0, pthread_create(&second, NULL, decrement_count_wait, &err_second));
	TEST_ASSERT_EQUAL(0, pthread_create(&third, NULL, increment_count_broadcast, &err_third));
	TEST_ASSERT_EQUAL(0, pthread_join(first, NULL));
	TEST_ASSERT_EQUAL(0, pthread_join(second, NULL));
	TEST_ASSERT_EQUAL(0, pthread_join(third, NULL));

	TEST_ASSERT_EQUAL(0, err_first.err1);
	TEST_ASSERT_EQUAL(0, err_first.err2);
	TEST_ASSERT_EQUAL(0, err_first.err3);
	TEST_ASSERT_EQUAL(0, err_second.err1);
	TEST_ASSERT_EQUAL(0, err_second.err2);
	TEST_ASSERT_EQUAL(0, err_second.err3);
	TEST_ASSERT_EQUAL(0, err_third.err1);
	TEST_ASSERT_EQUAL(0, err_third.err2);
	TEST_ASSERT_EQUAL(0, err_third.err3);
}


TEST(test_pthread_cond, pthread_cond_timedwait_pass_signal)
{
	pthread_t first, second;
	thread_args.count = 0;
	thread_err_t err_first, err_second;

	TEST_ASSERT_EQUAL(0, pthread_mutex_init(&thread_args.count_lock, NULL));
	TEST_ASSERT_EQUAL(0, pthread_cond_init(&thread_args.count_nonzero, NULL));
	TEST_ASSERT_EQUAL(0, pthread_create(&first, NULL, decrement_count_timed_wait_pass, &err_first));
	TEST_ASSERT_EQUAL(0, pthread_create(&second, NULL, increment_count_signal, &err_second));
	TEST_ASSERT_EQUAL(0, pthread_join(first, NULL));
	TEST_ASSERT_EQUAL(0, pthread_join(second, NULL));

	TEST_ASSERT_EQUAL(0, err_first.err1);
	TEST_ASSERT_EQUAL(0, err_first.err2);
	TEST_ASSERT_EQUAL(0, err_first.err3);
	TEST_ASSERT_EQUAL(0, err_second.err1);
	TEST_ASSERT_EQUAL(0, err_second.err2);
	TEST_ASSERT_EQUAL(0, err_second.err3);
}


TEST(test_pthread_cond, pthread_cond_timedwait_fail_signal_incorrect_timeout)
{
	pthread_t first, second;
	thread_args.count = 0;
	thread_err_t err_first, err_second;

	TEST_ASSERT_EQUAL(0, pthread_mutex_init(&thread_args.count_lock, NULL));
	TEST_ASSERT_EQUAL(0, pthread_cond_init(&thread_args.count_nonzero, NULL));
	TEST_ASSERT_EQUAL(0, pthread_create(&first, NULL, decrement_count_timed_wait_fail_incorrect_timeout, &err_first));
	TEST_ASSERT_EQUAL(0, pthread_create(&second, NULL, increment_count_signal, &err_second));
	TEST_ASSERT_EQUAL(0, pthread_join(first, NULL));
	TEST_ASSERT_EQUAL(0, pthread_join(second, NULL));

	TEST_ASSERT_EQUAL(0, err_first.err1);
	TEST_ASSERT_EQUAL(ETIMEDOUT, err_first.err2);
	TEST_ASSERT_EQUAL(0, err_first.err3);
	TEST_ASSERT_EQUAL(0, err_second.err1);
	TEST_ASSERT_EQUAL(0, err_second.err2);
	TEST_ASSERT_EQUAL(0, err_second.err3);
}


TEST(test_pthread_cond, pthread_cond_timedwait_pass_broadcast)
{
	pthread_t first, second, third;
	thread_args.count = 0;
	thread_err_t err_first, err_second, err_third;

	TEST_ASSERT_EQUAL(0, pthread_mutex_init(&thread_args.count_lock, NULL));
	TEST_ASSERT_EQUAL(0, pthread_cond_init(&thread_args.count_nonzero, NULL));
	TEST_ASSERT_EQUAL(0, pthread_create(&first, NULL, decrement_count_timed_wait_pass, &err_first));
	TEST_ASSERT_EQUAL(0, pthread_create(&second, NULL, decrement_count_timed_wait_pass, &err_second));
	TEST_ASSERT_EQUAL(0, pthread_create(&third, NULL, increment_count_broadcast, &err_third));
	TEST_ASSERT_EQUAL(0, pthread_join(first, NULL));
	TEST_ASSERT_EQUAL(0, pthread_join(second, NULL));
	TEST_ASSERT_EQUAL(0, pthread_join(third, NULL));

	TEST_ASSERT_EQUAL(0, err_first.err1);
	TEST_ASSERT_EQUAL(0, err_first.err2);
	TEST_ASSERT_EQUAL(0, err_first.err3);
	TEST_ASSERT_EQUAL(0, err_second.err1);
	TEST_ASSERT_EQUAL(0, err_second.err2);
	TEST_ASSERT_EQUAL(0, err_second.err3);
	TEST_ASSERT_EQUAL(0, err_third.err1);
	TEST_ASSERT_EQUAL(0, err_third.err2);
	TEST_ASSERT_EQUAL(0, err_third.err3);
}


TEST(test_pthread_cond, pthread_cond_timedwait_fail_broadcast_incorrect_timeout)
{
	pthread_t first, second, third;
	thread_args.count = 0;
	thread_err_t err_first, err_second, err_third;

	TEST_ASSERT_EQUAL(0, pthread_mutex_init(&thread_args.count_lock, NULL));
	TEST_ASSERT_EQUAL(0, pthread_cond_init(&thread_args.count_nonzero, NULL));
	TEST_ASSERT_EQUAL(0, pthread_create(&first, NULL, decrement_count_timed_wait_fail_incorrect_timeout, &err_first));
	TEST_ASSERT_EQUAL(0, pthread_create(&second, NULL, decrement_count_timed_wait_fail_incorrect_timeout, &err_second));
	TEST_ASSERT_EQUAL(0, pthread_create(&third, NULL, increment_count_broadcast, &err_third));
	TEST_ASSERT_EQUAL(0, pthread_join(first, NULL));
	TEST_ASSERT_EQUAL(0, pthread_join(second, NULL));
	TEST_ASSERT_EQUAL(0, pthread_join(third, NULL));

	TEST_ASSERT_EQUAL(0, err_first.err1);
	TEST_ASSERT_EQUAL(ETIMEDOUT, err_first.err2);
	TEST_ASSERT_EQUAL(0, err_first.err3);
	TEST_ASSERT_EQUAL(0, err_second.err1);
	TEST_ASSERT_EQUAL(ETIMEDOUT, err_second.err2);
	TEST_ASSERT_EQUAL(0, err_second.err3);
	TEST_ASSERT_EQUAL(0, err_third.err1);
	TEST_ASSERT_EQUAL(0, err_third.err2);
	TEST_ASSERT_EQUAL(0, err_third.err3);
}


TEST_SETUP(test_pthread_cleanup)
{
}


TEST_TEAR_DOWN(test_pthread_cleanup)
{
}


TEST(test_pthread_cleanup, pthread_cleanup_push_exit)
{
	pthread_t thread;
	int val1 = 42;

	TEST_ASSERT_EQUAL(0, pthread_create(&thread, NULL, test_threadCleanup1, &val1));
	TEST_ASSERT_EQUAL(0, pthread_join(thread, NULL));

	TEST_ASSERT_EQUAL(42 * 3 * 2, val1);
}


TEST(test_pthread_cleanup, pthread_cleanup_push_pop_no_exec)
{
	pthread_t thread;
	int val1 = 42;

	TEST_ASSERT_EQUAL(0, pthread_create(&thread, NULL, test_threadCleanup2, &val1));
	TEST_ASSERT_EQUAL(0, pthread_join(thread, NULL));

	TEST_ASSERT_EQUAL(42, val1);
}


TEST(test_pthread_cleanup, pthread_cleanup_push_pop_exec)
{
	pthread_t thread;
	int vals[2] = { 42, 0 };

	TEST_ASSERT_EQUAL(0, pthread_create(&thread, NULL, test_threadCleanup3, &vals));
	TEST_ASSERT_EQUAL(0, pthread_join(thread, NULL));

	TEST_ASSERT_EQUAL(42 * 3 * 2, vals[0]);
	TEST_ASSERT_EQUAL(42 * 3, vals[1]);
}


TEST(test_pthread_cleanup, pthread_cleanup_push_pop_exec_pthread_exit)
{
	pthread_t thread;
	int vals[2] = { 42, 0 };

	TEST_ASSERT_EQUAL(0, pthread_create(&thread, NULL, test_threadCleanup4, &vals));
	TEST_ASSERT_EQUAL(0, pthread_join(thread, NULL));

	TEST_ASSERT_EQUAL(42 * 3 * 2, vals[0]);
	TEST_ASSERT_EQUAL(42 * 3, vals[1]);
}


TEST_GROUP_RUNNER(test_pthread_cond)
{
	RUN_TEST_CASE(test_pthread_cond, pthread_cond_init);
	RUN_TEST_CASE(test_pthread_cond, pthread_condattr_setclock);
	RUN_TEST_CASE(test_pthread_cond, pthread_condattr_setpshared);
	RUN_TEST_CASE(test_pthread_cond, pthread_cond_wait_signal);
	RUN_TEST_CASE(test_pthread_cond, pthread_cond_wait_broadcast);
	RUN_TEST_CASE(test_pthread_cond, pthread_cond_timedwait_pass_signal);
	RUN_TEST_CASE(test_pthread_cond, pthread_cond_timedwait_fail_signal_incorrect_timeout);
	RUN_TEST_CASE(test_pthread_cond, pthread_cond_timedwait_pass_broadcast);
	RUN_TEST_CASE(test_pthread_cond, pthread_cond_timedwait_fail_broadcast_incorrect_timeout);
}


TEST_GROUP_RUNNER(test_pthread_cleanup)
{
	RUN_TEST_CASE(test_pthread_cleanup, pthread_cleanup_push_exit);
	RUN_TEST_CASE(test_pthread_cleanup, pthread_cleanup_push_pop_no_exec);
	RUN_TEST_CASE(test_pthread_cleanup, pthread_cleanup_push_pop_exec);
	RUN_TEST_CASE(test_pthread_cleanup, pthread_cleanup_push_pop_exec_pthread_exit);
}


TEST_GROUP(test_pthread_detach);


TEST_SETUP(test_pthread_detach)
{
}


TEST_TEAR_DOWN(test_pthread_detach)
{
}


static void *test_detach_worker(void *arg)
{
	(void)arg;
	return NULL;
}


TEST(test_pthread_detach, detach_stale_handle_no_uaf)
{
	pthread_t thread;
	int rc;

	/* A detached thread frees its own control block when it exits. Re-detaching
	 * the now-stale handle must be rejected with an error, NOT crash with a
	 * use-after-free (regression: libphoenix pthread_detach dereferenced the
	 * freed ctx directly; it now validates the handle against the live list). */
	TEST_ASSERT_EQUAL(0, pthread_create(&thread, NULL, test_detach_worker, NULL));
	TEST_ASSERT_EQUAL(0, pthread_detach(thread));

	/* Give the detached worker time to terminate and self-free its ctx. */
	usleep(100000);

	rc = pthread_detach(thread);
	TEST_ASSERT_NOT_EQUAL(0, rc);
}


TEST(test_pthread_detach, detach_null_handle)
{
#ifdef __phoenix__
	/* Phoenix rejects a NULL handle with ESRCH (glibc: UB, may fault). */
	TEST_ASSERT_EQUAL(ESRCH, pthread_detach((pthread_t)0));
#else
	TEST_IGNORE_MESSAGE("pthread_detach(NULL) is undefined behaviour on glibc");
#endif
}


static void *test_detach_touch(void *arg)
{
	/* Touch most of the stack so a leaked (unreclaimed) stack holds physical RAM,
	 * not just address space. 64 KiB fits well inside the 256 KiB churn stack. */
	volatile char buf[64 * 1024];
	size_t i;
	(void)arg;
	for (i = 0; i < sizeof(buf); i += 4096) {
		buf[i] = (char)i;
	}
	return NULL;
}


TEST(test_pthread_detach, detached_burst_stack_reclaim)
{
	/* Regression: a detached thread with a libphoenix-mmap'd stack crashed on
	 * EXIT under bursty exits -- the old deferred cross-thread stack free munmap'd
	 * a still-live stack (Data Abort EL0 in the munmap epilogue, far==sp) on SMP.
	 * (a) bursts of concurrent detached exits reproduce the crash; (b) sustained
	 * one-at-a-time churn with a touched stack verifies the stacks are reclaimed
	 * (not leaked) -- a leak would exhaust address space/RAM and fail create. */
	pthread_attr_t attr;
	int round, i;

	for (round = 0; round < 4; round++) {
		for (i = 0; i < 8; i++) {
			pthread_t t;
			TEST_ASSERT_EQUAL(0, pthread_attr_init(&attr));
			TEST_ASSERT_EQUAL(0, pthread_attr_setstacksize(&attr, 64 * 1024));
			TEST_ASSERT_EQUAL(0, pthread_create(&t, &attr, test_detach_worker, NULL));
			TEST_ASSERT_EQUAL(0, pthread_attr_destroy(&attr));
			TEST_ASSERT_EQUAL(0, pthread_detach(t));
		}
		usleep(30000);
	}

	for (i = 0; i < 200; i++) {
		pthread_t t;
		TEST_ASSERT_EQUAL(0, pthread_attr_init(&attr));
		TEST_ASSERT_EQUAL(0, pthread_attr_setstacksize(&attr, 256 * 1024));
		TEST_ASSERT_EQUAL(0, pthread_create(&t, &attr, test_detach_touch, NULL));
		TEST_ASSERT_EQUAL(0, pthread_attr_destroy(&attr));
		TEST_ASSERT_EQUAL(0, pthread_detach(t));
		usleep(2000);
	}
}


TEST_GROUP_RUNNER(test_pthread_detach)
{
	RUN_TEST_CASE(test_pthread_detach, detach_stale_handle_no_uaf);
	RUN_TEST_CASE(test_pthread_detach, detach_null_handle);
	RUN_TEST_CASE(test_pthread_detach, detached_burst_stack_reclaim);
}


/*
 * New POSIX threading features carried by the 2026-08 upstream merge:
 * pthread spinlocks (kernel-less atomic locks) and mutex attributes wired to
 * the kernel's recursive/errorcheck/robust/priority-ceiling lock support
 * (mutexCreateWithAttr). Coverage was previously absent.
 */

TEST_GROUP(test_pthread_newlocks);


TEST_SETUP(test_pthread_newlocks)
{
}


TEST_TEAR_DOWN(test_pthread_newlocks)
{
}


#define NEWLOCKS_STACKSZ (64 * 1024) /* the 1-page pthread default is too small */

static pthread_spinlock_t g_newlocks_spin;
static unsigned long g_newlocks_counter;
static pthread_mutex_t g_newlocks_robust;


static int spawn_with_stack(pthread_t *t, void *(*fn)(void *), void *arg)
{
	pthread_attr_t attr;
	int err = pthread_attr_init(&attr);
	if (err != 0) {
		return err;
	}
	(void)pthread_attr_setstacksize(&attr, NEWLOCKS_STACKSZ);
	err = pthread_create(t, &attr, fn, arg);
	pthread_attr_destroy(&attr);
	return err;
}


TEST(test_pthread_newlocks, spin_lock_trylock_unlock)
{
	pthread_spinlock_t lock;

	TEST_ASSERT_EQUAL_INT(0, pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE));
	TEST_ASSERT_EQUAL_INT(0, pthread_spin_lock(&lock));
	/* trylock on a held spinlock must fail with EBUSY, not block. */
	TEST_ASSERT_EQUAL_INT(EBUSY, pthread_spin_trylock(&lock));
	TEST_ASSERT_EQUAL_INT(0, pthread_spin_unlock(&lock));
	/* now free: trylock succeeds. */
	TEST_ASSERT_EQUAL_INT(0, pthread_spin_trylock(&lock));
	TEST_ASSERT_EQUAL_INT(0, pthread_spin_unlock(&lock));
	TEST_ASSERT_EQUAL_INT(0, pthread_spin_destroy(&lock));
}


static void *spin_incrementer(void *arg)
{
	unsigned int iters = *(unsigned int *)arg;
	unsigned int i;

	for (i = 0; i < iters; ++i) {
		pthread_spin_lock(&g_newlocks_spin);
		/* Non-atomic read-modify-write: a working spinlock serializes it, so
		 * no update is lost; a broken lock would drop increments under
		 * contention from the other threads. */
		unsigned long v = g_newlocks_counter;
		g_newlocks_counter = v + 1;
		pthread_spin_unlock(&g_newlocks_spin);
	}
	return NULL;
}


TEST(test_pthread_newlocks, spin_mutual_exclusion)
{
	enum { NTH = 4, ITERS = 4000 };
	pthread_t th[NTH];
	unsigned int iters = ITERS;
	int i;

	g_newlocks_counter = 0;
	TEST_ASSERT_EQUAL_INT(0, pthread_spin_init(&g_newlocks_spin, PTHREAD_PROCESS_PRIVATE));

	for (i = 0; i < NTH; ++i) {
		TEST_ASSERT_EQUAL_INT(0, spawn_with_stack(&th[i], spin_incrementer, &iters));
	}
	for (i = 0; i < NTH; ++i) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(th[i], NULL));
	}

	TEST_ASSERT_EQUAL_UINT(NTH * ITERS, g_newlocks_counter);
	TEST_ASSERT_EQUAL_INT(0, pthread_spin_destroy(&g_newlocks_spin));
}


TEST(test_pthread_newlocks, mutex_recursive)
{
	pthread_mutexattr_t attr;
	pthread_mutex_t m;

	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&m, &attr));

	/* The owner may relock a recursive mutex; it must unlock the same number
	 * of times. A non-recursive mutex would deadlock on the second lock. */
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&m));

	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_destroy(&attr));
}


TEST(test_pthread_newlocks, mutex_errorcheck_relock_edeadlk)
{
	pthread_mutexattr_t attr;
	pthread_mutex_t m;

	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&m, &attr));

	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&m));
	/* Relocking by the owner returns EDEADLK instead of self-deadlocking. */
	TEST_ASSERT_EQUAL_INT(EDEADLK, pthread_mutex_lock(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&m));

	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&m));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_destroy(&attr));
}


TEST(test_pthread_newlocks, mutexattr_roundtrip)
{
	pthread_mutexattr_t attr;
	int v;

	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_init(&attr));

	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
	v = -1;
	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_gettype(&attr, &v));
	TEST_ASSERT_EQUAL_INT(PTHREAD_MUTEX_RECURSIVE, v);

	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_PROTECT));
	v = -1;
	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_getprotocol(&attr, &v));
	TEST_ASSERT_EQUAL_INT(PTHREAD_PRIO_PROTECT, v);

	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST));
	v = -1;
	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_getrobust(&attr, &v));
	TEST_ASSERT_EQUAL_INT(PTHREAD_MUTEX_ROBUST, v);

	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_destroy(&attr));
}


static void *robust_killer(void *arg)
{
	(void)arg;
	/* Lock the robust mutex and terminate while still holding it. The kernel
	 * force-unlocks a dying thread's held locks and marks a robust one
	 * inconsistent, so the next locker inherits it with EOWNERDEAD. */
	pthread_mutex_lock(&g_newlocks_robust);
	return NULL;
}


TEST(test_pthread_newlocks, mutex_robust_owner_death)
{
	pthread_mutexattr_t attr;
	pthread_t th;

	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&g_newlocks_robust, &attr));

	/* A worker locks the robust mutex, then dies without unlocking. */
	TEST_ASSERT_EQUAL_INT(0, spawn_with_stack(&th, robust_killer, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(th, NULL));

	/* The next acquirer inherits the abandoned lock with EOWNERDEAD, must make
	 * it consistent, then owns and releases it. */
	TEST_ASSERT_EQUAL_INT(EOWNERDEAD, pthread_mutex_lock(&g_newlocks_robust));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_consistent(&g_newlocks_robust));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&g_newlocks_robust));

	/* Recovered: a fresh lock/unlock cycle now succeeds cleanly. */
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&g_newlocks_robust));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&g_newlocks_robust));

	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&g_newlocks_robust));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutexattr_destroy(&attr));
}


TEST_GROUP_RUNNER(test_pthread_newlocks)
{
	RUN_TEST_CASE(test_pthread_newlocks, spin_lock_trylock_unlock);
	RUN_TEST_CASE(test_pthread_newlocks, spin_mutual_exclusion);
	RUN_TEST_CASE(test_pthread_newlocks, mutex_recursive);
	RUN_TEST_CASE(test_pthread_newlocks, mutex_errorcheck_relock_edeadlk);
	RUN_TEST_CASE(test_pthread_newlocks, mutexattr_roundtrip);
	RUN_TEST_CASE(test_pthread_newlocks, mutex_robust_owner_death);
}


/* An fd sweep racing an open().
 *
 * posix_open has to publish p->fds[fd].file BEFORE the file is filled in,
 * because the slot is how the descriptor is reserved across the blocking IPCs
 * the open performs. A half-built open_file_t is therefore reachable from every
 * other thread of the process, and any thread that walks the fd space closing
 * descriptors -- a close-all sweep like the one below, or the exit-time sweep --
 * will find it. That close used to decrement an uninitialised refs: measured on
 * a Pi4, 869 of 869 raced closes read refs == 0, each one clearing the slot and
 * leaking the file, and had the recycled value been 1 it would have freed the
 * file while open() was still writing into it.
 *
 * The kernel now takes a construction reference for the duration of the open,
 * so the racing close can drop the slot's reference without ever reaching zero.
 *
 * This cannot assert on the race itself; it asserts that the two threads run to
 * completion with the descriptor table still coherent afterwards. A regression
 * shows up as a fault, a hang, or a descriptor table that no longer works.
 */
#define FDRACE_PATH   "pthread_fdrace.txt"
#define FDRACE_ROUNDS 2000
#define FDRACE_MAXFD  32

TEST_GROUP(test_pthread_fdrace);

TEST_SETUP(test_pthread_fdrace)
{
}

TEST_TEAR_DOWN(test_pthread_fdrace)
{
	/* Here rather than at the end of the test body: a failing assertion leaves
	 * the body early, and a leftover file in the (NFS) root changes which
	 * branch a later create takes -- a footgun that has cost this project time
	 * before. */
	(void)unlink(FDRACE_PATH);
}

static volatile int fdrace_stop;
static long fdrace_opened;
static long fdrace_raced;   /* opens the sweeper took the descriptor from */
static long fdrace_othererr;

static void *fdrace_opener(void *arg)
{
	long i;

	(void)arg;
	for (i = 0; i < FDRACE_ROUNDS; ++i) {
		int fd = open(FDRACE_PATH, O_CREAT | O_RDWR, 0666);
		if (fd >= 0) {
			fdrace_opened++;
			(void)close(fd);
		}
		else if (errno == EBADF) {
			/* The sweeper closed the descriptor while open() was still
			 * building the file, so the open has no descriptor to return.
			 * Expected under this workload -- open() does several blocking
			 * IPCs, so the window is far wider than a sweep iteration -- and
			 * far better than being handed a number that now refers to a
			 * different file. */
			fdrace_raced++;
		}
		else {
			fdrace_othererr++;
		}
	}
	fdrace_stop = 1;
	return NULL;
}


static void *fdrace_sweeper(void *arg)
{
	(void)arg;
	while (fdrace_stop == 0) {
		int fd;
		/* from 3, so the harness keeps stdin/stdout/stderr */
		for (fd = 3; fd < FDRACE_MAXFD; ++fd) {
			(void)close(fd);
		}
	}
	return NULL;
}


TEST(test_pthread_fdrace, close_sweep_races_open)
{
	pthread_t opener, sweeper;
	int fd;

	fdrace_stop = 0;
	fdrace_opened = 0;
	fdrace_raced = 0;
	fdrace_othererr = 0;

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&opener, NULL, fdrace_opener, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&sweeper, NULL, fdrace_sweeper, NULL));

	TEST_ASSERT_EQUAL_INT(0, pthread_join(opener, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(sweeper, NULL));

	/* Every open must have either succeeded or lost its descriptor to the
	 * sweeper -- nothing else. Which of the two dominates depends on timing
	 * (with the root on NFS the open window is wide and the sweeper wins
	 * almost every time), so asserting on either count alone would be
	 * flaky; asserting the sum is non-zero keeps the test non-vacuous. */
	TEST_ASSERT_EQUAL_INT(0, fdrace_othererr);
	TEST_ASSERT_GREATER_THAN_INT(0, fdrace_opened + fdrace_raced);

	/* The descriptor table still has to work after all that. */
	fd = open(FDRACE_PATH, O_CREAT | O_RDWR, 0666);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
	TEST_ASSERT_EQUAL_INT(0, close(fd));
}


/* The same window through posix_newFile instead of posix_open.
 *
 * socket()/socketpair() also have to publish the fd slot before filling the
 * file in, and accept() blocks inside that window waiting for a connection. A
 * racing close used to take the file's last reference and free it while the
 * constructor was still writing to it, with the error path freeing it a second
 * time -- one block on the kernel's zone free list twice. Measured on a Pi4,
 * the slot really is stolen mid-construction under this workload.
 *
 * Like close_sweep_races_open, this asserts that both threads finish and the
 * descriptor table still works; a regression shows up as a fault or a hang.
 */
static void *fdrace_sockmaker(void *arg)
{
	long i;

	(void)arg;
	for (i = 0; i < FDRACE_ROUNDS; ++i) {
		int sv[2];
		int s = socket(AF_UNIX, SOCK_STREAM, 0);
		if (s >= 0) {
			fdrace_opened++;
			(void)close(s);
		}
		else {
			fdrace_raced++;
		}
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
			fdrace_opened++;
			(void)close(sv[0]);
			(void)close(sv[1]);
		}
		else {
			fdrace_raced++;
		}
	}
	fdrace_stop = 1;
	return NULL;
}


TEST(test_pthread_fdrace, close_sweep_races_socket)
{
	pthread_t maker, sweeper;
	int s;

	fdrace_stop = 0;
	fdrace_opened = 0;
	fdrace_raced = 0;

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&maker, NULL, fdrace_sockmaker, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&sweeper, NULL, fdrace_sweeper, NULL));

	TEST_ASSERT_EQUAL_INT(0, pthread_join(maker, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(sweeper, NULL));

	/* Non-vacuity: the maker ran. Successes vs races is timing-dependent. */
	TEST_ASSERT_GREATER_THAN_INT(0, fdrace_opened + fdrace_raced);

	/* The descriptor table still has to work afterwards. */
	s = socket(AF_UNIX, SOCK_STREAM, 0);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, s);
	TEST_ASSERT_EQUAL_INT(0, close(s));
}


TEST_GROUP_RUNNER(test_pthread_fdrace)
{
	RUN_TEST_CASE(test_pthread_fdrace, close_sweep_races_open);
	RUN_TEST_CASE(test_pthread_fdrace, close_sweep_races_socket);
}
