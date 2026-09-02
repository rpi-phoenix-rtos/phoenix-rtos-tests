/*
 * Phoenix-RTOS
 *
 * phoenix-rtos-tests
 *
 * Libcache tests utilities
 *
 * Copyright 2022 Phoenix Systems
 * Author: Malgorzata Wrobel
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "libcache_utils.h"
#include "unity_fixture.h"

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>


int srcMem; /* File descriptor - simulates cached source memory */


/* Generate the byte-valued source file the cache tests read through.
 *
 * Returns EOK if the file is present afterwards -- INCLUDING when it already
 * existed. It used to return the initial -1 in that case, and since the runner
 * gates every test group on this, the suite ran its tests exactly once per
 * fresh filesystem and then silently reported "0 Tests 0 Failures ... OK"
 * forever after, because the file it had generated was still there. */
int test_genCharFile(void)
{
	int i;
	FILE *file;
	char num;

	/* TODO: remove temporary solution with access function
	 *  necessary to work around the issue:
	 *  https://github.com/phoenix-rtos/phoenix-rtos-project/issues/507
	 */
	if (access("/var/libcache_test_char.txt", F_OK) == 0) {
		return EOK;
	}

	file = fopen("/var/libcache_test_char.txt", "w");
	if (file == NULL) {
		fprintf(stderr, "Unable to open /var/libcache_test_char.txt\n");
		return -1;
	}

	if (fseek(file, 0, SEEK_SET) != 0) {
		fprintf(stderr, "fseek() fail in %s\n", __FILE__);
		fclose(file);
		return -1;
	}

	for (i = 0; i < LIBCACHE_SRC_MEM_SIZE; ++i) {
		num = rand();
		fwrite_unlocked(&num, sizeof(char), 1, file);
	}

	fclose(file);

	return EOK;
}


/* As test_genCharFile, for the int-valued source file. */
int test_genIntFile(void)
{
	int i;
	FILE *file;
	int num;

	/* TODO: remove temporary solution with access function
	 *  necessary to work around the issue:
	 *  https://github.com/phoenix-rtos/phoenix-rtos-project/issues/507
	 */
	if (access("/var/libcache_test_int.txt", F_OK) == 0) {
		return EOK;
	}

	file = fopen("/var/libcache_test_int.txt", "w");
	if (file == NULL) {
		fprintf(stderr, "Unable to open /var/libcache_test_int.txt\n");
		return -1;
	}

	if (fseek(file, 0, SEEK_SET) != 0) {
		fprintf(stderr, "fseek() fail in %s\n", __FILE__);
		fclose(file);
		return -1;
	}

	for (i = 0; i < LIBCACHE_SRC_MEM_SIZE / 4; ++i) {
		num = rand();
		fwrite_unlocked(&num, sizeof(int), 1, file);
	}

	fclose(file);

	return EOK;
}


ssize_t test_readCb(uint64_t offset, void *buffer, size_t count, cache_devCtx_t *ctx)
{
	ssize_t ret = -1;

	lseek(srcMem, offset, SEEK_SET);
	ret = read(srcMem, buffer, count);

	return ret;
}

ssize_t test_writeCb(uint64_t offset, const void *buffer, size_t count, cache_devCtx_t *ctx)
{
	ssize_t ret = -1;

	lseek(srcMem, offset, SEEK_SET);
	ret = write(srcMem, buffer, count);

	return ret;
}

ssize_t test_readCbErr(uint64_t offset, void *buffer, size_t count, cache_devCtx_t *ctx)
{
	return -EIO;
}

ssize_t test_writeCbErr(uint64_t offset, const void *buffer, size_t count, cache_devCtx_t *ctx)
{
	return -EIO;
}

void *test_cache_write(void *args)
{
	test_write_args_t *arguments = args;

	arguments->actualCount = cache_write(arguments->cache, arguments->addr, arguments->buffer, arguments->count, arguments->policy);

	return NULL;
}

void *test_cache_read(void *args)
{
	test_read_args_t *arguments = args;

	arguments->actualCount = cache_read(arguments->cache, arguments->addr, arguments->buffer, arguments->count);

	return NULL;
}
