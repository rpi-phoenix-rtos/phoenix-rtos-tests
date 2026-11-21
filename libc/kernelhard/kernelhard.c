/*
 * Phoenix-RTOS
 *
 * test-libc-kernelhard
 *
 * kh_exec:  a program started by exec must not inherit the FP control state
 *           (FPCR rounding mode) of the program that ran before it, whether the
 *           exec follows fork or vfork.
 * kh_range: a buffer that starts inside a mapping and runs past its end is not
 *           a valid buffer: read(), write() and sigaltstack() must fail with
 *           EFAULT instead of the kernel touching the unmapped part. A buffer
 *           spanning two adjacent mappings stays valid.
 * kh_sigframe: the signal frame the kernel writes to the user stack holds the
 *           interrupted context only, never kernel data: no kernel stack
 *           address, no kernel bits of CPACR_EL1, and a zero FP area when the
 *           interrupted code had not used the FPU.
 * kh_vm:    MAP_CONTIGUOUS memory arrives zeroed; an unaligned access to Device
 *           memory kills the process instead of faulting for ever (aarch64).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/ucontext.h>
#include <sys/wait.h>

#include <unity_fixture.h>

#include "kernelhard.h"

#define KH_PAGE      4096
#define KH_FPCR_RMZ  (3UL << 22) /* FPCR.RMode = round towards zero */
#define KH_SELF      "/bin/test-libc-kernelhard"


#if defined(__aarch64__)
static uint64_t kh_fpcrGet(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, fpcr" : "=r"(v));
	return v;
}


static void kh_fpcrSet(uint64_t v)
{
	__asm__ volatile("msr fpcr, %0" : : "r"(v));
}
#endif


int kh_childMain(void)
{
#if defined(__aarch64__)
	return ((kh_fpcrGet() & KH_FPCR_RMZ) == 0) ? 0 : 1;
#else
	return 0;
#endif
}


static int kh_execChild(int useVfork)
{
	static char *const argv[] = { KH_SELF, NULL };
	static char *const envp[] = { KH_CHILD_ENV "=1", NULL };
	int status = -1;
	pid_t pid;

	pid = (useVfork != 0) ? vfork() : fork();
	if (pid == 0) {
		execve(KH_SELF, argv, envp);
		_exit(99);
	}
	if (pid < 0) {
		return -1;
	}
	if (waitpid(pid, &status, 0) != pid) {
		return -1;
	}
	return status;
}


TEST_GROUP(kh_exec);


TEST_SETUP(kh_exec)
{
}


TEST_TEAR_DOWN(kh_exec)
{
#if defined(__aarch64__)
	kh_fpcrSet(0);
#endif
}


TEST(kh_exec, fork_exec_resets_fpcr)
{
#if defined(__aarch64__)
	int status;

	kh_fpcrSet(KH_FPCR_RMZ);
	TEST_ASSERT_EQUAL_HEX64(KH_FPCR_RMZ, kh_fpcrGet() & KH_FPCR_RMZ);
	status = kh_execChild(0);
	TEST_ASSERT_TRUE(WIFEXITED(status));
	TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
#else
	TEST_IGNORE_MESSAGE("aarch64 only");
#endif
}


TEST(kh_exec, vfork_exec_resets_fpcr)
{
#if defined(__aarch64__)
	int status;

	kh_fpcrSet(KH_FPCR_RMZ);
	status = kh_execChild(1);
	TEST_ASSERT_TRUE(WIFEXITED(status));
	TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
#else
	TEST_IGNORE_MESSAGE("aarch64 only");
#endif
}


TEST_GROUP_RUNNER(kh_exec)
{
	RUN_TEST_CASE(kh_exec, fork_exec_resets_fpcr);
	RUN_TEST_CASE(kh_exec, vfork_exec_resets_fpcr);
}


static char *kh_map;  /* 3 pages, the middle one unmapped: a guaranteed hole */
static int kh_pipe[2];


TEST_GROUP(kh_range);


TEST_SETUP(kh_range)
{
	kh_map = mmap(NULL, 3 * KH_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, kh_map);
	memset(kh_map, 'k', 3 * KH_PAGE);
	TEST_ASSERT_EQUAL_INT(0, munmap(kh_map + KH_PAGE, KH_PAGE));
	TEST_ASSERT_EQUAL_INT(0, pipe(kh_pipe));
}


TEST_TEAR_DOWN(kh_range)
{
	munmap(kh_map, KH_PAGE);
	munmap(kh_map + 2 * KH_PAGE, KH_PAGE);
	close(kh_pipe[0]);
	close(kh_pipe[1]);
}


TEST(kh_range, write_straddling_hole_faults)
{
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, write(kh_pipe[1], kh_map + KH_PAGE - 16, 32));
	TEST_ASSERT_EQUAL_INT(EFAULT, errno);
}


TEST(kh_range, read_straddling_hole_faults)
{
	char src[32];

	memset(src, 'r', sizeof(src));
	TEST_ASSERT_EQUAL_INT(sizeof(src), write(kh_pipe[1], src, sizeof(src)));
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, read(kh_pipe[0], kh_map + KH_PAGE - 16, 32));
	TEST_ASSERT_EQUAL_INT(EFAULT, errno);
}


TEST(kh_range, sigaltstack_straddling_hole_fails)
{
	stack_t ss = { .ss_sp = kh_map + KH_PAGE - MINSIGSTKSZ / 2, .ss_size = MINSIGSTKSZ, .ss_flags = 0 };

	TEST_ASSERT_EQUAL_INT(-1, sigaltstack(&ss, NULL));
}


TEST(kh_range, adjacent_mappings_stay_valid)
{
	char *q = mmap(NULL, 2 * KH_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	char back[32];

	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, q);
	memset(q, 'a', 2 * KH_PAGE);
	/* Splits the mapping into two adjacent entries */
	TEST_ASSERT_EQUAL_INT(0, mprotect(q + KH_PAGE, KH_PAGE, PROT_READ));
	TEST_ASSERT_EQUAL_INT(32, write(kh_pipe[1], q + KH_PAGE - 16, 32));
	TEST_ASSERT_EQUAL_INT(32, read(kh_pipe[0], back, sizeof(back)));
	TEST_ASSERT_EACH_EQUAL_CHAR('a', back, sizeof(back));
	munmap(q, 2 * KH_PAGE);
}


TEST_GROUP_RUNNER(kh_range)
{
	RUN_TEST_CASE(kh_range, write_straddling_hole_faults);
	RUN_TEST_CASE(kh_range, read_straddling_hole_faults);
	RUN_TEST_CASE(kh_range, sigaltstack_straddling_hole_fails);
	RUN_TEST_CASE(kh_range, adjacent_mappings_stay_valid);
}


#if defined(__aarch64__)
/*
 * The aarch64 kernel's cpu_context_t (hal/aarch64/arch/cpu.h), which it writes
 * just below the interrupted sp as the signal frame, in 64-bit words:
 *   savesp, cpacr, fpcr, fpsr, freg[64], psr, pc, x[31], sp
 * The handler checks that it found the frame by comparing x[] with the
 * registers in its ucontext_t, so a layout change fails the test loudly
 * instead of making it check the wrong words.
 */
#define KH_CTX_WORDS  102
#define KH_CTX_SAVESP 0
#define KH_CTX_CPACR  1
#define KH_CTX_FP     2  /* fpcr, fpsr, freg[64] */
#define KH_CTX_FPLEN  66
#define KH_CTX_X      70
#define KH_CPACR_FPEN (3UL << 20)

/* Only general-purpose registers: code that must not enable the FPU */
#define KH_NOFP __attribute__((target("general-regs-only")))

static struct {
	volatile int calls;
	int found;      /* the frame below the interrupted sp matches the ucontext */
	int stackPadOk; /* the padding of uc_stack is zero */
	uint64_t frame[KH_CTX_X];
} kh_sig;

static struct sigaction kh_oldUsr1, kh_oldUsr2;


static KH_NOFP int kh_isKernelAddr(uint64_t v)
{
	return ((v >> 48) == 0xffffU) ? 1 : 0;
}


static KH_NOFP void kh_sigframeHandler(int sig, siginfo_t *info, void *ucv)
{
	const ucontext_t *uc = ucv;
	const volatile uint64_t *frame = (const volatile uint64_t *)(uintptr_t)(uc->uc_mcontext.sp - KH_CTX_WORDS * sizeof(uint64_t));
	const volatile unsigned char *stack = (const volatile unsigned char *)&uc->uc_stack;
	size_t i;

	(void)sig;
	(void)info;

	kh_sig.found = 1;
	for (i = 0; i < 31; i++) {
		if (frame[KH_CTX_X + i] != uc->uc_mcontext.regs[i]) {
			kh_sig.found = 0;
		}
	}

	/* Word loops, not memcpy: nothing here may touch the FP/SIMD registers */
	for (i = 0; i < KH_CTX_X; i++) {
		kh_sig.frame[i] = frame[i];
	}

	kh_sig.stackPadOk = 1;
	for (i = offsetof(stack_t, ss_flags) + sizeof(int); i < offsetof(stack_t, ss_size); i++) {
		if (stack[i] != 0U) {
			kh_sig.stackPadOk = 0;
		}
	}

	kh_sig.calls++;
}


/* A handler is entered with the FPU trapped. This one does not enable it
 * (raise() is getpid, gettid and tkill: integer code and syscall stubs), so
 * the frame of the signal it raises is built from a kernel entry that saved
 * no FP state. */
static KH_NOFP void kh_sigframeOuter(int sig)
{
	(void)sig;
	(void)raise(SIGUSR1);
}


static void kh_sigframeAssertClean(void)
{
	size_t i;

	TEST_ASSERT_EQUAL_INT(1, kh_sig.calls);
	TEST_ASSERT_TRUE_MESSAGE(kh_sig.found, "signal frame not found below the interrupted sp");
	TEST_ASSERT_TRUE_MESSAGE(kh_sig.stackPadOk, "uc_stack padding is not zero");
	TEST_ASSERT_FALSE_MESSAGE(kh_isKernelAddr(kh_sig.frame[KH_CTX_SAVESP]), "kernel stack address in the frame");
	TEST_ASSERT_EQUAL_HEX64(0, kh_sig.frame[KH_CTX_SAVESP]);
	TEST_ASSERT_EQUAL_HEX64(0, kh_sig.frame[KH_CTX_CPACR] & ~KH_CPACR_FPEN);

	if ((kh_sig.frame[KH_CTX_CPACR] & KH_CPACR_FPEN) != KH_CPACR_FPEN) {
		for (i = KH_CTX_FP; i < KH_CTX_FP + KH_CTX_FPLEN; i++) {
			TEST_ASSERT_FALSE_MESSAGE(kh_isKernelAddr(kh_sig.frame[i]), "kernel address in the FP area");
			TEST_ASSERT_EQUAL_HEX64_MESSAGE(0, kh_sig.frame[i], "FP area not zero with the FPU unused");
		}
	}
}
#endif


TEST_GROUP(kh_sigframe);


TEST_SETUP(kh_sigframe)
{
#if defined(__aarch64__)
	struct sigaction sa;

	memset(&kh_sig, 0, sizeof(kh_sig));
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = kh_sigframeHandler;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR1, &sa, &kh_oldUsr1));

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = kh_sigframeOuter;
	sigemptyset(&sa.sa_mask);
	TEST_ASSERT_EQUAL_INT(0, sigaction(SIGUSR2, &sa, &kh_oldUsr2));
#endif
}


TEST_TEAR_DOWN(kh_sigframe)
{
#if defined(__aarch64__)
	sigaction(SIGUSR1, &kh_oldUsr1, NULL);
	sigaction(SIGUSR2, &kh_oldUsr2, NULL);
#endif
}


/* The main thread: the FPU may or may not be in use */
TEST(kh_sigframe, frame_holds_no_kernel_data)
{
#if defined(__aarch64__)
	TEST_ASSERT_EQUAL_INT(0, raise(SIGUSR1));
	kh_sigframeAssertClean();
#else
	TEST_IGNORE_MESSAGE("aarch64 only");
#endif
}


/* A signal raised by code that has not used the FPU: its frame's FP area must
 * be zero. Before the kernel built the frame field by field, it held whatever
 * an earlier kernel entry had left in the kernel frame: FP registers saved
 * then, or the all-ones fill of a new thread's first frame. */
TEST(kh_sigframe, fp_area_zero_when_fpu_unused)
{
#if defined(__aarch64__)
	TEST_ASSERT_EQUAL_INT(0, raise(SIGUSR2));

	if ((kh_sig.calls == 1) && ((kh_sig.frame[KH_CTX_CPACR] & KH_CPACR_FPEN) == KH_CPACR_FPEN)) {
		/* The handler was entered with the FPU enabled: the case cannot be set up */
		TEST_IGNORE_MESSAGE("the FPU was in use before the signal");
	}
	kh_sigframeAssertClean();
#else
	TEST_IGNORE_MESSAGE("aarch64 only");
#endif
}


TEST_GROUP_RUNNER(kh_sigframe)
{
	RUN_TEST_CASE(kh_sigframe, frame_holds_no_kernel_data);
	RUN_TEST_CASE(kh_sigframe, fp_area_zero_when_fpu_unused);
}


/*
 * kh_vm: memory the kernel hands out, and faults it must not resolve.
 *
 * MAP_CONTIGUOUS memory came straight from the page allocator, with whatever its previous owner
 * had left in it. And on aarch64 every abort went to the page-fault handler, so an alignment
 * fault (an unaligned access to Device memory) on a present page was "resolved" and taken again
 * for ever: the thread spun in the kernel. The Device mapping below is a second mapping of one
 * of the test's own RAM pages, so nothing outside the test is touched.
 */

#define KH_CONTIG_SIZE (256 * 1024)


TEST_GROUP(kh_vm);


TEST_SETUP(kh_vm)
{
}


TEST_TEAR_DOWN(kh_vm)
{
}


TEST(kh_vm, contiguous_memory_is_zeroed)
{
	unsigned char *p;
	size_t i, nonzero = 0;
	int round;

	for (round = 0; round < 4; round++) {
		p = mmap(NULL, KH_CONTIG_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_CONTIGUOUS, -1, 0);
		TEST_ASSERT_NOT_EQUAL(MAP_FAILED, p);
		for (i = 0; i < KH_CONTIG_SIZE; i++) {
			if (p[i] != 0U) {
				nonzero++;
			}
		}
		/* Leave a pattern for the next allocation, which usually gets the same pages back */
		memset(p, 0xa5, KH_CONTIG_SIZE);
		TEST_ASSERT_EQUAL_INT(0, munmap(p, KH_CONTIG_SIZE));
	}
	TEST_ASSERT_EQUAL_size_t(0, nonzero);
}


#ifdef __aarch64__
static void kh_unalignedLoad(volatile void *addr)
{
	unsigned int v;

	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(addr) : "memory");
	(void)v;
}
#endif


TEST(kh_vm, device_alignment_fault_kills)
{
#ifdef __aarch64__
	unsigned char *ram, *dev;
	addr_t pa;
	pid_t pid;
	int status = 0, i;

	ram = mmap(NULL, KH_PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_CONTIGUOUS, -1, 0);
	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, ram);
	ram[0] = 1;
	pa = va2pa(ram);
	TEST_ASSERT_NOT_EQUAL(0, pa);
	dev = mmap(NULL, KH_PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PHYSMEM | MAP_DEVICE, -1, (off_t)(pa & ~(addr_t)(KH_PAGE - 1)));
	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, dev);

	pid = fork();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);
	if (pid == 0) {
		kh_unalignedLoad(dev + 1);
		_exit(0); /* not reached: the load must not complete */
	}

	/* Before the fix the child never came back from the fault: give it 3 s, then stop it */
	for (i = 0; i < 300; i++) {
		if (waitpid(pid, &status, WNOHANG) == pid) {
			break;
		}
		usleep(10 * 1000);
	}
	if (i == 300) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
	}
	munmap(dev, KH_PAGE);
	munmap(ram, KH_PAGE);
	TEST_ASSERT_LESS_THAN_INT_MESSAGE(300, i, "an unaligned load from Device memory hung in the kernel");
	TEST_ASSERT_TRUE(WIFSIGNALED(status));
#else
	TEST_IGNORE_MESSAGE("aarch64 only");
#endif
}


/* Private non-cacheable pages copied at fork(): each process must see its own
 * data, intact, on both sides. The copy's cache maintenance runs while the
 * parent's tables are live (it must work through the physical page, not the
 * child's virtual address). */
TEST(kh_vm, uncached_private_survives_fork)
{
	const size_t pages = 4u, len = pages * KH_PAGE;
	unsigned int *p;
	size_t i, n = len / sizeof(*p);
	pid_t pid;
	int round, status, bad;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_UNCACHED, -1, 0);
	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, p);

	for (round = 0; round < 20; round++) {
		for (i = 0; i < n; i++) {
			p[i] = 0x5a000000u ^ ((unsigned int)round << 16) ^ (unsigned int)i;
		}

		pid = fork();
		TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);
		if (pid == 0) {
			bad = 0;
			for (i = 0; i < n; i++) {
				if (p[i] != (0x5a000000u ^ ((unsigned int)round << 16) ^ (unsigned int)i)) {
					bad = 1;
				}
				p[i] = 0xa5a5a5a5u;
			}
			for (i = 0; i < n; i++) {
				if (p[i] != 0xa5a5a5a5u) {
					bad = 1;
				}
			}
			_exit(bad);
		}

		status = -1;
		TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
		TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(status), "the child faulted on its copy");
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, WEXITSTATUS(status), "the child saw wrong data");
		for (i = 0; i < n; i++) {
			if (p[i] != (0x5a000000u ^ ((unsigned int)round << 16) ^ (unsigned int)i)) {
				break;
			}
		}
		TEST_ASSERT_EQUAL_MESSAGE(n, i, "the child's writes reached the parent");
	}

	munmap(p, len);
}


/* memExportMapped(): an export nobody maps counts 0; withdrawn or unknown -> -ENOENT */
TEST(kh_vm, export_mapped_counts)
{
	uint32_t port;
	oid_t oid;
	void *p;

	TEST_ASSERT_EQUAL_INT(0, portCreate(&port));
	p = mmap(NULL, KH_PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_CONTIGUOUS, -1, 0);
	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, p);

	oid.port = port;
	oid.id = 7;
	TEST_ASSERT_EQUAL_INT(-ENOENT, memExportMapped(&oid));
	TEST_ASSERT_EQUAL_INT(0, memExport(&oid, p, KH_PAGE));
	TEST_ASSERT_EQUAL_INT(0, memExportMapped(&oid));
	TEST_ASSERT_EQUAL_INT(0, memUnexport(&oid));
	TEST_ASSERT_EQUAL_INT(-ENOENT, memExportMapped(&oid));

	munmap(p, KH_PAGE);
	portDestroy(port);
}


TEST_GROUP_RUNNER(kh_vm)
{
	RUN_TEST_CASE(kh_vm, contiguous_memory_is_zeroed);
	RUN_TEST_CASE(kh_vm, device_alignment_fault_kills);
	RUN_TEST_CASE(kh_vm, uncached_private_survives_fork);
	RUN_TEST_CASE(kh_vm, export_mapped_counts);
}
