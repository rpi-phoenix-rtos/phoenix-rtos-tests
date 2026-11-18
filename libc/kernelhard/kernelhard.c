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
#include <sys/mman.h>
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
