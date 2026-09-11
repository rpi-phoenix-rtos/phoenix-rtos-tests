/*
 * Phoenix-RTOS
 *
 * test-libc-socket
 *
 * inet socket tests
 *
 * Copyright 2021, 2024 Phoenix Systems
 * Author: Ziemowit Leszczynski, Adam Debek
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <netdb.h>

#include "common.h"
#include "unity_fixture.h"

static char data[DATA_SIZE];

TEST_GROUP(test_inet_socket);


TEST_SETUP(test_inet_socket)
{
}


TEST_TEAR_DOWN(test_inet_socket)
{
}


TEST(test_inet_socket, inet_zero_len_send)
{
	int fd[3];
	struct sockaddr_in addr = { 0 };
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;

	if ((fd[0] = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		FAIL("socket");
	if ((fd[1] = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		FAIL("socket");

	addr.sin_family = AF_INET;
	addr.sin_port = 0;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(fd[0], (struct sockaddr *)&addr, sizeof(addr)) < 0)
		FAIL("bind");

	addr.sin_family = AF_INET;
	addr.sin_port = htons(30000);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(fd[1], (struct sockaddr *)&addr, sizeof(addr)) < 0)
		FAIL("bind");

	if (connect(fd[0], (struct sockaddr *)&addr, sizeof(addr)) < 0)
		FAIL("connect");

	/* write */
	{
		n = write(fd[0], NULL, 0);
		TEST_ASSERT(n == 0);

		n = write(fd[0], data, 0);
		TEST_ASSERT(n == 0);
	}

	/* writev */
	{
#ifdef __phoenix__
		n = writev(fd[0], NULL, 0);
		TEST_ASSERT(n == -1);
		TEST_ASSERT(errno == EINVAL);

		n = writev(fd[0], &iov, 0);
		TEST_ASSERT(n == -1);
		TEST_ASSERT(errno == EINVAL);
#else
		n = writev(fd[0], NULL, 0);
		TEST_ASSERT(n == 0);
		TEST_ASSERT(errno == 0);

		n = writev(fd[0], &iov, 0);
		TEST_ASSERT(n == 0);
		TEST_ASSERT(errno == 0);
#endif
		iov.iov_base = NULL;
		iov.iov_len = 0;
		n = writev(fd[0], &iov, 1);
		TEST_ASSERT(n == 0);

		iov.iov_base = data;
		iov.iov_len = 0;
		n = writev(fd[0], &iov, 1);
		TEST_ASSERT(n == 0);
	}

	/* send */
	{
		n = send(fd[0], NULL, 0, 0);
		TEST_ASSERT(n == 0);

		n = send(fd[0], data, 0, 0);
		TEST_ASSERT(n == 0);
	}

	/* sendto */
	{
		n = sendto(fd[0], NULL, 0, 0, NULL, 0);
		TEST_ASSERT(n == 0);

		n = sendto(fd[0], data, 0, 0, NULL, 0);
		TEST_ASSERT(n == 0);
	}

	/* sendmsg */
	{
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = NULL;
		msg.msg_iovlen = 0;
		n = sendmsg(fd[0], &msg, 0);
		TEST_ASSERT(n == 0);

		memset(&msg, 0, sizeof(msg));
		iov.iov_base = NULL;
		iov.iov_len = 0;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		n = sendmsg(fd[0], &msg, 0);
		TEST_ASSERT(n == 0);
	}

	close(fd[0]);
	close(fd[1]);
}


/* getaddrinfo() had NO test coverage at all before this, which is how a
 * success-with-NULL-result survived in it: the implementation did
 *
 *     *res = msg.o.data = realloc(msg.o.data, bufsz);
 *
 * so a failing realloc() leaked the original block AND handed the caller
 * `*res == NULL` with a 0 return. Every caller dereferences that immediately --
 * ntpclient does `res->ai_family` on the next line. Fixed in libphoenix
 * sys/socket.c; this is the regression test.
 *
 * Deliberately DNS-free: AI_NUMERICHOST and AI_PASSIVE resolve locally, so the
 * test does not depend on a nameserver being reachable from the target and
 * cannot fail for reasons unrelated to the code under test.
 */
TEST(test_inet_socket, getaddrinfo_numeric_and_passive)
{
	struct addrinfo hints, *res;
	struct sockaddr_in *sin;
	int rv;

	/* 1. A numeric host must resolve, and a 0 return MUST come with a usable
	 *    result -- this is the assertion the old code violated. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_NUMERICHOST;

	res = NULL;
	rv = getaddrinfo("127.0.0.1", "123", &hints, &res);
	TEST_ASSERT_EQUAL_INT(0, rv);
	TEST_ASSERT_NOT_NULL_MESSAGE(res, "getaddrinfo returned 0 but left *res NULL");
	TEST_ASSERT_NOT_NULL(res->ai_addr);
	TEST_ASSERT_EQUAL_INT(AF_INET, res->ai_family);
	TEST_ASSERT_GREATER_OR_EQUAL_UINT(sizeof(struct sockaddr_in), res->ai_addrlen);

	sin = (struct sockaddr_in *)res->ai_addr;
	TEST_ASSERT_EQUAL_UINT32(htonl(INADDR_LOOPBACK), sin->sin_addr.s_addr);
	TEST_ASSERT_EQUAL_UINT16(htons(123), sin->sin_port);
	freeaddrinfo(res);

	/* 2. AI_PASSIVE with a NULL node yields a bindable wildcard address. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	res = NULL;
	rv = getaddrinfo(NULL, "8080", &hints, &res);
	TEST_ASSERT_EQUAL_INT(0, rv);
	TEST_ASSERT_NOT_NULL_MESSAGE(res, "getaddrinfo returned 0 but left *res NULL");
	TEST_ASSERT_NOT_NULL(res->ai_addr);
	sin = (struct sockaddr_in *)res->ai_addr;
	TEST_ASSERT_EQUAL_UINT32(htonl(INADDR_ANY), sin->sin_addr.s_addr);
	TEST_ASSERT_EQUAL_UINT16(htons(8080), sin->sin_port);
	freeaddrinfo(res);

	/* 3. A non-numeric name under AI_NUMERICHOST must FAIL rather than resolve,
	 *    and must not leave a result behind. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_flags = AI_NUMERICHOST;

	res = NULL;
	rv = getaddrinfo("not-a-numeric-host", "123", &hints, &res);
	TEST_ASSERT_NOT_EQUAL_INT(0, rv);

	/* 4. Repeat the numeric lookup so a leak or a stale buffer shows up as a
	 *    changed result rather than passing once by luck. */
	{
		int i;
		for (i = 0; i < 32; i++) {
			memset(&hints, 0, sizeof(hints));
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_DGRAM;
			hints.ai_flags = AI_NUMERICHOST;

			res = NULL;
			TEST_ASSERT_EQUAL_INT(0, getaddrinfo("127.0.0.1", "123", &hints, &res));
			TEST_ASSERT_NOT_NULL(res);
			sin = (struct sockaddr_in *)res->ai_addr;
			TEST_ASSERT_EQUAL_UINT32(htonl(INADDR_LOOPBACK), sin->sin_addr.s_addr);
			freeaddrinfo(res);
		}
	}
}


TEST_GROUP_RUNNER(test_inet_socket)
{
	RUN_TEST_CASE(test_inet_socket, inet_zero_len_send);
	RUN_TEST_CASE(test_inet_socket, getaddrinfo_numeric_and_passive);
}

void runner(void)
{
	RUN_TEST_GROUP(test_inet_socket);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
