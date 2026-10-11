// RPCS3-PS5 (m73): frame generation, the host side of FSR3's optical flow and frame interpolation (ps5_framegen.h).
//
// The order of the passes, their sizes, the resources each binds and the constants are those of the FidelityFX SDK 2.3.0's
// ffx_opticalflow.cpp (dispatch()) and ffx_frameinterpolation.cpp (ffxFrameInterpolationDispatch()), MIT, Advanced Micro Devices.
//
// PS5SX2: see ps5_framegen.h for where this copy comes from.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ps5_framegen.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>

namespace ps5::framegen
{
	namespace
	{
		std::atomic<bool> g_wanted{false};
		std::atomic<std::uint32_t> g_limit{0};
		std::atomic<bool> g_debug{false};
		std::atomic<std::uint32_t> g_multiplier{2};
	}

	void set_multiplier(std::uint32_t times)
	{
		g_multiplier.store(times < 2 ? 2 : times > 4 ? 4 : times, std::memory_order_relaxed);
	}

	std::uint32_t multiplier()
	{
		return g_multiplier.load(std::memory_order_relaxed);
	}

	void set_debug(bool on)
	{
		g_debug.store(on, std::memory_order_relaxed);
	}

	bool debug()
	{
		return g_debug.load(std::memory_order_relaxed);
	}

	void set_wanted(bool on)
	{
		g_wanted.store(on, std::memory_order_relaxed);
	}

	bool wanted()
	{
		return g_wanted.load(std::memory_order_relaxed);
	}

	void set_limit(std::uint32_t fps)
	{
		g_limit.store(fps, std::memory_order_relaxed);
	}

	std::uint32_t limit()
	{
		return g_limit.load(std::memory_order_relaxed);
	}

	namespace
	{
		struct fg_binding
		{
			std::uint32_t binding;
			VkDescriptorType type;
			const char* name;
		};

		struct fg_shader
		{
			const char* name;
			const std::uint32_t* code;
			std::size_t words;
			std::uint32_t local[3];
			const fg_binding* bindings;
			std::uint32_t binding_count;
		};

		using u32 = std::uint32_t;

#include "framegen/fg_shaders.inc"

		enum shader_index : u32
		{
			sh_of_prepare_luma,
			sh_of_luma_pyramid,
			sh_of_scd_histogram,
			sh_of_scd_divergence,
			sh_of_search,
			sh_of_filter,
			sh_of_scale,
			sh_fi_setup,
			sh_fi_vector_field,
			sh_fi_interpolate,
			sh_count
		};

		static_assert(sizeof(k_fg_shaders) / sizeof(k_fg_shaders[0]) == sh_count);

		constexpr u32 k_levels = 7;          // the optical flow's pyramid (OpticalFlowMaxPyramidLevels)
		constexpr u32 k_block = 8;           // its block size
		constexpr u32 k_slots = 8;           // constant ring slots (frames in flight)
		constexpr u32 k_histogram_width = 256 * 9; // GetSCDHistogramTextureWidth()

		// The uniform blocks, as the SPIR-V lays them out (tools/spvubo.py).
		struct cb_of
		{
			std::int32_t input_luma_resolution[2];
			u32 pyramid_level;
			u32 pyramid_level_count;
			u32 frame_index;
			u32 backbuffer_transfer_function;
			float min_max_luminance[2];
		};
		static_assert(sizeof(cb_of) == 32 && offsetof(cb_of, frame_index) == 16 && offsetof(cb_of, min_max_luminance) == 24);

		struct cb_of_spd
		{
			u32 mips;
			u32 num_work_groups;
			u32 work_group_offset[2];
			u32 num_work_groups_pyramid;
			u32 pad[3];
		};
		static_assert(sizeof(cb_of_spd) == 32 && offsetof(cb_of_spd, num_work_groups_pyramid) == 16);

		struct cb_fi
		{
			std::int32_t render_size[2];
			std::int32_t display_size[2];
			float display_size_rcp[2];
			float camera_near;
			float camera_far;
			std::int32_t upscaler_target_size[2];
			std::int32_t mode;
			std::int32_t reset;
			float device_to_view_depth[4];
			float delta_time;
			std::int32_t hudless_attached;
			std::int32_t distortion_field_size[2];
			float optical_flow_scale[2];
			std::int32_t optical_flow_block_size;
			u32 dispatch_flags;
			std::int32_t max_render_size[2];
			std::int32_t optical_flow_half_res_mode;
			std::int32_t num_instances;
			std::int32_t interpolation_rect_base[2];
			std::int32_t interpolation_rect_size[2];
			float debug_bar_color[3];
			u32 back_buffer_transfer_function;
			float min_max_luminance[2];
			float tan_half_fov;
			float interpolation_t; // m74: the moment generated (0 or 0.5: halfway)
			float jitter[2];
			float motion_vector_scale[2];
		};
		static_assert(sizeof(cb_fi) == 176 && offsetof(cb_fi, device_to_view_depth) == 48 && offsetof(cb_fi, optical_flow_scale) == 80 &&
			offsetof(cb_fi, interpolation_rect_base) == 112 && offsetof(cb_fi, debug_bar_color) == 128 && offsetof(cb_fi, jitter) == 160);

		// The blocks of a constant slot: cbOF for each pyramid level, cbOF_SPD, cbFI (m74: one for each frame generated, up to 3).
		constexpr u32 k_block_of = 0;
		constexpr u32 k_block_of_spd = k_levels;
		constexpr u32 k_block_fi = k_levels + 1;
		constexpr u32 k_max_generated = 3;
		constexpr u32 k_blocks = k_levels + 1 + k_max_generated;

		void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage, VkAccessFlags src_access, VkAccessFlags dst_access)
		{
			VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			barrier.srcAccessMask = src_access;
			barrier.dstAccessMask = dst_access;
			vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
		}

		const char* vk_error(VkResult r)
		{
			switch (r)
			{
			case VK_ERROR_OUT_OF_HOST_MEMORY: return "out of host memory";
			case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "out of device memory";
			case VK_ERROR_INITIALIZATION_FAILED: return "initialization failed";
			case VK_ERROR_FEATURE_NOT_PRESENT: return "feature not present";
			case VK_ERROR_FORMAT_NOT_SUPPORTED: return "format not supported";
			default: return "error";
			}
		}
	}

	interpolator::~interpolator()
	{
		destroy();
	}

	u32 interpolator::memory_type(u32 bits, VkMemoryPropertyFlags props) const
	{
		for (u32 i = 0; i < m_memory_props.memoryTypeCount; i++)
		{
			if ((bits & (1u << i)) && (m_memory_props.memoryTypes[i].propertyFlags & props) == props)
			{
				return i;
			}
		}

		return UINT32_MAX;
	}

	bool interpolator::make_image(image_t& img, VkFormat format, u32 w, u32 h, VkImageUsageFlags usage, std::string& error)
	{
		img.format = format;
		img.w = std::max<u32>(w, 1);
		img.h = std::max<u32>(h, 1);

		VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = format;
		info.extent = {img.w, img.h, 1};
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = usage;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		if (VkResult r = vkCreateImage(m_device, &info, nullptr, &img.image); r != VK_SUCCESS)
		{
			error = std::string("vkCreateImage: ") + vk_error(r);
			return false;
		}

		VkMemoryRequirements req{};
		vkGetImageMemoryRequirements(m_device, img.image, &req);

		VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

		if (alloc.memoryTypeIndex == UINT32_MAX)
		{
			error = "no device-local memory type for an image";
			return false;
		}

		if (VkResult r = vkAllocateMemory(m_device, &alloc, nullptr, &img.memory); r != VK_SUCCESS)
		{
			error = std::string("vkAllocateMemory (image): ") + vk_error(r);
			return false;
		}

		vkBindImageMemory(m_device, img.image, img.memory, 0);

		VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		view.image = img.image;
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = format;
		view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

		if (VkResult r = vkCreateImageView(m_device, &view, nullptr, &img.view); r != VK_SUCCESS)
		{
			error = std::string("vkCreateImageView: ") + vk_error(r);
			return false;
		}

		return true;
	}

	void interpolator::free_image(image_t& img)
	{
		if (img.view) vkDestroyImageView(m_device, img.view, nullptr);
		if (img.image) vkDestroyImage(m_device, img.image, nullptr);
		if (img.memory) vkFreeMemory(m_device, img.memory, nullptr);
		img = {};
	}

	bool interpolator::create(VkPhysicalDevice physical, VkDevice device, u32 width, u32 height, std::string& error, VkFormat frame_format)
	{
		destroy();

		if (!width || !height)
		{
			error = "empty frame size";
			return false;
		}

		m_physical = physical;
		m_device = device;
		m_width = width;
		m_height = height;
		vkGetPhysicalDeviceMemoryProperties(physical, &m_memory_props);

		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(physical, &props);
		m_ubo_block = static_cast<u32>(std::max<VkDeviceSize>(256, props.limits.minUniformBufferOffsetAlignment));
		m_ubo_slot = m_ubo_block * k_blocks;

		const auto fail = [&]() { destroy(); return false; };

		// The frames and the interpolated one.
		const VkImageUsageFlags color_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		const VkImageUsageFlags work_usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

		for (image_t& c : m_color)
		{
			if (!make_image(c, frame_format, width, height, color_usage, error)) return fail();
		}

		if (!make_image(m_output, k_format, width, height, work_usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, error)) return fail();
		if (!make_image(m_static, VK_FORMAT_R8_UINT, width, height, work_usage, error)) return fail(); // m73b: the still history
		if (!make_image(m_quad_motion, VK_FORMAT_R16G16_SFLOAT, (width + 1) / 2, (height + 1) / 2, work_usage, error)) return fail(); // m74

		// The optical flow's luma pyramids (ffx_opticalflow.cpp getInternalResourceDescriptions: the full size, then >> level) and
		// its vectors (a block of 8x8 pixels, then halved, rounded up).
		u32 fw = (width + k_block - 1) / k_block;
		u32 fh = (height + k_block - 1) / k_block;

		for (u32 level = 0; level < k_levels; level++)
		{
			for (u32 set = 0; set < 2; set++)
			{
				if (!make_image(m_luma[set][level], VK_FORMAT_R8_UINT, width >> level, height >> level, work_usage, error)) return fail();
				if (!make_image(m_flow[set][level], VK_FORMAT_R16G16_SINT, fw, fh, work_usage, error)) return fail();
			}

			fw = (fw + 1) / 2;
			fh = (fh + 1) / 2;
		}

		const u32 flow_w = (width + k_block - 1) / k_block;
		const u32 flow_h = (height + k_block - 1) / k_block;

		if (!make_image(m_flow_out, VK_FORMAT_R16G16_SINT, flow_w, flow_h, work_usage, error)) return fail();
		if (!make_image(m_scd_histogram, VK_FORMAT_R32_UINT, k_histogram_width, 1, work_usage, error)) return fail();
		if (!make_image(m_scd_previous_histogram, VK_FORMAT_R32_SFLOAT, k_histogram_width, 1, work_usage, error)) return fail();
		if (!make_image(m_scd_temp, VK_FORMAT_R32_UINT, 3, 1, work_usage, error)) return fail();
		if (!make_image(m_scd_output, VK_FORMAT_R32_UINT, 3, 1, work_usage, error)) return fail();

		// The vector field, at the optical flow's size (FSR3 makes it at the render size; every access is within the flow's size).
		if (!make_image(m_field_x, VK_FORMAT_R32_UINT, flow_w, flow_h, work_usage, error)) return fail();
		if (!make_image(m_field_y, VK_FORMAT_R32_UINT, flow_w, flow_h, work_usage, error)) return fail();

		// The counters (a storage buffer, zeroed at the first frame) and the constant ring (host visible).
		{
			VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			info.size = 256;
			info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			if (vkCreateBuffer(m_device, &info, nullptr, &m_counters) != VK_SUCCESS) { error = "vkCreateBuffer (counters)"; return fail(); }

			VkMemoryRequirements req{};
			vkGetBufferMemoryRequirements(m_device, m_counters, &req);
			VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			alloc.allocationSize = req.size;
			alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

			if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &alloc, nullptr, &m_counters_memory) != VK_SUCCESS)
			{
				error = "vkAllocateMemory (counters)";
				return fail();
			}

			vkBindBufferMemory(m_device, m_counters, m_counters_memory, 0);
		}

		{
			VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			info.size = VkDeviceSize{m_ubo_slot} * k_slots;
			info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
			info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			if (vkCreateBuffer(m_device, &info, nullptr, &m_ubo) != VK_SUCCESS) { error = "vkCreateBuffer (constants)"; return fail(); }

			VkMemoryRequirements req{};
			vkGetBufferMemoryRequirements(m_device, m_ubo, &req);
			VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			alloc.allocationSize = req.size;
			alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

			if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &alloc, nullptr, &m_ubo_memory) != VK_SUCCESS)
			{
				error = "vkAllocateMemory (constants)";
				return fail();
			}

			vkBindBufferMemory(m_device, m_ubo, m_ubo_memory, 0);

			void* map = nullptr;

			if (vkMapMemory(m_device, m_ubo_memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)
			{
				error = "vkMapMemory (constants)";
				return fail();
			}

			m_ubo_map = static_cast<std::uint8_t*>(map);
		}

		{
			VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
			info.magFilter = VK_FILTER_LINEAR;
			info.minFilter = VK_FILTER_LINEAR;
			info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			info.maxLod = 0.f;

			if (vkCreateSampler(m_device, &info, nullptr, &m_sampler) != VK_SUCCESS) { error = "vkCreateSampler"; return fail(); }
		}

		// m73b: the GPU timer, if the device has timestamps on its graphics and compute queues (the profile's layer,
		// ps5_gpuprof_vk.cpp, writes them the same way on the console). Not having one is not an error.
		if (props.limits.timestampComputeAndGraphics && props.limits.timestampPeriod > 0.f)
		{
			m_write_timestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(vkGetDeviceProcAddr(m_device, "vkCmdWriteTimestamp"));
			VkQueryPoolCreateInfo qinfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
			qinfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
			qinfo.queryCount = 3 * k_slots; // m74: three a slot: before, between the optical flow and the interpolation, after

			if (!m_write_timestamp || vkCreateQueryPool(m_device, &qinfo, nullptr, &m_queries) != VK_SUCCESS)
			{
				m_write_timestamp = nullptr;
				m_queries = VK_NULL_HANDLE;
			}

			m_timestamp_ns = props.limits.timestampPeriod;
			m_timestamp_mask = ~0ull; // the queue family's valid bits are not known here: differences wrap at 64 bits
		}

		if (!make_pipelines(error) || !make_dispatches(error))
		{
			return fail();
		}

		m_frames = 0;
		m_interpolated = 0;
		m_flow_frame_index = 0;
		m_cleared = false;
		m_prepared = false;
		m_timed_slots = 0;
		m_gpu_ms_sum = m_gpu_ms_max = m_gpu_flow_ms_sum = 0.0;
		m_gpu_count = 0;
		return true;
	}

	bool interpolator::make_pipelines(std::string& error)
	{
		m_pipelines.resize(sh_count);

		for (u32 i = 0; i < sh_count; i++)
		{
			const fg_shader& sh = k_fg_shaders[i];
			pipeline_t& p = m_pipelines[i];
			p.shader = i;

			// PS5SX2 (2026-10-08): the bindings numbered 0..n-1, in the order of the shader's own numbers (0, 100, 200, 300, 405 ...,
			// FSR3's HLSL registers), in the SPIR-V's Binding decorations and in the layout and the writes alike. ps5vk's descriptor
			// tables run to a set's highest binding number and its compiler keeps a binding number in a byte, so the sparse numbers
			// made a 409-slot table and its compiler failed (vkCreateComputePipelines, "internal error"). The order is kept, so the
			// dynamic uniform blocks' offsets follow the bindings as before.
			{
				std::vector<u32> order(sh.binding_count);
				for (u32 b = 0; b < sh.binding_count; b++)
					order[b] = b;
				std::sort(order.begin(), order.end(), [&sh](u32 a, u32 b) { return sh.bindings[a].binding < sh.bindings[b].binding; });
				p.slot.assign(sh.binding_count, 0);
				for (u32 k = 0; k < sh.binding_count; k++)
					p.slot[order[k]] = k;
			}

			std::vector<u32> code(sh.code, sh.code + sh.words);

			for (std::size_t at = 5; at < code.size();)
			{
				const u32 op = code[at] & 0xffffu;
				const u32 count = code[at] >> 16;

				if (count == 0 || at + count > code.size())
				{
					error = std::string("framegen: the SPIR-V of ") + sh.name + " is cut short";
					return false;
				}

				if (op == 71 && count >= 4 && code[at + 2] == 33) // OpDecorate <id> Binding <n>
				{
					bool found = false;

					for (u32 b = 0; b < sh.binding_count && !found; b++)
					{
						if (sh.bindings[b].binding == code[at + 3])
						{
							code[at + 3] = p.slot[b];
							found = true;
						}
					}

					if (!found)
					{
						error = std::string("framegen: binding ") + std::to_string(code[at + 3]) + " of " + sh.name + " is not in its table";
						return false;
					}
				}

				at += count;
			}

			VkShaderModuleCreateInfo mod{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			mod.codeSize = code.size() * 4;
			mod.pCode = code.data();

			if (vkCreateShaderModule(m_device, &mod, nullptr, &p.module) != VK_SUCCESS)
			{
				error = std::string("vkCreateShaderModule ") + sh.name;
				return false;
			}

			std::vector<VkDescriptorSetLayoutBinding> bindings(sh.binding_count);

			for (u32 b = 0; b < sh.binding_count; b++)
			{
				bindings[b] = {p.slot[b], sh.bindings[b].type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
			}

			VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			set_info.bindingCount = sh.binding_count;
			set_info.pBindings = bindings.data();

			if (vkCreateDescriptorSetLayout(m_device, &set_info, nullptr, &p.set_layout) != VK_SUCCESS)
			{
				error = std::string("vkCreateDescriptorSetLayout ") + sh.name;
				return false;
			}

			VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			layout_info.setLayoutCount = 1;
			layout_info.pSetLayouts = &p.set_layout;

			if (vkCreatePipelineLayout(m_device, &layout_info, nullptr, &p.layout) != VK_SUCCESS)
			{
				error = std::string("vkCreatePipelineLayout ") + sh.name;
				return false;
			}

			VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, p.module, "CS", nullptr};
			info.layout = p.layout;

			if (VkResult r = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &p.pipeline); r != VK_SUCCESS)
			{
				error = std::string("vkCreateComputePipelines ") + sh.name + ": " + vk_error(r);
				return false;
			}
		}

		return true;
	}

	bool interpolator::make_dispatches(std::string& error)
	{
		const u32 w = m_width;
		const u32 h = m_height;

		// The optical flow's level sizes (ffx_opticalflow.cpp: opticalFlowTextureSizes).
		u32 flow_w[k_levels], flow_h[k_levels];
		flow_w[0] = (w + k_block - 1) / k_block;
		flow_h[0] = (h + k_block - 1) / k_block;

		for (u32 i = 1; i < k_levels; i++)
		{
			flow_w[i] = (flow_w[i - 1] + 1) / 2;
			flow_h[i] = (flow_h[i - 1] + 1) / 2;
		}

		// What each dispatch binds, by the shaders' names, for frame parity p (ffx_opticalflow.cpp's srvBindings and uavBindings
		// for a frame with isOddFrame == p, and ffx_frameinterpolation.cpp's).
		struct res
		{
			const image_t* img = nullptr;
			bool counters = false;
		};

		struct spec
		{
			u32 shader;
			u32 groups[3];
			u32 ubo[2];
			u32 ubo_count;
			bool interpolation;
			// name -> resource, for each parity
			std::vector<std::pair<const char*, res>> binds[2];
		};

		std::vector<spec> specs;
		specs.reserve(64); // add() hands out pointers into it

		const auto add = [&](u32 shader, u32 gx, u32 gy, u32 gz, std::initializer_list<u32> ubos, bool interp)
		{
			spec s{};
			s.shader = shader;
			s.groups[0] = std::max<u32>(gx, 1);
			s.groups[1] = std::max<u32>(gy, 1);
			s.groups[2] = std::max<u32>(gz, 1);
			s.ubo_count = 0;

			for (u32 u : ubos)
			{
				s.ubo[s.ubo_count++] = u;
			}

			s.interpolation = interp;
			specs.push_back(std::move(s));
			return &specs.back();
		};

		const auto bind = [](spec* s, u32 p, const char* name, const image_t& img) { s->binds[p].push_back({name, res{&img, false}}); };

		// Optical flow, the frame's luma and its pyramid, the scene change detection.
		{
			spec* s = add(sh_of_prepare_luma, ((w + 1) / 2 + 15) / 16, ((h + 1) / 2 + 15) / 16, 1, {k_block_of + 0}, false);
			for (u32 p = 0; p < 2; p++) { bind(s, p, "r_input_color", m_color[p]); bind(s, p, "rw_optical_flow_input", m_luma[p][0]); }
		}
		{
			// ffxSpdSetup(rect 0, 0, w, h): one group per 64x64 tile; the shader always makes 6 levels.
			const u32 gx = (w - 1) / 64 + 1;
			const u32 gy = (h - 1) / 64 + 1;
			spec* s = add(sh_of_luma_pyramid, gx, gy, 1, {k_block_of + 0, k_block_of_spd}, false);
			static const char* names[] = {"rw_optical_flow_input", "rw_optical_flow_input_level_1", "rw_optical_flow_input_level_2",
				"rw_optical_flow_input_level_3", "rw_optical_flow_input_level_4", "rw_optical_flow_input_level_5", "rw_optical_flow_input_level_6"};
			for (u32 p = 0; p < 2; p++)
				for (u32 l = 0; l < k_levels; l++)
					bind(s, p, names[l], m_luma[p][l]);
		}
		{
			const u32 strata_width = (w / 4) / 3;
			spec* s = add(sh_of_scd_histogram, (strata_width + 31) / 32, 16, 9, {k_block_of + 0}, false);
			for (u32 p = 0; p < 2; p++) { bind(s, p, "r_optical_flow_input", m_luma[p][0]); bind(s, p, "rw_optical_flow_scd_histogram", m_scd_histogram); }
		}
		{
			spec* s = add(sh_of_scd_divergence, 9, 3, 1, {k_block_of + 0}, false);
			for (u32 p = 0; p < 2; p++)
			{
				bind(s, p, "rw_optical_flow_scd_histogram", m_scd_histogram);
				bind(s, p, "rw_optical_flow_scd_previous_histogram", m_scd_previous_histogram);
				bind(s, p, "rw_optical_flow_scd_temp", m_scd_temp);
				bind(s, p, "rw_optical_flow_scd_output", m_scd_output);
			}
		}

		// The search, the filter and the upscale at each level, coarsest first.
		for (int level = k_levels - 1; level >= 0; level--)
		{
			const u32 l = static_cast<u32>(level);
			const u32 luma_w = std::max<u32>(w >> l, 1);
			const u32 luma_h = std::max<u32>(h >> l, 1);

			spec* search = add(sh_of_search, ((luma_w + 3) / 4 * 16 + 63) / 64, (luma_h + 15) / 16, 1, {k_block_of + l}, false);
			spec* filter = add(sh_of_filter, (flow_w[l] + 15) / 16, (flow_h[l] + 3) / 4, 1, {k_block_of + l}, false);
			spec* scale = level > 0 ? add(sh_of_scale, (flow_w[l - 1] + 3) / 4, (flow_h[l - 1] + 3) / 4, 1, {k_block_of + l}, false) : nullptr;

			// add() may have moved the earlier entries: look them up again by position
			const std::size_t n = specs.size();
			search = &specs[n - (scale ? 3 : 2)];
			filter = &specs[n - (scale ? 2 : 1)];
			scale = scale ? &specs[n - 1] : nullptr;

			for (u32 p = 0; p < 2; p++)
			{
				const bool odd_level = (l & 1) != 0;
				const u32 a = (p != 0) != odd_level ? 1 : 0;
				const u32 b = 1 - a;

				bind(search, p, "r_optical_flow_input", m_luma[p][l]);
				bind(search, p, "r_optical_flow_previous_input", m_luma[1 - p][l]);
				bind(search, p, "rw_optical_flow", m_flow[a][l]);
				bind(search, p, "rw_optical_flow_scd_output", m_scd_output);

				bind(filter, p, "r_optical_flow_previous", m_flow[a][l]);
				bind(filter, p, "rw_optical_flow", level == 0 ? m_flow_out : m_flow[b][l]);

				if (scale)
				{
					bind(scale, p, "r_optical_flow_input", m_luma[p][l]);
					bind(scale, p, "r_optical_flow_previous_input", m_luma[1 - p][l]);
					bind(scale, p, "r_optical_flow", m_flow[b][l]);
					bind(scale, p, "rw_optical_flow_next_level", m_flow[b][l - 1]);
					bind(scale, p, "rw_optical_flow_scd_output", m_scd_output);
				}
			}
		}

		// Frame interpolation (optical flow only): reset the vector field and the counters, fill the field, interpolate.
		{
			spec* s = add(sh_fi_setup, (flow_w[0] + 7) / 8, (flow_h[0] + 7) / 8, 1, {k_block_fi}, true);
			for (u32 p = 0; p < 2; p++)
			{
				bind(s, p, "r_optical_flow_scd", m_scd_output);
				bind(s, p, "rw_optical_flow_motion_vector_field_x", m_field_x);
				bind(s, p, "rw_optical_flow_motion_vector_field_y", m_field_y);
				s->binds[p].push_back({"rw_counters", res{nullptr, true}});
			}
		}
		{
			const u32 gx = static_cast<u32>(w / static_cast<float>(k_block) + 7) / 8;
			const u32 gy = static_cast<u32>(h / static_cast<float>(k_block) + 7) / 8;
			spec* s = add(sh_fi_vector_field, gx, gy, 1, {k_block_fi}, true);
			for (u32 p = 0; p < 2; p++)
			{
				bind(s, p, "r_optical_flow", m_flow_out);
				bind(s, p, "r_previous_interpolation_source", m_color[1 - p]);
				bind(s, p, "r_current_interpolation_source", m_color[p]);
				bind(s, p, "rw_optical_flow_motion_vector_field_x", m_field_x);
				bind(s, p, "rw_optical_flow_motion_vector_field_y", m_field_y);
			}
		}
		{
			// m73b: a 2x2 quad of pixels a thread
			spec* s = add(sh_fi_interpolate, ((w + 1) / 2 + 7) / 8, ((h + 1) / 2 + 7) / 8, 1, {k_block_fi}, true);
			for (u32 p = 0; p < 2; p++)
			{
				bind(s, p, "r_optical_flow", m_flow_out);
				bind(s, p, "r_optical_flow_motion_vector_field_x", m_field_x);
				bind(s, p, "r_optical_flow_motion_vector_field_y", m_field_y);
				bind(s, p, "r_previous_interpolation_source", m_color[1 - p]);
				bind(s, p, "r_current_interpolation_source", m_color[p]);
				bind(s, p, "rw_output", m_output);
				bind(s, p, "rw_static_history", m_static);
				bind(s, p, "rw_quad_motion", m_quad_motion); // m74: each quad's motion, for the next frame
				s->binds[p].push_back({"r_counters", res{nullptr, true}});
			}
		}

		// m74: m73f's cleanup pass (the median of the edge pixels' neighbours) is gone: the specks it was for were not edge pixels
		// (ffx_frameinterpolation.h, FgUnlike), it changed little else, and a pass over the whole picture costs time.

		// The descriptor pool: two sets per dispatch.
		u32 counts[5] = {}; // sampled image, storage image, uniform dynamic, storage buffer, sampler

		for (const spec& s : specs)
		{
			const fg_shader& sh = k_fg_shaders[s.shader];

			for (u32 b = 0; b < sh.binding_count; b++)
			{
				switch (sh.bindings[b].type)
				{
				case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: counts[0] += 2; break;
				case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: counts[1] += 2; break;
				case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: counts[2] += 2; break;
				case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: counts[3] += 2; break;
				case VK_DESCRIPTOR_TYPE_SAMPLER: counts[4] += 2; break;
				default: break;
				}
			}
		}

		const VkDescriptorType types[5] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLER};
		std::vector<VkDescriptorPoolSize> sizes;

		for (u32 i = 0; i < 5; i++)
		{
			if (counts[i]) sizes.push_back({types[i], counts[i]});
		}

		VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pool_info.maxSets = static_cast<u32>(specs.size() * 2);
		pool_info.poolSizeCount = static_cast<u32>(sizes.size());
		pool_info.pPoolSizes = sizes.data();

		if (vkCreateDescriptorPool(m_device, &pool_info, nullptr, &m_pool) != VK_SUCCESS)
		{
			error = "vkCreateDescriptorPool";
			return false;
		}

		m_dispatches.clear();
		m_dispatches.reserve(specs.size());

		for (const spec& s : specs)
		{
			const fg_shader& sh = k_fg_shaders[s.shader];
			dispatch_t d{};
			d.pipeline = s.shader;
			d.ubo_count = s.ubo_count;
			d.groups[0] = s.groups[0];
			d.groups[1] = s.groups[1];
			d.groups[2] = s.groups[2];
			d.interpolation = s.interpolation;

			for (u32 i = 0; i < s.ubo_count; i++)
			{
				d.ubo_offsets[i] = s.ubo[i] * m_ubo_block;
			}

			for (u32 p = 0; p < 2; p++)
			{
				VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
				alloc.descriptorPool = m_pool;
				alloc.descriptorSetCount = 1;
				alloc.pSetLayouts = &m_pipelines[s.shader].set_layout;

				if (vkAllocateDescriptorSets(m_device, &alloc, &d.set[p]) != VK_SUCCESS)
				{
					error = std::string("vkAllocateDescriptorSets ") + sh.name;
					return false;
				}

				std::vector<VkWriteDescriptorSet> writes(sh.binding_count);
				std::vector<VkDescriptorImageInfo> image_infos(sh.binding_count);
				std::vector<VkDescriptorBufferInfo> buffer_infos(sh.binding_count);
				u32 ubo_index = 0;

				for (u32 b = 0; b < sh.binding_count; b++)
				{
					const fg_binding& fb = sh.bindings[b];
					VkWriteDescriptorSet& wr = writes[b];
					wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
					wr.dstSet = d.set[p];
					wr.dstBinding = m_pipelines[s.shader].slot[b]; // PS5SX2: the compact number (create())
					wr.descriptorCount = 1;
					wr.descriptorType = fb.type;

					if (fb.type == VK_DESCRIPTOR_TYPE_SAMPLER)
					{
						image_infos[b] = {m_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
						wr.pImageInfo = &image_infos[b];
						continue;
					}

					if (fb.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
					{
						// The blocks in the order of the bindings (200: cbOF or cbFI, 201: cbOF_SPD); the dynamic offsets follow it.
						buffer_infos[b] = {m_ubo, 0, m_ubo_block};
						wr.pBufferInfo = &buffer_infos[b];
						ubo_index++;
						continue;
					}

					const res* r = nullptr;

					for (const auto& [name, rr] : s.binds[p])
					{
						if (std::strcmp(name, fb.name) == 0)
						{
							r = &rr;
						}
					}

					if (!r)
					{
						error = std::string("framegen: nothing bound to ") + fb.name + " in " + sh.name;
						return false;
					}

					if (fb.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
					{
						buffer_infos[b] = {m_counters, 0, VK_WHOLE_SIZE};
						wr.pBufferInfo = &buffer_infos[b];
					}
					else
					{
						image_infos[b] = {VK_NULL_HANDLE, r->img->view, VK_IMAGE_LAYOUT_GENERAL};
						wr.pImageInfo = &image_infos[b];
					}
				}

				if (ubo_index != s.ubo_count)
				{
					error = std::string("framegen: uniform block count of ") + sh.name;
					return false;
				}

				vkUpdateDescriptorSets(m_device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
			}

			if (s.shader == sh_fi_interpolate)
			{
				m_interp_dispatch = static_cast<u32>(m_dispatches.size()); // m74: interpolate_next() dispatches it again
			}

			m_dispatches.push_back(d);
		}

		return true;
	}

	void interpolator::destroy()
	{
		if (!m_device)
		{
			return;
		}

		for (pipeline_t& p : m_pipelines)
		{
			if (p.pipeline) vkDestroyPipeline(m_device, p.pipeline, nullptr);
			if (p.layout) vkDestroyPipelineLayout(m_device, p.layout, nullptr);
			if (p.set_layout) vkDestroyDescriptorSetLayout(m_device, p.set_layout, nullptr);
			if (p.module) vkDestroyShaderModule(m_device, p.module, nullptr);
		}

		m_pipelines.clear();
		m_dispatches.clear();

		if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
		if (m_queries) vkDestroyQueryPool(m_device, m_queries, nullptr);
		m_queries = VK_NULL_HANDLE;
		m_write_timestamp = nullptr;
		if (m_sampler) vkDestroySampler(m_device, m_sampler, nullptr);
		if (m_ubo_map) vkUnmapMemory(m_device, m_ubo_memory);
		if (m_ubo) vkDestroyBuffer(m_device, m_ubo, nullptr);
		if (m_ubo_memory) vkFreeMemory(m_device, m_ubo_memory, nullptr);
		if (m_counters) vkDestroyBuffer(m_device, m_counters, nullptr);
		if (m_counters_memory) vkFreeMemory(m_device, m_counters_memory, nullptr);
		m_pool = VK_NULL_HANDLE;
		m_sampler = VK_NULL_HANDLE;
		m_ubo_map = nullptr;
		m_ubo = VK_NULL_HANDLE;
		m_ubo_memory = VK_NULL_HANDLE;
		m_counters = VK_NULL_HANDLE;
		m_counters_memory = VK_NULL_HANDLE;

		for (image_t& c : m_color) free_image(c);
		for (auto& set : m_luma) for (image_t& i : set) free_image(i);
		for (auto& set : m_flow) for (image_t& i : set) free_image(i);
		free_image(m_flow_out);
		free_image(m_scd_histogram);
		free_image(m_scd_previous_histogram);
		free_image(m_scd_temp);
		free_image(m_scd_output);
		free_image(m_field_x);
		free_image(m_field_y);
		free_image(m_output);
		free_image(m_quad_motion);
		free_image(m_static);

		m_device = VK_NULL_HANDLE;
		m_physical = VK_NULL_HANDLE;
		m_width = m_height = 0;
	}

	void interpolator::prepare(VkCommandBuffer cmd)
	{
		if (m_prepared || !m_device)
		{
			return;
		}

		VkImageMemoryBarrier barriers[2];

		for (u32 i = 0; i < 2; i++)
		{
			VkImageMemoryBarrier& b = barriers[i];
			b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			b.srcAccessMask = 0;
			b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = m_color[i].image;
			b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		}

		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			0, 0, nullptr, 0, nullptr, 2, barriers);
		m_prepared = true;
	}

	VkImage interpolator::frame_image() const
	{
		return m_color[m_frames & 1].image;
	}

	VkImageView interpolator::frame_view() const
	{
		return m_color[m_frames & 1].view;
	}

	VkImage interpolator::previous_image() const
	{
		return m_color[(m_frames & 1) ^ 1].image;
	}

	void interpolator::advance()
	{
		m_frames++;
	}

	void interpolator::write_constants(u32 slot, bool reset)
	{
		std::uint8_t* base = m_ubo_map + std::size_t{slot} * m_ubo_slot;

		for (u32 level = 0; level < k_levels; level++)
		{
			cb_of c{};
			c.input_luma_resolution[0] = static_cast<std::int32_t>(m_width);
			c.input_luma_resolution[1] = static_cast<std::int32_t>(m_height);
			c.pyramid_level = level;
			c.pyramid_level_count = k_levels;
			c.frame_index = m_flow_frame_index;
			c.backbuffer_transfer_function = 0; // SDR, sRGB-encoded
			c.min_max_luminance[0] = 0.f;
			c.min_max_luminance[1] = 1.f;
			std::memcpy(base + (k_block_of + level) * m_ubo_block, &c, sizeof(c));
		}

		{
			cb_of_spd c{};
			const u32 gx = (m_width - 1) / 64 + 1;
			const u32 gy = (m_height - 1) / 64 + 1;
			c.mips = 4;
			c.num_work_groups = gx * gy;
			c.num_work_groups_pyramid = gx * gy;
			std::memcpy(base + k_block_of_spd * m_ubo_block, &c, sizeof(c));
		}

		{
			cb_fi c{};
			const std::int32_t w = static_cast<std::int32_t>(m_width);
			const std::int32_t h = static_cast<std::int32_t>(m_height);
			c.render_size[0] = c.display_size[0] = c.upscaler_target_size[0] = c.max_render_size[0] = c.interpolation_rect_size[0] = w;
			c.render_size[1] = c.display_size[1] = c.upscaler_target_size[1] = c.max_render_size[1] = c.interpolation_rect_size[1] = h;
			c.display_size_rcp[0] = 1.f / m_width;
			c.display_size_rcp[1] = 1.f / m_height;
			c.camera_near = 0.1f;
			c.camera_far = 1000.f;
			c.reset = reset ? 1 : 0;
			c.delta_time = 33.3f;
			c.distortion_field_size[0] = c.distortion_field_size[1] = 1;
			c.optical_flow_scale[0] = 1.f / m_width;
			c.optical_flow_scale[1] = 1.f / m_height;
			c.optical_flow_block_size = static_cast<std::int32_t>(k_block);
			c.num_instances = 1;
			c.back_buffer_transfer_function = 0;
			c.min_max_luminance[0] = 0.f;
			c.min_max_luminance[1] = 1.f;
			c.tan_half_fov = 1.f;
			c.dispatch_flags = m_debug_view ? (1u << 2) : 0u; // FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW

			// m74: the moment of each frame generated, a (i + 1) / (n + 1) of the way; the ones after the first keep the history as it is
			// (FG_DISPATCH_KEEP_HISTORY in ffx_frameinterpolation.h)
			for (u32 i = 0; i < k_max_generated; i++)
			{
				c.interpolation_t = m_generated == 1 ? 0.5f : static_cast<float>(i + 1) / static_cast<float>(m_generated + 1);
				c.dispatch_flags = (m_debug_view ? (1u << 2) : 0u) | (i ? (1u << 8) : 0u);
				std::memcpy(base + (k_block_fi + i) * m_ubo_block, &c, sizeof(c));
			}
		}
	}

	void interpolator::read_gpu_time(u32 slot)
	{
		// The slot was last written k_slots frames ago: long done, unless the GPU is that far behind (then it is skipped).
		if (!(m_timed_slots & (1u << slot)))
		{
			return;
		}

		m_timed_slots &= ~(1u << slot);
		std::uint64_t ts[6] = {}; // three timestamps, each with its availability
		const VkResult r = vkGetQueryPoolResults(m_device, m_queries, slot * 3, 3, sizeof(ts), ts, 2 * sizeof(std::uint64_t),
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

		if (r != VK_SUCCESS || !ts[1] || !ts[3] || !ts[5] || ts[4] < ts[0] || ts[2] < ts[0] || ts[4] < ts[2])
		{
			return;
		}

		const double ms = static_cast<double>((ts[4] - ts[0]) & m_timestamp_mask) * m_timestamp_ns / 1e6;
		const double flow_ms = static_cast<double>((ts[2] - ts[0]) & m_timestamp_mask) * m_timestamp_ns / 1e6;

		if (ms < 1000.0)
		{
			m_gpu_ms_sum += ms;
			m_gpu_ms_max = std::max(m_gpu_ms_max, ms);
			m_gpu_flow_ms_sum += flow_ms;
			m_gpu_count++;
		}
	}

	bool interpolator::take_gpu_time(double& mean_ms, double& max_ms, u32& count)
	{
		double flow_ms = 0.0;
		return take_gpu_time(mean_ms, max_ms, count, flow_ms);
	}

	bool interpolator::take_gpu_time(double& mean_ms, double& max_ms, u32& count, double& flow_mean_ms)
	{
		count = m_gpu_count;
		mean_ms = m_gpu_count ? m_gpu_ms_sum / m_gpu_count : 0.0;
		flow_mean_ms = m_gpu_count ? m_gpu_flow_ms_sum / m_gpu_count : 0.0;
		max_ms = m_gpu_ms_max;
		m_gpu_ms_sum = m_gpu_ms_max = m_gpu_flow_ms_sum = 0.0;
		m_gpu_count = 0;
		return count != 0;
	}

	bool interpolator::record(VkCommandBuffer cmd, bool reset)
	{
		if (!m_device)
		{
			return false;
		}

		const u32 parity = static_cast<u32>(m_frames & 1);
		const u32 slot = static_cast<u32>(m_frames % k_slots);

		if (!m_cleared)
		{
			reset = true;
		}

		m_last_slot = slot;
		m_last_parity = parity;

		m_flow_frame_index = reset ? 0 : m_flow_frame_index + 1;
		write_constants(slot, reset);

		// The first frame: every image to GENERAL (once), the counters zeroed.
		if (!m_cleared)
		{
			std::vector<VkImageMemoryBarrier> barriers;

			const auto to_general = [&](const image_t& img, bool keep)
			{
				VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
				b.srcAccessMask = keep ? (VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) : 0;
				b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
				b.oldLayout = keep ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
				b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
				b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.image = img.image;
				b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				barriers.push_back(b);
			};

			// The frames were put in GENERAL by prepare(); the rest starts undefined.
			for (const auto& set : m_luma) for (const image_t& i : set) to_general(i, false);
			for (const auto& set : m_flow) for (const image_t& i : set) to_general(i, false);
			to_general(m_flow_out, false);
			to_general(m_scd_histogram, false);
			to_general(m_scd_previous_histogram, false);
			to_general(m_scd_temp, false);
			to_general(m_scd_output, false);
			to_general(m_field_x, false);
			to_general(m_field_y, false);
			to_general(m_output, false);
			to_general(m_static, false);
			to_general(m_quad_motion, false);

			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0, 0, nullptr, 0, nullptr, static_cast<u32>(barriers.size()), barriers.data());

			vkCmdFillBuffer(cmd, m_counters, 0, VK_WHOLE_SIZE, 0);

			// m74: the still history starts at 0. It was left undefined: the interpolation pass, which clears it on the first frame after a
			// reset, does not run on that frame, so whatever the memory held counted as frames a pixel had not changed, and pixels that
			// happened to stay the same for one frame (dark, flat parts of a moving character) were held still at once.
			{
				memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
					VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
				const VkClearColorValue zero{};
				const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				vkCmdClearColorImage(cmd, m_static.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
				vkCmdClearColorImage(cmd, m_quad_motion.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
			}

			m_cleared = true;
		}

		if (reset)
		{
			// ffx_opticalflow.cpp on a reset: the scene change detection's state and both luma pyramids cleared.
			const VkClearColorValue zero{};
			const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			const auto clear = [&](const image_t& img) { vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range); };

			clear(m_scd_temp);
			clear(m_scd_output);
			clear(m_scd_histogram);
			clear(m_scd_previous_histogram);

			for (const auto& set : m_luma) for (const image_t& i : set) clear(i);
			// m73b: the still history is kept: the HUD stays where it was across a cut, and a pixel that changes starts again
			// m74: the quads' motions are not (they belong to the scene before)
			clear(m_quad_motion);
		}

		// The frame's picture (copied or drawn by the caller) and the clears, before the compute passes.
		memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

		// m73b: the GPU timer's first timestamp, once the work before it (the game's frame, the copy) is done
		if (m_queries)
		{
			read_gpu_time(slot);
			vkCmdResetQueryPool(cmd, m_queries, slot * 3, 3);
			m_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queries, slot * 3);
		}

		bool first = true;
		bool flow_timed = false; // m74: the middle timestamp, after the optical flow's passes

		for (const dispatch_t& d : m_dispatches)
		{
			if (d.interpolation && reset)
			{
				continue;
			}

			if (d.interpolation && !flow_timed && m_queries)
			{
				flow_timed = true;
				m_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queries, slot * 3 + 1);
			}

			if (!first)
			{
				memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
					VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
			}

			first = false;

			const pipeline_t& p = m_pipelines[d.pipeline];
			u32 offsets[2];

			for (u32 i = 0; i < d.ubo_count; i++)
			{
				offsets[i] = slot * m_ubo_slot + d.ubo_offsets[i];
			}

			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &d.set[parity], d.ubo_count, offsets);
			vkCmdDispatch(cmd, d.groups[0], d.groups[1], d.groups[2]);
		}

		// The interpolated frame (and the frame itself) for the caller's copies.
		memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);

		if (m_queries)
		{
			if (!flow_timed)
			{
				m_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queries, slot * 3 + 1);
			}

			m_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queries, slot * 3 + 2);
			m_timed_slots |= 1u << slot;
		}

		if (!reset)
		{
			m_interpolated++;
		}

		m_last_made = !reset;
		return !reset;
	}

	bool interpolator::interpolate_next(VkCommandBuffer cmd, u32 i)
	{
		if (!m_device || !m_last_made || i == 0 || i >= m_generated || m_interp_dispatch >= m_dispatches.size())
		{
			return false;
		}

		const dispatch_t& d = m_dispatches[m_interp_dispatch];
		const pipeline_t& p = m_pipelines[d.pipeline];

		// The caller's copy of the frame before reads output_image(): this one writes it only after that; and the still history and the
		// quads' motions the first one wrote are read here.
		memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

		// The same pass as record()'s last, with the i-th frame's constants (write_constants: one cbFI block a frame generated)
		u32 offsets[2] = {};
		for (u32 k = 0; k < d.ubo_count; k++)
		{
			offsets[k] = m_last_slot * m_ubo_slot + d.ubo_offsets[k] + (k == 0 ? i * m_ubo_block : 0);
		}

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &d.set[m_last_parity], d.ubo_count, offsets);
		vkCmdDispatch(cmd, d.groups[0], d.groups[1], d.groups[2]);

		memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
		m_interpolated++;
		return true;
	}
}
