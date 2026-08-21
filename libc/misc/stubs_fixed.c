/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - stdlib.h (wctomb)
 *    - sys/sysmacros.h (makedev / major / minor)
 *
 * TESTED (libphoenix — functions that were unimplemented stubs returning 0):
 *    - wctomb()  — C/POSIX-locale 1:1 byte encoding + EILSEQ out of range
 *    - makedev() / major() / minor() — consistent dev_t pack/extract
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdlib.h>
#include <sys/sysmacros.h>
#include <errno.h>
#include <string.h>
#include <wchar.h>

#include <unity_fixture.h>


TEST_GROUP(misc_stubs_fixed);


TEST_SETUP(misc_stubs_fixed)
{
}


TEST_TEAR_DOWN(misc_stubs_fixed)
{
}


TEST(misc_stubs_fixed, wctomb_ascii)
{
	char buf[4];

	/* str==NULL queries state-dependence: C locale is stateless -> 0 */
	TEST_ASSERT_EQUAL_INT(0, wctomb(NULL, 0));

	memset(buf, 0xaa, sizeof(buf));
	TEST_ASSERT_EQUAL_INT(1, wctomb(buf, L'A'));
	TEST_ASSERT_EQUAL_UINT8('A', (unsigned char)buf[0]);

	TEST_ASSERT_EQUAL_INT(1, wctomb(buf, L'\0'));
	TEST_ASSERT_EQUAL_UINT8(0x00, (unsigned char)buf[0]);

	TEST_ASSERT_EQUAL_INT(1, wctomb(buf, (wchar_t)0xff));
	TEST_ASSERT_EQUAL_UINT8(0xff, (unsigned char)buf[0]);

	/* out of single-byte range -> EILSEQ, -1 */
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, wctomb(buf, (wchar_t)0x100));
	TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
}


TEST(misc_stubs_fixed, dev_makedev_roundtrip)
{
	dev_t d;

	d = makedev(8, 1);
	TEST_ASSERT_EQUAL_UINT(8, major(d));
	TEST_ASSERT_EQUAL_UINT(1, minor(d));

	d = makedev(259, 42);
	TEST_ASSERT_EQUAL_UINT(259, major(d));
	TEST_ASSERT_EQUAL_UINT(42, minor(d));

	/* distinct (maj,min) must not collide — the stub made everything 0 */
	TEST_ASSERT_TRUE(makedev(0, 0) != makedev(8, 1));
	TEST_ASSERT_TRUE(makedev(1, 0) != makedev(0, 1));

	/* large minor exercises the high-minor split */
	d = makedev(4, 0x12345);
	TEST_ASSERT_EQUAL_UINT(4, major(d));
	TEST_ASSERT_EQUAL_UINT(0x12345, minor(d));
}


TEST_GROUP_RUNNER(misc_stubs_fixed)
{
	RUN_TEST_CASE(misc_stubs_fixed, wctomb_ascii);
	RUN_TEST_CASE(misc_stubs_fixed, dev_makedev_roundtrip);
}
