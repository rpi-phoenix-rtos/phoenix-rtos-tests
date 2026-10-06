/*
 * Phoenix-RTOS
 *
 * Filesystem tests: one very large write() and read-back
 *
 * A single write() of tens of megabytes reaches the filesystem server as ONE
 * mtWrite. On the NFS root (build 39) that became one 54 MB NFSv4 WRITE: the
 * server never answers a request that size, the libnfs call ran into its
 * overall deadline, and the clean-up of that timed-out call wrote into a dead
 * stack frame of the "nfs" server -- a Data Abort that took "/" down with it.
 *
 * Checked here, on the filesystem under test (default /root, i.e. the NFS root
 * when netbooted -- NOT /tmp, which is a RAM tmpfs there):
 *   1. one write() of the whole buffer returns the whole count;
 *   2. the file then has exactly that size;
 *   3. reading it back gives the same bytes, position for position (the
 *      pattern encodes each word's offset, so a chunk written at the wrong
 *      offset is caught, not just a wrong checksum);
 *   4. the same for a write at an unaligned offset, of an unaligned length, so
 *      a server that splits the transfer must get every chunk offset right.
 *
 * Usage: test_bigwrite [dir] [MiB]     (defaults: /root 64)
 * Last line: "TEST-BIGWRITE: PASS" or "TEST-BIGWRITE: FAIL <first failure>".
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
#include <sys/stat.h>

#define READ_CHUNK (1024 * 1024)

/* The unaligned case: odd offset, odd length, spanning several 1 MiB chunks. */
#define ODD_OFFSET 12345
#define ODD_LENGTH ((3 * 1024 * 1024) + 7)

static int failures;
static char firstFailure[160];


static void check(const char *what, int ok)
{
	printf("%-56s %s\n", what, (ok != 0) ? "PASS" : "FAIL");
	if (ok == 0) {
		if (failures == 0) {
			snprintf(firstFailure, sizeof(firstFailure), "%s", what);
		}
		failures++;
	}
}


/* The byte that belongs at file offset `pos` of the file written with `seed`. */
static unsigned char patternAt(uint64_t pos, uint32_t seed)
{
	uint64_t x = (pos >> 3) * 0x9e3779b97f4a7c15ULL + seed;

	x ^= x >> 29;
	x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 32;

	return (unsigned char)(x >> ((pos & 7u) * 8u));
}


static void fillPattern(unsigned char *buf, size_t len, uint64_t pos, uint32_t seed)
{
	size_t i;

	for (i = 0; i < len; i++) {
		buf[i] = patternAt(pos + i, seed);
	}
}


static double nowSec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}


/* Read [pos, pos+len) back and compare with the pattern. Returns 1 if intact;
 * otherwise prints the first bad offset. */
static int verify(int fd, uint64_t pos, size_t len, uint32_t seed, const char *tag)
{
	unsigned char *rb = malloc(READ_CHUNK);
	size_t done = 0;
	int ok = 1;

	if (rb == NULL) {
		printf("%s: no memory for the read buffer\n", tag);
		return 0;
	}
	if (lseek(fd, (off_t)pos, SEEK_SET) != (off_t)pos) {
		printf("%s: lseek(%llu) failed: %s\n", tag, (unsigned long long)pos, strerror(errno));
		free(rb);
		return 0;
	}

	while ((done < len) && (ok != 0)) {
		size_t want = len - done;
		ssize_t got;
		size_t i;

		if (want > READ_CHUNK) {
			want = READ_CHUNK;
		}
		/* Short reads are legal (the NFS server returns at most one RPC's
		 * worth); only 0 before the end, or an error, is a failure. */
		got = read(fd, rb, want);
		if (got <= 0) {
			printf("%s: read at %llu returned %zd (%s)\n", tag,
				(unsigned long long)(pos + done), got, (got < 0) ? strerror(errno) : "early EOF");
			ok = 0;
			break;
		}
		for (i = 0; i < (size_t)got; i++) {
			if (rb[i] != patternAt(pos + done + i, seed)) {
				printf("%s: first wrong byte at file offset %llu: 0x%02x, expected 0x%02x\n", tag,
					(unsigned long long)(pos + done + i), rb[i], patternAt(pos + done + i, seed));
				ok = 0;
				break;
			}
		}
		done += (size_t)got;
	}

	free(rb);
	return ok;
}


static void test_one_big_write(const char *dir, size_t len)
{
	char path[256], what[96];
	unsigned char *buf;
	struct stat st;
	ssize_t wr;
	double t0, t1;
	int fd;

	snprintf(path, sizeof(path), "%s/bigwrite.bin", dir);
	(void)unlink(path);

	buf = malloc(len);
	if (buf == NULL) {
		check("big: allocate the write buffer", 0);
		return;
	}
	fillPattern(buf, len, 0, 1u);

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("big: open %s: %s\n", path, strerror(errno));
		check("big: create", 0);
		free(buf);
		return;
	}

	t0 = nowSec();
	wr = write(fd, buf, len);
	t1 = nowSec();
	if (wr != (ssize_t)len) {
		printf("big: write(%zu) returned %zd (%s)\n", len, wr, (wr < 0) ? strerror(errno) : "short");
	}
	else {
		printf("big: %zu bytes in one write() in %.2f s (%.1f MB/s)\n", len, t1 - t0,
			(t1 > t0) ? ((double)len / 1e6) / (t1 - t0) : 0.0);
	}
	snprintf(what, sizeof(what), "big: one write() of %zu MiB returns all of it", len >> 20);
	check(what, wr == (ssize_t)len);
	free(buf);

	check("big: fstat size equals the write", (fstat(fd, &st) == 0) && (st.st_size == (off_t)len));
	check("big: read-back matches, offset for offset", verify(fd, 0, len, 1u, "big"));

	(void)close(fd);
	check("big: unlink", unlink(path) == 0);
}


static void test_unaligned_write(const char *dir)
{
	char path[256];
	unsigned char *buf;
	struct stat st;
	ssize_t wr;
	int fd;

	snprintf(path, sizeof(path), "%s/bigwrite_odd.bin", dir);
	(void)unlink(path);

	buf = malloc(ODD_LENGTH);
	if (buf == NULL) {
		check("odd: allocate the write buffer", 0);
		return;
	}

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check("odd: create", 0);
		free(buf);
		return;
	}

	/* Prefix written separately, so the big write starts mid-file. */
	fillPattern(buf, ODD_OFFSET, 0, 2u);
	check("odd: prefix write", write(fd, buf, ODD_OFFSET) == ODD_OFFSET);

	fillPattern(buf, ODD_LENGTH, ODD_OFFSET, 2u);
	wr = write(fd, buf, ODD_LENGTH);
	if (wr != (ssize_t)ODD_LENGTH) {
		printf("odd: write(%d) at %d returned %zd (%s)\n", ODD_LENGTH, ODD_OFFSET, wr,
			(wr < 0) ? strerror(errno) : "short");
	}
	check("odd: unaligned write of 3 MiB + 7 at offset 12345", wr == (ssize_t)ODD_LENGTH);
	free(buf);

	check("odd: fstat size", (fstat(fd, &st) == 0) && (st.st_size == (off_t)(ODD_OFFSET + ODD_LENGTH)));
	check("odd: read-back matches, offset for offset", verify(fd, 0, ODD_OFFSET + ODD_LENGTH, 2u, "odd"));

	(void)close(fd);
	check("odd: unlink", unlink(path) == 0);
}


int main(int argc, char **argv)
{
	const char *dir = (argc > 1) ? argv[1] : "/root";
	long mib = (argc > 2) ? strtol(argv[2], NULL, 10) : 64;

	if ((mib <= 0) || (mib > 1024)) {
		printf("usage: %s [dir] [MiB 1..1024]\n", argv[0]);
		return 2;
	}

	printf("test_bigwrite: dir=%s size=%ld MiB\n", dir, mib);
	test_one_big_write(dir, (size_t)mib << 20);
	test_unaligned_write(dir);

	if (failures == 0) {
		printf("TEST-BIGWRITE: PASS\n");
		return 0;
	}
	printf("TEST-BIGWRITE: FAIL %s (%d check(s) failed)\n", firstFailure, failures);
	return 1;
}
