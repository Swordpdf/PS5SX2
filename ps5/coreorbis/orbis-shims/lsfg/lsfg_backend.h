// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Lossless Scaling frame generation as PS5SX2's second frame generation method
// (PS5SX2/FrameGenMethod = Lossless; GSDeviceVK.cpp, "frame generation"). What Eden's FrameGen class
// (renderer_vulkan/present/frame_gen.cpp) does around the chain, shaped like PS5SX2's own interpolator
// (ps5_framegen.h) so that GSDeviceVK.cpp's pacing, 60/120 Hz output and present stay the same:
//   - the caller draws each new frame of the game into FrameImage() (the chain's input, in the swapchain's format) and
//     leaves it in GENERAL;
//   - Record() runs the shared passes and, from the third frame since a reset, makes the frame halfway between the last
//     two in OutputImage() (R8G8B8A8, in GENERAL), which the caller presents;
//   - one generated frame per game frame (the chain can make up to three; PS5SX2's pacing shows one).
// The shaders come from the player's own Lossless.dll (lossless_dll.h); nothing of Lossless Scaling ships with PS5SX2.

#pragma once

#include <memory>
#include <string>

#include "lossless_dll.h"
#include "lsfg_vk.h"

namespace lsfg
{
class LsfgChain;
class LsfgShaders;
class LsfgImage;

class Backend
{
public:
	static constexpr VkFormat kOutputFormat = VK_FORMAT_R8G8B8A8_UNORM;

	Backend();
	~Backend();
	Backend(const Backend&) = delete;
	Backend& operator=(const Backend&) = delete;

	// The shaders' pipelines and the chain's images for frames of width x height in `frame_format`, the optical flow at
	// `flow_scale` (1 = full size, 0.5 = half). false, with `error`, when something could not be made (all of it freed).
	// `wide_flow`: the flow images in R32_SFLOAT with the shaders' r8 storage declarations retyped to r32f: -1 when the driver takes
	// no R8_UNORM storage image (ps5vk), 0 never, 1 always (the host test compares the two). Notes() says what was chosen.
	bool Create(VkPhysicalDevice physical, VkDevice device, VmaAllocator allocator, const ShaderModules& modules, u32 width,
		u32 height, VkFormat frame_format, f32 flow_scale, std::string& error, int wide_flow = -1);
	const std::string& Notes() const { return m_notes; }
	void Destroy();

	bool Ready() const { return m_chain != nullptr; }
	u32 Width() const { return m_width; }
	u32 Height() const { return m_height; }
	f32 FlowScale() const { return m_flow_scale; }

	// The next frame has no neighbour (a load, a pause): two frames before one is generated again.
	void Reset() { m_count = 0; }
	// The image this frame of the game is drawn into, and the generated one.
	VkImage FrameImage() const;
	VkImage FrameImage(u32 index) const; // both (index 0, 1) for wrapping
	VkImage OutputImage() const;

	// After the frame was drawn into FrameImage() and put in GENERAL: records the passes into `cmd`; true when the
	// generated frame was written (to OutputImage(), left in GENERAL). Moves on to the next frame.
	bool Record(VkCommandBuffer cmd);

	u64 Frames() const { return m_frames; }
	u64 Generated() const { return m_generated; }

private:
	Device m_device;
	std::unique_ptr<LsfgShaders> m_shaders;
	std::unique_ptr<LsfgChain> m_chain;
	std::unique_ptr<LsfgImage> m_output;
	u32 m_width = 0;
	u32 m_height = 0;
	f32 m_flow_scale = 1.0f;
	u64 m_count = 0;
	u64 m_frames = 0;
	u64 m_generated = 0;
	std::string m_notes;
};
} // namespace lsfg
