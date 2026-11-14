// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's renderer_vulkan/present/lsfg_common.h on lsfg_vk.h. Changes: the namespace
// (lsfg), no MemoryAllocator parameter (the Device has VMA's allocator), the images' usage can be given (the chain's input
// frames are colour attachments PCSX2's present draws into, not storage images: a BGRA8 swapchain format need not take
// storage), and a descriptor for a missing image is a 1x1 black image (LsfgDescriptorWriter::SetNullView) where Eden
// writes a null descriptor (VK_EXT_robustness2's nullDescriptor, which PS5SX2's device does not enable).

#pragma once

#include <array>
#include <deque>
#include <initializer_list>
#include <map>
#include <utility>
#include <vector>

#include "lsfg_vk.h"

namespace lsfg
{
class LsfgShaders;

constexpr VkFormat LSFG_DEFAULT_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat LSFG_FLOW_FORMAT = VK_FORMAT_R8_UNORM;
constexpr VkFormat LSFG_MOTION_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
// PS5SX2: the format the flow images are made in: LSFG_FLOW_FORMAT, or R32_SFLOAT where the driver takes no R8_UNORM storage image
// (ps5vk; the shaders' r8 declarations are retyped to match, lsfg_backend.cpp). Set by the chain's owner before it is built.
VkFormat LsfgFlowFormat();
void SetLsfgFlowFormat(VkFormat format);
constexpr VkImageUsageFlags LSFG_DEFAULT_USAGE =
	VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

constexpr size_t LSFG_HISTORY_SLOTS = 3;
constexpr size_t LSFG_MAX_TARGETS = 7;
constexpr size_t LSFG_MAX_GENERATIONS = 3;

constexpr size_t LSFG_GENERATION_SLOTS = LSFG_MAX_GENERATIONS * (LSFG_MAX_GENERATIONS + 1) / 2;

[[nodiscard]] constexpr size_t LsfgGenerationSlot(size_t generation_count, size_t generation)
{
	return (generation_count - 1) * generation_count / 2 + generation;
}

[[nodiscard]] constexpr f32 LsfgTimestamp(size_t generation, size_t generation_count)
{
	return static_cast<f32>(generation + 1) / static_cast<f32>(generation_count + 1);
}

[[nodiscard]] constexpr size_t LsfgSlotCount(size_t slot)
{
	size_t count = 1;
	while (LsfgGenerationSlot(count + 1, 0) <= slot)
		++count;
	return count;
}

[[nodiscard]] constexpr f32 LsfgSlotTimestamp(size_t slot)
{
	const size_t count = LsfgSlotCount(slot);
	return LsfgTimestamp(slot - LsfgGenerationSlot(count, 0), count);
}

class LsfgImage
{
public:
	LsfgImage() = default;
	LsfgImage(const Device& device, VkExtent2D extent_, VkFormat format = LSFG_DEFAULT_FORMAT,
		VkImageUsageFlags usage = LSFG_DEFAULT_USAGE);

	[[nodiscard]] VkImage Handle() const { return *image; }
	[[nodiscard]] VkImageView View() const { return *view; }
	[[nodiscard]] VkExtent2D Extent() const { return extent; }
	[[nodiscard]] VkFormat Format() const { return format; }
	[[nodiscard]] VkImageLayout Layout() const { return layout; }
	void SetLayout(VkImageLayout new_layout) { layout = new_layout; }

private:
	Image image;
	ImageView view;
	VkExtent2D extent{};
	VkFormat format{VK_FORMAT_UNDEFINED};
	VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
};

using LsfgImagePair = std::array<LsfgImage, 2>;
using LsfgImageHistory = std::array<LsfgImagePair, LSFG_HISTORY_SLOTS>;

class LsfgResources
{
public:
	LsfgResources() = default;
	LsfgResources(const Device& device_, f32 flow_scale_) : device{&device_}, flow_scale{flow_scale_} {}

	[[nodiscard]] VkSampler GetSampler(VkSamplerAddressMode address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		VkCompareOp compare_op = VK_COMPARE_OP_NEVER, bool white_border = false);

	[[nodiscard]] VkBuffer GetBuffer(f32 timestamp = 0.0f, bool first_iter = false, bool first_iter_s = false);

	[[nodiscard]] static VkDeviceSize BufferSize();

private:
	const Device* device{};
	f32 flow_scale{1.0f};

	std::map<u64, Sampler> samplers;
	std::map<u64, Buffer> buffers;
};

class LsfgBarriers
{
public:
	explicit LsfgBarriers(CommandBuffer cmdbuf_) : cmdbuf{cmdbuf_} {}

	LsfgBarriers& WriteToRead(LsfgImage& image);
	LsfgBarriers& ReadToWrite(LsfgImage& image);
	LsfgBarriers& WriteToRead(LsfgImage* image);
	LsfgBarriers& ReadToWrite(LsfgImage* image);
	LsfgBarriers& DiscardToWrite(VkImage image);

	template <typename Range>
	LsfgBarriers& WriteToReadAll(Range& images)
	{
		for (auto& image : images)
			WriteToRead(image);
		return *this;
	}

	template <typename Range>
	LsfgBarriers& ReadToWriteAll(Range& images)
	{
		for (auto& image : images)
			ReadToWrite(image);
		return *this;
	}

	void Build();

private:
	LsfgBarriers& Push(LsfgImage& image, VkAccessFlags src_access, VkAccessFlags dst_access);

	CommandBuffer cmdbuf;
	std::vector<VkImageMemoryBarrier> barriers;
};

class LsfgDescriptorWriter
{
public:
	explicit LsfgDescriptorWriter(VkDescriptorSet set_) : set{set_} {}

	LsfgDescriptorWriter& AddSampler(VkSampler sampler);
	LsfgDescriptorWriter& AddSampledImage(const LsfgImage& image);
	LsfgDescriptorWriter& AddSampledImage(const LsfgImage* image);
	LsfgDescriptorWriter& AddStorageImage(const LsfgImage& image);
	LsfgDescriptorWriter& AddStorageView(VkImageView view);
	LsfgDescriptorWriter& AddUniformBuffer(VkBuffer buffer, VkDeviceSize size);

	template <typename Range>
	LsfgDescriptorWriter& AddSampledImages(const Range& images)
	{
		for (const auto& image : images)
			AddSampledImage(image);
		return *this;
	}

	template <typename Range>
	LsfgDescriptorWriter& AddStorageImages(const Range& images)
	{
		for (const auto& image : images)
			AddStorageImage(image);
		return *this;
	}

	void Build(const Device& device);

	// PS5SX2: the view written for a missing image (AddSampledImage(nullptr)), in GENERAL; VK_NULL_HANDLE writes a null
	// descriptor as Eden does. The chain sets it while it builds (one chain at a time, on the GS thread).
	static void SetNullView(VkImageView view) { s_null_view = view; }

private:
	LsfgDescriptorWriter& PushImage(VkDescriptorType type, VkSampler sampler, VkImageView view);

	static inline VkImageView s_null_view = VK_NULL_HANDLE;

	VkDescriptorSet set;
	u32 binding{};
	std::deque<VkDescriptorImageInfo> image_infos;
	std::deque<VkDescriptorBufferInfo> buffer_infos;
	std::vector<VkWriteDescriptorSet> writes;
};

using LsfgBindings = std::initializer_list<std::pair<u32, VkDescriptorType>>;

class LsfgPass
{
public:
	LsfgPass() = default;
	LsfgPass(const Device& device, const LsfgShaders& shaders, u32 shader_id, LsfgBindings bindings);

	[[nodiscard]] VkDescriptorSetLayout SetLayout() const { return *descriptor_set_layout; }
	[[nodiscard]] u32 DescriptorCount() const { return descriptor_count; }

	void Bind(CommandBuffer cmdbuf, VkDescriptorSet set) const;
	void BindPipeline(CommandBuffer cmdbuf) const;
	void BindSet(CommandBuffer cmdbuf, VkDescriptorSet set) const;

private:
	DescriptorSetLayout descriptor_set_layout;
	PipelineLayout pipeline_layout;
	Pipeline pipeline;
	u32 descriptor_count{};
};

[[nodiscard]] DescriptorPool CreateLsfgDescriptorPool(const Device& device, u32 max_sets);

[[nodiscard]] Sampler CreateLsfgSampler(const Device& device, VkSamplerAddressMode address_mode, VkCompareOp compare_op, bool white_border);
} // namespace lsfg
