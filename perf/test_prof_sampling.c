/*
 * Phoenix-RTOS
 *
 * Profiling events of the kernel trace
 *
 * HEADER:
 *    - sys/perf.h
 *
 * TESTED:
 *    - perf_start(perf_mode_trace, PERF_TRACE_FLAG_SAMPLE, perf_trace_cfg_t), perf_read(), perf_stop(),
 *      perf_finish(); the thread_sample, thread_wait, thread_wakeup and msg_* events
 *
 * What it checks:
 *   prof_sampling.busy_loop_attributed - a thread spinning in a known function is sampled, and its
 *     samples are in user mode with the PC inside that function.
 *   prof_sampling.blocked_send_attributed - a thread blocked in msgSend() on a server that holds the
 *     request is seen: msg_send names the port, msg_recv the server thread that took the request,
 *     thread_wait the client with the port as its syscall argument and the caller of msgSend() as
 *     its user lr, and thread_wakeup the server as the waker. The trace records waits of 1 ms or
 *     more when they end (waitMinUs, as prof does), so the wait carries its length: at least the
 *     time the server held the request. The trace is read while it records, as prof does.
 *   prof_sampling.idle_volume - prof's default recording of an idle system for 2 s stays under
 *     PROF_IDLE_BUDGET and loses no event (trace_stats); build 39 recorded 78 MB in 5 s and lost
 *     34954 events before event classes (perf_trace_cfg_t.events) existed.
 *   These tests FAIL on a kernel without "perf: trace where threads run, what they wait for and who
 *   wakes them" (phoenix-rtos-kernel, 2026-10-06): it records none of these events.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/perf.h>
#include <sys/threads.h>
#include <phoenix/syscalls.h>

#include <unity_fixture.h>


#define PROF_HOLD_MS      300  /* the server holds the request this long */
#define PROF_FN_SPAN      256U /* bytes of a function its PCs are expected in */
#define PROF_MIN_SAMPLES  50U  /* of the busy thread, sampled at 1 kHz for over PROF_HOLD_MS */
#define PROF_NCHANS_MAX   64
#define PROF_IDLE_MS      2000 /* the idle recording */
#define PROF_IDLE_BUDGET  (2U << 20) /* bytes it may take with prof's defaults (build 39: 78 MB in 5 s) */

/* event ids, mirror phoenix-rtos-kernel/perf/tsdl/metadata */
#define EV_SAMPLE  0x40
#define EV_WAIT    0x41
#define EV_WAKEUP  0x42
#define EV_SEND    0x43
#define EV_RECV    0x44
#define EV_RESPOND 0x45
#define EV_STATS   0x46

/* thread_wait payload offsets */
#define WAIT_FLAGS   2U
#define WAIT_BLOCKED 11U
#define WAIT_SYSCALL 15U
#define WAIT_ARGS    17U
#define WAIT_NK      49U
#define WAIT_KFRAMES 50U

/* syscall numbers, in the kernel's order */
#define PROF_SYSCALL_ID(name) prof_sc_##name,
enum { SYSCALLS(PROF_SYSCALL_ID) prof_sc_count };


static struct {
	uint32_t port;
	volatile int busyStop;
	volatile int drainStop;
	volatile unsigned long busyCount;
	volatile int busyTid, clientTid, serverTid;
	int sendErr;

	uint8_t *chan[PROF_NCHANS_MAX];
	size_t chanLen[PROF_NCHANS_MAX];
	int nchans;
} prof_common;


/* The parsed events the tests look at */
typedef struct {
	unsigned int busySamples, busyInLoop;
	int sendFound, recvFound, waitFound, wakeFound;
	uint32_t mid, sendTs, wakeTs, waitBlocked;
	uint8_t waitFlags;
	uint16_t waitSyscall;
	uint64_t waitArg0, waitLr;
	int recvTid, waker, wakeCause;
} prof_result_t;


static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}


static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}


static uint64_t rd64(const uint8_t *p)
{
	return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}


__attribute__((noinline)) static void prof_busyLoop(void)
{
	while (prof_common.busyStop == 0) {
		prof_common.busyCount++;
	}
}


static void *prof_busyThread(void *arg)
{
	(void)arg;
	prof_common.busyTid = gettid();
	prof_busyLoop();
	return NULL;
}


static void *prof_serverThread(void *arg)
{
	msg_t msg;
	msg_rid_t rid;

	(void)arg;
	prof_common.serverTid = gettid();
	if (msgRecv(prof_common.port, &msg, &rid) == 0) {
		usleep(PROF_HOLD_MS * 1000);
		msg.o.err = 0;
		(void)msgRespond(prof_common.port, &msg, rid);
	}

	return NULL;
}


__attribute__((noinline)) static void *prof_clientThread(void *arg)
{
	msg_t msg;

	(void)arg;
	prof_common.clientTid = gettid();
	memset(&msg, 0, sizeof(msg));
	msg.type = mtGetAttr;
	prof_common.sendErr = msgSend(prof_common.port, &msg);

	return NULL;
}


/* Size of the user part of a sample/wait at offset o, 0 if truncated */
static size_t prof_urecEnd(const uint8_t *p, size_t avail, size_t o)
{
	size_t ns;

	if (avail < o + 33U) {
		return 0;
	}
	o += 33U + (size_t)p[o + 32U] * 8U;
	if (avail < o + 2U) {
		return 0;
	}
	ns = rd16(p + o);
	o += 2U + ns * 8U;

	return (avail < o) ? 0U : o;
}


static size_t prof_evSize(uint8_t id, const uint8_t *p, size_t avail)
{
	static const uint8_t fixed[] = {
		[0x20] = 1, [0x21] = 1, [0x22] = 2, [0x23] = 2, [0x24] = 2, [0x25] = 2, [0x26] = 133, [0x27] = 4,
		[0x28] = 3, [0x29] = 3, [0x2a] = 1, [0x2b] = 1, [0x2c] = 20, [0x2d] = 6, [0x2e] = 6, [0x2f] = 6,
		[0x30] = 6, [0x31] = 3, [0x32] = 2, [0x33] = 133, [EV_WAKEUP] = 5, [EV_SEND] = 14, [EV_RECV] = 12,
		[EV_RESPOND] = 10, [EV_STATS] = 8
	};

	if (id == EV_SAMPLE) {
		return (avail < 12U) ? 0U : prof_urecEnd(p, avail, 12U + (size_t)p[11] * 8U);
	}
	if (id == EV_WAIT) {
		return (avail < WAIT_KFRAMES) ? 0U : prof_urecEnd(p, avail, WAIT_KFRAMES + (size_t)p[WAIT_NK] * 8U);
	}
	if ((id >= sizeof(fixed)) || (fixed[id] == 0U) || (avail < fixed[id])) {
		return 0;
	}

	return fixed[id];
}


/* One pass over every stream; msg events first, as the waits are matched to the send */
static void prof_parse(prof_result_t *r, int pass)
{
	const uintptr_t loop = (uintptr_t)prof_busyLoop;
	size_t o, sz;
	const uint8_t *p;
	uint32_t ts;
	uint8_t id;
	int c;

	for (c = 0; c < prof_common.nchans; c++) {
		for (o = 0; o + 5U <= prof_common.chanLen[c]; o += 5U + sz) {
			ts = rd32(prof_common.chan[c] + o);
			id = prof_common.chan[c][o + 4U];
			p = prof_common.chan[c] + o + 5U;
			sz = prof_evSize(id, p, prof_common.chanLen[c] - o - 5U);
			if (sz == 0U) {
				break;
			}

			if (pass == 0) {
				if ((id == EV_SEND) && (rd16(p) == (uint16_t)prof_common.clientTid) && (rd32(p + 2) == prof_common.port)) {
					r->sendFound = 1;
					r->mid = rd32(p + 10);
					r->sendTs = ts;
				}
				else if ((id == EV_SAMPLE) && (rd16(p) == (uint16_t)prof_common.busyTid) && (p[2] == 0U)) {
					/* user part after the kernel frames: pc first */
					uint64_t pc = rd64(p + 12U + (size_t)p[11] * 8U);
					r->busySamples++;
					if ((pc >= loop) && (pc < loop + PROF_FN_SPAN)) {
						r->busyInLoop++;
					}
				}
			}
			else if (r->sendFound != 0) {
				if ((id == EV_RECV) && (rd32(p + 6) == r->mid)) {
					r->recvFound = 1;
					r->recvTid = rd16(p);
				}
				else if ((id == EV_WAIT) && (rd16(p) == (uint16_t)prof_common.clientTid) && (ts >= r->sendTs) && (r->waitFound == 0)) {
					/* deferred (waitMinUs): written when the wait ended, with its length */
					size_t u = WAIT_KFRAMES + (size_t)p[WAIT_NK] * 8U;
					r->waitFound = 1;
					r->waitFlags = p[WAIT_FLAGS];
					r->waitBlocked = rd32(p + WAIT_BLOCKED);
					r->waitSyscall = rd16(p + WAIT_SYSCALL);
					r->waitArg0 = rd64(p + WAIT_ARGS);
					r->waitLr = rd64(p + u + 8U);
				}
				else if ((id == EV_WAKEUP) && (rd16(p) == (uint16_t)prof_common.clientTid) && (ts >= r->sendTs) && (ts >= r->wakeTs)) {
					/* the latest one, whichever CPU stream it is in: the response */
					r->wakeFound = 1;
					r->wakeTs = ts;
					r->waker = rd16(p + 2);
					r->wakeCause = p[4];
				}
			}
		}
	}
}


/*
 * One pass over the channels, at most 64 reads (4 MB) of each so that it ends while tracing.
 * Returns the bytes read, or a negative error.
 */
static int prof_drain(void)
{
	uint8_t *buf = malloc(1U << 16), *n;
	int c, got = 0, k, total = 0;

	if (buf == NULL) {
		return -ENOMEM;
	}
	/* the kernel fills it holding a spinlock: no page fault there */
	memset(buf, 0, 1U << 16);

	for (c = 0; c < prof_common.nchans; c++) {
		for (k = 0; (k < 64) && ((got = perf_read(perf_mode_trace, buf, 1U << 16, c)) > 0); k++) {
			n = realloc(prof_common.chan[c], prof_common.chanLen[c] + (size_t)got);
			if (n == NULL) {
				free(buf);
				return -ENOMEM;
			}
			memcpy(n + prof_common.chanLen[c], buf, (size_t)got);
			prof_common.chan[c] = n;
			prof_common.chanLen[c] += (size_t)got;
			total += got;
		}
		if (got < 0) {
			free(buf);
			return got;
		}
	}

	free(buf);

	return total;
}


/* Reads the trace while it is recorded: a channel that fills up loses events */
static void *prof_drainThread(void *arg)
{
	(void)arg;
	while (prof_common.drainStop == 0) {
		if (prof_drain() < 0) {
			break;
		}
		usleep(20 * 1000);
	}

	return NULL;
}


/* Records the busy thread and the blocked client once, for both tests */
static int prof_record(prof_result_t *r)
{
	static int recorded = 0;
	static prof_result_t result;
	/* as prof records by default (a profile; waits of 1 ms or longer, written when they end), sampled faster */
	perf_trace_cfg_t cfg = { .samplePeriodUs = 1000, .depth = 8, .sampleStack = 0, .waitStack = 0, .waitMinUs = 1000,
		.events = PERF_TRACE_EV_PROFILE };
	pthread_t busy, server, client, drainer;
	int ret;

	if (recorded != 0) {
		*r = result;
		return 0;
	}

	memset(&result, 0, sizeof(result));
	TEST_ASSERT_EQUAL_INT(0, portCreate(&prof_common.port));

	ret = perf_start(perf_mode_trace, PERF_TRACE_FLAG_SAMPLE, &cfg, sizeof(cfg));
	if (ret < 0) {
		portDestroy(prof_common.port);
		return ret;
	}
	prof_common.nchans = (ret < PROF_NCHANS_MAX) ? ret : PROF_NCHANS_MAX;

	prof_common.busyStop = 0;
	prof_common.drainStop = 0;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&drainer, NULL, prof_drainThread, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&server, NULL, prof_serverThread, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&busy, NULL, prof_busyThread, NULL));
	usleep(20 * 1000);
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&client, NULL, prof_clientThread, NULL));

	/* the client returns once the server answers: by then the busy thread has spun PROF_HOLD_MS */
	pthread_join(client, NULL);
	usleep(20 * 1000);
	prof_common.busyStop = 1;
	pthread_join(busy, NULL);
	pthread_join(server, NULL);

	ret = perf_stop(perf_mode_trace);
	prof_common.drainStop = 1;
	pthread_join(drainer, NULL);
	/* stopped: the channels no longer grow */
	while ((ret >= 0) && ((ret = prof_drain()) > 0)) {
	}
	(void)perf_finish(perf_mode_trace);
	portDestroy(prof_common.port);

	if (ret < 0) {
		return ret;
	}

	prof_parse(&result, 0);
	prof_parse(&result, 1);
	recorded = 1;
	*r = result;

	return 0;
}


TEST_GROUP(prof_sampling);


TEST_SETUP(prof_sampling)
{
}


TEST_TEAR_DOWN(prof_sampling)
{
}


TEST(prof_sampling, busy_loop_attributed)
{
	prof_result_t r;

	TEST_ASSERT_EQUAL_INT_MESSAGE(0, prof_record(&r), "perf_start/perf_read failed (another trace running?)");

	TEST_ASSERT_MESSAGE(r.busySamples >= PROF_MIN_SAMPLES, "too few thread_sample events of the busy thread (no sampling?)");
	/* nearly every user-mode sample of a thread that only spins lands in its loop */
	TEST_ASSERT_MESSAGE(r.busyInLoop * 10U >= r.busySamples * 9U, "busy thread's sampled PCs are not in its loop");
}


TEST(prof_sampling, blocked_send_attributed)
{
	prof_result_t r;

	TEST_ASSERT_EQUAL_INT_MESSAGE(0, prof_record(&r), "perf_start/perf_read failed (another trace running?)");
	TEST_ASSERT_EQUAL_INT(0, prof_common.sendErr);

	TEST_ASSERT_MESSAGE(r.sendFound != 0, "no msg_send of the client to the port");
	TEST_ASSERT_MESSAGE(r.recvFound != 0, "no msg_recv of that message");
	TEST_ASSERT_EQUAL_INT_MESSAGE(prof_common.serverTid, r.recvTid, "msg_recv names another thread than the server");

	TEST_ASSERT_MESSAGE(r.waitFound != 0, "no thread_wait of the client after its msg_send");
	TEST_ASSERT_EQUAL_UINT64_MESSAGE(prof_common.port, r.waitArg0, "thread_wait: msgSend's port is not the first argument");
	TEST_ASSERT_EQUAL_INT_MESSAGE(prof_sc_msgSend, r.waitSyscall, "thread_wait: syscall is not msgSend (the SVC before its pc)");
	/* msgSend() is a leaf stub, so lr is its return address in the client thread */
	TEST_ASSERT_MESSAGE((r.waitLr > (uintptr_t)prof_clientThread) && (r.waitLr < (uintptr_t)prof_clientThread + 4U * PROF_FN_SPAN),
		"thread_wait: user lr is not in the caller of msgSend()");

	TEST_ASSERT_MESSAGE(r.wakeFound != 0, "no thread_wakeup of the client");
	TEST_ASSERT_EQUAL_INT_MESSAGE(prof_common.serverTid, r.waker, "the client was not woken by the server");
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.wakeCause, "thread_wakeup cause is not an explicit wakeup");
	TEST_ASSERT_MESSAGE((r.waitFlags & 2U) != 0U, "thread_wait of the client is not a deferred one (waitMinUs)");
	TEST_ASSERT_MESSAGE(r.waitBlocked >= (uint32_t)(PROF_HOLD_MS - 50) * 1000U, "blocked time shorter than the server held the request");
}


/*
 * prof's default recording of an idle system stays small and loses nothing: it records a profile
 * (no event of every switch, syscall, lock and interrupt) and only waits that last.
 */
TEST(prof_sampling, idle_volume)
{
	perf_trace_cfg_t cfg = { .samplePeriodUs = 2000, .depth = 16, .sampleStack = 512, .waitStack = 512, .waitMinUs = 1000,
		.events = PERF_TRACE_EV_PROFILE };
	uint64_t bytes[256] = { 0 }, total = 0;
	uint32_t discarded = 0, dropped = 0;
	pthread_t drainer;
	int ret, c, stats = 0;
	unsigned int i;
	size_t o, sz;
	char line[64];

	for (c = 0; c < PROF_NCHANS_MAX; c++) {
		free(prof_common.chan[c]);
		prof_common.chan[c] = NULL;
		prof_common.chanLen[c] = 0;
	}

	ret = perf_start(perf_mode_trace, PERF_TRACE_FLAG_SAMPLE, &cfg, sizeof(cfg));
	TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, ret, "perf_start failed (another trace running?)");
	prof_common.nchans = (ret < PROF_NCHANS_MAX) ? ret : PROF_NCHANS_MAX;

	prof_common.drainStop = 0;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&drainer, NULL, prof_drainThread, NULL));
	usleep(PROF_IDLE_MS * 1000);

	ret = perf_stop(perf_mode_trace);
	prof_common.drainStop = 1;
	pthread_join(drainer, NULL);
	while ((ret >= 0) && ((ret = prof_drain()) > 0)) {
	}
	(void)perf_finish(perf_mode_trace);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, ret);

	for (c = 0; c < prof_common.nchans; c++) {
		for (o = 0; o + 5U <= prof_common.chanLen[c]; o += 5U + sz) {
			const uint8_t *p = prof_common.chan[c] + o + 5U;
			uint8_t id = prof_common.chan[c][o + 4U];

			sz = prof_evSize(id, p, prof_common.chanLen[c] - o - 5U);
			if (sz == 0U) {
				break;
			}
			bytes[id] += 5U + sz;
			if (id == EV_STATS) {
				stats = 1;
				discarded += rd32(p);
				dropped += rd32(p + 4);
			}
		}
		total += prof_common.chanLen[c];
	}

	if ((total > PROF_IDLE_BUDGET) || (discarded != 0U)) {
		/* what filled it, for the log */
		for (i = 0; i < 256U; i++) {
			if (bytes[i] != 0U) {
				snprintf(line, sizeof(line), "idle_volume: event 0x%02x %llu bytes", i, (unsigned long long)bytes[i]);
				TEST_MESSAGE(line);
			}
		}
	}

	TEST_ASSERT_MESSAGE(stats != 0, "no trace_stats event at the end of the trace");
	TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, discarded, "the kernel lost events (a full channel)");
	TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, dropped, "the kernel lost waits (no free wait slot)");
	TEST_ASSERT_MESSAGE(total <= PROF_IDLE_BUDGET, "an idle 2 s recording exceeds its byte budget");
}


TEST_GROUP_RUNNER(prof_sampling)
{
	RUN_TEST_CASE(prof_sampling, busy_loop_attributed);
	RUN_TEST_CASE(prof_sampling, blocked_send_attributed);
	RUN_TEST_CASE(prof_sampling, idle_volume);
}


static void runner(void)
{
	RUN_TEST_GROUP(prof_sampling);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
