/*
 * Phoenix-RTOS
 *
 * test-libc-unix-poll-wake
 *
 * How long after a peer's send() does a poll() blocked on an AF_UNIX socket
 * return? Every case sends a stream of small timestamped messages (CLOCK_MONOTONIC
 * taken just before sendmsg()) to a receiver that sleeps in poll(timeout = -1)
 * and drains the socket with recvmsg(MSG_DONTWAIT) after each return, the way
 * WebKit's IPC (GLib GSource on a SOCK_SEQPACKET pair, SCM_RIGHTS attachments)
 * does. Latency = poll() return - send time.
 *
 *    TESTED (each prints one `UPW case=...` line):
 *    - seqpacket_thread:   SEQPACKET pair, receiver thread, 0-2 ms random gaps
 *    - seqpacket_process:  the same with the receiver in a forked child
 *    - seqpacket_and_pipe: the receiver polls the socket AND a pipe (GLib's
 *                          wake-up pipe); the pipe's own wake latency is reported
 *                          as pipe_* (see pipe_poll_wake below)
 *    - stream_fds, dgram_fds, seqpacket_fds: each type, an fd on every 10th message
 *    - two_writers:        two threads send to the same socket at once
 *    - burst:              64 messages back to back, drained in a recvmsg() loop
 *    - crowd:              4 idle threads also sleep in poll() on other AF_UNIX
 *                          sockets, so every notify finds sleepers
 *    - cpu_load:           one CPU-bound thread per CPU runs throughout
 *    - seqpacket_pollout:  POLLOUT on a full SEQPACKET ring means the message fits,
 *                          and a writer blocked for POLLOUT wakes when it drains
 *
 *    FAILS when a message is lost, duplicated, reordered or malformed, when an fd
 *    does not travel with its message, when any wake takes more than 1 s, or when
 *    a message is not seen within 5 s of the last send, or when a sender finds no
 *    room for 5 s (tx_err=ETIMEDOUT: the receiver stopped draining). `--strict`
 *    also fails a readiness-woken case on any wake >= 15 ms (POLL_INTERVAL is
 *    20 ms: such a wake came from the kernel's fallback timer, i.e. a notify was
 *    lost).
 *    `--count N` changes the number of messages per case (default 5000).
 *
 * The same question for a PIPE as the ready descriptor (group pipe_poll_wake,
 * same `UPW case=...` lines). GLib wakes its main loops through a pipe, so this
 * is the cost of every cross-thread dispatch to a GLib main loop (WebKit's sync
 * IPC). Anonymous pipes live in the kernel (posix/pipe.c), and each state change
 * of one wakes the poll() sets that watch it. They used to be posixsrv objects:
 * poll() learnt of their readiness from an atPollStatus query, and woke at once
 * only when posixsrv called pollNotify() on the change - otherwise the kernel
 * re-asked every POLL_INTERVAL (20 ms). A pty is still served by posixsrv.
 *
 *    TESTED:
 *    - pipe_thread:     8-byte timestamp tokens, writer thread, 0.2-2 ms gaps
 *    - pipe_process:    the same with the writer in a forked child
 *    - pipe_and_unix:   the receiver polls the pipe AND an idle AF_UNIX socket
 *                       (the GLib shape: a mixed set); only the pipe is written
 *    - pipe_pollout:    a writer waits for POLLOUT on a full pipe, the reader
 *                       drains 4 kB (latency = drain -> poll() return)
 *    - pipe_hup:        a reader waits for POLLIN, the last writer closes:
 *                       POLLHUP, then read() = 0
 *    - pipe_hup_writer: a writer waits for POLLOUT on a full pipe, the last
 *                       reader closes: POLLHUP or POLLERR, then write() = EPIPE
 *    - pty_slave_in:    the same for a pty (posixsrv too): a reader polls a raw
 *                       slave, one byte is written to the master
 *    - pty_master_in:   a reader polls the master, one byte is written to the slave
 *    - pipe_nonblock_contention: non-blocking writer and reader plus a poller on
 *                       one pipe for 2 s; FAILS on any EAGAIN the byte counters
 *                       prove spurious (expected to FAIL on a posixsrv that
 *                       try-locks O_NONBLOCK requests, build 39 and older)
 *
 *    FAILS when a token is lost or malformed, a wake takes more than 1 s, a wake
 *    reports the wrong events, or p99 latency is >= 5 ms (UPW_PIPE_P99_US). That
 *    bound sits far below POLL_INTERVAL: it holds only if every pipe readiness
 *    change wakes poll() at once.
 *
 *    EXPECTED on a posixsrv that does NOT call pollNotify() (build 39 and older):
 *    EVERY pipe and pty case FAILS, with p50 ~8-15 ms and max ~20 ms (the fallback
 *    re-query: a uniform 0-20 ms quantum). With the notify: p50 well under 1 ms
 *    (two posixsrv round trips), all PASS; kernel pipes need no round trip at all.
 *    The pipe token count is --count / 5.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "unity_fixture.h"


#define UPW_MSG_SIZE    64
#define UPW_HDR_SIZE    24 /* sent as a separate iovec, as WebKit sends MessageInfo */
#define UPW_MAGIC       0x55505701U
#define UPW_QUIT        0x55505702U
#define UPW_MAX_FDS     16
#define UPW_FAIL_US     1000000U /* any wake slower than this fails */
#define UPW_SLOW_US     100000U
#define UPW_LATE_US     10000U
#define UPW_STRICT_US   15000U
#define UPW_SETTLE_MS   5000 /* every message must be seen this long after the last send */
#define UPW_PIPE_EVERY  50   /* seqpacket_and_pipe: a pipe token every this many messages */
#define UPW_PIPE_QUIET  25000 /* us of socket silence after a pipe token */
#define UPW_CROWD       4
#define UPW_MAX_SPIN    16
#define UPW_FILL_SIZE   4000 /* seqpacket_pollout: WebKit's messageMaxSize is 4096 */
#define UPW_PIPE_P99_US 5000U /* pipe cases: p99 wake latency must stay below this */
#define UPW_PIPE_REPS   50    /* pipe_pollout, pipe_hup*: transitions measured */
#define UPW_PIPE_ARM_US 5000U /* time given to a poller to fall asleep before the change */
#define UPW_PIPE_CHUNK  512   /* pipe fill granule (PIPE_BUF) */
#define UPW_PIPE_DRAIN  4096  /* pipe_pollout: bytes the reader frees (a Linux pipe needs a whole page) */


typedef struct {
	uint32_t magic;
	uint16_t writer;
	uint16_t hasFd;
	uint32_t seq;
	uint32_t pad;
	int64_t sendNs;
	uint8_t body[UPW_MSG_SIZE - UPW_HDR_SIZE];
} upw_msg_t;

_Static_assert(sizeof(upw_msg_t) == UPW_MSG_SIZE, "upw_msg_t is one message");


typedef struct {
	const char *name;
	int type;
	int fork;          /* receiver in a child process */
	int wakePipe;      /* receiver also polls a pipe the sender writes tokens to */
	int fdEvery;       /* an fd on every Nth message, 0: none */
	int writers;       /* 1 or 2 sender threads */
	int burst;         /* > 0: messages back to back in groups of this many */
	int crowd;         /* idle pollers on unrelated AF_UNIX sockets */
	int spin;          /* CPU-bound threads, one per CPU */
	int readinessWoken; /* every wake should come from a notify (--strict) */
} upw_case_t;


typedef struct {
	unsigned expected;
	unsigned recv;
	unsigned lost;
	unsigned dup;
	unsigned bad;
	unsigned fdsRecv;
	unsigned fdMismatch;
	unsigned wakes;
	unsigned emptyWakes;
	unsigned rxErrs;
	unsigned hup;
	unsigned complete;
	unsigned ge10ms;
	unsigned ge15ms;
	unsigned gt100ms;
	unsigned gt1s;
	unsigned p50Us;
	unsigned p99Us;
	unsigned maxUs;
	unsigned pipeN;
	unsigned pipeP50Us;
	unsigned pipeMaxUs;
} upw_stats_t;


typedef struct {
	const upw_case_t *c;
	int sock;
	int pipeRd;
	unsigned perWriter[2];
	unsigned next[2];
	unsigned latCap;
	uint32_t *lat;
	unsigned pipeCap;
	uint32_t *pipeLat;
	int quit;
	unsigned char sbuf[UPW_MSG_SIZE * 64];
	size_t shave;
	upw_stats_t st;
	volatile int done;
} upw_rx_t;


typedef struct {
	const upw_case_t *c;
	int sock;
	int pipeWr;
	int fdToPass;
	unsigned writer;
	unsigned count;
	unsigned seed;
	unsigned sent;
	unsigned fdsSent;
	unsigned eagain;
	int err;
} upw_tx_t;


static struct {
	unsigned count;
	int strict;
	volatile int stopIdle;
} upw_common = { .count = 5000 };


static int64_t upw_nowNs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}


static void upw_sleepUs(unsigned us)
{
	struct timespec ts = { .tv_sec = us / 1000000U, .tv_nsec = (long)(us % 1000000U) * 1000L };

	while ((nanosleep(&ts, &ts) < 0) && (errno == EINTR)) {
	}
}


static int upw_cmpU32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return (x > y) - (x < y);
}


/* p in 0..100, nearest-rank; 0 for an empty set */
static unsigned upw_pct(uint32_t *v, unsigned n, unsigned p)
{
	unsigned idx;

	if (n == 0U) {
		return 0;
	}
	idx = (unsigned)(((uint64_t)n * p + 99U) / 100U);
	return v[(idx == 0U) ? 0U : idx - 1U];
}


static uint32_t upw_latUs(int64_t wakeNs, int64_t sendNs)
{
	int64_t us = (wakeNs - sendNs) / 1000;

	if (us < 0) {
		/* sent after poll() returned and read by the same drain loop */
		return 0;
	}
	return (us > (int64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)us;
}


/* Closes the descriptors a message carried, returns how many there were. */
static unsigned upw_takeFds(struct msghdr *mh)
{
	struct cmsghdr *cmsg;
	unsigned n = 0, i, cnt;
	int fd;

	for (cmsg = CMSG_FIRSTHDR(mh); cmsg != NULL; cmsg = CMSG_NXTHDR(mh, cmsg)) {
		if ((cmsg->cmsg_level != SOL_SOCKET) || (cmsg->cmsg_type != SCM_RIGHTS) || (cmsg->cmsg_len < CMSG_LEN(0))) {
			continue;
		}
		cnt = (unsigned)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
		for (i = 0; i < cnt; ++i) {
			memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
			close(fd);
		}
		n += cnt;
	}
	return n;
}


/* fdGot < 0: the socket type cannot tie descriptors to one message (SOCK_STREAM) */
static void upw_account(upw_rx_t *rx, const upw_msg_t *m, int64_t wakeNs, int fdGot)
{
	upw_stats_t *st = &rx->st;
	unsigned *next;
	uint32_t lat;

	if (m->magic == UPW_QUIT) {
		rx->quit = 1;
		return;
	}
	if ((m->magic != UPW_MAGIC) || (m->writer >= 2U)) {
		st->bad++;
		return;
	}

	next = &rx->next[m->writer];
	if (m->seq == *next) {
		(*next)++;
	}
	else if (m->seq > *next) {
		st->lost += m->seq - *next;
		*next = m->seq + 1U;
	}
	else {
		st->dup++;
	}

	lat = upw_latUs(wakeNs, m->sendNs);
	if (st->recv < rx->latCap) {
		rx->lat[st->recv] = lat;
	}
	st->recv++;

	if ((fdGot >= 0) && ((fdGot != 0) != (m->hasFd != 0U))) {
		st->fdMismatch++;
	}
}


static int upw_isComplete(const upw_rx_t *rx)
{
	return ((rx->next[0] >= rx->perWriter[0]) && (rx->next[1] >= rx->perWriter[1])) ? 1 : 0;
}


/* Reads every queued message without blocking; returns the number read or -1. */
static int upw_drainSocket(upw_rx_t *rx, int64_t wakeNs)
{
	union {
		struct cmsghdr align;
		char buf[CMSG_SPACE(sizeof(int) * UPW_MAX_FDS)];
	} cbuf;
	unsigned char buf[UPW_MSG_SIZE * 2];
	struct msghdr mh;
	struct iovec iov;
	upw_msg_t m;
	ssize_t r;
	unsigned fds;
	int framed = (rx->c->type != SOCK_STREAM) ? 1 : 0;
	int got = 0;

	for (;;) {
		memset(&mh, 0, sizeof(mh));
		if (framed != 0) {
			iov.iov_base = buf;
			iov.iov_len = sizeof(buf);
		}
		else {
			iov.iov_base = rx->sbuf + rx->shave;
			iov.iov_len = sizeof(rx->sbuf) - rx->shave;
		}
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		mh.msg_control = cbuf.buf;
		mh.msg_controllen = sizeof(cbuf.buf);

		r = recvmsg(rx->sock, &mh, MSG_DONTWAIT);
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
				return got;
			}
			rx->st.rxErrs++;
			return -1;
		}
		if (r == 0) {
			/* no case sends an empty message: this is the peer going away */
			rx->st.hup = 1;
			return got;
		}

		fds = upw_takeFds(&mh);
		rx->st.fdsRecv += fds;

		if (framed != 0) {
			if (r != UPW_MSG_SIZE) {
				rx->st.bad++;
			}
			else {
				memcpy(&m, buf, sizeof(m));
				upw_account(rx, &m, wakeNs, (int)fds);
			}
			got++;
		}
		else {
			rx->shave += (size_t)r;
			while (rx->shave >= UPW_MSG_SIZE) {
				memcpy(&m, rx->sbuf, sizeof(m));
				upw_account(rx, &m, wakeNs, -1);
				rx->shave -= UPW_MSG_SIZE;
				memmove(rx->sbuf, rx->sbuf + UPW_MSG_SIZE, rx->shave);
				got++;
			}
		}

		if ((rx->quit != 0) || (upw_isComplete(rx) != 0)) {
			return got;
		}
	}
}


static int upw_drainPipe(upw_rx_t *rx, int64_t wakeNs)
{
	int64_t token;
	int got = 0;

	/* the read end is non-blocking */
	while (read(rx->pipeRd, &token, sizeof(token)) == (ssize_t)sizeof(token)) {
		if (rx->st.pipeN < rx->pipeCap) {
			rx->pipeLat[rx->st.pipeN] = upw_latUs(wakeNs, token);
		}
		rx->st.pipeN++;
		got++;
	}
	return got;
}


static void upw_finish(upw_rx_t *rx)
{
	upw_stats_t *st = &rx->st;
	unsigned i, n = (st->recv < rx->latCap) ? st->recv : rx->latCap;
	unsigned pn = (st->pipeN < rx->pipeCap) ? st->pipeN : rx->pipeCap;

	for (i = 0; i < n; ++i) {
		if (rx->lat[i] >= UPW_LATE_US) {
			st->ge10ms++;
		}
		if (rx->lat[i] >= UPW_STRICT_US) {
			st->ge15ms++;
		}
		if (rx->lat[i] > UPW_SLOW_US) {
			st->gt100ms++;
		}
		if (rx->lat[i] > UPW_FAIL_US) {
			st->gt1s++;
		}
	}
	qsort(rx->lat, n, sizeof(rx->lat[0]), upw_cmpU32);
	st->p50Us = upw_pct(rx->lat, n, 50);
	st->p99Us = upw_pct(rx->lat, n, 99);
	st->maxUs = (n > 0U) ? rx->lat[n - 1U] : 0U;

	qsort(rx->pipeLat, pn, sizeof(rx->pipeLat[0]), upw_cmpU32);
	st->pipeP50Us = upw_pct(rx->pipeLat, pn, 50);
	st->pipeMaxUs = (pn > 0U) ? rx->pipeLat[pn - 1U] : 0U;

	st->complete = (unsigned)upw_isComplete(rx);
}


/* The receiver: poll(timeout = -1), then drain everything that is queued. */
static void upw_receive(upw_rx_t *rx)
{
	struct pollfd pfd[2];
	nfds_t npfd = (rx->pipeRd >= 0) ? 2U : 1U;
	int64_t wakeNs;
	int n, got, r;

	while ((upw_isComplete(rx) == 0) && (rx->quit == 0) && (rx->st.hup == 0U)) {
		pfd[0].fd = rx->sock;
		pfd[0].events = POLLIN;
		pfd[0].revents = 0;
		pfd[1].fd = rx->pipeRd;
		pfd[1].events = POLLIN;
		pfd[1].revents = 0;

		n = poll(pfd, npfd, -1);
		wakeNs = upw_nowNs();
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			rx->st.rxErrs++;
			break;
		}
		rx->st.wakes++;

		got = 0;
		if ((npfd > 1U) && ((pfd[1].revents & POLLIN) != 0)) {
			got += upw_drainPipe(rx, wakeNs);
		}
		if ((pfd[0].revents & POLLNVAL) != 0) {
			rx->st.rxErrs++;
			break;
		}
		if (pfd[0].revents != 0) {
			r = upw_drainSocket(rx, wakeNs);
			if (r < 0) {
				break;
			}
			got += r;
		}
		if (got == 0) {
			rx->st.emptyWakes++;
		}
	}

	upw_finish(rx);
}


static void *upw_receiveThread(void *arg)
{
	upw_rx_t *rx = arg;

	upw_receive(rx);
	__atomic_store_n(&rx->done, 1, __ATOMIC_RELEASE);
	return NULL;
}


static int upw_sendOne(upw_tx_t *tx, const upw_msg_t *m, int withFd)
{
	union {
		struct cmsghdr align;
		char buf[CMSG_SPACE(sizeof(int))];
	} cbuf;
	struct iovec iov[2];
	struct msghdr mh;
	struct pollfd pfd;
	struct cmsghdr *cmsg;
	size_t left = UPW_MSG_SIZE;
	int64_t stuckSince = 0;
	ssize_t r;

	memset(&mh, 0, sizeof(mh));
	if (tx->c->type == SOCK_STREAM) {
		iov[0].iov_base = (void *)m;
		iov[0].iov_len = UPW_MSG_SIZE;
		mh.msg_iovlen = 1;
	}
	else {
		/* header and body apart, as WebKit sends them */
		iov[0].iov_base = (void *)m;
		iov[0].iov_len = UPW_HDR_SIZE;
		iov[1].iov_base = (char *)m + UPW_HDR_SIZE;
		iov[1].iov_len = UPW_MSG_SIZE - UPW_HDR_SIZE;
		mh.msg_iovlen = 2;
	}
	mh.msg_iov = iov;

	if (withFd != 0) {
		memset(&cbuf, 0, sizeof(cbuf));
		mh.msg_control = cbuf.buf;
		mh.msg_controllen = CMSG_SPACE(sizeof(int));
		cmsg = CMSG_FIRSTHDR(&mh);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &tx->fdToPass, sizeof(int));
	}

	/*
	 * WebKit's Connection::sendOutputMessage(): a non-blocking socket, poll(POLLOUT)
	 * on EAGAIN. The wait for room is bounded here, in time rather than per poll()
	 * (a POLLOUT that the next sendmsg() contradicts makes that poll() return at
	 * once): a receiver that never wakes fills the ring, and the case must then
	 * fail rather than hang before its watchdog.
	 */
	for (;;) {
		r = sendmsg(tx->sock, &mh, 0);
		if (r == (ssize_t)left) {
			return 0;
		}
		if ((r > 0) && (tx->c->type == SOCK_STREAM) && ((size_t)r < left)) {
			/* a byte stream takes what fits; the fd went with the first byte */
			left -= (size_t)r;
			iov[0].iov_base = (char *)m + (UPW_MSG_SIZE - left);
			iov[0].iov_len = left;
			mh.msg_iovlen = 1;
			mh.msg_control = NULL;
			mh.msg_controllen = 0;
			continue;
		}
		if (r >= 0) {
			/* a frame goes whole or not at all */
			tx->err = EMSGSIZE;
			return -1;
		}
		if (errno == EINTR) {
			continue;
		}
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
			tx->eagain++;
			if (stuckSince == 0) {
				stuckSince = upw_nowNs();
			}
			else if ((upw_nowNs() - stuckSince) > (int64_t)UPW_SETTLE_MS * 1000000LL) {
				tx->err = ETIMEDOUT;
				return -1;
			}
			pfd.fd = tx->sock;
			pfd.events = POLLOUT;
			pfd.revents = 0;
			(void)poll(&pfd, 1, UPW_SETTLE_MS);
			continue;
		}
		tx->err = errno;
		return -1;
	}
}


static void *upw_sendThread(void *arg)
{
	upw_tx_t *tx = arg;
	const upw_case_t *c = tx->c;
	upw_msg_t m;
	unsigned i;
	int withFd;
	int64_t token;

	memset(&m, 0, sizeof(m));
	m.magic = UPW_MAGIC;
	m.writer = (uint16_t)tx->writer;
	memset(m.body, 0xa5, sizeof(m.body));

	for (i = 0; i < tx->count; ++i) {
		if (c->burst > 0) {
			if ((i != 0U) && ((i % (unsigned)c->burst) == 0U)) {
				upw_sleepUs(2000U + (unsigned)rand_r(&tx->seed) % 4000U);
			}
		}
		else {
			unsigned gap = (unsigned)rand_r(&tx->seed) % 2001U;
			if (gap != 0U) {
				upw_sleepUs(gap);
			}
		}

		withFd = ((c->fdEvery > 0) && ((i % (unsigned)c->fdEvery) == (unsigned)c->fdEvery - 1U)) ? 1 : 0;
		m.seq = i;
		m.hasFd = (uint16_t)withFd;
		m.sendNs = upw_nowNs();
		if (upw_sendOne(tx, &m, withFd) < 0) {
			break;
		}
		tx->sent++;
		tx->fdsSent += (unsigned)withFd;

		if ((tx->pipeWr >= 0) && ((i % UPW_PIPE_EVERY) == UPW_PIPE_EVERY / 2U) && (i + 1U < tx->count)) {
			/* let the socket go quiet, so only the pipe can wake the receiver */
			upw_sleepUs(2000);
			token = upw_nowNs();
			if (write(tx->pipeWr, &token, sizeof(token)) != (ssize_t)sizeof(token)) {
				tx->err = errno;
				break;
			}
			upw_sleepUs(UPW_PIPE_QUIET);
		}
	}

	return NULL;
}


/* Sleeps in poll() on a socket nobody writes to until told to quit; counts its wakes. */
typedef struct {
	int sv[2];
	unsigned wakes;
	pthread_t tid;
} upw_idle_t;


static void *upw_idleThread(void *arg)
{
	upw_idle_t *id = arg;
	struct pollfd pfd;
	char ch;

	for (;;) {
		pfd.fd = id->sv[1];
		pfd.events = POLLIN;
		pfd.revents = 0;
		if ((poll(&pfd, 1, -1) < 0) && (errno != EINTR)) {
			break;
		}
		id->wakes++;
		if ((pfd.revents & POLLIN) != 0) {
			(void)recv(id->sv[1], &ch, 1, MSG_DONTWAIT);
			break;
		}
		if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
			break;
		}
	}
	return NULL;
}


static void *upw_spinThread(void *arg)
{
	volatile unsigned long *ctr = arg;

	while (__atomic_load_n(&upw_common.stopIdle, __ATOMIC_RELAXED) == 0) {
		(*ctr)++;
	}
	return NULL;
}


static void upw_sendQuit(int sock)
{
	upw_msg_t m;

	memset(&m, 0, sizeof(m));
	m.magic = UPW_QUIT;
	(void)send(sock, &m, sizeof(m), MSG_DONTWAIT);
}


/* Waits up to ms for *flag; returns 1 if it got set. */
static int upw_waitFlag(volatile int *flag, unsigned ms)
{
	int64_t end = upw_nowNs() + (int64_t)ms * 1000000LL;

	while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) == 0) {
		if (upw_nowNs() >= end) {
			return 0;
		}
		upw_sleepUs(2000);
	}
	return 1;
}


/* Reads a stats record from a child; returns 1 when it arrived within ms. */
static int upw_readStats(int fd, upw_stats_t *st, unsigned ms)
{
	struct pollfd pfd;
	int64_t end = upw_nowNs() + (int64_t)ms * 1000000LL;
	size_t have = 0;
	ssize_t r;
	int left;

	while (have < sizeof(*st)) {
		left = (int)((end - upw_nowNs()) / 1000000LL);
		if (left <= 0) {
			return 0;
		}
		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, left) <= 0) {
			continue;
		}
		r = read(fd, (char *)st + have, sizeof(*st) - have);
		if (r <= 0) {
			return 0;
		}
		have += (size_t)r;
	}
	return 1;
}


static const char *upw_typeName(int type)
{
	switch (type) {
		case SOCK_STREAM:
			return "stream";
		case SOCK_DGRAM:
			return "dgram";
		default:
			return "seqpacket";
	}
}


typedef struct {
	upw_stats_t st;
	unsigned sent;
	unsigned fdsSent;
	unsigned eagain;
	int txErr;
	int rxTimedOut;
	unsigned idleWakes;
	unsigned long spins;
	int64_t elapsedMs;
} upw_result_t;


static void upw_run(const upw_case_t *c, upw_result_t *res)
{
	int sv[2], wp[2] = { -1, -1 }, fp[2] = { -1, -1 }, rp[2] = { -1, -1 };
	upw_idle_t idle[UPW_CROWD];
	pthread_t spinTid[UPW_MAX_SPIN], txTid[2], rxTid;
	volatile unsigned long spinCtr[UPW_MAX_SPIN];
	upw_tx_t tx[2];
	upw_rx_t *rx;
	unsigned i, nspin = 0, writers = (c->writers > 1) ? 2U : 1U;
	unsigned total = upw_common.count;
	int64_t t0;
	pid_t pid = -1;
	int status;

	memset(res, 0, sizeof(*res));
	t0 = upw_nowNs();

	TEST_ASSERT_EQUAL_INT_MESSAGE(0, socketpair(AF_UNIX, c->type, 0, sv), "socketpair");
	/* the sender end is non-blocking, as in WebKit; the receiver uses MSG_DONTWAIT */
	TEST_ASSERT_EQUAL_INT(0, fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK));
	if (c->wakePipe != 0) {
		TEST_ASSERT_EQUAL_INT(0, pipe(wp));
		TEST_ASSERT_EQUAL_INT(0, fcntl(wp[0], F_SETFL, fcntl(wp[0], F_GETFL) | O_NONBLOCK));
	}
	if (c->fdEvery > 0) {
		TEST_ASSERT_EQUAL_INT(0, pipe(fp));
	}

	rx = calloc(1, sizeof(*rx));
	TEST_ASSERT_NOT_NULL(rx);
	rx->c = c;
	rx->sock = sv[1];
	rx->pipeRd = wp[0];
	for (i = 0; i < writers; ++i) {
		rx->perWriter[i] = total / writers;
	}
	rx->st.expected = (total / writers) * writers;

	for (i = 0; i < (unsigned)c->crowd && i < UPW_CROWD; ++i) {
		idle[i].wakes = 0;
		TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_SEQPACKET, 0, idle[i].sv));
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&idle[i].tid, NULL, upw_idleThread, &idle[i]));
	}

	__atomic_store_n(&upw_common.stopIdle, 0, __ATOMIC_RELAXED);
	if (c->spin != 0) {
		long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
		nspin = (ncpu < 1) ? 1U : ((ncpu > UPW_MAX_SPIN) ? UPW_MAX_SPIN : (unsigned)ncpu);
		for (i = 0; i < nspin; ++i) {
			spinCtr[i] = 0;
			TEST_ASSERT_EQUAL_INT(0, pthread_create(&spinTid[i], NULL, upw_spinThread, (void *)&spinCtr[i]));
		}
	}

	if (c->fork != 0) {
		TEST_ASSERT_EQUAL_INT(0, pipe(rp));
		fflush(stdout);
		pid = fork();
		TEST_ASSERT_TRUE_MESSAGE(pid >= 0, "fork");
		if (pid == 0) {
			close(sv[0]);
			close(rp[0]);
			rx->latCap = total;
			rx->lat = calloc(total, sizeof(uint32_t));
			if (rx->lat == NULL) {
				_exit(2);
			}
			upw_receive(rx);
			_exit((write(rp[1], &rx->st, sizeof(rx->st)) == (ssize_t)sizeof(rx->st)) ? 0 : 3);
		}
		close(rp[1]);
	}
	else {
		rx->latCap = total;
		rx->lat = calloc(total, sizeof(uint32_t));
		rx->pipeCap = total / UPW_PIPE_EVERY + 1U;
		rx->pipeLat = calloc(rx->pipeCap, sizeof(uint32_t));
		TEST_ASSERT_NOT_NULL(rx->lat);
		TEST_ASSERT_NOT_NULL(rx->pipeLat);
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&rxTid, NULL, upw_receiveThread, rx));
	}

	for (i = 0; i < writers; ++i) {
		memset(&tx[i], 0, sizeof(tx[i]));
		tx[i].c = c;
		tx[i].sock = sv[0];
		tx[i].pipeWr = (i == 0U) ? wp[1] : -1;
		tx[i].fdToPass = fp[0];
		tx[i].writer = i;
		tx[i].count = total / writers;
		tx[i].seed = 0x5eed0000U + i;
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&txTid[i], NULL, upw_sendThread, &tx[i]));
	}
	for (i = 0; i < writers; ++i) {
		pthread_join(txTid[i], NULL);
		res->sent += tx[i].sent;
		res->fdsSent += tx[i].fdsSent;
		res->eagain += tx[i].eagain;
		if ((res->txErr == 0) && (tx[i].err != 0)) {
			res->txErr = tx[i].err;
		}
	}

	if (c->fork != 0) {
		if (upw_readStats(rp[0], &res->st, UPW_SETTLE_MS) == 0) {
			res->rxTimedOut = 1;
			upw_sendQuit(sv[0]);
			if (upw_readStats(rp[0], &res->st, 2000) == 0) {
				kill(pid, SIGKILL);
			}
		}
		(void)waitpid(pid, &status, 0);
		close(rp[0]);
	}
	else {
		if (upw_waitFlag(&rx->done, UPW_SETTLE_MS) == 0) {
			res->rxTimedOut = 1;
			upw_sendQuit(sv[0]);
			if (upw_waitFlag(&rx->done, 2000) == 0) {
				/* still asleep in poll(): a hang-up is the last thing that can wake it */
				shutdown(sv[0], SHUT_RDWR);
				(void)upw_waitFlag(&rx->done, 2000);
			}
		}
		if (__atomic_load_n(&rx->done, __ATOMIC_ACQUIRE) != 0) {
			pthread_join(rxTid, NULL);
			res->st = rx->st;
			free(rx->lat);
			free(rx->pipeLat);
		}
		else {
			/* never woke: report what it saw and leave it (and its memory) be */
			res->st = rx->st;
			res->st.complete = 0;
			rx = NULL;
		}
	}

	__atomic_store_n(&upw_common.stopIdle, 1, __ATOMIC_RELAXED);
	for (i = 0; i < nspin; ++i) {
		pthread_join(spinTid[i], NULL);
		res->spins += spinCtr[i];
	}
	for (i = 0; i < (unsigned)c->crowd && i < UPW_CROWD; ++i) {
		(void)send(idle[i].sv[0], "q", 1, 0);
		pthread_join(idle[i].tid, NULL);
		res->idleWakes += idle[i].wakes;
		close(idle[i].sv[0]);
		close(idle[i].sv[1]);
	}

	close(sv[0]);
	if (rx != NULL) {
		close(sv[1]);
		free(rx);
	}
	if (wp[0] >= 0) {
		close(wp[0]);
		close(wp[1]);
	}
	if (fp[0] >= 0) {
		close(fp[0]);
		close(fp[1]);
	}

	res->elapsedMs = (upw_nowNs() - t0) / 1000000LL;
}


static void upw_report(const upw_case_t *c, const upw_result_t *res)
{
	const upw_stats_t *st = &res->st;

	printf("UPW case=%s type=%s expected=%u sent=%u recv=%u lost=%u dup=%u bad=%u fds_sent=%u fds_recv=%u fd_mismatch=%u "
		   "wakes=%u empty_wakes=%u p50_us=%u p99_us=%u max_us=%u ge10ms=%u ge15ms=%u gt100ms=%u gt1s=%u "
		   "complete=%u rx_timeout=%d rx_errs=%u tx_err=%d tx_eagain=%u",
		c->name, upw_typeName(c->type), st->expected, res->sent, st->recv, st->lost, st->dup, st->bad, res->fdsSent, st->fdsRecv,
		st->fdMismatch, st->wakes, st->emptyWakes, st->p50Us, st->p99Us, st->maxUs, st->ge10ms, st->ge15ms, st->gt100ms,
		st->gt1s, st->complete, res->rxTimedOut, st->rxErrs, res->txErr, res->eagain);
	if (c->wakePipe != 0) {
		printf(" pipe_n=%u pipe_p50_us=%u pipe_max_us=%u", st->pipeN, st->pipeP50Us, st->pipeMaxUs);
	}
	if (c->crowd != 0) {
		printf(" idle_wakes=%u", res->idleWakes);
	}
	if (c->spin != 0) {
		printf(" spins=%lu", res->spins);
	}
	printf(" elapsed_ms=%lld\n", (long long)res->elapsedMs);
	fflush(stdout);
}


static void upw_check(const upw_case_t *c)
{
	upw_result_t res;
	const upw_stats_t *st = &res.st;
	int ok;

	upw_run(c, &res);
	upw_report(c, &res);

	ok = (res.txErr == 0) && (res.rxTimedOut == 0) && (st->complete != 0U) && (st->recv == st->expected) &&
		(st->lost == 0U) && (st->dup == 0U) && (st->bad == 0U) && (st->rxErrs == 0U) && (st->fdMismatch == 0U) &&
		(st->fdsRecv == res.fdsSent) && (st->gt1s == 0U) && (st->pipeMaxUs <= UPW_FAIL_US) &&
		((upw_common.strict == 0) || (c->readinessWoken == 0) || (st->ge15ms == 0U));
	printf("UPW case=%s verdict=%s\n", c->name, (ok != 0) ? "PASS" : "FAIL");
	fflush(stdout);

	TEST_ASSERT_EQUAL_INT_MESSAGE(0, res.txErr, "sendmsg() failed (errno)");
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, res.rxTimedOut, "a message was not seen within 5 s of the last send");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, st->lost, "messages lost (sequence gap)");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, st->dup, "messages duplicated or reordered");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, st->bad, "malformed messages");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, st->rxErrs, "poll()/recvmsg() failed");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(st->expected, st->recv, "not every message arrived");
	TEST_ASSERT_TRUE_MESSAGE(st->complete != 0U, "receiver did not complete");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, st->fdMismatch, "an fd did not travel with its message");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(res.fdsSent, st->fdsRecv, "fds sent != fds received");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, st->gt1s, "a poll() wake took more than 1 s");
	TEST_ASSERT_TRUE_MESSAGE(st->pipeMaxUs <= UPW_FAIL_US, "a pipe wake took more than 1 s");
	if ((upw_common.strict != 0) && (c->readinessWoken != 0)) {
		TEST_ASSERT_EQUAL_UINT_MESSAGE(0, st->ge15ms, "--strict: a wake >= 15 ms came from the fallback timer (lost notify)");
	}
}


TEST_GROUP(unix_poll_wake);


TEST_SETUP(unix_poll_wake)
{
}


TEST_TEAR_DOWN(unix_poll_wake)
{
}


/* a) */
TEST(unix_poll_wake, seqpacket_thread)
{
	static const upw_case_t c = { .name = "seqpacket_thread", .type = SOCK_SEQPACKET, .writers = 1, .readinessWoken = 1 };

	upw_check(&c);
}


/* b) */
TEST(unix_poll_wake, seqpacket_process)
{
	static const upw_case_t c = { .name = "seqpacket_process", .type = SOCK_SEQPACKET, .fork = 1, .writers = 1, .readinessWoken = 1 };

	upw_check(&c);
}


/* c) the GLib shape: the socket next to a wake-up pipe */
TEST(unix_poll_wake, seqpacket_and_pipe)
{
	static const upw_case_t c = { .name = "seqpacket_and_pipe", .type = SOCK_SEQPACKET, .wakePipe = 1, .writers = 1, .readinessWoken = 1 };

	upw_check(&c);
}


/* d) */
TEST(unix_poll_wake, stream_fds)
{
	static const upw_case_t c = { .name = "stream_fds", .type = SOCK_STREAM, .fdEvery = 10, .writers = 1, .readinessWoken = 1 };

	upw_check(&c);
}


TEST(unix_poll_wake, dgram_fds)
{
	static const upw_case_t c = { .name = "dgram_fds", .type = SOCK_DGRAM, .fdEvery = 10, .writers = 1, .readinessWoken = 1 };

	upw_check(&c);
}


TEST(unix_poll_wake, seqpacket_fds)
{
	static const upw_case_t c = { .name = "seqpacket_fds", .type = SOCK_SEQPACKET, .fdEvery = 10, .writers = 1, .readinessWoken = 1 };

	upw_check(&c);
}


/* e) */
TEST(unix_poll_wake, two_writers)
{
	static const upw_case_t c = { .name = "two_writers", .type = SOCK_SEQPACKET, .fdEvery = 10, .writers = 2, .readinessWoken = 1 };

	upw_check(&c);
}


/* f) */
TEST(unix_poll_wake, burst)
{
	static const upw_case_t c = { .name = "burst", .type = SOCK_SEQPACKET, .burst = 64, .writers = 1, .readinessWoken = 1 };

	upw_check(&c);
}


/* Every send then finds other pollers asleep on AF_UNIX readiness. */
TEST(unix_poll_wake, crowd)
{
	static const upw_case_t c = { .name = "crowd", .type = SOCK_SEQPACKET, .crowd = UPW_CROWD, .writers = 1, .readinessWoken = 1 };

	upw_check(&c);
}


/* Wake-up to running: every CPU is busy with an equal-priority thread. */
TEST(unix_poll_wake, cpu_load)
{
	static const upw_case_t c = { .name = "cpu_load", .type = SOCK_SEQPACKET, .spin = 1, .writers = 1 };

	upw_check(&c);
}


typedef struct {
	int sock;
	int rc;
	short revents;
	int64_t wokeNs;
} upw_pollout_t;


static void *upw_polloutThread(void *arg)
{
	upw_pollout_t *p = arg;
	struct pollfd pfd = { .fd = p->sock, .events = POLLOUT, .revents = 0 };

	p->rc = poll(&pfd, 1, 3000);
	p->wokeNs = upw_nowNs();
	p->revents = pfd.revents;
	return NULL;
}


/*
 * WebKit's sender loops on sendmsg() -> EAGAIN -> poll(POLLOUT). That only waits
 * if POLLOUT means "the message fits": otherwise it spins at 100% CPU until the
 * reader has drained enough. And a writer waiting for POLLOUT has to wake when
 * the reader drains the ring.
 */
TEST(unix_poll_wake, seqpacket_pollout)
{
	int sv[2];
	char *buf;
	unsigned queued = 0;
	ssize_t r;
	struct pollfd pfd;
	pthread_t tid;
	upw_pollout_t p;
	int64_t drainedNs;
	unsigned wakeUs;
	int fits = -1;

	buf = calloc(1, UPW_FILL_SIZE);
	TEST_ASSERT_NOT_NULL(buf);
	TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sv));

	for (;;) {
		r = send(sv[0], buf, UPW_FILL_SIZE, 0);
		if (r < 0) {
			break;
		}
		TEST_ASSERT_EQUAL_INT(UPW_FILL_SIZE, r);
		queued++;
		TEST_ASSERT_TRUE_MESSAGE(queued < 10000U, "the ring never fills");
	}
	TEST_ASSERT_TRUE((errno == EAGAIN) || (errno == EWOULDBLOCK));
	TEST_ASSERT_TRUE(queued > 0U);

	pfd.fd = sv[0];
	pfd.events = POLLOUT;
	pfd.revents = 0;
	r = poll(&pfd, 1, 0);
	if ((r == 1) && ((pfd.revents & POLLOUT) != 0)) {
		r = send(sv[0], buf, UPW_FILL_SIZE, 0);
		fits = (r == UPW_FILL_SIZE) ? 1 : 0;
		if (r == UPW_FILL_SIZE) {
			queued++;
		}
	}

	/* a writer blocked for POLLOUT; the reader drains the ring 50 ms later */
	p.sock = sv[0];
	p.rc = -2;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&tid, NULL, upw_polloutThread, &p));
	upw_sleepUs(50000);
	drainedNs = upw_nowNs();
	while (queued > 0U) {
		r = recv(sv[1], buf, UPW_FILL_SIZE, MSG_DONTWAIT);
		if (r != UPW_FILL_SIZE) {
			break;
		}
		queued--;
	}
	pthread_join(tid, NULL);
	wakeUs = upw_latUs(p.wokeNs, drainedNs);

	printf("UPW case=seqpacket_pollout fill=%d full_reports_pollout=%d pollout_then_fits=%d writer_rc=%d writer_revents=0x%x writer_wake_us=%u left=%u\n",
		UPW_FILL_SIZE, (fits >= 0) ? 1 : 0, fits, p.rc, (unsigned)p.revents, wakeUs, queued);
	fflush(stdout);

	close(sv[0]);
	close(sv[1]);
	free(buf);

	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, queued, "the reader could not drain the ring");
	TEST_ASSERT_TRUE_MESSAGE(fits != 0, "poll() reported POLLOUT but the message did not fit (a POLLOUT waiter spins)");
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, p.rc, "a writer waiting for POLLOUT was not woken when the ring drained");
	TEST_ASSERT_TRUE((p.revents & POLLOUT) != 0);
	TEST_ASSERT_TRUE_MESSAGE(wakeUs < UPW_FAIL_US, "POLLOUT wake took more than 1 s");
}


/* ---- pipe_poll_wake: a pipe is the descriptor that becomes ready ---------- */


typedef struct {
	const char *name;
	int fork;     /* the writer is a forked child */
	int withUnix; /* the receiver also polls an AF_UNIX socket nobody writes to */
} upw_pipeCase_t;


typedef struct {
	int wr;
	unsigned count;
	unsigned seed;
	unsigned sent;
	int err;
} upw_pipeTx_t;


typedef struct {
	unsigned recv;
	unsigned bad;
	unsigned wakes;
	unsigned emptyWakes;
	unsigned rxErrs;
	unsigned unixWakes;
	int eof;
	int timedOut;
	unsigned latCap;
	uint32_t *lat;
} upw_pipeRx_t;


typedef struct {
	int fd;
	short events;
	volatile int armed;
	int rc;
	short revents;
	int64_t wokeNs;
} upw_pipeWaiter_t;


enum { upw_pipePollout, upw_pipeHup, upw_pipeHupWriter };


static struct sigaction upw_oldSigpipe;


static unsigned upw_pipeCount(void)
{
	unsigned n = upw_common.count / 5U;

	return (n < 2U) ? 2U : n;
}


/* Writes `count` CLOCK_MONOTONIC tokens, 0.2-2 ms apart, to a blocking write end. */
static void upw_pipeSend(upw_pipeTx_t *tx)
{
	int64_t token;
	unsigned i;
	ssize_t r;

	for (i = 0; i < tx->count; ++i) {
		upw_sleepUs(200U + (unsigned)rand_r(&tx->seed) % 1801U);
		token = upw_nowNs();
		do {
			r = write(tx->wr, &token, sizeof(token));
		} while ((r < 0) && (errno == EINTR));
		if (r != (ssize_t)sizeof(token)) {
			tx->err = (r < 0) ? errno : EIO;
			return;
		}
		tx->sent++;
	}
}


static void *upw_pipeSendThread(void *arg)
{
	upw_pipeSend(arg);
	return NULL;
}


/* Reads every queued token from the non-blocking read end; returns the number read. */
static int upw_pipeDrain(upw_pipeRx_t *rx, int rd, int64_t wakeNs, unsigned char *buf, size_t *have)
{
	int64_t token;
	ssize_t r;
	int got = 0;

	for (;;) {
		r = read(rd, buf + *have, 512U - *have);
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			if ((errno != EAGAIN) && (errno != EWOULDBLOCK)) {
				rx->rxErrs++;
			}
			return got;
		}
		if (r == 0) {
			rx->eof = 1;
			return got;
		}

		*have += (size_t)r;
		while (*have >= sizeof(token)) {
			memcpy(&token, buf, sizeof(token));
			if ((token <= 0) || (token > upw_nowNs())) {
				rx->bad++;
			}
			else {
				if (rx->recv < rx->latCap) {
					rx->lat[rx->recv] = upw_latUs(wakeNs, token);
				}
				rx->recv++;
			}
			got++;
			*have -= sizeof(token);
			memmove(buf, buf + sizeof(token), *have);
		}
	}
}


/* poll() on the read end (and the idle socket, if any) until `expected` tokens arrived. */
static void upw_pipeReceive(upw_pipeRx_t *rx, int rd, int idleSock, unsigned expected)
{
	struct pollfd pfd[2];
	unsigned char buf[512];
	size_t have = 0;
	nfds_t n = 0, pi;
	int64_t wakeNs;
	int r;

	if (idleSock >= 0) {
		pfd[n].fd = idleSock;
		pfd[n].events = POLLIN;
		n++;
	}
	pi = n;
	pfd[n].fd = rd;
	pfd[n].events = POLLIN;
	n++;

	while ((rx->recv + rx->bad < expected) && (rx->eof == 0)) {
		pfd[0].revents = 0;
		pfd[1].revents = 0;
		r = poll(pfd, n, UPW_SETTLE_MS);
		wakeNs = upw_nowNs();
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			rx->rxErrs++;
			break;
		}
		if (r == 0) {
			rx->timedOut = 1;
			break;
		}
		rx->wakes++;
		if ((pi > 0U) && (pfd[0].revents != 0)) {
			/* nobody writes to it: a spurious event, and a busy loop if it persists */
			rx->unixWakes++;
			break;
		}
		if ((pfd[pi].revents & (POLLERR | POLLNVAL)) != 0) {
			rx->rxErrs++;
			break;
		}
		if (upw_pipeDrain(rx, rd, wakeNs, buf, &have) == 0) {
			rx->emptyWakes++;
		}
	}
}


static void upw_pipeCheck(const upw_pipeCase_t *c)
{
	int p[2] = { -1, -1 }, sv[2] = { -1, -1 };
	unsigned expected = upw_pipeCount(), n, i, ge10ms = 0, ge15ms = 0, gt1s = 0, p50, p99, maxUs;
	upw_pipeTx_t tx;
	upw_pipeRx_t rx;
	pthread_t tid;
	pid_t pid = -1;
	int status = 0, ok, complete;
	int64_t t0 = upw_nowNs();

	memset(&tx, 0, sizeof(tx));
	memset(&rx, 0, sizeof(rx));

	TEST_ASSERT_EQUAL_INT(0, pipe(p));
	TEST_ASSERT_EQUAL_INT(0, fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK));
	if (c->withUnix != 0) {
		TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
	}
	rx.latCap = expected;
	rx.lat = calloc(expected, sizeof(uint32_t));
	TEST_ASSERT_NOT_NULL(rx.lat);

	tx.wr = p[1];
	tx.count = expected;
	tx.seed = 0x50495045U;

	if (c->fork != 0) {
		fflush(stdout);
		pid = fork();
		TEST_ASSERT_TRUE_MESSAGE(pid >= 0, "fork");
		if (pid == 0) {
			close(p[0]);
			upw_pipeSend(&tx);
			_exit((tx.err == 0) ? 0 : 1);
		}
		/* the child holds the write end now: its exit is the end of the stream */
		close(p[1]);
		p[1] = -1;
	}
	else {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&tid, NULL, upw_pipeSendThread, &tx));
	}

	upw_pipeReceive(&rx, p[0], sv[1], expected);
	complete = (rx.recv == expected) ? 1 : 0;

	if (complete == 0) {
		/* a writer blocked on a full pipe gets EPIPE (SIGPIPE is ignored) */
		close(p[0]);
		p[0] = -1;
	}
	if (c->fork != 0) {
		if (complete == 0) {
			kill(pid, SIGKILL);
		}
		(void)waitpid(pid, &status, 0);
		tx.sent = (complete != 0) ? expected : 0U;
		if ((complete != 0) && (!WIFEXITED(status) || (WEXITSTATUS(status) != 0))) {
			tx.err = ECHILD;
		}
	}
	else {
		pthread_join(tid, NULL);
	}

	n = (rx.recv < rx.latCap) ? rx.recv : rx.latCap;
	for (i = 0; i < n; ++i) {
		ge10ms += (rx.lat[i] >= UPW_LATE_US) ? 1U : 0U;
		ge15ms += (rx.lat[i] >= UPW_STRICT_US) ? 1U : 0U;
		gt1s += (rx.lat[i] > UPW_FAIL_US) ? 1U : 0U;
	}
	qsort(rx.lat, n, sizeof(rx.lat[0]), upw_cmpU32);
	p50 = upw_pct(rx.lat, n, 50);
	p99 = upw_pct(rx.lat, n, 99);
	maxUs = (n > 0U) ? rx.lat[n - 1U] : 0U;
	free(rx.lat);

	for (i = 0; i < 2U; ++i) {
		if (p[i] >= 0) {
			close(p[i]);
		}
		if (sv[i] >= 0) {
			close(sv[i]);
		}
	}

	printf("UPW case=%s expected=%u sent=%u recv=%u bad=%u wakes=%u empty_wakes=%u p50_us=%u p99_us=%u max_us=%u "
		   "ge10ms=%u ge15ms=%u gt1s=%u rx_timeout=%d rx_errs=%u eof=%d tx_err=%d",
		c->name, expected, tx.sent, rx.recv, rx.bad, rx.wakes, rx.emptyWakes, p50, p99, maxUs, ge10ms, ge15ms, gt1s,
		rx.timedOut, rx.rxErrs, rx.eof, tx.err);
	if (c->withUnix != 0) {
		printf(" unix_wakes=%u", rx.unixWakes);
	}
	printf(" elapsed_ms=%lld\n", (long long)((upw_nowNs() - t0) / 1000000LL));

	ok = (complete != 0) && (tx.err == 0) && (rx.bad == 0U) && (rx.rxErrs == 0U) && (rx.unixWakes == 0U) &&
		(gt1s == 0U) && (p99 < UPW_PIPE_P99_US);
	printf("UPW case=%s verdict=%s\n", c->name, (ok != 0) ? "PASS" : "FAIL");
	fflush(stdout);

	TEST_ASSERT_EQUAL_INT_MESSAGE(0, tx.err, "write() to the pipe failed (errno)");
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, rx.timedOut, "no token for 5 s");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, rx.bad, "malformed tokens");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, rx.rxErrs, "poll()/read() failed");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, rx.unixWakes, "poll() reported the idle AF_UNIX socket");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(expected, rx.recv, "not every token arrived");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, gt1s, "a pipe wake took more than 1 s");
	TEST_ASSERT_TRUE_MESSAGE(p99 < UPW_PIPE_P99_US, "pipe p99 wake >= 5 ms: pipe readiness does not wake poll()");
}


static void *upw_pipeWaitThread(void *arg)
{
	upw_pipeWaiter_t *w = arg;
	struct pollfd pfd = { .fd = w->fd, .events = w->events, .revents = 0 };

	__atomic_store_n(&w->armed, 1, __ATOMIC_RELEASE);
	w->rc = poll(&pfd, 1, 3000);
	w->wokeNs = upw_nowNs();
	w->revents = pfd.revents;
	return NULL;
}


/* Fills a pipe through its non-blocking write end; returns the bytes queued. */
static size_t upw_pipeFill(int wr)
{
	static const char chunk[UPW_PIPE_CHUNK];
	size_t queued = 0;
	ssize_t r;

	while (queued < (1U << 20)) {
		r = write(wr, chunk, sizeof(chunk));
		if (r <= 0) {
			break;
		}
		queued += (size_t)r;
	}
	return queued;
}


/*
 * One readiness change at a time, UPW_PIPE_REPS times: a thread sleeps in
 * poll() on one end of a fresh pipe and the main thread changes the other end.
 */
static void upw_pipeTransition(const char *name, int kind)
{
	static char buf[UPW_PIPE_DRAIN]; /* off the 12 kB main stack */
	uint32_t lat[UPW_PIPE_REPS];
	unsigned rep, n = 0, timeouts = 0, badEvents = 0, badAfter = 0, setupErrs = 0, ge10ms = 0, p50, p99, maxUs, i;
	upw_pipeWaiter_t w;
	pthread_t tid;
	int64_t changeNs;
	ssize_t r;
	int p[2], ok;
	short want;

	for (rep = 0; rep < UPW_PIPE_REPS; ++rep) {
		if (pipe(p) != 0) {
			setupErrs++;
			break;
		}

		memset(&w, 0, sizeof(w));
		if (kind == upw_pipeHup) {
			w.fd = p[0];
			w.events = POLLIN;
		}
		else {
			if ((fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL) | O_NONBLOCK) != 0) || (upw_pipeFill(p[1]) == 0U)) {
				setupErrs++;
				close(p[0]);
				close(p[1]);
				continue;
			}
			w.fd = p[1];
			w.events = POLLOUT;
		}

		if (pthread_create(&tid, NULL, upw_pipeWaitThread, &w) != 0) {
			setupErrs++;
			close(p[0]);
			close(p[1]);
			break;
		}
		(void)upw_waitFlag(&w.armed, 1000);
		upw_sleepUs(UPW_PIPE_ARM_US);

		changeNs = upw_nowNs();
		switch (kind) {
			case upw_pipePollout:
				if (read(p[0], buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
					setupErrs++;
				}
				break;
			case upw_pipeHup:
				close(p[1]);
				p[1] = -1;
				break;
			default:
				close(p[0]);
				p[0] = -1;
				break;
		}
		pthread_join(tid, NULL);

		if (w.rc == 0) {
			/* 3 s each: two are enough to know */
			timeouts++;
		}
		else if (w.rc != 1) {
			badEvents++;
		}
		else {
			lat[n++] = upw_latUs(w.wokeNs, changeNs);

			want = (kind == upw_pipePollout) ? POLLOUT : ((kind == upw_pipeHup) ? POLLHUP : (POLLHUP | POLLERR));
			if ((w.revents & want) == 0) {
				badEvents++;
			}
		}

		if (kind == upw_pipeHup) {
			/* the last writer is gone: end of file, not EAGAIN and not a block */
			if (read(p[0], buf, 1) != 0) {
				badAfter++;
			}
		}
		else if (kind == upw_pipeHupWriter) {
			/* the last reader is gone: EPIPE (SIGPIPE is ignored for the group) */
			r = write(p[1], buf, 1);
			if ((r >= 0) || (errno != EPIPE)) {
				badAfter++;
			}
		}

		if (p[0] >= 0) {
			close(p[0]);
		}
		if (p[1] >= 0) {
			close(p[1]);
		}

		if (timeouts >= 2U) {
			break;
		}
	}

	for (i = 0; i < n; ++i) {
		ge10ms += (lat[i] >= UPW_LATE_US) ? 1U : 0U;
	}
	qsort(lat, n, sizeof(lat[0]), upw_cmpU32);
	p50 = upw_pct(lat, n, 50);
	p99 = upw_pct(lat, n, 99);
	maxUs = (n > 0U) ? lat[n - 1U] : 0U;

	printf("UPW case=%s reps=%u woken=%u p50_us=%u p99_us=%u max_us=%u ge10ms=%u timeouts=%u bad_events=%u bad_after=%u setup_errs=%u\n",
		name, UPW_PIPE_REPS, n, p50, p99, maxUs, ge10ms, timeouts, badEvents, badAfter, setupErrs);
	ok = (n == UPW_PIPE_REPS) && (timeouts == 0U) && (badEvents == 0U) && (badAfter == 0U) && (setupErrs == 0U) &&
		(maxUs <= UPW_FAIL_US) && (p99 < UPW_PIPE_P99_US);
	printf("UPW case=%s verdict=%s\n", name, (ok != 0) ? "PASS" : "FAIL");
	fflush(stdout);

	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, setupErrs, "could not set the pipe up (pipe/fcntl/fill/read)");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, timeouts, "the change never woke poll() (3 s)");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, badEvents, "poll() failed or reported the wrong events");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, badAfter, "the end did not behave as reported (EOF / EPIPE)");
	TEST_ASSERT_TRUE_MESSAGE(maxUs <= UPW_FAIL_US, "a pipe wake took more than 1 s");
	TEST_ASSERT_TRUE_MESSAGE(p99 < UPW_PIPE_P99_US, "pipe p99 wake >= 5 ms: pipe readiness does not wake poll()");
}


/*
 * A pty is posixsrv's other poll()able object: bash, psh and a terminal emulator
 * poll its ends. UPW_PIPE_REPS times, a thread sleeps in poll(POLLIN) on one end
 * and the main thread writes one byte to the other (the slave is raw, so a byte
 * is a whole read and nothing is echoed).
 */
static void upw_ptyTransition(const char *name, int pollSlave)
{
	uint32_t lat[UPW_PIPE_REPS];
	unsigned rep, n = 0, timeouts = 0, badEvents = 0, badAfter = 0, setupErrs = 0, ge10ms = 0, p50, p99, maxUs;
	upw_pipeWaiter_t w;
	struct termios tio;
	pthread_t tid;
	int64_t changeNs;
	const char *pts;
	int master, slave, ok;
	char ch;

	master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
	if (master < 0) {
		TEST_IGNORE_MESSAGE("no /dev/ptmx");
	}
	pts = ((grantpt(master) == 0) && (unlockpt(master) == 0)) ? ptsname(master) : NULL;
	slave = (pts != NULL) ? open(pts, O_RDWR | O_NOCTTY) : -1;
	if ((slave < 0) || (tcgetattr(slave, &tio) != 0)) {
		close(master);
		if (slave >= 0) {
			close(slave);
		}
		TEST_FAIL_MESSAGE("could not open the pty slave");
	}
	cfmakeraw(&tio);
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;
	TEST_ASSERT_EQUAL_INT(0, tcsetattr(slave, TCSANOW, &tio));

	for (rep = 0; (rep < UPW_PIPE_REPS) && (timeouts < 2U); ++rep) {
		memset(&w, 0, sizeof(w));
		w.fd = (pollSlave != 0) ? slave : master;
		w.events = POLLIN;
		if (pthread_create(&tid, NULL, upw_pipeWaitThread, &w) != 0) {
			setupErrs++;
			break;
		}
		(void)upw_waitFlag(&w.armed, 1000);
		upw_sleepUs(UPW_PIPE_ARM_US);

		changeNs = upw_nowNs();
		if (write((pollSlave != 0) ? master : slave, "x", 1) != 1) {
			setupErrs++;
		}
		pthread_join(tid, NULL);

		if (w.rc == 0) {
			timeouts++;
		}
		else if ((w.rc != 1) || ((w.revents & POLLIN) == 0)) {
			badEvents++;
		}
		else {
			lat[n++] = upw_latUs(w.wokeNs, changeNs);
			if ((read(w.fd, &ch, 1) != 1) || (ch != 'x')) {
				badAfter++;
			}
		}
	}
	close(slave);
	close(master);

	for (rep = 0; rep < n; ++rep) {
		ge10ms += (lat[rep] >= UPW_LATE_US) ? 1U : 0U;
	}
	qsort(lat, n, sizeof(lat[0]), upw_cmpU32);
	p50 = upw_pct(lat, n, 50);
	p99 = upw_pct(lat, n, 99);
	maxUs = (n > 0U) ? lat[n - 1U] : 0U;

	printf("UPW case=%s reps=%u woken=%u p50_us=%u p99_us=%u max_us=%u ge10ms=%u timeouts=%u bad_events=%u bad_after=%u setup_errs=%u\n",
		name, UPW_PIPE_REPS, n, p50, p99, maxUs, ge10ms, timeouts, badEvents, badAfter, setupErrs);
	ok = (n == UPW_PIPE_REPS) && (timeouts == 0U) && (badEvents == 0U) && (badAfter == 0U) && (setupErrs == 0U) &&
		(maxUs <= UPW_FAIL_US) && (p99 < UPW_PIPE_P99_US);
	printf("UPW case=%s verdict=%s\n", name, (ok != 0) ? "PASS" : "FAIL");
	fflush(stdout);

	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, setupErrs, "could not write to the pty");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, timeouts, "the byte never woke poll() (3 s)");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, badEvents, "poll() failed or did not report POLLIN");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, badAfter, "the byte could not be read back");
	TEST_ASSERT_TRUE_MESSAGE(maxUs <= UPW_FAIL_US, "a pty wake took more than 1 s");
	TEST_ASSERT_TRUE_MESSAGE(p99 < UPW_PIPE_P99_US, "pty p99 wake >= 5 ms: pty readiness does not wake poll()");
}


/*
 * O_NONBLOCK must mean "do not wait for the pipe's state", never "fail if
 * someone else is using the pipe right now". A writer and a reader, both
 * non-blocking, move single bytes through a pipe kept nearly empty, while a
 * third thread keeps poll()ing both ends (each poll() is a status query to the
 * pipe's server). An EAGAIN is spurious when the counters prove it wrong: a
 * write with at most UPW_NB_INFLIGHT bytes queued (far below any pipe's
 * capacity), or a read with bytes known to be queued. GLib's wake-up pipe is
 * written exactly like this and drops the byte on EAGAIN: a lost wake-up.
 */
#define UPW_NB_SECS     2
#define UPW_NB_INFLIGHT 256U


typedef struct {
	int rd, wr;
	volatile int stop;
	unsigned written; /* atomic: bytes the writer has had accepted */
	unsigned readn;   /* atomic: bytes the reader has taken */
	unsigned wrEagain, rdEagain, errs, bad, polls;
} upw_nb_t;


static void *upw_nbWriter(void *arg)
{
	upw_nb_t *nb = arg;
	unsigned w, inflight;
	unsigned char b;
	ssize_t r;

	while (__atomic_load_n(&nb->stop, __ATOMIC_ACQUIRE) == 0) {
		w = __atomic_load_n(&nb->written, __ATOMIC_RELAXED);
		/* the reader only ever lowers it: an upper bound on what is queued */
		inflight = w - __atomic_load_n(&nb->readn, __ATOMIC_ACQUIRE);
		if (inflight >= UPW_NB_INFLIGHT) {
			upw_sleepUs(100);
			continue;
		}
		b = (unsigned char)w;
		r = write(nb->wr, &b, 1);
		if (r == 1) {
			__atomic_store_n(&nb->written, w + 1U, __ATOMIC_RELEASE);
		}
		else if ((r < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
			/* at most UPW_NB_INFLIGHT bytes were queued: there was room */
			nb->wrEagain++;
		}
		else if ((r >= 0) || (errno != EINTR)) {
			nb->errs++;
			break;
		}
	}
	return NULL;
}


static void *upw_nbReader(void *arg)
{
	upw_nb_t *nb = arg;
	unsigned n, queued;
	unsigned char b;
	ssize_t r;

	for (;;) {
		n = __atomic_load_n(&nb->readn, __ATOMIC_RELAXED);
		/* written only grows: a lower bound on what is queued */
		queued = __atomic_load_n(&nb->written, __ATOMIC_ACQUIRE) - n;
		if (queued == 0U) {
			if (__atomic_load_n(&nb->stop, __ATOMIC_ACQUIRE) != 0) {
				break;
			}
			upw_sleepUs(100);
			continue;
		}
		r = read(nb->rd, &b, 1);
		if (r == 1) {
			if (b != (unsigned char)n) {
				nb->bad++;
			}
			__atomic_store_n(&nb->readn, n + 1U, __ATOMIC_RELEASE);
		}
		else if ((r < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
			/* at least one byte was queued */
			nb->rdEagain++;
		}
		else if ((r >= 0) || (errno != EINTR)) {
			nb->errs++;
			break;
		}
	}
	return NULL;
}


static void *upw_nbPoller(void *arg)
{
	upw_nb_t *nb = arg;
	struct pollfd pfd[2];

	while (__atomic_load_n(&nb->stop, __ATOMIC_ACQUIRE) == 0) {
		pfd[0].fd = nb->rd;
		pfd[0].events = POLLIN;
		pfd[0].revents = 0;
		pfd[1].fd = nb->wr;
		pfd[1].events = POLLOUT;
		pfd[1].revents = 0;
		if (poll(pfd, 2, 0) < 0) {
			nb->errs++;
			break;
		}
		nb->polls++;
	}
	return NULL;
}


TEST_GROUP(pipe_poll_wake);


TEST_SETUP(pipe_poll_wake)
{
	struct sigaction sa;

	/* a failed case closes the read end under a blocked writer: EPIPE, not death */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	(void)sigaction(SIGPIPE, &sa, &upw_oldSigpipe);
}


TEST_TEAR_DOWN(pipe_poll_wake)
{
	(void)sigaction(SIGPIPE, &upw_oldSigpipe, NULL);
}


TEST(pipe_poll_wake, pipe_thread)
{
	static const upw_pipeCase_t c = { .name = "pipe_thread" };

	upw_pipeCheck(&c);
}


TEST(pipe_poll_wake, pipe_process)
{
	static const upw_pipeCase_t c = { .name = "pipe_process", .fork = 1 };

	upw_pipeCheck(&c);
}


/* The GLib shape: a main loop's wake-up pipe next to an IPC socket. */
TEST(pipe_poll_wake, pipe_and_unix)
{
	static const upw_pipeCase_t c = { .name = "pipe_and_unix", .withUnix = 1 };

	upw_pipeCheck(&c);
}


TEST(pipe_poll_wake, pipe_pollout)
{
	upw_pipeTransition("pipe_pollout", upw_pipePollout);
}


TEST(pipe_poll_wake, pipe_hup)
{
	upw_pipeTransition("pipe_hup", upw_pipeHup);
}


TEST(pipe_poll_wake, pipe_hup_writer)
{
	upw_pipeTransition("pipe_hup_writer", upw_pipeHupWriter);
}


TEST(pipe_poll_wake, pty_slave_in)
{
	upw_ptyTransition("pty_slave_in", 1);
}


TEST(pipe_poll_wake, pty_master_in)
{
	upw_ptyTransition("pty_master_in", 0);
}


TEST(pipe_poll_wake, pipe_nonblock_contention)
{
	upw_nb_t nb;
	pthread_t wt, rt, pt;
	int p[2], ok;

	memset(&nb, 0, sizeof(nb));
	TEST_ASSERT_EQUAL_INT(0, pipe(p));
	nb.rd = p[0];
	nb.wr = p[1];
	TEST_ASSERT_EQUAL_INT(0, fcntl(nb.rd, F_SETFL, fcntl(nb.rd, F_GETFL) | O_NONBLOCK));
	TEST_ASSERT_EQUAL_INT(0, fcntl(nb.wr, F_SETFL, fcntl(nb.wr, F_GETFL) | O_NONBLOCK));

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&pt, NULL, upw_nbPoller, &nb));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&rt, NULL, upw_nbReader, &nb));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&wt, NULL, upw_nbWriter, &nb));
	upw_sleepUs(UPW_NB_SECS * 1000000U);
	__atomic_store_n(&nb.stop, 1, __ATOMIC_RELEASE);
	pthread_join(wt, NULL);
	pthread_join(pt, NULL);
	pthread_join(rt, NULL);
	close(p[0]);
	close(p[1]);

	printf("UPW case=pipe_nonblock_contention secs=%d written=%u read=%u polls=%u wr_spurious_eagain=%u "
		   "rd_spurious_eagain=%u bad=%u errs=%u\n",
		UPW_NB_SECS, nb.written, nb.readn, nb.polls, nb.wrEagain, nb.rdEagain, nb.bad, nb.errs);
	ok = (nb.wrEagain == 0U) && (nb.rdEagain == 0U) && (nb.bad == 0U) && (nb.errs == 0U) &&
		(nb.written == nb.readn) && (nb.written > 0U);
	printf("UPW case=pipe_nonblock_contention verdict=%s\n", (ok != 0) ? "PASS" : "FAIL");
	fflush(stdout);

	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, nb.errs, "write()/read()/poll() failed");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, nb.bad, "bytes reordered or corrupted");
	TEST_ASSERT_TRUE_MESSAGE(nb.written > 0U, "nothing was written");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(nb.written, nb.readn, "bytes written != bytes read");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, nb.wrEagain, "a non-blocking write got EAGAIN with room in the pipe");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, nb.rdEagain, "a non-blocking read got EAGAIN with data in the pipe");
}


TEST_GROUP_RUNNER(pipe_poll_wake)
{
	RUN_TEST_CASE(pipe_poll_wake, pipe_thread);
	RUN_TEST_CASE(pipe_poll_wake, pipe_process);
	RUN_TEST_CASE(pipe_poll_wake, pipe_and_unix);
	RUN_TEST_CASE(pipe_poll_wake, pipe_pollout);
	RUN_TEST_CASE(pipe_poll_wake, pipe_hup);
	RUN_TEST_CASE(pipe_poll_wake, pipe_hup_writer);
	RUN_TEST_CASE(pipe_poll_wake, pty_slave_in);
	RUN_TEST_CASE(pipe_poll_wake, pty_master_in);
	RUN_TEST_CASE(pipe_poll_wake, pipe_nonblock_contention);
}


TEST_GROUP_RUNNER(unix_poll_wake)
{
	RUN_TEST_CASE(unix_poll_wake, seqpacket_thread);
	RUN_TEST_CASE(unix_poll_wake, seqpacket_process);
	RUN_TEST_CASE(unix_poll_wake, seqpacket_and_pipe);
	RUN_TEST_CASE(unix_poll_wake, stream_fds);
	RUN_TEST_CASE(unix_poll_wake, dgram_fds);
	RUN_TEST_CASE(unix_poll_wake, seqpacket_fds);
	RUN_TEST_CASE(unix_poll_wake, two_writers);
	RUN_TEST_CASE(unix_poll_wake, burst);
	RUN_TEST_CASE(unix_poll_wake, crowd);
	RUN_TEST_CASE(unix_poll_wake, cpu_load);
	RUN_TEST_CASE(unix_poll_wake, seqpacket_pollout);
}


static void runner(void)
{
	RUN_TEST_GROUP(unix_poll_wake);
	RUN_TEST_GROUP(pipe_poll_wake);
}


int main(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; ++i) {
		if ((strcmp(argv[i], "--count") == 0) && (i + 1 < argc)) {
			upw_common.count = (unsigned)strtoul(argv[++i], NULL, 0);
			if ((upw_common.count < 2U) || (upw_common.count > 1000000U)) {
				fprintf(stderr, "--count must be 2..1000000\n");
				return EXIT_FAILURE;
			}
		}
		else if (strcmp(argv[i], "--strict") == 0) {
			upw_common.strict = 1;
		}
	}

	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
