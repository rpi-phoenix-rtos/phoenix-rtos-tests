/*
 * Phoenix-RTOS
 *
 * libc-tests
 *
 * <stdio_ext.h> — the Solaris/glibc FILE-buffer accessors gnulib requires.
 *
 * These exist so GNU ports stop shipping their own per-libc reimplementations
 * of our FILE internals, so the tests deliberately check the SEMANTICS gnulib
 * relies on, not just that the symbols link.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdio.h>
#include <stdio_ext.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <unity_fixture.h>


#define TESTFILE "/tmp/stdio_ext_test"

static FILE *fp;


TEST_GROUP(stdio_ext);


TEST_SETUP(stdio_ext)
{
	fp = NULL;
}


TEST_TEAR_DOWN(stdio_ext)
{
	if (fp != NULL) {
		fclose(fp);
		fp = NULL;
	}
	unlink(TESTFILE);
}


/* __fpending: bytes written but not yet flushed. This is the one coreutils uses
 * to decide whether a close can lose data, so 0-after-flush must be exact. */
TEST(stdio_ext, fpending_counts_unflushed_bytes)
{
	fp = fopen(TESTFILE, "w");
	TEST_ASSERT_NOT_NULL(fp);

	/* A fully buffered stream must not have written anything through yet. */
	TEST_ASSERT_EQUAL_INT(0, (int)__fpending(fp));

	TEST_ASSERT_EQUAL_INT(5, fprintf(fp, "hello"));
	TEST_ASSERT_EQUAL_INT(5, (int)__fpending(fp));

	TEST_ASSERT_EQUAL_INT(4, (int)fwrite("abcd", 1, 4, fp));
	TEST_ASSERT_EQUAL_INT(9, (int)__fpending(fp));

	TEST_ASSERT_EQUAL_INT(0, fflush(fp));
	TEST_ASSERT_EQUAL_INT(0, (int)__fpending(fp));
}


TEST(stdio_ext, fpending_is_zero_on_a_read_stream)
{
	fp = fopen(TESTFILE, "w");
	TEST_ASSERT_NOT_NULL(fp);
	TEST_ASSERT_EQUAL_INT(4, (int)fwrite("data", 1, 4, fp));
	TEST_ASSERT_EQUAL_INT(0, fclose(fp));

	fp = fopen(TESTFILE, "r");
	TEST_ASSERT_NOT_NULL(fp);
	TEST_ASSERT_EQUAL_INT('d', fgetc(fp));
	/* Reading must never be reported as pending output. */
	TEST_ASSERT_EQUAL_INT(0, (int)__fpending(fp));
}


/* __freadahead + __freadptr + __freadseek: the trio gnulib uses to consume
 * buffered input without going back to the fd. */
TEST(stdio_ext, readahead_ptr_and_seek_agree)
{
	const char payload[] = "0123456789";
	const char *p;
	size_t avail = 0;

	fp = fopen(TESTFILE, "w");
	TEST_ASSERT_NOT_NULL(fp);
	TEST_ASSERT_EQUAL_INT(10, (int)fwrite(payload, 1, 10, fp));
	TEST_ASSERT_EQUAL_INT(0, fclose(fp));

	fp = fopen(TESTFILE, "r");
	TEST_ASSERT_NOT_NULL(fp);

	/* One read pulls the whole file into the buffer; 9 bytes then remain. */
	TEST_ASSERT_EQUAL_INT('0', fgetc(fp));
	TEST_ASSERT_EQUAL_INT(9, (int)__freadahead(fp));

	p = __freadptr(fp, &avail);
	TEST_ASSERT_NOT_NULL(p);
	TEST_ASSERT_EQUAL_INT(9, (int)avail);
	TEST_ASSERT_EQUAL_INT(0, memcmp(p, "123456789", 9));

	/* Skipping 4 buffered bytes must be indistinguishable from reading them. */
	__freadseek(fp, 4);
	TEST_ASSERT_EQUAL_INT(5, (int)__freadahead(fp));
	TEST_ASSERT_EQUAL_INT('5', fgetc(fp));

	/* An over-long seek must be refused rather than run past the buffer. */
	__freadseek(fp, 1000);
	TEST_ASSERT_EQUAL_INT(4, (int)__freadahead(fp));
}


TEST(stdio_ext, reading_and_writing_report_direction)
{
	fp = fopen(TESTFILE, "w");
	TEST_ASSERT_NOT_NULL(fp);
	TEST_ASSERT_TRUE(__fwriting(fp) != 0);
	TEST_ASSERT_TRUE(__fwritable(fp) != 0);
	TEST_ASSERT_TRUE(__freadable(fp) == 0);
	TEST_ASSERT_EQUAL_INT(0, fclose(fp));

	fp = fopen(TESTFILE, "r");
	TEST_ASSERT_NOT_NULL(fp);
	TEST_ASSERT_TRUE(__freading(fp) != 0);
	TEST_ASSERT_TRUE(__freadable(fp) != 0);
	TEST_ASSERT_TRUE(__fwritable(fp) == 0);
	TEST_ASSERT_EQUAL_INT(0, fclose(fp));

	fp = fopen(TESTFILE, "r+");
	TEST_ASSERT_NOT_NULL(fp);
	TEST_ASSERT_TRUE(__freadable(fp) != 0);
	TEST_ASSERT_TRUE(__fwritable(fp) != 0);
}


/* __fseterr: gnulib sets the error flag on a stream it has decided is broken.
 * ferror() must then agree, and clearerr() must undo it. */
TEST(stdio_ext, fseterr_sets_the_error_indicator)
{
	fp = fopen(TESTFILE, "w");
	TEST_ASSERT_NOT_NULL(fp);

	TEST_ASSERT_EQUAL_INT(0, ferror(fp));
	__fseterr(fp);
	TEST_ASSERT_TRUE(ferror(fp) != 0);

	clearerr(fp);
	TEST_ASSERT_EQUAL_INT(0, ferror(fp));
}


/* __fpurge used to be an empty stub, so buffered data survived a call that is
 * documented to discard it. Check the discard actually happens: purged output
 * must never reach the file. */
TEST(stdio_ext, fpurge_discards_buffered_output)
{
	char buf[64];
	size_t n;

	fp = fopen(TESTFILE, "w");
	TEST_ASSERT_NOT_NULL(fp);
	TEST_ASSERT_EQUAL_INT(6, (int)fwrite("dropme", 1, 6, fp));
	TEST_ASSERT_EQUAL_INT(6, (int)__fpending(fp));

	__fpurge(fp);
	TEST_ASSERT_EQUAL_INT(0, (int)__fpending(fp));

	TEST_ASSERT_EQUAL_INT(4, (int)fwrite("keep", 1, 4, fp));
	TEST_ASSERT_EQUAL_INT(0, fclose(fp));
	fp = NULL;

	fp = fopen(TESTFILE, "r");
	TEST_ASSERT_NOT_NULL(fp);
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	buf[n] = '\0';
	TEST_ASSERT_EQUAL_STRING("keep", buf);
}


TEST(stdio_ext, fpurge_discards_buffered_input)
{
	fp = fopen(TESTFILE, "w");
	TEST_ASSERT_NOT_NULL(fp);
	TEST_ASSERT_EQUAL_INT(10, (int)fwrite("0123456789", 1, 10, fp));
	TEST_ASSERT_EQUAL_INT(0, fclose(fp));

	fp = fopen(TESTFILE, "r");
	TEST_ASSERT_NOT_NULL(fp);
	TEST_ASSERT_EQUAL_INT('0', fgetc(fp));
	TEST_ASSERT_TRUE(__freadahead(fp) > 0);

	__fpurge(fp);
	TEST_ASSERT_EQUAL_INT(0, (int)__freadahead(fp));
}


TEST(stdio_ext, bufsize_and_linebuffering)
{
	fp = fopen(TESTFILE, "w");
	TEST_ASSERT_NOT_NULL(fp);

	/* A default fully-buffered stream has a buffer and is not line buffered. */
	TEST_ASSERT_TRUE(__fbufsize(fp) > 0);
	TEST_ASSERT_EQUAL_INT(0, __flbf(fp));

	TEST_ASSERT_EQUAL_INT(0, setvbuf(fp, NULL, _IOLBF, 0));
	TEST_ASSERT_TRUE(__flbf(fp) != 0);
}


/* Every accessor must tolerate NULL rather than fault: gnulib calls some of
 * these on streams it has not proven are open. */
TEST(stdio_ext, null_stream_is_safe)
{
	size_t sz = 12345;

	TEST_ASSERT_EQUAL_INT(0, (int)__fpending(NULL));
	TEST_ASSERT_EQUAL_INT(0, (int)__freadahead(NULL));
	TEST_ASSERT_EQUAL_INT(0, __freading(NULL));
	TEST_ASSERT_EQUAL_INT(0, __fwriting(NULL));
	TEST_ASSERT_EQUAL_INT(0, __freadable(NULL));
	TEST_ASSERT_EQUAL_INT(0, __fwritable(NULL));
	TEST_ASSERT_EQUAL_INT(0, __flbf(NULL));
	TEST_ASSERT_EQUAL_INT(0, (int)__fbufsize(NULL));
	TEST_ASSERT_NULL(__freadptr(NULL, &sz));
	__fseterr(NULL);
	__fpurge(NULL);
	__freadseek(NULL, 1);
}


TEST_GROUP_RUNNER(stdio_ext)
{
	RUN_TEST_CASE(stdio_ext, fpending_counts_unflushed_bytes);
	RUN_TEST_CASE(stdio_ext, fpending_is_zero_on_a_read_stream);
	RUN_TEST_CASE(stdio_ext, readahead_ptr_and_seek_agree);
	RUN_TEST_CASE(stdio_ext, reading_and_writing_report_direction);
	RUN_TEST_CASE(stdio_ext, fseterr_sets_the_error_indicator);
	RUN_TEST_CASE(stdio_ext, fpurge_discards_buffered_output);
	RUN_TEST_CASE(stdio_ext, fpurge_discards_buffered_input);
	RUN_TEST_CASE(stdio_ext, bufsize_and_linebuffering);
	RUN_TEST_CASE(stdio_ext, null_stream_is_safe);
}
