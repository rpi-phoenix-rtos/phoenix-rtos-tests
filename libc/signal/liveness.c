/*
 * Phoenix-RTOS
 *
 * libc-tests
 *
 * kill(pid, 0) — the process-liveness probe.
 *
 * Signal 0 is the one kill() must NOT deliver: POSIX says it performs error
 * checking only. Every program that asks "is this pid still alive?" uses it,
 * and one of them ships on this board — Xorg's LockServer() reads the pid out
 * of /tmp/.X0-lock and calls kill(pid, 0) to decide whether the lock is stale.
 * If that probe answers wrongly, a dead X server's lock file is never cleaned
 * up and the server refuses to start (#66). The existing signal tests only ever
 * send real signals to self, so this path had no coverage at all.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "sig_internal.h"

#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/wait.h>

#include <unity_fixture.h>


TEST_GROUP(liveness);


static volatile sig_atomic_t liveness_delivered;


static void liveness_catchAll(int sig)
{
	(void)sig;
	liveness_delivered = 1;
}


TEST_SETUP(liveness)
{
	liveness_delivered = 0;
}


TEST_TEAR_DOWN(liveness)
{
	for (int signo = 1; signo < USERSPACE_NSIG; ++signo) {
		signal(signo, SIG_DFL);
	}
}


/* A live process answers 0, and nothing is delivered. */
TEST(liveness, probe_self_alive)
{
	for (int signo = 1; signo < USERSPACE_NSIG; ++signo) {
		/* SIGKILL/SIGSTOP cannot be caught; skip rather than fail on them. */
		if ((signo != SIGKILL) && (signo != SIGSTOP)) {
			(void)signal(signo, liveness_catchAll);
		}
	}

	errno = 0;
	TEST_ASSERT_EQUAL_INT(0, kill(getpid(), 0));
	TEST_ASSERT_EQUAL_INT(0, liveness_delivered);
}


/* A pid that has exited AND been reaped must answer -1/ESRCH. This is the one
 * Xorg depends on: without it a stale lock file looks like a running server. */
TEST(liveness, probe_reaped_child_is_esrch)
{
	pid_t child = fork();
	TEST_ASSERT_NOT_EQUAL_INT(-1, child);

	if (child == 0) {
		_exit(0);
	}

	int status = 0;
	TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));

	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, kill(child, 0));
	TEST_ASSERT_EQUAL_INT(ESRCH, errno);
}


/* A pid that was never allocated must answer -1/ESRCH too. */
TEST(liveness, probe_absurd_pid_is_esrch)
{
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, kill(0x7ffffff0, 0));
	TEST_ASSERT_EQUAL_INT(ESRCH, errno);
}


/* An out-of-range signal number is EINVAL, not ESRCH — the caller has to be
 * able to tell "no such process" from "you asked for nonsense". */
TEST(liveness, bad_signo_is_einval)
{
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, kill(getpid(), -1));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);

	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, kill(getpid(), NSIG));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}


/* Probing a live CHILD answers 0 without disturbing it: it must still be
 * waitable afterwards with the exit status it chose. */
TEST(liveness, probe_live_child_does_not_disturb_it)
{
	int pipefd[2];
	TEST_ASSERT_EQUAL_INT(0, pipe(pipefd));

	pid_t child = fork();
	TEST_ASSERT_NOT_EQUAL_INT(-1, child);

	if (child == 0) {
		char c;
		(void)close(pipefd[1]);
		/* Block until the parent has probed us, then exit with a known code. */
		(void)read(pipefd[0], &c, 1);
		(void)close(pipefd[0]);
		_exit(42);
	}

	(void)close(pipefd[0]);

	errno = 0;
	TEST_ASSERT_EQUAL_INT(0, kill(child, 0));

	TEST_ASSERT_EQUAL_INT(1, write(pipefd[1], "x", 1));
	(void)close(pipefd[1]);

	int status = 0;
	TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
	TEST_ASSERT_TRUE(WIFEXITED(status));
	TEST_ASSERT_EQUAL_INT(42, WEXITSTATUS(status));
}


TEST_GROUP_RUNNER(liveness)
{
	RUN_TEST_CASE(liveness, probe_self_alive);
	RUN_TEST_CASE(liveness, probe_reaped_child_is_esrch);
	RUN_TEST_CASE(liveness, probe_absurd_pid_is_esrch);
	RUN_TEST_CASE(liveness, bad_signo_is_einval);
	RUN_TEST_CASE(liveness, probe_live_child_does_not_disturb_it);
}
