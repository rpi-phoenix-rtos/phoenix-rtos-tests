/*
 * Phoenix-RTOS
 *
 * test-libc-mutex
 *
 * Main entry point.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdlib.h>

#include "unity_fixture.h"


void runner(void)
{
	RUN_TEST_GROUP(mutex_fast);
	RUN_TEST_GROUP(mutex_semantics);
	RUN_TEST_GROUP(cond_fast);
	RUN_TEST_GROUP(mutex_fork);
	RUN_TEST_GROUP(futex);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
