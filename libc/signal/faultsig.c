/*
 * Phoenix-RTOS
 *
 * libc-tests
 *
 * Signals the kernel raises on a thread's own behalf, and the edges of signal
 * delivery: a fault whose signal cannot be delivered must end the process
 * instead of re-executing the faulting instruction forever, sigreturn must not
 * let a crafted frame raise the thread's privilege, and a signal posted to a
 * thread that has not run yet must be handled before its first instruction.
 *
 * Every case that can hang or kill runs in a forked child, and the parent
 * waits with a deadline, so a kernel without the fix fails the case instead of
 * hanging the run.
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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <unity_fixture.h>


#define FAULTSIG_DEADLINE_MS 1000
#define FAULTSIG_POLL_MS     10


#if defined(__phoenix__) && defined(__aarch64__)
/* The exit of the plain (not SA_SIGINFO) signal trampoline: libphoenix's
 * syscall stub, declared in no header. The arguments are those the kernel put
 * in the signal frame: the mask to restore, the saved context, and the pc, sp
 * and psr to resume at. */
extern void sigreturn(unsigned int oldmask, void *ctx, uintptr_t pc, uintptr_t sp, uint64_t psr);
#define FAULTSIG_HAVE_SIGRETURN

#define PSR_EL0      0x000U
#define PSR_EL1H     0x005U
#define PSR_DAIF_ALL 0x3c0U
#endif


#if defined(__aarch64__)
/* The generic timer is readable at EL0 and needs no system call */
static uint64_t faultsig_counter(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}


static uint64_t faultsig_counterFreq(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}
#endif


static pid_t faultsig_fork(void)
{
	pid_t pid = fork();

	if (pid < 0) {
		if (errno == ENOSYS) {
			TEST_IGNORE_MESSAGE("fork syscall not supported");
		}
		FAIL("fork");
	}

	return pid;
}


/* Wait for pid for at most deadlineMs. On timeout the child is killed and
 * reaped, and -1 returned: the kernel let it run on instead of ending it. */
static int faultsig_wait(pid_t pid, int deadlineMs, int *status)
{
	int waited;
	pid_t ret;

	for (waited = 0; waited <= deadlineMs; waited += FAULTSIG_POLL_MS) {
		ret = waitpid(pid, status, WNOHANG);
		if (ret == pid) {
			return 0;
		}
		if (ret < 0) {
			FAIL("waitpid");
		}
		(void)usleep(FAULTSIG_POLL_MS * 1000);
	}

	(void)kill(pid, SIGKILL);
	(void)waitpid(pid, status, 0);

	return -1;
}


/* The child must be ended by sig within the deadline */
static void faultsig_expectDeathBy(pid_t pid, int sig)
{
	int status = 0;
	char msg[96];

	TEST_ASSERT_EQUAL_INT_MESSAGE(0, faultsig_wait(pid, FAULTSIG_DEADLINE_MS, &status),
			"the faulting child was still running after the deadline (fault loop)");

	(void)snprintf(msg, sizeof(msg), "child status 0x%x, expected death by signal %d", status, sig);
	TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(status), msg);
	TEST_ASSERT_EQUAL_INT_MESSAGE(sig, WTERMSIG(status), msg);
}


static void faultsig_nullStore(void)
{
	*(volatile int *)NULL = 1;
}


static void faultsig_block(int sig)
{
	sigset_t set;

	(void)sigemptyset(&set);
	(void)sigaddset(&set, sig);
	if (sigprocmask(SIG_BLOCK, &set, NULL) != 0) {
		_exit(2);
	}
}


TEST_GROUP(faultsig);


TEST_SETUP(faultsig)
{
}


TEST_TEAR_DOWN(faultsig)
{
	sigset_t set;
	TEST_ASSERT_EQUAL_INT(0, sigemptyset(&set));
	TEST_ASSERT_EQUAL_INT(0, sigprocmask(SIG_SETMASK, &set, NULL));

	for (int signo = 1; signo < USERSPACE_NSIG; ++signo) {
		if (signal_is_unblockable(signo)) {
			continue;
		}
		TEST_ASSERT_NOT_EQUAL(SIG_ERR, signal(signo, SIG_DFL));
	}
}


/* (a) A NULL store with SIGSEGV blocked ends the process by SIGSEGV */
TEST(faultsig, segv_blocked_kills)
{
	pid_t pid = faultsig_fork();

	if (pid == 0) {
		faultsig_block(SIGSEGV);
		faultsig_nullStore();
		_exit(3);
	}

	faultsig_expectDeathBy(pid, SIGSEGV);
}


/* (b) The same with SIGSEGV ignored */
TEST(faultsig, segv_ignored_kills)
{
	pid_t pid = faultsig_fork();

	if (pid == 0) {
		if (signal(SIGSEGV, SIG_IGN) == SIG_ERR) {
			_exit(2);
		}
		faultsig_nullStore();
		_exit(3);
	}

	faultsig_expectDeathBy(pid, SIGSEGV);
}


static void faultsig_faultingHandler(int sig)
{
	(void)sig;

	/* SIGSEGV is blocked while its own handler runs */
	faultsig_nullStore();
	_exit(4);
}


/* (a') The usual way to get there: a SIGSEGV handler that faults itself */
TEST(faultsig, segv_in_handler_kills)
{
	pid_t pid = faultsig_fork();

	if (pid == 0) {
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = faultsig_faultingHandler;
		(void)sigemptyset(&sa.sa_mask);
		if (sigaction(SIGSEGV, &sa, NULL) != 0) {
			_exit(2);
		}
		faultsig_nullStore();
		_exit(3);
	}

	faultsig_expectDeathBy(pid, SIGSEGV);
}


/* (a'') An undefined instruction with SIGILL blocked ends the process by SIGILL */
TEST(faultsig, sigill_blocked_kills)
{
#if defined(__aarch64__)
	pid_t pid = faultsig_fork();

	if (pid == 0) {
		faultsig_block(SIGILL);
		__asm__ volatile(".inst 0x00000000"); /* udf #0 */
		_exit(3);
	}

	faultsig_expectDeathBy(pid, SIGILL);
#else
	TEST_IGNORE_MESSAGE("no undefined instruction encoding for this architecture");
#endif
}


#ifdef FAULTSIG_HAVE_SIGRETURN

/* The context sigreturn restores the registers from. Larger than the kernel's
 * cpu_context_t; zeroed, so only pc, sp and psr matter. */
static unsigned char faultsig_ctx[4096] __attribute__((aligned(16)));
static volatile sig_atomic_t faultsig_ticked;
static pthread_t faultsig_target;


static void faultsig_tickHandler(int sig)
{
	(void)sig;
	faultsig_ticked = 1;
}


/* A signal to a thread running in user mode is delivered when its CPU next
 * enters the kernel, at the latest on the next timer interrupt. If the thread
 * runs with interrupts masked, that never happens. */
static void *faultsig_kicker(void *arg)
{
	(void)arg;

	for (;;) {
		(void)usleep(50 * 1000);
		(void)pthread_kill(faultsig_target, SIGUSR1);
	}

	return NULL;
}


/* Where the crafted sigreturn resumes. Exits 42 if a signal reached it while it
 * spun without system calls (it runs at EL0 with interrupts enabled), 43 if
 * none did for two seconds. */
static void faultsig_landing(void)
{
	const uint64_t start = faultsig_counter();
	const uint64_t limit = 2U * faultsig_counterFreq();

	faultsig_ticked = 0;
	while ((faultsig_ticked == 0) && ((faultsig_counter() - start) < limit)) {
	}

	_exit((faultsig_ticked != 0) ? 42 : 43);
}


static void faultsig_sigreturnChild(uint64_t psr)
{
	/* The landing function runs on this, inside the thread's own stack: the
	 * kernel pushes a signal frame only within the stack it knows of */
	unsigned char landingStack[16384] __attribute__((aligned(16)));
	struct sigaction sa;
	pthread_t kicker;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = faultsig_tickHandler;
	(void)sigemptyset(&sa.sa_mask);
	if (sigaction(SIGUSR1, &sa, NULL) != 0) {
		_exit(2);
	}

	faultsig_target = pthread_self();
	if (pthread_create(&kicker, NULL, faultsig_kicker, NULL) != 0) {
		_exit(3);
	}

	sigreturn(0U, faultsig_ctx, (uintptr_t)faultsig_landing, (uintptr_t)(landingStack + sizeof(landingStack)), psr);
	_exit(4);
}


static void faultsig_sigreturnCase(uint64_t psr, int deathAllowed)
{
	int status = 0;
	char msg[96];
	pid_t pid = faultsig_fork();

	if (pid == 0) {
		faultsig_sigreturnChild(psr);
	}

	TEST_ASSERT_EQUAL_INT_MESSAGE(0, faultsig_wait(pid, 5000, &status), "the child neither finished nor died");

	(void)snprintf(msg, sizeof(msg), "psr 0x%x: child status 0x%x", (unsigned int)psr, status);
	if ((deathAllowed != 0) && WIFSIGNALED(status)) {
		/* Killed for the crafted frame: acceptable */
		return;
	}
	TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(status), msg);
	/* 43: it ran in user mode with interrupts masked; anything else: it never
	 * reached the landing function in user mode */
	TEST_ASSERT_EQUAL_INT_MESSAGE(42, WEXITSTATUS(status), msg);
}

#endif


/* (c) A frame asking for EL1 (with interrupts masked) is refused or sanitised:
 * the process is killed, or it resumes at EL0 with interrupts enabled.
 * NOTE: on a kernel without the fix this can run the landing function at EL1. */
TEST(faultsig, sigreturn_el1_psr_cannot_escalate)
{
#ifdef FAULTSIG_HAVE_SIGRETURN
	faultsig_sigreturnCase(PSR_EL1H | PSR_DAIF_ALL, 1);
#else
	TEST_IGNORE_MESSAGE("crafted sigreturn frame not implemented for this target");
#endif
}


/* (c') A frame asking for EL0 with interrupts masked resumes with them enabled */
TEST(faultsig, sigreturn_daif_psr_sanitised)
{
#ifdef FAULTSIG_HAVE_SIGRETURN
	faultsig_sigreturnCase(PSR_EL0 | PSR_DAIF_ALL, 0);
#else
	TEST_IGNORE_MESSAGE("crafted sigreturn frame not implemented for this target");
#endif
}


#if defined(__aarch64__)

#define FIRSTENTRY_TRIALS 50

static volatile sig_atomic_t firstEntry_handled;
static volatile int firstEntry_spin;
static int firstEntry_posted;
static volatile uint64_t firstEntry_startedAt;
static volatile int firstEntry_seenPosted;
static volatile int firstEntry_seenHandled;


static void firstEntry_handler(int sig)
{
	(void)sig;
	firstEntry_handled = 1;
}


/* No system call before these loads: a kernel that delivers pending signals
 * only on the way out of an exception or system call would run the handler
 * after them. */
static void *firstEntry_thread(void *arg)
{
	(void)arg;

	firstEntry_startedAt = faultsig_counter();
	firstEntry_seenPosted = __atomic_load_n(&firstEntry_posted, __ATOMIC_SEQ_CST);
	firstEntry_seenHandled = firstEntry_handled;

	return NULL;
}


/* Keeps every CPU busy, so a new thread waits on the ready queue until the
 * next timer tick instead of starting at once on an idle CPU */
static void *firstEntry_spinner(void *arg)
{
	(void)arg;

	while (firstEntry_spin != 0) {
	}

	return NULL;
}

#endif


/* (d) A handled signal sent to a thread that has not run yet is handled before
 * the thread function's first instruction.
 *
 * A trial is conclusive only if the thread started well after pthread_kill()
 * returned: the start-up code before the thread function runs for a few
 * microseconds, so a millisecond of margin means the thread first entered user
 * mode with the signal already pending. */
TEST(faultsig, pending_at_first_user_entry)
{
#if defined(__aarch64__)
	struct sigaction sa;
	pthread_t spinners[8], tid;
	long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	const uint64_t margin = faultsig_counterFreq() / 1000U;
	uint64_t killedAt;
	int i, nspinners, created = 0, conclusive = 0, late = 0;
	char msg[96];

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = firstEntry_handler;
	TEST_ASSERT_EQUAL_INT(0, sigemptyset(&sa.sa_mask));
	TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &sa, NULL));

	nspinners = (ncpus < 1) ? 1 : (int)ncpus;
	if (nspinners > (int)(sizeof(spinners) / sizeof(spinners[0]))) {
		nspinners = (int)(sizeof(spinners) / sizeof(spinners[0]));
	}

	firstEntry_spin = 1;
	for (i = 0; i < nspinners; i++) {
		TEST_ASSERT_EQUAL_INT(0, pthread_create(&spinners[i], NULL, firstEntry_spinner, NULL));
	}

	for (i = 0; i < FIRSTENTRY_TRIALS; i++) {
		firstEntry_handled = 0;
		firstEntry_seenPosted = 0;
		firstEntry_seenHandled = 0;
		__atomic_store_n(&firstEntry_posted, 0, __ATOMIC_SEQ_CST);

		if (pthread_create(&tid, NULL, firstEntry_thread, NULL) != 0) {
			break;
		}
		created++;
		(void)pthread_kill(tid, SIGUSR1);
		killedAt = faultsig_counter();
		__atomic_store_n(&firstEntry_posted, 1, __ATOMIC_SEQ_CST);
		(void)pthread_join(tid, NULL);

		if ((firstEntry_seenPosted == 0) || ((int64_t)(firstEntry_startedAt - killedAt) < (int64_t)margin)) {
			continue; /* started too early to tell */
		}

		conclusive++;
		if (firstEntry_seenHandled == 0) {
			late++;
		}
	}

	firstEntry_spin = 0;
	for (int j = 0; j < nspinners; j++) {
		(void)pthread_join(spinners[j], NULL);
	}

	TEST_ASSERT_EQUAL_INT_MESSAGE(FIRSTENTRY_TRIALS, created, "pthread_create failed");
	(void)snprintf(msg, sizeof(msg), "%d of %d conclusive trials ran the thread function before the handler", late, conclusive);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, late, msg);
	if (conclusive == 0) {
		TEST_IGNORE_MESSAGE("no thread started late enough after pthread_kill() to tell");
	}
#else
	TEST_IGNORE_MESSAGE("needs a user-readable counter (aarch64 only)");
#endif
}


TEST_GROUP_RUNNER(faultsig)
{
	RUN_TEST_CASE(faultsig, segv_blocked_kills);
	RUN_TEST_CASE(faultsig, segv_ignored_kills);
	RUN_TEST_CASE(faultsig, segv_in_handler_kills);
	RUN_TEST_CASE(faultsig, sigill_blocked_kills);
	RUN_TEST_CASE(faultsig, sigreturn_el1_psr_cannot_escalate);
	RUN_TEST_CASE(faultsig, sigreturn_daif_psr_sanitised);
	RUN_TEST_CASE(faultsig, pending_at_first_user_entry);
}
