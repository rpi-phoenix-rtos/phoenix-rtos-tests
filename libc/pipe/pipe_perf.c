/*
 * Phoenix-RTOS
 *
 * test-libc-pipe: pipe throughput and latency
 *
 * Not pass/fail benchmarks: each case checks that the data arrived and prints
 * one `PIPE case=<name> ...` line with its numbers.
 *
 *    - throughput_64k, throughput_4k: 64 MB from a parent to a forked child,
 *      in 64 kB and 4 kB writes (the child reads 64 kB at a time)
 *    - pingpong: one byte there and back between two processes over two pipes
 *    - pingpong_poll: the same, with poll() before each read - the shape of a
 *      GLib main loop woken through its wake-up pipe
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include <unity_fixture.h>


#define TPP_BYTES (64U * 1024U * 1024U)
#define TPP_TRIPS 10000U


static char tpp_buf[64 * 1024];


static int64_t tpp_nowUs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}


static void tpp_throughput(const char *name, size_t wsize)
{
	int fd[2], status;
	size_t sent = 0, i;
	int64_t t0, us;
	ssize_t r;
	pid_t pid;

	for (i = 0; i < sizeof(tpp_buf); i++) {
		tpp_buf[i] = (char)i;
	}

	TEST_ASSERT_EQUAL_INT(0, pipe(fd));
	pid = fork();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);
	if (pid == 0) {
		size_t got = 0;
		unsigned sum = 0;
		(void)close(fd[1]);
		while ((r = read(fd[0], tpp_buf, sizeof(tpp_buf))) > 0) {
			/* touch every 4 kB so the data is really read, and check it */
			for (i = 0; i < (size_t)r; i += 4096) {
				sum += (unsigned char)tpp_buf[i] ^ (unsigned char)(got + i);
			}
			got += (size_t)r;
		}
		_exit(((r == 0) && (got == TPP_BYTES) && (sum == 0)) ? 0 : 1);
	}
	(void)close(fd[0]);

	t0 = tpp_nowUs();
	while (sent < TPP_BYTES) {
		r = write(fd[1], tpp_buf, wsize);
		if (r <= 0) {
			break;
		}
		sent += (size_t)r;
	}
	(void)close(fd[1]);
	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	us = tpp_nowUs() - t0;

	printf("PIPE case=%s bytes=%u write_size=%u us=%lld MBps=%.1f\n", name, (unsigned)sent, (unsigned)wsize,
		(long long)us, (us > 0) ? ((double)sent / (double)us) : 0.0);
	fflush(stdout);

	TEST_ASSERT_EQUAL_UINT(TPP_BYTES, sent);
	TEST_ASSERT_TRUE(WIFEXITED(status));
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, WEXITSTATUS(status), "the reader got the wrong bytes");
}


static void tpp_pingpong(const char *name, int usePoll)
{
	int ab[2], ba[2], status;
	struct pollfd pfd;
	int64_t t0, us;
	unsigned i, ok = 0;
	pid_t pid;
	char c;

	TEST_ASSERT_EQUAL_INT(0, pipe(ab));
	TEST_ASSERT_EQUAL_INT(0, pipe(ba));
	pid = fork();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);
	if (pid == 0) {
		(void)close(ab[1]);
		(void)close(ba[0]);
		pfd.fd = ab[0];
		pfd.events = POLLIN;
		for (;;) {
			if ((usePoll != 0) && (poll(&pfd, 1, -1) != 1)) {
				_exit(2);
			}
			if (read(ab[0], &c, 1) != 1) {
				break;
			}
			if (write(ba[1], &c, 1) != 1) {
				_exit(3);
			}
		}
		_exit(0);
	}
	(void)close(ab[0]);
	(void)close(ba[1]);

	pfd.fd = ba[0];
	pfd.events = POLLIN;
	t0 = tpp_nowUs();
	for (i = 0; i < TPP_TRIPS; i++) {
		c = (char)i;
		if (write(ab[1], &c, 1) != 1) {
			break;
		}
		if ((usePoll != 0) && (poll(&pfd, 1, 5000) != 1)) {
			break;
		}
		if ((read(ba[0], &c, 1) != 1) || (c != (char)i)) {
			break;
		}
		ok++;
	}
	us = tpp_nowUs() - t0;
	(void)close(ab[1]);
	(void)close(ba[0]);
	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));

	printf("PIPE case=%s trips=%u us=%lld us_per_trip=%.2f\n", name, ok, (long long)us,
		(ok > 0U) ? ((double)us / (double)ok) : 0.0);
	fflush(stdout);

	TEST_ASSERT_EQUAL_UINT(TPP_TRIPS, ok);
	TEST_ASSERT_TRUE(WIFEXITED(status) && (WEXITSTATUS(status) == 0));
}


TEST_GROUP(pipe_perf);


TEST_SETUP(pipe_perf)
{
}


TEST_TEAR_DOWN(pipe_perf)
{
}


TEST(pipe_perf, throughput_64k)
{
	tpp_throughput("throughput_64k", 64U * 1024U);
}


TEST(pipe_perf, throughput_4k)
{
	tpp_throughput("throughput_4k", 4096U);
}


TEST(pipe_perf, pingpong)
{
	tpp_pingpong("pingpong", 0);
}


TEST(pipe_perf, pingpong_poll)
{
	tpp_pingpong("pingpong_poll", 1);
}


TEST_GROUP_RUNNER(pipe_perf)
{
	RUN_TEST_CASE(pipe_perf, throughput_64k);
	RUN_TEST_CASE(pipe_perf, throughput_4k);
	RUN_TEST_CASE(pipe_perf, pingpong);
	RUN_TEST_CASE(pipe_perf, pingpong_poll);
}
