/*
 * Phoenix-RTOS
 *
 * POSIX standard library functions tests
 *
 * HEADER:
 *    - dlfcn.h
 *
 * TESTED:
 *    - dlopen(NULL, ...) — a handle to the main program (POSIX "global object")
 *    - dlsym()/dlclose()/dlerror() contract on that handle
 *
 * dlopen(NULL) must return a usable, non-NULL handle whose dlsym resolves against
 * the running program's own symbol table (used e.g. by CPython's ctypes PyDLL(None)).
 * NOTE: positive symbol resolution needs the host linked *unstripped* (dl reads its
 * .symtab); the installed test binaries are stripped, so the end-to-end resolve path
 * is covered by the ctypes integration test on the (unstripped) python. Here we test
 * the API contract that holds regardless of stripping.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <dlfcn.h>
#include <stddef.h>

#include <unity_fixture.h>


TEST_GROUP(dlopen_self);


TEST_SETUP(dlopen_self)
{
	(void)dlerror(); /* clear any stale error */
}


TEST_TEAR_DOWN(dlopen_self)
{
}


/* dlopen(NULL) returns a non-NULL main-program handle and leaves no error */
TEST(dlopen_self, open_null_returns_handle)
{
	void *h = dlopen(NULL, RTLD_NOW);

	TEST_ASSERT_NOT_NULL(h);
	TEST_ASSERT_NULL(dlerror());
	TEST_ASSERT_EQUAL_INT(0, dlclose(h));
}


/* the main-program handle is stable across calls */
TEST(dlopen_self, open_null_is_stable)
{
	void *a = dlopen(NULL, RTLD_NOW);
	void *b = dlopen(NULL, RTLD_LAZY);

	TEST_ASSERT_NOT_NULL(a);
	TEST_ASSERT_EQUAL_PTR(a, b);
	TEST_ASSERT_EQUAL_INT(0, dlclose(a));
	TEST_ASSERT_EQUAL_INT(0, dlclose(b));
}


/* a missing symbol resolves to NULL and reports an error via dlerror() */
TEST(dlopen_self, sym_missing_reports_error)
{
	void *h = dlopen(NULL, RTLD_NOW);

	TEST_ASSERT_NOT_NULL(h);
	TEST_ASSERT_NULL(dlsym(h, "phoenix_no_such_symbol_zzz42"));
	TEST_ASSERT_NOT_NULL(dlerror());     /* error was set */
	TEST_ASSERT_NULL(dlerror());         /* and cleared by the read */
	TEST_ASSERT_EQUAL_INT(0, dlclose(h));
}


/* bad arguments are rejected without crashing */
TEST(dlopen_self, bad_args)
{
	void *h = dlopen(NULL, RTLD_NOW);

	TEST_ASSERT_NOT_NULL(h);
	TEST_ASSERT_NULL(dlsym(h, NULL));            /* NULL symbol */
	TEST_ASSERT_NULL(dlsym(NULL, "anything"));   /* NULL handle */
	TEST_ASSERT_NULL(dlopen("/no/such/lib.so", RTLD_NOW)); /* missing file -> NULL */
	TEST_ASSERT_EQUAL_INT(0, dlclose(h));
}


TEST_GROUP_RUNNER(dlopen_self)
{
	RUN_TEST_CASE(dlopen_self, open_null_returns_handle);
	RUN_TEST_CASE(dlopen_self, open_null_is_stable);
	RUN_TEST_CASE(dlopen_self, sym_missing_reports_error);
	RUN_TEST_CASE(dlopen_self, bad_args);
}
