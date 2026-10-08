/*
 * Phoenix-RTOS
 *
 * test-libc-semaphore
 *
 * Main entry point.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdlib.h>

#include "unity_fixture.h"


/* no need for forward declarations, RUN_TEST_GROUP does it by itself */
void runner(void)
{
	/* libphoenix counting semaphores (semaphoreCreate/Up/Down) */
	RUN_TEST_GROUP(test_semaphore);
	/* POSIX semaphores: our suite, then upstream's unnamed and named suites */
	RUN_TEST_GROUP(posix_semaphore);
	RUN_TEST_GROUP(sem_unnamed);
	RUN_TEST_GROUP(sem_named);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
