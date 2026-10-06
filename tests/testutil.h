/* ============================================================================
 * testutil.h  --  a minimal unit-test harness (standard library only)
 *
 * CHECK(cond)           record a pass or a failure with file:line
 * CHECK_INT(got, want)  compare integers, print both on failure
 * CHECK_STR(got, want)  compare strings
 * TEST_DONE()           print the totals; return from main with the status
 * ==========================================================================*/
#ifndef NEMOC_TESTUTIL_H
#define NEMOC_TESTUTIL_H

#include <cstdio>
#include <cstring>
#include <string>

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond)                                                          \
	do {                                                                    \
		g_checks++;                                                         \
		if (!(cond)) {                                                      \
			g_fails++;                                                      \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
		}                                                                   \
	} while (0)

#define CHECK_INT(got, want)                                                 \
	do {                                                                    \
		long _g = (long)(got), _w = (long)(want);                           \
		g_checks++;                                                         \
		if (_g != _w) {                                                     \
			g_fails++;                                                      \
			printf("  FAIL %s:%d: %s = %ld, expected %ld\n", __FILE__,      \
			       __LINE__, #got, _g, _w);                                 \
		}                                                                   \
	} while (0)

#define CHECK_STR(got, want)                                                 \
	do {                                                                    \
		std::string _g = (got), _w = (want);                                \
		g_checks++;                                                         \
		if (_g != _w) {                                                     \
			g_fails++;                                                      \
			printf("  FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__,\
			       __LINE__, #got, _g.c_str(), _w.c_str());                 \
		}                                                                   \
	} while (0)

#define TEST_DONE()                                                          \
	do {                                                                    \
		printf("  %d checks, %d failed\n", g_checks, g_fails);              \
		return g_fails ? 1 : 0;                                             \
	} while (0)

#endif
