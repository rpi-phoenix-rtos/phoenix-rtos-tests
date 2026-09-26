/*
 * Phoenix-RTOS
 *
 * BSD/POSIX-extension library tests
 *
 * HEADER:
 *    - sys/file.h
 *
 * TESTED:
 *    - flock(), LOCK_SH / LOCK_EX / LOCK_NB / LOCK_UN
 *
 * libphoenix declared flock() in <sys/file.h> but kept the LOCK_* constants in
 * <fcntl.h> only (Mesa's fossilize_db.c: "LOCK_UN undeclared"), and flock()
 * itself was a stub returning 0 -- two processes could both "hold" the same
 * exclusive lock. It is now emulated with whole-file fcntl() record locks,
 * which the kernel implements. The cross-process cases below are the ones a
 * lock file depends on; the stub fails every conflict check.
 *
 * This file includes <sys/file.h> and NOT <fcntl.h> on purpose: the constants
 * must come from the header flock() is declared in.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <unity_fixture.h>


#define FLOCK_PATH "/tmp/test_flock"

/* Child exit codes; anything else is a harness failure. */
#define CHILD_OK       0
#define CHILD_GOT_LOCK 10
#define CHILD_BLOCKED  11
#define CHILD_ERROR    12


static int flock_fd = -1;


static int flock_open(void)
{
	return open(FLOCK_PATH, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
}


/* Fork a child that opens the file itself and attempts `op`. The child exits
 * CHILD_GOT_LOCK if it got the lock, CHILD_BLOCKED if it failed with
 * EWOULDBLOCK, CHILD_ERROR on anything else. Returns the exit code, or -1 if
 * fork is unavailable (NOMMU targets). */
static int flock_tryInChild(int op)
{
	pid_t pid;
	int status = 0;

	pid = fork();
	if (pid < 0) {
		return -1;
	}
	if (pid == 0) {
		int fd = flock_open();
		int rc;

		if (fd < 0) {
			_exit(CHILD_ERROR);
		}
		rc = flock(fd, op);
		if (rc == 0) {
			_exit(CHILD_GOT_LOCK);
		}
		_exit((errno == EWOULDBLOCK) ? CHILD_BLOCKED : CHILD_ERROR);
	}

	if ((waitpid(pid, &status, 0) != pid) || !WIFEXITED(status)) {
		return CHILD_ERROR;
	}
	return WEXITSTATUS(status);
}


#define ASSERT_CHILD(expected, op) \
	do { \
		int _r = flock_tryInChild(op); \
		if (_r < 0) { \
			TEST_IGNORE_MESSAGE("fork() unavailable"); \
		} \
		TEST_ASSERT_EQUAL_INT((expected), _r); \
	} while (0)


TEST_GROUP(sys_file_flock);


TEST_SETUP(sys_file_flock)
{
	flock_fd = flock_open();
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, flock_fd);
}


TEST_TEAR_DOWN(sys_file_flock)
{
	if (flock_fd >= 0) {
		close(flock_fd);
		flock_fd = -1;
	}
	unlink(FLOCK_PATH);
}


TEST(sys_file_flock, constants_from_sys_file_h)
{
	/* Distinct bits, so LOCK_NB can be or-ed into either lock type. */
	TEST_ASSERT_NOT_EQUAL_INT(0, LOCK_SH);
	TEST_ASSERT_NOT_EQUAL_INT(0, LOCK_EX);
	TEST_ASSERT_NOT_EQUAL_INT(0, LOCK_NB);
	TEST_ASSERT_NOT_EQUAL_INT(0, LOCK_UN);
	TEST_ASSERT_EQUAL_INT(0, LOCK_SH & LOCK_EX);
	TEST_ASSERT_EQUAL_INT(0, (LOCK_SH | LOCK_EX | LOCK_UN) & LOCK_NB);
}


TEST(sys_file_flock, invalid_operation)
{
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, flock(flock_fd, 0));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);

	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, flock(flock_fd, LOCK_NB));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);

	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, flock(flock_fd, LOCK_SH | LOCK_EX));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}


TEST(sys_file_flock, bad_fd)
{
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, flock(-1, LOCK_EX));
	TEST_ASSERT_EQUAL_INT(EBADF, errno);
}


TEST(sys_file_flock, lock_unlock)
{
	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_EX));
	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_UN));
	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_SH | LOCK_NB));
	/* converting a held lock is allowed */
	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_EX | LOCK_NB));
	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_UN));
	/* unlocking an unlocked file is not an error */
	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_UN));
}


TEST(sys_file_flock, exclusive_excludes_other_process)
{
	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_EX));

	ASSERT_CHILD(CHILD_BLOCKED, LOCK_EX | LOCK_NB);
	ASSERT_CHILD(CHILD_BLOCKED, LOCK_SH | LOCK_NB);

	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_UN));

	ASSERT_CHILD(CHILD_GOT_LOCK, LOCK_EX | LOCK_NB);
}


TEST(sys_file_flock, shared_admits_shared_only)
{
	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_SH));

	ASSERT_CHILD(CHILD_GOT_LOCK, LOCK_SH | LOCK_NB);
	ASSERT_CHILD(CHILD_BLOCKED, LOCK_EX | LOCK_NB);

	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_UN));
}


/* Without LOCK_NB the child must WAIT for the holder, then get the lock. */
TEST(sys_file_flock, blocking_waits_for_release)
{
	int ready[2], status = 0;
	pid_t pid;
	char c;

	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_EX));
	TEST_ASSERT_EQUAL_INT(0, pipe(ready));

	pid = fork();
	if (pid < 0) {
		close(ready[0]);
		close(ready[1]);
		TEST_IGNORE_MESSAGE("fork() unavailable");
	}
	if (pid == 0) {
		int fd = flock_open();

		close(ready[0]);
		if (fd < 0) {
			_exit(CHILD_ERROR);
		}
		/* still held by the parent: a non-blocking try must fail... */
		if ((flock(fd, LOCK_EX | LOCK_NB) == 0) || (errno != EWOULDBLOCK)) {
			_exit(CHILD_ERROR);
		}
		(void)write(ready[1], "r", 1);
		/* ...and a blocking one must succeed once the parent lets go */
		_exit((flock(fd, LOCK_EX) == 0) ? CHILD_OK : CHILD_ERROR);
	}

	close(ready[1]);
	TEST_ASSERT_EQUAL_INT(1, read(ready[0], &c, 1));
	close(ready[0]);

	usleep(100 * 1000);
	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_UN));

	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	TEST_ASSERT_TRUE(WIFEXITED(status));
	TEST_ASSERT_EQUAL_INT(CHILD_OK, WEXITSTATUS(status));
}


/* A lock dies with its holder, or a crashed program would wedge every later
 * user of the lock file. */
TEST(sys_file_flock, released_when_holder_exits)
{
	pid_t pid;
	int status = 0;

	pid = fork();
	if (pid < 0) {
		TEST_IGNORE_MESSAGE("fork() unavailable");
	}
	if (pid == 0) {
		int fd = flock_open();

		_exit(((fd >= 0) && (flock(fd, LOCK_EX) == 0)) ? CHILD_OK : CHILD_ERROR);
	}
	TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
	TEST_ASSERT_EQUAL_INT(CHILD_OK, WEXITSTATUS(status));

	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_EX | LOCK_NB));
	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_UN));
}


/* Documented deviation of the emulation: record locks belong to the process,
 * BSD flock() locks to the open file description. So on Phoenix a second
 * descriptor in the SAME process does not conflict, while on a native flock()
 * (Linux, BSD) it does. Pinned so the difference cannot change silently. */
TEST(sys_file_flock, same_process_second_descriptor)
{
	int fd2 = flock_open();
	int rc;

	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd2);
	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_EX));

	errno = 0;
	rc = flock(fd2, LOCK_EX | LOCK_NB);
#ifdef __phoenix__
	TEST_ASSERT_EQUAL_INT(0, rc);
#else
	TEST_ASSERT_EQUAL_INT(-1, rc);
	TEST_ASSERT_EQUAL_INT(EWOULDBLOCK, errno);
#endif

	close(fd2);
	TEST_ASSERT_EQUAL_INT(0, flock(flock_fd, LOCK_UN));
}


TEST_GROUP_RUNNER(sys_file_flock)
{
	RUN_TEST_CASE(sys_file_flock, constants_from_sys_file_h);
	RUN_TEST_CASE(sys_file_flock, invalid_operation);
	RUN_TEST_CASE(sys_file_flock, bad_fd);
	RUN_TEST_CASE(sys_file_flock, lock_unlock);
	RUN_TEST_CASE(sys_file_flock, exclusive_excludes_other_process);
	RUN_TEST_CASE(sys_file_flock, shared_admits_shared_only);
	RUN_TEST_CASE(sys_file_flock, blocking_waits_for_release);
	RUN_TEST_CASE(sys_file_flock, released_when_holder_exits);
	RUN_TEST_CASE(sys_file_flock, same_process_second_descriptor);
}
