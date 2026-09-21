/*
 * Phoenix-RTOS
 *
 * Filesystem tests: holes in sparse files read as zeros
 *
 * A hole -- a block a file's inode does not point at -- must read as zeros.
 * ext2_block_init() used to hand an unallocated block number straight to
 * ext2_block_read(), which meant block 0 OF THE DEVICE: the boot sector and
 * superblock. So a hole read back superblock bytes, and every freshly created
 * file's first block started as a copy of block 0 with the written bytes laid
 * over it, leaking that content into user data once the file was extended.
 *
 * Both cases are checked here. Run against a directory on the filesystem under
 * test, e.g. `test-fs-sparse /mnt/umass1` -- the default is the cwd.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BLOCKSZ  4096 /* worst case of the block sizes we ship (1 KiB and 4 KiB) */
#define HOLE_OFF (2 * BLOCKSZ)

static int failures;


static void check(const char *what, int ok)
{
	printf("%-52s %s\n", what, (ok != 0) ? "PASS" : "FAIL");
	if (ok == 0) {
		failures++;
	}
}


/* Returns the number of non-zero bytes in buf, so a failure can say how bad. */
static size_t countNonZero(const unsigned char *buf, size_t len)
{
	size_t i, n = 0;

	for (i = 0; i < len; i++) {
		if (buf[i] != 0) {
			n++;
		}
	}

	return n;
}


/*
 * A file with a genuine hole: one byte at 0, one byte well past a block
 * boundary, nothing in between. Everything between must read as zeros.
 */
static void test_hole_reads_zeros(const char *dir)
{
	char path[256];
	unsigned char *buf;
	int fd;
	ssize_t got;
	size_t nz;

	snprintf(path, sizeof(path), "%s/sparse_hole.bin", dir);
	(void)unlink(path);

	buf = malloc(BLOCKSZ);
	if (buf == NULL) {
		check("hole: allocate buffer", 0);
		return;
	}

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("hole: open(%s) failed: %s\n", path, strerror(errno));
		check("hole: create file", 0);
		free(buf);
		return;
	}

	check("hole: write first byte", write(fd, "A", 1) == 1);
	check("hole: seek past a block boundary", lseek(fd, HOLE_OFF, SEEK_SET) == (off_t)HOLE_OFF);
	check("hole: write byte after the hole", write(fd, "B", 1) == 1);

	/* Read the block that is entirely inside the hole. */
	check("hole: seek back into the hole", lseek(fd, BLOCKSZ, SEEK_SET) == (off_t)BLOCKSZ);
	memset(buf, 0xaa, BLOCKSZ);
	got = read(fd, buf, BLOCKSZ);
	check("hole: read returns a full block", got == (ssize_t)BLOCKSZ);

	nz = (got > 0) ? countNonZero(buf, (size_t)got) : 1u;
	if (nz != 0) {
		printf("hole: %zu of %zd bytes in the hole were NOT zero (first=0x%02x)\n",
			nz, got, buf[0]);
	}
	check("hole: every byte in the hole is zero", nz == 0);

	(void)close(fd);
	(void)unlink(path);
	free(buf);
}


/*
 * A short write then an extend. The tail of the first block was never written
 * by anyone, so after growing the file it must read as zeros -- not as whatever
 * the block happened to contain before it was allocated.
 */
static void test_extend_tail_zeros(const char *dir)
{
	static const char payload[] = "seventeen bytes!";
	char path[256];
	unsigned char *buf;
	int fd;
	ssize_t got;
	size_t nz, taillen = BLOCKSZ - sizeof(payload);

	snprintf(path, sizeof(path), "%s/sparse_tail.bin", dir);
	(void)unlink(path);

	buf = malloc(BLOCKSZ);
	if (buf == NULL) {
		check("tail: allocate buffer", 0);
		return;
	}

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("tail: open(%s) failed: %s\n", path, strerror(errno));
		check("tail: create file", 0);
		free(buf);
		return;
	}

	check("tail: short write", write(fd, payload, sizeof(payload)) == (ssize_t)sizeof(payload));

	/* Grow the file so the untouched tail of that same block becomes readable. */
	check("tail: extend to one block", ftruncate(fd, (off_t)BLOCKSZ) == 0);

	check("tail: seek to the untouched tail",
		lseek(fd, (off_t)sizeof(payload), SEEK_SET) == (off_t)sizeof(payload));
	memset(buf, 0xaa, BLOCKSZ);
	got = read(fd, buf, taillen);
	check("tail: read returns the whole tail", got == (ssize_t)taillen);

	nz = (got > 0) ? countNonZero(buf, (size_t)got) : 1u;
	if (nz != 0) {
		printf("tail: %zu of %zd untouched bytes were NOT zero (first=0x%02x) "
			   "-- stale block content is visible\n",
			nz, got, buf[0]);
	}
	check("tail: untouched tail of the block is zero", nz == 0);

	(void)close(fd);
	(void)unlink(path);
	free(buf);
}


int main(int argc, char *argv[])
{
	const char *dir = (argc > 1) ? argv[1] : ".";

	printf("SPARSE TEST STARTED (dir=%s)\n", dir);

	test_hole_reads_zeros(dir);
	test_extend_tail_zeros(dir);

	printf("SPARSE TEST %s (%d failure(s))\n", (failures == 0) ? "OK" : "FAILED", failures);

	return (failures == 0) ? 0 : 1;
}
