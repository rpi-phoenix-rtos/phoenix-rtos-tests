/*
 * Phoenix-RTOS
 *
 *    Allocator under concurrency, and the single-threaded lock elision
 *    TESTED:
 *    - malloc(), calloc(), realloc(), free(), posix_memalign() from 4 threads at once
 *    - blocks allocated while single-threaded, verified and freed by other threads
 *    - (Phoenix) __libc_multithreaded: 0 while single-threaded, 1 once
 *      beginthreadex(), pthread_create() or alarm() has started a thread
 *
 * libphoenix takes the heap lock only once the process has a second thread
 * (stdlib/malloc_dl.c, malloc_lock()); the flag is set by beginthreadex(), which
 * every way of starting a thread goes through. If some path started a thread
 * without setting it, the allocator would run UNLOCKED with several threads in
 * it. The stress case below is built to turn that into a failure: 4 threads
 * start together on a barrier and churn a shared heap, every block carries a
 * pattern derived from its own random seed, and each block is checked before it
 * is resized or freed. Two threads popping the same free chunk, or a bin list
 * torn by an unlocked update, hand one block to two owners: one owner's check
 * then finds the other's bytes, or the heap faults. The flag cases check the
 * flag itself, so a missed flip is caught even on a run where the race stays
 * quiet.
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
#include <unistd.h>
#include <sys/wait.h>

#include <unity_fixture.h>

#ifdef __phoenix__
#include <sys/threads.h>

/* libphoenix internal (sys/threads-internal.h): nonzero once a second thread was started */
extern int __libc_multithreaded;
#endif


#define MT_THREADS     4
#define MT_SLOTS       48
#define MT_ITERS       20000
#define MT_SINGLE_ITER 20000
#define MT_ERRLEN      160

/* A large block is rare but always crosses the small/large bin boundary and is
 * big enough to get a heap of its own, so heap creation and release run
 * concurrently too. */
#define MT_SMALL_MAX  512u
#define MT_MEDIUM_MAX 8192u
#define MT_LARGE_MIN  (32u * 1024u)
#define MT_LARGE_SPAN (96u * 1024u)


typedef struct {
	unsigned char *p;
	size_t size;
	uint32_t seed;
} mt_block_t;


typedef struct {
	int id;
	uint32_t rng;
	unsigned int errors;
	char err[MT_ERRLEN];
	mt_block_t slot[MT_SLOTS];
} mt_worker_t;


static struct {
	mt_worker_t w[MT_THREADS];
	mt_worker_t single;
	pthread_barrier_t start;
} mt_common;


static uint32_t mt_rand(uint32_t *s)
{
	uint32_t x = *s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;

	return x;
}


static size_t mt_size(uint32_t *s)
{
	uint32_t r = mt_rand(s);

	if ((r % 64u) == 0u) {
		return MT_LARGE_MIN + ((r >> 8) % MT_LARGE_SPAN);
	}
	if ((r % 16u) == 0u) {
		return MT_SMALL_MAX + ((r >> 8) % (MT_MEDIUM_MAX - MT_SMALL_MAX));
	}

	return 1u + ((r >> 8) % MT_SMALL_MAX);
}


/* The top byte of i * K + seed depends on every bit of the seed, so blocks with
 * different seeds disagree within the first few bytes. */
static inline unsigned char mt_byte(uint32_t seed, size_t i)
{
	return (unsigned char)((((uint32_t)i * 2654435761u) + seed) >> 24);
}


static void mt_fill(mt_block_t *b, size_t from)
{
	size_t i;

	for (i = from; i < b->size; i++) {
		b->p[i] = mt_byte(b->seed, i);
	}
}


static void mt_error(mt_worker_t *w, const char *what, const mt_block_t *b, size_t at)
{
	if (w->errors++ == 0u) {
		(void)snprintf(w->err, sizeof(w->err), "thread %d: %s (block %p size %zu, byte %zu)",
			w->id, what, (void *)b->p, b->size, at);
	}
}


/* Checks bytes [0, n) of the block's pattern */
static int mt_check(mt_worker_t *w, const mt_block_t *b, size_t n, const char *what)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (b->p[i] != mt_byte(b->seed, i)) {
			mt_error(w, what, b, i);
			return -1;
		}
	}

	return 0;
}


static void mt_alloc(mt_worker_t *w, mt_block_t *b)
{
	uint32_t kind = mt_rand(&w->rng) % 8u;
	size_t i, align;
	void *p = NULL;

	b->size = mt_size(&w->rng);
	b->seed = mt_rand(&w->rng);

	if (kind == 6u) {
		b->p = calloc(1, b->size);
		if (b->p != NULL) {
			for (i = 0; i < b->size; i++) {
				if (b->p[i] != 0u) {
					mt_error(w, "calloc() block not zeroed", b, i);
					break;
				}
			}
		}
	}
	else if (kind == 7u) {
		align = (size_t)16u << (mt_rand(&w->rng) % 5u); /* 16 .. 256 */
		if (posix_memalign(&p, align, b->size) != 0) {
			p = NULL;
		}
		b->p = p;
		if ((b->p != NULL) && (((uintptr_t)b->p & (align - 1u)) != 0u)) {
			mt_error(w, "posix_memalign() block misaligned", b, align);
		}
	}
	else {
		b->p = malloc(b->size);
	}

	if (b->p == NULL) {
		mt_error(w, "allocation failed", b, 0);
		b->size = 0;
		return;
	}

	mt_fill(b, 0);
}


static void mt_release(mt_worker_t *w, mt_block_t *b)
{
	if (b->p != NULL) {
		(void)mt_check(w, b, b->size, "block changed before free()");
		free(b->p);
		b->p = NULL;
		b->size = 0;
	}
}


static void mt_resize(mt_worker_t *w, mt_block_t *b)
{
	size_t nsize = mt_size(&w->rng), keep;
	unsigned char *np;

	if (mt_check(w, b, b->size, "block changed before realloc()") != 0) {
		return;
	}

	np = realloc(b->p, nsize);
	if (np == NULL) {
		mt_error(w, "realloc() failed", b, nsize);
		return;
	}

	keep = (nsize < b->size) ? nsize : b->size;
	b->p = np;
	b->size = nsize;
	/* the old contents must survive the move, then the new tail gets the same pattern */
	if (mt_check(w, b, keep, "realloc() lost the contents") == 0) {
		mt_fill(b, keep);
	}
}


static void mt_churn(mt_worker_t *w, unsigned int iters)
{
	unsigned int n;
	mt_block_t *b;

	for (n = 0; (n < iters) && (w->errors == 0u); n++) {
		b = &w->slot[mt_rand(&w->rng) % MT_SLOTS];

		if (b->p == NULL) {
			mt_alloc(w, b);
			continue;
		}

		switch (mt_rand(&w->rng) % 4u) {
			case 0:
			case 1:
				mt_release(w, b);
				break;

			case 2:
				mt_resize(w, b);
				break;

			default:
				mt_release(w, b);
				mt_alloc(w, b);
				break;
		}
	}
}


static void *mt_thread(void *arg)
{
	mt_worker_t *w = arg;
	mt_block_t *b;
	int i;

	(void)pthread_barrier_wait(&mt_common.start);

	/* Blocks the main thread allocated while it was the only thread: they must
	 * be intact and freeable from here */
	for (i = w->id; i < MT_SLOTS; i += MT_THREADS) {
		b = &mt_common.single.slot[i];
		if (b->p != NULL) {
			(void)mt_check(w, b, b->size, "single-threaded block changed");
			free(b->p);
			b->p = NULL;
		}
	}

	mt_churn(w, MT_ITERS);

	for (i = 0; i < MT_SLOTS; i++) {
		mt_release(w, &w->slot[i]);
	}

	return NULL;
}


#ifdef __phoenix__

enum { mt_ok = 0, mt_wasSet, mt_createFailed, mt_notSet, mt_joinFailed };


static char mt_stack[4096] __attribute__((aligned(16)));


static void mt_rawThread(void *arg)
{
	(void)arg;
	endthread();
}


static void *mt_pthread(void *arg)
{
	return arg;
}


/* Runs start() in a fork()ed child of this (still single-threaded) process, so
 * every case sees the flag's 0 -> 1 transition afresh. start() returns 0 once
 * it has started (and, if it can, joined) one thread. */
static void mt_forkExpectFlip(int (*start)(void))
{
	int status = -1, code;
	pid_t pid;

	if (__atomic_load_n(&__libc_multithreaded, __ATOMIC_RELAXED) != 0) {
		TEST_IGNORE_MESSAGE("the test process is already multithreaded (repeated run?)");
	}

	pid = fork();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);

	if (pid == 0) {
		if (__atomic_load_n(&__libc_multithreaded, __ATOMIC_RELAXED) != 0) {
			_exit(mt_wasSet);
		}
		code = start();
		if ((code == mt_ok) && (__atomic_load_n(&__libc_multithreaded, __ATOMIC_RELAXED) != 1)) {
			code = mt_notSet;
		}
		_exit(code);
	}

	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	TEST_ASSERT_TRUE(WIFEXITED(status));
	switch (WEXITSTATUS(status)) {
		case mt_ok:
			break;
		case mt_wasSet:
			TEST_FAIL_MESSAGE("flag already set in a single-threaded child");
			break;
		case mt_createFailed:
			TEST_FAIL_MESSAGE("could not start a thread");
			break;
		case mt_notSet:
			TEST_FAIL_MESSAGE("a thread was started but __libc_multithreaded is still 0: malloc() runs unlocked");
			break;
		default:
			TEST_FAIL_MESSAGE("could not join the thread");
			break;
	}
}


static int mt_startRaw(void)
{
	handle_t tid;

	if (beginthreadex(mt_rawThread, 4, mt_stack, sizeof(mt_stack), NULL, &tid) != 0) {
		return mt_createFailed;
	}
	return (threadJoin(tid, 0) == tid) ? mt_ok : mt_joinFailed;
}


static int mt_startPthread(void)
{
	pthread_t th;

	if (pthread_create(&th, NULL, mt_pthread, NULL) != 0) {
		return mt_createFailed;
	}
	return (pthread_join(th, NULL) == 0) ? mt_ok : mt_joinFailed;
}


/* alarm() starts libc's own helper thread the first time it is called */
static int mt_startAlarm(void)
{
	(void)alarm(3600);
	(void)alarm(0);
	return mt_ok;
}

#endif /* __phoenix__ */


TEST_GROUP(malloc_mt);


TEST_SETUP(malloc_mt)
{
}


TEST_TEAR_DOWN(malloc_mt)
{
}


/* The syscall stub and the servers that call it directly: beginthread() is an
 * inline over this very symbol */
TEST(malloc_mt, beginthreadex_sets_flag)
{
#ifdef __phoenix__
	mt_forkExpectFlip(mt_startRaw);
#else
	TEST_IGNORE_MESSAGE("libphoenix only");
#endif
}


TEST(malloc_mt, pthread_create_sets_flag)
{
#ifdef __phoenix__
	mt_forkExpectFlip(mt_startPthread);
#else
	TEST_IGNORE_MESSAGE("libphoenix only");
#endif
}


TEST(malloc_mt, alarm_thread_sets_flag)
{
#ifdef __phoenix__
	mt_forkExpectFlip(mt_startAlarm);
#else
	TEST_IGNORE_MESSAGE("libphoenix only");
#endif
}


TEST(malloc_mt, single_then_concurrent)
{
	pthread_t th[MT_THREADS];
	unsigned int errors = 0;
	const char *first = "";
	int i;
#ifdef __phoenix__
	int wasSingle = (__atomic_load_n(&__libc_multithreaded, __ATOMIC_RELAXED) == 0) ? 1 : 0;
#endif

	memset(&mt_common, 0, sizeof(mt_common));

	/* Phase 1: the only thread, i.e. the unlocked path. Leaves up to MT_SLOTS
	 * blocks live for the workers to check and free. */
	mt_common.single.id = -1;
	mt_common.single.rng = 0x2545f491u;
	mt_churn(&mt_common.single, MT_SINGLE_ITER);
	TEST_ASSERT_EQUAL_STRING("", mt_common.single.err);
#ifdef __phoenix__
	if (wasSingle != 0) {
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, __atomic_load_n(&__libc_multithreaded, __ATOMIC_RELAXED),
			"single-threaded phase ran with the flag set");
	}
#endif

	/* Phase 2: MT_THREADS threads released together */
	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_init(&mt_common.start, NULL, MT_THREADS + 1));
	for (i = 0; i < MT_THREADS; i++) {
		mt_common.w[i].id = i;
		mt_common.w[i].rng = 0x9e3779b9u * (uint32_t)(i + 1);
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&th[i], NULL, mt_thread, &mt_common.w[i]));
	}
#ifdef __phoenix__
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, __atomic_load_n(&__libc_multithreaded, __ATOMIC_RELAXED),
		"threads started but the flag is clear: the allocator is running unlocked");
#endif
	(void)pthread_barrier_wait(&mt_common.start);

	for (i = 0; i < MT_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(th[i], NULL));
		if ((mt_common.w[i].errors != 0u) && (errors == 0u)) {
			first = mt_common.w[i].err;
		}
		errors += mt_common.w[i].errors;
	}
	(void)pthread_barrier_destroy(&mt_common.start);

	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, errors, first);
}


TEST_GROUP_RUNNER(malloc_mt)
{
	/* the fork() cases need this process still single-threaded: run them first */
	RUN_TEST_CASE(malloc_mt, beginthreadex_sets_flag);
	RUN_TEST_CASE(malloc_mt, pthread_create_sets_flag);
	RUN_TEST_CASE(malloc_mt, alarm_thread_sets_flag);
	RUN_TEST_CASE(malloc_mt, single_then_concurrent);
}
