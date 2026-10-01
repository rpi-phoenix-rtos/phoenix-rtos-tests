/*
 * Phoenix-RTOS
 *
 * libc-tests
 *
 * sigaltstack() and SA_ONSTACK: handlers that run on an alternate signal
 * stack, which is what lets a program survive the SIGSEGV of a stack overflow.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "sig_internal.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

#include <unity_fixture.h>


#define ALTSTACK_SIZE SIGSTKSZ


static unsigned char altstack_buf[ALTSTACK_SIZE] __attribute__((aligned(16)));

static volatile int altstack_calls;
static volatile uintptr_t altstack_local;  /* a local of the handler: where it ran */
static volatile int altstack_queryErr;     /* sigaltstack() inside the handler */
static volatile int altstack_queryFlags;
static volatile int altstack_setErrno;     /* changing it inside the handler */
static volatile int altstack_ucFlags = -1; /* uc_stack, SA_SIGINFO only */
static void *volatile altstack_ucSp;
static volatile size_t altstack_ucSize;


static int altstack_set(void *sp, size_t size, int flags)
{
	stack_t ss;

	memset(&ss, 0, sizeof(ss));
	ss.ss_sp = sp;
	ss.ss_size = size;
	ss.ss_flags = flags;

	return sigaltstack(&ss, NULL);
}


static int altstack_on(uintptr_t addr)
{
	return ((addr > (uintptr_t)altstack_buf) && (addr <= (uintptr_t)altstack_buf + sizeof(altstack_buf))) ? 1 : 0;
}


static void altstack_handler(int sig)
{
	volatile int here;
	stack_t oss, ss;

	(void)sig;
	altstack_calls++;
	altstack_local = (uintptr_t)&here;

	altstack_queryErr = sigaltstack(NULL, &oss);
	altstack_queryFlags = oss.ss_flags;

	/* POSIX: the stack in use cannot be changed */
	memset(&ss, 0, sizeof(ss));
	ss.ss_flags = SS_DISABLE;
	altstack_setErrno = (sigaltstack(&ss, NULL) == 0) ? 0 : errno;
}


static void altstack_infoHandler(int sig, siginfo_t *info, void *ucv)
{
	altstack_handler(sig);

	/* As in siginfo.c, trust the context only if siginfo is real */
	if ((info != NULL) && (ucv != NULL) && (info->si_signo == sig)) {
		const ucontext_t *uc = ucv;
		altstack_ucFlags = uc->uc_stack.ss_flags;
		altstack_ucSp = uc->uc_stack.ss_sp;
		altstack_ucSize = uc->uc_stack.ss_size;
	}
}


TEST_GROUP(sigaltstack);


TEST_SETUP(sigaltstack)
{
	altstack_calls = 0;
	altstack_local = 0;
	altstack_queryErr = -1;
	altstack_queryFlags = 0;
	altstack_setErrno = -1;
	altstack_ucFlags = -1;
	altstack_ucSp = NULL;
	altstack_ucSize = 0;
}


TEST_TEAR_DOWN(sigaltstack)
{
	(void)altstack_set(NULL, 0, SS_DISABLE);
	(void)signal(SIGUSR1, SIG_DFL);
}


TEST(sigaltstack, none_by_default)
{
	stack_t oss;

	memset(&oss, 0, sizeof(oss));
	TEST_ASSERT_EQUAL_INT(0, sigaltstack(NULL, &oss));
	TEST_ASSERT_TRUE((oss.ss_flags & SS_DISABLE) != 0);
}


TEST(sigaltstack, set_and_query)
{
	stack_t oss;

	TEST_ASSERT_EQUAL_INT(0, altstack_set(altstack_buf, sizeof(altstack_buf), 0));

	memset(&oss, 0, sizeof(oss));
	TEST_ASSERT_EQUAL_INT(0, sigaltstack(NULL, &oss));
	TEST_ASSERT_EQUAL_PTR(altstack_buf, oss.ss_sp);
	TEST_ASSERT_EQUAL_UINT(sizeof(altstack_buf), oss.ss_size);
	TEST_ASSERT_EQUAL_INT(0, oss.ss_flags);

	TEST_ASSERT_EQUAL_INT(0, altstack_set(NULL, 0, SS_DISABLE));
	TEST_ASSERT_EQUAL_INT(0, sigaltstack(NULL, &oss));
	TEST_ASSERT_TRUE((oss.ss_flags & SS_DISABLE) != 0);
}


TEST(sigaltstack, invalid_args)
{
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, altstack_set(altstack_buf, MINSIGSTKSZ - 1, 0));
	TEST_ASSERT_EQUAL_INT(ENOMEM, errno);

	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, altstack_set(altstack_buf, sizeof(altstack_buf), 0x100));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}


/* SA_ONSTACK runs the handler there; there it reports SS_ONSTACK and cannot
 * change the stack */
TEST(sigaltstack, onstack_handler_runs_there)
{
	struct sigaction sa;
	volatile int here;

	TEST_ASSERT_EQUAL_INT(0, altstack_set(altstack_buf, sizeof(altstack_buf), 0));

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = altstack_handler;
	sa.sa_flags = SA_ONSTACK;
	TEST_ASSERT_EQUAL_INT(0, sigemptyset(&sa.sa_mask));
	TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &sa, NULL));

	TEST_ASSERT_EQUAL_INT(0, raise(SIGUSR1));

	TEST_ASSERT_EQUAL_INT(1, altstack_calls);
	TEST_ASSERT_TRUE_MESSAGE(altstack_on(altstack_local), "the handler did not run on the alternate stack");
	TEST_ASSERT_FALSE(altstack_on((uintptr_t)&here));
	TEST_ASSERT_EQUAL_INT(0, altstack_queryErr);
	TEST_ASSERT_EQUAL_INT(SS_ONSTACK, altstack_queryFlags);
	TEST_ASSERT_EQUAL_INT(EPERM, altstack_setErrno);
}


/* Without SA_ONSTACK the handler stays on the thread stack */
TEST(sigaltstack, plain_handler_stays_off)
{
	TEST_ASSERT_EQUAL_INT(0, altstack_set(altstack_buf, sizeof(altstack_buf), 0));
	TEST_ASSERT_NOT_EQUAL(SIG_ERR, signal(SIGUSR1, altstack_handler));

	TEST_ASSERT_EQUAL_INT(0, raise(SIGUSR1));

	TEST_ASSERT_EQUAL_INT(1, altstack_calls);
	TEST_ASSERT_FALSE(altstack_on(altstack_local));
	TEST_ASSERT_EQUAL_INT(0, altstack_queryFlags & SS_ONSTACK);
}


/* An SA_SIGINFO handler finds the alternate stack in uc_stack */
TEST(sigaltstack, uc_stack_reported)
{
	struct sigaction sa;

	TEST_ASSERT_EQUAL_INT(0, altstack_set(altstack_buf, sizeof(altstack_buf), 0));

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = altstack_infoHandler;
	sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
	TEST_ASSERT_EQUAL_INT(0, sigemptyset(&sa.sa_mask));
	TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &sa, NULL));

	TEST_ASSERT_EQUAL_INT(0, raise(SIGUSR1));

	TEST_ASSERT_TRUE(altstack_on(altstack_local));
	/* The interrupted code was not on it */
	TEST_ASSERT_EQUAL_INT(0, altstack_ucFlags);
	TEST_ASSERT_EQUAL_PTR(altstack_buf, altstack_ucSp);
	TEST_ASSERT_EQUAL_UINT(sizeof(altstack_buf), altstack_ucSize);
}


/* The point of it all: a thread that overflows its stack takes the SIGSEGV on
 * the alternate stack. Run in a child, which a missing feature kills. */

static void altstack_overflowHandler(int sig)
{
	(void)sig;
	_exit(42);
}


static __attribute__((noinline)) int altstack_recurse(volatile int depth)
{
	volatile char pad[256];

	/* Never true: only there to show the compiler an end */
	if (depth < 0) {
		return 0;
	}

	pad[0] = (char)depth;
	/* Not a tail call: the result is used after it */
	return altstack_recurse(depth + 1) + pad[0];
}


static void *altstack_overflowThread(void *arg)
{
	static unsigned char tstack[ALTSTACK_SIZE] __attribute__((aligned(16)));
	struct sigaction sa;

	(void)arg;

	if (altstack_set(tstack, sizeof(tstack), 0) != 0) {
		_exit(1);
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = altstack_overflowHandler;
	sa.sa_flags = SA_ONSTACK;
	(void)sigemptyset(&sa.sa_mask);
	if (sigaction(SIGSEGV, &sa, NULL) != 0) {
		_exit(2);
	}

	(void)altstack_recurse(0);
	_exit(3);

	return NULL;
}


TEST(sigaltstack, stack_overflow_caught)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0) {
		if (errno == ENOSYS) {
			TEST_IGNORE_MESSAGE("fork syscall not supported");
		}
		FAIL("fork");
	}

	if (pid == 0) {
		pthread_t tid;
		pthread_attr_t attr;

		/* A guarded thread stack of its own, so the overflow faults */
		(void)pthread_attr_init(&attr);
		(void)pthread_attr_setstacksize(&attr, 64U * 1024U);
		if (pthread_create(&tid, &attr, altstack_overflowThread, NULL) != 0) {
			_exit(4);
		}
		(void)pthread_join(tid, NULL);
		_exit(5);
	}

	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(status), "the overflow killed the child");
	TEST_ASSERT_EQUAL_INT(42, WEXITSTATUS(status));
}


TEST_GROUP_RUNNER(sigaltstack)
{
	RUN_TEST_CASE(sigaltstack, none_by_default);
	RUN_TEST_CASE(sigaltstack, set_and_query);
	RUN_TEST_CASE(sigaltstack, invalid_args);
	RUN_TEST_CASE(sigaltstack, onstack_handler_runs_there);
	RUN_TEST_CASE(sigaltstack, plain_handler_stays_off);
	RUN_TEST_CASE(sigaltstack, uc_stack_reported);
	RUN_TEST_CASE(sigaltstack, stack_overflow_caught);
}
