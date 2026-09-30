/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - unistd.h, sys/stat.h, stdlib.h, signal.h
 *
 * TESTED:
 *    - access(), stat(), realpath() under a stream of SIGCHLD with SA_RESTART
 *
 * POSIX lists no EINTR for these, and a program that installs its handlers with
 * SA_RESTART relies on that. On Phoenix a signal that arrives while a path query
 * is still queued at a busy filesystem server aborted the send, and access()
 * answered "no such file" for a file that exists. That is how WindowMaker lost
 * swback.png: its WorkspaceBack option runs `sh -c "wmsetbg ... &"`, the shell
 * exits at once, and the SIGCHLD lands during the next option's image lookups.
 *
 * The test reproduces that shape: a child that exits immediately, over and over,
 * while the parent and a few sibling threads (to keep the server's queue
 * non-empty) query an existing path. The race needs the server to be busy, so on
 * a broken libc it fails probabilistically -- most readily on a network root.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <unity_fixture.h>


#define PATH_QUERIED "/etc/passwd"
#define N_CHILDREN   300
#define N_QUERIES    8
#define N_HAMMERS    3


static struct {
	volatile sig_atomic_t stop;
	volatile sig_atomic_t reaped;
	volatile int failures;
	volatile int lastErrno;
	struct sigaction oldAct;
	pthread_mutex_t lock;
} restart_common;


static void restart_onChild(int sig)
{
	int saved = errno, status;

	(void)sig;
	/* reap as WindowMaker's buryChild() does */
	while (waitpid(-1, &status, WNOHANG) > 0) {
		restart_common.reaped++;
	}
	errno = saved;
}


static void restart_fail(int err)
{
	pthread_mutex_lock(&restart_common.lock);
	restart_common.failures++;
	restart_common.lastErrno = err;
	pthread_mutex_unlock(&restart_common.lock);
}


/* One round of the queries WindowMaker's image search and defaults check make */
static void restart_query(void)
{
	struct stat st;
	char *resolved;

	if (access(PATH_QUERIED, F_OK) != 0) {
		restart_fail(errno);
	}
	if (stat(PATH_QUERIED, &st) != 0) {
		restart_fail(errno);
	}
	/* heap buffer: thread stacks are small on Phoenix */
	resolved = realpath(PATH_QUERIED, NULL);
	if (resolved == NULL) {
		restart_fail(errno);
	}
	free(resolved);
}


static void *restart_hammer(void *arg)
{
	(void)arg;
	while (restart_common.stop == 0) {
		restart_query();
	}
	return NULL;
}


TEST_GROUP(sa_restart_path_queries);


TEST_SETUP(sa_restart_path_queries)
{
	struct sigaction act;

	restart_common.stop = 0;
	restart_common.reaped = 0;
	restart_common.failures = 0;
	restart_common.lastErrno = 0;
	TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&restart_common.lock, NULL));

	memset(&act, 0, sizeof(act));
	act.sa_handler = restart_onChild;
	act.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigemptyset(&act.sa_mask);
	TEST_ASSERT_EQUAL_INT(0, sigaction(SIGCHLD, &act, &restart_common.oldAct));
}


TEST_TEAR_DOWN(sa_restart_path_queries)
{
	int status;

	(void)sigaction(SIGCHLD, &restart_common.oldAct, NULL);
	while (waitpid(-1, &status, WNOHANG) > 0) {
	}
	(void)pthread_mutex_destroy(&restart_common.lock);
}


TEST(sa_restart_path_queries, sigchld_storm)
{
	pthread_t hammers[N_HAMMERS];
	int i, j;
	pid_t pid;

	/* the path must exist, or every failure below would be a real one */
	TEST_ASSERT_EQUAL_INT(0, access(PATH_QUERIED, F_OK));

	for (i = 0; i < N_HAMMERS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&hammers[i], NULL, restart_hammer, NULL));
	}

	for (i = 0; i < N_CHILDREN; i++) {
		pid = fork();
		if (pid == 0) {
			_exit(0);
		}
		if (pid < 0) {
			break;
		}
		for (j = 0; j < N_QUERIES; j++) {
			restart_query();
		}
	}

	restart_common.stop = 1;
	for (j = 0; j < N_HAMMERS; j++) {
		pthread_join(hammers[j], NULL);
	}

	TEST_ASSERT_EQUAL_INT_MESSAGE(N_CHILDREN, i, "fork() failed");
	TEST_ASSERT_GREATER_THAN_INT(0, restart_common.reaped);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, restart_common.lastErrno, strerror(restart_common.lastErrno));
	TEST_ASSERT_EQUAL_INT(0, restart_common.failures);
}


TEST_GROUP_RUNNER(sa_restart_path_queries)
{
	RUN_TEST_CASE(sa_restart_path_queries, sigchld_storm);
}
