/*
 * Phoenix-RTOS
 *
 * POSIX standard library functions tests
 *
 * HEADER:
 *    - sys/mman.h
 *
 * TESTED:
 *    - mlock(), munlock(), mlockall(), munlockall()
 *
 * Phoenix has no swap-to-disk, so memory locking is a no-op that succeeds.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <sys/mman.h>

#include <unity_fixture.h>


static char mlock_buf[4096];


TEST_GROUP(mlock_noswap);


TEST_SETUP(mlock_noswap)
{
}


TEST_TEAR_DOWN(mlock_noswap)
{
}


TEST(mlock_noswap, mlock_munlock)
{
	TEST_ASSERT_EQUAL_INT(0, mlock(mlock_buf, sizeof(mlock_buf)));
	TEST_ASSERT_EQUAL_INT(0, munlock(mlock_buf, sizeof(mlock_buf)));
}


TEST(mlock_noswap, mlockall_munlockall)
{
	TEST_ASSERT_EQUAL_INT(0, mlockall(MCL_CURRENT | MCL_FUTURE));
	TEST_ASSERT_EQUAL_INT(0, munlockall());
}


TEST_GROUP_RUNNER(mlock_noswap)
{
	RUN_TEST_CASE(mlock_noswap, mlock_munlock);
	RUN_TEST_CASE(mlock_noswap, mlockall_munlockall);
}
