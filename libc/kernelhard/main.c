/*
 * Phoenix-RTOS
 *
 * test-libc-kernelhard
 *
 * Kernel hardening checks seen from user space: what a new program inherits
 * across exec, how the kernel treats buffers that run past a mapping, and
 * what a signal frame exposes.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdlib.h>

#include <unity_fixture.h>

#include "kernelhard.h"


void runner(void)
{
	RUN_TEST_GROUP(kh_exec);
	RUN_TEST_GROUP(kh_range);
	RUN_TEST_GROUP(kh_sigframe);
	RUN_TEST_GROUP(kh_vm);
}


int main(int argc, char *argv[])
{
	/* Re-executed by the exec tests: report the inherited state, run no tests */
	if (getenv(KH_CHILD_ENV) != NULL) {
		return kh_childMain();
	}

	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
