// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSFunctionMap.h"
#include "Memory.h"

namespace GSCodeReserve
{
	static u8* s_memory_base;
	static u8* s_memory_end;
	static u8* s_memory_ptr;
}

void GSCodeReserve::ResetMemory()
{
	s_memory_base = SysMemory::GetSWRec();
	s_memory_end = SysMemory::GetSWRecEnd();
	s_memory_ptr = s_memory_base;
}

size_t GSCodeReserve::GetMemoryUsed()
{
	return s_memory_ptr - s_memory_base;
}

u8* GSCodeReserve::ReserveMemory(size_t size)
{
	// PS5 port (2026-10-08, AI-assisted): nullptr when the region is full, so the function map gives up and the rasterizer resets
	// the cache (GSDrawScanline::SetupDraw returns false, then ResetCodeCache) instead of writing code past the region's end. The
	// port's region is 8 MiB (Memory.h SWrecSize), and the scanline JIT is on by default since vk-285-136. Needs proper testing.
	if (s_memory_ptr + size > s_memory_end)
		return nullptr;
	return s_memory_ptr;
}

void GSCodeReserve::CommitMemory(size_t size)
{
	pxAssert((s_memory_ptr + size) <= s_memory_end);
	s_memory_ptr += size;
}
