/*
 * Phoenix-RTOS
 *
 * POSIX/BSD standard library functions tests
 *
 * HEADER:
 *    - sys/statfs.h
 *
 * TESTED:
 *    - statfs(), fstatfs()  (thin mapping over the statvfs syscall)
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <sys/statfs.h>
#include <fcntl.h>
#include <unistd.h>

#include <unity_fixture.h>


TEST_GROUP(statfs_basic);


TEST_SETUP(statfs_basic)
{
}


TEST_TEAR_DOWN(statfs_basic)
{
}


TEST(statfs_basic, statfs_root)
{
	struct statfs sf;

	sf.f_bsize = -1;
	sf.f_type = -1;

	TEST_ASSERT_EQUAL_INT(0, statfs("/", &sf));
	TEST_ASSERT_TRUE(sf.f_bsize > 0);       /* mapped from statvfs f_bsize */
	TEST_ASSERT_EQUAL_INT(0, sf.f_type);    /* Phoenix: no fs-type magic */
	TEST_ASSERT_TRUE(sf.f_namelen > 0);
}


TEST(statfs_basic, fstatfs_fd)
{
	struct statfs sf;
	int fd;

	fd = open("/", O_RDONLY);
	TEST_ASSERT_TRUE(fd >= 0);

	sf.f_bsize = -1;
	TEST_ASSERT_EQUAL_INT(0, fstatfs(fd, &sf));
	TEST_ASSERT_TRUE(sf.f_bsize > 0);

	close(fd);
}


TEST_GROUP_RUNNER(statfs_basic)
{
	RUN_TEST_CASE(statfs_basic, statfs_root);
	RUN_TEST_CASE(statfs_basic, fstatfs_fd);
}
