// PS5 port host test (2026-10-08, AI-assisted): the SW JIT's function map when its code region runs full (vk-285-136 turned the
// scanline JIT on by default). GSFunctionMap.h and GSFunctionMap.cpp are the real ones; the code generator is a fake that
// "writes" a function of a size chosen per key, and the region is a small buffer.
//
//   ps5/coreorbis/tests/swjit/test-function-map.sh
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "GS/Renderers/Common/GSFunctionMap.h"
#include "Memory.h" // stub/Memory.h

#include <cstdio>
#include <cstring>
#include <vector>

static std::vector<u8> s_region(64 * 1024);
u8* SysMemory::GetSWRec() { return s_region.data(); }
u8* SysMemory::GetSWRecEnd() { return s_region.data() + s_region.size(); }

using Fn = int (*)();

// A function's code is 3000 bytes of its key (the real ones are 1-3 KB; the reserve asks for MAX_SIZE = 8192 each time).
struct FakeCG
{
	u64 key;
	u8* code;
	size_t size = 0;
	FakeCG(u64 k, u8* c, size_t) : key(k), code(c) {}
	void Generate()
	{
		size = 3000;
		std::memset(code, static_cast<int>(key & 0xff), size);
	}
	size_t GetSize() const { return size; }
	void* GetCode() const { return code; }
};

using Map = GSCodeGeneratorFunctionMap<FakeCG, u64, Fn>;

static int s_failed = 0;
static void check(bool ok, const char* what)
{
	std::printf("%s: %s\n", ok ? "ok" : "FAILED", what);
	if (!ok)
		s_failed++;
}

int main()
{
	GSCodeReserve::ResetMemory();
	Map map("test");
	// 64 KiB holds the functions while 8 KiB is left to reserve: (65536 - 8192) / 3000 -> 20 of them.
	std::vector<Fn> got;
	u64 key = 1;
	for (; key <= 64; key++)
	{
		Fn f = map[key];
		if (!f)
			break;
		got.push_back(f);
	}
	check(got.size() == 20, "20 functions fit before the region is full");
	check(key == 21, "the 21st is refused (nullptr) instead of written past the end");
	check(GSCodeReserve::GetMemoryUsed() == 20 * 3000, "nothing was committed for the refused one");
	check(reinterpret_cast<u8*>(got.back()) + 3000 <= SysMemory::GetSWRecEnd(), "the last function ends inside the region");
	check(map[5] == got[4], "a function made earlier is found again");
	check(map[21] == nullptr, "the refused key stays refused until the cache is reset");

	// What GSDrawScanline::ResetCodeCache does.
	map.Clear();
	GSCodeReserve::ResetMemory();
	Fn again = map[21];
	check(again != nullptr, "after the reset the refused key gets its function");
	check(reinterpret_cast<u8*>(again) == SysMemory::GetSWRec(), "at the start of the region");
	check(*reinterpret_cast<u8*>(again) == 21, "with its own code");
	Fn old5 = map[5];
	check(old5 != got[4], "an old key is made again, not taken from before the reset (its old code was thrown away)");
	check(*reinterpret_cast<u8*>(old5) == 5, "and has its own code");
	check(GSCodeReserve::GetMemoryUsed() == 2 * 3000, "two functions in the region after the reset");

	std::printf(s_failed ? "function map: %d FAILED\n" : "function map: all passed\n", s_failed);
	return s_failed ? 1 : 0;
}
