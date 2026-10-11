// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// PS5 port (vk-285-98): an inline copy for the small blocks the EE and VU threads move per VU1 program
// (VIF1 unpack data into the MTVU ring, GIF packet data into a path's buffer). The system memcpy lives in
// another module, so each call is a far jump into code the instruction TLB and caches have usually dropped
// by then (the EE thread runs the game's recompiled code in between): vk-285-97's Shadow of the Colossus EE
// profile had ~2% of the thread in memcpy, a good part of it on its first instructions. Blocks over 1 KB
// still take memcpy. Needs proper testing.

#include "common/Pcsx2Defs.h"

#include <cstring>
#include <immintrin.h>

static __fi void OrbisCopy(void* dst, const void* src, size_t n)
{
	if (n > 1024)
	{
		std::memcpy(dst, src, n);
		return;
	}
	u8* d = static_cast<u8*>(dst);
	const u8* s = static_cast<const u8*>(src);
	while (n >= 32)
	{
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(d), _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s)));
		d += 32;
		s += 32;
		n -= 32;
	}
	if (n >= 16)
	{
		_mm_storeu_si128(reinterpret_cast<__m128i*>(d), _mm_loadu_si128(reinterpret_cast<const __m128i*>(s)));
		d += 16;
		s += 16;
		n -= 16;
	}
	if (n >= 8)
	{
		u64 v;
		std::memcpy(&v, s, 8);
		std::memcpy(d, &v, 8);
		d += 8;
		s += 8;
		n -= 8;
	}
	if (n >= 4)
	{
		u32 v;
		std::memcpy(&v, s, 4);
		std::memcpy(d, &v, 4);
		d += 4;
		s += 4;
		n -= 4;
	}
	switch (n)
	{
		case 3:
			d[2] = s[2];
			[[fallthrough]];
		case 2:
			d[1] = s[1];
			[[fallthrough]];
		case 1:
			d[0] = s[0];
			break;
		default:
			break;
	}
}
