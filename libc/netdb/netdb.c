/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library functions tests
 *
 * HEADER:
 *    - netdb.h, unistd.h, sys/utsname.h
 *
 * TESTED:
 *    - gethostname(), uname() nodename
 *    - getaddrinfo() for names the host answers itself: "localhost",
 *      "*.localhost", the own hostname, /etc/hosts, the empty name, NULL
 *    - gethostbyname() for the own hostname
 *
 * The motivating case is gethostbyname(gethostname()), which games use to find
 * their own address (QuakeSpasm "UDP_Init: gethostbyname failed"). It failed on
 * Phoenix twice over: the kernel hostname is empty until set, and the network
 * stack's resolver answers only "localhost" and dotted quads, sending every other
 * name to a name server.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <unity_fixture.h>


#define HOSTNAME_SIZE 256


static char hostname[HOSTNAME_SIZE];


/* The first IPv4 answer of a getaddrinfo() result (glibc returns one per socket type) */
static const struct sockaddr_in *netdb_firstInet(const struct addrinfo *res)
{
	for (; res != NULL; res = res->ai_next) {
		if (res->ai_family == AF_INET) {
			TEST_ASSERT_NOT_NULL(res->ai_addr);
			TEST_ASSERT_EQUAL_INT(sizeof(struct sockaddr_in), res->ai_addrlen);
			return (const struct sockaddr_in *)res->ai_addr;
		}
	}
	TEST_FAIL_MESSAGE("no AF_INET answer");
	return NULL;
}


static void netdb_assertResolvesTo(const char *node, const char *service, int socktype, in_addr_t addr, in_port_t port)
{
	struct addrinfo hints, *res = NULL;
	const struct sockaddr_in *sin;
	int err;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = socktype;

	err = getaddrinfo(node, service, &hints, &res);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, err, (node != NULL) ? node : "(null)");
	TEST_ASSERT_NOT_NULL(res);

	sin = netdb_firstInet(res);
	TEST_ASSERT_EQUAL_INT(AF_INET, sin->sin_family);
	TEST_ASSERT_EQUAL_HEX32(addr, ntohl(sin->sin_addr.s_addr));
	TEST_ASSERT_EQUAL_UINT16(port, ntohs(sin->sin_port));

	freeaddrinfo(res);
}


TEST_GROUP(netdb_hostname);


TEST_SETUP(netdb_hostname)
{
	memset(hostname, 0, sizeof(hostname));
}


TEST_TEAR_DOWN(netdb_hostname)
{
}


TEST(netdb_hostname, not_empty)
{
	TEST_ASSERT_EQUAL_INT(0, gethostname(hostname, sizeof(hostname)));
	TEST_ASSERT_NOT_EQUAL_MESSAGE('\0', hostname[0], "gethostname() returned an empty name");
	TEST_ASSERT_NOT_NULL(memchr(hostname, '\0', sizeof(hostname)));
}


TEST(netdb_hostname, uname_agrees)
{
	struct utsname u;

	TEST_ASSERT_EQUAL_INT(0, gethostname(hostname, sizeof(hostname)));
	TEST_ASSERT_EQUAL_INT(0, uname(&u));
	TEST_ASSERT_EQUAL_STRING(hostname, u.nodename);
}


TEST(netdb_hostname, stable)
{
	char again[HOSTNAME_SIZE];

	TEST_ASSERT_EQUAL_INT(0, gethostname(hostname, sizeof(hostname)));
	TEST_ASSERT_EQUAL_INT(0, gethostname(again, sizeof(again)));
	TEST_ASSERT_EQUAL_STRING(hostname, again);
}


TEST_GROUP(netdb_getaddrinfo);


TEST_SETUP(netdb_getaddrinfo)
{
	memset(hostname, 0, sizeof(hostname));
}


TEST_TEAR_DOWN(netdb_getaddrinfo)
{
}


TEST(netdb_getaddrinfo, localhost)
{
	netdb_assertResolvesTo("localhost", NULL, 0, INADDR_LOOPBACK, 0);
	netdb_assertResolvesTo("LocalHost", "80", SOCK_STREAM, INADDR_LOOPBACK, 80);
}


TEST(netdb_getaddrinfo, localhost_subdomain)
{
	/* RFC 6761 6.3: names under .localhost are loopback, no name server asked */
	netdb_assertResolvesTo("test.localhost", "7", SOCK_DGRAM, INADDR_LOOPBACK, 7);
}


TEST(netdb_getaddrinfo, own_hostname)
{
	struct addrinfo hints, *res = NULL;
	const struct sockaddr_in *sin;

	TEST_ASSERT_EQUAL_INT(0, gethostname(hostname, sizeof(hostname)));

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_flags = AI_CANONNAME;
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, getaddrinfo(hostname, NULL, &hints, &res), hostname);
	TEST_ASSERT_NOT_NULL(res);

	sin = netdb_firstInet(res);
	TEST_ASSERT_NOT_EQUAL(0, sin->sin_addr.s_addr);
	TEST_ASSERT_NOT_NULL(res->ai_canonname);
	TEST_ASSERT_NOT_EQUAL('\0', res->ai_canonname[0]);

	freeaddrinfo(res);
}


TEST(netdb_getaddrinfo, hosts_file_alias)
{
	/* "localhost.localdomain" is an alias on the 127.0.0.1 line of the shipped
	 * /etc/hosts; the network stack's resolver knows only "localhost" itself */
	netdb_assertResolvesTo("localhost.localdomain", NULL, 0, INADDR_LOOPBACK, 0);
}


TEST(netdb_getaddrinfo, service_name)
{
	struct addrinfo hints, *res = NULL;

	netdb_assertResolvesTo("localhost", "http", SOCK_STREAM, INADDR_LOOPBACK, 80);

	/* AI_NUMERICSERV: a service name is not a port number */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;
	TEST_ASSERT_EQUAL_INT(EAI_NONAME, getaddrinfo("localhost", "http", &hints, &res));
}


TEST(netdb_getaddrinfo, empty_name)
{
	struct addrinfo hints, *res = NULL;
	int err;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_flags = AI_PASSIVE;

	/* An empty string is a name (not NULL) that names no host: "not known", not
	 * the EAI_FAIL "non-recoverable failure" of a rejected name-server query */
	err = getaddrinfo("", "80", &hints, &res);
#ifdef __GLIBC__
	/* glibc answers with its GNU extension EAI_NODATA ("no address") */
	TEST_ASSERT_NOT_EQUAL(0, err);
	TEST_ASSERT_NOT_EQUAL_MESSAGE(EAI_FAIL, err, gai_strerror(err));
#else
	TEST_ASSERT_EQUAL_INT_MESSAGE(EAI_NONAME, err, gai_strerror(err));
#endif
}


TEST(netdb_getaddrinfo, null_name)
{
	struct addrinfo hints, *res = NULL;
	const struct sockaddr_in *sin;

	/* NULL + AI_PASSIVE: the wildcard address to bind() to */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE;
	TEST_ASSERT_EQUAL_INT(0, getaddrinfo(NULL, "27960", &hints, &res));
	sin = netdb_firstInet(res);
	TEST_ASSERT_EQUAL_HEX32(INADDR_ANY, ntohl(sin->sin_addr.s_addr));
	TEST_ASSERT_EQUAL_UINT16(27960, ntohs(sin->sin_port));
	freeaddrinfo(res);

	/* NULL without AI_PASSIVE: the loopback address to connect() to */
	netdb_assertResolvesTo(NULL, "27960", SOCK_DGRAM, INADDR_LOOPBACK, 27960);
}


TEST(netdb_getaddrinfo, numeric)
{
	struct addrinfo hints, *res = NULL;
	const struct sockaddr_in *sin;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_flags = AI_NUMERICHOST;
	TEST_ASSERT_EQUAL_INT(0, getaddrinfo("10.42.0.1", "2049", &hints, &res));
	sin = netdb_firstInet(res);
	TEST_ASSERT_EQUAL_HEX32(0x0a2a0001, ntohl(sin->sin_addr.s_addr));
	TEST_ASSERT_EQUAL_UINT16(2049, ntohs(sin->sin_port));
	freeaddrinfo(res);

	/* AI_NUMERICHOST: a name is never looked up */
	TEST_ASSERT_NOT_EQUAL(0, getaddrinfo("localhost", NULL, &hints, &res));
}


TEST_GROUP(netdb_gethostbyname);


TEST_SETUP(netdb_gethostbyname)
{
	memset(hostname, 0, sizeof(hostname));
}


TEST_TEAR_DOWN(netdb_gethostbyname)
{
}


TEST(netdb_gethostbyname, own_hostname)
{
	struct hostent *he;

	TEST_ASSERT_EQUAL_INT(0, gethostname(hostname, sizeof(hostname)));

	he = gethostbyname(hostname);
	TEST_ASSERT_NOT_NULL_MESSAGE(he, hostname);
	TEST_ASSERT_EQUAL_INT(AF_INET, he->h_addrtype);
	TEST_ASSERT_EQUAL_INT(sizeof(struct in_addr), he->h_length);
	TEST_ASSERT_NOT_NULL(he->h_addr_list[0]);
	TEST_ASSERT_NOT_NULL(he->h_name);
}


TEST(netdb_gethostbyname, localhost)
{
	struct hostent *he;
	struct in_addr addr;

	he = gethostbyname("localhost");
	TEST_ASSERT_NOT_NULL(he);
	TEST_ASSERT_EQUAL_INT(AF_INET, he->h_addrtype);
	memcpy(&addr, he->h_addr_list[0], sizeof(addr));
	TEST_ASSERT_EQUAL_HEX32(INADDR_LOOPBACK, ntohl(addr.s_addr));
}


TEST(netdb_gethostbyname, empty_name)
{
	TEST_ASSERT_NULL(gethostbyname(""));
}


TEST_GROUP_RUNNER(netdb_hostname)
{
	RUN_TEST_CASE(netdb_hostname, not_empty);
	RUN_TEST_CASE(netdb_hostname, uname_agrees);
	RUN_TEST_CASE(netdb_hostname, stable);
}


TEST_GROUP_RUNNER(netdb_getaddrinfo)
{
	RUN_TEST_CASE(netdb_getaddrinfo, localhost);
	RUN_TEST_CASE(netdb_getaddrinfo, localhost_subdomain);
	RUN_TEST_CASE(netdb_getaddrinfo, own_hostname);
	RUN_TEST_CASE(netdb_getaddrinfo, hosts_file_alias);
	RUN_TEST_CASE(netdb_getaddrinfo, service_name);
	RUN_TEST_CASE(netdb_getaddrinfo, empty_name);
	RUN_TEST_CASE(netdb_getaddrinfo, null_name);
	RUN_TEST_CASE(netdb_getaddrinfo, numeric);
}


TEST_GROUP_RUNNER(netdb_gethostbyname)
{
	RUN_TEST_CASE(netdb_gethostbyname, own_hostname);
	RUN_TEST_CASE(netdb_gethostbyname, localhost);
	RUN_TEST_CASE(netdb_gethostbyname, empty_name);
}
