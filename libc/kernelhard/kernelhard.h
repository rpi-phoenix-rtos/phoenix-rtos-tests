/*
 * Phoenix-RTOS
 *
 * test-libc-kernelhard - shared declarations
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef KERNELHARD_H
#define KERNELHARD_H

#define KH_CHILD_ENV "KH_EXEC_CHILD"

/* The re-executed child: exit 0 if it starts with the default FP control state, 1 otherwise */
int kh_childMain(void);

#endif
