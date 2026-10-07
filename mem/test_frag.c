/*
 * Phoenix-RTOS
 *
 * phoenix-rtos-test
 *
 * Physically contiguous memory after a big process
 *
 * A process allocates and touches nearly all free memory (all of it but max(256 MiB, 10%), in
 * 64 MiB pieces) and exits. Then contiguous blocks must be had again (MAP_ANONYMOUS |
 * MAP_CONTIGUOUS, what shmsrv and the GPU driver allocate buffers with): one of 16 MiB, and
 * min(1 GiB, half of what the process had) in blocks of 4 MiB held together. That is more than
 * the memory the process left free, so the blocks must come from the memory it gave back.
 *   - after_big_process: the process alone. Fails on a kernel whose page allocator does not merge
 *     the blocks it frees back into bigger ones (vm/page.c before "merge a freed block with its
 *     buddy at every size": 2.9 GB free and nothing above 32 KiB on the Pi 4 after this).
 *   - beside_kernel_pages_of_another: meanwhile another process takes a page table of its own
 *     after every 2 MiB the big one touches (it maps one page of a file at a new 2 MiB slot of
 *     its address space: a page table, no page of memory) and keeps them. Fails on a kernel that
 *     takes kernel pages from the same blocks as user pages (vm/page.c before "group pages by
 *     mobility"): the page tables end up one per 2 MiB of the memory given back.
 *
 * Environment: FRAG_DIR (directory of the one-page file the second case maps, default /root).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "unity_fixture.h"


#define MIB (1024UL * 1024UL)

/* The big process maps its memory in pieces of this size */
#define CHUNK (64UL * MIB)

/* Address space a page table covers (4 KiB pages, 512 entries): the pinner's slot size */
#define SLOT (2UL * MIB)

/* Free memory the big process leaves to everybody else: at least this, or 10% of it */
#define MARGIN_MIN (256ULL * MIB)

/* Contiguous blocks asked for afterwards */
#define BIG_BLOCK   (16UL * MIB)
#define BLOCK       (4UL * MIB)
#define BLOCKS_MAX  (1024UL * MIB)
#define BLOCKS_SLOT 512U


static struct {
	char path[256];
	size_t pagesz;
	pid_t pinner;
	void *blocks[BLOCKS_SLOT];
} common;


static unsigned long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
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


/* Free memory once it has not changed for a second (a process's memory is given back a while
 * after waitpid() returns: ~800 000 pages and their anon_t take time), at most 30 s; *ms says
 * how long that took */
static unsigned long long freeSettled(unsigned long long *ms)
{
	unsigned long long start = now_ms(), f = freeBytes(), last, stable = start;

	for (;;) {
		usleep(100000);
		last = f;
		f = freeBytes();
		if (f != last) {
			stable = now_ms();
		}
		if (((now_ms() - stable) >= 1000ULL) || ((now_ms() - start) >= 30000ULL)) {
			break;
		}
	}
	*ms = now_ms() - start;

	return f;
}


/* The longest run of free pages, from the kernel's page map (diagnostics) */
static unsigned long long largestFreeRun(void)
{
	static pageinfo_t map[65536];
	meminfo_t info;
	unsigned long long run = 0, best = 0;
	addr_t next = 0;
	int i, n;

	memset(&info, 0, sizeof(info));
	info.page.mapsz = (int)(sizeof(map) / sizeof(map[0]));
	info.page.map = map;
	info.entry.mapsz = -1;
	info.entry.kmapsz = -1;
	info.maps.mapsz = -1;
	meminfo(&info);

	n = (info.page.mapsz < (int)(sizeof(map) / sizeof(map[0]))) ? info.page.mapsz : (int)(sizeof(map) / sizeof(map[0]));
	for (i = 0; i < n; ++i) {
		if ((map[i].marker == '.') && (run != 0U) && (map[i].addr == next)) {
			run += (unsigned long long)map[i].count * common.pagesz;
		}
		else {
			run = (map[i].marker == '.') ? ((unsigned long long)map[i].count * common.pagesz) : 0U;
		}
		next = map[i].addr + ((addr_t)map[i].count * common.pagesz);
		best = (run > best) ? run : best;
	}

	return best;
}


/* Free memory left to everybody else while the big process runs */
static unsigned long long margin(unsigned long long freesz)
{
	unsigned long long m = freesz / 10U;

	return (m < MARGIN_MIN) ? MARGIN_MIN : m;
}


/*
 * In a child: maps len bytes of anonymous memory in pieces and writes every page. With pin >= 0,
 * after every SLOT bytes it writes a byte to pin and waits for one on ack (the pinner takes its
 * page table in between). Returns the child's exit status.
 */
static int bigProcess(unsigned long long len, int pin, int ack)
{
	unsigned long long done;
	unsigned char *m;
	size_t i, chunk = 0;
	char c = 'p';
	pid_t pid;
	int status = -1;

	pid = fork();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);
	if (pid == 0) {
		for (done = 0; done < len; done += chunk) {
			chunk = ((len - done) < CHUNK) ? (size_t)(len - done) : CHUNK;
			m = mmap(NULL, chunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (m == MAP_FAILED) {
				_exit(2);
			}
			for (i = 0; i < chunk; i += common.pagesz) {
				m[i] = (unsigned char)((done + i) / common.pagesz);
				if ((pin >= 0) && (((done + i + common.pagesz) % SLOT) == 0U)) {
					if ((write(pin, &c, 1) != 1) || (read(ack, &c, 1) != 1)) {
						_exit(4);
					}
				}
			}
		}
		_exit(0);
	}

	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));

	return status;
}


/*
 * Starts the pinner: for every byte on cmd, it maps the first page of fd at the next SLOT of a
 * reserved range of its address space, reads it (a page table; the page is the file's) and
 * answers on ack. It keeps everything until killed.
 */
static void pinnerStart(int fd, size_t slots, int cmd, int ack, int cmdw, int ackr)
{
	unsigned char *base, *at;
	size_t n = 0;
	char c;
	pid_t pid;

	pid = fork();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);
	if (pid == 0) {
		(void)close(cmdw);
		(void)close(ackr);
		base = mmap(NULL, (slots + 1U) * SLOT, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED) {
			_exit(2);
		}
		base = (unsigned char *)(((uintptr_t)base + SLOT - 1U) & ~((uintptr_t)SLOT - 1U));

		while (read(cmd, &c, 1) == 1) {
			if (n < slots) {
				/* A pinner that cannot take its page tables would make the test pass vacuously:
				 * it dies instead, and the big process with it (end of file on ack) */
				at = mmap(base + n * SLOT, common.pagesz, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0);
				if ((at != base + n * SLOT) || (*(volatile unsigned char *)at != 0x5aU)) {
					_exit(5);
				}
				n++;
			}
			if (write(ack, &c, 1) != 1) {
				_exit(3);
			}
		}
		/* Keep the page tables until killed */
		for (;;) {
			sleep(1);
		}
	}

	common.pinner = pid;
}


static void pinnerStop(void)
{
	int status;

	if (common.pinner > 0) {
		(void)kill(common.pinner, SIGKILL);
		(void)waitpid(common.pinner, &status, 0);
		common.pinner = 0;
	}
}


/* Takes one BIG_BLOCK and then up to want bytes in BLOCKs, all held together; gives them back */
static void contiguousAfter(unsigned long long want)
{
	void *big;
	size_t n = 0, i, max = (size_t)(want / BLOCK);

	big = mmap(NULL, BIG_BLOCK, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_CONTIGUOUS, -1, 0);

	while ((n < max) && (n < BLOCKS_SLOT)) {
		common.blocks[n] = mmap(NULL, BLOCK, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_CONTIGUOUS, -1, 0);
		if (common.blocks[n] == MAP_FAILED) {
			break;
		}
		n++;
	}

	printf("frag: a %lu MiB block: %s; %zu of %zu blocks of %lu MiB\n", BIG_BLOCK / MIB, (big != MAP_FAILED) ? "yes" : "NO",
		n, max, BLOCK / MIB);

	for (i = 0; i < n; i++) {
		(void)munmap(common.blocks[i], BLOCK);
	}
	if (big != MAP_FAILED) {
		(void)munmap(big, BIG_BLOCK);
	}

	TEST_ASSERT_TRUE_MESSAGE(big != MAP_FAILED, "no 16 MiB contiguous block after the big process");
	TEST_ASSERT_EQUAL_UINT_MESSAGE(max, n, "not enough 4 MiB contiguous blocks after the big process");
}


/* The big process (beside the pinner if fd >= 0), then the contiguous blocks */
static void run(int fd)
{
	unsigned long long before, after, target, want, ms;
	int cmd[2] = { -1, -1 }, ack[2] = { -1, -1 };
	int status;

	before = freeBytes();
	target = (before - margin(before)) & ~((unsigned long long)common.pagesz - 1ULL);
	want = target / 2U;
	want = ((want < BLOCKS_MAX) ? want : BLOCKS_MAX) & ~((unsigned long long)BLOCK - 1ULL);
	TEST_ASSERT_TRUE_MESSAGE(want >= BLOCK, "not enough free memory for the test");

	printf("frag: free %llu MiB (largest free run %llu MiB), the big process touches %llu MiB%s\n", before / MIB,
		largestFreeRun() / MIB, target / MIB, (fd >= 0) ? ", the pinner beside it" : "");

	if (fd >= 0) {
		TEST_ASSERT_EQUAL_INT(0, pipe(cmd));
		TEST_ASSERT_EQUAL_INT(0, pipe(ack));
		pinnerStart(fd, (size_t)(target / SLOT) + 1U, cmd[0], ack[1], cmd[1], ack[0]);
		/* The pinner's ends: if it dies, the big process sees the end of file */
		(void)close(cmd[0]);
		(void)close(ack[1]);
	}

	status = bigProcess(target, cmd[1], ack[0]);

	if (fd >= 0) {
		(void)close(cmd[1]);
		(void)close(ack[0]);
	}

	after = freeSettled(&ms);
	printf("frag: big process status 0x%x; free %llu MiB after %llu ms (largest free run %llu MiB), want %llu MiB in %lu MiB blocks\n",
		(unsigned int)status, after / MIB, ms, largestFreeRun() / MIB, want / MIB, BLOCK / MIB);
	TEST_ASSERT_FALSE_MESSAGE(WIFEXITED(status) && (WEXITSTATUS(status) == 4), "the pinner could not map its pages (MAP_FIXED file mapping)");
	TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(status) && (WEXITSTATUS(status) == 0), "the big process could not get its memory");

	contiguousAfter(want);
}


TEST_GROUP(test_frag);


TEST_SETUP(test_frag)
{
	const char *dir = getenv("FRAG_DIR");

	if ((dir == NULL) || (dir[0] == '\0')) {
		dir = "/root";
	}
	snprintf(common.path, sizeof(common.path), "%s/frag_test.bin", dir);
	common.pagesz = (size_t)sysconf(_SC_PAGESIZE);
	common.pinner = 0;
}


TEST_TEAR_DOWN(test_frag)
{
	pinnerStop();
	unlink(common.path);
}


TEST(test_frag, after_big_process)
{
	run(-1);
}


TEST(test_frag, beside_kernel_pages_of_another)
{
	unsigned char *page;
	int fd;

	page = calloc(1, common.pagesz);
	TEST_ASSERT_NOT_NULL(page);
	page[0] = 0x5a;
	fd = open(common.path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, fd, strerror(errno));
	TEST_ASSERT_EQUAL_INT((int)common.pagesz, (int)write(fd, page, common.pagesz));
	free(page);

	run(fd);
	pinnerStop();
	(void)close(fd);
}


TEST_GROUP_RUNNER(test_frag)
{
	RUN_TEST_CASE(test_frag, after_big_process);
	RUN_TEST_CASE(test_frag, beside_kernel_pages_of_another);
}


static void runner(void)
{
	RUN_TEST_GROUP(test_frag);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
