// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// PS5 port (vk-285-100): see OrbisEEDiag.h. Needs proper testing.

#include "OrbisEEDiag.h"
#include "OrbisEEHle.h"
#include "R5900.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

extern bool OrbisFlag(const char* name); // orbis-shims/orbis_paths.cpp

#include <dirent.h>
#include <string>
#include <vector>

// PS5 port (vk-285-104): this file's printf/fflush(stdout) go to the deferred log (OrbisDeferredLog.h); the
// ticker thread writes them out, so the GS thread never waits on /data or on stdout's lock.
#include "OrbisDeferredLog.h"
#define printf OrbisDeferredPrintf
#define fflush OrbisDeferredFlush

namespace
{
	// vk-285-105: the flags from OrbisFlag's snapshot (orbis_paths.cpp), refreshed by the ticker thread.
	bool OrbisHasFlag(const char* name)
	{
		return OrbisFlag(name);
	}
} // namespace

namespace OrbisEEDiag
{
	std::atomic<int> on{0};
	u64 syscalls[256] = {};
	u64 cpu_int[32] = {};
	u64 dmac_irq[16] = {};
	u64 intc_irq[16] = {};
	u64 exc_intc = 0, exc_dmac = 0;
	u64 ev_dma = 0;
	u64 iop_rapid = 0;
	u64 rcnt = 0;
	u64 vif1_codes[128] = {};
} // namespace OrbisEEDiag

// PS5 port (vk-285-107): the switches (read by the EE, GS and VU threads, written once a second by the GS thread)
// and the EE thread's counters (written up to ~2.5 million times a second) on cache lines of their own. In
// vk-285-106 g_orbis_nt_store, which the GS thread reads a few times a draw, shared a line with g_orbis_vif1_fused,
// which the fused VIF1 loop increments at every pass: the EE profile's hottest vif1Interrupt instruction (0.7% of
// the EE thread) sat right after that increment. Each group starts a line, and a 64-byte fence ends it so nothing
// the linker puts next shares its last line. Needs proper testing.
// Initialised data (.data): the EE's switches, then the GS thread's (on a line of its own), then the GetThreadId
// cache (a whole line: OrbisEEHle.h) and a fence.
alignas(64) std::atomic<int> g_orbis_vif1_fuse{1};
std::atomic<int> g_orbis_hle_tid{1}; // vk-285-105: on by default (flags/nohletid)
std::atomic<int> g_orbis_eret_fast{1}; // vk-285-105: on by default (flags/noeretfast)
std::atomic<int> g_orbis_vif_fast{1};
std::atomic<int> g_orbis_waitloop_ext{1}; // vk-285-126: on unless flags/waitloop_upstream
alignas(64) std::atomic<int> g_orbis_gs_pfw{1}; // vk-285-103: flags/gspfw; vk-285-107: on unless flags/nogspfw (VKStreamBuffer.cpp)
OrbisTidCache g_orbis_tid; // vk-285-102 (OrbisEEHle.h)
alignas(64) char g_orbis_ee_data_fence[64] = {1};
// Zeroed data (.bss): the switches that start off (read, not written, by the EE and GS threads), then the EE
// thread's counters and a fence.
std::atomic<int> g_orbis_hle_flush{0};
std::atomic<int> g_orbis_pfw{0};
std::atomic<int> g_orbis_nt_store{0};
alignas(64) u64 g_orbis_vif1_fused = 0;
u64 g_orbis_hle_flushes = 0;
u64 g_orbis_tid_hits = 0, g_orbis_tid_checks = 0, g_orbis_tid_mismatches = 0, g_orbis_eret_skips = 0;
u64 g_orbis_eret_block[4] = {}; // vk-285-103: ERETs whose event test flags/eretfast kept: INTC, DMAC, timer, VU0
u64 g_orbis_waitloop_ext_blocks = 0, g_orbis_waitloop_ext_runs = 0; // vk-285-126
alignas(64) char g_orbis_ee_counters_fence[64] = {};

// The EE core's cycles per TSC tick, x1e6, measured on the EE thread every 300 vsyncs (0: not yet).
static std::atomic<u64> s_orbis_ee_ratio_u{0};

void OrbisEEVsyncClock()
{
	static u32 s_vsyncs = 0;
	if ((++s_vsyncs % 300) != 1)
		return;
	s_orbis_ee_ratio_u.store(static_cast<u64>(OrbisCoreCyclesPerTsc() * 1e6), std::memory_order_relaxed);
}

namespace
{
	// Deltas of a counter array since the last call, the biggest first.
	template <size_t N>
	struct OrbisDeltas
	{
		u64 prev[N] = {};
		std::pair<u64, u32> top[N];
		u64 total = 0;
		u32 n = 0;

		void Take(const u64 (&cur)[N])
		{
			total = 0;
			n = 0;
			for (u32 i = 0; i < N; i++)
			{
				const u64 now = cur[i];
				const u64 d = now - prev[i];
				prev[i] = now;
				total += d;
				if (d)
					top[n++] = {d, i};
			}
			std::sort(top, top + n, [](const auto& a, const auto& b) { return a.first > b.first; });
		}
	};

	const char* const s_event_names[32] = {"vif0", "vif1", "gif", "fromipu", "toipu", "sif0", "sif1", "sif2",
		"fromspr", "tospr", "mfifovif", "mfifogif", "12", "stall", "mfifoempty", "buserr", "gifunit", "vu0fin",
		"vu1fin", "ipuproc", "mtvubusy", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31"};

	void OrbisPrintCounters()
	{
		using namespace OrbisEEDiag;
		static OrbisDeltas<256> s_sys;
		static OrbisDeltas<32> s_int;
		static OrbisDeltas<16> s_dmac, s_intc;
		static OrbisDeltas<128> s_codes;
		static u64 s_prev[6] = {};
		const u64 cur[6] = {exc_intc, exc_dmac, ev_dma, iop_rapid, rcnt, 0};
		u64 d[6];
		for (int i = 0; i < 6; i++)
		{
			d[i] = cur[i] - s_prev[i];
			s_prev[i] = cur[i];
		}

		s_sys.Take(syscalls);
		printf("[eesys] per s: syscalls=%llu", static_cast<unsigned long long>(s_sys.total));
		for (u32 i = 0; i < std::min<u32>(s_sys.n, 8); i++)
			printf(" %s(%#x)=%llu", R5900::bios[s_sys.top[i].second], s_sys.top[i].second,
				static_cast<unsigned long long>(s_sys.top[i].first));
		printf(" | interrupts taken intc=%llu dmac=%llu", static_cast<unsigned long long>(d[0]),
			static_cast<unsigned long long>(d[1]));
		s_dmac.Take(dmac_irq);
		printf(" | dmac irq");
		for (u32 i = 0; i < s_dmac.n; i++)
			printf(" %s=%llu", s_event_names[s_dmac.top[i].second], static_cast<unsigned long long>(s_dmac.top[i].first));
		s_intc.Take(intc_irq);
		printf(" | intc irq");
		for (u32 i = 0; i < s_intc.n; i++)
			printf(" %u=%llu", s_intc.top[i].second, static_cast<unsigned long long>(s_intc.top[i].first));
		printf("\n");

		s_int.Take(cpu_int);
		printf("[evsrc] per s: evtests with a DMA/MTVU event pending=%llu iop-rapid=%llu counters=%llu | cpu_int",
			static_cast<unsigned long long>(d[2]), static_cast<unsigned long long>(d[3]),
			static_cast<unsigned long long>(d[4]));
		for (u32 i = 0; i < s_int.n; i++)
			printf(" %s=%llu", s_event_names[s_int.top[i].second], static_cast<unsigned long long>(s_int.top[i].first));
		printf("\n");

		s_codes.Take(vif1_codes);
		printf("[vifcodes] per s: vif1 codes=%llu", static_cast<unsigned long long>(s_codes.total));
		for (u32 i = 0; i < std::min<u32>(s_codes.n, 14); i++)
			printf(" %02x=%llu", s_codes.top[i].second, static_cast<unsigned long long>(s_codes.top[i].first));
		printf("\n");
	}
} // namespace

void OrbisEEDiagSecond(bool print)
{
	// vk-285-105: OrbisFlag answers from the snapshot main-boot's ticker refreshes (no system call here).
	const bool diag = OrbisHasFlag("eediag");
	const bool was_diag = OrbisEEDiag::on.exchange(diag ? 1 : 0, std::memory_order_relaxed) != 0;
	struct Switch
	{
		std::atomic<int>& value;
		int on;
		const char* name;
	};
	Switch switches[] = {
		{g_orbis_vif1_fuse, OrbisHasFlag("vif1nofuse") ? 0 : 1, "vif1 fused loop"},
		{g_orbis_hle_flush, OrbisHasFlag("hleflush") ? 1 : 0, "FlushCache HLE"},
		{g_orbis_pfw, OrbisHasFlag("pfw") ? 1 : 0, "write prefetches"},
		{g_orbis_hle_tid, OrbisHasFlag("nohletid") ? 0 : 1, "GetThreadId cache"},
		{g_orbis_eret_fast, OrbisHasFlag("noeretfast") ? 0 : 1, "ERET without event test"},
		{g_orbis_vif_fast, OrbisHasFlag("vifslow") ? 0 : 1, "VIF simple codes in the loop"},
		{g_orbis_nt_store, OrbisHasFlag("ntstore") ? 1 : 0, "GS streaming stores"},
		{g_orbis_gs_pfw, OrbisHasFlag("nogspfw") ? 0 : 1, "GS stream prefetch"}, // vk-285-107: on unless flags/nogspfw
		{g_orbis_waitloop_ext, OrbisHasFlag("waitloop_upstream") ? 0 : 1, "wider wait-loop rule"}, // vk-285-126
	};
	bool changed = diag != was_diag;
	for (Switch& sw : switches)
		changed |= sw.value.exchange(sw.on, std::memory_order_relaxed) != sw.on;
	if (!print)
		return;
	static bool s_first = true;
	if (s_first || changed)
	{
		printf("[eeswitch]");
		for (const Switch& sw : switches)
			printf(" %s %s,", sw.name, sw.on ? "on" : "off");
		printf(" eediag %s\n", diag ? "on" : "off");
		s_first = false;
	}

	// The fast paths' counts per second, when any moved (plain loads of EE-thread counters).
	{
		static u64 s_prev[12] = {};
		const u64 cur[12] = {g_orbis_vif1_fused, g_orbis_hle_flushes, g_orbis_tid_hits, g_orbis_tid_checks,
			g_orbis_tid_mismatches, g_orbis_eret_skips, g_orbis_eret_block[0], g_orbis_eret_block[1],
			g_orbis_eret_block[2], g_orbis_eret_block[3], g_orbis_waitloop_ext_runs, g_orbis_waitloop_ext_blocks};
		if (std::memcmp(cur, s_prev, sizeof(cur)) != 0)
			printf("[eefast] per s: vif1 passes in place=%llu FlushCache HLE=%llu | GetThreadId cached=%llu checked=%llu "
				   "mismatched=%llu (total %llu) | ERETs without event test=%llu, kept by intc=%llu dmac=%llu timer=%llu "
				   "vu0=%llu | wider wait loops: fast-forwards=%llu blocks=%llu (total)\n",
				static_cast<unsigned long long>(cur[0] - s_prev[0]), static_cast<unsigned long long>(cur[1] - s_prev[1]),
				static_cast<unsigned long long>(cur[2] - s_prev[2]), static_cast<unsigned long long>(cur[3] - s_prev[3]),
				static_cast<unsigned long long>(cur[4] - s_prev[4]), static_cast<unsigned long long>(cur[4]),
				static_cast<unsigned long long>(cur[5] - s_prev[5]), static_cast<unsigned long long>(cur[6] - s_prev[6]),
				static_cast<unsigned long long>(cur[7] - s_prev[7]), static_cast<unsigned long long>(cur[8] - s_prev[8]),
				static_cast<unsigned long long>(cur[9] - s_prev[9]), static_cast<unsigned long long>(cur[10] - s_prev[10]),
				static_cast<unsigned long long>(cur[11]));
		std::memcpy(s_prev, cur, sizeof(cur));
	}

	// [cpuclk] every 5 s: the TSC's rate against steady_clock over the 5 s, and the EE's and this (GS)
	// thread's cores' clocks from the add chain.
	{
		static u32 s_seconds = 0;
		static u64 s_tsc = 0;
		static auto s_t = std::chrono::steady_clock::now();
		if ((s_seconds++ % 5) == 0)
		{
			const u64 tsc = __builtin_ia32_rdtsc();
			const auto now = std::chrono::steady_clock::now();
			const double sec = std::chrono::duration<double>(now - s_t).count();
			if (s_tsc != 0 && sec > 1.0)
			{
				const double tsc_mhz = static_cast<double>(tsc - s_tsc) / sec / 1e6;
				const double gs_ratio = OrbisCoreCyclesPerTsc();
				const double ee_ratio = static_cast<double>(s_orbis_ee_ratio_u.load(std::memory_order_relaxed)) / 1e6;
				printf("[cpuclk] tsc %.2f MHz | ee core %.0f MHz (%.4f cycles/tick) | gs core %.0f MHz (%.4f)\n", tsc_mhz,
					ee_ratio * tsc_mhz, ee_ratio, gs_ratio * tsc_mhz, gs_ratio);
			}
			s_tsc = tsc;
			s_t = now;
		}
	}

	// The counters only count while eediag is on, so the first line after it appears covers part of a second.
	if (diag)
		OrbisPrintCounters();
}
