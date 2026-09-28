/*
 * Phoenix-RTOS
 *
 * test-sys-threadinfo
 *
 * Tests of threadsinfo(): the cpuId field reports the core a thread last ran
 * on. Busy threads on an SMP system must be seen on more than one core, and
 * every reported core must exist.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/threads.h>

#include <unity_fixture.h>


#define BUSY_THREADS  4
#define BUSY_MS       200
#define POLL_US       2000
#define BUSY_PRIORITY 4


static struct {
	char stack[BUSY_THREADS][8192] __attribute__((aligned(8)));
	handle_t tid[BUSY_THREADS];
	volatile int done;
	threadinfo_t *info;
	int ninfo;
} ti_common;


static long elapsed_ms(const struct timespec *from, const struct timespec *to)
{
	return (to->tv_sec - from->tv_sec) * 1000L + (to->tv_nsec - from->tv_nsec) / 1000000L;
}


static void busy_thread(void *arg)
{
	struct timespec start, now;

	(void)arg;

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while (elapsed_ms(&start, &now) < BUSY_MS);

	__atomic_fetch_add(&ti_common.done, 1, __ATOMIC_RELEASE);
	endthread();
}


static int is_busy_thread(const threadinfo_t *info)
{
	int i;

	if (info->pid != getpid()) {
		return 0;
	}

	for (i = 0; i < BUSY_THREADS; i++) {
		if (info->tid == (unsigned int)ti_common.tid[i]) {
			return 1;
		}
	}

	return 0;
}


/*
 * Take one snapshot of all threads into ti_common.info, growing it as needed.
 * The buffer is zeroed first: the kernel writes only the fields it fills, so a
 * kernel that never sets cpuId must read as 0, not as stale buffer contents.
 */
static int snapshot(void)
{
	threadinfo_t *p;
	int cnt;

	for (;;) {
		memset(ti_common.info, 0, ti_common.ninfo * sizeof(*ti_common.info));
		cnt = threadsinfo(ti_common.ninfo, PH_THREADINFO_TID | PH_THREADINFO_STATE, ti_common.info);
		if (cnt < ti_common.ninfo) {
			return cnt;
		}

		p = realloc(ti_common.info, 2 * cnt * sizeof(*p));
		if (p == NULL) {
			return -1;
		}
		ti_common.info = p;
		ti_common.ninfo = 2 * cnt;
	}
}


TEST_GROUP(threadinfo_smp);


TEST_SETUP(threadinfo_smp)
{
	ti_common.done = 0;
	ti_common.ninfo = threadcount() * 2 + 16;
	ti_common.info = malloc(ti_common.ninfo * sizeof(*ti_common.info));
	TEST_ASSERT_NOT_NULL(ti_common.info);
}


TEST_TEAR_DOWN(threadinfo_smp)
{
	free(ti_common.info);
	ti_common.info = NULL;
}


TEST(threadinfo_smp, busy_threads_report_their_cpu)
{
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	unsigned long long seen = 0;
	int i, cnt, distinct = 0, samples = 0, snapErr = 0, badCpuId = -1;
	char msg[96];

	TEST_ASSERT_GREATER_THAN_MESSAGE(0, ncpu, "sysconf(_SC_NPROCESSORS_ONLN)");
	if (ncpu < 2) {
		TEST_IGNORE_MESSAGE("single CPU system");
	}
	TEST_ASSERT_LESS_OR_EQUAL(64, ncpu);

	for (i = 0; i < BUSY_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, beginthreadex(busy_thread, BUSY_PRIORITY, ti_common.stack[i], sizeof(ti_common.stack[i]), NULL, &ti_common.tid[i]));
	}

	/* No assertions until the threads are joined: bailing out would leave them running */
	while (__atomic_load_n(&ti_common.done, __ATOMIC_ACQUIRE) < BUSY_THREADS) {
		cnt = snapshot();
		if (cnt < 0) {
			snapErr = cnt;
			break;
		}
		samples++;

		for (i = 0; i < cnt; i++) {
			if ((ti_common.info[i].cpuId < 0) || (ti_common.info[i].cpuId >= ncpu)) {
				badCpuId = ti_common.info[i].cpuId;
			}
			else if (is_busy_thread(&ti_common.info[i]) != 0) {
				seen |= 1ULL << ti_common.info[i].cpuId;
			}
		}

		usleep(POLL_US);
	}

	for (i = 0; i < BUSY_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(ti_common.tid[i], threadJoin(ti_common.tid[i], 0));
	}

	TEST_ASSERT_EQUAL_INT_MESSAGE(0, snapErr, "threadsinfo() failed");
	TEST_ASSERT_GREATER_THAN_MESSAGE(0, samples, "no threadsinfo() snapshot taken");

	(void)snprintf(msg, sizeof(msg), "cpuId %d reported, system has %ld CPUs", badCpuId, ncpu);
	TEST_ASSERT_EQUAL_INT_MESSAGE(-1, badCpuId, msg);

	for (i = 0; i < ncpu; i++) {
		if ((seen & (1ULL << i)) != 0) {
			distinct++;
		}
	}

	(void)snprintf(msg, sizeof(msg), "distinct cpuIds of %d busy threads over %d snapshots, %ld CPUs", BUSY_THREADS, samples, ncpu);
	TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1, distinct, msg);
}


TEST_GROUP_RUNNER(threadinfo_smp)
{
	RUN_TEST_CASE(threadinfo_smp, busy_threads_report_their_cpu);
}


void runner(void)
{
	RUN_TEST_GROUP(threadinfo_smp);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
