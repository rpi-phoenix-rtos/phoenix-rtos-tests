/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - dirent.h
 *
 * TESTED:
 *    - scandir()
 *    - alphasort()
 *
 * Both were missing (Mesa's xmlconfig.c needs scandir; Window Maker carried
 * its own copy). The directory is created fresh in the current directory,
 * like the other dirent groups, and "." / ".." are filtered out because
 * whether readdir() returns them depends on the filesystem.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <unity_fixture.h>


#define SCANDIR_DIR "test_scandir"

static const char *const scandir_names[] = { "delta", "alpha", "charlie", "bravo", "echo_long_name_0123456789" };
#define SCANDIR_N (sizeof(scandir_names) / sizeof(scandir_names[0]))


static int scandir_noDots(const struct dirent *d)
{
	return (d->d_name[0] != '.') ? 1 : 0;
}


static int scandir_onlyA(const struct dirent *d)
{
	return (strchr(d->d_name, 'a') != NULL) && (d->d_name[0] != '.');
}


static int scandir_reverse(const struct dirent **a, const struct dirent **b)
{
	return -strcmp((*a)->d_name, (*b)->d_name);
}


static void scandir_free(struct dirent **list, int n)
{
	while (n > 0) {
		free(list[--n]);
	}
	free(list);
}


TEST_GROUP(dirent_scandir);


TEST_SETUP(dirent_scandir)
{
	char path[128];
	size_t i;
	int fd;

	mkdir(SCANDIR_DIR, 0777);
	for (i = 0; i < SCANDIR_N; i++) {
		strcpy(path, SCANDIR_DIR "/");
		strcat(path, scandir_names[i]);
		fd = open(path, O_WRONLY | O_CREAT, 0666);
		TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
		close(fd);
	}
}


TEST_TEAR_DOWN(dirent_scandir)
{
	char path[128];
	size_t i;

	for (i = 0; i < SCANDIR_N; i++) {
		strcpy(path, SCANDIR_DIR "/");
		strcat(path, scandir_names[i]);
		unlink(path);
	}
	rmdir(SCANDIR_DIR);
}


TEST(dirent_scandir, sorted_with_alphasort)
{
	struct dirent **list = NULL;
	int n = scandir(SCANDIR_DIR, &list, scandir_noDots, alphasort);

	TEST_ASSERT_EQUAL_INT((int)SCANDIR_N, n);
	TEST_ASSERT_EQUAL_STRING("alpha", list[0]->d_name);
	TEST_ASSERT_EQUAL_STRING("bravo", list[1]->d_name);
	TEST_ASSERT_EQUAL_STRING("charlie", list[2]->d_name);
	TEST_ASSERT_EQUAL_STRING("delta", list[3]->d_name);
	/* each entry holds its whole name, however long */
	TEST_ASSERT_EQUAL_STRING("echo_long_name_0123456789", list[4]->d_name);
	scandir_free(list, n);
}


TEST(dirent_scandir, filter_and_custom_order)
{
	struct dirent **list = NULL;
	int n = scandir(SCANDIR_DIR, &list, scandir_onlyA, scandir_reverse);

	/* names containing 'a': delta, alpha, charlie, bravo, echo_long_name */
	TEST_ASSERT_EQUAL_INT(5, n);
	TEST_ASSERT_EQUAL_STRING("echo_long_name_0123456789", list[0]->d_name);
	TEST_ASSERT_EQUAL_STRING("delta", list[1]->d_name);
	TEST_ASSERT_EQUAL_STRING("alpha", list[4]->d_name);
	scandir_free(list, n);
}


TEST(dirent_scandir, unsorted_and_unfiltered)
{
	struct dirent **list = NULL;
	int n = scandir(SCANDIR_DIR, &list, NULL, NULL), i, found = 0;

	/* everything, possibly plus "." and ".." */
	TEST_ASSERT_GREATER_OR_EQUAL_INT((int)SCANDIR_N, n);
	TEST_ASSERT_LESS_OR_EQUAL_INT((int)SCANDIR_N + 2, n);
	for (i = 0; i < n; i++) {
		if (list[i]->d_name[0] != '.') {
			found++;
		}
	}
	TEST_ASSERT_EQUAL_INT((int)SCANDIR_N, found);
	scandir_free(list, n);
}


TEST(dirent_scandir, missing_directory)
{
	struct dirent **list = (struct dirent **)&list;

	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, scandir(SCANDIR_DIR "/no-such-dir", &list, NULL, alphasort));
	TEST_ASSERT_EQUAL_INT(ENOENT, errno);
	TEST_ASSERT_TRUE(list == (struct dirent **)&list); /* untouched */
}


TEST(dirent_scandir, alphasort_orders_names)
{
	struct dirent **list = NULL;
	int n = scandir(SCANDIR_DIR, &list, scandir_noDots, alphasort), i;

	TEST_ASSERT_EQUAL_INT((int)SCANDIR_N, n);
	for (i = 1; i < n; i++) {
		TEST_ASSERT_TRUE(alphasort((const struct dirent **)&list[i - 1], (const struct dirent **)&list[i]) < 0);
		TEST_ASSERT_TRUE(alphasort((const struct dirent **)&list[i], (const struct dirent **)&list[i - 1]) > 0);
	}
	TEST_ASSERT_EQUAL_INT(0, alphasort((const struct dirent **)&list[0], (const struct dirent **)&list[0]));
	scandir_free(list, n);
}


TEST_GROUP_RUNNER(dirent_scandir)
{
	RUN_TEST_CASE(dirent_scandir, sorted_with_alphasort);
	RUN_TEST_CASE(dirent_scandir, filter_and_custom_order);
	RUN_TEST_CASE(dirent_scandir, unsorted_and_unfiltered);
	RUN_TEST_CASE(dirent_scandir, missing_directory);
	RUN_TEST_CASE(dirent_scandir, alphasort_orders_names);
}
