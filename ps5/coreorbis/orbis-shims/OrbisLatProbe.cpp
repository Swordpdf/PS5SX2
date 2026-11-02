// PS5SX2 (test build 186, AI-assisted): how long a presented frame waits before the display shows it, with the flag file latprobe.
//
// swordpdf, 2026-11-02: input delay in Ratchet & Clank "that wasn't there before". By feel: 2.03 right, 2.15 "a little slower", 2.20 / 185 /
// 186 delayed, and 186 linked with the driver of 2.10 (libps5vk before 964919b914's 120 Hz output work) "I think it's gone". So:
// measure it. Every vkQueuePresentKHR (GSDeviceVK::SubmitCommandBuffer) counts a present and asks VideoOut for its flip status
// (ps5-payload-sdk's ps5platform/videoout.h: 16 words; word 0 the flips shown, word 1 when the latest showed, word 3 its argument).
// The driver opens VideoOut with the swapchain, so the shown count of that handle starts with the swapchain's first present:
//   frames waiting = presents since then - flips shown (the frame just presented included: 1 is as short as it gets)
//   present -> shown = when the latest flip showed - when its present was made (the n-th flip being the n-th present)
// Every 2 s one "[lat]" line (boot.log; and a shorter one in settings.log, which a build whose stdout does not reach boot.log still writes): averages and maxima, plus the handle and the raw words the first time (to check the layout). A count that
// goes backwards (a new swapchain, VideoOut opened again) starts the counting over. Nothing is changed: it only looks.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <sys/stat.h>

extern "C" int sceVideoOutGetFlipStatus(int32_t handle, uint64_t status[16]);
extern "C" unsigned long sceKernelGetProcessTime(void); // microseconds since the process started
bool OrbisFlag(const char* name) __attribute__((weak));
extern "C" void orbis_event_log(const char* line) __attribute__((weak)); // settings.log, where the perf lines go

namespace
{
constexpr int kRing = 512;

struct LatProbe
{
	int enabled = -1;
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
};

LatProbe g_lat;

int FindHandle(uint64_t st[16])
{
	for (int h = 0; h < 64; h++)
		if (sceVideoOutGetFlipStatus(h, st) == 0)
			return h;
	return -1;
}
} // namespace

void OrbisLatProbePresent()
{
	LatProbe& p = g_lat;
	if (p.enabled < 0)
	{
		// read once, at the first present (after the jailbreak), from the file itself: OrbisFlag answers from a snapshot made before the jailbreak,
		// which is empty when the sandbox shows no /data then (ShadowMount+ busy, 2026-11-02 23:21 on the owner's console)
		struct stat st_flag = {};
		p.enabled = stat("/data/PCSX2/flags/latprobe", &st_flag) == 0 || (OrbisFlag && OrbisFlag("latprobe")) ? 1 : 0;
	}
	if (!p.enabled)
		return;
	const uint64_t now_us = sceKernelGetProcessTime();
	uint64_t st[16] = {};
	if (p.handle < 0 || sceVideoOutGetFlipStatus(p.handle, st) != 0)
	{
		const int was = p.handle;
		p.handle = FindHandle(st);
		p.based = false;
		if (p.handle < 0)
		{
			if (!p.said_none)
				std::printf("[lat] no VideoOut handle answers sceVideoOutGetFlipStatus (0..63): nothing measured\n");
			p.said_none = true;
			return;
		}
		std::printf("[lat] measuring on VideoOut handle %d%s\n", p.handle, was >= 0 ? " (the earlier one stopped answering)" : "");
		std::fflush(stdout);
	}
	const uint64_t shown = st[0];
	if (!p.said_words)
	{
		p.said_words = true;
		std::printf("[lat] flip status words 0-7: %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " 0x%016" PRIx64 " 0x%016" PRIx64
					"; process time now %" PRIu64 " us\n", st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], now_us);
		std::fflush(stdout);
	}
	if (!p.based || shown < p.last_shown)
	{
		// the handle's flips start with the swapchain: the presents are counted from here, this one being the first
		if (p.based)
			std::printf("[lat] the shown count went back (%" PRIu64 " -> %" PRIu64 "): a new swapchain, counted from here\n", p.last_shown, shown);
		p.based = true;
		p.presents = 0;
		p.base_shown = shown;
	}
	p.presents++;
	p.present_us[p.presents % kRing] = now_us;
	const uint64_t shown_since = shown - p.base_shown;
	const uint64_t waiting = p.presents > shown_since ? p.presents - shown_since : 0;
	p.n++;
	p.wait_sum += waiting;
	if (waiting > p.wait_max)
		p.wait_max = waiting;
	const int64_t pending = static_cast<int32_t>(st[6] >> 32); // VideoOut's own count of flips queued and not shown (PS4's layout)
	p.pending_sum += pending;
	if (pending > p.pending_max)
		p.pending_max = pending;
	if (shown != p.last_shown && shown_since >= 1 && shown_since <= p.presents && p.presents - shown_since < kRing)
	{
		const uint64_t made = p.present_us[shown_since % kRing], at = st[1];
		if (at >= made && at - made < 2000000)
		{
			const uint64_t lat = at - made;
			p.lat_n++;
			p.lat_sum_us += lat;
			if (lat > p.lat_max_us)
				p.lat_max_us = lat;
			if (lat < p.lat_min_us)
				p.lat_min_us = lat;
		}
	}
	p.last_shown = shown;
	if (p.window_start_us == 0)
		p.window_start_us = now_us;
	if (now_us - p.window_start_us >= 2000000 && p.n)
	{
		std::printf("[lat] %" PRIu64 " presents: frames waiting avg %.2f max %" PRIu64 " (VideoOut's pending avg %.2f max %" PRId64 "); present -> shown "
					"%s\n",
			p.n, static_cast<double>(p.wait_sum) / p.n, p.wait_max, static_cast<double>(p.pending_sum) / p.n, p.pending_max,
			p.lat_n ? ([&] {
				static char text[96];
				std::snprintf(text, sizeof(text), "avg %.1f ms, min %.1f, max %.1f (%" PRIu64 " frames)", p.lat_sum_us / 1000.0 / p.lat_n,
					p.lat_min_us / 1000.0, p.lat_max_us / 1000.0, p.lat_n);
				return static_cast<const char*>(text);
			}())
					 : "not measured (no flip matched a present)");
		std::fflush(stdout);
		if (orbis_event_log)
		{
			char line[200];
			std::snprintf(line, sizeof(line), "latprobe: frames waiting avg %.2f max %" PRIu64 ", present -> shown avg %.1f ms max %.1f (%" PRIu64 " frames)",
				static_cast<double>(p.wait_sum) / p.n, p.wait_max, p.lat_n ? p.lat_sum_us / 1000.0 / p.lat_n : 0.0, p.lat_max_us / 1000.0, p.lat_n);
			orbis_event_log(line);
		}
		p.window_start_us = now_us;
		p.n = p.wait_sum = p.wait_max = p.lat_n = p.lat_sum_us = p.lat_max_us = 0;
		p.lat_min_us = UINT64_MAX;
		p.pending_sum = p.pending_max = 0;
	}
}
