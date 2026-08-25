/*
 * Phoenix-RTOS
 *
 * test-libc-poll
 *
 * poll tests
 *
 * Copyright 2024 Phoenix Systems
 * Author: Adam Greloch
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>

#include "unity_fixture.h"


#define MS_BETWEEN(ts0, ts1) \
	((ts1).tv_sec - (ts0).tv_sec) * 1000 + ((ts1).tv_nsec - (ts0).tv_nsec) / 1000000;


TEST_GROUP(test_poll);


TEST_SETUP(test_poll)
{
}


TEST_TEAR_DOWN(test_poll)
{
}


static int get_bad_fd(int min_num, int max_num)
{
	int rv, fd;
	for (fd = min_num; fd <= max_num; fd++) {
		rv = fcntl(fd, F_GETFD);
		if (rv < 0) {
			return fd;
		}
	}
	return -1;
}


TEST(test_poll, select_errnos)
{
	fd_set rfds;
	struct timeval tv;
	struct timespec ts[2];
	int rv, ms, bad_fd;

	tv.tv_sec = 0;
	tv.tv_usec = 300 * 1000;
	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);
	clock_gettime(CLOCK_REALTIME, &ts[0]);
	rv = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
	clock_gettime(CLOCK_REALTIME, &ts[1]);
	ms = MS_BETWEEN(ts[0], ts[1]);
	TEST_ASSERT(rv == 0);
	TEST_ASSERT_EQUAL_INT(0, FD_ISSET(STDIN_FILENO, &rfds));
	TEST_ASSERT_LESS_THAN(350, ms);
	TEST_ASSERT_GREATER_THAN(290, ms);

	tv.tv_sec = 0;
	tv.tv_usec = 1000;
	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);
	errno = 0;
	rv = select(-1, &rfds, NULL, NULL, &tv);
	TEST_ASSERT_EQUAL_INT(-1, rv);
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);

	tv.tv_sec = 0;
	tv.tv_usec = -1;
	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);
	errno = 0;
	rv = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
	TEST_ASSERT_EQUAL_INT(-1, rv);
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);

	tv.tv_sec = 0;
	tv.tv_usec = 1000;
	bad_fd = get_bad_fd(0, 1024);
	if (bad_fd < 0) {
		FAIL("get_bad_fd");
	}
	FD_ZERO(&rfds);
	FD_SET(bad_fd, &rfds);
	errno = 0;
	rv = select(FD_SETSIZE, &rfds, NULL, NULL, &tv);
	TEST_ASSERT_EQUAL_INT(-1, rv);
	TEST_ASSERT_EQUAL_INT(EBADF, errno);
}


/* Regression: select() with a NULL (infinite) timeout must BLOCK until a
 * descriptor is ready, not return 0 immediately. A libphoenix bug clamped the
 * NULL timeout to a 0 ms poll (returned 0 at once), which made GNU readline's
 * blocking select() in rl_getc() abort -> interactive bash exited at its first
 * prompt. Here a child writes to a pipe after a short delay; the parent's
 * select(read-end, NULL) must block and then return 1 (readable). */
TEST(test_poll, select_null_timeout_blocks)
{
	int fds[2];
	pid_t pid;
	fd_set rfds;
	int rv;
	char c;

	TEST_ASSERT_EQUAL_INT(0, pipe(fds));

	pid = fork();
	TEST_ASSERT_TRUE(pid >= 0);
	if (pid == 0) {
		struct timespec ts = { 0, 200000000L }; /* 200 ms */
		close(fds[0]);
		nanosleep(&ts, NULL);
		(void)write(fds[1], "x", 1);
		close(fds[1]);
		_exit(0);
	}

	close(fds[1]);
	alarm(5); /* guard: if select(NULL) truly blocks forever, fail rather than hang the suite */
	FD_ZERO(&rfds);
	FD_SET(fds[0], &rfds);
	rv = select(fds[0] + 1, &rfds, NULL, NULL, NULL); /* NULL timeout: must block, then return 1 */
	alarm(0);

	TEST_ASSERT_EQUAL_INT(1, rv); /* pre-fix bug returned 0 immediately */
	TEST_ASSERT_TRUE(FD_ISSET(fds[0], &rfds));
	TEST_ASSERT_EQUAL_INT(1, read(fds[0], &c, 1));

	close(fds[0]);
	waitpid(pid, NULL, 0);
}


TEST_GROUP_RUNNER(test_poll)
{
	RUN_TEST_CASE(test_poll, select_errnos);
	RUN_TEST_CASE(test_poll, select_null_timeout_blocks);
}


static void runner(void)
{
	RUN_TEST_GROUP(test_poll);
}


int main(int argc, char *argv[])
{
	int failures = UnityMain(argc, (const char **)argv, runner);

	return (failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
