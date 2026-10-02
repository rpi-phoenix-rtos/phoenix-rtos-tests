/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 * HEADER:
 *    - dirent.h
 * TESTED:
 *    - readdir()
 *
 * Copyright 2023-2026 Phoenix Systems
 * Authors: Arkadiusz Kozlowski, Lukasz Kruszynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include <fcntl.h>

#include <unity_fixture.h>

#include "common.h"
#include "dirent_helper_functions.h"

#define MAIN_DIR            "test_readdir"
#define INO_T_TEST_MAX_DIRS 10

int d_ino_in(ino_t arg, ino_t *arr, int n)
{
	for (int i = 0; i < n; ++i) {
		if (arg == arr[i]) {
			return i;
		}
	}
	return -1;
}


TEST_GROUP(dirent_readdir);

TEST_SETUP(dirent_readdir)
{
	TEST_MKDIR_ASSERTED(MAIN_DIR, 0700);

	TEST_MKDIR_ASSERTED(MAIN_DIR "/dir1", S_IRUSR | S_IWUSR | S_IXUSR);
	TEST_MKDIR_ASSERTED(MAIN_DIR "/dir2", S_IRUSR | S_IWUSR | S_IXUSR);

	TEST_MKDIR_ASSERTED(MAIN_DIR "/dir1/nest1", S_IRUSR);
	TEST_MKDIR_ASSERTED(MAIN_DIR "/dir1/nest2", S_IRUSR);

	TEST_MKDIR_ASSERTED(MAIN_DIR "/dir2/nest1", S_IRUSR);
	TEST_MKDIR_ASSERTED(MAIN_DIR "/dir2/nest2", S_IRUSR);

	int fd;

	fd = creat(MAIN_DIR "/file1.txt", S_IRUSR);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
	TEST_ASSERT_EQUAL_INT(0, close(fd));

	fd = creat(MAIN_DIR "/file2.dat", S_IRUSR);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
	TEST_ASSERT_EQUAL_INT(0, close(fd));

	fd = creat(MAIN_DIR "/file3.json", S_IRUSR);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
	TEST_ASSERT_EQUAL_INT(0, close(fd));
}


TEST_TEAR_DOWN(dirent_readdir)
{
	rmdir(MAIN_DIR "/dir1/nest1");
	rmdir(MAIN_DIR "/dir1/nest2");

	rmdir(MAIN_DIR "/dir2/nest1");
	rmdir(MAIN_DIR "/dir2/nest2");

	rmdir(MAIN_DIR "/dir1");
	rmdir(MAIN_DIR "/dir2");

	remove(MAIN_DIR "/file1.txt");
	remove(MAIN_DIR "/file2.dat");
	remove(MAIN_DIR "/file3.json");

	rmdir(MAIN_DIR);
}


TEST(dirent_readdir, long_name_directory_check)
{
	/* issue #1615: https://github.com/phoenix-rtos/phoenix-rtos-project/issues/1615 */
	TEST_IGNORE_MESSAGE("#1615 issue");
	DIR *dp = NULL;
	struct dirent *info;
	char longDirName[NAME_MAX + 1];
	char longDirPath[NAME_MAX + 2 + sizeof(MAIN_DIR)];
	int dir_created = 0;
	int found = 0;

	memset(longDirName, 'a', NAME_MAX);
	longDirName[NAME_MAX] = '\0';
	snprintf(longDirPath, sizeof(longDirPath), MAIN_DIR "/%s", longDirName);

	if (TEST_PROTECT()) {
		TEST_MKDIR_ASSERTED(longDirPath, S_IRUSR);
		dir_created = 1;

		dp = TEST_OPENDIR_ASSERTED(MAIN_DIR);

		while ((info = readdir(dp)) != NULL) {
			if (info->d_name[0] == 'a') {
				TEST_ASSERT_EQUAL_STRING(longDirName, info->d_name);
				TEST_ASSERT_EQUAL_INT(NAME_MAX, strlen(info->d_name));
				found = 1;
				break;
			}
		}
		TEST_ASSERT_TRUE_MESSAGE(found, "Long directory name not found");
	}

	if (dp != NULL)
		closedir(dp);
	if (dir_created)
		rmdir(longDirPath);
}


TEST(dirent_readdir, basic_listing_count)
{
	DIR *dp = TEST_OPENDIR_ASSERTED(MAIN_DIR);
	int entry_counter = 0;
	struct dirent *info;

	while ((info = readdir(dp)) != NULL) {
		entry_counter++;
	}

	closedir(dp);

	/* 5 files from setup, and both . and .. directories */
	TEST_ASSERT_EQUAL_INT(7, entry_counter);
}


TEST(dirent_readdir, reading_in_parent_and_child)
{
	DIR *dp1 = NULL, *dp2 = NULL;
	pid_t pid = -1;
	int status;

	if (TEST_PROTECT()) {
		dp1 = TEST_OPENDIR_ASSERTED(MAIN_DIR "/dir1");
		dp2 = TEST_OPENDIR_ASSERTED(MAIN_DIR "/dir2");

		TEST_ASSERT_NOT_NULL(readdir(dp1));
		TEST_ASSERT_NOT_NULL(readdir(dp2));

		pid = fork();

		if (pid == -1) {
			TEST_IGNORE_MESSAGE("Fork failed");
		}

		/* Since there are two different dir streams, there is no reading from the same stream in two processes */
		if (pid > 0) {
			/* Check for parent */
			TEST_ASSERT_NOT_NULL(readdir(dp1));
			rewinddir(dp1);
			TEST_ASSERT_NOT_NULL(readdir(dp1));
			wait(&status);
			TEST_ASSERT_TRUE(WIFEXITED(status));
			TEST_ASSERT_EQUAL_INT(EXIT_SUCCESS, WEXITSTATUS(status));
		}

		else {
			/* Check for child */
			status = EXIT_SUCCESS;
			if (!readdir(dp2)) {
				status = EXIT_FAILURE;
			}

			rewinddir(dp2);

			if (!readdir(dp2)) {
				status = EXIT_FAILURE;
			}


			closedir(dp1);
			closedir(dp2);
			_exit(status);
		}
	}
	if (dp1 != NULL) {
		closedir(dp1);
	}
	if (dp2 != NULL) {
		closedir(dp2);
	}
}


TEST(dirent_readdir, hardlink_inode_correct_number)
{
	const char *originalFilePath = MAIN_DIR "/original_file.txt";
	const char *linkFilePath = MAIN_DIR "/linked_file.txt";
	int created = 0;
	int linked = 0;
	DIR *dp = NULL;
	FILE *f = NULL;

	if (TEST_PROTECT()) {
		f = fopen(originalFilePath, "w+");
		TEST_ASSERT_NOT_NULL(f);
		fclose(f);
		created = 1;

		TEST_ASSERT_EQUAL_INT(0, link(originalFilePath, linkFilePath));
		linked = 1;

		dp = TEST_OPENDIR_ASSERTED(MAIN_DIR);
		struct dirent *info;
		ino_t orig_ino = 0;
		ino_t link_ino = 0;
		int files_found = 0;

		while ((info = readdir(dp)) != NULL) {
			if (!strcmp(info->d_name, "original_file.txt")) {
				orig_ino = info->d_ino;
				files_found++;
			}
			else if (!strcmp(info->d_name, "linked_file.txt")) {
				link_ino = info->d_ino;
				files_found++;
			}
		}

		TEST_ASSERT_EQUAL_INT(2, files_found);
		TEST_ASSERT_EQUAL_UINT64(orig_ino, link_ino);
	}

	if (dp != NULL)
		closedir(dp);
	if (created) {
		unlink(linkFilePath);
	}
	if (linked) {
		remove(originalFilePath);
	}
}


TEST(dirent_readdir, distinct_inode_nums)
{
	ino_t inode_arr[INO_T_TEST_MAX_DIRS];
	DIR *dp = NULL;
	int inode_counter = 0;
	struct dirent *info;

	if (TEST_PROTECT()) {
		dp = TEST_OPENDIR_ASSERTED(MAIN_DIR);
		/* Assert distinct inodes */
		while ((info = readdir(dp)) != NULL) {
			TEST_ASSERT_LESS_THAN_INT(INO_T_TEST_MAX_DIRS, inode_counter);
			/* Pass inode_counter as 'n' to avoid reading garbage/uninitialized memory */
			TEST_ASSERT_EQUAL_INT(-1, d_ino_in(info->d_ino, inode_arr, inode_counter));
			inode_arr[inode_counter++] = info->d_ino;
		}
	}
	if (dp != NULL) {
		closedir(dp);
	}
}


TEST(dirent_readdir, same_file_reading_by_two_pointers)
{
	DIR *dp1 = NULL, *dp2 = NULL;

	if (TEST_PROTECT()) {
		dp1 = TEST_OPENDIR_ASSERTED(MAIN_DIR);
		dp2 = TEST_OPENDIR_ASSERTED(MAIN_DIR);

		int counter = 2;

		TEST_ASSERT_NOT_NULL(readdir(dp1));
		TEST_ASSERT_NOT_NULL(readdir(dp1));

		errno = 0;

		while (readdir(dp2) != NULL) {
			continue;
		}

		TEST_ASSERT_EQUAL_INT(0, errno);

		while (readdir(dp1) != NULL) {
			counter++;
		}

		TEST_ASSERT_EQUAL_INT(0, errno);

		TEST_ASSERT_EQUAL_INT(7, counter);
	}
	if (dp1 != NULL) {
		closedir(dp1);
	}
	if (dp2 != NULL) {
		closedir(dp2);
	}
}


TEST(dirent_readdir, correct_dirent_names)
{
	struct dirent *info;
	int filename_bits = 0;
	DIR *dp = TEST_OPENDIR_ASSERTED(MAIN_DIR);
	if (TEST_PROTECT()) {
		while ((info = readdir(dp)) != NULL) {

			/* Set corresponding bit of filename_bits each time info->d_name is encountered */
			if (!strcmp(info->d_name, "dir1")) {

				filename_bits |= 1;
			}

			else if (!strcmp(info->d_name, "file1.txt")) {
				filename_bits |= 2;
			}

			else if (!strcmp(info->d_name, "file2.dat")) {
				filename_bits |= 4;
			}

			else if (!strcmp(info->d_name, ".")) {
				filename_bits |= 8;
			}

			else if (!strcmp(info->d_name, "file3.json")) {
				filename_bits |= 16;
			}

			else if (!strcmp(info->d_name, "..")) {
				filename_bits |= 32;
			}

			else if (!strcmp(info->d_name, "dir2")) {
				filename_bits |= 64;
			}

			else
				TEST_FAIL_MESSAGE(info->d_name);
		}
	}

	TEST_ASSERT_EQUAL_INT(0x7f, filename_bits);
	if (dp != NULL) {
		closedir(dp);
	}
}


TEST(dirent_readdir, read_past_end_of_stream)
{
	DIR *dp = NULL;

	if (TEST_PROTECT()) {
		dp = TEST_OPENDIR_ASSERTED(MAIN_DIR);
		while (readdir(dp) != NULL) {
			continue;
		}

		errno = 0;
		for (int i = 0; i < 10; i++) {
			TEST_ASSERT_NULL(readdir(dp));
			TEST_ASSERT_EQUAL_INT(0, errno);
		}
	}

	if (dp != NULL) {
		closedir(dp);
	}
}


TEST(dirent_readdir, large_directory_pagination)
{
	const int NUM_FILES = 40;
	char path[PATH_MAX];
	DIR *dp = NULL;
	int count = 0;
	int dir_created = 0;
	int files_created = 0;

	memset(path, 0, sizeof(path));

	if (TEST_PROTECT()) {
		TEST_MKDIR_ASSERTED(MAIN_DIR "/stress_dir", 0700);
		dir_created = 1;

		for (int i = 0; i < NUM_FILES; i++) {
			sprintf(path, MAIN_DIR "/stress_dir/file_%d.txt", i);
			int fd = creat(path, S_IRUSR);
			TEST_ASSERT_NOT_EQUAL(-1, fd);
			close(fd);
			files_created++;
		}

		dp = TEST_OPENDIR_ASSERTED(MAIN_DIR "/stress_dir");

		while (readdir(dp) != NULL) {
			count++;
		}
		/* NUM_FILES + "." + ".." */
		TEST_ASSERT_EQUAL_INT(NUM_FILES + 2, count);
	}

	if (dp != NULL)
		closedir(dp);

	if (dir_created) {
		for (int i = 0; i < files_created; i++) {
			snprintf(path, PATH_MAX, MAIN_DIR "/stress_dir/file_%d.txt", i);
			remove(path);
		}
		rmdir(MAIN_DIR "/stress_dir");
	}
}


TEST(dirent_readdir, unlink_during_iteration)
{
	DIR *dp = NULL;
	struct dirent *info;
	const char *victim_path = MAIN_DIR "/victim.txt";
	int file_created = 0;

	if (TEST_PROTECT()) {
		int fd = creat(victim_path, S_IRUSR);
		TEST_ASSERT_NOT_EQUAL(-1, fd);
		close(fd);
		file_created = 1;

		dp = TEST_OPENDIR_ASSERTED(MAIN_DIR);
		TEST_ASSERT_NOT_NULL(dp);

		info = readdir(dp);
		TEST_ASSERT_NOT_NULL(info);

		TEST_ASSERT_EQUAL_INT(0, remove(victim_path));
		/* Clear flag to avoid double remove */
		file_created = 0;

		errno = 0;
		while ((info = readdir(dp)) != NULL) {
			continue;
		}

		TEST_ASSERT_EQUAL_INT(0, errno);
	}

	if (dp != NULL)
		closedir(dp);
	if (file_created)
		remove(victim_path);
}


/*
 * Removing entries while iterating (rm -rf, find -delete, GLib's recursive
 * delete). Each entry must be returned exactly once and the directory must end
 * up empty: a readdir position that shifts when an earlier entry is removed
 * skips the next one, and rm -rf then fails with "Directory not empty"
 * (seen on the NFS root and on dummyfs). Enough entries for an ext2 directory
 * to span several blocks.
 */

#define WALK_DIR      MAIN_DIR "/walk"
#define WALK_FILES    400
#define WALK_SUBDIRS  8
#define WALK_SUBFILES 5


static void walk_path(char *buf, size_t size, int file, int subdir, int inner)
{
	if (subdir < 0) {
		snprintf(buf, size, WALK_DIR "/entry-%04d-with-a-longer-name", file);
	}
	else if (inner < 0) {
		snprintf(buf, size, WALK_DIR "/subdir-%02d", subdir);
	}
	else {
		snprintf(buf, size, WALK_DIR "/subdir-%02d/inner-%d", subdir, inner);
	}
}


static void walk_create(int files, int subdirs)
{
	char path[PATH_MAX];

	TEST_MKDIR_ASSERTED(WALK_DIR, 0700);

	/* subdirectories spread among the files, so the scan meets them mid-way */
	for (int i = 0; i < files; i++) {
		walk_path(path, sizeof(path), i, -1, -1);
		int fd = creat(path, S_IRUSR | S_IWUSR);
		TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
		TEST_ASSERT_EQUAL_INT(0, close(fd));

		if ((subdirs > 0) && ((i % (files / subdirs)) == 0) && ((i / (files / subdirs)) < subdirs)) {
			int j = i / (files / subdirs);
			walk_path(path, sizeof(path), 0, j, -1);
			TEST_MKDIR_ASSERTED(path, 0700);
			for (int k = 0; k < WALK_SUBFILES; k++) {
				walk_path(path, sizeof(path), 0, j, k);
				fd = creat(path, S_IRUSR | S_IWUSR);
				TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
				TEST_ASSERT_EQUAL_INT(0, close(fd));
			}
		}
	}
}


/* Remove everything by name, without readdir(), so a failed test leaves nothing behind */
static void walk_cleanup(void)
{
	char path[PATH_MAX];

	for (int i = 0; i < WALK_FILES; i++) {
		walk_path(path, sizeof(path), i, -1, -1);
		unlink(path);
	}
	for (int j = 0; j < WALK_SUBDIRS; j++) {
		for (int k = 0; k < WALK_SUBFILES; k++) {
			walk_path(path, sizeof(path), 0, j, k);
			unlink(path);
		}
		walk_path(path, sizeof(path), 0, j, -1);
		rmdir(path);
	}
	rmdir(WALK_DIR);
}


static int walk_seenFile[WALK_FILES];
static int walk_seenDir[WALK_SUBDIRS];
static int walk_seenInner[WALK_SUBDIRS][WALK_SUBFILES];
static int walk_unknown, walk_rmFailed;


static int walk_isDot(const char *name)
{
	return (strcmp(name, ".") == 0) || (strcmp(name, "..") == 0);
}


/* rm -rf of one subdirectory's contents: read an entry, remove it, read on */
static void walk_rmSub(int j)
{
	char path[PATH_MAX];
	struct dirent *info;
	int k;

	walk_path(path, sizeof(path), 0, j, -1);
	DIR *dp = opendir(path);
	if (dp == NULL) {
		walk_rmFailed++;
		return;
	}

	while ((info = readdir(dp)) != NULL) {
		if (walk_isDot(info->d_name)) {
			continue;
		}
		if ((sscanf(info->d_name, "inner-%d", &k) != 1) || (k < 0) || (k >= WALK_SUBFILES)) {
			walk_unknown++;
			continue;
		}
		walk_seenInner[j][k]++;
		walk_path(path, sizeof(path), 0, j, k);
		if (unlink(path) != 0) {
			walk_rmFailed++;
		}
	}
	closedir(dp);
}


/* rm -rf WALK_DIR, the way busybox does it: each entry is removed (a
 * subdirectory after its own contents) before the next one is read */
static void walk_rmTop(void)
{
	char path[PATH_MAX];
	struct dirent *info;
	int i;

	memset(walk_seenFile, 0, sizeof(walk_seenFile));
	memset(walk_seenDir, 0, sizeof(walk_seenDir));
	memset(walk_seenInner, 0, sizeof(walk_seenInner));
	walk_unknown = 0;
	walk_rmFailed = 0;

	DIR *dp = TEST_OPENDIR_ASSERTED(WALK_DIR);
	while ((info = readdir(dp)) != NULL) {
		if (walk_isDot(info->d_name)) {
			continue;
		}
		if ((sscanf(info->d_name, "entry-%d", &i) == 1) && (i >= 0) && (i < WALK_FILES)) {
			walk_seenFile[i]++;
			walk_path(path, sizeof(path), i, -1, -1);
			if (unlink(path) != 0) {
				walk_rmFailed++;
			}
		}
		else if ((sscanf(info->d_name, "subdir-%d", &i) == 1) && (i >= 0) && (i < WALK_SUBDIRS)) {
			walk_seenDir[i]++;
			walk_rmSub(i);
			walk_path(path, sizeof(path), 0, i, -1);
			if (rmdir(path) != 0) {
				walk_rmFailed++;
			}
		}
		else {
			walk_unknown++;
		}
	}
	closedir(dp);
}


static int walk_countOff(const int *seen, int n, int *repeated)
{
	int missed = 0;

	for (int i = 0; i < n; i++) {
		if (seen[i] == 0) {
			missed++;
		}
		else if (seen[i] > 1) {
			(*repeated)++;
		}
	}

	return missed;
}


TEST(dirent_readdir, rm_rf_while_iterating)
{
	if (TEST_PROTECT()) {
		walk_create(WALK_FILES, WALK_SUBDIRS);
		walk_rmTop();

		int repeated = 0;
		int missed = walk_countOff(walk_seenFile, WALK_FILES, &repeated);
		missed += walk_countOff(walk_seenDir, WALK_SUBDIRS, &repeated);
		missed += walk_countOff(&walk_seenInner[0][0], WALK_SUBDIRS * WALK_SUBFILES, &repeated);

		TEST_ASSERT_EQUAL_INT_MESSAGE(0, missed, "entries never returned by readdir()");
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, repeated, "entries returned more than once");
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, walk_unknown, "names that were never created");
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, walk_rmFailed, "unlink()/rmdir() failures");
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, rmdir(WALK_DIR), "rmdir of the emptied directory");
	}

	walk_cleanup();
}


TEST(dirent_readdir, unlink_each_entry_as_read)
{
	char path[PATH_MAX];
	struct dirent *info;
	int i;

	if (TEST_PROTECT()) {
		walk_create(WALK_FILES, 0);
		memset(walk_seenFile, 0, sizeof(walk_seenFile));
		walk_unknown = 0;
		walk_rmFailed = 0;

		DIR *dp = TEST_OPENDIR_ASSERTED(WALK_DIR);
		while ((info = readdir(dp)) != NULL) {
			if (walk_isDot(info->d_name)) {
				continue;
			}
			if ((sscanf(info->d_name, "entry-%d", &i) != 1) || (i < 0) || (i >= WALK_FILES)) {
				walk_unknown++;
				continue;
			}
			walk_seenFile[i]++;
			walk_path(path, sizeof(path), i, -1, -1);
			if (unlink(path) != 0) {
				walk_rmFailed++;
			}
		}
		closedir(dp);

		int repeated = 0;
		int missed = walk_countOff(walk_seenFile, WALK_FILES, &repeated);
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, missed, "entries never returned by readdir()");
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, repeated, "entries returned more than once");
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, walk_unknown, "names that were never created");
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, walk_rmFailed, "unlink() failures");
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, rmdir(WALK_DIR), "rmdir of the emptied directory");
	}

	walk_cleanup();
}


/* Another process removes an entry this scan has not reached yet. What must
 * hold everywhere: nothing else is lost or repeated. POSIX leaves open whether
 * an entry removed after opendir() is still returned, and a libc that buffers
 * several entries per call (glibc) may already hold it. libphoenix asks the
 * filesystem for every entry, so on Phoenix a returned victim is a name the
 * filesystem had already removed (ext2 used to read its leftover bytes). */
TEST(dirent_readdir, unlink_unread_entry)
{
	const int files = 64;
	static int order[64];
	char path[PATH_MAX];
	struct dirent *info;
	DIR *dp = NULL;
	int i, n = 0;

	if (TEST_PROTECT()) {
		walk_create(files, 0);
		dp = TEST_OPENDIR_ASSERTED(WALK_DIR);

		while ((info = readdir(dp)) != NULL) {
			if ((sscanf(info->d_name, "entry-%d", &i) == 1) && (n < files)) {
				order[n++] = i;
			}
		}
		TEST_ASSERT_EQUAL_INT(files, n);

		/* Read up to the victim, so the stream is positioned exactly at it */
		rewinddir(dp);
		memset(walk_seenFile, 0, sizeof(walk_seenFile));
		for (int got = 0; got < files / 2;) {
			info = readdir(dp);
			TEST_ASSERT_NOT_NULL(info);
			if (sscanf(info->d_name, "entry-%d", &i) == 1) {
				walk_seenFile[i]++;
				got++;
			}
		}

		int victim = order[files / 2];
		walk_path(path, sizeof(path), victim, -1, -1);
		TEST_ASSERT_EQUAL_INT(0, unlink(path));

		while ((info = readdir(dp)) != NULL) {
			if (sscanf(info->d_name, "entry-%d", &i) == 1) {
				walk_seenFile[i]++;
			}
		}

		int missed = 0, repeated = 0;
		for (i = 0; i < files; i++) {
			if (i == victim) {
				continue;
			}
			if (walk_seenFile[i] == 0) {
				missed++;
			}
			else if (walk_seenFile[i] > 1) {
				repeated++;
			}
		}
#ifdef __phoenix__
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, walk_seenFile[victim], "the unlinked entry was returned");
#else
		TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(1, walk_seenFile[victim], "the unlinked entry was returned twice");
#endif
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, missed, "other entries never returned");
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, repeated, "other entries returned more than once");
	}

	if (dp != NULL) {
		closedir(dp);
	}
	walk_cleanup();
}


TEST_GROUP_RUNNER(dirent_readdir)
{
	RUN_TEST_CASE(dirent_readdir, basic_listing_count);
	RUN_TEST_CASE(dirent_readdir, correct_dirent_names);
	RUN_TEST_CASE(dirent_readdir, distinct_inode_nums);
	RUN_TEST_CASE(dirent_readdir, hardlink_inode_correct_number);
	RUN_TEST_CASE(dirent_readdir, same_file_reading_by_two_pointers);
	RUN_TEST_CASE(dirent_readdir, reading_in_parent_and_child);
	RUN_TEST_CASE(dirent_readdir, long_name_directory_check);
	RUN_TEST_CASE(dirent_readdir, read_past_end_of_stream);
	RUN_TEST_CASE(dirent_readdir, large_directory_pagination);
	RUN_TEST_CASE(dirent_readdir, unlink_during_iteration);
	RUN_TEST_CASE(dirent_readdir, rm_rf_while_iterating);
	RUN_TEST_CASE(dirent_readdir, unlink_each_entry_as_read);
	RUN_TEST_CASE(dirent_readdir, unlink_unread_entry);
}
