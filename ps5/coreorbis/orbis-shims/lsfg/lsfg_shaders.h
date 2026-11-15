// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's renderer_vulkan/present/lsfg_shaders.h. Eden loads the DLL here and checks the
// device's features; in PS5SX2 the caller does both (GSDeviceVK.cpp, so that the log can say which one stopped it) and
// hands the modules in. Each module's entry point is read from its SPIR-V.

#pragma once

#include <map>
#include <string>

#include "lossless_dll.h"
#include "lsfg_vk.h"

namespace lsfg
{
class LsfgShaders
{
public:
	LsfgShaders(const Device& device, const ShaderModules& code);

	[[nodiscard]] bool IsValid() const { return valid; }
	[[nodiscard]] VkShaderModule Get(u32 shader_id) const;
	[[nodiscard]] const char* EntryPoint(u32 shader_id) const;

private:
	struct Module
	{
		ShaderModule module;
		std::string entry_point;
	};
	std::map<u32, Module> modules;
	bool valid{};
};
} // namespace lsfg
