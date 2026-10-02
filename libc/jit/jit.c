/*
 * Phoenix-RTOS
 *
 * test-libc-jit
 *
 * TESTED:
 *    - mmap(PROT_READ | PROT_WRITE | PROT_EXEC) of anonymous memory, as a JIT's code pool
 *    - __builtin___clear_cache() from EL0: code written to that memory, made visible to
 *      instruction fetch, executed, rewritten in place and executed again
 *
 * A JIT (JavaScriptCore, the Quake III VM) writes instructions through the data side and
 * then cleans the data cache and invalidates the instruction cache by address (DC CVAU /
 * IC IVAU, which __builtin___clear_cache emits on aarch64). Both are EL0 instructions only
 * when the kernel sets SCTLR_EL1.UCI; without it they trap. Mapping executable memory works
 * only at mmap() time on Phoenix: mprotect() cannot add PROT_EXEC to a mapping later.
 *
 * Each rewrite stores a NEW immediate into an instruction that has just run, so the old one
 * is in the instruction cache. A missing or ineffective flush returns the previous value
 * (Cortex-A72's instruction cache does not snoop data writes), and the check fails. The last
 * test does the rewrite without the flush and reports how often the stale value came back:
 * that is the evidence that the other tests can fail on this CPU (on a target whose
 * instruction fetch sees data writes, e.g. an emulator, it is ignored instead).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "unity_fixture.h"


#if defined(__aarch64__)

#define JIT_ROUNDS 1000U

/* movz w0, #imm16 ; ret */
#define JIT_MOVZ_W0(imm) (0x52800000U | (((uint32_t)(imm) & 0xffffU) << 5))
#define JIT_RET          0xd65f03c0U

typedef int (*jit_fn_t)(void);


static struct {
	unsigned char *pool; /* two pages, RWX */
	size_t pagesz;
} jit;


/* Writes `movz w0, #value ; ret` at code and makes it visible to instruction fetch */
static void jit_emit(uint32_t *code, unsigned int value, int flush)
{
	code[0] = JIT_MOVZ_W0(value);
	code[1] = JIT_RET;
	if (flush != 0) {
		__builtin___clear_cache((char *)code, (char *)(code + 2));
	}
}


static int jit_call(const uint32_t *code)
{
	jit_fn_t fn;

	/* An object pointer to a function pointer: what every JIT does */
	memcpy(&fn, &code, sizeof(fn));
	return fn();
}


TEST_GROUP(jit_icache);


TEST_SETUP(jit_icache)
{
	jit.pagesz = (size_t)sysconf(_SC_PAGESIZE);
	/* The flags of WebKit's executable pool (WTF OSAllocatorPOSIX): private anonymous RWX */
	jit.pool = mmap(NULL, 2 * jit.pagesz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	TEST_ASSERT_NOT_EQUAL(MAP_FAILED, jit.pool);
}


TEST_TEAR_DOWN(jit_icache)
{
	TEST_ASSERT_EQUAL_INT(0, munmap(jit.pool, 2 * jit.pagesz));
}


/* Code written into fresh RWX memory runs */
TEST(jit_icache, emit_and_run)
{
	uint32_t *code = (uint32_t *)jit.pool;

	jit_emit(code, 42U, 1);
	TEST_ASSERT_EQUAL_INT(42, jit_call(code));
}


/* The same instructions, rewritten and run again, return the new value every time */
TEST(jit_icache, rewrite_in_place)
{
	uint32_t *code = (uint32_t *)jit.pool;
	unsigned int i, stale = 0;
	int got;

	jit_emit(code, 0U, 1);
	TEST_ASSERT_EQUAL_INT(0, jit_call(code));

	for (i = 1U; i <= JIT_ROUNDS; i++) {
		jit_emit(code, i, 1);
		got = jit_call(code);
		if (got != (int)i) {
			stale++;
		}
	}
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, stale, "a rewritten instruction ran its old version");
}


/* Code that straddles a page boundary (a flush range of two pages) */
TEST(jit_icache, rewrite_across_pages)
{
	uint32_t *code = (uint32_t *)(jit.pool + jit.pagesz - sizeof(uint32_t));
	unsigned int i, stale = 0;

	jit_emit(code, 7U, 1);
	TEST_ASSERT_EQUAL_INT(7, jit_call(code));

	for (i = 1U; i <= JIT_ROUNDS; i++) {
		jit_emit(code, 0x8000U + i, 1);
		if (jit_call(code) != (int)(0x8000U + i)) {
			stale++;
		}
	}
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, stale, "a rewritten instruction ran its old version");
}


/* Another thread (likely on another CPU) rewrites and flushes, this one runs the code: a
 * concurrent JIT compiler thread and the mutator. IC IVAU is broadcast to the inner shareable
 * domain; the reader's ISB after it sees the new round discards what it fetched before. */
static struct {
	uint32_t *code;
	volatile unsigned int round; /* written by the writer after the flush */
	volatile unsigned int ack;   /* written by the reader after running round */
} jit_xthread;


static void *jit_writer(void *arg)
{
	unsigned int i;

	(void)arg;
	for (i = 1U; i <= JIT_ROUNDS; i++) {
		while (__atomic_load_n(&jit_xthread.ack, __ATOMIC_ACQUIRE) != i - 1U) {
			usleep(0);
		}
		jit_emit(jit_xthread.code, i, 1);
		__atomic_store_n(&jit_xthread.round, i, __ATOMIC_RELEASE);
	}

	return NULL;
}


TEST(jit_icache, rewrite_by_other_thread)
{
	pthread_t tid;
	unsigned int i, stale = 0;

	jit_xthread.code = (uint32_t *)jit.pool;
	jit_xthread.round = 0;
	jit_xthread.ack = 0;
	jit_emit(jit_xthread.code, 0U, 1);
	TEST_ASSERT_EQUAL_INT(0, jit_call(jit_xthread.code));

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&tid, NULL, jit_writer, NULL));
	for (i = 1U; i <= JIT_ROUNDS; i++) {
		while (__atomic_load_n(&jit_xthread.round, __ATOMIC_ACQUIRE) != i) {
			usleep(0);
		}
		__asm__ volatile("isb" ::: "memory");
		if (jit_call(jit_xthread.code) != (int)i) {
			stale++;
		}
		__atomic_store_n(&jit_xthread.ack, i, __ATOMIC_RELEASE);
	}
	TEST_ASSERT_EQUAL_INT(0, pthread_join(tid, NULL));

	TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, stale, "code rewritten by another thread ran its old version");
}


/* The control: the same rewrite WITHOUT the flush. Not a requirement, a measurement: a stale
 * result here shows that the checks above detect a missing flush on this CPU. */
TEST(jit_icache, stale_without_flush_is_detectable)
{
	uint32_t *code = (uint32_t *)jit.pool;
	unsigned int i, stale = 0;
	char msg[96];

	for (i = 1U; i <= JIT_ROUNDS; i++) {
		jit_emit(code, 2U * i, 1);
		(void)jit_call(code); /* the old instruction is now in the instruction cache */
		jit_emit(code, 2U * i + 1U, 0);
		if (jit_call(code) != (int)(2U * i + 1U)) {
			stale++;
		}
	}
	jit_emit(code, 0U, 1);

	(void)snprintf(msg, sizeof(msg), "without a flush %u of %u rewrites ran the old instruction", stale, JIT_ROUNDS);
	if (stale == 0U) {
		TEST_IGNORE_MESSAGE("instruction fetch saw every write without a flush here: the flush checks cannot fail on this target");
	}
	TEST_MESSAGE(msg);
}


TEST_GROUP_RUNNER(jit_icache)
{
	RUN_TEST_CASE(jit_icache, emit_and_run);
	RUN_TEST_CASE(jit_icache, rewrite_in_place);
	RUN_TEST_CASE(jit_icache, rewrite_across_pages);
	RUN_TEST_CASE(jit_icache, rewrite_by_other_thread);
	RUN_TEST_CASE(jit_icache, stale_without_flush_is_detectable);
}


static void runner(void)
{
	RUN_TEST_GROUP(jit_icache);
}

#else /* !__aarch64__ */

TEST_GROUP(jit_icache);


TEST_SETUP(jit_icache)
{
}


TEST_TEAR_DOWN(jit_icache)
{
}


TEST(jit_icache, emit_and_run)
{
	TEST_IGNORE_MESSAGE("the instruction encodings are aarch64");
}


TEST_GROUP_RUNNER(jit_icache)
{
	RUN_TEST_CASE(jit_icache, emit_and_run);
}


static void runner(void)
{
	RUN_TEST_GROUP(jit_icache);
}

#endif


int main(int argc, char *argv[])
{
	return (UnityMain(argc, (const char **)argv, runner) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
