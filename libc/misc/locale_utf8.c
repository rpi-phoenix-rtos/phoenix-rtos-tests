/*
 * Phoenix-RTOS
 *
 * libc-tests
 *
 * HEADER:
 *    - locale.h, stdlib.h, wchar.h, uchar.h, langinfo.h, stdio.h
 *
 * TESTED:
 *    - setlocale() with UTF-8 names and "" (LC_ALL, LC_<category>, LANG)
 *    - MB_CUR_MAX and nl_langinfo(CODESET) following LC_CTYPE
 *    - mbrtowc(), wcrtomb(), mbstowcs(), wcstombs(), mbtowc(), wctomb(),
 *      btowc(), wctob(), mbrtoc16() in a UTF-8 locale and in the C locale
 *    - wcwidth(), wcswidth()
 *    - printf("%ls"), printf("%lc")
 *
 * libphoenix's LC_CTYPE is either the single-byte C/POSIX locale (each byte is
 * the code point of the same value) or strict UTF-8. Every other category of a
 * UTF-8 locale is the C locale, so names such as "en_US.UTF-8" are accepted
 * without locale data -- which is why this group is libphoenix-only.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uchar.h>
#include <wchar.h>

#include <unity_fixture.h>


#define ENV_COUNT 4


static const char *const utf8_envNames[ENV_COUNT] = { "LC_ALL", "LC_CTYPE", "LC_NUMERIC", "LANG" };


static struct {
	char locale[512];
	char env[ENV_COUNT][64];
	int envSet[ENV_COUNT];
	mbstate_t st;
} utf8_common;


/* Encodes wc with wcrtomb() and checks the bytes */
static void utf8_assertEncodes(wchar_t wc, const char *bytes)
{
	char out[MB_LEN_MAX];
	size_t len = strlen(bytes);

	memset(&utf8_common.st, 0, sizeof(utf8_common.st));
	TEST_ASSERT_EQUAL_UINT(len, wcrtomb(out, wc, &utf8_common.st));
	TEST_ASSERT_EQUAL_MEMORY(bytes, out, len);
}


/* Decodes bytes with mbrtowc(), whole and then one byte at a time */
static void utf8_assertDecodes(const char *bytes, wchar_t expected)
{
	size_t len = strlen(bytes), i;
	wchar_t wc = 0;

	memset(&utf8_common.st, 0, sizeof(utf8_common.st));
	TEST_ASSERT_EQUAL_UINT(len, mbrtowc(&wc, bytes, len, &utf8_common.st));
	TEST_ASSERT_EQUAL_HEX32(expected, wc);
	TEST_ASSERT_NOT_EQUAL(0, mbsinit(&utf8_common.st));

	wc = 0;
	for (i = 0; i + 1u < len; i++) {
		TEST_ASSERT_EQUAL_UINT((size_t)-2, mbrtowc(&wc, &bytes[i], 1, &utf8_common.st));
		TEST_ASSERT_EQUAL_INT(0, mbsinit(&utf8_common.st));
	}
	TEST_ASSERT_EQUAL_UINT(1u, mbrtowc(&wc, &bytes[len - 1u], 1, &utf8_common.st));
	TEST_ASSERT_EQUAL_HEX32(expected, wc);
	TEST_ASSERT_NOT_EQUAL(0, mbsinit(&utf8_common.st));
}


/* A sequence that must fail with EILSEQ within n bytes */
static void utf8_assertInvalid(const char *bytes, size_t n)
{
	wchar_t wc;

	memset(&utf8_common.st, 0, sizeof(utf8_common.st));
	errno = 0;
	TEST_ASSERT_EQUAL_UINT((size_t)-1, mbrtowc(&wc, bytes, n, &utf8_common.st));
	TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
	TEST_ASSERT_EQUAL_INT(-1, mbtowc(&wc, bytes, n));
}


TEST_GROUP(locale_utf8);


TEST_SETUP(locale_utf8)
{
	const char *val;
	int i;

	strncpy(utf8_common.locale, setlocale(LC_ALL, NULL), sizeof(utf8_common.locale) - 1u);
	for (i = 0; i < ENV_COUNT; i++) {
		val = getenv(utf8_envNames[i]);
		utf8_common.envSet[i] = (val != NULL) ? 1 : 0;
		if (val != NULL) {
			strncpy(utf8_common.env[i], val, sizeof(utf8_common.env[i]) - 1u);
		}
		unsetenv(utf8_envNames[i]);
	}
	TEST_ASSERT_NOT_NULL(setlocale(LC_ALL, "C"));
}


/* the group runs in one process with every other libc/misc group */
TEST_TEAR_DOWN(locale_utf8)
{
	int i;

	for (i = 0; i < ENV_COUNT; i++) {
		if (utf8_common.envSet[i] != 0) {
			setenv(utf8_envNames[i], utf8_common.env[i], 1);
		}
		else {
			unsetenv(utf8_envNames[i]);
		}
	}
	setlocale(LC_ALL, utf8_common.locale);
}


TEST(locale_utf8, setlocale_names)
{
	static const char *const good[] = {
		"C.UTF-8", "C.utf8", "POSIX.UTF-8", "en_US.UTF-8", "pl_PL.utf8", "sr_RS.UTF-8@latin", "de_DE.Utf-8",
	};
	static const char *const bad[] = {
		"en_US", "en_US.ISO-8859-1", "xx_YY.NOSUCH", "C.UTF-16", "C.UTF8x", "../x.UTF-8", ".UTF-8", "en_.UTF-8",
		"a_very_long_language_name_XX.UTF-8",
	};
	size_t i;

	for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
		TEST_ASSERT_EQUAL_STRING(good[i], setlocale(LC_ALL, good[i]));
		TEST_ASSERT_EQUAL_STRING(good[i], setlocale(LC_CTYPE, NULL));
		TEST_ASSERT_EQUAL_STRING(good[i], setlocale(LC_MESSAGES, NULL));
	}

	/* a refused name changes nothing */
	TEST_ASSERT_NOT_NULL(setlocale(LC_ALL, "C.UTF-8"));
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		TEST_ASSERT_NULL(setlocale(LC_ALL, bad[i]));
		TEST_ASSERT_NULL(setlocale(LC_CTYPE, bad[i]));
		TEST_ASSERT_EQUAL_STRING("C.UTF-8", setlocale(LC_ALL, NULL));
	}

	TEST_ASSERT_EQUAL_STRING("C", setlocale(LC_ALL, "C"));
	TEST_ASSERT_EQUAL_STRING("POSIX", setlocale(LC_ALL, "POSIX"));
	TEST_ASSERT_NULL(setlocale(12345, "C"));
}


/* setlocale(LC_ALL, NULL) of mixed categories goes back into setlocale() */
TEST(locale_utf8, setlocale_mixed_round_trip)
{
	char saved[512];

	TEST_ASSERT_NOT_NULL(setlocale(LC_ALL, "C"));
	TEST_ASSERT_EQUAL_STRING("C.UTF-8", setlocale(LC_CTYPE, "C.UTF-8"));
	strncpy(saved, setlocale(LC_ALL, NULL), sizeof(saved) - 1u);
	saved[sizeof(saved) - 1u] = '\0';
	TEST_ASSERT_NOT_NULL(strstr(saved, "LC_CTYPE=C.UTF-8"));
	TEST_ASSERT_NOT_NULL(strstr(saved, "LC_NUMERIC=C"));

	TEST_ASSERT_NOT_NULL(setlocale(LC_ALL, "C"));
	TEST_ASSERT_EQUAL_UINT(1u, MB_CUR_MAX);
	TEST_ASSERT_EQUAL_STRING(saved, setlocale(LC_ALL, saved));
	TEST_ASSERT_EQUAL_STRING("C.UTF-8", setlocale(LC_CTYPE, NULL));
	TEST_ASSERT_EQUAL_STRING("C", setlocale(LC_TIME, NULL));
	TEST_ASSERT_EQUAL_UINT(4u, MB_CUR_MAX);

	TEST_ASSERT_NULL(setlocale(LC_ALL, "LC_CTYPE=C.UTF-8;LC_BOGUS=C"));
	TEST_ASSERT_NULL(setlocale(LC_ALL, "LC_CTYPE=en_US"));
	TEST_ASSERT_EQUAL_STRING(saved, setlocale(LC_ALL, NULL));
}


/* "" resolves through LC_ALL, then LC_<category>, then LANG (POSIX XBD 8.2) */
TEST(locale_utf8, setlocale_environment)
{
	TEST_ASSERT_EQUAL_STRING("POSIX", setlocale(LC_ALL, ""));

	TEST_ASSERT_EQUAL_INT(0, setenv("LANG", "pl_PL.UTF-8", 1));
	TEST_ASSERT_EQUAL_STRING("pl_PL.UTF-8", setlocale(LC_ALL, ""));
	TEST_ASSERT_EQUAL_UINT(4u, MB_CUR_MAX);

	TEST_ASSERT_EQUAL_INT(0, setenv("LC_CTYPE", "C", 1));
	TEST_ASSERT_NOT_NULL(setlocale(LC_ALL, ""));
	TEST_ASSERT_EQUAL_STRING("C", setlocale(LC_CTYPE, NULL));
	TEST_ASSERT_EQUAL_STRING("pl_PL.UTF-8", setlocale(LC_NUMERIC, NULL));
	TEST_ASSERT_EQUAL_UINT(1u, MB_CUR_MAX);

	TEST_ASSERT_EQUAL_INT(0, setenv("LC_ALL", "C.UTF-8", 1));
	TEST_ASSERT_EQUAL_STRING("C.UTF-8", setlocale(LC_ALL, ""));
	TEST_ASSERT_EQUAL_UINT(4u, MB_CUR_MAX);

	/* an unknown name in the environment fails like any other */
	TEST_ASSERT_EQUAL_INT(0, setenv("LC_ALL", "xx_YY.NOSUCH", 1));
	TEST_ASSERT_NULL(setlocale(LC_ALL, ""));
	TEST_ASSERT_EQUAL_STRING("C.UTF-8", setlocale(LC_ALL, NULL));
}


TEST(locale_utf8, mb_cur_max_and_codeset)
{
	TEST_ASSERT_EQUAL_UINT(1u, MB_CUR_MAX);
	TEST_ASSERT_EQUAL_STRING("ANSI_X3.4-1968", nl_langinfo(CODESET));
	TEST_ASSERT_GREATER_OR_EQUAL_INT(4, MB_LEN_MAX);

	/* only LC_CTYPE decides the encoding */
	TEST_ASSERT_NOT_NULL(setlocale(LC_NUMERIC, "C.UTF-8"));
	TEST_ASSERT_EQUAL_UINT(1u, MB_CUR_MAX);

	TEST_ASSERT_NOT_NULL(setlocale(LC_CTYPE, "C.UTF-8"));
	TEST_ASSERT_EQUAL_UINT(4u, MB_CUR_MAX);
	TEST_ASSERT_EQUAL_STRING("UTF-8", nl_langinfo(CODESET));
	TEST_ASSERT_EQUAL_STRING("Sunday", nl_langinfo(DAY_1)); /* the rest is C */

	TEST_ASSERT_NOT_NULL(setlocale(LC_ALL, "POSIX"));
	TEST_ASSERT_EQUAL_UINT(1u, MB_CUR_MAX);
	TEST_ASSERT_EQUAL_STRING("ANSI_X3.4-1968", nl_langinfo(CODESET));
}


TEST(locale_utf8, round_trip)
{
	wchar_t wout[8];
	char out[32];

	TEST_ASSERT_NOT_NULL(setlocale(LC_CTYPE, "C.UTF-8"));

	utf8_assertEncodes(L'A', "A");
	utf8_assertEncodes(0xe9, "\xc3\xa9");
	utf8_assertEncodes(0x20ac, "\xe2\x82\xac");
	utf8_assertEncodes(0x4f60, "\xe4\xbd\xa0");
	utf8_assertEncodes(0x1f600, "\xf0\x9f\x98\x80");
	utf8_assertEncodes(0x10ffff, "\xf4\x8f\xbf\xbf");

	utf8_assertDecodes("\xc3\xa9", 0xe9);
	utf8_assertDecodes("\xe2\x82\xac", 0x20ac);
	utf8_assertDecodes("\xf0\x9f\x98\x80", 0x1f600);
	utf8_assertDecodes("\xf4\x8f\xbf\xbf", 0x10ffff);

	TEST_ASSERT_EQUAL_UINT(4u, mbstowcs(wout, "a\xc3\xa9\xe4\xbd\xa0\xf0\x9f\x98\x80", 8));
	TEST_ASSERT_EQUAL_HEX32(0x1f600, wout[3]);
	TEST_ASSERT_EQUAL_HEX32(0, wout[4]);
	TEST_ASSERT_EQUAL_UINT(4u, mbstowcs(NULL, "a\xc3\xa9\xe4\xbd\xa0\xf0\x9f\x98\x80", 0));

	TEST_ASSERT_EQUAL_UINT(10u, wcstombs(out, wout, sizeof(out)));
	TEST_ASSERT_EQUAL_STRING("a\xc3\xa9\xe4\xbd\xa0\xf0\x9f\x98\x80", out);
	TEST_ASSERT_EQUAL_UINT(10u, wcstombs(NULL, wout, 0));
	/* a character that does not fit whole is not written */
	memset(out, '#', sizeof(out));
	TEST_ASSERT_EQUAL_UINT(3u, wcstombs(out, wout, 5));
	TEST_ASSERT_EQUAL_CHAR('#', out[3]);

	TEST_ASSERT_EQUAL_INT(3, wctomb(out, 0x20ac));
	TEST_ASSERT_EQUAL_INT(0, wctomb(NULL, 0)); /* no shift states */
	TEST_ASSERT_EQUAL_INT(2, mblen("\xc3\xa9", 2));
}


TEST(locale_utf8, invalid_sequences)
{
	char out[MB_LEN_MAX];
	wchar_t wc;

	TEST_ASSERT_NOT_NULL(setlocale(LC_CTYPE, "C.UTF-8"));

	utf8_assertInvalid("\x80", 1);             /* continuation without a lead */
	utf8_assertInvalid("\xc0\x80", 2);         /* overlong NUL */
	utf8_assertInvalid("\xc1\xbf", 2);         /* overlong */
	utf8_assertInvalid("\xe0\x80\x80", 3);     /* overlong */
	utf8_assertInvalid("\xf0\x80\x80\x80", 4); /* overlong */
	utf8_assertInvalid("\xed\xa0\x80", 3);     /* surrogate U+D800 */
	utf8_assertInvalid("\xf4\x90\x80\x80", 4); /* U+110000 */
	utf8_assertInvalid("\xf5\x80\x80\x80", 4); /* lead beyond U+10FFFF */
	utf8_assertInvalid("\xff", 1);
	utf8_assertInvalid("\xc3\x28", 2);         /* lead followed by ASCII */
	utf8_assertInvalid("\xe4\xbd", 3);         /* cut short by the terminator */

	/* rejected at the first byte that cannot continue a valid character */
	utf8_assertInvalid("\xed\xa0", 2);
	utf8_assertInvalid("\xe0\x9f", 2);

	/* a valid but incomplete prefix is (size_t)-2, and the state remembers it */
	memset(&utf8_common.st, 0, sizeof(utf8_common.st));
	TEST_ASSERT_EQUAL_UINT((size_t)-2, mbrtowc(&wc, "\xe4\xbd", 2, &utf8_common.st));
	TEST_ASSERT_EQUAL_INT(0, mbsinit(&utf8_common.st));
	errno = 0;
	TEST_ASSERT_EQUAL_UINT((size_t)-1, mbrtowc(NULL, NULL, 0, &utf8_common.st));
	TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
	/* incomplete is an error for mbtowc(), which has no state to keep it in */
	TEST_ASSERT_EQUAL_INT(-1, mbtowc(&wc, "\xe4\xbd", 2));

	memset(&utf8_common.st, 0, sizeof(utf8_common.st));
	errno = 0;
	TEST_ASSERT_EQUAL_UINT((size_t)-1, wcrtomb(out, 0xd800, &utf8_common.st));
	TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
	errno = 0;
	TEST_ASSERT_EQUAL_UINT((size_t)-1, wcrtomb(out, 0x110000, &utf8_common.st));
	TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
	TEST_ASSERT_EQUAL_UINT((size_t)-1, mbstowcs(NULL, "ok\xed\xa0\x80", 0));
}


TEST(locale_utf8, single_bytes_and_char16)
{
	char16_t c16[2] = { 0, 0 };

	/* the C locale: every byte is a character, Latin-1 */
	TEST_ASSERT_EQUAL_HEX32(0xe9, btowc(0xe9));
	TEST_ASSERT_EQUAL_INT(0xe9, wctob(0xe9));
	TEST_ASSERT_EQUAL_HEX32(WEOF, btowc(EOF));

	TEST_ASSERT_NOT_NULL(setlocale(LC_CTYPE, "C.UTF-8"));
	TEST_ASSERT_EQUAL_HEX32('A', btowc('A'));
	TEST_ASSERT_EQUAL_HEX32(WEOF, btowc(0xe9));
	TEST_ASSERT_EQUAL_INT(EOF, wctob(0xe9));
	TEST_ASSERT_EQUAL_INT('A', wctob(L'A'));

	/* above U+FFFF, char16_t takes a surrogate pair */
	memset(&utf8_common.st, 0, sizeof(utf8_common.st));
	TEST_ASSERT_EQUAL_UINT(4u, mbrtoc16(&c16[0], "\xf0\x9f\x98\x80", 4, &utf8_common.st));
	TEST_ASSERT_EQUAL_UINT((size_t)-3, mbrtoc16(&c16[1], "", 1, &utf8_common.st));
	TEST_ASSERT_EQUAL_HEX16(0xd83d, c16[0]);
	TEST_ASSERT_EQUAL_HEX16(0xde00, c16[1]);
}


TEST(locale_utf8, c_locale_unchanged)
{
	char out[MB_LEN_MAX];
	wchar_t wc = 0;

	/* after a UTF-8 locale, the C locale is single-byte again */
	TEST_ASSERT_NOT_NULL(setlocale(LC_CTYPE, "C.UTF-8"));
	TEST_ASSERT_NOT_NULL(setlocale(LC_CTYPE, "C"));

	memset(&utf8_common.st, 0, sizeof(utf8_common.st));
	TEST_ASSERT_EQUAL_UINT(1u, mbrtowc(&wc, "\xc3\xa9", 2, &utf8_common.st));
	TEST_ASSERT_EQUAL_HEX32(0xc3, wc);
	TEST_ASSERT_EQUAL_UINT(1u, wcrtomb(out, 0xe9, &utf8_common.st));
	TEST_ASSERT_EQUAL_HEX8(0xe9, (unsigned char)out[0]);
	errno = 0;
	TEST_ASSERT_EQUAL_UINT((size_t)-1, wcrtomb(out, 0x4f60, &utf8_common.st));
	TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
	TEST_ASSERT_EQUAL_UINT(6u, mbstowcs(NULL, "\xe4\xbd\xa0\xe5\xa5\xbd", 0));
}


TEST(locale_utf8, wcwidth)
{
	/* the C locale: everything printable is one column */
	TEST_ASSERT_EQUAL_INT(1, wcwidth(0x4f60));
	TEST_ASSERT_EQUAL_INT(-1, wcwidth(0x07));

	TEST_ASSERT_NOT_NULL(setlocale(LC_CTYPE, "C.UTF-8"));
	TEST_ASSERT_EQUAL_INT(0, wcwidth(0));
	TEST_ASSERT_EQUAL_INT(1, wcwidth(L'a'));
	TEST_ASSERT_EQUAL_INT(1, wcwidth(0xe9));
	TEST_ASSERT_EQUAL_INT(1, wcwidth(0xad));    /* SOFT HYPHEN */
	TEST_ASSERT_EQUAL_INT(0, wcwidth(0x301));   /* COMBINING ACUTE ACCENT */
	TEST_ASSERT_EQUAL_INT(0, wcwidth(0x200b));  /* ZERO WIDTH SPACE */
	TEST_ASSERT_EQUAL_INT(0, wcwidth(0x1160));  /* Hangul medial vowel filler */
	TEST_ASSERT_EQUAL_INT(2, wcwidth(0x4f60));  /* CJK */
	TEST_ASSERT_EQUAL_INT(2, wcwidth(0xff21));  /* FULLWIDTH A */
	TEST_ASSERT_EQUAL_INT(2, wcwidth(0x1f600)); /* emoji */
	TEST_ASSERT_EQUAL_INT(1, wcwidth(0xe000));  /* private use */
	TEST_ASSERT_EQUAL_INT(-1, wcwidth(0x07));
	TEST_ASSERT_EQUAL_INT(-1, wcwidth(0x85));
	TEST_ASSERT_EQUAL_INT(-1, wcwidth(0x378));  /* unassigned */
	TEST_ASSERT_EQUAL_INT(-1, wcwidth(0xd800)); /* surrogate */
	TEST_ASSERT_EQUAL_INT(-1, wcwidth(0x110000));

	TEST_ASSERT_EQUAL_INT(4, wcswidth(L"a\x4f60\x301z", 8));
	TEST_ASSERT_EQUAL_INT(3, wcswidth(L"a\x4f60\x301z", 3));
	TEST_ASSERT_EQUAL_INT(-1, wcswidth(L"a\x07", 8));
}


TEST(locale_utf8, printf_wide)
{
	static const wchar_t cafe[] = { L'c', L'a', L'f', 0xe9, 0 };
	static const wchar_t nihao[] = { 0x4f60, 0x597d, 0 };
	static const wchar_t smile[] = { 0x1f600, 0 };
	char buf[32];

	/* the C locale: Latin-1 below U+0100, an encoding error above */
	TEST_ASSERT_EQUAL_INT(4, snprintf(buf, sizeof(buf), "%ls", cafe));
	TEST_ASSERT_EQUAL_STRING("caf\xe9", buf);
	TEST_ASSERT_EQUAL_INT(-1, snprintf(buf, sizeof(buf), "%ls", smile));

	TEST_ASSERT_NOT_NULL(setlocale(LC_CTYPE, "C.UTF-8"));
	TEST_ASSERT_EQUAL_INT(5, snprintf(buf, sizeof(buf), "%ls", cafe));
	TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", buf);
	TEST_ASSERT_EQUAL_INT(4, snprintf(buf, sizeof(buf), "%ls", smile));
	TEST_ASSERT_EQUAL_STRING("\xf0\x9f\x98\x80", buf);
	TEST_ASSERT_EQUAL_INT(3, snprintf(buf, sizeof(buf), "%lc", (wint_t)0x20ac));
	TEST_ASSERT_EQUAL_STRING("\xe2\x82\xac", buf);

	/* precision counts bytes and never splits a character */
	TEST_ASSERT_EQUAL_INT(3, snprintf(buf, sizeof(buf), "%.5ls", nihao));
	TEST_ASSERT_EQUAL_STRING("\xe4\xbd\xa0", buf);
	/* field width counts bytes, too */
	TEST_ASSERT_EQUAL_INT(8, snprintf(buf, sizeof(buf), "%8ls", nihao));
	TEST_ASSERT_EQUAL_STRING("  \xe4\xbd\xa0\xe5\xa5\xbd", buf);
}


TEST_GROUP_RUNNER(locale_utf8)
{
	RUN_TEST_CASE(locale_utf8, setlocale_names);
	RUN_TEST_CASE(locale_utf8, setlocale_mixed_round_trip);
	RUN_TEST_CASE(locale_utf8, setlocale_environment);
	RUN_TEST_CASE(locale_utf8, mb_cur_max_and_codeset);
	RUN_TEST_CASE(locale_utf8, round_trip);
	RUN_TEST_CASE(locale_utf8, invalid_sequences);
	RUN_TEST_CASE(locale_utf8, single_bytes_and_char16);
	RUN_TEST_CASE(locale_utf8, c_locale_unchanged);
	RUN_TEST_CASE(locale_utf8, wcwidth);
	RUN_TEST_CASE(locale_utf8, printf_wide);
}
