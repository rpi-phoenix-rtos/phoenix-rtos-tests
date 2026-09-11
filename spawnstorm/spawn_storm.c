/*
 * Phoenix-RTOS
 *
 * phoenix-rtos-tests
 *
 * spawn-storm: launch one program repeatedly to expose process-startup faults
 *
 * Two pre-main failures have been seen on this target -- an app that hung
 * before main() and a libc init that read a statically initialised pointer as
 * NULL -- both in large binaries demand-paged from the NFS root, at roughly
 * one occurrence in 30 to 60 launches. A boot-per-launch bench needs hours to
 * collect one event, so this walks the same path many times inside a single
 * boot.
 *
 * The launch index is printed BEFORE each spawn and the stream is unbuffered,
 * so a child that never returns leaves its index as the last line in the UART
 * log; no watchdog is needed to localise the hang.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/time.h>


static unsigned long spawn_elapsedMs(const struct timeval *start, const struct timeval *end)
{
	return (unsigned long)(end->tv_sec - start->tv_sec) * 1000uL
			+ (unsigned long)((end->tv_usec - start->tv_usec) / 1000);
}


int main(int argc, char *argv[])
{
	char *const *childArgv;
	struct timeval before, after;
	unsigned long iterations, i, slowest = 0, ok = 0, failed = 0;
	pid_t pid;
	int status, res;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <iterations> <path> [args...]\n", argv[0]);
		return EXIT_FAILURE;
	}

	iterations = strtoul(argv[1], NULL, 10);
	if (iterations == 0) {
		fprintf(stderr, "spawn-storm: iterations must be non-zero\n");
		return EXIT_FAILURE;
	}

	/* The child inherits these, so the index survives a child that wedges. */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	childArgv = &argv[2];

	printf("spawn-storm: %lu launches of %s\n", iterations, argv[2]);

	for (i = 0; i < iterations; i++) {
		printf("spawn-storm: launch %lu/%lu\n", i + 1, iterations);

		gettimeofday(&before, NULL);

		pid = vfork();
		if (pid < 0) {
			printf("spawn-storm: launch %lu vfork failed (%s)\n", i + 1, strerror(errno));
			failed++;
			continue;
		}

		if (pid == 0) {
			execv(argv[2], childArgv);
			_exit(EXIT_FAILURE);
		}

		do {
			res = waitpid(pid, &status, 0);
		} while ((res < 0) && (errno == EINTR));

		gettimeofday(&after, NULL);

		if (res < 0) {
			printf("spawn-storm: launch %lu waitpid failed (%s)\n", i + 1, strerror(errno));
			failed++;
			continue;
		}

		if (WIFEXITED(status) && (WEXITSTATUS(status) == 0)) {
			ok++;
		}
		else {
			failed++;
			printf("spawn-storm: launch %lu BAD status 0x%x (exited=%d code=%d signalled=%d sig=%d)\n",
					i + 1, (unsigned int)status, WIFEXITED(status),
					WIFEXITED(status) ? WEXITSTATUS(status) : -1,
					WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : -1);
		}

		/* A pre-main stall that eventually recovers shows up here, not in the counts. */
		if (spawn_elapsedMs(&before, &after) > slowest) {
			slowest = spawn_elapsedMs(&before, &after);
			printf("spawn-storm: launch %lu is the new slowest at %lu ms\n", i + 1, slowest);
		}
	}

	printf("spawn-storm: DONE %lu ok, %lu failed, slowest %lu ms\n", ok, failed, slowest);

	return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
