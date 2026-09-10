/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - unistd.h
 *
 * TESTED (libphoenix):
 *    - the POSIX OPTION macros, and the contract they carry.
 *
 * libphoenix declares _POSIX_VERSION 200809L. An option macro is a promise: a
 * caller that sees _POSIX_MONOTONIC_CLOCK is entitled to clock_gettime with a
 * monotonic clock, one that sees _POSIX_THREADS is entitled to pthreads, and so
 * on. Two things can go wrong and both are silent:
 *
 *   1. The macro is MISSING for a group we do implement. Portable code then
 *      takes its fallback path -- which is how Quake II's frame timer and
 *      MicroPython's time.ticks_ms() ended up on the WALL clock, and how
 *      libstdc++'s configure probe was defeated, leaving std::chrono at
 *      1-second resolution.
 *   2. The macro is PRESENT for a group we do not implement. Then portable code
 *      calls a function that is not there -- a link error at best.
 *
 * So this group asserts both directions: every claimed option is positive AND
 * the functions it promises are callable; every deliberately-unclaimed option
 * is absent. It is a contract test, not a behaviour test -- it exists to fail
 * the moment someone adds a claim without the implementation, or deletes an
 * implementation without dropping the claim.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <unity_fixture.h>


TEST_GROUP(posix_options);


TEST_SETUP(posix_options)
{
}


TEST_TEAR_DOWN(posix_options)
{
}


/* A claimed option must be POSITIVE. -1 means "not supported" and 0 means "ask
 * sysconf at runtime", but a great deal of real code tests these with a bare
 * #ifdef, to which both read as supported -- so a claim has to be > 0. */
TEST(posix_options, claimed_options_are_positive)
{
#if !defined(_POSIX_MONOTONIC_CLOCK) || _POSIX_MONOTONIC_CLOCK <= 0
	TEST_FAIL_MESSAGE("_POSIX_MONOTONIC_CLOCK must be defined and positive");
#endif
#if !defined(_POSIX_TIMERS) || _POSIX_TIMERS <= 0
	TEST_FAIL_MESSAGE("_POSIX_TIMERS must be defined and positive");
#endif
#if !defined(_POSIX_CLOCK_SELECTION) || _POSIX_CLOCK_SELECTION <= 0
	TEST_FAIL_MESSAGE("_POSIX_CLOCK_SELECTION must be defined and positive");
#endif
#if !defined(_POSIX_THREADS) || _POSIX_THREADS <= 0
	TEST_FAIL_MESSAGE("_POSIX_THREADS must be defined and positive");
#endif
#if !defined(_POSIX_READER_WRITER_LOCKS) || _POSIX_READER_WRITER_LOCKS <= 0
	TEST_FAIL_MESSAGE("_POSIX_READER_WRITER_LOCKS must be defined and positive");
#endif
#if !defined(_POSIX_MEMORY_PROTECTION) || _POSIX_MEMORY_PROTECTION <= 0
	TEST_FAIL_MESSAGE("_POSIX_MEMORY_PROTECTION must be defined and positive");
#endif
#if !defined(_POSIX_FSYNC) || _POSIX_FSYNC <= 0
	TEST_FAIL_MESSAGE("_POSIX_FSYNC must be defined and positive");
#endif
	TEST_PASS();
}


/* The gate that portable code actually writes. This exact spelling is what
 * openssl's rand_unix.c and MicroPython's unix_mphal.c use to decide between
 * CLOCK_MONOTONIC and the wall clock. */
TEST(posix_options, the_portable_monotonic_gate_is_true)
{
#if _POSIX_TIMERS > 0 && defined(_POSIX_MONOTONIC_CLOCK)
	struct timespec tp;

	TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC, &tp));
#else
	TEST_FAIL_MESSAGE("the portable monotonic-clock gate is false: "
			"portable code will silently use the wall clock");
#endif
}


/* Every claim above has to be backed by a callable function, or the claim turns
 * a silent fallback into a link error for whoever believes it. */
TEST(posix_options, claims_are_backed_by_callable_functions)
{
	struct timespec res;
	pthread_rwlock_t rwlock;
	pthread_attr_t attr;
	size_t stacksize = 0;
	struct sched_param sp = { 0 };

	/* _POSIX_MONOTONIC_CLOCK / _POSIX_TIMERS (clock side) */
	TEST_ASSERT_EQUAL_INT(0, clock_getres(CLOCK_MONOTONIC, &res));

	/* _POSIX_CLOCK_SELECTION. POSIX ties this option to TWO things: the
	 * clock_nanosleep() family AND the ability to select the clock a condition
	 * variable times out against, so both are exercised. The second half is the
	 * one that matters in practice -- a portable timed wait that must not be
	 * disturbed when ntpclient steps the wall clock selects CLOCK_MONOTONIC
	 * here, and silently gets realtime semantics if the claim is hollow. */
	{
		struct timespec req = { .tv_sec = 0, .tv_nsec = 1000000 };
		TEST_ASSERT_EQUAL_INT(0, clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL));
	}
	{
		pthread_condattr_t cattr;
		pthread_cond_t cond;
		pthread_mutex_t mutex;
		clockid_t readback = (clockid_t)-1;
		struct timespec deadline;

		TEST_ASSERT_EQUAL_INT(0, pthread_condattr_init(&cattr));
		TEST_ASSERT_EQUAL_INT(0, pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC));
		TEST_ASSERT_EQUAL_INT(0, pthread_condattr_getclock(&cattr, &readback));
		TEST_ASSERT_EQUAL_INT(CLOCK_MONOTONIC, readback);

		/* And the selection has to reach the wait: time out against a deadline
		 * read from the SELECTED clock. If the cond were still on realtime this
		 * absolute monotonic deadline would be interpreted against a different
		 * epoch, and the wait would return immediately or hang rather than
		 * report ETIMEDOUT. */
		TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&cond, &cattr));
		TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&mutex, NULL));
		TEST_ASSERT_EQUAL_INT(0, clock_gettime(CLOCK_MONOTONIC, &deadline));
		deadline.tv_nsec += 20 * 1000 * 1000;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_nsec -= 1000000000L;
			deadline.tv_sec += 1;
		}
		TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&mutex));
		TEST_ASSERT_EQUAL_INT(ETIMEDOUT, pthread_cond_timedwait(&cond, &mutex, &deadline));
		TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&mutex));

		TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&mutex));
		TEST_ASSERT_EQUAL_INT(0, pthread_cond_destroy(&cond));
		TEST_ASSERT_EQUAL_INT(0, pthread_condattr_destroy(&cattr));
	}

	/* _POSIX_READER_WRITER_LOCKS */
	TEST_ASSERT_EQUAL_INT(0, pthread_rwlock_init(&rwlock, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_rwlock_rdlock(&rwlock));
	TEST_ASSERT_EQUAL_INT(0, pthread_rwlock_unlock(&rwlock));
	TEST_ASSERT_EQUAL_INT(0, pthread_rwlock_wrlock(&rwlock));
	TEST_ASSERT_EQUAL_INT(0, pthread_rwlock_unlock(&rwlock));
	TEST_ASSERT_EQUAL_INT(0, pthread_rwlock_destroy(&rwlock));

	/* _POSIX_THREADS + _POSIX_THREAD_ATTR_STACKSIZE */
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_setstacksize(&attr, 128 * 1024));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_getstacksize(&attr, &stacksize));
	TEST_ASSERT_EQUAL_UINT(128 * 1024, (unsigned)stacksize);
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_destroy(&attr));

	/* _POSIX_PRIORITY_SCHEDULING — the value is not asserted, only that the
	 * call exists and reports something sane for SCHED_RR. */
	TEST_ASSERT_TRUE(sched_get_priority_max(SCHED_RR) >= sched_get_priority_min(SCHED_RR));
	(void)sp;

	/* _POSIX_MEMORY_PROTECTION */
	{
		long pagesz = sysconf(_SC_PAGESIZE);
		void *p;

		TEST_ASSERT_TRUE(pagesz > 0);
		p = mmap(NULL, (size_t)pagesz, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		TEST_ASSERT_TRUE(p != MAP_FAILED);
		TEST_ASSERT_EQUAL_INT(0, mprotect(p, (size_t)pagesz, PROT_READ));
		TEST_ASSERT_EQUAL_INT(0, munmap(p, (size_t)pagesz));
	}
}


/* The other half of the contract: groups libphoenix does NOT implement must stay
 * unclaimed, so portable code keeps taking its fallback instead of calling a
 * function that is not there. Each of these is absent for a checked reason --
 * no sem_* and no <semaphore.h>, no pthread_barrier_*, no posix_spawn, no
 * shm_open, no sigqueue/sigwaitinfo, no mq_*, no clock_getcpuclockid. */
TEST(posix_options, unimplemented_groups_stay_unclaimed)
{
#ifdef _POSIX_SEMAPHORES
	TEST_FAIL_MESSAGE("_POSIX_SEMAPHORES claimed but libphoenix has no sem_*");
#endif
#ifdef _POSIX_BARRIERS
	TEST_FAIL_MESSAGE("_POSIX_BARRIERS claimed but libphoenix has no pthread_barrier_*");
#endif
#ifdef _POSIX_SPAWN
	TEST_FAIL_MESSAGE("_POSIX_SPAWN claimed but libphoenix has no posix_spawn");
#endif
#ifdef _POSIX_SHARED_MEMORY_OBJECTS
	TEST_FAIL_MESSAGE("_POSIX_SHARED_MEMORY_OBJECTS claimed but libphoenix has no shm_open");
#endif
#ifdef _POSIX_REALTIME_SIGNALS
	TEST_FAIL_MESSAGE("_POSIX_REALTIME_SIGNALS claimed but libphoenix has no sigqueue");
#endif
#ifdef _POSIX_MESSAGE_PASSING
	TEST_FAIL_MESSAGE("_POSIX_MESSAGE_PASSING claimed but libphoenix has no mq_*");
#endif
#ifdef _POSIX_CPUTIME
	TEST_FAIL_MESSAGE("_POSIX_CPUTIME claimed but libphoenix has no clock_getcpuclockid");
#endif
	TEST_PASS();
}


TEST_GROUP_RUNNER(posix_options)
{
	RUN_TEST_CASE(posix_options, claimed_options_are_positive);
	RUN_TEST_CASE(posix_options, the_portable_monotonic_gate_is_true);
	RUN_TEST_CASE(posix_options, claims_are_backed_by_callable_functions);
	RUN_TEST_CASE(posix_options, unimplemented_groups_stay_unclaimed);
}
