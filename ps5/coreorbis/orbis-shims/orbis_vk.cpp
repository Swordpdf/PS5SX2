// PS5 port, Vulkan build: stand-ins for hooks that only the GL build implements,
// and a survey of the driver's GPU address window.
//
// The GL build defines the hooks in GSDeviceOGL.cpp and the GL shims, which the
// Vulkan build does not compile (Makefile.vk). Each is reached only on a GL device,
// so on GSDeviceVK they are never called; they exist so the link resolves.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sys/types.h>

class GSTexture;

// GSRenderer.cpp: the periodic GL/GS readback dump (live.ini diag=1 on the GL presenter).
void OrbisPresentGLFrame()
{
}

// GSRenderer.cpp / GSRendererSW.cpp (eerec-279/280): GL readbacks of a texture and of
// the window back buffer, for diag=1 on the GL device only.
void OrbisDiagTexture(const char* tag, GSTexture* t)
{
	(void)tag;
	(void)t;
}

void OrbisSampleWindow()
{
}

// vk-285-115 (include-orbis/OrbisDriver.h): RADV's own GetInstanceProcAddr is in the eboot only when link-radv.sh linked
// mihawk-99's RADV archive; in a ps5vk eboot the weak reference is resolved to null at link time.
extern "C" void* radv_GetInstanceProcAddr(void* instance, const char* name) __attribute__((weak));

bool OrbisDriverIsRADV()
{
	return radv_GetInstanceProcAddr != nullptr;
}

const char* OrbisDriverName()
{
	return OrbisDriverIsRADV() ? "RADV" : "ps5vk";
}

// The kernel's answer about one mapping (the layout homebrew SDKs publish for it).
struct OrbisVirtualQueryInfo
{
	uintptr_t start;
	uintptr_t end;
	int64_t offset;
	int32_t protection;
	int32_t memory_type;
	uint8_t flags; // bit 0 flexible, 1 direct, 2 stack, 3 pooled, 4 committed
	char name[32];
};

extern "C" int sceKernelVirtualQuery(const void* address, int flags, OrbisVirtualQueryInfo* info, size_t size);
extern "C" size_t sceKernelGetDirectMemorySize();
extern "C" int sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t alignment, off_t* start_out, size_t* size_out);

// The driver's GPU-visible memory must lie in [0x2'0000'0000, 0x3'0000'0000): its
// shaders combine 32-bit pointers with the high word 2 (Swordpdf/PS5HB_Vulkan,
// ps5vk_private.h). Print what already occupies that window and how much direct
// memory is left, so an out-of-memory at device creation says why.
void orbis_vk_window_survey(const char* when)
{
	constexpr uintptr_t window_start = 0x200000000ULL;
	constexpr uintptr_t window_end = 0x300000000ULL;
	std::printf("[vkmem] %s: GPU window %#lx-%#lx\n", when, static_cast<unsigned long>(window_start),
		static_cast<unsigned long>(window_end));

	uintptr_t at = window_start;
	uintptr_t used = 0;
	uintptr_t largest_gap = 0;
	uintptr_t previous_end = window_start;
	for (int count = 0; at < window_end && count < 64; count++)
	{
		OrbisVirtualQueryInfo info = {};
		const int rc = sceKernelVirtualQuery(reinterpret_cast<const void*>(at), 1 /* find next */, &info, sizeof(info));
		if (rc != 0 || info.end <= info.start)
		{
			if (rc != 0 && count == 0)
				std::printf("[vkmem]   query rc=%#x\n", rc);
			break;
		}
		if (info.start >= window_end)
			break;
		const uintptr_t start = info.start < window_start ? window_start : info.start;
		const uintptr_t end = info.end > window_end ? window_end : info.end;
		if (start > previous_end && start - previous_end > largest_gap)
			largest_gap = start - previous_end;
		used += end - start;
		previous_end = end;
		info.name[sizeof(info.name) - 1] = '\0';
		std::printf("[vkmem]   %#lx-%#lx %6lu MiB prot=%#x type=%d flags=%#x %s\n", static_cast<unsigned long>(info.start),
			static_cast<unsigned long>(info.end), static_cast<unsigned long>((info.end - info.start) >> 20), info.protection,
			info.memory_type, info.flags, info.name);
		at = info.end;
	}
	if (window_end > previous_end && window_end - previous_end > largest_gap)
		largest_gap = window_end - previous_end;

	off_t free_start = 0;
	size_t free_size = 0;
	const size_t direct_total = sceKernelGetDirectMemorySize();
	const int rc = sceKernelAvailableDirectMemorySize(0, static_cast<off_t>(direct_total), 0x10000, &free_start, &free_size);
	std::printf("[vkmem]   window used %lu MiB, largest free gap %lu MiB; direct memory %lu MiB, largest free %lu MiB (rc=%#x)\n",
		static_cast<unsigned long>(used >> 20), static_cast<unsigned long>(largest_gap >> 20),
		static_cast<unsigned long>(direct_total >> 20), static_cast<unsigned long>(free_size >> 20), rc);
	std::fflush(stdout);
}
