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


/* socket() is the one inet call with NO coverage, and it is also the one that
 * changed: the kernel used to resolve /dev/netsocket by PATH on every call, which
 * deadlocked any process that was itself the filesystem owning "/" (see
 * phoenix-rtos-kernel posix/inet.c). It now resolves once and caches. These cases
 * exercise what that cache can get wrong: the second and later calls, a mix of
 * families and types through the same cached port, and a FAILED call in between --
 * which must not poison the cache for the next good one. */
#define SOCKET_BURST 64


TEST(test_inet_socket, socket_many_sequential)
{
	int fd[SOCKET_BURST];
	int i, j;

	for (i = 0; i < SOCKET_BURST; i++) {
		fd[i] = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd[i] < 0) {
			/* Close what we did get, so a failure here does not leak into the
			 * rest of the group as EMFILE. */
			for (j = 0; j < i; j++) {
				close(fd[j]);
			}
			FAIL("socket");
		}
	}

	/* Distinct descriptors: a cache that handed back one shared object would
	 * still return a valid fd, so "it worked" is not enough. */
	for (i = 0; i < SOCKET_BURST; i++) {
		for (j = i + 1; j < SOCKET_BURST; j++) {
			TEST_ASSERT_NOT_EQUAL_INT(fd[i], fd[j]);
		}
	}

	for (i = 0; i < SOCKET_BURST; i++) {
		TEST_ASSERT_EQUAL_INT(0, close(fd[i]));
	}
}


TEST(test_inet_socket, socket_mixed_family_and_type)
{
	int dgram, stream, v6;

	dgram = socket(AF_INET, SOCK_DGRAM, 0);
	TEST_ASSERT_TRUE(dgram >= 0);

	stream = socket(AF_INET, SOCK_STREAM, 0);
	TEST_ASSERT_TRUE(stream >= 0);
	TEST_ASSERT_NOT_EQUAL_INT(dgram, stream);

	/* AF_INET6 may legitimately be unsupported; what must not happen is a
	 * crash or a success that hands back a v4 socket. */
	v6 = socket(AF_INET6, SOCK_DGRAM, 0);
	if (v6 >= 0) {
		TEST_ASSERT_NOT_EQUAL_INT(dgram, v6);
		TEST_ASSERT_NOT_EQUAL_INT(stream, v6);
		TEST_ASSERT_EQUAL_INT(0, close(v6));
	}
	else {
		TEST_ASSERT_TRUE((errno == EAFNOSUPPORT) || (errno == EPROTONOSUPPORT) ||
			(errno == EINVAL));
	}

	TEST_ASSERT_EQUAL_INT(0, close(stream));
	TEST_ASSERT_EQUAL_INT(0, close(dgram));
}


TEST(test_inet_socket, socket_bad_args_do_not_poison)
{
	int bad, good;

	bad = socket(AF_INET, 0x7ffe, 0);
	TEST_ASSERT_EQUAL_INT(-1, bad);
	/* ESOCKTNOSUPPORT is not defined by libphoenix, so it is not listed. */
	TEST_ASSERT_TRUE((errno == EINVAL) || (errno == EPROTONOSUPPORT) ||
		(errno == EAFNOSUPPORT));

	/* The call after the failure is the point of this case. */
	good = socket(AF_INET, SOCK_DGRAM, 0);
	TEST_ASSERT_TRUE(good >= 0);
	TEST_ASSERT_EQUAL_INT(0, close(good));
}


TEST(test_inet_socket, bind_ephemeral_reports_a_port)
{
	int fd;
	struct sockaddr_in addr = { 0 };
	struct sockaddr_in got = { 0 };
	socklen_t len = sizeof(got);

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	TEST_ASSERT_TRUE(fd >= 0);

	addr.sin_family = AF_INET;
	addr.sin_port = 0; /* let the stack choose */
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	TEST_ASSERT_EQUAL_INT(0, bind(fd, (struct sockaddr *)&addr, sizeof(addr)));

	TEST_ASSERT_EQUAL_INT(0, getsockname(fd, (struct sockaddr *)&got, &len));
	TEST_ASSERT_EQUAL_INT(AF_INET, got.sin_family);
	/* A bound socket with port 0 would be unreachable -- the whole point of
	 * asking for an ephemeral port is that one gets assigned. */
	TEST_ASSERT_NOT_EQUAL_INT(0, ntohs(got.sin_port));

	TEST_ASSERT_EQUAL_INT(0, close(fd));
}


TEST(test_inet_socket, udp_loopback_roundtrip)
{
	static const char payload[] = "phoenix-inet-roundtrip";
	int tx, rx;
	struct sockaddr_in rxaddr = { 0 };
	struct sockaddr_in bound = { 0 };
	struct sockaddr_in from = { 0 };
	socklen_t len;
	char buf[sizeof(payload)];
	ssize_t n;

	rx = socket(AF_INET, SOCK_DGRAM, 0);
	TEST_ASSERT_TRUE(rx >= 0);
	tx = socket(AF_INET, SOCK_DGRAM, 0);
	TEST_ASSERT_TRUE(tx >= 0);

	rxaddr.sin_family = AF_INET;
	rxaddr.sin_port = 0;
	rxaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	TEST_ASSERT_EQUAL_INT(0, bind(rx, (struct sockaddr *)&rxaddr, sizeof(rxaddr)));

	len = sizeof(bound);
	TEST_ASSERT_EQUAL_INT(0, getsockname(rx, (struct sockaddr *)&bound, &len));

	n = sendto(tx, payload, sizeof(payload), 0, (struct sockaddr *)&bound, sizeof(bound));
	TEST_ASSERT_EQUAL_INT((ssize_t)sizeof(payload), n);

	len = sizeof(from);
	n = recvfrom(rx, buf, sizeof(buf), 0, (struct sockaddr *)&from, &len);
	TEST_ASSERT_EQUAL_INT((ssize_t)sizeof(payload), n);
	TEST_ASSERT_EQUAL_MEMORY(payload, buf, sizeof(payload));
	/* The datagram must come back attributed to loopback, not to whatever was
	 * left in the caller's buffer. */
	TEST_ASSERT_EQUAL_INT(AF_INET, from.sin_family);
	TEST_ASSERT_EQUAL_UINT32(htonl(INADDR_LOOPBACK), from.sin_addr.s_addr);

	TEST_ASSERT_EQUAL_INT(0, close(tx));
	TEST_ASSERT_EQUAL_INT(0, close(rx));
}


TEST_GROUP_RUNNER(test_inet_socket)
{
	RUN_TEST_CASE(test_inet_socket, inet_zero_len_send);
	RUN_TEST_CASE(test_inet_socket, getaddrinfo_numeric_and_passive);
	RUN_TEST_CASE(test_inet_socket, socket_many_sequential);
	RUN_TEST_CASE(test_inet_socket, socket_mixed_family_and_type);
	RUN_TEST_CASE(test_inet_socket, socket_bad_args_do_not_poison);
	RUN_TEST_CASE(test_inet_socket, bind_ephemeral_reports_a_port);
	RUN_TEST_CASE(test_inet_socket, udp_loopback_roundtrip);
}

void runner(void)
{
	RUN_TEST_GROUP(test_inet_socket);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
