// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): the Vulkan wrapper the LSFG passes use (lsfg_vk.h).

#include "lsfg_vk.h"

#include <algorithm>
#include <cstring>

namespace lsfg
{
const char* VkResultName(VkResult result)
{
	switch (result)
	{
		case VK_SUCCESS: return "VK_SUCCESS";
		case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
		case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
		case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
		case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
		case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
		case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
		case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
		case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
		case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
		case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
		default: return "VkResult (other)";
	}
}

void Device::Fail(const std::string& what, VkResult result) const
{
	if (!error.empty())
		return;
	error = what + " failed (" + VkResultName(result) + ", " + std::to_string(static_cast<int>(result)) + ")";
}

void DestroyImageView(VkDevice device, VkImageView handle) { vkDestroyImageView(device, handle, nullptr); }
void DestroySampler(VkDevice device, VkSampler handle) { vkDestroySampler(device, handle, nullptr); }
void DestroyShaderModule(VkDevice device, VkShaderModule handle) { vkDestroyShaderModule(device, handle, nullptr); }
void DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout handle) { vkDestroyDescriptorSetLayout(device, handle, nullptr); }
void DestroyPipelineLayout(VkDevice device, VkPipelineLayout handle) { vkDestroyPipelineLayout(device, handle, nullptr); }
void DestroyPipeline(VkDevice device, VkPipeline handle) { vkDestroyPipeline(device, handle, nullptr); }
void DestroyDescriptorPool(VkDevice device, VkDescriptorPool handle) { vkDestroyDescriptorPool(device, handle, nullptr); }

Image& Image::operator=(Image&& other) noexcept
{
	if (this != &other)
	{
		Reset();
		allocator = std::exchange(other.allocator, VmaAllocator{VK_NULL_HANDLE});
		image = std::exchange(other.image, VkImage{VK_NULL_HANDLE});
		allocation = std::exchange(other.allocation, VmaAllocation{VK_NULL_HANDLE});
	}
	return *this;
}

void Image::Reset()
{
	if (image != VK_NULL_HANDLE)
		vmaDestroyImage(allocator, image, allocation);
	image = VK_NULL_HANDLE;
	allocation = VK_NULL_HANDLE;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept
{
	if (this != &other)
	{
		Reset();
		allocator = std::exchange(other.allocator, VmaAllocator{VK_NULL_HANDLE});
		buffer = std::exchange(other.buffer, VkBuffer{VK_NULL_HANDLE});
		allocation = std::exchange(other.allocation, VmaAllocation{VK_NULL_HANDLE});
		mapped = std::exchange(other.mapped, nullptr);
		size = std::exchange(other.size, VkDeviceSize{0});
	}
	return *this;
}

void Buffer::Reset()
{
	if (buffer != VK_NULL_HANDLE)
		vmaDestroyBuffer(allocator, buffer, allocation);
	buffer = VK_NULL_HANDLE;
	allocation = VK_NULL_HANDLE;
	mapped = nullptr;
	size = 0;
}

void Buffer::Flush() const
{
	if (allocation != VK_NULL_HANDLE)
		vmaFlushAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
}

Image CreateImage(const Device& device, const VkImageCreateInfo& info)
{
	if (!device.Ok())
		return {};
	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	const VkResult res = vmaCreateImage(device.allocator, &info, &aci, &image, &allocation, nullptr);
	if (res != VK_SUCCESS)
	{
		device.Fail("vmaCreateImage " + std::to_string(info.extent.width) + "x" + std::to_string(info.extent.height) + " format " +
						std::to_string(static_cast<int>(info.format)),
			res);
		return {};
	}
	return Image(device.allocator, image, allocation);
}

Buffer CreateUploadBuffer(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage)
{
	if (!device.Ok())
		return {};
	const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	VmaAllocationInfo info = {};
	const VkResult res = vmaCreateBuffer(device.allocator, &bci, &aci, &buffer, &allocation, &info);
	if (res != VK_SUCCESS)
	{
		device.Fail("vmaCreateBuffer", res);
		return {};
	}
	if (!info.pMappedData)
	{
		vmaDestroyBuffer(device.allocator, buffer, allocation);
		device.Fail("mapping a uniform buffer", VK_ERROR_MEMORY_MAP_FAILED);
		return {};
	}
	return Buffer(device.allocator, buffer, allocation, info.pMappedData, size);
}

ImageView CreateImageView(const Device& device, VkImage image, VkFormat format)
{
	if (!device.Ok() || image == VK_NULL_HANDLE)
		return {};
	const VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, image, VK_IMAGE_VIEW_TYPE_2D, format,
		{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	VkImageView view = VK_NULL_HANDLE;
	const VkResult res = vkCreateImageView(device.device, &ci, nullptr, &view);
	if (res != VK_SUCCESS)
	{
		device.Fail("vkCreateImageView", res);
		return {};
	}
	return ImageView(device.device, view);
}

Sampler CreateSampler(const Device& device, const VkSamplerCreateInfo& info)
{
	if (!device.Ok())
		return {};
	VkSampler sampler = VK_NULL_HANDLE;
	const VkResult res = vkCreateSampler(device.device, &info, nullptr, &sampler);
	if (res != VK_SUCCESS)
	{
		device.Fail("vkCreateSampler", res);
		return {};
	}
	return Sampler(device.device, sampler);
}

ShaderModule CreateShaderModule(const Device& device, std::span<const u32> code)
{
	if (!device.Ok())
		return {};
	const VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, code.size() * sizeof(u32), code.data()};
	VkShaderModule module = VK_NULL_HANDLE;
	const VkResult res = vkCreateShaderModule(device.device, &ci, nullptr, &module);
	if (res != VK_SUCCESS)
	{
		device.Fail("vkCreateShaderModule", res);
		return {};
	}
	return ShaderModule(device.device, module);
}

DescriptorSetLayout CreateDescriptorSetLayout(const Device& device, std::span<const VkDescriptorType> types, VkShaderStageFlags stages)
{
	if (!device.Ok())
		return {};
	std::vector<VkDescriptorSetLayoutBinding> bindings(types.size());
	for (size_t i = 0; i < types.size(); i++)
		bindings[i] = VkDescriptorSetLayoutBinding{static_cast<u32>(i), types[i], 1, stages, nullptr};
	const VkDescriptorSetLayoutCreateInfo ci = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, static_cast<u32>(bindings.size()), bindings.data()};
	VkDescriptorSetLayout layout = VK_NULL_HANDLE;
	const VkResult res = vkCreateDescriptorSetLayout(device.device, &ci, nullptr, &layout);
	if (res != VK_SUCCESS)
	{
		device.Fail("vkCreateDescriptorSetLayout", res);
		return {};
	}
	return DescriptorSetLayout(device.device, layout);
}

PipelineLayout CreatePipelineLayout(const Device& device, VkDescriptorSetLayout layout)
{
	if (!device.Ok() || layout == VK_NULL_HANDLE)
		return {};
	const VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &layout, 0, nullptr};
	VkPipelineLayout pl = VK_NULL_HANDLE;
	const VkResult res = vkCreatePipelineLayout(device.device, &ci, nullptr, &pl);
	if (res != VK_SUCCESS)
	{
		device.Fail("vkCreatePipelineLayout", res);
		return {};
	}
	return PipelineLayout(device.device, pl);
}

Pipeline CreateComputePipeline(const Device& device, VkPipelineLayout layout, VkShaderModule module, const char* entry_point)
{
	if (!device.Ok() || layout == VK_NULL_HANDLE)
		return {};
	if (module == VK_NULL_HANDLE)
	{
		device.Fail("a compute pipeline without its shader", VK_ERROR_INITIALIZATION_FAILED);
		return {};
	}
	VkComputePipelineCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	ci.stage.module = module;
	ci.stage.pName = entry_point;
	ci.layout = layout;
	ci.basePipelineIndex = -1;
	VkPipeline pipeline = VK_NULL_HANDLE;
	const VkResult res = vkCreateComputePipelines(device.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
	if (res != VK_SUCCESS)
	{
		device.Fail("vkCreateComputePipelines", res);
		return {};
	}
	return Pipeline(device.device, pipeline);
}

DescriptorPool CreateDescriptorPool(const Device& device, u32 descriptors_per_type, u32 max_sets, std::initializer_list<VkDescriptorType> types)
{
	if (!device.Ok())
		return {};
	std::vector<VkDescriptorPoolSize> sizes;
	for (const VkDescriptorType type : types)
		sizes.push_back(VkDescriptorPoolSize{type, descriptors_per_type});
	const VkDescriptorPoolCreateInfo ci = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, max_sets, static_cast<u32>(sizes.size()), sizes.data()};
	VkDescriptorPool pool = VK_NULL_HANDLE;
	const VkResult res = vkCreateDescriptorPool(device.device, &ci, nullptr, &pool);
	if (res != VK_SUCCESS)
	{
		device.Fail("vkCreateDescriptorPool", res);
		return {};
	}
	return DescriptorPool(device.device, pool);
}

DescriptorSets AllocateDescriptorSets(const Device& device, VkDescriptorPool pool, std::span<const VkDescriptorSetLayout> layouts)
{
	DescriptorSets sets(layouts.size(), VK_NULL_HANDLE);
	if (!device.Ok() || pool == VK_NULL_HANDLE || layouts.empty())
		return sets;
	for (const VkDescriptorSetLayout layout : layouts)
	{
		if (layout == VK_NULL_HANDLE)
			return sets;
	}
	const VkDescriptorSetAllocateInfo ai = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, static_cast<u32>(layouts.size()), layouts.data()};
	const VkResult res = vkAllocateDescriptorSets(device.device, &ai, sets.data());
	if (res != VK_SUCCESS)
	{
		device.Fail("vkAllocateDescriptorSets (" + std::to_string(layouts.size()) + " sets)", res);
		std::fill(sets.begin(), sets.end(), VkDescriptorSet{VK_NULL_HANDLE});
	}
	return sets;
}

void UpdateDescriptorSets(const Device& device, std::span<const VkWriteDescriptorSet> writes)
{
	if (!device.Ok() || writes.empty())
		return;
	for (const VkWriteDescriptorSet& write : writes)
	{
		if (write.dstSet == VK_NULL_HANDLE)
			return;
	}
	vkUpdateDescriptorSets(device.device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
}
} // namespace lsfg
