// PS5 port frontend: the Vulkan renderer. Two passes a frame: the scene (background, reflections,
// the PS2 cases and the selection's outline glow) into an offscreen image, then FXAA from it into
// the display image with the UI (text and shapes) on top.
//
// Written for what the PS5 driver runs: classic render passes, no MSAA (its resolve is a CPU copy),
// no mip chains, no image queries in shaders, 16-bit indices, push constants of 128 bytes.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_math.h"
#include "fe_text.h"
#include "fe_vk.h"

#include <string>
#include <vector>

namespace fe
{
struct Texture
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	uint32_t width = 0, height = 0;
};

struct BoxDraw
{
	Mat4 model;
	VkDescriptorSet set = VK_NULL_HANDLE; // cover, placeholder, spine
	float brightness = 1, cover_mix = 0, selected = 0;
	float sheen_pos = 0, sheen_strength = 0, alpha = 1;
};

struct HaloDraw
{
	bool enabled = false;
	Mat4 model;
	float half_w = 0, half_h = 0, z = 0, margin = 0;
	float color[4] = {1, 1, 1, 1}; // rgb, intensity
	float radius = 0.04f, line = 0.012f, glow_width = 0.08f;
};

struct BgParams
{
	float glow_color[4];
	float glow_pos[4];
	float floor_glow[4];
	float top[4];
	float mid[4];
	float bottom[4];
	float misc[4];
};

// Texture changes within the UI vertex stream (AI-assisted).
struct UiImageRange
{
	uint32_t first = 0, count = 0;
	VkDescriptorSet set = VK_NULL_HANDLE;
};

struct FrameDesc
{
	Mat4 view_proj;
	Vec3 cam_pos;
	float time = 0;
	float glow[4] = {0.5f, 0.4f, 1.0f, 1.0f};
	float floor_y = -1, reflect_strength = 0.3f, reflect_falloff = 2.0f;
	BgParams bg = {};
	std::vector<BoxDraw> reflections;
	std::vector<BoxDraw> boxes;
	HaloDraw halo;
	std::vector<UiVertex> ui;
	std::vector<UiImageRange> ui_images;
	float fade = 1.0f;
};

class Renderer
{
public:
	// `out_images`: the display images (the swapchain's, or offscreen images on a PC), in
	// `out_format`, `width` x `height`; they end each frame in `out_final_layout`.
	bool Init(Vk* vk, VkPhysicalDevice physical_device, VkDevice device, uint32_t queue_family, VkQueue queue,
		uint32_t width, uint32_t height, VkFormat out_format, const std::vector<VkImage>& out_images,
		VkImageLayout out_final_layout);
	void Shutdown();

	// An RGBA8 (4 bytes a pixel) or R8 texture; the upload happens with the next frame.
	Texture* CreateTexture(uint32_t w, uint32_t h, VkFormat format, const void* pixels);
	void DestroyTexture(Texture* t); // freed once the GPU is done with it

	VkDescriptorSet AllocTextureSet(Texture* t0, Texture* t1, Texture* t2);
	void FreeTextureSet(VkDescriptorSet set);

	void SetAtlas(Texture* atlas);

	// Waits until the next frame's resources (its command buffer and the semaphores the platform
	// pairs with it) are free; Render does this itself when it wasn't called.
	bool WaitForSlot();

	// Renders into display image `out_index`, waiting for `wait` and signalling `signal` (either may
	// be null). Returns false on a Vulkan error (error() says which).
	bool Render(const FrameDesc& f, uint32_t out_index, VkSemaphore wait, VkSemaphore signal);

	// PC only: the display image as RGBA8 rows (its format must be R8G8B8A8 or B8G8R8A8).
	bool ReadPixels(uint32_t out_index, std::vector<uint8_t>& rgba);

	void WaitIdle();
	const std::string& error() const { return m_error; }

	uint32_t width() const { return m_width; }
	uint32_t height() const { return m_height; }

private:
	struct Buffer
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void* mapped = nullptr;
		VkDeviceSize size = 0;
	};
	struct Upload
	{
		Texture* texture;
		Buffer staging;
	};
	struct Slot
	{
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		Buffer ubo;
		Buffer ui;
		VkDescriptorSet frame_set = VK_NULL_HANDLE;
		std::vector<Buffer> dead_buffers;
		std::vector<Texture*> dead_textures;
		std::vector<VkDescriptorSet> dead_sets;
		bool submitted = false;
	};

	bool Fail(const char* what, VkResult r);
	uint32_t FindMemory(uint32_t type_bits, VkMemoryPropertyFlags want) const;
	bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out);
	void DestroyBuffer(Buffer& b);
	bool CreateImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
		Texture& out);
	void DestroyImage(Texture& t);
	bool CreatePipelines();
	VkShaderModule Module(const uint32_t* words, size_t bytes);
	bool CreateBoxMesh();
	void Reclaim(Slot& s);
	Slot& RetireSlot(); // where a resource retired now waits: the last submitted frame's slot
	void Barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
		VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage);

	Vk* m_vk = nullptr;
	VkPhysicalDevice m_pd = VK_NULL_HANDLE;
	VkDevice m_dev = VK_NULL_HANDLE;
	VkQueue m_queue = VK_NULL_HANDLE;
	uint32_t m_queue_family = 0;
	VkPhysicalDeviceMemoryProperties m_mem = {};
	uint32_t m_width = 0, m_height = 0;
	VkFormat m_out_format = VK_FORMAT_UNDEFINED;
	VkImageLayout m_out_final_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	std::vector<VkImage> m_out_images;
	std::vector<VkImageView> m_out_views;
	std::vector<VkFramebuffer> m_out_fbs;

	Texture m_scene;
	Texture m_depth;
	VkFramebuffer m_scene_fb = VK_NULL_HANDLE;
	VkRenderPass m_scene_pass = VK_NULL_HANDLE;
	VkRenderPass m_final_pass = VK_NULL_HANDLE;

	VkDescriptorSetLayout m_set0_layout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_set1_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_layout = VK_NULL_HANDLE;
	VkDescriptorPool m_pool = VK_NULL_HANDLE;
	VkSampler m_sampler = VK_NULL_HANDLE;

	VkPipeline m_bg = VK_NULL_HANDLE;
	VkPipeline m_box = VK_NULL_HANDLE;
	VkPipeline m_box_reflect = VK_NULL_HANDLE;
	VkPipeline m_halo = VK_NULL_HANDLE;
	VkPipeline m_post = VK_NULL_HANDLE;
	VkPipeline m_ui = VK_NULL_HANDLE;

	Buffer m_box_vb, m_box_ib;
	Buffer m_quad_vb; // the full-screen triangle, then the halo's six corners
	uint32_t m_box_index_count = 0;

	Texture* m_white = nullptr;
	Texture* m_atlas = nullptr;
	VkDescriptorSet m_post_set = VK_NULL_HANDLE;
	VkDescriptorSet m_atlas_set = VK_NULL_HANDLE;

	VkCommandPool m_cmd_pool = VK_NULL_HANDLE;
	static constexpr int kSlots = 2;
	Slot m_slots[kSlots];
	int m_slot = 0;
	std::vector<Upload> m_pending;
	std::vector<bool> m_out_initialized;

	std::string m_error;
};

// The case's half extents in world units (1 unit = 100 mm): a PS2 DVD case is 135 x 190 x 14 mm.
constexpr float kBoxHalfW = 0.675f, kBoxHalfH = 0.95f, kBoxHalfD = 0.07f;
} // namespace fe
