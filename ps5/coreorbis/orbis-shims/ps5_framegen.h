// RPCS3-PS5 (m73): frame generation. AMD FSR3's frame interpolation (FidelityFX SDK 2.3.0, MIT) on plain Vulkan: from two frames
// of the game, the frame halfway between them, shown before the second one (a game held at 30 fps is shown at 60).
//
// A PS3 game gives no motion vectors or depth, so this is FSR3's optical flow path alone (FFX_FG_OF_ONLY in the shaders):
//   optical flow    the luma of each frame and its pyramid (7 levels), a block search from the coarsest level down with a filter
//                   and an upscale at each level (FSR3's "v5" optical flow), scene change detection from luma histograms
//   interpolation   the flow's vectors halved and pushed to where each block is halfway (the vector field); then (m73b) each
//                   pixel tries no motion, the field, its 4 nearest cells, its block's flow and (m73f) the 4 blocks' around it,
//                   (m74) its quad's motion in the last generated frame and the best of them refined to a fraction of a pixel,
//                   and keeps the motion under which the previous frame half a vector back and the current one half a vector
//                   ahead agree, a motion that is not the flow's own where it samples counting less (m73g); where none agrees
//                   (the edge of something in front), the sample another motion explains is dropped; a pixel unchanged for 8
//                   frames (HUD, text) does not move (docs/algorithm.md has the details)
// FSR3's passes for the game's motion vectors, the disocclusion mask and the inpainting are not run. The shaders are FSR3's HLSL
// compiled to SPIR-V (tools/build-shaders.sh, sources in shaders/); this file and its .cpp are
// the host side (the SDK's ffx_opticalflow.cpp and ffx_frameinterpolation.cpp redone on Vulkan, without the SDK's backend).
//
// No RPCS3 dependency: the host tool tools/fg-test.cpp runs the same code on a software Vulkan device.
//
// PS5SX2 (2026-10-08, AI-assisted): this copy comes from Swordpdf's RPCS3-PS5 / ps5-framegen (GPL-2.0-only there; relicensed by its
// author for PS5SX2) with two changes: the Vulkan commands are PCSX2's loader's (VKLoader.h), and the frames may be in another format
// than R8G8B8A8 (the swapchain's, so that PCSX2's present pipelines draw into them). Needs proper testing on ps5vk.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef ORBIS_VULKAN
#include "GS/Renderers/Vulkan/VKLoader.h"
#else
#include <vulkan/vulkan_core.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace ps5::framegen
{
	// ps5.yml "Frame Generation" (also in a step, a test mode or a game's Title Steps): whether the game's frames get the frame
	// between them shown too. The renderer (in RPCS3-PS5, rpcs3/Emu/RSX/VK/VKPresent.cpp) reads it at every flip.
	void set_wanted(bool on);
	bool wanted();

	// m73b: ps5.yml "Frame Generation Limit": 0 (the default) leaves the game at its own frame rate (up to RPCS3's frame limit) and
	// shows one generated frame after each of its frames; 30, 50 or 60 holds it at that rate (RPCS3's frame limit) while frame
	// generation is on. 30 on a 120 Hz display is m73's even 30 -> 60.
	void set_limit(std::uint32_t fps);
	std::uint32_t limit();

	// m73b: ps5.yml "Frame Generation Debug": the generated frames show the interpolation's decisions in colour instead
	// (interpolator::set_debug_view).
	void set_debug(bool on);
	bool debug();

	// m74: ps5.yml "Frame Generation Multiplier": 2 (the default) shows one generated frame between two of the game's, 3 two (at a
	// third and two thirds of the way: 40 fps shown at 120), 4 three (30 at 120). Each extra frame delays the game's own frame by
	// one more refresh, so 2 has the least input lag.
	void set_multiplier(std::uint32_t times);
	std::uint32_t multiplier();

	class interpolator
	{
	public:
		interpolator() = default;
		interpolator(const interpolator&) = delete;
		interpolator& operator=(const interpolator&) = delete;
		~interpolator();

		// The passes, images and buffers for frames of width x height (the size of the picture shown). false (and `error`) when
		// the device lacks something or memory runs out.
		// PS5SX2: `frame_format` is the frames' format (default R8G8B8A8_UNORM; B8G8R8A8_UNORM works the same: the passes only sample them).
		bool create(VkPhysicalDevice physical, VkDevice device, std::uint32_t width, std::uint32_t height, std::string& error,
			VkFormat frame_format = VK_FORMAT_R8G8B8A8_UNORM);
		void destroy();

		bool ready() const { return m_device != VK_NULL_HANDLE; }
		std::uint32_t width() const { return m_width; }
		std::uint32_t height() const { return m_height; }

		// The frames are R8G8B8A8_UNORM images kept in VK_IMAGE_LAYOUT_GENERAL (usage: transfer source and destination, sampled,
		// colour attachment). Each frame the caller calls prepare() (it puts the images in GENERAL the first time), copies (or
		// draws) this frame's picture into frame_image(), then calls record().
		static constexpr VkFormat k_format = VK_FORMAT_R8G8B8A8_UNORM;
		void prepare(VkCommandBuffer cmd);
		VkImage frame_image() const;
		VkImageView frame_view() const;
		VkImage previous_image() const;

		// Records the optical flow (always: the next frame needs this one's) and, unless `reset` (the first frame, a cut, a frame
		// that does not follow the previous one), the interpolation into output_image() (GENERAL; the barriers for the caller's
		// transfer reads of output_image() and frame_image() are included). Returns whether output_image() was made.
		bool record(VkCommandBuffer cmd, bool reset);
		VkImage output_image() const { return m_output.image; }

		// m74: the frames generated between two of the game's (1 to 3; set before record()). record() makes the first one, at a 1 / (n + 1)
		// of the way; interpolate_next(cmd, i) makes the i-th of the others (1 to n - 1) in output_image(), after the caller has copied
		// the one before (the barriers included). The first one keeps the still history and the quads' motions; the others only read them.
		void set_generated(std::uint32_t n) { m_generated = n < 1 ? 1 : n > 3 ? 3 : n; }
		std::uint32_t generated() const { return m_generated; }
		bool interpolate_next(VkCommandBuffer cmd, std::uint32_t i);

		// After the frame's images were used (copied for presenting): frame_image() becomes previous_image().
		void advance();

		// For tests: the optical flow at level 0 (R16G16_SINT, pixels), the vector field (R32_UINT x and y, packed), the SCD output.
		VkImage debug_flow() const { return m_flow_out.image; }
		VkImage debug_field_x() const { return m_field_x.image; }
		VkImage debug_field_y() const { return m_field_y.image; }
		VkImage debug_scd() const { return m_scd_output.image; }
		VkImage debug_luma(int set, int level) const { return m_luma[set][level].image; }
		std::uint32_t flow_width() const { return m_flow_out.w; }
		std::uint32_t flow_height() const { return m_flow_out.h; }

		std::uint64_t frames() const { return m_frames; }
		std::uint64_t interpolated() const { return m_interpolated; }

		// m73b: the decisions of the interpolation in colour instead of the picture (grey: no motion, green: a motion, red / blue:
		// an edge where the previous / current frame's side was kept, magenta: an edge blended). For tests.
		void set_debug_view(bool on) { m_debug_view = on; }

		// m73b: the GPU time of record()'s passes (timestamps after the frame's own work and after the passes, read back k_slots
		// frames later, never waiting): the mean and the most per frame since take_gpu_time() last ran, in ms. false when the
		// device has no timestamps or no frame was measured since.
		bool take_gpu_time(double& mean_ms, double& max_ms, std::uint32_t& count);
		// m74: and of that, the optical flow's part (the rest is the interpolation's)
		bool take_gpu_time(double& mean_ms, double& max_ms, std::uint32_t& count, double& flow_mean_ms);

	private:
		struct image_t
		{
			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkFormat format = VK_FORMAT_UNDEFINED;
			std::uint32_t w = 0, h = 0;
		};

		struct pipeline_t
		{
			VkShaderModule module = VK_NULL_HANDLE;
			VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
			VkPipelineLayout layout = VK_NULL_HANDLE;
			VkPipeline pipeline = VK_NULL_HANDLE;
			std::uint32_t shader = 0; // index into the shader table
			// PS5SX2: the binding number each of the shader's bindings has here, in its table's order (create(), "compact").
			std::vector<std::uint32_t> slot;
		};

		// One dispatch of a frame: its pipeline, its descriptor set (one for each frame parity), the uniform blocks it reads
		// (offsets inside a slot of the constant ring) and its size.
		struct dispatch_t
		{
			std::uint32_t pipeline = 0;
			VkDescriptorSet set[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
			std::uint32_t ubo_offsets[2] = {0, 0};
			std::uint32_t ubo_count = 0;
			std::uint32_t groups[3] = {1, 1, 1};
			bool interpolation = false; // part of the interpolation (skipped on a reset frame)
		};

		bool make_image(image_t& img, VkFormat format, std::uint32_t w, std::uint32_t h, VkImageUsageFlags usage, std::string& error);
		void free_image(image_t& img);
		std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags props) const;
		bool make_pipelines(std::string& error);
		bool make_dispatches(std::string& error);
		void write_constants(std::uint32_t slot, bool reset);
		void read_gpu_time(std::uint32_t slot);

		VkPhysicalDevice m_physical = VK_NULL_HANDLE;
		VkDevice m_device = VK_NULL_HANDLE;
		VkPhysicalDeviceMemoryProperties m_memory_props{};
		std::uint32_t m_width = 0, m_height = 0;

		image_t m_color[2];        // the frames, by parity: m_color[p] is this frame's when (m_frames & 1) == p
		image_t m_luma[2][7];      // the optical flow's luma pyramids (R8_UINT), by parity
		image_t m_flow[2][7];      // the optical flow's vectors at each level (R16G16_SINT), the two sets the levels alternate between
		image_t m_flow_out;        // the result at level 0
		image_t m_scd_histogram, m_scd_previous_histogram, m_scd_temp, m_scd_output;
		image_t m_field_x, m_field_y; // the interpolation's vector field (R32_UINT, packed with priorities)
		image_t m_output;          // the interpolated frame
		image_t m_static;          // m73b: for each pixel, the frames in a row it has not changed (R8_UINT)
		image_t m_quad_motion;     // m74: each 2x2 quad's motion in the last generated frame (R16G16_SFLOAT, half vectors in pixels)

		VkBuffer m_counters = VK_NULL_HANDLE; // FSR3's counters (frames since a reset)
		VkDeviceMemory m_counters_memory = VK_NULL_HANDLE;
		VkBuffer m_ubo = VK_NULL_HANDLE;      // the constants, a slot per frame in flight
		VkDeviceMemory m_ubo_memory = VK_NULL_HANDLE;
		std::uint8_t* m_ubo_map = nullptr;
		std::uint32_t m_ubo_block = 256;      // one uniform block's stride (minUniformBufferOffsetAlignment rounded up)
		std::uint32_t m_ubo_slot = 0;         // one slot's size

		VkSampler m_sampler = VK_NULL_HANDLE;
		VkDescriptorPool m_pool = VK_NULL_HANDLE;
		std::vector<pipeline_t> m_pipelines;
		std::vector<dispatch_t> m_dispatches;

		std::uint32_t m_generated = 1;        // m74: frames generated between two of the game's
		std::uint32_t m_last_slot = 0;        // m74: record()'s constant slot and frame parity, for interpolate_next()
		std::uint32_t m_last_parity = 0;
		bool m_last_made = false;
		std::uint32_t m_interp_dispatch = ~0u; // m74: the interpolation's entry in m_dispatches

		std::uint64_t m_frames = 0;           // frames recorded
		std::uint64_t m_interpolated = 0;
		std::uint32_t m_flow_frame_index = 0; // the optical flow's frame index (0 after a reset)
		bool m_cleared = false;               // the work images are in GENERAL, the counters were zeroed
		bool m_prepared = false;              // the frames are in GENERAL
		bool m_debug_view = false;

		// m73b: the GPU timer (m74: three timestamps a constant slot)
		VkQueryPool m_queries = VK_NULL_HANDLE;
		PFN_vkCmdWriteTimestamp m_write_timestamp = nullptr;
		double m_timestamp_ns = 0.0;          // a tick
		std::uint64_t m_timestamp_mask = 0;   // the valid bits
		std::uint32_t m_timed_slots = 0;      // the slots whose timestamps are written and not read yet (bits)
		double m_gpu_ms_sum = 0.0, m_gpu_ms_max = 0.0, m_gpu_flow_ms_sum = 0.0;
		std::uint32_t m_gpu_count = 0;
	};
}
