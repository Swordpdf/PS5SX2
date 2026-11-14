// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Lossless Scaling frame generation (LSFG), ported from the Eden emulator
// (git.eden-emu.dev, commit 67bada77f8a43a90da2e94e89b8e7da73c256989, src/video_core/renderer_vulkan/present/lsfg_*).
// Eden's passes are written against its own Vulkan wrapper (vk::Image, vk::Buffer, MemoryAllocator, Device,
// present/util.h). This header is the part of that wrapper they use, redone on plain Vulkan and VMA (PCSX2's allocator):
// move-only handles, a command buffer with the calls the passes make, and the create helpers. Nothing throws (PS5SX2 is
// built with -fno-exceptions): a failed create leaves a null handle and the Device keeps the first error, which the
// caller checks once the chain is built (lsfg_backend.h).
//
// No PCSX2 dependency beyond VKLoader.h on the console: the host test (ps5/coreorbis/tests/lsfg) builds the same files
// against the system's Vulkan loader and lavapipe.

#pragma once

#ifdef ORBIS_VULKAN
#include "GS/Renderers/Vulkan/VKLoader.h"
#else
#include <vulkan/vulkan.h>
#ifndef VMA_STATIC_VULKAN_FUNCTIONS
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#endif
#include "vk_mem_alloc.h"
#endif

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lsfg
{
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using f32 = float;

const char* VkResultName(VkResult result);

// Eden's Device and MemoryAllocator in one: the handles the passes need, and the first thing that failed.
struct Device
{
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VmaAllocator allocator = VK_NULL_HANDLE;
	mutable std::string error;

	bool Ok() const { return error.empty(); }
	// Keeps the first failure only: what broke first is what the log needs.
	void Fail(const std::string& what, VkResult result) const;
};

// A device object with its destroy call (vk::ImageView, vk::Sampler, ...): move-only, null when its create failed.
template <typename T, void (*Destroy)(VkDevice, T)>
class Handle
{
public:
	Handle() = default;
	Handle(VkDevice device_, T handle_) : device{device_}, handle{handle_} {}
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	Handle(Handle&& other) noexcept { *this = std::move(other); }
	Handle& operator=(Handle&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			device = std::exchange(other.device, VkDevice{VK_NULL_HANDLE});
			handle = std::exchange(other.handle, T{VK_NULL_HANDLE});
		}
		return *this;
	}
	~Handle() { Reset(); }

	void Reset()
	{
		if (handle != VK_NULL_HANDLE)
			Destroy(device, handle);
		handle = VK_NULL_HANDLE;
	}

	T operator*() const { return handle; }
	explicit operator bool() const { return handle != VK_NULL_HANDLE; }

private:
	VkDevice device = VK_NULL_HANDLE;
	T handle = VK_NULL_HANDLE;
};

void DestroyImageView(VkDevice device, VkImageView handle);
void DestroySampler(VkDevice device, VkSampler handle);
void DestroyShaderModule(VkDevice device, VkShaderModule handle);
void DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout handle);
void DestroyPipelineLayout(VkDevice device, VkPipelineLayout handle);
void DestroyPipeline(VkDevice device, VkPipeline handle);
void DestroyDescriptorPool(VkDevice device, VkDescriptorPool handle);

using ImageView = Handle<VkImageView, DestroyImageView>;
using Sampler = Handle<VkSampler, DestroySampler>;
using ShaderModule = Handle<VkShaderModule, DestroyShaderModule>;
using DescriptorSetLayout = Handle<VkDescriptorSetLayout, DestroyDescriptorSetLayout>;
using PipelineLayout = Handle<VkPipelineLayout, DestroyPipelineLayout>;
using Pipeline = Handle<VkPipeline, DestroyPipeline>;
using DescriptorPool = Handle<VkDescriptorPool, DestroyDescriptorPool>;
// Freed with their pool.
using DescriptorSets = std::vector<VkDescriptorSet>;

// An image and its memory (VMA).
class Image
{
public:
	Image() = default;
	Image(VmaAllocator allocator_, VkImage image_, VmaAllocation allocation_)
		: allocator{allocator_}, image{image_}, allocation{allocation_}
	{
	}
	Image(const Image&) = delete;
	Image& operator=(const Image&) = delete;
	Image(Image&& other) noexcept { *this = std::move(other); }
	Image& operator=(Image&& other) noexcept;
	~Image() { Reset(); }
	void Reset();

	VkImage operator*() const { return image; }
	explicit operator bool() const { return image != VK_NULL_HANDLE; }

private:
	VmaAllocator allocator = VK_NULL_HANDLE;
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
};

// A buffer the CPU writes (Eden's MemoryUsage::Upload): mapped for its whole life.
class Buffer
{
public:
	Buffer() = default;
	Buffer(VmaAllocator allocator_, VkBuffer buffer_, VmaAllocation allocation_, void* mapped_, VkDeviceSize size_)
		: allocator{allocator_}, buffer{buffer_}, allocation{allocation_}, mapped{mapped_}, size{size_}
	{
	}
	Buffer(const Buffer&) = delete;
	Buffer& operator=(const Buffer&) = delete;
	Buffer(Buffer&& other) noexcept { *this = std::move(other); }
	Buffer& operator=(Buffer&& other) noexcept;
	~Buffer() { Reset(); }
	void Reset();

	VkBuffer operator*() const { return buffer; }
	explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
	std::span<u8> Mapped() const { return {static_cast<u8*>(mapped), mapped ? static_cast<size_t>(size) : 0u}; }
	void Flush() const;

private:
	VmaAllocator allocator = VK_NULL_HANDLE;
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	void* mapped = nullptr;
	VkDeviceSize size = 0;
};

// Eden's vk::CommandBuffer: the calls the passes record.
class CommandBuffer
{
public:
	CommandBuffer() = default;
	explicit CommandBuffer(VkCommandBuffer cmdbuf_) : cmdbuf{cmdbuf_} {}

	VkCommandBuffer operator*() const { return cmdbuf; }

	void PipelineBarrier(VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage, VkDependencyFlags flags,
		std::span<const VkMemoryBarrier> memory, std::span<const VkBufferMemoryBarrier> buffers,
		std::span<const VkImageMemoryBarrier> images) const
	{
		vkCmdPipelineBarrier(cmdbuf, src_stage, dst_stage, flags, static_cast<u32>(memory.size()), memory.data(),
			static_cast<u32>(buffers.size()), buffers.data(), static_cast<u32>(images.size()), images.data());
	}
	void BindPipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline) const { vkCmdBindPipeline(cmdbuf, bind_point, pipeline); }
	void BindDescriptorSets(VkPipelineBindPoint bind_point, VkPipelineLayout layout, u32 first, VkDescriptorSet set) const
	{
		vkCmdBindDescriptorSets(cmdbuf, bind_point, layout, first, 1, &set, 0, nullptr);
	}
	void Dispatch(u32 x, u32 y, u32 z) const { vkCmdDispatch(cmdbuf, x, y, z); }

private:
	VkCommandBuffer cmdbuf = VK_NULL_HANDLE;
};

// Eden's present/util.h helpers (CreateWrapped*), each null on failure (the Device says why).
Image CreateImage(const Device& device, const VkImageCreateInfo& info);
Buffer CreateUploadBuffer(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage);
ImageView CreateImageView(const Device& device, VkImage image, VkFormat format);
Sampler CreateSampler(const Device& device, const VkSamplerCreateInfo& info);
ShaderModule CreateShaderModule(const Device& device, std::span<const u32> code);
// One binding per descriptor, numbered from 0 in order (the order lsfg_translate.cpp gives the shaders' bindings).
DescriptorSetLayout CreateDescriptorSetLayout(const Device& device, std::span<const VkDescriptorType> types, VkShaderStageFlags stages);
PipelineLayout CreatePipelineLayout(const Device& device, VkDescriptorSetLayout layout);
Pipeline CreateComputePipeline(const Device& device, VkPipelineLayout layout, VkShaderModule module, const char* entry_point);
DescriptorPool CreateDescriptorPool(const Device& device, u32 descriptors_per_type, u32 max_sets,
	std::initializer_list<VkDescriptorType> types);
DescriptorSets AllocateDescriptorSets(const Device& device, VkDescriptorPool pool, std::span<const VkDescriptorSetLayout> layouts);
void UpdateDescriptorSets(const Device& device, std::span<const VkWriteDescriptorSet> writes);
} // namespace lsfg
