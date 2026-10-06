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
 *                          as pipe_* (posixsrv does not notify: 0-20 ms quantum)
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
