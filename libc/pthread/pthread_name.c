/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - pthread.h
 *
 * TESTED:
 *    - pthread_setname_np(), pthread_getname_np() (GNU extensions)
 *
 * GLib names its threads with pthread_setname_np() when it exists, and
 * WebKit's thread code does too.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#define _GNU_SOURCE /* pthread_setname_np() on glibc (host-generic-pc) */

#include <errno.h>
#include <pthread.h>
#include <string.h>

#include <unity_fixture.h>


static pthread_barrier_t name_barrier;
static char name_seen[16];


static void *name_worker(void *arg)
{
	(void)arg;

	(void)pthread_barrier_wait(&name_barrier); /* named by the main thread */
	(void)pthread_getname_np(pthread_self(), name_seen, sizeof(name_seen));
	(void)pthread_barrier_wait(&name_barrier);

	return NULL;
}


TEST_GROUP(pthread_name);


TEST_SETUP(pthread_name)
{
}


TEST_TEAR_DOWN(pthread_name)
{
}


TEST(pthread_name, self_round_trip)
{
	char buf[16], old[16];

	TEST_ASSERT_EQUAL_INT(0, pthread_getname_np(pthread_self(), old, sizeof(old)));
	TEST_ASSERT_EQUAL_INT(0, pthread_setname_np(pthread_self(), "unity-main"));
	TEST_ASSERT_EQUAL_INT(0, pthread_getname_np(pthread_self(), buf, sizeof(buf)));
	TEST_ASSERT_EQUAL_STRING("unity-main", buf);

	/* 15 characters is the most there is room for */
	TEST_ASSERT_EQUAL_INT(0, pthread_setname_np(pthread_self(), "123456789012345"));
	TEST_ASSERT_EQUAL_INT(0, pthread_getname_np(pthread_self(), buf, sizeof(buf)));
	TEST_ASSERT_EQUAL_STRING("123456789012345", buf);

	TEST_ASSERT_EQUAL_INT(0, pthread_setname_np(pthread_self(), old));
}


TEST(pthread_name, too_long)
{
	char buf[4];

	TEST_ASSERT_EQUAL_INT(ERANGE, pthread_setname_np(pthread_self(), "1234567890123456"));

	TEST_ASSERT_EQUAL_INT(0, pthread_setname_np(pthread_self(), "a-long-name"));
	TEST_ASSERT_EQUAL_INT(ERANGE, pthread_getname_np(pthread_self(), buf, sizeof(buf)));
	TEST_ASSERT_EQUAL_INT(0, pthread_setname_np(pthread_self(), ""));
}


/* Named from another thread, seen by the thread itself */
TEST(pthread_name, other_thread)
{
	pthread_t thread;
	char buf[16];

	memset(name_seen, 0, sizeof(name_seen));
	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_init(&name_barrier, NULL, 2));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, name_worker, NULL));

	TEST_ASSERT_EQUAL_INT(0, pthread_setname_np(thread, "worker-7"));
	TEST_ASSERT_EQUAL_INT(0, pthread_getname_np(thread, buf, sizeof(buf)));
	(void)pthread_barrier_wait(&name_barrier);
	(void)pthread_barrier_wait(&name_barrier);

	TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_destroy(&name_barrier));

	TEST_ASSERT_EQUAL_STRING("worker-7", buf);
	TEST_ASSERT_EQUAL_STRING("worker-7", name_seen);
}


TEST(pthread_name, exited_thread)
{
#ifdef __phoenix__
	pthread_t thread;
	char buf[16];

	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_init(&name_barrier, NULL, 1));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, name_worker, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_destroy(&name_barrier));

	TEST_ASSERT_EQUAL_INT(ESRCH, pthread_setname_np(thread, "gone"));
	TEST_ASSERT_EQUAL_INT(ESRCH, pthread_getname_np(thread, buf, sizeof(buf)));
#else
	TEST_IGNORE_MESSAGE("a joined thread's handle is undefined behaviour on glibc");
#endif
}


TEST_GROUP_RUNNER(pthread_name)
{
	RUN_TEST_CASE(pthread_name, self_round_trip);
	RUN_TEST_CASE(pthread_name, too_long);
	RUN_TEST_CASE(pthread_name, other_thread);
	RUN_TEST_CASE(pthread_name, exited_thread);
}
