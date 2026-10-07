/*
 * Phoenix-RTOS
 *
 * phoenix-rtos-tests
 *
 * pthread thread-specific data (keys)
 *
 * TSD had essentially no coverage, which matters more here than it looks:
 * libstdc++ for this target is built with threads but WITHOUT TLS
 * (_GLIBCXX_HAS_GTHREADS=1, _GLIBCXX_HAVE_TLS undef), so its per-thread state --
 * __cxa_eh_globals among it -- is routed through pthread keys. Every C++ program
 * with threads therefore leans on exactly this code.
 *
 * The lookup takes no lock (WebKit, GLib and libstdc++ call it for every
 * per-thread read), so beyond the semantics this covers what that must
 * survive: keys deleted and re-created at the same address, a thread cancelled
 * while reading its keys, and lookups racing a thread that deletes keys -- plus
 * a cost bound that the old lock-and-syscall lookup fails.
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

#include "unity_fixture.h"


#define TSD_THREADS 4

static pthread_key_t tsd_key;
static pthread_key_t tsd_dtorKey;
static volatile int tsd_dtorCalls;
static void *tsd_seen[TSD_THREADS];


TEST_GROUP(pthread_tsd);


TEST_SETUP(pthread_tsd)
{
	tsd_dtorCalls = 0;
	(void)memset(tsd_seen, 0, sizeof(tsd_seen));
}


TEST_TEAR_DOWN(pthread_tsd)
{
}


/* Each thread must see ONLY its own value. A shared/global store would make the
 * threads read each other's, which is the failure this exists to catch. */
static void *tsd_isolationBody(void *arg)
{
	int idx = (int)(intptr_t)arg;
	void *mine = (void *)(intptr_t)(0x1000 + idx);
	int i;

	if (pthread_setspecific(tsd_key, mine) != 0) {
		return (void *)(intptr_t)-1;
	}

	/* Give the other threads time to overwrite a shared slot if there is one. */
	for (i = 0; i < 100; i++) {
		usleep(100);
		if (pthread_getspecific(tsd_key) != mine) {
			return (void *)(intptr_t)-1;
		}
	}

	tsd_seen[idx] = pthread_getspecific(tsd_key);
	return NULL;
}


TEST(pthread_tsd, value_is_per_thread)
{
	pthread_t th[TSD_THREADS];
	void *ret;
	int i;

	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_key, NULL));

	for (i = 0; i < TSD_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&th[i], NULL, tsd_isolationBody,
			(void *)(intptr_t)i));
	}
	for (i = 0; i < TSD_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(th[i], &ret));
		TEST_ASSERT_NULL(ret);
	}
	for (i = 0; i < TSD_THREADS; i++) {
		TEST_ASSERT_EQUAL_PTR((void *)(intptr_t)(0x1000 + i), tsd_seen[i]);
	}

	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_key));
}


/* A key created in one thread must be usable from another, and must start unset
 * there -- a fresh thread inherits no values. */
static void *tsd_freshBody(void *arg)
{
	(void)arg;
	return pthread_getspecific(tsd_key);
}


TEST(pthread_tsd, new_thread_starts_unset)
{
	pthread_t th;
	void *ret;

	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_key, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_setspecific(tsd_key, (void *)0xabc));

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, tsd_freshBody, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(th, &ret));
	TEST_ASSERT_NULL(ret);

	/* ...and the creating thread's value survived the other thread. */
	TEST_ASSERT_EQUAL_PTR((void *)0xabc, pthread_getspecific(tsd_key));

	TEST_ASSERT_EQUAL_INT(0, pthread_setspecific(tsd_key, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_key));
}


static void tsd_dtor(void *value)
{
	if (value != NULL) {
		tsd_dtorCalls++;
	}
}


static void *tsd_dtorBody(void *arg)
{
	(void)arg;
	(void)pthread_setspecific(tsd_dtorKey, (void *)0x55);
	return NULL;
}


/* The destructor must run when a thread with a non-NULL value exits. This is what
 * libstdc++ relies on to tear down its per-thread state without TLS. */
TEST(pthread_tsd, destructor_runs_at_thread_exit)
{
	pthread_t th;

	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_dtorKey, tsd_dtor));

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, tsd_dtorBody, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(th, NULL));

	TEST_ASSERT_EQUAL_INT(1, tsd_dtorCalls);

	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_dtorKey));
}


/* Overwriting a value must replace it, not stack a second entry that a later
 * lookup could find first. */
TEST(pthread_tsd, overwrite_replaces_value)
{
	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_key, NULL));

	TEST_ASSERT_EQUAL_INT(0, pthread_setspecific(tsd_key, (void *)0x11));
	TEST_ASSERT_EQUAL_PTR((void *)0x11, pthread_getspecific(tsd_key));
	TEST_ASSERT_EQUAL_INT(0, pthread_setspecific(tsd_key, (void *)0x22));
	TEST_ASSERT_EQUAL_PTR((void *)0x22, pthread_getspecific(tsd_key));
	TEST_ASSERT_EQUAL_INT(0, pthread_setspecific(tsd_key, NULL));
	TEST_ASSERT_NULL(pthread_getspecific(tsd_key));

	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_key));
}


/* ---- many keys, deletion, key reuse ---- */

#define TSD_MANY_KEYS 64

static pthread_key_t tsd_many[TSD_MANY_KEYS];


static void *tsd_tag(int thread, int key, unsigned int seq)
{
	/* Never NULL, and unique per (thread, key, seq & 0xffff) */
	return (void *)(((uintptr_t)(thread + 1) << 24) | ((uintptr_t)key << 16) | (uintptr_t)(seq & 0xffffu));
}


static void *tsd_manyBody(void *arg)
{
	int idx = (int)(intptr_t)arg;
	int i, round;

	for (round = 0; round < 50; round++) {
		for (i = 0; i < TSD_MANY_KEYS; i++) {
			if (pthread_setspecific(tsd_many[i], tsd_tag(idx, i, (unsigned int)round)) != 0) {
				return (void *)(intptr_t)-1;
			}
		}
		usleep(100);
		for (i = 0; i < TSD_MANY_KEYS; i++) {
			if (pthread_getspecific(tsd_many[i]) != tsd_tag(idx, i, (unsigned int)round)) {
				return (void *)(intptr_t)-1;
			}
		}
	}

	return NULL;
}


/* Many keys, set in several threads at once: every thread must read back, for
 * every key, exactly what it stored there -- not another key's value, not
 * another thread's. */
TEST(pthread_tsd, many_keys_per_thread)
{
	pthread_t th[TSD_THREADS];
	void *ret;
	int i;

	for (i = 0; i < TSD_MANY_KEYS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_many[i], NULL));
	}
	for (i = 0; i < TSD_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&th[i], NULL, tsd_manyBody, (void *)(intptr_t)i));
	}
	for (i = 0; i < TSD_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(th[i], &ret));
		TEST_ASSERT_NULL(ret);
	}
	/* This thread set none of them */
	for (i = 0; i < TSD_MANY_KEYS; i++) {
		TEST_ASSERT_NULL(pthread_getspecific(tsd_many[i]));
	}
	for (i = 0; i < TSD_MANY_KEYS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_many[i]));
	}
}


#define TSD_REUSE_ROUNDS 200

static pthread_barrier_t tsd_barrier;
static volatile int tsd_reuseFailures;


static void *tsd_reuseBody(void *arg)
{
	int round;

	(void)arg;
	for (round = 0; round < TSD_REUSE_ROUNDS; round++) {
		(void)pthread_barrier_wait(&tsd_barrier); /* the key of this round exists */
		if (pthread_getspecific(tsd_key) != NULL) {
			tsd_reuseFailures++;
		}
		if (pthread_setspecific(tsd_key, tsd_tag(1, 0, (unsigned int)round)) != 0) {
			tsd_reuseFailures++;
		}
		if (pthread_getspecific(tsd_key) != tsd_tag(1, 0, (unsigned int)round)) {
			tsd_reuseFailures++;
		}
		(void)pthread_barrier_wait(&tsd_barrier); /* set; the key may now be deleted */
	}

	return NULL;
}


/* A key deleted and then created again -- usually at the same address, as the
 * allocator hands the freed block straight back -- must start out unset in
 * every thread, including one that held a value under the old key and has not
 * touched its keys since. */
TEST(pthread_tsd, recreated_key_starts_unset)
{
	pthread_t th;
	pthread_key_t prev = NULL;
	int round, reused = 0;

	tsd_reuseFailures = 0;
	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_init(&tsd_barrier, NULL, 2));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, tsd_reuseBody, NULL));

	for (round = 0; round < TSD_REUSE_ROUNDS; round++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_key, NULL));
		if (tsd_key == prev) {
			reused++;
		}
		TEST_ASSERT_NULL(pthread_getspecific(tsd_key));
		TEST_ASSERT_EQUAL_INT(0, pthread_setspecific(tsd_key, tsd_tag(0, 0, (unsigned int)round)));
		(void)pthread_barrier_wait(&tsd_barrier);
		(void)pthread_barrier_wait(&tsd_barrier);
		TEST_ASSERT_EQUAL_PTR(tsd_tag(0, 0, (unsigned int)round), pthread_getspecific(tsd_key));
		prev = tsd_key;
		TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_key));
	}

	TEST_ASSERT_EQUAL_INT(0, pthread_join(th, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_destroy(&tsd_barrier));
	TEST_ASSERT_EQUAL_INT(0, tsd_reuseFailures);

	/* Not a requirement, but without reuse this case proves less than it says */
	printf("TSD recreated_key_starts_unset: key address reused in %d/%d rounds\n", reused, TSD_REUSE_ROUNDS - 1);
}


static void *tsd_setThenWaitBody(void *arg)
{
	(void)arg;
	(void)pthread_setspecific(tsd_dtorKey, (void *)0x77);
	(void)pthread_barrier_wait(&tsd_barrier); /* value set */
	(void)pthread_barrier_wait(&tsd_barrier); /* key deleted: exit holding the value */
	return NULL;
}


/* POSIX: pthread_key_delete() calls no destructor, and a thread that held a
 * value under the deleted key must not have it destroyed when it exits. */
TEST(pthread_tsd, deleted_key_destructor_not_called)
{
	pthread_t th;

	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_init(&tsd_barrier, NULL, 2));
	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_dtorKey, tsd_dtor));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, tsd_setThenWaitBody, NULL));

	(void)pthread_barrier_wait(&tsd_barrier);
	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_dtorKey));
	TEST_ASSERT_EQUAL_INT(0, tsd_dtorCalls);
	(void)pthread_barrier_wait(&tsd_barrier);

	TEST_ASSERT_EQUAL_INT(0, pthread_join(th, NULL));
	TEST_ASSERT_EQUAL_INT(0, tsd_dtorCalls);
	TEST_ASSERT_EQUAL_INT(0, pthread_barrier_destroy(&tsd_barrier));
}


#define TSD_DTOR_KEYS 8

static pthread_key_t tsd_dtorKeys[TSD_DTOR_KEYS];
static volatile int tsd_dtorMask;


static void tsd_dtorMark(void *value)
{
	__atomic_fetch_or(&tsd_dtorMask, (int)(intptr_t)value, __ATOMIC_RELAXED);
}


static void *tsd_dtorManyBody(void *arg)
{
	int i;

	(void)arg;
	for (i = 0; i < TSD_DTOR_KEYS; i++) {
		(void)pthread_setspecific(tsd_dtorKeys[i], (void *)(intptr_t)(1 << i));
	}
	return NULL;
}


/* Every key with a value gets its own destructor call with its own value. */
TEST(pthread_tsd, destructor_per_key)
{
	pthread_t th;
	int i;

	tsd_dtorMask = 0;
	for (i = 0; i < TSD_DTOR_KEYS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_dtorKeys[i], tsd_dtorMark));
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, tsd_dtorManyBody, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(th, NULL));
	TEST_ASSERT_EQUAL_HEX32((1 << TSD_DTOR_KEYS) - 1, tsd_dtorMask);
	for (i = 0; i < TSD_DTOR_KEYS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_dtorKeys[i]));
	}
}


static void *tsd_createKeyBody(void *arg)
{
	(void)arg;
	return (void *)(intptr_t)pthread_key_create(&tsd_key, NULL);
}


/* A key stays valid when the thread that created it exits. libphoenix once
 * refused to delete a key no live thread held a value for -- EINVAL. */
TEST(pthread_tsd, delete_after_creator_exited)
{
	pthread_t th;
	void *ret;

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, tsd_createKeyBody, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(th, &ret));
	TEST_ASSERT_NULL(ret);

	TEST_ASSERT_NULL(pthread_getspecific(tsd_key));
	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_key));
}


/* ---- cancellation of a thread that is reading its keys ---- */

static volatile int tsd_cancelReady;
static volatile int tsd_cancelBad;


static void *tsd_cancelBody(void *arg)
{
	(void)arg;
	(void)pthread_setspecific(tsd_dtorKey, (void *)0x99);
	__atomic_store_n(&tsd_cancelReady, 1, __ATOMIC_RELEASE);

	/* pthread_cancel() runs this thread's destructors from the CANCELLING thread
	 * while this one still runs; its lookups take no lock, so they race the
	 * cleanup and must only ever see the value or NULL. */
	for (;;) {
		void *v = pthread_getspecific(tsd_dtorKey);
		if ((v != (void *)0x99) && (v != NULL)) {
			tsd_cancelBad = 1;
		}
	}

	return NULL;
}


TEST(pthread_tsd, cancel_while_reading)
{
	pthread_t th;
	void *ret = NULL;

	tsd_cancelReady = 0;
	tsd_cancelBad = 0;
	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_dtorKey, tsd_dtor));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, tsd_cancelBody, NULL));
	while (__atomic_load_n(&tsd_cancelReady, __ATOMIC_ACQUIRE) == 0) {
		usleep(1000);
	}
	usleep(10000);

	TEST_ASSERT_EQUAL_INT(0, pthread_cancel(th));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(th, &ret));
	TEST_ASSERT_EQUAL_PTR((void *)PTHREAD_CANCELED, ret);
	TEST_ASSERT_EQUAL_INT(1, tsd_dtorCalls);
	TEST_ASSERT_EQUAL_INT(0, tsd_cancelBad);
	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_dtorKey));
}


/* ---- concurrency stress ---- */

#define TSD_STRESS_WORKERS 4
#define TSD_STRESS_STABLE  8
#define TSD_STRESS_MS      3000

static struct {
	pthread_key_t stable[TSD_STRESS_STABLE];
	pthread_key_t churn; /* deleted and re-created all the time, under `lock` */
	unsigned int gen;    /* bumped with every new churn key, under `lock` */
	pthread_rwlock_t lock;
	volatile int stop;
	volatile int bad;
	volatile unsigned long ops;
	volatile unsigned long churns;
} tsd_stress;


static void tsd_stressBad(void)
{
	__atomic_store_n(&tsd_stress.bad, 1, __ATOMIC_RELAXED);
}


static void *tsd_stressWorker(void *arg)
{
	int idx = (int)(intptr_t)arg;
	unsigned int seq, last[TSD_STRESS_STABLE];
	unsigned long ops = 0;
	int i;

	for (i = 0; i < TSD_STRESS_STABLE; i++) {
		last[i] = 0;
		if ((pthread_setspecific(tsd_stress.stable[i], tsd_tag(idx, i, 0)) != 0)) {
			tsd_stressBad();
		}
	}

	for (seq = 1; __atomic_load_n(&tsd_stress.stop, __ATOMIC_RELAXED) == 0; seq++) {
		/* Stable keys, no lock: these lookups walk a list in which the churn
		 * thread is tombstoning entries at the same moment. Each must return
		 * exactly the value this thread stored last under that key. */
		for (i = 0; i < TSD_STRESS_STABLE; i++) {
			if (pthread_getspecific(tsd_stress.stable[i]) != tsd_tag(idx, i, last[i])) {
				tsd_stressBad();
			}
			if ((seq % (unsigned int)(i + 1)) == 0) {
				last[i] = seq;
				if (pthread_setspecific(tsd_stress.stable[i], tsd_tag(idx, i, seq)) != 0) {
					tsd_stressBad();
				}
			}
			ops += 2;
		}

		/* The churn key: a key's handle may only be used while it exists, so
		 * under the read lock. A value of this thread's must belong to the
		 * CURRENT generation: one from an earlier key at the same address is the
		 * key-reuse bug. */
		if ((seq % 16u) == 0) {
			(void)pthread_rwlock_rdlock(&tsd_stress.lock);
			void *v = pthread_getspecific(tsd_stress.churn);
			void *want = tsd_tag(idx, TSD_STRESS_STABLE, tsd_stress.gen);
			if ((v != NULL) && (v != want)) {
				tsd_stressBad();
			}
			if (pthread_setspecific(tsd_stress.churn, want) != 0) {
				tsd_stressBad();
			}
			(void)pthread_rwlock_unlock(&tsd_stress.lock);
			ops += 2;
		}
	}

	__atomic_fetch_add(&tsd_stress.ops, ops, __ATOMIC_RELAXED);
	return NULL;
}


static void *tsd_stressChurner(void *arg)
{
	pthread_key_t spare;

	(void)arg;
	while (__atomic_load_n(&tsd_stress.stop, __ATOMIC_RELAXED) == 0) {
		(void)pthread_rwlock_wrlock(&tsd_stress.lock);
		if ((pthread_key_delete(tsd_stress.churn) != 0) || (pthread_key_create(&tsd_stress.churn, NULL) != 0)) {
			tsd_stressBad();
		}
		tsd_stress.gen++;
		(void)pthread_rwlock_unlock(&tsd_stress.lock);

		/* A key no worker uses: its delete still walks every thread's list */
		if (pthread_key_create(&spare, NULL) != 0) {
			tsd_stressBad();
		}
		else {
			(void)pthread_setspecific(spare, (void *)1);
			(void)pthread_key_delete(spare);
		}
		tsd_stress.churns++;
		usleep(100);
	}

	return NULL;
}


/* Workers read and write their keys without pause while another thread
 * deletes and re-creates keys. Nothing may crash, and no lookup may return a
 * value stored under a different key or by a different thread. */
TEST(pthread_tsd, stress_lookups_during_key_churn)
{
	pthread_t workers[TSD_STRESS_WORKERS], churner;
	int i;

	(void)memset(&tsd_stress, 0, sizeof(tsd_stress));
	TEST_ASSERT_EQUAL_INT(0, pthread_rwlock_init(&tsd_stress.lock, NULL));
	for (i = 0; i < TSD_STRESS_STABLE; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_stress.stable[i], NULL));
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_stress.churn, NULL));

	for (i = 0; i < TSD_STRESS_WORKERS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&workers[i], NULL, tsd_stressWorker, (void *)(intptr_t)i));
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&churner, NULL, tsd_stressChurner, NULL));

	usleep(TSD_STRESS_MS * 1000);
	__atomic_store_n(&tsd_stress.stop, 1, __ATOMIC_RELAXED);

	for (i = 0; i < TSD_STRESS_WORKERS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_join(workers[i], NULL));
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_join(churner, NULL));

	printf("TSD stress: %lu lookups+stores, %lu key churns in %d ms\n", tsd_stress.ops, tsd_stress.churns, TSD_STRESS_MS);
	TEST_ASSERT_EQUAL_INT(0, tsd_stress.bad);
	/* The churn key must actually have been churned while workers ran */
	TEST_ASSERT_GREATER_THAN(10, (int)tsd_stress.churns);

	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_stress.churn));
	for (i = 0; i < TSD_STRESS_STABLE; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_stress.stable[i]));
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_rwlock_destroy(&tsd_stress.lock));
}


/* ---- cost ---- */

#define TSD_PERF_CALLS     1000000
#define TSD_PERF_FILLERS   8
#define TSD_PERF_BUDGET_MS 200


static uint64_t tsd_nowNs(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}


/* pthread_getspecific() is what WebKit's WTF::Thread::current(), GLib's
 * GPrivate and libstdc++ (built without TLS here) call for every per-thread
 * read: in the JetStream `splay` profile the WebKit main thread spent 60% of a
 * CPU in the kernel, nearly all of it in the two mutex system calls the old
 * lookup made per call (plus pthread_self()'s own). A lookup must cost no more
 * than a short list walk: 1,000,000 of them, with the key behind 8 others,
 * well under 200 ms.
 *
 * This case FAILS on the libphoenix before the lock-free lookup (2026-10-07):
 * each call took pthread_list_lock twice and pthread_key_lock once, every one
 * a kernel mutex, i.e. several microseconds per call -- seconds in total. */
TEST(pthread_tsd, getspecific_cost)
{
	pthread_key_t fillers[TSD_PERF_FILLERS];
	volatile uintptr_t sink = 0;
	uintptr_t got;
	uint64_t t0, getNs, setNs, selfNs;
	int i;

	/* Set first: new entries go to the head of the list, so this one ends up
	 * last, behind every filler */
	TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&tsd_key, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_setspecific(tsd_key, (void *)0x5a));
	for (i = 0; i < TSD_PERF_FILLERS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_key_create(&fillers[i], NULL));
		TEST_ASSERT_EQUAL_INT(0, pthread_setspecific(fillers[i], (void *)(intptr_t)(i + 1)));
	}

	t0 = tsd_nowNs();
	for (i = 0; i < TSD_PERF_CALLS; i++) {
		sink += (uintptr_t)pthread_getspecific(tsd_key);
	}
	getNs = tsd_nowNs() - t0;
	got = sink;

	t0 = tsd_nowNs();
	for (i = 0; i < TSD_PERF_CALLS; i++) {
		(void)pthread_setspecific(tsd_key, (void *)(uintptr_t)(i | 1));
	}
	setNs = tsd_nowNs() - t0;

	t0 = tsd_nowNs();
	for (i = 0; i < TSD_PERF_CALLS; i++) {
		sink += (uintptr_t)pthread_self();
	}
	selfNs = tsd_nowNs() - t0;

	printf("TSD getspecific ns_per_call=%u\n", (unsigned int)(getNs / TSD_PERF_CALLS));
	printf("TSD setspecific ns_per_call=%u\n", (unsigned int)(setNs / TSD_PERF_CALLS));
	printf("TSD pthread_self ns_per_call=%u\n", (unsigned int)(selfNs / TSD_PERF_CALLS));

	TEST_ASSERT_TRUE(got == TSD_PERF_CALLS * (uintptr_t)0x5a);
	TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)((TSD_PERF_CALLS - 1) | 1), pthread_getspecific(tsd_key));

	for (i = 0; i < TSD_PERF_FILLERS; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(fillers[i]));
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_key_delete(tsd_key));

	TEST_ASSERT_LESS_THAN_UINT(TSD_PERF_BUDGET_MS, (unsigned int)(getNs / 1000000u));
	TEST_ASSERT_LESS_THAN_UINT(TSD_PERF_BUDGET_MS, (unsigned int)(setNs / 1000000u));
}


TEST_GROUP_RUNNER(pthread_tsd)
{
	RUN_TEST_CASE(pthread_tsd, value_is_per_thread);
	RUN_TEST_CASE(pthread_tsd, new_thread_starts_unset);
	RUN_TEST_CASE(pthread_tsd, destructor_runs_at_thread_exit);
	RUN_TEST_CASE(pthread_tsd, overwrite_replaces_value);
	RUN_TEST_CASE(pthread_tsd, many_keys_per_thread);
	RUN_TEST_CASE(pthread_tsd, recreated_key_starts_unset);
	RUN_TEST_CASE(pthread_tsd, deleted_key_destructor_not_called);
	RUN_TEST_CASE(pthread_tsd, destructor_per_key);
	RUN_TEST_CASE(pthread_tsd, delete_after_creator_exited);
	RUN_TEST_CASE(pthread_tsd, cancel_while_reading);
	RUN_TEST_CASE(pthread_tsd, stress_lookups_during_key_churn);
	RUN_TEST_CASE(pthread_tsd, getspecific_cost);
}


void runner(void)
{
	RUN_TEST_GROUP(pthread_tsd);
}


int main(int argc, char *argv[])
{
	return UnityMain(argc, (const char **)argv, runner) == 0 ? 0 : 1;
}
