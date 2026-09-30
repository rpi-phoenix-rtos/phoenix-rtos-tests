/*
 * Phoenix-RTOS
 *
 *    Stack protector runtime (GCC -fstack-protector ABI)
 *    TESTED:
 *    - __stack_chk_guard (seeded before main(), low byte zero, kept across fork())
 *    - __stack_chk_fail() (message on stderr, then abort())
 *
 * This file is built with -fstack-protector-strong (see ../Makefile), so every
 * function here with a local array checks the guard on return. Without the libc
 * runtime the program does not link: __stack_chk_guard and __stack_chk_fail are
 * undefined.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include <unity_fixture.h>


#define SP_BUF_SIZE 16

/* Enough to run over the guard slot above the buffer, not so much that the
 * overwrite reaches past the caller's frame. */
#define SP_OVERFLOW (SP_BUF_SIZE + 16)

/* The static value the guard holds before libc seeds it (libphoenix misc/stack_chk.c) */
#define SP_UNSEEDED_GUARD ((uintptr_t)0xe3a8d5c60f1b7400ULL)


#ifdef __phoenix__
/* glibc keeps its canary in the thread control block; libphoenix uses the global
 * GCC reads on every Phoenix target */
extern uintptr_t __stack_chk_guard;
#endif


/* Read through a volatile so the compiler cannot see the length and fold, warn
 * about or bounds-check the write */
static volatile size_t sp_len;


/* Protected (a local array): writes sp_len bytes into a 16-byte buffer. The
 * volatile store cannot become a memset(), so no fortified libc routine gets to
 * catch the overflow first; only the guard check on return can. */
__attribute__((noinline)) static int sp_fill(void)
{
	char buf[SP_BUF_SIZE];
	volatile char *p = buf;
	size_t i, len = sp_len;

	for (i = 0; i < len; i++) {
		p[i] = 'A';
	}

	return p[0] + p[SP_BUF_SIZE - 1];
}


/* Protected, and forks from INSIDE its frame: the child returns through a frame
 * whose guard copy was stored by the parent. Returns 0 in the child. */
__attribute__((noinline)) static pid_t sp_forkInFrame(void)
{
	char buf[SP_BUF_SIZE];
	volatile char *p = buf;
	pid_t pid;

	p[0] = 'x';
	pid = fork();
	p[SP_BUF_SIZE - 1] = (pid == 0) ? 'c' : 'p';

	return (p[0] == 'x') ? pid : -1;
}


static pid_t sp_fork(void)
{
	pid_t pid = fork();

	if (pid < 0) {
		if (errno == ENOSYS) {
			TEST_IGNORE_MESSAGE("fork syscall not supported");
		}
		TEST_FAIL_MESSAGE("fork");
	}

	return pid;
}


static int sp_wait(pid_t pid)
{
	int status = 0;

	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));

	return status;
}


TEST_GROUP(stack_protector);


TEST_SETUP(stack_protector)
{
}


TEST_TEAR_DOWN(stack_protector)
{
}


TEST(stack_protector, guard_seeded)
{
#ifdef __phoenix__
	uintptr_t guard = __stack_chk_guard;

	TEST_ASSERT_NOT_EQUAL(0, guard);
	/* string-overflow terminator, as glibc */
	TEST_ASSERT_EQUAL_HEX(0, guard & 0xff);
	/* seeded at start-up, not left at its link-time value */
	TEST_ASSERT_NOT_EQUAL(SP_UNSEEDED_GUARD, guard);
#else
	TEST_IGNORE_MESSAGE("the guard is not a global on this libc");
#endif
}


TEST(stack_protector, in_bounds_returns)
{
	sp_len = SP_BUF_SIZE;
	TEST_ASSERT_EQUAL_INT('A' + 'A', sp_fill());
}


TEST(stack_protector, fork_keeps_guard)
{
#ifdef __phoenix__
	uintptr_t parent = __stack_chk_guard;
#endif
	pid_t pid = sp_forkInFrame();
	int status;

	if (pid == 0) {
		/* reached only if returning from sp_forkInFrame() passed its check */
#ifdef __phoenix__
		_exit((__stack_chk_guard == parent) ? 0 : 1);
#else
		_exit(0);
#endif
	}
	TEST_ASSERT_GREATER_THAN_INT(0, pid);

	status = sp_wait(pid);
	TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(status), "child did not return through the parent's frame");
	TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}


TEST(stack_protector, overflow_aborts)
{
	char msg[256];
	size_t len = 0;
	ssize_t n;
	int fds[2], status;
	pid_t pid;

	TEST_ASSERT_EQUAL_INT(0, pipe(fds));

	pid = sp_fork();
	if (pid == 0) {
		close(fds[0]);
		if (dup2(fds[1], STDERR_FILENO) < 0) {
			_exit(2);
		}
		sp_len = SP_OVERFLOW;
		(void)sp_fill();
		/* the overflow went undetected */
		_exit(3);
	}

	close(fds[1]);
	while (len < sizeof(msg) - 1) {
		n = read(fds[0], msg + len, sizeof(msg) - 1 - len);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break;
		}
		len += (size_t)n;
	}
	msg[len] = '\0';
	close(fds[0]);

	status = sp_wait(pid);
	TEST_ASSERT_FALSE_MESSAGE(WIFEXITED(status) && (WEXITSTATUS(status) == 3), "overflow not detected");
	TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(status), "child not killed by a signal");
	TEST_ASSERT_EQUAL_INT(SIGABRT, WTERMSIG(status));
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(msg, "stack smashing detected"), msg);
#ifdef __phoenix__
	/* libphoenix also names the program (glibc stopped doing so in 2.26) */
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(msg, getprogname()), msg);
#endif
}


TEST_GROUP_RUNNER(stack_protector)
{
	RUN_TEST_CASE(stack_protector, guard_seeded);
	RUN_TEST_CASE(stack_protector, in_bounds_returns);
	RUN_TEST_CASE(stack_protector, fork_keeps_guard);
	RUN_TEST_CASE(stack_protector, overflow_aborts);
}
