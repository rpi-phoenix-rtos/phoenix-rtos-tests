/*
 * Phoenix-RTOS
 *
 * POSIX.1-2024 standard library functions tests
 *
 * HEADER:
 *    - unistd.h
 *
 * TESTED:
 *    - pipe2()
 *
 * libsoup and GLib create their wakeup pipes with pipe2(O_CLOEXEC |
 * O_NONBLOCK); libphoenix had only pipe().
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#define _GNU_SOURCE /* pipe2() on older glibc */

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <unity_fixture.h>


static int pipe2_fds[2];


static void pipe2_assertFlags(int cloexec, int nonblock)
{
	int i, fd, fl;

	for (i = 0; i < 2; i++) {
		fd = pipe2_fds[i];
		TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);

		fl = fcntl(fd, F_GETFD);
		TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fl);
		TEST_ASSERT_EQUAL_INT(cloexec, ((fl & FD_CLOEXEC) != 0) ? 1 : 0);

		fl = fcntl(fd, F_GETFL);
		TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fl);
		TEST_ASSERT_EQUAL_INT(nonblock, ((fl & O_NONBLOCK) != 0) ? 1 : 0);
	}
}


/* The pipe works: what goes in one end comes out of the other */
static void pipe2_assertFlows(void)
{
	char c = 0;

	TEST_ASSERT_EQUAL_INT(1, write(pipe2_fds[1], "x", 1));
	TEST_ASSERT_EQUAL_INT(1, read(pipe2_fds[0], &c, 1));
	TEST_ASSERT_EQUAL_INT('x', c);
}


TEST_GROUP(unistd_pipe2);


TEST_SETUP(unistd_pipe2)
{
	pipe2_fds[0] = -1;
	pipe2_fds[1] = -1;
}


TEST_TEAR_DOWN(unistd_pipe2)
{
	if (pipe2_fds[0] >= 0) {
		(void)close(pipe2_fds[0]);
	}
	if (pipe2_fds[1] >= 0) {
		(void)close(pipe2_fds[1]);
	}
}


TEST(unistd_pipe2, no_flags)
{
	TEST_ASSERT_EQUAL_INT(0, pipe2(pipe2_fds, 0));
	pipe2_assertFlags(0, 0);
	pipe2_assertFlows();
}


TEST(unistd_pipe2, cloexec)
{
	TEST_ASSERT_EQUAL_INT(0, pipe2(pipe2_fds, O_CLOEXEC));
	pipe2_assertFlags(1, 0);
	pipe2_assertFlows();
}


TEST(unistd_pipe2, nonblock)
{
	char c;

	TEST_ASSERT_EQUAL_INT(0, pipe2(pipe2_fds, O_NONBLOCK));
	pipe2_assertFlags(0, 1);

	/* Empty: does not block */
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, read(pipe2_fds[0], &c, 1));
	TEST_ASSERT_TRUE((errno == EAGAIN) || (errno == EWOULDBLOCK));
	pipe2_assertFlows();
}


TEST(unistd_pipe2, both)
{
	TEST_ASSERT_EQUAL_INT(0, pipe2(pipe2_fds, O_CLOEXEC | O_NONBLOCK));
	pipe2_assertFlags(1, 1);
	pipe2_assertFlows();
}


TEST(unistd_pipe2, invalid_flags)
{
	int fds[2] = { -1, -1 };

	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, pipe2(fds, O_TRUNC));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
	TEST_ASSERT_EQUAL_INT(-1, fds[0]);
	TEST_ASSERT_EQUAL_INT(-1, fds[1]);
}


TEST_GROUP_RUNNER(unistd_pipe2)
{
	RUN_TEST_CASE(unistd_pipe2, no_flags);
	RUN_TEST_CASE(unistd_pipe2, cloexec);
	RUN_TEST_CASE(unistd_pipe2, nonblock);
	RUN_TEST_CASE(unistd_pipe2, both);
	RUN_TEST_CASE(unistd_pipe2, invalid_flags);
}
