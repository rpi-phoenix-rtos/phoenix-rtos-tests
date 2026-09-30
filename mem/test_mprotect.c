/*
 * Phoenix-RTOS
 *
 * phoenix-rtos-test
 *
 * mprotect syscall tests
 *
 * Copyright 2023 Phoenix Systems
 * Author: Hubert Badocha
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "unity_fixture.h"


#define PAGES 4


static long page_size;


TEST_GROUP(test_mprotect);


TEST_SETUP(test_mprotect)
{
	page_size = sysconf(_SC_PAGESIZE);
}


TEST_TEAR_DOWN(test_mprotect)
{
}


TEST(test_mprotect, test_mprotect_singlecore)
{
	unsigned char *area = mmap(NULL, page_size * PAGES, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	TEST_ASSERT(area != MAP_FAILED);

	for (int page = 0; page < PAGES; page++) {
		area[page * page_size] = 0x42;
	}

	TEST_ASSERT_EQUAL(0, mprotect(area, page_size * PAGES, PROT_READ));

	for (int page = 0; page < PAGES; page++) {
		TEST_ASSERT_EQUAL(0x42, area[page * page_size]);
	}

	TEST_ASSERT_EQUAL(0, mprotect(area, page_size * PAGES, PROT_READ | PROT_WRITE));

	for (int page = 0; page < PAGES; page++) {
		area[(page * page_size) + 0x6] = 0x9;
		TEST_ASSERT_EQUAL(0x9, area[(page * page_size) + 0x6]);
	}

	TEST_ASSERT_EQUAL(0, munmap(area, page_size * PAGES));
}


TEST(test_mprotect, pages_in_child_copied)
{
	unsigned char *area = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	TEST_ASSERT(area != MAP_FAILED);

	area[0] = 0x42;

	TEST_ASSERT_EQUAL_INT(0, mprotect(area, page_size, PROT_READ));

	pid_t pid = fork();
	TEST_ASSERT(pid >= 0);
	if (pid == 0) {
		/* Wait for modifications in parent. */
		sleep(1);
		if (area[0] != 0x42) {
			exit(1);
		}
		exit(0);
	}

	TEST_ASSERT_EQUAL_INT(0, mprotect(area, page_size, PROT_READ | PROT_WRITE));
	area[0] = 0x41;

	int returnStatus;
	TEST_ASSERT(pid == waitpid(pid, &returnStatus, 0));
	TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(returnStatus));

	TEST_ASSERT_EQUAL_INT(0, munmap(area, page_size));
}


TEST(test_mprotect, pages_in_parent_copied)
{
	unsigned char *area = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	TEST_ASSERT(area != MAP_FAILED);

	area[0] = 0x42;

	TEST_ASSERT_EQUAL_INT(0, mprotect(area, page_size, PROT_READ));

	pid_t pid = fork();
	TEST_ASSERT(pid >= 0);
	if (pid == 0) {
		TEST_ASSERT_EQUAL_INT(0, mprotect(area, page_size, PROT_READ | PROT_WRITE));
		area[0] = 0x41;
		exit(0);
	}

	int returnStatus;
	TEST_ASSERT(pid == waitpid(pid, &returnStatus, 0));
	TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(returnStatus));

	TEST_ASSERT_EQUAL_INT(0x42, area[0]);

	TEST_ASSERT_EQUAL_INT(0, munmap(area, page_size));
}


/* Stores to addr in a child: a store to a page mprotect() left read-only kills
 * the child, and the parent then fails the test instead of dying itself. */
static void assert_child_writes(volatile unsigned char *addr)
{
	pid_t pid = fork();
	TEST_ASSERT(pid >= 0);
	if (pid == 0) {
		*addr = 0x5a;
		_exit((*addr == 0x5a) ? 0 : 1);
	}

	int status;
	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	TEST_ASSERT_FALSE_MESSAGE(WIFSIGNALED(status), "store faulted: page is not writable");
	TEST_ASSERT_TRUE(WIFEXITED(status));
	TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}


/* POSIX: addr must be page-aligned, len need not be a page multiple -- the
 * call covers every page the range touches. Phoenix used to fail such a call
 * with EINVAL (quake3's JIT: "mprotect(RX) failed"). */
TEST(test_mprotect, unaligned_len_covers_last_page)
{
	unsigned char *area = mmap(NULL, page_size * 3, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	TEST_ASSERT(area != MAP_FAILED);
	area[0] = area[page_size] = area[2 * page_size] = 0x42;

	TEST_ASSERT_EQUAL_INT(0, mprotect(area, page_size * 3, PROT_READ));

	/* One byte into page 1: pages 0 and 1 become writable, all of page 1. */
	TEST_ASSERT_EQUAL_INT(0, mprotect(area, page_size + 1, PROT_READ | PROT_WRITE));
	assert_child_writes(area);
	assert_child_writes(area + page_size);
	assert_child_writes(area + (2 * page_size) - 1);

	TEST_ASSERT_EQUAL_INT(0x42, area[page_size]);
	TEST_ASSERT_EQUAL_INT(0, munmap(area, page_size * 3));
}


/* ...and only those pages: the rounding must not reach the next one. */
TEST(test_mprotect, unaligned_len_stops_at_last_page)
{
	unsigned char *area = mmap(NULL, page_size * 3, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	TEST_ASSERT(area != MAP_FAILED);
	area[0] = area[page_size] = area[2 * page_size] = 0x42;

	TEST_ASSERT_EQUAL_INT(0, mprotect(area, page_size + 1, PROT_READ));
	assert_child_writes(area + (2 * page_size));
	TEST_ASSERT_EQUAL_INT(0x42, area[page_size]);

	TEST_ASSERT_EQUAL_INT(0, mprotect(area, (2 * page_size) - 1, PROT_READ | PROT_WRITE));
	assert_child_writes(area + (2 * page_size) - 1);

	TEST_ASSERT_EQUAL_INT(0, munmap(area, page_size * 3));
}


TEST(test_mprotect, args)
{
	unsigned char *area = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	TEST_ASSERT(area != MAP_FAILED);

	/* An unaligned addr is the one alignment error POSIX specifies. */
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, mprotect(area + 1, page_size - 1, PROT_READ));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);

	/* An empty range touches no page; not an error (as on Linux and the BSDs). */
	TEST_ASSERT_EQUAL_INT(0, mprotect(area, 0, PROT_READ));
	assert_child_writes(area);

	TEST_ASSERT_EQUAL_INT(0, munmap(area, page_size));
}


TEST_GROUP_RUNNER(test_mprotect)
{
	RUN_TEST_CASE(test_mprotect, test_mprotect_singlecore);
	RUN_TEST_CASE(test_mprotect, pages_in_child_copied);
	RUN_TEST_CASE(test_mprotect, pages_in_parent_copied);
	RUN_TEST_CASE(test_mprotect, unaligned_len_covers_last_page);
	RUN_TEST_CASE(test_mprotect, unaligned_len_stops_at_last_page);
	RUN_TEST_CASE(test_mprotect, args);
}


static void runner(void)
{
	RUN_TEST_GROUP(test_mprotect);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
