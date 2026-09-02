/*
 * Phoenix-RTOS
 *
 * test-libc-socket
 *
 * unix socket tests
 *
 * Copyright 2021, 2024 Phoenix Systems
 * Author: Ziemowit Leszczynski, Adam Debek, Adam Greloch
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include "common.h"
#include "unity_fixture.h"

#define BAD_FD 33333 /* should be bad descriptor */


/* A child that fails says so before it goes.
 *
 * Both of these used to exit(1) silently, so every child-side failure reached
 * the parent as nothing but a status code -- and the parent only asserts
 * WIFEXITED/WEXITSTATUS, so the log showed "Expected 0 Was 1" with no clue
 * which check failed or why. On the rpi4b port, where these tests are being
 * used to chase an intermittent fault, that turned every child failure into a
 * dead end. errno is included because the failing predicate is almost always a
 * syscall result. */
#define CHILD_FAIL(what) \
	do { \
		fprintf(stderr, "CHILD-FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__, (what), errno); \
		fflush(stderr); \
		exit(1); \
	} while (0)


#define FAIL_OR_EXIT(pid, msg) \
	do { \
		if (pid != 0) \
			FAIL(msg); \
		else \
			CHILD_FAIL(msg); \
	} while (0)


#define CHILD_ASSERT(pred) \
	do { \
		if (!(pred)) { \
			CHILD_FAIL(#pred); \
		} \
	} while (0)


#define MS_BETWEEN(ts0, ts1) \
	((ts1).tv_sec - (ts0).tv_sec) * 1000 + ((ts1).tv_nsec - (ts0).tv_nsec) / 1000000;

static char data[DATA_SIZE];
static char buf[DATA_SIZE];


/* Checksum of data[] as it was when TEST_SETUP filled it. data[] is random, so
 * there is no generator to re-derive it from -- but a checksum taken at fill
 * time answers the only question that matters when a comparison fails: was the
 * expected pattern still intact? */
static unsigned long data_sum;

static unsigned long unix_data_checksum(void)
{
	unsigned long h = 1469598103934665603UL; /* FNV-1a 64-bit offset basis */
	size_t i;

	for (i = 0; i < sizeof(data); ++i) {
		h ^= (unsigned long)(unsigned char)data[i];
		h *= 1099511628211UL;
	}

	return h;
}


static int pollTimeoutDelay = 30;
static int transferLoopCnt = TRANSFER_LOOP_CNT;


static ssize_t unix_named_socket(int type, const char *name)
{
	int fd;
	struct sockaddr_un addr = { 0 };

	unlink(name);

	if ((fd = socket(AF_UNIX, type, 0)) < 0)
		return -1;

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, name);

	if (bind(fd, (struct sockaddr *)&addr, SUN_LEN(&addr)) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}


static int connect_to_named(int fd, const char *name)
{
	struct sockaddr_un addr = { 0 };

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, name);

	return connect(fd, (struct sockaddr *)&addr, SUN_LEN(&addr));
}


static int unlink_files(size_t cnt)
{
	size_t i;
	char buf[64];

	for (i = 0; i < cnt; ++i) {
		snprintf(buf, sizeof(buf), "/tmp/test_file_%zu", i);
		if (unlink(buf) < 0)
			return -1;
	}

	return 0;
}


static pid_t safe_fork(void)
{
	pid_t pid;
	if ((pid = fork()) < 0) {
		if (errno == ENOSYS) {
			TEST_IGNORE_MESSAGE("fork syscall not supported");
		}
		else {
			FAIL("fork");
		}
	}
	return pid;
}


/* The children in this suite die with a Data Abort at libphoenix's
 * fwrite() -> mutexLock(stream->lock), far=0x30, because a stdio FILE* global
 * is NULL. stdin/stdout/stderr are heap pointers stored in .data
 * (libphoenix stdio/file.c:57, allocated once in _file_init), so NULL means
 * the GLOBAL was zeroed after startup.
 *
 * These checks name the operation that zeroes it. They report with a raw
 * write(2) and _exit(): stdio is the casualty, so any fprintf here would fault
 * before the message reached the log -- which is exactly how this hid for so
 * long (a child that crashes while reporting looks like a silent crash).
 */
static FILE *stdio_snap[3];

static void stdio_snapshot(void)
{
	stdio_snap[0] = stdin;
	stdio_snap[1] = stdout;
	stdio_snap[2] = stderr;
}


static void stdio_verify(const char *where)
{
	static const char *const names[3] = { "stdin", "stdout", "stderr" };
	FILE *now[3];
	char msg[192];
	int i, n;

	now[0] = stdin;
	now[1] = stdout;
	now[2] = stderr;

	for (i = 0; i < 3; i++) {
		if (now[i] == stdio_snap[i]) {
			continue;
		}
		n = snprintf(msg, sizeof(msg), "STDIO-CLOBBER at %s: %s was %p now %p\n",
			where, names[i], (void *)stdio_snap[i], (void *)now[i]);
		if (n > 0) {
			(void)write(2, msg, (size_t)n);
		}
		_exit(97);
	}
}


TEST_GROUP(test_unix_socket);


TEST_SETUP(test_unix_socket)
{
	stdio_snapshot();
	size_t i;

	srandom(time(NULL));

	for (i = 0; i < sizeof(data); i++) {
		data[i] = rand();
	}

	data_sum = unix_data_checksum();
}


TEST_TEAR_DOWN(test_unix_socket)
{
	/* Flush after every test so a run that dies or stalls still shows how far
	 * it got. Without this, a suite whose stdout ends up fully buffered (the
	 * console is not always a tty at the moment the shell starts it) produces
	 * NO output at all until it exits -- which is indistinguishable, in the
	 * log, from a binary that never ran. That ambiguity has cost real time on
	 * the rpi4b port, where these tests are used to chase an intermittent
	 * fault. Unity's own per-test line is what makes progress visible; this
	 * only guarantees it reaches the console. */
	fflush(stdout);
}


TEST(test_unix_socket, zero_len_send)
{
	int fd[3];
	struct msghdr msg;
	struct iovec iov;
	struct cmsghdr *cmsg;
	union {
		char buf[CMSG_SPACE(sizeof(int)) * 3];
		struct cmsghdr align;
	} u;
	ssize_t n;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) < 0) {
		FAIL("socketpair");
	}

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

		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 0;
		msg.msg_control = u.buf;
		msg.msg_controllen = CMSG_LEN(sizeof(int) * 2);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 2);
		memcpy(CMSG_DATA(cmsg), fd, sizeof(int) * 2);
		n = sendmsg(fd[0], &msg, 0);
		TEST_ASSERT(n == 0);

		fd[2] = BAD_FD;
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 0;
		msg.msg_control = u.buf;
		msg.msg_controllen = CMSG_LEN(sizeof(int) * 3);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 3);
		memcpy(CMSG_DATA(cmsg), fd, sizeof(int) * 3);
		/* NOTE: control data should be validated in any case */
		n = sendmsg(fd[0], &msg, 0);
		TEST_ASSERT(n == -1);
		TEST_ASSERT(errno == EBADF);
	}

	close(fd[0]);
	close(fd[1]);
}


TEST(test_unix_socket, zero_len_recv)
{
	int fd[2];
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) < 0)
		FAIL("socketpair");

		/* NOTE: receiving should block on zero len hence we use O_NONBLOCK or MSG_DONTWAIT below */

#if 0
	/* read */
	{
		if (set_nonblock(fd[1], 1) < 0)
			FAIL("set_nonblock");

		n = read(fd[1], NULL, 0);
		TEST_ASSERT(n == -1);
		TEST_ASSERT(errno == EAGAIN);

		if (set_nonblock(fd[1], 0) < 0)
			FAIL("set_nonblock");
	}

	/* readv - fails */
	{
		if (set_nonblock(fd[1], 1) < 0)
			FAIL("set_nonblock");

		n = readv(fd[1], NULL, 0);
		TEST_ASSERT(n == -1);
		TEST_ASSERT(errno == EINVAL);

		n = readv(fd[1], &iov, 0);
		TEST_ASSERT(n == -1);
		TEST_ASSERT(errno == EINVAL);

		iov.iov_base = NULL;
		iov.iov_len = 0;
		n = readv(fd[1], &iov, 1);
		TEST_ASSERT(n == -1);
		TEST_ASSERT(errno == EAGAIN);

		if (set_nonblock(fd[1], 0) < 0)
			FAIL("set_nonblock");
	}
#endif

	/* recv */
	n = recv(fd[1], NULL, 0, MSG_DONTWAIT);
	TEST_ASSERT(n == -1);
	TEST_ASSERT(errno == EAGAIN);

	/* recvfrom */
	n = recvfrom(fd[1], NULL, 0, MSG_DONTWAIT, NULL, 0);
	TEST_ASSERT(n == -1);
	TEST_ASSERT(errno == EAGAIN);

	/* recvmsg */
	{
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = NULL;
		msg.msg_iovlen = 0;
		n = recvmsg(fd[1], &msg, MSG_DONTWAIT);
		TEST_ASSERT(n == -1);
		TEST_ASSERT(errno == EAGAIN);

		memset(&msg, 0, sizeof(msg));
		iov.iov_base = NULL;
		iov.iov_len = 0;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		n = recvmsg(fd[1], &msg, MSG_DONTWAIT);
		TEST_ASSERT(n == -1);
		TEST_ASSERT(errno == EAGAIN);
	}

	close(fd[0]);
	close(fd[1]);
}


TEST(test_unix_socket, close)
{
	unsigned int i;
	int fd[2];
	ssize_t n;
	const char *socket_name = "/tmp/test_close";

	for (i = 0; i < CLOSE_LOOP_CNT; ++i) {
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) < 0)
			FAIL("socketpair");

		n = close(fd[0]);
		TEST_ASSERT(n == 0);
		n = close(fd[1]);
		TEST_ASSERT(n == 0);
	}
	// TODO: check memory leak

	for (i = 0; i < CLOSE_LOOP_CNT; ++i) {
		if ((fd[0] = unix_named_socket(SOCK_DGRAM, socket_name)) < 0)
			FAIL("unix_named_socket");

		n = close(fd[0]);
		TEST_ASSERT(n == 0);
	}
	// TODO: check memory leak

	for (i = 0; i < CLOSE_LOOP_CNT; ++i) {
		int sfd, rfd;
		size_t rfdcnt;

		if ((sfd = unix_named_socket(SOCK_DGRAM, socket_name)) < 0)
			FAIL("unix_named_socket");

		if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) < 0)
			FAIL("socketpair");

		n = msg_send(fd[0], data, 1, &sfd, 1);
		TEST_ASSERT(n == 1);

		n = msg_recv(fd[1], buf, sizeof(buf), &rfd, &rfdcnt);
		TEST_ASSERT(n == 1);
		TEST_ASSERT(rfdcnt == 1);

		n = close(rfd);
		TEST_ASSERT(n == 0);
		n = close(sfd);
		TEST_ASSERT(n == 0);
		n = close(fd[0]);
		TEST_ASSERT(n == 0);
		n = close(fd[1]);
		TEST_ASSERT(n == 0);
	}
	// TODO: check memory leak
	unlink(socket_name);
}


static void unix_msg_data_only(int type)
{
	unsigned int i;
	int fd[2];
	ssize_t n, m, r, sum = 0;
	size_t fdcnt = 0;

	if (socketpair(AF_UNIX, type | SOCK_NONBLOCK, 0, fd) < 0)
		FAIL("socketpair");

	for (i = 0; i < SENDMSG_LOOP_CNT; ++i, sum = 0) {
		m = 1 + rand() % sizeof(data);

		while (sum != m) {
			errno = 0;
			n = msg_send(fd[0], data, m - sum, NULL, fdcnt);
			if (n < 0) {
				TEST_ASSERT(errno = EMSGSIZE);
				break;
			}
			TEST_ASSERT(n >= 0 && errno == 0);

			r = msg_recv(fd[1], buf, sizeof(buf), NULL, &fdcnt);
			TEST_ASSERT(n == r);
			TEST_ASSERT(fdcnt == 0);
			TEST_ASSERT(memcmp(data, buf, n) == 0);

			sum += n;
		}
	}

	close(fd[0]);
	close(fd[1]);
}


TEST(test_unix_socket, msg_data_only)
{
	unix_msg_data_only(SOCK_STREAM);
	unix_msg_data_only(SOCK_DGRAM);
}


static void unix_msg_data_and_fd(int type)
{
	int i;
	int fd[2];
	int sfd[MAX_FD_CNT];
	int rfd[MAX_FD_CNT];
	ssize_t n, m, r, sum = 0;
	size_t sfdcnt, rfdcnt;

	if (socketpair(AF_UNIX, type | SOCK_NONBLOCK, 0, fd) < 0)
		FAIL("socketpair");

	for (i = 0; i < SENDMSG_LOOP_CNT; ++i) {
		m = 1 + rand() % DATA_SIZE;
		sfdcnt = rand() % (MAX_FD_CNT + 1);

		if (open_files(sfd, sfdcnt) < 0)
			FAIL("open_files");

		while (sum != m) {
			errno = 0;
			n = msg_send(fd[0], data, m - sum, sfd, sfdcnt);
			if (n < 0) {
				TEST_ASSERT(errno = EMSGSIZE);

				if (close_files(sfd, sfdcnt) < 0)
					FAIL("close_files");
				if (unlink_files(sfdcnt) < 0)
					FAIL("unlink_files");

				break;
			}
			TEST_ASSERT(n >= 0 && errno == 0);

			if (close_files(sfd, sfdcnt) < 0)
				FAIL("close_files");

			r = msg_recv(fd[1], buf, sizeof(buf), rfd, &rfdcnt);
			TEST_ASSERT(n == r);
			TEST_ASSERT(rfdcnt == sfdcnt);
			TEST_ASSERT(memcmp(data, buf, n) == 0);

			if (close_files(rfd, rfdcnt) < 0)
				FAIL("close_files");

			if (stat_files(sfd, sfdcnt, 0) < 0)
				FAIL("stat_files");

			if (stat_files(rfd, rfdcnt, 0) < 0)
				FAIL("stat_files");

			if (unlink_files(rfdcnt) < 0)
				FAIL("unlink_files");

			sum += n;
		}
	}

	close(fd[0]);
	close(fd[1]);
}


TEST(test_unix_socket, stream_sock_data_and_fd)
{
	unix_msg_data_and_fd(SOCK_STREAM);
}


TEST(test_unix_socket, dgram_sock_data_and_fd)
{
	unix_msg_data_and_fd(SOCK_DGRAM);
}


/*
 * When passing a file descriptor with sendmsg(), file status flags are shared
 * with the receiver, while file descriptor flags (such as FD_CLOEXEC) are not.
 */
static void unix_msg_fd_flags(int type)
{
	int fd[2];
	int oflags[] = {
		O_WRONLY,
		O_RDWR,
		O_RDWR | O_NONBLOCK,
		O_RDWR | O_APPEND,
		O_RDWR | O_CLOEXEC
	};
	pid_t pid;
	size_t sfdcnt, rfdcnt;


	sfdcnt = sizeof(oflags) / sizeof(oflags[0]);

	if (socketpair(AF_UNIX, type, 0, fd) < 0)
		FAIL("socketpair");

	pid = safe_fork();

	if (pid != 0) {
		int sfd[MAX_FD_CNT];
		ssize_t n;
		int status, flags;

		if (open_files_with_flags(sfd, oflags, sfdcnt) < 0)
			FAIL("open_files");

		flags = get_flags(sfd[0]);
		TEST_ASSERT(flags >= 0);
		TEST_ASSERT((flags & O_ACCMODE) == O_WRONLY);
		flags = get_flags(sfd[1]);
		TEST_ASSERT(flags >= 0);
		TEST_ASSERT((flags & O_ACCMODE) == O_RDWR);
		flags = get_flags(sfd[2]);
		TEST_ASSERT(flags >= 0);
		TEST_ASSERT((flags & O_NONBLOCK) != 0);
		flags = get_flags(sfd[3]);
		TEST_ASSERT(flags >= 0);
		TEST_ASSERT((flags & O_APPEND) != 0);
		flags = get_fd_flags(sfd[4]);
		TEST_ASSERT(flags >= 0);
		TEST_ASSERT((flags & FD_CLOEXEC) != 0);

		n = msg_send(fd[0], data, 1, sfd, sfdcnt);
		TEST_ASSERT(n == 1);

		if (close_files(sfd, sfdcnt) < 0)
			FAIL("close_files");

		if (waitpid(pid, &status, 0) < 0)
			FAIL("waitpid");

		TEST_ASSERT(WIFEXITED(status));
		TEST_ASSERT(WEXITSTATUS(status) == 0);

		if (stat_files(sfd, sfdcnt, 0) < 0)
			FAIL("stat_files");

		if (unlink_files(sfdcnt) < 0)
			FAIL("unlink_files");

		close(fd[0]);
		close(fd[1]);
	}
	else {
		int rfd[MAX_FD_CNT];
		ssize_t n;
		int flags;

		n = msg_recv(fd[1], buf, sizeof(buf), rfd, &rfdcnt);
		if (n != 1 || rfdcnt != sfdcnt)
			exit(1);

		flags = get_flags(rfd[0]);
		CHILD_ASSERT(flags >= 0);
		CHILD_ASSERT((flags & O_ACCMODE) == O_WRONLY);
		flags = get_flags(rfd[1]);
		CHILD_ASSERT(flags >= 0);
		CHILD_ASSERT((flags & O_ACCMODE) == O_RDWR);
		flags = get_flags(rfd[2]);
		CHILD_ASSERT(flags >= 0);
		CHILD_ASSERT((flags & O_NONBLOCK) != 0);
		flags = get_flags(rfd[3]);
		CHILD_ASSERT(flags >= 0);
		CHILD_ASSERT((flags & O_APPEND) != 0);
		flags = get_fd_flags(rfd[4]);
		CHILD_ASSERT(flags >= 0);
		CHILD_ASSERT((flags & FD_CLOEXEC) == 0);

		if (close_files(rfd, rfdcnt) < 0)
			exit(2);

		if (stat_files(rfd, rfdcnt, 0) < 0)
			exit(3);

		exit(0);
	}
}


TEST(test_unix_socket, stream_sock_fd_flags)
{
	unix_msg_fd_flags(SOCK_STREAM);
}


TEST(test_unix_socket, dgram_sock_fd_flags)
{
	unix_msg_fd_flags(SOCK_DGRAM);
}


static void unix_msg_fork(int type)
{
	int fd[2];
	pid_t pid;
	size_t sfdcnt, rfdcnt;

	sfdcnt = rand() % (MAX_FD_CNT + 1);

	if (socketpair(AF_UNIX, type, 0, fd) < 0)
		FAIL("socketpair");

	pid = safe_fork();

	if (pid != 0) {
		int sfd[MAX_FD_CNT];
		ssize_t n;
		int status;

		if (open_files(sfd, sfdcnt) < 0)
			FAIL("open_files");

		if (write_files(sfd, sfdcnt, data) < 0)
			FAIL("write_files");

		n = msg_send(fd[0], data, 1, sfd, sfdcnt);
		TEST_ASSERT(n == 1);

		if (close_files(sfd, sfdcnt) < 0)
			FAIL("close_files");

		if (waitpid(pid, &status, 0) < 0)
			FAIL("waitpid");

		TEST_ASSERT(WIFEXITED(status));
		TEST_ASSERT(WEXITSTATUS(status) == 0);

		if (stat_files(sfd, sfdcnt, 0) < 0)
			FAIL("stat_files");

		if (unlink_files(sfdcnt) < 0)
			FAIL("unlink_files");

		close(fd[0]);
		close(fd[1]);
	}
	else {
		int rfd[MAX_FD_CNT];
		ssize_t n;

		n = msg_recv(fd[1], buf, sizeof(buf), rfd, &rfdcnt);
		if (n != 1 || rfdcnt != sfdcnt)
			exit(1);

		if (read_files(rfd, rfdcnt, data, buf) < 0)
			exit(1);

		if (close_files(rfd, rfdcnt) < 0)
			exit(2);

		if (stat_files(rfd, rfdcnt, 0) < 0)
			exit(1);

		exit(0);
	}
}


TEST(test_unix_socket, stream_sock_msg_fork)
{
	unsigned int i;

	for (i = 0; i < FORK_LOOP_CNT; ++i) {
		unix_msg_fork(SOCK_STREAM);
	}
}


TEST(test_unix_socket, dgram_sock_msg_fork)
{
	unsigned int i;

	for (i = 0; i < FORK_LOOP_CNT; ++i) {
		unix_msg_fork(SOCK_DGRAM);
	}
}


static void unix_data_report(const char *buf_, size_t pos, size_t len, size_t i)
{
	unsigned long now = unix_data_checksum();
	size_t k, base = (i > 4u) ? (i - 4u) : 0u;

	/* The decisive datum: if data[] still checksums, the socket really
	 * delivered the wrong bytes; if it does not, the expected pattern itself
	 * was overwritten and this is memory corruption wearing a data-mismatch
	 * costume. */
	fprintf(stderr, "DATA-MISMATCH i=%u pos=%u len=%u got=0x%02x want=0x%02x data[]=%s\n",
		(unsigned int)i, (unsigned int)pos, (unsigned int)len,
		(unsigned char)buf_[i], (unsigned char)data[(pos + i) % sizeof(data)],
		(now == data_sum) ? "INTACT" : "CORRUPTED");

	fprintf(stderr, "DATA-MISMATCH  recv:");
	for (k = base; (k < base + 8u) && (k < len); ++k) {
		fprintf(stderr, " %02x", (unsigned char)buf_[k]);
	}
	fprintf(stderr, "\nDATA-MISMATCH  want:");
	for (k = base; (k < base + 8u) && (k < len); ++k) {
		fprintf(stderr, " %02x", (unsigned char)data[(pos + k) % sizeof(data)]);
	}
	fprintf(stderr, "\n");

	/* Where do the received bytes actually live in data[]? A constant shift
	 * means the two sides simply lost sync (and names the amount -- 8 bytes
	 * would be a datagram length prefix delivered as payload, for instance);
	 * "nowhere" means the bytes were never sent at all. */
	{
		size_t off, m;
		int found = -1;

		for (off = 0; off < sizeof(data); ++off) {
			for (m = 0; (m < 8u) && ((i + m) < len); ++m) {
				if (data[(off + m) % sizeof(data)] != (char)buf_[i + m]) {
					break;
				}
			}
			if ((m >= 8u) || ((i + m) >= len)) {
				found = (int)off;
				break;
			}
		}

		if (found >= 0) {
			unsigned int want_off = (unsigned int)((pos + i) % sizeof(data));
			fprintf(stderr, "DATA-MISMATCH  recv found at data[%d], expected data[%u], shift %d\n",
				found, want_off, found - (int)want_off);
		}
		else {
			fprintf(stderr, "DATA-MISMATCH  recv bytes appear nowhere in data[] (never sent)\n");
		}
	}
	fflush(stderr);
}


static int unix_data_cmp(char *buf, size_t pos, size_t len)
{
	size_t i;

	for (i = 0; i < len; ++i) {
		if (buf[i] != data[(pos + i) % sizeof(data)]) {
			unix_data_report(buf, pos, len, i);
			return 1;
		}
	}

	return 0;
}



static void unix_transfer(int type)
{
	int fd[2];
	pid_t pid;
	size_t tot_len;

	tot_len = 1 + rand() % MAX_TRANSFER_CNT;

	if (socketpair(AF_UNIX, type | SOCK_NONBLOCK, 0, fd) < 0)
		FAIL("socketpair");

	pid = safe_fork();

	if (pid != 0) {
		size_t max_len, len, pos = 0;
		ssize_t n;
		int status;

		while (tot_len > 0) {
			max_len = sizeof(data) - pos;
			if (tot_len < max_len)
				max_len = tot_len;
			len = 1 + rand() % max_len;
			n = send(fd[0], data + pos, len, 0);
			TEST_ASSERT(n > 0 || errno == EAGAIN);
			if (n > 0) {
				tot_len -= n;
				pos = (pos + n) % sizeof(data);
			}
		}

		stdio_verify("parent-after-send");

		if (waitpid(pid, &status, 0) < 0)
			FAIL("waitpid");

		TEST_ASSERT(WIFEXITED(status));
		TEST_ASSERT(WEXITSTATUS(status) == 0);

		close(fd[0]);
		close(fd[1]);
	}
	else {
		size_t pos = 0;
		ssize_t n;

		stdio_verify("child-start");

		while (tot_len > 0) {
			n = recv(fd[1], buf, sizeof(buf), 0);
			stdio_verify("child-after-recv");
			CHILD_ASSERT(n > 0 || errno == EAGAIN);
			if (n > 0) {
				CHILD_ASSERT(unix_data_cmp(buf, pos, n) == 0);
				tot_len -= n;
				pos = (pos + n) % sizeof(data);
			}
		}

		stdio_verify("child-before-exit");
		exit(0);
	}
}


TEST(test_unix_socket, transfer)
{
	unsigned int i;

	for (i = 0; i < transferLoopCnt; ++i) {
		unix_transfer(SOCK_STREAM);
		unix_transfer(SOCK_DGRAM);
	}
}


static void unix_close_connected(int type)
{
	int fd[2];

	if (socketpair(AF_UNIX, type, 0, fd) < 0)
		FAIL("socketpair");

	close(fd[0]);
	close(fd[1]);
}


TEST(test_unix_socket, close_connected)
{
	unsigned int i;

	for (i = 0; i < CONNECTED_LOOP_CNT; ++i) {
		unix_close_connected(SOCK_STREAM);
		unix_close_connected(SOCK_DGRAM);
		unix_close_connected(SOCK_SEQPACKET);
	}
}


volatile int got_epipe;

static void sighandler(int sig)
{
	got_epipe = 1;
}


static void unix_send_after_close(int type, int epipe, int err)
{
	int fd[2];
	ssize_t n;

	signal(SIGPIPE, sighandler);

	if (socketpair(AF_UNIX, type, 0, fd) < 0)
		FAIL("socketpair");

	close(fd[1]);

	got_epipe = 0;
	n = send(fd[0], data, sizeof(data), 0);
	TEST_ASSERT(got_epipe == epipe);
	TEST_ASSERT(n == -1);
	TEST_ASSERT(errno == err);

	got_epipe = 0;
	n = send(fd[0], data, sizeof(data), 0);
	TEST_ASSERT(got_epipe == epipe);
	TEST_ASSERT(n == -1);

	close(fd[0]);

	signal(SIGPIPE, SIG_DFL);
}


TEST(test_unix_socket, send_after_close)
{
	unsigned int i;

	for (i = 0; i < CONNECTED_LOOP_CNT; ++i) {
		unix_send_after_close(SOCK_STREAM, 1, EPIPE);
#ifdef __phoenix__
		unix_send_after_close(SOCK_DGRAM, 0, ECONNREFUSED);
		unix_send_after_close(SOCK_SEQPACKET, 1, EPIPE);
#endif
	}
}


static void unix_recv_after_close(int type)
{
	int fd[2];
	ssize_t n;

	if (socketpair(AF_UNIX, type, 0, fd) < 0)
		FAIL("socketpair");

	close(fd[1]);

	n = recv(fd[0], buf, sizeof(buf), 0);
	TEST_ASSERT(n == 0); /* EOS */

	close(fd[0]);

	if (socketpair(AF_UNIX, type, 0, fd) < 0)
		FAIL("socketpair");

	n = send(fd[1], data, sizeof(data), 0);
	TEST_ASSERT(n == sizeof(data));

	close(fd[1]);

	n = recv(fd[0], buf, sizeof(buf), 0);
	TEST_ASSERT(n == sizeof(buf));

	n = recv(fd[0], buf, sizeof(buf), 0);
	TEST_ASSERT(n == 0); /* EOS */

	close(fd[0]);
}


TEST(test_unix_socket, recv_after_close)
{
	unsigned int i;

	for (i = 0; i < CONNECTED_LOOP_CNT; ++i) {
		unix_recv_after_close(SOCK_STREAM);
		unix_recv_after_close(SOCK_SEQPACKET);
	}
}


static void unix_connect_after_close(int type)
{
	int fd[3];
	int rv;
	const char *socket_name = "/tmp/test_connect_after_close";

	if (socketpair(AF_UNIX, type, 0, fd) < 0)
		FAIL("socketpair");

	close(fd[1]);

	if ((fd[2] = unix_named_socket(SOCK_DGRAM, socket_name)) < 0)
		FAIL("unix_named_socket(SOCK_DGRAM, ");

	rv = connect_to_named(fd[0], socket_name);
	TEST_ASSERT(rv == -1);
	/* EPROTOTYPE??? */
	// TEST_ASSERT(errno == EISCONN);

	close(fd[0]);
	unlink(socket_name);
}


TEST(test_unix_socket, connect_after_close)
{
	unsigned int i;

	for (i = 0; i < CONNECTED_LOOP_CNT; ++i) {
		unix_connect_after_close(SOCK_STREAM);
		unix_connect_after_close(SOCK_SEQPACKET);
	}
}


static void unix_poll(int type)
{
	int fd[2];
	struct pollfd fds[2];
	struct timespec ts[2];
	int rv, ms;

	fds[0].fd = 11111;
	fds[1].fd = 22222;
	fds[0].events = 0;
	fds[1].events = 0;
	fds[0].revents = 0;
	fds[1].revents = 0;
	rv = poll(fds, 2, 0);
	TEST_ASSERT(rv == 2);
	TEST_ASSERT(fds[0].revents == POLLNVAL);
	TEST_ASSERT(fds[1].revents == POLLNVAL);

	if (socketpair(AF_UNIX, type, 0, fd) < 0)
		FAIL("socketpair");

	fds[0].fd = fd[0];
	fds[1].fd = fd[1];

	clock_gettime(CLOCK_REALTIME, &ts[0]);
	fds[0].events = POLLIN;
	fds[1].events = POLLIN;
	fds[0].revents = 0;
	fds[1].revents = 0;
	rv = poll(fds, 2, 300);
	clock_gettime(CLOCK_REALTIME, &ts[1]);
	ms = MS_BETWEEN(ts[0], ts[1]);
	TEST_ASSERT(rv == 0);
	TEST_ASSERT(fds[0].revents == 0);
	TEST_ASSERT(fds[1].revents == 0);
	TEST_ASSERT_LESS_THAN(300 + pollTimeoutDelay, ms);
	TEST_ASSERT_GREATER_THAN(290, ms);

	clock_gettime(CLOCK_REALTIME, &ts[0]);
	fds[0].events = POLLIN | POLLOUT;
	fds[1].events = POLLIN | POLLOUT;
	fds[0].revents = 0;
	fds[1].revents = 0;
	rv = poll(fds, 2, 1000);
	clock_gettime(CLOCK_REALTIME, &ts[1]);
	ms = MS_BETWEEN(ts[0], ts[1]);
	TEST_ASSERT(rv == 2);
	TEST_ASSERT(fds[0].revents == POLLOUT);
	TEST_ASSERT(fds[1].revents == POLLOUT);
	TEST_ASSERT_LESS_THAN(5, ms);

	send(fd[0], data, sizeof(data), 0);
	send(fd[1], data, sizeof(data), 0);

	clock_gettime(CLOCK_REALTIME, &ts[0]);
	fds[0].events = POLLIN;
	fds[1].events = POLLIN;
	fds[0].revents = 0;
	fds[1].revents = 0;
	rv = poll(fds, 2, 1000);
	clock_gettime(CLOCK_REALTIME, &ts[1]);
	ms = MS_BETWEEN(ts[0], ts[1]);
	TEST_ASSERT(rv == 2);
	TEST_ASSERT(fds[0].revents == POLLIN);
	TEST_ASSERT(fds[1].revents == POLLIN);
	TEST_ASSERT_LESS_THAN(5, ms);

	close(fd[0]);
	close(fd[1]);
}


TEST(test_unix_socket, poll)
{
	unix_poll(SOCK_STREAM);
	unix_poll(SOCK_DGRAM);
	unix_poll(SOCK_SEQPACKET);
}


#define READ_MSG(fd, pid, flags) \
	{ \
		int read_msg_len = 128; \
		int rv; \
		memset(buf, 0, read_msg_len); \
		rv = read(fd, buf, read_msg_len); \
		if (pid != 0) { \
			TEST_ASSERT(rv >= 0); \
		} \
		else { \
			if (rv < 0) { \
				exit(1); \
			} \
		} \
		if (pid != 0) { \
			TEST_ASSERT_EQUAL_INT(read_msg_len, rv); \
		} \
		else { \
			if (read_msg_len != rv) { \
				exit(1); \
			} \
		} \
		rv = strncmp(buf, data, read_msg_len); \
		if (pid != 0) { \
			TEST_ASSERT_EQUAL_INT(0, rv); \
		} \
		else { \
			if (rv != 0) { \
				exit(1); \
			} \
		} \
	}


#define SEND_MSG(fd, pid, flags) \
	{ \
		int send_msg_len = 128; \
		int rv; \
		rv = send(fd, data, send_msg_len, flags); \
		if (pid != 0) { \
			TEST_ASSERT(rv >= 0); \
		} \
		else { \
			if (rv < 0) { \
				exit(1); \
			} \
		} \
		if (pid != 0) { \
			TEST_ASSERT_EQUAL_INT(send_msg_len, rv); \
		} \
		else { \
			if (send_msg_len != rv) { \
				exit(1); \
			} \
		} \
	}


/** Note: makes sense for child processes only */
static int connect_to_named_or_timeout(int fd, const char *name, int timeout_ms)
{
	struct timespec ts[2];
	int ms, rv;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts[0]);
	while (true) {
		errno = 0;
		rv = connect_to_named(fd, name);
		if (rv == 0) {
			break;
		}
		else {
			if (rv < 0 && errno != ECONNREFUSED && errno != ENOENT)
				exit(1);

			clock_gettime(CLOCK_MONOTONIC_RAW, &ts[1]);
			ms = (ts[1].tv_sec - ts[0].tv_sec) * 1000 + (ts[1].tv_nsec - ts[0].tv_nsec) / 1000000;
			if (ms > timeout_ms)
				exit(1);

			usleep(150);
		}
	}
	return rv;
}


static void unix_accept_connect_errnos(int type)
{
	int fd, named, rv, conn;

	const char *socket_name = "/tmp/test_accept_connect_errnos";

	errno = 0;
	rv = connect_to_named(BAD_FD, socket_name);
	TEST_ASSERT(rv < 0);
	TEST_ASSERT_EQUAL_INT(EBADF, errno);

	if ((fd = socket(AF_UNIX, type, 0)) < 0)
		FAIL("socket");

	errno = 0;
	rv = connect_to_named(fd, socket_name);
	TEST_ASSERT(rv < 0);
	TEST_ASSERT(errno == ECONNREFUSED || errno == ENOENT);

	if ((named = unix_named_socket(type, socket_name)) < 0)
		FAIL("unix_named_socket");

	if (set_nonblock(named, 1) < 0)
		FAIL("set_nonblock");

	if (listen(named, 0) < 0)
		FAIL("listen");

	errno = 0;
	conn = accept(named, NULL, NULL);
	TEST_ASSERT(conn < 0);
	TEST_ASSERT_EQUAL_INT(EWOULDBLOCK, errno);

	if (set_nonblock(fd, 1) < 0)
		FAIL("set_nonblock");

	errno = 0;
	rv = connect_to_named(fd, socket_name);
#ifdef __phoenix__
	TEST_ASSERT(rv < 0);
	TEST_ASSERT_EQUAL_INT(EINPROGRESS, errno);
#else
	/* glibc allows connect to succeed after nonblocking accept returns
	 * EWOULDBLOCK. This is still POSIX compliant as the standard
	 * doesn't specify whether accept() changes system state when returning
	 * EWOULDBLOCK. Currently phoenix doesn't implement this behavior */
	TEST_ASSERT(rv == 0);
#endif

	errno = 0;
	rv = connect_to_named(fd, socket_name);
	TEST_ASSERT(rv < 0);
#ifdef __phoenix__
	TEST_ASSERT_EQUAL_INT(EALREADY, errno);
#else
	TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
#endif

	close(fd);
	close(named);
	unlink(socket_name);
}


TEST(test_unix_socket, accept_connect_errnos)
{
	unsigned int i;

	for (i = 0; i < CONNECTED_LOOP_CNT; ++i) {
		unix_accept_connect_errnos(SOCK_STREAM);
		unix_accept_connect_errnos(SOCK_SEQPACKET);
	}
}


static void unix_accept_connect_async(int type)
{
	int client_fd, server_fd, rv, conn;
	struct pollfd fds[3];

	const char *socket_name = "/tmp/test_accept_connect_async";

	if ((server_fd = unix_named_socket(type, socket_name)) < 0)
		FAIL("unix_named_socket");

	if (set_nonblock(server_fd, 1) < 0)
		FAIL("set_nonblock");

	if (listen(server_fd, 0) < 0)
		FAIL("listen");

	if ((client_fd = socket(AF_UNIX, type, 0)) < 0)
		FAIL("socket");

	if (set_nonblock(client_fd, 1) < 0)
		FAIL("set_nonblock");

	errno = 0;
	conn = accept(server_fd, NULL, NULL);
	TEST_ASSERT(conn < 0);
	TEST_ASSERT_EQUAL_INT(EWOULDBLOCK, errno);

	errno = 0;
	rv = connect_to_named(client_fd, socket_name);
#ifdef __phoenix__
	TEST_ASSERT(rv < 0);
	TEST_ASSERT_EQUAL_INT(EINPROGRESS, errno);
#endif

	fds[0].fd = server_fd;
	fds[0].events = POLLIN;
	fds[1].fd = client_fd;
	fds[1].events = POLLOUT;

	/* poll for incoming connection on server_fd (POLLIN) */
#ifdef __phoenix__
	TEST_ASSERT_EQUAL_INT(1, poll(fds, 2, 1000));
	TEST_ASSERT_EQUAL_INT(POLLIN, fds[0].revents);
	TEST_ASSERT_EQUAL_INT(0, fds[1].revents);
#else
	TEST_ASSERT_EQUAL_INT(2, poll(fds, 2, 1000));
	TEST_ASSERT_EQUAL_INT(POLLIN, fds[0].revents);
	TEST_ASSERT_EQUAL_INT(POLLOUT, fds[1].revents);
#endif

	fds[2].fd = accept(server_fd, NULL, NULL);
	fds[2].events = POLLIN;
	TEST_ASSERT(fds[2].fd > 0);

	errno = 0;
	rv = connect_to_named(client_fd, socket_name);
	TEST_ASSERT(rv < 0);
	TEST_ASSERT_EQUAL_INT(EISCONN, errno);

	/* poll for connection on client_fd to be established (POLLOUT) */
	TEST_ASSERT_EQUAL_INT(1, poll(fds, 2, 1000));
	TEST_ASSERT_EQUAL_INT(0, fds[0].revents);
	TEST_ASSERT_EQUAL_INT(POLLOUT, fds[1].revents);

	errno = 0;
	rv = connect_to_named(client_fd, socket_name);
	TEST_ASSERT(rv < 0);
	TEST_ASSERT_EQUAL_INT(EISCONN, errno);

	SEND_MSG(client_fd, 1, 0);

	TEST_ASSERT_EQUAL_INT(2, poll(fds, 3, 1000));
	TEST_ASSERT_EQUAL_INT(0, fds[0].revents);
	TEST_ASSERT_EQUAL_INT(POLLOUT, fds[1].revents);
	TEST_ASSERT_EQUAL_INT(POLLIN, fds[2].revents);

	READ_MSG(fds[2].fd, 1, 0);

	close(fds[0].fd);
	close(fds[1].fd);
	close(fds[2].fd);
	unlink(socket_name);
}


TEST(test_unix_socket, accept_connect_async)
{
	unsigned int i;

	for (i = 0; i < CONNECTED_LOOP_CNT; ++i) {
		unix_accept_connect_async(SOCK_STREAM);
		unix_accept_connect_async(SOCK_SEQPACKET);
	}
}


static void unix_accept_connect_liveness_helper(int type)
{
	pid_t pid;
	int fd, named, rv, conn, status;
	struct pollfd fds[2];

	const char *socket_name = "/tmp/test_accept_connect";

	/* blocking connect, blocking accept */

	pid = safe_fork();

	if (pid != 0) {
		if ((named = unix_named_socket(type, socket_name)) < 0)
			FAIL("unix_named_socket");

		if (listen(named, 0) < 0)
			FAIL("listen");

		if ((conn = accept(named, NULL, NULL)) < 0)
			FAIL("accept");

		/* assert that child is still running, and more importantly that it hasn't
		 * exited abnormally, because if it had, the parent may block forever on read */
		TEST_ASSERT_EQUAL_INT(0, waitpid(pid, NULL, WNOHANG));

		READ_MSG(conn, pid, 0);

		/* send msg to child so that it can terminate */
		SEND_MSG(conn, pid, 0);

		if (waitpid(pid, &status, 0) < 0)
			FAIL("waitpid");

		TEST_ASSERT(WIFEXITED(status));
		TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

		close(conn);
		close(named);
		unlink(socket_name);
	}
	else {
		if ((fd = socket(AF_UNIX, type, 0)) < 0)
			exit(1);

		rv = connect_to_named_or_timeout(fd, socket_name, 3000);

		SEND_MSG(fd, pid, 0);

		/* read msg from parent so that parent does the read
		 * before conn gets closed */
		READ_MSG(fd, pid, 0);
		close(fd);

		exit(0);
	}

	/* blocking connect, nonblocking accept */

	pid = safe_fork();

	if (pid != 0) {
		if ((named = unix_named_socket(type, socket_name)) < 0)
			FAIL("unix_named_socket");

		if (set_nonblock(named, 1) < 0)
			FAIL("set_nonblock");

		if (listen(named, 0) < 0)
			FAIL("listen");

		fds[0].fd = named;
		fds[0].events = POLLIN;

		TEST_ASSERT_EQUAL_INT(1, poll(fds, 1, 500));
		TEST_ASSERT_EQUAL_INT(POLLIN, fds[0].revents);

		conn = accept(fds[0].fd, NULL, NULL);
		TEST_ASSERT(conn > 0);

		TEST_ASSERT_EQUAL_INT(0, waitpid(pid, NULL, WNOHANG));

		READ_MSG(conn, pid, 0);

		SEND_MSG(conn, pid, 0);

		if (waitpid(pid, &status, 0) < 0)
			FAIL("waitpid");

		TEST_ASSERT(WIFEXITED(status));
		TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

		close(conn);
		close(named);
		unlink(socket_name);
	}
	else {
		if ((fd = socket(AF_UNIX, type, 0)) < 0)
			exit(1);

		rv = connect_to_named_or_timeout(fd, socket_name, 3000);

		SEND_MSG(fd, pid, 0);

		READ_MSG(fd, pid, 0);
		close(fd);

		exit(0);
	}

	/* nonblocking connect, blocking accept */

	pid = safe_fork();

	if (pid != 0) {
		if ((fd = socket(AF_UNIX, type, 0)) < 0)
			FAIL("socket");

		if (set_nonblock(fd, 1) < 0)
			FAIL("set_nonblock");

		while (true) {
			errno = 0;
			rv = connect_to_named(fd, socket_name);
			if (rv >= 0) {
#ifdef __phoenix__
				FAIL("should never happen - child proc should sleep for longer");
#else
				/* glibc behaves differently - see note in unix_accept_connect_errnos() */
				break;
#endif
			}
			else if (errno == EINPROGRESS) {
				break;
			}
			else {
				TEST_ASSERT(rv < 0);
				TEST_ASSERT(errno == ECONNREFUSED || errno == ENOENT);
				usleep(500);
			}
		}

		fds[0].fd = fd;
		fds[0].events = POLLOUT;

		TEST_ASSERT_EQUAL_INT(1, poll(fds, 1, 700));
		TEST_ASSERT_EQUAL_INT(POLLOUT, fds[0].revents);

		int optval = 0;
		socklen_t optlen = sizeof(optval);

		TEST_ASSERT_EQUAL_INT(0, getsockopt(fds[0].fd, SOL_SOCKET, SO_ERROR, &optval, &optlen));

		fds[0].events = POLLIN;
		TEST_ASSERT_EQUAL_INT(1, poll(fds, 1, 250));
		TEST_ASSERT_EQUAL_INT(POLLIN, fds[0].revents);

		TEST_ASSERT_EQUAL_INT(0, waitpid(pid, NULL, WNOHANG));

		READ_MSG(fds[0].fd, pid, 0);

		fds[0].events = POLLOUT;
		TEST_ASSERT_EQUAL_INT(1, poll(fds, 1, 250));
		TEST_ASSERT_EQUAL_INT(POLLOUT, fds[0].revents);

		SEND_MSG(fds[0].fd, pid, 0);

		if (waitpid(pid, &status, 0) < 0)
			FAIL("waitpid");

		TEST_ASSERT(WIFEXITED(status));
		TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

		close(fds[0].fd);
	}
	else {
		if ((named = unix_named_socket(type, socket_name)) < 0)
			exit(1);

		if (listen(named, 0) < 0)
			exit(1);

		usleep(50 * 1000); /* sleep so that connect would block */

		if ((conn = accept(named, NULL, NULL)) < 0)
			exit(1);

		SEND_MSG(conn, pid, 0);

		/* read something from parent so that parent does the first
		 * POLLOUT before conn gets closed  */
		READ_MSG(conn, pid, 0);
		close(conn);

		close(named);
		unlink(socket_name);

		exit(0);
	}
}


static void unix_accept_connect_liveness(int type)
{
	unsigned int i = 0;

	for (i = 0; i < 25; ++i) {
		unix_accept_connect_liveness_helper(type);
	}
}


TEST(test_unix_socket, accept_connect_liveness)
{
	unix_accept_connect_liveness(SOCK_STREAM);
	unix_accept_connect_liveness(SOCK_SEQPACKET);
}


static void unix_socket_recv_msg_peek(int flags)
{
	int fd[2];
	int msg_len = 128;
	ssize_t n;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) < 0)
		FAIL("socketpair");

	n = write(fd[0], data, msg_len);
	TEST_ASSERT(n == msg_len);

	n = write(fd[0], data, 1);
	TEST_ASSERT(n == 1);

	/** Peek on first 2 iterations, on 3th do a normal read */
	for (int i = 0; i < 4; i++) {
		n = recv(fd[1], buf, msg_len, flags | (i < 2 ? MSG_PEEK : 0));
		if (i < 3) {
			/* Should read the same message 3 times */
			TEST_ASSERT(n == msg_len);
			TEST_ASSERT(strncmp(buf, data, msg_len) == 0);
		}
		else {
			/* Should read one byte as previous message was normally read on 3rd
			 * iteration */
			TEST_ASSERT(n == 1);
		}
	}

	close(fd[0]);
	close(fd[1]);
}


TEST(test_unix_socket, recv_msg_peek)
{
	unix_socket_recv_msg_peek(0);
	unix_socket_recv_msg_peek(MSG_DONTWAIT);
}


// TODO: add listen() backlog test when implemented

TEST(test_unix_socket, flags)
{
	int fd, err;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);

	errno = 0;
	err = fcntl(fd, F_GETFL);
	TEST_ASSERT_EQUAL_INT(O_RDWR, err);
	TEST_ASSERT_EQUAL_INT(0, errno);

	errno = 0;
	err = fcntl(fd, F_SETFL, O_NONBLOCK);
	TEST_ASSERT_EQUAL_INT(0, err);
	TEST_ASSERT_EQUAL_INT(0, errno);

	errno = 0;
	err = fcntl(fd, F_GETFL);
	TEST_ASSERT_EQUAL_INT(O_RDWR | O_NONBLOCK, err);
	TEST_ASSERT_EQUAL_INT(0, errno);

	close(fd);
}


static void unix_wrong_family(int type)
{
	int fd[2], err;
	struct sockaddr_un addr = { 0 };
	const char *socket_name = "/tmp/wrong_family";

	fd[0] = unix_named_socket(type, socket_name);
	if (fd[0] < 0) {
		FAIL("unix_named_socket");
	}

	fd[1] = socket(AF_UNIX, type, 0);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd[1]);

	addr.sun_family = AF_INET;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_name);

	errno = 0;
	err = bind(fd[1], (struct sockaddr *)&addr, SUN_LEN(&addr));
	TEST_ASSERT_EQUAL_INT(-1, err);
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);

	errno = 0;
	err = connect(fd[1], (struct sockaddr *)&addr, SUN_LEN(&addr));
	TEST_ASSERT_EQUAL_INT(-1, err);
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);

	if (type == SOCK_DGRAM) {
		errno = 0;
		err = sendto(fd[1], "data", 4, 0, (struct sockaddr *)&addr, SUN_LEN(&addr));
		TEST_ASSERT_EQUAL_INT(-1, err);
		TEST_ASSERT_EQUAL_INT(EINVAL, errno);
	}

	close(fd[0]);
	close(fd[1]);
	unlink(socket_name);
}


TEST(test_unix_socket, wrong_family)
{
	unix_wrong_family(SOCK_STREAM);
	unix_wrong_family(SOCK_SEQPACKET);
	unix_wrong_family(SOCK_DGRAM);
}


static void unix_wrong_port(int type)
{
	int fd[2], err;
	struct sockaddr_un addr = { 0 };
	const char *socket_name = "/tmp/wrong_port";


	fd[0] = open(socket_name, O_CREAT | O_WRONLY | O_TRUNC, 0666);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd[0]);

	fd[1] = socket(AF_UNIX, type, 0);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd[1]);

	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_name);

	errno = 0;
	err = connect(fd[1], (struct sockaddr *)&addr, SUN_LEN(&addr));
	TEST_ASSERT_EQUAL_INT(-1, err);
	TEST_ASSERT_EQUAL_INT(ECONNREFUSED, errno);

	if (type == SOCK_DGRAM) {
		errno = 0;
		err = sendto(fd[1], "data", 4, 0, (struct sockaddr *)&addr, SUN_LEN(&addr));
		TEST_ASSERT_EQUAL_INT(-1, err);
		TEST_ASSERT_EQUAL_INT(ECONNREFUSED, errno);
	}

	close(fd[0]);
	close(fd[1]);
	unlink(socket_name);
}


TEST(test_unix_socket, wrong_port)
{
	unix_wrong_port(SOCK_STREAM);
	unix_wrong_port(SOCK_SEQPACKET);
	unix_wrong_port(SOCK_DGRAM);
}


static void unix_wrong_type(int type)
{
	int fd[2], err, wrong_type;
	struct sockaddr_un addr = { 0 };
	const char *socket_name = "/tmp/wrong_type";

	switch (type) {
		case SOCK_STREAM:
			wrong_type = SOCK_SEQPACKET;
			break;
		case SOCK_SEQPACKET:
			wrong_type = SOCK_DGRAM;
			break;
		case SOCK_DGRAM:
			wrong_type = SOCK_STREAM;
			break;
		default:
			FAIL("invalid_type");
	}

	fd[0] = unix_named_socket(wrong_type, socket_name);
	if (fd[0] < 0) {
		FAIL("unix_named_socket");
	}

	fd[1] = socket(AF_UNIX, type, 0);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd[1]);

	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_name);

	errno = 0;
	err = connect(fd[1], (struct sockaddr *)&addr, SUN_LEN(&addr));
	TEST_ASSERT_EQUAL_INT(-1, err);
	TEST_ASSERT_EQUAL_INT(EPROTOTYPE, errno);

	if (type == SOCK_DGRAM) {
		errno = 0;
		err = sendto(fd[1], "data", 4, 0, (struct sockaddr *)&addr, SUN_LEN(&addr));
		TEST_ASSERT_EQUAL_INT(-1, err);
		TEST_ASSERT_EQUAL_INT(EPROTOTYPE, errno);
	}

	close(fd[0]);
	close(fd[1]);
	unlink(socket_name);
}


TEST(test_unix_socket, wrong_type)
{
	unix_wrong_type(SOCK_STREAM);
	unix_wrong_type(SOCK_SEQPACKET);
	unix_wrong_type(SOCK_DGRAM);
}


TEST(test_unix_socket, send_clear_peer_closed)
{
	int fd[2], err;
	struct sockaddr_un addr = { 0 };
	const char *socket_name = "/tmp/send_clear_peer_closed";

	fd[0] = unix_named_socket(SOCK_DGRAM, socket_name);
	if (fd[0] < 0) {
		FAIL("unix_named_socket");
	}

	fd[1] = socket(AF_UNIX, SOCK_DGRAM, 0);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd[1]);

	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_name);

	errno = 0;
	err = connect(fd[1], (struct sockaddr *)&addr, SUN_LEN(&addr));
	TEST_ASSERT_EQUAL_INT(0, err);
	TEST_ASSERT_EQUAL_INT(0, errno);

	errno = 0;
	err = close(fd[0]);
	TEST_ASSERT_EQUAL_INT(0, err);
	TEST_ASSERT_EQUAL_INT(0, errno);

	errno = 0;
	err = unlink(socket_name);
	TEST_ASSERT_EQUAL_INT(0, err);
	TEST_ASSERT_EQUAL_INT(0, errno);

	errno = 0;
	err = send(fd[1], "data", 4, 0);
	TEST_ASSERT_EQUAL_INT(-1, err);
	TEST_ASSERT_EQUAL_INT(ECONNREFUSED, errno);

	errno = 0;
	err = send(fd[1], "data", 4, 0);
	TEST_ASSERT_EQUAL_INT(-1, err);
	TEST_ASSERT_EQUAL_INT(ENOTCONN, errno);

	close(fd[1]);
}


/* A bound socket's NAME outlives the socket: POSIX removes it on unlink(), not
 * on close(). So after a bound socket is closed without unlinking, its path is
 * still there -- and connecting or sending to it must fail, never reach some
 * OTHER socket.
 *
 * It used to reach another socket. AF_UNIX ids were allocated lowest-free, so
 * the closed socket's id was immediately handed to the next socket created,
 * and the stale name -- which stores {US_PORT, id} -- then resolved straight
 * to that new, unrelated socket. A datagram sent to the dead name landed in
 * the live socket's buffer, so its reader returned a whole chunk nobody had
 * sent it. These two tests pin the behaviour down from both ends: the send
 * must fail, AND the innocent socket must receive nothing.
 */
#define STALE_CHURN_CNT 8

TEST(test_unix_socket, stale_name_dgram_no_crosstalk)
{
	const char *staleName = "/tmp/test_stale_dgram";
	const char *victimName = "/tmp/test_stale_victim";
	int stale, victim, sender, churn[STALE_CHURN_CNT];
	struct sockaddr_un addr = { 0 };
	char buf[32];
	ssize_t n;
	int i;

	stale = unix_named_socket(SOCK_DGRAM, staleName);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, stale);

	/* Closed, deliberately NOT unlinked: the path stays behind. */
	TEST_ASSERT_EQUAL_INT(0, close(stale));

	/* Churn ids so a lowest-free allocator hands the dead socket's id out
	 * again; a monotonic one never does. */
	for (i = 0; i < STALE_CHURN_CNT; i++) {
		churn[i] = socket(AF_UNIX, SOCK_DGRAM, 0);
		TEST_ASSERT_GREATER_OR_EQUAL_INT(0, churn[i]);
	}
	for (i = 0; i < STALE_CHURN_CNT; i++) {
		TEST_ASSERT_EQUAL_INT(0, close(churn[i]));
	}

	/* The innocent bystander -- the socket most likely to inherit the id. */
	victim = unix_named_socket(SOCK_DGRAM, victimName);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, victim);

	sender = socket(AF_UNIX, SOCK_DGRAM, 0);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sender);

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, staleName);

	errno = 0;
	n = sendto(sender, "STALE", 5, 0, (struct sockaddr *)&addr, SUN_LEN(&addr));
	TEST_ASSERT_EQUAL_INT(-1, n);
	TEST_ASSERT_TRUE((errno == ECONNREFUSED) || (errno == ENOENT));

	/* The part that actually catches cross-talk: even if the send had
	 * reported failure, nothing may have been delivered elsewhere. */
	errno = 0;
	n = recv(victim, buf, sizeof(buf), MSG_DONTWAIT);
	TEST_ASSERT_EQUAL_INT(-1, n);
	TEST_ASSERT_TRUE((errno == EAGAIN) || (errno == EWOULDBLOCK));

	close(sender);
	close(victim);
	unlink(staleName);
	unlink(victimName);
}


TEST(test_unix_socket, stale_name_stream_no_crosstalk)
{
	const char *staleName = "/tmp/test_stale_stream";
	const char *victimName = "/tmp/test_stale_listener";
	int stale, victim, client, churn[STALE_CHURN_CNT];
	int err, i;

	stale = unix_named_socket(SOCK_STREAM, staleName);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, stale);
	TEST_ASSERT_EQUAL_INT(0, listen(stale, 4));
	TEST_ASSERT_EQUAL_INT(0, close(stale));

	for (i = 0; i < STALE_CHURN_CNT; i++) {
		churn[i] = socket(AF_UNIX, SOCK_STREAM, 0);
		TEST_ASSERT_GREATER_OR_EQUAL_INT(0, churn[i]);
	}
	for (i = 0; i < STALE_CHURN_CNT; i++) {
		TEST_ASSERT_EQUAL_INT(0, close(churn[i]));
	}

	victim = unix_named_socket(SOCK_STREAM, victimName);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, victim);
	TEST_ASSERT_EQUAL_INT(0, listen(victim, 4));
	TEST_ASSERT_EQUAL_INT(0, fcntl(victim, F_SETFL, O_NONBLOCK));

	client = socket(AF_UNIX, SOCK_STREAM, 0);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, client);

	errno = 0;
	err = connect_to_named(client, staleName);
	TEST_ASSERT_EQUAL_INT(-1, err);
	TEST_ASSERT_TRUE((errno == ECONNREFUSED) || (errno == ENOENT));

	/* Nobody may have been connected to instead. */
	errno = 0;
	err = accept(victim, NULL, NULL);
	TEST_ASSERT_EQUAL_INT(-1, err);
	TEST_ASSERT_TRUE((errno == EAGAIN) || (errno == EWOULDBLOCK));

	close(client);
	close(victim);
	unlink(staleName);
	unlink(victimName);
}


TEST_GROUP_RUNNER(test_unix_socket)
{
	RUN_TEST_CASE(test_unix_socket, zero_len_send);
	RUN_TEST_CASE(test_unix_socket, zero_len_recv);
	RUN_TEST_CASE(test_unix_socket, close);
	RUN_TEST_CASE(test_unix_socket, msg_data_only);
	RUN_TEST_CASE(test_unix_socket, stream_sock_data_and_fd);
	RUN_TEST_CASE(test_unix_socket, dgram_sock_data_and_fd);
	RUN_TEST_CASE(test_unix_socket, stream_sock_fd_flags);
	RUN_TEST_CASE(test_unix_socket, dgram_sock_fd_flags);
	RUN_TEST_CASE(test_unix_socket, stream_sock_msg_fork);
	RUN_TEST_CASE(test_unix_socket, dgram_sock_msg_fork);
	RUN_TEST_CASE(test_unix_socket, transfer);
	RUN_TEST_CASE(test_unix_socket, close_connected);
	RUN_TEST_CASE(test_unix_socket, send_after_close);
	RUN_TEST_CASE(test_unix_socket, recv_after_close);
	RUN_TEST_CASE(test_unix_socket, connect_after_close);
	RUN_TEST_CASE(test_unix_socket, poll);
	RUN_TEST_CASE(test_unix_socket, recv_msg_peek);
	RUN_TEST_CASE(test_unix_socket, accept_connect_errnos);
	RUN_TEST_CASE(test_unix_socket, accept_connect_async);
	RUN_TEST_CASE(test_unix_socket, accept_connect_liveness);
	RUN_TEST_CASE(test_unix_socket, flags);
	RUN_TEST_CASE(test_unix_socket, wrong_family);
	RUN_TEST_CASE(test_unix_socket, wrong_port);
	RUN_TEST_CASE(test_unix_socket, wrong_type);
	RUN_TEST_CASE(test_unix_socket, send_clear_peer_closed);
	RUN_TEST_CASE(test_unix_socket, stale_name_dgram_no_crosstalk);
	RUN_TEST_CASE(test_unix_socket, stale_name_stream_no_crosstalk);
}

void runner(void)
{
	RUN_TEST_GROUP(test_unix_socket);
}


int main(int argc, char *argv[])
{
	/* Due to scheduling delays of host system on which emulator runs,
	 * add some extra value to poll timeout checks
	 */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--extra-poll-delay-ms") == 0) {
			pollTimeoutDelay = atoi(argv[i + 1]);
			if (pollTimeoutDelay <= 0) {
				fprintf(stderr, "--extra-poll-delay-ms argument is not positive integer\n");
				exit(EXIT_FAILURE);
			}
		}
		if (strcmp(argv[i], "--transfer-loop-cnt") == 0) {
			transferLoopCnt = atoi(argv[i + 1]);
			if (transferLoopCnt < 0 || transferLoopCnt > 50) {
				fprintf(stderr, "--transfer-loop-cnt shall be between 0 and 50\n");
				exit(EXIT_FAILURE);
			}
		}
	}

	int isMissing;

	if (createTmpIfMissing(&isMissing) < 0) {
		exit(EXIT_FAILURE);
	}

	int failures = UnityMain(argc, (const char **)argv, runner);

	if (isMissing != 0) {
		rmdir("/tmp");
	}

	return (failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
