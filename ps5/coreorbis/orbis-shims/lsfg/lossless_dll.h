// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's src/video_core/frame_gen/lossless_dll.h. The player's own Lossless.dll (from
// Lossless Scaling; nothing of it ships with PS5SX2) holds the frame generation's compute shaders as SPIR-V in its PE
// resources (RCDATA); this finds them. Changes from Eden: no shader cache file (the shaders are only renumbered,
// lsfg_translate.cpp, which takes no time, and every write to /data costs the console's write budget), the folder is a
// parameter (PS5SX2: /data/PCSX2/lossless), the file's name is matched without case, and the parse works on memory too
// (the host test's synthetic DLL).

#pragma once

#include <array>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "lsfg_vk.h"

namespace lsfg
{
enum class LosslessStatus : u32
{
	Ok,
	NotInstalled,
	UnreadableFile,
	NotPortableExecutable,
	MissingShaders,
	TranslationFailed,
};

// What the log says for each status (ProsperoEden's words).
const char* LosslessStatusText(LosslessStatus status);

using ShaderModules = std::map<u32, std::vector<u32>>;

namespace PerformanceShader
{
	constexpr u32 MIPMAPS = 255;
	constexpr u32 GENERATE = 256;
	constexpr std::array<u32, 4> ALPHA{290, 291, 292, 293};
	constexpr std::array<u32, 5> BETA{298, 299, 300, 301, 302};
	constexpr std::array<u32, 5> GAMMA{280, 282, 283, 284, 285};
	constexpr std::array<u32, 10> DELTA{280, 286, 287, 288, 289, 281, 294, 295, 296, 297};

	// The resource ids of the shaders used are these ids plus this offset (the native FP16 "performance" set).
	constexpr u32 NATIVE_FP16_OFFSET = 49;
} // namespace PerformanceShader

// The shader ids the chain uses (MIPMAPS, GENERATE and 280..302), in order.
std::vector<u32> PerformanceShaderIds();

// The file in `folder` named Lossless.dll in any case ("lossless.dll" from a FAT32 copy); `folder`/Lossless.dll when none.
std::string FindLosslessDll(const std::string& folder);

// The shaders of a DLL in memory, renumbered for the chain's descriptor layouts (lsfg_translate.h).
LosslessStatus ParseShaderModules(std::span<const u8> image, ShaderModules& out_modules);
// The same from a file. `out_size` is the file's size (0 when it could not be read).
LosslessStatus LoadShaderModules(const std::string& path, ShaderModules& out_modules, u64* out_size = nullptr);
} // namespace lsfg
