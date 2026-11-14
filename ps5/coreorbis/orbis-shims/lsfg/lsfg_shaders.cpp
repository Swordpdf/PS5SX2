// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's renderer_vulkan/present/lsfg_shaders.cpp (lsfg_shaders.h says what changed).

#include "lsfg_shaders.h"
#include "lsfg_translate.h"

namespace lsfg
{
LsfgShaders::LsfgShaders(const Device& device, const ShaderModules& code)
{
	for (const auto& [id, words] : code)
	{
		Module m;
		m.module = CreateShaderModule(device, words);
		if (!m.module)
		{
			if (device.Ok())
				device.Fail("vkCreateShaderModule", VK_ERROR_INITIALIZATION_FAILED);
			device.error += " for shader " + std::to_string(id);
			return;
		}
		m.entry_point = DescribeSpirv(words).entry_point;
		modules.emplace(id, std::move(m));
	}
	valid = !modules.empty() && device.Ok();
}

VkShaderModule LsfgShaders::Get(u32 shader_id) const
{
	const auto hit = modules.find(shader_id);
	return hit == modules.end() ? VK_NULL_HANDLE : *hit->second.module;
}

const char* LsfgShaders::EntryPoint(u32 shader_id) const
{
	const auto hit = modules.find(shader_id);
	return hit == modules.end() ? "main" : hit->second.entry_point.c_str();
}
} // namespace lsfg
