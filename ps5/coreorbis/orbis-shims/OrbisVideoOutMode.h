// PS5SX2 (2.11, AI-assisted): switching the console's VideoOut to its 119.88 Hz output from the app, for frame generation.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Frame generation shows a 60 fps game at 120 (a generated frame before each of the game's) and a PAL game at 50 as 100. That needs
// the console's output at 119.88 Hz. The 2.02 build had it from its driver (ps5vk's "high frame rate" output, selector 15 of
// sceVideoOutConfigureOutput, from the public ps5-opengl runtime and Mihawk-99's PS5_Vulkan); the drivers 2.03 and later were linked
// with have no output code, so the display stayed at 59.94 Hz and only games at 30 fps or less got generated frames. This is the
// app-side way, used ONLY when frame generation is on, the driver exports no output control of its own (ps5vk_output_*), and the
// switch file fg120 is there (flags/fg120; flags/nofg120 turns it off for good): it makes the driver's sequence by hand, then
// leaves the output at 120 Hz for the driver's own VideoOut handle. NOT TESTED ON A CONSOLE.
//
// The sequence, as the driver author measured it on a console (PS5_Vulkan docs/HARDWARE_FINDINGS.md, M5_PHASE_C.md):
//   - sceVideoOutOpen on a fresh handle (user 0xff, bus 0, index 0), before any flip rate or buffer is set
//   - sceVideoOutIsOutputSupported(15) must answer > 0; at 1080p console output it answered 0 and ConfigureOutput returned
//     0x80290019; a title that doesn't declare high frame rates (param.json attribute3 0x80040) gets 0x80290016. Anything but a
//     positive answer: nothing is configured.
//   - sceVideoOutConfigureOutput(15) must return 0, and then the vblank period is TIMED: an accepted mode once kept refreshing at
//     60 Hz. The median of 6 intervals between bare sceVideoOutWaitVblank calls (the first call finding the phase) has to be under
//     12.5 ms (8.34 ms at 119.88 Hz, 16.68 at 59.94); if it is not, or a wait fails, selector 1 puts the default output back.
//   - an output at 120 Hz outlives the process that asked for it, so it is put back (selector 1) before the process ends.
// Then the handle is closed WITHOUT restoring, and 5 s pass before the driver opens VideoOut again: the 2.02 driver would not open
// it sooner than 5,000,000,000 ns after a 120 Hz output closed (its constant, read from the disassembly), because an open right
// after the close failed. The drivers 2.03 and later have no such guard and only two retries 500 ms apart. The caller runs
// SelectHighFrameRate on a thread of its own while the VM boots, so the 5 s are mostly overlapped.
//
// A marker file (logs/output-120.txt) is written before the output is configured and removed when it is restored: a start that finds
// it (the app was killed, a crash, the console's menu) restores the default output first, and does not switch in that run.
//
// Pure logic over a table of calls (Ops), so a PC test runs it with a fake VideoOut and clock (tests/videoout);
// OrbisVideoOutMode.cpp has the real calls, the thread and the marker file.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace orbis_vout
{
	constexpr uint32_t kSelectorRestore = 1;        // the default output (59.94 Hz)
	constexpr uint32_t kSelectorHighFrameRate = 15; // 119.88 Hz
	constexpr unsigned kPeriodIntervals = 6;        // vblank intervals timed (after one to find the phase)
	constexpr uint64_t kHighPeriodLimitNs = 12'500'000; // halfway between 8.342 ms (119.88 Hz) and 16.683 ms (59.94 Hz)
	constexpr uint64_t kHighPeriodMinNs = 7'500'000;    // a period from here to the next is the 119.88 Hz output (8.342 ms), measured
	constexpr uint64_t kHighPeriodMaxNs = 9'200'000;    // with the jitter of a thread's sleeps: reported as exactly 119.88 Hz
	constexpr uint32_t kHighRefreshMilliHz = 119880;
	constexpr uint32_t kReopenSettleMs = 5000;      // from the 2.02 driver: 5,000,000,000 ns since a 120 Hz output closed
	constexpr uint32_t kProbeSelectors = 64;        // flag videoprobe asks about 0..63

	constexpr uint32_t kErrNotDeclared = 0x80290016u;     // the title doesn't declare high frame rates
	constexpr uint32_t kErrNoFormat = 0x80290019u;        // the console's video output setting has no 120 Hz format here
	constexpr uint32_t kErrUnknownSelector = 0x8029001Eu; // no such selector

	// The system calls, a marker and a clock. `ctx` is passed to each. The marker calls may be null (no marker).
	struct Ops
	{
		void* ctx = nullptr;
		int (*open)(void* ctx) = nullptr;                                     // a handle (>= 0), or an error code (< 0)
		int (*close)(void* ctx, int handle) = nullptr;
		int (*supported)(void* ctx, int handle, uint32_t selector) = nullptr; // sceVideoOutIsOutputSupported: > 0 yes, 0 no, < 0 error
		int (*configure)(void* ctx, int handle, uint32_t selector) = nullptr; // sceVideoOutConfigureOutput: 0 done
		int (*wait_vblank)(void* ctx, int handle) = nullptr;                  // sceVideoOutWaitVblank: 0 done
		uint64_t (*now_ns)(void* ctx) = nullptr;
		void (*sleep_ms)(void* ctx, uint32_t ms) = nullptr;
		bool (*marker_present)(void* ctx) = nullptr;
		void (*marker_set)(void* ctx) = nullptr;
		void (*marker_clear)(void* ctx) = nullptr;
	};

	enum class Outcome
	{
		Active,       // 119.88 Hz, measured; the handle is closed, the output left as it is
		NotOffered,   // IsOutputSupported(15) did not answer yes: nothing was configured
		Refused,      // ConfigureOutput(15) returned an error: the output is as it was
		SlowVblank,   // accepted, but the vblank period says 60 Hz: the default output was put back
		VblankFailed, // accepted, but a vblank wait failed: the default output was put back
		OpenFailed,   // sceVideoOutOpen failed
	};

	struct Result
	{
		Outcome outcome = Outcome::OpenFailed;
		int open_code = 0;         // the handle, or the open's error
		int supported = 0;         // IsOutputSupported(15)'s answer
		int configured = 0;        // ConfigureOutput(15)'s answer
		bool restore_ran = false;  // selector 1 was sent after an accepted configure
		int restored = 0;          // and its answer
		uint64_t period_ns = 0;    // the median vblank period (0: not measured)
		uint32_t refresh_mhz = 0;  // Active: 1e12 / period
	};

	// The median of kPeriodIntervals intervals between bare vblank waits, the first wait only finding the phase. 0 when a wait fails.
	inline uint64_t MedianVblankPeriod(const Ops& ops, int handle)
	{
		if (ops.wait_vblank(ops.ctx, handle) != 0)
			return 0;
		uint64_t previous = ops.now_ns(ops.ctx);
		uint64_t intervals[kPeriodIntervals];
		for (unsigned at = 0; at < kPeriodIntervals; at++)
		{
			if (ops.wait_vblank(ops.ctx, handle) != 0)
				return 0;
			const uint64_t now = ops.now_ns(ops.ctx);
			intervals[at] = now > previous ? now - previous : 0;
			previous = now;
		}
		std::sort(intervals, intervals + kPeriodIntervals);
		return intervals[kPeriodIntervals / 2];
	}

	// Opens VideoOut, configures the 119.88 Hz output when it is offered, times it, and either leaves it (Active: the handle closed,
	// then kReopenSettleMs of sleep so that the driver's own sceVideoOutOpen is not made right after the close) or puts the default
	// output back. Every handle opened is closed once, on every path. The marker is written before the output is configured and
	// stays while the output is at 120 Hz (or could not be put back).
	inline Result SelectHighFrameRate(const Ops& ops)
	{
		Result r;
		const int handle = ops.open(ops.ctx);
		r.open_code = handle;
		if (handle < 0)
		{
			r.outcome = Outcome::OpenFailed;
			return r;
		}
		r.supported = ops.supported(ops.ctx, handle, kSelectorHighFrameRate);
		if (r.supported <= 0)
		{
			r.outcome = Outcome::NotOffered; // never configured: an output that is not offered is not asked for
			ops.close(ops.ctx, handle);
			return r;
		}
		if (ops.marker_set)
			ops.marker_set(ops.ctx);
		r.configured = ops.configure(ops.ctx, handle, kSelectorHighFrameRate);
		if (r.configured != 0)
		{
			r.outcome = Outcome::Refused;
			if (ops.marker_clear)
				ops.marker_clear(ops.ctx);
			ops.close(ops.ctx, handle);
			return r;
		}
		r.period_ns = MedianVblankPeriod(ops, handle);
		if (r.period_ns == 0 || r.period_ns > kHighPeriodLimitNs)
		{
			r.outcome = r.period_ns == 0 ? Outcome::VblankFailed : Outcome::SlowVblank;
			r.restore_ran = true;
			r.restored = ops.configure(ops.ctx, handle, kSelectorRestore);
			if (r.restored == 0 && ops.marker_clear)
				ops.marker_clear(ops.ctx);
			ops.close(ops.ctx, handle);
			return r;
		}
		r.outcome = Outcome::Active;
		// (clamped: a wait that returns at once measures a period of a few ns, and a double above 2^32 is not a uint32_t; such a
		// rate is no display's, and the display rate's pick (OrbisDisplayHz.h) skips it)
		const double refresh_mhz = 1e12 / static_cast<double>(r.period_ns) + 0.5;
		r.refresh_mhz = refresh_mhz < 4e9 ? static_cast<uint32_t>(refresh_mhz) : 4'000'000'000u;
		// The console's high output is 119.88 Hz; a period near its 8.342 ms is that, not 118.2 Hz because one wait came late (the
		// pacing needs 1.97 refreshes of a 59.94 Hz game's frame: 118.1 Hz would take its room away).
		if (r.period_ns >= kHighPeriodMinNs && r.period_ns <= kHighPeriodMaxNs)
			r.refresh_mhz = kHighRefreshMilliHz;
		ops.close(ops.ctx, handle); // the output stays at 119.88 Hz; the driver's handle comes next
		ops.sleep_ms(ops.ctx, kReopenSettleMs);
		return r;
	}

	// The default output back, on a handle of its own. The 5 s of settling are not needed when the process is about to end.
	// `code` gets the open's or ConfigureOutput(1)'s answer. The marker goes when the output is back.
	inline bool RestoreDefault(const Ops& ops, int* code = nullptr)
	{
		const int handle = ops.open(ops.ctx);
		if (handle < 0)
		{
			if (code)
				*code = handle;
			return false;
		}
		const int answer = ops.configure(ops.ctx, handle, kSelectorRestore);
		ops.close(ops.ctx, handle);
		if (code)
			*code = answer;
		if (answer == 0 && ops.marker_clear)
			ops.marker_clear(ops.ctx);
		return answer == 0;
	}

	enum class BootCheck
	{
		NoMarker,      // the last run did not leave the output at 120 Hz (or never switched it)
		Restored,      // it did: the default output is back (and the 5 s settle has passed)
		RestoreFailed, // it did, and the output could not be put back: the marker stays for the next start
	};

	// At the start of the app, before the shelf: a marker means the last run ended without putting the output back. The default
	// output goes back first; the driver opens VideoOut in this very process next, so a restore that worked is followed by the
	// same settling as a closed 120 Hz output.
	inline BootCheck RestoreAtBoot(const Ops& ops, int* code = nullptr)
	{
		if (!ops.marker_present || !ops.marker_present(ops.ctx))
			return BootCheck::NoMarker;
		if (!RestoreDefault(ops, code))
			return BootCheck::RestoreFailed;
		ops.sleep_ms(ops.ctx, kReopenSettleMs);
		return BootCheck::Restored;
	}

	// How long a driver open still has to wait after the 120 Hz output was closed at `closed_ns` (0: never closed).
	inline uint32_t SettleLeftMs(uint64_t closed_ns, uint64_t now_ns)
	{
		if (closed_ns == 0)
			return 0;
		const uint64_t need = static_cast<uint64_t>(kReopenSettleMs) * 1'000'000;
		const uint64_t passed = now_ns > closed_ns ? now_ns - closed_ns : 0;
		if (passed >= need)
			return 0;
		return static_cast<uint32_t>((need - passed + 999'999) / 1'000'000);
	}

	// Flag videoprobe: IsOutputSupported for each selector, on one handle. Nothing is configured.
	struct ProbeRow
	{
		uint32_t selector;
		int answer;
	};

	inline std::vector<ProbeRow> Probe(const Ops& ops, int* open_code = nullptr)
	{
		std::vector<ProbeRow> rows;
		const int handle = ops.open(ops.ctx);
		if (open_code)
			*open_code = handle;
		if (handle < 0)
			return rows;
		for (uint32_t selector = 0; selector < kProbeSelectors; selector++)
			rows.push_back({selector, ops.supported(ops.ctx, handle, selector)});
		ops.close(ops.ctx, handle);
		return rows;
	}

	// ---- text for boot.log ----------------------------------------------------------------------------------------------------

	inline std::string Hex(uint32_t value)
	{
		char text[16];
		std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned>(value));
		return text;
	}

	// The system's code and what it means here, when it is one of the known ones.
	inline std::string CodeText(int code)
	{
		const uint32_t u = static_cast<uint32_t>(code);
		switch (u)
		{
			case kErrNotDeclared:
				return Hex(u) + " (the title does not declare high frame rates: param.json attribute3 0x80040)";
			case kErrNoFormat:
				return Hex(u) + " (the console's video output setting has no 120 Hz format at its resolution)";
			case kErrUnknownSelector:
				return Hex(u) + " (the console does not know this output)";
			default:
				break;
		}
		if (code < 0)
			return Hex(u);
		return std::to_string(code);
	}

	inline std::string Describe(const Result& r)
	{
		char text[400];
		switch (r.outcome)
		{
			case Outcome::Active:
				std::snprintf(text, sizeof(text),
					"119.88 Hz selected, a vblank every %.3f ms (%.2f Hz); VideoOut closed without restoring, the driver opens it again "
					"after %u s",
					r.period_ns / 1e6, r.refresh_mhz / 1000.0, static_cast<unsigned>(kReopenSettleMs / 1000));
				return text;
			case Outcome::NotOffered:
				return "the 120 Hz output is not offered (sceVideoOutIsOutputSupported(15) answered " + CodeText(r.supported) +
					   (r.supported == 0 ? ": the console's video output setting or the TV takes no 120 Hz format; 4K or 1440p at 120 Hz "
										   "is needed" :
										   "") +
					   "); the display stays at 59.94 Hz";
			case Outcome::Refused:
				return "119.88 Hz refused (sceVideoOutConfigureOutput(15) returned " + CodeText(r.configured) +
					   "); the display stays at 59.94 Hz";
			case Outcome::SlowVblank:
				std::snprintf(text, sizeof(text),
					"119.88 Hz accepted, but a vblank comes every %.3f ms: the display is not refreshing at 120 Hz, so the default "
					"output was put back (%s)",
					r.period_ns / 1e6, Hex(static_cast<uint32_t>(r.restored)).c_str());
				return text;
			case Outcome::VblankFailed:
				return "119.88 Hz accepted, but a vblank wait failed, so the default output was put back (" +
					   Hex(static_cast<uint32_t>(r.restored)) + ")";
			case Outcome::OpenFailed:
				break;
		}
		return "sceVideoOutOpen failed (" + CodeText(r.open_code) + "); the display stays at 59.94 Hz";
	}

	// The probe's answers, one line: for each distinct answer, the selectors that gave it.
	inline std::string DescribeProbe(const std::vector<ProbeRow>& rows, int open_code)
	{
		if (rows.empty())
			return "sceVideoOutOpen failed (" + CodeText(open_code) + "); no selector was asked about";
		std::vector<int> answers;
		for (const ProbeRow& row : rows)
			if (std::find(answers.begin(), answers.end(), row.answer) == answers.end())
				answers.push_back(row.answer);
		std::sort(answers.begin(), answers.end(), [](int a, int b) {
			if ((a > 0) != (b > 0))
				return a > 0;
			return static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
		});
		std::string text;
		for (const int answer : answers)
		{
			text += text.empty() ? "" : "; ";
			text += (answer > 0 ? "supported (" + std::to_string(answer) + ")" : "answer " + CodeText(answer)) + " for";
			for (const ProbeRow& row : rows)
				if (row.answer == answer)
					text += " " + std::to_string(row.selector);
		}
		return text;
	}
} // namespace orbis_vout

// ---- the app's side (OrbisVideoOutMode.cpp) ---------------------------------------------------------------------------------------
// All of it does nothing at all unless frame generation is on, the driver exports no output control, flags/fg120 is there and
// flags/nofg120 is not.

// At the start of the app, before the shelf: puts the default output back when the last run left a marker (and skips the switch in
// this run). Flag videoprobe: also asks the console about output selectors 0..63 (nothing is configured) and logs the answers.
void OrbisVideoOutBootCheck();
// After the game's settings are read (PS5SX2/FrameGeneration): starts the switch on a thread of its own when it is wanted.
void OrbisVideoOutStart(bool frame_generation_wanted);
// Before the display surface is made: waits for that thread (the swapchain is made after the 5 s) and, when the swapchain was
// closed since, for the rest of 5 s. Returns whether the output is at 119.88 Hz now.
bool OrbisVideoOutWait();
// The refresh of the output in millihertz when this switch made it 119.88 Hz, else 0.
uint32_t OrbisVideoOutRefreshMilliHz();
// The display surface was closed (the driver closes its VideoOut handle with its swapchain): the 5 s start now.
void OrbisVideoOutNoteSurfaceClosed();
// Puts the default output back when this switch changed it. Every way out of the process calls this (main-boot.cpp's
// orbis_output_default).
void OrbisVideoOutRestore();
// One line for boot.log and the frame generation log: what the switch did, or why it did not run.
std::string OrbisVideoOutSummary();
// vk-285-325: the linked driver's own output control (ps5vk's ps5vk_output_*, or PS5SX2's RADV's wsi_videoout_*). Its refresh of
// the output in millihertz (0: no such call, or the output not open), its last sentence on the output (nullptr: none), whether it
// has such control at all, and the default output put back (false: the driver has no call for it).
uint32_t OrbisDriverOutputMilliHz();
const char* OrbisDriverOutputStatus();
bool OrbisDriverHasOutputControl();
bool OrbisDriverRestoreOutput();
