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
 * log.
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
 * STEP TAGS AND THE WATCHDOG. The first -f runs stopped dead, about once in a
 * hundred launches, with no fault and no status -- and the console was cut
 * mid-line, so the tty driver had stopped draining too. Output that goes
 * through the tty therefore cannot say where the launch stopped. With -t
 * (implied by -f; -n turns it off) each step prints a tag through debug(),
 * which the kernel writes straight to the UART; it bypasses the tty server, so
 * a tag is on the wire before the call it announces is made:
 *
 *   STORM p fc <i>             parent, about to call fork()/vfork()
 *   STORM p fr <i> <pid>       parent, fork()/vfork() returned
 *   STORM c ac <i>             child (-f), about to call access()
 *   STORM c ax <i>             child (-f), access() returned
 *   STORM c ex <i>             child, about to call execv()
 *   STORM c ef <i>             child, execv() failed
 *   STORM p wc <i>             parent, about to call waitpid()
 *   STORM p we <i>             parent, waitpid() was interrupted (EINTR)
 *   STORM p wr <i> <pid> <status> <ms>   parent, waitpid() returned
 *
 * <i> is the launch that was in flight most recently (under -p the parent's
 * wait tags name the newest launch, not the one reaped). The tags are short on
 * purpose: the kernel console busy-waits on the UART with the console spinlock
 * held, about 87 us per byte.
 *
 * Unless -w 0 is given, the program started from the shell is only a monitor:
 * it runs the storm as a child of its own (the worker, same binary) and checks
 * once a second, through threadsinfo(), that the worker keeps creating
 * children. A watchdog inside the worker would need a thread or an alarm, and
 * libphoenix's alarm() starts one, which would change what every fork() copies;
 * a monitor process leaves the worker's fork()/waitpid() path exactly as it
 * was. After -w seconds with no new child the monitor prints `STORM HANG`, a
 * snapshot of every thread and a second one of whatever moved since, then
 * SIGKILLs the stuck child -- or, when no child is left, sends SIGUSR1 to the
 * worker to break a waitpid() that missed its child's exit -- so one run can
 * catch several hangs. It also prints `STORM m tick` every 5 s: if the ticks
 * stop as well, the whole system froze, not the storm.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/debug.h>
#include <sys/threads.h>
#include <fcntl.h>


#define SPAWN_MAX_PARALLEL 32u

/* Room for every thread of a desktop session; a larger count is truncated. */
#define STORM_MAX_THREADS 256

/* Default and bound for -w, in seconds. The slowest healthy launch seen is ~30 ms. */
#define STORM_WATCHDOG_DEFAULT 10uL
#define STORM_WATCHDOG_MAX     600uL

#define STORM_TICK_SECS 5uL

/* Rounds of recovery a single stall gets before the monitor only keeps ticking. */
#define STORM_MAX_DUMPS 3u

#define STORM_WORKER_ARG "--worker"


/* Written by a -f child before exec, so a .bss page is COW-split. */
static volatile unsigned long spawn_childTouch;

/* Monitor-only. Static, so a snapshot allocates nothing while the system is wedged. */
static threadinfo_t storm_snap[STORM_MAX_THREADS];
static threadinfo_t storm_snap2[STORM_MAX_THREADS];


static unsigned long spawn_elapsedMs(const struct timeval *start, const struct timeval *end)
{
	return (unsigned long)(end->tv_sec - start->tv_sec) * 1000uL
			+ (unsigned long)((end->tv_usec - start->tv_usec) / 1000);
}


/* Formats without stdio, so the tags are as safe between fork and exec as the
 * syscall they go out through. Negative values print with a sign. */
static char *storm_putNum(char *p, long long v)
{
	char digits[24];
	unsigned long long u = (v < 0) ? (unsigned long long)(-(v + 1)) + 1uLL : (unsigned long long)v;
	unsigned int n = 0;

	if (v < 0) {
		*p++ = '-';
	}

	do {
		digits[n++] = (char)('0' + (int)(u % 10uLL));
		u /= 10uLL;
	} while (u != 0uLL);

	while (n > 0u) {
		*p++ = digits[--n];
	}

	return p;
}


/* One step tag: "STORM <who> <what> <i>[ <a>[ <b>[ <c>]]]\n". */
static void storm_tag(int enabled, const char *who, const char *what, unsigned long i, int nvals, long long a, long long b, long long c)
{
	char line[96];
	char *p = line;
	const char *s;

	if (enabled == 0) {
		return;
	}

	for (s = "STORM "; *s != '\0'; s++) {
		*p++ = *s;
	}
	for (s = who; *s != '\0'; s++) {
		*p++ = *s;
	}
	*p++ = ' ';
	for (s = what; *s != '\0'; s++) {
		*p++ = *s;
	}
	*p++ = ' ';
	p = storm_putNum(p, (long long)i);
	if (nvals > 0) {
		*p++ = ' ';
		p = storm_putNum(p, a);
	}
	if (nvals > 1) {
		*p++ = ' ';
		p = storm_putNum(p, b);
	}
	if (nvals > 2) {
		*p++ = ' ';
		p = storm_putNum(p, c);
	}
	*p++ = '\n';
	*p = '\0';

	debug(line);
}


/* Monitor lines go to the kernel console for the same reason the tags do. */
static void storm_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void storm_say(const char *fmt, ...)
{
	char line[192];
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	debug(line);
}


static void storm_pokeHandler(int sig)
{
	/* Only here to turn SIGUSR1 into an EINTR from waitpid(). */
	(void)sig;
}


static unsigned long storm_monoSecs(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);

	return (unsigned long)ts.tv_sec;
}


static int storm_snapshot(threadinfo_t *snap)
{
	/* No PH_THREADINFO_VMEM: it walks each process's map under the map lock
	 * (threads.c _proc_calculateVmem), unsynchronised against exec replacing
	 * that map -- the very path under suspicion. */
	int n = threadsinfo(STORM_MAX_THREADS, PH_THREADINFO_BASIC | PH_THREADINFO_NAME, snap);

	if (n > STORM_MAX_THREADS) {
		n = STORM_MAX_THREADS;
	}

	return n;
}


static void storm_dumpThread(const char *tag, const threadinfo_t *t)
{
	storm_say("STORM %s pid=%d tid=%u ppid=%d st=%d pri=%d cpu=%lld maxwait=%lld load=%d %.48s\n",
			tag, (int)t->pid, t->tid, (int)t->ppid, t->state, t->priority,
			(long long)t->cpuTime, (long long)t->wait, t->load, t->name);
}


/* The full picture, then 2 s later only what changed: a thread whose cpu time
 * grows while the storm is stuck is spinning, one whose state flips is alive. */
static void storm_dumpAll(pid_t self)
{
	int n, n2, i, j;

	n = storm_snapshot(storm_snap);
	storm_say("STORM d begin threads=%d\n", n);
	for (i = 0; i < n; i++) {
		storm_dumpThread("d", &storm_snap[i]);
	}

	sleep(2);

	n2 = storm_snapshot(storm_snap2);
	for (j = 0; j < n2; j++) {
		const threadinfo_t *t = &storm_snap2[j];

		if (t->pid == self) {
			continue;
		}
		for (i = 0; i < n; i++) {
			if (storm_snap[i].tid == t->tid) {
				break;
			}
		}
		if ((i == n) || (storm_snap[i].cpuTime != t->cpuTime) || (storm_snap[i].state != t->state)) {
			storm_dumpThread((i == n) ? "d2-new" : "d2-moved", t);
		}
	}
	for (i = 0; i < n; i++) {
		for (j = 0; j < n2; j++) {
			if (storm_snap2[j].tid == storm_snap[i].tid) {
				break;
			}
		}
		if (j == n2) {
			storm_dumpThread("d2-gone", &storm_snap[i]);
		}
	}
	storm_say("STORM d end threads=%d\n", n2);
}


/* Runs the storm in a child and watches it; see the header. */
static int storm_monitor(int argc, char *argv[], unsigned long watchdog)
{
	char *workerArgv[argc + 2];
	unsigned long start, now, lastProgress, lastTick, dumps = 0;
	pid_t worker, self = getpid(), maxKid = 0;
	int i, n, status, res;

	workerArgv[0] = argv[0];
	workerArgv[1] = STORM_WORKER_ARG;
	for (i = 1; i < argc; i++) {
		workerArgv[i + 1] = argv[i];
	}
	workerArgv[argc + 1] = NULL;

	worker = vfork();
	if (worker < 0) {
		fprintf(stderr, "spawn-storm: monitor vfork failed (%s)\n", strerror(errno));
		return EXIT_FAILURE;
	}
	if (worker == 0) {
		execv(argv[0], workerArgv);
		_exit(127);
	}

	/* Above the storm and the tty driver, so a userspace spinner cannot starve
	 * the watchdog; it wakes once a second, so it costs them nothing. */
	(void)setPriority(1);

	start = storm_monoSecs();
	lastProgress = start;
	lastTick = start;
	storm_say("STORM m start worker=%d watchdog=%lus\n", (int)worker, watchdog);

	for (;;) {
		sleep(1);

		res = waitpid(worker, &status, WNOHANG);
		if (res == worker) {
			storm_say("STORM m done worker=%d status=0x%x after %lus\n", (int)worker, (unsigned int)status, storm_monoSecs() - start);
			if (WIFEXITED(status) && (WEXITSTATUS(status) == 127)) {
				fprintf(stderr, "spawn-storm: could not re-run %s as the worker (use an absolute path)\n", argv[0]);
			}
			return (WIFEXITED(status) && (WEXITSTATUS(status) == 0)) ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		if ((res < 0) && (errno != EINTR)) {
			storm_say("STORM m waitpid(worker) failed errno=%d\n", errno);
			return EXIT_FAILURE;
		}

		/* Progress = a child pid we have not seen before. Pids come from a
		 * rising counter, and a healthy child lives for most of a ~30 ms
		 * launch, so a 1 Hz sample sees new ones all the time. */
		n = storm_snapshot(storm_snap);
		for (i = 0; i < n; i++) {
			if ((storm_snap[i].ppid == worker) && (storm_snap[i].pid > maxKid)) {
				maxKid = storm_snap[i].pid;
				lastProgress = storm_monoSecs();
				dumps = 0;
			}
		}

		now = storm_monoSecs();
		if ((now - lastTick) >= STORM_TICK_SECS) {
			lastTick = now;
			storm_say("STORM m tick t=%lu newest-child=%d quiet=%lus\n", now - start, (int)maxKid, now - lastProgress);
		}

		if (((now - lastProgress) < watchdog) || (dumps >= STORM_MAX_DUMPS)) {
			continue;
		}

		dumps++;
		{
			int kids = 0;

			/* One line naming every live child, then the snapshots. */
			for (i = 0; i < n; i++) {
				if (storm_snap[i].ppid == worker) {
					kids++;
					storm_say("STORM HANG worker=%d child=%d tid=%u st=%d quiet=%lus round=%lu\n",
							(int)worker, (int)storm_snap[i].pid, storm_snap[i].tid, storm_snap[i].state,
							now - lastProgress, dumps);
				}
			}
			if (kids == 0) {
				storm_say("STORM HANG worker=%d child=none quiet=%lus round=%lu\n", (int)worker, now - lastProgress, dumps);
			}

			storm_dumpAll(self);

			if (kids != 0) {
				for (i = 0; i < n; i++) {
					if (storm_snap[i].ppid == worker) {
						res = kill(storm_snap[i].pid, SIGKILL);
						storm_say("STORM m kill child=%d rc=%d\n", (int)storm_snap[i].pid, res);
					}
				}
			}
			else {
				res = kill(worker, SIGUSR1);
				storm_say("STORM m poke worker=%d rc=%d\n", (int)worker, res);
			}
		}

		/* Give the recovery a full window before judging it. */
		lastProgress = storm_monoSecs();
		if (dumps == STORM_MAX_DUMPS) {
			storm_say("STORM m stuck: %u rounds without progress, ticking only\n", STORM_MAX_DUMPS);
		}
	}
}


int main(int argc, char *argv[])
{
	char *const *childArgv;
	char *childPath;
	struct timeval before[SPAWN_MAX_PARALLEL], waitStart, after;
	unsigned long iterations, parallel = 1, i, launched = 0, reaped = 0;
	unsigned long slowest = 0, ok = 0, failed = 0, watchdog = STORM_WATCHDOG_DEFAULT;
	pid_t inflight[SPAWN_MAX_PARALLEL];
	unsigned long slot;
	pid_t pid;
	int status, res, devNull, argi = 1, useFork = 0, tags = 0, noTags = 0, isWorker = 0;

	if ((argc > 1) && (strcmp(argv[1], STORM_WORKER_ARG) == 0)) {
		isWorker = 1;
		argi = 2;
	}

	/* All options are optional so the invocations already on record still work. */
	for (;;) {
		if ((argc > (argi + 1)) && (strcmp(argv[argi], "-p") == 0)) {
			parallel = strtoul(argv[argi + 1], NULL, 10);
			if ((parallel == 0) || (parallel > SPAWN_MAX_PARALLEL)) {
				fprintf(stderr, "spawn-storm: -p must be 1..%u\n", (unsigned int)SPAWN_MAX_PARALLEL);
				return EXIT_FAILURE;
			}
			argi += 2;
		}
		else if ((argc > (argi + 1)) && (strcmp(argv[argi], "-w") == 0)) {
			watchdog = strtoul(argv[argi + 1], NULL, 10);
			if (watchdog > STORM_WATCHDOG_MAX) {
				fprintf(stderr, "spawn-storm: -w must be 0..%lu seconds\n", STORM_WATCHDOG_MAX);
				return EXIT_FAILURE;
			}
			argi += 2;
		}
		else if ((argc > argi) && (strcmp(argv[argi], "-f") == 0)) {
			useFork = 1;
			tags = 1;
			argi += 1;
		}
		else if ((argc > argi) && (strcmp(argv[argi], "-t") == 0)) {
			tags = 1;
			argi += 1;
		}
		else if ((argc > argi) && (strcmp(argv[argi], "-n") == 0)) {
			noTags = 1;
			argi += 1;
		}
		else {
			break;
		}
	}

	/* -n wins over the -t that -f implies, in any order: it is the fallback for
	 * when the tags' own UART time turns out to hide the hang. */
	if (noTags != 0) {
		tags = 0;
	}

	if (argc < (argi + 2)) {
		fprintf(stderr, "usage: %s [-f] [-t|-n] [-w <secs>] [-p <parallel>] <iterations> <path> [args...]\n", argv[0]);
		return EXIT_FAILURE;
	}

	iterations = strtoul(argv[argi], NULL, 10);
	if (iterations == 0) {
		fprintf(stderr, "spawn-storm: iterations must be non-zero\n");
		return EXIT_FAILURE;
	}

	if ((isWorker == 0) && (watchdog != 0)) {
		return storm_monitor(argc, argv, watchdog);
	}

	childPath = argv[argi + 1];
	childArgv = &argv[argi + 1];

	/* The child inherits these, so the index survives a child that wedges. */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	/* No SA_RESTART: the monitor's SIGUSR1 must end a waitpid() that missed its child. */
	if (isWorker != 0) {
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = storm_pokeHandler;
		sigemptyset(&sa.sa_mask);
		(void)sigaction(SIGUSR1, &sa, NULL);
	}

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

			storm_tag(tags, "p", "fc", launched + 1, 0, 0, 0, 0);
			pid = (useFork != 0) ? fork() : vfork();
			if (pid != 0) {
				storm_tag(tags, "p", "fr", launched + 1, 1, (long long)pid, 0, 0);
			}
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
				 * vfork and exec where stdio would not be; so is debug(). */
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

					storm_tag(tags, "c", "ac", launched + 1, 0, 0, 0, 0);
					(void)access(childPath, X_OK);
					storm_tag(tags, "c", "ax", launched + 1, 0, 0, 0, 0);
					if (scratch != NULL) {
						memset(scratch, 0x5a, 64);
						free(scratch);
					}
					spawn_childTouch = launched;
				}
				storm_tag(tags, "c", "ex", launched + 1, 0, 0, 0, 0);
				execv(childPath, childArgv);
				storm_tag(tags, "c", "ef", launched + 1, 1, (long long)errno, 0, 0);
				_exit(EXIT_FAILURE);
			}

			inflight[slot] = pid;
			launched++;
		}

		if (launched == reaped) {
			continue;
		}

		gettimeofday(&waitStart, NULL);
		for (;;) {
			storm_tag(tags, "p", "wc", launched, 0, 0, 0, 0);
			res = waitpid(-1, &status, 0);
			if ((res >= 0) || (errno != EINTR)) {
				break;
			}
			storm_tag(tags, "p", "we", launched, 0, 0, 0, 0);
		}

		gettimeofday(&after, NULL);
		storm_tag(tags, "p", "wr", launched, 3, (long long)res, (long long)((res >= 0) ? status : -errno),
				(long long)spawn_elapsedMs(&waitStart, &after));

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
