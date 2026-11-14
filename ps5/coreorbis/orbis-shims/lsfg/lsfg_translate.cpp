// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's src/video_core/frame_gen/lsfg_translate.cpp, plus DescribeSpirv.

#include <algorithm>
#include <cstring>
#include <map>
#include <tuple>

#include "lsfg_translate.h"

namespace lsfg
{
namespace
{
	constexpr u32 SPIRV_MAGIC = 0x07230203;
	constexpr u32 SPIRV_WORD_COUNT_SHIFT = 16;
	constexpr u32 SPIRV_OPCODE_MASK = 0xffff;
	constexpr u32 SPIRV_OP_CAPABILITY = 17;
	constexpr u32 SPIRV_OP_ENTRY_POINT = 15;
	constexpr u32 SPIRV_OP_FUNCTION = 54;
	constexpr u32 SPIRV_OP_DECORATE = 71;
	constexpr u32 SPIRV_DECORATION_BINDING = 33;
	constexpr u32 SPIRV_DECORATION_DESCRIPTOR_SET = 34;
	constexpr u32 SPIRV_EXECUTION_MODEL_GL_COMPUTE = 5;

	constexpr u32 DECORATION_LITERAL_WORD = 3;
	constexpr size_t SPIRV_HEADER_WORDS = 5;

	void RenumberBindingsInOrder(std::vector<u32>& words)
	{
		struct Slot
		{
			u32 set;
			u32 binding;
			size_t literal_offset;
		};

		std::map<u32, u32> sets;
		std::vector<Slot> slots;

		size_t offset = SPIRV_HEADER_WORDS;
		while (offset + 1 <= words.size())
		{
			const u32 length = words[offset] >> SPIRV_WORD_COUNT_SHIFT;
			const u32 opcode = words[offset] & SPIRV_OPCODE_MASK;
			if (length == 0 || offset + length > words.size())
				return;
			if (opcode == SPIRV_OP_FUNCTION)
				break;
			if (opcode == SPIRV_OP_DECORATE && length >= 4)
			{
				if (words[offset + 2] == SPIRV_DECORATION_DESCRIPTOR_SET)
					sets[words[offset + 1]] = words[offset + 3];
				else if (words[offset + 2] == SPIRV_DECORATION_BINDING)
					slots.push_back(Slot{0, words[offset + 3], offset + DECORATION_LITERAL_WORD});
			}
			offset += length;
		}

		for (Slot& slot : slots)
		{
			const auto hit = sets.find(words[slot.literal_offset - 2]);
			slot.set = hit == sets.end() ? 0 : hit->second;
		}

		std::ranges::stable_sort(slots, [](const Slot& lhs, const Slot& rhs) {
			return std::tie(lhs.set, lhs.binding) < std::tie(rhs.set, rhs.binding);
		});

		for (size_t i = 0; i < slots.size(); ++i)
			words[slots[i].literal_offset] = static_cast<u32>(i);
	}
} // Anonymous namespace

bool IsSpirvModule(std::span<const u8> blob)
{
	if (blob.size() < SPIRV_HEADER_WORDS * sizeof(u32) || blob.size() % sizeof(u32) != 0)
		return false;
	u32 magic{};
	std::memcpy(&magic, blob.data(), sizeof(magic));
	return magic == SPIRV_MAGIC;
}

std::vector<u32> AdoptSpirvModule(std::span<const u8> blob)
{
	if (!IsSpirvModule(blob))
		return {};

	std::vector<u32> words(blob.size() / sizeof(u32));
	std::memcpy(words.data(), blob.data(), blob.size());

	RenumberBindingsInOrder(words);
	return words;
}

size_t RetypeStorageImages(std::vector<u32>& words, u32 from, u32 to)
{
	// OpTypeImage: result, sampled type, dim, depth, arrayed, MS, sampled, format [, access]
	constexpr u32 SPIRV_OP_TYPE_IMAGE = 25;
	size_t changed = 0;
	if (words.size() < SPIRV_HEADER_WORDS || words[0] != SPIRV_MAGIC)
		return 0;
	size_t offset = SPIRV_HEADER_WORDS;
	while (offset < words.size())
	{
		const u32 length = words[offset] >> SPIRV_WORD_COUNT_SHIFT;
		const u32 opcode = words[offset] & SPIRV_OPCODE_MASK;
		if (length == 0 || offset + length > words.size())
			break;
		if (opcode == SPIRV_OP_FUNCTION)
			break;
		if (opcode == SPIRV_OP_TYPE_IMAGE && length >= 9 && words[offset + 7] == 2 && words[offset + 8] == from)
		{
			words[offset + 8] = to;
			changed++;
		}
		offset += length;
	}
	return changed;
}

SpirvInfo DescribeSpirv(std::span<const u32> words)
{
	SpirvInfo info;
	if (words.size() < SPIRV_HEADER_WORDS || words[0] != SPIRV_MAGIC)
		return info;
	info.version = words[1];
	bool have_entry = false;
	size_t offset = SPIRV_HEADER_WORDS;
	while (offset < words.size())
	{
		const u32 length = words[offset] >> SPIRV_WORD_COUNT_SHIFT;
		const u32 opcode = words[offset] & SPIRV_OPCODE_MASK;
		if (length == 0 || offset + length > words.size())
			return info;
		if (opcode == SPIRV_OP_FUNCTION)
			break;
		if (opcode == SPIRV_OP_CAPABILITY && length >= 2)
			info.capabilities.push_back(words[offset + 1]);
		else if (opcode == SPIRV_OP_ENTRY_POINT && length >= 4 && !have_entry && words[offset + 1] == SPIRV_EXECUTION_MODEL_GL_COMPUTE)
		{
			// The name: a nul-terminated string in the words after the model and the id.
			const char* name = reinterpret_cast<const char*>(&words[offset + 3]);
			const size_t max = (length - 3) * sizeof(u32);
			info.entry_point.assign(name, strnlen(name, max));
			have_entry = true;
		}
		else if (opcode == SPIRV_OP_DECORATE && length >= 4 && words[offset + 2] == SPIRV_DECORATION_BINDING)
			info.bindings++;
		offset += length;
	}
	info.valid = have_entry;
	return info;
}

std::string SpirvCapabilityName(u32 capability)
{
	switch (capability)
	{
		case 1: return "Shader";
		case 9: return "Float16";
		case 22: return "Int16";
		case 39: return "Int8";
		case 49: return "StorageImageExtendedFormats";
		case 50: return "ImageQuery";
		case 55: return "StorageImageReadWithoutFormat";
		case 56: return "StorageImageWriteWithoutFormat";
		case 4433: return "StorageBuffer16BitAccess";
		case 4434: return "UniformAndStorageBuffer16BitAccess";
		case 4435: return "StoragePushConstant16";
		case 4436: return "StorageInputOutput16";
		case 5345: return "VulkanMemoryModel";
		case 5346: return "VulkanMemoryModelDeviceScope";
		case 5301: return "ShaderNonUniform";
		case 4448: return "StorageBuffer8BitAccess";
		case 61: return "GroupNonUniform";
		default: return std::to_string(capability);
	}
}
} // namespace lsfg
