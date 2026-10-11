// SPDX-FileCopyrightText: 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// PS5 port: the GS thread's time in Vulkan calls, by kind -- nanoseconds, calls and the longest
// single call since main-boot's ticker last took it -- which the ticker prints once a second:
// kinds 0-6 as [vkwait] (vk-285-36), kinds 7 and 8 as [shaders] (vk-285-38).
//   0 a command buffer's fence before it is reused (ActivateCommandBuffer)
//   1 an explicit wait after a submission (ExecuteCommandBuffer)
//   2 a fence counter (stream buffers, texture copies: WaitForFenceCounter)
//   3 vkAcquireNextImageKHR (VKSwapChain.cpp)
//   4 vkQueueSubmit
//   5 vkQueuePresentKHR
//   6 vkDeviceWaitIdle
//   7 vkCreateGraphicsPipelines and vkCreateComputePipelines (VKBuilders.cpp): the driver's shader
//     cache lookups, plus its compiles and stores when the cache lacks a stage
//   8 GLSL to SPIR-V (VKShaderCache::CompileShaderToSPV), when PCSX2's own SPIR-V cache lacks it
// Plain counters: the GS thread is their one writer, and a torn read costs one log line.

#ifdef ORBIS_VULKAN

#include <chrono>

inline constexpr int ORBIS_VKW_KINDS = 10;
extern unsigned long long g_orbis_vkw_ns[ORBIS_VKW_KINDS], g_orbis_vkw_n[ORBIS_VKW_KINDS],
	g_orbis_vkw_max_ns[ORBIS_VKW_KINDS];

struct OrbisVkWaitTimer
{
	int kind;
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	explicit OrbisVkWaitTimer(int k)
		: kind(k)
	{
	}
	~OrbisVkWaitTimer()
	{
		const unsigned long long ns = static_cast<unsigned long long>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
		g_orbis_vkw_ns[kind] += ns;
		g_orbis_vkw_n[kind]++;
		if (ns > g_orbis_vkw_max_ns[kind])
			g_orbis_vkw_max_ns[kind] = ns;
	}
};
#define ORBIS_VKW(kind) OrbisVkWaitTimer orbis_vkw_timer_(kind)

// vk-285-39: GSDeviceVK::CopyRect's copies by kind -- counts and bytes, which main-boot prints as
// the [copies] line: the ones made as a convert draw, and the image copies (the driver's CPU copy)
// between render targets, from a render target into a texture, between depth buffers, from a depth
// buffer into a texture, and every other pair.
enum : int
{
	ORBIS_COPY_DRAW,
	ORBIS_COPY_RT_RT,
	ORBIS_COPY_RT_TEX,
	ORBIS_COPY_DS_DS,
	ORBIS_COPY_DS_TEX,
	ORBIS_COPY_OTHER,
	ORBIS_COPY_KINDS,
};
extern unsigned long long g_orbis_copy_n[ORBIS_COPY_KINDS], g_orbis_copy_bytes[ORBIS_COPY_KINDS];

// vk-285-113: the GS's readbacks (GSDownloadTextureVK): the copies recorded (count, bytes), and the GS thread's waits for them
// (GSDownloadTextureVK::Flush: one that submitted the command buffer or waited on its fence) with the longest since the ticker
// last took it. Main-boot prints them as [readbacks] and the settings log's minute line sums them. Guitar Hero II and III and
// OutRun 2006 read their frame back every frame, and each one stalls the GPU pipeline. The GS thread is the one writer.
extern unsigned long long g_orbis_readback_n, g_orbis_readback_bytes, g_orbis_readback_wait_ns, g_orbis_readback_wait_n,
	g_orbis_readback_wait_max_ns, g_orbis_readback_wait_max_min_ns; // the last: the longest of the settings log's minute

struct OrbisReadbackWaitTimer
{
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	~OrbisReadbackWaitTimer()
	{
		const unsigned long long ns = static_cast<unsigned long long>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
		g_orbis_readback_wait_ns += ns;
		g_orbis_readback_wait_n++;
		if (ns > g_orbis_readback_wait_max_ns)
			g_orbis_readback_wait_max_ns = ns;
		if (ns > g_orbis_readback_wait_max_min_ns)
			g_orbis_readback_wait_max_min_ns = ns;
	}
};

#else

#define ORBIS_VKW(kind) \
	do \
	{ \
	} while (0)

#endif
