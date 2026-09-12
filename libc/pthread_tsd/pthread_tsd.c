/*
 * Phoenix-RTOS
 *
 * phoenix-rtos-tests
 *
 * pthread thread-specific data (keys)
 *
 * TSD had essentially no coverage, which matters more here than it looks:
 * libstdc++ for this target is built with threads but WITHOUT TLS
 * (_GLIBCXX_HAS_GTHREADS=1, _GLIBCXX_HAVE_TLS undef), so its per-thread state --
 * __cxa_eh_globals among it -- is routed through pthread keys. Every C++ program
 * with threads therefore leans on exactly this code.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity_fixture.h"


#define TSD_THREADS 4

static pthread_key_t tsd_key;
static pthread_key_t tsd_dtorKey;
static volatile int tsd_dtorCalls;
static void *tsd_seen[TSD_THREADS];


TEST_GROUP(pthread_tsd);


TEST_SETUP(pthread_tsd)
{
	tsd_dtorCalls = 0;
	(void)memset(tsd_seen, 0, sizeof(tsd_seen));
}


TEST_TEAR_DOWN(pthread_tsd)
{
}


/* Each thread must see ONLY its own value. A shared/global store would make the
 * threads read each other's, which is the failure this exists to catch. */
static void *tsd_isolationBody(void *arg)
{
	int idx = (int)(intptr_t)arg;
	void *mine = (void *)(intptr_t)(0x1000 + idx);
	int i;

	if (pthread_setspecific(tsd_key, mine) != 0) {
		return (void *)(intptr_t)-1;
	}

	/* Give the other threads time to overwrite a shared slot if there is one. */
	for (i = 0; i < 100; i++) {
		usleep(100);
		if (pthread_getspecific(tsd_key) != mine) {
			return (void *)(intptr_t)-1;
		}
	}

	tsd_seen[idx] = pthread_getspecific(tsd_key);
	return NULL;
}


TEST(pthread_tsd, value_is_per_thread)
{
	pthread_t th[TSD_THREADS];
	void *ret;
	int i;

	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_key, NULL));

	for (i = 0; i < TSD_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&th[i], NULL, tsd_isolationBody,
			(void *)(intptr_t)i));
	}
	for (i = 0; i < TSD_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(th[i], &ret));
		TEST_ASSERT_NULL(ret);
	}
	for (i = 0; i < TSD_THREADS; i++) {
		TEST_ASSERT_EQUAL_PTR((void *)(intptr_t)(0x1000 + i), tsd_seen[i]);
	}

	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_key));
}


/* A key created in one thread must be usable from another, and must start unset
 * there -- a fresh thread inherits no values. */
static void *tsd_freshBody(void *arg)
{
	(void)arg;
	return pthread_getspecific(tsd_key);
}


TEST(pthread_tsd, new_thread_starts_unset)
{
	pthread_t th;
	void *ret;

	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_key, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_setspecific(tsd_key, (void *)0xabc));

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, tsd_freshBody, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(th, &ret));
	TEST_ASSERT_NULL(ret);

	/* ...and the creating thread's value survived the other thread. */
	TEST_ASSERT_EQUAL_PTR((void *)0xabc, pthread_getspecific(tsd_key));

	TEST_ASSERT_EQUAL_INT(0, pthread_setspecific(tsd_key, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_key));
}


static void tsd_dtor(void *value)
{
	if (value != NULL) {
		tsd_dtorCalls++;
	}
}


static void *tsd_dtorBody(void *arg)
{
	(void)arg;
	(void)pthread_setspecific(tsd_dtorKey, (void *)0x55);
	return NULL;
}


/* The destructor must run when a thread with a non-NULL value exits. This is what
 * libstdc++ relies on to tear down its per-thread state without TLS. */
TEST(pthread_tsd, destructor_runs_at_thread_exit)
{
	pthread_t th;

	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_dtorKey, tsd_dtor));

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, tsd_dtorBody, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(th, NULL));

	TEST_ASSERT_EQUAL_INT(1, tsd_dtorCalls);

	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_dtorKey));
}


/* Overwriting a value must replace it, not stack a second entry that a later
 * lookup could find first. */
TEST(pthread_tsd, overwrite_replaces_value)
{
	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_key, NULL));

	TEST_ASSERT_EQUAL_INT(0, pthread_setspecific(tsd_key, (void *)0x11));
	TEST_ASSERT_EQUAL_PTR((void *)0x11, pthread_getspecific(tsd_key));
	TEST_ASSERT_EQUAL_INT(0, pthread_setspecific(tsd_key, (void *)0x22));
	TEST_ASSERT_EQUAL_PTR((void *)0x22, pthread_getspecific(tsd_key));
	TEST_ASSERT_EQUAL_INT(0, pthread_setspecific(tsd_key, NULL));
	TEST_ASSERT_NULL(pthread_getspecific(tsd_key));

	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_key));
}


TEST_GROUP_RUNNER(pthread_tsd)
{
	RUN_TEST_CASE(pthread_tsd, value_is_per_thread);
	RUN_TEST_CASE(pthread_tsd, new_thread_starts_unset);
	RUN_TEST_CASE(pthread_tsd, destructor_runs_at_thread_exit);
	RUN_TEST_CASE(pthread_tsd, overwrite_replaces_value);
}


void runner(void)
{
	RUN_TEST_GROUP(pthread_tsd);
}


int main(int argc, char *argv[])
{
	return UnityMain(argc, (const char **)argv, runner) == 0 ? 0 : 1;
}
