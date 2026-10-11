// SPDX-FileCopyrightText: 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0+

// PS5 port, Vulkan build (vk-285-8): what the EE profiler (the port's orbis_eeprof.cpp) needs
// from the EE thread's waits. The profiler samples the EE thread only while this count is 0,
// so its samples are the thread's busy time and never land in an accounted wait (the [load]
// line's waitgs, ringfull, vsyncq, waitvu, vuring and throttle). Needs proper testing.

#pragma once

#include <atomic>

extern std::atomic<int> g_orbis_ee_waiting;

struct OrbisEEWaitScope
{
	OrbisEEWaitScope() { g_orbis_ee_waiting.fetch_add(1, std::memory_order_relaxed); }
	~OrbisEEWaitScope() { g_orbis_ee_waiting.fetch_sub(1, std::memory_order_relaxed); }
	OrbisEEWaitScope(const OrbisEEWaitScope&) = delete;
	OrbisEEWaitScope& operator=(const OrbisEEWaitScope&) = delete;
};
