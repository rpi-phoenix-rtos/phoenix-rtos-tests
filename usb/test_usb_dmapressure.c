/*
 * Phoenix-RTOS
 *
 * phoenix-rtos-tests
 *
 * test-usb-dmapressure: USB mass-storage data integrity under memory pressure
 *
 * The USB host stack takes a fresh physically contiguous DMA buffer for every
 * transfer larger than a page (phoenix-rtos-usb usb/mem.c), so where its data
 * lands in physical memory depends on what is free at that moment. On a lightly
 * loaded system that is always low memory. Under memory pressure it can be the
 * top of RAM, and a host controller that cannot reach the top of RAM then fails
 * silently: on the Raspberry Pi 4 the VL805 behind the BCM2711 PCIe bridge
 * returned wrong data for buffers above 3 GiB, with no transfer error, and the
 * next write ended in an xHCI Host System Error that left the controller dead
 * until power-off. A quiet system never showed it.
 *
 * So this program first takes and touches all free memory but a margin and holds
 * it to the end, which leaves the USB stack only what this program left free.
 * Then it writes every 512-byte sector of the range under test with a pattern
 * made from the sector's own LBA and a per-run seed, reads the range back and
 * checks every sector. A sector can match only its own LBA in this run: data
 * that went to or came from the wrong place, data from an earlier run and a
 * read that never landed (the buffer is filled with POISON before every read)
 * all fail, and the first few are described.
 *
 * It also reports how much of the memory left free lies at or above 3 GiB. If
 * none does, the USB stack had no high memory to be given and a PASS proves
 * little: leave less free (-f) or stop other programs.
 *
 * The read-back starts at the beginning of the range, so the umass driver's
 * block cache (2 MiB) holds nothing of it any more by the time it gets there,
 * as long as the range is several times larger than that (MIN_RANGE).
 *
 * DESTRUCTIVE: the range under test is overwritten. Nothing runs without -y.
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
#include <sys/stat.h>


#define KIB (1024ULL)
#define MIB (1024ULL * KIB)
#define GIB (1024ULL * MIB)

#define SECTOR      512U
#define SECTOR_WORDS (SECTOR / sizeof(uint64_t))

/* Every request moves this much (the last one of the range may be shorter) */
#define IO_SIZE (1024U * 1024U)

#define DEFAULT_RANGE (1ULL * GIB)
#define DEFAULT_LEAVE (300ULL * MIB)

/* Several times the umass block cache, so the read-back cannot be served from it */
#define MIN_RANGE (16ULL * MIB)

/* The memory hog maps in pieces of this size */
#define HOG_CHUNK      (64ULL * MIB)
#define HOG_CHUNKS_MAX 1024U

/* Physical address above which the Raspberry Pi 4's USB controller loses data */
#define HIGH_MEMORY 0xc0000000ULL

/* A failed transfer costs the xHCI driver's 5 s timeout: give up on a dead device */
#define MAX_CONSECUTIVE_IOERR 3U

/* Wrong sectors described in detail; the rest are only counted */
#define MAX_REPORTED 16U

/* Fills the read buffer before every read */
#define POISON 0xa5U

/* Entries of the kernel's page map read for the high-memory figure */
#define PAGEMAP_MAX 65536


static struct {
	const char *path;
	int fd;
	uint64_t seed;
	unsigned long long range;
	unsigned long long leave;
	size_t pagesz;

	unsigned char *wbuf;
	unsigned char *rbuf;

	void *hog[HOG_CHUNKS_MAX];
	size_t hogLen[HOG_CHUNKS_MAX];
	unsigned int hogChunks;
	unsigned long long hogBytes;
	int highOnly;

	unsigned long long badSectors;
	unsigned long long ioErrSectors;
	unsigned int reported;
	int aborted;
} common;


static pageinfo_t pagemap[PAGEMAP_MAX];


static void usage(const char *prog)
{
	printf("usage: %s -y [-s SIZE] [-f SIZE] [-S SEED] DEVICE\n", prog);
	printf("\n");
	printf("Writes every 512-byte sector of the first SIZE bytes of DEVICE with a pattern\n");
	printf("unique to that sector and this run, reads it all back and checks it, while\n");
	printf("holding all free memory but the -f margin so the USB stack must use what is\n");
	printf("left (on a Raspberry Pi 4, memory above 3 GiB).\n");
	printf("\n");
	printf("  -y       really overwrite DEVICE (DESTRUCTIVE; without it nothing is written)\n");
	printf("  -s SIZE  how much of DEVICE to test, from its start (default 1G; capped at\n");
	printf("           the device size)\n");
	printf("  -f SIZE  free memory to leave to the rest of the system (default 300M)\n");
	printf("  -S SEED  pattern seed (default: from the clock)\n");
	printf("  -H       keep only memory above 3 GiB free (low pages stay held), so USB DMA must\n           cope with high memory: data must stay correct (errors are acceptable)\n");
	printf("  SIZE takes a K, M or G suffix; a bare number is in MiB.\n");
	printf("\n");
	printf("DEVICE is a USB mass-storage node, /dev/umassN, N counting from 0 over the\n");
	printf("nodes of all sticks in the order they were found. The umass driver creates\n");
	printf("one node per non-empty entry of a stick's MBR partition table, and that node\n");
	printf("covers only its partition: offset 0 is the partition's first sector. A stick\n");
	printf("without a partition table gets one node for the whole disk where the driver\n");
	printf("supports that, and none otherwise; one that logs \"umass: reading the partition\n");
	printf("table failed\" has none. `ls /dev` shows the nodes. Everything in the tested\n");
	printf("range is destroyed, file systems included.\n");
	printf("\n");
	printf("Reading the result: bad>0 is data corruption. ioerr>0 with bad=0 can be the USB\n");
	printf("stack running out of DMA memory it can use (look for \"usb: no DMA memory\")\n");
	printf("rather than a device fault: leave more free with -f. highfree=0 means the run\n");
	printf("did not cover memory above 3 GiB.\n");
	printf("\n");
	printf("Prints \"RESULT: PASS\" or \"RESULT: FAIL\" with the counts; exits 0 on PASS.\n");
}


/* "300", "300M", "1G", "512K" -> bytes; 0 on a malformed or zero value */
static unsigned long long parseSize(const char *s)
{
	char *end;
	unsigned long long v, unit = MIB;

	errno = 0;
	v = strtoull(s, &end, 10);
	if ((errno != 0) || (end == s)) {
		return 0;
	}

	switch (*end) {
		case '\0':
			break;
		case 'k':
		case 'K':
			unit = KIB;
			end++;
			break;
		case 'm':
		case 'M':
			end++;
			break;
		case 'g':
		case 'G':
			unit = GIB;
			end++;
			break;
		default:
			return 0;
	}

	if ((*end != '\0') || (v > (~0ULL / unit))) {
		return 0;
	}

	return v * unit;
}


static unsigned long long nowUs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000ULL + (unsigned long long)ts.tv_nsec / 1000ULL;
}


static double mibPerSec(unsigned long long bytes, unsigned long long us)
{
	return (us == 0U) ? 0.0 : ((double)bytes / (double)MIB) / ((double)us / 1e6);
}


static unsigned long long freeMemory(void)
{
	long pages = sysconf(_SC_AVPHYS_PAGES);

	return (pages < 0) ? 0U : (unsigned long long)pages * common.pagesz;
}


/*
 * Free memory at or above HIGH_MEMORY, from the kernel's page map ('.' runs are
 * free). *truncated is set when the map has more runs than PAGEMAP_MAX, which
 * makes the result a lower bound.
 */
static unsigned long long freeHighMemory(int *truncated)
{
	meminfo_t info;
	unsigned long long high = 0, start, end;
	int i, n;

	memset(&info, 0, sizeof(info));
	info.page.mapsz = PAGEMAP_MAX;
	info.page.map = pagemap;
	info.entry.mapsz = -1;
	info.entry.kmapsz = -1;
	info.maps.mapsz = -1;
	meminfo(&info);

	n = (info.page.mapsz < PAGEMAP_MAX) ? info.page.mapsz : PAGEMAP_MAX;
	*truncated = (info.page.mapsz > PAGEMAP_MAX) ? 1 : 0;

	for (i = 0; i < n; i++) {
		if (pagemap[i].marker != '.') {
			continue;
		}
		start = (unsigned long long)pagemap[i].addr;
		end = start + (unsigned long long)pagemap[i].count * common.pagesz;
		if (end > HIGH_MEMORY) {
			high += end - ((start > HIGH_MEMORY) ? start : HIGH_MEMORY);
		}
	}

	return high;
}


/*
 * Takes and touches free memory in HOG_CHUNK pieces until only common.leave is
 * left (or the kernel refuses), and keeps it until the process exits. Every page
 * is written, so it is really allocated and not left for the USB stack.
 */
static void hogMemory(void)
{
	unsigned long long avail;
	size_t len, i;
	unsigned char *m;

	while (common.hogChunks < HOG_CHUNKS_MAX) {
		avail = freeMemory();
		if (avail <= common.leave + common.pagesz) {
			break;
		}

		len = (size_t)(((avail - common.leave) < HOG_CHUNK) ? (avail - common.leave) : HOG_CHUNK);
		len &= ~(common.pagesz - 1U);

		m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (m == MAP_FAILED) {
			break;
		}

		for (i = 0; i < len; i += common.pagesz) {
			m[i] = 0x5aU;
		}

		common.hog[common.hogChunks] = m;
		common.hogLen[common.hogChunks] = len;
		common.hogChunks++;
		common.hogBytes += len;
	}
}


/*
 * -H: give back every held page at or above HIGH_MEMORY, so the only free memory
 * left (apart from the -f margin) is high. The USB stack's next DMA allocations
 * then come from the region the VL805 corrupts; a correct stack either places
 * them below it anyway or fails the transfer, but never returns wrong data.
 */
static unsigned long long hogKeepLowOnly(void)
{
	unsigned long long released = 0;
	unsigned int c;
	size_t off;

	for (c = 0; c < common.hogChunks; c++) {
		for (off = 0; off < common.hogLen[c]; off += common.pagesz) {
			unsigned char *pg = (unsigned char *)common.hog[c] + off;
			if ((unsigned long long)va2pa(pg) >= HIGH_MEMORY) {
				if (munmap(pg, common.pagesz) == 0) {
					released += common.pagesz;
				}
			}
		}
	}
	common.hogBytes -= released;
	return released;
}


static void hogRelease(void)
{
	while (common.hogChunks > 0U) {
		common.hogChunks--;
		(void)munmap(common.hog[common.hogChunks], common.hogLen[common.hogChunks]);
	}
}


/* xorshift64*: cheap, and every word depends on the seed it started from */
static uint64_t prngNext(uint64_t *state)
{
	uint64_t x = *state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*state = x;

	return x * 0x2545f4914f6cdd1dULL;
}


/*
 * The pattern of one sector: its LBA and the run's seed in the first two words
 * (so a wrong sector can be identified), then a stream seeded by both.
 */
static void patternFill(uint64_t *w, uint64_t lba, uint64_t seed)
{
	uint64_t state = (lba * 0x9e3779b97f4a7c15ULL) ^ seed;
	unsigned int i;

	if (state == 0U) {
		state = 1U; /* xorshift's one fixed point */
	}

	w[0] = lba;
	w[1] = seed;
	for (i = 2; i < SECTOR_WORDS; i++) {
		w[i] = prngNext(&state);
	}
}


static void patternBlock(unsigned char *buf, uint64_t lba, size_t len)
{
	size_t off;

	for (off = 0; off < len; off += SECTOR) {
		patternFill((uint64_t *)(buf + off), lba + off / SECTOR, common.seed);
	}
}


/* What a wrong sector holds instead, for the report */
static void describeBad(uint64_t lba, const unsigned char *got)
{
	uint64_t want[SECTOR_WORDS], other[SECTOR_WORDS], w0, w1;
	unsigned int first, n;

	patternFill(want, lba, common.seed);
	first = 0;
	while ((first < SECTOR) && (got[first] == ((const unsigned char *)want)[first])) {
		first++;
	}
	n = 1;
	while ((n < SECTOR) && (got[n] == got[0])) {
		n++;
	}

	memcpy(&w0, got, sizeof(w0));
	memcpy(&w1, got + sizeof(w0), sizeof(w1));

	printf("dmapressure: bad sector %llu (first wrong byte %u): ", (unsigned long long)lba, first);
	if ((n == SECTOR) && (got[0] == POISON)) {
		printf("still holds the read buffer's poison fill: the data never landed\n");
	}
	else if (n == SECTOR) {
		printf("every byte is 0x%02x\n", got[0]);
	}
	else {
		patternFill(other, w0, w1);
		if (memcmp(got, other, SECTOR) == 0) {
			if (w1 == common.seed) {
				printf("holds sector %llu of this run\n", (unsigned long long)w0);
			}
			else {
				printf("holds sector %llu of an earlier run (seed 0x%llx)\n", (unsigned long long)w0,
					(unsigned long long)w1);
			}
		}
		else {
			printf("unrecognised data (%016llx %016llx ...)\n", (unsigned long long)w0, (unsigned long long)w1);
		}
	}
}


/* pread()/pwrite() the whole length, or -1 with errno */
static int ioFull(int isWrite, unsigned char *buf, size_t len, off_t offs)
{
	size_t done = 0;
	ssize_t r;

	while (done < len) {
		if (isWrite != 0) {
			r = pwrite(common.fd, buf + done, len - done, offs + (off_t)done);
		}
		else {
			r = pread(common.fd, buf + done, len - done, offs + (off_t)done);
		}

		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		if (r == 0) {
			errno = EIO; /* the device ended the transfer early */
			return -1;
		}
		done += (size_t)r;
	}

	return 0;
}


/* One pass over the range; returns the time it took in microseconds */
static unsigned long long pass(int isWrite)
{
	unsigned long long offs, start = nowUs(), progress = 0;
	unsigned int consecutive = 0;
	size_t len, s;
	uint64_t lba;

	printf("dmapressure: %s %llu MiB...\n", (isWrite != 0) ? "writing" : "reading back", common.range / MIB);

	for (offs = 0; offs < common.range; offs += len) {
		len = ((common.range - offs) < IO_SIZE) ? (size_t)(common.range - offs) : IO_SIZE;
		lba = offs / SECTOR;

		if (isWrite != 0) {
			patternBlock(common.wbuf, lba, len);
		}
		else {
			memset(common.rbuf, POISON, len);
		}

		if (ioFull(isWrite, (isWrite != 0) ? common.wbuf : common.rbuf, len, (off_t)offs) < 0) {
			printf("dmapressure: %s error at sector %llu (+%zu sectors): %s\n", (isWrite != 0) ? "write" : "read",
				(unsigned long long)lba, len / SECTOR, strerror(errno));
			common.ioErrSectors += len / SECTOR;
			if (++consecutive >= MAX_CONSECUTIVE_IOERR) {
				printf("dmapressure: %u requests in a row failed, giving up\n", consecutive);
				common.aborted = 1;
				break;
			}
			continue;
		}
		consecutive = 0;

		if (isWrite == 0) {
			for (s = 0; s < len; s += SECTOR) {
				patternFill((uint64_t *)common.wbuf, lba + s / SECTOR, common.seed);
				if (memcmp(common.rbuf + s, common.wbuf, SECTOR) != 0) {
					if (common.reported < MAX_REPORTED) {
						describeBad(lba + s / SECTOR, common.rbuf + s);
						common.reported++;
					}
					common.badSectors++;
				}
			}
		}

		if (offs + len >= progress + 256U * MIB) {
			progress = offs + len;
			printf("dmapressure:   %llu MiB\n", progress / MIB);
		}
	}

	return nowUs() - start;
}


int main(int argc, char *argv[])
{
	unsigned long long devSize, freeBefore, freeHigh, wUs = 0, rUs = 0;
	struct timespec ts;
	struct stat st;
	int c, yes = 0, seedGiven = 0, rangeGiven = 0, truncated, pass1;
	char *end;

	common.range = DEFAULT_RANGE;
	common.leave = DEFAULT_LEAVE;
	common.pagesz = (size_t)sysconf(_SC_PAGESIZE);

	while ((c = getopt(argc, argv, "ys:f:S:Hh")) != -1) {
		switch (c) {
			case 'y':
				yes = 1;
				break;
			case 's':
				common.range = parseSize(optarg);
				rangeGiven = 1;
				if (common.range == 0U) {
					fprintf(stderr, "dmapressure: bad size '%s'\n", optarg);
					return 2;
				}
				break;
			case 'f':
				common.leave = parseSize(optarg);
				if (common.leave == 0U) {
					fprintf(stderr, "dmapressure: bad size '%s'\n", optarg);
					return 2;
				}
				break;
			case 'H':
				common.highOnly = 1;
				break;
			case 'S':
				errno = 0;
				common.seed = strtoull(optarg, &end, 0);
				if ((errno != 0) || (end == optarg) || (*end != '\0')) {
					fprintf(stderr, "dmapressure: bad seed '%s'\n", optarg);
					return 2;
				}
				seedGiven = 1;
				break;
			case 'h':
				usage(argv[0]);
				return 0;
			default:
				usage(argv[0]);
				return 2;
		}
	}

	if (optind != (argc - 1)) {
		usage(argv[0]);
		return 2;
	}
	common.path = argv[optind];

	common.fd = open(common.path, O_RDWR);
	if (common.fd < 0) {
		fprintf(stderr, "dmapressure: cannot open %s: %s\n", common.path, strerror(errno));
		return 2;
	}

	devSize = 0;
	if (fstat(common.fd, &st) == 0) {
		devSize = (unsigned long long)st.st_size;
	}
	if (devSize == 0U) {
		/* Some storage servers do not report a size through stat(); try seeking to the end */
		off_t end = lseek(common.fd, 0, SEEK_END);
		if ((end > 0) && (lseek(common.fd, 0, SEEK_SET) == 0)) {
			devSize = (unsigned long long)end;
		}
	}
	if (devSize == 0U) {
		/* Still unknown: trust the range the user gave explicitly with -s */
		if (rangeGiven == 0) {
			fprintf(stderr, "dmapressure: %s reports no size; give the range to test with -s\n", common.path);
			return 2;
		}
		fprintf(stderr, "dmapressure: %s reports no size; testing the -s range as given\n", common.path);
		devSize = common.range;
	}
	if (common.range > devSize) {
		common.range = devSize;
	}
	common.range -= common.range % SECTOR;

	printf("dmapressure: %s holds %llu MiB; the test OVERWRITES its first %llu MiB (sectors 0-%llu)\n",
		common.path, devSize / MIB, common.range / MIB, common.range / SECTOR - 1U);
	if (yes == 0) {
		printf("dmapressure: nothing written; add -y to run it\n");
		return 2;
	}
	if (common.range < MIN_RANGE) {
		fprintf(stderr, "dmapressure: test at least %llu MiB, or the read-back may come from the driver's cache\n",
			MIN_RANGE / MIB);
		return 2;
	}

	if (seedGiven == 0) {
		clock_gettime(CLOCK_REALTIME, &ts);
		common.seed = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec ^ ((uint64_t)getpid() << 16);
	}

	/* The I/O buffers first: they must not come out of the margin left to the USB stack */
	common.wbuf = mmap(NULL, IO_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	common.rbuf = mmap(NULL, IO_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if ((common.wbuf == MAP_FAILED) || (common.rbuf == MAP_FAILED)) {
		fprintf(stderr, "dmapressure: no memory for the I/O buffers\n");
		return 2;
	}
	memset(common.wbuf, 0, IO_SIZE);
	memset(common.rbuf, 0, IO_SIZE);

	freeBefore = freeMemory();
	hogMemory();
	if (common.highOnly != 0) {
		printf("dmapressure: -H gave back %llu MiB above 0x%llx; only high memory is free now\n",
			hogKeepLowOnly() / MIB, HIGH_MEMORY);
	}
	freeHigh = freeHighMemory(&truncated);

	printf("dmapressure: seed 0x%llx; free memory %llu MiB, held %llu MiB, %llu MiB left free (%s%llu MiB of it at or above 0x%llx)\n",
		(unsigned long long)common.seed, freeBefore / MIB, common.hogBytes / MIB, freeMemory() / MIB,
		(truncated != 0) ? "at least " : "", freeHigh / MIB, HIGH_MEMORY);
	if (freeHigh == 0U) {
		printf("dmapressure: WARNING: no free memory above 0x%llx, so the USB stack cannot be given any; "
			   "a PASS here does not cover high memory\n",
			HIGH_MEMORY);
	}

	wUs = pass(1);
	pass1 = common.aborted;
	if (pass1 == 0) {
		rUs = pass(0);
	}

	hogRelease();
	(void)close(common.fd);

	if (common.reported < common.badSectors) {
		printf("dmapressure: (%llu more bad sectors not described)\n", common.badSectors - common.reported);
	}

	printf("RESULT: %s sectors=%llu bad=%llu ioerr=%llu%s write=%.1f MiB/s read=%.1f MiB/s held=%lluMiB highfree=%lluMiB\n",
		((common.badSectors == 0U) && (common.ioErrSectors == 0U) && (common.aborted == 0)) ? "PASS" : "FAIL",
		common.range / SECTOR, common.badSectors, common.ioErrSectors,
		(common.aborted != 0) ? ((pass1 != 0) ? " aborted-in-write" : " aborted-in-read") : "",
		(pass1 == 0) ? mibPerSec(common.range, wUs) : 0.0, (common.aborted == 0) ? mibPerSec(common.range, rUs) : 0.0,
		common.hogBytes / MIB, freeHigh / MIB);

	return ((common.badSectors == 0U) && (common.ioErrSectors == 0U) && (common.aborted == 0)) ? 0 : 1;
}
