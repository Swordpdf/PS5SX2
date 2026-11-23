// PS5SX2 (2.11, AI-assisted): the app's own switch of the console's VideoOut to its 119.88 Hz output, for frame generation. The
// sequence, the reasons for it and the rules are in OrbisVideoOutMode.h; this file has the real system calls, the thread, the
// marker file and the log lines. Opt-in (flags/fg120), NOT TESTED ON A CONSOLE.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisVideoOutMode.h"

#include "OrbisDriver.h"
#include "OrbisPaths.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

extern "C"
{
	// As demo_renderer.cpp and the driver declare them (the SDK has no header for the output calls).
	int sceVideoOutOpen(int32_t user_id, int32_t bus_type, int32_t index, const void* param);
	int sceVideoOutClose(int32_t handle);
	int sceVideoOutWaitVblank(int32_t handle);
	int sceVideoOutIsOutputSupported(int32_t handle, uint32_t mode, const void* a, const void* b, const void* c);
	int sceVideoOutConfigureOutput(int32_t handle, uint32_t mode, const void* a, const void* b, const void* c);

	// A driver that has its own output control (the 2.02 build's did) exports these; then it does the switch and this does nothing.
	uint32_t ps5vk_output_refresh_millihertz(void) __attribute__((weak));
	const char* ps5vk_output_status(void) __attribute__((weak));
	bool ps5vk_output_restore_default(void) __attribute__((weak));
	// vk-285-325: PS5SX2's RADV (Mihawk's PS5 Mesa with our patches, wsi_common_videoout.c) has its own too: the output follows the
	// mode of each swapchain, and these say how it went.
	uint32_t wsi_videoout_refresh_millihertz(void) __attribute__((weak));
	const char* wsi_videoout_output_status(void) __attribute__((weak));
	void wsi_videoout_restore_default(void) __attribute__((weak));
}

uint32_t OrbisDriverOutputMilliHz()
{
	if (ps5vk_output_refresh_millihertz)
		return ps5vk_output_refresh_millihertz();
	if (wsi_videoout_refresh_millihertz)
		return wsi_videoout_refresh_millihertz();
	return 0;
}

const char* OrbisDriverOutputStatus()
{
	if (ps5vk_output_status)
		return ps5vk_output_status();
	if (wsi_videoout_output_status)
		return wsi_videoout_output_status();
	return nullptr;
}

bool OrbisDriverHasOutputControl()
{
	return ps5vk_output_status || ps5vk_output_refresh_millihertz || ps5vk_output_restore_default || wsi_videoout_refresh_millihertz;
}

bool OrbisDriverRestoreOutput()
{
	if (ps5vk_output_restore_default)
	{
		ps5vk_output_restore_default();
		return true;
	}
	if (wsi_videoout_restore_default)
	{
		wsi_videoout_restore_default();
		return true;
	}
	return false;
}

namespace
{
	std::mutex s_mutex;              // the summary and the thread
	std::mutex s_wait_mutex;         // one joiner at a time
	std::thread* s_thread = nullptr; // never destroyed with the process: a thread still sleeping at exit() must not reach std::terminate
	bool s_started = false;
	bool s_skip_this_run = false;    // the last run left the output at 120 Hz: restored at the start, not switched again now
	std::string s_summary = "not run";
	std::atomic<bool> s_switched{false};
	std::atomic<uint32_t> s_mhz{0};
	std::atomic<uint64_t> s_closed_ns{0}; // when the display surface was last closed while the output was at 120 Hz

	struct Real
	{
		int open_tries = 3; // the 2.02 driver tried sceVideoOutOpen three times, 500 ms apart
	};

	uint64_t NowNs()
	{
		timespec ts = {};
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
	}

	void SleepMs(uint32_t ms)
	{
		for (uint32_t left = ms; left > 0;)
		{
			const uint32_t step = left < 100 ? left : 100;
			usleep(step * 1000);
			left -= step;
		}
	}

	std::string MarkerPath()
	{
		return OrbisLogPath("output-120.txt");
	}

	int RealOpen(void* ctx)
	{
		const int tries = static_cast<Real*>(ctx)->open_tries;
		int handle = -1;
		for (int attempt = 1; attempt <= tries; attempt++)
		{
			handle = sceVideoOutOpen(0xff, 0, 0, nullptr);
			if (handle >= 0)
				break;
			printf("[videoout] sceVideoOutOpen failed: 0x%08x (try %d of %d)\n", static_cast<unsigned>(handle), attempt, tries);
			fflush(stdout);
			if (attempt < tries)
				SleepMs(500);
		}
		return handle;
	}

	int RealClose(void*, int handle)
	{
		return sceVideoOutClose(handle);
	}

	int RealSupported(void*, int handle, uint32_t selector)
	{
		return sceVideoOutIsOutputSupported(handle, selector, nullptr, nullptr, nullptr);
	}

	int RealConfigure(void*, int handle, uint32_t selector)
	{
		return sceVideoOutConfigureOutput(handle, selector, nullptr, nullptr, nullptr);
	}

	int RealWaitVblank(void*, int handle)
	{
		return sceVideoOutWaitVblank(handle);
	}

	uint64_t RealNow(void*)
	{
		return NowNs();
	}

	void RealSleep(void*, uint32_t ms)
	{
		SleepMs(ms);
	}

	bool MarkerPresent(void*)
	{
		struct stat st = {};
		return stat(MarkerPath().c_str(), &st) == 0;
	}

	void MarkerSet(void*)
	{
		if (FILE* file = fopen(MarkerPath().c_str(), "w"))
		{
			fputs("PS5SX2 set the console's video output to 119.88 Hz (flag fg120) and has not put it back yet.\n"
				  "The next start puts the default output back and deletes this file.\n",
				file);
			fclose(file);
		}
	}

	void MarkerClear(void*)
	{
		unlink(MarkerPath().c_str());
	}

	orbis_vout::Ops RealOps(Real* real)
	{
		orbis_vout::Ops ops;
		ops.ctx = real;
		ops.open = RealOpen;
		ops.close = RealClose;
		ops.supported = RealSupported;
		ops.configure = RealConfigure;
		ops.wait_vblank = RealWaitVblank;
		ops.now_ns = RealNow;
		ops.sleep_ms = RealSleep;
		ops.marker_present = MarkerPresent;
		ops.marker_set = MarkerSet;
		ops.marker_clear = MarkerClear;
		return ops;
	}

	void SetSummary(const std::string& text)
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		s_summary = text;
	}

	// Says why the switch does not run, in boot.log and for the frame generation log.
	void NotRun(const std::string& why, bool log)
	{
		SetSummary("[videoout] not switched: " + why);
		if (log)
		{
			printf("[videoout] not switched: %s\n", why.c_str());
			fflush(stdout);
		}
	}

	void Worker()
	{
		Real real;
		const orbis_vout::Ops ops = RealOps(&real);
		printf("[videoout] frame generation is on and the driver has no output control: switching VideoOut to 119.88 Hz (flag fg120), "
			   "then waiting %u s before the driver opens it\n",
			static_cast<unsigned>(orbis_vout::kReopenSettleMs / 1000));
		fflush(stdout);
		const orbis_vout::Result result = orbis_vout::SelectHighFrameRate(ops);
		const std::string text = orbis_vout::Describe(result);
		SetSummary("[videoout] " + text);
		printf("[videoout] %s\n", text.c_str());
		fflush(stdout);
		if (result.outcome == orbis_vout::Outcome::Active)
		{
			s_mhz.store(result.refresh_mhz, std::memory_order_relaxed);
			s_switched.store(true, std::memory_order_release);
		}
	}

	// The thread, if it is still running: whoever needs its answer waits for it.
	void JoinWorker()
	{
		std::lock_guard<std::mutex> wait_lock(s_wait_mutex);
		std::thread* worker = nullptr;
		{
			std::lock_guard<std::mutex> lock(s_mutex);
			worker = s_thread;
			s_thread = nullptr;
		}
		if (worker)
		{
			if (worker->joinable())
				worker->join();
			delete worker;
		}
	}

	// The driver's sceVideoOutOpen, and our restore's, must not come within 5 s of the 120 Hz output's handle closing.
	void SettleAfterClose(const char* before)
	{
		const uint64_t closed = s_closed_ns.exchange(0, std::memory_order_acq_rel);
		const uint32_t left = orbis_vout::SettleLeftMs(closed, NowNs());
		if (left == 0)
			return;
		printf("[videoout] waiting %.1f s before %s (the 120 Hz output closed)\n", left / 1000.0, before);
		fflush(stdout);
		SleepMs(left);
	}
} // namespace

void OrbisVideoOutBootCheck()
{
	Real real;
	const orbis_vout::Ops ops = RealOps(&real);
	int code = 0;
	switch (orbis_vout::RestoreAtBoot(ops, &code))
	{
		case orbis_vout::BootCheck::NoMarker:
			break;
		case orbis_vout::BootCheck::Restored:
			s_skip_this_run = true;
			printf("[videoout] the last run left the output at 119.88 Hz (logs/output-120.txt): the default output is back, and %u s "
				   "passed before the driver opens VideoOut; no switch in this run\n",
				static_cast<unsigned>(orbis_vout::kReopenSettleMs / 1000));
			break;
		case orbis_vout::BootCheck::RestoreFailed:
			s_skip_this_run = true;
			printf("[videoout] the last run left the output at 119.88 Hz (logs/output-120.txt) and putting the default output back "
				   "failed (%s); restart the console if the display stays at 120 Hz\n",
				orbis_vout::CodeText(code).c_str());
			break;
	}
	fflush(stdout);

	if (OrbisFlag("videoprobe"))
	{
		int open_code = 0;
		const std::vector<orbis_vout::ProbeRow> rows = orbis_vout::Probe(ops, &open_code);
		printf("[videoout] probe (flag videoprobe; nothing is configured): %s\n", orbis_vout::DescribeProbe(rows, open_code).c_str());
		fflush(stdout);
	}
}

void OrbisVideoOutStart(bool frame_generation_wanted)
{
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (s_started)
			return;
		s_started = true;
	}
	if (!frame_generation_wanted)
	{
		NotRun("frame generation is off", false);
		return;
	}
	if (OrbisDriverIsRADV())
	{
		NotRun("RADV lists its modes itself", true);
		return;
	}
	if (ps5vk_output_status || ps5vk_output_refresh_millihertz || ps5vk_output_restore_default)
	{
		NotRun("the driver has its own output control (ps5vk): its verdict is the [display] swapchain line", true);
		return;
	}
	if (OrbisFlag("nofg120"))
	{
		NotRun("flag nofg120", true);
		return;
	}
	if (!OrbisFlag("fg120"))
	{
		NotRun("the driver exports no output control (the 2.02 build's did), and the app's own switch is opt-in (flag fg120); a display "
			   "that stays at 60 Hz (see the [display] line) gives generated frames only to games at 30 fps or less",
			true);
		return;
	}
	if (s_skip_this_run)
	{
		NotRun("the last run ended with the output at 120 Hz; it was put back for this run", true);
		return;
	}
	std::lock_guard<std::mutex> lock(s_mutex);
	s_summary = "[videoout] switching to 119.88 Hz";
	s_thread = new std::thread(Worker);
}

bool OrbisVideoOutWait()
{
	JoinWorker();
	if (s_switched.load(std::memory_order_acquire))
		SettleAfterClose("the driver opens VideoOut again");
	return s_switched.load(std::memory_order_acquire);
}

uint32_t OrbisVideoOutRefreshMilliHz()
{
	return s_switched.load(std::memory_order_acquire) ? s_mhz.load(std::memory_order_relaxed) : 0;
}

void OrbisVideoOutNoteSurfaceClosed()
{
	if (s_switched.load(std::memory_order_acquire))
		s_closed_ns.store(NowNs(), std::memory_order_release);
}

void OrbisVideoOutRestore()
{
	JoinWorker();
	if (!s_switched.exchange(false, std::memory_order_acq_rel))
		return;
	s_mhz.store(0, std::memory_order_relaxed);
	SettleAfterClose("the default output is put back");
	Real real;
	real.open_tries = 2;
	int code = 0;
	const bool ok = orbis_vout::RestoreDefault(RealOps(&real), &code);
	printf("[videoout] the default output is %s (%s)\n", ok ? "back" : "NOT back; the next start tries again",
		ok ? "0" : orbis_vout::CodeText(code).c_str());
	fflush(stdout);
	SetSummary(std::string("[videoout] switched to 119.88 Hz, then ") + (ok ? "put back" : "NOT put back"));
}

std::string OrbisVideoOutSummary()
{
	std::lock_guard<std::mutex> lock(s_mutex);
	return s_summary;
}
