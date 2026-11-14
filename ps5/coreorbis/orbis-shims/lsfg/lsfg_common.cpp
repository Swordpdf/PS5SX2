// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's renderer_vulkan/present/lsfg_common.cpp (lsfg_common.h says what changed).

#include <algorithm>
#include <cstring>

#include "lsfg_common.h"
#include "lsfg_shaders.h"

namespace lsfg
{
namespace
{
	constexpr u32 DESCRIPTORS_PER_TYPE = 4096;
	VkFormat s_flow_format = LSFG_FLOW_FORMAT;

	struct LsfgConstants
	{
		std::array<u32, 2> input_offset;
		u32 first_iter;
		u32 first_iter_s;
		u32 advanced_color_kind;
		u32 hdr_support;
		f32 resolution_inv_scale;
		f32 timestamp;
		f32 ui_threshold;
		std::array<u32, 3> padding;
	};
	static_assert(sizeof(LsfgConstants) == 48);

	Image CreateChainImage(const Device& device, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage)
	{
		VkImageCreateInfo image_ci = {};
		image_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_ci.imageType = VK_IMAGE_TYPE_2D;
		image_ci.format = format;
		image_ci.extent = {extent.width, extent.height, 1};
		image_ci.mipLevels = 1;
		image_ci.arrayLayers = 1;
		image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
		image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
		image_ci.usage = usage;
		image_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		return CreateImage(device, image_ci);
	}

	VkImageMemoryBarrier MakeBarrier(const LsfgImage& image, VkAccessFlags src_access, VkAccessFlags dst_access)
	{
		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = src_access;
		barrier.dstAccessMask = dst_access;
		barrier.oldLayout = image.Layout();
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image.Handle();
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		return barrier;
	}
} // Anonymous namespace

VkFormat LsfgFlowFormat()
{
	return s_flow_format;
}

void SetLsfgFlowFormat(VkFormat format)
{
	s_flow_format = format;
}

LsfgImage::LsfgImage(const Device& device, VkExtent2D extent_, VkFormat format_, VkImageUsageFlags usage)
	: extent{std::max(1u, extent_.width), std::max(1u, extent_.height)}
	, format{format_}
{
	image = CreateChainImage(device, extent, format, usage);
	view = CreateImageView(device, *image, format);
}

LsfgBarriers& LsfgBarriers::Push(LsfgImage& image, VkAccessFlags src_access, VkAccessFlags dst_access)
{
	if (image.Handle() == VK_NULL_HANDLE)
		return *this;
	barriers.push_back(MakeBarrier(image, src_access, dst_access));
	image.SetLayout(VK_IMAGE_LAYOUT_GENERAL);
	return *this;
}

LsfgBarriers& LsfgBarriers::WriteToRead(LsfgImage& image)
{
	return Push(image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}

LsfgBarriers& LsfgBarriers::ReadToWrite(LsfgImage& image)
{
	return Push(image, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
}

LsfgBarriers& LsfgBarriers::WriteToRead(LsfgImage* image)
{
	return image == nullptr ? *this : WriteToRead(*image);
}

LsfgBarriers& LsfgBarriers::ReadToWrite(LsfgImage* image)
{
	return image == nullptr ? *this : ReadToWrite(*image);
}

LsfgBarriers& LsfgBarriers::DiscardToWrite(VkImage image)
{
	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	barriers.push_back(barrier);
	return *this;
}

VkDeviceSize LsfgResources::BufferSize()
{
	return sizeof(LsfgConstants);
}

VkSampler LsfgResources::GetSampler(VkSamplerAddressMode address_mode, VkCompareOp compare_op, bool white_border)
{
	const u64 key = static_cast<u64>(address_mode) | (static_cast<u64>(compare_op) << 8) | (static_cast<u64>(white_border) << 16);

	const auto it = samplers.find(key);
	if (it != samplers.end())
		return *it->second;

	const auto [entry, inserted] = samplers.emplace(key, CreateLsfgSampler(*device, address_mode, compare_op, white_border));
	return *entry->second;
}

VkBuffer LsfgResources::GetBuffer(f32 timestamp, bool first_iter, bool first_iter_s)
{
	u32 timestamp_bits{};
	std::memcpy(&timestamp_bits, &timestamp, sizeof(timestamp_bits));
	const u64 key = static_cast<u64>(timestamp_bits) | (static_cast<u64>(first_iter) << 32) | (static_cast<u64>(first_iter_s) << 33);

	const auto it = buffers.find(key);
	if (it != buffers.end())
		return *it->second;

	Buffer buffer = CreateUploadBuffer(*device, sizeof(LsfgConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	if (!buffer)
		return VK_NULL_HANDLE;

	LsfgConstants constants = {};
	constants.input_offset = {0, 0};
	constants.first_iter = first_iter ? 1u : 0u;
	constants.first_iter_s = first_iter_s ? 1u : 0u;
	constants.advanced_color_kind = 0;
	constants.hdr_support = 0;
	constants.resolution_inv_scale = 1.0f / flow_scale;
	constants.timestamp = timestamp;
	constants.ui_threshold = 0.5f;
	constants.padding = {0, 0, 0};

	const std::span<u8> mapped = buffer.Mapped();
	std::memcpy(mapped.data(), &constants, sizeof(constants));
	buffer.Flush();

	const auto [entry, inserted] = buffers.emplace(key, std::move(buffer));
	return *entry->second;
}

void LsfgBarriers::Build()
{
	if (barriers.empty())
		return;
	cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barriers);
	barriers.clear();
}

LsfgDescriptorWriter& LsfgDescriptorWriter::PushImage(VkDescriptorType type, VkSampler sampler, VkImageView view)
{
	if (type != VK_DESCRIPTOR_TYPE_SAMPLER && view == VK_NULL_HANDLE)
		view = s_null_view; // PS5SX2: the black 1x1 image, when there is one
	VkDescriptorImageInfo info = {};
	info.sampler = sampler;
	info.imageView = view;
	info.imageLayout = view == VK_NULL_HANDLE ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
	image_infos.push_back(info);
	VkWriteDescriptorSet write = {};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = set;
	write.dstBinding = binding++;
	write.dstArrayElement = 0;
	write.descriptorCount = 1;
	write.descriptorType = type;
	write.pImageInfo = &image_infos.back();
	writes.push_back(write);
	return *this;
}

LsfgDescriptorWriter& LsfgDescriptorWriter::AddSampler(VkSampler sampler)
{
	return PushImage(VK_DESCRIPTOR_TYPE_SAMPLER, sampler, VK_NULL_HANDLE);
}

LsfgDescriptorWriter& LsfgDescriptorWriter::AddSampledImage(const LsfgImage& image)
{
	return PushImage(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_NULL_HANDLE, image.View());
}

LsfgDescriptorWriter& LsfgDescriptorWriter::AddSampledImage(const LsfgImage* image)
{
	return PushImage(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_NULL_HANDLE, image == nullptr ? VK_NULL_HANDLE : image->View());
}

LsfgDescriptorWriter& LsfgDescriptorWriter::AddStorageImage(const LsfgImage& image)
{
	return PushImage(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE, image.View());
}

LsfgDescriptorWriter& LsfgDescriptorWriter::AddStorageView(VkImageView view)
{
	return PushImage(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE, view);
}

LsfgDescriptorWriter& LsfgDescriptorWriter::AddUniformBuffer(VkBuffer buffer, VkDeviceSize size)
{
	VkDescriptorBufferInfo info = {};
	info.buffer = buffer;
	info.offset = 0;
	info.range = size;
	buffer_infos.push_back(info);
	VkWriteDescriptorSet write = {};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = set;
	write.dstBinding = binding++;
	write.dstArrayElement = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	write.pBufferInfo = &buffer_infos.back();
	writes.push_back(write);
	return *this;
}

void LsfgDescriptorWriter::Build(const Device& device)
{
	if (writes.empty())
		return;
	// PS5SX2: a null image, sampler or buffer means something failed to be made, which the Device has noted: nothing is written.
	for (const VkWriteDescriptorSet& write : writes)
	{
		if (write.pBufferInfo && write.pBufferInfo->buffer == VK_NULL_HANDLE)
			return;
		if (write.pImageInfo && write.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER && write.pImageInfo->sampler == VK_NULL_HANDLE)
			return;
		if (write.pImageInfo && write.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER && write.pImageInfo->imageView == VK_NULL_HANDLE &&
			s_null_view != VK_NULL_HANDLE)
			return;
	}
	UpdateDescriptorSets(device, writes);
	writes.clear();
}

LsfgPass::LsfgPass(const Device& device, const LsfgShaders& shaders, u32 shader_id, LsfgBindings bindings)
{
	std::vector<VkDescriptorType> types;
	for (const auto& [count, type] : bindings)
		types.insert(types.end(), count, type);
	descriptor_count = static_cast<u32>(types.size());

	descriptor_set_layout = CreateDescriptorSetLayout(device, std::span<const VkDescriptorType>{types}, VK_SHADER_STAGE_COMPUTE_BIT);
	pipeline_layout = CreatePipelineLayout(device, *descriptor_set_layout);
	// PS5SX2: which shader failed, for the log (the Device keeps the first failure only).
	if (device.Ok() && shaders.Get(shader_id) == VK_NULL_HANDLE)
		device.Fail("shader " + std::to_string(shader_id) + " (not loaded)", VK_ERROR_INITIALIZATION_FAILED);
	const bool was_ok = device.Ok();
	pipeline = CreateComputePipeline(device, *pipeline_layout, shaders.Get(shader_id), shaders.EntryPoint(shader_id));
	if (was_ok && !device.Ok())
		device.error += " for shader " + std::to_string(shader_id);
}

void LsfgPass::Bind(CommandBuffer cmdbuf, VkDescriptorSet set) const
{
	BindPipeline(cmdbuf);
	BindSet(cmdbuf, set);
}

void LsfgPass::BindPipeline(CommandBuffer cmdbuf) const
{
	cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
}

void LsfgPass::BindSet(CommandBuffer cmdbuf, VkDescriptorSet set) const
{
	cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline_layout, 0, set);
}

DescriptorPool CreateLsfgDescriptorPool(const Device& device, u32 max_sets)
{
	return CreateDescriptorPool(device, DESCRIPTORS_PER_TYPE, max_sets,
		{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});
}

Sampler CreateLsfgSampler(const Device& device, VkSamplerAddressMode address_mode, VkCompareOp compare_op, bool white_border)
{
	VkSamplerCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	ci.magFilter = VK_FILTER_LINEAR;
	ci.minFilter = VK_FILTER_LINEAR;
	ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	ci.addressModeU = address_mode;
	ci.addressModeV = address_mode;
	ci.addressModeW = address_mode;
	ci.mipLodBias = 0.0f;
	ci.anisotropyEnable = VK_FALSE;
	ci.maxAnisotropy = 0.0f;
	ci.compareEnable = VK_FALSE;
	ci.compareOp = compare_op;
	ci.minLod = 0.0f;
	ci.maxLod = VK_LOD_CLAMP_NONE;
	ci.borderColor = white_border ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	ci.unnormalizedCoordinates = VK_FALSE;
	return CreateSampler(device, ci);
}
} // namespace lsfg
