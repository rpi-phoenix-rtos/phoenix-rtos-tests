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


/*
 * Truncating a file that contains HOLES must not walk off the group descriptor
 * table. The release path coalesced runs by tracking the last block number, and
 * a hole set that to 0, so the run was freed as `1 - n` -- a uint32 underflow
 * that produced a block number of ~4e9, a group index of 524287, and a read
 * past fs->gdt[]. On hardware that killed the storage driver outright, taking
 * the filesystem with it, so this test is as much about surviving as passing.
 */
static void test_truncate_with_holes(const char *dir)
{
	char path[256];
	unsigned char buf[BLOCKSZ];
	int fd;
	off_t off;
	int i;

	snprintf(path, sizeof(path), "%s/trunc_holes.bin", dir);
	(void)unlink(path);

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check("trunc-holes: create", 0);
		return;
	}

	/* Alternate data and hole across several blocks. */
	memset(buf, 0x5A, sizeof(buf));
	for (i = 0; i < 6; i++) {
		off = (off_t)i * 2 * BLOCKSZ;
		if (lseek(fd, off, SEEK_SET) != off || write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
			check("trunc-holes: build sparse file", 0);
			close(fd);
			return;
		}
	}
	check("trunc-holes: built a sparse file", 1);

	/* Truncate back through the holes. If the driver survives this, the
	 * underflow is gone -- the old code faulted here. */
	check("trunc-holes: truncate to 0 survives", ftruncate(fd, 0) == 0);
	check("trunc-holes: size is 0", lseek(fd, 0, SEEK_END) == 0);

	/* And the filesystem still works afterwards. */
	memset(buf, 0x11, sizeof(buf));
	check("trunc-holes: writable afterwards", write(fd, buf, 64) == 64);

	close(fd);
	(void)unlink(path);
}


/*
 * Truncating to a NON-block-aligned size leaves the rest of that block on disk.
 * Nothing reads it while the file is short, but extending the file again must
 * still see zeros there -- otherwise deleted content becomes readable.
 */
static void test_truncate_tail_not_leaked(const char *dir)
{
	char path[256];
	unsigned char buf[BLOCKSZ];
	unsigned char got[BLOCKSZ];
	const off_t shortSz = BLOCKSZ + (BLOCKSZ / 3); /* deliberately not aligned */
	const off_t reExtend = 3 * BLOCKSZ;
	size_t gap;
	int fd;

	snprintf(path, sizeof(path), "%s/trunc_tail.bin", dir);
	(void)unlink(path);

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check("trunc-tail: create", 0);
		return;
	}

	memset(buf, 0xAA, sizeof(buf));
	if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf) ||
			write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf) ||
			write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
		check("trunc-tail: fill 3 blocks with 0xAA", 0);
		close(fd);
		return;
	}
	check("trunc-tail: filled 3 blocks with 0xAA", 1);

	check("trunc-tail: truncate to an unaligned size", ftruncate(fd, shortSz) == 0);

	/* Extend past the truncation point again. */
	if (lseek(fd, reExtend, SEEK_SET) != reExtend || write(fd, "END", 3) != 3) {
		check("trunc-tail: re-extend", 0);
		close(fd);
		return;
	}

	gap = (size_t)(reExtend - shortSz);
	if (gap > sizeof(got)) {
		gap = sizeof(got);
	}

	memset(got, 0xFF, sizeof(got));
	if (lseek(fd, shortSz, SEEK_SET) != shortSz || read(fd, got, gap) != (ssize_t)gap) {
		check("trunc-tail: read the gap", 0);
		close(fd);
		return;
	}
	check("trunc-tail: read the gap", 1);

	{
		size_t bad = countNonZero(got, gap);
		if (bad != 0) {
			printf("  trunc-tail: %zu of %zu gap bytes are STALE (first 0x%02x)\n",
					bad, gap, got[0]);
		}
		check("trunc-tail: no deleted content is readable", bad == 0);
	}

	close(fd);
	(void)unlink(path);
}


int main(int argc, char *argv[])
{
	const char *dir = (argc > 1) ? argv[1] : ".";

	printf("SPARSE TEST STARTED (dir=%s)\n", dir);

	test_hole_reads_zeros(dir);
	test_extend_tail_zeros(dir);
	test_truncate_with_holes(dir);
	test_truncate_tail_not_leaked(dir);

	printf("SPARSE TEST %s (%d failure(s))\n", (failures == 0) ? "OK" : "FAILED", failures);

	return (failures == 0) ? 0 : 1;
}
