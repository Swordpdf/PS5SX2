// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's src/video_core/frame_gen/lsfg_translate.h, plus DescribeSpirv for the log
// (what the shaders ask of the device).

#pragma once

#include <span>
#include <string>
#include <vector>

#include "lsfg_vk.h"

namespace lsfg
{
[[nodiscard]] bool IsSpirvModule(std::span<const u8> blob);

// The module with its bindings renumbered 0, 1, 2... in (set, binding) order: the order the chain's descriptor layouts
// list them in (lsfg_common.cpp LsfgPass). Empty when the blob is not SPIR-V.
[[nodiscard]] std::vector<u32> AdoptSpirvModule(std::span<const u8> blob);

// PS5SX2 (2026-11-14): the storage images a module declares in SPIR-V image format `from` (OpTypeImage, Sampled = 2) declared in
// `to` instead; returns how many were changed. The PS5 driver (ps5vk) takes no R8_UNORM storage image, so the flow images are kept
// in R32_SFLOAT there and the shaders' r8 (15) storage declarations become r32f (3) (lsfg_backend.cpp).
constexpr u32 SPIRV_IMAGE_FORMAT_R32F = 3;
constexpr u32 SPIRV_IMAGE_FORMAT_R8 = 15;
size_t RetypeStorageImages(std::vector<u32>& words, u32 from, u32 to);

// PS5SX2: what a module declares, for the "[fg] lsfg:" lines and the device's feature checks.
struct SpirvInfo
{
	u32 version = 0;               // 0x00010300 = SPIR-V 1.3
	std::string entry_point = "main"; // the first compute entry point's name
	std::vector<u32> capabilities; // OpCapability values
	u32 bindings = 0;              // Binding decorations
	bool valid = false;
};
[[nodiscard]] SpirvInfo DescribeSpirv(std::span<const u32> words);
// "Shader", "Float16", "VulkanMemoryModel"... for the capabilities the LSFG shaders are known to use, else the number.
[[nodiscard]] std::string SpirvCapabilityName(u32 capability);
} // namespace lsfg
