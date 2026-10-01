/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - pthread.h
 *
 * TESTED:
 *    - pthread_getattr_np() (GNU extension) + pthread_attr_getstack()
 *
 * Garbage-collected runtimes (WebKit's JavaScriptCore, via WTF::StackBounds)
 * scan every thread's stack between its current stack pointer and the
 * reported top, and place their stack-overflow limit near the reported
 * bottom. Both must be exact: a top above the real one reads unmapped memory,
 * one below it misses live pointers. The main thread is the hard case,
 * because its stack is mapped by the kernel and not by the pthread library;
 * the test binary is linked with "-z stack-size=12288", so on Phoenix the
 * kernel gives main exactly 12 KiB (proc/process.c, PT_GNU_STACK).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#define _GNU_SOURCE /* pthread_getattr_np() on glibc (host-generic-pc) */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <unity_fixture.h>


/* The PT_GNU_STACK size the libc tests are linked with (libc/Makefile) */
#define GETATTR_MAIN_STACK 12288u

#define GETATTR_THREAD_STACK (128u * 1024u)


extern char **environ;


typedef struct {
	void *addr;
	size_t size;
	size_t guard;
	int detach;
	int err;
	uintptr_t local; /* an address on the thread's own stack */
	volatile int done;
} getattr_result_t;


static int getattr_query(pthread_t thread, getattr_result_t *res)
{
	pthread_attr_t attr;
	int err;

	err = pthread_getattr_np(thread, &attr);
	if (err != 0) {
		return err;
	}

	if ((pthread_attr_getstack(&attr, &res->addr, &res->size) != 0) ||
			(pthread_attr_getguardsize(&attr, &res->guard) != 0) ||
			(pthread_attr_getdetachstate(&attr, &res->detach) != 0)) {
		err = -1;
	}
	(void)pthread_attr_destroy(&attr);

	return err;
}


static int getattr_contains(const getattr_result_t *res, uintptr_t p)
{
	return ((p >= (uintptr_t)res->addr) && ((p - (uintptr_t)res->addr) < res->size)) ? 1 : 0;
}


static void *getattr_self(void *arg)
{
	getattr_result_t *res = arg;
	volatile char local = 0;

	res->local = (uintptr_t)&local;
	res->err = getattr_query(pthread_self(), res);
	__atomic_store_n(&res->done, 1, __ATOMIC_RELEASE);

	return NULL;
}


static void *getattr_wait(void *arg)
{
	pthread_barrier_t *barrier = arg;

	(void)pthread_barrier_wait(barrier);
	(void)pthread_barrier_wait(barrier);

	return NULL;
}


static pthread_t getattr_mainThread;


static void *getattr_ofMain(void *arg)
{
	getattr_result_t *res = arg;

	res->err = getattr_query(getattr_mainThread, res);

	return NULL;
}


TEST_GROUP(pthread_getattr);


TEST_SETUP(pthread_getattr)
{
}


TEST_TEAR_DOWN(pthread_getattr)
{
}


/* A thread that asks about itself gets the stack it is running on */
TEST(pthread_getattr, self_default_attr)
{
	pthread_t thread;
	getattr_result_t res = { 0 };
	pthread_attr_t attr;
	size_t defsize, defguard;

	TEST_ASSERT_EQUAL_INT(0, pthread_attr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_getstacksize(&attr, &defsize));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_getguardsize(&attr, &defguard));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_destroy(&attr));

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, getattr_self, &res));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));

	TEST_ASSERT_EQUAL_INT(0, res.err);
	TEST_ASSERT_NOT_NULL(res.addr);
	TEST_ASSERT_TRUE(getattr_contains(&res, res.local));
	TEST_ASSERT_EQUAL_INT(PTHREAD_CREATE_JOINABLE, res.detach);
	TEST_ASSERT_EQUAL_UINT(defguard, res.guard);
#ifdef __phoenix__
	/* The default stack is allocated as asked, the guard below it */
	TEST_ASSERT_EQUAL_UINT(defsize, res.size);
#else
	/* glibc carves the guard out of the requested size */
	TEST_ASSERT_GREATER_OR_EQUAL_UINT(defsize - defguard, res.size);
#endif
}


/* An explicit size is reported back, and the detach state with it */
TEST(pthread_getattr, self_explicit_size_detached)
{
	pthread_t thread;
	pthread_attr_t attr;
	getattr_result_t res = { 0 };
	int i;

	TEST_ASSERT_EQUAL_INT(0, pthread_attr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_setstacksize(&attr, GETATTR_THREAD_STACK));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, &attr, getattr_self, &res));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_destroy(&attr));

	/* Detached: no join, wait for the result instead (bounded, 5 s) */
	for (i = 0; (i < 500) && (__atomic_load_n(&res.done, __ATOMIC_ACQUIRE) == 0); i++) {
		usleep(10000);
	}

	TEST_ASSERT_EQUAL_INT(1, res.done);
	TEST_ASSERT_EQUAL_INT(0, res.err);
	TEST_ASSERT_TRUE(getattr_contains(&res, res.local));
	TEST_ASSERT_EQUAL_INT(PTHREAD_CREATE_DETACHED, res.detach);
#ifdef __phoenix__
	TEST_ASSERT_EQUAL_UINT(GETATTR_THREAD_STACK, res.size);
#else
	TEST_ASSERT_GREATER_OR_EQUAL_UINT(GETATTR_THREAD_STACK - res.guard, res.size);
#endif
}


/* A stack the caller supplied is reported exactly, with no guard */
TEST(pthread_getattr, caller_supplied_stack)
{
	const size_t size = 64u * 1024u;
	pthread_t thread;
	pthread_attr_t attr;
	getattr_result_t res = { 0 };
	void *stack = NULL;

	TEST_ASSERT_EQUAL_INT(0, posix_memalign(&stack, 4096, size));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_setstack(&attr, stack, size));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, &attr, getattr_self, &res));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_destroy(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));

	TEST_ASSERT_EQUAL_INT(0, res.err);
	TEST_ASSERT_EQUAL_PTR(stack, res.addr);
	TEST_ASSERT_EQUAL_UINT(size, res.size);
	TEST_ASSERT_EQUAL_UINT(0u, res.guard);
	TEST_ASSERT_TRUE(getattr_contains(&res, res.local));
	free(stack);
}


/* Asked about from another thread, a thread's stack is the same */
TEST(pthread_getattr, other_thread)
{
	pthread_t thread;
	pthread_attr_t attr;
	pthread_barrier_t barrier;
	getattr_result_t res = { 0 };

	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_init(&barrier, NULL, 2));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_setstacksize(&attr, GETATTR_THREAD_STACK));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, &attr, getattr_wait, &barrier));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_destroy(&attr));

	(void)pthread_barrier_wait(&barrier); /* the thread is running */
	res.err = getattr_query(thread, &res);
	(void)pthread_barrier_wait(&barrier);
	TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_destroy(&barrier));

	TEST_ASSERT_EQUAL_INT(0, res.err);
	TEST_ASSERT_NOT_NULL(res.addr);
	TEST_ASSERT_EQUAL_INT(PTHREAD_CREATE_JOINABLE, res.detach);
	/* Our own stack is not the other thread's */
	TEST_ASSERT_FALSE(getattr_contains(&res, (uintptr_t)&res));
#ifdef __phoenix__
	TEST_ASSERT_EQUAL_UINT(GETATTR_THREAD_STACK, res.size);
#endif
}


/* The main thread: its stack holds our frame and the initial environment */
TEST(pthread_getattr, main_thread)
{
	getattr_result_t res = { 0 };
	volatile char local = 0;

	TEST_ASSERT_EQUAL_INT(0, getattr_query(pthread_self(), &res));
	TEST_ASSERT_NOT_NULL(res.addr);
	TEST_ASSERT_TRUE(getattr_contains(&res, (uintptr_t)&local));
	/* Both kernels put the environment block at the top of the main stack */
	TEST_ASSERT_TRUE(getattr_contains(&res, (uintptr_t)environ));
#ifdef __phoenix__
	/* Exactly the stack the kernel mapped: neither a default nor a guess */
	TEST_ASSERT_EQUAL_UINT(GETATTR_MAIN_STACK, res.size);
	TEST_ASSERT_EQUAL_UINT(0u, res.guard);
	/* All of it is mapped: both ends can be read (the lowest 16 bytes are the
	 * kernel's canary, so read just above it) */
	{
		volatile const char *p = res.addr;
		char c = p[16];

		c ^= p[res.size - 1];
		(void)c;
	}
#endif
}


/* ... and another thread gets the same answer for it */
TEST(pthread_getattr, main_thread_from_other)
{
	pthread_t thread;
	getattr_result_t res = { 0 }, own = { 0 };

	getattr_mainThread = pthread_self();
	TEST_ASSERT_EQUAL_INT(0, getattr_query(getattr_mainThread, &own));

	res.err = -1;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, getattr_ofMain, &res));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));

	TEST_ASSERT_EQUAL_INT(0, res.err);
	TEST_ASSERT_EQUAL_PTR(own.addr, res.addr);
	TEST_ASSERT_EQUAL_UINT(own.size, res.size);
	TEST_ASSERT_TRUE(getattr_contains(&res, (uintptr_t)&own));
}


TEST(pthread_getattr, bad_arguments)
{
#ifdef __phoenix__
	pthread_t thread;
	getattr_result_t res = { 0 };
	pthread_attr_t attr;

	TEST_ASSERT_EQUAL_INT(EINVAL, pthread_getattr_np(pthread_self(), NULL));
	TEST_ASSERT_EQUAL_INT(ESRCH, pthread_getattr_np((pthread_t)0, &attr));

	/* A joined thread is gone: its handle must not be read */
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, getattr_self, &res));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
	TEST_ASSERT_EQUAL_INT(ESRCH, pthread_getattr_np(thread, &attr));
#else
	TEST_IGNORE_MESSAGE("invalid arguments are undefined behaviour on glibc");
#endif
}


TEST_GROUP_RUNNER(pthread_getattr)
{
	RUN_TEST_CASE(pthread_getattr, self_default_attr);
	RUN_TEST_CASE(pthread_getattr, self_explicit_size_detached);
	RUN_TEST_CASE(pthread_getattr, caller_supplied_stack);
	RUN_TEST_CASE(pthread_getattr, other_thread);
	RUN_TEST_CASE(pthread_getattr, main_thread);
	RUN_TEST_CASE(pthread_getattr, main_thread_from_other);
	RUN_TEST_CASE(pthread_getattr, bad_arguments);
}
