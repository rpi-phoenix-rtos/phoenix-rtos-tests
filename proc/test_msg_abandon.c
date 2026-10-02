/*
 * Phoenix-RTOS
 *
 * A client that exits or is killed while a server holds its request
 *
 * HEADER:
 *    - sys/msg.h
 *
 * TESTED:
 *    - msgSend() of a process that exits or is killed after the server's msgRecv() and
 *      before its msgRespond(), msgRespond() to such a request, waitpid() on the client
 *
 * What it checks:
 *   msg_abandon - a client process whose thread is parked in msgSend() on a request that a live
 *     server has received (and does not answer) can still exit, by _exit() from another thread or
 *     by SIGKILL: waitpid() reaps it within ABANDON_REAP_MS. The server's late msgRespond() to that
 *     request fails with -ENOENT, and the port keeps working. The request carries no payload
 *     mapped into the server (no i.data/o.data): only such a request can be abandoned safely.
 *     These tests FAIL on a kernel without "proc: let an exiting sender abandon a received request
 *     without windows" (phoenix-rtos-kernel, 2026-10-02): the client lingers until the server
 *     answers. Their tear-down answers the request, so even there nothing outlives the test.
 *   msg_abandon_keep - what must not change:
 *     a request whose payload is mapped into the server (a page-aligned 4 KiB o.data window) is NOT
 *     abandoned: the client stays until the server answers, then goes. (The kernel names such a
 *     request on the console after 2 s: "proc: pid ... exit waits for tid ... in msgSend ...".)
 *     A signal that the client catches does not end the wait: the answer still arrives.
 *     Round trips with packed, windowed and unaligned payloads deliver the data both ways.
 *     A server that exits holding a request fails it with -EINVAL (kernel 38ad32cf).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/wait.h>

#include <unity_fixture.h>


#define ABANDON_PAGE       4096
#define ABANDON_RECV_MS    3000 /* for the server to receive the request */
#define ABANDON_REAP_MS    3000 /* for an abandoning client to be reaped (immediate on a fixed kernel) */
#define ABANDON_KEEP_MS    3000 /* a client that must stay is checked for this long: past the 2 s report */
#define ABANDON_FINISH_MS  5000 /* for a client to finish once its request is answered */
#define ABANDON_OERR       42   /* o.err of the server's answers */
#define ABANDON_EXIT_OK    7    /* exit status of a child that saw what it expected */
#define ABANDON_EXIT_WRONG 8    /* ... and of one that did not */
#define ABANDON_EXIT_SIDE  9    /* ... and of one that another of its threads ended */


/* The server: one thread that receives one request and answers it only when told to */
static struct {
	uint32_t port;
	pthread_t thread;
	int running;

	msg_t msg;
	msg_rid_t rid;
	volatile int received;   /* 1 once msgRecv() returned the request */
	volatile int respondNow; /* set by the test */
	volatile int responded;  /* 1 once msgRespond() returned */
	volatile int respondRc;

	pid_t child;
	int pipefd[2];
} abandon_common;


static long abandon_nowMs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}


/* Waits up to ms for *flag to become non-zero */
static int abandon_waitFlag(volatile int *flag, long ms)
{
	long end = abandon_nowMs() + ms;

	while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) == 0) {
		if (abandon_nowMs() >= end) {
			return -1;
		}
		usleep(1000);
	}

	return 0;
}


/* Waits up to ms for the child to be reaped: returns its status, or -1 if it is still there */
static int abandon_reap(long ms)
{
	long end = abandon_nowMs() + ms;
	int status;
	pid_t pid;

	for (;;) {
		pid = waitpid(abandon_common.child, &status, WNOHANG);
		if (pid == abandon_common.child) {
			abandon_common.child = -1;
			return status;
		}
		if ((pid < 0) || (abandon_nowMs() >= end)) {
			return -1;
		}
		usleep(1000);
	}
}


static void *abandon_server(void *arg)
{
	(void)arg;

	if (msgRecv(abandon_common.port, &abandon_common.msg, &abandon_common.rid) < 0) {
		__atomic_store_n(&abandon_common.received, -1, __ATOMIC_RELEASE);
		return NULL;
	}
	__atomic_store_n(&abandon_common.received, 1, __ATOMIC_RELEASE);

	(void)abandon_waitFlag(&abandon_common.respondNow, 24L * 3600L * 1000L);

	/* Answer like a device server: echo the input into the output, if there is room */
	if ((abandon_common.msg.o.data != NULL) && (abandon_common.msg.i.data != NULL)) {
		memcpy(abandon_common.msg.o.data, abandon_common.msg.i.data,
				(abandon_common.msg.i.size < abandon_common.msg.o.size) ? abandon_common.msg.i.size : abandon_common.msg.o.size);
	}
	abandon_common.msg.o.err = ABANDON_OERR;
	abandon_common.respondRc = msgRespond(abandon_common.port, &abandon_common.msg, abandon_common.rid);
	__atomic_store_n(&abandon_common.responded, 1, __ATOMIC_RELEASE);

	return NULL;
}


static void abandon_serverStart(void)
{
	abandon_common.received = 0;
	abandon_common.respondNow = 0;
	abandon_common.responded = 0;
	abandon_common.respondRc = 1;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&abandon_common.thread, NULL, abandon_server, NULL));
	abandon_common.running = 1;
}


/* Lets the server answer, and returns what its msgRespond() returned */
static int abandon_serverRespond(void)
{
	__atomic_store_n(&abandon_common.respondNow, 1, __ATOMIC_RELEASE);
	if (abandon_waitFlag(&abandon_common.responded, ABANDON_FINISH_MS) < 0) {
		return 1;
	}
	return abandon_common.respondRc;
}


/* The child's helper thread: _exit()s when the parent writes to the pipe */
static void *abandon_childExiter(void *arg)
{
	char c;

	(void)arg;
	while (read(abandon_common.pipefd[0], &c, 1) != 1) {
	}
	_exit(ABANDON_EXIT_SIDE);

	return NULL;
}


static void abandon_onSignal(int sig)
{
	(void)sig;
}


/*
 * Forks a client that sends one mtDevCtl request with the given output buffer to the server. With
 * exiter, a second thread of the client _exit()s when the parent writes to the pipe. With
 * catchSignal, the client catches SIGUSR1. The client _exit()s ABANDON_EXIT_OK if its msgSend()
 * came back with the server's answer.
 */
static void abandon_forkClient(void *odata, size_t osize, int exiter, int catchSignal)
{
	pthread_t t;
	msg_t msg;
	int rc;

	TEST_ASSERT_EQUAL_INT(0, pipe(abandon_common.pipefd));

	abandon_common.child = fork();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, abandon_common.child);

	if (abandon_common.child == 0) {
		if (catchSignal != 0) {
			signal(SIGUSR1, abandon_onSignal);
		}
		if ((exiter != 0) && (pthread_create(&t, NULL, abandon_childExiter, NULL) != 0)) {
			_exit(ABANDON_EXIT_WRONG);
		}

		memset(&msg, 0, sizeof(msg));
		msg.type = mtDevCtl;
		msg.o.data = odata;
		msg.o.size = osize;
		rc = msgSend(abandon_common.port, &msg);
		_exit(((rc == 0) && (msg.o.err == ABANDON_OERR)) ? ABANDON_EXIT_OK : ABANDON_EXIT_WRONG);
	}
}


TEST_GROUP(msg_abandon);
TEST_GROUP(msg_abandon_keep);


static void abandon_setUp(void)
{
	abandon_common.running = 0;
	abandon_common.child = -1;
	abandon_common.pipefd[0] = -1;
	abandon_common.pipefd[1] = -1;
	TEST_ASSERT_EQUAL_INT(0, portCreate(&abandon_common.port));
}


/* Also cleans up after a failed test: answers a held request, so that even a client that could not
 * abandon it goes, and reaps that client */
static void abandon_tearDown(void)
{
	if (abandon_common.running != 0) {
		if (abandon_common.received == 0) {
			/* No request came: the server is still in msgRecv. Send it one to answer. */
			msg_t msg;

			memset(&msg, 0, sizeof(msg));
			__atomic_store_n(&abandon_common.respondNow, 1, __ATOMIC_RELEASE);
			(void)msgSend(abandon_common.port, &msg);
		}
		else {
			(void)abandon_serverRespond();
		}
		pthread_join(abandon_common.thread, NULL);
		abandon_common.running = 0;
	}

	if (abandon_common.child > 0) {
		(void)kill(abandon_common.child, SIGKILL);
		(void)abandon_reap(ABANDON_FINISH_MS);
	}

	if (abandon_common.pipefd[0] >= 0) {
		close(abandon_common.pipefd[0]);
		close(abandon_common.pipefd[1]);
	}

	portDestroy(abandon_common.port);
}


TEST_SETUP(msg_abandon)
{
	abandon_setUp();
}


TEST_TEAR_DOWN(msg_abandon)
{
	abandon_tearDown();
}


TEST_SETUP(msg_abandon_keep)
{
	abandon_setUp();
}


TEST_TEAR_DOWN(msg_abandon_keep)
{
	abandon_tearDown();
}


/* After an abandoned request: the server's answer is refused, and the port still works */
static void abandon_checkLateRespond(void)
{
	msg_t msg;
	long start;

	TEST_ASSERT_EQUAL_INT(-ENOENT, abandon_serverRespond());
	pthread_join(abandon_common.thread, NULL);
	abandon_common.running = 0;

	abandon_serverStart();
	memset(&msg, 0, sizeof(msg));
	msg.type = mtDevCtl;
	__atomic_store_n(&abandon_common.respondNow, 1, __ATOMIC_RELEASE);
	start = abandon_nowMs();
	TEST_ASSERT_EQUAL_INT(0, msgSend(abandon_common.port, &msg));
	TEST_ASSERT_EQUAL_INT(ABANDON_OERR, msg.o.err);
	TEST_ASSERT_LESS_THAN_INT(ABANDON_FINISH_MS, (int)(abandon_nowMs() - start));
	pthread_join(abandon_common.thread, NULL);
	abandon_common.running = 0;
}


TEST(msg_abandon, exit_while_received)
{
	int status;

	abandon_serverStart();
	abandon_forkClient(NULL, 0, 1, 0);
	TEST_ASSERT_EQUAL_INT(0, abandon_waitFlag(&abandon_common.received, ABANDON_RECV_MS));
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, abandon_common.received, "the server's msgRecv() failed");
	TEST_ASSERT_EQUAL_INT(abandon_common.child, abandon_common.msg.pid);

	/* Another thread of the client exits the process */
	TEST_ASSERT_EQUAL_INT(1, (int)write(abandon_common.pipefd[1], "x", 1));
	status = abandon_reap(ABANDON_REAP_MS);
	TEST_ASSERT_MESSAGE(status != -1, "the client did not go: its thread is still in msgSend");
	TEST_ASSERT_TRUE(WIFEXITED(status));
	TEST_ASSERT_EQUAL_INT(ABANDON_EXIT_SIDE, WEXITSTATUS(status));

	abandon_checkLateRespond();
}


TEST(msg_abandon, kill_while_received)
{
	int status;

	abandon_serverStart();
	abandon_forkClient(NULL, 0, 0, 0);
	TEST_ASSERT_EQUAL_INT(0, abandon_waitFlag(&abandon_common.received, ABANDON_RECV_MS));
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, abandon_common.received, "the server's msgRecv() failed");

	TEST_ASSERT_EQUAL_INT(0, kill(abandon_common.child, SIGKILL));
	status = abandon_reap(ABANDON_REAP_MS);
	TEST_ASSERT_MESSAGE(status != -1, "the client did not go: its thread is still in msgSend");
	TEST_ASSERT_TRUE(WIFSIGNALED(status));
	TEST_ASSERT_EQUAL_INT(SIGKILL, WTERMSIG(status));

	abandon_checkLateRespond();
}


TEST(msg_abandon_keep, window_waits_for_answer)
{
	void *page;
	int status;

	page = mmap(NULL, ABANDON_PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, page);

	abandon_serverStart();
	abandon_forkClient(page, ABANDON_PAGE, 0, 0);
	TEST_ASSERT_EQUAL_INT(0, abandon_waitFlag(&abandon_common.received, ABANDON_RECV_MS));
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, abandon_common.received, "the server's msgRecv() failed");
	TEST_ASSERT_NOT_NULL(abandon_common.msg.o.data);
	TEST_ASSERT_EQUAL_INT(ABANDON_PAGE, (int)abandon_common.msg.o.size);

	/* The server can still write the window: it maps the client's own page */
	memset(abandon_common.msg.o.data, 0x5a, ABANDON_PAGE);

	TEST_ASSERT_EQUAL_INT(0, kill(abandon_common.child, SIGKILL));
	TEST_ASSERT_EQUAL_INT_MESSAGE(-1, abandon_reap(ABANDON_KEEP_MS), "a request with a payload window was abandoned");
	memset(abandon_common.msg.o.data, 0xa5, ABANDON_PAGE);

	TEST_ASSERT_EQUAL_INT(0, abandon_serverRespond());
	status = abandon_reap(ABANDON_FINISH_MS);
	TEST_ASSERT_MESSAGE(status != -1, "the client did not go after its answer");
	TEST_ASSERT_TRUE(WIFSIGNALED(status));

	munmap(page, ABANDON_PAGE);
}


TEST(msg_abandon_keep, signal_does_not_abandon)
{
	int status;

	abandon_serverStart();
	abandon_forkClient(NULL, 0, 0, 1);
	TEST_ASSERT_EQUAL_INT(0, abandon_waitFlag(&abandon_common.received, ABANDON_RECV_MS));
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, abandon_common.received, "the server's msgRecv() failed");

	/* Caught: the client's msgSend() keeps waiting */
	TEST_ASSERT_EQUAL_INT(0, kill(abandon_common.child, SIGUSR1));
	TEST_ASSERT_EQUAL_INT_MESSAGE(-1, abandon_reap(500), "a caught signal ended the client");

	TEST_ASSERT_EQUAL_INT(0, abandon_serverRespond());
	status = abandon_reap(ABANDON_FINISH_MS);
	TEST_ASSERT_MESSAGE(status != -1, "the client did not go after its answer");
	TEST_ASSERT_TRUE(WIFEXITED(status));
	TEST_ASSERT_EQUAL_INT_MESSAGE(ABANDON_EXIT_OK, WEXITSTATUS(status), "the client's msgSend() did not return the answer");
}


/* One request with the given payloads through the server thread: it echoes i into o */
static void abandon_roundTrip(int type, void *idata, size_t isize, void *odata, size_t osize)
{
	msg_t msg;

	abandon_serverStart();
	memset(&msg, 0, sizeof(msg));
	msg.type = type;
	msg.i.data = idata;
	msg.i.size = isize;
	msg.o.data = odata;
	msg.o.size = osize;
	__atomic_store_n(&abandon_common.respondNow, 1, __ATOMIC_RELEASE);
	TEST_ASSERT_EQUAL_INT(0, msgSend(abandon_common.port, &msg));
	TEST_ASSERT_EQUAL_INT(ABANDON_OERR, msg.o.err);
	pthread_join(abandon_common.thread, NULL);
	abandon_common.running = 0;
}


TEST(msg_abandon_keep, round_trips)
{
	static const size_t sizes[] = { 1, 40, 64, 100, ABANDON_PAGE, 5000 };
	static const size_t offsets[] = { 0, 16, ABANDON_PAGE - 55 };
	static const int types[] = { mtDevCtl, mtRead };
	char *in, *out;
	size_t s, o, t, n, k;

	in = mmap(NULL, 4 * ABANDON_PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	out = mmap(NULL, 4 * ABANDON_PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, in);
	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, out);

	for (t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
		for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
			for (o = 0; o < sizeof(offsets) / sizeof(offsets[0]); o++) {
				n = sizes[s];
				for (k = 0; k < n; k++) {
					in[offsets[o] + k] = (char)(k * 13U + s + t);
				}
				memset(out, 0, 4 * ABANDON_PAGE);

				/* The output starts at another offset than the input, so the two take different paths */
				abandon_roundTrip(types[t], in + offsets[o], n, out + offsets[(o + 1U) % 3U], n);
				TEST_ASSERT_EQUAL_MEMORY(in + offsets[o], out + offsets[(o + 1U) % 3U], n);
			}
		}
	}

	munmap(in, 4 * ABANDON_PAGE);
	munmap(out, 4 * ABANDON_PAGE);
}


/* The child is the server here: it receives one request and exits without answering */
static void *abandon_sendToChild(void *arg)
{
	msg_t msg;

	(void)arg;
	memset(&msg, 0, sizeof(msg));
	msg.type = mtDevCtl;
	abandon_common.respondRc = msgSend(abandon_common.port, &msg);
	__atomic_store_n(&abandon_common.responded, 1, __ATOMIC_RELEASE);

	return NULL;
}


TEST(msg_abandon_keep, server_exit_fails_request)
{
	pthread_t t;
	uint32_t port;
	msg_t msg;
	msg_rid_t rid;
	int status;

	TEST_ASSERT_EQUAL_INT(0, pipe(abandon_common.pipefd));
	abandon_common.child = fork();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, abandon_common.child);

	if (abandon_common.child == 0) {
		if ((portCreate(&port) < 0) || (write(abandon_common.pipefd[1], &port, sizeof(port)) != sizeof(port))) {
			_exit(ABANDON_EXIT_WRONG);
		}
		if (msgRecv(port, &msg, &rid) < 0) {
			_exit(ABANDON_EXIT_WRONG);
		}
		_exit(ABANDON_EXIT_OK);
	}

	TEST_ASSERT_EQUAL_INT(sizeof(port), read(abandon_common.pipefd[0], &port, sizeof(port)));

	/* This test's own port goes; the sender uses the child's */
	portDestroy(abandon_common.port);
	abandon_common.port = port;
	abandon_common.responded = 0;
	abandon_common.respondRc = 1;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, abandon_sendToChild, NULL));

	status = abandon_reap(ABANDON_FINISH_MS);
	TEST_ASSERT_MESSAGE(status != -1, "the server child did not exit");
	TEST_ASSERT_TRUE(WIFEXITED(status));
	TEST_ASSERT_EQUAL_INT(ABANDON_EXIT_OK, WEXITSTATUS(status));

	/* On a kernel without 38ad32cf the sender never returns, and neither does this test */
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, abandon_waitFlag(&abandon_common.responded, ABANDON_FINISH_MS), "the request outlived its server");
	TEST_ASSERT_EQUAL_INT(-EINVAL, abandon_common.respondRc);
	pthread_join(t, NULL);
}


TEST_GROUP_RUNNER(msg_abandon)
{
	RUN_TEST_CASE(msg_abandon, exit_while_received);
	RUN_TEST_CASE(msg_abandon, kill_while_received);
}


TEST_GROUP_RUNNER(msg_abandon_keep)
{
	RUN_TEST_CASE(msg_abandon_keep, window_waits_for_answer);
	RUN_TEST_CASE(msg_abandon_keep, signal_does_not_abandon);
	RUN_TEST_CASE(msg_abandon_keep, round_trips);
	RUN_TEST_CASE(msg_abandon_keep, server_exit_fails_request);
}


static void runner(void)
{
	RUN_TEST_GROUP(msg_abandon);
	RUN_TEST_GROUP(msg_abandon_keep);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
