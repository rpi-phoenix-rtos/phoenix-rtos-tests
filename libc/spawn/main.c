/*
 * Phoenix-RTOS
 *
 * test-libc-spawn
 *
 * Main entry point. The binary is also the program the tests spawn: run with
 * "--child <check> ..." it performs one check (see spawn.c) and reports the
 * result in its exit status instead of running the suite.
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


extern int spawn_childMain(int argc, char *argv[]);
extern void spawn_setSelf(const char *argv0);


void runner(void)
{
	RUN_TEST_GROUP(spawn_basic);
	RUN_TEST_GROUP(spawn_file_actions);
	RUN_TEST_GROUP(spawn_attr);
}


int main(int argc, char *argv[])
{
	if ((argc >= 3) && (strcmp(argv[1], "--child") == 0)) {
		return spawn_childMain(argc - 2, argv + 2);
	}

	spawn_setSelf(argv[0]);

	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
