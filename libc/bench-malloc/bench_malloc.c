/*
 * Phoenix-RTOS
 *
 * bench-malloc: cost of a malloc(64) + free() pair, before and after the
 * process becomes multithreaded, and of a frame-sized malloc + memset + free
 *
 * libphoenix skips the heap lock while a process has only one thread
 * (stdlib/malloc_dl.c); on Phoenix the lock is a syscall pair. The first number
 * is the unlocked path, the second the locked one, measured in the same process
 * after one thread was started and joined (the flag never goes back to 0). The
 * difference is the price of the lock; if the two are equal the elision is not
 * in this build.
 *
 * The frame line allocates, fills and frees a 3 MiB block (a 1080p frame)
 * in a loop, as a video player or decoder does per frame. An allocator that
 * returns the block's heap to the system on free() pays a fresh mapping and a
 * page fault per page on every iteration; one that keeps the heap pays only for
 * the memset. "mapped after free" is how much the allocator still has mapped
 * once the last block is freed (libphoenix only).
 *
 *   bench-malloc [pairs] [frames]    (defaults 200000, 100)
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
#include <string.h>
#include <time.h>

#ifdef __phoenix__
#include <malloc.h>
#endif

#ifdef __phoenix__
/* libphoenix internal (sys/threads-internal.h): nonzero once a second thread was started */
extern int __libc_multithreaded;
#define BENCH_FLAG() __atomic_load_n(&__libc_multithreaded, __ATOMIC_RELAXED)
#else
#define BENCH_FLAG() -1
#endif


/* Stores through this keep the compiler from pairing up and removing the calls */
static void *volatile bench_sink;


static uint64_t bench_nowNs(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);

	return ((uint64_t)ts.tv_sec * 1000000000u) + (uint64_t)ts.tv_nsec;
}


/* Returns tenths of a nanosecond per pair (no floating point: IO_NO_FLOAT targets) */
static uint64_t bench_pairs(unsigned long n)
{
	unsigned long i;
	uint64_t t0, t1;

	/* warm the small bin so the timed loop reuses one chunk */
	for (i = 0; i < 1000u; i++) {
		bench_sink = malloc(64);
		free(bench_sink);
	}

	t0 = bench_nowNs();
	for (i = 0; i < n; i++) {
		bench_sink = malloc(64);
		free(bench_sink);
	}
	t1 = bench_nowNs();

	return ((t1 - t0) * 10u) / n;
}


#define BENCH_FRAME (3u * 1024u * 1024u)


/* Returns nanoseconds per frame-sized malloc + memset + free */
static uint64_t bench_frames(unsigned long n)
{
	unsigned long i;
	uint64_t t0, t1;
	unsigned char *p;

	/* the first one creates the heap */
	p = malloc(BENCH_FRAME);
	if (p != NULL) {
		memset(p, 0, BENCH_FRAME);
		free(p);
	}

	t0 = bench_nowNs();
	for (i = 0; i < n; i++) {
		p = malloc(BENCH_FRAME);
		if (p == NULL) {
			return 0;
		}
		memset(p, (int)i, BENCH_FRAME);
		/* the memset is a dead store to the compiler, which knows free() */
		__asm__ volatile("" : : "r"(p) : "memory");
		free(p);
	}
	t1 = bench_nowNs();

	return (t1 - t0) / n;
}


static void *bench_thread(void *arg)
{
	return arg;
}


int main(int argc, char *argv[])
{
	unsigned long n = 200000u, frames = 100u;
	uint64_t single, multi, frame;
	pthread_t th;
	int flag0, flag1;

	if (argc > 1) {
		n = strtoul(argv[1], NULL, 0);
		if (n == 0u) {
			fprintf(stderr, "usage: %s [pairs] [frames]\n", argv[0]);
			return EXIT_FAILURE;
		}
	}
	if (argc > 2) {
		frames = strtoul(argv[2], NULL, 0);
		if (frames == 0u) {
			fprintf(stderr, "usage: %s [pairs] [frames]\n", argv[0]);
			return EXIT_FAILURE;
		}
	}

	flag0 = BENCH_FLAG();
	single = bench_pairs(n);

	if (pthread_create(&th, NULL, bench_thread, NULL) != 0) {
		fprintf(stderr, "bench-malloc: pthread_create() failed\n");
		return EXIT_FAILURE;
	}
	(void)pthread_join(th, NULL);

	flag1 = BENCH_FLAG();
	multi = bench_pairs(n);

	printf("bench-malloc: %lu malloc(64)+free() pairs\n", n);
	printf("bench-malloc: single-threaded  %7llu.%llu ns/pair (multithreaded flag %d)\n",
		(unsigned long long)(single / 10u), (unsigned long long)(single % 10u), flag0);
	printf("bench-malloc: after one thread %7llu.%llu ns/pair (multithreaded flag %d)\n",
		(unsigned long long)(multi / 10u), (unsigned long long)(multi % 10u), flag1);
	if (single != 0u) {
		printf("bench-malloc: ratio %llu.%llux\n", (unsigned long long)(multi / single),
			(unsigned long long)(((multi % single) * 10u) / single));
	}

	frame = bench_frames(frames);
	printf("bench-malloc: frame %u B malloc+memset+free %7llu ns/frame (%lu frames)\n",
		BENCH_FRAME, (unsigned long long)frame, frames);
#ifdef __phoenix__
	{
		mallocInfo_t info;

		mallocInfo(&info);
		printf("bench-malloc: mapped after free %zu kB\n", info.mapsz / 1024u);
	}
#endif

	return EXIT_SUCCESS;
}
