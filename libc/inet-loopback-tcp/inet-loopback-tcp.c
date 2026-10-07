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
 * block until the socket is ready (posix_poll's single-inet-socket path). A
 * call that blocks there must not hold up the socket's other calls: before
 * build 45 one blocked accept() or recv() stalled every later call on the same
 * socket -- even getsockname() -- until it returned.
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
 *    - python_settimeout_connect: CPython 3.14's exact sequence for
 *      `threading.Thread(target=s.accept)` + `c.settimeout(5)` +
 *      `c.connect(s.getsockname())`: accept4(SOCK_CLOEXEC) blocks in a thread,
 *      then socket(SOCK_CLOEXEC), ioctl(FIONBIO), getsockname() ON THE LISTENER,
 *      connect() = EINPROGRESS, poll(POLLOUT | POLLERR, 5000), SO_ERROR
 *    - ops_on_listener_during_accept: getsockname, getsockopt, fcntl(F_GETFL)
 *      and a 0-timeout poll on a listener another thread is blocked accept()ing
 *    - send_while_peer_thread_recvs: one thread blocks in recv() on a socket
 *      while another thread sends on the SAME socket, then the reply arrives
 *    - poll_while_other_thread_recvs: one thread blocks in recv() while another
 *      polls the same socket with a 300 ms timeout
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
	int cpython; /* accept4(SOCK_CLOEXEC) with an address buffer, as CPython */
	volatile int accepted;
	volatile int err;
};


static void *server_thread(void *arg)
{
	struct server *srv = arg;
	char buf[64];
	ssize_t n;
	int c;

	if (srv->cpython != 0) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		c = accept4(srv->lsock, (struct sockaddr *)&peer, &plen, SOCK_CLOEXEC);
	}
	else {
		c = accept(srv->lsock, NULL, NULL);
	}
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


TEST(inet_loopback_tcp, python_settimeout_connect)
{
	struct server srv;
	struct sockaddr_in addr;
	socklen_t len, slen;
	struct pollfd pfd;
	unsigned int nb;
	int l, c, err, serr;

	watchdog_arm("python_settimeout_connect");

	/* s = socket(); s.bind(("127.0.0.1", 0)); s.listen(1) */
	step("listener");
	l = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	TEST_ASSERT_TRUE(l >= 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	TEST_ASSERT_EQUAL_INT(0, bind(l, (struct sockaddr *)&addr, sizeof(addr)));
	TEST_ASSERT_EQUAL_INT(0, listen(l, 1));

	/* Thread(target=s.accept).start(); time.sleep(0.5) */
	memset(&srv, 0, sizeof(srv));
	srv.lsock = l;
	srv.echo = 0;
	srv.cpython = 1;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&srv.thread, NULL, server_thread, &srv));
	usleep(500 * 1000);

	/* c = socket(); c.settimeout(5) -> internal_setblocking(): ioctl(FIONBIO) */
	step("client socket(SOCK_CLOEXEC)");
	c = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	TEST_ASSERT_TRUE(c >= 0);
	step("ioctl(FIONBIO)");
	nb = 1;
	TEST_ASSERT_EQUAL_INT(0, ioctl(c, FIONBIO, &nb));

	/* c.connect(s.getsockname()): the address comes from the LISTENER, on
	 * which the other thread is blocked in accept() */
	step("getsockname(listener) while accept() blocks on it");
	slen = sizeof(addr);
	TEST_ASSERT_EQUAL_INT(0, getsockname(l, (struct sockaddr *)&addr, &slen));

	/* internal_connect(): connect(), EINPROGRESS, then sock_call_ex() ->
	 * internal_select() = poll(POLLOUT | POLLERR, 5000), then
	 * sock_connect_impl() = getsockopt(SO_ERROR) */
	step("connect");
	err = connect(c, (struct sockaddr *)&addr, slen);
	TEST_ASSERT_TRUE((err == 0) || (errno == EINPROGRESS));
	if (err != 0) {
		step("poll(POLLOUT | POLLERR, 5000)");
		pfd.fd = c;
		pfd.events = POLLOUT | POLLERR;
		pfd.revents = 0;
		TEST_ASSERT_EQUAL_INT(1, poll(&pfd, 1, 5000));
		step("getsockopt(SO_ERROR)");
		serr = -1;
		len = sizeof(serr);
		TEST_ASSERT_EQUAL_INT(0, getsockopt(c, SOL_SOCKET, SO_ERROR, &serr, &len));
		TEST_ASSERT_EQUAL_INT(0, serr);
	}

	server_join(&srv);
	close(c);
	close(l);
}


TEST(inet_loopback_tcp, ops_on_listener_during_accept)
{
	struct server srv;
	struct sockaddr_in addr, got;
	struct pollfd pfd;
	socklen_t len;
	int l, c, type = 0;

	watchdog_arm("ops_on_listener_during_accept");
	l = listener(&addr);
	server_start(&srv, l, 0);

	step("getsockname(listener)");
	len = sizeof(got);
	TEST_ASSERT_EQUAL_INT(0, getsockname(l, (struct sockaddr *)&got, &len));
	TEST_ASSERT_EQUAL_UINT16(addr.sin_port, got.sin_port);

	step("getsockopt(listener, SO_TYPE)");
	len = sizeof(type);
	TEST_ASSERT_EQUAL_INT(0, getsockopt(l, SOL_SOCKET, SO_TYPE, &type, &len));
	TEST_ASSERT_EQUAL_INT(SOCK_STREAM, type);

	step("fcntl(listener, F_GETFL)");
	TEST_ASSERT_TRUE(fcntl(l, F_GETFL) >= 0);

	step("poll(listener, POLLIN, 0)");
	pfd.fd = l;
	pfd.events = POLLIN;
	pfd.revents = 0;
	TEST_ASSERT_EQUAL_INT(0, poll(&pfd, 1, 0));

	step("connect to release the accept thread");
	c = socket(AF_INET, SOCK_STREAM, 0);
	TEST_ASSERT_TRUE(c >= 0);
	TEST_ASSERT_EQUAL_INT(0, connect(c, (struct sockaddr *)&addr, sizeof(addr)));
	server_join(&srv);

	close(c);
	close(l);
}


/* A connected pair made in one thread: *pc connected to *pa */
static void connected_pair(int *pl, int *pc, int *pa)
{
	struct sockaddr_in addr;
	int fl;

	*pl = listener(&addr);
	step("pair: connect");
	*pc = socket(AF_INET, SOCK_STREAM, 0);
	TEST_ASSERT_TRUE(*pc >= 0);
	fl = fcntl(*pc, F_GETFL);
	TEST_ASSERT_EQUAL_INT(0, fcntl(*pc, F_SETFL, fl | O_NONBLOCK));
	TEST_ASSERT_TRUE((connect(*pc, (struct sockaddr *)&addr, sizeof(addr)) == 0) || (errno == EINPROGRESS));
	step("pair: accept");
	*pa = accept(*pl, NULL, NULL);
	TEST_ASSERT_TRUE(*pa >= 0);
	wait_connected(*pc);
	TEST_ASSERT_EQUAL_INT(0, fcntl(*pc, F_SETFL, fl));
}


struct receiver {
	pthread_t thread;
	int sock;
	volatile int started;
	volatile ssize_t n;
	volatile int err;
	char buf[16];
};


static void *receiver_thread(void *arg)
{
	struct receiver *r = arg;

	r->started = 1;
	r->n = recv(r->sock, r->buf, sizeof(r->buf), 0);
	r->err = (r->n < 0) ? errno : 0;
	return NULL;
}


TEST(inet_loopback_tcp, send_while_peer_thread_recvs)
{
	struct receiver r;
	int l, c, a;
	char b;

	watchdog_arm("send_while_peer_thread_recvs");
	connected_pair(&l, &c, &a);

	/* A thread blocks in recv(c); this thread sends on c, the peer echoes */
	memset(&r, 0, sizeof(r));
	r.sock = c;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&r.thread, NULL, receiver_thread, &r));
	usleep(100 * 1000);

	step("send(c) while another thread blocks in recv(c)");
	TEST_ASSERT_EQUAL_INT(1, (int)send(c, "q", 1, 0));
	step("recv(a)");
	TEST_ASSERT_EQUAL_INT(1, (int)recv(a, &b, 1, 0));
	TEST_ASSERT_EQUAL_CHAR('q', b);
	step("send(a): wakes the receiver");
	TEST_ASSERT_EQUAL_INT(1, (int)send(a, "r", 1, 0));

	step("join the receiver");
	TEST_ASSERT_EQUAL_INT(0, pthread_join(r.thread, NULL));
	TEST_ASSERT_EQUAL_INT(0, r.err);
	TEST_ASSERT_EQUAL_INT(1, (int)r.n);
	TEST_ASSERT_EQUAL_CHAR('r', r.buf[0]);

	close(a);
	close(c);
	close(l);
}


TEST(inet_loopback_tcp, poll_while_other_thread_recvs)
{
	struct receiver r;
	struct pollfd pfd;
	time_t t0, dt;
	int l, c, a;

	watchdog_arm("poll_while_other_thread_recvs");
	connected_pair(&l, &c, &a);

	memset(&r, 0, sizeof(r));
	r.sock = c;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&r.thread, NULL, receiver_thread, &r));
	usleep(100 * 1000);

	step("poll(c, POLLOUT, 300) while another thread blocks in recv(c)");
	pfd.fd = c;
	pfd.events = POLLOUT;
	pfd.revents = 0;
	TEST_ASSERT_EQUAL_INT(1, poll(&pfd, 1, 300));

	step("poll(c, POLLPRI, 300) while another thread blocks in recv(c)");
	pfd.events = POLLPRI;
	pfd.revents = 0;
	t0 = now_ms();
	TEST_ASSERT_EQUAL_INT(0, poll(&pfd, 1, 300));
	dt = now_ms() - t0;
	TEST_ASSERT_TRUE_MESSAGE(dt < 3000, "poll() overran its timeout by > 2.7 s");

	step("send(a): wakes the receiver");
	TEST_ASSERT_EQUAL_INT(1, (int)send(a, "s", 1, 0));
	step("join the receiver");
	TEST_ASSERT_EQUAL_INT(0, pthread_join(r.thread, NULL));
	TEST_ASSERT_EQUAL_INT(1, (int)r.n);

	close(a);
	close(c);
	close(l);
}


TEST_GROUP_RUNNER(inet_loopback_tcp)
{
	RUN_TEST_CASE(inet_loopback_tcp, single_thread_nonblocking);
	RUN_TEST_CASE(inet_loopback_tcp, two_threads_blocking);
	RUN_TEST_CASE(inet_loopback_tcp, two_threads_fionbio_poll);
	RUN_TEST_CASE(inet_loopback_tcp, poll_timeout_with_blocked_accept);
	RUN_TEST_CASE(inet_loopback_tcp, python_settimeout_connect);
	RUN_TEST_CASE(inet_loopback_tcp, ops_on_listener_during_accept);
	RUN_TEST_CASE(inet_loopback_tcp, send_while_peer_thread_recvs);
	RUN_TEST_CASE(inet_loopback_tcp, poll_while_other_thread_recvs);
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
