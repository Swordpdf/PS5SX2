// PS5SX2 (187, AI-assisted): PC test of the EE recompiler's block check, OrbisSmcCheck in pcsx2/x86/ix86-32/iR5900.cpp
// (the lines between its 187-SMC-BEGIN/END marks, which test-eerec.sh extracts), emitted by the real emitter and run.
// For blocks of 1 to 80 words and longer ones up to the longest (1025 words) and past it, in each of its encodings
// (32-byte VEX as on the PS5, 16-byte VEX, 16-byte legacy SSE), at every 4-byte alignment and against PROT_NONE pages
// on both sides: unchanged code falls through; one changed word anywhere (a random bit) takes the discard path with the
// block's start and size, as the old compare per word does (rebuilt below as iR5900.cpp still emits it with
// flags/rec_smc_perword, and run on the same inputs); the words just outside the block are not checked, and nothing
// outside the block is read.
// "test_smc_check bench": the old check against the new one, in a loop of block entries (PC numbers, not the PS5's).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/emitter/x86emitter.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace x86Emitter;

#include "smc_check.inc" // OrbisSmcCheck, extracted from iR5900.cpp

extern int g_assert_failures; // stubs.cpp

namespace
{
	char g_case[160] = "setup";

	void OnSignal(int sig)
	{
		std::fprintf(stderr, "FAILED: signal %d in %s\n", sig, g_case);
		_exit(1);
	}

	constexpr size_t PAGE = 4096;
	constexpr size_t CODE_SIZE = 4 << 20;
	constexpr size_t RAM_BYTES = 2 * PAGE; // two data pages between two PROT_NONE pages

	u8* g_code;
	u8* g_discard; // DispatchBlockDiscard's stand-in: keeps edi/esi (the block's start and size), returns 1
	u8* g_block_area;
	u32 g_seen[2];
	u8* g_ram;

	enum class Mode
	{
		Ymm,
		Xmm,
		Sse,
		PerWord
	};
	const char* const s_mode_name[] = {"32-byte VEX", "16-byte VEX", "16-byte SSE", "per word"};

	long g_checks = 0;
	int g_fail = 0;

	void Expect(bool ok, const char* what)
	{
		g_checks++;
		if (!ok && g_fail++ < 25)
			std::fprintf(stderr, "FAILED: %s: %s\n", g_case, what);
	}

	void EmitDiscardStub()
	{
		xSetPtr(g_code);
		g_discard = xGetPtr();
		xMOV64(rcx, (sptr)g_seen);
		xMOV(ptr32[rcx], edi);
		xMOV(ptr32[rcx + 4], esi);
		xMOV(eax, 1);
		xRET();
		g_block_area = reinterpret_cast<u8*>((reinterpret_cast<uptr>(xGetPtr()) + 63) & ~static_cast<uptr>(63));
	}

	void SetMode(Mode mode)
	{
		x86Emitter::use_avx = (mode != Mode::Sse);
	}

	// As memory_protect_recompiled_code (ProtMode_Manual) emits it: the discard arguments, rax = the block's code in
	// host memory, then the vector check or, with flags/rec_smc_perword, the compare per word.
	bool EmitCheck(Mode mode, const u32* host, u32 start, u32 nwords)
	{
		xMOV(edi, start);
		xMOV(esi, nwords);
		xMOV64(rax, (sptr)host);
		bool vec = false;
		if (mode != Mode::PerWord && nwords <= OrbisSmcCheck::MAX_WORDS)
		{
			for (u32 w = 0; w < nwords; w++)
				OrbisSmcCheck::s_words[w] = host[w];
			vec = OrbisSmcCheck::Emit(nwords, g_discard, mode == Mode::Ymm);
			if (vec)
			{
				const u32 bytes = nwords * 4;
				const u32 want = (mode == Mode::Ymm) ? bytes / 32 + ((bytes % 32) >= 16 ? 1 : 0) : bytes / 16;
				Expect(OrbisSmcCheck::s_vecs == want, "vector compare count");
			}
			else
			{
				Expect(false, "Emit refused a block it should take");
			}
		}
		if (!vec)
		{
			s32 disp = 0;
			for (u32 w = 0; w < nwords; w++, disp += 4)
			{
				xCMP(ptr32[rax + disp], host[w]);
				xJNE(g_discard);
			}
		}
		return vec;
	}

	using BlockFn = int (*)();

	BlockFn EmitBlock(Mode mode, const u32* host, u32 start, u32 nwords)
	{
		SetMode(mode);
		std::memset(g_block_area, 0xcc, 128 * 1024);
		xSetPtr(g_block_area);
		u8* const fn = xGetPtr();
		EmitCheck(mode, host, start, nwords);
		xMOV(eax, 0); // fell through: the block runs
		xRET();
		OrbisSmcCheck::Finish();
		Expect(OrbisSmcCheck::s_vecs == 0, "Finish left nothing pending");
		return reinterpret_cast<BlockFn>(fn);
	}

	int Run(BlockFn fn)
	{
		g_seen[0] = g_seen[1] = 0xdeadbeefu;
		return fn();
	}

	// One block, both checks (new and old) on the same words and the same changes.
	void TestBlock(Mode mode, u32* host, u32 nwords, std::mt19937& rng)
	{
		for (u32 w = 0; w < nwords; w++)
			host[w] = rng();
		const u32 start = 0x00100000u + static_cast<u32>(reinterpret_cast<u8*>(host) - g_ram);
		const BlockFn fn = EmitBlock(mode, host, start, nwords);
		Expect(Run(fn) == 0, "unchanged code falls through");
		for (u32 p = 0; p < nwords; p++)
		{
			const u32 old = host[p];
			host[p] ^= 1u << (rng() % 32);
			Expect(Run(fn) == 1, "a changed word takes the discard path");
			Expect(g_seen[0] == start && g_seen[1] == nwords, "discard gets the block's start and size");
			host[p] = old;
		}
		Expect(Run(fn) == 0, "restored code falls through");
		if (reinterpret_cast<u8*>(host) > g_ram)
		{
			host[-1] ^= 0x80000000u;
			Expect(Run(fn) == 0, "the word before the block is not checked");
			host[-1] ^= 0x80000000u;
		}
		if (reinterpret_cast<u8*>(host + nwords) < g_ram + RAM_BYTES)
		{
			host[nwords] ^= 1u;
			Expect(Run(fn) == 0, "the word after the block is not checked");
			host[nwords] ^= 1u;
		}
	}

	int Test()
	{
		std::mt19937 rng(187);
		std::vector<u32> sizes;
		for (u32 n = 1; n <= 80; n++)
			sizes.push_back(n);
		for (u32 n : {95u, 96u, 97u, 127u, 128u, 129u, 255u, 256u, 257u, 511u, 512u, 1023u, 1024u, 1025u, 1039u, 1040u})
			sizes.push_back(n);
		long blocks = 0;
		for (Mode mode : {Mode::Ymm, Mode::Xmm, Mode::Sse, Mode::PerWord})
		{
			for (u32 n : sizes)
			{
				// at the end of the data (the next page is PROT_NONE), at its start (so is the one before), and at
				// every 4-byte offset of a 32-byte line in between
				std::vector<u32*> places;
				places.push_back(reinterpret_cast<u32*>(g_ram + RAM_BYTES - n * 4));
				places.push_back(reinterpret_cast<u32*>(g_ram));
				for (u32 k = 1; k < 8; k++)
				{
					if (64 + 4 * k + n * 4 <= RAM_BYTES)
						places.push_back(reinterpret_cast<u32*>(g_ram + 64 + 4 * k));
				}
				for (u32* host : places)
				{
					std::snprintf(g_case, sizeof(g_case), "%s, %u words at RAM+0x%x", s_mode_name[static_cast<int>(mode)], n,
						static_cast<unsigned>(reinterpret_cast<u8*>(host) - g_ram));
					TestBlock(mode, host, n, rng);
					blocks++;
				}
			}
		}

		// Longer than any block: Emit declines and emits nothing.
		std::snprintf(g_case, sizeof(g_case), "1041 words");
		SetMode(Mode::Ymm);
		xSetPtr(g_block_area);
		u8* const before = xGetPtr();
		Expect(!OrbisSmcCheck::Emit(OrbisSmcCheck::MAX_WORDS + 1, g_discard, true), "Emit declines past MAX_WORDS");
		Expect(xGetPtr() == before && OrbisSmcCheck::s_vecs == 0, "nothing emitted past MAX_WORDS");

		// The encoding, once (for the report): an 8-word block, 32-byte compare.
		{
			SetMode(Mode::Ymm);
			u32* host = reinterpret_cast<u32*>(g_ram + 64);
			xSetPtr(g_block_area);
			xMOV64(rax, (sptr)host);
			u8* const s = xGetPtr();
			for (u32 w = 0; w < 8; w++)
				OrbisSmcCheck::s_words[w] = host[w];
			OrbisSmcCheck::Emit(8, g_discard, true);
			std::printf("8-word check (32-byte VEX):");
			for (u8* b = s; b < xGetPtr(); b++)
				std::printf(" %02x", *b);
			std::printf("\n");
			OrbisSmcCheck::Finish();
		}

		Expect(g_assert_failures == 0, "no emitter assertion");
		std::printf("blocks %ld, checks %ld, failed %d\n", blocks, g_checks, g_fail);
		if (g_fail)
			return 1;
		std::printf("ALL PASSED\n");
		return 0;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// bench: a loop of block entries (the discard arguments, the movabs, the check) with no block body.

	using LoopFn = void (*)(u64);

	LoopFn EmitLoop(Mode mode, const u32* host, u32 nwords, bool check, u32* check_bytes)
	{
		SetMode(mode);
		std::memset(g_block_area, 0xcc, 128 * 1024);
		xSetPtr(g_block_area);
		u8* const fn = xGetPtr();
		xMOV(r8, rdi);
		while (reinterpret_cast<uptr>(xGetPtr()) & 31)
			xNOP();
		u8* const loop = xGetPtr();
		u8* const c0 = xGetPtr();
		if (check)
		{
			EmitCheck(mode, host, 0x00100000u, nwords);
		}
		else
		{
			xMOV(edi, 0x00100000u);
			xMOV(esi, nwords);
			xMOV64(rax, (sptr)host);
		}
		if (check_bytes)
			*check_bytes = static_cast<u32>(xGetPtr() - c0);
		xSUB(r8, 1);
		xJNZ(loop);
		xRET();
		OrbisSmcCheck::Finish();
		return reinterpret_cast<LoopFn>(fn);
	}

	double TimeLoop(LoopFn fn, u64 iters)
	{
		double best = 1e30;
		for (int rep = 0; rep < 9; rep++)
		{
			const auto t0 = std::chrono::steady_clock::now();
			fn(iters);
			const auto t1 = std::chrono::steady_clock::now();
			best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(iters));
		}
		return best;
	}

	int Bench()
	{
		std::mt19937 rng(7);
		u32* host = reinterpret_cast<u32*>(g_ram + 68); // a block start that is not 32-byte aligned, as most are
		for (u32 w = 0; w < 1024; w++)
			host[w] = rng();
		const u64 iters = 4000000;
		std::printf("ns per block entry (best of 9 x %llu), check bytes in brackets; check cost = minus 'entry only'\n",
			static_cast<unsigned long long>(iters));
		std::printf("%6s %10s %16s %16s %16s %22s\n", "words", "entry only", "per word (old)", "32-byte (new)", "16-byte",
			"check: old -> new");
		for (u32 n : {1u, 2u, 3u, 4u, 6u, 8u, 12u, 16u, 21u, 32u, 48u, 70u, 128u, 256u})
		{
			u32 bw = 0, by = 0, bx = 0;
			const double base = TimeLoop(EmitLoop(Mode::Ymm, host, n, false, nullptr), iters);
			const double word = TimeLoop(EmitLoop(Mode::PerWord, host, n, true, &bw), iters);
			const double ymm = TimeLoop(EmitLoop(Mode::Ymm, host, n, true, &by), iters);
			const double xmm = TimeLoop(EmitLoop(Mode::Xmm, host, n, true, &bx), iters);
			std::printf("%6u %10.2f %10.2f [%4u] %10.2f [%4u] %10.2f [%4u] %9.2f -> %5.2f ns\n", n, base, word, bw, ymm, by,
				xmm, bx, std::max(0.0, word - base), std::max(0.0, ymm - base));
		}
		return 0;
	}
} // namespace

int main(int argc, char** argv)
{
	std::signal(SIGSEGV, OnSignal);
	std::signal(SIGBUS, OnSignal);
	std::signal(SIGILL, OnSignal);
	g_code = static_cast<u8*>(mmap(nullptr, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	// The RAM stand-in above 4 GiB where the kernel allows (the PS5's is), so rax gets a movabs as on the console.
	u8* const area = static_cast<u8*>(mmap(reinterpret_cast<void*>(0x600000000ull), RAM_BYTES + 2 * PAGE, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	if (g_code == MAP_FAILED || area == MAP_FAILED || mprotect(area + PAGE, RAM_BYTES, PROT_READ | PROT_WRITE) != 0)
	{
		std::fprintf(stderr, "FAILED: mmap\n");
		return 1;
	}
	g_ram = area + PAGE;
	std::printf("code %p, RAM %p (PROT_NONE before and after)\n", static_cast<void*>(g_code), static_cast<void*>(g_ram));
	EmitDiscardStub();
	if (argc > 1 && std::strcmp(argv[1], "bench") == 0)
		return Bench();
	return Test();
}
