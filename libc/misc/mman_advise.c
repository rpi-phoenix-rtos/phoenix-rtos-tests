/*
 * Phoenix-RTOS
 *
 * POSIX standard library functions tests
 *
 * HEADER:
 *    - sys/mman.h
 *
 * TESTED:
 *    - posix_madvise()
 *    - madvise() (BSD/Linux)
 *
 * posix_madvise() advice is a hint: it never changes what a range holds.
 * madvise(MADV_DONTNEED) is not: on Linux a private range reads back as zeros
 * afterwards, and allocators (jemalloc, WebKit's libpas) skip clearing reused
 * memory because of it. A system that cannot drop pages must say so with an
 * error, never report success and keep the old bytes. The cases below hold on
 * both kinds of system.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <unity_fixture.h>


#define ADVISE_PAGES 4


static unsigned char *advise_buf;
static size_t advise_len;


static void advise_fill(void)
{
	size_t i;

	for (i = 0; i < advise_len; i++) {
		advise_buf[i] = (unsigned char)(i * 7u + 1u);
	}
}


static int advise_intact(void)
{
	size_t i;

	for (i = 0; i < advise_len; i++) {
		if (advise_buf[i] != (unsigned char)(i * 7u + 1u)) {
			return 0;
		}
	}

	return 1;
}


static int advise_zero(void)
{
	size_t i;

	for (i = 0; i < advise_len; i++) {
		if (advise_buf[i] != 0u) {
			return 0;
		}
	}

	return 1;
}


TEST_GROUP(mman_advise);


TEST_SETUP(mman_advise)
{
	advise_len = ADVISE_PAGES * (size_t)sysconf(_SC_PAGESIZE);
	advise_buf = mmap(NULL, advise_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	TEST_ASSERT_TRUE(advise_buf != MAP_FAILED);
	advise_fill();
}


TEST_TEAR_DOWN(mman_advise)
{
	TEST_ASSERT_EQUAL_INT(0, munmap(advise_buf, advise_len));
}


TEST(mman_advise, posix_hints_keep_contents)
{
	static const int advice[] = {
		POSIX_MADV_NORMAL,
		POSIX_MADV_RANDOM,
		POSIX_MADV_SEQUENTIAL,
		POSIX_MADV_WILLNEED,
		POSIX_MADV_DONTNEED,
	};
	size_t i;

	for (i = 0; i < sizeof(advice) / sizeof(advice[0]); i++) {
		TEST_ASSERT_EQUAL_INT(0, posix_madvise(advise_buf, advise_len, advice[i]));
		TEST_ASSERT_TRUE(advise_intact());
	}
}


TEST(mman_advise, posix_bad_arguments)
{
	int err = errno;

	/* The error is the return value; errno is left alone */
	TEST_ASSERT_EQUAL_INT(EINVAL, posix_madvise(advise_buf, advise_len, 12345));
	TEST_ASSERT_EQUAL_INT(EINVAL, posix_madvise(advise_buf + 1, advise_len - 1, POSIX_MADV_NORMAL));
	TEST_ASSERT_EQUAL_INT(err, errno);
}


TEST(mman_advise, access_hints_keep_contents)
{
	static const int advice[] = { MADV_NORMAL, MADV_RANDOM, MADV_SEQUENTIAL, MADV_WILLNEED };
	size_t i;

	for (i = 0; i < sizeof(advice) / sizeof(advice[0]); i++) {
		TEST_ASSERT_EQUAL_INT(0, madvise(advise_buf, advise_len, advice[i]));
		TEST_ASSERT_TRUE(advise_intact());
	}
}


/* MADV_FREE: the old bytes or zeros, page by page; and the range stays usable */
TEST(mman_advise, free_leaves_usable_memory)
{
	size_t i, page = (size_t)sysconf(_SC_PAGESIZE);

	TEST_ASSERT_EQUAL_INT(0, madvise(advise_buf, advise_len, MADV_FREE));
	for (i = 0; i < advise_len; i++) {
		TEST_ASSERT_TRUE((advise_buf[i] == 0u) || (advise_buf[i] == (unsigned char)(i * 7u + 1u)));
	}

	memset(advise_buf, 0xa5, page);
	TEST_ASSERT_EQUAL_HEX8(0xa5, advise_buf[page - 1]);
}


/* Either it discards (zeros) or it fails; success with old data is the bug */
TEST(mman_advise, dontneed_never_fakes_a_discard)
{
	int rc = madvise(advise_buf, advise_len, MADV_DONTNEED);

	if (rc == 0) {
		TEST_ASSERT_TRUE(advise_zero());
	}
	else {
		TEST_ASSERT_EQUAL_INT(-1, rc);
		TEST_ASSERT_EQUAL_INT(EINVAL, errno);
		TEST_ASSERT_TRUE(advise_intact());
	}
}


TEST(mman_advise, bad_arguments)
{
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, madvise(advise_buf, advise_len, 12345));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);

	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, madvise(advise_buf + 1, advise_len - 1, MADV_NORMAL));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);

	/* An empty range is not an error */
	TEST_ASSERT_EQUAL_INT(0, madvise(advise_buf, 0, MADV_NORMAL));
	TEST_ASSERT_TRUE(advise_intact());
}


TEST_GROUP_RUNNER(mman_advise)
{
	RUN_TEST_CASE(mman_advise, posix_hints_keep_contents);
	RUN_TEST_CASE(mman_advise, posix_bad_arguments);
	RUN_TEST_CASE(mman_advise, access_hints_keep_contents);
	RUN_TEST_CASE(mman_advise, free_leaves_usable_memory);
	RUN_TEST_CASE(mman_advise, dontneed_never_fakes_a_discard);
	RUN_TEST_CASE(mman_advise, bad_arguments);
}
