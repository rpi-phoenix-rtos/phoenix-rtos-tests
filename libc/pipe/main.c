/*
 * Phoenix-RTOS
 *
 * test-libc-pipe
 *
 * Main entry point. `--pipe-child <case> <args...>` runs a child that a case
 * started with exec, to check what crosses an exec.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdlib.h>
#include <string.h>

#include "unity_fixture.h"


extern const char *pipe_self;
extern int pipe_childMain(int argc, char *argv[]);


void runner(void)
{
	RUN_TEST_GROUP(pipe_semantics);
	RUN_TEST_GROUP(pipe_perf);
}


int main(int argc, char *argv[])
{
	if ((argc >= 3) && (strcmp(argv[1], "--pipe-child") == 0)) {
		return pipe_childMain(argc - 2, argv + 2);
	}

	pipe_self = argv[0];

	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
