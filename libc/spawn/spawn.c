/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - spawn.h
 *
 * TESTED:
 *    - posix_spawn(), posix_spawnp()
 *    - posix_spawn_file_actions_addopen(), _addclose(), _adddup2()
 *    - posix_spawnattr_setflags() with POSIX_SPAWN_SETSIGMASK,
 *      POSIX_SPAWN_SETSIGDEF, POSIX_SPAWN_SETPGROUP
 *
 * The child is this test binary itself, started as "--child <check> ...": it
 * checks one property of the state it was started in and exits 0 when it
 * holds (spawn_childMain()). GLib takes posix_spawn() when it exists, and
 * WebKit arranges its helper-process launches so that it can.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <unity_fixture.h>


#define SPAWN_MARK      "spawned-ok"
#define SPAWN_FD        9
#define SPAWN_TMPDIR    "/tmp"
#define SPAWN_OUTFILE   SPAWN_TMPDIR "/test-libc-spawn.out"
#define SPAWN_SCRIPT    SPAWN_TMPDIR "/test-libc-spawn.script"
#define SPAWN_SCRIPT_RC 42


extern char **environ;

static char spawn_self[1024];
static char spawn_selfDir[1024];


/* The child side: one check per run, the result in the exit status */
int spawn_childMain(int argc, char *argv[])
{
	const char *check = argv[0];
	struct sigaction sa;
	sigset_t set;
	int arg = (argc > 1) ? atoi(argv[1]) : 0;
	size_t len = strlen(check), slen = strlen(".script");

	if ((len > slen) && (strcmp(check + len - slen, ".script") == 0)) {
		/* Run through "#!<self> --child": argv[0] is the script's path */
		return ((argc == 2) && (strcmp(argv[1], "a1") == 0)) ? SPAWN_SCRIPT_RC : 1;
	}
	if (strcmp(check, "exit") == 0) {
		return arg;
	}
	if (strcmp(check, "args") == 0) {
		return ((argc == 4) && (strcmp(argv[1], "one") == 0) && (strcmp(argv[2], "") == 0) &&
					   (strcmp(argv[3], "three four") == 0)) ?
				0 :
				1;
	}
	if (strcmp(check, "env") == 0) {
		const char *v = getenv("SPAWN_TEST");
		return ((v != NULL) && (strcmp(v, "42") == 0) && (getenv("SPAWN_ABSENT") == NULL)) ? 0 : 1;
	}
	if (strcmp(check, "write") == 0) {
		return (write(arg, SPAWN_MARK, sizeof(SPAWN_MARK) - 1) == (ssize_t)(sizeof(SPAWN_MARK) - 1)) ? 0 : 1;
	}
	if (strcmp(check, "closed") == 0) {
		return ((fcntl(arg, F_GETFD) < 0) && (errno == EBADF)) ? 0 : 1;
	}
	if (strcmp(check, "open") == 0) {
		return (fcntl(arg, F_GETFD) >= 0) ? 0 : 1;
	}
	if ((strcmp(check, "blocked") == 0) || (strcmp(check, "unblocked") == 0)) {
		if (sigprocmask(SIG_BLOCK, NULL, &set) != 0) {
			return 2;
		}
		return (sigismember(&set, arg) == ((check[0] == 'b') ? 1 : 0)) ? 0 : 1;
	}
	if ((strcmp(check, "default") == 0) || (strcmp(check, "ignored") == 0)) {
		if (sigaction(arg, NULL, &sa) != 0) {
			return 2;
		}
		return (sa.sa_handler == ((check[0] == 'd') ? SIG_DFL : SIG_IGN)) ? 0 : 1;
	}
	if (strcmp(check, "pgroup") == 0) {
		return (getpgrp() == getpid()) ? 0 : 1;
	}

	return 3;
}


void spawn_setSelf(const char *argv0)
{
	const char *path, *end;
	struct stat st;
	char *slash;

	/* No /proc/self/exe on Phoenix: argv[0], looked up in PATH if bare */
	spawn_self[0] = '\0';
	if (strchr(argv0, '/') != NULL) {
		(void)snprintf(spawn_self, sizeof(spawn_self), "%s", argv0);
	}
	else {
		path = getenv("PATH");
		while ((path != NULL) && (*path != '\0')) {
			end = strchr(path, ':');
			if (end == NULL) {
				end = path + strlen(path);
			}
			(void)snprintf(spawn_self, sizeof(spawn_self), "%.*s/%s", (int)(end - path), path, argv0);
			if ((stat(spawn_self, &st) == 0) && S_ISREG(st.st_mode)) {
				break;
			}
			spawn_self[0] = '\0';
			path = (*end == ':') ? (end + 1) : end;
		}
	}

	(void)snprintf(spawn_selfDir, sizeof(spawn_selfDir), "%s", spawn_self);
	slash = strrchr(spawn_selfDir, '/');
	if (slash != NULL) {
		*slash = '\0';
	}
}


/* Wait for pid; its exit status, or -1 if it did not exit normally */
static int spawn_wait(pid_t pid)
{
	int status;
	pid_t r;

	do {
		r = waitpid(pid, &status, 0);
	} while ((r < 0) && (errno == EINTR));

	if ((r != pid) || !WIFEXITED(status)) {
		return -1;
	}

	return WEXITSTATUS(status);
}


/* Spawn this binary with "--child check arg" and return its exit status */
static int spawn_check(const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *attr, const char *check, int arg)
{
	char argbuf[16];
	char *argv[] = { spawn_self, "--child", (char *)check, argbuf, NULL };
	pid_t pid = -1;
	int err;

	(void)snprintf(argbuf, sizeof(argbuf), "%d", arg);
	err = posix_spawn(&pid, spawn_self, fa, attr, argv, environ);
	if (err != 0) {
		return -100 - err;
	}

	return spawn_wait(pid);
}


TEST_GROUP(spawn_basic);


TEST_SETUP(spawn_basic)
{
	TEST_ASSERT_NOT_EQUAL_MESSAGE('\0', spawn_self[0], "cannot locate the test binary");
}


TEST_TEAR_DOWN(spawn_basic)
{
}


TEST(spawn_basic, exit_status)
{
	char *argv[] = { spawn_self, "--child", "exit", "7", NULL };
	pid_t pid = -1;

	TEST_ASSERT_EQUAL_INT(0, posix_spawn(&pid, spawn_self, NULL, NULL, argv, environ));
	TEST_ASSERT_GREATER_THAN_INT(0, pid);
	TEST_ASSERT_EQUAL_INT(7, spawn_wait(pid));
}


TEST(spawn_basic, arguments_and_environment)
{
	char *argv[] = { spawn_self, "--child", "args", "one", "", "three four", NULL };
	char *argvEnv[] = { spawn_self, "--child", "env", NULL };
	char *envp[] = { "SPAWN_TEST=42", "PATH=/bin:/usr/bin", NULL };
	pid_t pid = -1;

	TEST_ASSERT_EQUAL_INT(0, posix_spawn(&pid, spawn_self, NULL, NULL, argv, environ));
	TEST_ASSERT_EQUAL_INT(0, spawn_wait(pid));

	/* envp replaces the environment */
	TEST_ASSERT_EQUAL_INT(0, setenv("SPAWN_ABSENT", "1", 1));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn(&pid, spawn_self, NULL, NULL, argvEnv, envp));
	TEST_ASSERT_EQUAL_INT(0, unsetenv("SPAWN_ABSENT"));
	TEST_ASSERT_EQUAL_INT(0, spawn_wait(pid));
}


TEST(spawn_basic, spawnp_searches_path)
{
	const char *name = strrchr(spawn_self, '/') + 1;
	char *argv[] = { (char *)name, "--child", "exit", "5", NULL };
	char *oldPath = getenv("PATH"), *saved = NULL;
	pid_t pid = -1;
	int err;

	if (oldPath != NULL) {
		saved = strdup(oldPath);
		TEST_ASSERT_NOT_NULL(saved);
	}

	/* A missing directory and an empty element before the right one */
	{
		char path[1100];
		(void)snprintf(path, sizeof(path), "/nonexistent-dir::%s", spawn_selfDir);
		TEST_ASSERT_EQUAL_INT(0, setenv("PATH", path, 1));
	}
	err = posix_spawnp(&pid, name, NULL, NULL, argv, environ);

	if (saved != NULL) {
		(void)setenv("PATH", saved, 1);
		free(saved);
	}
	else {
		(void)unsetenv("PATH");
	}

	TEST_ASSERT_EQUAL_INT(0, err);
	TEST_ASSERT_EQUAL_INT(5, spawn_wait(pid));
}


/* A failure to exec is the return value of posix_spawn(), not a child */
TEST(spawn_basic, missing_file_is_reported)
{
	char *argv[] = { "nothing", NULL };
	pid_t pid = 12345;
	int status;

	TEST_ASSERT_EQUAL_INT(ENOENT, posix_spawn(&pid, "/nonexistent-dir/nothing", NULL, NULL, argv, environ));
	TEST_ASSERT_EQUAL_INT(ENOENT, posix_spawnp(&pid, "nonexistent-spawn-test-binary", NULL, NULL, argv, environ));
	/* No child is left behind */
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, waitpid(-1, &status, WNOHANG));
	TEST_ASSERT_EQUAL_INT(ECHILD, errno);
}


TEST(spawn_basic, interpreter_script)
{
	char *argv[] = { "script", "a1", NULL };
	pid_t pid = -1;
	FILE *f;
	int err;

	TEST_ASSERT_TRUE((mkdir(SPAWN_TMPDIR, 0777) == 0) || (errno == EEXIST));
	f = fopen(SPAWN_SCRIPT, "w");
	TEST_ASSERT_NOT_NULL(f);
	TEST_ASSERT_GREATER_THAN_INT(0, fprintf(f, "#!%s --child\nthis line is not read\n", spawn_self));
	TEST_ASSERT_EQUAL_INT(0, fclose(f));
	TEST_ASSERT_EQUAL_INT(0, chmod(SPAWN_SCRIPT, 0755));

	err = posix_spawn(&pid, SPAWN_SCRIPT, NULL, NULL, argv, environ);
	(void)unlink(SPAWN_SCRIPT);

	TEST_ASSERT_EQUAL_INT(0, err);
	TEST_ASSERT_EQUAL_INT(SPAWN_SCRIPT_RC, spawn_wait(pid));
}


static volatile int spawn_churnStop;


static void *spawn_churn(void *arg)
{
	(void)arg;

	while (spawn_churnStop == 0) {
		void *p = malloc(64 + (rand() % 4096));
		free(p);
	}

	return NULL;
}


/* The child must not use the caller's heap: another thread is using it */
TEST(spawn_basic, while_another_thread_allocates)
{
	pthread_t thread;
	int i, rc[20];

	spawn_churnStop = 0;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&thread, NULL, spawn_churn, NULL));
	for (i = 0; i < 20; i++) {
		rc[i] = spawn_check(NULL, NULL, "exit", i);
	}
	spawn_churnStop = 1;
	TEST_ASSERT_EQUAL_INT(0, pthread_join(thread, NULL));

	for (i = 0; i < 20; i++) {
		TEST_ASSERT_EQUAL_INT(i, rc[i]);
	}
	free(malloc(1)); /* the heap is still sound */
}


TEST_GROUP_RUNNER(spawn_basic)
{
	RUN_TEST_CASE(spawn_basic, exit_status);
	RUN_TEST_CASE(spawn_basic, arguments_and_environment);
	RUN_TEST_CASE(spawn_basic, spawnp_searches_path);
	RUN_TEST_CASE(spawn_basic, missing_file_is_reported);
	RUN_TEST_CASE(spawn_basic, interpreter_script);
	RUN_TEST_CASE(spawn_basic, while_another_thread_allocates);
}


TEST_GROUP(spawn_file_actions);


TEST_SETUP(spawn_file_actions)
{
	TEST_ASSERT_NOT_EQUAL_MESSAGE('\0', spawn_self[0], "cannot locate the test binary");
}


TEST_TEAR_DOWN(spawn_file_actions)
{
}


/* dup2 a pipe end onto SPAWN_FD, close the read end, write through it */
TEST(spawn_file_actions, dup2_and_close)
{
	posix_spawn_file_actions_t fa;
	char buf[32] = { 0 };
	int p[2];
	ssize_t n;

	TEST_ASSERT_EQUAL_INT(0, pipe(p));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_init(&fa));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_adddup2(&fa, p[1], SPAWN_FD));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_addclose(&fa, p[0]));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_addclose(&fa, p[1]));

	TEST_ASSERT_EQUAL_INT(0, spawn_check(&fa, NULL, "write", SPAWN_FD));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_destroy(&fa));
	TEST_ASSERT_EQUAL_INT(0, close(p[1]));

	n = read(p[0], buf, sizeof(buf) - 1);
	TEST_ASSERT_EQUAL_INT(0, close(p[0]));
	TEST_ASSERT_EQUAL_INT((int)(sizeof(SPAWN_MARK) - 1), (int)n);
	TEST_ASSERT_EQUAL_STRING(SPAWN_MARK, buf);
}


TEST(spawn_file_actions, close_is_applied_in_child_only)
{
	posix_spawn_file_actions_t fa;
	int p[2];

	TEST_ASSERT_EQUAL_INT(0, pipe(p));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_init(&fa));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_addclose(&fa, p[0]));
	/* Closing what is not open does not fail the spawn */
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_addclose(&fa, 1000));

	TEST_ASSERT_EQUAL_INT(0, spawn_check(&fa, NULL, "closed", p[0]));
	TEST_ASSERT_EQUAL_INT(0, spawn_check(NULL, NULL, "open", p[0]));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_destroy(&fa));

	/* Still open here */
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fcntl(p[0], F_GETFD));
	TEST_ASSERT_EQUAL_INT(0, close(p[0]));
	TEST_ASSERT_EQUAL_INT(0, close(p[1]));
}


/* adddup2(fd, fd): the descriptor stays, inherited despite FD_CLOEXEC */
TEST(spawn_file_actions, dup2_onto_itself_clears_cloexec)
{
	posix_spawn_file_actions_t fa;
	int p[2];

	TEST_ASSERT_EQUAL_INT(0, pipe(p));
	TEST_ASSERT_EQUAL_INT(0, fcntl(p[1], F_SETFD, FD_CLOEXEC));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_init(&fa));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_adddup2(&fa, p[1], p[1]));

	TEST_ASSERT_EQUAL_INT(0, spawn_check(&fa, NULL, "open", p[1]));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_destroy(&fa));

	/* Not in the caller */
	TEST_ASSERT_EQUAL_INT(FD_CLOEXEC, fcntl(p[1], F_GETFD) & FD_CLOEXEC);
	TEST_ASSERT_EQUAL_INT(0, close(p[0]));
	TEST_ASSERT_EQUAL_INT(0, close(p[1]));
}


TEST(spawn_file_actions, open_creates_file)
{
	posix_spawn_file_actions_t fa;
	char buf[32] = { 0 };
	ssize_t n;
	int fd;

	TEST_ASSERT_TRUE((mkdir(SPAWN_TMPDIR, 0777) == 0) || (errno == EEXIST));
	(void)unlink(SPAWN_OUTFILE);

	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_init(&fa));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_addopen(&fa, SPAWN_FD, SPAWN_OUTFILE, O_WRONLY | O_CREAT | O_TRUNC, 0644));
	TEST_ASSERT_EQUAL_INT(0, spawn_check(&fa, NULL, "write", SPAWN_FD));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_destroy(&fa));

	fd = open(SPAWN_OUTFILE, O_RDONLY);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
	n = read(fd, buf, sizeof(buf) - 1);
	TEST_ASSERT_EQUAL_INT(0, close(fd));
	(void)unlink(SPAWN_OUTFILE);

	TEST_ASSERT_EQUAL_INT((int)(sizeof(SPAWN_MARK) - 1), (int)n);
	TEST_ASSERT_EQUAL_STRING(SPAWN_MARK, buf);
}


/* An open action that fails fails the spawn */
TEST(spawn_file_actions, open_failure_is_reported)
{
	posix_spawn_file_actions_t fa;
	char *argv[] = { spawn_self, "--child", "exit", "0", NULL };
	pid_t pid;

	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_init(&fa));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_addopen(&fa, SPAWN_FD, "/nonexistent-dir/file", O_RDONLY, 0));
	TEST_ASSERT_EQUAL_INT(ENOENT, posix_spawn(&pid, spawn_self, &fa, NULL, argv, environ));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_destroy(&fa));
}


TEST(spawn_file_actions, bad_descriptors)
{
	posix_spawn_file_actions_t fa;

	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_init(&fa));
	TEST_ASSERT_EQUAL_INT(EBADF, posix_spawn_file_actions_addclose(&fa, -1));
	TEST_ASSERT_EQUAL_INT(EBADF, posix_spawn_file_actions_adddup2(&fa, -1, 3));
	TEST_ASSERT_EQUAL_INT(EBADF, posix_spawn_file_actions_adddup2(&fa, 3, -1));
	TEST_ASSERT_EQUAL_INT(EBADF, posix_spawn_file_actions_addopen(&fa, -1, "/dev/null", O_RDONLY, 0));
	TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_destroy(&fa));
}


TEST_GROUP_RUNNER(spawn_file_actions)
{
	RUN_TEST_CASE(spawn_file_actions, dup2_and_close);
	RUN_TEST_CASE(spawn_file_actions, close_is_applied_in_child_only);
	RUN_TEST_CASE(spawn_file_actions, dup2_onto_itself_clears_cloexec);
	RUN_TEST_CASE(spawn_file_actions, open_creates_file);
	RUN_TEST_CASE(spawn_file_actions, open_failure_is_reported);
	RUN_TEST_CASE(spawn_file_actions, bad_descriptors);
}


TEST_GROUP(spawn_attr);


TEST_SETUP(spawn_attr)
{
	TEST_ASSERT_NOT_EQUAL_MESSAGE('\0', spawn_self[0], "cannot locate the test binary");
}


TEST_TEAR_DOWN(spawn_attr)
{
	sigset_t set;

	(void)sigemptyset(&set);
	(void)sigprocmask(SIG_SETMASK, &set, NULL);
	(void)signal(SIGUSR1, SIG_DFL);
	(void)signal(SIGUSR2, SIG_DFL);
}


TEST(spawn_attr, flags_round_trip)
{
	posix_spawnattr_t attr;
	short flags = -1;

	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_getflags(&attr, &flags));
	TEST_ASSERT_EQUAL_INT(0, flags);
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP));
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_getflags(&attr, &flags));
	TEST_ASSERT_EQUAL_INT(POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP, flags);
	TEST_ASSERT_EQUAL_INT(EINVAL, posix_spawnattr_setflags(&attr, 0x4000));
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_destroy(&attr));
}


/* Without SETSIGMASK the child has the caller's mask, not the one used while
 * spawning, and the caller gets its own back */
TEST(spawn_attr, caller_mask_is_inherited)
{
	sigset_t set, now;

	(void)sigemptyset(&set);
	(void)sigaddset(&set, SIGUSR2);
	TEST_ASSERT_EQUAL_INT(0, sigprocmask(SIG_SETMASK, &set, NULL));

	TEST_ASSERT_EQUAL_INT(0, spawn_check(NULL, NULL, "blocked", SIGUSR2));
	TEST_ASSERT_EQUAL_INT(0, spawn_check(NULL, NULL, "unblocked", SIGUSR1));

	TEST_ASSERT_EQUAL_INT(0, sigprocmask(SIG_BLOCK, NULL, &now));
	TEST_ASSERT_EQUAL_INT(1, sigismember(&now, SIGUSR2));
	TEST_ASSERT_EQUAL_INT(0, sigismember(&now, SIGUSR1));
}


TEST(spawn_attr, setsigmask)
{
	posix_spawnattr_t attr;
	sigset_t set;

	(void)sigemptyset(&set);
	(void)sigaddset(&set, SIGUSR1);
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_setsigmask(&attr, &set));
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK));

	TEST_ASSERT_EQUAL_INT(0, spawn_check(NULL, &attr, "blocked", SIGUSR1));
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_destroy(&attr));

	/* Only the child's */
	TEST_ASSERT_EQUAL_INT(0, sigprocmask(SIG_BLOCK, NULL, &set));
	TEST_ASSERT_EQUAL_INT(0, sigismember(&set, SIGUSR1));
}


static void spawn_handler(int sig)
{
	(void)sig;
}


/* Ignored stays ignored unless SETSIGDEF names it; caught becomes default */
TEST(spawn_attr, setsigdef)
{
	posix_spawnattr_t attr;
	sigset_t set;

	TEST_ASSERT_TRUE(signal(SIGUSR1, SIG_IGN) != SIG_ERR);
	TEST_ASSERT_TRUE(signal(SIGUSR2, spawn_handler) != SIG_ERR);

	TEST_ASSERT_EQUAL_INT(0, spawn_check(NULL, NULL, "ignored", SIGUSR1));
	TEST_ASSERT_EQUAL_INT(0, spawn_check(NULL, NULL, "default", SIGUSR2));

	(void)sigemptyset(&set);
	(void)sigaddset(&set, SIGUSR1);
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_setsigdefault(&attr, &set));
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF));
	TEST_ASSERT_EQUAL_INT(0, spawn_check(NULL, &attr, "default", SIGUSR1));
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_destroy(&attr));

	/* The caller's dispositions are untouched */
	TEST_ASSERT_TRUE(signal(SIGUSR1, SIG_DFL) == SIG_IGN);
	TEST_ASSERT_TRUE(signal(SIGUSR2, SIG_DFL) == spawn_handler);
}


TEST(spawn_attr, setpgroup)
{
	posix_spawnattr_t attr;

	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_init(&attr));
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_setpgroup(&attr, 0));
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP));
	TEST_ASSERT_EQUAL_INT(0, spawn_check(NULL, &attr, "pgroup", 0));
	TEST_ASSERT_EQUAL_INT(0, posix_spawnattr_destroy(&attr));
}


TEST_GROUP_RUNNER(spawn_attr)
{
	RUN_TEST_CASE(spawn_attr, flags_round_trip);
	RUN_TEST_CASE(spawn_attr, caller_mask_is_inherited);
	RUN_TEST_CASE(spawn_attr, setsigmask);
	RUN_TEST_CASE(spawn_attr, setsigdef);
	RUN_TEST_CASE(spawn_attr, setpgroup);
}
