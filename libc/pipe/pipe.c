/*
 * Phoenix-RTOS
 *
 * test-libc-pipe: POSIX semantics of anonymous pipes
 *
 * HEADER:
 *    - unistd.h
 *
 * TESTED:
 *    - pipe(), pipe2(): read/write, end-of-file once every write end is closed
 *      (including copies made by dup(), dup2() and fork()), EPIPE + SIGPIPE once
 *      every read end is closed, EBADF on the wrong end, zero-length I/O
 *    - O_NONBLOCK from pipe2(), fcntl(F_SETFL) and ioctl(FIONBIO), EAGAIN on
 *      both ends, FIONREAD
 *    - atomic writes: a write of at most PIPE_BUF bytes is never split, whether
 *      it blocks or not, also with several writer processes
 *    - a large non-blocking write is partial, a large blocking one complete
 *    - lseek/pread/pwrite: ESPIPE; fstat(): S_IFIFO, one inode for both ends;
 *      isatty(): 0; F_GETFL access modes
 *    - poll()/select(): POLLIN, POLLOUT (only with room for PIPE_BUF bytes),
 *      POLLHUP on the read end, POLLERR on the write end, a wake-up from
 *      another process
 *    - descriptors: F_DUPFD_CLOEXEC and pipe2(O_CLOEXEC) across exec, a pipe
 *      end passed with SCM_RIGHTS
 *    - EINTR: a blocked read interrupted by a signal
 *
 * Written against Linux behaviour, which the test also passes on (host-generic-pc).
 * Where Linux and POSIX allow more than one answer - a PIPE_BUF write into a
 * pipe with less than a page free - either is accepted, but never a partial one.
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
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <unity_fixture.h>


/*
 * The size up to which a pipe write is atomic. libphoenix does not define
 * PIPE_BUF; Phoenix's kernel pipes guarantee 4096 (PIPE_ATOMIC in the kernel's
 * posix/pipe.h), the same as Linux.
 */
#ifdef PIPE_BUF
#define TP_ATOMIC PIPE_BUF
#else
#define TP_ATOMIC 4096
#endif

#define TP_BIG (1024 * 1024)


const char *pipe_self;

static int tp_fd[2];
static struct sigaction tp_oldPipe, tp_oldUsr1;
static volatile sig_atomic_t tp_sigpipes, tp_usr1;
static char tp_buf[TP_BIG]; /* off the 12 kB stack */


static void tp_close(int *fd)
{
	if (*fd >= 0) {
		(void)close(*fd);
		*fd = -1;
	}
}


static void tp_setNonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL);

	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fl);
	TEST_ASSERT_EQUAL_INT(0, fcntl(fd, F_SETFL, fl | O_NONBLOCK));
}


static int tp_avail(int fd)
{
	int n = -1;

	TEST_ASSERT_EQUAL_INT(0, ioctl(fd, FIONREAD, &n));
	return n;
}


/* Fills a pipe through its non-blocking write end; returns the bytes queued */
static int tp_fill(int wr)
{
	static const char chunk[512];
	int queued = 0;
	ssize_t r;

	for (;;) {
		r = write(wr, chunk, sizeof(chunk));
		if (r < 0) {
			TEST_ASSERT_TRUE((errno == EAGAIN) || (errno == EWOULDBLOCK));
			break;
		}
		/* 512 <= PIPE_BUF: whole or not at all */
		TEST_ASSERT_EQUAL_INT((int)sizeof(chunk), (int)r);
		queued += (int)r;
		TEST_ASSERT_LESS_THAN_INT(16 * 1024 * 1024, queued);
	}
	return queued;
}


static void tp_drain(int rd, int n)
{
	ssize_t r;

	while (n > 0) {
		r = read(rd, tp_buf, ((size_t)n < sizeof(tp_buf)) ? (size_t)n : sizeof(tp_buf));
		TEST_ASSERT_GREATER_THAN_INT(0, (int)r);
		n -= (int)r;
	}
}


static short tp_poll(int fd, short events)
{
	struct pollfd pfd = { .fd = fd, .events = events, .revents = 0 };

	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, poll(&pfd, 1, 0));
	return pfd.revents;
}


static int64_t tp_nowUs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}


static void tp_onSigpipe(int sig)
{
	(void)sig;
	tp_sigpipes++;
}


static void tp_onUsr1(int sig)
{
	(void)sig;
	tp_usr1++;
}


/* The exec'd child: argv = { case, args... } */
int pipe_childMain(int argc, char *argv[])
{
	int keep, gone;

	if ((strcmp(argv[0], "cloexec") == 0) && (argc == 3)) {
		/* `keep` survived the exec and is still a pipe end, `gone` did not */
		keep = atoi(argv[1]);
		gone = atoi(argv[2]);
		if ((fcntl(gone, F_GETFD) != -1) || (errno != EBADF)) {
			return 2;
		}
		if (fcntl(keep, F_GETFD) != 0) {
			return 3;
		}
		return (write(keep, "K", 1) == 1) ? 0 : 4;
	}

	return 1;
}


TEST_GROUP(pipe_semantics);


TEST_SETUP(pipe_semantics)
{
	struct sigaction sa;

	tp_fd[0] = -1;
	tp_fd[1] = -1;

	/* EPIPE without dying; a case that wants the signal installs a handler */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	(void)sigaction(SIGPIPE, &sa, &tp_oldPipe);
	(void)sigaction(SIGUSR1, NULL, &tp_oldUsr1);
}


TEST_TEAR_DOWN(pipe_semantics)
{
	tp_close(&tp_fd[0]);
	tp_close(&tp_fd[1]);
	(void)sigaction(SIGPIPE, &tp_oldPipe, NULL);
	(void)sigaction(SIGUSR1, &tp_oldUsr1, NULL);
}


TEST(pipe_semantics, flow_and_eof)
{
	char b[8];

	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	TEST_ASSERT_EQUAL_INT(5, write(tp_fd[1], "hello", 5));
	TEST_ASSERT_EQUAL_INT(5, tp_avail(tp_fd[0]));
	TEST_ASSERT_EQUAL_INT(3, read(tp_fd[0], b, 3));
	TEST_ASSERT_EQUAL_MEMORY("hel", b, 3);
	TEST_ASSERT_EQUAL_INT(2, tp_avail(tp_fd[0]));

	/* the bytes written before the last close are still read, then EOF */
	tp_close(&tp_fd[1]);
	TEST_ASSERT_EQUAL_INT(2, read(tp_fd[0], b, sizeof(b)));
	TEST_ASSERT_EQUAL_MEMORY("lo", b, 2);
	TEST_ASSERT_EQUAL_INT(0, read(tp_fd[0], b, sizeof(b)));
	TEST_ASSERT_EQUAL_INT(0, read(tp_fd[0], b, sizeof(b)));
}


TEST(pipe_semantics, zero_length)
{
	char b;

	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	/* neither blocks on an empty pipe */
	TEST_ASSERT_EQUAL_INT(0, read(tp_fd[0], &b, 0));
	TEST_ASSERT_EQUAL_INT(0, write(tp_fd[1], &b, 0));
	TEST_ASSERT_EQUAL_INT(0, tp_avail(tp_fd[0]));
}


TEST(pipe_semantics, wrong_end)
{
	char b = 'x';

	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, write(tp_fd[0], &b, 1));
	TEST_ASSERT_EQUAL_INT(EBADF, errno);
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, read(tp_fd[1], &b, 1));
	TEST_ASSERT_EQUAL_INT(EBADF, errno);

	TEST_ASSERT_EQUAL_INT(O_RDONLY, fcntl(tp_fd[0], F_GETFL) & O_ACCMODE);
	TEST_ASSERT_EQUAL_INT(O_WRONLY, fcntl(tp_fd[1], F_GETFL) & O_ACCMODE);
}


/* EOF only once EVERY copy of the write end is gone: dup(), dup2() */
TEST(pipe_semantics, eof_needs_every_writer)
{
	int d1, d2 = 100;
	char b;

	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	tp_setNonblock(tp_fd[0]);
	d1 = dup(tp_fd[1]);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, d1);
	TEST_ASSERT_EQUAL_INT(d2, dup2(tp_fd[1], d2));

	tp_close(&tp_fd[1]);
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, read(tp_fd[0], &b, 1));
	TEST_ASSERT_EQUAL_INT(EAGAIN, errno);

	TEST_ASSERT_EQUAL_INT(1, write(d2, "y", 1));
	(void)close(d1);
	TEST_ASSERT_EQUAL_INT(1, read(tp_fd[0], &b, 1));
	TEST_ASSERT_EQUAL_INT('y', b);
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, read(tp_fd[0], &b, 1));
	TEST_ASSERT_EQUAL_INT(EAGAIN, errno);

	(void)close(d2);
	TEST_ASSERT_EQUAL_INT(0, read(tp_fd[0], &b, 1));
}


/* A forked child holds a write end: the reader blocks until the child exits */
TEST(pipe_semantics, eof_after_child_exit)
{
	int64_t t0, us;
	pid_t pid;
	char b[8];
	int status;

	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	pid = fork();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);
	if (pid == 0) {
		(void)close(tp_fd[0]);
		if (write(tp_fd[1], "abc", 3) != 3) {
			_exit(1);
		}
		usleep(200 * 1000);
		_exit(0); /* closes the inherited write end */
	}
	tp_close(&tp_fd[1]);

	t0 = tp_nowUs();
	TEST_ASSERT_EQUAL_INT(3, read(tp_fd[0], b, sizeof(b)));
	TEST_ASSERT_EQUAL_MEMORY("abc", b, 3);
	TEST_ASSERT_EQUAL_INT(0, read(tp_fd[0], b, sizeof(b)));
	us = tp_nowUs() - t0;
	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	TEST_ASSERT_TRUE(WIFEXITED(status) && (WEXITSTATUS(status) == 0));
	/* it waited for the child, but not much longer */
	TEST_ASSERT_GREATER_THAN_INT(100 * 1000, (int)us);
	TEST_ASSERT_LESS_THAN_INT(5 * 1000 * 1000, (int)us);
}


TEST(pipe_semantics, epipe_and_sigpipe)
{
	struct sigaction sa;
	int i;

	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	TEST_ASSERT_EQUAL_INT(1, write(tp_fd[1], "x", 1));
	tp_close(&tp_fd[0]);

	/* ignored: EPIPE only */
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, write(tp_fd[1], "x", 1));
	TEST_ASSERT_EQUAL_INT(EPIPE, errno);

	/* caught: EPIPE and the signal */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = tp_onSigpipe;
	sigemptyset(&sa.sa_mask);
	TEST_ASSERT_EQUAL_INT(0, sigaction(SIGPIPE, &sa, NULL));
	tp_sigpipes = 0;
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, write(tp_fd[1], "x", 1));
	TEST_ASSERT_EQUAL_INT(EPIPE, errno);
	for (i = 0; (i < 100) && (tp_sigpipes == 0); i++) {
		usleep(1000);
	}
	TEST_ASSERT_EQUAL_INT(1, tp_sigpipes);
}


/* SIGPIPE's default action terminates the writer */
TEST(pipe_semantics, sigpipe_kills)
{
	pid_t pid;
	int status;

	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	tp_close(&tp_fd[0]);
	pid = fork();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);
	if (pid == 0) {
		signal(SIGPIPE, SIG_DFL);
		if (write(tp_fd[1], "x", 1) < 0) {
			_exit(6);
		}
		_exit(7);
	}
	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	TEST_ASSERT_TRUE(WIFSIGNALED(status));
	TEST_ASSERT_EQUAL_INT(SIGPIPE, WTERMSIG(status));
}


/* O_NONBLOCK three ways, on both ends */
TEST(pipe_semantics, nonblock)
{
	int on = 1, off = 0, cap;
	char b;

	TEST_ASSERT_EQUAL_INT(0, pipe2(tp_fd, O_NONBLOCK));
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, read(tp_fd[0], &b, 1));
	TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
	cap = tp_fill(tp_fd[1]);
	printf("PIPE capacity=%d\n", cap);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(TP_ATOMIC, cap);
	TEST_ASSERT_EQUAL_INT(cap, tp_avail(tp_fd[0]));
	tp_close(&tp_fd[0]);
	tp_close(&tp_fd[1]);

	/* FIONBIO sets and clears O_NONBLOCK of the open file */
	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	TEST_ASSERT_EQUAL_INT(0, fcntl(tp_fd[0], F_GETFL) & O_NONBLOCK);
	TEST_ASSERT_EQUAL_INT(0, ioctl(tp_fd[0], FIONBIO, &on));
	TEST_ASSERT_NOT_EQUAL(0, fcntl(tp_fd[0], F_GETFL) & O_NONBLOCK);
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, read(tp_fd[0], &b, 1));
	TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
	TEST_ASSERT_EQUAL_INT(0, ioctl(tp_fd[0], FIONBIO, &off));
	TEST_ASSERT_EQUAL_INT(0, fcntl(tp_fd[0], F_GETFL) & O_NONBLOCK);

	/* fcntl(F_SETFL) on the write end; the read end's flags are its own */
	tp_setNonblock(tp_fd[1]);
	TEST_ASSERT_EQUAL_INT(0, fcntl(tp_fd[0], F_GETFL) & O_NONBLOCK);
	TEST_ASSERT_EQUAL_INT(cap, tp_fill(tp_fd[1]));
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, write(tp_fd[1], "x", 1));
	TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
}


/* A write of up to PIPE_BUF bytes goes in whole or not at all */
TEST(pipe_semantics, atomic_nonblocking)
{
	static char rec[TP_ATOMIC];
	int cap, before;
	ssize_t r;

	TEST_ASSERT_EQUAL_INT(0, pipe2(tp_fd, O_NONBLOCK));
	cap = tp_fill(tp_fd[1]);

	/* less than PIPE_BUF free */
	tp_drain(tp_fd[0], 1000);
	before = tp_avail(tp_fd[0]);
	TEST_ASSERT_EQUAL_INT(cap - 1000, before);
	TEST_ASSERT_EQUAL_INT(0, tp_poll(tp_fd[1], POLLOUT) & POLLOUT);
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, write(tp_fd[1], rec, sizeof(rec)));
	TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
	TEST_ASSERT_EQUAL_INT(before, tp_avail(tp_fd[0]));

	/* 512 bytes may fit (Phoenix) or not (Linux: no free page), but never in part */
	r = write(tp_fd[1], rec, 512);
	TEST_ASSERT_TRUE((r == 512) || ((r == -1) && (errno == EAGAIN)));
	TEST_ASSERT_EQUAL_INT(before + ((r > 0) ? 512 : 0), tp_avail(tp_fd[0]));

	/* with room for PIPE_BUF bytes: POLLOUT, and the whole write goes in */
	tp_drain(tp_fd[0], 2 * TP_ATOMIC);
	TEST_ASSERT_EQUAL_INT(POLLOUT, tp_poll(tp_fd[1], POLLOUT));
	before = tp_avail(tp_fd[0]);
	TEST_ASSERT_EQUAL_INT(TP_ATOMIC, write(tp_fd[1], rec, sizeof(rec)));
	TEST_ASSERT_EQUAL_INT(before + TP_ATOMIC, tp_avail(tp_fd[0]));
}


/* Above PIPE_BUF: a non-blocking write is partial, a blocking one complete */
TEST(pipe_semantics, large_writes)
{
	pid_t pid;
	ssize_t r;
	size_t i, got;
	int status, cap;

	for (i = 0; i < sizeof(tp_buf); i++) {
		tp_buf[i] = (char)(i % 251);
	}

	TEST_ASSERT_EQUAL_INT(0, pipe2(tp_fd, O_NONBLOCK));
	r = write(tp_fd[1], tp_buf, TP_BIG);
	TEST_ASSERT_GREATER_THAN_INT(0, (int)r);
	TEST_ASSERT_LESS_THAN_INT(TP_BIG, (int)r);
	cap = (int)r;
	TEST_ASSERT_EQUAL_INT(cap, tp_avail(tp_fd[0]));
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, write(tp_fd[1], tp_buf, TP_BIG));
	TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
	tp_close(&tp_fd[0]);
	tp_close(&tp_fd[1]);

	/* one blocking write of 1 MB, read by a child in odd-sized pieces */
	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	pid = fork();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);
	if (pid == 0) {
		static char in[3001];
		(void)close(tp_fd[1]);
		got = 0;
		while ((r = read(tp_fd[0], in, sizeof(in))) > 0) {
			for (i = 0; i < (size_t)r; i++) {
				if (in[i] != (char)((got + i) % 251)) {
					_exit(2);
				}
			}
			got += (size_t)r;
		}
		_exit((got == TP_BIG) ? 0 : 3);
	}
	tp_close(&tp_fd[0]);
	TEST_ASSERT_EQUAL_INT(TP_BIG, write(tp_fd[1], tp_buf, TP_BIG));
	tp_close(&tp_fd[1]);
	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	TEST_ASSERT_TRUE(WIFEXITED(status));
	TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}


/*
 * Several writer processes, blocking, PIPE_BUF-sized records each filled with
 * one tag; the reader reads odd-sized pieces and finds every record whole.
 */
#define TP_WRITERS 4
#define TP_RECS    500

TEST(pipe_semantics, atomic_writers)
{
	static unsigned char rec[TP_ATOMIC];
	pid_t pid[TP_WRITERS];
	size_t fill = 0, total = 0, chunk = 777;
	unsigned bad = 0, k;
	int w, i, status;
	ssize_t r;

	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	for (w = 0; w < TP_WRITERS; w++) {
		pid[w] = fork();
		TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid[w]);
		if (pid[w] == 0) {
			(void)close(tp_fd[0]);
			for (i = 0; i < TP_RECS; i++) {
				memset(rec, w * 64 + (i % 64), sizeof(rec));
				if (write(tp_fd[1], rec, sizeof(rec)) != (ssize_t)sizeof(rec)) {
					_exit(2);
				}
			}
			_exit(0);
		}
	}
	tp_close(&tp_fd[1]);

	while ((r = read(tp_fd[0], rec + fill, (chunk < sizeof(rec) - fill) ? chunk : sizeof(rec) - fill)) > 0) {
		fill += (size_t)r;
		total += (size_t)r;
		chunk = (chunk == 777) ? 1500 : 777;
		if (fill == sizeof(rec)) {
			for (k = 1; k < sizeof(rec); k++) {
				if (rec[k] != rec[0]) {
					bad++;
					break;
				}
			}
			fill = 0;
		}
	}

	for (w = 0; w < TP_WRITERS; w++) {
		TEST_ASSERT_EQUAL_INT(pid[w], waitpid(pid[w], &status, 0));
		TEST_ASSERT_TRUE(WIFEXITED(status) && (WEXITSTATUS(status) == 0));
	}
	TEST_ASSERT_EQUAL_INT(0, (int)r);
	TEST_ASSERT_EQUAL_INT(TP_WRITERS * TP_RECS * TP_ATOMIC, (int)total);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, bad, "a PIPE_BUF write was interleaved with another");
}


TEST(pipe_semantics, not_seekable)
{
	struct stat st0, st1, other;
	char b = 0;
	int p2[2];

	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, (int)lseek(tp_fd[0], 0, SEEK_CUR));
	TEST_ASSERT_EQUAL_INT(ESPIPE, errno);
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, (int)lseek(tp_fd[1], 0, SEEK_SET));
	TEST_ASSERT_EQUAL_INT(ESPIPE, errno);
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, pwrite(tp_fd[1], &b, 1, 0));
	TEST_ASSERT_EQUAL_INT(ESPIPE, errno);
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, pread(tp_fd[0], &b, 1, 0));
	TEST_ASSERT_EQUAL_INT(ESPIPE, errno);

	TEST_ASSERT_EQUAL_INT(0, fstat(tp_fd[0], &st0));
	TEST_ASSERT_EQUAL_INT(0, fstat(tp_fd[1], &st1));
	TEST_ASSERT_TRUE(S_ISFIFO(st0.st_mode));
	TEST_ASSERT_TRUE(S_ISFIFO(st1.st_mode));
	/* one pipe, one inode; another pipe, another inode */
	TEST_ASSERT_TRUE((st0.st_dev == st1.st_dev) && (st0.st_ino == st1.st_ino));
	TEST_ASSERT_EQUAL_INT(0, pipe(p2));
	TEST_ASSERT_EQUAL_INT(0, fstat(p2[0], &other));
	(void)close(p2[0]);
	(void)close(p2[1]);
	TEST_ASSERT_FALSE((other.st_dev == st0.st_dev) && (other.st_ino == st0.st_ino));

	TEST_ASSERT_EQUAL_INT(0, isatty(tp_fd[0]));
	TEST_ASSERT_EQUAL_INT(0, isatty(tp_fd[1]));
}


TEST(pipe_semantics, poll_events)
{
	fd_set rd, wr;
	struct timeval tv = { 0, 0 };
	char b[16];
	int cap;

	TEST_ASSERT_EQUAL_INT(0, pipe2(tp_fd, O_NONBLOCK));
	TEST_ASSERT_EQUAL_INT(0, tp_poll(tp_fd[0], POLLIN));
	TEST_ASSERT_EQUAL_INT(POLLOUT, tp_poll(tp_fd[1], POLLOUT));

	TEST_ASSERT_EQUAL_INT(1, write(tp_fd[1], "z", 1));
	TEST_ASSERT_EQUAL_INT(POLLIN, tp_poll(tp_fd[0], POLLIN));

	FD_ZERO(&rd);
	FD_ZERO(&wr);
	FD_SET(tp_fd[0], &rd);
	FD_SET(tp_fd[1], &wr);
	TEST_ASSERT_EQUAL_INT(2, select(((tp_fd[0] > tp_fd[1]) ? tp_fd[0] : tp_fd[1]) + 1, &rd, &wr, NULL, &tv));
	TEST_ASSERT_TRUE(FD_ISSET(tp_fd[0], &rd) && FD_ISSET(tp_fd[1], &wr));

	/* full: no POLLOUT */
	cap = tp_fill(tp_fd[1]);
	TEST_ASSERT_EQUAL_INT(0, tp_poll(tp_fd[1], POLLOUT));

	/* the writer gone: POLLIN while bytes are left, POLLHUP; then POLLHUP alone */
	tp_close(&tp_fd[1]);
	TEST_ASSERT_EQUAL_INT(POLLIN | POLLHUP, tp_poll(tp_fd[0], POLLIN));
	tp_drain(tp_fd[0], cap + 1);
	TEST_ASSERT_EQUAL_INT(POLLHUP, tp_poll(tp_fd[0], POLLIN));
	TEST_ASSERT_EQUAL_INT(0, read(tp_fd[0], b, sizeof(b)));
	tp_close(&tp_fd[0]);

	/* the reader gone: POLLERR on the write end, as Linux */
	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	tp_close(&tp_fd[0]);
	TEST_ASSERT_NOT_EQUAL(0, tp_poll(tp_fd[1], POLLOUT) & POLLERR);
}


/* poll() sleeping on a pipe is woken by a write from another process */
TEST(pipe_semantics, poll_wake_from_child)
{
	struct pollfd pfd;
	int64_t t0, us;
	pid_t pid;
	int status;

	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	pid = fork();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);
	if (pid == 0) {
		usleep(100 * 1000);
		_exit((write(tp_fd[1], "w", 1) == 1) ? 0 : 1);
	}

	pfd.fd = tp_fd[0];
	pfd.events = POLLIN;
	pfd.revents = 0;
	t0 = tp_nowUs();
	TEST_ASSERT_EQUAL_INT(1, poll(&pfd, 1, 5000));
	us = tp_nowUs() - t0;
	TEST_ASSERT_EQUAL_INT(POLLIN, pfd.revents);
	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	TEST_ASSERT_LESS_THAN_INT(2 * 1000 * 1000, (int)us);
}


/* Descriptor flags across exec: FD_CLOEXEC ends are gone, the others are not */
TEST(pipe_semantics, cloexec_across_exec)
{
	char keepArg[16], goneArg[16], b = 0;
	int gone, p2[2], status;
	pid_t pid;

	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	gone = fcntl(tp_fd[1], F_DUPFD_CLOEXEC, 50);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(50, gone);
	TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(gone, F_GETFD));
	TEST_ASSERT_EQUAL_INT(0, fcntl(tp_fd[1], F_GETFD));

	TEST_ASSERT_EQUAL_INT(0, pipe2(p2, O_CLOEXEC));
	TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(p2[0], F_GETFD));
	TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(p2[1], F_GETFD));

	(void)snprintf(keepArg, sizeof(keepArg), "%d", tp_fd[1]);
	(void)snprintf(goneArg, sizeof(goneArg), "%d", gone);

	pid = fork();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);
	if (pid == 0) {
		char *argv[] = { (char *)pipe_self, "--pipe-child", "cloexec", keepArg, goneArg, NULL };
		(void)close(tp_fd[0]);
		execvp(pipe_self, argv);
		_exit(9);
	}
	(void)close(gone);
	(void)close(p2[0]);
	(void)close(p2[1]);
	tp_close(&tp_fd[1]);

	TEST_ASSERT_EQUAL_INT(1, read(tp_fd[0], &b, 1));
	TEST_ASSERT_EQUAL_INT('K', b);
	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	TEST_ASSERT_TRUE(WIFEXITED(status));
	TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
	/* the child was the last writer */
	TEST_ASSERT_EQUAL_INT(0, read(tp_fd[0], &b, 1));
}


/* A pipe end passed over an AF_UNIX socket is the same open file */
TEST(pipe_semantics, scm_rights)
{
	char cbuf[CMSG_SPACE(sizeof(int))], b = 0, one = '1';
	struct iovec iov = { .iov_base = &one, .iov_len = 1 };
	struct msghdr msg;
	struct cmsghdr *cmsg;
	int sv[2], rfd = -1;

	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

	memset(&msg, 0, sizeof(msg));
	memset(cbuf, 0, sizeof(cbuf));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &tp_fd[0], sizeof(int));
	TEST_ASSERT_EQUAL_INT(1, sendmsg(sv[0], &msg, 0));
	tp_close(&tp_fd[0]);

	memset(&msg, 0, sizeof(msg));
	memset(cbuf, 0, sizeof(cbuf));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);
	TEST_ASSERT_EQUAL_INT(1, recvmsg(sv[1], &msg, 0));
	cmsg = CMSG_FIRSTHDR(&msg);
	TEST_ASSERT_NOT_NULL(cmsg);
	memcpy(&rfd, CMSG_DATA(cmsg), sizeof(int));
	(void)close(sv[0]);
	(void)close(sv[1]);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, rfd);

	TEST_ASSERT_EQUAL_INT(1, write(tp_fd[1], "s", 1));
	TEST_ASSERT_EQUAL_INT(1, read(rfd, &b, 1));
	TEST_ASSERT_EQUAL_INT('s', b);
	tp_close(&tp_fd[1]);
	TEST_ASSERT_EQUAL_INT(0, read(rfd, &b, 1));
	(void)close(rfd);
}


static pthread_t tp_mainThread;
static volatile int tp_kicked;

static void *tp_kicker(void *arg)
{
	int wr = *(int *)arg;

	usleep(100 * 1000);
	(void)pthread_kill(tp_mainThread, SIGUSR1);
	/* never leave the main thread blocked if the signal did not interrupt it */
	usleep(2000 * 1000);
	if (__atomic_load_n(&tp_kicked, __ATOMIC_ACQUIRE) == 0) {
		if (write(wr, "!", 1) != 1) {
			return NULL;
		}
	}
	return NULL;
}


TEST(pipe_semantics, eintr)
{
	struct sigaction sa;
	pthread_t tid;
	ssize_t r;
	int err;
	char b;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = tp_onUsr1;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0; /* no SA_RESTART */
	TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &sa, NULL));

	TEST_ASSERT_EQUAL_INT(0, pipe(tp_fd));
	tp_usr1 = 0;
	tp_kicked = 0;
	tp_mainThread = pthread_self();
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&tid, NULL, tp_kicker, &tp_fd[1]));
	errno = 0;
	r = read(tp_fd[0], &b, 1);
	err = errno;
	__atomic_store_n(&tp_kicked, 1, __ATOMIC_RELEASE);
	pthread_join(tid, NULL);

	TEST_ASSERT_EQUAL_INT(1, tp_usr1);
	TEST_ASSERT_EQUAL_INT(-1, (int)r);
	TEST_ASSERT_EQUAL_INT(EINTR, err);
}


TEST_GROUP_RUNNER(pipe_semantics)
{
	RUN_TEST_CASE(pipe_semantics, flow_and_eof);
	RUN_TEST_CASE(pipe_semantics, zero_length);
	RUN_TEST_CASE(pipe_semantics, wrong_end);
	RUN_TEST_CASE(pipe_semantics, eof_needs_every_writer);
	RUN_TEST_CASE(pipe_semantics, eof_after_child_exit);
	RUN_TEST_CASE(pipe_semantics, epipe_and_sigpipe);
	RUN_TEST_CASE(pipe_semantics, sigpipe_kills);
	RUN_TEST_CASE(pipe_semantics, nonblock);
	RUN_TEST_CASE(pipe_semantics, atomic_nonblocking);
	RUN_TEST_CASE(pipe_semantics, large_writes);
	RUN_TEST_CASE(pipe_semantics, atomic_writers);
	RUN_TEST_CASE(pipe_semantics, not_seekable);
	RUN_TEST_CASE(pipe_semantics, poll_events);
	RUN_TEST_CASE(pipe_semantics, poll_wake_from_child);
	RUN_TEST_CASE(pipe_semantics, cloexec_across_exec);
	RUN_TEST_CASE(pipe_semantics, scm_rights);
	RUN_TEST_CASE(pipe_semantics, eintr);
}
