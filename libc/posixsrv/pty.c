/*
 * Phoenix-RTOS
 *
 * libc/posixsrv
 *
 * tests for pseudo-terminals (/dev/ptmx + /dev/pts/N, served by posixsrv)
 *
 * The sequence mirrors how xterm sets up its pty: open the multiplexor,
 * grantpt/unlockpt/ptsname, open the slave, then set the line discipline
 * and the window size.
 *
 * POSIX declares the ioctl request as `int`, and plenty of code keeps it in
 * one (so did libphoenix's own tcsetattr() before it was fixed). On a 64-bit
 * target such an int sign-extends every IOC_IN request -- TCSETS 0x805c7402
 * reaches ioctl() as 0xffffffff805c7402 -- and the system must still treat it
 * as the same request. The *_int_request cases below issue exactly that.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

/* grantpt(), unlockpt() and ptsname() are XSI */
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "unity_fixture.h"


static int master = -1, slave = -1;


static int ioctl_int_request(int fd, int request, void *arg)
{
	/* the int -> unsigned long conversion at the call is the point of the test */
	return ioctl(fd, request, arg);
}


TEST_GROUP(pty);


TEST_SETUP(pty)
{
	char *name;

	master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
	if (master < 0) {
		TEST_IGNORE_MESSAGE("no /dev/ptmx");
	}

	TEST_ASSERT_EQUAL_INT(0, grantpt(master));
	TEST_ASSERT_EQUAL_INT(0, unlockpt(master));

	name = ptsname(master);
	TEST_ASSERT_NOT_NULL(name);
	TEST_ASSERT_EQUAL_INT(0, strncmp(name, "/dev/pts/", 9));

	slave = open(name, O_RDWR | O_NOCTTY);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(0, slave);
}


TEST_TEAR_DOWN(pty)
{
	if (slave >= 0) {
		close(slave);
		slave = -1;
	}

	if (master >= 0) {
		close(master);
		master = -1;
	}
}


TEST(pty, tcsetattr_slave)
{
	struct termios tio;

	TEST_ASSERT_EQUAL_INT(0, tcgetattr(slave, &tio));
	TEST_ASSERT_EQUAL_INT(0, tcsetattr(slave, TCSANOW, &tio));
	TEST_ASSERT_EQUAL_INT(0, tcsetattr(slave, TCSADRAIN, &tio));
	TEST_ASSERT_EQUAL_INT(0, tcsetattr(slave, TCSAFLUSH, &tio));
}


TEST(pty, tcsets_int_request)
{
	struct termios tio;

	TEST_ASSERT_EQUAL_INT(0, tcgetattr(slave, &tio));
	tio.c_lflag &= ~(tcflag_t)ECHO;

	errno = 0;
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, ioctl_int_request(slave, (int)TCSETS, &tio), strerror(errno));

	/* the request must have been applied, not just accepted */
	memset(&tio, 0, sizeof(tio));
	TEST_ASSERT_EQUAL_INT(0, tcgetattr(slave, &tio));
	TEST_ASSERT_EQUAL_INT(0, tio.c_lflag & ECHO);
}


TEST(pty, tiocswinsz_int_request)
{
	struct winsize ws = { .ws_row = 24, .ws_col = 80 };
	struct winsize got;

	errno = 0;
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, ioctl_int_request(master, (int)TIOCSWINSZ, &ws), strerror(errno));

	ws.ws_row = 25;
	ws.ws_col = 81;
	errno = 0;
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, ioctl_int_request(slave, (int)TIOCSWINSZ, &ws), strerror(errno));

	memset(&got, 0, sizeof(got));
	TEST_ASSERT_EQUAL_INT(0, ioctl(master, TIOCGWINSZ, &got));
	TEST_ASSERT_EQUAL_UINT(25, got.ws_row);
	TEST_ASSERT_EQUAL_UINT(81, got.ws_col);
}


TEST_GROUP_RUNNER(pty)
{
	RUN_TEST_CASE(pty, tcsetattr_slave);
	RUN_TEST_CASE(pty, tcsets_int_request);
	RUN_TEST_CASE(pty, tiocswinsz_int_request);
}
