/*
 * Phoenix-RTOS
 *
 * dlopen() tests: a plugin with a thread-local variable
 *
 * There is no dynamic TLS: dlopen() must refuse the object with an error that
 * says so.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "../dltest.h"


static __thread int dltest_tls_counter;


int dltest_tls_bump(void)
{
	return ++dltest_tls_counter;
}
