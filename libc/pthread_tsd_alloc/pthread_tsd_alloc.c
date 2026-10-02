/*
 * Phoenix-RTOS
 *
 * phoenix-rtos-tests
 *
 * pthread keys under an allocator that itself uses pthread keys
 *
 * mimalloc, which the WebKit ports link in place of libphoenix's malloc, calls
 * pthread_setspecific() on a thread's first allocation. libphoenix's
 * pthread_setspecific() used to allocate its list node while holding its
 * (non-recursive) key lock, so a thread whose first allocation happened there
 * deadlocked on itself, and every other thread then blocked on the lock: WPE
 * WebKit hung at start (2026-10-02). This binary replaces malloc() with a
 * small allocator that does the same as mimalloc, so the old code hangs here.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <malloc.h>

#include "unity_fixture.h"


/* ---- the allocator: a bump arena, never reused; enough for one short test run ---- */

#define ARENA_SIZE (32u << 20)

static uint8_t *arena;
static size_t arenaUsed;
static volatile int arenaLock;
static pthread_key_t allocKey;
static volatile int allocKeyReady;
static __thread int allocThreadSeen;


static void *arena_alloc(size_t size)
{
	size_t *hdr, need = ((size + sizeof(size_t) + 15u) & ~(size_t)15u);

	while (__atomic_exchange_n(&arenaLock, 1, __ATOMIC_ACQUIRE) != 0) {
	}
	if (arena == NULL) {
		void *m = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		arena = (m == MAP_FAILED) ? NULL : m;
	}
	if ((arena == NULL) || (arenaUsed + need > ARENA_SIZE)) {
		__atomic_store_n(&arenaLock, 0, __ATOMIC_RELEASE);
		return NULL;
	}
	hdr = (size_t *)(arena + arenaUsed + 8u);
	arenaUsed += need + 16u;
	__atomic_store_n(&arenaLock, 0, __ATOMIC_RELEASE);
	hdr[-1] = size;
	return hdr;
}


void *malloc(size_t size)
{
	/* What mimalloc does on a thread's first allocation: register the thread's
	 * heap under a pthread key (_mi_prim_thread_associate_default_heap) */
	if ((allocThreadSeen == 0) && (allocKeyReady != 0)) {
		allocThreadSeen = 1;
		(void)pthread_setspecific(allocKey, &allocThreadSeen);
	}
	return arena_alloc(size);
}


void free(void *ptr)
{
	(void)ptr;
}


void *calloc(size_t n, size_t size)
{
	if ((size != 0u) && (n > SIZE_MAX / size)) {
		return NULL;
	}
	return malloc(n * size); /* fresh mmap()ed memory is zero */
}


size_t malloc_usable_size(void *ptr)
{
	return (ptr == NULL) ? 0u : ((size_t *)ptr)[-1];
}


void *realloc(void *ptr, size_t size)
{
	void *p = malloc(size);
	if ((p != NULL) && (ptr != NULL)) {
		size_t old = malloc_usable_size(ptr);
		memcpy(p, ptr, (old < size) ? old : size);
	}
	return p;
}


void *reallocf(void *ptr, size_t size)
{
	return realloc(ptr, size);
}


void *_malloc_aligned(size_t alignment, size_t size)
{
	uint8_t *p = malloc(size + alignment);
	return (p == NULL) ? NULL : (void *)(((uintptr_t)p + alignment - 1u) & ~(uintptr_t)(alignment - 1u));
}


int malloc_trim(size_t pad)
{
	(void)pad;
	return 0;
}


void mallocInfo(mallocInfo_t *info)
{
	(void)memset(info, 0, sizeof(*info));
}


/* libphoenix's startup and fork() call these; they live next to its malloc */
void _malloc_init(void)
{
}


void _malloc_forkPrepare(void)
{
}


void _malloc_forkParent(void)
{
}


void _malloc_forkChild(void)
{
}


/* ---- the tests ---- */

static pthread_key_t tsdKey;
static sem_t tsdDone;
static volatile int tsdResult;
static int tsdMarker;


TEST_GROUP(pthread_tsd_alloc);


TEST_SETUP(pthread_tsd_alloc)
{
	tsdResult = -1;
	TEST_ASSERT_EQUAL_INT(0, sem_init(&tsdDone, 0, 0));
}


TEST_TEAR_DOWN(pthread_tsd_alloc)
{
	(void)sem_destroy(&tsdDone);
}


/* The thread's FIRST allocation is the list node pthread_setspecific() makes */
static void *tsd_firstActIsSetspecific(void *arg)
{
	(void)arg;
	tsdResult = pthread_setspecific(tsdKey, &tsdMarker);
	if ((tsdResult == 0) && (pthread_getspecific(tsdKey) != &tsdMarker)) {
		tsdResult = EINVAL;
	}
	(void)sem_post(&tsdDone);
	return NULL;
}


static int tsd_waitDone(int secs)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += secs;
	return sem_timedwait(&tsdDone, &ts);
}


TEST(pthread_tsd_alloc, setspecific_when_allocator_uses_keys)
{
	pthread_t t;

	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsdKey, NULL));

	/* A deadlocked thread cannot be joined: wait for it with a bound instead */
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, tsd_firstActIsSetspecific, NULL));
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, tsd_waitDone(5), "pthread_setspecific() deadlocked under a key-using allocator");
	TEST_ASSERT_EQUAL_INT(0, tsdResult);
	TEST_ASSERT_EQUAL_INT(0, pthread_join(t, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsdKey));
}


/* Many threads at once: each one's first allocation re-enters the key code */
TEST(pthread_tsd_alloc, many_threads_first_setspecific)
{
	pthread_t t[8];
	int i;

	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsdKey, NULL));
	for (i = 0; i < 8; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&t[i], NULL, tsd_firstActIsSetspecific, NULL));
	}
	for (i = 0; i < 8; i++) {
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, tsd_waitDone(5), "pthread_setspecific() deadlocked under a key-using allocator");
	}
	for (i = 0; i < 8; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(t[i], NULL));
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsdKey));
}


TEST_GROUP_RUNNER(pthread_tsd_alloc)
{
	RUN_TEST_CASE(pthread_tsd_alloc, setspecific_when_allocator_uses_keys);
	RUN_TEST_CASE(pthread_tsd_alloc, many_threads_first_setspecific);
}


static void runner(void)
{
	/* The allocator's key exists before any thread starts allocating */
	if (pthread_key_create(&allocKey, NULL) == 0) {
		allocKeyReady = 1;
	}
	RUN_TEST_GROUP(pthread_tsd_alloc);
}


int main(int argc, char *argv[])
{
	return UnityMain(argc, (const char **)argv, runner) == 0 ? 0 : 1;
}
