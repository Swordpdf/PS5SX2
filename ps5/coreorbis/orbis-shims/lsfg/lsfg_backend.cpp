// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): see lsfg_backend.h.

#include "lsfg_backend.h"
#include "lsfg_chain.h"
#include "lsfg_shaders.h"
#include "lsfg_translate.h"

namespace lsfg
{
namespace
{
	// Eden: LSFG_REQUIRED_FRAMES / LSFG_RECURRENCE_FRAMES. The shared passes need the frame before (the mipmaps of two
	// frames), the generation the alpha passes of the last two (their history slots).
	constexpr u64 SHARED_FROM = 1;
	constexpr u64 GENERATE_FROM = 2;
} // namespace

Backend::Backend() = default;

Backend::~Backend()
{
	Destroy();
}

bool Backend::Create(VkPhysicalDevice physical, VkDevice device, VmaAllocator allocator, const ShaderModules& modules, u32 width,
	u32 height, VkFormat frame_format, f32 flow_scale, std::string& error, int wide_flow)
{
	Destroy();
	m_device.physical = physical;
	m_device.device = device;
	m_device.allocator = allocator;
	m_device.error.clear();
	m_notes.clear();

	// What the driver stores (2026-11-14: ps5vk takes RGBA8 and RGBA16F storage images, not R8_UNORM ones; ps5vk_image.c).
	const auto storage = [&](VkFormat f) {
		VkFormatProperties props = {};
		vkGetPhysicalDeviceFormatProperties(physical, f, &props);
		return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
	};
	const auto sampled = [&](VkFormat f) {
		VkFormatProperties props = {};
		vkGetPhysicalDeviceFormatProperties(physical, f, &props);
		return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
	};
	for (const VkFormat f : {LSFG_DEFAULT_FORMAT, LSFG_MOTION_FORMAT})
	{
		if (!storage(f))
		{
			error = "the driver takes no storage image in format " + std::to_string(static_cast<int>(f));
			return false;
		}
	}
	if (!sampled(frame_format))
	{
		error = "the driver cannot sample the frames' format " + std::to_string(static_cast<int>(frame_format));
		return false;
	}
	const bool r8_ok = storage(LSFG_FLOW_FORMAT);
	const bool wide = wide_flow == 1 || (wide_flow == -1 && !r8_ok);
	ShaderModules retyped;
	if (wide)
	{
		if (!storage(VK_FORMAT_R32_SFLOAT))
		{
			error = "the driver takes neither R8_UNORM nor R32_SFLOAT storage images (the flow images)";
			return false;
		}
		retyped = modules;
		size_t changed = 0;
		for (auto& [id, words] : retyped)
			changed += RetypeStorageImages(words, SPIRV_IMAGE_FORMAT_R8, SPIRV_IMAGE_FORMAT_R32F);
		m_notes = "the flow images in R32_SFLOAT (" + std::string(r8_ok ? "asked for" : "the driver takes no R8_UNORM storage image") +
		          "; " + std::to_string(changed) + " r8 storage declarations made r32f)";
	}
	else
		m_notes = "the flow images in R8_UNORM";
	SetLsfgFlowFormat(wide ? VK_FORMAT_R32_SFLOAT : LSFG_FLOW_FORMAT);

	// The border samplers (2026-11-14: ps5vk refuses CLAMP_TO_BORDER and any border colour but transparent black; ps5vk_image.c):
	// tried once here, so that the error says what the driver lacks rather than "vkCreateSampler failed".
	for (const bool white : {false, true})
	{
		VkSamplerCreateInfo ci = {};
		ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
		ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		ci.compareOp = VK_COMPARE_OP_NEVER;
		ci.maxLod = VK_LOD_CLAMP_NONE;
		ci.borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
		VkSampler probe = VK_NULL_HANDLE;
		const VkResult res = vkCreateSampler(device, &ci, nullptr, &probe);
		if (res != VK_SUCCESS)
		{
			error = std::string("the driver takes no sampler that clamps to a ") + (white ? "white" : "transparent black") +
			        " border (" + VkResultName(res) + "), which Lossless Scaling's shaders read past the picture's edge; clamping to " +
			        "the edge instead gives wrong frames, so a driver with border samplers is needed";
			return false;
		}
		vkDestroySampler(device, probe, nullptr);
	}

	m_shaders = std::make_unique<LsfgShaders>(m_device, wide ? retyped : modules);
	if (!m_shaders->IsValid())
	{
		error = m_device.error.empty() ? std::string("no shaders") : m_device.error;
		Destroy();
		return false;
	}
	const VkExtent2D extent{width, height};
	m_chain = std::make_unique<LsfgChain>(m_device, *m_shaders, extent, frame_format, flow_scale);
	m_output = std::make_unique<LsfgImage>(m_device, extent, kOutputFormat,
		VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	if (m_device.Ok())
		m_chain->SetTarget(m_device, 1, 0, 0, m_output->View());
	if (!m_device.Ok())
	{
		error = m_device.error;
		Destroy();
		return false;
	}
	m_width = width;
	m_height = height;
	m_flow_scale = flow_scale;
	m_count = 0;
	return true;
}

void Backend::Destroy()
{
	m_chain.reset();
	m_output.reset();
	m_shaders.reset();
	m_width = m_height = 0;
	m_count = 0;
}

VkImage Backend::FrameImage() const
{
	return m_chain ? m_chain->Input(m_count).Handle() : VK_NULL_HANDLE;
}

VkImage Backend::FrameImage(u32 index) const
{
	return m_chain ? m_chain->Input(index).Handle() : VK_NULL_HANDLE;
}

VkImage Backend::OutputImage() const
{
	return m_output ? m_output->Handle() : VK_NULL_HANDLE;
}

bool Backend::Record(VkCommandBuffer cmd)
{
	if (!m_chain)
		return false;
	const CommandBuffer cmdbuf(cmd);
	m_chain->Prepare(cmdbuf);
	const u64 count = m_count++;
	m_frames++;
	// The caller drew the frame and left it in GENERAL: what the chain's barriers start from.
	m_chain->Input(count).SetLayout(VK_IMAGE_LAYOUT_GENERAL);
	if (count < SHARED_FROM)
		return false;
	m_chain->DispatchShared(cmdbuf, count);
	if (count < GENERATE_FROM)
		return false;
	m_chain->DispatchGeneration(cmdbuf, count, 1, 0, 0, m_output->Handle(), m_output->Extent());
	m_output->SetLayout(VK_IMAGE_LAYOUT_GENERAL);
	m_generated++;
	return true;
}
} // namespace lsfg
