// PS5SX2 (187, AI-assisted): PC test of the EE's GetThreadId cache (pcsx2/OrbisEEHle.h), and of OrbisTidInvalidate, which cpuReset and a
// savestate load (SaveState.cpp PreLoadPrep) call: the registers and the running thread after a load or a reset are another run's.
// Each case is run without the invalidation first, to show the stale answer is real, then with it.
//   ps5/coreorbis/tests/tidcache/test-tidcache.sh
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisEEHle.h"

#include <cstdio>
#include <cstring>

// What OrbisEEDiag.cpp and R5900.cpp define in the eboot.
alignas(16) cpuRegistersPack _cpuRegistersPack;
std::atomic<int> g_orbis_hle_tid{1};
OrbisTidCache g_orbis_tid;
u64 g_orbis_tid_hits = 0, g_orbis_tid_checks = 0, g_orbis_tid_mismatches = 0, g_orbis_eret_skips = 0;

namespace
{
int g_passed = 0, g_failed = 0;

void Check(bool ok, const char* what)
{
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	(ok ? g_passed : g_failed)++;
}

constexpr u32 kKernel = 0x80000280; // where the kernel's handlers run (any address: the cache never looks at it)

// A thread of the guest: where it runs, its stack pointer and the id the kernel gives it.
struct Thread
{
	u32 pc;
	u32 sp;
	u64 id;
};

void RunAt(const Thread& t)
{
	cpuRegs.pc = t.pc;
	cpuRegs.GPR.n.sp.UL[0] = t.sp;
	cpuRegs.CP0.n.Status.b.EXL = 0;
}

// The kernel's exception entry (cpuException, R5900.cpp: OrbisTidException comes first) at `epc`.
void Enter(u32 epc)
{
	cpuRegs.pc = epc;
	OrbisTidException(0);
	cpuRegs.CP0.n.EPC = epc;
	cpuRegs.CP0.n.Status.b.EXL = 1;
	cpuRegs.pc = kKernel;
}

// ERET (COP0.cpp: OrbisTidEret after the new pc is set) to `pc` on the stack `sp`.
void Eret(u32 pc, u32 sp)
{
	cpuRegs.CP0.n.Status.b.EXL = 0;
	cpuRegs.pc = pc;
	cpuRegs.GPR.n.sp.UL[0] = sp;
	OrbisTidEret();
}

// GetThreadId (SYSCALL 0x2f, R5900OpcodeImpl.cpp) by `t`: the cache's answer, else the kernel's (EPC the SYSCALL, back past it, v0 = id).
// `answered` tells which.
u64 GetThreadId(const Thread& t, bool& answered)
{
	RunAt(t);
	cpuRegs.GPR.n.v0.UD[0] = 0xdeadbeef;
	answered = OrbisTidSyscall();
	if (answered)
		return cpuRegs.GPR.n.v0.UD[0];
	Enter(t.pc);
	cpuRegs.GPR.n.v0.UD[0] = t.id;
	Eret(t.pc + 4, t.sp);
	return cpuRegs.GPR.n.v0.UD[0];
}

// An interrupt taken by `t` and returned from (the same thread runs on).
void Interrupt(const Thread& t)
{
	Enter(t.pc + 0x40);
	Eret(t.pc + 0x40, t.sp);
}

void Fresh()
{
	std::memset(&_cpuRegistersPack, 0, sizeof(_cpuRegistersPack));
	g_orbis_tid = OrbisTidCache();
	g_orbis_hle_tid = 1;
	g_orbis_tid_hits = g_orbis_tid_checks = g_orbis_tid_mismatches = 0;
}

// The id thread `b` of the loaded run gets from its first GetThreadId after a load that came while `a` ran with the cache valid. `inflight`:
// the load came inside an interrupt of `a`'s (EXL set, its ERET not yet run), and the loaded run's own handler returns to the same EPC and
// stack pointer (a static stack and the same spot of the game's loop: the thread's id may still differ, threads made in another order).
u64 AfterLoad(const Thread& a, const Thread& b, bool invalidate, bool inflight, bool& answered)
{
	Fresh();
	bool hit = false;
	GetThreadId(a, hit); // the kernel's answer, now cached
	GetThreadId(a, hit);
	if (!hit)
		return 0;
	if (inflight)
		Enter(a.pc + 0x40);
	// the load (or the reset) puts the other run's registers in place
	if (invalidate)
		OrbisTidInvalidate();
	if (inflight)
		Eret(a.pc + 0x40, a.sp); // the loaded run's handler, back where `a`'s was
	return GetThreadId(b, answered);
}
} // namespace

int main()
{
	const Thread a = {0x00100200, 0x01ff0000, 0x11};
	const Thread b = {0x00200300, 0x01fe0000, 0x22};

	// 1. The cache as the game sees it.
	{
		Fresh();
		bool hit = true;
		Check(GetThreadId(a, hit) == a.id && !hit, "a fresh cache: the first call goes to the kernel");
		Check(GetThreadId(a, hit) == a.id && hit, "the second call is answered from the cache");
		Interrupt(a);
		Check(GetThreadId(a, hit) == a.id && hit, "an interrupt that returns to the same thread keeps it");
		Enter(a.pc + 0x40);
		Eret(b.pc, b.sp); // the kernel switched threads
		Check(GetThreadId(b, hit) == b.id && !hit, "an ERET to another thread drops it: the kernel answers");
		Check(GetThreadId(b, hit) == b.id && hit, "... and the new thread's id is cached");
	}

	// 2. A load while the cache is valid, nothing in flight: the loaded thread's first call.
	{
		bool answered = false;
		const u64 old_way = AfterLoad(a, b, false, false, answered);
		Check(old_way == a.id && answered, "without the invalidation the loaded thread gets the run before's id (the bug)");
		const u64 now = AfterLoad(a, b, true, false, answered);
		Check(now == b.id && !answered, "with OrbisTidInvalidate the call goes to the kernel: the loaded thread's own id");
		bool hit = false;
		Check(GetThreadId(b, hit) == b.id && hit, "... and the cache works again from there");
	}

	// 3. A load inside an interrupt (the vsync's event test runs with EXL set), the loaded run returning to the same EPC and stack.
	{
		bool answered = false;
		const u64 old_way = AfterLoad(a, a, false, true, answered);
		Check(answered, "without the invalidation the ERET to the old EPC and stack keeps the old entry valid (the bug)");
		Thread a2 = a;
		a2.id = 0x33; // same code, same stack, another id
		const u64 old_id = AfterLoad(a, a2, false, true, answered);
		Check(old_id == a.id && answered && old_way == a.id, "... and answers the run before's id");
		const u64 now = AfterLoad(a, a2, true, true, answered);
		Check(now == a2.id && !answered, "with OrbisTidInvalidate the exception in flight is forgotten: the kernel answers");
	}

	// 4. A load while the kernel runs a GetThreadId call (its ERET would record v0, now the loaded run's register, as the id).
	{
		for (int invalidate = 0; invalidate < 2; invalidate++)
		{
			Fresh();
			RunAt(a);
			Check(!OrbisTidSyscall(), "a GetThreadId call goes to the kernel");
			Enter(a.pc); // its exception: the cache is to record v0 at the ERET
			if (invalidate)
				OrbisTidInvalidate();
			cpuRegs.GPR.n.v0.UD[0] = 0x4242; // the loaded run's v0, no thread id
			Eret(a.pc + 4, a.sp);
			bool hit = false;
			const u64 got = GetThreadId(a, hit);
			if (!invalidate)
				Check(got == 0x4242 && hit, "without the invalidation the loaded run's v0 is recorded as the id (the bug)");
			else
				Check(got == a.id && !hit, "with OrbisTidInvalidate it isn't: the next call goes to the kernel");
		}
	}

	// 5. A VM reset (cpuReset: the registers zeroed, then the invalidation).
	{
		Fresh();
		bool hit = false;
		GetThreadId(a, hit);
		std::memset(&_cpuRegistersPack, 0, sizeof(_cpuRegistersPack));
		OrbisTidInvalidate();
		Check(GetThreadId(a, hit) == a.id && !hit, "after a reset the first call goes to the kernel");
	}

	// 6. seq starts at 1 and only grows, so valid_seq 0 never matches; invalidating twice is harmless.
	{
		Fresh();
		Check(g_orbis_tid.seq == 1 && g_orbis_tid.valid_seq == 0, "a new cache: seq 1, valid_seq 0");
		OrbisTidInvalidate();
		OrbisTidInvalidate();
		bool hit = true;
		Check(g_orbis_tid.seq == 3 && GetThreadId(b, hit) == b.id && !hit, "two invalidations in a row: seq 3, the next call goes to the kernel");
	}

	// 7. flags/nohletid (the cache off, its answers checked against the kernel's): no false mismatch after a load.
	{
		for (int invalidate = 0; invalidate < 2; invalidate++)
		{
			Fresh();
			g_orbis_hle_tid = 0;
			bool hit = false;
			GetThreadId(a, hit);
			GetThreadId(a, hit);
			const u64 checks = g_orbis_tid_checks, mismatches = g_orbis_tid_mismatches;
			if (invalidate)
				OrbisTidInvalidate();
			GetThreadId(b, hit);
			if (!invalidate)
				Check(g_orbis_tid_mismatches == mismatches + 1, "check mode without the invalidation: the load counts as a mismatch");
			else
				Check(g_orbis_tid_checks == checks && g_orbis_tid_mismatches == mismatches, "with it, the loaded thread's call is not checked");
		}
	}

	std::printf("%s: %d passed, %d failed\n", g_failed ? "FAILED" : "ALL PASSED", g_passed, g_failed);
	return g_failed ? 1 : 0;
}
