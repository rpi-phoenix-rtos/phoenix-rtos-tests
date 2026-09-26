/*
 * Phoenix-RTOS
 *
 * C99 standard library tests
 *
 * HEADER:
 *    - inttypes.h
 *
 * TESTED:
 *    - the PRI and SCN format macros, including SCN*PTR, SCN*MAX and PRI*FAST*
 *    - imaxabs(), imaxdiv()
 *    - strtoimax(), strtoumax(), wcstoimax(), wcstoumax()
 *
 * libphoenix's <inttypes.h> stopped short of C99: SCNxPTR/SCNuPTR (which Mesa's
 * nir_opt_varyings.c uses, failing -Werror=format with a "spurious trailing
 * '%'"), every SCN*MAX, every PRI*FAST* and the imax* / wcsto*max functions were
 * missing. printf/sscanf carry the format attribute, so the -Wformat -Werror
 * build of this file is itself the check that each macro's length modifier
 * matches its type; the runtime cases check the values round-trip.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <unity_fixture.h>


TEST_GROUP(stdlib_inttypes);


TEST_SETUP(stdlib_inttypes)
{
}


TEST_TEAR_DOWN(stdlib_inttypes)
{
}


TEST(stdlib_inttypes, scn_ptr_roundtrip)
{
	char buf[64];
	int x = 0;
	uintptr_t in = (uintptr_t)&x, out = 0;
	intptr_t sin = -12345, sout = 0;

	snprintf(buf, sizeof(buf), "%" PRIxPTR, in);
	TEST_ASSERT_EQUAL_INT(1, sscanf(buf, "%" SCNxPTR, &out));
	TEST_ASSERT_TRUE(in == out);

	out = 0;
	snprintf(buf, sizeof(buf), "%" PRIuPTR, in);
	TEST_ASSERT_EQUAL_INT(1, sscanf(buf, "%" SCNuPTR, &out));
	TEST_ASSERT_TRUE(in == out);

	out = 0;
	snprintf(buf, sizeof(buf), "%" PRIoPTR, in);
	TEST_ASSERT_EQUAL_INT(1, sscanf(buf, "%" SCNoPTR, &out));
	TEST_ASSERT_TRUE(in == out);

	snprintf(buf, sizeof(buf), "%" PRIdPTR, sin);
	TEST_ASSERT_EQUAL_INT(1, sscanf(buf, "%" SCNdPTR, &sout));
	TEST_ASSERT_TRUE(sin == sout);

	sout = 0;
	TEST_ASSERT_EQUAL_INT(1, sscanf("-0x10", "%" SCNiPTR, &sout));
	TEST_ASSERT_TRUE(sout == -16);

	/* The widest pointer value must survive: a missing "l" on a 64-bit target
	 * would store only 32 bits (or overrun, depending on the libc). */
	out = 0;
	snprintf(buf, sizeof(buf), "%" PRIxPTR, (uintptr_t)UINTPTR_MAX);
	TEST_ASSERT_EQUAL_INT(1, sscanf(buf, "%" SCNxPTR, &out));
	TEST_ASSERT_TRUE(out == UINTPTR_MAX);
}


TEST(stdlib_inttypes, scn_max_extremes)
{
	char buf[64];
	intmax_t s = 0;
	uintmax_t u = 0;

	snprintf(buf, sizeof(buf), "%" PRIdMAX, (intmax_t)INTMAX_MIN);
	TEST_ASSERT_EQUAL_INT(1, sscanf(buf, "%" SCNdMAX, &s));
	TEST_ASSERT_TRUE(s == INTMAX_MIN);

	snprintf(buf, sizeof(buf), "%" PRIiMAX, (intmax_t)INTMAX_MAX);
	TEST_ASSERT_EQUAL_INT(1, sscanf(buf, "%" SCNiMAX, &s));
	TEST_ASSERT_TRUE(s == INTMAX_MAX);

	snprintf(buf, sizeof(buf), "%" PRIuMAX, (uintmax_t)UINTMAX_MAX);
	TEST_ASSERT_EQUAL_INT(1, sscanf(buf, "%" SCNuMAX, &u));
	TEST_ASSERT_TRUE(u == UINTMAX_MAX);

	u = 0;
	snprintf(buf, sizeof(buf), "%" PRIxMAX, (uintmax_t)UINTMAX_MAX);
	TEST_ASSERT_EQUAL_STRING("ffffffffffffffff", buf);
	TEST_ASSERT_EQUAL_INT(1, sscanf(buf, "%" SCNxMAX, &u));
	TEST_ASSERT_TRUE(u == UINTMAX_MAX);

	u = 0;
	TEST_ASSERT_EQUAL_INT(1, sscanf("1777777777777777777777", "%" SCNoMAX, &u));
	TEST_ASSERT_TRUE(u == UINTMAX_MAX);
}


TEST(stdlib_inttypes, pri_fast)
{
	char buf[128];
	int_fast8_t f8 = INT8_MIN;
	int_fast16_t f16 = INT16_MIN;
	int_fast32_t f32 = INT32_MIN;
	int_fast64_t f64 = INT64_MIN;
	uint_fast8_t u8 = UINT8_MAX;
	uint_fast16_t u16 = UINT16_MAX;
	uint_fast32_t u32 = UINT32_MAX;
	uint_fast64_t u64 = UINT64_MAX;

	snprintf(buf, sizeof(buf), "%" PRIdFAST8 " %" PRIdFAST16 " %" PRIdFAST32 " %" PRIdFAST64, f8, f16, f32, f64);
	TEST_ASSERT_EQUAL_STRING("-128 -32768 -2147483648 -9223372036854775808", buf);

	snprintf(buf, sizeof(buf), "%" PRIuFAST8 " %" PRIuFAST16 " %" PRIuFAST32 " %" PRIuFAST64, u8, u16, u32, u64);
	TEST_ASSERT_EQUAL_STRING("255 65535 4294967295 18446744073709551615", buf);

	snprintf(buf, sizeof(buf), "%" PRIxFAST8 " %" PRIXFAST16 " %" PRIoFAST32 " %" PRIiFAST64, u8, u16, u32, f64);
	TEST_ASSERT_EQUAL_STRING("ff FFFF 37777777777 -9223372036854775808", buf);

	/* ...and the SCN side of the same types, which already existed. */
	f32 = 0;
	TEST_ASSERT_EQUAL_INT(1, sscanf("-7", "%" SCNdFAST32, &f32));
	TEST_ASSERT_TRUE(f32 == -7);
}


TEST(stdlib_inttypes, imaxabs_imaxdiv)
{
	imaxdiv_t d;

	TEST_ASSERT_TRUE(imaxabs(0) == 0);
	TEST_ASSERT_TRUE(imaxabs(-5) == 5);
	TEST_ASSERT_TRUE(imaxabs(INTMAX_MAX) == INTMAX_MAX);
	TEST_ASSERT_TRUE(imaxabs(-INTMAX_MAX) == INTMAX_MAX);

	/* C99 truncation toward zero, remainder takes the sign of the dividend */
	d = imaxdiv(7, 2);
	TEST_ASSERT_TRUE((d.quot == 3) && (d.rem == 1));
	d = imaxdiv(-7, 2);
	TEST_ASSERT_TRUE((d.quot == -3) && (d.rem == -1));
	d = imaxdiv(7, -2);
	TEST_ASSERT_TRUE((d.quot == -3) && (d.rem == 1));
	d = imaxdiv(-7, -2);
	TEST_ASSERT_TRUE((d.quot == 3) && (d.rem == -1));
	d = imaxdiv(INTMAX_MIN, 1);
	TEST_ASSERT_TRUE((d.quot == INTMAX_MIN) && (d.rem == 0));
	d = imaxdiv(INTMAX_MAX, 10);
	TEST_ASSERT_TRUE(d.quot * 10 + d.rem == INTMAX_MAX);
}


TEST(stdlib_inttypes, strto_wcsto_max)
{
	char *end;
	wchar_t *wend;
	const wchar_t *wmin = L"-9223372036854775808 tail";
	const wchar_t *wmax = L"0xffffffffffffffffZ";

	TEST_ASSERT_TRUE(strtoimax("-9223372036854775808", &end, 10) == INTMAX_MIN);
	TEST_ASSERT_EQUAL_CHAR('\0', *end);
	TEST_ASSERT_TRUE(strtoumax("18446744073709551615", &end, 10) == UINTMAX_MAX);

	TEST_ASSERT_TRUE(wcstoimax(wmin, &wend, 10) == INTMAX_MIN);
	TEST_ASSERT_TRUE(wend == wmin + 20);
	TEST_ASSERT_TRUE(wcstoumax(wmax, &wend, 16) == UINTMAX_MAX);
	TEST_ASSERT_TRUE(wend == wmax + 18);
	TEST_ASSERT_TRUE(wcstoimax(L"  +42", NULL, 0) == 42);

	/* No digits: value 0 and endptr back at the start. */
	TEST_ASSERT_TRUE(wcstoumax(wmin + 21, &wend, 10) == 0);
	TEST_ASSERT_TRUE(wend == wmin + 21);
}


TEST_GROUP_RUNNER(stdlib_inttypes)
{
	RUN_TEST_CASE(stdlib_inttypes, scn_ptr_roundtrip);
	RUN_TEST_CASE(stdlib_inttypes, scn_max_extremes);
	RUN_TEST_CASE(stdlib_inttypes, pri_fast);
	RUN_TEST_CASE(stdlib_inttypes, imaxabs_imaxdiv);
	RUN_TEST_CASE(stdlib_inttypes, strto_wcsto_max);
}
