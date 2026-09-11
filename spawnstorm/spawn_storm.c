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
 * 3063 SEQUENTIAL launches produced zero failures, which retired the idea that
 * the fault is a per-launch lottery -- so -p runs up to N children at once.
 * Concurrency is the variable those runs lacked: every one of them started a
 * process on an otherwise quiet system, whereas the original failure happened
 * while a desktop and a GPU app were live. Startup takes a blocking ioctl to
 * the tty through a port, and this target has already had one multi-waiter
 * wakeup bug, so contention on that path is worth a direct test.
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


#define SPAWN_MAX_PARALLEL 32u


static unsigned long spawn_elapsedMs(const struct timeval *start, const struct timeval *end)
{
	return (unsigned long)(end->tv_sec - start->tv_sec) * 1000uL
			+ (unsigned long)((end->tv_usec - start->tv_usec) / 1000);
}


int main(int argc, char *argv[])
{
	char *const *childArgv;
	char *childPath;
	struct timeval before[SPAWN_MAX_PARALLEL], after;
	unsigned long iterations, parallel = 1, i, launched = 0, reaped = 0;
	unsigned long slowest = 0, ok = 0, failed = 0;
	pid_t inflight[SPAWN_MAX_PARALLEL];
	unsigned long slot;
	pid_t pid;
	int status, res, argi = 1;

	/* -p is optional so the sequential invocations already on record still work. */
	if ((argc > 2) && (strcmp(argv[1], "-p") == 0)) {
		parallel = strtoul(argv[2], NULL, 10);
		if ((parallel == 0) || (parallel > SPAWN_MAX_PARALLEL)) {
			fprintf(stderr, "spawn-storm: -p must be 1..%u\n", (unsigned int)SPAWN_MAX_PARALLEL);
			return EXIT_FAILURE;
		}
		argi = 3;
	}

	if (argc < (argi + 2)) {
		fprintf(stderr, "usage: %s [-p <parallel>] <iterations> <path> [args...]\n", argv[0]);
		return EXIT_FAILURE;
	}

	iterations = strtoul(argv[argi], NULL, 10);
	if (iterations == 0) {
		fprintf(stderr, "spawn-storm: iterations must be non-zero\n");
		return EXIT_FAILURE;
	}

	childPath = argv[argi + 1];
	childArgv = &argv[argi + 1];

	/* The child inherits these, so the index survives a child that wedges. */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	for (slot = 0; slot < SPAWN_MAX_PARALLEL; slot++) {
		inflight[slot] = -1;
	}

	printf("spawn-storm: %lu launches of %s, %lu at a time\n", iterations, childPath, parallel);

	while (reaped < iterations) {
		/* Fill the window before reaping, so `parallel` children really do overlap. */
		while ((launched < iterations) && ((launched - reaped) < parallel)) {
			/* The window invariant leaves a slot free, but do not bet a live pid on it. */
			for (slot = 0; slot < parallel; slot++) {
				if (inflight[slot] == -1) {
					break;
				}
			}
			if (slot == parallel) {
				printf("spawn-storm: BUG no free slot with %lu in flight\n", launched - reaped);
				break;
			}

			printf("spawn-storm: launch %lu/%lu\n", launched + 1, iterations);

			gettimeofday(&before[slot], NULL);

			pid = vfork();
			if (pid < 0) {
				printf("spawn-storm: launch %lu vfork failed (%s)\n", launched + 1, strerror(errno));
				failed++;
				launched++;
				reaped++;
				continue;
			}

			if (pid == 0) {
				execv(childPath, childArgv);
				_exit(EXIT_FAILURE);
			}

			inflight[slot] = pid;
			launched++;
		}

		if (launched == reaped) {
			continue;
		}

		do {
			res = waitpid(-1, &status, 0);
		} while ((res < 0) && (errno == EINTR));

		gettimeofday(&after, NULL);

		if (res < 0) {
			printf("spawn-storm: waitpid failed with %lu in flight (%s)\n",
					launched - reaped, strerror(errno));
			/* Nothing left to reap -- the remaining children are unaccounted for. */
			failed += launched - reaped;
			reaped = launched;
			continue;
		}

		slot = SPAWN_MAX_PARALLEL;
		for (i = 0; i < parallel; i++) {
			if (inflight[i] == res) {
				slot = i;
				inflight[i] = -1;
				break;
			}
		}

		reaped++;

		if (WIFEXITED(status) && (WEXITSTATUS(status) == 0)) {
			ok++;
		}
		else {
			failed++;
			printf("spawn-storm: pid %d BAD status 0x%x (exited=%d code=%d signalled=%d sig=%d)\n",
					(int)res, (unsigned int)status, WIFEXITED(status),
					WIFEXITED(status) ? WEXITSTATUS(status) : -1,
					WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : -1);
		}

		/* A pre-main stall that eventually recovers shows up here, not in the counts. */
		if (slot < SPAWN_MAX_PARALLEL) {
			unsigned long ms = spawn_elapsedMs(&before[slot], &after);

			if (ms > slowest) {
				slowest = ms;
				printf("spawn-storm: pid %d is the new slowest at %lu ms\n", (int)res, slowest);
			}
		}
	}

	printf("spawn-storm: DONE %lu ok, %lu failed, slowest %lu ms\n", ok, failed, slowest);

	return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
