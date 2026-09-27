/*
 * Phoenix-RTOS
 *
 * POSIX.1-2017 standard library tests
 *
 * HEADER:
 *    - netinet/in.h
 *
 * TESTED:
 *    - struct ipv6_mreq (ipv6mr_multiaddr, ipv6mr_interface)
 *    - IPV6_JOIN_GROUP, IPV6_LEAVE_GROUP, IPV6_V6ONLY
 *    - IN6_IS_ADDR_MC_* (argument expressions, scope nibble with flag bits set)
 *
 * struct ipv6_mreq was missing, so every IPv6 multicast user carried its own
 * copy. Mostly a compile test: if the struct or a member is absent, this file
 * does not build. On Phoenix the option numbers are pinned as well: libphoenix
 * passes setsockopt()'s optname to lwip unchanged, so they must equal lwip's
 * (lwip/sockets.h) or lwip rejects them.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Phoenix-RTOS RPi4 port
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <assert.h>
#include <netinet/in.h>
#include <stddef.h>
#include <string.h>

#include <unity_fixture.h>


/* POSIX: ipv6mr_multiaddr is a struct in6_addr, ipv6mr_interface an unsigned int */
static_assert(sizeof(((struct ipv6_mreq *)0)->ipv6mr_multiaddr) == sizeof(struct in6_addr), "ipv6mr_multiaddr is an in6_addr");
static_assert(sizeof(((struct ipv6_mreq *)0)->ipv6mr_interface) == sizeof(unsigned int), "ipv6mr_interface is an unsigned int");
/* the layout every libc and lwip share: 16-byte address, then the interface index */
static_assert(offsetof(struct ipv6_mreq, ipv6mr_multiaddr) == 0, "ipv6mr_multiaddr first");
static_assert(offsetof(struct ipv6_mreq, ipv6mr_interface) == 16, "ipv6mr_interface after the address");
static_assert(sizeof(struct ipv6_mreq) == 20, "no padding");


TEST_GROUP(netinet_in_ipv6_mreq);


TEST_SETUP(netinet_in_ipv6_mreq)
{
}


TEST_TEAR_DOWN(netinet_in_ipv6_mreq)
{
}


TEST(netinet_in_ipv6_mreq, members)
{
	struct ipv6_mreq mreq;
	/* ff02::1, all-nodes link-local */
	static const unsigned char group[16] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

	memset(&mreq, 0, sizeof(mreq));
	memcpy(mreq.ipv6mr_multiaddr.s6_addr, group, sizeof(group));
	mreq.ipv6mr_interface = 3U;

	TEST_ASSERT_TRUE(IN6_IS_ADDR_MULTICAST(&mreq.ipv6mr_multiaddr));
	TEST_ASSERT_EQUAL_UINT(3U, mreq.ipv6mr_interface);
	TEST_ASSERT_EQUAL_MEMORY(group, &mreq, sizeof(group));
}


TEST(netinet_in_ipv6_mreq, option_numbers)
{
	/* POSIX requires the two membership options and IPV6_V6ONLY to be distinct */
	TEST_ASSERT_NOT_EQUAL(IPV6_JOIN_GROUP, IPV6_LEAVE_GROUP);
	TEST_ASSERT_NOT_EQUAL(IPV6_JOIN_GROUP, IPV6_V6ONLY);
	TEST_ASSERT_NOT_EQUAL(IPV6_LEAVE_GROUP, IPV6_V6ONLY);
#ifdef __phoenix__
	/* lwip's numbers (lwip/sockets.h), which phoenix-rtos-lwip receives unchanged */
	TEST_ASSERT_EQUAL_INT(12, IPV6_JOIN_GROUP);
	TEST_ASSERT_EQUAL_INT(13, IPV6_LEAVE_GROUP);
	TEST_ASSERT_EQUAL_INT(27, IPV6_V6ONLY);
	TEST_ASSERT_EQUAL_INT(7, IPV6_CHECKSUM);
	TEST_ASSERT_EQUAL_INT(IPV6_JOIN_GROUP, IPV6_ADD_MEMBERSHIP);
	TEST_ASSERT_EQUAL_INT(IPV6_LEAVE_GROUP, IPV6_DROP_MEMBERSHIP);
#endif
}


TEST(netinet_in_ipv6_mreq, mc_scope)
{
	struct {
		int pad;
		struct in6_addr addr;
	} s;

	/* ff02::1: link-local scope, no flags. The argument is an expression, so the
	 * macro must parenthesise it (`&s.addr` failed to compile before). */
	memset(&s, 0, sizeof(s));
	s.addr.s6_addr[0] = 0xff;
	s.addr.s6_addr[1] = 0x02;
	s.addr.s6_addr[15] = 1;
	TEST_ASSERT_TRUE(IN6_IS_ADDR_MC_LINKLOCAL(&s.addr));
	TEST_ASSERT_FALSE(IN6_IS_ADDR_MC_GLOBAL(&s.addr));

	/* ff12::1: the same scope with the T (transient) flag set in the high nibble
	 * (RFC 4291 2.7): still link-local */
	s.addr.s6_addr[1] = 0x12;
	TEST_ASSERT_TRUE(IN6_IS_ADDR_MC_LINKLOCAL(&s.addr));

	/* ff1e::1: global scope with a flag */
	s.addr.s6_addr[1] = 0x1e;
	TEST_ASSERT_TRUE(IN6_IS_ADDR_MC_GLOBAL(&s.addr));
	TEST_ASSERT_FALSE(IN6_IS_ADDR_MC_LINKLOCAL(&s.addr));

	/* not multicast at all */
	s.addr.s6_addr[0] = 0xfe;
	s.addr.s6_addr[1] = 0x80;
	TEST_ASSERT_FALSE(IN6_IS_ADDR_MC_LINKLOCAL(&s.addr));
}


TEST_GROUP_RUNNER(netinet_in_ipv6_mreq)
{
	RUN_TEST_CASE(netinet_in_ipv6_mreq, members);
	RUN_TEST_CASE(netinet_in_ipv6_mreq, option_numbers);
	RUN_TEST_CASE(netinet_in_ipv6_mreq, mc_scope);
}
