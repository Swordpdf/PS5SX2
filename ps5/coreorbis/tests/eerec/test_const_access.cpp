// PS5SX2 (187, AI-assisted): PC test of the EE recompiler's constant-address loads and stores, OrbisConstLoadGPR,
// OrbisConstLoadXMM and OrbisConstStore in pcsx2/x86/ix86-32/recVTLB.cpp (the lines between its 187-VTLB-BEGIN/END
// marks, which test-eerec.sh extracts), emitted by the real emitter and run. For every width and sign and every
// destination or value register (rsp excepted: the allocator never hands it out; rbp, r12 and r13 take the scratch
// path), the old sequence (flags/rec_vtlb_pushpop: push scratch; movabs; access; pop) and the new one run from the
// same registers and memory: the same loaded value or stored bytes, equal to what the access should give; the old one
// changes nothing else, the new one at most its scratch (rax, or rcx when rax is the stored value). With VEX encodings
// (the PS5's) and legacy SSE ones.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/emitter/x86emitter.h"

#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace x86Emitter;

#include "vtlb_const.inc" // OrbisConstLoadGPR/XMM, OrbisConstStore, g_orbis_vtlb_pushpop, extracted from recVTLB.cpp

extern int g_assert_failures; // stubs.cpp

namespace
{
	char g_case[160] = "setup";

	void OnSignal(int sig)
	{
		std::fprintf(stderr, "FAILED: signal %d in %s\n", sig, g_case);
		_exit(1);
	}

	struct alignas(64) Regs
	{
		u64 gpr[16];
		alignas(16) u8 xmm[16][16];
	};
	static_assert(offsetof(Regs, xmm) == 128);

	using HarnessFn = void (*)(Regs*);

	constexpr size_t PAGE = 4096;
	u8* g_code;
	u8* g_cursor;
	u8* g_mem; // the accessed memory (a page above 4 GiB where possible, as the PS5's)
	u8* g_addr; // the address the accesses use (16-byte aligned, for movaps)

	long g_checks = 0;
	int g_fail = 0;

	void Expect(bool ok, const char* what)
	{
		g_checks++;
		if (!ok && g_fail++ < 25)
			std::fprintf(stderr, "FAILED: %s: %s\n", g_case, what);
	}

	// Loads every register but rsp from the Regs at rdi, runs the sequence, stores them all back.
	template <typename Body>
	HarnessFn EmitHarness(const Body& body)
	{
		xSetPtr(g_cursor);
		u8* const fn = xGetPtr();
		for (const xAddressReg& r : {rbx, rbp, r12, r13, r14, r15})
			xPUSH(r);
		xPUSH(rdi);
		xMOV(rax, rdi);
		for (int x = 0; x < 16; x++)
			xMOVDQU(xRegisterSSE(x), ptr[rax + 128 + x * 16]);
		for (int i = 1; i < 16; i++)
		{
			if (i != 4)
				xMOV(xRegister64(i), ptr64[rax + i * 8]);
		}
		xMOV(rax, ptr64[rax]);
		body();
		xPUSH(rax);
		xMOV(rax, ptr64[rsp + 8]);
		for (int i = 1; i < 16; i++)
		{
			if (i != 4)
				xMOV(ptr64[rax + i * 8], xRegister64(i));
		}
		for (int x = 0; x < 16; x++)
			xMOVDQU(ptr[rax + 128 + x * 16], xRegisterSSE(x));
		xPOP(rcx);
		xMOV(ptr64[rax], rcx);
		xPOP(rdi);
		for (const xAddressReg& r : {r15, r14, r13, r12, rbp, rbx})
			xPOP(r);
		xRET();
		g_cursor = reinterpret_cast<u8*>((reinterpret_cast<uptr>(xGetPtr()) + 63) & ~static_cast<uptr>(63));
		return reinterpret_cast<HarnessFn>(fn);
	}

	enum class Kind
	{
		LoadGPR,
		LoadXMM,
		StoreGPR,
		StoreXMM
	};

	struct Case
	{
		Kind kind;
		u32 bits;
		bool sign; // GPR loads
		bool movd; // 32-bit xmm loads: movd (the INTC_STAT path) instead of movss
		int reg; // destination or value register
	};

	void Emit(const Case& c, bool pushpop)
	{
		g_orbis_vtlb_pushpop = pushpop;
		const uptr host = reinterpret_cast<uptr>(g_addr);
		switch (c.kind)
		{
			case Kind::LoadGPR: OrbisConstLoadGPR(c.bits, c.sign, c.reg, host); break;
			case Kind::LoadXMM: OrbisConstLoadXMM(c.bits, c.reg, host, c.movd); break;
			case Kind::StoreGPR: OrbisConstStore(c.bits, false, c.reg, host); break;
			case Kind::StoreXMM: OrbisConstStore(c.bits, true, c.reg, host); break;
		}
	}

	u64 Load(u32 bits, bool sign)
	{
		u64 v = 0;
		std::memcpy(&v, g_addr, bits / 8);
		if (sign)
		{
			const u32 shift = 64 - bits;
			v = static_cast<u64>(static_cast<s64>(v << shift) >> shift);
		}
		return v;
	}

	void FillMemory(std::mt19937_64& rng)
	{
		for (size_t i = 0; i < PAGE; i += 8)
		{
			const u64 v = rng() | 0x8080808080808080ull; // high bits set: sign extension matters
			std::memcpy(g_mem + i, &v, 8);
		}
	}

	void RunCase(const Case& c, std::mt19937_64& rng)
	{
		const HarnessFn old_fn = EmitHarness([&] { Emit(c, true); });
		const HarnessFn new_fn = EmitHarness([&] { Emit(c, false); });

		Regs init;
		for (int i = 0; i < 16; i++)
			init.gpr[i] = rng();
		for (int x = 0; x < 16; x++)
		{
			for (int b = 0; b < 16; b += 8)
			{
				const u64 v = rng();
				std::memcpy(&init.xmm[x][b], &v, 8);
			}
		}
		FillMemory(rng);
		u8 mem0[PAGE];
		std::memcpy(mem0, g_mem, PAGE);

		Regs a = init, b = init;
		old_fn(&a);
		u8 mem_old[PAGE];
		std::memcpy(mem_old, g_mem, PAGE);
		std::memcpy(g_mem, mem0, PAGE);
		new_fn(&b);
		u8 mem_new[PAGE];
		std::memcpy(mem_new, g_mem, PAGE);
		std::memcpy(g_mem, mem0, PAGE);

		// what the access should give
		u8 mem_want[PAGE];
		std::memcpy(mem_want, mem0, PAGE);
		const size_t at = static_cast<size_t>(g_addr - g_mem);
		u32 gpr_dest = 16, xmm_dest = 16;
		u64 gpr_want = 0;
		u8 xmm_want[16] = {};
		switch (c.kind)
		{
			case Kind::LoadGPR:
				gpr_dest = c.reg;
				gpr_want = Load(c.bits, c.sign);
				break;
			case Kind::LoadXMM:
				xmm_dest = c.reg;
				std::memcpy(xmm_want, g_addr, c.bits / 8);
				break;
			case Kind::StoreGPR:
				std::memcpy(mem_want + at, &init.gpr[c.reg], c.bits / 8);
				break;
			case Kind::StoreXMM:
				std::memcpy(mem_want + at, init.xmm[c.reg], c.bits / 8);
				break;
		}

		// the registers the new sequence may change besides its destination
		u32 scratch_new = 0;
		if (c.kind == Kind::LoadGPR)
			scratch_new = ((c.reg & 7) == 4 || (c.reg & 7) == 5) ? 1u << 0 : 0;
		else if (c.kind == Kind::StoreGPR)
			scratch_new = (c.reg == 0) ? 1u << 1 : 1u << 0;
		else
			scratch_new = 1u << 0;

		for (u32 i = 0; i < 16; i++)
		{
			if (i == 4)
				continue;
			if (i == gpr_dest)
			{
				Expect(a.gpr[i] == gpr_want, "old sequence: loaded value");
				Expect(b.gpr[i] == gpr_want, "new sequence: loaded value");
				continue;
			}
			Expect(a.gpr[i] == init.gpr[i], "old sequence changes no other GPR");
			if (!(scratch_new & (1u << i)))
				Expect(b.gpr[i] == init.gpr[i], "new sequence changes no GPR but its scratch");
		}
		for (u32 x = 0; x < 16; x++)
		{
			if (x == xmm_dest)
			{
				Expect(std::memcmp(a.xmm[x], xmm_want, 16) == 0, "old sequence: loaded xmm");
				Expect(std::memcmp(b.xmm[x], xmm_want, 16) == 0, "new sequence: loaded xmm");
				continue;
			}
			Expect(std::memcmp(a.xmm[x], init.xmm[x], 16) == 0 && std::memcmp(b.xmm[x], init.xmm[x], 16) == 0,
				"no other xmm changes");
		}
		Expect(std::memcmp(mem_old, mem_want, PAGE) == 0, "old sequence: memory");
		Expect(std::memcmp(mem_new, mem_want, PAGE) == 0, "new sequence: memory");
	}
} // namespace

int main()
{
	std::signal(SIGSEGV, OnSignal);
	std::signal(SIGBUS, OnSignal);
	std::signal(SIGILL, OnSignal);
	g_code = static_cast<u8*>(mmap(nullptr, 16 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	g_mem = static_cast<u8*>(mmap(reinterpret_cast<void*>(0x610000000ull), PAGE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	if (g_code == MAP_FAILED || g_mem == MAP_FAILED)
	{
		std::fprintf(stderr, "FAILED: mmap\n");
		return 1;
	}
	g_addr = g_mem + 1024;
	std::printf("code %p, memory %p\n", static_cast<void*>(g_code), static_cast<void*>(g_mem));
	std::mt19937_64 rng(187);
	static const char* const names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11",
		"r12", "r13", "r14", "r15"};
	int cases = 0;
	for (bool avx : {true, false})
	{
		x86Emitter::use_avx = avx;
		g_cursor = g_code;
		std::vector<Case> list;
		for (int r = 0; r < 16; r++)
		{
			if (r == 4)
				continue;
			for (u32 bits : {8u, 16u, 32u})
			{
				list.push_back({Kind::LoadGPR, bits, false, false, r});
				list.push_back({Kind::LoadGPR, bits, true, false, r});
			}
			list.push_back({Kind::LoadGPR, 64, false, false, r});
			for (u32 bits : {8u, 16u, 32u, 64u})
				list.push_back({Kind::StoreGPR, bits, false, false, r});
		}
		for (int x = 0; x < 16; x++)
		{
			list.push_back({Kind::LoadXMM, 32, false, false, x});
			list.push_back({Kind::LoadXMM, 32, false, true, x});
			list.push_back({Kind::LoadXMM, 128, false, false, x});
			list.push_back({Kind::StoreXMM, 32, false, false, x});
			list.push_back({Kind::StoreXMM, 128, false, false, x});
		}
		for (const Case& c : list)
		{
			static const char* const kinds[] = {"load", "xmm load", "store", "xmm store"};
			std::snprintf(g_case, sizeof(g_case), "%s, %s %u%s%s, %s%s", avx ? "VEX" : "SSE", kinds[static_cast<int>(c.kind)],
				c.bits, c.sign ? " signed" : "", c.movd ? " movd" : "",
				(c.kind == Kind::LoadXMM || c.kind == Kind::StoreXMM) ? "xmm" : names[c.reg],
				(c.kind == Kind::LoadXMM || c.kind == Kind::StoreXMM) ? std::to_string(c.reg).c_str() : "");
			RunCase(c, rng);
			cases++;
			if (g_cursor > g_code + (15 << 20))
				g_cursor = g_code;
		}
	}
	Expect(g_assert_failures == 0, "no emitter assertion");
	std::printf("cases %d (old and new sequence each), checks %ld, failed %d\n", cases, g_checks, g_fail);
	if (g_fail)
		return 1;
	std::printf("ALL PASSED\n");
	return 0;
}
