/*
 * Phoenix-RTOS
 *
 * phoenix-rtos-test
 *
 * File object cache tests
 *
 * The kernel may keep the pages of a file object after its last mapping is gone, so the next
 * mapping of the file finds them in memory (vm/object.c, "File object cache"). Checked here, on
 * the filesystem under test (default /root, i.e. the NFS root when netbooted -- NOT /tmp, a RAM
 * filesystem whose reads cost about as much as a cache hit):
 *   - second_mapping_is_fast: mapping a file again after unmapping it is much faster than the
 *     first time. Fails on a kernel without the cache: every mapping reads the file again (on the
 *     NFS root ~3 ms per 64 KiB read-ahead cluster);
 *   - a mapping always shows the file's current content, whatever was cached: after a rewrite of
 *     the same size, a rewrite of one page, a rewrite to a smaller and to a larger size, a
 *     replacement by rename() and a removal and re-creation under the same name;
 *   - memory_pressure_evicts_cache: cached pages count as free memory and are given up when it
 *     runs out -- a child process can allocate and touch (almost) all of the free memory meminfo
 *     reports, cached file included, and the file still reads back correctly afterwards.
 *
 * Environment: OBJCACHE_DIR (directory of the test file, default /root), OBJCACHE_MIB (size of
 * the test file in MiB, default 96).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "unity_fixture.h"


#define MIB         (1024UL * 1024UL)
#define DEFAULT_MIB 96UL

/* A cache hit maps a resident page; a miss reads it from the server: expect at least this ratio */
#define WARM_SPEEDUP 3ULL

/* The memory pressure child maps its memory in pieces of this size */
#define CHUNK (64UL * MIB)


static struct {
	char path[256];
	char tmppath[256];
	size_t size;
	size_t pagesz;
	unsigned char *buf;
} common;


/* The 8-byte word of the file written with seed at file offset offs (a multiple of 8) */
static uint64_t pattern(uint64_t offs, uint32_t seed)
{
	uint64_t x = (offs >> 3) * 0x9e3779b97f4a7c15ULL + ((uint64_t)seed << 32) + seed;

	x ^= x >> 29;
	x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 32;

	return x;
}


static uint8_t patternByte(uint64_t offs, uint32_t seed)
{
	return (uint8_t)(pattern(offs & ~7ULL, seed) >> ((offs & 7U) * 8U));
}


static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ((uint64_t)ts.tv_sec * 1000000ULL) + ((uint64_t)ts.tv_nsec / 1000ULL);
}


static void fill(unsigned char *buf, size_t len, uint64_t offs, uint32_t seed)
{
	size_t i;

	for (i = 0; i < len; ++i) {
		buf[i] = patternByte(offs + i, seed);
	}
}


/* Writes [offs, offs + len) of path with the pattern of seed */
static void writeRange(const char *path, int flags, uint64_t offs, size_t len, uint32_t seed)
{
	size_t done = 0, chunk;
	ssize_t r;
	int fd;

	fd = open(path, O_WRONLY | flags, 0644);
	TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, fd, strerror(errno));

	while (done < len) {
		chunk = ((len - done) < MIB) ? (len - done) : MIB;
		fill(common.buf, chunk, offs + done, seed);
		r = pwrite(fd, common.buf, chunk, (off_t)(offs + done));
		TEST_ASSERT_EQUAL_INT64_MESSAGE((int64_t)chunk, (int64_t)r, strerror(errno));
		done += chunk;
	}

	TEST_ASSERT_EQUAL_INT(0, close(fd));
}


static void writeFile(const char *path, size_t len, uint32_t seed)
{
	writeRange(path, O_CREAT | O_TRUNC, 0, len, seed);
}


/* Seed of every page of the file, but one page that was rewritten with another */
typedef struct {
	uint32_t seed;
	size_t page;      /* index of the rewritten page, or (size_t)-1 */
	uint32_t pageSeed;
} expect_t;


static uint8_t expectByte(const expect_t *e, uint64_t offs)
{
	return patternByte(offs, ((offs / common.pagesz) == e->page) ? e->pageSeed : e->seed);
}


/*
 * Maps the len bytes of path, reads one byte at a different position of every page (and every
 * byte of the last page, whose tail past the end of the file must read as zeros), unmaps it.
 * Returns the number of wrong bytes; *us is the time from mmap() to munmap().
 */
static unsigned int mapCheck(const char *path, size_t len, const expect_t *e, uint64_t *us)
{
	const volatile unsigned char *p;
	unsigned int bad = 0;
	size_t pages, i, j, offs;
	uint64_t start;
	void *m;
	int fd;

	pages = (len + common.pagesz - 1U) / common.pagesz;

	fd = open(path, O_RDONLY);
	TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, fd, strerror(errno));

	start = now_us();

	m = mmap(NULL, pages * common.pagesz, PROT_READ, MAP_PRIVATE, fd, 0);
	TEST_ASSERT_TRUE_MESSAGE(m != MAP_FAILED, strerror(errno));
	p = m;

	for (i = 0; i < pages; ++i) {
		offs = (i * common.pagesz) + ((i * 8U + 3U) % common.pagesz);
		if (offs >= len) {
			offs = i * common.pagesz;
		}
		if (p[offs] != expectByte(e, offs)) {
			bad++;
		}
	}

	/* The last page: the end of the file, then zeros */
	for (j = (pages - 1U) * common.pagesz; j < pages * common.pagesz; ++j) {
		if (p[j] != ((j < len) ? expectByte(e, j) : 0U)) {
			bad++;
		}
	}

	TEST_ASSERT_EQUAL_INT(0, munmap(m, pages * common.pagesz));

	*us = now_us() - start;

	TEST_ASSERT_EQUAL_INT(0, close(fd));

	return bad;
}


static unsigned long long freeBytes(void)
{
	meminfo_t info;

	memset(&info, 0, sizeof(info));
	info.page.mapsz = -1;
	info.entry.mapsz = -1;
	info.entry.kmapsz = -1;
	info.maps.mapsz = -1;
	meminfo(&info);

	return (unsigned long long)info.page.free;
}


TEST_GROUP(test_objcache);


TEST_SETUP(test_objcache)
{
	const char *dir = getenv("OBJCACHE_DIR");
	const char *mib = getenv("OBJCACHE_MIB");
	unsigned long n = DEFAULT_MIB;

	if ((dir == NULL) || (dir[0] == '\0')) {
		dir = "/root";
	}
	if ((mib != NULL) && (mib[0] != '\0')) {
		n = strtoul(mib, NULL, 10);
		if (n == 0UL) {
			n = DEFAULT_MIB;
		}
	}

	snprintf(common.path, sizeof(common.path), "%s/objcache_test.bin", dir);
	snprintf(common.tmppath, sizeof(common.tmppath), "%s/objcache_test.tmp", dir);
	common.size = n * MIB;
	common.pagesz = (size_t)sysconf(_SC_PAGESIZE);

	common.buf = malloc(MIB);
	TEST_ASSERT_NOT_NULL(common.buf);
}


TEST_TEAR_DOWN(test_objcache)
{
	free(common.buf);
	common.buf = NULL;
	unlink(common.path);
	unlink(common.tmppath);
}


TEST(test_objcache, second_mapping_is_fast)
{
	expect_t e = { .seed = 1, .page = (size_t)-1 };
	uint64_t cold, warm;

	writeFile(common.path, common.size, e.seed);

	TEST_ASSERT_EQUAL_UINT(0, mapCheck(common.path, common.size, &e, &cold));
	TEST_ASSERT_EQUAL_UINT(0, mapCheck(common.path, common.size, &e, &warm));

	printf("objcache: %s, %zu MiB: first mapping %llu ms, second %llu ms\n", common.path,
		common.size / MIB, (unsigned long long)(cold / 1000U), (unsigned long long)(warm / 1000U));

	TEST_ASSERT_TRUE_MESSAGE((warm * WARM_SPEEDUP) < cold, "the second mapping read the file again");
}


TEST(test_objcache, rewrite_same_size_is_seen)
{
	expect_t e = { .seed = 1, .page = (size_t)-1 };
	uint64_t us;

	writeFile(common.path, common.size, e.seed);
	TEST_ASSERT_EQUAL_UINT(0, mapCheck(common.path, common.size, &e, &us));

	/* Same size, same second: neither size nor mtime tells the versions apart */
	e.seed = 2;
	writeRange(common.path, 0, 0, common.size, e.seed);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mapCheck(common.path, common.size, &e, &us), "the old content was mapped");

	/* One page in the middle */
	e.page = (common.size / common.pagesz) / 2U;
	e.pageSeed = 3;
	writeRange(common.path, 0, (uint64_t)e.page * common.pagesz, common.pagesz, e.pageSeed);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mapCheck(common.path, common.size, &e, &us), "the old page was mapped");
}


TEST(test_objcache, rewrite_other_size_is_seen)
{
	expect_t e = { .seed = 1, .page = (size_t)-1 };
	size_t smaller = (common.size / 2U) + 5000U, larger = common.size + (2U * common.pagesz) + 123U;
	uint64_t us;

	writeFile(common.path, common.size, e.seed);
	TEST_ASSERT_EQUAL_UINT(0, mapCheck(common.path, common.size, &e, &us));

	e.seed = 4;
	writeFile(common.path, smaller, e.seed);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mapCheck(common.path, smaller, &e, &us), "wrong content after shrinking");

	e.seed = 5;
	writeFile(common.path, larger, e.seed);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mapCheck(common.path, larger, &e, &us), "wrong content after growing");
}


TEST(test_objcache, replaced_file_is_seen)
{
	expect_t e = { .seed = 1, .page = (size_t)-1 };
	uint64_t us;

	writeFile(common.path, common.size, e.seed);
	TEST_ASSERT_EQUAL_UINT(0, mapCheck(common.path, common.size, &e, &us));

	/* Written beside it and renamed over it */
	e.seed = 6;
	writeFile(common.tmppath, common.size, e.seed);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, rename(common.tmppath, common.path), strerror(errno));
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mapCheck(common.path, common.size, &e, &us), "the renamed-over file was mapped");

	/* Removed and created again under the same name (a filesystem may reuse the id) */
	e.seed = 7;
	TEST_ASSERT_EQUAL_INT(0, unlink(common.path));
	writeFile(common.path, common.size, e.seed);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, mapCheck(common.path, common.size, &e, &us), "the removed file was mapped");
}


TEST(test_objcache, memory_pressure_evicts_cache)
{
	expect_t e = { .seed = 8, .page = (size_t)-1 };
	unsigned long long freesz, margin, target, done;
	uint64_t us, start;
	unsigned char *m;
	size_t i, chunk;
	pid_t pid;
	int status;

	writeFile(common.path, common.size, e.seed);
	TEST_ASSERT_EQUAL_UINT(0, mapCheck(common.path, common.size, &e, &us));

	/* With the cache, the file's pages are now kept, and meminfo counts them as free. Leave less
	 * than the file's size to everybody else: the child can only get its memory if the cached
	 * pages are given up. */
	freesz = freeBytes();
	margin = common.size / 2U;
	TEST_ASSERT_GREATER_THAN_UINT64_MESSAGE(margin + common.size, freesz, "not enough free memory for the test");
	target = (freesz - margin) & ~((unsigned long long)common.pagesz - 1ULL);

	printf("objcache: free %llu MiB, child allocates and touches %llu MiB\n", freesz / MIB, target / MIB);

	start = now_us();
	pid = fork();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);
	if (pid == 0) {
		/* In chunks: an entry's amap covers the whole entry, and one for gigabytes would be a
		 * multi-megabyte contiguous kernel allocation, failing for reasons of its own */
		for (done = 0; done < target; done += chunk) {
			chunk = ((target - done) < CHUNK) ? (size_t)(target - done) : CHUNK;
			m = mmap(NULL, chunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (m == MAP_FAILED) {
				_exit(2);
			}
			/* A page that cannot be had kills the child at the fault */
			for (i = 0; i < chunk; i += common.pagesz) {
				m[i] = (unsigned char)((done + i) / common.pagesz);
			}
			for (i = 0; i < chunk; i += common.pagesz) {
				if (m[i] != (unsigned char)((done + i) / common.pagesz)) {
					_exit(3);
				}
			}
		}
		_exit(0);
	}

	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	printf("objcache: child took %llu ms, status 0x%x\n", (unsigned long long)((now_us() - start) / 1000U), (unsigned int)status);
	TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(status), "the child was killed: an allocation failed");
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, WEXITSTATUS(status), "the child could not get its memory");

	/* Whether the file was evicted or not, it reads back right */
	TEST_ASSERT_EQUAL_UINT(0, mapCheck(common.path, common.size, &e, &us));
}


TEST_GROUP_RUNNER(test_objcache)
{
	RUN_TEST_CASE(test_objcache, second_mapping_is_fast);
	RUN_TEST_CASE(test_objcache, rewrite_same_size_is_seen);
	RUN_TEST_CASE(test_objcache, rewrite_other_size_is_seen);
	RUN_TEST_CASE(test_objcache, replaced_file_is_seen);
	RUN_TEST_CASE(test_objcache, memory_pressure_evicts_cache);
}


static void runner(void)
{
	RUN_TEST_GROUP(test_objcache);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
