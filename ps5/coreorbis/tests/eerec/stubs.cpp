// PS5SX2 (187, AI-assisted): what the real emitter (common/emitter) needs from the rest of PCSX2 in the PC tests of
// test-eerec.sh. A failed assertion is counted (and printed) instead of aborting, so test_sibwarn can see one.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/Console.h"

#include <cstdarg>
#include <cstdio>

int g_assert_failures = 0;
char g_last_assert[512];

void pxOnAssertFail(const char* file, int line, const char* func, const char* msg)
{
	g_assert_failures++;
	std::snprintf(g_last_assert, sizeof(g_last_assert), "%s:%d: %s: %s", file, line, func, msg);
	std::fprintf(stderr, "assertion: %s\n", g_last_assert);
}

void Log::Writev(LOGLEVEL level, ConsoleColors color, const char* format, va_list ap)
{
	(void)level;
	(void)color;
	(void)format;
	(void)ap;
}
