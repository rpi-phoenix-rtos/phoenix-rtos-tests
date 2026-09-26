/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - stdio.h
 *
 * TESTED:
 *    - open_memstream()
 *    - fmemopen()
 *
 * Both were missing (libdrm and Mesa need open_memstream). libphoenix builds
 * them on its ordinary buffered FILE, with memory in place of a descriptor, so
 * these cases are mostly about the buffer the CALLER sees: when *bufp and *sizep
 * are valid, NUL termination, zero-filled gaps, where fmemopen puts its NUL,
 * and that fixed buffers are never written past. Where glibc deviates from
 * POSIX (it truncates a memory stream on a write after a backward seek), the
 * POSIX expectation is asserted on Phoenix only.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unity_fixture.h>


TEST_GROUP(stdio_memstream);
TEST_GROUP(stdio_fmemopen);


TEST_SETUP(stdio_memstream)
{
}


TEST_TEAR_DOWN(stdio_memstream)
{
}


TEST(stdio_memstream, fprintf_then_fclose)
{
	char *buf = NULL;
	size_t size = 12345;
	FILE *f = open_memstream(&buf, &size);

	TEST_ASSERT_NOT_NULL(f);
	TEST_ASSERT_EQUAL_INT(11, fprintf(f, "hello %05d", 42));
	TEST_ASSERT_EQUAL_INT(0, fclose(f));

	TEST_ASSERT_NOT_NULL(buf);
	TEST_ASSERT_EQUAL_size_t(11, size);
	TEST_ASSERT_EQUAL_STRING("hello 00042", buf); /* NUL-terminated */
	free(buf);
}


TEST(stdio_memstream, empty_stream_is_empty_string)
{
	char *buf = NULL;
	size_t size = 12345;
	FILE *f = open_memstream(&buf, &size);

	TEST_ASSERT_NOT_NULL(f);
	TEST_ASSERT_EQUAL_INT(0, fclose(f));
	TEST_ASSERT_NOT_NULL(buf);
	TEST_ASSERT_EQUAL_size_t(0, size);
	TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
	free(buf);
}


TEST(stdio_memstream, fflush_publishes_buffer_and_size)
{
	char *buf = NULL;
	size_t size = 0;
	FILE *f = open_memstream(&buf, &size);

	TEST_ASSERT_NOT_NULL(f);
	fputs("abc", f);
	TEST_ASSERT_EQUAL_INT(0, fflush(f));
	TEST_ASSERT_EQUAL_size_t(3, size);
	TEST_ASSERT_EQUAL_STRING("abc", buf);

	fputs("defg", f);
	TEST_ASSERT_EQUAL_INT(0, fflush(f));
	TEST_ASSERT_EQUAL_size_t(7, size);
	TEST_ASSERT_EQUAL_STRING("abcdefg", buf);
	TEST_ASSERT_EQUAL_INT(7, (int)ftell(f));

	TEST_ASSERT_EQUAL_INT(0, fclose(f));
	free(buf);
}


TEST(stdio_memstream, grows_past_several_buffers)
{
	char *buf = NULL;
	size_t size = 0, i;
	const size_t n = 3u * BUFSIZ + 123u;
	FILE *f = open_memstream(&buf, &size);

	TEST_ASSERT_NOT_NULL(f);
	for (i = 0; i < n; i++) {
		TEST_ASSERT_EQUAL_INT((int)('a' + (i % 26)), fputc((int)('a' + (i % 26)), f));
	}
	TEST_ASSERT_EQUAL_INT(0, fclose(f));

	TEST_ASSERT_EQUAL_size_t(n, size);
	for (i = 0; i < n; i++) {
		if (buf[i] != (char)('a' + (i % 26))) {
			TEST_FAIL_MESSAGE("contents corrupted");
		}
	}
	TEST_ASSERT_EQUAL_CHAR('\0', buf[n]);
	free(buf);
}


TEST(stdio_memstream, seek_past_end_zero_fills_gap)
{
	char *buf = NULL;
	size_t size = 0;
	FILE *f = open_memstream(&buf, &size);

	TEST_ASSERT_NOT_NULL(f);
	fputs("ab", f);
	TEST_ASSERT_EQUAL_INT(0, fseek(f, 10, SEEK_SET));
	fputc('z', f);
	TEST_ASSERT_EQUAL_INT(0, fclose(f));

	TEST_ASSERT_EQUAL_size_t(11, size);
	TEST_ASSERT_EQUAL_MEMORY("ab\0\0\0\0\0\0\0\0z", buf, 11);
	TEST_ASSERT_EQUAL_CHAR('\0', buf[11]);
	free(buf);
}


TEST(stdio_memstream, size_is_position_after_seek_back)
{
	char *buf = NULL;
	size_t size = 0;
	FILE *f = open_memstream(&buf, &size);

	TEST_ASSERT_NOT_NULL(f);
	fputs("hello world", f);
	TEST_ASSERT_EQUAL_INT(0, fseek(f, 5, SEEK_SET));
	TEST_ASSERT_EQUAL_INT(0, fflush(f));
	/* POSIX: the smaller of the length and the position */
	TEST_ASSERT_EQUAL_size_t(5, size);
	TEST_ASSERT_EQUAL_MEMORY("hello world", buf, 11);

	TEST_ASSERT_EQUAL_INT(0, fclose(f));
	TEST_ASSERT_EQUAL_size_t(5, size);
	TEST_ASSERT_EQUAL_STRING("hello", buf); /* terminated at the size */
	free(buf);
}


TEST(stdio_memstream, length_only_grows)
{
#ifdef __phoenix__
	char *buf = NULL;
	size_t size = 0;
	FILE *f = open_memstream(&buf, &size);

	TEST_ASSERT_NOT_NULL(f);
	fputs("0123456789", f);
	TEST_ASSERT_EQUAL_INT(0, fseek(f, 2, SEEK_SET));
	fputs("ab", f);
	/* POSIX: a write only moves the length if it passes it, so SEEK_END is
	 * still 10 bytes out. (glibc truncates the stream to 4 here.) */
	TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_END));
	TEST_ASSERT_EQUAL_INT(10, (int)ftell(f));
	TEST_ASSERT_EQUAL_INT(0, fclose(f));
	TEST_ASSERT_EQUAL_size_t(10, size);
	TEST_ASSERT_EQUAL_STRING("01ab456789", buf);
	free(buf);
#else
	TEST_IGNORE_MESSAGE("glibc truncates on a write after a backward seek");
#endif
}


TEST(stdio_memstream, is_write_only)
{
	char *buf = NULL;
	size_t size = 0;
	FILE *f = open_memstream(&buf, &size);

	TEST_ASSERT_NOT_NULL(f);
	fputs("x", f);
	rewind(f);
#ifdef __phoenix__
	/* POSIX: "opened for writing". (glibc's can be read back.) */
	TEST_ASSERT_EQUAL_INT(EOF, fgetc(f));
	TEST_ASSERT_TRUE(ferror(f));
#endif
	TEST_ASSERT_EQUAL_INT(-1, fileno(f)); /* no descriptor behind it */
	TEST_ASSERT_EQUAL_INT(0, fclose(f));
	free(buf);
}


TEST(stdio_memstream, null_arguments)
{
#ifdef __phoenix__
	size_t size;
	char *buf;

	errno = 0;
	TEST_ASSERT_NULL(open_memstream(NULL, &size));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
	errno = 0;
	TEST_ASSERT_NULL(open_memstream(&buf, NULL));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
#else
	TEST_IGNORE_MESSAGE("not portable: glibc does not check");
#endif
}


TEST_GROUP_RUNNER(stdio_memstream)
{
	RUN_TEST_CASE(stdio_memstream, fprintf_then_fclose);
	RUN_TEST_CASE(stdio_memstream, empty_stream_is_empty_string);
	RUN_TEST_CASE(stdio_memstream, fflush_publishes_buffer_and_size);
	RUN_TEST_CASE(stdio_memstream, grows_past_several_buffers);
	RUN_TEST_CASE(stdio_memstream, seek_past_end_zero_fills_gap);
	RUN_TEST_CASE(stdio_memstream, size_is_position_after_seek_back);
	RUN_TEST_CASE(stdio_memstream, length_only_grows);
	RUN_TEST_CASE(stdio_memstream, is_write_only);
	RUN_TEST_CASE(stdio_memstream, null_arguments);
}


TEST_SETUP(stdio_fmemopen)
{
}


TEST_TEAR_DOWN(stdio_fmemopen)
{
}


TEST(stdio_fmemopen, read_whole_buffer_including_nuls)
{
	char buf[8] = { 'a', 'b', '\0', 'c', 'd', 'e', 'f', 'g' };
	char out[16];
	FILE *f = fmemopen(buf, sizeof(buf), "r");

	TEST_ASSERT_NOT_NULL(f);
	TEST_ASSERT_EQUAL_size_t(8, fread(out, 1, sizeof(out), f));
	TEST_ASSERT_EQUAL_MEMORY(buf, out, 8);
	TEST_ASSERT_TRUE(feof(f));

	/* ungetc() + re-read, and rewinding */
	TEST_ASSERT_EQUAL_INT(0, fseek(f, 3, SEEK_SET));
	TEST_ASSERT_EQUAL_INT('c', fgetc(f));
	TEST_ASSERT_EQUAL_INT('X', ungetc('X', f));
	TEST_ASSERT_EQUAL_INT('X', fgetc(f));
	TEST_ASSERT_EQUAL_INT('d', fgetc(f));
	TEST_ASSERT_EQUAL_INT(5, (int)ftell(f));
	TEST_ASSERT_EQUAL_MEMORY("ab\0cdefg", buf, 8); /* ungetc never writes */

	/* a read-only stream refuses writes */
	TEST_ASSERT_EQUAL_size_t(0, fwrite("zz", 1, 2, f));
	TEST_ASSERT_EQUAL_INT(0, fclose(f));
	TEST_ASSERT_EQUAL_MEMORY("ab\0cdefg", buf, 8);
}


TEST(stdio_fmemopen, write_truncates_and_terminates)
{
	char buf[16];
	FILE *f;

	memset(buf, 'q', sizeof(buf));
	f = fmemopen(buf, sizeof(buf), "w");
	TEST_ASSERT_NOT_NULL(f);
	TEST_ASSERT_EQUAL_INT(5, fprintf(f, "%d", 12345));
	TEST_ASSERT_EQUAL_INT(0, fclose(f));

	TEST_ASSERT_EQUAL_STRING("12345", buf);
	TEST_ASSERT_EQUAL_CHAR('q', buf[6]); /* nothing written past the NUL */
}


TEST(stdio_fmemopen, never_writes_past_the_buffer)
{
	char mem[16], *buf = mem + 4;
	FILE *f;
	size_t n;

	memset(mem, 'q', sizeof(mem));
	f = fmemopen(buf, 8, "w");
	TEST_ASSERT_NOT_NULL(f);
	setvbuf(f, NULL, _IONBF, 0);

	n = fwrite("0123456789", 1, 10, f);
	TEST_ASSERT_TRUE(n < 10);
	TEST_ASSERT_TRUE(ferror(f));
	fclose(f);

	/* a write-only stream that filled the buffer ends in a NUL in its last byte */
	TEST_ASSERT_EQUAL_MEMORY("0123456", buf, 7);
	TEST_ASSERT_EQUAL_CHAR('\0', buf[7]);
	TEST_ASSERT_EQUAL_MEMORY("qqqq", mem, 4);
	TEST_ASSERT_EQUAL_MEMORY("qqqq", mem + 12, 4);
}


TEST(stdio_fmemopen, append_starts_at_first_nul)
{
	char buf[16] = "abc";
	FILE *f = fmemopen(buf, sizeof(buf), "a");

	TEST_ASSERT_NOT_NULL(f);
	TEST_ASSERT_EQUAL_INT(3, (int)ftell(f));
	fputs("de", f);
	/* append: a seek does not move where writes go */
	TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_SET));
	fputs("f", f);
	TEST_ASSERT_EQUAL_INT(0, fclose(f));
	TEST_ASSERT_EQUAL_STRING("abcdef", buf);
}


TEST(stdio_fmemopen, update_overwrites_in_place)
{
	char buf[12] = "hello world";
	char out[8] = { 0 };
	FILE *f = fmemopen(buf, sizeof(buf), "r+");

	TEST_ASSERT_NOT_NULL(f);
	TEST_ASSERT_EQUAL_INT(0, fseek(f, 6, SEEK_SET));
	fputs("W", f);
	TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_CUR)); /* write -> read turnaround */
	TEST_ASSERT_EQUAL_size_t(4, fread(out, 1, 4, f));
	TEST_ASSERT_EQUAL_STRING("orld", out);
	TEST_ASSERT_EQUAL_INT(0, fclose(f));
	/* not growing the contents writes no NUL */
	TEST_ASSERT_EQUAL_STRING("hello World", buf);
}


TEST(stdio_fmemopen, seek_bounds)
{
	char buf[10] = "0123456789";
	FILE *f = fmemopen(buf, sizeof(buf), "r");

	TEST_ASSERT_NOT_NULL(f);
	TEST_ASSERT_EQUAL_INT(0, fseek(f, 4, SEEK_SET));
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, fseek(f, 11, SEEK_SET));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
#ifdef __phoenix__
	/* a failed seek leaves the position alone (glibc 2.43 loses it) */
	TEST_ASSERT_EQUAL_INT(4, (int)ftell(f));
	TEST_ASSERT_EQUAL_INT('4', fgetc(f));
#endif
	TEST_ASSERT_EQUAL_INT(0, fseek(f, 4, SEEK_SET));
	errno = 0;
	TEST_ASSERT_EQUAL_INT(-1, fseek(f, -5, SEEK_CUR));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
	TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_END));
	TEST_ASSERT_EQUAL_INT(10, (int)ftell(f));
	TEST_ASSERT_EQUAL_INT(EOF, fgetc(f));
	TEST_ASSERT_EQUAL_INT(0, fclose(f));
}


TEST(stdio_fmemopen, private_buffer)
{
	char out[8] = { 0 };
	FILE *f = fmemopen(NULL, 64, "w+");

	TEST_ASSERT_NOT_NULL(f);
	fputs("xyz", f);
	rewind(f);
	TEST_ASSERT_EQUAL_size_t(3, fread(out, 1, sizeof(out), f));
	TEST_ASSERT_EQUAL_STRING("xyz", out);
	TEST_ASSERT_EQUAL_INT(0, fclose(f)); /* frees the private buffer */
}


TEST(stdio_fmemopen, invalid_arguments)
{
	char buf[4];

	errno = 0;
	TEST_ASSERT_NULL(fmemopen(buf, sizeof(buf), "x"));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
#ifdef __phoenix__
	/* POSIX "may fail"; libphoenix (like musl) does, glibc does not */
	errno = 0;
	TEST_ASSERT_NULL(fmemopen(buf, 0, "r"));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
#endif
}


TEST_GROUP_RUNNER(stdio_fmemopen)
{
	RUN_TEST_CASE(stdio_fmemopen, read_whole_buffer_including_nuls);
	RUN_TEST_CASE(stdio_fmemopen, write_truncates_and_terminates);
	RUN_TEST_CASE(stdio_fmemopen, never_writes_past_the_buffer);
	RUN_TEST_CASE(stdio_fmemopen, append_starts_at_first_nul);
	RUN_TEST_CASE(stdio_fmemopen, update_overwrites_in_place);
	RUN_TEST_CASE(stdio_fmemopen, seek_bounds);
	RUN_TEST_CASE(stdio_fmemopen, private_buffer);
	RUN_TEST_CASE(stdio_fmemopen, invalid_arguments);
}
