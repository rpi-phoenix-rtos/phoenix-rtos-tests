/*
 * Phoenix-RTOS
 *
 * test-libc-inet-loopback-tcp
 *
 * TCP over 127.0.0.1 inside ONE process: connect, accept and echo with one
 * thread and with two, and poll() timeouts on an inet socket while another
 * thread of the process is blocked in accept(). This is the shape of CPython's
 * socket tests and of any program that runs a local server thread (the
 * `threading.Thread(target=accept)` + `settimeout()` + `connect()` pattern).
 *
 * On Phoenix each inet socket is served by its own thread of the lwip process
 * and every call is a message to it; poll() on one inet socket lets that thread
 * block until the socket is ready (posix_poll's single-inet-socket path).
 *
 *    TESTED:
 *    - single_thread_nonblocking: O_NONBLOCK connect, poll(listener, POLLIN),
 *      accept, poll(client, POLLOUT), SO_ERROR, one byte each way
 *    - two_threads_blocking: accept() blocks in a server thread, the main thread
 *      connect()s (blocking) and runs ECHO_ROUNDS 32-byte round trips
 *    - two_threads_fionbio_poll: CPython's settimeout() connect -- ioctl(FIONBIO),
 *      connect() = EINPROGRESS, poll(POLLOUT), getsockopt(SO_ERROR) -- while a
 *      server thread blocks in accept(), then echo
 *    - poll_timeout_with_blocked_accept: a thread blocks in accept() on one
 *      listener while the main thread polls an idle connected socket with a
 *      300 ms timeout: poll() must return 0 after about 300 ms
 *
 *    FAILS on any error, and -- instead of hanging -- when a case makes no
 *    progress for WATCHDOG_S seconds: a watchdog thread prints the case and the
 *    step it was in and exits the process with status 2.
 *
 * Copyright 2026 Phoenix Systems
 *
 * %LICENSE%
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "unity_fixture.h"


#define WATCHDOG_S  10
#define ECHO_ROUNDS 500
#define POLL_MS     5000


/* ---- watchdog: a hang fails the run, it does not stall it ---------------- */

static struct {
	pthread_t thread;
	volatile int armed;
	volatile time_t deadline;
	const char *volatile name;
	const char *volatile step;
} wd;


static time_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (time_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


static void *watchdog_thread(void *arg)
{
	char buf[200];
	int n;

	(void)arg;
	for (;;) {
		usleep(100 * 1000);
		if ((wd.armed != 0) && (now_ms() > wd.deadline)) {
			n = snprintf(buf, sizeof(buf), "\nWATCHDOG: %s hung in '%s' for %d s -- FAIL\n",
				wd.name, wd.step, WATCHDOG_S);
			(void)!write(STDERR_FILENO, buf, (size_t)n);
			(void)!write(STDOUT_FILENO, buf, (size_t)n);
			_exit(2);
		}
	}
	return NULL;
}


static void step(const char *what)
{
	wd.step = what;
}


static void watchdog_arm(const char *name)
{
	wd.name = name;
	wd.step = "start";
	wd.deadline = now_ms() + WATCHDOG_S * 1000;
	wd.armed = 1;
}


/* ---- helpers --------------------------------------------------------------- */

static int listener(struct sockaddr_in *addr)
{
	socklen_t len = sizeof(*addr);
	int s, on = 1;

	s = socket(AF_INET, SOCK_STREAM, 0);
	TEST_ASSERT_TRUE(s >= 0);
	(void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = 0;
	addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	TEST_ASSERT_EQUAL_INT(0, bind(s, (struct sockaddr *)addr, sizeof(*addr)));
	TEST_ASSERT_EQUAL_INT(0, listen(s, 4));
	TEST_ASSERT_EQUAL_INT(0, getsockname(s, (struct sockaddr *)addr, &len));
	TEST_ASSERT_NOT_EQUAL_INT(0, ntohs(addr->sin_port));

	return s;
}


/* A non-blocking connect that has completed: POLLOUT, then SO_ERROR == 0 */
static void wait_connected(int c)
{
	struct pollfd pfd = { .fd = c, .events = POLLOUT };
	socklen_t len;
	int err = -1;

	step("poll(client, POLLOUT)");
	TEST_ASSERT_EQUAL_INT(1, poll(&pfd, 1, POLL_MS));
	TEST_ASSERT_TRUE((pfd.revents & POLLOUT) != 0);

	step("getsockopt(SO_ERROR)");
	len = sizeof(err);
	TEST_ASSERT_EQUAL_INT(0, getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &len));
	TEST_ASSERT_EQUAL_INT(0, err);
}


static void echo_rounds(int c, int rounds)
{
	char msg[32], buf[64];
	ssize_t n;
	int i, got;

	step("echo");
	for (i = 0; i < rounds; i++) {
		memset(msg, 'a' + (i % 26), sizeof(msg));
		TEST_ASSERT_EQUAL_INT((int)sizeof(msg), (int)send(c, msg, sizeof(msg), 0));
		for (got = 0; got < (int)sizeof(msg); got += (int)n) {
			n = recv(c, buf + got, sizeof(buf) - (size_t)got, 0);
			TEST_ASSERT_TRUE(n > 0);
		}
		TEST_ASSERT_EQUAL_INT((int)sizeof(msg), got);
		TEST_ASSERT_EQUAL_MEMORY(msg, buf, sizeof(msg));
	}
}


/* Server thread: accept one connection, echo until the peer closes */
struct server {
	pthread_t thread;
	int lsock;
	int echo;
	volatile int accepted;
	volatile int err;
};


static void *server_thread(void *arg)
{
	struct server *srv = arg;
	char buf[64];
	ssize_t n;
	int c;

	c = accept(srv->lsock, NULL, NULL);
	if (c < 0) {
		srv->err = errno;
		return NULL;
	}
	srv->accepted = 1;

	while (srv->echo != 0) {
		n = recv(c, buf, sizeof(buf), 0);
		if (n <= 0) {
			if (n < 0) {
				srv->err = errno;
			}
			break;
		}
		if (send(c, buf, (size_t)n, 0) != n) {
			srv->err = errno;
			break;
		}
	}

	close(c);
	return NULL;
}


static void server_start(struct server *srv, int lsock, int echo)
{
	memset(srv, 0, sizeof(*srv));
	srv->lsock = lsock;
	srv->echo = echo;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&srv->thread, NULL, server_thread, srv));
	/* Give the server thread time to block in accept() first */
	usleep(50 * 1000);
}


static void server_join(struct server *srv)
{
	step("join the server thread");
	TEST_ASSERT_EQUAL_INT(0, pthread_join(srv->thread, NULL));
	TEST_ASSERT_EQUAL_INT(0, srv->err);
	TEST_ASSERT_TRUE(srv->accepted != 0);
}


/* ---- tests ----------------------------------------------------------------- */

TEST_GROUP(inet_loopback_tcp);


TEST_SETUP(inet_loopback_tcp)
{
}


TEST_TEAR_DOWN(inet_loopback_tcp)
{
	wd.armed = 0;
}


TEST(inet_loopback_tcp, single_thread_nonblocking)
{
	struct sockaddr_in addr;
	struct pollfd pfd;
	int l, c, a, err;
	char b = 'p';

	watchdog_arm("single_thread_nonblocking");
	l = listener(&addr);

	step("socket + O_NONBLOCK");
	c = socket(AF_INET, SOCK_STREAM, 0);
	TEST_ASSERT_TRUE(c >= 0);
	TEST_ASSERT_EQUAL_INT(0, fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK));

	step("connect");
	err = connect(c, (struct sockaddr *)&addr, sizeof(addr));
	TEST_ASSERT_TRUE((err == 0) || (errno == EINPROGRESS));

	step("poll(listener, POLLIN)");
	pfd.fd = l;
	pfd.events = POLLIN;
	pfd.revents = 0;
	TEST_ASSERT_EQUAL_INT(1, poll(&pfd, 1, POLL_MS));

	step("accept");
	a = accept(l, NULL, NULL);
	TEST_ASSERT_TRUE(a >= 0);

	wait_connected(c);

	step("one byte each way");
	TEST_ASSERT_EQUAL_INT(1, (int)send(c, &b, 1, 0));
	b = 0;
	TEST_ASSERT_EQUAL_INT(1, (int)recv(a, &b, 1, 0));
	TEST_ASSERT_EQUAL_CHAR('p', b);
	TEST_ASSERT_EQUAL_INT(1, (int)send(a, &b, 1, 0));
	b = 0;
	pfd.fd = c;
	pfd.events = POLLIN;
	pfd.revents = 0;
	TEST_ASSERT_EQUAL_INT(1, poll(&pfd, 1, POLL_MS));
	TEST_ASSERT_EQUAL_INT(1, (int)recv(c, &b, 1, 0));
	TEST_ASSERT_EQUAL_CHAR('p', b);

	close(a);
	close(c);
	close(l);
}


TEST(inet_loopback_tcp, two_threads_blocking)
{
	struct server srv;
	struct sockaddr_in addr;
	int l, c, on = 1;

	watchdog_arm("two_threads_blocking");
	l = listener(&addr);
	server_start(&srv, l, 1);

	step("socket");
	c = socket(AF_INET, SOCK_STREAM, 0);
	TEST_ASSERT_TRUE(c >= 0);
	step("blocking connect");
	TEST_ASSERT_EQUAL_INT(0, connect(c, (struct sockaddr *)&addr, sizeof(addr)));
	(void)setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

	echo_rounds(c, ECHO_ROUNDS);

	close(c);
	server_join(&srv);
	close(l);
}


TEST(inet_loopback_tcp, two_threads_fionbio_poll)
{
	struct server srv;
	struct sockaddr_in addr;
	int l, c, err, nb;

	watchdog_arm("two_threads_fionbio_poll");
	l = listener(&addr);
	server_start(&srv, l, 1);

	step("socket");
	c = socket(AF_INET, SOCK_STREAM, 0);
	TEST_ASSERT_TRUE(c >= 0);

	step("ioctl(FIONBIO, 1)");
	nb = 1;
	TEST_ASSERT_EQUAL_INT(0, ioctl(c, FIONBIO, &nb));
	TEST_ASSERT_TRUE((fcntl(c, F_GETFL) & O_NONBLOCK) != 0);

	step("connect");
	err = connect(c, (struct sockaddr *)&addr, sizeof(addr));
	TEST_ASSERT_TRUE((err == 0) || (errno == EINPROGRESS));

	wait_connected(c);

	step("ioctl(FIONBIO, 0)");
	nb = 0;
	TEST_ASSERT_EQUAL_INT(0, ioctl(c, FIONBIO, &nb));

	echo_rounds(c, ECHO_ROUNDS);

	close(c);
	server_join(&srv);
	close(l);
}


TEST(inet_loopback_tcp, poll_timeout_with_blocked_accept)
{
	struct server srv;
	struct sockaddr_in addr, addr2;
	struct pollfd pfd;
	time_t t0, dt;
	int l, l2, c, a, c2, err;

	watchdog_arm("poll_timeout_with_blocked_accept");

	/* This thread blocks in accept() on l2 until the end of the case */
	l2 = listener(&addr2);
	server_start(&srv, l2, 0);

	/* An idle connected pair, made without a second thread */
	l = listener(&addr);
	step("socket + O_NONBLOCK");
	c = socket(AF_INET, SOCK_STREAM, 0);
	TEST_ASSERT_TRUE(c >= 0);
	TEST_ASSERT_EQUAL_INT(0, fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK));
	step("connect");
	err = connect(c, (struct sockaddr *)&addr, sizeof(addr));
	TEST_ASSERT_TRUE((err == 0) || (errno == EINPROGRESS));
	pfd.fd = l;
	pfd.events = POLLIN;
	pfd.revents = 0;
	step("poll(listener, POLLIN)");
	TEST_ASSERT_EQUAL_INT(1, poll(&pfd, 1, POLL_MS));
	step("accept");
	a = accept(l, NULL, NULL);
	TEST_ASSERT_TRUE(a >= 0);
	wait_connected(c);

	/* Nothing will arrive: poll must time out, neither early nor never */
	step("poll(idle, POLLIN, 300 ms)");
	pfd.fd = c;
	pfd.events = POLLIN;
	pfd.revents = 0;
	t0 = now_ms();
	TEST_ASSERT_EQUAL_INT(0, poll(&pfd, 1, 300));
	dt = now_ms() - t0;
	TEST_ASSERT_TRUE_MESSAGE(dt >= 250, "poll() returned before its timeout");
	TEST_ASSERT_TRUE_MESSAGE(dt < 3000, "poll() overran its timeout by > 2.7 s");

	/* Release the blocked accept() */
	step("connect to release the accept thread");
	c2 = socket(AF_INET, SOCK_STREAM, 0);
	TEST_ASSERT_TRUE(c2 >= 0);
	TEST_ASSERT_EQUAL_INT(0, connect(c2, (struct sockaddr *)&addr2, sizeof(addr2)));
	server_join(&srv);

	close(c2);
	close(a);
	close(c);
	close(l);
	close(l2);
}


TEST_GROUP_RUNNER(inet_loopback_tcp)
{
	RUN_TEST_CASE(inet_loopback_tcp, single_thread_nonblocking);
	RUN_TEST_CASE(inet_loopback_tcp, two_threads_blocking);
	RUN_TEST_CASE(inet_loopback_tcp, two_threads_fionbio_poll);
	RUN_TEST_CASE(inet_loopback_tcp, poll_timeout_with_blocked_accept);
}


static void runner(void)
{
	RUN_TEST_GROUP(inet_loopback_tcp);
}


int main(int argc, char *argv[])
{
	if (pthread_create(&wd.thread, NULL, watchdog_thread, NULL) != 0) {
		fprintf(stderr, "cannot start the watchdog thread\n");
		return EXIT_FAILURE;
	}
	(void)pthread_detach(wd.thread);

	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
