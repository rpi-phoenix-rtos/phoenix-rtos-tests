/*
 * Phoenix-RTOS
 *
 * phoenix-rtos-test
 *
 * Demand-zero anonymous memory tests
 *
 * A plain anonymous mapping (no MAP_PHYSMEM, MAP_CONTIGUOUS, MAP_UNCACHED or MAP_DEVICE) only
 * reserves address space: a page is allocated, zeroed and counted in meminfo's anonsz on its first
 * access. The tests check both halves of that -- reserving costs nothing, touching costs exactly
 * the touched pages -- and the paths that used to rely on every page being present: fork (COW of a
 * partly touched region), mprotect and munmap of pages never touched, a file read into and a write
 * from untouched memory (the kernel maps the buffer's frames into the file server), meminfo into
 * an untouched buffer (the kernel writes it holding the map lock), a signal delivered on an
 * untouched thread stack and alternate stack (the frame is written under the scheduler lock), and
 * va2pa() of an untouched page.
 *
 * The tests that look at memory use fail on a kernel that maps every anonymous page at mmap()
 * time; the others pass there too and guard the demand-zero paths.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "unity_fixture.h"


#define RESERVE_SIZE (64UL * 1024UL * 1024UL)
#define BSS_SIZE     (8UL * 1024UL * 1024UL)
#define STACK_SIZE   (64UL * 1024UL)


static size_t page_size;
static const char *filename = "./mmap_demand_zero_file";

/* Never touched, except by bss_is_demand_zero(): an 8 MiB .bss the process should not pay for */
static unsigned char big_bss[BSS_SIZE] __attribute__((aligned(4096)));


TEST_GROUP(test_mmap_demand_zero);


TEST_SETUP(test_mmap_demand_zero)
{
	page_size = (size_t)sysconf(_SC_PAGESIZE);
}


TEST_TEAR_DOWN(test_mmap_demand_zero)
{
	unlink(filename);
}


static unsigned long long free_bytes(void)
{
	meminfo_t info;

	memset(&info, 0, sizeof(info));
	info.page.mapsz = -1;
	info.entry.mapsz = -1;
	info.entry.kmapsz = -1;
	info.maps.mapsz = -1;
	meminfo(&info);

	return info.page.free;
}


/* This process's map entries, into a buffer from malloc() */
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
		memset(map, 0, (size_t)mapsz * sizeof(*map));
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


/* The entry holding addr; it may also hold neighbouring memory it merged with */
static int entry_of(const void *addr, entryinfo_t *out)
{
	entryinfo_t *map;
	int count, i, found = 0;

	map = self_entries(&count);
	if (map == NULL) {
		return -1;
	}

	for (i = 0; i < count; i++) {
		if (((uintptr_t)addr >= (uintptr_t)map[i].vaddr) && ((uintptr_t)addr < ((uintptr_t)map[i].vaddr + map[i].size))) {
			*out = map[i];
			found = 1;
			break;
		}
	}
	free(map);

	return (found != 0) ? 0 : -1;
}


/* Resident anonymous memory of the entries inside [addr, addr + size), (size_t)-1 on error.
 * Asserts nothing, so a forked child can use it. */
static size_t anon_in_raw(const void *addr, size_t size)
{
	entryinfo_t *map;
	int count, i;
	size_t sum = 0;

	map = self_entries(&count);
	if (map == NULL) {
		return (size_t)-1;
	}

	for (i = 0; i < count; i++) {
		if (((uintptr_t)map[i].vaddr >= (uintptr_t)addr) &&
				(((uintptr_t)map[i].vaddr + map[i].size) <= ((uintptr_t)addr + size))) {
			/* An anonymous entry always reports its resident memory, if only 0 */
			if (map[i].anonsz == (size_t)-1) {
				sum = (size_t)-1;
				break;
			}
			sum += map[i].anonsz;
		}
	}
	free(map);

	return sum;
}


static size_t anon_in(const void *addr, size_t size)
{
	size_t sum = anon_in_raw(addr, size);

	TEST_ASSERT_NOT_EQUAL_UINT64((size_t)-1, sum);

	return sum;
}


/* pages of fresh anonymous memory in an entry of their own: an inaccessible page on either side
 * keeps it from merging with its neighbours, so meminfo reports its anonsz alone */
static unsigned char *isolated_map(size_t pages)
{
	unsigned char *base = mmap(NULL, (pages + 2U) * page_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	TEST_ASSERT(base != MAP_FAILED);
	TEST_ASSERT_EQUAL_INT(0, mprotect(base, page_size, PROT_NONE));
	TEST_ASSERT_EQUAL_INT(0, mprotect(base + ((pages + 1U) * page_size), page_size, PROT_NONE));

	return base + page_size;
}


static void isolated_unmap(unsigned char *area, size_t pages)
{
	TEST_ASSERT_EQUAL_INT(0, munmap(area - page_size, (pages + 2U) * page_size));
}


static int page_is(const volatile unsigned char *p, unsigned char val)
{
	size_t i;

	for (i = 0; i < page_size; i++) {
		if (p[i] != val) {
			return 0;
		}
	}

	return 1;
}


TEST(test_mmap_demand_zero, reserve_costs_no_memory)
{
	unsigned long long before, after, consumed;
	entryinfo_t e;
	unsigned char *area;

	before = free_bytes();
	area = mmap(NULL, RESERVE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	TEST_ASSERT(area != MAP_FAILED);
	after = free_bytes();

	/* Other processes allocate meanwhile: allow them an eighth of the reservation */
	consumed = (before > after) ? (before - after) : 0U;
	TEST_ASSERT_LESS_THAN_UINT64(RESERVE_SIZE / 8U, consumed);

	/* The entry may have merged with a neighbour; none of the 64 MiB is resident */
	TEST_ASSERT_EQUAL_INT(0, entry_of(area, &e));
	TEST_ASSERT_GREATER_OR_EQUAL_UINT64(RESERVE_SIZE, e.size);
	TEST_ASSERT_LESS_OR_EQUAL_UINT64(e.size - RESERVE_SIZE, e.anonsz);

	TEST_ASSERT_EQUAL_INT(0, munmap(area, RESERVE_SIZE));
}


TEST(test_mmap_demand_zero, touched_pages_counted_and_zero)
{
	const size_t pages = 64, written[] = { 0, 5, 17, 63 }, readOnly = 30;
	volatile unsigned char *area = isolated_map(pages);
	size_t i;

	TEST_ASSERT_EQUAL_UINT64(0, anon_in((void *)area, pages * page_size));

	for (i = 0; i < sizeof(written) / sizeof(written[0]); i++) {
		TEST_ASSERT_TRUE(page_is(area + (written[i] * page_size), 0));
		memset((void *)(area + (written[i] * page_size)), (int)(0x10U + i), page_size);
	}
	/* A read allocates the page too (there is no shared zero page) */
	TEST_ASSERT_TRUE(page_is(area + (readOnly * page_size), 0));

	TEST_ASSERT_EQUAL_UINT64((sizeof(written) / sizeof(written[0]) + 1U) * page_size, anon_in((void *)area, pages * page_size));

	for (i = 0; i < sizeof(written) / sizeof(written[0]); i++) {
		TEST_ASSERT_TRUE(page_is(area + (written[i] * page_size), (unsigned char)(0x10U + i)));
	}
	/* A page next to a written one is still zero */
	TEST_ASSERT_TRUE(page_is(area + (6U * page_size), 0));

	isolated_unmap((unsigned char *)area, pages);
}


TEST(test_mmap_demand_zero, munmap_untouched)
{
	const size_t pages = 16;
	unsigned long long before, after, consumed;
	volatile unsigned char *area = isolated_map(pages), *hole;
	unsigned char *big;
	size_t i;

	area[0] = 0x31;
	area[15U * page_size] = 0x32;

	/* Pages 4..11 were never touched */
	hole = area + (4U * page_size);
	TEST_ASSERT_EQUAL_INT(0, munmap((void *)hole, 8U * page_size));

	area[2U * page_size] = 0x33;
	area[13U * page_size] = 0x34;
	TEST_ASSERT_EQUAL_UINT64(4U * page_size, anon_in((void *)area, pages * page_size));

	/* Map the hole again: its pages are new and zero */
	TEST_ASSERT(mmap((void *)hole, 8U * page_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0) == (void *)hole);
	for (i = 0; i < 8U; i++) {
		TEST_ASSERT_TRUE(page_is(hole + (i * page_size), 0));
	}
	TEST_ASSERT_EQUAL_INT(0x31, area[0]);
	TEST_ASSERT_EQUAL_INT(0x33, area[2U * page_size]);
	TEST_ASSERT_EQUAL_INT(0x34, area[13U * page_size]);
	TEST_ASSERT_EQUAL_INT(0x32, area[15U * page_size]);

	isolated_unmap((unsigned char *)area, pages);

	/* A big, partly touched mapping gives its memory back */
	before = free_bytes();
	big = mmap(NULL, RESERVE_SIZE / 2U, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	TEST_ASSERT(big != MAP_FAILED);
	for (i = 0; i < 16U; i++) {
		big[i * 64U * page_size] = 1;
	}
	TEST_ASSERT_EQUAL_INT(0, munmap(big, RESERVE_SIZE / 2U));
	after = free_bytes();
	consumed = (before > after) ? (before - after) : 0U;
	TEST_ASSERT_LESS_THAN_UINT64(RESERVE_SIZE / 8U, consumed);
}


TEST(test_mmap_demand_zero, mprotect_untouched)
{
	const size_t pages = 8;
	volatile unsigned char *area = isolated_map(pages);

	/* Read-only, never touched: reads see zero */
	TEST_ASSERT_EQUAL_INT(0, mprotect((void *)area, pages * page_size, PROT_READ));
	TEST_ASSERT_TRUE(page_is(area + page_size, 0));

	TEST_ASSERT_EQUAL_INT(0, mprotect((void *)area, pages * page_size, PROT_READ | PROT_WRITE));
	area[page_size + 1U] = 0x41;
	area[2U * page_size] = 0x42;

	/* Inaccessible and back, never touched in between */
	TEST_ASSERT_EQUAL_INT(0, mprotect((void *)(area + (4U * page_size)), 2U * page_size, PROT_NONE));
	TEST_ASSERT_EQUAL_INT(0, mprotect((void *)(area + (4U * page_size)), 2U * page_size, PROT_READ | PROT_WRITE));
	TEST_ASSERT_TRUE(page_is(area + (4U * page_size), 0));
	area[4U * page_size] = 0x43;

	TEST_ASSERT_EQUAL_INT(0x41, area[page_size + 1U]);
	TEST_ASSERT_EQUAL_INT(0x42, area[2U * page_size]);
	TEST_ASSERT_EQUAL_INT(0x43, area[4U * page_size]);

	/* Pages 1, 2 and 4, whichever entries the area is split into */
	TEST_ASSERT_EQUAL_UINT64(3U * page_size, anon_in((void *)area, pages * page_size));

	isolated_unmap((unsigned char *)area, pages);
}


/* Pages 0..7 of 16 written; the child sees them, sees zero in the rest, has only the 8 written
 * pages copied, and its writes stay its own */
TEST(test_mmap_demand_zero, fork_partly_touched)
{
	const size_t pages = 16;
	volatile unsigned char *area = isolated_map(pages);
	size_t i;
	int status;
	pid_t pid;

	for (i = 0; i < 8U; i++) {
		memset((void *)(area + (i * page_size)), (int)(0x50U + i), page_size);
	}

	pid = fork();
	TEST_ASSERT(pid >= 0);
	if (pid == 0) {
		if (anon_in_raw((void *)area, pages * page_size) != (8U * page_size)) {
			_exit(2);
		}
		for (i = 0; i < pages; i++) {
			if (page_is(area + (i * page_size), (i < 8U) ? (unsigned char)(0x50U + i) : 0U) == 0) {
				_exit(3);
			}
		}
		for (i = 4; i < 12U; i++) {
			memset((void *)(area + (i * page_size)), 0xc0, page_size);
		}
		for (i = 4; i < 12U; i++) {
			if (page_is(area + (i * page_size), 0xc0) == 0) {
				_exit(4);
			}
		}
		_exit(0);
	}

	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	TEST_ASSERT_TRUE(WIFEXITED(status));
	/* 2: the child got more than the written pages, 3: wrong contents, 4: its writes failed */
	TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

	for (i = 0; i < pages; i++) {
		TEST_ASSERT_TRUE(page_is(area + (i * page_size), (i < 8U) ? (unsigned char)(0x50U + i) : 0U));
	}

	isolated_unmap((unsigned char *)area, pages);
}


/* After fork, one process faults in a page neither had touched, the other then writes it: the
 * first must not see that write. A page zero-filled into the amap the two still share would be
 * shared too. prot is the protection of the page while the first process reads it. */
static void fork_first_touch(int prot, int parentReadsFirst)
{
	const size_t pages = 4, page = 2;
	volatile unsigned char *area = isolated_map(pages);
	int toChild[2], toParent[2], status, failed = 0;
	char c = 0;
	pid_t pid;

	/* Page 0 has data, so the area has an amap, which fork() shares -- but no page is resident
	 * (page 0 is inaccessible), so the child copies none at fork() and keeps sharing it */
	area[0] = 0x11;
	TEST_ASSERT_EQUAL_INT(0, mprotect((void *)area, page_size, PROT_NONE));
	TEST_ASSERT_EQUAL_INT(0, mprotect((void *)(area + page_size), (pages - 1U) * page_size, prot));

	TEST_ASSERT_EQUAL_INT(0, pipe(toChild));
	TEST_ASSERT_EQUAL_INT(0, pipe(toParent));

	pid = fork();
	TEST_ASSERT(pid >= 0);
	if (pid == 0) {
		if (parentReadsFirst != 0) {
			/* Write once the parent has read */
			if ((read(toChild[0], &c, 1) != 1) || (mprotect((void *)area, pages * page_size, PROT_READ | PROT_WRITE) != 0)) {
				_exit(2);
			}
			area[page * page_size] = 0x77;
			if ((area[page * page_size] != 0x77) || (write(toParent[1], &c, 1) != 1)) {
				_exit(3);
			}
			_exit(0);
		}

		/* Read, let the parent write, read again */
		if ((page_is(area + (page * page_size), 0) == 0) || (write(toParent[1], &c, 1) != 1) || (read(toChild[0], &c, 1) != 1)) {
			_exit(2);
		}
		_exit((page_is(area + (page * page_size), 0) != 0) ? 0 : 4);
	}

	if (parentReadsFirst != 0) {
		failed = (page_is(area + (page * page_size), 0) == 0) ? 1 : 0;
		TEST_ASSERT_EQUAL_INT(1, write(toChild[1], &c, 1));
		TEST_ASSERT_EQUAL_INT(1, read(toParent[0], &c, 1));
		/* The child's write must not show here */
		if (page_is(area + (page * page_size), 0) == 0) {
			failed = 1;
		}
	}
	else {
		TEST_ASSERT_EQUAL_INT(1, read(toParent[0], &c, 1));
		TEST_ASSERT_EQUAL_INT(0, mprotect((void *)area, pages * page_size, PROT_READ | PROT_WRITE));
		area[page * page_size] = 0x55;
		TEST_ASSERT_EQUAL_INT(1, write(toChild[1], &c, 1));
	}

	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	TEST_ASSERT_TRUE(WIFEXITED(status));
	/* 4: the child saw the parent's write */
	TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
	TEST_ASSERT_FALSE_MESSAGE(failed != 0, "the parent saw the child's write");

	close(toChild[0]);
	close(toChild[1]);
	close(toParent[0]);
	close(toParent[1]);

	/* The page that was inaccessible across fork() kept its data */
	TEST_ASSERT_EQUAL_INT(0, mprotect((void *)area, pages * page_size, PROT_READ | PROT_WRITE));
	TEST_ASSERT_EQUAL_INT(0x11, area[0]);
	isolated_unmap((unsigned char *)area, pages);
}


TEST(test_mmap_demand_zero, fork_first_touch_writable)
{
	fork_first_touch(PROT_READ | PROT_WRITE, 1);
	fork_first_touch(PROT_READ | PROT_WRITE, 0);
}


TEST(test_mmap_demand_zero, fork_first_touch_readonly)
{
	fork_first_touch(PROT_READ, 1);
	fork_first_touch(PROT_READ, 0);
}


/* The kernel hands the file server the frames of the caller's buffer: untouched ones too */
TEST(test_mmap_demand_zero, file_io_through_untouched_buffer)
{
	const size_t len = (3U * 4096U) + 123U, at = 4096U - 100U, span = 1024U * 1024U;
	unsigned char *pattern, *buf;
	size_t i;
	int fd;

	pattern = malloc(len);
	TEST_ASSERT_NOT_NULL(pattern);
	for (i = 0; i < len; i++) {
		pattern[i] = (unsigned char)((i * 7U) + 1U);
	}

	fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
	TEST_ASSERT(fd >= 0);
	TEST_ASSERT_EQUAL_INT((int)len, (int)write(fd, pattern, len));

	/* Read at a page-crossing offset: the first and last pages are partial */
	buf = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	TEST_ASSERT(buf != MAP_FAILED);
	TEST_ASSERT_EQUAL_INT64(0, lseek(fd, 0, SEEK_SET));
	TEST_ASSERT_EQUAL_INT((int)len, (int)read(fd, buf + at, len));
	TEST_ASSERT_EQUAL_MEMORY(pattern, buf + at, len);
	for (i = 0; i < at; i++) {
		TEST_ASSERT_EQUAL_HEX8(0, buf[i]);
	}
	for (i = at + len; i < (at + len + page_size); i++) {
		TEST_ASSERT_EQUAL_HEX8(0, buf[i]);
	}

	/* Write from memory never touched: the file gets zeros */
	TEST_ASSERT_EQUAL_INT64(0, lseek(fd, 0, SEEK_SET));
	TEST_ASSERT_EQUAL_INT((int)len, (int)write(fd, buf + (span / 2U) + 50U, len));
	memset(pattern, 0xff, len);
	TEST_ASSERT_EQUAL_INT64(0, lseek(fd, 0, SEEK_SET));
	TEST_ASSERT_EQUAL_INT((int)len, (int)read(fd, pattern, len));
	for (i = 0; i < len; i++) {
		TEST_ASSERT_EQUAL_HEX8(0, pattern[i]);
	}

	TEST_ASSERT_EQUAL_INT(0, close(fd));
	TEST_ASSERT_EQUAL_INT(0, munmap(buf, span));
	free(pattern);
}


/* The kernel fills meminfo's entry table holding this process's map lock: it must not fault */
TEST(test_mmap_demand_zero, meminfo_into_untouched_buffer)
{
	const size_t pages = 16;
	entryinfo_t *map = mmap(NULL, pages * page_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	meminfo_t info;
	int i, found = 0;

	TEST_ASSERT(map != MAP_FAILED);

	memset(&info, 0, sizeof(info));
	info.page.mapsz = -1;
	info.maps.mapsz = -1;
	info.entry.kmapsz = -1;
	info.entry.pid = (unsigned int)getpid();
	info.entry.mapsz = (int)((pages * page_size) / sizeof(*map));
	info.entry.map = map;
	meminfo(&info);

	TEST_ASSERT_GREATER_THAN_INT(0, info.entry.mapsz);
	TEST_ASSERT_LESS_OR_EQUAL_INT((int)((pages * page_size) / sizeof(*map)), info.entry.mapsz);
	for (i = 0; i < info.entry.mapsz; i++) {
		if (((uintptr_t)map >= (uintptr_t)map[i].vaddr) && ((uintptr_t)map < ((uintptr_t)map[i].vaddr + map[i].size))) {
			found = 1;
		}
	}
	TEST_ASSERT_TRUE(found);

	TEST_ASSERT_EQUAL_INT(0, munmap(map, pages * page_size));
}


static volatile sig_atomic_t signal_seen;
static volatile uintptr_t signal_sp;


static void signal_handler(int sig)
{
	volatile int local = sig;

	signal_sp = (uintptr_t)&local;
	signal_seen = 1;
}


static void *signal_thread(void *arg)
{
	stack_t *alt = arg;
	struct sigaction sa;
	int i;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	if (alt != NULL) {
		if (sigaltstack(alt, NULL) != 0) {
			return (void *)1;
		}
		sa.sa_flags = SA_ONSTACK;
	}
	if (sigaction(SIGUSR1, &sa, NULL) != 0) {
		return (void *)2;
	}

	signal_seen = 0;
	if (pthread_kill(pthread_self(), SIGUSR1) != 0) {
		return (void *)3;
	}
	for (i = 0; (i < 100) && (signal_seen == 0); i++) {
		usleep(10000);
	}

	return (signal_seen != 0) ? NULL : (void *)4;
}


/* The scheduler writes a signal frame into the thread's stack holding its spinlock, where a
 * page fault cannot be served. A fresh mmap()ed stack has never been touched below the frame. */
TEST(test_mmap_demand_zero, signal_on_untouched_thread_stack)
{
	pthread_attr_t attr;
	pthread_t tid;
	void *ret = (void *)-1;
	unsigned char *stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	TEST_ASSERT(stack != MAP_FAILED);
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_setstack(&attr, stack, STACK_SIZE));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&tid, &attr, signal_thread, NULL));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(tid, &ret));
	TEST_ASSERT_NULL(ret);
	TEST_ASSERT_EQUAL_INT(0, pthread_attr_destroy(&attr));

	signal(SIGUSR1, SIG_DFL);
	TEST_ASSERT_EQUAL_INT(0, munmap(stack, STACK_SIZE));
}


TEST(test_mmap_demand_zero, signal_on_untouched_altstack)
{
	pthread_t tid;
	stack_t alt;
	void *ret = (void *)-1;
	unsigned char *mem = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	TEST_ASSERT(mem != MAP_FAILED);
	alt.ss_sp = mem;
	alt.ss_size = STACK_SIZE;
	alt.ss_flags = 0;

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&tid, NULL, signal_thread, &alt));
	TEST_ASSERT_EQUAL_INT(0, pthread_join(tid, &ret));
	if (ret == (void *)1) {
		TEST_IGNORE_MESSAGE("sigaltstack() not supported");
	}
	TEST_ASSERT_NULL(ret);
	/* The handler did run on the alternate stack */
	TEST_ASSERT_TRUE((signal_sp >= (uintptr_t)mem) && (signal_sp < ((uintptr_t)mem + STACK_SIZE)));

	signal(SIGUSR1, SIG_DFL);
	TEST_ASSERT_EQUAL_INT(0, munmap(mem, STACK_SIZE));
}


/* va2pa() of a page never touched gives the frame the page then keeps -- and only that page
 * gets one */
TEST(test_mmap_demand_zero, va2pa_untouched_page)
{
	volatile unsigned char *area = isolated_map(2);
	addr_t pa;

	pa = va2pa((void *)(area + page_size));
	TEST_ASSERT_NOT_EQUAL_UINT64(0, (uint64_t)pa);
	TEST_ASSERT_EQUAL_UINT64(page_size, anon_in((void *)area, 2U * page_size));

	area[page_size] = 0x5a;
	TEST_ASSERT_EQUAL_UINT64((uint64_t)pa, (uint64_t)va2pa((void *)(area + page_size)));
	TEST_ASSERT_EQUAL_UINT64((uint64_t)pa + 0x10U, (uint64_t)va2pa((void *)(area + page_size + 0x10U)));
	TEST_ASSERT_EQUAL_UINT64(page_size, anon_in((void *)area, 2U * page_size));

	isolated_unmap((unsigned char *)area, 2);
}


/* .bss past the file-backed part of the data segment is plain anonymous memory too */
TEST(test_mmap_demand_zero, bss_is_demand_zero)
{
	entryinfo_t e;

	TEST_ASSERT_EQUAL_INT(0, entry_of(big_bss + (BSS_SIZE / 2U), &e));
	TEST_ASSERT_NOT_EQUAL_UINT64((size_t)-1, e.anonsz);
	/* The entry also holds the rest of .bss and may have merged with the heap: half the array
	 * is a generous bound for those, and an eagerly mapped .bss holds all of it */
	TEST_ASSERT_LESS_THAN_UINT64(BSS_SIZE / 2U, e.anonsz);

	TEST_ASSERT_EQUAL_HEX8(0, big_bss[BSS_SIZE / 2U]);
	TEST_ASSERT_EQUAL_HEX8(0, big_bss[BSS_SIZE - 1U]);
}


TEST_GROUP_RUNNER(test_mmap_demand_zero)
{
	RUN_TEST_CASE(test_mmap_demand_zero, bss_is_demand_zero);
	RUN_TEST_CASE(test_mmap_demand_zero, reserve_costs_no_memory);
	RUN_TEST_CASE(test_mmap_demand_zero, touched_pages_counted_and_zero);
	RUN_TEST_CASE(test_mmap_demand_zero, munmap_untouched);
	RUN_TEST_CASE(test_mmap_demand_zero, mprotect_untouched);
	RUN_TEST_CASE(test_mmap_demand_zero, fork_partly_touched);
	RUN_TEST_CASE(test_mmap_demand_zero, fork_first_touch_writable);
	RUN_TEST_CASE(test_mmap_demand_zero, fork_first_touch_readonly);
	RUN_TEST_CASE(test_mmap_demand_zero, file_io_through_untouched_buffer);
	RUN_TEST_CASE(test_mmap_demand_zero, meminfo_into_untouched_buffer);
	RUN_TEST_CASE(test_mmap_demand_zero, signal_on_untouched_thread_stack);
	RUN_TEST_CASE(test_mmap_demand_zero, signal_on_untouched_altstack);
	RUN_TEST_CASE(test_mmap_demand_zero, va2pa_untouched_page);
}


static void runner(void)
{
	RUN_TEST_GROUP(test_mmap_demand_zero);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
