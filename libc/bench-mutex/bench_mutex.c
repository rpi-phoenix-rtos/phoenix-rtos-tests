/*
 * Phoenix-RTOS
 *
 * bench-mutex: what a lock costs
 *
 * Prints, in nanoseconds:
 *   - an uncontended pthread_mutex_lock()+unlock() pair: normal and recursive
 *   - (Phoenix) the same pair on a raw kernel mutex, mutexLock()+mutexUnlock()
 *   - a malloc(64)+free() pair while a second thread does the same
 *   - one locked increment of a shared counter by 2 threads at once
 *   - a condition-variable round trip between 2 threads
 *
 * Before libphoenix's user-space mutexes every pthread pair was a kernel mutex
 * pair (~4.5 us on the Pi 4), and so was every multithreaded malloc()+free().
 * The source builds against either library, so one binary per library shows the
 * change. Not a unity test.
 *
 *   bench-mutex [iterations]    (default 200000)
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef __phoenix__
#include <sys/threads.h>
#endif


static struct {
	pthread_mutex_t m;
	pthread_cond_t c;
	pthread_barrier_t start;
	volatile unsigned long counter;
	volatile int turn;
	unsigned long n;
} bench_common;


/* Stores through this keep the compiler from pairing up and removing the calls */
static void *volatile bench_sink;


static uint64_t bench_nowNs(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);

	return ((uint64_t)ts.tv_sec * 1000000000u) + (uint64_t)ts.tv_nsec;
}


/* Tenths of a nanosecond per operation (no floating point: IO_NO_FLOAT targets) */
static uint64_t bench_per(uint64_t ns, unsigned long n)
{
	return (ns * 10u) / n;
}


static void bench_print(const char *what, uint64_t tenths)
{
	printf("bench-mutex: %-44s %8llu.%llu ns\n", what, (unsigned long long)(tenths / 10u), (unsigned long long)(tenths % 10u));
}


static uint64_t bench_pthreadPairs(pthread_mutex_t *m, unsigned long n)
{
	unsigned long i;
	uint64_t t0;

	for (i = 0; i < 1000u; i++) {
		pthread_mutex_lock(m);
		pthread_mutex_unlock(m);
	}

	t0 = bench_nowNs();
	for (i = 0; i < n; i++) {
		pthread_mutex_lock(m);
		pthread_mutex_unlock(m);
	}

	return bench_per(bench_nowNs() - t0, n);
}


#ifdef __phoenix__
static uint64_t bench_kernelPairs(unsigned long n)
{
	unsigned long i;
	uint64_t t0;
	handle_t h;

	if (mutexCreate(&h) < 0) {
		return 0;
	}

	t0 = bench_nowNs();
	for (i = 0; i < n; i++) {
		mutexLock(h);
		mutexUnlock(h);
	}
	t0 = bench_per(bench_nowNs() - t0, n);

	(void)resourceDestroy(h);

	return t0;
}
#endif


static void *bench_mallocThread(void *arg)
{
	unsigned long i;
	uint64_t t0;

	pthread_barrier_wait(&bench_common.start);
	t0 = bench_nowNs();
	for (i = 0; i < bench_common.n; i++) {
		bench_sink = malloc(64);
		free(bench_sink);
	}
	*(uint64_t *)arg = bench_nowNs() - t0;

	return NULL;
}


static void *bench_counterThread(void *arg)
{
	unsigned long i;
	uint64_t t0;

	pthread_barrier_wait(&bench_common.start);
	t0 = bench_nowNs();
	for (i = 0; i < bench_common.n; i++) {
		pthread_mutex_lock(&bench_common.m);
		bench_common.counter++;
		pthread_mutex_unlock(&bench_common.m);
	}
	*(uint64_t *)arg = bench_nowNs() - t0;

	return NULL;
}


/* Runs fn in 2 threads started together; returns the slower one's time */
static uint64_t bench_two(void *(*fn)(void *))
{
	pthread_t th[2];
	uint64_t t[2] = { 0, 0 };
	int i;

	pthread_barrier_init(&bench_common.start, NULL, 2);
	for (i = 0; i < 2; i++) {
		if (pthread_create(&th[i], NULL, fn, &t[i]) != 0) {
			fprintf(stderr, "bench-mutex: pthread_create() failed\n");
			exit(EXIT_FAILURE);
		}
	}
	for (i = 0; i < 2; i++) {
		pthread_join(th[i], NULL);
	}
	pthread_barrier_destroy(&bench_common.start);

	return (t[0] > t[1]) ? t[0] : t[1];
}


static void *bench_ponger(void *arg)
{
	unsigned long i;
	(void)arg;

	pthread_mutex_lock(&bench_common.m);
	for (i = 0; i < bench_common.n; i++) {
		while (bench_common.turn != 1) {
			pthread_cond_wait(&bench_common.c, &bench_common.m);
		}
		bench_common.turn = 0;
		pthread_cond_signal(&bench_common.c);
	}
	pthread_mutex_unlock(&bench_common.m);

	return NULL;
}


static uint64_t bench_pingPong(unsigned long n)
{
	pthread_t th;
	unsigned long i;
	uint64_t t0;

	bench_common.n = n;
	bench_common.turn = 0;
	if (pthread_create(&th, NULL, bench_ponger, NULL) != 0) {
		return 0;
	}

	t0 = bench_nowNs();
	pthread_mutex_lock(&bench_common.m);
	for (i = 0; i < n; i++) {
		bench_common.turn = 1;
		pthread_cond_signal(&bench_common.c);
		while (bench_common.turn != 0) {
			pthread_cond_wait(&bench_common.c, &bench_common.m);
		}
	}
	pthread_mutex_unlock(&bench_common.m);
	t0 = bench_nowNs() - t0;

	pthread_join(th, NULL);

	return bench_per(t0, n);
}


int main(int argc, char *argv[])
{
	pthread_mutexattr_t attr;
	pthread_mutex_t rec;
	unsigned long n = 200000u;

	if (argc > 1) {
		n = strtoul(argv[1], NULL, 0);
		if (n == 0u) {
			fprintf(stderr, "usage: %s [iterations]\n", argv[0]);
			return EXIT_FAILURE;
		}
	}

	pthread_mutex_init(&bench_common.m, NULL);
	pthread_cond_init(&bench_common.c, NULL);
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&rec, &attr);

	printf("bench-mutex: %lu iterations\n", n);
	bench_print("pthread mutex lock+unlock, uncontended", bench_pthreadPairs(&bench_common.m, n));
	bench_print("recursive mutex lock+unlock, uncontended", bench_pthreadPairs(&rec, n));
#ifdef __phoenix__
	bench_print("kernel mutexLock+mutexUnlock (reference)", bench_kernelPairs(n));
#endif

	bench_common.n = n;
	bench_print("malloc(64)+free, 2 threads at once, per pair", bench_per(bench_two(bench_mallocThread), n));

	bench_common.counter = 0;
	bench_print("locked increment, 2 threads at once, per op", bench_per(bench_two(bench_counterThread), 2u * n));
	if (bench_common.counter != 2u * n) {
		printf("bench-mutex: COUNTER WRONG: %lu, expected %lu\n", bench_common.counter, 2u * n);
		return EXIT_FAILURE;
	}

	bench_print("cond signal/wait round trip, 2 threads", bench_pingPong(n / 10u + 1u));

	pthread_mutex_destroy(&rec);
	pthread_mutexattr_destroy(&attr);
	pthread_cond_destroy(&bench_common.c);
	pthread_mutex_destroy(&bench_common.m);

	return EXIT_SUCCESS;
}
