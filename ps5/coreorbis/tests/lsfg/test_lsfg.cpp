// PS5SX2 (2026-11-14, AI-assisted): PC test of the Lossless Scaling frame generation port (orbis-shims/lsfg).
//   1. The DLL reader on synthetic PE files (no Lossless.dll is used or shipped): the 25 shaders found and renumbered,
//      and each way a file can be wrong (missing, empty, not a PE, a shader missing, not SPIR-V, damaged tables).
//   2. The whole chain on lavapipe with stand-in shaders (stub.comp for every pass, stub_generate.comp for the last):
//      every pipeline, descriptor set and dispatch the console makes, and the generated frame read back (the mix of the
//      last two frames by the chain's timestamp), in R8G8B8A8 and B8G8R8A8 frames, and after a reset.
//   test_lsfg <stub.spv> <stub_generate.spv> <scratch dir>
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#define VMA_IMPLEMENTATION 1
#include "lsfg_vk.h"

#include "lossless_dll.h"
#include "lsfg_backend.h"
#include "lsfg_translate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace lsfg;

static int g_fail = 0;
static int g_pass = 0;
#define CHECK(cond, ...) \
	do \
	{ \
		if (cond) \
			g_pass++; \
		else \
		{ \
			g_fail++; \
			std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			std::printf(__VA_ARGS__); \
			std::printf("\n"); \
		} \
	} while (0)

static std::vector<u8> ReadFile(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void WriteFile(const std::string& path, const std::vector<u8>& data)
{
	std::ofstream f(path, std::ios::binary);
	f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

template <typename T>
static void Put(std::vector<u8>& v, size_t off, T value)
{
	if (v.size() < off + sizeof(T))
		v.resize(off + sizeof(T));
	std::memcpy(v.data() + off, &value, sizeof(T));
}

// A PE32+ DLL with one .rsrc section holding `resources` as RCDATA (type 10), one language each.
static std::vector<u8> BuildPe(const std::map<u32, std::vector<u8>>& resources, bool pe32 = false)
{
	constexpr u32 kSectionRva = 0x1000;
	constexpr u32 kSectionRaw = 0x400;
	const u16 opt_size = pe32 ? 224 : 240;
	std::vector<u8> pe(kSectionRaw, 0);
	Put<u16>(pe, 0, 0x5A4D);
	Put<u32>(pe, 0x3C, 0x40);
	Put<u32>(pe, 0x40, 0x00004550);
	const size_t coff = 0x44;
	Put<u16>(pe, coff + 0, 0x8664);
	Put<u16>(pe, coff + 2, 1); // sections
	Put<u16>(pe, coff + 16, opt_size);
	const size_t opt = coff + 20;
	Put<u16>(pe, opt, pe32 ? 0x010B : 0x020B);
	const size_t dirs = opt + (pe32 ? 96 : 112);

	// The resource tree, laid out from the section's start.
	std::vector<u8> rs;
	const u32 n = static_cast<u32>(resources.size());
	// root: 1 id entry (RCDATA)
	Put<u16>(rs, 14, 1);
	const u32 type_dir = 16 + 8;
	Put<u32>(rs, 16, 10);
	Put<u32>(rs, 20, 0x80000000u | type_dir);
	// type directory: n id entries
	Put<u16>(rs, type_dir + 14, static_cast<u16>(n));
	const u32 name_dirs = type_dir + 16 + 8 * n;
	const u32 data_entries = name_dirs + n * (16 + 8);
	u32 data_at = data_entries + n * 16;
	u32 i = 0;
	for (const auto& [id, blob] : resources)
	{
		Put<u32>(rs, type_dir + 16 + 8 * i, id);
		const u32 name_dir = name_dirs + i * 24;
		Put<u32>(rs, type_dir + 16 + 8 * i + 4, 0x80000000u | name_dir);
		Put<u16>(rs, name_dir + 14, 1);
		Put<u32>(rs, name_dir + 16, 0x409);
		const u32 data_entry = data_entries + i * 16;
		Put<u32>(rs, name_dir + 20, data_entry);
		data_at = (data_at + 3) & ~3u;
		Put<u32>(rs, data_entry, kSectionRva + data_at);
		Put<u32>(rs, data_entry + 4, static_cast<u32>(blob.size()));
		if (rs.size() < data_at + blob.size())
			rs.resize(data_at + blob.size());
		std::memcpy(rs.data() + data_at, blob.data(), blob.size());
		data_at += static_cast<u32>(blob.size());
		i++;
	}
	Put<u32>(pe, dirs + 2 * 8, kSectionRva);
	Put<u32>(pe, dirs + 2 * 8 + 4, static_cast<u32>(rs.size()));
	const size_t sec = opt + opt_size;
	std::memcpy(pe.data() + sec, ".rsrc\0\0\0", 8);
	Put<u32>(pe, sec + 8, static_cast<u32>(rs.size()));
	Put<u32>(pe, sec + 12, kSectionRva);
	Put<u32>(pe, sec + 16, static_cast<u32>(rs.size()));
	Put<u32>(pe, sec + 20, kSectionRaw);
	pe.insert(pe.end(), rs.begin(), rs.end());
	return pe;
}

static std::map<u32, std::vector<u8>> StubResources(const std::vector<u8>& stub, const std::vector<u8>& generate)
{
	std::map<u32, std::vector<u8>> res;
	for (const u32 id : PerformanceShaderIds())
		res[id + PerformanceShader::NATIVE_FP16_OFFSET] = id == PerformanceShader::GENERATE ? generate : stub;
	// What else a real DLL has (the other shader sets): ignored.
	res[1] = {1, 2, 3, 4};
	res[600] = stub;
	return res;
}

// The Binding literals of a module, in the order of its decorations.
static std::vector<u32> Bindings(const std::vector<u32>& words)
{
	std::vector<u32> out;
	for (size_t off = 5; off < words.size();)
	{
		const u32 len = words[off] >> 16, op = words[off] & 0xffff;
		if (len == 0)
			break;
		if (op == 54)
			break;
		if (op == 71 && len >= 4 && words[off + 2] == 33)
			out.push_back(words[off + 3]);
		off += len;
	}
	return out;
}

static void TestDll(const std::vector<u8>& stub, const std::vector<u8>& generate, const std::string& dir)
{
	ShaderModules modules;
	const std::vector<u8> good = BuildPe(StubResources(stub, generate));
	CHECK(ParseShaderModules(good, modules) == LosslessStatus::Ok, "the synthetic DLL parses");
	CHECK(modules.size() == 25, "25 shaders (got %zu)", modules.size());
	CHECK(modules.count(PerformanceShader::GENERATE) == 1 && modules.count(PerformanceShader::MIPMAPS) == 1, "mipmaps and generate");
	if (modules.count(PerformanceShader::GENERATE))
	{
		std::vector<u32> b = Bindings(modules[PerformanceShader::GENERATE]);
		std::sort(b.begin(), b.end());
		CHECK(b == (std::vector<u32>{0, 1, 2, 3, 4, 5, 6, 7, 8}), "generate's bindings renumbered 0..8 (%zu)", b.size());
		const SpirvInfo info = DescribeSpirv(modules[PerformanceShader::GENERATE]);
		CHECK(info.valid && info.entry_point == "main" && info.bindings == 9, "generate described (entry %s, %u bindings)",
			info.entry_point.c_str(), info.bindings);
	}
	if (modules.count(280))
	{
		const SpirvInfo info = DescribeSpirv(modules[280]);
		bool f16 = false, vmm = false;
		for (const u32 c : info.capabilities)
		{
			f16 |= SpirvCapabilityName(c) == "Float16";
			vmm |= SpirvCapabilityName(c) == "VulkanMemoryModel";
		}
		CHECK(f16 && vmm, "the stub's capabilities are read (Float16 %d, VulkanMemoryModel %d)", f16, vmm);
	}
	ShaderModules m32;
	CHECK(ParseShaderModules(BuildPe(StubResources(stub, generate), true), m32) == LosslessStatus::Ok && m32.size() == 25, "a PE32 DLL too");

	// The ways it can be wrong.
	CHECK(ParseShaderModules({}, modules) == LosslessStatus::NotPortableExecutable, "empty: not a PE");
	std::vector<u8> text(4096, 'x');
	CHECK(ParseShaderModules(text, modules) == LosslessStatus::NotPortableExecutable, "text: not a PE");
	auto missing = StubResources(stub, generate);
	missing.erase(PerformanceShader::GENERATE + PerformanceShader::NATIVE_FP16_OFFSET);
	CHECK(ParseShaderModules(BuildPe(missing), modules) == LosslessStatus::MissingShaders, "generate missing");
	auto not_spirv = StubResources(stub, generate);
	not_spirv[290 + PerformanceShader::NATIVE_FP16_OFFSET] = std::vector<u8>(64, 0x11);
	CHECK(ParseShaderModules(BuildPe(not_spirv), modules) == LosslessStatus::MissingShaders, "a shader that is not SPIR-V");
	std::map<u32, std::vector<u8>> none;
	none[1] = {1, 2, 3, 4};
	CHECK(ParseShaderModules(BuildPe(none), modules) == LosslessStatus::MissingShaders, "a DLL without the shaders");
	// Every truncation and a sweep of damaged bytes: no crash, never Ok with a short file (one whose last resource is a
	// shader the chain needs: a cut into the extra resources after them leaves a usable DLL).
	auto needed_only = StubResources(stub, generate);
	needed_only.erase(1);
	needed_only.erase(600);
	const std::vector<u8> tight = BuildPe(needed_only);
	CHECK(ParseShaderModules(tight, modules) == LosslessStatus::Ok, "the DLL with the needed shaders only");
	int truncated_ok = 0;
	for (size_t len = 0; len + 1 < tight.size(); len += 97)
	{
		std::vector<u8> t(tight.begin(), tight.begin() + static_cast<std::ptrdiff_t>(len));
		truncated_ok += ParseShaderModules(t, modules) == LosslessStatus::Ok;
	}
	CHECK(truncated_ok == 0, "a truncated DLL is never ok (%d were)", truncated_ok);
	for (size_t off = 0; off < 0x400 + 512 && off < good.size(); off++)
	{
		std::vector<u8> d = good;
		d[off] ^= 0xA5;
		(void)ParseShaderModules(d, modules);
	}
	g_pass++; // no crash

	// From files: missing, unreadable (a folder), found in any case.
	const std::string folder = dir + "/lossless";
	mkdir(folder.c_str(), 0755);
	CHECK(LoadShaderModules(folder + "/Lossless.dll", modules) == LosslessStatus::NotInstalled, "no file: not installed");
	CHECK(FindLosslessDll(folder) == folder + "/Lossless.dll", "the default name when none");
	WriteFile(folder + "/LOSSLESS.DLL", good);
	const std::string found = FindLosslessDll(folder);
	CHECK(found == folder + "/LOSSLESS.DLL", "found without case (%s)", found.c_str());
	u64 size = 0;
	CHECK(LoadShaderModules(found, modules, &size) == LosslessStatus::Ok && size == good.size(), "loaded from the file");
	CHECK(LoadShaderModules(folder, modules) == LosslessStatus::UnreadableFile, "a folder: unreadable");
	WriteFile(folder + "/empty.dll", {});
	CHECK(LoadShaderModules(folder + "/empty.dll", modules) == LosslessStatus::UnreadableFile, "an empty file: unreadable");
	CHECK(std::string(LosslessStatusText(LosslessStatus::NotInstalled)) == "Lossless.dll not installed", "status text");
}

// ---- The chain on lavapipe.

struct Gpu
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	u32 family = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	VmaAllocator allocator = VK_NULL_HANDLE;
};

static bool HasExtension(VkPhysicalDevice pd, const char* name)
{
	u32 n = 0;
	vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> ext(n);
	vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, ext.data());
	for (const auto& e : ext)
		if (!std::strcmp(e.extensionName, name))
			return true;
	return false;
}

static bool MakeGpu(Gpu& g)
{
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1; // as GSDeviceVK.cpp
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	if (vkCreateInstance(&ici, nullptr, &g.instance) != VK_SUCCESS)
		return false;
	u32 n = 1;
	if (vkEnumeratePhysicalDevices(g.instance, &n, &g.physical) < 0 || n == 0)
		return false;
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(g.physical, &props);
	std::printf("device: %s\n", props.deviceName);

	// The features the LSFG shaders need, enabled as GSDeviceVK.cpp does when the method is Lossless Scaling.
	VkPhysicalDeviceVulkanMemoryModelFeatures vmm = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
	VkPhysicalDeviceShaderFloat16Int8Features f16 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
	f16.pNext = &vmm;
	VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	f2.pNext = &f16;
	vkGetPhysicalDeviceFeatures2(g.physical, &f2);
	if (!vmm.vulkanMemoryModel || !f16.shaderFloat16)
	{
		std::printf("the device lacks the Vulkan memory model or float16\n");
		return false;
	}
	std::vector<const char*> exts;
	for (const char* e : {"VK_KHR_vulkan_memory_model", "VK_KHR_shader_float16_int8"})
		if (HasExtension(g.physical, e))
			exts.push_back(e);
	// What Lossless Scaling's own shaders declare (2026-11-14, a real DLL: SPIR-V 1.0, Float16, StorageImageExtendedFormats,
	// StorageImageWriteWithoutFormat, ImageQuery; no memory model): the core features GSDeviceVK.cpp turns on for them.
	VkPhysicalDeviceFeatures features = {};
	features.shaderStorageImageWriteWithoutFormat = f2.features.shaderStorageImageWriteWithoutFormat;
	features.shaderStorageImageExtendedFormats = f2.features.shaderStorageImageExtendedFormats;
	f2.features = features;
	vmm.vulkanMemoryModelAvailabilityVisibilityChains = VK_FALSE;
	f16.shaderInt8 = VK_FALSE;

	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = 0;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext = &f2;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = static_cast<u32>(exts.size());
	dci.ppEnabledExtensionNames = exts.data();
	if (vkCreateDevice(g.physical, &dci, nullptr, &g.device) != VK_SUCCESS)
		return false;
	vkGetDeviceQueue(g.device, 0, 0, &g.queue);
	VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	vkCreateCommandPool(g.device, &pci, nullptr, &g.pool);
	VmaAllocatorCreateInfo aci = {};
	aci.vulkanApiVersion = VK_API_VERSION_1_1;
	aci.physicalDevice = g.physical;
	aci.device = g.device;
	aci.instance = g.instance;
	return vmaCreateAllocator(&aci, &g.allocator) == VK_SUCCESS;
}

static void FreeGpu(Gpu& g)
{
	if (g.allocator)
		vmaDestroyAllocator(g.allocator);
	if (g.pool)
		vkDestroyCommandPool(g.device, g.pool, nullptr);
	if (g.device)
		vkDestroyDevice(g.device, nullptr);
	if (g.instance)
		vkDestroyInstance(g.instance, nullptr);
}

template <typename F>
static void Submit(Gpu& g, F&& record)
{
	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkCommandBuffer cb;
	vkAllocateCommandBuffers(g.device, &ai, &cb);
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cb, &bi);
	record(cb);
	vkEndCommandBuffer(cb);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(g.queue);
	vkFreeCommandBuffers(g.device, g.pool, 1, &cb);
}

static void Barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags ss,
	VkPipelineStageFlags ds)
{
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = src;
	b.dstAccessMask = dst;
	b.oldLayout = from;
	b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Draws one frame of a solid colour into the backend's frame image (the present's draw on the console), records the
// passes, and reads the generated frame's centre pixel back when there is one.
static bool RunFrame(Gpu& g, Backend& fg, const float rgba[4], u8 out[4], bool& generated)
{
	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
	aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT};
	VkBuffer buf;
	VmaAllocation alloc;
	VmaAllocationInfo info;
	if (vmaCreateBuffer(g.allocator, &bci, &aci, &buf, &alloc, &info) != VK_SUCCESS)
		return false;
	Submit(g, [&](VkCommandBuffer cb) {
		const VkImage frame = fg.FrameImage();
		Barrier(cb, frame, VK_IMAGE_LAYOUT_UNDEFINED, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkClearColorValue c;
		std::memcpy(c.float32, rgba, sizeof(c.float32));
		const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdClearColorImage(cb, frame, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
		Barrier(cb, frame, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		generated = fg.Record(cb);
		if (generated)
		{
			Barrier(cb, fg.OutputImage(), VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy region = {};
			region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.imageOffset = {static_cast<int32_t>(fg.Width() / 2), static_cast<int32_t>(fg.Height() / 2), 0};
			region.imageExtent = {1, 1, 1};
			vkCmdCopyImageToBuffer(cb, fg.OutputImage(), VK_IMAGE_LAYOUT_GENERAL, buf, 1, &region);
		}
	});
	vmaInvalidateAllocation(g.allocator, alloc, 0, VK_WHOLE_SIZE);
	std::memcpy(out, info.pMappedData, 4);
	vmaDestroyBuffer(g.allocator, buf, alloc);
	return true;
}

static bool Near(const u8 px[4], int r, int g, int b, int a)
{
	return std::abs(px[0] - r) <= 1 && std::abs(px[1] - g) <= 1 && std::abs(px[2] - b) <= 1 && std::abs(px[3] - a) <= 1;
}

static void TestChain(const std::vector<u8>& stub, const std::vector<u8>& generate)
{
	Gpu g;
	if (!MakeGpu(g))
	{
		CHECK(false, "no Vulkan device with the memory model and float16 (set VK_ICD_FILENAMES to lavapipe's)");
		FreeGpu(g);
		return;
	}
	ShaderModules modules;
	CHECK(ParseShaderModules(BuildPe(StubResources(stub, generate)), modules) == LosslessStatus::Ok, "modules for the chain");

	for (const VkFormat format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM})
	{
		for (const f32 flow : {1.0f, 0.5f})
		{
			Backend fg;
			std::string error;
			const bool made = fg.Create(g.physical, g.device, g.allocator, modules, 320, 180, format, flow, error);
			CHECK(made, "the chain at 320x180, format %d, flow %.1f: %s", static_cast<int>(format), flow, error.c_str());
			if (!made)
				continue;
			CHECK(fg.FrameImage(0) != fg.FrameImage(1) && fg.FrameImage(0) && fg.OutputImage(), "two frame images and the output");
			const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1}, white[4] = {1, 1, 1, 1};
			u8 px[4] = {};
			bool gen = true;
			RunFrame(g, fg, red, px, gen);
			CHECK(!gen, "nothing generated from the first frame");
			RunFrame(g, fg, green, px, gen);
			CHECK(!gen, "nothing generated from the second frame (the shared passes only)");
			RunFrame(g, fg, blue, px, gen);
			CHECK(gen && Near(px, 0, 128, 128, 255), "third frame: green and blue mixed (%u %u %u %u)", px[0], px[1], px[2], px[3]);
			RunFrame(g, fg, white, px, gen);
			CHECK(gen && Near(px, 128, 128, 255, 255), "fourth frame: blue and white mixed (%u %u %u %u)", px[0], px[1], px[2], px[3]);
			fg.Reset();
			RunFrame(g, fg, red, px, gen);
			CHECK(!gen, "after a reset: nothing from the first frame");
			RunFrame(g, fg, green, px, gen);
			CHECK(!gen, "after a reset: nothing from the second");
			RunFrame(g, fg, blue, px, gen);
			CHECK(gen && Near(px, 0, 128, 128, 255), "after a reset: generated again (%u %u %u %u)", px[0], px[1], px[2], px[3]);
			CHECK(fg.Frames() == 7 && fg.Generated() == 3, "counts: %llu frames, %llu generated", (unsigned long long)fg.Frames(),
				(unsigned long long)fg.Generated());
		}
	}

	// A DLL whose generate shader is missing: Create says which.
	ShaderModules partial = modules;
	partial.erase(PerformanceShader::GENERATE);
	{
		Backend fg;
		std::string error;
		CHECK(!fg.Create(g.physical, g.device, g.allocator, partial, 64, 64, VK_FORMAT_R8G8B8A8_UNORM, 1.0f, error) &&
				  error.find("shader 256") != std::string::npos,
			"a missing shader is named (%s)", error.c_str());
		CHECK(!fg.Ready() && fg.FrameImage() == VK_NULL_HANDLE, "and nothing is left");
	}
	FreeGpu(g);
}

// ---- With the player's own Lossless.dll (optional: test-lsfg.sh passes LSFG_DLL; nothing of it is kept or shipped).

// Uploads `pixels` (RGBA8, w x h) into the frame image, records the passes, and reads the whole generated frame back.
static bool RunPictureFrame(Gpu& g, Backend& fg, const std::vector<u8>& pixels, std::vector<u8>& out, bool& generated)
{
	const VkDeviceSize bytes = static_cast<VkDeviceSize>(fg.Width()) * fg.Height() * 4;
	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
	aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, bytes,
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
	VkBuffer up, down;
	VmaAllocation upa, downa;
	VmaAllocationInfo upi, downi;
	if (vmaCreateBuffer(g.allocator, &bci, &aci, &up, &upa, &upi) != VK_SUCCESS ||
		vmaCreateBuffer(g.allocator, &bci, &aci, &down, &downa, &downi) != VK_SUCCESS)
		return false;
	std::memcpy(upi.pMappedData, pixels.data(), bytes);
	vmaFlushAllocation(g.allocator, upa, 0, VK_WHOLE_SIZE);
	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {fg.Width(), fg.Height(), 1};
	Submit(g, [&](VkCommandBuffer cb) {
		const VkImage frame = fg.FrameImage();
		Barrier(cb, frame, VK_IMAGE_LAYOUT_UNDEFINED, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdCopyBufferToImage(cb, up, frame, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
		Barrier(cb, frame, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		generated = fg.Record(cb);
		if (generated)
		{
			Barrier(cb, fg.OutputImage(), VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			vkCmdCopyImageToBuffer(cb, fg.OutputImage(), VK_IMAGE_LAYOUT_GENERAL, down, 1, &region);
		}
	});
	vmaInvalidateAllocation(g.allocator, downa, 0, VK_WHOLE_SIZE);
	out.assign(static_cast<const u8*>(downi.pMappedData), static_cast<const u8*>(downi.pMappedData) + bytes);
	vmaDestroyBuffer(g.allocator, up, upa);
	vmaDestroyBuffer(g.allocator, down, downa);
	return true;
}

// A grey picture with a white square whose left edge is at x.
static std::vector<u8> SquarePicture(u32 w, u32 h, int x, int y, int side)
{
	std::vector<u8> p(static_cast<size_t>(w) * h * 4);
	for (u32 j = 0; j < h; j++)
		for (u32 i = 0; i < w; i++)
		{
			const bool in = static_cast<int>(i) >= x && static_cast<int>(i) < x + side && static_cast<int>(j) >= y && static_cast<int>(j) < y + side;
			u8* px = &p[(static_cast<size_t>(j) * w + i) * 4];
			px[0] = px[1] = px[2] = in ? 240 : 64;
			px[3] = 255;
		}
	return p;
}

// The centre of the bright pixels; false when there are none.
static bool BrightCentre(const std::vector<u8>& p, u32 w, u32 h, double& cx, double& cy, size_t& count)
{
	double sx = 0, sy = 0;
	count = 0;
	for (u32 j = 0; j < h; j++)
		for (u32 i = 0; i < w; i++)
		{
			const u8* px = &p[(static_cast<size_t>(j) * w + i) * 4];
			if (px[0] + px[1] + px[2] > 3 * 160)
			{
				sx += i;
				sy += j;
				count++;
			}
		}
	if (!count)
		return false;
	cx = sx / count;
	cy = sy / count;
	return true;
}

static void WritePpm(const std::string& path, const std::vector<u8>& rgba, u32 w, u32 h)
{
	std::ofstream f(path, std::ios::binary);
	f << "P6\n" << w << " " << h << "\n255\n";
	for (size_t i = 0; i < static_cast<size_t>(w) * h; i++)
		f.write(reinterpret_cast<const char*>(&rgba[i * 4]), 3);
}

static void TestRealDll(const std::string& path, const std::string& dir)
{
	ShaderModules modules;
	u64 size = 0;
	const LosslessStatus st = LoadShaderModules(path, modules, &size);
	CHECK(st == LosslessStatus::Ok && modules.size() == 25, "the real DLL: %s, %zu shaders", LosslessStatusText(st), modules.size());
	if (st != LosslessStatus::Ok)
		return;
	Gpu g;
	if (!MakeGpu(g))
	{
		CHECK(false, "no Vulkan device for the real shaders");
		FreeGpu(g);
		return;
	}
	constexpr u32 W = 640, H = 360, SIDE = 48, Y = 156, STEP = 24, X0 = 160;
	// R8_UNORM flow images as Lossless Scaling makes them, and R32_SFLOAT ones (ps5vk takes no R8_UNORM storage image). Clamping the
	// border samplers to the edge (ps5vk takes no border sampler) was measured here too on 2026-11-14 and dropped: a mean error of
	// 72 of 255 against the true halfway picture, against 2.7 with border samplers and 17 for a repeated frame.
	constexpr int kSetups[] = {0, 1};
	const char* const kSetupNames[] = {"R8_UNORM flow images", "R32_SFLOAT flow images"};
	for (const VkFormat format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM})
	{
		for (const f32 flow : {1.0f, 0.5f})
		for (const int wide : kSetups)
		{
			Backend fg;
			std::string error;
			const bool made = fg.Create(g.physical, g.device, g.allocator, modules, W, H, format, flow, error, wide);
			CHECK(made, "the real chain at %ux%u, format %d, flow %.1f, wide %d: %s", W, H, static_cast<int>(format), flow, wide,
				error.c_str());
			if (!made)
				continue;
			std::printf("  %s\n", fg.Notes().c_str());
			std::vector<u8> out;
			for (u32 n = 0; n < 6; n++)
			{
				// In a BGRA8 frame the bytes are B, G, R, A: grey and white are the same either way.
				const int x = static_cast<int>(X0 + STEP * n);
				bool gen = false;
				RunPictureFrame(g, fg, SquarePicture(W, H, x, Y, SIDE), out, gen);
				if (n < 2)
				{
					CHECK(!gen, "real shaders: nothing generated from frame %u", n);
					continue;
				}
				double cx = 0, cy = 0;
				size_t count = 0;
				const bool any = gen && BrightCentre(out, W, H, cx, cy, count);
				// Halfway between the last two squares: the previous one's centre plus half a step.
				const double want_x = x - STEP / 2.0 + SIDE / 2.0 - 0.5, want_y = Y + SIDE / 2.0 - 0.5;
				std::printf("  format %d flow %.1f frame %u: generated square centre %.1f,%.1f (%zu px), halfway is %.1f,%.1f; "
							"the two frames' are %.1f and %.1f\n",
					static_cast<int>(format), flow, n, cx, cy, count, want_x, want_y, want_x - STEP / 2.0, want_x + STEP / 2.0);
				CHECK(any && std::abs(cx - want_x) <= 4.0 && std::abs(cy - want_y) <= 3.0 && count > SIDE * SIDE / 2 &&
						  count < SIDE * SIDE * 2,
					"real shaders, frame %u: the square halfway (%.1f,%.1f, %zu px; want %.1f,%.1f)", n, cx, cy, count, want_x, want_y);
				if (n == 5 && flow == 1.0f && wide == 0)
					WritePpm(dir + "/real_generated_" + std::to_string(static_cast<int>(format)) + ".ppm", out, W, H);
			}
		}
	}

	// A detailed picture moving 5 px a frame (a smooth pattern with sharp edges): the generated frame with R32_SFLOAT flow images and
	// with R8_UNORM ones, each against the true halfway picture (the pattern drawn 2.5 px back). The R32_SFLOAT one must be as good.
	const auto pattern = [&](double shift) {
		std::vector<u8> p(static_cast<size_t>(W) * H * 4);
		for (u32 j = 0; j < H; j++)
			for (u32 i = 0; i < W; i++)
			{
				const double x = i - shift;
				const int checker = static_cast<int>(std::floor(x / 16.0)) + static_cast<int>(j) / 16;
				const double v = 0.5 + 0.25 * std::sin(x * 0.07) * std::cos(j * 0.05) + ((checker & 1) ? 0.2 : -0.2);
				u8* px = &p[(static_cast<size_t>(j) * W + i) * 4];
				px[0] = static_cast<u8>(std::clamp(v, 0.0, 1.0) * 255);
				px[1] = static_cast<u8>(std::clamp(1.0 - v, 0.0, 1.0) * 255);
				px[2] = static_cast<u8>(std::clamp(0.5 + 0.4 * std::sin(x * 0.11 + j * 0.03), 0.0, 1.0) * 255);
				px[3] = 255;
			}
		return p;
	};
	const std::vector<u8> truth = pattern(22.5); // halfway between frame 4 (20 px) and frame 5 (25 px)
	// The mean error against the truth inside (8 px in, 40 px from the left where the picture comes in from outside) or in the
	// 32 px band along the edges (where the border samplers read past the picture).
	const auto error_to_truth = [&](const std::vector<u8>& out, bool band) {
		double sum = 0;
		size_t n = 0;
		for (u32 j = 0; j < H; j++)
			for (u32 i = 0; i < W; i++)
			{
				const bool in_band = i < 32 || j < 32 || i >= W - 32 || j >= H - 32;
				const bool inside = i >= 40 && j >= 8 && i < W - 8 && j < H - 8;
				if (band ? !in_band : !inside)
					continue;
				for (int c = 0; c < 3; c++)
				{
					const size_t k = (static_cast<size_t>(j) * W + i) * 4 + c;
					sum += std::abs(static_cast<int>(out[k]) - static_cast<int>(truth[k]));
					n++;
				}
			}
		return sum / n;
	};
	for (const f32 flow : {1.0f, 0.5f})
	{
		std::vector<u8> outs[2];
		for (int s = 0; s < 2; s++)
		{
			Backend fg;
			std::string error;
			if (!fg.Create(g.physical, g.device, g.allocator, modules, W, H, VK_FORMAT_R8G8B8A8_UNORM, flow, error, kSetups[s]))
			{
				CHECK(false, "the pattern chain, %s: %s", kSetupNames[s], error.c_str());
				continue;
			}
			bool gen = false;
			for (int n = 0; n < 6; n++)
				RunPictureFrame(g, fg, pattern(5.0 * n), outs[s], gen);
			CHECK(gen, "the pattern: generated (%s)", kSetupNames[s]);
		}
		if (outs[0].size() != outs[1].size() || outs[0].empty())
			continue;
		const std::vector<u8> repeat = pattern(25.0); // what a frame without generation would show: the last game frame, 2.5 px off
		std::printf("  pattern, flow %.1f: mean error against the true halfway picture, inside / along the edges:\n", flow);
		for (int s = 0; s < 2; s++)
			std::printf("    %-34s %.2f / %.2f\n", kSetupNames[s], error_to_truth(outs[s], false), error_to_truth(outs[s], true));
		std::printf("    %-34s %.2f / %.2f\n", "the last game frame repeated", error_to_truth(repeat, false), error_to_truth(repeat, true));
		const double in8 = error_to_truth(outs[0], false), in_ps5 = error_to_truth(outs[1], false);
		const double band8 = error_to_truth(outs[0], true), band_ps5 = error_to_truth(outs[1], true);
		CHECK(in_ps5 <= in8 * 1.10 + 0.25, "R32_SFLOAT flow images are as good inside (flow %.1f: %.2f against %.2f)", flow, in_ps5, in8);
		CHECK(band_ps5 < error_to_truth(repeat, true) * 0.75,
			"R32_SFLOAT flow images beat a repeated frame along the edges (flow %.1f: %.2f; R8_UNORM %.2f; repeated %.2f)", flow, band_ps5,
			band8, error_to_truth(repeat, true));
		if (flow == 1.0f)
			WritePpm(dir + "/pattern_r32f.ppm", outs[1], W, H);
	}
	FreeGpu(g);
}

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		std::printf("usage: test_lsfg <stub.spv> <stub_generate.spv> <scratch dir>\n");
		return 2;
	}
	const std::vector<u8> stub = ReadFile(argv[1]);
	const std::vector<u8> generate = ReadFile(argv[2]);
	if (stub.empty() || generate.empty())
	{
		std::printf("FAIL: no stub shaders\n");
		return 1;
	}
	TestDll(stub, generate, argv[3]);
	TestChain(stub, generate);
	if (argc > 4 && argv[4][0])
		TestRealDll(argv[4], argc > 5 ? argv[5] : argv[3]);
	std::printf("%s: %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
