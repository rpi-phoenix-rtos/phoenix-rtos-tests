/*
 * Phoenix-RTOS
 *
 * test-libc-execinfo
 *
 * HEADER:
 *    - execinfo.h (glibc/BSD extension)
 * TESTED:
 *    - backtrace()
 *    - backtrace_symbols()
 *    - backtrace_symbols_fd()
 *
 * libphoenix's backtrace() walks the frame-pointer chain, so this binary is
 * built with -fno-omit-frame-pointer (libc/Makefile). The strongest check is
 * exact: the second frame must equal the return address the innermost helper
 * reads with __builtin_return_address(0), so a walker that skips, repeats or
 * invents a frame fails. On a Phoenix architecture without a walker the
 * contract is "0 frames", and that is what is checked there.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Phoenix-RTOS RPi4 port
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <execinfo.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity_fixture.h"


/* Architectures on which libphoenix's backtrace() has a frame walker. Any
 * other libc (the host-generic-pc build) is expected to unwind as well. */
#if defined(__aarch64__) || !defined(__phoenix__)
#define EXECINFO_HAS_WALKER 1
#else
#define EXECINFO_HAS_WALKER 0
#endif

#define BT_MAX 32

/* A helper's code is shorter than this; bounds "the address lies in it". */
#define BT_FUNC_SPAN 512U


static struct {
	void *buf[BT_MAX];
	int n;
	void *retLevel2; /* __builtin_return_address(0) in bt_level2(): into bt_level1() */
} bt;


/* noinline + noclone keep both helpers real frames, and the "+ 1" after each
 * call keeps GCC from turning them into tail calls (which would drop a frame). */
__attribute__((noinline, noclone)) static int bt_level2(int size)
{
	bt.retLevel2 = __builtin_return_address(0);
	bt.n = backtrace(bt.buf, size);

	return bt.n + 1;
}


__attribute__((noinline, noclone)) static int bt_level1(int size)
{
	return bt_level2(size) + 1;
}


#if EXECINFO_HAS_WALKER
static int bt_inFunc(const void *addr, int (*func)(int))
{
	uintptr_t a = (uintptr_t)addr, f = (uintptr_t)func;

	return (a > f) && (a < f + BT_FUNC_SPAN);
}
#endif


TEST_GROUP(execinfo_backtrace);


TEST_SETUP(execinfo_backtrace)
{
	memset(&bt, 0, sizeof(bt));
}


TEST_TEAR_DOWN(execinfo_backtrace)
{
}


TEST(execinfo_backtrace, nested_frames)
{
	int ret = bt_level1(BT_MAX);

	TEST_ASSERT_EQUAL_INT(bt.n + 2, ret);

#if EXECINFO_HAS_WALKER
	TEST_ASSERT_GREATER_OR_EQUAL_INT(2, bt.n);
	TEST_ASSERT_LESS_OR_EQUAL_INT(BT_MAX, bt.n);
	/* frame 0: backtrace()'s caller, i.e. just after the call in bt_level2() */
	TEST_ASSERT_TRUE_MESSAGE(bt_inFunc(bt.buf[0], bt_level2), "frame 0 is not inside bt_level2()");
	/* frame 1: bt_level2()'s own return address, into bt_level1() */
	TEST_ASSERT_EQUAL_PTR(bt.retLevel2, bt.buf[1]);
	TEST_ASSERT_TRUE_MESSAGE(bt_inFunc(bt.buf[1], bt_level1), "frame 1 is not inside bt_level1()");
#else
	TEST_ASSERT_EQUAL_INT(0, bt.n);
#endif
}


TEST(execinfo_backtrace, size_limits)
{
	void *sentinel = (void *)&bt;

	bt.buf[1] = sentinel;
	(void)bt_level1(1);
#if EXECINFO_HAS_WALKER
	TEST_ASSERT_EQUAL_INT(1, bt.n);
	TEST_ASSERT_TRUE_MESSAGE(bt_inFunc(bt.buf[0], bt_level2), "frame 0 is not inside bt_level2()");
#else
	TEST_ASSERT_EQUAL_INT(0, bt.n);
#endif
	/* nothing written past size */
	TEST_ASSERT_EQUAL_PTR(sentinel, bt.buf[1]);

	bt.buf[0] = sentinel;
	(void)bt_level1(0);
	TEST_ASSERT_EQUAL_INT(0, bt.n);
	TEST_ASSERT_EQUAL_PTR(sentinel, bt.buf[0]);
}


TEST(execinfo_backtrace, symbols)
{
	char **strings;
	char expected[32];
	int i;

	(void)bt_level1(BT_MAX);
#if !EXECINFO_HAS_WALKER
	/* no frames to describe: still exercise the formatting on known values */
	bt.buf[0] = (void *)(uintptr_t)0x1234U;
	bt.buf[1] = (void *)&bt;
	bt.n = 2;
#endif
	TEST_ASSERT_GREATER_OR_EQUAL_INT(2, bt.n);

	strings = backtrace_symbols(bt.buf, bt.n);
	TEST_ASSERT_NOT_NULL(strings);
	for (i = 0; i < bt.n; i++) {
		TEST_ASSERT_NOT_NULL(strings[i]);
#ifdef __phoenix__
		/* libphoenix: no symbolization, exactly "0x<hex>", one block after the array */
		(void)snprintf(expected, sizeof(expected), "0x%" PRIxPTR, (uintptr_t)bt.buf[i]);
		TEST_ASSERT_EQUAL_STRING(expected, strings[i]);
		TEST_ASSERT_TRUE((uintptr_t)strings[i] >= (uintptr_t)(strings + bt.n));
#else
		/* glibc: "binary(symbol+off) [0x<hex>]" */
		(void)expected;
		TEST_ASSERT_NOT_NULL(strstr(strings[i], "0x"));
#endif
	}
	/* one allocation: freeing the array frees the strings */
	free(strings);
}


TEST(execinfo_backtrace, symbols_fd)
{
	char out[BT_MAX * 64];
	char expected[32];
	const char *line;
	ssize_t len;
	size_t got = 0;
	int fds[2], lines = 0;

	(void)bt_level1(4);
#if !EXECINFO_HAS_WALKER
	bt.buf[0] = (void *)(uintptr_t)0xabcU;
	bt.n = 1;
#endif
	TEST_ASSERT_GREATER_OR_EQUAL_INT(1, bt.n);

	TEST_ASSERT_EQUAL_INT(0, pipe(fds));
	backtrace_symbols_fd(bt.buf, bt.n, fds[1]);
	(void)close(fds[1]);
	while ((got < sizeof(out) - 1U) && ((len = read(fds[0], out + got, sizeof(out) - 1U - got)) > 0)) {
		got += (size_t)len;
	}
	(void)close(fds[0]);
	out[got] = '\0';

	for (line = out; *line != '\0'; line = strchr(line, '\n') + 1) {
		TEST_ASSERT_NOT_NULL_MESSAGE(strchr(line, '\n'), "last line not newline-terminated");
#ifdef __phoenix__
		TEST_ASSERT_LESS_THAN_INT(bt.n, lines);
		(void)snprintf(expected, sizeof(expected), "0x%" PRIxPTR "\n", (uintptr_t)bt.buf[lines]);
		TEST_ASSERT_EQUAL_INT(0, strncmp(expected, line, strlen(expected)));
#else
		(void)expected;
#endif
		lines++;
	}
	TEST_ASSERT_EQUAL_INT(bt.n, lines);

	/* size 0 writes nothing */
	TEST_ASSERT_EQUAL_INT(0, pipe(fds));
	backtrace_symbols_fd(bt.buf, 0, fds[1]);
	(void)close(fds[1]);
	TEST_ASSERT_EQUAL_INT(0, read(fds[0], out, sizeof(out)));
	(void)close(fds[0]);
}


TEST_GROUP_RUNNER(execinfo_backtrace)
{
	RUN_TEST_CASE(execinfo_backtrace, nested_frames);
	RUN_TEST_CASE(execinfo_backtrace, size_limits);
	RUN_TEST_CASE(execinfo_backtrace, symbols);
	RUN_TEST_CASE(execinfo_backtrace, symbols_fd);
}


static void runner(void)
{
	RUN_TEST_GROUP(execinfo_backtrace);
}


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
