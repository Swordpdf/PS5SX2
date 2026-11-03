// PS5SX2 (test build 186, AI-assisted): how long a presented frame waits before the display shows it (flag file latprobe), and the cap
// on that wait (on unless the flag file nolatcap is there).
//
// swordpdf, 2026-11-02: input delay in Ratchet & Clank "that wasn't there before". By feel: 2.03 right, 2.15 "a little slower", 2.20 / 185 /
// 186 delayed, and 186 linked with the driver of 2.10 (libps5vk before 964919b914's 120 Hz output work) "I think it's gone". So:
// measure it. Every vkQueuePresentKHR (GSDeviceVK::SubmitCommandBuffer) counts a present and asks VideoOut for its flip status
// (ps5-payload-sdk's ps5platform/videoout.h: 16 words; word 0 the flips shown, word 1 when the latest showed, word 3 its argument).
// The driver opens VideoOut with the swapchain, so the shown count of that handle starts with the swapchain's first present:
//   frames waiting = presents since then - flips shown (the frame just presented included: 1 is as short as it gets)
//   present -> shown = when the latest flip showed - when its present was made (the n-th flip being the n-th present)
// Every 2 s one "[lat]" line (boot.log; and a shorter one in settings.log, which a build whose stdout does not reach boot.log still writes): averages and maxima, plus the handle and the raw words the first time (to check the layout). A count that
// goes backwards (a new swapchain, VideoOut opened again) starts the counting over.
//
// What it measured (2026-11-03, Ratchet & Clank at 6x, 119.88 Hz): 3 frames waiting and 31-43 ms from present to screen while frames
// were generated, 1-2 and 14-20 ms while not. VideoOut itself holds at most 1 flip: the third frame waits in the driver's own queue.
// The cap (OrbisLatCapBeforePresent, right before vkQueuePresentKHR): while more than 1 frame presented earlier is not on the screen
// yet, the present waits (0.25 ms steps, 25 ms at most). The game's thread waits there instead of running a frame further ahead of the
// screen, so what is shown is never more than 2 frames behind it (the frame being presented and 1 before). Flags: nolatcap (off),
// latcap0 (0 before: only the frame being presented waits), latcap2 (2 before). 3 waits in a row that run out mean the count is off (a
// present that never became a flip): it is taken again from what VideoOut has shown.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <sys/stat.h>

extern "C" int sceVideoOutGetFlipStatus(int32_t handle, uint64_t status[16]);
// the driver's own word for the VideoOut handle the device presents through (driver/ps5vk_debug.h; -1 for none): the console's handles are not
// the small numbers a search finds (0..63 answered nothing on the owner's console)
extern "C" int ps5vk_debug_video_handle(void* device) __attribute__((weak));
extern "C" unsigned long sceKernelGetProcessTime(void); // microseconds since the process started
extern "C" int sceKernelUsleep(unsigned microseconds);
bool OrbisFlag(const char* name) __attribute__((weak));
extern "C" void orbis_event_log(const char* line) __attribute__((weak)); // settings.log, where the perf lines go
// GSDeviceVK.cpp: frames are being generated. The cap stands aside then: a generated pair needs the GS thread a frame ahead of the screen
// (swordpdf's original PS5 at firmware 4.03, Ratchet & Clank at 6x, 2026-11-03: 120 presents a second for 172 of 177 s without the cap,
// 25 of 49 s with it), and frame generation's own frame of delay is the price of its smoothness anyway.
extern std::atomic<bool> g_orbis_fg_active;

namespace
{
constexpr int kRing = 512;
constexpr uint64_t kCapMaxWaitUs = 25000; // a present never waits longer (3 refreshes at 120 Hz, 1.5 at 60)
constexpr uint64_t kCapStepUs = 250;
constexpr int kCapRecountAfter = 3;       // waits in a row that ran out: the count is taken again
constexpr uint64_t kCapLineUs = 30000000; // the cap's own line without the probe: every 30 s

struct LatProbe
{
	int enabled = -1; // the probe's lines (latprobe); -1: the flags not read yet
	int cap = -1;     // frames presented earlier and not shown yet that a present may leave waiting; -1 off
	int handle = -1;
	bool said_none = false, said_words = false;
	uint64_t presents = 0, base_shown = 0;
	bool based = false;
	uint64_t present_us[kRing] = {};
	uint64_t last_shown = 0;
	// the 2-second window
	uint64_t window_start_us = 0;
	uint64_t n = 0, wait_sum = 0, wait_max = 0, lat_n = 0, lat_sum_us = 0, lat_max_us = 0, lat_min_us = UINT64_MAX;
	int64_t pending_sum = 0, pending_max = 0;
	// the cap, since its last line (the probe's every 2 s, else its own every 30 s)
	uint64_t cap_presents = 0, cap_waits = 0, cap_wait_us = 0, cap_wait_max_us = 0, cap_timeouts = 0, cap_line_us = 0, cap_fg = 0;
	int timeouts_row = 0, recounts = 0;
};

LatProbe g_lat;

bool FlagFile(const char* name)
{
	// from the file itself: OrbisFlag answers from a snapshot made before the jailbreak, which is empty when the sandbox shows no /data
	// then (ShadowMount+ busy, 2026-11-02 23:21 on the owner's console)
	char path[96];
	std::snprintf(path, sizeof(path), "/data/PCSX2/flags/%s", name);
	struct stat st = {};
	return stat(path, &st) == 0 || (OrbisFlag && OrbisFlag(name));
}

// Once, at the first present (after the jailbreak).
void ReadFlags(LatProbe& p)
{
	if (p.enabled >= 0)
		return;
	p.enabled = FlagFile("latprobe") ? 1 : 0;
	p.cap = FlagFile("nolatcap") ? -1 : FlagFile("latcap0") ? 0 : FlagFile("latcap2") ? 2 : 1;
	if (p.cap < 0)
		std::printf("[latcap] off (flag nolatcap): presents never wait for the display\n");
	else
		std::printf("[latcap] on: a present waits while more than %d frame(s) presented before it are not on the screen yet (up to %u ms; "
					"flags nolatcap, latcap0, latcap2)\n", p.cap, static_cast<unsigned>(kCapMaxWaitUs / 1000));
	std::fflush(stdout);
}

int FindHandle(uint64_t st[16])
{
	for (int h = 0; h < 64; h++)
		if (sceVideoOutGetFlipStatus(h, st) == 0)
			return h;
	return -1;
}

// The cap's numbers since its last line, as text (empty when it is off).
const char* CapText(LatProbe& p)
{
	static char text[200];
	text[0] = '\0';
	if (p.cap < 0)
		return text;
	std::snprintf(text, sizeof(text), "cap %d: waited before %" PRIu64 " of %" PRIu64 " presents, avg %.1f ms, most %.1f; %" PRIu64 " ran out%s; "
		"%" PRIu64 " presents while frames were generated (no cap)",
		p.cap, p.cap_waits, p.cap_presents, p.cap_waits ? p.cap_wait_us / 1000.0 / p.cap_waits : 0.0, p.cap_wait_max_us / 1000.0,
		p.cap_timeouts, p.recounts ? " (counted again)" : "", p.cap_fg);
	p.cap_presents = p.cap_waits = p.cap_wait_us = p.cap_wait_max_us = p.cap_timeouts = p.cap_fg = 0;
	return text;
}
} // namespace

void OrbisLatCapBeforePresent(void* /*vk_device*/)
{
	LatProbe& p = g_lat;
	ReadFlags(p);
	if (p.cap < 0 || p.handle < 0 || !p.based)
		return;
	if (g_orbis_fg_active.load(std::memory_order_relaxed))
	{
		p.cap_fg++;
		p.timeouts_row = 0;
		return;
	}
	uint64_t st[16] = {};
	// a handle that stopped answering or a count that went back: OrbisLatProbePresent finds the handle again or counts again
	if (sceVideoOutGetFlipStatus(p.handle, st) != 0 || st[0] < p.last_shown)
		return;
	// presented before this one and not shown yet (modular: the count taken again may have put base_shown above shown)
	const auto queued = [&](uint64_t shown) { return static_cast<int64_t>(p.presents - (shown - p.base_shown)); };
	int64_t q = queued(st[0]);
	p.cap_presents++;
	if (q <= p.cap)
	{
		p.timeouts_row = 0;
		return;
	}
	const uint64_t t0 = sceKernelGetProcessTime();
	uint64_t now = t0;
	bool ran_out = false;
	while (q > p.cap)
	{
		if (now - t0 >= kCapMaxWaitUs)
		{
			ran_out = true;
			break;
		}
		sceKernelUsleep(static_cast<unsigned>(kCapStepUs));
		now = sceKernelGetProcessTime();
		if (sceVideoOutGetFlipStatus(p.handle, st) != 0 || st[0] < p.last_shown)
			break;
		q = queued(st[0]);
	}
	const uint64_t waited = now - t0;
	p.cap_waits++;
	p.cap_wait_us += waited;
	if (waited > p.cap_wait_max_us)
		p.cap_wait_max_us = waited;
	if (!ran_out)
	{
		p.timeouts_row = 0;
		return;
	}
	p.cap_timeouts++;
	if (++p.timeouts_row < kCapRecountAfter)
		return;
	// the count is off (a present that never became a flip, or flips VideoOut dropped): what is not shown yet is taken as `cap` frames
	p.base_shown = st[0] + static_cast<uint64_t>(p.cap) - p.presents;
	p.timeouts_row = 0;
	if (p.recounts++ < 5)
	{
		std::printf("[latcap] %d waits in a row ran out (%u ms each): the frames waiting are counted again from the display's (%" PRIu64
					" shown, %" PRIu64 " presented)\n", kCapRecountAfter, static_cast<unsigned>(kCapMaxWaitUs / 1000), st[0], p.presents);
		std::fflush(stdout);
	}
}

void OrbisLatProbePresent(void* vk_device)
{
	LatProbe& p = g_lat;
	ReadFlags(p);
	if (!p.enabled && p.cap < 0)
		return;
	const uint64_t now_us = sceKernelGetProcessTime();
	uint64_t st[16] = {};
	if (p.handle < 0 || sceVideoOutGetFlipStatus(p.handle, st) != 0)
	{
		const int was = p.handle;
		p.handle = -1;
		if (ps5vk_debug_video_handle && vk_device)
		{
			const int h = ps5vk_debug_video_handle(vk_device);
			if (h >= 0 && sceVideoOutGetFlipStatus(h, st) == 0)
				p.handle = h;
		}
		if (p.handle < 0)
			p.handle = FindHandle(st);
		p.based = false;
		if (p.handle < 0)
		{
			if (!p.said_none)
				std::printf("[lat] no VideoOut handle answers sceVideoOutGetFlipStatus (the driver's: %d; 0..63): nothing measured%s\n",
					ps5vk_debug_video_handle && vk_device ? ps5vk_debug_video_handle(vk_device) : -2, p.cap >= 0 ? ", and no cap" : "");
			p.said_none = true;
			return;
		}
		std::printf("[lat] counting on VideoOut handle %d%s\n", p.handle, was >= 0 ? " (the earlier one stopped answering)" : "");
		std::fflush(stdout);
	}
	const uint64_t shown = st[0];
	if (p.enabled && !p.said_words)
	{
		p.said_words = true;
		std::printf("[lat] flip status words 0-7: %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " 0x%016" PRIx64 " 0x%016" PRIx64
					"; process time now %" PRIu64 " us\n", st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], now_us);
		std::fflush(stdout);
	}
	if (!p.based || shown < p.last_shown)
	{
		// the handle's flips start with the swapchain: the presents are counted from here, this one being the first
		if (p.based && p.enabled)
			std::printf("[lat] the shown count went back (%" PRIu64 " -> %" PRIu64 "): a new swapchain, counted from here\n", p.last_shown, shown);
		p.based = true;
		p.presents = 0;
		p.base_shown = shown;
		p.timeouts_row = 0;
	}
	p.presents++;
	p.last_shown = shown;
	if (!p.enabled)
	{
		// the cap alone: its line every 30 s
		if (p.cap_line_us == 0)
			p.cap_line_us = now_us;
		if (now_us - p.cap_line_us >= kCapLineUs)
		{
			std::printf("[latcap] last %u s: %s\n", static_cast<unsigned>((now_us - p.cap_line_us) / 1000000), CapText(p));
			std::fflush(stdout);
			p.cap_line_us = now_us;
		}
		return;
	}
	p.present_us[p.presents % kRing] = now_us;
	const uint64_t shown_since = shown - p.base_shown;
	const uint64_t waiting = static_cast<int64_t>(p.presents - shown_since) > 0 ? p.presents - shown_since : 0;
	p.n++;
	p.wait_sum += waiting;
	if (waiting > p.wait_max)
		p.wait_max = waiting;
	const int64_t pending = static_cast<int32_t>(st[6] >> 32); // VideoOut's own count of flips queued and not shown (PS4's layout)
	p.pending_sum += pending;
	if (pending > p.pending_max)
		p.pending_max = pending;
	if (static_cast<int64_t>(shown_since) >= 1 && shown_since <= p.presents && p.presents - shown_since < kRing)
	{
		static uint64_t s_last_matched = 0;
		const uint64_t made = p.present_us[shown_since % kRing], at = st[1];
		if (shown_since != s_last_matched && at >= made && at - made < 2000000)
		{
			s_last_matched = shown_since;
			const uint64_t lat = at - made;
			p.lat_n++;
			p.lat_sum_us += lat;
			if (lat > p.lat_max_us)
				p.lat_max_us = lat;
			if (lat < p.lat_min_us)
				p.lat_min_us = lat;
		}
	}
	if (p.window_start_us == 0)
		p.window_start_us = now_us;
	if (now_us - p.window_start_us >= 2000000 && p.n)
	{
		char lat_text[96];
		if (p.lat_n)
			std::snprintf(lat_text, sizeof(lat_text), "avg %.1f ms, min %.1f, max %.1f (%" PRIu64 " frames)", p.lat_sum_us / 1000.0 / p.lat_n,
				p.lat_min_us / 1000.0, p.lat_max_us / 1000.0, p.lat_n);
		else
			std::snprintf(lat_text, sizeof(lat_text), "not measured (no flip matched a present)");
		const char* cap_text = CapText(p);
		std::printf("[lat] %" PRIu64 " presents: frames waiting avg %.2f max %" PRIu64 " (VideoOut's pending avg %.2f max %" PRId64 "); present -> shown "
					"%s%s%s\n",
			p.n, static_cast<double>(p.wait_sum) / p.n, p.wait_max, static_cast<double>(p.pending_sum) / p.n, p.pending_max, lat_text,
			cap_text[0] ? "; " : "", cap_text);
		std::fflush(stdout);
		if (orbis_event_log)
		{
			char line[320];
			std::snprintf(line, sizeof(line), "latprobe: frames waiting avg %.2f max %" PRIu64 ", present -> shown avg %.1f ms max %.1f (%" PRIu64 " frames)%s%s",
				static_cast<double>(p.wait_sum) / p.n, p.wait_max, p.lat_n ? p.lat_sum_us / 1000.0 / p.lat_n : 0.0, p.lat_max_us / 1000.0, p.lat_n,
				cap_text[0] ? "; " : "", cap_text);
			orbis_event_log(line);
		}
		p.window_start_us = now_us;
		p.n = p.wait_sum = p.wait_max = p.lat_n = p.lat_sum_us = p.lat_max_us = 0;
		p.lat_min_us = UINT64_MAX;
		p.pending_sum = p.pending_max = 0;
	}
}
