/*
 * Phoenix-RTOS
 *
 * POSIX standard library functions tests
 *
 * HEADER:
 *    - dlfcn.h
 *
 * TESTED:
 *    - dlsym()/dladdr() on the program's export table (its dynamic symbol table)
 *    - dlopen() of -fPIC plugins whose undefined symbols the program exports
 *    - undefined weak symbols in a plugin
 *    - refusal of plugins dlopen() cannot serve: no DT_HASH, thread-local storage
 *
 * This program is linked with -Wl,--no-dynamic-linker -Wl,--dynamic-list=exports.list,
 * which gives the static program a dynamic symbol table holding exactly the listed
 * symbols. That table is loaded with the program and survives strip, so these tests
 * hold for the installed (stripped) binary, where the program file has no .symtab.
 *
 * The plugins are installed next to the program; DLTEST_DIR in the environment
 * overrides their directory.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unity_fixture.h>

#include "dltest.h"


#ifndef DLTEST_DIR
#define DLTEST_DIR "/bin"
#endif


/* exported by exports.list */
int dltest_host_value = 7;


int dltest_host_add(int a, int b)
{
	return a + b;
}


/* a global the export list leaves out */
int dltest_host_not_exported(int a)
{
	return a - 1;
}


static const char *dltest_path(const char *name)
{
	static char path[256];
	const char *dir = getenv("DLTEST_DIR");

	(void)snprintf(path, sizeof(path), "%s/test-libc-dlopen-%s.so", (dir != NULL) ? dir : DLTEST_DIR, name);
	return path;
}


/* dlopen() a plugin, failing the test with dlerror() when it does not load */
static void *dltest_open(const char *name)
{
	void *h = dlopen(dltest_path(name), RTLD_NOW);

	if (h == NULL) {
		const char *err = dlerror();
		TEST_FAIL_MESSAGE((err != NULL) ? err : "dlopen failed without an error");
	}
	return h;
}


/* dlopen() a plugin that must be refused, and check that the error names the reason */
static void dltest_refused(const char *name, const char *reason)
{
	void *h = dlopen(dltest_path(name), RTLD_NOW);
	const char *err;

	if (h != NULL) {
		(void)dlclose(h);
		TEST_FAIL_MESSAGE("dlopen loaded a plugin it must refuse");
	}
	err = dlerror();
	TEST_ASSERT_NOT_NULL(err);
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, reason), err);
}


TEST_GROUP(dlopen_exports);


TEST_SETUP(dlopen_exports)
{
	(void)dlerror();
}


TEST_TEAR_DOWN(dlopen_exports)
{
}


/* an exported function: dlsym() on the program handle returns its address */
TEST(dlopen_exports, function_found)
{
	void *h = dlopen(NULL, RTLD_NOW);

	TEST_ASSERT_NOT_NULL(h);
	TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)dltest_host_add, dlsym(h, "dltest_host_add"));
	TEST_ASSERT_EQUAL_INT(0, dlclose(h));
}


/* an exported object */
TEST(dlopen_exports, object_found)
{
	void *h = dlopen(NULL, RTLD_NOW);

	TEST_ASSERT_NOT_NULL(h);
	TEST_ASSERT_EQUAL_PTR(&dltest_host_value, dlsym(h, "dltest_host_value"));
	TEST_ASSERT_EQUAL_INT(0, dlclose(h));
}


/* the export table is the whole list: a linked global it leaves out is not found */
TEST(dlopen_exports, unlisted_not_found)
{
	/* called through a pointer: a direct call could be inlined, and --gc-sections would
	   then drop the function, which would make this test pass for the wrong reason */
	int (*volatile notExported)(int) = dltest_host_not_exported;
	void *h = dlopen(NULL, RTLD_NOW);

	TEST_ASSERT_EQUAL_INT(4, notExported(5));
	TEST_ASSERT_NOT_NULL(h);
	TEST_ASSERT_NULL(dlsym(h, "dltest_host_not_exported"));
	TEST_ASSERT_NOT_NULL(dlerror());
	TEST_ASSERT_EQUAL_INT(0, dlclose(h));
}


/* dladdr() names an exported function even in a stripped program */
TEST(dlopen_exports, dladdr_names_export)
{
	Dl_info info;
	const char *fn = (const char *)(uintptr_t)dltest_host_add;

	TEST_ASSERT_NOT_EQUAL(0, dladdr(fn, &info));
	TEST_ASSERT_NOT_NULL(info.dli_sname);
	TEST_ASSERT_EQUAL_STRING("dltest_host_add", info.dli_sname);
	TEST_ASSERT_EQUAL_PTR(fn, info.dli_saddr);
}


TEST_GROUP_RUNNER(dlopen_exports)
{
	RUN_TEST_CASE(dlopen_exports, function_found);
	RUN_TEST_CASE(dlopen_exports, object_found);
	RUN_TEST_CASE(dlopen_exports, unlisted_not_found);
	RUN_TEST_CASE(dlopen_exports, dladdr_names_export);
}


TEST_GROUP(dlopen_plugin);


TEST_SETUP(dlopen_plugin)
{
	(void)dlerror();
}


TEST_TEAR_DOWN(dlopen_plugin)
{
	dltest_host_value = 7;
}


/* the plugin's calls and loads reach the program's function and object */
TEST(dlopen_plugin, imports_resolve)
{
	void *h = dltest_open("import");
	int (*entry)(int) = (int (*)(int))(uintptr_t)dlsym(h, "dltest_plugin_entry");

	TEST_ASSERT_NOT_NULL(entry);
	TEST_ASSERT_EQUAL_INT(42, entry(35));
	/* the plugin reads the program's object itself, not a copy */
	dltest_host_value = 10;
	TEST_ASSERT_EQUAL_INT(45, entry(35));
	TEST_ASSERT_EQUAL_INT(0, dlclose(h));
}


/* data relocations against imported symbols hold the program's addresses */
TEST(dlopen_plugin, data_relocations)
{
	void *h = dltest_open("import");
	int (*(*addPtr)(void))(int, int) = (int (*(*)(void))(int, int))(uintptr_t)dlsym(h, "dltest_plugin_add_ptr");
	int *const *valuePtr = (int *const *)dlsym(h, "dltest_plugin_value_ptr");

	TEST_ASSERT_NOT_NULL(addPtr);
	TEST_ASSERT_NOT_NULL(valuePtr);
	TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)dltest_host_add, (void *)(uintptr_t)addPtr());
	TEST_ASSERT_EQUAL_PTR(&dltest_host_value, *valuePtr);
	TEST_ASSERT_EQUAL_INT(0, dlclose(h));
}


/* the plugin's constructor ran once, before dlopen() returned */
TEST(dlopen_plugin, constructor_ran)
{
	void *h = dltest_open("import");
	int *count = (int *)dlsym(h, "dltest_plugin_ctor_count");

	TEST_ASSERT_NOT_NULL(count);
	TEST_ASSERT_EQUAL_INT(1, *count);
	TEST_ASSERT_EQUAL_INT(0, dlclose(h));
}


/* a hidden symbol is not in the plugin's dynamic symbol table */
TEST(dlopen_plugin, hidden_not_found)
{
	void *h = dltest_open("import");

	TEST_ASSERT_NULL(dlsym(h, "dltest_plugin_hidden"));
	TEST_ASSERT_NOT_NULL(dlerror());
	TEST_ASSERT_EQUAL_INT(0, dlclose(h));
}


/* undefined weak references load, and are NULL */
TEST(dlopen_plugin, weak_undefined_is_null)
{
	void *h = dltest_open("weak");
	int (*probe)(void) = (int (*)(void))(uintptr_t)dlsym(h, "dltest_weak_probe");

	TEST_ASSERT_NOT_NULL(probe);
	TEST_ASSERT_EQUAL_INT(2, probe());
	TEST_ASSERT_EQUAL_INT(0, dlclose(h));
}


/* without DT_HASH dlsym() could find nothing: the plugin is refused */
TEST(dlopen_plugin, no_dt_hash_refused)
{
	dltest_refused("gnuhash", "DT_HASH");
}


/* there is no dynamic TLS: a plugin with thread-local storage is refused */
TEST(dlopen_plugin, tls_refused)
{
	dltest_refused("tls", "thread-local");
}


TEST_GROUP_RUNNER(dlopen_plugin)
{
	RUN_TEST_CASE(dlopen_plugin, imports_resolve);
	RUN_TEST_CASE(dlopen_plugin, data_relocations);
	RUN_TEST_CASE(dlopen_plugin, constructor_ran);
	RUN_TEST_CASE(dlopen_plugin, hidden_not_found);
	RUN_TEST_CASE(dlopen_plugin, weak_undefined_is_null);
	RUN_TEST_CASE(dlopen_plugin, no_dt_hash_refused);
	RUN_TEST_CASE(dlopen_plugin, tls_refused);
}
