/*
 * Phoenix-RTOS
 *
 * libc-tests
 *
 * SA_SIGINFO handlers: the siginfo_t and ucontext_t they receive, and the
 * registers and signal mask they change in the ucontext taking effect on return.
 *
 * Runtimes depend on all of it: a garbage collector suspends a thread with
 * pthread_kill() and reads its registers from the ucontext, a JIT or a
 * bounds-checking runtime recovers from a fault by moving the pc. A handler on a
 * system without the support still runs, but gets a junk siginfo pointer, so
 * every handler here copies what it was given and checks si_signo before it
 * trusts anything else, and leaves through siglongjmp() when it cannot.
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
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

#include <unity_fixture.h>


#define SIGINFO_SENTINEL 0x5a5a1234UL


/* What the last handler invocation saw. Written in the handler, read after it. */
static struct {
	volatile int calls;
	volatile int valid; /* si_signo matched: info (and so uc) can be trusted */
	siginfo_t si;
	const void *volatile info;
	const void *volatile uc;
	volatile uintptr_t pc;
	volatile uintptr_t sp;
	volatile uintptr_t faultAddress;
	volatile int onThread;
} siginfo_rec;


static volatile int siginfo_skipFault; /* SIGSEGV handler: resume past the fault */
static pthread_t siginfo_target;


static void siginfo_copy(int sig, const siginfo_t *info, const void *ucv)
{
	siginfo_rec.calls++;
	siginfo_rec.info = info;
	siginfo_rec.uc = ucv;
	siginfo_rec.onThread = pthread_equal(pthread_self(), siginfo_target);

	/* Without SA_SIGINFO support info is junk but readable (it is the handler's
	 * own address on old Phoenix), so copying it is safe and si_signo tells. */
	if (info != NULL) {
		memcpy(&siginfo_rec.si, info, sizeof(siginfo_rec.si));
	}
	siginfo_rec.valid = ((info != NULL) && (ucv != NULL) && (siginfo_rec.si.si_signo == sig)) ? 1 : 0;

#if defined(__aarch64__)
	if (siginfo_rec.valid != 0) {
		const ucontext_t *uc = ucv;
		siginfo_rec.pc = (uintptr_t)uc->uc_mcontext.pc;
		siginfo_rec.sp = (uintptr_t)uc->uc_mcontext.sp;
		siginfo_rec.faultAddress = (uintptr_t)uc->uc_mcontext.fault_address;
	}
#endif
}


static void siginfo_handler(int sig, siginfo_t *info, void *ucv)
{
	siginfo_copy(sig, info, ucv);
}


static void siginfo_install(int sig, void (*handler)(int, siginfo_t *, void *), int flags)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | flags;
	TEST_ASSERT_EQUAL_INT(0, sigemptyset(&sa.sa_mask));
	TEST_ASSERT_EQUAL_INT(0, sigaction(sig, &sa, NULL));
}


TEST_GROUP(siginfo);


TEST_SETUP(siginfo)
{
	memset((void *)&siginfo_rec, 0, sizeof(siginfo_rec));
	siginfo_skipFault = 0;
	siginfo_target = pthread_self();
}


TEST_TEAR_DOWN(siginfo)
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


/* raise() is directed at the calling thread: SI_TKILL on Linux and Phoenix */
TEST(siginfo, raise_fills_siginfo)
{
	siginfo_install(SIGUSR1, siginfo_handler, 0);

	TEST_ASSERT_EQUAL_INT(0, raise(SIGUSR1));

	TEST_ASSERT_EQUAL_INT(1, siginfo_rec.calls);
	TEST_ASSERT_TRUE_MESSAGE(siginfo_rec.valid, "handler got no valid siginfo_t");
	TEST_ASSERT_EQUAL_INT(SIGUSR1, siginfo_rec.si.si_signo);
	TEST_ASSERT_EQUAL_INT(SI_TKILL, siginfo_rec.si.si_code);
	TEST_ASSERT_EQUAL_INT(getpid(), siginfo_rec.si.si_pid);
}


TEST(siginfo, kill_fills_siginfo)
{
	siginfo_install(SIGUSR2, siginfo_handler, 0);

	TEST_ASSERT_EQUAL_INT(0, kill(getpid(), SIGUSR2));

	TEST_ASSERT_EQUAL_INT(1, siginfo_rec.calls);
	TEST_ASSERT_TRUE_MESSAGE(siginfo_rec.valid, "handler got no valid siginfo_t");
	TEST_ASSERT_EQUAL_INT(SIGUSR2, siginfo_rec.si.si_signo);
	TEST_ASSERT_EQUAL_INT(SI_USER, siginfo_rec.si.si_code);
	TEST_ASSERT_EQUAL_INT(getpid(), siginfo_rec.si.si_pid);
}


/* The context in the ucontext is the interrupted one: its sp is on this stack */
TEST(siginfo, ucontext_is_interrupted_context)
{
	volatile int here;
	uintptr_t local = (uintptr_t)&here;

	siginfo_install(SIGUSR1, siginfo_handler, 0);

	TEST_ASSERT_EQUAL_INT(0, raise(SIGUSR1));

	TEST_ASSERT_TRUE_MESSAGE(siginfo_rec.valid, "handler got no valid siginfo_t");
#if defined(__aarch64__)
	/* raise() is a few frames deeper than this one, never far */
	TEST_ASSERT_TRUE(siginfo_rec.sp < local);
	TEST_ASSERT_TRUE(siginfo_rec.sp > local - 4096U);
	TEST_ASSERT_NOT_EQUAL(0, siginfo_rec.pc);
#else
	(void)local;
#endif
}


#if defined(__aarch64__)

static volatile uintptr_t siginfo_faultInsn; /* address of the faulting store */
static sigjmp_buf siginfo_escape;


/* Stores to addr and returns x0 as it is after the store, which the SIGSEGV
 * handler resumes past with x0 set to the sentinel. */
static __attribute__((noinline)) unsigned long siginfo_faultingStore(volatile void *addr)
{
	unsigned long ret;
	uintptr_t pc;

	__asm__ volatile(
			"mov x0, #0\n\t"
			"adr %1, 1f\n\t"
			"1: str xzr, [%2]\n\t"
			"mov %0, x0\n\t"
			: "=&r"(ret), "=&r"(pc)
			: "r"(addr)
			: "x0", "memory");

	siginfo_faultInsn = pc;
	return ret;
}


static void siginfo_segvHandler(int sig, siginfo_t *info, void *ucv)
{
	siginfo_copy(sig, info, ucv);

	/* No usable context, or the pc change did not take: leave rather than
	 * fault again forever */
	if ((siginfo_rec.valid == 0) || (siginfo_skipFault == 0) || (siginfo_rec.calls > 1)) {
		siglongjmp(siginfo_escape, 1);
	}

	ucontext_t *uc = ucv;
	uc->uc_mcontext.pc += 4U;
	uc->uc_mcontext.regs[0] = SIGINFO_SENTINEL;
}


static void siginfo_faultTest(void *addr, int code)
{
	volatile unsigned long ret = 0;

	siginfo_install(SIGSEGV, siginfo_segvHandler, 0);
	siginfo_skipFault = 1;
	siginfo_faultInsn = 0;

	if (sigsetjmp(siginfo_escape, 1) == 0) {
		ret = siginfo_faultingStore(addr);
	}

	TEST_ASSERT_EQUAL_INT(1, siginfo_rec.calls);
	TEST_ASSERT_TRUE_MESSAGE(siginfo_rec.valid, "handler got no valid siginfo_t");
	TEST_ASSERT_EQUAL_INT(SIGSEGV, siginfo_rec.si.si_signo);
	TEST_ASSERT_EQUAL_INT(code, siginfo_rec.si.si_code);
	TEST_ASSERT_EQUAL_PTR(addr, siginfo_rec.si.si_addr);
	TEST_ASSERT_EQUAL_HEX64((uintptr_t)addr, siginfo_rec.faultAddress);

	/* Execution resumed after the store, with the x0 the handler wrote */
	TEST_ASSERT_EQUAL_HEX64(SIGINFO_SENTINEL, ret);
	TEST_ASSERT_EQUAL_HEX64(siginfo_faultInsn, siginfo_rec.pc);
}

#endif


/* SIGSEGV on an unmapped address: si_addr is that address, the pc is the
 * faulting instruction, and moving the pc past it lets the code carry on */
TEST(siginfo, segv_unmapped_resume)
{
#if defined(__aarch64__)
	const size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
	char *page = mmap(NULL, pagesz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, page);
	TEST_ASSERT_EQUAL_INT(0, munmap(page, pagesz));

	siginfo_faultTest(page + 0x18, SEGV_MAPERR);
#else
	TEST_IGNORE_MESSAGE("needs an aarch64 mcontext_t");
#endif
}


/* SIGSEGV on a write to a read-only mapping */
TEST(siginfo, segv_readonly_resume)
{
#if defined(__aarch64__)
	const size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
	char *page = mmap(NULL, pagesz, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, page);

	siginfo_faultTest(page + 0x40, SEGV_ACCERR);

	TEST_ASSERT_EQUAL_INT(0, munmap(page, pagesz));
#else
	TEST_IGNORE_MESSAGE("needs an aarch64 mcontext_t");
#endif
}


static void siginfo_blockHandler(int sig, siginfo_t *info, void *ucv)
{
	siginfo_copy(sig, info, ucv);

	if (siginfo_rec.valid != 0) {
		ucontext_t *uc = ucv;
		(void)sigaddset(&uc->uc_sigmask, SIGUSR2);
	}
}


/* uc_sigmask is the mask restored on return, so a handler can change it */
TEST(siginfo, uc_sigmask_restored)
{
	sigset_t set;

	siginfo_install(SIGUSR1, siginfo_blockHandler, 0);

	TEST_ASSERT_EQUAL_INT(0, raise(SIGUSR1));
	TEST_ASSERT_TRUE_MESSAGE(siginfo_rec.valid, "handler got no valid siginfo_t");

	TEST_ASSERT_EQUAL_INT(0, sigprocmask(SIG_SETMASK, NULL, &set));
	TEST_ASSERT_EQUAL_INT(1, sigismember(&set, SIGUSR2));
	TEST_ASSERT_EQUAL_INT(0, sigismember(&set, SIGUSR1));
}


/* pthread_kill(): the signal runs on the target thread, with that thread's
 * context. The handler then waits in sigsuspend() for a second signal, as a
 * garbage collector's suspend/resume handshake does, so the nested delivery
 * and both returns are exercised too. */

#define SIGINFO_STACKSZ (64U * 1024U)

static unsigned char siginfo_stack[SIGINFO_STACKSZ] __attribute__((aligned(16)));
static volatile int siginfo_suspended;
static volatile int siginfo_resumed;
static volatile int siginfo_depth;
static volatile int siginfo_threadDone;


static void siginfo_suspendHandler(int sig, siginfo_t *info, void *ucv)
{
	sigset_t wait;

	siginfo_depth++;
	if (siginfo_depth == 1) {
		siginfo_copy(sig, info, ucv);
		siginfo_suspended = 1;

		(void)sigfillset(&wait);
		(void)sigdelset(&wait, sig);
		(void)sigsuspend(&wait);
		siginfo_resumed = 1;
	}
	siginfo_depth--;
}


static void *siginfo_thread(void *arg)
{
	(void)arg;

	while (siginfo_resumed == 0) {
		usleep(1000);
	}
	siginfo_threadDone = 1;

	return NULL;
}


TEST(siginfo, pthread_kill_target_context)
{
	pthread_attr_t attr;
	pthread_t tid;
	int i, suspended;

	siginfo_suspended = 0;
	siginfo_resumed = 0;
	siginfo_depth = 0;
	siginfo_threadDone = 0;

	/* SIGUSR1 is blocked in its handler until sigsuspend() lets the resume in,
	 * so a resume sent early is held rather than lost */
	siginfo_install(SIGUSR1, siginfo_suspendHandler, 0);

	TEST_ASSERT_EQUAL_INT(0, pthread_attr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_setstack(&attr, siginfo_stack, sizeof(siginfo_stack)));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&tid, &attr, siginfo_thread, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_destroy(&attr));
	siginfo_target = tid;

	TEST_ASSERT_EQUAL_INT(0, pthread_kill(tid, SIGUSR1));
	for (i = 0; (i < 5000) && (siginfo_suspended == 0); i++) {
		usleep(1000);
	}
	suspended = siginfo_suspended;

	/* The thread is parked in the handler, so what it recorded is stable. Let
	 * it go before checking anything, so a failure does not leave it behind. */
	if (suspended != 0) {
		TEST_ASSERT_EQUAL_INT(0, pthread_kill(tid, SIGUSR1));
	}
	else {
		siginfo_resumed = 1;
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_join(tid, NULL));

	TEST_ASSERT_EQUAL_INT_MESSAGE(1, suspended, "the thread never ran the handler");
	TEST_ASSERT_TRUE(siginfo_rec.onThread);
	TEST_ASSERT_TRUE_MESSAGE(siginfo_rec.valid, "handler got no valid siginfo_t");
	TEST_ASSERT_EQUAL_INT(SIGUSR1, siginfo_rec.si.si_signo);
	TEST_ASSERT_EQUAL_INT(SI_TKILL, siginfo_rec.si.si_code);
	TEST_ASSERT_EQUAL_INT(getpid(), siginfo_rec.si.si_pid);
#if defined(__aarch64__)
	TEST_ASSERT_TRUE(siginfo_rec.sp > (uintptr_t)siginfo_stack);
	TEST_ASSERT_TRUE(siginfo_rec.sp <= (uintptr_t)siginfo_stack + sizeof(siginfo_stack));
#endif

	/* The resume ran nested in sigsuspend() and both handlers returned */
	TEST_ASSERT_EQUAL_INT(1, siginfo_resumed);
	TEST_ASSERT_EQUAL_INT(1, siginfo_threadDone);
	TEST_ASSERT_EQUAL_INT(0, siginfo_depth);
}


/* The same handshake on a thread that never enters the kernel, as JIT code in a loop does: a
 * garbage collector or a sampling profiler suspends such a thread too. The signal reaches it
 * when it is next preempted. While it waits in the handler it must not run on (its counter
 * stands still), and after the resume it must. Repeated, as a collector does on every cycle. */

#define SIGINFO_SPIN_ROUNDS 20

static volatile unsigned long siginfo_spins;
static volatile int siginfo_stopSpinning;


static void *siginfo_spinner(void *arg)
{
	(void)arg;

	while (siginfo_stopSpinning == 0) {
		siginfo_spins++;
	}

	return NULL;
}


static int siginfo_waitFor(volatile int *flag)
{
	int i;

	for (i = 0; (i < 5000) && (*flag == 0); i++) {
		usleep(1000);
	}

	return *flag;
}


TEST(siginfo, pthread_kill_running_thread)
{
	pthread_attr_t attr;
	pthread_t tid;
	unsigned long before, after;
	int round, suspended = 1, stood = 1, ran = 1, sp = 1, resumed = 1;

	siginfo_stopSpinning = 0;
	siginfo_spins = 0;
	siginfo_depth = 0;
	siginfo_install(SIGUSR1, siginfo_suspendHandler, 0);

	TEST_ASSERT_EQUAL_INT(0, pthread_attr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_setstack(&attr, siginfo_stack, sizeof(siginfo_stack)));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&tid, &attr, siginfo_spinner, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_destroy(&attr));
	siginfo_target = tid;

	for (round = 0; (round < SIGINFO_SPIN_ROUNDS) && (suspended != 0) && (resumed != 0); round++) {
		siginfo_suspended = 0;
		siginfo_resumed = 0;
		siginfo_rec.valid = 0;

		TEST_ASSERT_EQUAL_INT(0, pthread_kill(tid, SIGUSR1));
		suspended = siginfo_waitFor(&siginfo_suspended);
		if (suspended == 0) {
			break;
		}

		/* Parked in sigsuspend(): the loop does not advance */
		before = siginfo_spins;
		usleep(20000);
		stood = stood && (siginfo_spins == before);
		sp = sp && (siginfo_rec.valid != 0) && (siginfo_rec.sp > (uintptr_t)siginfo_stack) &&
				(siginfo_rec.sp <= (uintptr_t)siginfo_stack + sizeof(siginfo_stack));

		TEST_ASSERT_EQUAL_INT(0, pthread_kill(tid, SIGUSR1));
		resumed = siginfo_waitFor(&siginfo_resumed);

		/* ...and runs on after the resume */
		after = siginfo_spins;
		usleep(20000);
		ran = ran && (siginfo_spins != after);
	}

	siginfo_stopSpinning = 1;
	if ((suspended == 0) || (resumed == 0)) {
		/* the loop never stopped, or is still parked: the resume a failed round owes it */
		(void)pthread_kill(tid, SIGUSR1);
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_join(tid, NULL));

	TEST_ASSERT_EQUAL_INT_MESSAGE(1, suspended, "a thread spinning in user space never ran the handler");
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, resumed, "the suspended thread was not resumed");
	TEST_ASSERT_TRUE_MESSAGE(stood, "the thread ran on while it was suspended");
	TEST_ASSERT_TRUE_MESSAGE(ran, "the thread did not run after the resume");
#if defined(__aarch64__)
	TEST_ASSERT_TRUE_MESSAGE(sp, "the ucontext sp is not on the interrupted thread's stack");
#else
	(void)sp;
#endif
	TEST_ASSERT_EQUAL_INT(0, siginfo_depth);
}


/* SIGCHLD tells which child ended, and how */
TEST(siginfo, sigchld_fills_siginfo)
{
	pid_t pid;
	int status, i;

	siginfo_install(SIGCHLD, siginfo_handler, 0);

	pid = fork();
	if (pid < 0) {
		if (errno == ENOSYS) {
			TEST_IGNORE_MESSAGE("fork syscall not supported");
		}
		FAIL("fork");
	}
	if (pid == 0) {
		_exit(7);
	}

	/* Not waiting in waitpid() while the child exits: the parent is signalled */
	for (i = 0; (i < 5000) && (siginfo_rec.calls == 0); i++) {
		usleep(1000);
	}
	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));

	TEST_ASSERT_EQUAL_INT(1, siginfo_rec.calls);
	TEST_ASSERT_TRUE_MESSAGE(siginfo_rec.valid, "handler got no valid siginfo_t");
	TEST_ASSERT_EQUAL_INT(SIGCHLD, siginfo_rec.si.si_signo);
	TEST_ASSERT_EQUAL_INT(CLD_EXITED, siginfo_rec.si.si_code);
	TEST_ASSERT_EQUAL_INT(pid, siginfo_rec.si.si_pid);
	TEST_ASSERT_EQUAL_INT(7, siginfo_rec.si.si_status);
}


TEST_GROUP_RUNNER(siginfo)
{
	RUN_TEST_CASE(siginfo, raise_fills_siginfo);
	RUN_TEST_CASE(siginfo, kill_fills_siginfo);
	RUN_TEST_CASE(siginfo, ucontext_is_interrupted_context);
	RUN_TEST_CASE(siginfo, segv_unmapped_resume);
	RUN_TEST_CASE(siginfo, segv_readonly_resume);
	RUN_TEST_CASE(siginfo, uc_sigmask_restored);
	RUN_TEST_CASE(siginfo, pthread_kill_target_context);
	RUN_TEST_CASE(siginfo, pthread_kill_running_thread);
	RUN_TEST_CASE(siginfo, sigchld_fills_siginfo);
}
