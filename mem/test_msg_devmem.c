/*
 * Phoenix-RTOS
 *
 * Message payloads in uncached and in device memory
 *
 * HEADER:
 *    - sys/mman.h
 *
 * TESTED:
 *    - write() and read() on a file served by another process, with the buffer
 *      in a MAP_UNCACHED mapping or in a MAP_DEVICE | MAP_UNCACHED mapping
 *
 * !!! DANGEROUS ON A KERNEL WITHOUT THE proc/msg.c DEVICE-PAYLOAD FIX !!!
 * (phoenix-rtos-kernel "proc/msg: refuse device-memory payloads instead of
 * faulting on them", 2026-09-27). There, every test of group msg_devmem makes
 * the kernel copy from or to device memory at an unaligned address. On aarch64
 * that is an alignment fault the page-fault handler cannot resolve, so the
 * copying thread - the file server's receiving thread, or for the small
 * payloads this test's own thread - faults in the kernel for ever, printing an
 * "Exception #37: Data Abort (EL1)" dump each time. Reboot the board after it.
 * Group msg_uncached is safe on any kernel: `test-msg-devmem -g msg_uncached`.
 *
 * What it checks:
 *   msg_uncached - a payload in Normal non-cacheable memory (MAP_UNCACHED) at an
 *     unaligned offset crosses to the server intact, for the kernel's three copy
 *     paths: the partial first page, the partial last page, and a small payload
 *     packed into the message. Reads come back intact through the same paths.
 *   msg_devmem - the same payloads in device memory (the physical pages of an
 *     uncached buffer, mapped a second time MAP_DEVICE | MAP_UNCACHED, which is
 *     how drivers map MMIO) are refused: the call fails with EINVAL and the file
 *     is not changed. The device alias is never touched from user space here:
 *     only the kernel's copies are under test.
 *   msg_devnext - a payload in ordinary (cacheable) memory at an unaligned offset,
 *     in the page just BELOW a device mapping, crosses intact. The kernel used to
 *     look up the payload's mapping with a page-sized probe starting at the
 *     unaligned address, which also overlaps the mapping above; it could then
 *     take that mapping's MAP_DEVICE for the payload's own. With the device-payload
 *     fix alone such a payload may be refused (EINVAL); on a kernel with neither
 *     fix it may loop a thread at EL1 like group msg_devmem.
 *
 * The incident this reproduces: cycle mig-q3 on the Raspberry Pi 4, a 59-byte
 * write() at page offset 0x49 of a MAP_DEVICE | MAP_UNCACHED page kept
 * pl011-tty's receiving thread in an EL1 alignment-fault loop for 300 s. The
 * page was in fact ordinary memory - the render server's stdout buffer - with
 * the server's GPU registers mapped right above it (group msg_devnext).
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
#include <unistd.h>
#include <sys/mman.h>

#include <unity_fixture.h>


#define DEVMEM_PAGE  4096
#define DEVMEM_PAGES 2
#define DEVMEM_SIZE  (DEVMEM_PAGE * DEVMEM_PAGES)

/* Where the payloads start and how long they are. Each one makes the kernel take one copy path. */
#define OFFS_HEAD   0x49                /* the incident: one partial page, copied as a shadow page */
#define LEN_HEAD    59                  /* a write() packs at most 40 bytes, a read() 64 */
#define LEN_RDHEAD  100
#define OFFS_SPAN   (DEVMEM_PAGE - 55)  /* partial first page and partial last page */
#define LEN_SPAN    200
#define OFFS_TAIL   DEVMEM_PAGE         /* page-aligned start: partial last page only */
#define LEN_TAIL    100
#define OFFS_PACKED 1                   /* small enough to travel inside the message */
#define LEN_PACKED  20
#define OFFS_NEXT   0x21                /* mig-q3-fix: a 132-byte line whose first 33 bytes went out */
#define LEN_NEXT    99


static const char devmem_path[] = "/tmp/test-msg-devmem.dat";

static struct {
	char *nc;  /* MAP_UNCACHED: Normal non-cacheable memory */
	char *dev; /* the same physical pages as nc, mapped as device memory */
	char *mem; /* msg_devnext: an ordinary page, followed by a device mapping */
	char pattern[DEVMEM_SIZE];
	char back[DEVMEM_SIZE];
} devmem_common;


static void devmem_fillPattern(void)
{
	size_t i;

	for (i = 0; i < sizeof(devmem_common.pattern); i++) {
		devmem_common.pattern[i] = (char)('A' + (i * 7U) % 26U);
	}
}


static int devmem_openEmpty(void)
{
	int fd = open(devmem_path, O_RDWR | O_CREAT | O_TRUNC, 0600);

	TEST_ASSERT_NOT_EQUAL_INT(-1, fd);
	return fd;
}


static off_t devmem_fileSize(int fd)
{
	return lseek(fd, 0, SEEK_END);
}


/* write() len bytes of buf + offs to an empty file, and check they read back intact */
static void devmem_writeArrives(char *buf, size_t offs, size_t len)
{
	int fd = devmem_openEmpty();

	memcpy(buf + offs, devmem_common.pattern + offs, len);
	TEST_ASSERT_EQUAL_INT((int)len, (int)write(fd, buf + offs, len));

	TEST_ASSERT_EQUAL_INT((int)len, (int)devmem_fileSize(fd));
	TEST_ASSERT_EQUAL_INT(0, (int)lseek(fd, 0, SEEK_SET));
	memset(devmem_common.back, 0, len);
	TEST_ASSERT_EQUAL_INT((int)len, (int)read(fd, devmem_common.back, len));
	TEST_ASSERT_EQUAL_MEMORY(devmem_common.pattern + offs, devmem_common.back, len);

	TEST_ASSERT_EQUAL_INT(0, close(fd));
}


/* read() len bytes of a file into buf + offs, and check they arrive intact */
static void devmem_readArrives(char *buf, size_t offs, size_t len)
{
	int fd = devmem_openEmpty();

	TEST_ASSERT_EQUAL_INT((int)len, (int)write(fd, devmem_common.pattern, len));
	TEST_ASSERT_EQUAL_INT(0, (int)lseek(fd, 0, SEEK_SET));

	/* Only the payload: in group msg_devnext the page above is device memory */
	memset(buf + offs, 0, len);
	TEST_ASSERT_EQUAL_INT((int)len, (int)read(fd, buf + offs, len));
	TEST_ASSERT_EQUAL_MEMORY(devmem_common.pattern, buf + offs, len);

	TEST_ASSERT_EQUAL_INT(0, close(fd));
}


/* write() from the device alias is refused and leaves the file empty */
static void devmem_writeRefused(size_t offs, size_t len)
{
	int fd = devmem_openEmpty();

	memcpy(devmem_common.nc + offs, devmem_common.pattern + offs, len);
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, (int)write(fd, devmem_common.dev + offs, len));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
	TEST_ASSERT_EQUAL_INT(0, (int)devmem_fileSize(fd));

	TEST_ASSERT_EQUAL_INT(0, close(fd));
}


/* read() into the device alias is refused and does not change the memory */
static void devmem_readRefused(size_t offs, size_t len)
{
	int fd = devmem_openEmpty();

	TEST_ASSERT_EQUAL_INT((int)len, (int)write(fd, devmem_common.pattern, len));
	TEST_ASSERT_EQUAL_INT(0, (int)lseek(fd, 0, SEEK_SET));

	memset(devmem_common.nc, 0, DEVMEM_SIZE);
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, (int)read(fd, devmem_common.dev + offs, len));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
	memset(devmem_common.back, 0, DEVMEM_SIZE);
	TEST_ASSERT_EQUAL_MEMORY(devmem_common.back, devmem_common.nc, DEVMEM_SIZE);

	TEST_ASSERT_EQUAL_INT(0, close(fd));
}


TEST_GROUP(msg_uncached);
TEST_GROUP(msg_devmem);
TEST_GROUP(msg_devnext);


TEST_SETUP(msg_uncached)
{
	devmem_fillPattern();
	devmem_common.nc = mmap(NULL, DEVMEM_SIZE, PROT_READ | PROT_WRITE, MAP_UNCACHED | MAP_ANONYMOUS, -1, 0);
	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, devmem_common.nc);
}


TEST_TEAR_DOWN(msg_uncached)
{
	TEST_ASSERT_EQUAL_INT(0, munmap(devmem_common.nc, DEVMEM_SIZE));
	(void)unlink(devmem_path);
}


TEST(msg_uncached, write_head)
{
	devmem_writeArrives(devmem_common.nc, OFFS_HEAD, LEN_HEAD);
}


TEST(msg_uncached, write_span)
{
	devmem_writeArrives(devmem_common.nc, OFFS_SPAN, LEN_SPAN);
}


TEST(msg_uncached, write_tail)
{
	devmem_writeArrives(devmem_common.nc, OFFS_TAIL, LEN_TAIL);
}


TEST(msg_uncached, write_packed)
{
	devmem_writeArrives(devmem_common.nc, OFFS_PACKED, LEN_PACKED);
}


TEST(msg_uncached, read_head)
{
	devmem_readArrives(devmem_common.nc, OFFS_HEAD, LEN_RDHEAD);
}


TEST(msg_uncached, read_span)
{
	devmem_readArrives(devmem_common.nc, OFFS_SPAN, LEN_SPAN);
}


TEST(msg_uncached, read_packed)
{
	devmem_readArrives(devmem_common.nc, OFFS_PACKED, LEN_PACKED);
}


TEST_SETUP(msg_devmem)
{
	addr_t pa;

	devmem_fillPattern();

	/* Physically contiguous, so that the pages can be mapped again by physical address */
	devmem_common.nc = mmap(NULL, DEVMEM_SIZE, PROT_READ | PROT_WRITE, MAP_UNCACHED | MAP_CONTIGUOUS | MAP_ANONYMOUS, -1, 0);
	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, devmem_common.nc);
	memset(devmem_common.nc, 0, DEVMEM_SIZE);
	pa = va2pa(devmem_common.nc);
	TEST_ASSERT_NOT_EQUAL_UINT64(0, (uint64_t)pa);
	TEST_ASSERT_EQUAL_UINT64((uint64_t)pa + DEVMEM_PAGE, (uint64_t)va2pa(devmem_common.nc + DEVMEM_PAGE));

	devmem_common.dev = mmap(NULL, DEVMEM_SIZE, PROT_READ | PROT_WRITE, MAP_DEVICE | MAP_UNCACHED | MAP_PHYSMEM | MAP_ANONYMOUS, -1, (off_t)pa);
	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, devmem_common.dev);
}


TEST_TEAR_DOWN(msg_devmem)
{
	TEST_ASSERT_EQUAL_INT(0, munmap(devmem_common.dev, DEVMEM_SIZE));
	TEST_ASSERT_EQUAL_INT(0, munmap(devmem_common.nc, DEVMEM_SIZE));
	(void)unlink(devmem_path);
}


/* The incident: the file server's thread copies the partial page */
TEST(msg_devmem, write_head)
{
	devmem_writeRefused(OFFS_HEAD, LEN_HEAD);
}


TEST(msg_devmem, write_span)
{
	devmem_writeRefused(OFFS_SPAN, LEN_SPAN);
}


TEST(msg_devmem, write_tail)
{
	devmem_writeRefused(OFFS_TAIL, LEN_TAIL);
}


/* Old kernels: this thread copies the payload into the message and faults itself */
TEST(msg_devmem, write_packed)
{
	devmem_writeRefused(OFFS_PACKED, LEN_PACKED);
}


TEST(msg_devmem, read_head)
{
	devmem_readRefused(OFFS_HEAD, LEN_RDHEAD);
}


/* Old kernels: this thread copies the packed response into the buffer and faults itself */
TEST(msg_devmem, read_packed)
{
	devmem_readRefused(OFFS_PACKED, LEN_PACKED);
}


TEST_SETUP(msg_devnext)
{
	addr_t pa;

	devmem_fillPattern();

	/* The physical page behind the device mapping (never touched through it) */
	devmem_common.nc = mmap(NULL, DEVMEM_PAGE, PROT_READ | PROT_WRITE, MAP_UNCACHED | MAP_CONTIGUOUS | MAP_ANONYMOUS, -1, 0);
	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, devmem_common.nc);
	pa = va2pa(devmem_common.nc);
	TEST_ASSERT_NOT_EQUAL_UINT64(0, (uint64_t)pa);

	/* Two ordinary pages, the second one then replaced by a device mapping */
	devmem_common.mem = mmap(NULL, DEVMEM_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, devmem_common.mem);
	devmem_common.dev = mmap(devmem_common.mem + DEVMEM_PAGE, DEVMEM_PAGE, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_DEVICE | MAP_UNCACHED | MAP_PHYSMEM | MAP_ANONYMOUS, -1, (off_t)pa);
	TEST_ASSERT_EQUAL_PTR(devmem_common.mem + DEVMEM_PAGE, devmem_common.dev);
}


TEST_TEAR_DOWN(msg_devnext)
{
	TEST_ASSERT_EQUAL_INT(0, munmap(devmem_common.mem, DEVMEM_SIZE));
	TEST_ASSERT_EQUAL_INT(0, munmap(devmem_common.nc, DEVMEM_PAGE));
	(void)unlink(devmem_path);
}


/* mig-q3-fix: the rest of a stdout line after a short write() to the console */
TEST(msg_devnext, write_head)
{
	devmem_writeArrives(devmem_common.mem, OFFS_NEXT, LEN_NEXT);
}


TEST(msg_devnext, write_packed)
{
	devmem_writeArrives(devmem_common.mem, OFFS_PACKED, LEN_PACKED);
}


TEST(msg_devnext, read_head)
{
	devmem_readArrives(devmem_common.mem, OFFS_NEXT, LEN_RDHEAD);
}


TEST(msg_devnext, read_packed)
{
	devmem_readArrives(devmem_common.mem, OFFS_PACKED, LEN_PACKED);
}


TEST_GROUP_RUNNER(msg_uncached)
{
	RUN_TEST_CASE(msg_uncached, write_head);
	RUN_TEST_CASE(msg_uncached, write_span);
	RUN_TEST_CASE(msg_uncached, write_tail);
	RUN_TEST_CASE(msg_uncached, write_packed);
	RUN_TEST_CASE(msg_uncached, read_head);
	RUN_TEST_CASE(msg_uncached, read_span);
	RUN_TEST_CASE(msg_uncached, read_packed);
}


TEST_GROUP_RUNNER(msg_devmem)
{
	RUN_TEST_CASE(msg_devmem, write_head);
	RUN_TEST_CASE(msg_devmem, write_span);
	RUN_TEST_CASE(msg_devmem, write_tail);
	RUN_TEST_CASE(msg_devmem, write_packed);
	RUN_TEST_CASE(msg_devmem, read_head);
	RUN_TEST_CASE(msg_devmem, read_packed);
}


TEST_GROUP_RUNNER(msg_devnext)
{
	RUN_TEST_CASE(msg_devnext, write_head);
	RUN_TEST_CASE(msg_devnext, write_packed);
	RUN_TEST_CASE(msg_devnext, read_head);
	RUN_TEST_CASE(msg_devnext, read_packed);
}


static void runner(void)
{
	RUN_TEST_GROUP(msg_uncached);
	RUN_TEST_GROUP(msg_devmem);
	RUN_TEST_GROUP(msg_devnext);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
