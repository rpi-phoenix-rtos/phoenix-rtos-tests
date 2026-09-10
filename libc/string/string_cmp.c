/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 * HEADER:
 *    - string.h
 * TESTED:
 *    - memcmp()
 *    - strcmp()
 *    - strncmp()
 *    - strncasecmp()
 *    - strcoll()
 *
 * Copyright 2023 Phoenix Systems
 * Author: Damian Modzelewski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <strings.h>
#include <sys/mman.h>
#include <unity_fixture.h>

#include "testdata.h"

#define BUFF_SIZE 129
#define BIG_SIZE  1024

static char empty[BUFF_SIZE];

TEST_GROUP(string_memcmp);
TEST_GROUP(string_strncmp);
TEST_GROUP(string_strcmp);
TEST_GROUP(string_strcoll);


/*
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
*/


TEST_SETUP(string_memcmp)
{
}


TEST_TEAR_DOWN(string_memcmp)
{
}


TEST(string_memcmp, basic)
{
	const char *basicStr = "Test";

	TEST_ASSERT_EQUAL_INT(0, memcmp(basicStr, basicStr, 5));
	TEST_ASSERT_LESS_THAN_INT(0, memcmp(basicStr, "Tests", 5));
	TEST_ASSERT_GREATER_THAN_INT(0, memcmp("Tests", basicStr, 5));
}


TEST(string_memcmp, unsigned_char_cast)
{
	const signed char charEdgeValue = SCHAR_MIN;

	TEST_ASSERT_GREATER_THAN_INT(0, memcmp(&charEdgeValue, "\0", 1));
	TEST_ASSERT_LESS_THAN_INT(0, memcmp("\0", &charEdgeValue, 1));
}


TEST(string_memcmp, emptyInput)
{
	char *asciiSet = testdata_createCharStr(BUFF_SIZE),
		 separated[] = "\0\0\0\0\0TEST\0\0";

	TEST_ASSERT_NOT_NULL(asciiSet);

	asciiSet[0] = 0;

	TEST_ASSERT_LESS_THAN_INT(0, memcmp(empty, asciiSet, sizeof(empty)));
	TEST_ASSERT_GREATER_THAN_INT(0, memcmp(asciiSet, empty, sizeof(empty)));
	TEST_ASSERT_EQUAL_INT(0, memcmp(empty, empty, sizeof(empty)));

	/* Memory cmp is not sensitive for NUL characters */
	TEST_ASSERT_NOT_EQUAL_INT(0, memcmp(empty, separated, sizeof(separated)));

	free(asciiSet);
}


TEST(string_memcmp, big)
{
	char *hugeStr = testdata_createCharStr(BIG_SIZE),
		 hugeStr2[BIG_SIZE];

	TEST_ASSERT_NOT_NULL(hugeStr);

	memcpy(hugeStr2, hugeStr, BIG_SIZE);

	TEST_ASSERT_EQUAL_INT(0, memcmp(hugeStr, hugeStr, BIG_SIZE));
	/* Comparing the same strings, that are placed in different location */
	TEST_ASSERT_EQUAL_INT(0, memcmp(hugeStr, hugeStr2, BIG_SIZE));

	hugeStr[BIG_SIZE - 2] = 1;
	hugeStr2[sizeof(hugeStr2) - 2] = 2;

	TEST_ASSERT_LESS_THAN_INT(0, memcmp(hugeStr, hugeStr2, BIG_SIZE));
	TEST_ASSERT_GREATER_THAN_INT(0, memcmp(hugeStr2, hugeStr, BIG_SIZE));

	free(hugeStr);
}


TEST(string_memcmp, various_sizes)
{
	int i;

	char *asciiStr = testdata_createCharStr(BUFF_SIZE),
		 asciiStr2[BUFF_SIZE];

	TEST_ASSERT_NOT_NULL(asciiStr);

	memcpy(asciiStr2, asciiStr, sizeof(asciiStr2));
	for (i = 1; i < BUFF_SIZE - 1; i++) {
		asciiStr2[i] = asciiStr[i] - 1;
		TEST_ASSERT_EQUAL_INT(0, memcmp(asciiStr, asciiStr2, i));
		TEST_ASSERT_GREATER_THAN_INT(0, memcmp(asciiStr, asciiStr2, i + 1));
		asciiStr2[i] = asciiStr[i];
	}

	free(asciiStr);
}


TEST(string_memcmp, offsets)
{

	char dataSet[4000] = { 0 },
		 supportSet[4000] = { 0 };
	int s1Offs, s2Offs, szOffset, sz = sizeof(dataSet);

	for (s1Offs = 0; s1Offs < sz; s1Offs++) {
		dataSet[s1Offs] = s1Offs;
		supportSet[s1Offs] = s1Offs;
	}

	/*Testing different offset of data blocks with same space or different*/
	for (s1Offs = 0; s1Offs < 8; s1Offs++) {
		for (s2Offs = 0; s2Offs < 8; s2Offs++) {
			for (szOffset = 0; szOffset < 8; szOffset++) {

				if (s2Offs < s1Offs) {
					TEST_ASSERT_GREATER_THAN_INT(0, memcmp(&dataSet[s1Offs], &dataSet[s2Offs], sz - (s1Offs + szOffset)));
					TEST_ASSERT_GREATER_THAN_INT(0, memcmp(&dataSet[s1Offs], &supportSet[s2Offs], sz - (s1Offs + szOffset)));
				}
				else if (s2Offs == s1Offs) {
					TEST_ASSERT_EQUAL_INT(0, memcmp(&dataSet[s1Offs], &dataSet[s2Offs], sz - (s1Offs + s2Offs + szOffset)));
					TEST_ASSERT_EQUAL_INT(0, memcmp(&dataSet[s1Offs], &supportSet[s2Offs], sz - (s1Offs + s2Offs + szOffset)));
				}
				else {
					TEST_ASSERT_LESS_THAN_INT(0, memcmp(&dataSet[s1Offs], &dataSet[s2Offs], sz - (s2Offs + szOffset)));
					TEST_ASSERT_LESS_THAN_INT(0, memcmp(&dataSet[s1Offs], &supportSet[s2Offs], sz - (s2Offs + szOffset)));
				}
			}
		}
	}
}


/*
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
*/


TEST_SETUP(string_strncmp)
{
}


TEST_TEAR_DOWN(string_strncmp)
{
}


TEST(string_strncmp, basic)
{
	const char *basicStr = "Test";

	TEST_ASSERT_EQUAL_INT(0, strncmp(basicStr, basicStr, 6));
	TEST_ASSERT_LESS_THAN_INT(0, strncmp(basicStr, "Tests", 6));
	TEST_ASSERT_GREATER_THAN_INT(0, strncmp("Tests", basicStr, 6));
}


TEST(string_strncmp, unsigned_char_cast)
{
	const char charEdgeValue[1] = { SCHAR_MIN };

	TEST_ASSERT_GREATER_THAN_INT(0, strncmp(charEdgeValue, "\0", 1));
	TEST_ASSERT_LESS_THAN_INT(0, strncmp("\0", charEdgeValue, 1));
}


TEST(string_strncmp, emptyInput)
{
	char *asciiStr = testdata_createCharStr(BUFF_SIZE),
		 separated[] = "\0\0\0\0\0TEST\0\0";

	TEST_ASSERT_NOT_NULL(asciiStr);

	TEST_ASSERT_EQUAL_INT(0, strncmp(empty, empty, BUFF_SIZE));
	TEST_ASSERT_LESS_THAN_INT(0, strncmp(empty, asciiStr, BUFF_SIZE));
	TEST_ASSERT_GREATER_THAN_INT(0, strncmp(asciiStr, empty, BUFF_SIZE));

	/* Otherwise than in memcmp, strncmp is NUL character sensitive and treats 0 as the end of array */
	TEST_ASSERT_EQUAL_INT(0, strncmp(empty, separated, BUFF_SIZE));

	free(asciiStr);
}


TEST(string_strncmp, big)
{
	char *hugeStr = testdata_createCharStr(BIG_SIZE),
		 hugeStr2[BIG_SIZE];

	TEST_ASSERT_NOT_NULL(hugeStr);

	memcpy(hugeStr2, hugeStr, BIG_SIZE);

	TEST_ASSERT_EQUAL_INT(0, strncmp(hugeStr, hugeStr, BIG_SIZE));
	/* Comparing the same strings, that are placed in different location */
	TEST_ASSERT_EQUAL_INT(0, strncmp(hugeStr, hugeStr2, BIG_SIZE));

	hugeStr[BIG_SIZE - 2] = 1;
	hugeStr2[sizeof(hugeStr2) - 2] = 2;

	TEST_ASSERT_LESS_THAN_INT(0, strncmp(hugeStr, hugeStr2, BIG_SIZE));
	TEST_ASSERT_GREATER_THAN_INT(0, strncmp(hugeStr2, hugeStr, BIG_SIZE));

	free(hugeStr);
}


TEST(string_strncmp, various_sizes)
{
	int i;

	char *asciiStr = testdata_createCharStr(BUFF_SIZE),
		 asciiStr2[BUFF_SIZE];

	TEST_ASSERT_NOT_NULL(asciiStr);

	memcpy(asciiStr2, asciiStr, sizeof(asciiStr2));
	for (i = 1; i < BUFF_SIZE - 1; i++) {
		asciiStr2[i] = asciiStr[i] - 1;
		TEST_ASSERT_EQUAL_INT(0, strncmp(asciiStr, asciiStr2, i));
		TEST_ASSERT_GREATER_THAN_INT(0, strncmp(asciiStr, asciiStr2, i + 1));
		asciiStr2[i] = asciiStr[i];
	}

	free(asciiStr);
}


TEST(string_strncmp, offsets)
{
	char dataSet[4000] = { 0 },
		 supportSet[4000] = { 0 };
	int s1Offs, s2Offs, szOffset, sz = sizeof(dataSet);

	memset(dataSet, 1, sizeof(dataSet));
	memset(supportSet, 1, sizeof(supportSet));

	for (s1Offs = 0; s1Offs < sz; s1Offs++) {
		dataSet[s1Offs] = s1Offs;
		supportSet[s1Offs] = s1Offs;
	}

	/*Testing different offset of data blocks with same space or different*/
	for (s1Offs = 0; s1Offs < 8; s1Offs++) {
		for (s2Offs = 0; s2Offs < 8; s2Offs++) {
			for (szOffset = 0; szOffset < 8; szOffset++) {

				if (s2Offs < s1Offs) {
					TEST_ASSERT_GREATER_THAN_INT(0, strncmp(&dataSet[s1Offs], &dataSet[s2Offs], sz - (s1Offs + szOffset)));
					TEST_ASSERT_GREATER_THAN_INT(0, strncmp(&dataSet[s1Offs], &supportSet[s2Offs], sz - (s1Offs + szOffset)));
				}
				else if (s2Offs == s1Offs) {
					TEST_ASSERT_EQUAL_INT(0, strncmp(&dataSet[s1Offs], &dataSet[s2Offs], sz - (s1Offs + s2Offs + szOffset)));
					TEST_ASSERT_EQUAL_INT(0, strncmp(&dataSet[s1Offs], &supportSet[s2Offs], sz - (s1Offs + s2Offs + szOffset)));
				}
				else {
					TEST_ASSERT_LESS_THAN_INT(0, strncmp(&dataSet[s1Offs], &dataSet[s2Offs], sz - (s2Offs + szOffset)));
					TEST_ASSERT_LESS_THAN_INT(0, strncmp(&dataSet[s1Offs], &supportSet[s2Offs], sz - (s2Offs + szOffset)));
				}
			}
		}
	}
}


/*
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
*/


TEST_SETUP(string_strcmp)
{
}


TEST_TEAR_DOWN(string_strcmp)
{
}


TEST(string_strcmp, basic)
{
	const char *basicStr = "Test";

	TEST_ASSERT_EQUAL_INT(0, strcmp(basicStr, basicStr));
	TEST_ASSERT_LESS_THAN_INT(0, strcmp(basicStr, "Tests"));
	TEST_ASSERT_GREATER_THAN_INT(0, strcmp("Tests", basicStr));
}


TEST(string_strcmp, unsigned_char_cast)
{
	const char charEdgeValue[1] = { SCHAR_MIN };

	TEST_ASSERT_GREATER_THAN_INT(0, strcmp(charEdgeValue, "\0"));
	TEST_ASSERT_LESS_THAN_INT(0, strcmp("\0", charEdgeValue));
}


TEST(string_strcmp, emptyInput)
{
	char *asciiStr = testdata_createCharStr(BUFF_SIZE),
		 separated[] = "\0\0\0\0\0TEST\0\0";

	TEST_ASSERT_NOT_NULL(asciiStr);

	TEST_ASSERT_LESS_THAN_INT(0, strcmp(empty, asciiStr));
	TEST_ASSERT_GREATER_THAN_INT(0, strcmp(asciiStr, empty));
	TEST_ASSERT_EQUAL_INT(0, strcmp(empty, empty));

	/* Otherwise than in memcmp, strcmp is NUL character sensitive and treats 0 as the end of array */
	TEST_ASSERT_EQUAL_INT(0, strcmp(empty, separated));

	free(asciiStr);
}


TEST(string_strcmp, big)
{
	char *hugeStr = testdata_createCharStr(BIG_SIZE),
		 hugeStr2[BIG_SIZE];

	TEST_ASSERT_NOT_NULL(hugeStr);

	memcpy(hugeStr2, hugeStr, BIG_SIZE);

	TEST_ASSERT_EQUAL_INT(0, strcmp(hugeStr, hugeStr));
	/* Comparing the same strings, that are placed in different location */
	TEST_ASSERT_EQUAL_INT(0, strcmp(hugeStr, hugeStr2));

	hugeStr[BIG_SIZE - 2] = 1;
	hugeStr2[sizeof(hugeStr2) - 2] = 2;

	TEST_ASSERT_LESS_THAN_INT(0, strcmp(hugeStr, hugeStr2));
	TEST_ASSERT_GREATER_THAN_INT(0, strcmp(hugeStr2, hugeStr));

	free(hugeStr);
}


TEST(string_strcmp, offsets)
{
	char dataSet[4000] = { 0 },
		 supportSet[4000] = { 0 };
	int s1Offs, s2Offs, szOffset, sz = sizeof(dataSet);

	memset(dataSet, 1, sizeof(dataSet));
	memset(supportSet, 1, sizeof(supportSet));

	for (s1Offs = 0; s1Offs < sz; s1Offs++) {
		dataSet[s1Offs] = s1Offs;
		supportSet[s1Offs] = s1Offs;
	}

	/*Testing different offset of data blocks with same space or different*/
	for (s1Offs = 0; s1Offs < 8; s1Offs++) {
		for (s2Offs = 0; s2Offs < 8; s2Offs++) {
			for (szOffset = 0; szOffset < 8; szOffset++) {

				if (s2Offs < s1Offs) {
					TEST_ASSERT_GREATER_THAN_INT(0, strcmp(&dataSet[s1Offs], &dataSet[s2Offs]));
					TEST_ASSERT_GREATER_THAN_INT(0, strcmp(&dataSet[s1Offs], &supportSet[s2Offs]));
				}
				else if (s2Offs == s1Offs) {
					TEST_ASSERT_EQUAL_INT(0, strcmp(&dataSet[s1Offs], &dataSet[s2Offs]));
					TEST_ASSERT_EQUAL_INT(0, strcmp(&dataSet[s1Offs], &supportSet[s2Offs]));
				}
				else {
					TEST_ASSERT_LESS_THAN_INT(0, strcmp(&dataSet[s1Offs], &dataSet[s2Offs]));
					TEST_ASSERT_LESS_THAN_INT(0, strcmp(&dataSet[s1Offs], &supportSet[s2Offs]));
				}
			}
		}
	}
}


/*
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
*/


TEST_SETUP(string_strcoll)
{
}


TEST_TEAR_DOWN(string_strcoll)
{
}


TEST(string_strcoll, basic)
{
	const char *basicStr = "Test";

	TEST_ASSERT_EQUAL_INT(0, strcmp(basicStr, basicStr));
	TEST_ASSERT_LESS_THAN_INT(0, strcmp(basicStr, "Tests"));
	TEST_ASSERT_GREATER_THAN_INT(0, strcmp("Tests", basicStr));
}


TEST(string_strcoll, emptyInput)
{
	char *asciiStr = testdata_createCharStr(BUFF_SIZE),
		 separated[] = "\0\0\0\0\0TEST\0\0";

	TEST_ASSERT_NOT_NULL(asciiStr);

	TEST_ASSERT_EQUAL_INT(0, strcoll(empty, empty));
	TEST_ASSERT_LESS_THAN_INT(0, strcoll(empty, asciiStr));
	TEST_ASSERT_GREATER_THAN_INT(0, strcoll(asciiStr, empty));

	/* Otherwise than in memcmp, strcoll is NUL character sensitive and treats 0 as the end of array */
	TEST_ASSERT_EQUAL_INT(0, strcoll(empty, separated));

	free(asciiStr);
}


TEST(string_strcoll, big)
{
	char *hugeStr = testdata_createCharStr(BIG_SIZE),
		 hugeStr2[BIG_SIZE];

	TEST_ASSERT_NOT_NULL(hugeStr);

	memcpy(hugeStr2, hugeStr, BIG_SIZE);

	TEST_ASSERT_EQUAL_INT(0, strcoll(hugeStr, hugeStr));
	/* Comparing the same strings, that are placed in different location */
	TEST_ASSERT_EQUAL_INT(0, strcoll(hugeStr, hugeStr2));
	hugeStr[BIG_SIZE - 2] = 1;
	hugeStr2[sizeof(hugeStr2) - 2] = 2;
	TEST_ASSERT_LESS_THAN_INT(0, strcoll(hugeStr, hugeStr2));
	TEST_ASSERT_GREATER_THAN_INT(0, strcoll(hugeStr2, hugeStr));

	free(hugeStr);
}


TEST(string_strcoll, offsets)
{
	char dataSet[4000] = { 0 },
		 supportSet[4000] = { 0 };
	int s1Offs, s2Offs, szOffset, sz = sizeof(dataSet);

	memset(dataSet, 1, sizeof(dataSet));
	memset(supportSet, 1, sizeof(supportSet));

	for (s1Offs = 0; s1Offs < sz; s1Offs++) {
		dataSet[s1Offs] = s1Offs;
		supportSet[s1Offs] = s1Offs;
	}

	/*Testing different offset of data blocks with same space or different*/
	for (s1Offs = 0; s1Offs < 8; s1Offs++) {
		for (s2Offs = 0; s2Offs < 8; s2Offs++) {
			for (szOffset = 0; szOffset < 8; szOffset++) {

				if (s2Offs < s1Offs) {
					TEST_ASSERT_GREATER_THAN_INT(0, strcoll(&dataSet[s1Offs], &dataSet[s2Offs]));
					TEST_ASSERT_GREATER_THAN_INT(0, strcoll(&dataSet[s1Offs], &supportSet[s2Offs]));
				}
				else if (s2Offs == s1Offs) {
					TEST_ASSERT_EQUAL_INT(0, strcoll(&dataSet[s1Offs], &dataSet[s2Offs]));
					TEST_ASSERT_EQUAL_INT(0, strcoll(&dataSet[s1Offs], &supportSet[s2Offs]));
				}
				else {
					TEST_ASSERT_LESS_THAN_INT(0, strcoll(&dataSet[s1Offs], &dataSet[s2Offs]));
					TEST_ASSERT_LESS_THAN_INT(0, strcoll(&dataSet[s1Offs], &supportSet[s2Offs]));
				}
			}
		}
	}
}


/*
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
*/


TEST_GROUP_RUNNER(string_memcmp)
{
	RUN_TEST_CASE(string_memcmp, basic);
	RUN_TEST_CASE(string_memcmp, unsigned_char_cast);
	RUN_TEST_CASE(string_memcmp, emptyInput);
	RUN_TEST_CASE(string_memcmp, big);
	RUN_TEST_CASE(string_memcmp, various_sizes);
	RUN_TEST_CASE(string_memcmp, offsets);
}


/* An n-bounded compare must read AT MOST n bytes from each operand. The way to
 * prove it is to make byte n unreadable: map two pages, unmap the second, and
 * hand the function a string whose last byte is the final byte of the first
 * page, with NO terminator after it. An implementation that dereferences one
 * byte too far faults here instead of returning.
 *
 * munmap (not mprotect) makes the guard, because mprotect is best-effort on this
 * target and a PROT_NONE page that stays readable would make this test pass
 * vacuously -- the failure mode being tested is a READ, so the guard has to be a
 * genuinely absent mapping.
 *
 * That the guard is real was checked, not assumed: unmapping the second page of a
 * two-page mapping is a TAIL TRIM, and the kernel's _vm_munmap() (vm/map.c) handles
 * it explicitly -- `else if ((ptr_t)(e->vaddr + e->size) == overlapEnd)` shrinks the
 * entry rather than ignoring the request. If that ever stops splitting entries this
 * test goes quietly green while testing nothing, so re-check it there first.
 *
 * Regression test for a real fault: strncmp() looped on `*p && k < n`, and C's
 * left-to-right evaluation reads us1[n] before the bound is checked. On a
 * Raspberry Pi 4 that came back as Exception #36: Data Abort (EL0), far at a page
 * base, inside strncmp with n=1, reached from AngelScript's tokenizer. It needs a
 * string ending exactly at a page boundary, so it presented as an intermittent
 * crash. strncasecmp() had the identical loop and no test coverage at all.
 */
TEST(string_strncmp, no_read_past_n_at_page_edge)
{
	long pagesz = sysconf(_SC_PAGESIZE);
	char *region, *edge;

	TEST_ASSERT_GREATER_THAN_INT(0, pagesz);

	region = mmap(NULL, 2 * (size_t)pagesz, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	TEST_ASSERT_NOT_EQUAL_MESSAGE(MAP_FAILED, region, "cannot map the guard region");

	/* Drop the second page: reading region[pagesz] now faults. */
	TEST_ASSERT_EQUAL_INT(0, munmap(region + pagesz, (size_t)pagesz));

	/* Deliberately unterminated: every readable byte is 'x'. */
	memset(region, 'x', (size_t)pagesz);
	edge = region + pagesz - 1;

	/* Equal on the last readable byte: must not look at byte 1. */
	TEST_ASSERT_EQUAL_INT(0, strncmp(edge, "x", 1));
	TEST_ASSERT_EQUAL_INT(0, strncasecmp(edge, "X", 1));

	/* Unequal on byte 0: decided before the bound matters either way. */
	TEST_ASSERT_LESS_THAN_INT(0, strncmp(edge, "y", 1));
	TEST_ASSERT_GREATER_THAN_INT(0, strncmp(edge, "w", 1));

	/* n == 0 permits no read at all, so even a wholly invalid pointer is legal. */
	TEST_ASSERT_EQUAL_INT(0, strncmp(region + pagesz, "x", 0));
	TEST_ASSERT_EQUAL_INT(0, strncasecmp(region + pagesz, "x", 0));

	TEST_ASSERT_EQUAL_INT(0, munmap(region, (size_t)pagesz));
}


TEST_GROUP_RUNNER(string_strncmp)
{
	RUN_TEST_CASE(string_strncmp, basic);
	RUN_TEST_CASE(string_strncmp, unsigned_char_cast);
	RUN_TEST_CASE(string_strncmp, emptyInput);
	RUN_TEST_CASE(string_strncmp, big);
	RUN_TEST_CASE(string_strncmp, various_sizes);
	RUN_TEST_CASE(string_strncmp, offsets);
	RUN_TEST_CASE(string_strncmp, no_read_past_n_at_page_edge);
}


TEST_GROUP_RUNNER(string_strcmp)
{
	RUN_TEST_CASE(string_strcmp, basic);
	RUN_TEST_CASE(string_strcmp, unsigned_char_cast);
	RUN_TEST_CASE(string_strcmp, emptyInput);
	RUN_TEST_CASE(string_strcmp, big);
	RUN_TEST_CASE(string_strcmp, offsets);
}


TEST_GROUP_RUNNER(string_strcoll)
{
	RUN_TEST_CASE(string_strcoll, basic);
	RUN_TEST_CASE(string_strcoll, emptyInput);
	RUN_TEST_CASE(string_strcoll, big);
	RUN_TEST_CASE(string_strcoll, offsets);
}
