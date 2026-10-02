/*
 * Phoenix-RTOS
 *
 * dlopen() tests: a plugin that imports a function and an object from the program
 *
 * Built -shared -fPIC -nostdlib (see ../Makefile): every reference below to a
 * dltest_host_* symbol is left undefined and must be bound by dlopen() to the
 * program's export. The call goes through the PLT (R_AARCH64_JUMP_SLOT), the
 * object load through the GOT (R_AARCH64_GLOB_DAT), and the two initialised
 * pointers are R_AARCH64_ABS64 data relocations.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "../dltest.h"


int dltest_plugin_ctor_count;


/* hidden: in the object, but not in its dynamic symbol table */
__attribute__((visibility("hidden"), noinline)) int dltest_plugin_hidden(int x)
{
	return x + dltest_host_value;
}


int dltest_plugin_entry(int x)
{
	return dltest_host_add(x, dltest_plugin_hidden(0));
}


/* volatile: keeps the pointer in data, so it is relocated there and not folded into the code */
static int (*volatile dltest_plugin_addFn)(int, int) = dltest_host_add;
int *const dltest_plugin_value_ptr = &dltest_host_value;


int (*dltest_plugin_add_ptr(void))(int, int)
{
	return dltest_plugin_addFn;
}


__attribute__((constructor)) static void dltest_plugin_ctor(void)
{
	dltest_plugin_ctor_count++;
}
