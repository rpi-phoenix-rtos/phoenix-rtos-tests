/*
 * Phoenix-RTOS
 *
 * dlopen() tests: the interface between the test program and its plugins
 *
 * The program exports dltest_host_add and dltest_host_value (exports.list); the
 * plugins import them. dltest_host_not_exported is a global the list leaves out.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef DLTEST_H
#define DLTEST_H

/* defined by the test program */
extern int dltest_host_add(int a, int b);
extern int dltest_host_value;
extern int dltest_host_not_exported(int a);

/* plugins/import.c */
extern int dltest_plugin_entry(int x);
extern int (*dltest_plugin_add_ptr(void))(int, int);
extern int *const dltest_plugin_value_ptr;
extern int dltest_plugin_ctor_count;

/* plugins/weak.c */
extern int dltest_weak_probe(void);

/* plugins/gnuhash.c */
extern int dltest_gnuhash_probe(void);

/* plugins/tls.c */
extern int dltest_tls_bump(void);

#endif
