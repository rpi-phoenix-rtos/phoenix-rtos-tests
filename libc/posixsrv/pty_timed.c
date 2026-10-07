/*
 * Phoenix-RTOS
 *
 * libc/posixsrv
 *
 * tests for pseudo-terminal reads that wait with a timeout (VMIN/VTIME)
 *
 * In non-canonical mode a slave read with VTIME > 0 waits in posixsrv with a
 * timeout. Such a read ends either by its timeout or by data the master
 * writes; the cases below make both happen close together, many times, on
 * several ptys at once, and close a pty under a waiting read. A posixsrv that
 * loses the race frees the request twice, deadlocks, or never answers: every
 * case runs under a watchdog that ends the process if it hangs.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

/* grantpt(), unlockpt() and ptsname() are XSI */
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "unity_fixture.h"


#define WATCHDOG_S 60
#define RACE_PTYS  4
#define RACE_ITER  100


typedef struct {
	int master, slave;
} pty_t;


static struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_t thread;
	int armed;
	const char *name;
} watchdog = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };


static void *watchdog_thread(void *arg)
{
	struct timespec deadline;
	int err = 0;

	(void)arg;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += WATCHDOG_S;

	pthread_mutex_lock(&watchdog.lock);
	while (watchdog.armed && err != ETIMEDOUT) {
		err = pthread_cond_timedwait(&watchdog.cond, &watchdog.lock, &deadline);
	}
	if (watchdog.armed) {
		/* a read posixsrv never answers cannot be interrupted from here */
		fprintf(stderr, "\npty_timed %s: HUNG for %d s -- a pty read was never answered\n", watchdog.name, WATCHDOG_S);
		_exit(EXIT_FAILURE);
	}
	pthread_mutex_unlock(&watchdog.lock);
	return NULL;
}


static int pty_open(pty_t *p, int vmin, int vtime)
{
	struct termios tio;
	char *name;

	p->slave = -1;
	p->master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
	if (p->master < 0 || grantpt(p->master) < 0 || unlockpt(p->master) < 0 || (name = ptsname(p->master)) == NULL) {
		return -1;
	}

	p->slave = open(name, O_RDWR | O_NOCTTY);
	if (p->slave < 0 || tcgetattr(p->slave, &tio) < 0) {
		return -1;
	}

	tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
	tio.c_oflag &= ~OPOST;
	tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	tio.c_cc[VMIN] = vmin;
	tio.c_cc[VTIME] = vtime;
	return tcsetattr(p->slave, TCSANOW, &tio);
}


static void pty_close(pty_t *p)
{
	if (p->slave >= 0) {
		close(p->slave);
		p->slave = -1;
	}
	if (p->master >= 0) {
		close(p->master);
		p->master = -1;
	}
}


static void sleep_us(long us)
{
	struct timespec ts = { .tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000 };

	while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
	}
}


static long elapsed_ms(const struct timespec *t0)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}


static void *slave_read(void *arg)
{
	pty_t *p = arg;
	char buf[16];

	return (void *)(long)read(p->slave, buf, sizeof(buf));
}


TEST_GROUP(pty_timed);


TEST_SETUP(pty_timed)
{
	watchdog.armed = 1;
	watchdog.name = Unity.CurrentTestName;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&watchdog.thread, NULL, watchdog_thread, NULL));
}


TEST_TEAR_DOWN(pty_timed)
{
	pthread_mutex_lock(&watchdog.lock);
	watchdog.armed = 0;
	pthread_cond_signal(&watchdog.cond);
	pthread_mutex_unlock(&watchdog.lock);
	pthread_join(watchdog.thread, NULL);
}


/* A 100 ms read started while a 2 s one waits must not wait for the 2 s one. */
TEST(pty_timed, short_after_long)
{
	pty_t a = { -1, -1 }, b = { -1, -1 };
	pthread_t th;
	struct timespec t0;
	char buf[16];
	void *ret;
	long ms;

	if (pty_open(&a, 0, 20) < 0 || pty_open(&b, 0, 1) < 0) {
		pty_close(&a);
		pty_close(&b);
		TEST_IGNORE_MESSAGE("no /dev/ptmx");
	}

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, slave_read, &a));
	sleep_us(50 * 1000);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	TEST_ASSERT_EQUAL_INT(0, read(b.slave, buf, sizeof(buf)));
	ms = elapsed_ms(&t0);

	pthread_join(th, &ret);
	pty_close(&a);
	pty_close(&b);

	TEST_ASSERT_EQUAL_INT(0, (long)ret);
	/* VTIME=1 is 100 ms; the 2 s read expires at ~1950 ms */
	TEST_ASSERT_LESS_THAN_INT(1000, ms);
}


/* Reads armed together, likely due within the same clock tick, must all expire. */
TEST(pty_timed, same_deadline)
{
	pty_t p[4];
	pthread_t th[4];
	void *ret;
	int i, round;

	for (i = 0; i < 4; ++i) {
		if (pty_open(&p[i], 0, 1) < 0) {
			while (i >= 0) {
				pty_close(&p[i--]);
			}
			TEST_IGNORE_MESSAGE("no /dev/ptmx");
		}
	}

	for (round = 0; round < 10; ++round) {
		for (i = 0; i < 4; ++i) {
			TEST_ASSERT_EQUAL_INT(0, pthread_create(&th[i], NULL, slave_read, &p[i]));
		}
		for (i = 0; i < 4; ++i) {
			pthread_join(th[i], &ret);
			TEST_ASSERT_EQUAL_INT(0, (long)ret);
		}
	}

	for (i = 0; i < 4; ++i) {
		pty_close(&p[i]);
	}
}


/* VMIN=5, VTIME=1 with 2 bytes there: the inter-byte timer ends the read with those 2. */
TEST(pty_timed, vmin_partial)
{
	pty_t p;
	char buf[16];
	int n;

	if (pty_open(&p, 5, 1) < 0) {
		pty_close(&p);
		TEST_IGNORE_MESSAGE("no /dev/ptmx");
	}

	TEST_ASSERT_EQUAL_INT(2, write(p.master, "ab", 2));
	n = read(p.slave, buf, sizeof(buf));
	pty_close(&p);

	TEST_ASSERT_EQUAL_INT(2, n);
	TEST_ASSERT_EQUAL_MEMORY("ab", buf, 2);
}


typedef struct {
	pty_t p;
	long written, got;
	int done, err;
	unsigned seed;
} race_t;


static void *race_writer(void *arg)
{
	race_t *r = arg;
	int i, n;

	for (i = 0; i < RACE_ITER; ++i) {
		/* around the 100 ms deadline of the read in progress */
		sleep_us(80 * 1000 + rand_r(&r->seed) % (40 * 1000));
		n = 1 + rand_r(&r->seed) % 4;
		if (write(r->p.master, "wxyz", n) == n) {
			r->written += n;
		}
	}
	__atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
	return NULL;
}


static void *race_reader(void *arg)
{
	race_t *r = arg;
	char buf[64];
	int n, idle = 0;

	for (;;) {
		n = read(r->p.slave, buf, sizeof(buf));
		if (n < 0) {
			r->err = errno;
			return NULL;
		}
		r->got += n;
		idle = (n == 0) ? idle + 1 : 0;
		/* the writer is done: drain, then a few empty timeouts in a row */
		if (idle > 3 && __atomic_load_n(&r->done, __ATOMIC_ACQUIRE) && (r->got == r->written || idle > 30)) {
			return NULL;
		}
	}
}


/* VTIME=1 reads ended by their timeout or by a write landing near it: no byte lost or duplicated. */
TEST(pty_timed, write_vs_timeout)
{
	race_t r[RACE_PTYS];
	pthread_t tr[RACE_PTYS], tw[RACE_PTYS];
	int i;

	memset(r, 0, sizeof(r));
	for (i = 0; i < RACE_PTYS; ++i) {
		r[i].seed = 1 + i;
		if (pty_open(&r[i].p, 0, 1) < 0) {
			while (i >= 0) {
				pty_close(&r[i--].p);
			}
			TEST_IGNORE_MESSAGE("no /dev/ptmx");
		}
	}

	for (i = 0; i < RACE_PTYS; ++i) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&tr[i], NULL, race_reader, &r[i]));
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&tw[i], NULL, race_writer, &r[i]));
	}
	for (i = 0; i < RACE_PTYS; ++i) {
		pthread_join(tw[i], NULL);
		pthread_join(tr[i], NULL);
	}
	for (i = 0; i < RACE_PTYS; ++i) {
		pty_close(&r[i].p);
	}

	for (i = 0; i < RACE_PTYS; ++i) {
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, r[i].err, strerror(r[i].err));
		TEST_ASSERT_EQUAL_INT(r[i].written, r[i].got);
	}
}


/*
 * The master closes while a VTIME read waits on the slave, at points across
 * its 100 ms: the read must be answered, and its timeout must not fire later
 * on the freed request or pty. A new pty afterwards shows posixsrv survived.
 */
TEST(pty_timed, close_under_read)
{
	pty_t p, q;
	pthread_t th;
	int i;

	for (i = 0; i < 40; ++i) {
		if (pty_open(&p, 0, 1) < 0) {
			pty_close(&p);
			TEST_IGNORE_MESSAGE("no /dev/ptmx");
		}
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, slave_read, &p));
		sleep_us((i % 20) * 5 * 1000 + 2000); /* 2..97 ms into the 100 ms */
		close(p.master);
		p.master = -1;
		pthread_join(th, NULL);
		pty_close(&p);
	}

	sleep_us(300 * 1000); /* any timeout left behind has fired by now */
	TEST_ASSERT_EQUAL_INT(0, pty_open(&q, 0, 1));
	pty_close(&q);
}


TEST_GROUP_RUNNER(pty_timed)
{
	RUN_TEST_CASE(pty_timed, short_after_long);
	RUN_TEST_CASE(pty_timed, same_deadline);
	RUN_TEST_CASE(pty_timed, vmin_partial);
	RUN_TEST_CASE(pty_timed, write_vs_timeout);
	RUN_TEST_CASE(pty_timed, close_under_read);
}
