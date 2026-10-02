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
#include <string.h>
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


/* meminfo() credits each map entry with the anonymous pages of its own range. mprotect() splits
 * an entry, and the pieces share one amap at different offsets: the kernel used to count the
 * whole amap for each of them, so a mapping split in three reported its pages three times
 * (WebKit's memory-pressure handler reads this sum and saw GBs). */
static entryinfo_t *self_entries(int *count)
{
	meminfo_t info;
	entryinfo_t *map = NULL, *grown;
	int mapsz = 64;

	for (;;) {
		grown = realloc(map, (size_t)mapsz * sizeof(*map));
		if (grown == NULL) {
			free(map);
			return NULL;
		}
		map = grown;
		memset(&info, 0, sizeof(info));
		info.page.mapsz = -1;
		info.maps.mapsz = -1;
		info.entry.kmapsz = -1;
		info.entry.pid = (unsigned int)getpid();
		info.entry.mapsz = mapsz;
		info.entry.map = map;
		meminfo(&info);
		if (info.entry.mapsz < 0) {
			free(map);
			return NULL;
		}
		if (info.entry.mapsz <= mapsz) {
			*count = info.entry.mapsz;
			return map;
		}
		mapsz = info.entry.mapsz + 16;
	}
}


TEST(test_mprotect, meminfo_anonsz_of_split_entries)
{
	const int pages = 16, touched = 12;
	unsigned char *area = mmap(NULL, page_size * pages, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	unsigned char *mid = area + (4 * page_size);
	entryinfo_t *map;
	int count, i, found = 0;

	TEST_ASSERT(area != MAP_FAILED);
	for (i = 0; i < touched; i++) {
		area[i * page_size] = 0x42;
	}
	/* pages 4..7 read-only: three entries, [0,4) [4,8) [8,16), all of them on the one amap */
	TEST_ASSERT_EQUAL_INT(0, mprotect(mid, page_size * 4, PROT_READ));

	map = self_entries(&count);
	TEST_ASSERT_NOT_NULL(map);
	for (i = 0; i < count; i++) {
		/* no entry holds more anonymous memory than its size, in any mapping of this process */
		if (map[i].anonsz != (size_t)-1) {
			TEST_ASSERT_LESS_OR_EQUAL_UINT64(map[i].size, map[i].anonsz);
		}
		if (map[i].vaddr == mid) {
			/* the read-only piece cannot merge with a neighbour: exactly its own four pages */
			TEST_ASSERT_EQUAL_UINT64(page_size * 4, map[i].size);
			TEST_ASSERT_EQUAL_UINT64(page_size * 4, map[i].anonsz);
			found = 1;
		}
	}
	free(map);
	TEST_ASSERT_TRUE(found);

	TEST_ASSERT_EQUAL_INT(0, munmap(area, page_size * pages));
}


TEST_GROUP_RUNNER(test_mprotect)
{
	RUN_TEST_CASE(test_mprotect, test_mprotect_singlecore);
	RUN_TEST_CASE(test_mprotect, pages_in_child_copied);
	RUN_TEST_CASE(test_mprotect, pages_in_parent_copied);
	RUN_TEST_CASE(test_mprotect, unaligned_len_covers_last_page);
	RUN_TEST_CASE(test_mprotect, unaligned_len_stops_at_last_page);
	RUN_TEST_CASE(test_mprotect, args);
	RUN_TEST_CASE(test_mprotect, meminfo_anonsz_of_split_entries);
}


static void runner(void)
{
	RUN_TEST_GROUP(test_mprotect);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
