/*
 * Phoenix-RTOS
 *
 * libc-tests
 *
 * Testing unistd.h file/fs/directory related functions
 *
 * Copyright 2022 Phoenix Systems
 * Author: Mateusz Niewiadomski, Damian Loewnau
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <unity_fixture.h>

#include "common.h"


#define FNAME   "unistd_fsdir_file"
#define DIRNAME "unistd_fsdir_directory"

static FILE *filep;
static char testWorkDir[PATH_MAX];
static char buf[PATH_MAX];
static char toolongpath[PATH_MAX + 16];

TEST_GROUP(unistd_fsdir);

TEST_SETUP(unistd_fsdir)
{
	/* clear buffer */
	memset(buf, 0, sizeof(buf));

	/* save the test working diectory */
	TEST_ASSERT_NOT_NULL(getcwd(testWorkDir, sizeof(testWorkDir)));

	/* clear/create file */
	filep = fopen(FNAME, "w");
	if (filep != NULL)
		fclose(filep);

	/* set too long path */
	memset(toolongpath, 'a', sizeof(toolongpath) - 1);
	toolongpath[sizeof(toolongpath) - 1] = '\0';
}


TEST_TEAR_DOWN(unistd_fsdir)
{
	/* go back to the test working directory */
	TEST_ASSERT_EQUAL_INT(0, chdir(testWorkDir));
	TEST_ASSERT_EQUAL_INT(0, remove(FNAME));
}


TEST(unistd_fsdir, getcwd)
{
	/* assumption that chdir("/") works fine when returning 0 */
	TEST_ASSERT_EQUAL_INT(0, chdir("/"));

	TEST_ASSERT_NOT_NULL(getcwd(buf, sizeof(buf)));
	TEST_ASSERT_EQUAL_STRING("/", buf);

	TEST_ASSERT_NULL(getcwd(buf, 0));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);

	TEST_ASSERT_NULL(getcwd(buf, 1));
	TEST_ASSERT_EQUAL_INT(ERANGE, errno);
}


TEST(unistd_fsdir, chdir_absroot)
{
	/* test chdir to root */
	TEST_ASSERT_EQUAL_INT(0, chdir("/"));
	TEST_ASSERT_NOT_NULL(getcwd(buf, sizeof(buf)));
	TEST_ASSERT_EQUAL_STRING("/", buf);

	/* go back to the test working directory and assert it */
	TEST_ASSERT_EQUAL_INT(0, chdir(testWorkDir));
	TEST_ASSERT_NOT_NULL(getcwd(buf, sizeof(buf)));
	TEST_ASSERT_EQUAL_STRING(testWorkDir, buf);
}


TEST(unistd_fsdir, chdir_absdev)
{
	/* test chdir to some directory */
	TEST_ASSERT_EQUAL_INT(0, chdir("/dev"));
	TEST_ASSERT_NOT_NULL(getcwd(buf, sizeof(buf)));
	TEST_ASSERT_EQUAL_STRING("/dev", buf);

	/* go back to the test working directory and assert it */
	TEST_ASSERT_EQUAL_INT(0, chdir(testWorkDir));
	TEST_ASSERT_NOT_NULL(getcwd(buf, sizeof(buf)));
	TEST_ASSERT_EQUAL_STRING(testWorkDir, buf);
}


TEST(unistd_fsdir, chdir_rel)
{
	char absPath[PATH_MAX];
	size_t slen;

	strncpy(absPath, testWorkDir, sizeof(absPath));

	slen = strlen(absPath);
	TEST_ASSERT_GREATER_OR_EQUAL(slen + (size_t)sizeof(DIRNAME) + 2, (size_t)sizeof(absPath));

	if (absPath[slen - 1] != '/') {
		absPath[slen++] = '/';
		absPath[slen] = '\0';
	}
	strcpy(absPath + slen, DIRNAME);

	TEST_ASSERT_EQUAL_INT(0, mkdir(DIRNAME, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH));

	/* test chdir to some directory */
	TEST_ASSERT_EQUAL_INT(0, chdir(DIRNAME));
	TEST_ASSERT_NOT_NULL(getcwd(buf, sizeof(buf)));
	TEST_ASSERT_EQUAL_STRING(absPath, buf);

	/* test chdir to cwd */
	TEST_ASSERT_EQUAL_INT(0, chdir("."));
	TEST_ASSERT_NOT_NULL(getcwd(buf, sizeof(buf)));
	TEST_ASSERT_EQUAL_STRING(absPath, buf);

	/* test chdir back to working directory */
	TEST_ASSERT_EQUAL_INT(0, chdir(".."));
	TEST_ASSERT_NOT_NULL(getcwd(buf, sizeof(buf)));
	TEST_ASSERT_EQUAL_STRING(testWorkDir, buf);

	TEST_ASSERT_EQUAL_INT(0, rmdir(DIRNAME));
}


TEST(unistd_fsdir, chdir_toolongpath)
{
	/* test chdir with too long path */
	TEST_ASSERT_EQUAL_INT(-1, chdir(toolongpath));
	TEST_ASSERT_EQUAL_INT(ENAMETOOLONG, errno);
}


TEST(unistd_fsdir, chdir_nonexistent)
{
	/* test chdir to nonexisting directory */
	TEST_ASSERT_EQUAL_INT(-1, chdir("not_existing_directory"));
	TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}


TEST(unistd_fsdir, chdir_emptystring)
{
	/* test chdir to empty string */
	TEST_ASSERT_EQUAL_INT(-1, chdir(""));
	TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}


TEST(unistd_fsdir, chdir_tofile)
{
	/* test chdir to file */
	TEST_ASSERT_EQUAL_INT(-1, chdir(FNAME));
	TEST_ASSERT_EQUAL_INT(ENOTDIR, errno);
}


TEST(unistd_fsdir, rmdir_empty)
{
	/* test removing empty directory */
	TEST_ASSERT_EQUAL_INT(0, mkdir(DIRNAME, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH));
	TEST_ASSERT_EQUAL_INT(0, rmdir(DIRNAME));
}


TEST(unistd_fsdir, rmdir_nonexistent)
{
	/* test rmdir on nonexisting directory*/
	TEST_ASSERT_EQUAL_INT(-1, rmdir("not_existing_directory"));
	TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}


TEST(unistd_fsdir, rmdir_toolongpath)
{
	/* test rmdir with too long path */
	TEST_ASSERT_EQUAL_INT(-1, rmdir(toolongpath));
	TEST_ASSERT_EQUAL_INT(ENAMETOOLONG, errno);
}


TEST(unistd_fsdir, rmdir_emptystring)
{
	/* test rmdir on empty string */
	TEST_ASSERT_EQUAL_INT(-1, rmdir(""));
	TEST_ASSERT_EQUAL_INT(ENOENT, errno);
}


TEST(unistd_fsdir, rmdir_file)
{
	/* test rmdir on file */
	TEST_ASSERT_EQUAL_INT(-1, rmdir(FNAME));
	TEST_ASSERT_EQUAL_INT(ENOTDIR, errno);
}


TEST(unistd_fsdir, rmdir_notempty)
{
	/* prepare not empty directory */
	TEST_ASSERT_EQUAL_INT(0, mkdir(DIRNAME, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH));
	TEST_ASSERT_EQUAL_INT(0, chdir(DIRNAME));
	filep = fopen(FNAME, "w");
	TEST_ASSERT_NOT_NULL(filep);
	TEST_ASSERT_EQUAL_INT(0, fclose(filep));
	TEST_ASSERT_EQUAL_INT(0, chdir(".."));

	/* test removing not empty directory */
	TEST_ASSERT_EQUAL_INT(-1, rmdir(DIRNAME));
	TEST_ASSERT_EQUAL_INT(ENOTEMPTY, errno);

	/* cleanup */
	TEST_ASSERT_EQUAL_INT(0, chdir(DIRNAME));
	TEST_ASSERT_EQUAL_INT(0, remove(FNAME));
	TEST_ASSERT_EQUAL_INT(0, chdir(".."));
	TEST_ASSERT_EQUAL_INT(0, rmdir(DIRNAME));
}


TEST(unistd_fsdir, fchdir)
{
	/* fchdir() backed by the kernel fd->path record (open_file_t.path via sys_fdpath).
	 * Two halves of the contract:
	 *  (a) SUCCESS on a directory fd must actually change the cwd to that directory
	 *      (a false 0 without a real chdir is what broke gnulib's unlinkat emulation
	 *      and `rm -r`/fts -- root-caused via tools/coreutils-maketest);
	 *  (b) a non-directory fd must fail and leave the cwd untouched. */
	int fd, r;
	char before[PATH_MAX], after[PATH_MAX];

	TEST_ASSERT_NOT_NULL(getcwd(before, sizeof(before)));

	/* (a) directory fd -> cwd really moves to "/" */
	fd = open("/", O_RDONLY);
	TEST_ASSERT_TRUE(fd >= 0);
	errno = 0;
	TEST_ASSERT_EQUAL_INT(0, fchdir(fd));
	TEST_ASSERT_NOT_NULL(getcwd(after, sizeof(after)));
	TEST_ASSERT_EQUAL_STRING("/", after);
	close(fd);
	TEST_ASSERT_EQUAL_INT(0, chdir(before)); /* restore cwd */

	/* (b) regular-file fd -> must fail, cwd unchanged */
	fd = open(FNAME, O_RDONLY); /* FNAME is created in TEST_SETUP */
	TEST_ASSERT_TRUE(fd >= 0);

	errno = 0;
	r = fchdir(fd);
	TEST_ASSERT_EQUAL_INT(-1, r);        /* must NOT falsely succeed on a file fd */
	TEST_ASSERT_NOT_EQUAL_INT(0, errno); /* must set errno (ENOTDIR) */

	close(fd);

	TEST_ASSERT_NOT_NULL(getcwd(after, sizeof(after)));
	TEST_ASSERT_EQUAL_STRING(before, after); /* cwd unchanged */
}


IGNORE_TEST(unistd_fsdir, fchown)
{
	/*
		Unimplemented in libphoenix
		https://github.com/phoenix-rtos/phoenix-rtos-project/issues/280
	*/
}


/* The *at() family: dirfd-relative resolution (via the kernel fd->path record)
 * plus the AT_FDCWD and absolute-path fast paths, and the AT_REMOVEDIR/
 * AT_SYMLINK_NOFOLLOW flags. CWD == testWorkDir during the test (TEST_SETUP). */
TEST(unistd_fsdir, at_family)
{
	struct stat st;
	int dfd, ffd, wfd;
	ssize_t n;
	char lbuf[64];

	/* AT_FDCWD behaves exactly like the plain call. */
	ffd = openat(AT_FDCWD, FNAME, O_RDONLY); /* FNAME created in TEST_SETUP */
	TEST_ASSERT_TRUE(ffd >= 0);
	close(ffd);

	/* A real directory fd: names resolve relative to it. */
	dfd = open(".", O_RDONLY);
	TEST_ASSERT_TRUE(dfd >= 0);

	ffd = openat(dfd, FNAME, O_RDONLY);
	TEST_ASSERT_TRUE(ffd >= 0);
	close(ffd);

	/* create + stat a file via the dir fd */
	wfd = openat(dfd, "at_f", O_CREAT | O_WRONLY, 0644);
	TEST_ASSERT_TRUE(wfd >= 0);
	TEST_ASSERT_EQUAL_INT(3, write(wfd, "abc", 3));
	close(wfd);
	TEST_ASSERT_EQUAL_INT(0, fstatat(dfd, "at_f", &st, 0));
	TEST_ASSERT_TRUE(S_ISREG(st.st_mode));
	TEST_ASSERT_EQUAL_INT(3, (int)st.st_size);
	TEST_ASSERT_EQUAL_INT(0, faccessat(dfd, "at_f", F_OK, 0));

	/* mkdirat + fstatat on the new directory */
	TEST_ASSERT_EQUAL_INT(0, mkdirat(dfd, "at_sub", 0755));
	TEST_ASSERT_EQUAL_INT(0, fstatat(dfd, "at_sub", &st, 0));
	TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));

	/* renameat within the dir fd */
	TEST_ASSERT_EQUAL_INT(0, renameat(dfd, "at_f", dfd, "at_f2"));
	TEST_ASSERT_EQUAL_INT(-1, fstatat(dfd, "at_f", &st, 0)); /* old gone */
	TEST_ASSERT_EQUAL_INT(0, fstatat(dfd, "at_f2", &st, 0)); /* new present */

	/* symlinkat + readlinkat + AT_SYMLINK_NOFOLLOW */
	if (symlinkat("at_f2", dfd, "at_ln") == 0) {
		n = readlinkat(dfd, "at_ln", lbuf, sizeof(lbuf) - 1);
		TEST_ASSERT_TRUE(n > 0);
		lbuf[n] = '\0';
		TEST_ASSERT_EQUAL_STRING("at_f2", lbuf);
		TEST_ASSERT_EQUAL_INT(0, fstatat(dfd, "at_ln", &st, AT_SYMLINK_NOFOLLOW));
		TEST_ASSERT_TRUE(S_ISLNK(st.st_mode));
		TEST_ASSERT_EQUAL_INT(0, unlinkat(dfd, "at_ln", 0));
	}

	/* unlinkat routing: plain flag -> unlink() (which rejects a directory, EISDIR),
	 * AT_REMOVEDIR -> rmdir(). */
	TEST_ASSERT_EQUAL_INT(0, unlinkat(dfd, "at_f2", 0));
	TEST_ASSERT_EQUAL_INT(-1, unlinkat(dfd, "at_sub", 0)); /* a directory needs AT_REMOVEDIR */
	TEST_ASSERT_EQUAL_INT(0, unlinkat(dfd, "at_sub", AT_REMOVEDIR));

	/* a bad dir fd must fail, not act on the cwd */
	TEST_ASSERT_EQUAL_INT(-1, openat(-1, "at_nope", O_RDONLY));

	close(dfd);
}


/* POSIX conformance: unlink() must reject a directory (EISDIR); remove() routes a
 * directory to rmdir(); a symlink-to-directory is unlinked as the symlink. */
TEST(unistd_fsdir, unlink_rejects_dir)
{
	struct stat st;
	int fd;

	TEST_ASSERT_EQUAL_INT(0, mkdir("ud_dir", 0755));
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, unlink("ud_dir"));
	TEST_ASSERT_EQUAL_INT(EISDIR, errno);
	TEST_ASSERT_EQUAL_INT(0, stat("ud_dir", &st)); /* not removed */

	/* remove() on a directory routes to rmdir() */
	TEST_ASSERT_EQUAL_INT(0, remove("ud_dir"));
	TEST_ASSERT_EQUAL_INT(-1, stat("ud_dir", &st)); /* gone */

	/* a regular file: unlink() still works */
	fd = open("ud_file", O_CREAT | O_WRONLY, 0644);
	TEST_ASSERT_TRUE(fd >= 0);
	close(fd);
	TEST_ASSERT_EQUAL_INT(0, unlink("ud_file"));

	/* a symlink to a directory: unlink() removes the symlink, not the target */
	TEST_ASSERT_EQUAL_INT(0, mkdir("ud_target", 0755));
	if (symlink("ud_target", "ud_lnk") == 0) {
		TEST_ASSERT_EQUAL_INT(0, unlink("ud_lnk"));        /* symlink removed */
		TEST_ASSERT_EQUAL_INT(0, stat("ud_target", &st));  /* target survives */
	}
	TEST_ASSERT_EQUAL_INT(0, rmdir("ud_target"));
}


TEST_GROUP_RUNNER(unistd_fsdir)
{
	RUN_TEST_CASE(unistd_fsdir, getcwd);

	RUN_TEST_CASE(unistd_fsdir, chdir_absroot)
	RUN_TEST_CASE(unistd_fsdir, chdir_absdev)
	RUN_TEST_CASE(unistd_fsdir, chdir_rel)
	RUN_TEST_CASE(unistd_fsdir, chdir_toolongpath);
	RUN_TEST_CASE(unistd_fsdir, chdir_nonexistent);
	RUN_TEST_CASE(unistd_fsdir, chdir_emptystring);
	RUN_TEST_CASE(unistd_fsdir, chdir_tofile);

	RUN_TEST_CASE(unistd_fsdir, rmdir_empty);
	RUN_TEST_CASE(unistd_fsdir, rmdir_nonexistent);
	RUN_TEST_CASE(unistd_fsdir, rmdir_toolongpath);
	RUN_TEST_CASE(unistd_fsdir, rmdir_emptystring);
	RUN_TEST_CASE(unistd_fsdir, rmdir_file);
	RUN_TEST_CASE(unistd_fsdir, rmdir_notempty);

	RUN_TEST_CASE(unistd_fsdir, fchdir);
	RUN_TEST_CASE(unistd_fsdir, fchown);
	RUN_TEST_CASE(unistd_fsdir, at_family);
	RUN_TEST_CASE(unistd_fsdir, unlink_rejects_dir);
}
