/*
 * Phoenix-RTOS
 *
 * dlopen() tests: a plugin with undefined weak references
 *
 * Nothing defines dltest_no_such_function or dltest_no_such_object, neither the
 * plugin nor the program. Undefined weak symbols resolve to 0, so the plugin
 * must load and see both as NULL (this is how optional interfaces are probed).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stddef.h>

#include "../dltest.h"


extern int dltest_no_such_function(void) __attribute__((weak));
extern int dltest_no_such_object __attribute__((weak));


int dltest_weak_probe(void)
{
	if ((dltest_no_such_function != NULL) || (&dltest_no_such_object != NULL)) {
		return -1;
	}
	return dltest_host_add(1, 1);
}
