// PS5 port host test (2026-10-08, AI-assisted): stands in for pcsx2/Memory.h in GSFunctionMap.cpp: the SW JIT's code region is a
// small buffer of the test's (test_function_map.cpp sets it).
#pragma once
#include "common/Pcsx2Types.h"
namespace SysMemory
{
u8* GetSWRec();
u8* GetSWRecEnd();
}
