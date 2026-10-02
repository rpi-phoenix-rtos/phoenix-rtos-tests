/*
 * Phoenix-RTOS
 *
 * dlopen() tests: main entry point
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdlib.h>

#include <unity_fixture.h>


static void runner(void)
{
	RUN_TEST_GROUP(dlopen_exports);
	RUN_TEST_GROUP(dlopen_plugin);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
