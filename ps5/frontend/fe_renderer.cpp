// PS5 port frontend: the Vulkan renderer (see fe_renderer.h).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "fe_shaders.inc"

namespace fe
{
namespace
{
struct FrameUbo
{
	float view_proj[16];
	float cam_pos[4];
	float glow[4];
	float floor[4];
	float screen[4];
};

struct BoxPush
{
	float model[16];
	float p0[4];
	float p1[4];
	float p2[4];
	float p3[4];
};
static_assert(sizeof(BoxPush) == 128, "push constants are 128 bytes");

struct HaloPush
{
	float model[16];
	float size[4];
	float color[4];
	float shape[4];
	float unused[4];
};
static_assert(sizeof(HaloPush) == 128, "push constants are 128 bytes");

struct BoxVertex
{
	float pos[3];
	float normal[3];
	float uv[2];
	float face;
};

constexpr VkFormat kSceneFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
} // namespace

bool Renderer::Fail(const char* what, VkResult r)
{
	char buf[256];
	std::snprintf(buf, sizeof(buf), "%s failed (VkResult %d)", what, static_cast<int>(r));
	m_error = buf;
	return false;
}

uint32_t Renderer::FindMemory(uint32_t type_bits, VkMemoryPropertyFlags want) const
{
	for (uint32_t i = 0; i < m_mem.memoryTypeCount; i++)
		if ((type_bits & (1u << i)) && (m_mem.memoryTypes[i].propertyFlags & want) == want)
			return i;
	for (uint32_t i = 0; i < m_mem.memoryTypeCount; i++)
		if (type_bits & (1u << i))
			return i;
	return UINT32_MAX;
}

bool Renderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out)
{
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = size;
	bi.usage = usage;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkResult r = m_vk->vkCreateBuffer(m_dev, &bi, nullptr, &out.buffer);
	if (r != VK_SUCCESS)
		return Fail("vkCreateBuffer", r);
	VkMemoryRequirements req;
	m_vk->vkGetBufferMemoryRequirements(m_dev, out.buffer, &req);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex =
		FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	r = m_vk->vkAllocateMemory(m_dev, &ai, nullptr, &out.memory);
	if (r != VK_SUCCESS)
		return Fail("vkAllocateMemory (buffer)", r);
	r = m_vk->vkBindBufferMemory(m_dev, out.buffer, out.memory, 0);
	if (r != VK_SUCCESS)
		return Fail("vkBindBufferMemory", r);
	r = m_vk->vkMapMemory(m_dev, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
	if (r != VK_SUCCESS)
		return Fail("vkMapMemory", r);
	out.size = size;
	return true;
}

void Renderer::DestroyBuffer(Buffer& b)
{
	if (b.mapped)
		m_vk->vkUnmapMemory(m_dev, b.memory);
	if (b.buffer)
		m_vk->vkDestroyBuffer(m_dev, b.buffer, nullptr);
	if (b.memory)
		m_vk->vkFreeMemory(m_dev, b.memory, nullptr);
	b = Buffer();
}

bool Renderer::CreateImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
	Texture& out)
{
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = format;
	ii.extent = {w, h, 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = usage;
	ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkResult r = m_vk->vkCreateImage(m_dev, &ii, nullptr, &out.image);
	if (r != VK_SUCCESS)
		return Fail("vkCreateImage", r);
	VkMemoryRequirements req;
	m_vk->vkGetImageMemoryRequirements(m_dev, out.image, &req);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	r = m_vk->vkAllocateMemory(m_dev, &ai, nullptr, &out.memory);
	if (r != VK_SUCCESS)
		return Fail("vkAllocateMemory (image)", r);
	r = m_vk->vkBindImageMemory(m_dev, out.image, out.memory, 0);
	if (r != VK_SUCCESS)
		return Fail("vkBindImageMemory", r);
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = out.image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = format;
	vi.subresourceRange = {aspect, 0, 1, 0, 1};
	r = m_vk->vkCreateImageView(m_dev, &vi, nullptr, &out.view);
	if (r != VK_SUCCESS)
		return Fail("vkCreateImageView", r);
	out.width = w;
	out.height = h;
	return true;
}

void Renderer::DestroyImage(Texture& t)
{
	if (t.view)
		m_vk->vkDestroyImageView(m_dev, t.view, nullptr);
	if (t.image)
		m_vk->vkDestroyImage(m_dev, t.image, nullptr);
	if (t.memory)
		m_vk->vkFreeMemory(m_dev, t.memory, nullptr);
	t = Texture();
}

VkShaderModule Renderer::Module(const uint32_t* words, size_t bytes)
{
	VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = bytes;
	ci.pCode = words;
	VkShaderModule m = VK_NULL_HANDLE;
	const VkResult r = m_vk->vkCreateShaderModule(m_dev, &ci, nullptr, &m);
	if (r != VK_SUCCESS)
		Fail("vkCreateShaderModule", r);
	return m;
}

void Renderer::Barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
	VkImageLayout to, VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
	VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = src_access;
	b.dstAccessMask = dst_access;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {aspect, 0, 1, 0, 1};
	m_vk->vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// A PS2 case with rounded edges: each face is a grid whose outer rows wrap onto the rounded edge
// (the inner point is the face clamped to the box shrunk by the radius; the vertex sits one radius
// from it along the direction to the face point).
bool Renderer::CreateBoxMesh()
{
	const float h[3] = {kBoxHalfW, kBoxHalfH, kBoxHalfD};
	const float radius = 0.022f;
	const int band = 4;
	std::vector<BoxVertex> verts;
	std::vector<uint16_t> idx;

	// Sample coordinates along an axis of half size `hs`: dense in the rounded bands.
	auto samples = [&](float hs) {
		std::vector<float> s;
		const float inner = hs - radius;
		for (int k = band; k >= 1; k--)
			s.push_back(-(inner + radius * std::sin(k * (kPi * 0.5f) / band)));
		s.push_back(-inner);
		s.push_back(inner);
		for (int k = 1; k <= band; k++)
			s.push_back(inner + radius * std::sin(k * (kPi * 0.5f) / band));
		return s;
	};

	// face: axis n (0 x, 1 y, 2 z), sign, tangent axes a and b chosen so a x b = n*sign (CCW outside).
	struct Face
	{
		int n;
		float sign;
		int a, b;
		float face_id;
	};
	const Face faces[6] = {
		{2, 1, 0, 1, 0},  // front (+Z): a = +X, b = +Y
		{2, -1, 1, 0, 1}, // back (-Z)
		{0, -1, 1, 2, 2}, // spine (-X)
		{0, 1, 2, 1, 3},  // opening edge (+X)
		{1, 1, 2, 0, 4},  // top (+Y)
		{1, -1, 0, 2, 5}, // bottom (-Y)
	};
	for (const Face& f : faces)
	{
		const std::vector<float> sa = samples(h[f.a]);
		const std::vector<float> sb = samples(h[f.b]);
		const uint16_t base = static_cast<uint16_t>(verts.size());
		for (size_t j = 0; j < sb.size(); j++)
			for (size_t i = 0; i < sa.size(); i++)
			{
				float p[3];
				p[f.n] = f.sign * h[f.n];
				p[f.a] = sa[i];
				p[f.b] = sb[j];
				float inner[3], d[3];
				for (int k = 0; k < 3; k++)
				{
					const float lim = h[k] - radius;
					inner[k] = std::max(-lim, std::min(lim, p[k]));
					d[k] = p[k] - inner[k];
				}
				const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				BoxVertex v = {};
				for (int k = 0; k < 3; k++)
				{
					v.normal[k] = len > 0 ? d[k] / len : 0;
					v.pos[k] = inner[k] + v.normal[k] * radius;
				}
				// Texture coordinates from the flat face position.
				if (f.face_id == 0) // front: u across (+X), v down
				{
					v.uv[0] = (p[0] + h[0]) / (2 * h[0]);
					v.uv[1] = (h[1] - p[1]) / (2 * h[1]);
				}
				else if (f.face_id == 2) // spine: u from the back (-Z) to the front (+Z), v down
				{
					v.uv[0] = (p[2] + h[2]) / (2 * h[2]);
					v.uv[1] = (h[1] - p[1]) / (2 * h[1]);
				}
				else
				{
					v.uv[0] = 0;
					v.uv[1] = 0;
				}
				v.face = f.face_id;
				verts.push_back(v);
			}
		const uint16_t row = static_cast<uint16_t>(sa.size());
		for (size_t j = 0; j + 1 < sb.size(); j++)
			for (size_t i = 0; i + 1 < sa.size(); i++)
			{
				const uint16_t v0 = static_cast<uint16_t>(base + j * row + i);
				const uint16_t v1 = static_cast<uint16_t>(v0 + 1);
				const uint16_t v2 = static_cast<uint16_t>(v0 + row + 1);
				const uint16_t v3 = static_cast<uint16_t>(v0 + row);
				idx.insert(idx.end(), {v0, v1, v2, v0, v2, v3});
			}
	}
	if (!CreateBuffer(verts.size() * sizeof(BoxVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_box_vb) ||
		!CreateBuffer(idx.size() * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m_box_ib))
		return false;
	std::memcpy(m_box_vb.mapped, verts.data(), verts.size() * sizeof(BoxVertex));
	std::memcpy(m_box_ib.mapped, idx.data(), idx.size() * sizeof(uint16_t));
	m_box_index_count = static_cast<uint32_t>(idx.size());
	return true;
}

namespace
{
struct PipelineDesc
{
	const uint32_t* vs;
	size_t vs_bytes;
	const uint32_t* fs;
	size_t fs_bytes;
	const VkVertexInputBindingDescription* bindings;
	uint32_t binding_count;
	const VkVertexInputAttributeDescription* attrs;
	uint32_t attr_count;
	enum Blend
	{
		None,
		Alpha,
		Additive
	} blend;
	bool depth_test, depth_write;
	VkRenderPass pass;
};
} // namespace

bool Renderer::CreatePipelines()
{
	auto make = [&](const PipelineDesc& d, VkPipeline& out) -> bool {
		VkShaderModule vs = Module(d.vs, d.vs_bytes);
		VkShaderModule fs = Module(d.fs, d.fs_bytes);
		if (!vs || !fs)
			return false;
		VkPipelineShaderStageCreateInfo stages[2] = {};
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vs;
		stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fs;
		stages[1].pName = "main";

		VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		vi.vertexBindingDescriptionCount = d.binding_count;
		vi.pVertexBindingDescriptions = d.bindings;
		vi.vertexAttributeDescriptionCount = d.attr_count;
		vi.pVertexAttributeDescriptions = d.attrs;
		VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1;
		vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode = VK_CULL_MODE_NONE;
		rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		ds.depthTestEnable = d.depth_test;
		ds.depthWriteEnable = d.depth_write;
		ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
		VkPipelineColorBlendAttachmentState att = {};
		att.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		if (d.blend != PipelineDesc::None)
		{
			att.blendEnable = VK_TRUE;
			att.colorBlendOp = VK_BLEND_OP_ADD;
			att.alphaBlendOp = VK_BLEND_OP_ADD;
			if (d.blend == PipelineDesc::Alpha)
			{
				att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			}
			else
			{
				att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
				att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
				att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
				att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			}
		}
		VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1;
		cb.pAttachments = &att;
		const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dy.dynamicStateCount = 2;
		dy.pDynamicStates = dyn;

		VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		pi.stageCount = 2;
		pi.pStages = stages;
		pi.pVertexInputState = &vi;
		pi.pInputAssemblyState = &ia;
		pi.pViewportState = &vp;
		pi.pRasterizationState = &rs;
		pi.pMultisampleState = &ms;
		pi.pDepthStencilState = &ds;
		pi.pColorBlendState = &cb;
		pi.pDynamicState = &dy;
		pi.layout = m_layout;
		pi.renderPass = d.pass;
		const VkResult r = m_vk->vkCreateGraphicsPipelines(m_dev, VK_NULL_HANDLE, 1, &pi, nullptr, &out);
		m_vk->vkDestroyShaderModule(m_dev, vs, nullptr);
		m_vk->vkDestroyShaderModule(m_dev, fs, nullptr);
		if (r != VK_SUCCESS)
			return Fail("vkCreateGraphicsPipelines", r);
		return true;
	};

	const VkVertexInputBindingDescription vec2_binding = {0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
	const VkVertexInputAttributeDescription vec2_attr = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};

	const VkVertexInputBindingDescription box_binding = {0, sizeof(BoxVertex), VK_VERTEX_INPUT_RATE_VERTEX};
	const VkVertexInputAttributeDescription box_attrs[4] = {
		{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
		{1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
		{2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
		{3, 0, VK_FORMAT_R32_SFLOAT, 32},
	};
	const VkVertexInputBindingDescription ui_binding = {0, sizeof(UiVertex), VK_VERTEX_INPUT_RATE_VERTEX};
	const VkVertexInputAttributeDescription ui_attrs[4] = {
		{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
		{1, 0, VK_FORMAT_R32G32_SFLOAT, 8},
		{2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16},
		{3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20},
	};

	const PipelineDesc bg = {fe_spv_bg_vert, sizeof(fe_spv_bg_vert), fe_spv_bg_frag, sizeof(fe_spv_bg_frag),
		&vec2_binding, 1, &vec2_attr, 1, PipelineDesc::None, false, false, m_scene_pass};
	const PipelineDesc box = {fe_spv_box_vert, sizeof(fe_spv_box_vert), fe_spv_box_frag, sizeof(fe_spv_box_frag),
		&box_binding, 1, box_attrs, 4, PipelineDesc::None, true, true, m_scene_pass};
	const PipelineDesc box_reflect = {fe_spv_box_vert, sizeof(fe_spv_box_vert), fe_spv_box_frag,
		sizeof(fe_spv_box_frag), &box_binding, 1, box_attrs, 4, PipelineDesc::Alpha, true, true, m_scene_pass};
	const PipelineDesc halo = {fe_spv_halo_vert, sizeof(fe_spv_halo_vert), fe_spv_halo_frag, sizeof(fe_spv_halo_frag),
		&vec2_binding, 1, &vec2_attr, 1, PipelineDesc::Additive, true, false, m_scene_pass};
	const PipelineDesc post = {fe_spv_bg_vert, sizeof(fe_spv_bg_vert), fe_spv_post_frag, sizeof(fe_spv_post_frag),
		&vec2_binding, 1, &vec2_attr, 1, PipelineDesc::None, false, false, m_final_pass};
	const PipelineDesc ui = {fe_spv_ui_vert, sizeof(fe_spv_ui_vert), fe_spv_ui_frag, sizeof(fe_spv_ui_frag),
		&ui_binding, 1, ui_attrs, 4, PipelineDesc::Alpha, false, false, m_final_pass};
	return make(bg, m_bg) && make(box, m_box) && make(box_reflect, m_box_reflect) && make(halo, m_halo) &&
	       make(post, m_post) && make(ui, m_ui);
}

bool Renderer::Init(Vk* vk, VkPhysicalDevice physical_device, VkDevice device, uint32_t queue_family, VkQueue queue,
	uint32_t width, uint32_t height, VkFormat out_format, const std::vector<VkImage>& out_images,
	VkImageLayout out_final_layout)
{
	m_vk = vk;
	m_pd = physical_device;
	m_dev = device;
	m_queue_family = queue_family;
	m_queue = queue;
	m_width = width;
	m_height = height;
	m_out_format = out_format;
	m_out_images = out_images;
	m_out_final_layout = out_final_layout;
	m_out_initialized.assign(out_images.size(), false);
	m_vk->vkGetPhysicalDeviceMemoryProperties(m_pd, &m_mem);
	VkResult r;

	// Render passes.
	{
		VkAttachmentDescription att[2] = {};
		att[0].format = kSceneFormat;
		att[0].samples = VK_SAMPLE_COUNT_1_BIT;
		att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		att[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		att[1].format = kDepthFormat;
		att[1].samples = VK_SAMPLE_COUNT_1_BIT;
		att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		const VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		const VkAttachmentReference depth_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
		VkSubpassDescription sub = {};
		sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		sub.colorAttachmentCount = 1;
		sub.pColorAttachments = &color_ref;
		sub.pDepthStencilAttachment = &depth_ref;
		VkSubpassDependency deps[2] = {};
		deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		deps[0].dstSubpass = 0;
		deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
		                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		deps[1].srcSubpass = 0;
		deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		rp.attachmentCount = 2;
		rp.pAttachments = att;
		rp.subpassCount = 1;
		rp.pSubpasses = &sub;
		rp.dependencyCount = 2;
		rp.pDependencies = deps;
		r = m_vk->vkCreateRenderPass(m_dev, &rp, nullptr, &m_scene_pass);
		if (r != VK_SUCCESS)
			return Fail("vkCreateRenderPass (scene)", r);
	}
	{
		VkAttachmentDescription att = {};
		att.format = m_out_format;
		att.samples = VK_SAMPLE_COUNT_1_BIT;
		att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		att.finalLayout = m_out_final_layout;
		const VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription sub = {};
		sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		sub.colorAttachmentCount = 1;
		sub.pColorAttachments = &color_ref;
		VkSubpassDependency deps[2] = {};
		deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		deps[0].dstSubpass = 0;
		deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		deps[0].srcAccessMask = 0;
		deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		deps[1].srcSubpass = 0;
		deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		rp.attachmentCount = 1;
		rp.pAttachments = &att;
		rp.subpassCount = 1;
		rp.pSubpasses = &sub;
		rp.dependencyCount = 2;
		rp.pDependencies = deps;
		r = m_vk->vkCreateRenderPass(m_dev, &rp, nullptr, &m_final_pass);
		if (r != VK_SUCCESS)
			return Fail("vkCreateRenderPass (final)", r);
	}

	// Scene targets and framebuffers.
	if (!CreateImage(m_width, m_height, kSceneFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT, m_scene) ||
		!CreateImage(m_width, m_height, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			VK_IMAGE_ASPECT_DEPTH_BIT, m_depth))
		return false;
	{
		const VkImageView views[2] = {m_scene.view, m_depth.view};
		VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fi.renderPass = m_scene_pass;
		fi.attachmentCount = 2;
		fi.pAttachments = views;
		fi.width = m_width;
		fi.height = m_height;
		fi.layers = 1;
		r = m_vk->vkCreateFramebuffer(m_dev, &fi, nullptr, &m_scene_fb);
		if (r != VK_SUCCESS)
			return Fail("vkCreateFramebuffer (scene)", r);
	}
	for (VkImage img : m_out_images)
	{
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = img;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = m_out_format;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView view = VK_NULL_HANDLE;
		r = m_vk->vkCreateImageView(m_dev, &vi, nullptr, &view);
		if (r != VK_SUCCESS)
			return Fail("vkCreateImageView (display)", r);
		m_out_views.push_back(view);
		VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fi.renderPass = m_final_pass;
		fi.attachmentCount = 1;
		fi.pAttachments = &view;
		fi.width = m_width;
		fi.height = m_height;
		fi.layers = 1;
		VkFramebuffer fb = VK_NULL_HANDLE;
		r = m_vk->vkCreateFramebuffer(m_dev, &fi, nullptr, &fb);
		if (r != VK_SUCCESS)
			return Fail("vkCreateFramebuffer (display)", r);
		m_out_fbs.push_back(fb);
	}

	// Descriptors: set 0 = the frame's uniform buffer, set 1 = up to three textures.
	{
		VkDescriptorSetLayoutBinding b0 = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		li.bindingCount = 1;
		li.pBindings = &b0;
		r = m_vk->vkCreateDescriptorSetLayout(m_dev, &li, nullptr, &m_set0_layout);
		if (r != VK_SUCCESS)
			return Fail("vkCreateDescriptorSetLayout (0)", r);
		VkDescriptorSetLayoutBinding b1[3];
		for (uint32_t i = 0; i < 3; i++)
			b1[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		li.bindingCount = 3;
		li.pBindings = b1;
		r = m_vk->vkCreateDescriptorSetLayout(m_dev, &li, nullptr, &m_set1_layout);
		if (r != VK_SUCCESS)
			return Fail("vkCreateDescriptorSetLayout (1)", r);
		const VkDescriptorSetLayout sets[2] = {m_set0_layout, m_set1_layout};
		const VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
		VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		pl.setLayoutCount = 2;
		pl.pSetLayouts = sets;
		pl.pushConstantRangeCount = 1;
		pl.pPushConstantRanges = &push;
		r = m_vk->vkCreatePipelineLayout(m_dev, &pl, nullptr, &m_layout);
		if (r != VK_SUCCESS)
			return Fail("vkCreatePipelineLayout", r);
		const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8},
			{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * 1024}};
		VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		pi.maxSets = 1024 + 8;
		pi.poolSizeCount = 2;
		pi.pPoolSizes = sizes;
		r = m_vk->vkCreateDescriptorPool(m_dev, &pi, nullptr, &m_pool);
		if (r != VK_SUCCESS)
			return Fail("vkCreateDescriptorPool", r);
		VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		si.magFilter = VK_FILTER_LINEAR;
		si.minFilter = VK_FILTER_LINEAR;
		si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		si.maxLod = 0.0f;
		r = m_vk->vkCreateSampler(m_dev, &si, nullptr, &m_sampler);
		if (r != VK_SUCCESS)
			return Fail("vkCreateSampler", r);
	}

	if (!CreatePipelines() || !CreateBoxMesh())
		return false;

	// Command buffers, fences and the per-frame buffers.
	{
		VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		ci.queueFamilyIndex = m_queue_family;
		r = m_vk->vkCreateCommandPool(m_dev, &ci, nullptr, &m_cmd_pool);
		if (r != VK_SUCCESS)
			return Fail("vkCreateCommandPool", r);
		for (Slot& s : m_slots)
		{
			VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			ai.commandPool = m_cmd_pool;
			ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			ai.commandBufferCount = 1;
			r = m_vk->vkAllocateCommandBuffers(m_dev, &ai, &s.cmd);
			if (r != VK_SUCCESS)
				return Fail("vkAllocateCommandBuffers", r);
			VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			r = m_vk->vkCreateFence(m_dev, &fi, nullptr, &s.fence);
			if (r != VK_SUCCESS)
				return Fail("vkCreateFence", r);
			if (!CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, s.ubo) ||
				!CreateBuffer(256 * 1024, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, s.ui))
				return false;
			VkDescriptorSetAllocateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			di.descriptorPool = m_pool;
			di.descriptorSetCount = 1;
			di.pSetLayouts = &m_set0_layout;
			r = m_vk->vkAllocateDescriptorSets(m_dev, &di, &s.frame_set);
			if (r != VK_SUCCESS)
				return Fail("vkAllocateDescriptorSets (frame)", r);
			VkDescriptorBufferInfo bi = {s.ubo.buffer, 0, sizeof(FrameUbo)};
			VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			w.dstSet = s.frame_set;
			w.dstBinding = 0;
			w.descriptorCount = 1;
			w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			w.pBufferInfo = &bi;
			m_vk->vkUpdateDescriptorSets(m_dev, 1, &w, 0, nullptr);
		}
	}

	// The shared vertices: the full-screen triangle and the halo's corners.
	{
		const float tri[6] = {0, 0, 2, 0, 0, 2};
		const float quad[12] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
		if (!CreateBuffer(sizeof(tri) + sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_quad_vb))
			return false;
		std::memcpy(m_quad_vb.mapped, tri, sizeof(tri));
		std::memcpy(static_cast<uint8_t*>(m_quad_vb.mapped) + sizeof(tri), quad, sizeof(quad));
	}

	// A white texel for unused bindings, and the post pass's set.
	const uint32_t white = 0xffffffffu;
	m_white = CreateTexture(1, 1, VK_FORMAT_R8G8B8A8_UNORM, &white);
	if (!m_white)
		return false;
	m_post_set = AllocTextureSet(&m_scene, m_white, m_white);
	return m_post_set != VK_NULL_HANDLE;
}

Texture* Renderer::CreateTexture(uint32_t w, uint32_t h, VkFormat format, const void* pixels)
{
	const uint32_t bpp = format == VK_FORMAT_R8_UNORM ? 1 : 4;
	Texture* t = new Texture();
	if (!CreateImage(w, h, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT, *t))
	{
		DestroyImage(*t);
		delete t;
		return nullptr;
	}
	Upload up = {t, {}};
	const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * bpp;
	if (!CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, up.staging))
	{
		DestroyImage(*t);
		delete t;
		return nullptr;
	}
	std::memcpy(up.staging.mapped, pixels, static_cast<size_t>(bytes));
	m_pending.push_back(up);
	return t;
}

void Renderer::DestroyTexture(Texture* t)
{
	if (!t)
		return;
	// Not yet uploaded: drop the upload.
	for (size_t i = 0; i < m_pending.size(); i++)
		if (m_pending[i].texture == t)
		{
			DestroyBuffer(m_pending[i].staging);
			m_pending.erase(m_pending.begin() + static_cast<long>(i));
			break;
		}
	RetireSlot().dead_textures.push_back(t);
}

// A frame submitted before a resource was retired may still use it; the last submitted frame is
// the newest of them, so the resource waits in its slot until that slot's fence is next waited for.
Renderer::Slot& Renderer::RetireSlot()
{
	return m_slots[(m_slot + kSlots - 1) % kSlots];
}

VkDescriptorSet Renderer::AllocTextureSet(Texture* t0, Texture* t1, Texture* t2)
{
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkDescriptorSetAllocateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	di.descriptorPool = m_pool;
	di.descriptorSetCount = 1;
	di.pSetLayouts = &m_set1_layout;
	const VkResult r = m_vk->vkAllocateDescriptorSets(m_dev, &di, &set);
	if (r != VK_SUCCESS)
	{
		Fail("vkAllocateDescriptorSets (textures)", r);
		return VK_NULL_HANDLE;
	}
	Texture* ts[3] = {t0 ? t0 : m_white, t1 ? t1 : m_white, t2 ? t2 : m_white};
	VkDescriptorImageInfo ii[3];
	VkWriteDescriptorSet w[3];
	for (uint32_t i = 0; i < 3; i++)
	{
		ii[i] = {m_sampler, ts[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w[i].dstSet = set;
		w[i].dstBinding = i;
		w[i].descriptorCount = 1;
		w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[i].pImageInfo = &ii[i];
	}
	m_vk->vkUpdateDescriptorSets(m_dev, 3, w, 0, nullptr);
	return set;
}

void Renderer::FreeTextureSet(VkDescriptorSet set)
{
	if (set)
		RetireSlot().dead_sets.push_back(set);
}

void Renderer::SetAtlas(Texture* atlas)
{
	m_atlas = atlas;
	if (m_atlas_set)
		FreeTextureSet(m_atlas_set);
	m_atlas_set = atlas ? AllocTextureSet(atlas, m_white, m_white) : VK_NULL_HANDLE;
}

void Renderer::Reclaim(Slot& s)
{
	for (Buffer& b : s.dead_buffers)
		DestroyBuffer(b);
	s.dead_buffers.clear();
	for (Texture* t : s.dead_textures)
	{
		DestroyImage(*t);
		delete t;
	}
	s.dead_textures.clear();
	if (!s.dead_sets.empty())
		m_vk->vkFreeDescriptorSets(m_dev, m_pool, static_cast<uint32_t>(s.dead_sets.size()), s.dead_sets.data());
	s.dead_sets.clear();
}

bool Renderer::WaitForSlot()
{
	Slot& s = m_slots[m_slot];
	if (s.submitted)
	{
		VkResult r = m_vk->vkWaitForFences(m_dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
		if (r != VK_SUCCESS)
			return Fail("vkWaitForFences", r);
		r = m_vk->vkResetFences(m_dev, 1, &s.fence);
		if (r != VK_SUCCESS)
			return Fail("vkResetFences", r);
		s.submitted = false;
	}
	// Everything retired since this slot's last frame was submitted is free now.
	Reclaim(s);
	return true;
}

bool Renderer::Render(const FrameDesc& f, uint32_t out_index, VkSemaphore wait, VkSemaphore signal)
{
	Slot& s = m_slots[m_slot];
	VkResult r;
	if (!WaitForSlot())
		return false;

	// The frame's uniforms.
	FrameUbo ubo = {};
	std::memcpy(ubo.view_proj, f.view_proj.m, sizeof(ubo.view_proj));
	ubo.cam_pos[0] = f.cam_pos.x;
	ubo.cam_pos[1] = f.cam_pos.y;
	ubo.cam_pos[2] = f.cam_pos.z;
	ubo.cam_pos[3] = f.time;
	std::memcpy(ubo.glow, f.glow, sizeof(ubo.glow));
	ubo.floor[0] = f.floor_y;
	ubo.floor[1] = f.reflect_strength;
	ubo.floor[2] = f.reflect_falloff;
	ubo.screen[0] = static_cast<float>(m_width);
	ubo.screen[1] = static_cast<float>(m_height);
	ubo.screen[2] = 1.0f / m_width;
	ubo.screen[3] = 1.0f / m_height;
	std::memcpy(s.ubo.mapped, &ubo, sizeof(ubo));

	// The UI's vertices.
	const VkDeviceSize ui_bytes = f.ui.size() * sizeof(UiVertex);
	if (ui_bytes > s.ui.size)
	{
		DestroyBuffer(s.ui);
		if (!CreateBuffer(ui_bytes * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, s.ui))
			return false;
	}
	if (ui_bytes)
		std::memcpy(s.ui.mapped, f.ui.data(), static_cast<size_t>(ui_bytes));

	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	r = m_vk->vkResetCommandBuffer(s.cmd, 0);
	if (r != VK_SUCCESS)
		return Fail("vkResetCommandBuffer", r);
	r = m_vk->vkBeginCommandBuffer(s.cmd, &bi);
	if (r != VK_SUCCESS)
		return Fail("vkBeginCommandBuffer", r);
	VkCommandBuffer cmd = s.cmd;

	// Texture uploads.
	for (Upload& up : m_pending)
	{
		Barrier(cmd, up.texture->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy c = {};
		c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		c.imageExtent = {up.texture->width, up.texture->height, 1};
		m_vk->vkCmdCopyBufferToImage(cmd, up.staging.buffer, up.texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		Barrier(cmd, up.texture->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		s.dead_buffers.push_back(up.staging);
	}
	m_pending.clear();

	const VkViewport viewport = {0, 0, static_cast<float>(m_width), static_cast<float>(m_height), 0, 1};
	const VkRect2D scissor = {{0, 0}, {m_width, m_height}};
	const VkDeviceSize zero = 0;
	const VkDeviceSize halo_offset = 6 * sizeof(float);

	// The scene.
	{
		VkClearValue clear[2] = {};
		clear[0].color = {{0, 0, 0, 1}};
		clear[1].depthStencil = {1.0f, 0};
		VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rb.renderPass = m_scene_pass;
		rb.framebuffer = m_scene_fb;
		rb.renderArea = scissor;
		rb.clearValueCount = 2;
		rb.pClearValues = clear;
		m_vk->vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
		m_vk->vkCmdSetViewport(cmd, 0, 1, &viewport);
		m_vk->vkCmdSetScissor(cmd, 0, 1, &scissor);
		m_vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &s.frame_set, 0, nullptr);

		m_vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bg);
		m_vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1, &m_post_set, 0, nullptr);
		m_vk->vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			sizeof(BgParams), &f.bg);
		m_vk->vkCmdBindVertexBuffers(cmd, 0, 1, &m_quad_vb.buffer, &zero);
		m_vk->vkCmdDraw(cmd, 3, 1, 0, 0);

		auto draw_boxes = [&](const std::vector<BoxDraw>& list, bool reflection) {
			if (list.empty())
				return;
			m_vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, reflection ? m_box_reflect : m_box);
			m_vk->vkCmdBindVertexBuffers(cmd, 0, 1, &m_box_vb.buffer, &zero);
			m_vk->vkCmdBindIndexBuffer(cmd, m_box_ib.buffer, 0, VK_INDEX_TYPE_UINT16);
			for (const BoxDraw& d : list)
			{
				if (!d.set)
					continue;
				BoxPush p = {};
				std::memcpy(p.model, d.model.m, sizeof(p.model));
				p.p0[0] = d.brightness;
				p.p0[1] = d.cover_mix;
				p.p0[2] = reflection ? 1.0f : 0.0f;
				p.p0[3] = d.selected;
				p.p1[0] = d.sheen_pos;
				p.p1[1] = d.sheen_strength;
				p.p1[2] = d.alpha;
				m_vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1, &d.set, 0, nullptr);
				m_vk->vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
					sizeof(p), &p);
				m_vk->vkCmdDrawIndexed(cmd, m_box_index_count, 1, 0, 0, 0);
			}
		};
		draw_boxes(f.reflections, true);
		draw_boxes(f.boxes, false);

		if (f.halo.enabled)
		{
			HaloPush p = {};
			std::memcpy(p.model, f.halo.model.m, sizeof(p.model));
			p.size[0] = f.halo.half_w;
			p.size[1] = f.halo.half_h;
			p.size[2] = f.halo.z;
			p.size[3] = f.halo.margin;
			std::memcpy(p.color, f.halo.color, sizeof(p.color));
			p.shape[0] = f.halo.radius;
			p.shape[1] = f.halo.line;
			p.shape[2] = f.halo.glow_width;
			m_vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_halo);
			m_vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1, &m_post_set, 0, nullptr);
			m_vk->vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
				sizeof(p), &p);
			m_vk->vkCmdBindVertexBuffers(cmd, 0, 1, &m_quad_vb.buffer, &halo_offset);
			m_vk->vkCmdDraw(cmd, 6, 1, 0, 0);
		}
		m_vk->vkCmdEndRenderPass(cmd);
	}

	// The display image: FXAA from the scene, then the UI.
	{
		VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rb.renderPass = m_final_pass;
		rb.framebuffer = m_out_fbs[out_index];
		rb.renderArea = scissor;
		m_vk->vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
		m_vk->vkCmdSetViewport(cmd, 0, 1, &viewport);
		m_vk->vkCmdSetScissor(cmd, 0, 1, &scissor);
		m_vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_post);
		m_vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &s.frame_set, 0, nullptr);
		m_vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1, &m_post_set, 0, nullptr);
		const float post[4] = {1.0f / m_width, 1.0f / m_height, f.fade, 0};
		m_vk->vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			sizeof(post), post);
		m_vk->vkCmdBindVertexBuffers(cmd, 0, 1, &m_quad_vb.buffer, &zero);
		m_vk->vkCmdDraw(cmd, 3, 1, 0, 0);
		if (!f.ui.empty() && m_atlas_set)
		{
			m_vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ui);
			m_vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1, &m_atlas_set, 0, nullptr);
			m_vk->vkCmdBindVertexBuffers(cmd, 0, 1, &s.ui.buffer, &zero);
			// Preserve UI ordering when a badge temporarily replaces the font atlas. (AI-assisted)
			uint32_t first = 0;
			for (const auto& image : f.ui_images)
			{
				if (image.first < first || image.first + image.count > f.ui.size() || !image.set)
					continue;
				m_vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1, &m_atlas_set, 0, nullptr);
				if (image.first > first)
					m_vk->vkCmdDraw(cmd, image.first - first, 1, first, 0);
				m_vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1, &image.set, 0, nullptr);
				m_vk->vkCmdDraw(cmd, image.count, 1, image.first, 0);
				first = image.first + image.count;
			}
			m_vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1, &m_atlas_set, 0, nullptr);
			if (first < f.ui.size())
				m_vk->vkCmdDraw(cmd, static_cast<uint32_t>(f.ui.size()) - first, 1, first, 0);
		}
		m_vk->vkCmdEndRenderPass(cmd);
	}

	r = m_vk->vkEndCommandBuffer(cmd);
	if (r != VK_SUCCESS)
		return Fail("vkEndCommandBuffer", r);

	const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.waitSemaphoreCount = wait ? 1 : 0;
	si.pWaitSemaphores = &wait;
	si.pWaitDstStageMask = &wait_stage;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	si.signalSemaphoreCount = signal ? 1 : 0;
	si.pSignalSemaphores = &signal;
	r = m_vk->vkQueueSubmit(m_queue, 1, &si, s.fence);
	if (r != VK_SUCCESS)
		return Fail("vkQueueSubmit", r);
	s.submitted = true;
	m_slot = (m_slot + 1) % kSlots;
	return true;
}

bool Renderer::ReadPixels(uint32_t out_index, std::vector<uint8_t>& rgba)
{
	WaitIdle();
	Buffer b;
	const VkDeviceSize bytes = static_cast<VkDeviceSize>(m_width) * m_height * 4;
	if (!CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, b))
		return false;
	VkCommandBuffer cmd = m_slots[m_slot].cmd;
	m_vk->vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	m_vk->vkBeginCommandBuffer(cmd, &bi);
	VkBufferImageCopy c = {};
	c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	c.imageExtent = {m_width, m_height, 1};
	m_vk->vkCmdCopyImageToBuffer(cmd, m_out_images[out_index], m_out_final_layout, b.buffer, 1, &c);
	m_vk->vkEndCommandBuffer(cmd);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	VkResult r = m_vk->vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE);
	if (r != VK_SUCCESS)
		return Fail("vkQueueSubmit (readback)", r);
	m_vk->vkQueueWaitIdle(m_queue);
	rgba.resize(static_cast<size_t>(bytes));
	std::memcpy(rgba.data(), b.mapped, static_cast<size_t>(bytes));
	if (m_out_format == VK_FORMAT_B8G8R8A8_UNORM)
		for (size_t i = 0; i < rgba.size(); i += 4)
			std::swap(rgba[i], rgba[i + 2]);
	DestroyBuffer(b);
	return true;
}

void Renderer::WaitIdle()
{
	if (m_dev)
		m_vk->vkDeviceWaitIdle(m_dev);
}

void Renderer::Shutdown()
{
	if (!m_dev)
		return;
	WaitIdle();
	// Uploads that never ran: only their staging buffers go. The textures belong to whoever made
	// them (the shelf destroys its own first; m_white goes below). vk-285-40..46 freed them here as
	// well, so a frontend that stopped before its first frame freed m_white twice.
	for (Upload& up : m_pending)
		DestroyBuffer(up.staging);
	m_pending.clear();
	for (Slot& s : m_slots)
	{
		Reclaim(s);
		DestroyBuffer(s.ubo);
		DestroyBuffer(s.ui);
		if (s.fence)
			m_vk->vkDestroyFence(m_dev, s.fence, nullptr);
		s = Slot();
	}
	if (m_white)
	{
		DestroyImage(*m_white);
		delete m_white;
		m_white = nullptr;
	}
	if (m_cmd_pool)
		m_vk->vkDestroyCommandPool(m_dev, m_cmd_pool, nullptr);
	DestroyBuffer(m_box_vb);
	DestroyBuffer(m_box_ib);
	DestroyBuffer(m_quad_vb);
	for (VkPipeline* p : {&m_bg, &m_box, &m_box_reflect, &m_halo, &m_post, &m_ui})
		if (*p)
		{
			m_vk->vkDestroyPipeline(m_dev, *p, nullptr);
			*p = VK_NULL_HANDLE;
		}
	if (m_sampler)
		m_vk->vkDestroySampler(m_dev, m_sampler, nullptr);
	if (m_pool)
		m_vk->vkDestroyDescriptorPool(m_dev, m_pool, nullptr);
	if (m_layout)
		m_vk->vkDestroyPipelineLayout(m_dev, m_layout, nullptr);
	if (m_set0_layout)
		m_vk->vkDestroyDescriptorSetLayout(m_dev, m_set0_layout, nullptr);
	if (m_set1_layout)
		m_vk->vkDestroyDescriptorSetLayout(m_dev, m_set1_layout, nullptr);
	for (VkFramebuffer fb : m_out_fbs)
		m_vk->vkDestroyFramebuffer(m_dev, fb, nullptr);
	for (VkImageView v : m_out_views)
		m_vk->vkDestroyImageView(m_dev, v, nullptr);
	m_out_fbs.clear();
	m_out_views.clear();
	if (m_scene_fb)
		m_vk->vkDestroyFramebuffer(m_dev, m_scene_fb, nullptr);
	DestroyImage(m_scene);
	DestroyImage(m_depth);
	if (m_scene_pass)
		m_vk->vkDestroyRenderPass(m_dev, m_scene_pass, nullptr);
	if (m_final_pass)
		m_vk->vkDestroyRenderPass(m_dev, m_final_pass, nullptr);
	m_dev = VK_NULL_HANDLE;
}
} // namespace fe
