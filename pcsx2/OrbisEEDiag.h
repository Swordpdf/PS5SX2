// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// PS5 port (vk-285-100): EE-thread counters for the [eesys], [evsrc] and [vifcodes] lines, the live switches
// of vk-285-100's EE changes, and the core-clock probe. The GS thread polls the flag files once a second
// (OrbisEEDiagSecond, from GSRenderer.cpp's [load] line) and prints the counter lines while flags/eediag is
// present. The counters are plain u64s the EE thread adds to (only while eediag is on) and the GS thread reads
// once a second; a count read mid-add is off by one at most. Needs proper testing.

#include "common/Pcsx2Defs.h"

#include <atomic>

namespace OrbisEEDiag
{
	extern std::atomic<int> on; // flags/eediag
	extern u64 syscalls[256]; // SYSCALL, by call number
	extern u64 cpu_int[32]; // CPU_INT, by EE_EventType
	extern u64 dmac_irq[16]; // hwDmacIrq, by channel
	extern u64 intc_irq[16]; // hwIntcIrq, by source
	extern u64 exc_intc, exc_dmac; // interrupts taken at event tests (both counted when both are pending)
	extern u64 ev_dma; // event tests entered with a DMA or MTVU event pending
	extern u64 iop_rapid; // event tests that set the next one 48 cycles on (the IOP behind)
	extern u64 rcnt; // event tests that ran the counters
	extern u64 vif1_codes[128]; // VIF1 codes read, by command
} // namespace OrbisEEDiag

// Live switches (the GS thread sets them from flag files once a second; see OrbisEEDiag.cpp).
extern std::atomic<int> g_orbis_vif1_fuse; // 1 unless flags/vif1nofuse: vif1Interrupt's passes in one call
extern std::atomic<int> g_orbis_hle_flush; // 1 with flags/hleflush: FlushCache/iFlushCache without the kernel
extern std::atomic<int> g_orbis_pfw; // 1 with flags/pfw: vk-285-99's write prefetches (vk-285-102: off by default)
extern std::atomic<int> g_orbis_hle_tid; // 1 unless flags/nohletid: GetThreadId from the cache (OrbisEEHle.h)
extern std::atomic<int> g_orbis_eret_fast; // 1 unless flags/noeretfast: no event test after ERET without an interrupt
extern std::atomic<int> g_orbis_vif_fast; // 1 unless flags/vifslow: STMOD/STCYCL/BASE/OFFSET/ITOP in the loop
extern std::atomic<int> g_orbis_nt_store; // 1 with flags/ntstore: vk-285-98's streaming stores for UBOs/indices
extern u64 g_orbis_vif1_fused; // vif1Interrupt passes that continued in place (always counted, EE thread)
extern bool g_orbis_instant_dma_loop; // inside _cpuEventTest_Shared's Instant DMA loop (R5900.cpp)
extern u64 g_orbis_hle_flushes; // FlushCache/iFlushCache calls answered without the kernel (EE thread)
// vk-285-102 (EE thread): GetThreadId answered from the cache, checked against the kernel's answer while the
// cache is off (a call the cache could have answered), and the mismatches; ERETs without the event test.
extern u64 g_orbis_tid_hits, g_orbis_tid_checks, g_orbis_tid_mismatches, g_orbis_eret_skips;
// vk-285-126: the EE recompiler's wait-loop rule also trusts a register loaded through a base the loop never writes
// (iR5900.cpp); 1 unless flags/waitloop_upstream (read when a block is compiled). Blocks that only the wider rule
// fast-forwards, and how often they did (EE thread).
extern std::atomic<int> g_orbis_waitloop_ext;
extern u64 g_orbis_waitloop_ext_blocks, g_orbis_waitloop_ext_runs;

#define ORBIS_EEDIAG(expr) \
	do \
	{ \
		if (OrbisEEDiag::on.load(std::memory_order_relaxed)) [[unlikely]] \
		{ \
			expr; \
		} \
	} while (0)

// The core's clock in cycles per TSC tick while a chain of dependent adds ran (Zen 2 retires one a cycle):
// times the TSC's frequency, the clock the calling thread's core ran at. 2^16 iterations of 8 adds, ~0.15 ms.
static inline double OrbisCoreCyclesPerTsc()
{
	constexpr u64 iterations = 1u << 16;
	u64 x = 0, i = iterations;
	const u64 t0 = __builtin_ia32_rdtsc();
	asm volatile(
		"1:\n\t"
		"add $1, %0\n\t"
		"add $1, %0\n\t"
		"add $1, %0\n\t"
		"add $1, %0\n\t"
		"add $1, %0\n\t"
		"add $1, %0\n\t"
		"add $1, %0\n\t"
		"add $1, %0\n\t"
		"dec %1\n\t"
		"jnz 1b\n\t"
		: "+r"(x), "+r"(i)
		:
		: "cc");
	const u64 t1 = __builtin_ia32_rdtsc();
	return t1 > t0 ? static_cast<double>(iterations * 8) / static_cast<double>(t1 - t0) : 0.0;
}

void OrbisEEDiagSecond(bool print); // GS thread, once a second: the flags; with print, [cpuclk] every 5 s and the lines
void OrbisEEVsyncClock(); // EE thread, once a vsync: measures the EE core's clock every 300 vsyncs
