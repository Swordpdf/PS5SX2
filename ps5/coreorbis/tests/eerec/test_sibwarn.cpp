// PS5SX2 (187, AI-assisted): PC test of the emitter's truncation path (common/emitter/x86emitter.cpp, EmitSibMagic):
// an absolute memory operand out of disp32 reach (no text pointer, not rip-reachable, above 2 GiB) now fails an
// assertion (pxFailRel, which aborts on the console; stubs.cpp counts it here) instead of only printing [sibwarn];
// flags/rec_sibwarn (OrbisFlag, defined below as the port defines it) keeps the old log-and-continue; operands the
// emitter can reach (rip-relative, absolute below 2 GiB) never assert.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/emitter/x86emitter.h"

#include <cstdio>
#include <cstring>
#include <sys/mman.h>

using namespace x86Emitter;

extern int g_assert_failures; // stubs.cpp
extern char g_last_assert[512];

static bool s_flag_sibwarn = false;

// The port's flag files (orbis-shims/orbis_paths.cpp); the emitter declares it weak.
bool OrbisFlag(const char* name)
{
	return s_flag_sibwarn && std::strcmp(name, "rec_sibwarn") == 0;
}

int main()
{
	int fail = 0;
	auto expect = [&](bool ok, const char* what) {
		if (!ok)
		{
			fail++;
			std::fprintf(stderr, "FAILED: %s\n", what);
		}
	};
	u8* const buf = static_cast<u8*>(mmap(nullptr, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	if (buf == MAP_FAILED)
		return 1;
	xSetTextPtr(nullptr);
	void* const far = buf + (8ull << 30); // 8 GiB away, above 2 GiB: nothing reaches it with a disp32

	xSetPtr(buf);
	xMOV(eax, ptr32[buf + 4096]); // rip-relative
	xMOV(eax, ptr32[reinterpret_cast<void*>(0x10000)]); // absolute, below 2 GiB
	expect(g_assert_failures == 0, "reachable operands do not assert");

	xMOV(eax, ptr32[far]);
	expect(g_assert_failures == 1, "an out-of-reach operand asserts");
	expect(std::strstr(g_last_assert, "out of disp32 reach") != nullptr, "the assertion names the problem");
	std::printf("assertion text: %s\n", g_last_assert);

	s_flag_sibwarn = true;
	xMOV(ptr32[static_cast<u8*>(far) + 64], eax);
	expect(g_assert_failures == 1, "flags/rec_sibwarn: log and continue, no assertion");

	std::printf("failed %d\n", fail);
	if (fail)
		return 1;
	std::printf("ALL PASSED\n");
	return 0;
}
