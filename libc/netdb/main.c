/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - netdb.h
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdlib.h>

#include <unity_fixture.h>


static void runner(void)
{
	RUN_TEST_GROUP(netdb_hostname);
	RUN_TEST_GROUP(netdb_getaddrinfo);
	RUN_TEST_GROUP(netdb_gethostbyname);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
