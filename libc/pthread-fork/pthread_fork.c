/*
 * Phoenix-RTOS
 *
 *    pthread API in the child of a fork()
 *    TESTED:
 *    - pthread_self() is non-NULL, stable, and equal to the forking thread's
 *      pthread_t saved before the fork
 *    - pthread_getspecific() returns the value the forking thread set before
 *      the fork; pthread_setspecific()/getspecific() round-trip in the child
 *    - pthread_setcancelstate() works (it used to dereference NULL here)
 *    - pthread_create() + pthread_join() from the child
 *    - (Phoenix) the records of threads that do not exist in the child are gone
 *    each from a single-threaded parent, from a parent with a second thread
 *    running, and with the fork() issued by that second thread
 *
 * The child of a fork() runs under a new tid on Phoenix, and libphoenix keys
 * its thread records by tid; before the fix every check above failed in every
 * forked child, and pthread_setcancelstate() killed it with SIGSEGV.
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
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

#include "unity_fixture.h"


/* Failure bits a child reports in its exit status (0 = all checks passed) */
#define CHK_SELF_NULL    (1 << 0)
#define CHK_SELF_EQUAL   (1 << 1)
#define CHK_SELF_PARENT  (1 << 2)
#define CHK_TSD_INHERIT  (1 << 3)
#define CHK_TSD_SET      (1 << 4)
#define CHK_OTHER_GONE   (1 << 5)
#define CHK_CREATE_JOIN  (1 << 6)
#define CHK_CANCELSTATE  (1 << 7)


static pthread_key_t fork_key;
static int fork_keyValue;
static int fork_childValue;


static struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int release;
} idle;


static void *childThread(void *arg)
{
	return (void *)((uintptr_t)arg + 1u);
}


/* Runs in the forked child: never asserts (Unity cannot report from here),
 * returns the failure bits for the parent to decode. */
static int childChecks(pthread_t forker, pthread_t other)
{
	int fail = 0, old = -1;
	void *ret = NULL;
	pthread_t self = pthread_self(), t;

	if (self == (pthread_t)0) {
		fail |= CHK_SELF_NULL;
	}
	if (pthread_equal(self, pthread_self()) == 0) {
		fail |= CHK_SELF_EQUAL;
	}
	if (pthread_equal(self, forker) == 0) {
		fail |= CHK_SELF_PARENT;
	}

	if (pthread_getspecific(fork_key) != &fork_keyValue) {
		fail |= CHK_TSD_INHERIT;
	}
	if ((pthread_setspecific(fork_key, &fork_childValue) != 0) || (pthread_getspecific(fork_key) != &fork_childValue)) {
		fail |= CHK_TSD_SET;
	}

#ifdef __phoenix__
	/* Only the forking thread exists in the child (POSIX); Phoenix validates a
	 * handle against its live records, so another thread's is now ESRCH. */
	if ((other != (pthread_t)0) && (pthread_detach(other) != ESRCH)) {
		fail |= CHK_OTHER_GONE;
	}
#else
	(void)other;
#endif

	if ((pthread_create(&t, NULL, childThread, (void *)41) != 0) || (pthread_join(t, &ret) != 0) || (ret != (void *)42)) {
		fail |= CHK_CREATE_JOIN;
	}

	/* Last: on the old code this one dereferenced NULL and killed the child. */
	if ((pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old) != 0) || (old != PTHREAD_CANCEL_ENABLE) ||
			(pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old) != 0) || (old != PTHREAD_CANCEL_DISABLE)) {
		fail |= CHK_CANCELSTATE;
	}

	return fail;
}


/* Forks, runs childChecks() in the child, returns the child's wait status. */
static int forkAndCheck(pthread_t other)
{
	pthread_t self = pthread_self();
	int status = -1;
	pid_t pid;

	if (pthread_setspecific(fork_key, &fork_keyValue) != 0) {
		return -1;
	}

	pid = fork();
	if (pid == 0) {
		_exit(childChecks(self, other));
	}
	if ((pid < 0) || (waitpid(pid, &status, 0) != pid)) {
		return -1;
	}
	return status;
}


static void assertChildPassed(int status)
{
	char msg[96];

	TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, status, "fork()/waitpid() failed");
	if (WIFSIGNALED(status)) {
		(void)snprintf(msg, sizeof(msg), "child killed by signal %d", WTERMSIG(status));
		TEST_FAIL_MESSAGE(msg);
	}
	TEST_ASSERT_TRUE(WIFEXITED(status));
	/* Non-zero: the CHK_* bits of the checks that failed */
	TEST_ASSERT_EQUAL_HEX8(0, WEXITSTATUS(status));
}


static void *idleThread(void *arg)
{
	(void)arg;

	pthread_mutex_lock(&idle.lock);
	while (idle.release == 0) {
		pthread_cond_wait(&idle.cond, &idle.lock);
	}
	pthread_mutex_unlock(&idle.lock);
	return NULL;
}


static void *forkingThread(void *arg)
{
	return (void *)(intptr_t)forkAndCheck(*(pthread_t *)arg);
}


TEST_GROUP(pthread_fork);


TEST_SETUP(pthread_fork)
{
	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&fork_key, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&idle.lock, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&idle.cond, NULL));
	idle.release = 0;
}


TEST_TEAR_DOWN(pthread_fork)
{
	pthread_cond_destroy(&idle.cond);
	pthread_mutex_destroy(&idle.lock);
	pthread_key_delete(fork_key);
}


TEST(pthread_fork, single_threaded_parent)
{
	assertChildPassed(forkAndCheck((pthread_t)0));
}


/* The other thread's record is in the list the child copies */
TEST(pthread_fork, parent_with_second_thread)
{
	pthread_t other;
	int status;

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&other, NULL, idleThread, NULL));
	status = forkAndCheck(other);

	pthread_mutex_lock(&idle.lock);
	idle.release = 1;
	pthread_cond_signal(&idle.cond);
	pthread_mutex_unlock(&idle.lock);
	TEST_ASSERT_EQUAL_INT(0, pthread_join(other, NULL));

	assertChildPassed(status);
}


/* fork() from a pthread_create()d thread: the child's only thread is that one,
 * and the main thread's record is the one that must go */
TEST(pthread_fork, fork_from_second_thread)
{
	pthread_t mainThread = pthread_self(), forker;
	void *ret = NULL;

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&forker, NULL, forkingThread, &mainThread));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(forker, &ret));

	assertChildPassed((int)(intptr_t)ret);
}


TEST_GROUP_RUNNER(pthread_fork)
{
	RUN_TEST_CASE(pthread_fork, single_threaded_parent);
	RUN_TEST_CASE(pthread_fork, parent_with_second_thread);
	RUN_TEST_CASE(pthread_fork, fork_from_second_thread);
}
