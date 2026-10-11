// PS5 port frontend: loading fe::Vk.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_vk.h"

namespace fe
{
bool Vk::LoadGlobal(PFN_vkGetInstanceProcAddr gipa, const char** missing)
{
	vkGetInstanceProcAddr = gipa;
#define FE_VK_LOAD(name) \
	name = reinterpret_cast<PFN_##name>(gipa(VK_NULL_HANDLE, #name)); \
	if (!name) \
	{ \
		*missing = #name; \
		return false; \
	}
	FE_VK_GLOBAL_FUNCS(FE_VK_LOAD)
#undef FE_VK_LOAD
	return true;
}

bool Vk::LoadInstance(VkInstance instance, bool wsi, const char** missing)
{
#define FE_VK_LOAD(name) \
	name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name)); \
	if (!name) \
	{ \
		*missing = #name; \
		return false; \
	}
	FE_VK_INSTANCE_FUNCS(FE_VK_LOAD)
	if (wsi)
	{
		FE_VK_INSTANCE_WSI_FUNCS(FE_VK_LOAD)
	}
#undef FE_VK_LOAD
	return true;
}

bool Vk::LoadDevice(VkDevice device, bool wsi, const char** missing)
{
#define FE_VK_LOAD(name) \
	name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)); \
	if (!name) \
	{ \
		*missing = #name; \
		return false; \
	}
	FE_VK_DEVICE_FUNCS(FE_VK_LOAD)
	if (wsi)
	{
		FE_VK_DEVICE_WSI_FUNCS(FE_VK_LOAD)
	}
#undef FE_VK_LOAD
	return true;
}
} // namespace fe
