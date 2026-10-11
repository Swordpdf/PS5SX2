// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// PS5 port (vk-285-102/103): GetThreadId (EE syscall 0x2f) answered from a cache while the running thread can't
// have changed. Shadow of the Colossus's heavy view makes ~143,000 GetThreadId calls a second (of ~315,000
// syscalls, vk-285-101's [eesys] line). Each one ran the kernel's exception entry, its syscall dispatch, the
// context restore and the ERET, and the ERET forced an event test 4 cycles on.
//
// The PS2 kernel changes threads only inside its exception handlers, which all leave through ERET. So:
// - seq counts every exception entry (cpuException) and every ERET;
// - an exception entry remembers where it came from (its EPC and the stack pointer); when the ERET that
//   follows (with no other exception in between) returns there -- EPC, or EPC + 4 after a syscall, with the
//   same stack pointer -- the same thread runs again: a cache that held before the exception still holds, and
//   an ERET that ends a GetThreadId call records v0's low 64 bits as the id;
// - an ERET anywhere else (another thread, a new thread's start) or a second exception before the ERET
//   leaves the cache stale until the next GetThreadId call goes to the kernel.
// vk-285-102 kept the cache only across the GetThreadId calls themselves (~60K of the 143K calls a second
// answered); vk-285-103 keeps it across the other syscalls and interrupts that return to the same thread.
// The cache answers unless flags/nohletid (vk-285-105: on by default, after vk-285-104's check mode found no
// mismatch in ~130,000 checks a second); with the flag every call goes to the kernel, and the calls the cache could
// have answered are compared with the kernel's answer ([eefast] checked/mismatched). v0's upper 64 bits are
// left as they are (the kernel's restore leaves the caller's there). Needs proper testing.

#include "R5900.h"
#include "OrbisEEDiag.h"

// vk-285-107: a cache line of its own (the EE thread writes it at every exception and ERET).
struct alignas(64) OrbisTidCache
{
	u64 seq = 1;
	u64 valid_seq = 0; // seq at which value is the running thread's id (0: never)
	u64 value = 0;
	u64 ex_seq = 0; // the seq of the exception in flight (0: none, or one that can't be matched)
	u32 ex_pc = 0; // its EPC
	u32 ex_sp = 0; // and the stack pointer it interrupted
	bool ex_tid = false; // it is a GetThreadId call to record
	bool tid_pending = false; // a GetThreadId SYSCALL is about to raise its exception
	bool check = false; // that call is one the cache could have answered
	u64 expected = 0;
};
extern OrbisTidCache g_orbis_tid;

// SYSCALL 0x2f, before the exception: true when answered here (the caller returns without raising it).
static __fi bool OrbisTidSyscall()
{
	OrbisTidCache& t = g_orbis_tid;
	const bool hit = t.valid_seq == t.seq;
	if (hit && g_orbis_hle_tid.load(std::memory_order_relaxed))
	{
		cpuRegs.GPR.n.v0.UD[0] = t.value;
		g_orbis_tid_hits++;
		return true;
	}
	t.check = hit;
	t.expected = t.value;
	t.tid_pending = true;
	return false;
}

// Every exception entry, before cpuException changes anything: pc is the EPC to be (SYSCALL has already moved
// it back to the SYSCALL). Not matched: one in a branch delay slot (EPC is the branch then) and one taken with
// EXL already set (EPC keeps the outer exception's return).
static __fi void OrbisTidException(u32 bd)
{
	OrbisTidCache& t = g_orbis_tid;
	t.seq++;
	t.ex_seq = (bd || cpuRegs.CP0.n.Status.b.EXL) ? 0 : t.seq;
	t.ex_pc = cpuRegs.pc;
	t.ex_sp = cpuRegs.GPR.n.sp.UL[0];
	t.ex_tid = t.tid_pending;
	t.tid_pending = false;
}

// ERET, after the new pc is set.
static __fi void OrbisTidEret()
{
	OrbisTidCache& t = g_orbis_tid;
	const bool same_thread = t.ex_seq != 0 && t.ex_seq == t.seq &&
		(cpuRegs.pc == t.ex_pc || cpuRegs.pc == t.ex_pc + 4) && cpuRegs.GPR.n.sp.UL[0] == t.ex_sp;
	const bool held = t.valid_seq == t.ex_seq - 1; // valid just before that exception
	t.seq++;
	if (same_thread)
	{
		if (t.ex_tid)
		{
			const u64 id = cpuRegs.GPR.n.v0.UD[0];
			if (t.check)
			{
				g_orbis_tid_checks++;
				if (id != t.expected)
					g_orbis_tid_mismatches++;
			}
			t.value = id;
			t.valid_seq = t.seq;
		}
		else if (held)
			t.valid_seq = t.seq;
	}
	t.ex_seq = 0;
	t.ex_tid = false;
}
