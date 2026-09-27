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
 * -f launches with fork() instead of vfork(). Every launch above used vfork,
 * and the kernel's exec takes a different path for a fork()ed child: that
 * child owns a private copy of the parent's map, which exec destroys and then
 * re-creates in place, whereas a vfork child borrows the parent's map and exec
 * simply gives it a fresh one. psh starts ntpclient with fork()+exec, and
 * ntpclient is the one process whose data pages have been seen not to hold
 * what it wrote (docs/misc/2026-09-27-c9-ntpclient-faults.md in the
 * coordination repository). Like psh, the forked child writes to its copy of
 * the address space before it execs, so exec tears down COW-split pages.
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
#include <fcntl.h>


#define SPAWN_MAX_PARALLEL 32u


/* Written by a -f child before exec, so a .bss page is COW-split. */
static volatile unsigned long spawn_childTouch;


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
	int status, res, devNull, argi = 1, useFork = 0;

	/* Both options are optional so the invocations already on record still work. */
	for (;;) {
		if ((argc > (argi + 1)) && (strcmp(argv[argi], "-p") == 0)) {
			parallel = strtoul(argv[argi + 1], NULL, 10);
			if ((parallel == 0) || (parallel > SPAWN_MAX_PARALLEL)) {
				fprintf(stderr, "spawn-storm: -p must be 1..%u\n", (unsigned int)SPAWN_MAX_PARALLEL);
				return EXIT_FAILURE;
			}
			argi += 2;
		}
		else if ((argc > argi) && (strcmp(argv[argi], "-f") == 0)) {
			useFork = 1;
			argi += 1;
		}
		else {
			break;
		}
	}

	if (argc < (argi + 2)) {
		fprintf(stderr, "usage: %s [-f] [-p <parallel>] <iterations> <path> [args...]\n", argv[0]);
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

	devNull = (parallel > 1) ? open("/dev/null", O_WRONLY) : -1;

	printf("spawn-storm: %lu launches of %s via %s, %lu at a time%s\n", iterations, childPath,
			(useFork != 0) ? "fork" : "vfork", parallel,
			(parallel == 1) ? " (child output kept)"
					: ((devNull >= 0) ? " (child output muted)"
							: " (no /dev/null: child output will interleave)"));

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

			pid = (useFork != 0) ? fork() : vfork();
			if (pid < 0) {
				printf("spawn-storm: launch %lu %s failed (%s)\n", launched + 1,
						(useFork != 0) ? "fork" : "vfork", strerror(errno));
				failed++;
				launched++;
				reaped++;
				continue;
			}

			if (pid == 0) {
				/* Under -p, children share the tty and their writes shredded the
				 * parent's lines -- a mangled report is worse than none, since a
				 * failure line can be lost. Sequentially there is no such race, and
				 * the child's own startup prints are the evidence for WHERE a stalled
				 * launch stopped, so keep them. dup2 is a bare syscall, safe between
				 * vfork and exec where stdio would not be. */
				if ((parallel > 1) && (devNull >= 0)) {
					dup2(devNull, STDOUT_FILENO);
					dup2(devNull, STDERR_FILENO);
				}
				if (useFork != 0) {
					/* What psh_clockSync() does between fork and exec: a syscall
					 * that goes to the filesystem, plus writes that COW-split
					 * pages of .data/.bss, the stack and the heap, so exec has
					 * private pages to tear down rather than a pristine copy. */
					char *scratch = malloc(64);

					(void)access(childPath, X_OK);
					if (scratch != NULL) {
						memset(scratch, 0x5a, 64);
						free(scratch);
					}
					spawn_childTouch = launched;
				}
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

		/* A harness cutoff is the normal way these runs end, and the DONE line
		 * only prints if every launch completed -- so bank a tally as we go. */
		if ((reaped % 100uL) == 0uL) {
			printf("spawn-storm: PROGRESS %lu reaped, %lu ok, %lu failed\n", reaped, ok, failed);
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

	if (devNull >= 0) {
		close(devNull);
	}

	printf("spawn-storm: DONE %lu ok, %lu failed, slowest %lu ms\n", ok, failed, slowest);

	return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
