/*
 * Phoenix-RTOS
 *
 * POSIX/BSD standard library functions tests
 *
 * HEADER:
 *    - mntent.h
 *    - sys/mount.h (mount_record(), a Phoenix extension)
 *
 * TESTED:
 *    - setmntent(), getmntent(), getmntent_r(), addmntent(), endmntent(), hasmntopt()
 *    - the mount table MOUNTED: one entry per directory, sorted, mounted now
 *
 * df without arguments printed "no mount table support": there was no table.
 * MOUNTED is now a log on devfs that the mounters append to; reading it with
 * setmntent(MOUNTED, "r") returns the mounts in effect. The parser cases cover
 * what the first getmntent_r() got wrong: a malformed line ended the table,
 * missing fields were NULL, a long line was split in two entries.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/statvfs.h>

#include <unity_fixture.h>


#define MNTENT_TMPFILE "/tmp/test_mntent_fstab"


static FILE *mntent_open(const char *text)
{
	FILE *f = fmemopen((void *)text, strlen(text), "r");

	TEST_ASSERT_NOT_NULL(f);
	return f;
}


TEST_GROUP(misc_mntent);


TEST_SETUP(misc_mntent)
{
}


TEST_TEAR_DOWN(misc_mntent)
{
	(void)unlink(MNTENT_TMPFILE);
}


TEST(misc_mntent, parse_fields)
{
	char buf[256];
	struct mntent ent, *m;
	FILE *f = mntent_open("/dev/mmcblk0p2 / ext2 rw,noatime 1 2\n");

	m = getmntent_r(f, &ent, buf, sizeof(buf));
	TEST_ASSERT_EQUAL_PTR(&ent, m);
	TEST_ASSERT_EQUAL_STRING("/dev/mmcblk0p2", m->mnt_fsname);
	TEST_ASSERT_EQUAL_STRING("/", m->mnt_dir);
	TEST_ASSERT_EQUAL_STRING("ext2", m->mnt_type);
	TEST_ASSERT_EQUAL_STRING("rw,noatime", m->mnt_opts);
	TEST_ASSERT_EQUAL_INT(1, m->mnt_freq);
	TEST_ASSERT_EQUAL_INT(2, m->mnt_passno);
	TEST_ASSERT_NULL(getmntent_r(f, &ent, buf, sizeof(buf)));

	fclose(f);
}


TEST(misc_mntent, skip_comments_and_malformed)
{
	char buf[256];
	struct mntent ent, *m;
	FILE *f = mntent_open("# comment\n\n   \n  # indented comment\nonlyonefield\nsrc /mnt\n");

	/* The malformed line must not end the table. */
	m = getmntent_r(f, &ent, buf, sizeof(buf));
	TEST_ASSERT_NOT_NULL(m);
	TEST_ASSERT_EQUAL_STRING("src", m->mnt_fsname);
	TEST_ASSERT_EQUAL_STRING("/mnt", m->mnt_dir);
	/* Missing optional fields read as empty strings, not NULL. */
	TEST_ASSERT_EQUAL_STRING("", m->mnt_type);
	TEST_ASSERT_EQUAL_STRING("", m->mnt_opts);
	TEST_ASSERT_EQUAL_INT(0, m->mnt_freq);
	TEST_ASSERT_EQUAL_INT(0, m->mnt_passno);
	TEST_ASSERT_NULL(getmntent_r(f, &ent, buf, sizeof(buf)));

	fclose(f);
}


TEST(misc_mntent, skip_long_line)
{
	char buf[32];
	struct mntent ent, *m;
	FILE *f = mntent_open("this-device-name-is-far-too-long-for-the-buffer /x t o 0 0\nshort /y t o 0 0\n");

	/* Too long for the buffer: skipped whole, not split into two entries. */
	m = getmntent_r(f, &ent, buf, sizeof(buf));
	TEST_ASSERT_NOT_NULL(m);
	TEST_ASSERT_EQUAL_STRING("short", m->mnt_fsname);
	TEST_ASSERT_EQUAL_STRING("/y", m->mnt_dir);
	TEST_ASSERT_NULL(getmntent_r(f, &ent, buf, sizeof(buf)));

	fclose(f);
}


TEST(misc_mntent, escapes_round_trip)
{
	char *text = NULL;
	size_t size = 0;
	struct mntent in = { "my disk", "/mnt/a\tb\\c", "ext2", NULL, 0, 0 };
	struct mntent *m;
	FILE *f = open_memstream(&text, &size);

	TEST_ASSERT_NOT_NULL(f);
	TEST_ASSERT_EQUAL_INT(0, addmntent(f, &in));
	TEST_ASSERT_EQUAL_INT(0, fclose(f));
	/* Separators are escaped; missing options are written as "defaults". */
	TEST_ASSERT_EQUAL_STRING("my\\040disk /mnt/a\\011b\\134c ext2 defaults 0 0\n", text);

	f = mntent_open(text);
	m = getmntent(f);
	TEST_ASSERT_NOT_NULL(m);
	TEST_ASSERT_EQUAL_STRING("my disk", m->mnt_fsname);
	TEST_ASSERT_EQUAL_STRING("/mnt/a\tb\\c", m->mnt_dir);
	TEST_ASSERT_EQUAL_STRING("ext2", m->mnt_type);
	TEST_ASSERT_EQUAL_STRING(MNTOPT_DEFAULTS, m->mnt_opts);
	fclose(f);
	free(text);
}


TEST(misc_mntent, addmntent_rejects_incomplete)
{
	struct mntent noDir = { "src", NULL, "t", "o", 0, 0 };
	struct mntent noSrc = { "", "/x", "t", "o", 0, 0 };
	FILE *f = fopen("/dev/null", "w");

	TEST_ASSERT_NOT_NULL(f);
	TEST_ASSERT_NOT_EQUAL(0, addmntent(f, &noDir));
	TEST_ASSERT_NOT_EQUAL(0, addmntent(f, &noSrc));
	fclose(f);
}


TEST(misc_mntent, hasmntopt)
{
	struct mntent m = { "src", "/x", "t", "rw,noatime,size=4k", 0, 0 };

	TEST_ASSERT_EQUAL_PTR(m.mnt_opts, hasmntopt(&m, MNTOPT_RW));
	TEST_ASSERT_EQUAL_PTR(m.mnt_opts + 3, hasmntopt(&m, "noatime"));
	TEST_ASSERT_EQUAL_PTR(m.mnt_opts + 11, hasmntopt(&m, "size"));
	TEST_ASSERT_NULL(hasmntopt(&m, "noat"));
	TEST_ASSERT_NULL(hasmntopt(&m, MNTOPT_RO));
}


/* Any other file is read as written: duplicates and order are kept. */
TEST(misc_mntent, plain_file_is_not_filtered)
{
	struct mntent a = { "a", "/", "t", "o", 0, 0 };
	struct mntent b = { "b", "/", "t", "o", 0, 0 };
	struct mntent *m;
	FILE *f = setmntent(MNTENT_TMPFILE, "w");

	TEST_ASSERT_NOT_NULL(f);
	TEST_ASSERT_EQUAL_INT(0, addmntent(f, &b));
	TEST_ASSERT_EQUAL_INT(0, addmntent(f, &a));
	TEST_ASSERT_EQUAL_INT(1, endmntent(f));

	f = setmntent(MNTENT_TMPFILE, "r");
	TEST_ASSERT_NOT_NULL(f);
	m = getmntent(f);
	TEST_ASSERT_NOT_NULL(m);
	TEST_ASSERT_EQUAL_STRING("b", m->mnt_fsname);
	m = getmntent(f);
	TEST_ASSERT_NOT_NULL(m);
	TEST_ASSERT_EQUAL_STRING("a", m->mnt_fsname);
	TEST_ASSERT_NULL(getmntent(f));
	endmntent(f);
}


/* The live table: one entry per directory, sorted, each one a filesystem. */
TEST(misc_mntent, mounted_table)
{
	struct statvfs st;
	struct mntent *m;
	char *prev = NULL;
	int root = 0;
	FILE *f = setmntent(MOUNTED, "r");

	if (f == NULL) {
		TEST_IGNORE_MESSAGE("no mount table on this system (devfs not mounted on /dev?)");
	}

	while ((m = getmntent(f)) != NULL) {
		if (prev != NULL) {
			/* strictly increasing: sorted and no directory twice */
			TEST_ASSERT_TRUE_MESSAGE(strcmp(prev, m->mnt_dir) < 0, m->mnt_dir);
			free(prev);
		}
		prev = strdup(m->mnt_dir);
		TEST_ASSERT_NOT_NULL(prev);

		if (strcmp(m->mnt_dir, "/") == 0) {
			root = 1;
		}
		TEST_ASSERT_TRUE(m->mnt_fsname[0] != '\0');
		TEST_ASSERT_TRUE(m->mnt_type[0] != '\0');
		TEST_ASSERT_EQUAL_INT_MESSAGE(0, statvfs(m->mnt_dir, &st), m->mnt_dir);
	}
	free(prev);
	endmntent(f);

	if (root == 0) {
		TEST_IGNORE_MESSAGE("the root filesystem server does not record itself");
	}
}


/* A recorded directory that is not a mount point is not listed. (The entry stays
 * in the log; like an unmounted filesystem's, reading the table filters it.) */
TEST(misc_mntent, record_of_unmounted_dir_is_hidden)
{
	const char *dir = "/test-mntent-not-mounted";
	struct mntent *m;
	FILE *f;

	if (mount_record("test-mntent", dir, MNTTYPE_NONE, NULL) < 0) {
		TEST_ASSERT_EQUAL_INT(EAGAIN, errno);
		TEST_IGNORE_MESSAGE("mount table not writable (devfs not mounted on /dev?)");
	}

	f = setmntent(MOUNTED, "r");
	TEST_ASSERT_NOT_NULL(f);
	while ((m = getmntent(f)) != NULL) {
		TEST_ASSERT_NOT_EQUAL(0, strcmp(m->mnt_dir, dir));
	}
	endmntent(f);
}


TEST(misc_mntent, record_rejects_null)
{
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, mount_record(NULL, "/", "t", NULL));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}


TEST_GROUP_RUNNER(misc_mntent)
{
	RUN_TEST_CASE(misc_mntent, parse_fields);
	RUN_TEST_CASE(misc_mntent, skip_comments_and_malformed);
	RUN_TEST_CASE(misc_mntent, skip_long_line);
	RUN_TEST_CASE(misc_mntent, escapes_round_trip);
	RUN_TEST_CASE(misc_mntent, addmntent_rejects_incomplete);
	RUN_TEST_CASE(misc_mntent, hasmntopt);
	RUN_TEST_CASE(misc_mntent, plain_file_is_not_filtered);
	RUN_TEST_CASE(misc_mntent, mounted_table);
	RUN_TEST_CASE(misc_mntent, record_of_unmounted_dir_is_hidden);
	RUN_TEST_CASE(misc_mntent, record_rejects_null);
}
