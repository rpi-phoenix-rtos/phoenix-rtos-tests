/*
 * Phoenix-RTOS
 *
 * dlopen() tests: a plugin linked with only a GNU hash table (no DT_HASH)
 *
 * dlopen() takes the symbol count of an object from its DT_HASH; without one it
 * must refuse the object rather than load one whose symbols dlsym() cannot find.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "../dltest.h"


int dltest_gnuhash_probe(void)
{
	return 5;
}
