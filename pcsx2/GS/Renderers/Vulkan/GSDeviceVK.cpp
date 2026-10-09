// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GS.h"
#include "OrbisEEDiag.h" // vk-285-102: g_orbis_nt_store
#include "GS/GSGL.h"
#include "GS/GSPerfMon.h"
#include "GS/GSUtil.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Vulkan/VKShaderCache.h"
#include "GS/Renderers/Vulkan/VKSwapChain.h"
#include "GS/Renderers/Common/GSDevice.h"

#include "BuildVersion.h"
#include "Host.h"
#include "ImGui/ImGuiManager.h"

#include "common/Console.h"
#include "common/BitUtils.h"
#include "common/Error.h"
#include "common/HostSys.h"
#include "common/Path.h"
#include "common/ScopedGuard.h"

#include "imgui.h"

#include <bit>
#include <limits>
#include <mutex>
#include <sstream>
#ifdef ORBIS_VULKAN
#include <unistd.h> // vk-285-14: fsync, _exit (GPU-hang forensics)
#include <chrono> // 2026-10-08: frame generation's pacing
#include <string>
#include "OrbisPaths.h" // vk-285-33 (the port's include-orbis)
#include "OrbisDriver.h" // vk-285-115: ps5vk or RADV (the same objects link with either)
#include "OrbisExit.h" // vk-285-115: the app ends through the system (_exit() is a SIGSYS on the console)
#endif

// vk-285-36, vk-285-38: the GS thread's time in Vulkan calls, by kind (VKOrbisTiming.h), printed
// by main-boot's ticker as [vkwait] and [shaders].
#include "GS/Renderers/Vulkan/VKOrbisTiming.h"
// PS5 port (vk-285-104): this file's printf/fflush(stdout) go to the deferred log (OrbisDeferredLog.h); the
// ticker thread writes them out, so the GS thread never waits on /data or on stdout's lock.
#include "OrbisDeferredLog.h"
#ifdef ORBIS_VULKAN
#include "ps5/coreorbis/orbis-shims/ps5_framegen.h" // 2026-10-08: frame generation
#include "ps5/coreorbis/orbis-shims/OrbisFrameGenPacing.h" // vk-285-133: its pacing
#endif
#define printf OrbisDeferredPrintf
#define fflush OrbisDeferredFlush
#ifdef ORBIS_VULKAN
// vk-285-51: the port's settings log (frontend/fe_ps5.cpp), for the GPU-hang exit. Weak: tools have none.
extern "C" void orbis_event_log(const char* line) __attribute__((weak));
#endif
#ifdef ORBIS_VULKAN
unsigned long long g_orbis_vkw_ns[ORBIS_VKW_KINDS], g_orbis_vkw_n[ORBIS_VKW_KINDS],
	g_orbis_vkw_max_ns[ORBIS_VKW_KINDS];
unsigned long long g_orbis_copy_n[ORBIS_COPY_KINDS], g_orbis_copy_bytes[ORBIS_COPY_KINDS];
unsigned long long g_orbis_readback_n, g_orbis_readback_bytes, g_orbis_readback_wait_ns, g_orbis_readback_wait_n,
	g_orbis_readback_wait_max_ns, g_orbis_readback_wait_max_min_ns; // vk-285-113

// vk-285-113 (AI-assisted): draws one Vulkan command buffer may hold before it is submitted early. The PS5 driver copies all the command
// buffers of one vkQueueSubmit into a 2 MiB stream and refuses more than 523,503 words (ps5vk_queue.c); a draw is ~50-90 words (a feedback
// barrier adds 17), so 3500 draws x 90 = 315k words leaves room for uploads, clears and the present pass. RenderHW submits before a draw
// that would pass it, SendHWDraw between the groups of one full-barrier draw that is bigger than the whole budget by itself.
// live-8 (AI-assisted): 3500 -> 4400, and live-tunable. R&C1 on the base PS5 (fw 4.03, swordpdf, 2026-10-09): looking at one spot the
// frame held just over 3500 draws ("3500 draws in this submission and 2 more coming, submitting early"), so every frame became two
// submissions; on that firmware the second waits for the next vblank, so the game sat at 30 fps / 50% speed while it ran 60 everywhere
// else (8x or 6x made no difference). 4400 x 107 words (a 90-word draw plus a 17-word feedback barrier, the worst case) is 471k of the
// 523,503 the driver takes, ~52k left for uploads, clears and the present pass; the typical feedback draw is ~67 words (~295k).
// flags/vk_drawbudget holding a number (1000..4800) overrides it, read live. Needs proper testing on a console.
// live-9 (AI-assisted): the game's own default from PS5SX2_DRAW_BUDGET (main-boot.cpp sets 6000 for R&C1 since live-10, where swordpdf's base PS5 held
// 60 fps at the heavy spot with it, 2026-10-10); the flag still wins.
static u32 OrbisDefaultDrawBudget()
{
	static const u32 s_default = [] {
		const char* e = getenv("PS5SX2_DRAW_BUDGET");
		const unsigned long v = e ? std::strtoul(e, nullptr, 10) : 0;
		return (v >= 1000 && v <= 8000) ? static_cast<u32>(v) : 4400u;
	}();
	return s_default;
}
// live-10 (AI-assisted): swordpdf on live-8 with 4800, another R&C1 view: "still the same issue in ratchet" -- 30 fps / 50% again, the log
// "4800 draws in this submission and 1 more coming, submitting early (500 times)", 2 submissions and 1 present a frame. The 4800 cap was a
// worst-case guess (107 words a draw); vk-285-113 measured ~67 words a feedback draw and ~50 a plain one, so the stream has room for more.
// So the cap is 8000 now, and the words are measured: after every submission the driver's step sizes (ps5vk_debug_submission_steps, the
// last submission it queued) are summed, and a submission past 440,000 of the driver's 523,503 words drops the budget to 4400 for 10 s
// ("[vkwords] ... guard"). Every 600 submissions the largest is logged with its words per draw. Needs proper testing on a console.
struct OrbisDbgStage
{
	void* address;
	size_t bytes;
};
extern "C" uint32_t ps5vk_debug_submission_steps(VkDevice device, OrbisDbgStage* steps, uint32_t capacity) __attribute__((weak));
static constexpr u32 ORBIS_WORDS_GUARD = 440000;
static constexpr u32 ORBIS_GUARD_BUDGET = 4400;
// live-11 (AI-assisted): times on the TSC (~1.6 GHz on the PS5; only rough seconds are needed here), not steady_clock: live-10's
// autoprof had 36% of the GS thread's samples in clock_gettime (a kernel call) under steady_clock::now(), which the budget asked twice a draw.
static constexpr u64 ORBIS_TSC_SECOND = 1600000000ull;
static u64 s_orbis_guard_until_tsc = 0;
static void OrbisMeasureSubmitWords(VkDevice device, u32 draws)
{
	static u32 s_n = 0, s_peak_words = 0, s_peak_draws = 0, s_guards = 0;
	if (!ps5vk_debug_submission_steps || device == VK_NULL_HANDLE)
		return;
	OrbisDbgStage steps[64];
	const u32 count = std::min<u32>(ps5vk_debug_submission_steps(device, steps, 64), 64);
	u64 words = 0;
	for (u32 i = 0; i < count; i++)
		words += steps[i].bytes / sizeof(u32);
	if (words > s_peak_words)
	{
		s_peak_words = static_cast<u32>(words);
		s_peak_draws = draws;
	}
	if (words > ORBIS_WORDS_GUARD)
	{
		const u64 tsc = __builtin_ia32_rdtsc();
		const bool was = tsc < s_orbis_guard_until_tsc;
		s_orbis_guard_until_tsc = tsc + 10 * ORBIS_TSC_SECOND;
		if (!was && (++s_guards <= 5 || (s_guards % 100) == 0))
		{
			printf("[vkwords] guard: a submission of %llu words (%u draws) is past %u of the driver's 523,503: draw budget %u for 10 s (%u times)\n",
				static_cast<unsigned long long>(words), draws, ORBIS_WORDS_GUARD, ORBIS_GUARD_BUDGET, s_guards);
			fflush(stdout);
		}
	}
	if (++s_n >= 600)
	{
		if (s_peak_words >= 100000)
		{
			printf("[vkwords] largest of the last 600 submissions: %u words, %u draws (~%u a draw), of the driver's 523,503\n", s_peak_words,
				s_peak_draws, s_peak_draws ? s_peak_words / s_peak_draws : 0);
			fflush(stdout);
		}
		s_n = s_peak_words = s_peak_draws = 0;
	}
}
static u32 OrbisSubmitDrawBudgetRaw();
static u32 OrbisSubmitDrawBudget()
{
	const u32 b = OrbisSubmitDrawBudgetRaw();
	return (b > ORBIS_GUARD_BUDGET && s_orbis_guard_until_tsc != 0 && __builtin_ia32_rdtsc() < s_orbis_guard_until_tsc) ? ORBIS_GUARD_BUDGET : b;
}
static u32 OrbisSubmitDrawBudgetRaw()
{
	static u64 s_checked = 0;
	static u32 s_budget = OrbisDefaultDrawBudget();
	const u64 now = __builtin_ia32_rdtsc();
	if (now - s_checked >= ORBIS_TSC_SECOND)
	{
		s_checked = now;
		u32 budget = OrbisDefaultDrawBudget();
		std::string text;
		if (OrbisCachedRead(OrbisFlagPath("vk_drawbudget").c_str(), text))
		{
			const unsigned long v = std::strtoul(text.c_str(), nullptr, 10);
			if (v >= 1000 && v <= 8000) // live-10: 8000 (was 4800), with the words guard above
				budget = static_cast<u32>(v);
		}
		if (budget != s_budget)
		{
			Console.WriteLn("VK: submission draw budget %u (was %u)", budget, s_budget);
			s_budget = budget;
		}
	}
	return s_budget;
}
#define ORBIS_SUBMIT_DRAW_BUDGET (OrbisSubmitDrawBudget())

namespace
{
	// vk-285-39: the flag file vk_cputransfer (/data/PCSX2/flags) keeps every CopyRect the image
	// copy it was, as the driver's own flag of that name keeps its clears on the CPU. Checked at
	// most once a second.
	bool OrbisCopiesAsImageCopies()
	{
		static u64 s_checked = 0; // live-11: the TSC, not steady_clock (a kernel call per copy)
		static bool s_on = false;
		const u64 now = __builtin_ia32_rdtsc();
		if (now - s_checked >= ORBIS_TSC_SECOND)
		{
			s_checked = now;
			s_on = OrbisFlag("vk_cputransfer");
		}
		return s_on;
	}

	// vk-285-39: a copy the convert draw can make exactly: into a render target of the source's
	// Color format (COPY) or a depth buffer of its DepthStencil format (DEPTH_COPY), the two pairs
	// StretchRectAuto has a same-format shader for, from another texture.
	bool OrbisCopyByDraw(const GSTextureVK* src, const GSTextureVK* dst)
	{
		if (src == dst || src->GetFormat() != dst->GetFormat() || OrbisCopiesAsImageCopies())
			return false;
		switch (dst->GetFormat())
		{
			case GSTexture::Format::Color:
				return dst->IsRenderTarget();
			case GSTexture::Format::DepthStencil:
				return dst->IsDepthStencil();
			default:
				return false;
		}
	}

	// The [copies] line's kinds (VKOrbisTiming.h).
	int OrbisCopyKind(const GSTextureVK* src, const GSTextureVK* dst)
	{
		if (src == dst)
			return ORBIS_COPY_OTHER;
		if (src->IsRenderTarget())
			return dst->IsRenderTarget() ? ORBIS_COPY_RT_RT : (dst->IsDepthStencil() ? ORBIS_COPY_OTHER : ORBIS_COPY_RT_TEX);
		if (src->IsDepthStencil())
			return dst->IsDepthStencil() ? ORBIS_COPY_DS_DS : (dst->IsRenderTarget() ? ORBIS_COPY_OTHER : ORBIS_COPY_DS_TEX);
		return ORBIS_COPY_OTHER;
	}

	void OrbisCountCopy(int kind, const GSVector4i& r, const GSTextureVK* src)
	{
		const GSTexture::Format format = src->GetFormat();
		const u32 texel_bytes = (format == GSTexture::Format::ColorHDR || format == GSTexture::Format::ColorClip) ? 8u : 4u;
		g_orbis_copy_n[kind]++;
		g_orbis_copy_bytes[kind] += static_cast<unsigned long long>(r.width()) * static_cast<unsigned long long>(r.height()) * texel_bytes;
	}
} // namespace
#endif

// Tweakables
enum : u32
{
	MAX_DRAW_CALLS_PER_FRAME = 8192,
	MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME = 2 * MAX_DRAW_CALLS_PER_FRAME,
	MAX_SAMPLED_IMAGE_DESCRIPTORS_PER_FRAME =
		MAX_DRAW_CALLS_PER_FRAME, // assume at least half our draws aren't going to be shuffle/blending
	MAX_STORAGE_IMAGE_DESCRIPTORS_PER_FRAME = 4, // Currently used by CAS only
	MAX_INPUT_ATTACHMENT_IMAGE_DESCRIPTORS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME,
	MAX_DESCRIPTOR_SETS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME * 2,

	VERTEX_BUFFER_SIZE = 32 * 1024 * 1024,
	INDEX_BUFFER_SIZE = 16 * 1024 * 1024,
	VERTEX_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
	FRAGMENT_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
	TEXTURE_BUFFER_SIZE = 64 * 1024 * 1024,
};


#ifdef ENABLE_OGL_DEBUG
static u32 s_debug_scope_depth = 0;
#endif

static bool IsDATEModePrimIDInit(u32 flag)
{
	return flag == 1 || flag == 2;
}

static VkAttachmentLoadOp GetLoadOpForTexture(GSTextureVK* tex)
{
	if (!tex)
		return VK_ATTACHMENT_LOAD_OP_DONT_CARE;

	// clang-format off
	switch (tex->GetState())
	{
	case GSTextureVK::State::Cleared:       tex->SetState(GSTexture::State::Dirty); return VK_ATTACHMENT_LOAD_OP_CLEAR;
	case GSTextureVK::State::Invalidated:   tex->SetState(GSTexture::State::Dirty); return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	case GSTextureVK::State::Dirty:         return VK_ATTACHMENT_LOAD_OP_LOAD;
	default:                                return VK_ATTACHMENT_LOAD_OP_LOAD;
	}
	// clang-format on
}

static constexpr VkClearValue s_present_clear_color = {{{0.0f, 0.0f, 0.0f, 1.0f}}};

// We need to synchronize instance creation because of adapter enumeration from the UI thread.
static std::mutex s_instance_mutex;

// Device extensions that are required for PCSX2.
static constexpr const char* s_required_device_extensions[] = {
	VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
};

GSDeviceVK::GSDeviceVK()
{
#ifdef ENABLE_OGL_DEBUG
	s_debug_scope_depth = 0;
#endif

	std::memset(&m_pipeline_selector, 0, sizeof(m_pipeline_selector));
}

GSDeviceVK::~GSDeviceVK() = default;

VkInstance GSDeviceVK::CreateVulkanInstance(const WindowInfo& wi, OptionalExtensions* oe, bool enable_debug_utils,
	bool enable_validation_layer)
{
	ExtensionList enabled_extensions;
	if (!SelectInstanceExtensions(&enabled_extensions, wi, oe, enable_debug_utils))
		return VK_NULL_HANDLE;

	VkApplicationInfo app_info = {};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pNext = nullptr;
	app_info.pApplicationName = "PCSX2";
	app_info.applicationVersion = VK_MAKE_VERSION(
		BuildVersion::GitTagHi, BuildVersion::GitTagMid, BuildVersion::GitTagLo);
	app_info.pEngineName = "PCSX2";
	app_info.engineVersion = VK_MAKE_VERSION(
		BuildVersion::GitTagHi, BuildVersion::GitTagMid, BuildVersion::GitTagLo);
	app_info.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo instance_create_info = {};
	instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_create_info.pNext = nullptr;
	instance_create_info.flags = 0;
	instance_create_info.pApplicationInfo = &app_info;
	instance_create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
	instance_create_info.ppEnabledExtensionNames = enabled_extensions.data();
	instance_create_info.enabledLayerCount = 0;
	instance_create_info.ppEnabledLayerNames = nullptr;

	// Enable debug layer on debug builds
	if (enable_validation_layer)
	{
		static const char* layer_names[] = {"VK_LAYER_KHRONOS_validation"};
		instance_create_info.enabledLayerCount = 1;
		instance_create_info.ppEnabledLayerNames = layer_names;
	}

	VkInstance instance;
	VkResult res = vkCreateInstance(&instance_create_info, nullptr, &instance);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateInstance failed: ");
		return nullptr;
	}

	return instance;
}

bool GSDeviceVK::SelectInstanceExtensions(ExtensionList* extension_list, const WindowInfo& wi, OptionalExtensions* oe,
	bool enable_debug_utils)
{
	u32 extension_count = 0;
	VkResult res = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkEnumerateInstanceExtensionProperties failed: ");
		return false;
	}

	if (extension_count == 0)
	{
		Console.Error("VK: No extensions supported by instance.");
		return false;
	}

	std::vector<VkExtensionProperties> available_extension_list(extension_count);
	res = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available_extension_list.data());
	pxAssert(res == VK_SUCCESS);

	auto SupportsExtension = [&available_extension_list, extension_list](const char* name, bool required) {
		if (std::find_if(available_extension_list.begin(), available_extension_list.end(),
				[name](const VkExtensionProperties& properties) { return !strcmp(name, properties.extensionName); }) !=
			available_extension_list.end())
		{
			DevCon.WriteLn("VK: Enabling extension: %s", name);
			extension_list->push_back(name);
			return true;
		}

		if (required)
			Console.Error("VK: Missing required extension %s.", name);

		return false;
	};

	// Common extensions
	if (wi.type != WindowInfo::Type::Surfaceless && !SupportsExtension(VK_KHR_SURFACE_EXTENSION_NAME, true))
		return false;

#ifdef ORBIS_VULKAN
	// PS5: the display surface (VKSwapChain.cpp, CreateOrbisDisplaySurface).
	if (wi.type != WindowInfo::Type::Surfaceless && !SupportsExtension(VK_KHR_DISPLAY_EXTENSION_NAME, true))
		return false;
#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
	if (wi.type == WindowInfo::Type::Win32 && !SupportsExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME, true))
		return false;
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
	if (wi.type == WindowInfo::Type::X11 && !SupportsExtension(VK_KHR_XLIB_SURFACE_EXTENSION_NAME, true))
		return false;
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
	if (wi.type == WindowInfo::Type::Wayland && !SupportsExtension(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME, true))
		return false;
#endif
#if defined(VK_USE_PLATFORM_METAL_EXT)
	if (wi.type == WindowInfo::Type::MacOS && !SupportsExtension(VK_EXT_METAL_SURFACE_EXTENSION_NAME, true))
		return false;
#endif

	// VK_EXT_debug_utils
	if (enable_debug_utils && !SupportsExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, false))
		Console.Warning("VK: Debug report requested, but extension is not available.");

	oe->vk_swapchain_maintenance1 = wi.type != WindowInfo::Type::Surfaceless;
	if (wi.type != WindowInfo::Type::Surfaceless)
	{
		oe->vk_swapchain_maintenance1 = true;
		// VK_EXT_swapchain_maintenance1 requires VK_EXT_surface_maintenance1.
		// VK_KHR_swapchain_maintenance1 might require VK_KHR_surface_maintenance1 (It does on Nvidia).
		// If either VK_KHR_surface_maintenance1 is supported, or VK_EXT_swapchain_maintenance1 is unsupported, don't try VK_EXT_swapchain_maintenance1.
		oe->vk_swapchain_maintenance1_is_khr = SupportsExtension(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME, false) ||
			!SupportsExtension(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME, false);
	}
	else
		oe->vk_swapchain_maintenance1 = false;

	// Needed for exclusive fullscreen control.
	SupportsExtension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, false);

	return true;
}

GSDeviceVK::GPUList GSDeviceVK::EnumerateGPUs(VkInstance instance)
{
	GPUList gpus;

	u32 gpu_count = 0;
	VkResult res = vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
	if ((res != VK_SUCCESS && res != VK_INCOMPLETE) || gpu_count == 0)
	{
		LOG_VULKAN_ERROR(res, "vkEnumeratePhysicalDevices (1) failed: ");
		return gpus;
	}

	std::vector<VkPhysicalDevice> physical_devices(gpu_count);
	res = vkEnumeratePhysicalDevices(instance, &gpu_count, physical_devices.data());
	if (res == VK_INCOMPLETE)
	{
		Console.Warning("VK: First vkEnumeratePhysicalDevices() call returned %zu devices, but second returned %u",
			physical_devices.size(), gpu_count);
	}
	else if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkEnumeratePhysicalDevices (2) failed: ");
		return gpus;
	}

	// Maybe we lost a GPU?
	if (gpu_count < physical_devices.size())
		physical_devices.resize(gpu_count);

	gpus.reserve(physical_devices.size());
	for (VkPhysicalDevice device : physical_devices)
	{
		VkPhysicalDeviceProperties props = {};
		vkGetPhysicalDeviceProperties(device, &props);

		// Skip GPUs which don't support Vulkan 1.1, since we won't be able to create a device with them anyway.
		if (VK_API_VERSION_VARIANT(props.apiVersion) == 0 && VK_API_VERSION_MAJOR(props.apiVersion) <= 1 &&
			VK_API_VERSION_MINOR(props.apiVersion) < 1)
		{
			Console.Warning(fmt::format("VK: Ignoring GPU '{}' because it only claims support for Vulkan {}.{}.{}",
				props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
				VK_API_VERSION_PATCH(props.apiVersion)));
			continue;
		}

		// Query the extension list to ensure that we don't include GPUs that are missing the extensions we require.
		u32 extension_count = 0;
		res = vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr);
		if (res != VK_SUCCESS)
		{
			Console.Warning(fmt::format("VK: Ignoring GPU '{}' because vkEnumerateInstanceExtensionProperties() failed: ",
				props.deviceName, Vulkan::VkResultToString(res)));
			continue;
		}

		std::vector<VkExtensionProperties> available_extension_list(extension_count);
		if (extension_count > 0)
		{
			res = vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, available_extension_list.data());
			pxAssert(res == VK_SUCCESS);
		}
		bool has_missing_extension = false;
		for (const char* required_extension_name : s_required_device_extensions)
		{
			if (std::find_if(available_extension_list.begin(), available_extension_list.end(), [required_extension_name](const VkExtensionProperties& ext) {
					return (std::strcmp(required_extension_name, ext.extensionName) == 0);
				}) == available_extension_list.end())
			{
				Console.Warning(fmt::format("VK: Ignoring GPU '{}' because is is missing required extension {}",
					props.deviceName, required_extension_name));
				has_missing_extension = true;
			}
		}
		if (has_missing_extension)
			continue;

		GSAdapterInfo ai;
		ai.name = props.deviceName;
		ai.max_texture_size = std::min(props.limits.maxFramebufferWidth, props.limits.maxImageDimension2D);
		ai.max_upscale_multiplier = GSGetMaxUpscaleMultiplier(ai.max_texture_size);

		// handle duplicate adapter names
		if (std::any_of(
				gpus.begin(), gpus.end(), [&ai](const auto& other) { return (ai.name == other.second.name); }))
		{
			std::string original_adapter_name = std::move(ai.name);

			u32 current_extra = 2;
			do
			{
				ai.name = fmt::format("{} ({})", original_adapter_name, current_extra);
				current_extra++;
			} while (std::any_of(
				gpus.begin(), gpus.end(), [&ai](const auto& other) { return (ai.name == other.second.name); }));
		}

		gpus.emplace_back(device, std::move(ai));
	}

	return gpus;
}

GSDeviceVK::GPUList GSDeviceVK::EnumerateGPUs()
{
	std::unique_lock lock(s_instance_mutex);

	// Device shouldn't be torn down since we have the lock.
	GPUList gpus;
	if (g_gs_device && Vulkan::IsVulkanLibraryLoaded())
	{
		gpus = EnumerateGPUs(GSDeviceVK::GetInstance()->GetVulkanInstance());
	}
	else
	{
		if (Vulkan::LoadVulkanLibrary(nullptr))
		{
			OptionalExtensions oe = {};
			const VkInstance instance = CreateVulkanInstance(WindowInfo(), &oe, false, false);
			if (instance != VK_NULL_HANDLE)
			{
				if (Vulkan::LoadVulkanInstanceFunctions(instance))
					gpus = EnumerateGPUs(instance);

				vkDestroyInstance(instance, nullptr);
			}

			Vulkan::UnloadVulkanLibrary();
		}
	}

	return gpus;
}

bool GSDeviceVK::SelectDeviceExtensions(ExtensionList* extension_list, bool enable_surface)
{
	u32 extension_count = 0;
	VkResult res = vkEnumerateDeviceExtensionProperties(m_physical_device, nullptr, &extension_count, nullptr);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkEnumerateDeviceExtensionProperties failed: ");
		return false;
	}

	if (extension_count == 0)
	{
		Console.Error("VK: No extensions supported by device.");
		return false;
	}

	std::vector<VkExtensionProperties> available_extension_list(extension_count);
	res = vkEnumerateDeviceExtensionProperties(
		m_physical_device, nullptr, &extension_count, available_extension_list.data());
	pxAssert(res == VK_SUCCESS);

	auto SupportsExtension = [&available_extension_list, extension_list](const char* name, bool required) {
		if (std::find_if(available_extension_list.begin(), available_extension_list.end(),
				[name](const VkExtensionProperties& properties) { return !strcmp(name, properties.extensionName); }) !=
			available_extension_list.end())
		{
			if (std::none_of(extension_list->begin(), extension_list->end(),
					[name](const char* existing_name) { return (std::strcmp(existing_name, name) == 0); }))
			{
				DevCon.WriteLn("VK: Enabling extension: %s", name);
				extension_list->push_back(name);
			}

			return true;
		}

		if (required)
			Console.Error("VK: Missing required extension %s.", name);

		return false;
	};

	if (enable_surface && !SupportsExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME, true))
		return false;

	// Required extensions.
	for (const char* extension_name : s_required_device_extensions)
	{
		if (!SupportsExtension(extension_name, true))
			return false;
	}

	m_optional_extensions.vk_ext_provoking_vertex = SupportsExtension(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, false);
	m_optional_extensions.vk_ext_memory_budget = SupportsExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, false);
	m_optional_extensions.vk_ext_calibrated_timestamps =
		SupportsExtension(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, false);
	m_optional_extensions.vk_ext_rasterization_order_attachment_access =
		SupportsExtension(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, false);
	m_optional_extensions.vk_ext_attachment_feedback_loop_layout =
		SupportsExtension(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME, false);
	m_optional_extensions.vk_ext_line_rasterization = SupportsExtension(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME, false);
	m_optional_extensions.vk_khr_driver_properties = SupportsExtension(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME, false);

	if (m_optional_extensions.vk_swapchain_maintenance1)
	{
		const bool khr_swapchain_maintenance1 = SupportsExtension(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, false);
		// vk_swapchain_maintenance1_is_khr will be set if we havn't enabled VK_EXT_surface_maintenance1
		// This will happen if either the VK_EXT_surface_maintenance1 was unsupported, or we instead found the KHR version.
		// As the EXT version depends on the surface maintenance1 extension, we need to check that aswell.
		m_optional_extensions.vk_swapchain_maintenance1 = khr_swapchain_maintenance1 ? khr_swapchain_maintenance1 :
			(!m_optional_extensions.vk_swapchain_maintenance1_is_khr && SupportsExtension(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, false));

		m_optional_extensions.vk_swapchain_maintenance1_is_khr = khr_swapchain_maintenance1;
	}

	// glslang generates debug info instructions before phi nodes at the beginning of blocks when non-semantic debug info
	// is enabled, triggering errors by spirv-val. Gate it by an environment variable if you want source debugging until
	// this is fixed.
	if (const char* val = std::getenv("USE_NON_SEMANTIC_DEBUG_INFO"); val && StringUtil::FromChars<bool>(val).value_or(false))
	{
		m_optional_extensions.vk_khr_shader_non_semantic_info =
			SupportsExtension(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME, false);
	}

#ifdef _WIN32
	m_optional_extensions.vk_ext_full_screen_exclusive =
		enable_surface && SupportsExtension(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME, false);
#endif

	m_optional_extensions.vk_ext_fragment_shader_interlock = SupportsExtension(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, false);

	return true;
}

bool GSDeviceVK::SelectDeviceFeatures()
{
	VkPhysicalDeviceFeatures available_features;
	vkGetPhysicalDeviceFeatures(m_physical_device, &available_features);

	// Enable the features we use.
	m_device_features.dualSrcBlend = available_features.dualSrcBlend;
	m_device_features.largePoints = available_features.largePoints;
	m_device_features.wideLines = available_features.wideLines;
	m_device_features.fragmentStoresAndAtomics = available_features.fragmentStoresAndAtomics;
	m_device_features.textureCompressionBC = available_features.textureCompressionBC;
	m_device_features.geometryShader = available_features.geometryShader;
	m_device_features.fragmentStoresAndAtomics = available_features.fragmentStoresAndAtomics;
	m_device_features.pipelineStatisticsQuery = available_features.pipelineStatisticsQuery;

	return true;
}

bool GSDeviceVK::CreateDevice(VkSurfaceKHR surface, bool enable_validation_layer)
{
	u32 queue_family_count;
	vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &queue_family_count, nullptr);
	if (queue_family_count == 0)
	{
		Console.Error("No queue families found on specified vulkan physical device.");
		return false;
	}

	std::vector<VkQueueFamilyProperties> queue_family_properties(queue_family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &queue_family_count, queue_family_properties.data());
	DevCon.WriteLn("%u vulkan queue families", queue_family_count);

	// Find graphics and present queues.
	m_graphics_queue_family_index = queue_family_count;
	m_present_queue_family_index = queue_family_count;
	m_spin_queue_family_index = queue_family_count;
	u32 spin_queue_index = 0;
	for (uint32_t i = 0; i < queue_family_count; i++)
	{
		VkBool32 graphics_supported = queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
		if (graphics_supported)
		{
			m_graphics_queue_family_index = i;
			// Quit now, no need for a present queue.
			if (!surface)
			{
				break;
			}
		}

		if (surface)
		{
			VkBool32 present_supported;
			VkResult res = vkGetPhysicalDeviceSurfaceSupportKHR(m_physical_device, i, surface, &present_supported);
			if (res != VK_SUCCESS)
			{
				LOG_VULKAN_ERROR(res, "vkGetPhysicalDeviceSurfaceSupportKHR failed: ");
				return false;
			}

			if (present_supported)
			{
				m_present_queue_family_index = i;
			}

			// Prefer one queue family index that does both graphics and present.
			if (graphics_supported && present_supported)
			{
				break;
			}
		}
	}
	for (uint32_t i = 0; i < queue_family_count; i++)
	{
		// Pick a queue for spinning
		if (!(queue_family_properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
			continue; // We need compute
		if (queue_family_properties[i].timestampValidBits == 0)
			continue; // We need timing
		const bool queue_is_used = i == m_graphics_queue_family_index || i == m_present_queue_family_index;
		if (queue_is_used && m_spin_queue_family_index != queue_family_count)
			continue; // Found a non-graphics queue to use
		spin_queue_index = 0;
		m_spin_queue_family_index = i;
		if (queue_is_used && queue_family_properties[i].queueCount > 1)
			spin_queue_index = 1;
		if (!(queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			break; // Async compute queue, definitely pick this one
	}
	if (m_graphics_queue_family_index == queue_family_count)
	{
		Console.Error("VK: Failed to find an acceptable graphics queue.");
		return false;
	}
	if (surface != VK_NULL_HANDLE && m_present_queue_family_index == queue_family_count)
	{
		Console.Error("VK: Failed to find an acceptable present queue.");
		return false;
	}

	VkDeviceCreateInfo device_info = {};
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.pNext = nullptr;
	device_info.flags = 0;
	device_info.queueCreateInfoCount = 0;

	static constexpr float queue_priorities[] = {1.0f, 0.0f}; // Low priority for the spin queue
	std::array<VkDeviceQueueCreateInfo, 3> queue_infos;
	VkDeviceQueueCreateInfo& graphics_queue_info = queue_infos[device_info.queueCreateInfoCount++];
	graphics_queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	graphics_queue_info.pNext = nullptr;
	graphics_queue_info.flags = 0;
	graphics_queue_info.queueFamilyIndex = m_graphics_queue_family_index;
	graphics_queue_info.queueCount = 1;
	graphics_queue_info.pQueuePriorities = queue_priorities;

	if (surface != VK_NULL_HANDLE && m_graphics_queue_family_index != m_present_queue_family_index)
	{
		VkDeviceQueueCreateInfo& present_queue_info = queue_infos[device_info.queueCreateInfoCount++];
		present_queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		present_queue_info.pNext = nullptr;
		present_queue_info.flags = 0;
		present_queue_info.queueFamilyIndex = m_present_queue_family_index;
		present_queue_info.queueCount = 1;
		present_queue_info.pQueuePriorities = queue_priorities;
	}

	if (m_spin_queue_family_index == m_graphics_queue_family_index)
	{
		if (spin_queue_index != 0)
			graphics_queue_info.queueCount = 2;
	}
	else if (m_spin_queue_family_index == m_present_queue_family_index)
	{
		if (spin_queue_index != 0)
			queue_infos[1].queueCount = 2; // present queue
	}
	else if (m_spin_queue_family_index != queue_family_count)
	{
		VkDeviceQueueCreateInfo& spin_queue_info = queue_infos[device_info.queueCreateInfoCount++];
		spin_queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		spin_queue_info.pNext = nullptr;
		spin_queue_info.flags = 0;
		spin_queue_info.queueFamilyIndex = m_spin_queue_family_index;
		spin_queue_info.queueCount = 1;
		spin_queue_info.pQueuePriorities = queue_priorities + 1;
	}

	device_info.pQueueCreateInfos = queue_infos.data();

	ExtensionList enabled_extensions;
	if (!SelectDeviceExtensions(&enabled_extensions, surface != VK_NULL_HANDLE))
		return false;

	device_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
	device_info.ppEnabledExtensionNames = enabled_extensions.data();

	// Check for required features before creating.
	if (!SelectDeviceFeatures())
		return false;

	device_info.pEnabledFeatures = &m_device_features;

	// Enable debug layer on debug builds
	if (enable_validation_layer)
	{
		static const char* layer_names[] = {"VK_LAYER_LUNARG_standard_validation"};
		device_info.enabledLayerCount = 1;
		device_info.ppEnabledLayerNames = layer_names;
	}

	// provoking vertex
	VkPhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
	VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
	VkPhysicalDeviceLineRasterizationFeaturesEXT line_rasterization_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};
	VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT attachment_feedback_loop_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
	// VK_EXT_swapchain_maintenance1 types/enums are aliases of VK_KHR_swapchain_maintenance1 types/enums.
	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchain_maintenance1_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR};
	VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fragment_shader_interlock_ext_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};

	if (m_optional_extensions.vk_ext_provoking_vertex)
	{
		provoking_vertex_feature.provokingVertexLast = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &provoking_vertex_feature);
	}
	if (m_optional_extensions.vk_ext_line_rasterization)
	{
		line_rasterization_feature.bresenhamLines = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &line_rasterization_feature);
	}
	if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
	{
		rasterization_order_access_feature.rasterizationOrderColorAttachmentAccess = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &rasterization_order_access_feature);
	}
	if (m_optional_extensions.vk_ext_attachment_feedback_loop_layout)
	{
		attachment_feedback_loop_feature.attachmentFeedbackLoopLayout = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &attachment_feedback_loop_feature);
	}
	if (m_optional_extensions.vk_swapchain_maintenance1)
	{
		swapchain_maintenance1_feature.swapchainMaintenance1 = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &swapchain_maintenance1_feature);
	}
	if (m_optional_extensions.vk_ext_fragment_shader_interlock)
	{
		fragment_shader_interlock_ext_feature.fragmentShaderPixelInterlock = VK_TRUE;
		Vulkan::AddPointerToChain(&device_info, &fragment_shader_interlock_ext_feature);
	}

	VkResult res = vkCreateDevice(m_physical_device, &device_info, nullptr, &m_device);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateDevice failed: ");
		return false;
	}

	// With the device created, we can fill the remaining entry points.
	if (!Vulkan::LoadVulkanDeviceFunctions(m_device))
		return false;

	// Grab the graphics and present queues.
	vkGetDeviceQueue(m_device, m_graphics_queue_family_index, 0, &m_graphics_queue);
	if (surface)
	{
		vkGetDeviceQueue(m_device, m_present_queue_family_index, 0, &m_present_queue);
	}
	m_spinning_supported = m_spin_queue_family_index != queue_family_count &&
	                       queue_family_properties[m_graphics_queue_family_index].timestampValidBits > 0 &&
	                       m_device_properties.limits.timestampPeriod > 0;
	m_spin_queue_is_graphics_queue =
		m_spin_queue_family_index == m_graphics_queue_family_index && spin_queue_index == 0;
#ifdef ORBIS_VULKAN
	// vk-285-115 (AI-assisted): no GPU spinning on RADV. It exposes a compute family with timestamps and calibrated timestamps, which
	// turns the spin machinery on, and then every present asks for a new calibration (nothing clears the flag) and every waited submit
	// (each readback) makes one; on the PS5 each vkGetCalibratedTimestampsEXT is a GPU submission behind the queued work. The port
	// never spins (HWSpinGPUForReadbacks isn't offered), and ps5vk never had it. Needs testing on the console.
	if (OrbisDriverIsRADV())
		m_spinning_supported = false;
#endif

	m_gpu_timing_supported = (m_device_properties.limits.timestampComputeAndGraphics != 0 &&
							  queue_family_properties[m_graphics_queue_family_index].timestampValidBits > 0 &&
							  m_device_properties.limits.timestampPeriod > 0);
	DevCon.WriteLn("GPU timing is %s (TS=%u TS valid bits=%u, TS period=%f)",
		m_gpu_timing_supported ? "supported" : "not supported",
		static_cast<u32>(m_device_properties.limits.timestampComputeAndGraphics),
		queue_family_properties[m_graphics_queue_family_index].timestampValidBits,
		m_device_properties.limits.timestampPeriod);

	m_gpu_pipeline_statistics_supported = (m_device_features.pipelineStatisticsQuery != 0);
	DevCon.WriteLn("GPU pipeline statistics is %s", m_gpu_pipeline_statistics_supported ? "supported" : "not supported");

	if (!ProcessDeviceExtensions())
		return false;

	if (m_spinning_supported)
	{
		vkGetDeviceQueue(m_device, m_spin_queue_family_index, spin_queue_index, &m_spin_queue);

		m_spin_timestamp_scale = m_device_properties.limits.timestampPeriod;
		if (m_optional_extensions.vk_ext_calibrated_timestamps)
		{
#ifdef _WIN32
			LARGE_INTEGER Freq;
			QueryPerformanceFrequency(&Freq);
			m_queryperfcounter_to_ns = 1000000000.0 / static_cast<double>(Freq.QuadPart);
#endif
			CalibrateSpinTimestamp();
		}
	}

	return true;
}

bool GSDeviceVK::ProcessDeviceExtensions()
{
	// advanced feature checks
	VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	VkPhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex_features = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
	VkPhysicalDeviceLineRasterizationFeaturesEXT line_rasterization_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};
	VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
	// VK_EXT_swapchain_maintenance1 types/enums are aliases of VK_KHR_swapchain_maintenance1 types/enums.
	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchain_maintenance1_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR, nullptr, VK_TRUE};
	VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT attachment_feedback_loop_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
	VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fragment_shader_interlock_ext_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT };

	// add in optional feature structs
	if (m_optional_extensions.vk_ext_provoking_vertex)
		Vulkan::AddPointerToChain(&features2, &provoking_vertex_features);
	if (m_optional_extensions.vk_ext_line_rasterization)
		Vulkan::AddPointerToChain(&features2, &line_rasterization_feature);
	if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
		Vulkan::AddPointerToChain(&features2, &rasterization_order_access_feature);
	if (m_optional_extensions.vk_ext_attachment_feedback_loop_layout)
		Vulkan::AddPointerToChain(&features2, &attachment_feedback_loop_feature);
	if (m_optional_extensions.vk_swapchain_maintenance1)
		Vulkan::AddPointerToChain(&features2, &swapchain_maintenance1_feature);
	if (m_optional_extensions.vk_ext_fragment_shader_interlock)
		Vulkan::AddPointerToChain(&features2, &fragment_shader_interlock_ext_feature);

	// query
	vkGetPhysicalDeviceFeatures2(m_physical_device, &features2);

	// confirm we actually support it
	m_optional_extensions.vk_ext_provoking_vertex &= (provoking_vertex_features.provokingVertexLast == VK_TRUE);
	m_optional_extensions.vk_ext_rasterization_order_attachment_access &=
		(rasterization_order_access_feature.rasterizationOrderColorAttachmentAccess == VK_TRUE);
	m_optional_extensions.vk_ext_attachment_feedback_loop_layout &=
		(attachment_feedback_loop_feature.attachmentFeedbackLoopLayout == VK_TRUE);

	VkPhysicalDeviceProperties2 properties2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

	if (m_optional_extensions.vk_khr_driver_properties)
	{
		m_device_driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
		Vulkan::AddPointerToChain(&properties2, &m_device_driver_properties);
	}

	VkPhysicalDevicePushDescriptorPropertiesKHR push_descriptor_properties = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
	Vulkan::AddPointerToChain(&properties2, &push_descriptor_properties);

	// query
	vkGetPhysicalDeviceProperties2(m_physical_device, &properties2);

	// confirm we actually support it
	if (push_descriptor_properties.maxPushDescriptors < NUM_TFX_TEXTURES)
	{
		Console.Error("VK: maxPushDescriptors (%u) is below required (%u)", push_descriptor_properties.maxPushDescriptors,
			NUM_TFX_TEXTURES);
		return false;
	}

	if (m_optional_extensions.vk_ext_line_rasterization && !line_rasterization_feature.bresenhamLines)
	{
		Console.Warning("VK: bresenhamLines is not supported.");
		m_optional_extensions.vk_ext_line_rasterization = false;
	}

	// VK_EXT_calibrated_timestamps checking
	if (m_optional_extensions.vk_ext_calibrated_timestamps)
	{
		u32 count = 0;
		vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(m_physical_device, &count, nullptr);
		std::unique_ptr<VkTimeDomainEXT[]> time_domains = std::make_unique<VkTimeDomainEXT[]>(count);
		vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(m_physical_device, &count, time_domains.get());
		const VkTimeDomainEXT* begin = &time_domains[0];
		const VkTimeDomainEXT* end = &time_domains[count];
		if (std::find(begin, end, VK_TIME_DOMAIN_DEVICE_EXT) == end)
			m_optional_extensions.vk_ext_calibrated_timestamps = false;
		VkTimeDomainEXT preferred_types[] = {
#ifdef _WIN32
			VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT,
#else
#ifdef CLOCK_MONOTONIC_RAW
			VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT,
#endif
			VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT,
#endif
		};
		m_calibrated_timestamp_type = VK_TIME_DOMAIN_DEVICE_EXT;
		for (VkTimeDomainEXT type : preferred_types)
		{
			if (std::find(begin, end, type) != end)
			{
				m_calibrated_timestamp_type = type;
				break;
			}
		}
		if (m_calibrated_timestamp_type == VK_TIME_DOMAIN_DEVICE_EXT)
			m_optional_extensions.vk_ext_calibrated_timestamps = false;
	}

	m_optional_extensions.vk_swapchain_maintenance1 &= 
		(swapchain_maintenance1_feature.swapchainMaintenance1 == VK_TRUE);

	m_optional_extensions.vk_ext_fragment_shader_interlock &=
		(fragment_shader_interlock_ext_feature.fragmentShaderPixelInterlock == VK_TRUE);

	Console.WriteLn(
		"VK_EXT_provoking_vertex is %s", m_optional_extensions.vk_ext_provoking_vertex ? "supported" : "NOT supported");
	Console.WriteLn(
		"VK_EXT_memory_budget is %s", m_optional_extensions.vk_ext_memory_budget ? "supported" : "NOT supported");
	Console.WriteLn("VK_EXT_calibrated_timestamps is %s",
		m_optional_extensions.vk_ext_calibrated_timestamps ? "supported" : "NOT supported");
	Console.WriteLn("VK_EXT_rasterization_order_attachment_access is %s",
		m_optional_extensions.vk_ext_rasterization_order_attachment_access ? "supported" : "NOT supported");
	Console.WriteLn("VK_%s_swapchain_maintenance1 is %s",
		m_optional_extensions.vk_swapchain_maintenance1_is_khr ? "KHR" : "EXT",
		m_optional_extensions.vk_swapchain_maintenance1 ? "supported" : "NOT supported");
	Console.WriteLn("VK_EXT_full_screen_exclusive is %s",
		m_optional_extensions.vk_ext_full_screen_exclusive ? "supported" : "NOT supported");
	Console.WriteLn("VK_KHR_driver_properties is %s",
		m_optional_extensions.vk_khr_driver_properties ? "supported" : "NOT supported");
	Console.WriteLn("VK_EXT_attachment_feedback_loop_layout is %s",
		m_optional_extensions.vk_ext_attachment_feedback_loop_layout ? "supported" : "NOT supported");
	Console.WriteLn("VK_EXT_fragment_shader_interlock is %s",
		m_optional_extensions.vk_ext_fragment_shader_interlock ? "supported" : "NOT supported");

	return true;
}

bool GSDeviceVK::CreateAllocator()
{
	VmaAllocatorCreateInfo ci = {};
	ci.vulkanApiVersion = VK_API_VERSION_1_1;
	ci.flags = VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
	ci.physicalDevice = m_physical_device;
	ci.device = m_device;
	ci.instance = m_instance;

	if (m_optional_extensions.vk_ext_memory_budget)
		ci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;

	// Limit usage of the DEVICE_LOCAL upload heap when we're using a debug device.
	// On NVIDIA drivers, it results in frequently running out of device memory when trying to
	// play back captures in RenderDoc, making life very painful. Re-BAR GPUs should be fine.
	constexpr VkDeviceSize UPLOAD_HEAP_SIZE_THRESHOLD = 512 * 1024 * 1024;
	constexpr VkMemoryPropertyFlags UPLOAD_HEAP_PROPERTIES =
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> heap_size_limits;
	if (GSConfig.UseDebugDevice)
	{
		VkPhysicalDeviceMemoryProperties memory_properties;
		vkGetPhysicalDeviceMemoryProperties(m_physical_device, &memory_properties);

		bool has_upload_heap = false;
		heap_size_limits.fill(VK_WHOLE_SIZE);
		for (u32 i = 0; i < memory_properties.memoryTypeCount; i++)
		{
			// Look for any memory types which are upload-like.
			const VkMemoryType& type = memory_properties.memoryTypes[i];
			if ((type.propertyFlags & UPLOAD_HEAP_PROPERTIES) != UPLOAD_HEAP_PROPERTIES)
				continue;

			const VkMemoryHeap& heap = memory_properties.memoryHeaps[type.heapIndex];
			if (heap.size >= UPLOAD_HEAP_SIZE_THRESHOLD)
				continue;

			if (heap_size_limits[type.heapIndex] == VK_WHOLE_SIZE)
			{
				Console.Warning("VK: Disabling allocation from upload heap #%u (%.2f MB) due to debug device.",
					type.heapIndex, static_cast<float>(heap.size) / 1048576.0f);
				heap_size_limits[type.heapIndex] = 0;
				has_upload_heap = true;
			}
		}

		if (has_upload_heap)
			ci.pHeapSizeLimit = heap_size_limits.data();
	}

	VkResult res = vmaCreateAllocator(&ci, &m_allocator);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vmaCreateAllocator failed: ");
		return false;
	}

	return true;
}

bool GSDeviceVK::CreateCommandBuffers()
{
	VkResult res;

	uint32_t frame_index = 0;
	for (FrameResources& resources : m_frame_resources)
	{
		resources.needs_fence_wait = false;

		VkCommandPoolCreateInfo pool_info = {
			VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, m_graphics_queue_family_index};
		res = vkCreateCommandPool(m_device, &pool_info, nullptr, &resources.command_pool);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkCreateCommandPool failed: ");
			return false;
		}
		Vulkan::SetObjectName(m_device, resources.command_pool, "Frame Command Pool %u", frame_index);

		VkCommandBufferAllocateInfo buffer_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
			resources.command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			static_cast<u32>(resources.command_buffers.size())};

		res = vkAllocateCommandBuffers(m_device, &buffer_info, resources.command_buffers.data());
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkAllocateCommandBuffers failed: ");
			return false;
		}
		for (u32 i = 0; i < resources.command_buffers.size(); i++)
		{
			Vulkan::SetObjectName(m_device, resources.command_buffers[i], "Frame %u %sCommand Buffer", frame_index,
				(i == 0) ? "Init" : "");
		}

		VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};

		res = vkCreateFence(m_device, &fence_info, nullptr, &resources.fence);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkCreateFence failed: ");
			return false;
		}
		Vulkan::SetObjectName(m_device, resources.fence, "Frame Fence %u", frame_index);
		++frame_index;
	}

	ActivateCommandBuffer(0);
	return true;
}

bool GSDeviceVK::CreateGlobalDescriptorPool()
{
	static constexpr const VkDescriptorPoolSize pool_sizes[] = {
		{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2},
		{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
	};

	VkDescriptorPoolCreateInfo pool_create_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr,
		VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
		1024, // TODO: tweak this
		static_cast<u32>(std::size(pool_sizes)), pool_sizes};

	VkResult res = vkCreateDescriptorPool(m_device, &pool_create_info, nullptr, &m_global_descriptor_pool);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateDescriptorPool failed: ");
		return false;
	}
	Vulkan::SetObjectName(m_device, m_global_descriptor_pool, "Global Descriptor Pool");

	if (m_gpu_timing_supported)
	{
		const VkQueryPoolCreateInfo query_create_info = {
			VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, NUM_COMMAND_BUFFERS * 4, 0};
		res = vkCreateQueryPool(m_device, &query_create_info, nullptr, &m_timestamp_query_pool);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkCreateQueryPool failed: ");
			m_gpu_timing_supported = false;
			return false;
		}
	}

	if (m_gpu_pipeline_statistics_supported)
	{
		const VkQueryPoolCreateInfo query_create_info = {
			VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_PIPELINE_STATISTICS, NUM_COMMAND_BUFFERS,
			VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT };
		res = vkCreateQueryPool(m_device, &query_create_info, nullptr, &m_pipeline_statistics_query_pool);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkCreateQueryPool failed: ");
			m_gpu_pipeline_statistics_supported = false;
			return false;
		}
	}

	return true;
}

VkRenderPass GSDeviceVK::GetRenderPass(VkFormat color_format, VkFormat depth_format, VkAttachmentLoadOp color_load_op,
	VkAttachmentStoreOp color_store_op, VkAttachmentLoadOp depth_load_op, VkAttachmentStoreOp depth_store_op,
	VkAttachmentLoadOp stencil_load_op, VkAttachmentStoreOp stencil_store_op, bool color_feedback_loop,
	bool depth_sampling)
{
	RenderPassCacheKey key = {};
	key.color_format = color_format;
	key.depth_format = depth_format;
	key.color_load_op = color_load_op;
	key.color_store_op = color_store_op;
	key.depth_load_op = depth_load_op;
	key.depth_store_op = depth_store_op;
	key.stencil_load_op = stencil_load_op;
	key.stencil_store_op = stencil_store_op;
	key.color_feedback_loop = color_feedback_loop;
	key.depth_sampling = depth_sampling;

	auto it = m_render_pass_cache.find(key.key);
	if (it != m_render_pass_cache.end())
		return it->second;

	return CreateCachedRenderPass(key);
}

VkRenderPass GSDeviceVK::GetRenderPassForRestarting(VkRenderPass pass)
{
	for (const auto& it : m_render_pass_cache)
	{
		if (it.second != pass)
			continue;

		RenderPassCacheKey modified_key;
		modified_key.key = it.first;
		if (modified_key.color_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			modified_key.color_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
		if (modified_key.depth_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			modified_key.depth_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
		if (modified_key.stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			modified_key.stencil_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;

		if (modified_key.key == it.first)
			return pass;

		auto fit = m_render_pass_cache.find(modified_key.key);
		if (fit != m_render_pass_cache.end())
			return fit->second;

		return CreateCachedRenderPass(modified_key);
	}

	return pass;
}

VkCommandBuffer GSDeviceVK::GetCurrentInitCommandBuffer()
{
	FrameResources& res = m_frame_resources[m_current_frame];
	VkCommandBuffer buf = res.command_buffers[0];
	if (res.init_buffer_used)
		return buf;

	VkCommandBufferBeginInfo bi{
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
	vkBeginCommandBuffer(buf, &bi);
	res.init_buffer_used = true;
	return buf;
}

VkDescriptorSet GSDeviceVK::AllocatePersistentDescriptorSet(VkDescriptorSetLayout set_layout)
{
	VkDescriptorSetAllocateInfo allocate_info = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, m_global_descriptor_pool, 1, &set_layout};

	VkDescriptorSet descriptor_set;
	VkResult res = vkAllocateDescriptorSets(m_device, &allocate_info, &descriptor_set);
	if (res != VK_SUCCESS)
		return VK_NULL_HANDLE;

	return descriptor_set;
}

void GSDeviceVK::FreePersistentDescriptorSet(VkDescriptorSet set)
{
	vkFreeDescriptorSets(m_device, m_global_descriptor_pool, 1, &set);
}

void GSDeviceVK::WaitForFenceCounter(u64 fence_counter)
{
	if (m_completed_fence_counter >= fence_counter)
		return;

	// Find the first command buffer which covers this counter value.
	u32 index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
	while (index != m_current_frame)
	{
		if (m_frame_resources[index].fence_counter >= fence_counter)
			break;

		index = (index + 1) % NUM_COMMAND_BUFFERS;
	}

	pxAssert(index != m_current_frame);
	ORBIS_VKW(2);
	WaitForCommandBufferCompletion(index);
}

void GSDeviceVK::WaitForGPUIdle()
{
	ORBIS_VKW(6);
	vkDeviceWaitIdle(m_device);
}

float GSDeviceVK::GetAndResetAccumulatedGPUTime()
{
	const float time = m_accumulated_gpu_time;
	m_accumulated_gpu_time = 0.0f;
	return time;
}

// vk-285-87: the Vulkan device, for the port's CPU pinning (GSRenderer.cpp, OrbisApplyLayout), which places
// the driver's own threads with the emulator's.
void* OrbisVkDeviceHandle()
{
	if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::Vulkan)
		return nullptr;
	return GSDeviceVK::GetInstance()->GetDevice();
}

bool GSDeviceVK::SetGPUTimingEnabled(bool enabled)
{
	m_gpu_timing_enabled = enabled && m_gpu_timing_supported;
	return (enabled == m_gpu_timing_enabled);
}

GPUPipelineStatistics GSDeviceVK::GetAndResetAccumulatedGPUPipelineStatistics()
{
	GPUPipelineStatistics stats = m_accumulated_gpu_pipeline_statistics;
	m_accumulated_gpu_pipeline_statistics = {};
	return stats;
}

bool GSDeviceVK::SetGPUPipelineStatisticsEnabled(bool enabled)
{
	m_gpu_pipeline_statistics_enabled = enabled && m_gpu_pipeline_statistics_supported;
	return (enabled == m_gpu_pipeline_statistics_enabled);
}

void GSDeviceVK::ScanForCommandBufferCompletion()
{
	for (u32 check_index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS; check_index != m_current_frame;
	     check_index = (check_index + 1) % NUM_COMMAND_BUFFERS)
	{
		FrameResources& resources = m_frame_resources[check_index];
		if (resources.fence_counter <= m_completed_fence_counter)
			continue; // Already completed
		if (vkGetFenceStatus(m_device, resources.fence) != VK_SUCCESS)
			break; // Fence not signaled, later fences won't be either
		CommandBufferCompleted(check_index);
		m_completed_fence_counter = resources.fence_counter;
	}
	for (SpinResources& resources : m_spin_resources)
	{
		if (!resources.in_progress)
			continue;
		if (vkGetFenceStatus(m_device, resources.fence) != VK_SUCCESS)
			continue;
		SpinCommandCompleted(&resources - &m_spin_resources[0]);
	}
}

#ifdef ORBIS_VULKAN
// vk-285-14 (HW renderer bring-up): GPU-hang forensics. The PS5 driver's queue submit waits
// for the GPU, so a hang surfaces as VK_ERROR_DEVICE_LOST in the vkQueueSubmit of the command
// buffer that hung. The last draws, copies, clears, stretch draws, uploads and readbacks are
// kept in a ring; on a lost device they are written to /data/PCSX2/vkhang.txt and the app
// closes at once, instead of reopening the device and giving the hung GPU more work (vk-285-13
// did that, and the console restarted). The driver writes its own dump of the hung step
// (PS5VK_HANG_DUMP, main-boot).
namespace
{
	struct OrbisVkTraceEntry
	{
		u64 seq;
		u32 submit;
		u8 kind; // 1 draw, 2 copy, 3 clear, 4 stretch, 5 multi-stretch, 6 readback, 7 upload
		u8 topology, vs, depth, colormask, date, sampler, flags;
		u32 blend;
		u64 ps_lo, ps_hi;
		u32 nverts, nindices;
		u32 groups; // vk-285-113: drawlist groups of a full-barrier draw (one barrier + one driver draw each), 0 otherwise
		const void* a;
		const void* b;
		const void* c;
		const void* d;
		u16 aw, ah, bw, bh, cw, ch;
		u8 af, bf, cf, df;
		s32 r[4];
		u64 extra;
	};
	constexpr u32 ORBIS_VK_TRACE_N = 16384; // a frame of R&C draws more than 2,000 times, plus a pipe entry per pass
	// vk-285-113 (AI-assisted): without flags/vkring the LAST 512 entries (~60 KB, they stay in the cache) are kept anyway, so every
	// hang dump lists the draws before it. the six distinct dumps in the vk-285-108..112 reports all said "the draw ring was off".
	constexpr u32 ORBIS_VK_TRACE_SMALL_N = 512;
	static_assert((ORBIS_VK_TRACE_N & (ORBIS_VK_TRACE_N - 1)) == 0 && (ORBIS_VK_TRACE_SMALL_N & (ORBIS_VK_TRACE_SMALL_N - 1)) == 0, "ring sizes are masks");
	OrbisVkTraceEntry s_orbis_trace[ORBIS_VK_TRACE_N];
	u64 s_orbis_trace_seq = 0;
	u32 s_orbis_submit = 0;
	// vk-285-113: what one submission holds, from the GS thread's side: PCSX2 draw configs, driver draws (a full-barrier config is
	// one draw per drawlist group, SendHWDraw) and other traced operations, since the last submit ("cur"), in the last submit
	// ("last") and the largest submit of the last 600 ("peak"). The driver's stream holds ~7,800 feedback draws or ~10,500 plain ones.
	struct OrbisSubmitCounts
	{
		u32 configs = 0, draws = 0, ops = 0;
	};
	OrbisSubmitCounts s_orbis_sub_cur, s_orbis_sub_last, s_orbis_sub_peak;
	// The driver's "a submission of more than N words" refusal (ps5vk_queue.c:1528), seen by DebugMessengerCallback.
	std::atomic<u32> s_orbis_overflow_reports{0};
	std::atomic<u64> s_orbis_overflow_words{0}; // the limit the message names
	std::atomic<u64> s_orbis_overflow_total{0}; // "this one holds N" (the driver patch adds it): the refused submission's words, 0 if not there

	// vk-285-90: the ring only runs with the flag file flags/vkring (read at the first traced call). Two
	// fresh ~128-byte entries per draw in a 2 MB ring (always out of the caches) were ~2% of the GS thread in
	// vk-285-89's Shadow of the Colossus profile. Without it a GPU-hang dump (vkhang.txt) and the on-demand
	// vktrace.txt only say that the ring was off.
	bool s_orbis_trace_on = false, s_orbis_trace_read = false;
	__fi bool OrbisVkTraceOn()
	{
		if (!s_orbis_trace_read) [[unlikely]]
		{
			s_orbis_trace_read = true;
			s_orbis_trace_on = OrbisFlag("vkring");
			// vk-285-113: the run after a hang gets the big ring by itself (OrbisVkHangExit leaves the file, this run takes it), so the
			// next report of the same game has every draw of the hung submission without asking the tester for a flag.
			bool after_hang = false;
			{
				const std::string once = OrbisLogPath("vkring-next.txt");
				if (FILE* f = fopen(once.c_str(), "rb"))
				{
					fclose(f);
					unlink(once.c_str());
					after_hang = true;
					s_orbis_trace_on = true;
				}
			}
			printf("[vkhw] draw ring for GPU-hang dumps: %s (%s); the last %u entries are always kept\n", s_orbis_trace_on ? "on" : "off",
				after_hang ? "the run after a GPU hang" : "flags/vkring", ORBIS_VK_TRACE_SMALL_N);
			fflush(stdout);
		}
		return s_orbis_trace_on;
	}
	__fi u32 OrbisVkTraceCap() { return OrbisVkTraceOn() ? ORBIS_VK_TRACE_N : ORBIS_VK_TRACE_SMALL_N; }

	OrbisVkTraceEntry& OrbisVkTraceNew(u8 kind)
	{
		OrbisVkTraceEntry& e = s_orbis_trace[s_orbis_trace_seq & (OrbisVkTraceCap() - 1)]; // both sizes are powers of two
		e = {};
		e.seq = s_orbis_trace_seq++;
		e.submit = s_orbis_submit;
		e.kind = kind;
		return e;
	}

	void OrbisVkTraceTex(const GSTexture* t, const void*& p, u16& w, u16& h, u8& f)
	{
		p = t;
		if (t)
		{
			w = static_cast<u16>(t->GetWidth());
			h = static_cast<u16>(t->GetHeight());
			f = static_cast<u8>(t->GetFormat());
		}
	}

	void OrbisVkTraceDraw(const GSHWDrawConfig& config)
	{
		// vk-285-113: counted and recorded always (the small ring); vk-285-90's flag only sizes the ring now.
		s_orbis_sub_cur.configs++;
		s_orbis_sub_cur.draws += (config.require_full_barrier && config.drawlist && !config.drawlist->empty()) ?
		                             static_cast<u32>(config.drawlist->size()) :
		                             1u;
		OrbisVkTraceEntry& e = OrbisVkTraceNew(1);
		e.topology = static_cast<u8>(config.topology);
		e.vs = config.vs.key;
		e.depth = config.depth.key;
		e.colormask = config.colormask.key;
		e.date = static_cast<u8>(config.destination_alpha);
		e.sampler = config.sampler.key;
		e.flags = (config.require_one_barrier ? 1 : 0) | (config.require_full_barrier ? 2 : 0) |
				  (config.alpha_second_pass.enable ? 4 : 0) | (config.blend_multi_pass.enable ? 8 : 0) |
				  (config.line_expand ? 16 : 0) | (static_cast<u8>(config.tex_hazard) << 5);
		e.blend = config.blend.key;
		e.ps_lo = config.ps.key_lo;
		e.ps_hi = config.ps.key_hi;
		e.nverts = config.nverts;
		e.nindices = config.nindices;
		e.groups = (config.require_full_barrier && config.drawlist) ? static_cast<u32>(config.drawlist->size()) : 0;
		OrbisVkTraceTex(config.rt, e.a, e.aw, e.ah, e.af);
		OrbisVkTraceTex(config.ds, e.b, e.bw, e.bh, e.bf);
		OrbisVkTraceTex(config.tex, e.c, e.cw, e.ch, e.cf);
		u16 pw = 0, ph = 0;
		OrbisVkTraceTex(config.pal, e.d, pw, ph, e.df);
		e.r[0] = config.drawarea.x;
		e.r[1] = config.drawarea.y;
		e.r[2] = config.drawarea.z;
		e.r[3] = config.drawarea.w;
		e.extra = (static_cast<u64>(static_cast<u32>(config.scissor.x)) & 0xffff) |
				  ((static_cast<u64>(static_cast<u32>(config.scissor.y)) & 0xffff) << 16) |
				  ((static_cast<u64>(static_cast<u32>(config.scissor.z)) & 0xffff) << 32) |
				  ((static_cast<u64>(static_cast<u32>(config.scissor.w)) & 0xffff) << 48);
	}

	void OrbisVkTraceOp(u8 kind, const GSTexture* dst, const GSTexture* src, const GSVector4i& r, u64 extra)
	{
		s_orbis_sub_cur.ops++; // vk-285-113
		OrbisVkTraceEntry& e = OrbisVkTraceNew(kind);
		OrbisVkTraceTex(dst, e.a, e.aw, e.ah, e.af);
		OrbisVkTraceTex(src, e.b, e.bw, e.bh, e.bf);
		e.r[0] = r.x;
		e.r[1] = r.y;
		e.r[2] = r.z;
		e.r[3] = r.w;
		e.extra = extra;
	}

	// Writes the ring's entries of the submits from `hung - 2` on to `path`, after a header line.
	void OrbisVkTraceWrite(const char* path, const char* header, u32 hung)
	{
		if (FILE* f = fopen(path, "w"))
		{
			fprintf(f, "%s\n", header);
			if (!s_orbis_trace_on) // vk-285-90; vk-285-113: the last 512 entries are there anyway
				fprintf(f, "the big draw ring was off: only the last %u entries below (put flags/vkring in place, or relaunch: the run after a hang has it on)\n",
					ORBIS_VK_TRACE_SMALL_N);
			fprintf(f, "kinds: 1 draw, 2 copy, 3 clear, 4 stretch, 5 multi-stretch, 6 readback, 7 upload; tex = ptr WxH f<GSTexture::Format>\n");
			fprintf(f, "draw: topo(0 pt,1 line,2 tri) vs depth colormask date(0 off,1 stencil,2 stencilone,3 primid,4 full) sampler flags(1 one-barrier,2 full-barrier,4 alpha2,8 blend-mp,16 line-expand,32+ hazard) blend ps nv/ni grp(drawlist groups of a full-barrier draw) rt ds tex pal drawarea scissor\n");
			const u32 cap = OrbisVkTraceCap();
			const u64 first = s_orbis_trace_seq > cap ? s_orbis_trace_seq - cap : 0;
			for (u64 q = first; q < s_orbis_trace_seq; q++)
			{
				const OrbisVkTraceEntry& e = s_orbis_trace[q & (cap - 1)];
				if (e.submit + 2 < hung)
					continue;
				if (e.kind == 1)
					fprintf(f,
						"%llu s%u draw topo=%u vs=%02x depth=%02x cm=%x date=%u samp=%02x fl=%02x blend=%08x ps=%016llx.%016llx "
						"nv=%u ni=%u grp=%u rt=%p %ux%u f%u ds=%p %ux%u f%u tex=%p %ux%u f%u pal=%p f%u area=%d,%d,%d,%d sc=%llx\n",
						static_cast<unsigned long long>(e.seq), e.submit, e.topology, e.vs, e.depth, e.colormask, e.date,
						e.sampler, e.flags, e.blend, static_cast<unsigned long long>(e.ps_hi),
						static_cast<unsigned long long>(e.ps_lo), e.nverts, e.nindices, e.groups, e.a, e.aw, e.ah, e.af, e.b, e.bw,
						e.bh, e.bf, e.c, e.cw, e.ch, e.cf, e.d, e.df, e.r[0], e.r[1], e.r[2], e.r[3],
						static_cast<unsigned long long>(e.extra));
				else if (e.kind == 8)
					fprintf(f, "%llu s%u pipe ps=%016llx.%016llx fb=%u t0=%p t1=%p t2=%p pl=%llx\n",
						static_cast<unsigned long long>(e.seq), e.submit, static_cast<unsigned long long>(e.ps_hi),
						static_cast<unsigned long long>(e.ps_lo), e.flags, e.a, e.b, e.c,
						static_cast<unsigned long long>(e.extra));
				else
					fprintf(f, "%llu s%u %s dst=%p %ux%u f%u src=%p %ux%u f%u rect=%d,%d,%d,%d x=%llx\n",
						static_cast<unsigned long long>(e.seq), e.submit,
						e.kind == 2 ? "copy" : e.kind == 3 ? "clear" : e.kind == 4 ? "stretch" : e.kind == 5 ? "mstretch" :
						e.kind == 6 ? "readback" : "upload",
						e.a, e.aw, e.ah, e.af, e.b, e.bw, e.bh, e.bf, e.r[0], e.r[1], e.r[2], e.r[3],
						static_cast<unsigned long long>(e.extra));
			}
			fflush(f);
			fsync(fileno(f));
			fclose(f);
		}
	}

	// vk-285-21: one entry per GPU pass (BindDrawPipeline), with the pixel shader selector the pass
	// really uses, its feedback-loop flags and the TFX texture slots 0-2.
	void OrbisVkTracePipe(u64 ps_lo, u64 ps_hi, u8 feedback, const void* t0, const void* t1, const void* t2, u64 pipeline)
	{
		OrbisVkTraceEntry& e = OrbisVkTraceNew(8); // vk-285-113: always (the small ring)
		e.ps_lo = ps_lo;
		e.ps_hi = ps_hi;
		e.flags = feedback;
		e.a = t0;
		e.b = t1;
		e.c = t2;
		e.extra = pipeline;
	}

	// vk-285-113: the counters for the last submits and for one submission the driver refused; every 600 submits the largest one
	// is logged (peaks, not the driver profile's per-frame averages: a frame that overflows is a spike).
	void OrbisVkSubmitDone()
	{
		s_orbis_sub_last = s_orbis_sub_cur;
		if (s_orbis_sub_cur.draws > s_orbis_sub_peak.draws)
			s_orbis_sub_peak = s_orbis_sub_cur;
		s_orbis_sub_cur = {};
		if (((s_orbis_submit + 1) % 600) == 0)
		{
			if (s_orbis_sub_peak.draws >= 1000)
			{
				printf("[vkhw] largest of the last 600 submits: %u driver draws (%u draw configs, %u other ops); the driver's stream holds ~7,800 feedback draws or ~10,500 plain ones\n",
					s_orbis_sub_peak.draws, s_orbis_sub_peak.configs, s_orbis_sub_peak.ops);
				fflush(stdout);
			}
			s_orbis_sub_peak = {};
		}
	}

	// vk-285-113: the last max_bytes of a log, kept under another name (the previous copy as `older`). The console keeps 8 sessions of logs but
	// the report carries this one and the two before it, so a tester who plays on after a hang (vk-285-112's Zatch Bell: three hangs, then three
	// other games) sends a report without the hung session's stderr.log and emulog.txt.
	void OrbisKeepLogTail(const std::string& from, const std::string& to, const std::string& older, size_t max_bytes)
	{
		FILE* in = fopen(from.c_str(), "rb");
		if (!in)
			return;
		std::string data;
		if (fseek(in, 0, SEEK_END) == 0)
		{
			const long size = ftell(in);
			const long start = size > static_cast<long>(max_bytes) ? size - static_cast<long>(max_bytes) : 0;
			if (size > 0 && fseek(in, start, SEEK_SET) == 0)
			{
				data.resize(static_cast<size_t>(size - start));
				data.resize(fread(data.data(), 1, data.size(), in));
			}
		}
		fclose(in);
		if (data.empty())
			return;
		rename(to.c_str(), older.c_str());
		if (FILE* out = fopen(to.c_str(), "wb"))
		{
			fwrite(data.data(), 1, data.size(), out);
			fclose(out);
		}
	}

	[[noreturn]] void OrbisVkHangExit(const char* where)
	{
		const u32 hung = s_orbis_submit;
		const u32 overflows = s_orbis_overflow_reports.load();
		std::string header = StringUtil::StdStringFromFormat("GPU hang: VK_ERROR_DEVICE_LOST in %s; the hung command buffer is submit %u.", where, hung);
		if (overflows)
		{
			const unsigned long long held = s_orbis_overflow_total.load();
			header += StringUtil::StdStringFromFormat(
				"\nclass: STREAM OVERFLOW, not a GPU stall: the driver refused a submission of more than %llu words %u time(s) (ps5vk_queue.c:1528, "
				"emulog.txt has the 'VK: debug report' lines). A frame put more draws into one vkQueueSubmit than its 2 MiB stream holds",
				static_cast<unsigned long long>(s_orbis_overflow_words.load()), overflows);
			if (held)
				header += StringUtil::StdStringFromFormat("; the last refused submission held %llu words.", held);
			else
				header += " (its size is not in the message: a driver without the vk-285-113 note).";
		}
		else
		{
			header += "\nclass: no stream-overflow refusal seen. A completion-marker timeout (a GPU stall) writes ps5vk-hang.txt and a "
			          "'[ps5vk] GPU hang' line in stderr.log; other refusals show in emulog.txt as 'VK: debug report'.";
		}
		header += StringUtil::StdStringFromFormat(
			"\ndraws (configs/driver draws/other ops): not yet submitted %u/%u/%u, last submit %u/%u/%u, largest since the last log line %u/%u/%u"
			" (with vk_async the failing submit is up to 4 submits before this one).",
			s_orbis_sub_cur.configs, s_orbis_sub_cur.draws, s_orbis_sub_cur.ops, s_orbis_sub_last.configs, s_orbis_sub_last.draws,
			s_orbis_sub_last.ops, s_orbis_sub_peak.configs, s_orbis_sub_peak.draws, s_orbis_sub_peak.ops);
		// vk-285-51: the previous hang's dump is kept as vkhang.1.txt, and the hang goes in the
		// settings log (frontend/fe_ps5.cpp), next to the settings changes that may have caused it.
		const std::string path = OrbisLogPath("vkhang.txt");
		std::rename(path.c_str(), OrbisLogPath("vkhang.1.txt").c_str());
		OrbisVkTraceWrite(path.c_str(), header.c_str(), hung);
		printf("[vkhw] GPU hang (VK_ERROR_DEVICE_LOST in %s, submit %u): wrote %s; closing the app\n", where, hung, path.c_str());
		orbis_log_drain(); // vk-285-104: the deferred lines (this one included) to boot.log before _exit
		fflush(stderr);
		// vk-285-113: this session's stderr.log and emulog.txt outlive the report's three-session window.
		OrbisKeepLogTail(OrbisLogPath("stderr.log"), OrbisLogPath("hang-stderr.txt"), OrbisLogPath("hang-stderr.1.txt"), 96 * 1024);
		OrbisKeepLogTail(OrbisLogPath("emulog.txt"), OrbisLogPath("hang-emulog.txt"), OrbisLogPath("hang-emulog.1.txt"), 96 * 1024);
		// vk-285-113: the run after a hang has the big draw ring on (OrbisVkTraceOn takes this file).
		if (FILE* f = fopen(OrbisLogPath("vkring-next.txt").c_str(), "wb"))
		{
			fputs("the next run records the full draw ring (deleted by that run)\n", f);
			fclose(f);
		}
		if (orbis_event_log)
		{
			// vk-285-113: the class and the counters stay in settings.log (kept whole) after the rotated logs are gone.
			char line[400];
			snprintf(line, sizeof(line),
				"GPU hang (VK_ERROR_DEVICE_LOST in %s); the app closed itself. The dump is logs/vkhang.txt [%s; submit %u; driver draws not yet submitted %u, last submit %u, peak %u; refused submission %llu words]",
				where, overflows ? "class: stream overflow" : "class: no overflow refusal", hung, s_orbis_sub_cur.draws, s_orbis_sub_last.draws,
				s_orbis_sub_peak.draws, static_cast<unsigned long long>(s_orbis_overflow_total.load()));
			orbis_event_log(line);
		}
		OrbisExitApp(3); // vk-285-115: was _exit(3), which ends in SIGSYS on the console
	}
} // namespace

void OrbisVkTraceClear(const GSTexture* tex, u32 value)
{
	OrbisVkTraceOp(3, tex, nullptr, GSVector4i::zero(), value);
}

void OrbisVkTraceTransfer(u8 kind, const GSTexture* tex, const GSVector4i& r)
{
	OrbisVkTraceOp(kind, tex, nullptr, r, 0);
}
#endif

#ifdef ORBIS_VULKAN
// PS5 port (vk-285-95): DOWNSAMPLE_COPY pipelines with the factor and step compiled in, [factor][step]
// (GSDeviceVK::OrbisDownsamplePipeline), and flags/ds_dynamic (read once a second by OrbisGpSecond), which
// goes back to the original shader.
static VkPipeline s_orbis_ds_pipe[9][3] = {};
static bool s_orbis_ds_failed[9][3] = {};
static bool s_orbis_ds_dynamic = false;

// PS5 port (vk-285-94): the GPU profiler, with flags/gpuprof (live: OrbisGpSecond reads the flag once a
// second; a command buffer started while it's there is profiled). A profiled command buffer gets a GPU
// timestamp at its start, after each GS draw (RenderHW, all its passes), each utility draw (conversions,
// copies by draw, the present), image copy and blit, at each render pass start (its clears, and the store
// and layout changes since the last step), and at its end. The GPU writes a timestamp when all the work
// before it has finished (end of pipe), so the gap from one to the next is what that step added to the
// GPU's time. Once a second [gpuprof] lines: the GPU time per command buffer by kind of step, then the
// costliest groups of steps (same kind, shader, blend, barriers, depth and target size) with their share,
// count and pixels; every 5 s the costliest single steps of one command buffer, in order. The timestamps
// cost some GPU and recorder time of their own. Needs proper testing.
namespace
{
	constexpr u32 ORBIS_GP_MAX = 16384; // timestamps per command buffer (SotC's heavy view: ~1,300 GS draws a frame)
	enum : u8
	{
		GPK_START,
		GPK_DRAW,
		GPK_UTIL,
		GPK_COPY,
		GPK_BLIT,
		GPK_RPASS,
		GPK_END,
		GPK_KINDS
	};
	constexpr const char* s_gp_kind_name[GPK_KINDS] = {"start", "draw", "util", "copy", "blit", "rpass", "end"};

	struct OrbisGpMeta
	{
		u8 kind, flags, rt_fmt, tex_fmt;
		u16 rt_w, rt_h, area_w, area_h, tex_w, tex_h;
		u32 prims, blend, misc;
		u64 pipe;
		u64 ps_lo, ps_hi;
	};

	struct OrbisGpFrame
	{
		u32 count = 0;
		bool on = false, overflow = false;
		OrbisGpMeta meta[ORBIS_GP_MAX];
	};
	OrbisGpFrame s_gp_frame[3]; // GSDeviceVK::NUM_COMMAND_BUFFERS (checked in OrbisGpBegin)
	u64 s_gp_ts[ORBIS_GP_MAX];
	VkQueryPool s_gp_pool = VK_NULL_HANDLE;
	bool s_gp_want = false, s_gp_pool_failed = false;

	struct OrbisGpGroup
	{
		u64 key; // 0: free
		OrbisGpMeta ex;
		u64 ticks, pixels;
		u32 count;
	};
	constexpr u32 ORBIS_GP_GROUPS = 2048;
	OrbisGpGroup s_gp_group[ORBIS_GP_GROUPS];
	u32 s_gp_groups_used = 0, s_gp_groups_lost = 0;
	u64 s_gp_kind_ticks[GPK_KINDS];
	u32 s_gp_kind_n[GPK_KINDS];
	u64 s_gp_span_ticks = 0;
	u32 s_gp_cbs = 0, s_gp_overflows = 0, s_gp_bad = 0, s_gp_seconds = 0;

	// One command buffer's costliest steps, for the 5-second timeline.
	struct OrbisGpStep
	{
		OrbisGpMeta m;
		u64 ticks;
		u32 at;
	};
	constexpr u32 ORBIS_GP_TL = 40;
	OrbisGpStep s_gp_tl[ORBIS_GP_TL];
	u32 s_gp_tl_n = 0, s_gp_tl_steps = 0;
	u64 s_gp_tl_span = 0;
	bool s_gp_tl_want = false;

	u64 OrbisGpKey(const OrbisGpMeta& m)
	{
		u64 h = 0xcbf29ce484222325ull;
		const auto mix = [&h](u64 v) {
			h ^= v;
			h *= 0x100000001b3ull;
			h ^= h >> 29;
		};
		mix(m.kind);
		mix((static_cast<u64>(m.rt_w) << 16) | m.rt_h);
		mix(m.rt_fmt);
		switch (m.kind)
		{
			case GPK_DRAW:
				mix(m.ps_lo);
				mix(m.ps_hi);
				mix(m.blend);
				mix(m.misc);
				mix(m.flags);
				mix(m.pipe); // the second-pass shader's key, when there's one
				break;
			case GPK_UTIL:
				mix(m.pipe);
				break;
			case GPK_COPY:
			case GPK_BLIT:
				mix((static_cast<u64>(m.area_w) << 16) | m.area_h);
				break;
			case GPK_RPASS:
				mix(m.flags);
				mix(m.tex_fmt);
				break;
			default:
				break;
		}
		return h | 1;
	}

	void OrbisGpAdd(const OrbisGpMeta& m, u64 ticks)
	{
		const u64 key = OrbisGpKey(m);
		const u64 pixels = static_cast<u64>(m.area_w) * m.area_h;
		u32 i = static_cast<u32>(key) & (ORBIS_GP_GROUPS - 1);
		for (u32 probe = 0; probe < 32; probe++, i = (i + 1) & (ORBIS_GP_GROUPS - 1))
		{
			OrbisGpGroup& g = s_gp_group[i];
			if (g.key == key)
			{
				g.ticks += ticks;
				g.pixels += pixels;
				g.count++;
				return;
			}
			if (g.key == 0)
			{
				if (s_gp_groups_used >= ORBIS_GP_GROUPS * 3 / 4)
					break;
				g.key = key;
				g.ex = m;
				g.ticks = ticks;
				g.pixels = pixels;
				g.count = 1;
				s_gp_groups_used++;
				return;
			}
		}
		s_gp_groups_lost++;
	}

	// GSTexture::Format as a short name.
	const char* OrbisGpFmt(u8 f)
	{
		switch (static_cast<GSTexture::Format>(f))
		{
			case GSTexture::Format::Invalid: return "-";
			case GSTexture::Format::Color: return "C32";
			case GSTexture::Format::ColorHQ: return "C10";
			case GSTexture::Format::ColorHDR: return "C16F";
			case GSTexture::Format::ColorClip: return "CClip";
			case GSTexture::Format::DepthStencil: return "D32S8";
			case GSTexture::Format::DepthColor: return "DColor";
			case GSTexture::Format::UNorm8: return "U8";
			case GSTexture::Format::UInt16: return "U16";
			case GSTexture::Format::UInt32: return "U32";
			case GSTexture::Format::PrimID: return "PrimID";
			case GSTexture::Format::BC1: return "BC1";
			case GSTexture::Format::BC2: return "BC2";
			case GSTexture::Format::BC3: return "BC3";
			case GSTexture::Format::BC7: return "BC7";
			default: return "?";
		}
	}

	// The pixel shader selector's fields that are set, "name=value" (1-bit ones by name alone).
	void OrbisGpDescribePS(u64 lo, u64 hi, char* out, size_t n)
	{
		GSHWDrawConfig::PSSelector ps;
		ps.key_lo = lo;
		ps.key_hi = hi;
		size_t pos = 0;
		const auto add = [&](const char* name, u32 v, bool bit) {
			if (!v || pos + 24 >= n)
				return;
			pos += bit ? snprintf(out + pos, n - pos, " %s", name) : snprintf(out + pos, n - pos, " %s=%u", name, v);
		};
		add("aem_fmt", ps.aem_fmt, false);
		add("pal_fmt", ps.pal_fmt, false);
		add("dst_fmt", ps.dst_fmt, false);
		add("depth_fmt", ps.depth_fmt, false);
		add("aem", ps.aem, true);
		add("fba", ps.fba, true);
		add("fog", ps.fog, true);
		add("iip", ps.iip, true);
		add("date", ps.date, false);
		add("atst", static_cast<u32>(ps.atst), false);
		add("afail", static_cast<u32>(ps.afail), false);
		add("ztst", ps.ztst, false);
		add("fst", ps.fst, true);
		add("tfx", ps.tfx, false);
		add("tcc", ps.tcc, true);
		add("wms", ps.wms, false);
		add("wmt", ps.wmt, false);
		add("adjs", ps.adjs, true);
		add("adjt", ps.adjt, true);
		add("ltf", ps.ltf, true);
		add("shuffle", ps.shuffle, true);
		add("shuffle_same", ps.shuffle_same, true);
		add("real16src", ps.real16src, true);
		add("process_ba", ps.process_ba, false);
		add("process_rg", ps.process_rg, false);
		add("shuffle_across", ps.shuffle_across, true);
		add("write_rg", ps.write_rg, true);
		add("fbmask", ps.fbmask, true);
		add("blend_a", ps.blend_a, false);
		add("blend_b", ps.blend_b, false);
		add("blend_c", ps.blend_c, false);
		add("blend_d", ps.blend_d, false);
		add("fixed_one_a", ps.fixed_one_a, true);
		add("blend_hw", ps.blend_hw, false);
		add("a_masked", ps.a_masked, true);
		add("colclip_hw", ps.colclip_hw, true);
		add("rta_corr", ps.rta_correction, true);
		add("rta_src_corr", ps.rta_source_correction, true);
		add("colclip", ps.colclip, true);
		add("blend_mix", ps.blend_mix, false);
		add("round_inv", ps.round_inv, true);
		add("pabe", ps.pabe, true);
		add("no_color", ps.no_color, true);
		add("no_color1", ps.no_color1, true);
		add("channel", ps.channel, false);
		add("dither", ps.dither, false);
		add("dither_adjust", ps.dither_adjust, true);
		add("zclamp", ps.zclamp, true);
		add("zfloor", ps.zfloor, true);
		add("tcoffsethack", ps.tcoffsethack, true);
		add("tex_is_fb", ps.tex_is_fb, true);
		add("auto_lod", ps.automatic_lod, true);
		add("manual_lod", ps.manual_lod, true);
		add("point_sampler", ps.point_sampler, true);
		add("region_rect", ps.region_rect, true);
		add("scanmsk", ps.scanmsk, false);
		add("aa1", static_cast<u32>(ps.aa1), false);
		add("abe", ps.abe, true);
		add("sw_aniso", ps.sw_aniso, false);
		add("rov_color", ps.rov_color, true);
		add("rov_depth", static_cast<u32>(ps.rov_depth), false);
		if (pos == 0 && n > 0)
			snprintf(out, n, " -");
	}
} // namespace

void GSDeviceVK::OrbisGpBegin(u32 index)
{
	static_assert(std::size(s_gp_frame) == NUM_COMMAND_BUFFERS);
	OrbisGpFrame& f = s_gp_frame[index];
	f.count = 0;
	f.overflow = false;
	f.on = s_gp_want && !s_gp_pool_failed;
	if (!f.on)
		return;
	if (s_gp_pool == VK_NULL_HANDLE)
	{
		const VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP,
			ORBIS_GP_MAX * NUM_COMMAND_BUFFERS, 0};
		const VkResult res = vkCreateQueryPool(m_device, &ci, nullptr, &s_gp_pool);
		if (res != VK_SUCCESS)
		{
			printf("[gpuprof] no timestamp query pool (VkResult %d): the profiler stays off\n", static_cast<int>(res));
			s_gp_pool_failed = true;
			s_gp_pool = VK_NULL_HANDLE;
			f.on = false;
			return;
		}
	}
	vkCmdResetQueryPool(m_frame_resources[index].command_buffers[1], s_gp_pool, index * ORBIS_GP_MAX, ORBIS_GP_MAX);
	OrbisGpMeta m = {};
	m.kind = GPK_START;
	f.meta[0] = m;
	vkCmdWriteTimestamp(m_frame_resources[index].command_buffers[1], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_gp_pool,
		index * ORBIS_GP_MAX);
	f.count = 1;
}

static inline bool OrbisGpOn(u32 frame)
{
	return s_gp_frame[frame].on;
}

void GSDeviceVK::OrbisGpWrite(const void* meta)
{
	OrbisGpFrame& f = s_gp_frame[m_current_frame];
	if (!f.on)
		return;
	if (f.count >= ORBIS_GP_MAX)
	{
		f.overflow = true;
		return;
	}
	vkCmdWriteTimestamp(m_current_command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_gp_pool,
		m_current_frame * ORBIS_GP_MAX + f.count);
	f.meta[f.count++] = *static_cast<const OrbisGpMeta*>(meta);
}

void GSDeviceVK::OrbisGpStampDraw(const GSHWDrawConfig& config)
{
	if (!OrbisGpOn(m_current_frame)) [[likely]]
		return;
	OrbisGpMeta m = {};
	m.kind = GPK_DRAW;
	const GSTexture* rt = config.rt ? config.rt : config.ds;
	if (rt)
	{
		m.rt_w = static_cast<u16>(rt->GetWidth());
		m.rt_h = static_cast<u16>(rt->GetHeight());
		m.rt_fmt = static_cast<u8>(rt->GetFormat());
	}
	m.area_w = static_cast<u16>(std::max(config.drawarea.width(), 0));
	m.area_h = static_cast<u16>(std::max(config.drawarea.height(), 0));
	if (config.tex)
	{
		m.tex_w = static_cast<u16>(config.tex->GetWidth());
		m.tex_h = static_cast<u16>(config.tex->GetHeight());
		m.tex_fmt = static_cast<u8>(config.tex->GetFormat());
	}
	m.flags = (config.require_one_barrier ? 1 : 0) | (config.require_full_barrier ? 2 : 0) |
	          (config.alpha_second_pass.enable ? 4 : 0) | (config.blend_multi_pass.enable ? 8 : 0) |
	          (config.ps.colclip_hw ? 16 : 0) |
	          (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking ? 32 : 0) |
	          (config.ds ? 64 : 0) | (config.tex && (config.tex == config.rt || config.tex == config.ds) ? 128 : 0);
	m.prims = (config.require_full_barrier && config.drawlist) ? static_cast<u32>(config.drawlist->size()) :
	          config.indices_per_prim ? config.nindices / config.indices_per_prim : config.nindices;
	m.blend = config.blend.key;
	m.misc = static_cast<u32>(config.depth.key) | (static_cast<u32>(config.colormask.key) << 8) |
	         (static_cast<u32>(config.topology) << 16) | (static_cast<u32>(config.destination_alpha) << 24);
	m.ps_lo = config.ps.key_lo;
	m.ps_hi = config.ps.key_hi;
	m.pipe = config.alpha_second_pass.enable ? (config.alpha_second_pass.ps.key_lo ^ (config.alpha_second_pass.ps.key_hi << 1)) : 0;
	OrbisGpWrite(&m);
}

void GSDeviceVK::OrbisGpStampOp(u8 kind, const GSTexture* dst, u32 w, u32 h, u64 pipe, u32 prims, u8 flags)
{
	if (!OrbisGpOn(m_current_frame)) [[likely]]
		return;
	OrbisGpMeta m = {};
	m.kind = kind;
	m.flags = flags;
	if (dst)
	{
		m.rt_w = static_cast<u16>(dst->GetWidth());
		m.rt_h = static_cast<u16>(dst->GetHeight());
		m.rt_fmt = static_cast<u8>(dst->GetFormat());
	}
	m.area_w = static_cast<u16>(std::min<u32>(w, 0xffff));
	m.area_h = static_cast<u16>(std::min<u32>(h, 0xffff));
	m.pipe = pipe;
	m.prims = prims;
	OrbisGpWrite(&m);
}

void GSDeviceVK::OrbisGpStampPass(bool clear)
{
	if (!OrbisGpOn(m_current_frame)) [[likely]]
		return;
	OrbisGpMeta m = {};
	m.kind = GPK_RPASS;
	const GSTexture* rt = m_current_render_target ? static_cast<const GSTexture*>(m_current_render_target) :
	                                                 static_cast<const GSTexture*>(m_current_depth_target);
	if (rt)
	{
		m.rt_w = static_cast<u16>(rt->GetWidth());
		m.rt_h = static_cast<u16>(rt->GetHeight());
	}
	if (m_current_render_target)
		m.rt_fmt = static_cast<u8>(m_current_render_target->GetFormat());
	if (m_current_depth_target)
		m.tex_fmt = static_cast<u8>(m_current_depth_target->GetFormat());
	m.flags = (clear ? 1 : 0) | (m_current_render_target ? 2 : 0) | (m_current_depth_target ? 4 : 0);
	m.area_w = static_cast<u16>(m_current_render_pass_area.width());
	m.area_h = static_cast<u16>(m_current_render_pass_area.height());
	OrbisGpWrite(&m);
}

void GSDeviceVK::OrbisGpCollect(u32 index)
{
	OrbisGpFrame& f = s_gp_frame[index];
	if (!f.on)
		return;
	f.on = false;
	if (f.overflow)
		s_gp_overflows++;
	const u32 n = f.count;
	if (n < 2 || s_gp_pool == VK_NULL_HANDLE)
		return;
	const VkResult res = vkGetQueryPoolResults(m_device, s_gp_pool, index * ORBIS_GP_MAX, n, sizeof(u64) * n, s_gp_ts,
		sizeof(u64), VK_QUERY_RESULT_64_BIT);
	if (res != VK_SUCCESS || s_gp_ts[0] == 0)
	{
		s_gp_bad++;
		return;
	}
	const bool tl = s_gp_tl_want;
	u32 tl_n = 0;
	u64 prev = s_gp_ts[0];
	for (u32 i = 1; i < n; i++)
	{
		const u64 t = s_gp_ts[i];
		if (t == 0 || t < prev)
		{
			s_gp_bad++;
			continue;
		}
		const u64 d = t - prev;
		prev = t;
		const OrbisGpMeta& m = f.meta[i];
		s_gp_kind_ticks[m.kind] += d;
		s_gp_kind_n[m.kind]++;
		OrbisGpAdd(m, d);
		if (tl)
		{
			// Keep the ORBIS_GP_TL costliest (replace the cheapest kept).
			if (tl_n < ORBIS_GP_TL)
			{
				s_gp_tl[tl_n++] = {m, d, i};
			}
			else
			{
				u32 lo = 0;
				for (u32 k = 1; k < ORBIS_GP_TL; k++)
					if (s_gp_tl[k].ticks < s_gp_tl[lo].ticks)
						lo = k;
				if (d > s_gp_tl[lo].ticks)
					s_gp_tl[lo] = {m, d, i};
			}
		}
	}
	s_gp_span_ticks += prev - s_gp_ts[0];
	s_gp_cbs++;
	if (tl)
	{
		std::sort(s_gp_tl, s_gp_tl + tl_n, [](const OrbisGpStep& a, const OrbisGpStep& b) { return a.at < b.at; });
		s_gp_tl_n = tl_n;
		s_gp_tl_steps = n;
		s_gp_tl_span = prev - s_gp_ts[0];
		s_gp_tl_want = false;
	}
}

void GSDeviceVK::OrbisGpDescribe(const void* meta, char* out, size_t n) const
{
	const OrbisGpMeta& m = *static_cast<const OrbisGpMeta*>(meta);
	switch (m.kind)
	{
		case GPK_DRAW:
		{
			char ps[512];
			OrbisGpDescribePS(m.ps_lo, m.ps_hi, ps, sizeof(ps));
			GSHWDrawConfig::BlendState bs;
			bs.key = m.blend;
			const GSHWDrawConfig::DepthStencilSelector dss(static_cast<u8>(m.misc & 0xff));
			snprintf(out, n,
				"draw rt %ux%u %s area %ux%u tex %ux%u %s prims %u%s%s%s%s%s%s%s | bs %s op %u src %u dst %u%s | z tst %u we %u date %u/%u | cm %x topo %u datm %u | ps%s",
				m.rt_w, m.rt_h, OrbisGpFmt(m.rt_fmt), m.area_w, m.area_h, m.tex_w, m.tex_h, OrbisGpFmt(m.tex_fmt), m.prims,
				(m.flags & 1) ? " one-barrier" : "", (m.flags & 2) ? " FULL-BARRIER" : "", (m.flags & 4) ? " +alpha-pass" : "",
				(m.flags & 8) ? " +blend-pass" : "", (m.flags & 16) ? " colclip-hw" : "", (m.flags & 32) ? " primid-date" : "",
				(m.flags & 128) ? " tex=target" : "", bs.enable ? "on" : "off", bs.op, bs.src_factor, bs.dst_factor,
				bs.constant_enable ? " const" : "", dss.ztst, dss.zwe, dss.date, dss.date_one, (m.misc >> 8) & 0xf,
				(m.misc >> 16) & 0xff, (m.misc >> 24) & 0xff, ps);
			break;
		}
		case GPK_UTIL:
		{
			const VkPipeline p = reinterpret_cast<VkPipeline>(m.pipe);
			char name[64] = "other";
			for (u32 i = 0; i < m_convert.size(); i++)
			{
				if (m_convert[i] == p && p != VK_NULL_HANDLE)
				{
					const ShaderConvertSelector sel = ShaderConvertSelector::Get(i);
					snprintf(name, sizeof(name), "convert %s mask %x%s", sel.Name(), sel.Mask(), sel.Biln() ? " biln" : "");
					break;
				}
			}
			for (u32 i = 0; i < m_present.size(); i++)
				if (m_present[i] == p && p != VK_NULL_HANDLE)
					snprintf(name, sizeof(name), "present %u", i);
			for (u32 i = 0; i < m_merge.size(); i++)
				if (m_merge[i] == p && p != VK_NULL_HANDLE)
					snprintf(name, sizeof(name), "merge %u", i);
			for (u32 i = 0; i < m_interlace.size(); i++)
				if (m_interlace[i] == p && p != VK_NULL_HANDLE)
					snprintf(name, sizeof(name), "interlace %u", i);
			for (u32 i = 0; i < 4; i++)
			{
				if (m_colclip_setup_pipelines[i / 2][i % 2] == p && p != VK_NULL_HANDLE)
					snprintf(name, sizeof(name), "colclip setup");
				if (m_colclip_finish_pipelines[i / 2][i % 2] == p && p != VK_NULL_HANDLE)
					snprintf(name, sizeof(name), "colclip finish");
			}
			for (u32 i = 0; i < 8; i++)
				if (m_primid_image_setup_pipelines[i / 4][i % 4] == p && p != VK_NULL_HANDLE)
					snprintf(name, sizeof(name), "primid setup");
			if (p != VK_NULL_HANDLE && p == m_fxaa_pipeline)
				snprintf(name, sizeof(name), "fxaa");
			if (p != VK_NULL_HANDLE && p == m_shadeboost_pipeline)
				snprintf(name, sizeof(name), "shadeboost");
			if (p != VK_NULL_HANDLE && p == m_imgui_pipeline)
				snprintf(name, sizeof(name), "imgui");
			for (u32 f = 2; f < 9; f++) // vk-285-95
				for (u32 st = 1; st < 3; st++)
					if (p != VK_NULL_HANDLE && p == s_orbis_ds_pipe[f][st])
						snprintf(name, sizeof(name), "downsample fixed %ux%u step %u", f, f, st);
			snprintf(out, n, "util %s | dst %ux%u %s area %ux%u%s", name, m.rt_w, m.rt_h, OrbisGpFmt(m.rt_fmt), m.area_w,
				m.area_h, m.prims > 1 ? " (multi)" : "");
			break;
		}
		case GPK_COPY:
		case GPK_BLIT:
			snprintf(out, n, "%s %ux%u into %ux%u %s", s_gp_kind_name[m.kind], m.area_w, m.area_h, m.rt_w, m.rt_h,
				OrbisGpFmt(m.rt_fmt));
			break;
		case GPK_RPASS:
			snprintf(out, n, "rpass%s rt %ux%u %s ds %s area %ux%u (the last pass's end, barriers, this one's start)",
				(m.flags & 1) ? " CLEAR" : "", m.rt_w, m.rt_h, (m.flags & 2) ? OrbisGpFmt(m.rt_fmt) : "-",
				(m.flags & 4) ? OrbisGpFmt(m.tex_fmt) : "-", m.area_w, m.area_h);
			break;
		case GPK_END:
			snprintf(out, n, "end of the command buffer (what follows the last step: the present's draws, readbacks)");
			break;
		default:
			snprintf(out, n, "%s", m.kind < GPK_KINDS ? s_gp_kind_name[m.kind] : "?");
			break;
	}
}

extern "C" uint32_t ps5vk_debug_timestamp_flush(uint32_t flush) __attribute__((weak)); // the driver (ps5vk_debug.h)

void GSDeviceVK::OrbisGpSecond()
{
	const bool ds_dynamic = OrbisFlag("ds_dynamic"); // vk-285-95
	if (ds_dynamic != s_orbis_ds_dynamic)
	{
		printf("[vkhw] native-scaling downsample: %s (flags/ds_dynamic)\n", ds_dynamic ? "the original shader" : "the fixed-factor shaders");
		s_orbis_ds_dynamic = ds_dynamic;
	}
	const bool want = OrbisFlag("gpuprof");
	if (want != s_gp_want)
	{
		// The driver's timestamps carry the completion marker's cache actions (a GL2 write-back and
		// invalidate) unless told otherwise: after every draw that would slow the frame being measured.
		// flags/gpuprof_flush keeps them (to compare).
		const bool flush = !want || OrbisFlag("gpuprof_flush");
		if (ps5vk_debug_timestamp_flush)
			ps5vk_debug_timestamp_flush(flush ? 1u : 0u);
		printf("[gpuprof] %s (flags/gpuprof), timestamps %s\n", want ? "on" : "off",
			!ps5vk_debug_timestamp_flush ? "as the driver writes them" : flush ? "with cache actions" : "without cache actions");
		s_gp_want = want;
		s_gp_seconds = 0;
	}
	if (s_gp_cbs != 0)
	{
		const double tick_ms = static_cast<double>(m_device_properties.limits.timestampPeriod) / 1e6;
		const double per_cb = 1.0 / s_gp_cbs;
		u64 steps_ticks = 0;
		for (u32 k = 1; k < GPK_KINDS; k++)
			steps_ticks += s_gp_kind_ticks[k];
		printf("[gpuprof] %u command buffers, GPU %.2f ms each | per command buffer:", s_gp_cbs, s_gp_span_ticks * tick_ms * per_cb);
		for (u32 k = 1; k < GPK_KINDS; k++)
			printf(" %s %.2f ms (%.0f)", s_gp_kind_name[k], s_gp_kind_ticks[k] * tick_ms * per_cb, s_gp_kind_n[k] * per_cb);
		printf(" | groups %u lost %u, overflows %u, bad %u\n", s_gp_groups_used, s_gp_groups_lost, s_gp_overflows, s_gp_bad);

		// The costliest groups.
		constexpr u32 TOP = 18;
		u32 top[TOP];
		u32 top_n = 0;
		for (u32 i = 0; i < ORBIS_GP_GROUPS; i++)
		{
			if (s_gp_group[i].key == 0)
				continue;
			if (top_n < TOP)
			{
				top[top_n++] = i;
				continue;
			}
			u32 lo = 0;
			for (u32 k = 1; k < TOP; k++)
				if (s_gp_group[top[k]].ticks < s_gp_group[top[lo]].ticks)
					lo = k;
			if (s_gp_group[i].ticks > s_gp_group[top[lo]].ticks)
				top[lo] = i;
		}
		std::sort(top, top + top_n, [](u32 a, u32 b) { return s_gp_group[a].ticks > s_gp_group[b].ticks; });
		for (u32 r = 0; r < top_n; r++)
		{
			const OrbisGpGroup& g = s_gp_group[top[r]];
			char desc[1024];
			OrbisGpDescribe(&g.ex, desc, sizeof(desc));
			printf("[gpuprof] #%u %4.1f%% %.3f ms/cb n %.1f/cb avg %.1f us, %.2f Mpx/step (%.0f Mpx/ms) | %s\n", r + 1,
				steps_ticks ? 100.0 * g.ticks / steps_ticks : 0.0, g.ticks * tick_ms * per_cb, g.count * per_cb,
				g.ticks * tick_ms * 1000.0 / g.count, static_cast<double>(g.pixels) / g.count / 1e6,
				g.ticks ? (static_cast<double>(g.pixels) / 1e6) / (g.ticks * tick_ms) : 0.0, desc);
		}
	}
	if (s_gp_tl_n != 0)
	{
		const double tick_ms = static_cast<double>(m_device_properties.limits.timestampPeriod) / 1e6;
		printf("[gpuprof] one command buffer: %u steps, %.2f ms; its %u costliest steps in order:\n", s_gp_tl_steps,
			s_gp_tl_span * tick_ms, s_gp_tl_n);
		for (u32 i = 0; i < s_gp_tl_n; i++)
		{
			char desc[1024];
			OrbisGpDescribe(&s_gp_tl[i].m, desc, sizeof(desc));
			printf("[gpuprof]   step %5u %7.3f ms | %s\n", s_gp_tl[i].at, s_gp_tl[i].ticks * tick_ms, desc);
		}
		s_gp_tl_n = 0;
	}
	if (s_gp_want && (++s_gp_seconds % 5) == 0)
		s_gp_tl_want = true;
	if (s_gp_cbs != 0 || s_gp_tl_n != 0)
		fflush(stdout);
	std::memset(s_gp_group, 0, sizeof(s_gp_group));
	s_gp_groups_used = s_gp_groups_lost = 0;
	std::memset(s_gp_kind_ticks, 0, sizeof(s_gp_kind_ticks));
	std::memset(s_gp_kind_n, 0, sizeof(s_gp_kind_n));
	s_gp_span_ticks = 0;
	s_gp_cbs = s_gp_overflows = s_gp_bad = 0;
}

void GSDeviceVK::OrbisGpDestroy()
{
	if (s_gp_pool != VK_NULL_HANDLE)
	{
		vkDestroyQueryPool(m_device, s_gp_pool, nullptr);
		s_gp_pool = VK_NULL_HANDLE;
	}
	for (OrbisGpFrame& f : s_gp_frame)
		f.on = false;
}

// For GSRenderer.cpp (which doesn't include this header), once a second on the GS thread.
void OrbisVkGpuProfSecond()
{
	if (GSDeviceVK* dev = GSDeviceVK::GetInstance())
		dev->OrbisGpSecond();
}
#endif

void GSDeviceVK::WaitForCommandBufferCompletion(u32 index)
{
	// Wait for this command buffer to be completed.
	const VkResult res = vkWaitForFences(m_device, 1, &m_frame_resources[index].fence, VK_TRUE, UINT64_MAX);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkWaitForFences failed: ");
#ifdef ORBIS_VULKAN
		if (res == VK_ERROR_DEVICE_LOST)
			OrbisVkHangExit("vkWaitForFences");
#endif
		m_last_submit_failed = true;
		return;
	}

	// Clean up any resources for command buffers between the last known completed buffer and this
	// now-completed command buffer. If we use >2 buffers, this may be more than one buffer.
	const u64 now_completed_counter = m_frame_resources[index].fence_counter;
	u32 cleanup_index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
	while (cleanup_index != m_current_frame)
	{
		FrameResources& resources = m_frame_resources[cleanup_index];
		if (resources.fence_counter > now_completed_counter)
			break;

		if (resources.fence_counter > m_completed_fence_counter)
			CommandBufferCompleted(cleanup_index);

		cleanup_index = (cleanup_index + 1) % NUM_COMMAND_BUFFERS;
	}

	m_completed_fence_counter = now_completed_counter;
}

#ifdef ORBIS_VULKAN
// vk-285-13 (HW renderer bring-up): the PS5 driver refuses what it cannot encode yet, and
// vkEndCommandBuffer then fails with the reason on stderr and in the emulog. Instead of
// stopping at the first refusal, the frame's commands are dropped: the pool is reset and
// both buffers are re-recorded empty, so the fence and the swap chain's semaphores still
// signal. One run then lists every refusal the game reaches.
static void OrbisDropRefusedCommands(VkDevice device, VkCommandPool pool, VkCommandBuffer* buffers, bool init_used)
{
	static unsigned s_dropped = 0;
	if ((s_dropped++ % 60) == 0)
	{
		printf("[vkhw] the driver refused a command buffer: frame %u dropped (%u so far; reasons in stderr.log/emulog)\n",
			s_dropped, s_dropped);
		fflush(stdout);
	}
	vkResetCommandPool(device, pool, 0);
	const VkCommandBufferBeginInfo begin_info = {
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
	for (u32 i = init_used ? 0u : 1u; i < 2; i++)
	{
		vkBeginCommandBuffer(buffers[i], &begin_info);
		vkEndCommandBuffer(buffers[i]);
	}
}
#endif

void GSDeviceVK::SubmitCommandBuffer(VKSwapChain* present_swap_chain)
{
	FrameResources& resources = m_frame_resources[m_current_frame];

	// End the current command buffer.
	VkResult res;
#ifdef ORBIS_VULKAN
	bool orbis_refused = false;
#endif
	if (resources.init_buffer_used)
	{
		res = vkEndCommandBuffer(resources.command_buffers[0]);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkEndCommandBuffer failed: ");
#ifdef ORBIS_VULKAN
			orbis_refused = true;
#else
			pxFailRel("Failed to end command buffer");
#endif
		}
	}

	bool wants_timestamp = m_gpu_timing_enabled || m_spin_timer;
	if (wants_timestamp && resources.timestamp_written)
	{
		vkCmdWriteTimestamp(m_current_command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, m_timestamp_query_pool,
			m_current_frame * 2 + 1);
	}
#ifdef ORBIS_VULKAN
	OrbisGpStampOp(GPK_END, nullptr, 0, 0, 0, 0, 0); // vk-285-94
#endif

	if (resources.pipeline_statistics_query == QueryState::Querying)
	{
		// Didn't end query in BeginPresent() so end it here.
		resources.pipeline_statistics_query = QueryState::Ready;
		vkCmdEndQuery(m_current_command_buffer, m_pipeline_statistics_query_pool, m_current_frame);
	}

	res = vkEndCommandBuffer(resources.command_buffers[1]);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkEndCommandBuffer failed: ");
#ifdef ORBIS_VULKAN
		orbis_refused = true;
#else
		pxFailRel("Failed to end command buffer");
#endif
	}
#ifdef ORBIS_VULKAN
	if (orbis_refused)
	{
		OrbisDropRefusedCommands(m_device, resources.command_pool, resources.command_buffers.data(),
			resources.init_buffer_used);
		resources.timestamp_written = false;
		if (resources.pipeline_statistics_query != QueryState::None)
			resources.pipeline_statistics_query = QueryState::None;
	}
#endif

	// This command buffer now has commands, so can't be re-used without waiting.
	resources.needs_fence_wait = true;

	u32 spin_cycles = 0;
	const bool spin_enabled = m_spin_timer;
	if (spin_enabled)
	{
		ScanForCommandBufferCompletion();
		auto draw = m_spin_manager.DrawSubmitted(m_command_buffer_render_passes);
		u32 constant_offset =
			400000 * m_spin_manager.SpinsPerUnitTime(); // 400us, just to be safe since going over gets really bad
		if (m_optional_extensions.vk_ext_calibrated_timestamps)
			constant_offset /=
				2; // Safety factor isn't as important here, going over just hurts this one submission a bit
		u32 minimum_spin = 200000 * m_spin_manager.SpinsPerUnitTime();
		u32 maximum_spin = std::max<u32>(1024, 16000000 * m_spin_manager.SpinsPerUnitTime()); // 16ms
		if (draw.recommended_spin > minimum_spin + constant_offset)
			spin_cycles = std::min(draw.recommended_spin - constant_offset, maximum_spin);
		resources.spin_id = draw.id;
	}
	else
	{
		resources.spin_id = -1;
	}
	m_command_buffer_render_passes = 0;

	if (present_swap_chain != VK_NULL_HANDLE && m_spinning_supported)
	{
		m_spin_manager.NextFrame();
		if (m_spin_timer)
			m_spin_timer--;
		// Calibrate a max of once per frame
		m_wants_new_timestamp_calibration = m_optional_extensions.vk_ext_calibrated_timestamps;
	}

	if (spin_cycles != 0)
		WaitForSpinCompletion(m_current_frame);

	if (spin_enabled && m_optional_extensions.vk_ext_calibrated_timestamps)
		resources.submit_timestamp = GetCPUTimestamp();

	uint32_t wait_bits = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSemaphore semas[2];
	VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit_info.commandBufferCount = resources.init_buffer_used ? 2u : 1u;
	submit_info.pCommandBuffers =
		resources.init_buffer_used ? resources.command_buffers.data() : &resources.command_buffers[1];

	if (present_swap_chain)
	{
		submit_info.pWaitSemaphores = present_swap_chain->GetImageAvailableSemaphorePtr();
		submit_info.waitSemaphoreCount = 1;
		submit_info.pWaitDstStageMask = &wait_bits;

		if (spin_cycles != 0)
		{
			semas[0] = present_swap_chain->GetRenderingFinishedSemaphore();
			semas[1] = m_spin_resources[m_current_frame].semaphore;
			submit_info.signalSemaphoreCount = 2;
			submit_info.pSignalSemaphores = semas;
		}
		else
		{
			submit_info.pSignalSemaphores = present_swap_chain->GetRenderingFinishedSemaphorePtr();
			submit_info.signalSemaphoreCount = 1;
		}
	}
	else if (spin_cycles != 0)
	{
		submit_info.signalSemaphoreCount = 1;
		submit_info.pSignalSemaphores = &m_spin_resources[m_current_frame].semaphore;
	}

	{
		ORBIS_VKW(4);
#ifdef ORBIS_VULKAN
		// PS5 port (vk-285-98): the stream buffers' streaming stores (OrbisStreamCopy and the vertex upload's
		// storent) are weakly ordered; this orders them before the submission that lets the GPU read them.
		_mm_sfence();
#endif
		res = vkQueueSubmit(m_graphics_queue, 1, &submit_info, resources.fence);
	}
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkQueueSubmit failed: ");
#ifdef ORBIS_VULKAN
		if (res == VK_ERROR_DEVICE_LOST)
			OrbisVkHangExit("vkQueueSubmit");
#endif
		m_last_submit_failed = true;
		return;
	}
#ifdef ORBIS_VULKAN
	OrbisVkSubmitDone(); // vk-285-113
	if (!OrbisDriverIsRADV())
		OrbisMeasureSubmitWords(m_device, m_orbis_submit_draws); // live-10
	s_orbis_submit++;
	// vk-285-18: `putdata vktrace` asks for the last submits' draws while the game runs. Every
	// 64th submit (about once a second) checks for the flag file; the dump goes to vktrace.txt.
	if ((s_orbis_submit & 63) == 0 && OrbisFlag("vktrace"))
	{
		unlink(OrbisFlagPath("vktrace").c_str());
		char header[96];
		snprintf(header, sizeof(header), "on-demand trace at submit %u (submits %u and %u complete)", s_orbis_submit,
			s_orbis_submit - 2, s_orbis_submit - 1);
		OrbisVkTraceWrite(OrbisLogPath("vktrace.txt").c_str(), header, s_orbis_submit);
		printf("[vkhw] wrote /data/PCSX2/vktrace.txt at submit %u\n", s_orbis_submit);
		fflush(stdout);
	}
#endif

	if (spin_cycles != 0)
		SubmitSpinCommand(m_current_frame, spin_cycles);

	if (present_swap_chain)
	{
		const VkPresentInfoKHR present_info = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1,
			present_swap_chain->GetRenderingFinishedSemaphorePtr(), 1, present_swap_chain->GetSwapChainPtr(),
			present_swap_chain->GetCurrentImageIndexPtr(), nullptr};

		present_swap_chain->ResetImageAcquireResult();

		{
			ORBIS_VKW(5);
			res = vkQueuePresentKHR(m_present_queue, &present_info);
		}
		if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
		{
			// VK_ERROR_OUT_OF_DATE_KHR is not fatal, just means we need to recreate our swap chain.
			if (res == VK_ERROR_OUT_OF_DATE_KHR)
				// Defer until next frame, otherwise resizing would invalidate swapchain before next present.
				m_resize_requested = true;
			else
				LOG_VULKAN_ERROR(res, "vkQueuePresentKHR failed: ");

			return;
		}

		// Grab the next image as soon as possible, that way we spend less time blocked on the next
		// submission. Don't care if it fails, we'll deal with that at the presentation call site.
		// Credit to dxvk for the idea.
		present_swap_chain->AcquireNextImage();
	}
}

void GSDeviceVK::CommandBufferCompleted(u32 index)
{
	FrameResources& resources = m_frame_resources[index];
#ifdef ORBIS_VULKAN
	OrbisGpCollect(index); // vk-285-94
#endif

	for (auto& it : resources.cleanup_resources)
		it();
	resources.cleanup_resources.clear();

	bool wants_timestamps = m_gpu_timing_enabled || resources.spin_id >= 0;

	if (wants_timestamps && resources.timestamp_written)
	{
		std::array<u64, 2> timestamps;
		VkResult res =
			vkGetQueryPoolResults(m_device, m_timestamp_query_pool, index * 2, static_cast<u32>(timestamps.size()),
				sizeof(u64) * timestamps.size(), timestamps.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT);
		if (res == VK_SUCCESS)
		{
			// if we didn't write the timestamp at the start of the cmdbuffer (just enabled timing), the first TS will be zero
			if (timestamps[0] > 0 && m_gpu_timing_enabled)
			{
				const double ns_diff =
					(timestamps[1] - timestamps[0]) * static_cast<double>(m_device_properties.limits.timestampPeriod);
				m_accumulated_gpu_time += ns_diff / 1000000.0;
			}
			if (resources.spin_id >= 0)
			{
				if (m_optional_extensions.vk_ext_calibrated_timestamps && timestamps[1] > 0)
				{
					u64 end = timestamps[1] * m_spin_timestamp_scale + m_spin_timestamp_offset;
					m_spin_manager.DrawCompleted(resources.spin_id, resources.submit_timestamp, end);
				}
				else if (!m_optional_extensions.vk_ext_calibrated_timestamps && timestamps[0] > 0)
				{
					u64 begin = timestamps[0] * m_spin_timestamp_scale;
					u64 end = timestamps[1] * m_spin_timestamp_scale;
					m_spin_manager.DrawCompleted(resources.spin_id, begin, end);
				}
			}
		}
		else
		{
			LOG_VULKAN_ERROR(res, "vkGetQueryPoolResults failed: ");
		}
	}
}

void GSDeviceVK::MoveToNextCommandBuffer()
{
	ActivateCommandBuffer((m_current_frame + 1) % NUM_COMMAND_BUFFERS);
	InvalidateCachedState();
	SetInitialState(m_current_command_buffer);
}

void GSDeviceVK::ActivateCommandBuffer(u32 index)
{
	FrameResources& resources = m_frame_resources[index];

	// Wait for the GPU to finish with all resources for this command buffer.
	if (resources.fence_counter > m_completed_fence_counter)
	{
		ORBIS_VKW(0);
		WaitForCommandBufferCompletion(index);
	}

	// Reset fence to unsignaled before starting.
	VkResult res = vkResetFences(m_device, 1, &resources.fence);
	if (res != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkResetFences failed: ");

	// Reset command pools to beginning since we can re-use the memory now
	res = vkResetCommandPool(m_device, resources.command_pool, 0);
	if (res != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkResetCommandPool failed: ");

	// Enable commands to be recorded to the two buffers again.
	VkCommandBufferBeginInfo begin_info = {
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
	res = vkBeginCommandBuffer(resources.command_buffers[1], &begin_info);
	if (res != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkBeginCommandBuffer failed: ");

	bool wants_timestamp = m_gpu_timing_enabled || m_spin_timer;
	if (wants_timestamp)
	{
		vkCmdResetQueryPool(resources.command_buffers[1], m_timestamp_query_pool, index * 2, 2);
		vkCmdWriteTimestamp(
			resources.command_buffers[1], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, m_timestamp_query_pool, index * 2);
	}

	if (resources.pipeline_statistics_query == QueryState::Ready)
	{
		// Collect the pipeline statistics from the last time this cmdbuffer was used.
		resources.pipeline_statistics_query = QueryState::None;
		GPUPipelineStatistics stats{};
		VkResult res =
			vkGetQueryPoolResults(m_device, m_pipeline_statistics_query_pool, index, 1,
				sizeof(stats), &stats, sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
		if (res == VK_SUCCESS)
		{
			m_accumulated_gpu_pipeline_statistics.vs_invocations += stats.vs_invocations;
			m_accumulated_gpu_pipeline_statistics.ps_invocations += stats.ps_invocations;
		}
		else
		{
			LOG_VULKAN_ERROR(res, "vkGetQueryPoolResults failed: ");
		}
	}

	if (m_gpu_pipeline_statistics_enabled)
	{
		pxAssert(resources.pipeline_statistics_query == QueryState::None);
		resources.pipeline_statistics_query = QueryState::Querying;
		vkCmdResetQueryPool(resources.command_buffers[1], m_pipeline_statistics_query_pool, index, 1);
		vkCmdBeginQuery(resources.command_buffers[1], m_pipeline_statistics_query_pool, index, 0);
	}

	resources.fence_counter = m_next_fence_counter++;
	resources.init_buffer_used = false;
	resources.timestamp_written = wants_timestamp;

	m_current_frame = index;
	m_current_command_buffer = resources.command_buffers[1];
#ifdef ORBIS_VULKAN
	m_orbis_submit_draws = 0; // vk-285-113
#endif

	// using the lower 32 bits of the fence index should be sufficient here, I hope...
	vmaSetCurrentFrameIndex(m_allocator, static_cast<u32>(m_next_fence_counter));
#ifdef ORBIS_VULKAN
	OrbisGpBegin(index); // vk-285-94
#endif
}

void GSDeviceVK::ExecuteCommandBuffer(WaitType wait_for_completion)
{
	if (m_last_submit_failed)
		return;

	const u32 current_frame = m_current_frame;
	SubmitCommandBuffer(nullptr);
	MoveToNextCommandBuffer();

	if (wait_for_completion != WaitType::None)
	{
		ORBIS_VKW(1);
		// Calibrate while we wait
		if (m_wants_new_timestamp_calibration)
			CalibrateSpinTimestamp();
		if (wait_for_completion == WaitType::Spin)
		{
			while (vkGetFenceStatus(m_device, m_frame_resources[current_frame].fence) == VK_NOT_READY)
				ShortSpin();
		}
		WaitForCommandBufferCompletion(current_frame);
	}

	// Push constants need to be refreshed each command buffer.
	m_dirty_flags |= DIRTY_FLAG_VS_PUSH_CONSTANTS;
}

void GSDeviceVK::DeferBufferDestruction(VkBuffer object, VmaAllocation allocation)
{
	FrameResources& resources = m_frame_resources[m_current_frame];
	resources.cleanup_resources.push_back(
		[this, object, allocation]() { vmaDestroyBuffer(m_allocator, object, allocation); });
}

void GSDeviceVK::DeferFramebufferDestruction(VkFramebuffer object)
{
	FrameResources& resources = m_frame_resources[m_current_frame];
	resources.cleanup_resources.push_back([this, object]() { vkDestroyFramebuffer(m_device, object, nullptr); });
}

void GSDeviceVK::DeferImageDestruction(VkImage object, VmaAllocation allocation)
{
	FrameResources& resources = m_frame_resources[m_current_frame];
	resources.cleanup_resources.push_back(
		[this, object, allocation]() { vmaDestroyImage(m_allocator, object, allocation); });
}

void GSDeviceVK::DeferImageViewDestruction(VkImageView object)
{
	FrameResources& resources = m_frame_resources[m_current_frame];
	resources.cleanup_resources.push_back([this, object]() { vkDestroyImageView(m_device, object, nullptr); });
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData)
{
#ifdef ORBIS_VULKAN
	// vk-285-13 (HW renderer bring-up): a refused frame is dropped and the next one usually
	// hits the same refusal, so each distinct message is logged three times, then every 500th.
	{
		static std::mutex s_lock;
		static std::array<std::pair<u64, u32>, 128> s_seen{};
		const char* msg = pCallbackData->pMessage ? pCallbackData->pMessage : "";
		// vk-285-113: "a submission of more than 523503 words" (the driver's stream is full): the class of the hang that follows.
		if (const char* p = std::strstr(msg, "a submission of more than "))
		{
			s_orbis_overflow_words = std::strtoull(p + 25, nullptr, 10);
			if (const char* held = std::strstr(p, "this one holds "))
				s_orbis_overflow_total = std::strtoull(held + 15, nullptr, 10);
			s_orbis_overflow_reports++;
		}
		u64 h = 1469598103934665603ull;
		for (const char* c = msg; *c; c++)
			h = (h ^ static_cast<u8>(*c)) * 1099511628211ull;
		std::lock_guard<std::mutex> guard(s_lock);
		u32 count = 0;
		for (auto& e : s_seen)
		{
			if (e.first == h || e.first == 0)
			{
				e.first = h;
				count = ++e.second;
				break;
			}
		}
		if (count > 3 && (count % 500) != 0)
			return VK_FALSE;
		if (count > 3)
			Console.Error("VK: debug report repeated %u times:", count);
	}
#endif
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
	{
		Console.Error("VK: debug report: (%s) %s",
			pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "", pCallbackData->pMessage);
	}
	else if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
	{
		Console.Warning("VK: debug report: (%s) %s",
			pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "", pCallbackData->pMessage);
	}
	else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
	{
		Console.WriteLn("VK: debug report: (%s) %s",
			pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "", pCallbackData->pMessage);
	}
	else
	{
		DevCon.WriteLn("VK: debug report: (%s) %s",
			pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "", pCallbackData->pMessage);
	}

	return VK_FALSE;
}

bool GSDeviceVK::EnableDebugUtils()
{
	// Already enabled?
	if (m_debug_messenger_callback != VK_NULL_HANDLE)
		return true;

	// Check for presence of the functions before calling
	if (!vkCreateDebugUtilsMessengerEXT || !vkDestroyDebugUtilsMessengerEXT || !vkSubmitDebugUtilsMessageEXT)
	{
		return false;
	}

	VkDebugUtilsMessengerCreateInfoEXT messenger_info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		nullptr, 0,
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
		VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
		DebugMessengerCallback, nullptr};

	const VkResult res =
		vkCreateDebugUtilsMessengerEXT(m_instance, &messenger_info, nullptr, &m_debug_messenger_callback);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateDebugUtilsMessengerEXT failed: ");
		return false;
	}

	return true;
}

void GSDeviceVK::DisableDebugUtils()
{
	if (m_debug_messenger_callback != VK_NULL_HANDLE)
	{
		vkDestroyDebugUtilsMessengerEXT(m_instance, m_debug_messenger_callback, nullptr);
		m_debug_messenger_callback = VK_NULL_HANDLE;
	}
}

VkRenderPass GSDeviceVK::CreateCachedRenderPass(RenderPassCacheKey key)
{
	VkAttachmentReference color_reference;
	VkAttachmentReference* color_reference_ptr = nullptr;
	VkAttachmentReference depth_reference;
	VkAttachmentReference* depth_reference_ptr = nullptr;
	std::array<VkAttachmentReference, 2> input_reference;
	u32 num_subpass_inputs = 0;
	std::array<VkSubpassDependency, 2> subpass_dependency;
	u32 num_subpass_dependencies = 0;
	std::array<VkAttachmentDescription, 2> attachments;
	u32 num_attachments = 0;
	if (key.color_format != VK_FORMAT_UNDEFINED)
	{
		const VkImageLayout layout =
			key.color_feedback_loop ? (UseFeedbackLoopLayout() ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT :
																 VK_IMAGE_LAYOUT_GENERAL) :
									  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[num_attachments] = {0, static_cast<VkFormat>(key.color_format), VK_SAMPLE_COUNT_1_BIT,
			static_cast<VkAttachmentLoadOp>(key.color_load_op), static_cast<VkAttachmentStoreOp>(key.color_store_op),
			VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, layout, layout};
		color_reference.attachment = num_attachments;
		color_reference.layout = layout;
		color_reference_ptr = &color_reference;

		if (key.color_feedback_loop)
		{
			if (!UseFeedbackLoopLayout())
			{
				input_reference[num_subpass_inputs].attachment = num_attachments;
				input_reference[num_subpass_inputs].layout = layout;
				num_subpass_inputs++;
			}

			if (!m_features.framebuffer_fetch)
			{
				// don't need the framebuffer-local dependency when we have rasterization order attachment access
				subpass_dependency[num_subpass_dependencies].srcSubpass = 0;
				subpass_dependency[num_subpass_dependencies].dstSubpass = 0;
				subpass_dependency[num_subpass_dependencies].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				subpass_dependency[num_subpass_dependencies].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				subpass_dependency[num_subpass_dependencies].srcAccessMask =
					VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				subpass_dependency[num_subpass_dependencies].dstAccessMask =
					UseFeedbackLoopLayout() ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
				subpass_dependency[num_subpass_dependencies].dependencyFlags =
					UseFeedbackLoopLayout() ? (VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT) :
											  VK_DEPENDENCY_BY_REGION_BIT;
				num_subpass_dependencies++;
			}
		}

		num_attachments++;
	}
	if (key.depth_format != VK_FORMAT_UNDEFINED)
	{
		const VkImageLayout layout =
			key.depth_sampling ?
				((m_features.depth_feedback && UseFeedbackLoopLayout()) ?
					VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT :
					VK_IMAGE_LAYOUT_GENERAL) :
				VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		attachments[num_attachments] = {0, static_cast<VkFormat>(key.depth_format), VK_SAMPLE_COUNT_1_BIT,
			static_cast<VkAttachmentLoadOp>(key.depth_load_op), static_cast<VkAttachmentStoreOp>(key.depth_store_op),
			static_cast<VkAttachmentLoadOp>(key.stencil_load_op),
			static_cast<VkAttachmentStoreOp>(key.stencil_store_op), layout, layout};
		depth_reference.attachment = num_attachments;
		depth_reference.layout = layout;
		depth_reference_ptr = &depth_reference;

		if (key.depth_sampling)
		{
			if (!UseFeedbackLoopLayout())
			{
				input_reference[num_subpass_inputs].attachment = num_attachments;
				input_reference[num_subpass_inputs].layout = layout;
				num_subpass_inputs++;
			}

			if (!m_features.framebuffer_fetch)
			{
				// don't need the framebuffer-local dependency when we have rasterization order attachment access
				subpass_dependency[num_subpass_dependencies].srcSubpass = 0;
				subpass_dependency[num_subpass_dependencies].dstSubpass = 0;
				subpass_dependency[num_subpass_dependencies].srcStageMask =
					VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
				subpass_dependency[num_subpass_dependencies].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				subpass_dependency[num_subpass_dependencies].srcAccessMask =
					VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				subpass_dependency[num_subpass_dependencies].dstAccessMask =
					UseFeedbackLoopLayout() ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
				subpass_dependency[num_subpass_dependencies].dependencyFlags =
					UseFeedbackLoopLayout() ? (VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT) :
											  VK_DEPENDENCY_BY_REGION_BIT;
				num_subpass_dependencies++;
			}
		}

		num_attachments++;
	}

	const VkSubpassDescriptionFlags subpass_flags =
		(key.color_feedback_loop && m_optional_extensions.vk_ext_rasterization_order_attachment_access) ?
			VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT :
			0;
	const VkSubpassDescription subpass = {subpass_flags, VK_PIPELINE_BIND_POINT_GRAPHICS, num_subpass_inputs,
		num_subpass_inputs ? input_reference.data() : nullptr, color_reference_ptr ? 1u : 0u,
		color_reference_ptr ? color_reference_ptr : nullptr, nullptr, depth_reference_ptr, 0, nullptr};
	const VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0u, num_attachments,
		attachments.data(), 1u, &subpass, num_subpass_dependencies, num_subpass_dependencies ? subpass_dependency.data() : nullptr};

	VkRenderPass pass;
	const VkResult res = vkCreateRenderPass(m_device, &pass_info, nullptr, &pass);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateRenderPass failed: ");
		return VK_NULL_HANDLE;
	}

	m_render_pass_cache.emplace(key.key, pass);
	return pass;
}

static constexpr std::string_view SPIN_SHADER = R"(
#version 460 core

layout(std430, set=0, binding=0) buffer SpinBuffer { uint spin[]; };
layout(push_constant) uniform constants { uint cycles; };
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

void main()
{
	uint value = spin[0];
	// The compiler doesn't know, but spin[0] == 0, so this loop won't actually go anywhere
	for (uint i = 0; i < cycles; i++)
		value = spin[value];
	// Store the result back to the buffer so the compiler can't optimize it away
	spin[0] = value;
}
)";

bool GSDeviceVK::InitSpinResources()
{
	if (!m_spinning_supported)
		return true;

	// TODO: Move to safe destroy functions, use scoped guard.

	VkResult res;
#define CHECKED_CREATE(create_fn, create_struct, output_struct) \
	do \
	{ \
		if ((res = create_fn(m_device, create_struct, nullptr, output_struct)) != VK_SUCCESS) \
		{ \
			LOG_VULKAN_ERROR(res, #create_fn " failed: "); \
			return false; \
		} \
	} while (0)

	VkDescriptorSetLayoutBinding set_layout_binding = {};
	set_layout_binding.binding = 0;
	set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	set_layout_binding.descriptorCount = 1;
	set_layout_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo desc_set_layout_create = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	desc_set_layout_create.bindingCount = 1;
	desc_set_layout_create.pBindings = &set_layout_binding;
	CHECKED_CREATE(vkCreateDescriptorSetLayout, &desc_set_layout_create, &m_spin_descriptor_set_layout);

	const VkPushConstantRange push_constant_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u32)};
	VkPipelineLayoutCreateInfo pl_layout_create = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pl_layout_create.setLayoutCount = 1;
	pl_layout_create.pSetLayouts = &m_spin_descriptor_set_layout;
	pl_layout_create.pushConstantRangeCount = 1;
	pl_layout_create.pPushConstantRanges = &push_constant_range;
	CHECKED_CREATE(vkCreatePipelineLayout, &pl_layout_create, &m_spin_pipeline_layout);

	VkShaderModule shader_module = g_vulkan_shader_cache->GetComputeShader(SPIN_SHADER);
	if (shader_module == VK_NULL_HANDLE)
		return false;

	Vulkan::SetObjectName(m_device, shader_module, "Spin Shader");

	VkComputePipelineCreateInfo pl_create = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	pl_create.layout = m_spin_pipeline_layout;
	pl_create.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pl_create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pl_create.stage.pName = "main";
	pl_create.stage.module = shader_module;
	res = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pl_create, nullptr, &m_spin_pipeline);
	vkDestroyShaderModule(m_device, shader_module, nullptr);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateComputePipelines failed: ");
		return false;
	}
	Vulkan::SetObjectName(m_device, m_spin_pipeline, "Spin Pipeline");

	VmaAllocationCreateInfo buf_vma_create = {};
	buf_vma_create.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	VkBufferCreateInfo buf_create = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	buf_create.size = 4;
	buf_create.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if ((res = vmaCreateBuffer(m_allocator, &buf_create, &buf_vma_create, &m_spin_buffer, &m_spin_buffer_allocation,
			 nullptr)) != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vmaCreateBuffer failed: ");
		return false;
	}
	Vulkan::SetObjectName(m_device, m_spin_buffer, "Spin Buffer");

	VkDescriptorSetAllocateInfo desc_set_allocate = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	desc_set_allocate.descriptorPool = m_global_descriptor_pool;
	desc_set_allocate.descriptorSetCount = 1;
	desc_set_allocate.pSetLayouts = &m_spin_descriptor_set_layout;
	if ((res = vkAllocateDescriptorSets(m_device, &desc_set_allocate, &m_spin_descriptor_set)) != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkAllocateDescriptorSets failed: ");
		return false;
	}
	const VkDescriptorBufferInfo desc_buffer_info = {m_spin_buffer, 0, VK_WHOLE_SIZE};
	VkWriteDescriptorSet desc_set_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	desc_set_write.dstSet = m_spin_descriptor_set;
	desc_set_write.dstBinding = 0;
	desc_set_write.descriptorCount = 1;
	desc_set_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	desc_set_write.pBufferInfo = &desc_buffer_info;
	vkUpdateDescriptorSets(m_device, 1, &desc_set_write, 0, nullptr);

	for (SpinResources& resources : m_spin_resources)
	{
		u32 index = &resources - &m_spin_resources[0];
		VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pool_info.queueFamilyIndex = m_spin_queue_family_index;
		CHECKED_CREATE(vkCreateCommandPool, &pool_info, &resources.command_pool);
		Vulkan::SetObjectName(m_device, resources.command_pool, "Spin Command Pool %u", index);

		VkCommandBufferAllocateInfo buffer_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		buffer_info.commandPool = resources.command_pool;
		buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		buffer_info.commandBufferCount = 1;
		res = vkAllocateCommandBuffers(m_device, &buffer_info, &resources.command_buffer);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkAllocateCommandBuffers failed: ");
			return false;
		}
		Vulkan::SetObjectName(m_device, resources.command_buffer, "Spin Command Buffer %u", index);

		VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		CHECKED_CREATE(vkCreateFence, &fence_info, &resources.fence);
		Vulkan::SetObjectName(m_device, resources.fence, "Spin Fence %u", index);

		if (!m_spin_queue_is_graphics_queue)
		{
			VkSemaphoreCreateInfo sem_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			CHECKED_CREATE(vkCreateSemaphore, &sem_info, &resources.semaphore);
			Vulkan::SetObjectName(m_device, resources.semaphore, "Draw to Spin Semaphore %u", index);
		}
	}

#undef CHECKED_CREATE
	return true;
}

void GSDeviceVK::DestroySpinResources()
{
#define CHECKED_DESTROY(destructor, obj) \
	do \
	{ \
		if (obj != VK_NULL_HANDLE) \
		{ \
			destructor(m_device, obj, nullptr); \
			obj = VK_NULL_HANDLE; \
		} \
	} while (0)

	if (m_spin_buffer)
	{
		vmaDestroyBuffer(m_allocator, m_spin_buffer, m_spin_buffer_allocation);
		m_spin_buffer = VK_NULL_HANDLE;
		m_spin_buffer_allocation = VK_NULL_HANDLE;
	}
	CHECKED_DESTROY(vkDestroyPipeline, m_spin_pipeline);
	CHECKED_DESTROY(vkDestroyPipelineLayout, m_spin_pipeline_layout);
	CHECKED_DESTROY(vkDestroyDescriptorSetLayout, m_spin_descriptor_set_layout);
	if (m_spin_descriptor_set != VK_NULL_HANDLE)
	{
		vkFreeDescriptorSets(m_device, m_global_descriptor_pool, 1, &m_spin_descriptor_set);
		m_spin_descriptor_set = VK_NULL_HANDLE;
	}
	for (SpinResources& resources : m_spin_resources)
	{
		CHECKED_DESTROY(vkDestroySemaphore, resources.semaphore);
		CHECKED_DESTROY(vkDestroyFence, resources.fence);
		if (resources.command_buffer != VK_NULL_HANDLE)
		{
			vkFreeCommandBuffers(m_device, resources.command_pool, 1, &resources.command_buffer);
			resources.command_buffer = VK_NULL_HANDLE;
		}
		CHECKED_DESTROY(vkDestroyCommandPool, resources.command_pool);
	}
#undef CHECKED_DESTROY
}

void GSDeviceVK::WaitForSpinCompletion(u32 index)
{
	SpinResources& resources = m_spin_resources[index];
	if (!resources.in_progress)
		return;

	const VkResult res = vkWaitForFences(m_device, 1, &resources.fence, VK_TRUE, UINT64_MAX);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkWaitForFences failed: ");
		m_last_submit_failed = true;
		return;
	}
	SpinCommandCompleted(index);
}

void GSDeviceVK::SpinCommandCompleted(u32 index)
{
	SpinResources& resources = m_spin_resources[index];
	resources.in_progress = false;
	const u32 timestamp_base = (index + NUM_COMMAND_BUFFERS) * 2;
	std::array<u64, 2> timestamps;
	const VkResult res =
		vkGetQueryPoolResults(m_device, m_timestamp_query_pool, timestamp_base, static_cast<u32>(timestamps.size()),
			sizeof(timestamps), timestamps.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT);
	if (res == VK_SUCCESS)
	{
		u64 begin, end;
		if (m_optional_extensions.vk_ext_calibrated_timestamps)
		{
			begin = timestamps[0] * m_spin_timestamp_scale + m_spin_timestamp_offset;
			end = timestamps[1] * m_spin_timestamp_scale + m_spin_timestamp_offset;
		}
		else
		{
			begin = timestamps[0] * m_spin_timestamp_scale;
			end = timestamps[1] * m_spin_timestamp_scale;
		}
		m_spin_manager.SpinCompleted(resources.cycles, begin, end);
	}
	else
	{
		LOG_VULKAN_ERROR(res, "vkGetQueryPoolResults failed: ");
	}
}

void GSDeviceVK::SubmitSpinCommand(u32 index, u32 cycles)
{
	SpinResources& resources = m_spin_resources[index];
	VkResult res;

	// Reset fence to unsignaled before starting.
	if ((res = vkResetFences(m_device, 1, &resources.fence)) != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkResetFences failed: ");

	// Reset command pools to beginning since we can re-use the memory now
	if ((res = vkResetCommandPool(m_device, resources.command_pool, 0)) != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkResetCommandPool failed: ");

	// Enable commands to be recorded to the two buffers again.
	VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if ((res = vkBeginCommandBuffer(resources.command_buffer, &begin_info)) != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkBeginCommandBuffer failed: ");

	if (!m_spin_buffer_initialized)
	{
		m_spin_buffer_initialized = true;
		vkCmdFillBuffer(resources.command_buffer, m_spin_buffer, 0, VK_WHOLE_SIZE, 0);
		VkBufferMemoryBarrier barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.srcQueueFamilyIndex = m_spin_queue_family_index;
		barrier.dstQueueFamilyIndex = m_spin_queue_family_index;
		barrier.buffer = m_spin_buffer;
		barrier.offset = 0;
		barrier.size = VK_WHOLE_SIZE;
		vkCmdPipelineBarrier(resources.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
	}

	if (m_spin_queue_is_graphics_queue)
		vkCmdPipelineBarrier(resources.command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);

	const u32 timestamp_base = (index + NUM_COMMAND_BUFFERS) * 2;
	vkCmdResetQueryPool(resources.command_buffer, m_timestamp_query_pool, timestamp_base, 2);
	vkCmdWriteTimestamp(
		resources.command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, m_timestamp_query_pool, timestamp_base);
	vkCmdPushConstants(
		resources.command_buffer, m_spin_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(u32), &cycles);
	vkCmdBindPipeline(resources.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spin_pipeline);
	vkCmdBindDescriptorSets(resources.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_spin_pipeline_layout, 0, 1,
		&m_spin_descriptor_set, 0, nullptr);
	vkCmdDispatch(resources.command_buffer, 1, 1, 1);
	vkCmdWriteTimestamp(
		resources.command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_timestamp_query_pool, timestamp_base + 1);

	if ((res = vkEndCommandBuffer(resources.command_buffer)) != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkEndCommandBuffer failed: ");

	VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &resources.command_buffer;
	VkPipelineStageFlags sema_waits[] = {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
	if (!m_spin_queue_is_graphics_queue)
	{
		submit_info.waitSemaphoreCount = 1;
		submit_info.pWaitSemaphores = &resources.semaphore;
		submit_info.pWaitDstStageMask = sema_waits;
	}
	vkQueueSubmit(m_spin_queue, 1, &submit_info, resources.fence);
	resources.in_progress = true;
	resources.cycles = cycles;
}

void GSDeviceVK::CalibrateSpinTimestamp()
{
	if (!m_optional_extensions.vk_ext_calibrated_timestamps)
		return;
	VkCalibratedTimestampInfoEXT infos[2] = {
		{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_DEVICE_EXT},
		{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, m_calibrated_timestamp_type},
	};
	u64 timestamps[2];
	u64 maxDeviation;
	constexpr u64 MAX_MAX_DEVIATION = 100000; // 100us
	for (int i = 0; i < 4; i++) // 4 tries to get under MAX_MAX_DEVIATION
	{
		const VkResult res = vkGetCalibratedTimestampsEXT(m_device, std::size(infos), infos, timestamps, &maxDeviation);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vkGetCalibratedTimestampsEXT failed: ");
			return;
		}
		if (maxDeviation < MAX_MAX_DEVIATION)
			break;
	}
	if (maxDeviation >= MAX_MAX_DEVIATION)
		Console.Warning("vkGetCalibratedTimestampsEXT returned high max deviation of %lluus", maxDeviation / 1000);
	const double gpu_time = timestamps[0] * m_spin_timestamp_scale;
#ifdef _WIN32
	const double cpu_time = timestamps[1] * m_queryperfcounter_to_ns;
#else
	const double cpu_time = timestamps[1];
#endif
	m_spin_timestamp_offset = cpu_time - gpu_time;
}

u64 GSDeviceVK::GetCPUTimestamp()
{
#ifdef _WIN32
	LARGE_INTEGER value = {};
	QueryPerformanceCounter(&value);
	return static_cast<u64>(static_cast<double>(value.QuadPart) * m_queryperfcounter_to_ns);
#else
#ifdef CLOCK_MONOTONIC_RAW
	const bool use_raw = m_calibrated_timestamp_type == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT;
	const clockid_t clock = use_raw ? CLOCK_MONOTONIC_RAW : CLOCK_MONOTONIC;
#else
	const clockid_t clock = CLOCK_MONOTONIC;
#endif
	timespec ts = {};
	clock_gettime(clock, &ts);
	return static_cast<u64>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#endif
}

bool GSDeviceVK::AllocatePreinitializedGPUBuffer(u32 size, VkBuffer* gpu_buffer, VmaAllocation* gpu_allocation,
	VkBufferUsageFlags gpu_usage, const std::function<void(void*)>& fill_callback)
{
	// Try to place the fixed index buffer in GPU local memory.
	// Use the staging buffer to copy into it.

	const VkBufferCreateInfo cpu_bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE};
	const VmaAllocationCreateInfo cpu_aci = {VMA_ALLOCATION_CREATE_MAPPED_BIT, VMA_MEMORY_USAGE_CPU_ONLY, 0, 0};
	VkBuffer cpu_buffer;
	VmaAllocation cpu_allocation;
	VmaAllocationInfo cpu_ai;
	VkResult res = vmaCreateBuffer(m_allocator, &cpu_bci, &cpu_aci, &cpu_buffer, &cpu_allocation, &cpu_ai);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vmaCreateBuffer() for CPU expand buffer failed: ");
		return false;
	}

	const VkBufferCreateInfo gpu_bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size,
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
	const VmaAllocationCreateInfo gpu_aci = {0, VMA_MEMORY_USAGE_GPU_ONLY, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
	VmaAllocationInfo ai;
	res = vmaCreateBuffer(m_allocator, &gpu_bci, &gpu_aci, gpu_buffer, gpu_allocation, &ai);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vmaCreateBuffer() for expand buffer failed: ");
		vmaDestroyBuffer(m_allocator, cpu_buffer, cpu_allocation);
		return false;
	}

	const VkBufferCopy buf_copy = {0u, 0u, size};
	fill_callback(cpu_ai.pMappedData);
	vmaFlushAllocation(m_allocator, cpu_allocation, 0, size);
	vkCmdCopyBuffer(GetCurrentInitCommandBuffer(), cpu_buffer, *gpu_buffer, 1, &buf_copy);
	DeferBufferDestruction(cpu_buffer, cpu_allocation);
	return true;
}


std::vector<GSAdapterInfo> GSDeviceVK::GetAdapterInfo()
{
	GPUList gpus = EnumerateGPUs();
	std::vector<GSAdapterInfo> ret;
	ret.reserve(gpus.size());
	for (auto& [physical_device, ai] : gpus)
		ret.push_back(std::move(ai));
	return ret;
}

bool GSDeviceVK::IsSuitableDefaultRenderer()
{
	GPUList gpus = EnumerateGPUs();
	if (gpus.empty())
	{
		// No adapters, not gonna be able to use VK.
		return false;
	}

	// Check the first GPU, should be enough.
	const std::string& name = gpus.front().second.name;
	INFO_LOG("Using Vulkan GPU '{}' for automatic renderer check.", name);

	// Any software rendering (LLVMpipe, SwiftShader).
	if (StringUtil::StartsWithNoCase(name, "llvmpipe") || StringUtil::StartsWithNoCase(name, "SwiftShader"))
	{
		Console.WriteLn(Color_StrongOrange, "Not using Vulkan for software renderer.");
		return false;
	}

	// For Intel, OpenGL usually ends up faster on Linux, because of fbfetch.
	// Plus, the Ivy Bridge and Haswell drivers are incomplete.
	if (StringUtil::StartsWithNoCase(name, "Intel"))
	{
		Console.WriteLn(Color_StrongOrange, "Not using Vulkan for Intel GPU.");
		return false;
	}

	Console.WriteLn(Color_StrongGreen, "Allowing Vulkan as default renderer.");
	return true;
}

RenderAPI GSDeviceVK::GetRenderAPI() const
{
	return RenderAPI::Vulkan;
}

bool GSDeviceVK::HasSurface() const
{
	return static_cast<bool>(m_swap_chain);
}

bool GSDeviceVK::Create(GSVSyncMode vsync_mode, bool allow_present_throttle)
{
	if (!GSDevice::Create(vsync_mode, allow_present_throttle))
		return false;

	if (!CreateDeviceAndSwapChain())
		return false;

	if (!CheckFeatures())
	{
		Host::ReportErrorAsync("GS", TRANSLATE_SV("GSDeviceVK", "Your GPU does not support the required Vulkan features."));
		return false;
	}

	if (!CreateNullTexture())
	{
		Host::ReportErrorAsync("GS", "Failed to create dummy texture");
		return false;
	}

	if ((m_null_framebuffer = GSTextureVK::CreateNullFramebuffer(m_max_framebuffer_width, m_max_framebuffer_height)) == VK_NULL_HANDLE)
	{
		Host::ReportErrorAsync("GS", "Failed to create dummy framebuffer");
		return false;
	}

	{
		std::optional<std::string> shader = ReadShaderSource("shaders/vulkan/tfx.glsl");
		if (!shader.has_value())
		{
			Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/tfx.glsl.");
			return false;
		}

		m_tfx_source = std::move(*shader);
	}

	if (!CreatePipelineLayouts())
	{
		Host::ReportErrorAsync("GS", "Failed to create pipeline layouts");
		return false;
	}

	if (!CreateRenderPasses())
	{
		Host::ReportErrorAsync("GS", "Failed to create render passes");
		return false;
	}

	if (!CreateBuffers())
		return false;

	if (!CompileConvertPipelines() || !CompilePresentPipelines() || !CompileInterlacePipelines() ||
		!CompileMergePipelines() || !CompilePostProcessingPipelines() || !InitSpinResources())
	{
		Host::ReportErrorAsync("GS", "Failed to compile utility pipelines");
		return false;
	}

	if (!CreatePersistentDescriptorSets())
	{
		Host::ReportErrorAsync("GS", "Failed to create persistent descriptor sets");
		return false;
	}

	if (!CompileCASPipelines())
		return false;

	if (!CompileImGuiPipeline())
		return false;

	InitializeState();
	return true;
}

void GSDeviceVK::Destroy()
{
	std::unique_lock lock(s_instance_mutex);

	GSDevice::Destroy();

	EndRenderPass();
	if (GetCurrentCommandBuffer() != VK_NULL_HANDLE)
	{
		ExecuteCommandBuffer(false);
		WaitForGPUIdle();
	}

#ifdef ORBIS_VULKAN
	OrbisFrameGenRelease(); // its images and pipelines before the device (2026-10-08)
#endif
	m_swap_chain.reset();

	if (m_device != VK_NULL_HANDLE)
	{
		DestroySpinResources();
		DestroyResources();
	}

	VKShaderCache::Destroy();

	if (m_device != VK_NULL_HANDLE)
		vkDestroyDevice(m_device, nullptr);

	if (m_debug_messenger_callback != VK_NULL_HANDLE)
		DisableDebugUtils();

	if (m_instance != VK_NULL_HANDLE)
		vkDestroyInstance(m_instance, nullptr);

	Vulkan::UnloadVulkanLibrary();
}

bool GSDeviceVK::UpdateWindow()
{
	DestroySurface();

	if (!AcquireWindow(false))
		return false;

	if (m_window_info.type == WindowInfo::Type::Surfaceless)
		return true;

	// make sure previous frames are presented
	ExecuteCommandBuffer(false);
	WaitForGPUIdle();

	// recreate surface in existing swap chain if it already exists
	if (m_swap_chain)
	{
		if (m_swap_chain->RecreateSurface(m_window_info))
		{
			m_window_info = m_swap_chain->GetWindowInfo();
			return true;
		}

		m_swap_chain.reset();
	}

	VkSurfaceKHR surface = VKSwapChain::CreateVulkanSurface(m_instance, m_physical_device, &m_window_info);
	if (surface == VK_NULL_HANDLE)
	{
		Console.Error("VK: Failed to create new surface for swap chain");
		return false;
	}

	VkPresentModeKHR present_mode;
	if (!VKSwapChain::SelectPresentMode(surface, &m_vsync_mode, &present_mode) ||
		!(m_swap_chain = VKSwapChain::Create(m_window_info, surface, present_mode,
			  Pcsx2Config::GSOptions::TriStateToOptionalBoolean(GSConfig.ExclusiveFullscreenControl))))
	{
		Console.Error("VK: Failed to create swap chain");
		VKSwapChain::DestroyVulkanSurface(m_instance, &m_window_info, surface);
		return false;
	}

	m_window_info = m_swap_chain->GetWindowInfo();
	RenderBlankFrame();
	InvalidateCachedState();
	SetInitialState(m_current_command_buffer);
	return true;
}

void GSDeviceVK::ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale)
{
	m_resize_requested = false;

	if (!m_swap_chain || (m_swap_chain->GetWidth() == new_window_width &&
							 m_swap_chain->GetHeight() == new_window_height))
	{
		// skip unnecessary resizes
		m_window_info.surface_scale = new_window_scale;
		return;
	}

	// make sure previous frames are presented
	WaitForGPUIdle();

	if (!m_swap_chain->ResizeSwapChain(new_window_width, new_window_height, new_window_scale))
	{
		// AcquireNextImage() will fail, and we'll recreate the surface.
		Console.Error("VK: Failed to resize swap chain. Next present will fail.");
		return;
	}

	m_window_info = m_swap_chain->GetWindowInfo();
}

bool GSDeviceVK::SupportsExclusiveFullscreen() const
{
	return false;
}

void GSDeviceVK::DestroySurface()
{
	WaitForGPUIdle();
	m_swap_chain.reset();
}

std::string GSDeviceVK::GetDriverInfo() const
{
	std::string ret;
	const u32 api_version = m_device_properties.apiVersion;
	const u32 driver_version = m_device_properties.driverVersion;
	if (m_optional_extensions.vk_khr_driver_properties)
	{
		const VkPhysicalDeviceDriverProperties& props = m_device_driver_properties;
		ret = StringUtil::StdStringFromFormat(
			"Driver %u.%u.%u\nVulkan %u.%u.%u\nConformance Version %u.%u.%u.%u\n%s\n%s\n%s",
			VK_VERSION_MAJOR(driver_version), VK_VERSION_MINOR(driver_version), VK_VERSION_PATCH(driver_version),
			VK_API_VERSION_MAJOR(api_version), VK_API_VERSION_MINOR(api_version), VK_API_VERSION_PATCH(api_version),
			props.conformanceVersion.major, props.conformanceVersion.minor, props.conformanceVersion.subminor,
			props.conformanceVersion.patch, props.driverInfo, props.driverName, m_device_properties.deviceName);
	}
	else
	{
		ret = StringUtil::StdStringFromFormat("Driver %u.%u.%u\nVulkan %u.%u.%u\n%s", VK_VERSION_MAJOR(driver_version),
			VK_VERSION_MINOR(driver_version), VK_VERSION_PATCH(driver_version), VK_API_VERSION_MAJOR(api_version),
			VK_API_VERSION_MINOR(api_version), VK_API_VERSION_PATCH(api_version), m_device_properties.deviceName);
	}

	return ret;
}

void GSDeviceVK::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	m_allow_present_throttle = allow_present_throttle;
	if (!m_swap_chain)
	{
		// For when it is re-created.
		m_vsync_mode = mode;
		return;
	}

	VkPresentModeKHR present_mode;
	if (!VKSwapChain::SelectPresentMode(m_swap_chain->GetSurface(), &mode, &present_mode))
	{
		ERROR_LOG("Ignoring vsync mode change.");
		return;
	}

	// Actually changed? If using a fallback, it might not have.
	if (m_vsync_mode == mode)
		return;

	m_vsync_mode = mode;

	// This swap chain should not be used by the current buffer, thus safe to destroy.
	WaitForGPUIdle();
	if (!m_swap_chain->SetPresentMode(present_mode))
	{
		pxFailRel("Failed to update swap chain present mode.");
		m_swap_chain.reset();
	}
}

GSDevice::PresentResult GSDeviceVK::BeginPresent(bool frame_skip)
{
	EndRenderPass();

	// Check if the device was lost.
	if (m_last_submit_failed)
		return PresentResult::DeviceLost;

	if (frame_skip)
		return PresentResult::FrameSkipped;

	// If we're running surfaceless, kick the command buffer so we don't run out of descriptors.
	if (!m_swap_chain)
	{
		ExecuteCommandBuffer(false);
		return PresentResult::FrameSkipped;
	}

	// End the pipeline statistics for this cmdbuffer before postprocessing.
	FrameResources& resources = m_frame_resources[m_current_frame];
	if (resources.pipeline_statistics_query == QueryState::Querying)
	{
		resources.pipeline_statistics_query = QueryState::Ready;
		vkCmdEndQuery(m_current_command_buffer, m_pipeline_statistics_query_pool, m_current_frame);
	}

	VkResult res = m_resize_requested ? VK_ERROR_OUT_OF_DATE_KHR : m_swap_chain->AcquireNextImage();
	if (res != VK_SUCCESS)
	{
		m_swap_chain->ReleaseCurrentImage();

		if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
		{
			ResizeWindow(0, 0, m_window_info.surface_scale);
			ImGuiManager::WindowResized();
			res = m_swap_chain->AcquireNextImage();
		}
		else if (res == VK_ERROR_SURFACE_LOST_KHR)
		{
			Console.Warning("VK: Surface lost, attempting to recreate");
			if (!m_swap_chain->RecreateSurface(m_window_info))
			{
				Console.Error("VK: Failed to recreate surface after loss");
				ExecuteCommandBuffer(false);
				return PresentResult::FrameSkipped;
			}

			res = m_swap_chain->AcquireNextImage();
		}
		else
			LOG_VULKAN_ERROR(res, "vkAcquireNextImageKHR() failed: ");

		// This can happen when multiple resize events happen in quick succession.
		// In this case, just wait until the next frame to try again.
		if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
		{
			// Still submit the command buffer, otherwise we'll end up with several frames waiting.
			ExecuteCommandBuffer(false);
			return PresentResult::FrameSkipped;
		}
	}

	VkCommandBuffer cmdbuffer = GetCurrentCommandBuffer();

	// Swap chain images start in undefined
	GSTextureVK* swap_chain_texture = m_swap_chain->GetCurrentTexture();
	swap_chain_texture->OverrideImageLayout(GSTextureVK::Layout::Undefined);
	swap_chain_texture->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ColorAttachment);

	// Present render pass gets started out here, so we can't transition source textures in DoStretchRect
	// Make sure they're ready now
	if (!frame_skip && m_current)
		static_cast<GSTextureVK*>(m_current)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);

	const VkFramebuffer fb = swap_chain_texture->GetFramebuffer(false);
	if (fb == VK_NULL_HANDLE)
		return GSDevice::PresentResult::FrameSkipped;

	const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,
		GetRenderPass(swap_chain_texture->GetVkFormat(), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR,
			VK_ATTACHMENT_STORE_OP_STORE),
		fb,
		{{0, 0}, {static_cast<u32>(swap_chain_texture->GetWidth()), static_cast<u32>(swap_chain_texture->GetHeight())}},
		1u, &s_present_clear_color};
	vkCmdBeginRenderPass(GetCurrentCommandBuffer(), &rp, VK_SUBPASS_CONTENTS_INLINE);

	const VkViewport vp{0.0f, 0.0f, static_cast<float>(swap_chain_texture->GetWidth()),
		static_cast<float>(swap_chain_texture->GetHeight()), 0.0f, 1.0f};
	const VkRect2D scissor{
		{0, 0}, {static_cast<u32>(swap_chain_texture->GetWidth()), static_cast<u32>(swap_chain_texture->GetHeight())}};
	vkCmdSetViewport(GetCurrentCommandBuffer(), 0, 1, &vp);
	vkCmdSetScissor(GetCurrentCommandBuffer(), 0, 1, &scissor);
	m_is_presenting = true;
	return PresentResult::OK;
}

void GSDeviceVK::EndPresent()
{
	RenderImGui();

	VkCommandBuffer cmdbuffer = GetCurrentCommandBuffer();
	vkCmdEndRenderPass(cmdbuffer);
	m_is_presenting = false;
	m_swap_chain->GetCurrentTexture()->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::PresentSrc);
	g_perfmon.Put(GSPerfMon::RenderPasses, 1);

	SubmitCommandBuffer(m_swap_chain.get());
	MoveToNextCommandBuffer();

	InvalidateCachedState();
}

bool GSDeviceVK::IsPresenting() const
{
	return m_is_presenting;
}

#ifdef ENABLE_OGL_DEBUG
static std::array<float, 3> Palette(float phase, const std::array<float, 3>& a, const std::array<float, 3>& b,
	const std::array<float, 3>& c, const std::array<float, 3>& d)
{
	std::array<float, 3> result;
	result[0] = a[0] + b[0] * std::cos(6.28318f * (c[0] * phase + d[0]));
	result[1] = a[1] + b[1] * std::cos(6.28318f * (c[1] * phase + d[1]));
	result[2] = a[2] + b[2] * std::cos(6.28318f * (c[2] * phase + d[2]));
	return result;
}
#endif

void GSDeviceVK::PushDebugGroup(const char* fmt, ...)
{
#ifdef ENABLE_OGL_DEBUG
	if (!vkCmdBeginDebugUtilsLabelEXT || !GSConfig.UseDebugDevice)
		return;

	std::va_list ap;
	va_start(ap, fmt);
	const std::string buf(StringUtil::StdStringFromFormatV(fmt, ap));
	va_end(ap);

	const std::array<float, 3> color = Palette(
		++s_debug_scope_depth, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 0.5f}, {0.8f, 0.90f, 0.30f});

	const VkDebugUtilsLabelEXT label = {
		VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
		nullptr,
		buf.c_str(),
		{color[0], color[1], color[2], 1.0f},
	};
	vkCmdBeginDebugUtilsLabelEXT(GetCurrentCommandBuffer(), &label);
#endif
}

void GSDeviceVK::PopDebugGroup()
{
#ifdef ENABLE_OGL_DEBUG
	if (!vkCmdEndDebugUtilsLabelEXT || !GSConfig.UseDebugDevice)
		return;

	s_debug_scope_depth = (s_debug_scope_depth == 0) ? 0 : (s_debug_scope_depth - 1u);

	vkCmdEndDebugUtilsLabelEXT(GetCurrentCommandBuffer());
#endif
}

void GSDeviceVK::InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...)
{
#ifdef ENABLE_OGL_DEBUG
	if (!vkCmdInsertDebugUtilsLabelEXT || !GSConfig.UseDebugDevice)
		return;

	std::va_list ap;
	va_start(ap, fmt);
	const std::string buf(StringUtil::StdStringFromFormatV(fmt, ap));
	va_end(ap);

	if (buf.empty())
		return;

	static constexpr float colors[][3] = {
		{0.1f, 0.1f, 0.0f}, // Cache
		{0.1f, 0.1f, 0.0f}, // Reg
		{0.5f, 0.0f, 0.5f}, // Debug
		{0.0f, 0.5f, 0.5f}, // Message
		{0.0f, 0.2f, 0.0f} // Performance
	};

	const VkDebugUtilsLabelEXT label = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, nullptr, buf.c_str(),
		{colors[static_cast<int>(category)][0], colors[static_cast<int>(category)][1],
			colors[static_cast<int>(category)][2], 1.0f}};
	vkCmdInsertDebugUtilsLabelEXT(GetCurrentCommandBuffer(), &label);
#endif
}

bool GSDeviceVK::CreateDeviceAndSwapChain()
{
	std::unique_lock lock(s_instance_mutex);
#ifdef ORBIS_VULKAN
	// PS5: the driver reports why it refuses something through VK_EXT_debug_utils
	// only (Mesa drops the message when no messenger is registered), so always
	// register one: its messages land in PCSX2's log. No validation layer exists.
	bool enable_debug_utils = true;
#else
	bool enable_debug_utils = GSConfig.UseDebugDevice;
#endif
	bool enable_validation_layer = GSConfig.UseDebugDevice;

	Error error;
	if (!Vulkan::LoadVulkanLibrary(&error))
	{
		Error::AddPrefix(&error, "Failed to load Vulkan library. Does your GPU and/or driver support Vulkan?\nThe error was:\n");
		Host::ReportErrorAsync("Error", error.GetDescription());
		return false;
	}

	if (!AcquireWindow(true))
		return false;

	m_instance = CreateVulkanInstance(m_window_info, &m_optional_extensions, enable_debug_utils, enable_validation_layer);
	if (m_instance == VK_NULL_HANDLE)
	{
		if (enable_debug_utils || enable_validation_layer)
		{
			// Try again without the validation layer.
			enable_debug_utils = false;
			enable_validation_layer = false;
			m_instance = CreateVulkanInstance(m_window_info, &m_optional_extensions, enable_debug_utils, enable_validation_layer);
			if (m_instance == VK_NULL_HANDLE)
			{
				Host::ReportErrorAsync("Error", "Failed to create Vulkan instance. Does your GPU and/or driver support Vulkan?");
				return false;
			}

			ERROR_LOG("VK: validation/debug layers requested but are unavailable. Creating non-debug device.");
		}
	}

	if (!Vulkan::LoadVulkanInstanceFunctions(m_instance))
	{
		ERROR_LOG("Failed to load Vulkan instance functions");
		return false;
	}

	GPUList gpus = EnumerateGPUs(m_instance);
	if (gpus.empty())
	{
		Host::ReportErrorAsync("Error", "No physical devices found. Does your GPU and/or driver support Vulkan?");
		return false;
	}

	const bool is_default_gpu = GSConfig.Adapter == GetDefaultAdapter();
	if (!(GSConfig.Adapter.empty() || is_default_gpu))
	{
		u32 gpu_index = 0;
		for (; gpu_index < static_cast<u32>(gpus.size()); gpu_index++)
		{
			DEV_LOG("GPU {}: {}", gpu_index, gpus[gpu_index].second.name);
			if (gpus[gpu_index].second.name == GSConfig.Adapter)
			{
				m_physical_device = gpus[gpu_index].first;
				break;
			}
		}

		if (gpu_index == static_cast<u32>(gpus.size()))
		{
			WARNING_LOG("Requested GPU '{}' not found, using first ({})", GSConfig.Adapter, gpus[0].second.name);
			m_physical_device = gpus[0].first;
		}
	}
	else
	{
		INFO_LOG("{} GPU requested, using first ({})", is_default_gpu ? "Default" : "No", gpus[0].second.name);
		m_physical_device = gpus[0].first;
	}

	// Read device physical memory properties, we need it for allocating buffers
	vkGetPhysicalDeviceProperties(m_physical_device, &m_device_properties);

	// Stores the GPU name
	m_name = m_device_properties.deviceName;

	// We need this to be at least 32 byte aligned for AVX2 stores.
	m_device_properties.limits.minUniformBufferOffsetAlignment =
		std::max(m_device_properties.limits.minUniformBufferOffsetAlignment, static_cast<VkDeviceSize>(32));
#ifdef ORBIS_VULKAN
	// vk-285-115 (AI-assisted): OrbisUploadUniform (flags/ntstore) writes whole 64-byte lines, which ps5vk's 256-byte alignment always
	// leaves room for; RADV reports 4, so its uniform blocks are put 64 bytes apart too.
	if (OrbisDriverIsRADV())
		m_device_properties.limits.minUniformBufferOffsetAlignment =
			std::max(m_device_properties.limits.minUniformBufferOffsetAlignment, static_cast<VkDeviceSize>(64));
#endif
	m_device_properties.limits.minTexelBufferOffsetAlignment =
		std::max(m_device_properties.limits.minTexelBufferOffsetAlignment, static_cast<VkDeviceSize>(32));
	m_device_properties.limits.optimalBufferCopyOffsetAlignment =
		std::max(m_device_properties.limits.optimalBufferCopyOffsetAlignment, static_cast<VkDeviceSize>(32));
	m_device_properties.limits.optimalBufferCopyRowPitchAlignment = std::bit_ceil(
		std::max(m_device_properties.limits.optimalBufferCopyRowPitchAlignment, static_cast<VkDeviceSize>(32)));
	m_device_properties.limits.bufferImageGranularity =
		std::max(m_device_properties.limits.bufferImageGranularity, static_cast<VkDeviceSize>(32));

	if (enable_debug_utils)
		EnableDebugUtils();

	VkSurfaceKHR surface = VK_NULL_HANDLE;
	ScopedGuard surface_cleanup = [this, &surface]() {
		if (surface != VK_NULL_HANDLE)
			vkDestroySurfaceKHR(m_instance, surface, nullptr);
	};
	if (m_window_info.type != WindowInfo::Type::Surfaceless)
	{
		surface = VKSwapChain::CreateVulkanSurface(m_instance, m_physical_device, &m_window_info);
		if (surface == VK_NULL_HANDLE)
			return false;
	}

	// Attempt to create the device.
	if (!CreateDevice(surface, enable_validation_layer))
		return false;

	// And critical resources.
	if (!CreateAllocator() || !CreateGlobalDescriptorPool() || !CreateCommandBuffers())
		return false;

	VKShaderCache::Create();

	if (surface != VK_NULL_HANDLE)
	{
		VkPresentModeKHR present_mode;
		if (!VKSwapChain::SelectPresentMode(surface, &m_vsync_mode, &present_mode) ||
			!(m_swap_chain = VKSwapChain::Create(m_window_info, surface, present_mode,
				  Pcsx2Config::GSOptions::TriStateToOptionalBoolean(GSConfig.ExclusiveFullscreenControl))))
		{
			ERROR_LOG("Failed to create swap chain");
			return false;
		}

		// NOTE: This is assigned afterwards, because some platforms can modify the window info (e.g. Metal).
		m_window_info = m_swap_chain->GetWindowInfo();
	}

	surface_cleanup.Cancel();

	// Render a frame as soon as possible to clear out whatever was previously being displayed.
	if (m_window_info.type != WindowInfo::Type::Surfaceless)
		RenderBlankFrame();

	return true;
}

bool GSDeviceVK::CheckFeatures()
{
	const VkPhysicalDeviceLimits& limits = m_device_properties.limits;
	//const u32 vendorID = m_device_properties.vendorID;
	//const bool isAMD = (vendorID == 0x1002 || vendorID == 0x1022);
	//const bool isNVIDIA = (vendorID == 0x10DE);

	m_features.framebuffer_fetch =
		m_optional_extensions.vk_ext_rasterization_order_attachment_access && !GSConfig.DisableFramebufferFetch;
	m_features.texture_barrier = GSConfig.OverrideTextureBarriers != 0;
	m_features.multidraw_fb_copy = false;
	m_features.broken_point_sampler = false;

	// geometryShader is needed because gl_PrimitiveID is part of the Geometry SPIR-V Execution Model.
	m_features.primitive_id = m_device_features.geometryShader;

	m_features.prefer_new_textures = true;
	m_features.provoking_vertex_last = m_optional_extensions.vk_ext_provoking_vertex;
	m_features.vs_expand = !GSConfig.DisableVertexShaderExpand;

	if (!m_features.texture_barrier)
		Console.Warning("VK: Texture buffers are disabled. This may break some graphical effects.");

	// Test for D32S8 support.
	{
		VkFormatProperties props = {};
		vkGetPhysicalDeviceFormatProperties(m_physical_device, VK_FORMAT_D32_SFLOAT_S8_UINT, &props);
		m_features.stencil_buffer =
			((props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0);
	}

	// Fbfetch is useless if we don't have barriers enabled.
	m_features.framebuffer_fetch &= m_features.texture_barrier;

	// Buggy drivers with broken barriers probably have no chance using GENERAL layout for depth either...
	m_features.test_and_sample_depth = true;

	// Use D32F depth instead of D32S8 when we have framebuffer fetch.
	m_features.stencil_buffer &= !m_features.framebuffer_fetch;
#ifdef ORBIS_VULKAN
	// vk-285-13 (HW renderer bring-up): D32F depth. The PS5 driver copies and clears a
	// D32_SFLOAT image's depth (and samples it), while its D32_SFLOAT_S8_UINT entry has no
	// transfer yet, and PCSX2 creates every texture with TRANSFER_SRC|DST. Without a
	// stencil buffer, DATE uses the texture-barrier path.
	// vk-285-115: RADV has the D32S8 format complete, so PCSX2's own choice stands there (the stencil DATE paths).
	if (!OrbisDriverIsRADV())
		m_features.stencil_buffer = false;
#endif

	// whether we can do point/line expand depends on the range of the device
	const float f_upscale = static_cast<float>(GSConfig.UpscaleMultiplier);
	m_features.point_expand = (m_device_features.largePoints && limits.pointSizeRange[0] <= f_upscale &&
							   limits.pointSizeRange[1] >= f_upscale);
	m_features.line_expand =
		(m_device_features.wideLines && limits.lineWidthRange[0] <= f_upscale && limits.lineWidthRange[1] >= f_upscale);

	m_features.depth_feedback = m_features.feedback_loops();
	m_features.aa1 = GSConfig.HWAA1 && m_features.vs_expand && m_features.feedback_loops();

	DevCon.WriteLn("Optional features:%s%s%s%s%s", m_features.primitive_id ? " primitive_id" : "",
		m_features.texture_barrier ? " texture_barrier" : "", m_features.framebuffer_fetch ? " framebuffer_fetch" : "",
		m_features.provoking_vertex_last ? " provoking_vertex_last" : "", m_features.vs_expand ? " vs_expand" : "");

	DevCon.WriteLn("Using %s for point expansion and %s for line expansion.",
		m_features.point_expand ? "hardware" : "vertex expanding",
		m_features.line_expand ? "hardware" : "vertex expanding");

	bool has_rov_storage_flags = true;

	// Check texture format support before we try to create them.
	for (u32 fmt = static_cast<u32>(GSTexture::Format::Color); fmt < static_cast<u32>(GSTexture::Format::PrimID); fmt++)
	{
		const VkFormat vkfmt = LookupNativeFormat(static_cast<GSTexture::Format>(fmt));
		VkFormatFeatureFlags bits = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

		if (static_cast<GSTexture::Format>(fmt) == GSTexture::Format::DepthStencil)
			bits |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
		else
			bits |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;

		VkFormatProperties props = {};
		vkGetPhysicalDeviceFormatProperties(m_physical_device, vkfmt, &props);
		if ((props.optimalTilingFeatures & bits) != bits)
		{
			Host::ReportFormattedErrorAsync("VK: Renderer Unavailable",
				"Required format %u is missing bits, you may need to update your driver. (vk:%u, has:0x%x, needs:0x%x)",
				fmt, static_cast<unsigned>(vkfmt), props.optimalTilingFeatures, bits);
			return false;
		}

		if (GSTexture::IsShaderWriteFormat(static_cast<GSTexture::Format>(fmt)))
			has_rov_storage_flags &= ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0);
	}

	m_features.dxt_textures = m_device_features.textureCompressionBC;
	m_features.bptc_textures = m_device_features.textureCompressionBC;

	if (!m_features.texture_barrier && !m_features.stencil_buffer)
	{
		Host::AddKeyedOSDMessage("GSDeviceVK_NoTextureBarrierOrStencilBuffer",
			TRANSLATE_STR("GS",
				"Stencil buffers and texture barriers are both unavailable, this will break some graphical effects."),
			Host::OSD_WARNING_DURATION);
	}

	m_max_texture_size = m_device_properties.limits.maxImageDimension2D;
	m_max_framebuffer_width = m_device_properties.limits.maxFramebufferWidth;
	m_max_framebuffer_height = m_device_properties.limits.maxFramebufferHeight;

	m_features.rov = m_optional_extensions.vk_ext_fragment_shader_interlock &&
	                 m_device_features.fragmentStoresAndAtomics &&
	                 has_rov_storage_flags &&
	                 !m_features.framebuffer_fetch;

	return true;
}

void GSDeviceVK::DrawPrimitive()
{
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
#ifdef ORBIS_VULKAN
	m_orbis_submit_draws++;
#endif
	vkCmdDraw(GetCurrentCommandBuffer(), m_vertex.count, 1, m_vertex.start, 0);
}

void GSDeviceVK::DrawIndexedPrimitive()
{
	DrawIndexedPrimitive(0, m_index.count);
}

void GSDeviceVK::DrawIndexedPrimitive(int offset, int count)
{
	pxAssert(offset + count <= (int)m_index.count);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
#ifdef ORBIS_VULKAN
	m_orbis_submit_draws++;
#endif
	vkCmdDrawIndexed(GetCurrentCommandBuffer(), count, 1, m_index.start + offset, m_vertex.start, 0);
}

void GSDeviceVK::DrawIndexedPrimitiveVSExpand(int offset, int count, bool vs_indexing, int vs_indexing_expansion)
{
	pxAssert(offset + count <= (int)m_index.count);

	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
#ifdef ORBIS_VULKAN
	m_orbis_submit_draws++;
#endif
	if (vs_indexing)
	{
		SetVSPushConstants(m_vertex.start, m_index.start + offset);
		vkCmdDraw(GetCurrentCommandBuffer(), count * vs_indexing_expansion, 1, 0, 0);
	}
	else
	{
		SetVSPushConstants(m_vertex.start);
		vkCmdDrawIndexed(GetCurrentCommandBuffer(), count, 1, m_index.start + offset, 0, 0);
	}
}

void GSDeviceVK::Draw(const GSHWDrawConfig& config, int offset, int count)
{
	if (config.vs.expand != GSHWDrawConfig::VSExpand::None)
	{
		const bool vs_indexing = config.vs.UseVSExpandIndexBuffer();
		const u32 vs_indexing_expansion = GetExpansionFactor(config.vs.expand);
		DrawIndexedPrimitiveVSExpand(offset, count, vs_indexing, vs_indexing_expansion);
	}
	else
	{
		DrawIndexedPrimitive(offset, count);
	}
}

void GSDeviceVK::Draw(const GSHWDrawConfig& config)
{
	Draw(config, 0, m_index.count);
}

VkFormat GSDeviceVK::LookupNativeFormat(GSTexture::Format format) const
{
	static constexpr std::array<VkFormat, static_cast<int>(GSTexture::Format::Last) + 1> s_format_mapping = {{
		VK_FORMAT_UNDEFINED, // Invalid
		VK_FORMAT_R8G8B8A8_UNORM, // Color
		VK_FORMAT_A2B10G10R10_UNORM_PACK32, // ColorHQ
		VK_FORMAT_R16G16B16A16_SFLOAT, // ColorHDR
		VK_FORMAT_R16G16B16A16_UNORM, // ColorClip
		VK_FORMAT_D32_SFLOAT_S8_UINT, // DepthStencil
		VK_FORMAT_R32_SFLOAT, // DepthColor
		VK_FORMAT_R8_UNORM, // UNorm8
		VK_FORMAT_R16_UINT, // UInt16
		VK_FORMAT_R32_UINT, // UInt32
		VK_FORMAT_R32_SFLOAT, // PrimID
		VK_FORMAT_BC1_RGBA_UNORM_BLOCK, // BC1
		VK_FORMAT_BC2_UNORM_BLOCK, // BC2
		VK_FORMAT_BC3_UNORM_BLOCK, // BC3
		VK_FORMAT_BC7_UNORM_BLOCK, // BC7
	}};

	return (format != GSTexture::Format::DepthStencil || m_features.stencil_buffer) ?
		s_format_mapping[static_cast<int>(format)] :
		VK_FORMAT_D32_SFLOAT;
}

GSTexture* GSDeviceVK::CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format)
{
	std::unique_ptr<GSTexture> tex = GSTextureVK::Create(usage, format, width, height, levels);
	if (!tex)
	{
		// We're probably out of vram, try flushing the command buffer to release pending textures.
		PurgePool();
		ExecuteCommandBufferAndRestartRenderPass(true, "Couldn't allocate texture.");
		tex = GSTextureVK::Create(usage, format, width, height, levels);
	}

	return tex.release();
}

std::unique_ptr<GSDownloadTexture> GSDeviceVK::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return GSDownloadTextureVK::Create(width, height, format);
}

void GSDeviceVK::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
#ifdef ORBIS_VULKAN
	OrbisVkTraceOp(2, dTex, sTex, r, (static_cast<u64>(destY) << 32) | destX);
#endif
	// Empty rect, abort copy.
	if (r.rempty())
	{
		GL_INS("VK: CopyRect rect empty.");
		return;
	}

	GSTextureVK* const sTexVK = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* const dTexVK = static_cast<GSTextureVK*>(dTex);
	const GSVector4i dst_rect(0, 0, dTexVK->GetWidth(), dTexVK->GetHeight());
	const bool full_draw_copy = dst_rect.eq(r);

	// Source is cleared, if destination is a render target, we can carry the clear forward.
	if (sTexVK->GetState() == GSTexture::State::Cleared)
	{
		if (dTexVK->IsRenderTargetOrDepthStencil())
		{
			if (ProcessClearsBeforeCopy(sTex, dTex, full_draw_copy))
				return;

			// Do an attachment clear.
			const bool depth = dTexVK->IsDepthStencil();
			OMSetRenderTargets(depth ? nullptr : dTexVK, depth ? dTexVK : nullptr, dst_rect);
			BeginRenderPassForStretchRect(
				dTexVK, dst_rect, GSVector4i(destX, destY, destX + r.width(), destY + r.height()));

			// so use an attachment clear
			VkClearAttachment ca;
			ca.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			GSVector4::store<false>(ca.clearValue.color.float32, sTexVK->GetClearForFormat());
			ca.clearValue.depthStencil.depth = sTexVK->GetClearDepth();
			ca.clearValue.depthStencil.stencil = 0;
			ca.colorAttachment = 0;

			const VkClearRect cr = {{{0, 0}, {static_cast<u32>(r.width()), static_cast<u32>(r.height())}}, 0u, 1u};
			vkCmdClearAttachments(GetCurrentCommandBuffer(), 1, &ca, 1, &cr);

			return;
		}

		// commit the clear to the source first, then do normal copy
		sTexVK->CommitClear();
	}

#ifdef ORBIS_VULKAN
	// vk-285-39: the PS5 driver runs vkCmdCopyImage on the CPU at a split, texel by texel through
	// the tile map: tens of milliseconds for one 6x target, and GT4's menus make five or six a
	// frame. A copy the convert draw can make exactly (OrbisCopyByDraw) is that draw instead: nearest
	// sampling at the texel centres of an unscaled rectangle moves every texel unchanged, on the GPU.
	// It comes before the destination's clear is committed: a cleared destination is cleared by the
	// draw's own render pass (BeginRenderPassForStretchRect's load op), and DrawStretchRect counts the
	// copy for the perfmon. Needs proper testing beyond GT4 and Castlevania.
	if (OrbisCopyByDraw(sTexVK, dTexVK))
	{
		OrbisCountCopy(ORBIS_COPY_DRAW, r, sTexVK);
		const GSVector4 src_rect = GSVector4(r) / GSVector4(sTexVK->GetSize()).xyxy();
		const GSVector4 dst_draw(static_cast<float>(destX), static_cast<float>(destY),
			static_cast<float>(destX + r.width()), static_cast<float>(destY + r.height()));
		StretchRectAuto(sTexVK, src_rect, dTexVK, dst_draw, Nearest);
		return;
	}
	OrbisCountCopy(OrbisCopyKind(sTexVK, dTexVK), r, sTexVK);
#endif

	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	// if the destination has been cleared, and we're not overwriting the whole thing, commit the clear first
	// (the area outside of where we're copying to)
	if (dTexVK->GetState() == GSTexture::State::Cleared && !full_draw_copy)
		dTexVK->CommitClear();

	// *now* we can do a normal image copy.
	const VkImageAspectFlags src_aspect =
		(sTexVK->IsDepthStencil()) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageAspectFlags dst_aspect =
		(dTexVK->IsDepthStencil()) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageCopy ic = {{src_aspect, 0u, 0u, 1u}, {r.left, r.top, 0u}, {dst_aspect, 0u, 0u, 1u},
		{static_cast<s32>(destX), static_cast<s32>(destY), 0u},
		{static_cast<u32>(r.width()), static_cast<u32>(r.height()), 1u}};

	EndRenderPass();

	sTexVK->SetUseFenceCounter(GetCurrentFenceCounter());
	dTexVK->SetUseFenceCounter(GetCurrentFenceCounter());
	sTexVK->TransitionToLayout(
		(dTexVK == sTexVK) ? GSTextureVK::Layout::TransferSelf : GSTextureVK::Layout::TransferSrc);
	dTexVK->TransitionToLayout(
		(dTexVK == sTexVK) ? GSTextureVK::Layout::TransferSelf : GSTextureVK::Layout::TransferDst);

	vkCmdCopyImage(GetCurrentCommandBuffer(), sTexVK->GetImage(), sTexVK->GetVkLayout(), dTexVK->GetImage(),
		dTexVK->GetVkLayout(), 1, &ic);

	dTexVK->SetState(GSTexture::State::Dirty);
#ifdef ORBIS_VULKAN
	OrbisGpStampOp(GPK_COPY, dTexVK, r.width(), r.height(), 0, 1, 0); // vk-285-94
#endif
}

void GSDeviceVK::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvertSelector shader, Filter filter)
{
	pxAssert(dTex);
	filter = shader.SupportsBilinear() ? Nearest : filter; // Don't allow HW bilinear if SW bilinear is needed.
	const bool allow_discard = (shader.Mask() == 0xf);
	DoStretchRect(static_cast<GSTextureVK*>(sTex), sRect, static_cast<GSTextureVK*>(dTex), dRect,
		GetConvertPipeline(shader), filter, allow_discard);
}

void GSDeviceVK::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, const GSVector4& dRect,
	PresentShader shader, Filter filter)
{
	DoStretchRect(static_cast<GSTextureVK*>(sTex), sRect, nullptr, dRect,
		m_present.at(static_cast<u32>(shader)), filter, true);
}

#ifdef ORBIS_VULKAN
extern float g_orbis_present_param[4]; // GSRenderer.cpp (live.ini): x = FSR sharpening 0..1, y = split x in pixels
// Test build 1 (vk-285-55): the COPY present shader alpha-blended, for the testing watermark. A file
// static, not a member, so GSDeviceVK's layout (compiled into many objects) stays as it was.
static VkPipeline s_orbis_present_blend = VK_NULL_HANDLE;
#endif

void GSDeviceVK::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	PresentShader shader, float shaderTime, Filter filter)
{
	DisplayConstantBuffer cb;
	cb.SetSource(sRect, sTex->GetSize());
	cb.SetTarget(dRect, dTex ? dTex->GetSize() : GSVector2i(GetWindowWidth(), GetWindowHeight()));
	cb.SetTime(shaderTime);
#ifdef ORBIS_VULKAN
	// vk-285-12: u_orbis_sharp / u_orbis_split of the port's FSR present.glsl (cb10 after u_time).
	cb.TimeAndPad = GSVector4(shaderTime, g_orbis_present_param[0], g_orbis_present_param[1], 0.0f);
#endif
	SetUtilityPushConstants(&cb, sizeof(cb));

	DoStretchRect(static_cast<GSTextureVK*>(sTex), sRect, static_cast<GSTextureVK*>(dTex), dRect,
		m_present[static_cast<int>(shader)], filter, true);
}

#ifdef ORBIS_VULKAN
void GSDeviceVK::OrbisPresentBlend(GSTexture* sTex, const GSVector4& sRect, const GSVector4& dRect)
{
	if (s_orbis_present_blend == VK_NULL_HANDLE || !sTex)
		return;
	DisplayConstantBuffer cb;
	cb.SetSource(sRect, sTex->GetSize());
	cb.SetTarget(dRect, GSVector2i(GetWindowWidth(), GetWindowHeight()));
	cb.SetTime(0.0f);
	SetUtilityPushConstants(&cb, sizeof(cb));
	DoStretchRect(static_cast<GSTextureVK*>(sTex), sRect, nullptr, dRect, s_orbis_present_blend, Biln, true);
}

// For GSRenderer.cpp, which doesn't include this header.
void OrbisVkPresentBlend(GSTexture* tex, const GSVector4& sRect, const GSVector4& dRect)
{
	if (GSDeviceVK* dev = GSDeviceVK::GetInstance())
		dev->OrbisPresentBlend(tex, sRect, dRect);
}

// ---- Frame generation (2026-10-08, AI-assisted; needs proper testing on the console) -------------------------------------------
// PS5SX2/FrameGeneration, per game: the frame between two of the game's, made by ps5/coreorbis/orbis-shims/ps5_framegen (AMD FSR3's
// optical flow and RPCS3-PS5's interpolation, from Swordpdf's RPCS3-PS5 / ps5-framegen), presented before the second one. The game's
// frame is drawn into the interpolator's frame by the same present pipeline that draws it into the swapchain (the TV shader or the
// FSR present, the scaling, the black borders), so a generated frame looks like the game's own; PS5SX2's overlays (the FPS box,
// the watermark, the QR panel) are drawn over both. The TV is in its 120 Hz mode where the console allows it (VKSwapChain.cpp picks
// the mode, the driver configures VideoOut): a 60 fps game is shown at 120, a 30 fps one gets its generated frame twice. At 60 Hz
// only a game at 30 fps or less gets frames made. File statics, not members: GSDeviceVK's layout stays as it was.
// vk-285-133: paced in the PS2's vsyncs (ps5/coreorbis/orbis-shims/OrbisFrameGenPacing.h; vk-285-131's wall-clock pacing slowed
// a 60 fps game at 60 Hz to 30 fps), with a pause when the game slows while frames are generated; nothing is recorded while there
// is no room for a generated frame (a 60 fps game at 60 Hz), and the interpolator is only made once there is.
bool g_orbis_fg_wanted = false; // main-boot.cpp sets it from PS5SX2/FrameGeneration before the GS opens
std::atomic<bool> g_orbis_fg_active{false}; // frames are being generated (the FPS box says "FG")
bool g_orbis_fg_engaged = false; // vk-285-133: the pacing has room for generated frames (GSRenderer.cpp skips the repeats then)

// The driver's measured refresh of the output its swapchain presents to (ps5vk's high-frame-rate output, 2026-10-08); a driver
// without it presents at 59.94 Hz.
extern "C" uint32_t ps5vk_output_refresh_millihertz(void) __attribute__((weak));

namespace
{
	struct OrbisFrameGenState
	{
		std::unique_ptr<ps5::framegen::interpolator> fg;
		std::unique_ptr<GSTextureVK> frames[2]; // the interpolator's two frame images (this frame's and the previous one alternate)
		VkImage frame_images[2] = {};
		std::unique_ptr<GSTextureVK> output;    // the generated frame
		bool failed = false;     // it could not start: not tried again in this game
		bool prepared = false;   // the wrappers know the images are the interpolator's (GENERAL)
		bool reset_next = true;  // the interpolator was (re)made: its first frame has no neighbour
		bool have_output = false;
		double hz = 59.94;
		orbis_fg::Pacing pacing; // vk-285-133
		std::chrono::steady_clock::time_point stat_start{};
		std::chrono::steady_clock::time_point last_state_log{};
		int logged_state = -1;   // the pacing's state the log last described
		u64 stat_frames = 0, stat_engaged = 0, stat_made = 0, stat_presents = 0, stat_counted = 0;
		double stat_vsyncs = 0.0;
	};

	OrbisFrameGenState s_orbis_fg;

	double OrbisFrameGenDisplayHz()
	{
		if (ps5vk_output_refresh_millihertz)
		{
			const uint32_t mhz = ps5vk_output_refresh_millihertz();
			if (mhz >= 20000 && mhz <= 250000)
				return mhz / 1000.0;
		}
		return 59.94;
	}
}

void GSDeviceVK::OrbisFrameGenRelease()
{
	OrbisFrameGenState& s = s_orbis_fg;

	if (!s.fg)
		return;

	EndRenderPass();
	if (GetCurrentCommandBuffer() != VK_NULL_HANDLE)
	{
		ExecuteCommandBuffer(false);
		WaitForGPUIdle();
	}

	for (auto& t : s.frames)
	{
		if (t)
			t->Destroy(false);
		t.reset();
	}
	if (s.output)
		s.output->Destroy(false);
	s.output.reset();

	printf("[fg] released: %llu game frames, %llu generated\n", static_cast<unsigned long long>(s.fg->frames()),
		static_cast<unsigned long long>(s.fg->interpolated()));
	fflush(stdout);
	s.fg->destroy();
	s.fg.reset();
	s.prepared = false;
	s.reset_next = true;
	s.have_output = false;
	g_orbis_fg_active.store(false, std::memory_order_relaxed);
}

namespace
{
	// vk-285-133: the pacing's state, for boot.log, when it changes: at most one line in 5 s (a game at the edge would fill the
	// log), and the state then is logged when those 5 s are over if it still differs from the last line. The 10 s status line
	// says where it is anyway.
	void OrbisFrameGenLogState(OrbisFrameGenState& s, std::chrono::steady_clock::time_point now, double ps2_hz)
	{
		const orbis_fg::Pacing& p = s.pacing;
		const int state = static_cast<int>(p.GetState());
		if (state == s.logged_state)
			return;
		if (s.last_state_log != std::chrono::steady_clock::time_point{} && now - s.last_state_log < std::chrono::seconds(5))
			return;
		s.logged_state = state;
		s.last_state_log = now;
		switch (p.GetState())
		{
			case orbis_fg::State::Generating:
				printf("[fg] generating: a new frame every %.1f PS2 vsyncs (%.2f Hz) is %.2f refreshes at %.2f Hz; the generated frame "
					   "presented %u time(s) before the game's\n",
					p.MedianVsyncs(), ps2_hz, p.Refreshes(), s.hz, p.Repeats());
				break;
			case orbis_fg::State::NoRoom:
				printf("[fg] not generating: a new frame every %.1f PS2 vsyncs (%.2f Hz) is %.2f refreshes at %.2f Hz, no room for "
					   "another%s\n",
					p.MedianVsyncs(), ps2_hz, p.Refreshes(), s.hz,
					s.hz < 90.0 ? " (at 60 Hz only games at 30 fps or less get generated frames; 60 fps ones need the 120 Hz output)" : "");
				break;
			case orbis_fg::State::Warming:
				printf("[fg] not generating yet: %u of %u frames since the start or a load\n", p.WarmFrames(), orbis_fg::Pacing::kWindow);
				break;
			case orbis_fg::State::Unsteady: // vk-285-135
				printf("[fg] not generating: the game's frames aren't of one length (%u of the last %u alike; %u to start, %u to go on), "
					   "so the game's own frames are shown\n",
					p.SteadyFrames(), orbis_fg::Pacing::kWindow, orbis_fg::Pacing::kSteadyStart, orbis_fg::Pacing::kSteadyKeep);
				break;
			case orbis_fg::State::NotNominal:
				printf("[fg] not generating: the frame limiter isn't at normal speed (turbo, slow motion, the fast boot) or a video "
					   "is being captured\n");
				break;
			case orbis_fg::State::Paused:
				break; // its own line, OrbisFrameGenLogEvent's
		}
		fflush(stdout);
	}

	// vk-285-133: the safety net's news, at once.
	void OrbisFrameGenLogEvent(OrbisFrameGenState& s, orbis_fg::Event event)
	{
		const orbis_fg::Pacing& p = s.pacing;
		switch (event)
		{
			case orbis_fg::Event::None:
				return;
			case orbis_fg::Event::Paused:
				printf("[fg] paused for %.0f s: the game ran at %.0f%% of its speed for 3 s while frames were generated (the floor "
					   "%.0f%%)\n",
					p.PauseSeconds(), p.SlowSpeed(), p.PauseFloor());
				s.logged_state = static_cast<int>(orbis_fg::State::Paused);
				break;
			case orbis_fg::Event::ResumedCostly:
				printf("[fg] the pause is over: %.0f%% without generated frames against %.0f%% with them, so they cost speed here; "
					   "the next pause would be %.0f s\n",
					p.NoFgSpeed(), p.SlowSpeed(), p.NextPause());
				break;
			case orbis_fg::Event::ResumedSlowGame:
				printf("[fg] the pause is over: %.0f%% without generated frames, about as with them (%.0f%%): the game is slow by "
					   "itself here, so the floor is %.0f%% for a minute\n",
					p.NoFgSpeed(), p.SlowSpeed(), p.NoFgSpeed() - orbis_fg::Pacing::kCostly < orbis_fg::Pacing::kSlowSpeed ?
																  p.NoFgSpeed() - orbis_fg::Pacing::kCostly :
																  orbis_fg::Pacing::kSlowSpeed);
				break;
		}
		fflush(stdout);
	}
}

u32 GSDeviceVK::OrbisFrameGenRecord(GSTexture* current, const GSVector4& src_uv, const GSVector4& draw_rect, PresentShader shader,
	float shader_time, Filter filter, u32 vsyncs, u32 already, double ps2_hz, float speed, bool nominal)
{
	OrbisFrameGenState& s = s_orbis_fg;
	s.have_output = false;

	if (!g_orbis_fg_wanted || s.failed || !m_swap_chain || !current)
	{
		g_orbis_fg_engaged = false;
		g_orbis_fg_active.store(false, std::memory_order_relaxed);
		return 0;
	}

	// The pacing (vk-285-133): the game's frames in PS2 vsyncs against the display's refreshes, and the speed.
	const auto now = std::chrono::steady_clock::now();
	const double now_s = std::chrono::duration<double>(now.time_since_epoch()).count();
	s.hz = OrbisFrameGenDisplayHz(); // the driver's, each frame: the output goes back to 60 Hz before the process ends
	const orbis_fg::Decision decision = s.pacing.Frame(vsyncs, already, ps2_hz, s.hz, now_s, speed, nominal);
	OrbisFrameGenLogEvent(s, s.pacing.TakeEvent());
	OrbisFrameGenLogState(s, now, ps2_hz);
	g_orbis_fg_engaged = decision.engaged;

	// A status line every 10 s, generating or not.
	if (s.stat_start == std::chrono::steady_clock::time_point{})
		s.stat_start = now;
	s.stat_frames++;
	if (vsyncs <= orbis_fg::Pacing::kGapVsyncs) // a load's wait isn't a frame interval
	{
		s.stat_vsyncs += vsyncs;
		s.stat_counted++;
	}
	s.stat_engaged += decision.engaged ? 1 : 0;
	if (const double secs = std::chrono::duration<double>(now - s.stat_start).count(); secs >= 10.0)
	{
		double gpu_ms = 0.0, gpu_max = 0.0, flow_ms = 0.0;
		u32 timed = 0;
		const bool have_gpu = s.fg && s.fg->take_gpu_time(gpu_ms, gpu_max, timed, flow_ms);
		printf("[fg] %.1f game fps (%.2f PS2 vsyncs a frame at %.2f Hz), %.1f generated frames a second, %.1f presents each, "
			   "display %.2f Hz, speed %.0f%%, %s%s\n",
			s.stat_frames / secs, s.stat_counted ? s.stat_vsyncs / s.stat_counted : 0.0, ps2_hz, s.stat_made / secs,
			s.stat_made ? static_cast<double>(s.stat_presents) / s.stat_made : 0.0, s.hz, speed,
			s.pacing.Paused() ? "paused (the game slowed)" :
			s.stat_engaged == 0 ? "not generating" :
			s.stat_engaged < s.stat_frames ? "generating part of the time" : "generating",
			have_gpu ? fmt::format("; GPU {:.2f} ms a frame ({:.2f} the optical flow), most {:.2f}", gpu_ms, flow_ms, gpu_max).c_str() : "");
		fflush(stdout);
		s.stat_start = now;
		s.stat_frames = s.stat_engaged = s.stat_made = s.stat_presents = s.stat_counted = 0;
		s.stat_vsyncs = 0.0;
	}

	if (!decision.engaged)
	{
		g_orbis_fg_active.store(false, std::memory_order_relaxed);
		return 0;
	}

	const u32 w = static_cast<u32>(GetWindowWidth());
	const u32 h = static_cast<u32>(GetWindowHeight());
	const VkFormat format = m_swap_chain->GetTextureFormat();

	if (!s.fg || s.fg->width() != w || s.fg->height() != h)
	{
		OrbisFrameGenRelease();
		const auto t0 = std::chrono::steady_clock::now();
		std::string error;
		s.fg = std::make_unique<ps5::framegen::interpolator>();

		if (!s.fg->create(m_physical_device, m_device, w, h, error, format))
		{
			printf("[fg] frame generation could not start at %ux%u: %s; it stays off in this game\n", w, h, error.c_str());
			fflush(stdout);
			s.fg.reset();
			s.failed = true;
			g_orbis_fg_engaged = false;
			return 0;
		}

		// The interpolator's images, as textures PCSX2's present can draw into and read (they stay the interpolator's).
		s.frame_images[0] = s.fg->frame_image();
		s.frame_images[1] = s.fg->previous_image();

		for (int i = 0; i < 2; i++)
		{
			s.frames[i] = GSTextureVK::Adopt(s.frame_images[i], GSTexture::RenderTarget, GSTexture::Format::Color, static_cast<int>(w),
				static_cast<int>(h), 1, format);
		}
		s.output = GSTextureVK::Adopt(s.fg->output_image(), GSTexture::Texture, GSTexture::Format::Color, static_cast<int>(w),
			static_cast<int>(h), 1, ps5::framegen::interpolator::k_format);

		if (!s.frames[0] || !s.frames[1] || !s.output)
		{
			printf("[fg] the frame images could not be viewed; frame generation stays off in this game\n");
			fflush(stdout);
			OrbisFrameGenRelease();
			s.failed = true;
			g_orbis_fg_engaged = false;
			return 0;
		}

		s.prepared = false;
		s.reset_next = true;
		printf("[fg] ready for %ux%u in %.0f ms; the display at %.2f Hz\n", w, h,
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), s.hz);
		fflush(stdout);
	}

	ps5::framegen::interpolator& fg = *s.fg;
	VkCommandBuffer cmd = GetCurrentCommandBuffer();
	EndRenderPass();

	fg.prepare(cmd);

	if (!s.prepared)
	{
		// prepare() put the images in GENERAL the first time: what the wrappers are told.
		for (auto& t : s.frames)
			t->OverrideImageLayout(GSTextureVK::Layout::ComputeReadWriteImage);
		s.output->OverrideImageLayout(GSTextureVK::Layout::ComputeReadWriteImage);
		s.prepared = true;
	}

	// The last frame's presents read the generated frame: back to the interpolator.
	s.output->TransitionToLayout(cmd, GSTextureVK::Layout::ComputeReadWriteImage);

	GSTextureVK* const target = fg.frame_image() == s.frame_images[0] ? s.frames[0].get() : s.frames[1].get();

	// This frame as the present draws it into the swapchain: a pass of the swapchain's format cleared to black, and the present
	// pipeline over the same rectangle (PresentRect with no target draws into the pass that is open, as between BeginPresent and
	// EndPresent).
	{
		static_cast<GSTextureVK*>(current)->TransitionToLayout(cmd, GSTextureVK::Layout::ShaderReadOnly);
		target->TransitionToLayout(cmd, GSTextureVK::Layout::ColorAttachment);
		const VkFramebuffer fb = target->GetFramebuffer(false);

		if (fb == VK_NULL_HANDLE)
		{
			printf("[fg] no framebuffer for the frame image; frame generation stays off in this game\n");
			fflush(stdout);
			s.failed = true;
			g_orbis_fg_engaged = false;
			g_orbis_fg_active.store(false, std::memory_order_relaxed);
			return 0;
		}

		const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,
			GetRenderPass(format, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE), fb, {{0, 0}, {w, h}}, 1u,
			&s_present_clear_color};
		vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
		const VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
		const VkRect2D scissor{{0, 0}, {w, h}};
		vkCmdSetViewport(cmd, 0, 1, &vp);
		vkCmdSetScissor(cmd, 0, 1, &scissor);
		PresentRect(current, src_uv, nullptr, draw_rect, shader, shader_time, filter);
		vkCmdEndRenderPass(cmd);
		target->TransitionToLayout(cmd, GSTextureVK::Layout::ComputeReadWriteImage);
		InvalidateCachedState(); // the pass and the state were set by hand
	}

	fg.set_generated(1);
	const bool made = fg.record(cmd, decision.reset || s.reset_next);
	s.reset_next = false;

	if (made)
	{
		s.output->OverrideImageLayout(GSTextureVK::Layout::ComputeReadWriteImage); // record() wrote it in GENERAL
		s.output->TransitionToLayout(cmd, GSTextureVK::Layout::ShaderReadOnly);  // read in the present pass, which can't transition
		s.have_output = decision.presents > 0;
	}

	fg.advance();

	s.stat_made += s.have_output ? 1 : 0;
	s.stat_presents += s.have_output ? decision.presents : 0;
	g_orbis_fg_active.store(true, std::memory_order_relaxed); // the box's "FG": generating, even on a frame too short for one
	return s.have_output ? decision.presents : 0;
}

void GSDeviceVK::OrbisFrameGenDraw()
{
	OrbisFrameGenState& s = s_orbis_fg;

	if (!s.have_output || !s.output)
		return;

	const float w = static_cast<float>(GetWindowWidth());
	const float h = static_cast<float>(GetWindowHeight());
	PresentRect(s.output.get(), GSVector4(0.0f, 0.0f, 1.0f, 1.0f), nullptr, GSVector4(0.0f, 0.0f, w, h), PresentShader::COPY, 0.0f, Nearest);
}

// For GSRenderer.cpp, which doesn't include this header.
u32 OrbisVkFrameGenRecord(GSTexture* current, const GSVector4& src_uv, const GSVector4& draw_rect, PresentShader shader, float shader_time,
	Filter filter, u32 vsyncs, u32 already, double ps2_hz, float speed, bool nominal)
{
	GSDeviceVK* const dev = GSDeviceVK::GetInstance();
	return dev ? dev->OrbisFrameGenRecord(current, src_uv, draw_rect, shader, shader_time, filter, vsyncs, already, ps2_hz, speed, nominal) : 0;
}

void OrbisVkFrameGenDraw()
{
	if (GSDeviceVK* const dev = GSDeviceVK::GetInstance())
		dev->OrbisFrameGenDraw();
}
#endif

void GSDeviceVK::DrawMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvertSelector shader)
{
	GSTexture* last_tex = rects[0].src;
	Filter last_filter = rects[0].filter;
	u8 last_wmask = rects[0].wmask.wrgba;

	u32 first = 0;
	u32 count = 1;

	// Make sure all textures are in shader read only layout, so we don't need to break
	// the render pass to transition.
	for (u32 i = 0; i < num_rects; i++)
	{
		GSTextureVK* const stex = static_cast<GSTextureVK*>(rects[i].src);
		stex->CommitClear();
		if (stex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly)
		{
			EndRenderPass();
			stex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}
	}

	for (u32 i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].filter == last_filter && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}

		DoMultiStretchRects(rects + first, count, static_cast<GSTextureVK*>(dTex), shader);
		last_tex = rects[i].src;
		last_filter = rects[i].filter;
		last_wmask = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}

	DoMultiStretchRects(rects + first, count, static_cast<GSTextureVK*>(dTex), shader);
}

void GSDeviceVK::DoMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTextureVK* dTex, ShaderConvertSelector shader)
{
#ifdef ORBIS_VULKAN
	OrbisVkTraceOp(5, dTex, num_rects ? rects[0].src : nullptr, GSVector4i(num_rects, 0, 0, 0),
		static_cast<u64>(static_cast<int>(shader.Shader())));
#endif
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);
	
	// Set up vertices first.
	const u32 vertex_reserve_size = num_rects * 4 * sizeof(GSVertexPT1);
	const u32 index_reserve_size = num_rects * 6 * sizeof(u16);
	if (!m_vertex_stream_buffer.ReserveMemory(vertex_reserve_size, sizeof(GSVertexPT1)) ||
		!m_index_stream_buffer.ReserveMemory(index_reserve_size, sizeof(u16)))
	{
		ExecuteCommandBufferAndRestartRenderPass(false, "Uploading bytes to vertex buffer");
		if (!m_vertex_stream_buffer.ReserveMemory(vertex_reserve_size, sizeof(GSVertexPT1)) ||
			!m_index_stream_buffer.ReserveMemory(index_reserve_size, sizeof(u16)))
		{
			pxFailRel("Failed to reserve space for vertices");
		}
	}

	// Pain in the arse because the primitive topology for the pipelines is all triangle strips.
	// Don't use primitive restart here, it ends up slower on some drivers.
	const GSVector2 ds(static_cast<float>(dTex->GetWidth()), static_cast<float>(dTex->GetHeight()));
	GSVertexPT1* verts = reinterpret_cast<GSVertexPT1*>(m_vertex_stream_buffer.GetCurrentHostPointer());
	u16* idx = reinterpret_cast<u16*>(m_index_stream_buffer.GetCurrentHostPointer());
	u32 icount = 0;
	u32 vcount = 0;
	for (u32 i = 0; i < num_rects; i++)
	{
		const GSVector4& sRect = rects[i].src_rect;
		const GSVector4& dRect = rects[i].dst_rect;

		const float inv_x = 2.0f / ds.x;
		const float inv_y = 2.0f / ds.y;

		const float left = dRect.x * inv_x - 1.0f;
		const float right = dRect.z * inv_x - 1.0f;
		const float top = 1.0f - dRect.y * inv_y;
		const float bottom = 1.0f - dRect.w * inv_y;

		const u32 vstart = vcount;
		verts[vcount++] = {GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)};
		verts[vcount++] = {GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)};
		verts[vcount++] = {GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)};
		verts[vcount++] = {GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)};

		if (i > 0)
			idx[icount++] = vstart;

		idx[icount++] = vstart;
		idx[icount++] = vstart + 1;
		idx[icount++] = vstart + 2;
		idx[icount++] = vstart + 3;
		idx[icount++] = vstart + 3;
	};

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / sizeof(GSVertexPT1);
	m_vertex.count = vcount;
	m_index.start = m_index_stream_buffer.GetCurrentOffset() / sizeof(u16);
	m_index.count = icount;
	m_vertex_stream_buffer.CommitMemory(vcount * sizeof(GSVertexPT1));
	m_index_stream_buffer.CommitMemory(icount * sizeof(u16));
	SetIndexBuffer(m_index_stream_buffer.GetBuffer());

	// Even though we're batching, a cmdbuffer submit could've messed this up.
	const GSVector4i rc(dTex->GetRect());
	OMSetRenderTargets(dTex->IsRenderTarget() ? dTex : nullptr, dTex->IsDepthStencil() ? dTex : nullptr, rc);
	if (!InRenderPass())
		BeginRenderPassForStretchRect(dTex, rc, rc, false);
	SetUtilityTexture(rects[0].src, rects[0].filter == Biln ? m_linear_sampler : m_point_sampler);

	SetPipeline(GetConvertPipeline(shader.SetMask(rects[0].wmask.wrgba)));

	if (ApplyUtilityState())
		DrawIndexedPrimitive();
#ifdef ORBIS_VULKAN
	OrbisGpStampOp(GPK_UTIL, dTex, dTex->GetWidth(), dTex->GetHeight(), reinterpret_cast<u64>(m_current_pipeline), num_rects, 0); // vk-285-94
#endif
}

void GSDeviceVK::BeginRenderPassForStretchRect(
	GSTextureVK* dTex, const GSVector4i& dtex_rc, const GSVector4i& dst_rc, bool allow_discard)
{
	pxAssert(dst_rc.x >= 0 && dst_rc.y >= 0 && dst_rc.z <= dTex->GetWidth() && dst_rc.w <= dTex->GetHeight());

	const VkAttachmentLoadOp load_op =
		(allow_discard && dst_rc.eq(dtex_rc)) ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : GetLoadOpForTexture(dTex);
	dTex->SetState(GSTexture::State::Dirty);

	if (dTex->IsDepthStencil())
	{
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			BeginClearRenderPass(m_utility_depth_render_pass_clear, dtex_rc, dTex->GetClearDepth(), 0);
		else
			BeginRenderPass((load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE) ? m_utility_depth_render_pass_discard :
			                                                               m_utility_depth_render_pass_load, dtex_rc);
	}
	else if (dTex->GetFormat() == GSTexture::Format::Color)
	{
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			BeginClearRenderPass(m_utility_color_render_pass_clear, dtex_rc, dTex->GetClearColor());
		else
			BeginRenderPass((load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE) ? m_utility_color_render_pass_discard :
			                                                               m_utility_color_render_pass_load, dtex_rc);
	}
	else
	{
		// integer formats, etc
		const VkRenderPass rp = GetRenderPass(dTex->GetVkFormat(), VK_FORMAT_UNDEFINED, load_op,
			VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE);
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
		{
			BeginClearRenderPass(rp, dtex_rc, dTex->GetClearColor());
		}
		else
		{
			BeginRenderPass(rp, dtex_rc);
		}
	}
}

void GSDeviceVK::DoStretchRect(GSTextureVK* sTex, const GSVector4& sRect, GSTextureVK* dTex, const GSVector4& dRect,
	VkPipeline pipeline, Filter filter, bool allow_discard)
{
#ifdef ORBIS_VULKAN
	OrbisVkTraceOp(4, dTex, sTex, GSVector4i(dRect), reinterpret_cast<u64>(pipeline));
#endif
	if (sTex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly)
	{
		// can't transition in a render pass
		EndRenderPass();
		sTex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}

	SetUtilityTexture(sTex, filter == Biln ? m_linear_sampler : m_point_sampler);
	SetPipeline(pipeline);

	const bool is_present = (!dTex);
	const bool depth = (dTex && dTex->IsDepthStencil());
	const GSVector2i size(is_present ? GSVector2i(GetWindowWidth(), GetWindowHeight()) : dTex->GetSize());
	const GSVector4i dtex_rc(0, 0, size.x, size.y);
	const GSVector4i dst_rc(GSVector4i(dRect).rintersect(dtex_rc));

	// switch rts (which might not end the render pass), so check the bounds
	if (!is_present)
	{
		OMSetRenderTargets(depth ? nullptr : dTex, depth ? dTex : nullptr, dst_rc);
		if (InRenderPass() && dTex->GetState() == GSTexture::State::Cleared)
			EndRenderPass();
	}
	else
	{
		// this is for presenting, we don't want to screw with the viewport/scissor set by display
		m_dirty_flags &= ~(DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR);
	}

	if (!is_present && !InRenderPass())
		BeginRenderPassForStretchRect(dTex, dtex_rc, dst_rc, allow_discard);

	DrawStretchRect(sRect, dRect, size);
}

void GSDeviceVK::DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	// ia
	const float inv_x = 2.0f / ds.x;
	const float inv_y = 2.0f / ds.y;

	const float left = dRect.x * inv_x - 1.0f;
	const float right = dRect.z * inv_x - 1.0f;
	const float top = 1.0f - dRect.y * inv_y;
	const float bottom = 1.0f - dRect.w * inv_y;

	const GSVertexPT1 vertices[] = {
		{GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)},
		{GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)},
		{GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)},
		{GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)},
	};
	IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));

	if (ApplyUtilityState())
		DrawPrimitive();
#ifdef ORBIS_VULKAN
	// vk-285-94: the target is the one bound (or the swap chain's image for the present).
	OrbisGpStampOp(GPK_UTIL, m_current_render_target, static_cast<u32>(std::max(dRect.z - dRect.x, 0.0f)),
		static_cast<u32>(std::max(dRect.w - dRect.y, 0.0f)), reinterpret_cast<u64>(m_current_pipeline), 1, 0);
#endif
}

void GSDeviceVK::BlitRect(GSTexture* sTex, const GSVector4i& sRect, u32 sLevel, GSTexture* dTex,
	const GSVector4i& dRect, u32 dLevel, Filter filter)
{
	GSTextureVK* sTexVK = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* dTexVK = static_cast<GSTextureVK*>(dTex);

	EndRenderPass();

	sTexVK->TransitionToLayout(GSTextureVK::Layout::TransferSrc);
	dTexVK->TransitionToLayout(GSTextureVK::Layout::TransferDst);

	// ensure we don't leave this bound later on
	if (m_tfx_textures[0] == sTexVK)
		PSSetShaderResource(0, nullptr, false);

	pxAssert(sTexVK->IsDepthStencil() == dTexVK->IsDepthStencil());
	const VkImageAspectFlags aspect =
		sTexVK->IsDepthStencil() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageBlit ib{{aspect, sLevel, 0u, 1u}, {{sRect.left, sRect.top, 0}, {sRect.right, sRect.bottom, 1}},
		{aspect, dLevel, 0u, 1u}, {{dRect.left, dRect.top, 0}, {dRect.right, dRect.bottom, 1}}};

	vkCmdBlitImage(GetCurrentCommandBuffer(), sTexVK->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dTexVK->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ib,
		filter == Biln ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
#ifdef ORBIS_VULKAN
	OrbisGpStampOp(GPK_BLIT, dTexVK, dRect.width(), dRect.height(), 0, 1, 0); // vk-285-94
#endif
}

void GSDeviceVK::UpdateCLUTTexture(
	GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize)
{
	// Super annoying, but apparently NVIDIA doesn't like floats/ints packed together in the same vec4?
	struct alignas(16) Uniforms
	{
		u32 offsetX, offsetY, dOffset, pad1;
		float scale;
		float pad2[3];
	};

	const Uniforms uniforms = {offsetX, offsetY, dOffset, 0, sScale, {}};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const GSVector4 dRect(0, 0, dSize, 1);
	const ShaderConvert shader = (dSize == 16) ? ShaderConvert::CLUT_4 : ShaderConvert::CLUT_8;
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		GetConvertPipeline(shader), Nearest, true);
}

void GSDeviceVK::ConvertToIndexedTexture(
	GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	struct alignas(16) Uniforms
	{
		u32 SBW;
		u32 DBW;
		u32 PSM;
		u32 pad1[1];
		float ScaleFactor;
		float pad2[3];
	};

	const Uniforms uniforms = {SBW, DBW, SPSM, {}, sScale, {}};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const ShaderConvert shader = ((SPSM & 0xE) == 0) ? ShaderConvert::RGBA_TO_8I : ShaderConvert::RGB5A1_TO_8I;
	const GSVector4 dRect(0, 0, dTex->GetWidth(), dTex->GetHeight());
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		GetConvertPipeline(shader), Nearest, true);
}

void GSDeviceVK::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect)
{
	struct alignas(16) Uniforms
	{
		GSVector2i clamp_min;
		int downsample_factor;
		int pad0;
		float weight;
		float step_multiplier;
		float pad1[2];
	};

	const Uniforms uniforms = {
		clamp_min, static_cast<int>(downsample_factor), 0, static_cast<float>(downsample_factor * downsample_factor), (GSConfig.UserHacks_NativeScaling > GSNativeScaling::Aggressive) ? 2.0f : 1.0f};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const ShaderConvert shader = ShaderConvert::DOWNSAMPLE_COPY;
	VkPipeline pipeline = GetConvertPipeline(shader);
#ifdef ORBIS_VULKAN
	// vk-285-95: the same sum with the factor and step known to the compiler (OrbisDownsamplePipeline).
	if (!s_orbis_ds_dynamic)
	{
		if (const VkPipeline fixed = OrbisDownsamplePipeline(downsample_factor, uniforms.step_multiplier == 2.0f ? 2u : 1u))
			pipeline = fixed;
	}
#endif
	//const GSVector4 dRect = GSVector4(dTex->GetRect());
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		pipeline, Nearest, true);
}

void GSDeviceVK::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter)
{
	GL_PUSH("DoMerge");

	const GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);
	const u32 yuv_constants[4] = {EXTBUF.EMODA, EXTBUF.EMODC};
	const GSVector4 bg_color = GSVector4::unorm8(c);
	const bool feedback_write_2 = PMODE.EN2 && sTex[2] != nullptr && EXTBUF.FBIN == 1;
	const bool feedback_write_1 = PMODE.EN1 && sTex[2] != nullptr && EXTBUF.FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;
	const VkSampler& sampler = filter == Biln ? m_linear_sampler : m_point_sampler;
	// Merge the 2 source textures (sTex[0],sTex[1]). Final results go to dTex. Feedback write will go to sTex[2].
	// If either 2nd output is disabled or SLBG is 1, a background color will be used.
	// Note: background color is also used when outside of the unit rectangle area
	EndRenderPass();

	// transition everything before starting the new render pass
	const bool has_input_0 = (sTex[0] &&
		(sTex[0]->GetState() == GSTexture::State::Dirty || (sTex[0]->GetState() == GSTexture::State::Cleared || sTex[0]->GetClearColor() != 0)));
	const bool has_input_1 = (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg) && sTex[1] &&
		(sTex[1]->GetState() == GSTexture::State::Dirty || (sTex[1]->GetState() == GSTexture::State::Cleared || sTex[1]->GetClearColor() != 0));
	if (has_input_0)
	{
		static_cast<GSTextureVK*>(sTex[0])->CommitClear();
		static_cast<GSTextureVK*>(sTex[0])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}
	if (has_input_1)
	{
		static_cast<GSTextureVK*>(sTex[1])->CommitClear();
		static_cast<GSTextureVK*>(sTex[1])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);

	const GSVector2i dsize(dTex->GetSize());
	const GSVector4i darea(0, 0, dsize.x, dsize.y);
	bool dcleared = false;
	if (sTex[1] && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		// 2nd output is enabled and selected. Copy it to destination so we can blend it with 1st output
		// Note: value outside of dRect must contains the background color (c)
		if (sTex[1]->GetState() == GSTexture::State::Dirty)
		{
			static_cast<GSTextureVK*>(sTex[1])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
			OMSetRenderTargets(dTex, nullptr, darea);
			SetUtilityTexture(sTex[1], sampler);
			BeginClearRenderPass(m_utility_color_render_pass_clear, darea, c);
			SetPipeline(GetConvertPipeline(ShaderConvert::COPY));
			DrawStretchRect(sRect[1], PMODE.SLBG ? dRect[2] : dRect[1], dsize);
			dTex->SetState(GSTexture::State::Dirty);
			dcleared = true;
		}
	}

	// Upload constant to select YUV algo
	const GSVector2i fbsize(sTex[2] ? sTex[2]->GetSize() : GSVector2i(0, 0));
	const GSVector4i fbarea(0, 0, fbsize.x, fbsize.y);
	if (feedback_write_2)
	{
		EndRenderPass();
		OMSetRenderTargets(sTex[2], nullptr, fbarea);
		if (dcleared)
			SetUtilityTexture(dTex, sampler);
		// sTex[2] can be sTex[0], in which case it might be cleared (e.g. Xenosaga).
		BeginRenderPassForStretchRect(static_cast<GSTextureVK*>(sTex[2]), fbarea, GSVector4i(dRect[2]));
		if (dcleared)
		{
			SetPipeline(GetConvertPipeline(ShaderConvert::YUV));
			SetUtilityPushConstants(yuv_constants, sizeof(yuv_constants));
			DrawStretchRect(full_r, dRect[2], fbsize);
		}
		EndRenderPass();

		if (sTex[0] == sTex[2])
		{
			// need a barrier here because of the render pass
			static_cast<GSTextureVK*>(sTex[2])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}
	}

	// Restore background color to process the normal merge
	if (feedback_write_2_but_blend_bg || !dcleared)
	{
		EndRenderPass();
		OMSetRenderTargets(dTex, nullptr, darea);
		BeginClearRenderPass(m_utility_color_render_pass_clear, darea, c);
		dTex->SetState(GSTexture::State::Dirty);
	}
	else if (!InRenderPass())
	{
		OMSetRenderTargets(dTex, nullptr, darea);
		BeginRenderPass(m_utility_color_render_pass_load, darea);
	}

	if (sTex[0] && sTex[0]->GetState() == GSTexture::State::Dirty)
	{
		// 1st output is enabled. It must be blended
		SetUtilityTexture(sTex[0], sampler);
		SetPipeline(m_merge[PMODE.MMOD]);
		SetUtilityPushConstants(&bg_color, sizeof(bg_color));
		DrawStretchRect(sRect[0], dRect[0], dTex->GetSize());
	}

	if (feedback_write_1)
	{
		EndRenderPass();
		SetPipeline(GetConvertPipeline(ShaderConvert::YUV));
		SetUtilityTexture(dTex, sampler);
		SetUtilityPushConstants(yuv_constants, sizeof(yuv_constants));
		OMSetRenderTargets(sTex[2], nullptr, fbarea);
		BeginRenderPass(m_utility_color_render_pass_load, fbarea);
		DrawStretchRect(full_r, dRect[2], dsize);
	}

	EndRenderPass();

	// this texture is going to get used as an input, so make sure we don't read undefined data
	static_cast<GSTextureVK*>(dTex)->CommitClear();
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
}

void GSDeviceVK::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb)
{
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);

	const GSVector4i rc = GSVector4i(dRect);
	const GSVector4i dtex_rc = dTex->GetRect();
	const GSVector4i clamped_rc = rc.rintersect(dtex_rc);
	EndRenderPass();
	OMSetRenderTargets(dTex, nullptr, clamped_rc);
	SetUtilityTexture(sTex, filter == Biln ? m_linear_sampler : m_point_sampler);
	BeginRenderPassForStretchRect(static_cast<GSTextureVK*>(dTex), dTex->GetRect(), clamped_rc, false);
	SetPipeline(m_interlace[static_cast<int>(shader)]);
	SetUtilityPushConstants(&cb, sizeof(cb));
	DrawStretchRect(sRect, dRect, dTex->GetSize());
	EndRenderPass();

	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
}

void GSDeviceVK::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
	const GSVector4 sRect = GSVector4(0.0f, 0.0f, 1.0f, 1.0f);
	const GSVector4i dRect = dTex->GetRect();
	EndRenderPass();
	OMSetRenderTargets(dTex, nullptr, dRect);
	SetUtilityTexture(sTex, m_point_sampler);
	BeginRenderPass(m_utility_color_render_pass_discard, dRect);
	dTex->SetState(GSTexture::State::Dirty);
	SetPipeline(m_shadeboost_pipeline);
	SetUtilityPushConstants(params, sizeof(float) * 4);
	DrawStretchRect(sRect, GSVector4(dRect), dTex->GetSize());
	EndRenderPass();

	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
}

void GSDeviceVK::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
	const GSVector4 sRect = GSVector4(0.0f, 0.0f, 1.0f, 1.0f);
	const GSVector4i dRect = dTex->GetRect();
	EndRenderPass();
	OMSetRenderTargets(dTex, nullptr, dRect);
	SetUtilityTexture(sTex, m_linear_sampler);
	BeginRenderPass(m_utility_color_render_pass_discard, dRect);
	dTex->SetState(GSTexture::State::Dirty);
	SetPipeline(m_fxaa_pipeline);
	DrawStretchRect(sRect, GSVector4(dRect), dTex->GetSize());
	EndRenderPass();

	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
}

#ifdef ORBIS_VULKAN
// PS5 port (vk-285-98): a copy into a stream buffer with streaming stores. The driver's memory is ordinary
// cached memory ([membench]): each line the GS thread writes there was last written a lap of the ring ago and
// is long gone from the caches, so a plain store first reads it from memory; a streaming store doesn't
// (~2.6x faster on the Pro). The destination must be 16-byte aligned; the source may not be. The vertex
// upload already streams (GSVector4i::storent). SubmitCommandBuffer fences the stores.
static __fi void OrbisStreamCopy(void* dst, const void* src, size_t size)
{
	u8* d = static_cast<u8*>(dst);
	const u8* s = static_cast<const u8*>(src);
	for (; size >= 64; size -= 64, d += 64, s += 64)
	{
		const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
		const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16));
		const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 32));
		const __m128i e = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 48));
		_mm_stream_si128(reinterpret_cast<__m128i*>(d), a);
		_mm_stream_si128(reinterpret_cast<__m128i*>(d + 16), b);
		_mm_stream_si128(reinterpret_cast<__m128i*>(d + 32), c);
		_mm_stream_si128(reinterpret_cast<__m128i*>(d + 48), e);
	}
	for (; size >= 16; size -= 16, d += 16, s += 16)
		_mm_stream_si128(reinterpret_cast<__m128i*>(d), _mm_loadu_si128(reinterpret_cast<const __m128i*>(s)));
	if (size != 0)
		std::memcpy(d, s, size);
}

// PS5 port (vk-285-102): the streaming stores only with flags/ntstore (live). vk-285-101's GS profile, with the
// GS thread now near its limit, had its hottest samples on stores and loads right behind these copies (a full
// store queue: UpdateCurrentFencePosition's two stores 3%, ApplyTFXState's first load of the dirty flags 3.8%).
// Plain copies by default until an A/B says otherwise.
static __fi void OrbisUploadCopy(void* dst, const void* src, size_t size)
{
	if (g_orbis_nt_store.load(std::memory_order_relaxed))
		OrbisStreamCopy(dst, src, size);
	else
		std::memcpy(dst, src, size);
}

// PS5 port (vk-285-106): a constant buffer into its uniform stream buffer. With flags/ntstore (live), whole lines of
// streaming stores: the copy is padded with zeros to a multiple of 64 bytes, which stays inside the slot (the
// uniform buffers' offsets are 256-aligned, minUniformBufferOffsetAlignment, and nothing else is written in a slot's
// tail). vk-285-102's plain streaming copy left the VS buffer's 48 bytes and the PS buffer's last 16 as part-written
// lines, which a streaming store flushes as partial writes. `dst` must be 64-byte aligned. Needs proper testing.
template <typename T>
static __fi void OrbisUploadUniform(void* dst, const T& src)
{
	constexpr size_t size = sizeof(T), padded = (sizeof(T) + 63) & ~static_cast<size_t>(63);
	if (g_orbis_nt_store.load(std::memory_order_relaxed) && (reinterpret_cast<uintptr_t>(dst) & 63) == 0)
	{
		alignas(64) u8 line[padded];
		std::memcpy(line, &src, size);
		std::memset(line + size, 0, padded - size);
		OrbisStreamCopy(dst, line, padded);
	}
	else
	{
		std::memcpy(dst, &src, size);
	}
}
#endif

void GSDeviceVK::IASetVertexBuffer(const void* vertex, size_t stride, size_t count, size_t align_multiplier)
{
	const u32 size = static_cast<u32>(stride) * static_cast<u32>(count);
	if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride) * align_multiplier))
	{
		ExecuteCommandBufferAndRestartRenderPass(false, "Uploading bytes to vertex buffer");
		if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride) * align_multiplier))
			pxFailRel("Failed to reserve space for vertices");
	}

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / stride;
	m_vertex.count = count;

	GSVector4i::storent(m_vertex_stream_buffer.GetCurrentHostPointer(), vertex, count * stride);
	m_vertex_stream_buffer.CommitMemory(size);
}

void GSDeviceVK::UploadIndices(VKStreamBuffer& buffer, const void* index, size_t count)
{
	const u32 size = sizeof(u16) * static_cast<u32>(count);
#ifdef ORBIS_VULKAN
	// vk-285-98: 16-byte aligned, for OrbisStreamCopy (the start is still counted in indices).
	constexpr u32 align = 16;
#else
	constexpr u32 align = sizeof(u16);
#endif
	if (!buffer.ReserveMemory(size, align))
	{
		ExecuteCommandBufferAndRestartRenderPass(false, "Uploading bytes to index buffer");
		if (!buffer.ReserveMemory(size, align))
			pxFailRel("Failed to reserve space for vertices");
	}

	m_index.start = buffer.GetCurrentOffset() / sizeof(u16);
	m_index.count = count;

#ifdef ORBIS_VULKAN
	OrbisUploadCopy(buffer.GetCurrentHostPointer(), index, size); // vk-285-98, vk-285-102
#else
	std::memcpy(buffer.GetCurrentHostPointer(), index, size);
#endif
	buffer.CommitMemory(size);
}

void GSDeviceVK::IASetIndexBuffer(const void* index, size_t count)
{
	UploadIndices(m_index_stream_buffer, index, count);

	SetIndexBuffer(m_index_stream_buffer.GetBuffer());
}

void GSDeviceVK::VSSetIndexBuffer(const void* index, size_t count)
{
	UploadIndices(m_expand_index_stream_buffer, index, count);
}

void GSDeviceVK::OMSetRenderTargets(
	GSTexture* rt, GSTexture* ds, const GSVector4i& scissor, FeedbackLoopFlag feedback_loop,
	const GSVector2i& viewport_size)
{
	GSTextureVK* vkRt = static_cast<GSTextureVK*>(rt);
	GSTextureVK* vkDs = static_cast<GSTextureVK*>(ds);

	if (m_current_render_target != vkRt || m_current_depth_target != vkDs ||
		m_current_framebuffer_feedback_loop != feedback_loop ||
		m_current_framebuffer == VK_NULL_HANDLE)
	{
		// framebuffer change or feedback loop enabled/disabled
		EndRenderPass();

		if (vkRt)
		{
			m_current_framebuffer =
				vkRt->GetLinkedFramebuffer(vkDs,
					(feedback_loop & FeedbackLoopFlag_ReadAndWriteRT) != 0,
					(feedback_loop & (FeedbackLoopFlag_ReadAndWriteDepth | FeedbackLoopFlag_ReadDepth)) != 0);
		}
		else if (vkDs)
		{
			pxAssert(!(feedback_loop & FeedbackLoopFlag_ReadAndWriteRT));
			m_current_framebuffer = vkDs->GetLinkedFramebuffer(
				nullptr, false, (feedback_loop & (FeedbackLoopFlag_ReadAndWriteDepth | FeedbackLoopFlag_ReadDepth)) != 0);
		}
		else
		{
			m_current_framebuffer = m_null_framebuffer;
		}
	}
	else if (InRenderPass())
	{
		// Framebuffer unchanged, but check for clears
		// Use an attachment clear to wipe it out without restarting the render pass
		if (IsDeviceNVIDIA())
		{
			// Using vkCmdClearAttachments() within a render pass on NVIDIA seems to cause dependency issues
			// between draws that are testing depth which precede it. The result is flickering where Z tests
			// should be failing. Breaking/restarting the render pass isn't enough to work around the bug,
			// it needs an explicit pipeline barrier.
			if (vkRt && vkRt->GetState() != GSTexture::State::Dirty)
			{
				if (vkRt->GetState() == GSTexture::State::Cleared)
				{
					EndRenderPass();
					vkRt->TransitionSubresourcesToLayout(GetCurrentCommandBuffer(), 0, 1,
						vkRt->GetLayout(), vkRt->GetLayout());
				}
				else
				{
					// Invalidated -> Dirty.
					vkRt->SetState(GSTexture::State::Dirty);
				}
			}
			if (vkDs && vkDs->GetState() != GSTexture::State::Dirty)
			{
				if (vkDs->GetState() == GSTexture::State::Cleared)
				{
					EndRenderPass();
					vkDs->TransitionSubresourcesToLayout(GetCurrentCommandBuffer(), 0, 1,
						vkDs->GetLayout(), vkDs->GetLayout());
				}
				else
				{
					// Invalidated -> Dirty.
					vkDs->SetState(GSTexture::State::Dirty);
				}
			}
		}
		else
		{
			std::array<VkClearAttachment, 2> cas;
			u32 num_ca = 0;
			if (vkRt && vkRt->GetState() != GSTexture::State::Dirty)
			{
				if (vkRt->GetState() == GSTexture::State::Cleared)
				{
					VkClearAttachment& ca = cas[num_ca++];
					ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					ca.colorAttachment = 0;
					GSVector4::store<false>(ca.clearValue.color.float32, vkRt->GetClearForFormat());
				}

				vkRt->SetState(GSTexture::State::Dirty);
			}
			if (vkDs && vkDs->GetState() != GSTexture::State::Dirty)
			{
				if (vkDs->GetState() == GSTexture::State::Cleared)
				{
					VkClearAttachment& ca = cas[num_ca++];
					ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
					ca.colorAttachment = 1;
					ca.clearValue.depthStencil = {vkDs->GetClearDepth()};
				}

				vkDs->SetState(GSTexture::State::Dirty);
			}

			if (num_ca > 0)
			{
				const GSVector2i size = vkRt ? vkRt->GetSize() : vkDs->GetSize();
				const VkClearRect cr = {{{0, 0}, {static_cast<u32>(size.x), static_cast<u32>(size.y)}}, 0u, 1u};
				vkCmdClearAttachments(GetCurrentCommandBuffer(), num_ca, cas.data(), 1, &cr);
			}
		}
	}

	m_current_render_target = vkRt;
	m_current_depth_target = vkDs;
	m_current_framebuffer_feedback_loop = feedback_loop;

	if (!InRenderPass())
	{
		if (vkRt)
		{
			if (feedback_loop & FeedbackLoopFlag_ReadAndWriteRT)
			{
				// NVIDIA drivers appear to return random garbage when sampling the RT via a feedback loop, if the load op for
				// the render pass is CLEAR. Using vkCmdClearAttachments() doesn't work, so we have to clear the image instead.
				if (vkRt->GetState() == GSTexture::State::Cleared && IsDeviceNVIDIA())
					vkRt->CommitClear();

				if (vkRt->GetLayout() != GSTextureVK::Layout::FeedbackLoop)
				{
					// need to update descriptors to reflect the new layout
					m_dirty_flags |= (DIRTY_FLAG_TFX_TEXTURE_0 << TFX_TEXTURE_RT);
					if (m_tfx_textures[TFX_TEXTURE_TEXTURE] == vkRt)
						m_dirty_flags |= DIRTY_FLAG_TFX_TEXTURE_0 << TFX_TEXTURE_TEXTURE;
					vkRt->TransitionToLayout(GSTextureVK::Layout::FeedbackLoop);
				}
			}
			else
			{
				vkRt->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);
			}
		}
		if (vkDs)
		{
			// need to update descriptors to reflect the new layout
			if (feedback_loop & FeedbackLoopFlag_ReadAndWriteDepth)
			{
				// NVIDIA drivers appear to return random garbage when sampling the RT via a feedback loop, if the load op for
				// the render pass is CLEAR. Using vkCmdClearAttachments() doesn't work, so we have to clear the image instead.
				// Note: DS feedback loop was added later - we will assume that the same issue is relevant.
				if (vkDs->GetState() == GSTexture::State::Cleared && IsDeviceNVIDIA())
					vkDs->CommitClear();

				if (vkDs->GetLayout() != GSTextureVK::Layout::FeedbackLoop)
				{
					m_dirty_flags |= (DIRTY_FLAG_TFX_TEXTURE_0 << TFX_TEXTURE_DEPTH);
					if (m_tfx_textures[TFX_TEXTURE_TEXTURE] == vkDs)
						m_dirty_flags |= DIRTY_FLAG_TFX_TEXTURE_0 << TFX_TEXTURE_TEXTURE;
					vkDs->TransitionToLayout(GSTextureVK::Layout::FeedbackLoop);
				}
			}
			else if (feedback_loop & FeedbackLoopFlag_ReadDepth)
			{
				const GSTextureVK::Layout layout = m_features.depth_feedback ?
					GSTextureVK::Layout::FeedbackLoop : GSTextureVK::Layout::General;
				if (vkDs->GetLayout() != layout)
				{
					m_dirty_flags |= (DIRTY_FLAG_TFX_TEXTURE_0 << TFX_TEXTURE_TEXTURE);
					if (m_tfx_textures[TFX_TEXTURE_TEXTURE] == vkDs)
						m_dirty_flags |= DIRTY_FLAG_TFX_TEXTURE_0 << TFX_TEXTURE_TEXTURE;
					vkDs->TransitionToLayout(layout);
				}
			}
			else
			{
				vkDs->TransitionToLayout(GSTextureVK::Layout::DepthStencilAttachment);
			}
		}
	}

	// This is used to set/initialize the framebuffer for tfx rendering.
	const GSVector2i size = (vkRt ? vkRt->GetSize() : (vkDs ? vkDs->GetSize() : viewport_size));
	const VkViewport vp{0.0f, 0.0f, static_cast<float>(size.x), static_cast<float>(size.y), 0.0f, 1.0f};

	SetViewport(vp);
	SetScissor(scissor);
}

VkSampler GSDeviceVK::GetSampler(GSHWDrawConfig::SamplerSelector ss)
{
	const auto it = m_samplers.find(ss.key);
	if (it != m_samplers.end())
		return it->second;

	// See https://www.khronos.org/registry/vulkan/specs/1.2-extensions/man/html/VkSamplerCreateInfo.html#_description
	// for the reasoning behind 0.25f here.
	const VkSamplerCreateInfo ci = {
		VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, nullptr, 0,
		ss.IsMagFilterLinear() ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, // min
		ss.IsMinFilterLinear() ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, // mag
		ss.IsMipFilterLinear() ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST, // mip
		static_cast<VkSamplerAddressMode>(
			ss.tau ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE), // u
		static_cast<VkSamplerAddressMode>(
			ss.tav ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE), // v
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, // w
		0.0f, // lod bias
		VK_FALSE, // anisotropy enable
		1.0f, // anisotropy
		VK_FALSE, // compare enable
		VK_COMPARE_OP_ALWAYS, // compare op
		0.0f, // min lod
		(ss.lodclamp || !ss.UseMipmapFiltering()) ? 0.25f : VK_LOD_CLAMP_NONE, // max lod
		VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, // border
		VK_FALSE // unnormalized coordinates
	};
	VkSampler sampler = VK_NULL_HANDLE;
	VkResult res = vkCreateSampler(m_device, &ci, nullptr, &sampler);
	if (res != VK_SUCCESS)
		LOG_VULKAN_ERROR(res, "vkCreateSampler() failed: ");

	m_samplers.emplace(ss.key, sampler);
	return sampler;
}

void GSDeviceVK::ClearSamplerCache()
{
	ExecuteCommandBuffer(true);
	for (const auto& it : m_samplers)
	{
		if (it.second != VK_NULL_HANDLE)
			vkDestroySampler(m_device, it.second, nullptr);
	}
	m_samplers.clear();
	m_point_sampler = GetSampler(GSHWDrawConfig::SamplerSelector::Point());
	m_linear_sampler = GetSampler(GSHWDrawConfig::SamplerSelector::Linear());
	m_utility_sampler = m_point_sampler;
	m_tfx_sampler = m_point_sampler;
}

static void AddMacro(std::stringstream& ss, const char* name, int value)
{
	ss << "#define " << name << " " << value << "\n";
}

static void AddShaderHeader(std::stringstream& ss)
{
	const GSDeviceVK* dev = GSDeviceVK::GetInstance();
	const GSDevice::FeatureSupport features = dev->Features();

	ss << "#version 460 core\n";
	ss << "#extension GL_EXT_samplerless_texture_functions : require\n";

	if (!features.texture_barrier)
		ss << "#define DISABLE_TEXTURE_BARRIER 1\n";
	if (features.texture_barrier && dev->UseFeedbackLoopLayout())
		ss << "#define HAS_FEEDBACK_LOOP_LAYOUT 1\n";
	if (features.rov)
	{
		ss << "#extension GL_ARB_fragment_shader_interlock : require\n";
		ss << "#extension GL_ARB_shader_image_load_store : require\n";
	}
}

static void AddShaderStageMacro(std::stringstream& ss, bool vs, bool gs, bool fs)
{
	if (vs)
		ss << "#define VERTEX_SHADER 1\n";
	else if (gs)
		ss << "#define GEOMETRY_SHADER 1\n";
	else if (fs)
		ss << "#define FRAGMENT_SHADER 1\n";
}

static void AddUtilityVertexAttributes(Vulkan::GraphicsPipelineBuilder& gpb)
{
	gpb.AddVertexBuffer(0, sizeof(GSVertexPT1));
	gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
	gpb.AddVertexAttribute(1, 0, VK_FORMAT_R32G32_SFLOAT, 16);
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
}

static void SetPipelineProvokingVertex(const GSDevice::FeatureSupport& features, Vulkan::GraphicsPipelineBuilder& gpb)
{
	// We enable provoking vertex here anyway, in case it doesn't support multiple modes in the same pass.
	// Normally we wouldn't enable it on the present/swap chain, but apparently the rule is it applies to the last
	// pipeline bound before the render pass begun, and in this case, we can't bind null.
	if (features.provoking_vertex_last)
		gpb.SetProvokingVertex(VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);
}

VkShaderModule GSDeviceVK::GetUtilityVertexShader(const std::string& source, const char* replace_main = nullptr)
{
	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, true, false, false);
	if (replace_main)
		ss << "#define " << replace_main << " main\n";
	ss << source;

	return g_vulkan_shader_cache->GetVertexShader(ss.str());
}

VkShaderModule GSDeviceVK::GetUtilityFragmentShader(const std::string& source, const char* replace_main = nullptr)
{
	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, false, false, true);
	if (replace_main)
		ss << "#define " << replace_main << " main\n";
	ss << source;

	return g_vulkan_shader_cache->GetFragmentShader(ss.str());
}

bool GSDeviceVK::CreateNullTexture()
{
	m_null_texture = GSTextureVK::Create(GSTexture::ShaderWriteTarget, GSTexture::Format::Color, 1, 1, 1);
	if (!m_null_texture)
		return false;

	const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
	const VkImageSubresourceRange srr{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
	const VkClearColorValue ccv{};
	m_null_texture->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ClearDst);
	vkCmdClearColorImage(cmdbuf, m_null_texture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ccv, 1, &srr);
	m_null_texture->TransitionToLayout(cmdbuf, GSTextureVK::Layout::General);
	Vulkan::SetObjectName(m_device, m_null_texture->GetImage(), "Null texture");
	Vulkan::SetObjectName(m_device, m_null_texture->GetView(), "Null texture view");

	return true;
}

#ifdef ORBIS_VULKAN
// PS5 port (vk-285-97): what the stream buffers' memory costs to write and read, against the heap, once per
// run: the [membench] line. TSC ticks per 256 bytes written in order over 16 MiB (more than the L3), as
// plain copies, with a prefetchw 1 KiB ahead, and as non-temporal stores, and per 8 bytes read over 1 MiB.
// It tells cached memory from write-combined or uncached, which decides how the GS thread should fill the
// vertex, index and uniform buffers. Runs before the buffer's first use.
static void OrbisMemBench(u8* mapped, size_t mapped_bytes)
{
	static bool s_done = false;
	if (s_done || mapped == nullptr)
		return;
	s_done = true;
	const size_t bytes = std::min<size_t>(mapped_bytes, 16u << 20) & ~static_cast<size_t>(255);
	const size_t read_bytes = std::min<size_t>(bytes, 1u << 20);
	u8* const heap = static_cast<u8*>(_aligned_malloc(bytes, 4096));
	if (heap == nullptr)
		return;
	std::memset(heap, 0, bytes);
	alignas(64) u8 src[256];
	for (u32 i = 0; i < sizeof(src); i++)
		src[i] = static_cast<u8>(i);
	const auto write = [&](u8* dst, int mode) {
		const u64 start = __builtin_ia32_rdtsc();
		for (size_t off = 0; off < bytes; off += 256)
		{
			u8* const d = dst + off;
			if (mode == 1)
				__builtin_prefetch(d + 1024, 1, 3);
			if (mode == 2)
			{
				for (u32 i = 0; i < 256; i += 32)
					_mm256_stream_si256(reinterpret_cast<__m256i*>(d + i),
						_mm256_load_si256(reinterpret_cast<const __m256i*>(src + i)));
			}
			else
			{
				std::memcpy(d, src, 256);
			}
		}
		_mm_sfence();
		return static_cast<double>(__builtin_ia32_rdtsc() - start) / static_cast<double>(bytes / 256);
	};
	u64 sink = 0;
	const auto read = [&](const u8* p) {
		const u64 start = __builtin_ia32_rdtsc();
		for (size_t off = 0; off < read_bytes; off += 8)
			sink += *reinterpret_cast<const volatile u64*>(p + off);
		return static_cast<double>(__builtin_ia32_rdtsc() - start) / static_cast<double>(read_bytes / 8);
	};
	write(heap, 0);
	const double heap_w = write(heap, 0);
	const double heap_nt = write(heap, 2);
	write(mapped, 0);
	const double map_w = write(mapped, 0);
	const double map_pw = write(mapped, 1);
	const double map_nt = write(mapped, 2);
	read(heap);
	const double heap_r = read(heap);
	read(mapped);
	const double map_r = read(mapped);
	Console.WriteLn("[membench] TSC ticks per 256 B written over %zu MiB: heap %.0f, heap NT %.0f, mapped %.0f, "
					"mapped+prefetchw %.0f, mapped NT %.0f; per 8 B read over %zu KiB: heap %.1f, mapped %.1f (%llx)",
		bytes >> 20, heap_w, heap_nt, map_w, map_pw, map_nt, read_bytes >> 10, heap_r, map_r,
		static_cast<unsigned long long>(sink & 0xf));
	_aligned_free(heap);
}
#endif

bool GSDeviceVK::CreateBuffers()
{
	if (!m_vertex_stream_buffer.Create(
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (m_features.vs_expand ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0),
			VERTEX_BUFFER_SIZE))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate vertex buffer");
		return false;
	}
#ifdef ORBIS_VULKAN
	OrbisMemBench(m_vertex_stream_buffer.GetHostPointer(), VERTEX_BUFFER_SIZE); // vk-285-97
	m_vertex_stream_buffer.OrbisSetStreamed(true); // vk-285-106: IASetVertexBuffer's GSVector4i::storent
#endif

	if (!m_index_stream_buffer.Create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, INDEX_BUFFER_SIZE))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate index buffer");
		return false;
	}

	if (!m_expand_index_stream_buffer.Create(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_features.aa1 ? INDEX_BUFFER_SIZE : 4))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate expansion index buffer (VS resource)");
		return false;
	}

	if (!m_vertex_uniform_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VERTEX_UNIFORM_BUFFER_SIZE))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate vertex uniform buffer");
		return false;
	}

	if (!m_fragment_uniform_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, FRAGMENT_UNIFORM_BUFFER_SIZE))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate fragment uniform buffer");
		return false;
	}

	if (!m_texture_stream_buffer.Create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, TEXTURE_BUFFER_SIZE))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate texture upload buffer");
		return false;
	}

	if (!AllocatePreinitializedGPUBuffer(EXPAND_BUFFER_SIZE, &m_expand_index_buffer, &m_expand_index_buffer_allocation,
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &GSDevice::GenerateExpansionIndexBuffer))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate expansion index buffer");
		return false;
	}

	SetIndexBuffer(m_index_stream_buffer.GetBuffer());
	return true;
}

bool GSDeviceVK::CreatePipelineLayouts()
{
	VkDevice dev = m_device;
	Vulkan::DescriptorSetLayoutBuilder dslb;
	Vulkan::PipelineLayoutBuilder plb;

	//////////////////////////////////////////////////////////////////////////
	// Convert Pipeline Layout
	//////////////////////////////////////////////////////////////////////////

	dslb.SetPushFlag();
	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NUM_UTILITY_SAMPLERS, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_utility_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_utility_ds_layout, "Convert descriptor layout");

	plb.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CONVERT_PUSH_CONSTANTS_SIZE);
	plb.AddDescriptorSet(m_utility_ds_layout);
	if ((m_utility_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_utility_pipeline_layout, "Convert pipeline layout");

	//////////////////////////////////////////////////////////////////////////
	// Draw/TFX Pipeline Layout
	//////////////////////////////////////////////////////////////////////////
	dslb.AddBinding(
		0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if (m_features.vs_expand)
	{
		dslb.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT);
		plb.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GSHWDrawConfig::VSPushConstants));
	}
	if (m_features.aa1)
		dslb.AddBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT);
	if ((m_tfx_ubo_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_tfx_ubo_ds_layout, "TFX UBO descriptor layout");

	dslb.SetPushFlag();
	dslb.AddBinding(TFX_TEXTURE_TEXTURE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(TFX_TEXTURE_PALETTE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(TFX_TEXTURE_RT,
		(m_features.texture_barrier && !UseFeedbackLoopLayout()) ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT :
																                              VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(TFX_TEXTURE_PRIMID, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(TFX_TEXTURE_DEPTH,
		(m_features.texture_barrier && !UseFeedbackLoopLayout()) ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT :
		                                                           VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(TFX_TEXTURE_RT_ROV, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(TFX_TEXTURE_DEPTH_ROV, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_tfx_texture_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_tfx_texture_ds_layout, "TFX texture descriptor layout");

	plb.AddDescriptorSet(m_tfx_ubo_ds_layout);
	plb.AddDescriptorSet(m_tfx_texture_ds_layout);
	if ((m_tfx_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_tfx_pipeline_layout, "TFX pipeline layout");
	return true;
}

bool GSDeviceVK::CreateRenderPasses()
{
#define GET(dest, rt, depth, fbl, dsp, opa, opb, opc) \
	do \
	{ \
		dest = GetRenderPass( \
			(rt), (depth), ((rt) != VK_FORMAT_UNDEFINED) ? (opa) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* color load */ \
			((rt) != VK_FORMAT_UNDEFINED) ? VK_ATTACHMENT_STORE_OP_STORE : \
											VK_ATTACHMENT_STORE_OP_DONT_CARE, /* color store */ \
			((depth) != VK_FORMAT_UNDEFINED) ? (opb) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* depth load */ \
			((depth) != VK_FORMAT_UNDEFINED) ? VK_ATTACHMENT_STORE_OP_STORE : \
											   VK_ATTACHMENT_STORE_OP_DONT_CARE, /* depth store */ \
			((depth) != VK_FORMAT_UNDEFINED) ? (opc) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* stencil load */ \
			VK_ATTACHMENT_STORE_OP_DONT_CARE, /* stencil store */ \
			(fbl), /* feedback loop */ \
			(dsp) /* depth sampling */ \
		); \
		if (dest == VK_NULL_HANDLE) \
			return false; \
	} while (0)

	const VkFormat rt_format = LookupNativeFormat(GSTexture::Format::Color);
	const VkFormat colclip_rt_format = LookupNativeFormat(GSTexture::Format::ColorClip);
	const VkFormat depth_format = LookupNativeFormat(GSTexture::Format::DepthStencil);

	for (u32 rt = 0; rt < 2; rt++)
	{
		for (u32 ds = 0; ds < 2; ds++)
		{
			for (u32 colclip = 0; colclip < 2; colclip++)
			{
				for (u32 stencil = 0; stencil < 2; stencil++)
				{
					for (u32 fbl = 0; fbl < 2; fbl++)
					{
						for (u32 dsp = 0; dsp < 2; dsp++)
						{
							for (u32 opa = VK_ATTACHMENT_LOAD_OP_LOAD; opa <= VK_ATTACHMENT_LOAD_OP_DONT_CARE; opa++)
							{
								for (u32 opb = VK_ATTACHMENT_LOAD_OP_LOAD; opb <= VK_ATTACHMENT_LOAD_OP_DONT_CARE; opb++)
								{
									const VkFormat rp_rt_format =
										(rt != 0) ? ((colclip != 0) ? colclip_rt_format : rt_format) : VK_FORMAT_UNDEFINED;
									const VkFormat rp_depth_format = (ds != 0) ? depth_format : VK_FORMAT_UNDEFINED;
									const VkAttachmentLoadOp opc = (!stencil || !m_features.stencil_buffer) ?
									                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE :
									                                   VK_ATTACHMENT_LOAD_OP_LOAD;
									GET(m_tfx_render_pass[rt][ds][colclip][stencil][fbl][dsp][opa][opb], rp_rt_format,
										rp_depth_format, (fbl != 0), (dsp != 0), static_cast<VkAttachmentLoadOp>(opa),
										static_cast<VkAttachmentLoadOp>(opb), static_cast<VkAttachmentLoadOp>(opc));
								}
							}
						}
					}
				}
			}
		}
	}

	GET(m_utility_color_render_pass_load, rt_format, VK_FORMAT_UNDEFINED, false, false, VK_ATTACHMENT_LOAD_OP_LOAD,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_color_render_pass_clear, rt_format, VK_FORMAT_UNDEFINED, false, false, VK_ATTACHMENT_LOAD_OP_CLEAR,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_color_render_pass_discard, rt_format, VK_FORMAT_UNDEFINED, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_load, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_clear, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_discard, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);

	m_date_setup_render_pass = GetRenderPass(VK_FORMAT_UNDEFINED, depth_format, VK_ATTACHMENT_LOAD_OP_LOAD,
		VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
		m_features.stencil_buffer ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		m_features.stencil_buffer ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE);
	if (m_date_setup_render_pass == VK_NULL_HANDLE)
		return false;

#undef GET

	return true;
}

bool GSDeviceVK::CompileConvertPipelines()
{
	const std::optional<std::string> source = ReadShaderSource("shaders/vulkan/convert.glsl");
	if (!source)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/convert.glsl.");
		return false;
	}

	VkShaderModule vs = GetUtilityVertexShader(*source);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetVertexShader(vs);

	m_convert.resize(ShaderConvertSelector::NUM_TOTAL_SHADERS);
	for (u32 i = 0; i < ShaderConvertSelector::NUM_TOTAL_SHADERS; i++)
	{
		const ShaderConvertSelector shader = ShaderConvertSelector::Get(i);

		VkRenderPass rp;
		if (shader.DATMConvertShader())
		{
			rp = m_date_setup_render_pass;
		}
		else if (shader.DepthOutput())
		{
			rp = GetRenderPass(
				LookupNativeFormat(GSTexture::Format::Invalid),
				LookupNativeFormat(GSTexture::Format::DepthStencil),
				VK_ATTACHMENT_LOAD_OP_DONT_CARE);
		}
		else
		{
			rp = GetRenderPass(
				LookupNativeFormat(shader.OutputFormat()),
				LookupNativeFormat(GSTexture::Format::Invalid),
				VK_ATTACHMENT_LOAD_OP_DONT_CARE);
		}

		if (!rp)
			return false;

		gpb.SetRenderPass(rp, 0);

		if (shader.DATMConvertShader())
		{
			const VkStencilOpState sos = {
				VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 1u, 1u, 1u};
			gpb.SetDepthState(false, false, VK_COMPARE_OP_ALWAYS);
			gpb.SetStencilState(true, sos, sos);
		}
		else
		{
			gpb.SetDepthState(shader.DepthOutput(), shader.DepthOutput(), VK_COMPARE_OP_ALWAYS);
			gpb.SetNoStencilState();
		}

		gpb.SetColorWriteMask(0, shader.Mask());

		std::string macro;
		macro += fmt::format("#define PRIMID_MAX {}\n", GSShader::PRIMID_MAX);
		macro += fmt::format("#define PRIMID_MIN {}\n", GSShader::PRIMID_MIN);
		macro += fmt::format("#define HAS_BILN {}\n", static_cast<int>(shader.Biln()));
		macro += fmt::format("#define HAS_STENCIL_OUTPUT {}\n", static_cast<int>(shader.StencilOutput()));
		macro += fmt::format("#define HAS_INTEGER_OUTPUT {}\n", static_cast<int>(shader.IntegerOutputBpp() != 0));
		macro += fmt::format("#define HAS_DEPTH_OUTPUT {}\n", static_cast<int>(shader.DepthOutput()));
		macro += fmt::format("#define HAS_FLOAT32_INPUT {}\n", static_cast<int>(shader.Float32Input()));
		macro += fmt::format("#define HAS_FLOAT32_OUTPUT {}\n", static_cast<int>(shader.Float32Output()));

		std::string shader_with_header = macro + *source;

		VkShaderModule ps = GetUtilityFragmentShader(shader_with_header, shader.EntryPoint());
		if (ps == VK_NULL_HANDLE)
			return false;

		ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });
		gpb.SetFragmentShader(ps);

		VkPipeline pipe = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
					
		if (!pipe)
			return false;

		m_convert[i] = pipe;

		Vulkan::SetObjectName(m_device, pipe, "Convert pipeline (%s, mask=%x, depth=%d, biln=%d)",
			shader.Name(), shader.Mask(), static_cast<int>(shader.DepthOutput()),
			static_cast<int>(shader.Biln()));

		if (shader.Shader() == ShaderConvert::COLCLIP_INIT || shader.Shader() == ShaderConvert::COLCLIP_RESOLVE)
		{
			const bool is_setup = shader.Shader() == ShaderConvert::COLCLIP_INIT;
			VkPipeline(&arr)[2][2] = *(is_setup ? &m_colclip_setup_pipelines : &m_colclip_finish_pipelines);
			for (u32 ds = 0; ds < 2; ds++)
			{
				for (u32 fbl = 0; fbl < 2; fbl++)
				{
					pxAssert(!arr[ds][fbl]);

					gpb.SetRenderPass(GetTFXRenderPass(true, ds != 0, is_setup, false, fbl != 0, false,
						VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE),
						0);
					arr[ds][fbl] = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
					if (!arr[ds][fbl])
						return false;

					Vulkan::SetObjectName(m_device, arr[ds][fbl], "ColorClip %s/copy pipeline (ds=%u, fbl=%u)",
						is_setup ? "setup" : "finish", i, ds, fbl);
				}
			}
		}
	}

	// date image setup
	for (u32 ds = 0; ds < 2; ds++)
	{
		for (u32 clear = 0; clear < 2; clear++)
		{
			m_primid_image_setup_render_passes[ds][clear] = GetRenderPass(LookupNativeFormat(GSTexture::Format::PrimID),
				ds ? LookupNativeFormat(GSTexture::Format::DepthStencil) : VK_FORMAT_UNDEFINED,
				VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
				ds ? (clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD) :
					 VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				ds ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE);
		}
	}

	for (u32 datm = 0; datm < 4; datm++)
	{
		std::string macro;
		macro += fmt::format("#define PRIMID_MAX {}\n", GSShader::PRIMID_MAX);
		macro += fmt::format("#define PRIMID_MIN {}\n", GSShader::PRIMID_MIN);

		const std::string source_with_header = macro + *source;

		const std::string entry_point(StringUtil::StdStringFromFormat("ps_primid_image_init_%d", datm));
		VkShaderModule ps = GetUtilityFragmentShader(source_with_header, entry_point.c_str());
		if (ps == VK_NULL_HANDLE)
			return false;

		ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });
		gpb.SetPipelineLayout(m_utility_pipeline_layout);
		gpb.SetFragmentShader(ps);
		gpb.SetNoDepthTestState();
		gpb.SetNoStencilState();
		gpb.ClearBlendAttachments();
		gpb.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT);

		for (u32 ds = 0; ds < 2; ds++)
		{
			gpb.SetRenderPass(m_primid_image_setup_render_passes[ds][0], 0);
			m_primid_image_setup_pipelines[ds][datm] =
				gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
			if (!m_primid_image_setup_pipelines[ds][datm])
				return false;

			Vulkan::SetObjectName(m_device, m_primid_image_setup_pipelines[ds][datm],
				"DATE image clear pipeline (ds=%u, datm=%u)", ds, (datm == 1 || datm == 3));
		}
	}

	return true;
}

#ifdef ORBIS_VULKAN
// PS5 port (vk-285-95): convert.glsl's ps_downsample_copy sums factor x factor texels in loops bounded by
// a push constant. The compiler can't unroll them, so every texelFetch waits for the one before
// (image_load, then s_waitcnt vmcnt(0), 36 times a pixel at 6x): vk-285-94's GPU profile had
// Shadow of the Colossus's native-scaling downsamples at ~0.35 ms each for a 131x448 strip, 11-12 ms of a
// 25 ms frame at 6x. The same code with the factor and step as constants unrolls, and the loads go out
// in clauses. Same texels, same order of adds, same push constants. Made on first use; factors 2 to 8,
// steps 1 and 2. Needs proper testing.
VkPipeline GSDeviceVK::OrbisDownsamplePipeline(u32 factor, u32 step)
{
	if (factor < 2 || factor > 8 || step < 1 || step > 2)
		return VK_NULL_HANDLE;
	VkPipeline& slot = s_orbis_ds_pipe[factor][step];
	if (slot != VK_NULL_HANDLE || s_orbis_ds_failed[factor][step])
		return slot;
	s_orbis_ds_failed[factor][step] = true; // until it's made

	const std::optional<std::string> source = ReadShaderSource("shaders/vulkan/convert.glsl");
	if (!source)
		return VK_NULL_HANDLE;
	VkShaderModule vs = GetUtilityVertexShader(*source);
	if (vs == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });

	const ShaderConvertSelector shader(ShaderConvert::DOWNSAMPLE_COPY);
	std::string text;
	text += fmt::format("#define PRIMID_MAX {}\n", GSShader::PRIMID_MAX);
	text += fmt::format("#define PRIMID_MIN {}\n", GSShader::PRIMID_MIN);
	text += fmt::format("#define HAS_BILN {}\n", static_cast<int>(shader.Biln()));
	text += fmt::format("#define HAS_STENCIL_OUTPUT {}\n", static_cast<int>(shader.StencilOutput()));
	text += fmt::format("#define HAS_INTEGER_OUTPUT {}\n", static_cast<int>(shader.IntegerOutputBpp() != 0));
	text += fmt::format("#define HAS_DEPTH_OUTPUT {}\n", static_cast<int>(shader.DepthOutput()));
	text += fmt::format("#define HAS_FLOAT32_INPUT {}\n", static_cast<int>(shader.Float32Input()));
	text += fmt::format("#define HAS_FLOAT32_OUTPUT {}\n", static_cast<int>(shader.Float32Output()));
	text += fmt::format("#define ORBIS_DS_FACTOR {}\n#define ORBIS_DS_STEP {}\n", factor, step);
	text += *source;
	text += R"(
#ifdef FRAGMENT_SHADER
layout(push_constant) uniform cb10
{
	ivec2 ClampMin;
	int DownsampleFactor;
	int pad0;
	float Weight;
	float step_multiplier;
	vec2 pad1;
};
void orbis_downsample_fixed()
{
	ivec2 coord = max(ivec2(gl_FragCoord.xy) * ORBIS_DS_FACTOR, ClampMin);
	vec4 result = vec4(0);
	for (int yoff = 0; yoff < ORBIS_DS_FACTOR; yoff++)
	{
		for (int xoff = 0; xoff < ORBIS_DS_FACTOR; xoff++)
		{
			result += texelFetch(samp0, coord + ivec2(xoff * ORBIS_DS_STEP, yoff * ORBIS_DS_STEP), 0);
		}
	}
	OUTPUT = result / Weight;
}
#endif
)";
	VkShaderModule ps = GetUtilityFragmentShader(text, "orbis_downsample_fixed");
	if (ps == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;
	ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });

	const VkRenderPass rp = GetRenderPass(LookupNativeFormat(shader.OutputFormat()),
		LookupNativeFormat(GSTexture::Format::Invalid), VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	if (!rp)
		return VK_NULL_HANDLE;

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetVertexShader(vs);
	gpb.SetRenderPass(rp, 0);
	gpb.SetDepthState(false, false, VK_COMPARE_OP_ALWAYS);
	gpb.SetNoStencilState();
	gpb.SetColorWriteMask(0, shader.Mask());
	gpb.SetFragmentShader(ps);
	slot = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
	if (slot == VK_NULL_HANDLE)
	{
		printf("[vkhw] downsample pipeline %ux%u step %u: failed, the original shader stays\n", factor, factor, step);
		return VK_NULL_HANDLE;
	}
	s_orbis_ds_failed[factor][step] = false;
	Vulkan::SetObjectName(m_device, slot, "Downsample pipeline (factor %u, step %u)", factor, step);
	printf("[vkhw] downsample pipeline %ux%u step %u made\n", factor, factor, step);
	return slot;
}
#endif

bool GSDeviceVK::CompilePresentPipelines()
{
	// we may not have a swap chain if running in headless mode.
	m_swap_chain_render_pass =
		GetRenderPass(m_swap_chain ? m_swap_chain->GetTextureFormat() : VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED);
	if (m_swap_chain_render_pass == VK_NULL_HANDLE)
		return false;

	const std::optional<std::string> shader = ReadShaderSource("shaders/vulkan/present.glsl");
	if (!shader)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/present.glsl.");
		return false;
	}

	VkShaderModule vs = GetUtilityVertexShader(*shader);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetVertexShader(vs);
	gpb.SetDepthState(false, false, VK_COMPARE_OP_ALWAYS);
	gpb.SetNoStencilState();
	gpb.SetRenderPass(m_swap_chain_render_pass, 0);

	for (PresentShader i = PresentShader::COPY; i < PresentShader::Count; i = static_cast<PresentShader>(static_cast<int>(i) + 1))
	{
		const int index = static_cast<int>(i);

		VkShaderModule ps = GetUtilityFragmentShader(*shader, ShaderEntryPoint(i));
		if (ps == VK_NULL_HANDLE)
			return false;

		ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });
		gpb.SetFragmentShader(ps);

		m_present[index] = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!m_present[index])
			return false;

		Vulkan::SetObjectName(m_device, m_present[index], "Present pipeline %d", i);
	}

#ifdef ORBIS_VULKAN
	// Test build 1 (vk-285-55): the COPY shader again, alpha-blended over the frame, for the testing
	// watermark. Needs proper testing on other drivers; the PS5 driver blends the shelf's UI the same way.
	{
		VkShaderModule ps = GetUtilityFragmentShader(*shader, ShaderEntryPoint(PresentShader::COPY));
		if (ps != VK_NULL_HANDLE)
		{
			gpb.SetFragmentShader(ps);
			gpb.SetBlendAttachment(0, true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
				VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD);
			s_orbis_present_blend = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
			vkDestroyShaderModule(m_device, ps, nullptr);
			gpb.SetNoBlendingState();
		}
		if (s_orbis_present_blend != VK_NULL_HANDLE)
			Vulkan::SetObjectName(m_device, s_orbis_present_blend, "Present blend pipeline (watermark)");
		else
			Console.Warning("VK: the watermark's blended present pipeline couldn't be made");
	}
#endif

	return true;
}


bool GSDeviceVK::CompileInterlacePipelines()
{
	const std::optional<std::string> shader = ReadShaderSource("shaders/vulkan/interlace.glsl");
	if (!shader)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/interlace.glsl.");
		return false;
	}

	VkRenderPass rp =
		GetRenderPass(LookupNativeFormat(GSTexture::Format::Color), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);
	if (!rp)
		return false;

	VkShaderModule vs = GetUtilityVertexShader(*shader);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetNoBlendingState();
	gpb.SetRenderPass(rp, 0);
	gpb.SetVertexShader(vs);

	for (int i = 0; i < static_cast<int>(m_interlace.size()); i++)
	{
		VkShaderModule ps = GetUtilityFragmentShader(*shader, StringUtil::StdStringFromFormat("ps_main%d", i).c_str());
		if (ps == VK_NULL_HANDLE)
			return false;

		gpb.SetFragmentShader(ps);

		m_interlace[i] = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		vkDestroyShaderModule(m_device, ps, nullptr);
		if (!m_interlace[i])
			return false;

		Vulkan::SetObjectName(m_device, m_interlace[i], "Interlace pipeline %d", i);
	}

	return true;
}

bool GSDeviceVK::CompileMergePipelines()
{
	const std::optional<std::string> shader = ReadShaderSource("shaders/vulkan/merge.glsl");
	if (!shader)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/merge.glsl.");
		return false;
	}

	VkRenderPass rp =
		GetRenderPass(LookupNativeFormat(GSTexture::Format::Color), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);
	if (!rp)
		return false;

	VkShaderModule vs = GetUtilityVertexShader(*shader);
	if (vs == VK_NULL_HANDLE)
		return false;
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetRenderPass(rp, 0);
	gpb.SetVertexShader(vs);

	for (int i = 0; i < static_cast<int>(m_merge.size()); i++)
	{
		VkShaderModule ps = GetUtilityFragmentShader(*shader, StringUtil::StdStringFromFormat("ps_main%d", i).c_str());
		if (ps == VK_NULL_HANDLE)
			return false;

		gpb.SetFragmentShader(ps);
		gpb.SetBlendAttachment(0, true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);

		m_merge[i] = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		vkDestroyShaderModule(m_device, ps, nullptr);
		if (!m_merge[i])
			return false;

		Vulkan::SetObjectName(m_device, m_merge[i], "Merge pipeline %d", i);
	}

	return true;
}

bool GSDeviceVK::CompilePostProcessingPipelines()
{
	VkRenderPass rp =
		GetRenderPass(LookupNativeFormat(GSTexture::Format::Color), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);
	if (!rp)
		return false;

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetNoBlendingState();
	gpb.SetRenderPass(rp, 0);

	{
		const std::optional<std::string> vshader = ReadShaderSource("shaders/vulkan/convert.glsl");
		if (!vshader)
		{
			Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/convert.glsl.");
			return false;
		}

		const std::optional<std::string> pshader = ReadShaderSource("shaders/common/fxaa.fx");
		if (!pshader)
		{
			Host::ReportErrorAsync("GS", "Failed to read shaders/common/fxaa.fx.");
			return false;
		}

		const std::string psource = "#define FXAA_GLSL_VK 1\n" + *pshader;

		VkShaderModule vs = GetUtilityVertexShader(*vshader);
		if (vs == VK_NULL_HANDLE)
			return false;
		ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });
		VkShaderModule ps = GetUtilityFragmentShader(psource, "ps_main");
		if (ps == VK_NULL_HANDLE)
			return false;
		ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });
		gpb.SetVertexShader(vs);
		gpb.SetFragmentShader(ps);

		m_fxaa_pipeline = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!m_fxaa_pipeline)
			return false;

		Vulkan::SetObjectName(m_device, m_fxaa_pipeline, "FXAA pipeline");
	}

	{
		const std::optional<std::string> shader = ReadShaderSource("shaders/vulkan/shadeboost.glsl");
		if (!shader)
		{
			Host::ReportErrorAsync("GS", "Failed to read shaders/vulkan/shadeboost.glsl.");
			return false;
		}

		VkShaderModule vs = GetUtilityVertexShader(*shader);
		ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });
		if (vs == VK_NULL_HANDLE)
			return false;

		VkShaderModule ps = GetUtilityFragmentShader(*shader);
		ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });
		if (ps == VK_NULL_HANDLE)
			return false;

		gpb.SetVertexShader(vs);
		gpb.SetFragmentShader(ps);

		m_shadeboost_pipeline = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!m_shadeboost_pipeline)
			return false;

		Vulkan::SetObjectName(m_device, m_shadeboost_pipeline, "Shadeboost pipeline");
	}

	return true;
}

bool GSDeviceVK::CompileCASPipelines()
{
	VkDevice dev = m_device;
	Vulkan::DescriptorSetLayoutBuilder dslb;
	Vulkan::PipelineLayoutBuilder plb;

	dslb.SetPushFlag();
	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
	if ((m_cas_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_cas_ds_layout, "CAS descriptor layout");

	plb.AddPushConstants(VK_SHADER_STAGE_COMPUTE_BIT, 0, NUM_CAS_CONSTANTS * sizeof(u32));
	plb.AddDescriptorSet(m_cas_ds_layout);
	if ((m_cas_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(dev, m_cas_pipeline_layout, "CAS pipeline layout");

	// we use specialization constants to avoid compiling it twice
	std::optional<std::string> cas_source = ReadShaderSource("shaders/vulkan/cas.glsl");
	if (!cas_source.has_value() || !GetCASShaderSource(&cas_source.value()))
		return false;

	VkShaderModule mod = g_vulkan_shader_cache->GetComputeShader(cas_source->c_str());
	ScopedGuard mod_guard = [this, &mod]() { vkDestroyShaderModule(m_device, mod, nullptr); };
	if (mod == VK_NULL_HANDLE)
		return false;

	for (u8 sharpen_only = 0; sharpen_only < 2; sharpen_only++)
	{
		Vulkan::ComputePipelineBuilder cpb;
		cpb.SetPipelineLayout(m_cas_pipeline_layout);
		cpb.SetShader(mod, "main");
		cpb.SetSpecializationBool(0, sharpen_only != 0);
		m_cas_pipelines[sharpen_only] = cpb.Create(dev, g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!m_cas_pipelines[sharpen_only])
			return false;
	}

	m_features.cas_sharpening = true;
	return true;
}

bool GSDeviceVK::CompileImGuiPipeline()
{
	const std::optional<std::string> glsl = ReadShaderSource("shaders/vulkan/imgui.glsl");
	if (!glsl.has_value())
	{
		Console.Error("VK: Failed to read imgui.glsl");
		return false;
	}

	VkShaderModule vs = GetUtilityVertexShader(glsl.value(), "vs_main");
	if (vs == VK_NULL_HANDLE)
	{
		Console.Error("VK: Failed to compile ImGui vertex shader");
		return false;
	}
	ScopedGuard vs_guard([this, &vs]() { vkDestroyShaderModule(m_device, vs, nullptr); });

	VkShaderModule ps = GetUtilityFragmentShader(glsl.value(), "ps_main");
	if (ps == VK_NULL_HANDLE)
	{
		Console.Error("VK: Failed to compile ImGui pixel shader");
		return false;
	}
	ScopedGuard ps_guard([this, &ps]() { vkDestroyShaderModule(m_device, ps, nullptr); });

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetRenderPass(m_swap_chain_render_pass, 0);
	gpb.AddVertexBuffer(0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX);
	gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, pos));
	gpb.AddVertexAttribute(1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, uv));
	gpb.AddVertexAttribute(2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col));
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	gpb.SetVertexShader(vs);
	gpb.SetFragmentShader(ps);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetBlendAttachment(0, true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
		VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);

	m_imgui_pipeline = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(), false);
	if (!m_imgui_pipeline)
	{
		Console.Error("VK: Failed to compile ImGui pipeline");
		return false;
	}

	Vulkan::SetObjectName(m_device, m_imgui_pipeline, "ImGui pipeline");
	return true;
}

void GSDeviceVK::RenderImGui()
{
	// Orbis: this port has no ImGui backend, so no context is ever created (the same
	// guard as GSDeviceOGL::RenderImGui). ImGui::Render() would dereference the null
	// context and kill the MTGS thread at the first present (vk-285-2).
	if (!ImGui::GetCurrentContext())
		return;

	ImGui::Render();
	const ImDrawData* draw_data = ImGui::GetDrawData();
	if (draw_data->CmdLists.Size == 0)
		return;

	UpdateImGuiTextures();

	const GSVector4 uniforms(
		2.0f / static_cast<float>(m_window_info.surface_width),
		2.0f / static_cast<float>(m_window_info.surface_height),
		-1.0f,
		-1.0f);

	SetUtilityPushConstants(&uniforms, sizeof(uniforms));
	SetPipeline(m_imgui_pipeline);

	if (m_utility_sampler != m_linear_sampler)
	{
		m_utility_sampler = m_linear_sampler;
		m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
	}

	// this is for presenting, we don't want to screw with the viewport/scissor set by display
	m_dirty_flags &= ~(DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR);

	for (int n = 0; n < draw_data->CmdLists.Size; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];

		u32 vertex_offset;
		{
			const u32 size = sizeof(ImDrawVert) * static_cast<u32>(cmd_list->VtxBuffer.Size);
			if (!m_vertex_stream_buffer.ReserveMemory(size, sizeof(ImDrawVert)))
			{
				Console.Warning("VK: Skipping ImGui draw because of no vertex buffer space");
				return;
			}

			vertex_offset = m_vertex_stream_buffer.GetCurrentOffset() / sizeof(ImDrawVert);
			std::memcpy(m_vertex_stream_buffer.GetCurrentHostPointer(), cmd_list->VtxBuffer.Data, size);
			m_vertex_stream_buffer.CommitMemory(size);
		}

		static_assert(sizeof(ImDrawIdx) == sizeof(u16));
		IASetIndexBuffer(cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size);

		for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
		{
			const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
			pxAssert(!pcmd->UserCallback);

			const GSVector4 clip = GSVector4::load<false>(&pcmd->ClipRect);
			if ((clip.zwzw() <= clip.xyxy()).mask() != 0)
				continue;

			SetScissor(GSVector4i(clip).max_i32(GSVector4i::zero()));

			// Since we don't have the GSTexture...
			GSTextureVK* tex = reinterpret_cast<GSTextureVK*>(pcmd->GetTexID());
			if (tex)
				SetUtilityTexture(tex, m_linear_sampler);

			if (ApplyUtilityState())
			{
				vkCmdDrawIndexed(GetCurrentCommandBuffer(), pcmd->ElemCount, 1, m_index.start + pcmd->IdxOffset,
					vertex_offset + pcmd->VtxOffset, 0);
			}
		}

		g_perfmon.Put(GSPerfMon::DrawCalls, cmd_list->CmdBuffer.Size);
	}
}

void GSDeviceVK::RenderBlankFrame()
{
	VkResult res = m_swap_chain->AcquireNextImage();
	if (res != VK_SUCCESS)
	{
		Console.Error("VK: Failed to acquire image for blank frame present");
		return;
	}

	VkCommandBuffer cmdbuffer = GetCurrentCommandBuffer();
	GSTextureVK* sctex = m_swap_chain->GetCurrentTexture();
#ifdef ORBIS_VULKAN
	// PS5: the swap chain images are not transfer destinations (VKSwapChain.cpp), so
	// the blank frame is an empty render pass that clears on load, as BeginPresent's.
	sctex->OverrideImageLayout(GSTextureVK::Layout::Undefined);
	sctex->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ColorAttachment);
	const VkFramebuffer fb = sctex->GetFramebuffer(false);
	if (fb != VK_NULL_HANDLE)
	{
		const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,
			GetRenderPass(sctex->GetVkFormat(), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR,
				VK_ATTACHMENT_STORE_OP_STORE),
			fb, {{0, 0}, {static_cast<u32>(sctex->GetWidth()), static_cast<u32>(sctex->GetHeight())}}, 1u,
			&s_present_clear_color};
		vkCmdBeginRenderPass(cmdbuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdEndRenderPass(cmdbuffer);
	}
#else
	sctex->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::TransferDst);

	constexpr VkImageSubresourceRange srr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(
		cmdbuffer, sctex->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &s_present_clear_color.color, 1, &srr);
#endif

	m_swap_chain->GetCurrentTexture()->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::PresentSrc);
	SubmitCommandBuffer(m_swap_chain.get());
	ActivateCommandBuffer((m_current_frame + 1) % NUM_COMMAND_BUFFERS);
}

bool GSDeviceVK::DoCAS(
	GSTexture* sTex, GSTexture* dTex, bool sharpen_only, const std::array<u32, NUM_CAS_CONSTANTS>& constants)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	EndRenderPass();

	GSTextureVK* const sTexVK = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* const dTexVK = static_cast<GSTextureVK*>(dTex);
	VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();

	sTexVK->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ShaderReadOnly);
	dTexVK->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ComputeReadWriteImage);

	// only happening once a frame, so the update isn't a huge deal.
	Vulkan::DescriptorSetUpdateBuilder dsub;
	dsub.AddImageDescriptorWrite(VK_NULL_HANDLE, 0, sTexVK->GetView(), sTexVK->GetVkLayout());
	dsub.AddStorageImageDescriptorWrite(VK_NULL_HANDLE, 1, dTexVK->GetView(), dTexVK->GetVkLayout());
	dsub.PushUpdate(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_cas_pipeline_layout, 0, false);

	// the actual meat and potatoes! only four commands.
	static const int threadGroupWorkRegionDim = 16;
	const int dispatchX = (dTex->GetWidth() + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	const int dispatchY = (dTex->GetHeight() + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;

	vkCmdPushConstants(cmdbuf, m_cas_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, NUM_CAS_CONSTANTS * sizeof(u32),
		constants.data());
	vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_cas_pipelines[static_cast<u8>(sharpen_only)]);
	vkCmdDispatch(cmdbuf, dispatchX, dispatchY, 1);

	dTexVK->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);

	// all done!
	return true;
}

void GSDeviceVK::DestroyResources()
{
	if (m_tfx_ubo_descriptor_set != VK_NULL_HANDLE)
		FreePersistentDescriptorSet(m_tfx_ubo_descriptor_set);

	for (auto& it : m_tfx_pipelines)
		vkDestroyPipeline(m_device, it.second, nullptr);
#ifdef ORBIS_VULKAN
	for (OrbisPipeSlot& slot : m_orbis_pipe_cache) // vk-285-90
		slot.pipeline = VK_NULL_HANDLE;
#endif
	for (auto& it : m_tfx_fragment_shaders)
		vkDestroyShaderModule(m_device, it.second, nullptr);
	for (auto& it : m_tfx_vertex_shaders)
		vkDestroyShaderModule(m_device, it.second, nullptr);
	for (VkPipeline it : m_interlace)
	{
		if (it != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, it, nullptr);
	}
	for (VkPipeline it : m_merge)
	{
		if (it != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, it, nullptr);
	}
	for (VkPipeline it : m_present)
	{
		if (it != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, it, nullptr);
	}
#ifdef ORBIS_VULKAN
	if (s_orbis_present_blend != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(m_device, s_orbis_present_blend, nullptr);
		s_orbis_present_blend = VK_NULL_HANDLE;
	}
#endif
	for (const auto& pipe : m_convert)
	{
		if (pipe != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, pipe, nullptr);
	}
	for (u32 ds = 0; ds < 2; ds++)
	{
		for (u32 fbl = 0; fbl < 2; fbl++)
		{
			if (m_colclip_setup_pipelines[ds][fbl] != VK_NULL_HANDLE)
				vkDestroyPipeline(m_device, m_colclip_setup_pipelines[ds][fbl], nullptr);
			if (m_colclip_finish_pipelines[ds][fbl] != VK_NULL_HANDLE)
				vkDestroyPipeline(m_device, m_colclip_finish_pipelines[ds][fbl], nullptr);
		}
	}
	for (u32 ds = 0; ds < 2; ds++)
	{
		for (u32 datm = 0; datm < 4; datm++)
		{
			if (m_primid_image_setup_pipelines[ds][datm] != VK_NULL_HANDLE)
				vkDestroyPipeline(m_device, m_primid_image_setup_pipelines[ds][datm], nullptr);
		}
	}
	if (m_fxaa_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(m_device, m_fxaa_pipeline, nullptr);
	if (m_shadeboost_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(m_device, m_shadeboost_pipeline, nullptr);

	for (VkPipeline it : m_cas_pipelines)
	{
		if (it != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, it, nullptr);
	}
	if (m_cas_pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(m_device, m_cas_pipeline_layout, nullptr);
	if (m_cas_ds_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_device, m_cas_ds_layout, nullptr);
	if (m_imgui_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(m_device, m_imgui_pipeline, nullptr);

	for (const auto& it : m_samplers)
	{
		if (it.second != VK_NULL_HANDLE)
			vkDestroySampler(m_device, it.second, nullptr);
	}
	m_samplers.clear();

	m_texture_stream_buffer.Destroy(false);
	m_fragment_uniform_stream_buffer.Destroy(false);
	m_vertex_uniform_stream_buffer.Destroy(false);
	m_index_stream_buffer.Destroy(false);
	m_vertex_stream_buffer.Destroy(false);
	m_expand_index_stream_buffer.Destroy(false);
	if (m_expand_index_buffer != VK_NULL_HANDLE)
		vmaDestroyBuffer(m_allocator, m_expand_index_buffer, m_expand_index_buffer_allocation);

	if (m_tfx_pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(m_device, m_tfx_pipeline_layout, nullptr);
	if (m_tfx_texture_ds_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_device, m_tfx_texture_ds_layout, nullptr);
	if (m_tfx_ubo_ds_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_device, m_tfx_ubo_ds_layout, nullptr);
	if (m_utility_pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(m_device, m_utility_pipeline_layout, nullptr);
	if (m_utility_ds_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_device, m_utility_ds_layout, nullptr);

	if (m_null_framebuffer != VK_NULL_HANDLE)
	{
		vkDestroyFramebuffer(m_device, m_null_framebuffer, nullptr);
	}

	if (m_null_texture)
	{
		m_null_texture->Destroy(false);
		m_null_texture.reset();
	}

	for (FrameResources& resources : m_frame_resources)
	{
		for (auto& it : resources.cleanup_resources)
			it();
		resources.cleanup_resources.clear();

		if (resources.fence != VK_NULL_HANDLE)
			vkDestroyFence(m_device, resources.fence, nullptr);
		if (resources.command_buffers[0] != VK_NULL_HANDLE)
		{
			vkFreeCommandBuffers(m_device, resources.command_pool, static_cast<u32>(resources.command_buffers.size()),
				resources.command_buffers.data());
		}
		if (resources.command_pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(m_device, resources.command_pool, nullptr);
	}

	if (m_timestamp_query_pool != VK_NULL_HANDLE)
		vkDestroyQueryPool(m_device, m_timestamp_query_pool, nullptr);
#ifdef ORBIS_VULKAN
	OrbisGpDestroy(); // vk-285-94
	for (u32 f = 0; f < 9; f++) // vk-285-95
	{
		for (u32 st = 0; st < 3; st++)
		{
			if (s_orbis_ds_pipe[f][st] != VK_NULL_HANDLE)
				vkDestroyPipeline(m_device, s_orbis_ds_pipe[f][st], nullptr);
			s_orbis_ds_pipe[f][st] = VK_NULL_HANDLE;
			s_orbis_ds_failed[f][st] = false;
		}
	}
#endif

	if (m_pipeline_statistics_query_pool != VK_NULL_HANDLE)
		vkDestroyQueryPool(m_device, m_pipeline_statistics_query_pool, nullptr);

	if (m_global_descriptor_pool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(m_device, m_global_descriptor_pool, nullptr);

	for (auto& it : m_render_pass_cache)
		vkDestroyRenderPass(m_device, it.second, nullptr);
	m_render_pass_cache.clear();

	if (m_allocator != VK_NULL_HANDLE)
		vmaDestroyAllocator(m_allocator);
}

VkShaderModule GSDeviceVK::GetTFXVertexShader(GSHWDrawConfig::VSSelector sel)
{
	const auto it = m_tfx_vertex_shaders.find(sel.key);
	if (it != m_tfx_vertex_shaders.end())
		return it->second;

	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, true, false, false);
	AddMacro(ss, "VS_TME", sel.tme);
	AddMacro(ss, "VS_FST", sel.fst);
	AddMacro(ss, "VS_IIP", sel.iip);
	AddMacro(ss, "VS_POINT_SIZE", sel.point_size);
	AddMacro(ss, "VS_EXPAND", static_cast<int>(sel.expand));
	AddMacro(ss, "VS_PROVOKING_VERTEX_LAST", static_cast<int>(m_features.provoking_vertex_last));
	ss << m_tfx_source;

	VkShaderModule mod = g_vulkan_shader_cache->GetVertexShader(ss.str());
	if (mod)
		Vulkan::SetObjectName(m_device, mod, "TFX Vertex %08X", sel.key);

	m_tfx_vertex_shaders.emplace(sel.key, mod);
	return mod;
}

VkShaderModule GSDeviceVK::GetTFXFragmentShader(const GSHWDrawConfig::PSSelector& sel)
{
	const auto it = m_tfx_fragment_shaders.find(sel);
	if (it != m_tfx_fragment_shaders.end())
		return it->second;

	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, false, false, true);
	AddMacro(ss, "PS_FST", sel.fst);
	AddMacro(ss, "PS_WMS", sel.wms);
	AddMacro(ss, "PS_WMT", sel.wmt);
	AddMacro(ss, "PS_ADJS", sel.adjs);
	AddMacro(ss, "PS_ADJT", sel.adjt);
	AddMacro(ss, "PS_AEM_FMT", sel.aem_fmt);
	AddMacro(ss, "PS_PAL_FMT", sel.pal_fmt);
	AddMacro(ss, "PS_DST_FMT", sel.dst_fmt);
	AddMacro(ss, "PS_DEPTH_FMT", sel.depth_fmt);
	AddMacro(ss, "PS_CHANNEL_FETCH", sel.channel);
	AddMacro(ss, "PS_URBAN_CHAOS_HLE", sel.urban_chaos_hle);
	AddMacro(ss, "PS_TALES_OF_ABYSS_HLE", sel.tales_of_abyss_hle);
	AddMacro(ss, "PS_AEM", sel.aem);
	AddMacro(ss, "PS_TFX", sel.tfx);
	AddMacro(ss, "PS_TCC", sel.tcc);
	AddMacro(ss, "PS_ATST", static_cast<u32>(sel.atst));
	AddMacro(ss, "PS_AFAIL", static_cast<u32>(sel.afail));
	AddMacro(ss, "PS_FOG", sel.fog);
	AddMacro(ss, "PS_BLEND_HW", sel.blend_hw);
	AddMacro(ss, "PS_A_MASKED", sel.a_masked);
	AddMacro(ss, "PS_FBA", sel.fba);
	AddMacro(ss, "PS_LTF", sel.ltf);
	AddMacro(ss, "PS_AUTOMATIC_LOD", sel.automatic_lod);
	AddMacro(ss, "PS_MANUAL_LOD", sel.manual_lod);
	AddMacro(ss, "PS_COLCLIP", sel.colclip);
	AddMacro(ss, "PS_DATE", sel.date);
	AddMacro(ss, "PS_TCOFFSETHACK", sel.tcoffsethack);
	AddMacro(ss, "PS_REGION_RECT", sel.region_rect);
	AddMacro(ss, "PS_BLEND_A", sel.blend_a);
	AddMacro(ss, "PS_BLEND_B", sel.blend_b);
	AddMacro(ss, "PS_BLEND_C", sel.blend_c);
	AddMacro(ss, "PS_BLEND_D", sel.blend_d);
	AddMacro(ss, "PS_BLEND_MIX", sel.blend_mix);
	AddMacro(ss, "PS_ROUND_INV", sel.round_inv);
	AddMacro(ss, "PS_FIXED_ONE_A", sel.fixed_one_a);
	AddMacro(ss, "PS_IIP", sel.iip);
	AddMacro(ss, "PS_SHUFFLE", sel.shuffle);
	AddMacro(ss, "PS_SHUFFLE_SAME", sel.shuffle_same);
	AddMacro(ss, "PS_PROCESS_BA", sel.process_ba);
	AddMacro(ss, "PS_PROCESS_RG", sel.process_rg);
	AddMacro(ss, "PS_SHUFFLE_ACROSS", sel.shuffle_across);
	AddMacro(ss, "PS_READ16_SRC", sel.real16src);
	AddMacro(ss, "PS_WRITE_RG", sel.write_rg);
	AddMacro(ss, "PS_FBMASK", sel.fbmask);
	AddMacro(ss, "PS_COLCLIP_HW", sel.colclip_hw);
	AddMacro(ss, "PS_RTA_CORRECTION", sel.rta_correction);
	AddMacro(ss, "PS_RTA_SRC_CORRECTION", sel.rta_source_correction);
	AddMacro(ss, "PS_DITHER", sel.dither);
	AddMacro(ss, "PS_DITHER_ADJUST", sel.dither_adjust);
	AddMacro(ss, "PS_ZCLAMP", sel.zclamp);
	AddMacro(ss, "PS_ZFLOOR", sel.zfloor);
	AddMacro(ss, "PS_PABE", sel.pabe);
	AddMacro(ss, "PS_SCANMSK", sel.scanmsk);
	AddMacro(ss, "PS_TEX_IS_FB", sel.tex_is_fb);
	AddMacro(ss, "PS_NO_COLOR", sel.no_color);
	AddMacro(ss, "PS_NO_COLOR1", sel.no_color1);
	AddMacro(ss, "PS_ZTST", sel.ztst);
	AddMacro(ss, "PS_AA1", static_cast<u32>(sel.aa1));
	AddMacro(ss, "PS_ABE", sel.abe);
	AddMacro(ss, "PS_ANISOTROPIC_FILTERING", sel.sw_aniso);
	AddMacro(ss, "PS_ROV_COLOR", sel.rov_color);
	AddMacro(ss, "PS_ROV_DEPTH", static_cast<u32>(sel.rov_depth));
	ss << m_tfx_source;

	VkShaderModule mod = g_vulkan_shader_cache->GetFragmentShader(ss.str());
	if (mod)
		Vulkan::SetObjectName(m_device, mod, "TFX Fragment %016" PRIX64 "_%016" PRIX64, sel.key_hi, sel.key_lo);

	m_tfx_fragment_shaders.emplace(sel, mod);
	return mod;
}

VkPipeline GSDeviceVK::CreateTFXPipeline(const PipelineSelector& p)
{
	static constexpr std::array<VkPrimitiveTopology, 3> topology_lookup = {{
		VK_PRIMITIVE_TOPOLOGY_POINT_LIST, // Point
		VK_PRIMITIVE_TOPOLOGY_LINE_LIST, // Line
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, // Triangle
	}};

	GSHWDrawConfig::BlendState pbs{p.bs};
	GSHWDrawConfig::PSSelector pps{p.ps};
	if (!p.bs.IsEffective(p.cms))
	{
		// disable blending when colours are masked
		pbs = {};
		pps.no_color1 = true;
	}

	VkShaderModule vs = GetTFXVertexShader(p.vs);
	VkShaderModule fs = GetTFXFragmentShader(pps);
	if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);

	// Common state
	gpb.SetPipelineLayout(m_tfx_pipeline_layout);
	if (IsDATEModePrimIDInit(p.ps.date))
	{
		// DATE image prepass
		gpb.SetRenderPass(m_primid_image_setup_render_passes[p.ds][0], 0);
	}
	else
	{
		gpb.SetRenderPass(
			GetTFXRenderPass(p.rt, p.ds, p.ps.colclip_hw, p.dss.date,
				p.IsRTFeedbackLoop(), p.IsTestingAndSamplingDepth(),
				p.rt ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				p.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
			0);
	}
	gpb.SetPrimitiveTopology(topology_lookup[p.topology]);
	gpb.SetRasterizationState(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	if (m_optional_extensions.vk_ext_line_rasterization &&
		p.topology == static_cast<u8>(GSHWDrawConfig::Topology::Line))
	{
		gpb.SetLineRasterizationMode(VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT);
	}
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);

	// Shaders
	gpb.SetVertexShader(vs);
	gpb.SetFragmentShader(fs);

	// IA
	if (p.vs.expand == GSHWDrawConfig::VSExpand::None)
	{
		gpb.AddVertexBuffer(0, sizeof(GSVertex));
		gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, 0); // ST
		gpb.AddVertexAttribute(1, 0, VK_FORMAT_R8G8B8A8_UINT, 8); // RGBA
		gpb.AddVertexAttribute(2, 0, VK_FORMAT_R32_SFLOAT, 12); // Q
		gpb.AddVertexAttribute(3, 0, VK_FORMAT_R16G16_UINT, 16); // XY
		gpb.AddVertexAttribute(4, 0, VK_FORMAT_R32_UINT, 20); // Z
		gpb.AddVertexAttribute(5, 0, VK_FORMAT_R16G16_UINT, 24); // UV
		gpb.AddVertexAttribute(6, 0, VK_FORMAT_R8G8B8A8_UNORM, 28); // FOG
	}

	// DepthStencil
	static const VkCompareOp ztst[] = {
		VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER};
	gpb.SetDepthState((p.dss.ztst != ZTST_ALWAYS || p.dss.zwe), p.dss.zwe, ztst[p.dss.ztst]);
	if (p.dss.date)
	{
		const VkStencilOpState sos{VK_STENCIL_OP_KEEP, p.dss.date_one ? VK_STENCIL_OP_ZERO : VK_STENCIL_OP_KEEP,
			VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 1u, 1u, 1u};
		gpb.SetStencilState(true, sos, sos);
	}

	// Blending
	if (IsDATEModePrimIDInit(p.ps.date))
	{
		// image DATE prepass
		gpb.SetBlendAttachment(0, true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_MIN, VK_BLEND_FACTOR_ONE,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT);
	}
	else if (pbs.enable)
	{
		// clang-format off
		static constexpr std::array<VkBlendFactor, 16> vk_blend_factors = { {
			VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
			VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
			VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
			VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO
		}};
		static constexpr std::array<VkBlendOp, 3> vk_blend_ops = {{
				VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT
		}};
		// clang-format on

		gpb.SetBlendAttachment(0, true, vk_blend_factors[pbs.src_factor], vk_blend_factors[pbs.dst_factor],
			vk_blend_ops[pbs.op], vk_blend_factors[pbs.src_factor_alpha], vk_blend_factors[pbs.dst_factor_alpha],
			VK_BLEND_OP_ADD, p.cms.wrgba);
	}
	else
	{
		gpb.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, p.cms.wrgba);
	}

	// Tests have shown that it's faster to just enable rast order on the entire pass, rather than alternating
	// between turning it on and off for different draws, and adding the required barrier between non-rast-order
	// and rast-order draws.
	if (m_features.framebuffer_fetch && p.IsRTFeedbackLoop())
		gpb.AddBlendFlags(VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT);

	VkPipeline pipeline = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true));
	if (pipeline)
	{
		Vulkan::SetObjectName(
			m_device, pipeline, "TFX Pipeline %08X/%016" PRIX64 "_%016" PRIX64, p.vs.key, p.ps.key_hi, p.ps.key_lo);
	}

	return pipeline;
}

VkPipeline GSDeviceVK::GetTFXPipeline(const PipelineSelector& p)
{
#ifdef ORBIS_VULKAN
	// PS5 port (vk-285-85): consecutive draws often repeat the selector; the hash and map lookup were
	// ~1.3% of the GS thread in vk-285-84's Shadow of the Colossus profile. vk-285-90: a 64-entry
	// direct-mapped cache instead of the last lookup alone (the map lookup was still ~1.5% in vk-285-89's:
	// the game alternates between a few pipelines). Needs proper testing.
	u64 w[4];
	std::memcpy(w, &p, sizeof(w));
	static_assert(sizeof(PipelineSelector) == sizeof(w));
	OrbisPipeSlot& slot = m_orbis_pipe_cache[((w[0] ^ w[1] ^ w[2] ^ w[3]) * 0x9E3779B97F4A7C15ull) >> 58];
	if (slot.pipeline != VK_NULL_HANDLE && slot.sel == p)
		return slot.pipeline;
#endif
	const auto it = m_tfx_pipelines.find(p);
	if (it != m_tfx_pipelines.end())
	{
#ifdef ORBIS_VULKAN
		slot.sel = p;
		slot.pipeline = it->second;
#endif
		return it->second;
	}

	VkPipeline pipeline = CreateTFXPipeline(p);
	m_tfx_pipelines.emplace(p, pipeline);
#ifdef ORBIS_VULKAN
	slot.sel = p;
	slot.pipeline = pipeline;
#endif
	return pipeline;
}

bool GSDeviceVK::BindDrawPipeline(const PipelineSelector& p)
{
	VkPipeline pipeline = GetTFXPipeline(p);
	if (pipeline == VK_NULL_HANDLE)
		return false;
#ifdef ORBIS_VULKAN
	OrbisVkTracePipe(p.ps.key_lo, p.ps.key_hi, static_cast<u8>(p.feedback_loop_flags), m_tfx_textures[0], m_tfx_textures[1],
		m_tfx_textures[2], reinterpret_cast<u64>(pipeline));
#endif

	SetPipeline(pipeline);

	return ApplyTFXState();
}

void GSDeviceVK::InitializeState()
{
	m_current_framebuffer = VK_NULL_HANDLE;
	m_current_render_pass = VK_NULL_HANDLE;

	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
		m_tfx_textures[i] = m_null_texture.get();

	m_utility_texture = m_null_texture.get();

	m_point_sampler = GetSampler(GSHWDrawConfig::SamplerSelector::Point());
	if (m_point_sampler)
		Vulkan::SetObjectName(m_device, m_point_sampler, "Point sampler");
	m_linear_sampler = GetSampler(GSHWDrawConfig::SamplerSelector::Linear());
	if (m_linear_sampler)
		Vulkan::SetObjectName(m_device, m_linear_sampler, "Linear sampler");

	m_tfx_sampler_sel = GSHWDrawConfig::SamplerSelector::Point().key;
	m_tfx_sampler = m_point_sampler;

	InvalidateCachedState();
	SetInitialState(m_current_command_buffer);
}

bool GSDeviceVK::CreatePersistentDescriptorSets()
{
	const VkDevice dev = m_device;
	Vulkan::DescriptorSetUpdateBuilder dsub;

	// Allocate UBO descriptor sets for TFX.
	m_tfx_ubo_descriptor_set = AllocatePersistentDescriptorSet(m_tfx_ubo_ds_layout);
	if (m_tfx_ubo_descriptor_set == VK_NULL_HANDLE)
		return false;
	dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		m_vertex_uniform_stream_buffer.GetBuffer(), 0, sizeof(GSHWDrawConfig::VSConstantBuffer));
	dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		m_fragment_uniform_stream_buffer.GetBuffer(), 0, sizeof(GSHWDrawConfig::PSConstantBuffer));
	if (m_features.vs_expand)
	{
		dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			m_vertex_stream_buffer.GetBuffer(), 0, VERTEX_BUFFER_SIZE);
	}
	if (m_features.aa1)
	{
		dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			m_expand_index_stream_buffer.GetBuffer(), 0, INDEX_BUFFER_SIZE);
	}
	dsub.Update(dev);
	Vulkan::SetObjectName(dev, m_tfx_ubo_descriptor_set, "Persistent TFX UBO set");
	return true;
}

GSDeviceVK::WaitType GSDeviceVK::GetWaitType(bool wait, bool spin)
{
	if (!wait)
		return WaitType::None;
	if (spin)
		return WaitType::Spin;
	else
		return WaitType::Sleep;
}

void GSDeviceVK::ExecuteCommandBuffer(bool wait_for_completion)
{
	EndRenderPass();
	ExecuteCommandBuffer(GetWaitType(wait_for_completion, GSConfig.HWSpinCPUForReadbacks));
}

void GSDeviceVK::ExecuteCommandBuffer(bool wait_for_completion, const char* reason, ...)
{
	std::va_list ap;
	va_start(ap, reason);
	const std::string reason_str(StringUtil::StdStringFromFormatV(reason, ap));
	va_end(ap);

	Console.Warning("VK: Executing command buffer due to '%s'", reason_str.c_str());
	ExecuteCommandBuffer(wait_for_completion);
}

void GSDeviceVK::ExecuteCommandBufferAndRestartRenderPass(bool wait_for_completion, const char* reason)
{
	Console.Warning("VK: Executing command buffer due to '%s'", reason);

	const VkRenderPass render_pass = m_current_render_pass;
	const GSVector4i render_pass_area = m_current_render_pass_area;
	const GSVector4i scissor = m_scissor;
	GSTexture* const current_rt = m_current_render_target;
	GSTexture* const current_ds = m_current_depth_target;
	const FeedbackLoopFlag current_feedback_loop = m_current_framebuffer_feedback_loop;

	EndRenderPass();
	ExecuteCommandBuffer(GetWaitType(wait_for_completion, GSConfig.HWSpinCPUForReadbacks));

	if (render_pass != VK_NULL_HANDLE)
	{
		// rebind framebuffer
		OMSetRenderTargets(current_rt, current_ds, scissor, current_feedback_loop);

		// restart render pass
		BeginRenderPass(GetRenderPassForRestarting(render_pass), render_pass_area);
	}
}

void GSDeviceVK::ExecuteCommandBufferAndRestartPresent(bool wait_for_completion, const char* reason, ...)
{
	std::va_list ap;
	va_start(ap, reason);
	const std::string reason_str(StringUtil::StdStringFromFormatV(reason, ap));
	va_end(ap);

	Console.Warning("VK: Executing command buffer due to '%s'", reason_str.c_str());

	pxAssert(m_is_presenting);
	vkCmdEndRenderPass(GetCurrentCommandBuffer());
	ExecuteCommandBuffer(wait_for_completion);

	GSTextureVK* swap_chain_texture = m_swap_chain->GetCurrentTexture();

	const VkFramebuffer fb = swap_chain_texture->GetFramebuffer(false);
	pxAssert(fb);

	const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,
		GetRenderPass(swap_chain_texture->GetVkFormat(), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD,
			VK_ATTACHMENT_STORE_OP_STORE),
		fb,
		{{0, 0}, {static_cast<u32>(swap_chain_texture->GetWidth()), static_cast<u32>(swap_chain_texture->GetHeight())}},
		0u, nullptr};
	vkCmdBeginRenderPass(GetCurrentCommandBuffer(), &rp, VK_SUBPASS_CONTENTS_INLINE);
}

void GSDeviceVK::ExecuteCommandBufferForReadback()
{
	ExecuteCommandBuffer(true);
	if (m_spinning_supported && GSConfig.HWSpinGPUForReadbacks)
	{
		m_spin_timer = 30;
		m_spin_manager.ReadbackRequested();

		if (!m_optional_extensions.vk_ext_calibrated_timestamps && !m_warned_slow_spin)
		{
			m_warned_slow_spin = true;
			Host::AddKeyedOSDMessage("GSDeviceVK_NoCalibratedTimestamps",
				TRANSLATE_STR("GS", "Spin GPU During Readbacks is enabled, but calibrated timestamps are unavailable.  "
									"This might be really slow."),
				Host::OSD_WARNING_DURATION);
		}
	}
}

void GSDeviceVK::InvalidateCachedState()
{
	m_dirty_flags = ALL_DIRTY_STATE;
	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
		m_tfx_textures[i] = m_null_texture.get();
	m_utility_texture = m_null_texture.get();
	m_current_framebuffer = VK_NULL_HANDLE;
	m_current_render_target = nullptr;
	m_current_depth_target = nullptr;
	m_current_framebuffer_feedback_loop = FeedbackLoopFlag_None;

	m_current_pipeline_layout = PipelineLayout::Undefined;
	m_tfx_texture_descriptor_set = VK_NULL_HANDLE;
	m_tfx_rt_descriptor_set = VK_NULL_HANDLE;
	m_utility_descriptor_set = VK_NULL_HANDLE;
}

void GSDeviceVK::SetIndexBuffer(VkBuffer buffer)
{
	if (m_index_buffer == buffer)
		return;

	m_index_buffer = buffer;
	m_dirty_flags |= DIRTY_FLAG_INDEX_BUFFER;
}

void GSDeviceVK::SetBlendConstants(u8 color)
{
	if (m_blend_constant_color == color)
		return;

	m_blend_constant_color = color;
	m_dirty_flags |= DIRTY_FLAG_BLEND_CONSTANTS;
}

void GSDeviceVK::SetLineWidth(float width)
{
	if (m_current_line_width == width)
		return;

	m_current_line_width = width;
	m_dirty_flags |= DIRTY_FLAG_LINE_WIDTH;
}

void GSDeviceVK::PSSetROVs(GSTexture* rt, GSTexture* ds, bool write_rt, bool write_ds)
{
	GSTextureVK* vkRt = static_cast<GSTextureVK*>(rt);
	GSTextureVK* vkDs = static_cast<GSTextureVK*>(ds);
	GSTextureVK* oldVkRt = m_tfx_textures[TFX_TEXTURE_RT_ROV] != m_null_texture.get() ?
	                       m_tfx_textures[TFX_TEXTURE_RT_ROV] : nullptr;
	GSTextureVK* oldVkDs = m_tfx_textures[TFX_TEXTURE_DEPTH_ROV] != m_null_texture.get() ?
	                       m_tfx_textures[TFX_TEXTURE_DEPTH_ROV] : nullptr;

	if (!(vkRt || vkDs || oldVkRt || oldVkDs))
		return;

	pxAssert(!(vkRt || vkDs) || m_features.rov);

	if (vkRt)
	{
		PSSetShaderResource(TFX_TEXTURE_RT_ROV, vkRt, true, ResourceType::UAV);

		// Unbind conflicting RT texture
		PSSetShaderResource(TFX_TEXTURE_RT, nullptr, false);

		// Unbind conflicting source texture
		if (m_tfx_textures[TFX_TEXTURE_TEXTURE] == vkRt)
		{
			PSSetShaderResource(TFX_TEXTURE_TEXTURE, nullptr, false);
		}

		if (write_rt)
		{
			vkRt->SetState(GSTexture::State::Dirty);
		}
	}
	else
	{
		// Unbind to avoid conflicts with OM targets.
		PSSetShaderResource(TFX_TEXTURE_RT_ROV, nullptr, false);
	}

	if (vkDs)
	{
		PSSetShaderResource(TFX_TEXTURE_DEPTH_ROV, vkDs, true, ResourceType::UAV);

		// Unbind conflicting depth texture
		PSSetShaderResource(TFX_TEXTURE_DEPTH, nullptr, false);

		// Unbind conflicting source texture
		if (m_tfx_textures[TFX_TEXTURE_TEXTURE] == vkDs)
		{
			PSSetShaderResource(TFX_TEXTURE_TEXTURE, nullptr, false);
		}

		if (write_ds)
		{
			vkDs->SetState(GSTexture::State::Dirty);
		}
	}
	else
	{
		// Unbind to avoid conflicts with OM targets.
		PSSetShaderResource(TFX_TEXTURE_DEPTH_ROV, nullptr, false);
	}

	if (IsDeviceNVIDIA() && InRenderPass())
	{
		// Nvidia doesn't like switching ROV targets mid-render pass, doing so causes flickering or missing geometry.
		// End the render pass to avoid such issues.
		if (vkRt != oldVkRt || vkDs != oldVkDs)
		{
			GL_INS("VK: Ending render pass due to UAV switch");
			EndRenderPass();
		}
	}

	if (GSConfig.HWROVBarriersVK)
	{
		// This is to fix issues with some systems that seem to require barriers even with FSI.
		// Adds a barriers to the UAVs before every draw. Can result in a large performance hit.
		if ((vkRt || vkDs) && InRenderPass())
		{
			GL_INS("Ending render pass due to UAV barrier");
			EndRenderPass();
		}
		for (GSTextureVK* tex : std::array{ vkRt, vkDs })
		{
			if (tex)
			{
				if (InRenderPass())
				{
					GL_INS("Ending render pass due to UAV barrier");
					EndRenderPass();
				}
				// The subresources version does the barrier even when the before/after layout
				// are the same since we want a memory barrier here, not just a layout change.
				tex->TransitionSubresourcesToLayout(GetCurrentCommandBuffer(), 0, 1, tex->GetLayout(), tex->GetLayout());
			}
		}
	}
}

void GSDeviceVK::PSSetShaderResource(int i, GSTexture* sr, bool check_state, ResourceType type)
{
	GSTextureVK* vkTex = static_cast<GSTextureVK*>(sr);
	bool update_layout = false;
	if (vkTex)
	{
		if (check_state)
		{
			const GSTextureVK::Layout layout = GetResourceLayout(type);

			vkTex->CommitClear();

			if (vkTex->GetLayout() != layout)
			{
				update_layout = true;
				if (InRenderPass())
				{
					GL_INS("Ending render pass due to resource transition");
					EndRenderPass();
				}
				vkTex->TransitionToLayout(layout);
			}
		}
		vkTex->SetUseFenceCounter(GetCurrentFenceCounter());
	}
	else
	{
		vkTex = m_null_texture.get();
	}

	if (m_tfx_textures[i] == vkTex && !update_layout)
		return;

	m_tfx_textures[i] = vkTex;
	m_dirty_flags |= (DIRTY_FLAG_TFX_TEXTURE_0 << i);
}

void GSDeviceVK::PSSetSampler(GSHWDrawConfig::SamplerSelector sel)
{
	if (m_tfx_sampler_sel == sel.key)
		return;

	m_tfx_sampler_sel = sel.key;
	m_tfx_sampler = GetSampler(sel);
	m_dirty_flags |= DIRTY_FLAG_TFX_TEXTURE_0;
}

void GSDeviceVK::SetUtilityTexture(GSTexture* tex, VkSampler sampler)
{
	GSTextureVK* vkTex = static_cast<GSTextureVK*>(tex);
	if (vkTex)
	{
		vkTex->CommitClear();
		vkTex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		vkTex->SetUseFenceCounter(GetCurrentFenceCounter());
	}
	else
	{
		vkTex = m_null_texture.get();
	}

	if (m_utility_texture == vkTex && m_utility_sampler == sampler)
		return;

	m_utility_texture = vkTex;
	m_utility_sampler = sampler;
	m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
}

void GSDeviceVK::SetUtilityPushConstants(const void* data, u32 size)
{
	vkCmdPushConstants(GetCurrentCommandBuffer(), m_utility_pipeline_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
}

void GSDeviceVK::UnbindTexture(GSTextureVK* tex)
{
	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
	{
		if (m_tfx_textures[i] == tex)
		{
			m_tfx_textures[i] = m_null_texture.get();
			m_dirty_flags |= (DIRTY_FLAG_TFX_TEXTURE_0 << i);
		}
	}
	if (m_utility_texture == tex)
	{
		m_utility_texture = m_null_texture.get();
		m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
	}
	if (m_current_render_target == tex || m_current_depth_target == tex)
	{
		EndRenderPass();
		m_current_framebuffer = VK_NULL_HANDLE;
		m_current_render_target = nullptr;
		m_current_depth_target = nullptr;
	}
}

bool GSDeviceVK::InRenderPass()
{
	return m_current_render_pass != VK_NULL_HANDLE;
}

void GSDeviceVK::BeginRenderPass(VkRenderPass rp, const GSVector4i& rect)
{
	if (m_current_render_pass != VK_NULL_HANDLE)
		EndRenderPass();

	m_current_render_pass = rp;
	m_current_render_pass_area = rect;

	const VkRenderPassBeginInfo begin_info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, m_current_render_pass,
		m_current_framebuffer, {{rect.x, rect.y}, {static_cast<u32>(rect.width()), static_cast<u32>(rect.height())}}, 0,
		nullptr};

	m_command_buffer_render_passes++;
	vkCmdBeginRenderPass(GetCurrentCommandBuffer(), &begin_info, VK_SUBPASS_CONTENTS_INLINE);
#ifdef ORBIS_VULKAN
	OrbisGpStampPass(false); // vk-285-94
#endif
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, const VkClearValue* cv, u32 cv_count)
{
	if (m_current_render_pass != VK_NULL_HANDLE)
		EndRenderPass();

	m_current_render_pass = rp;
	m_current_render_pass_area = rect;

	const VkRenderPassBeginInfo begin_info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, m_current_render_pass,
		m_current_framebuffer, {{rect.x, rect.y}, {static_cast<u32>(rect.width()), static_cast<u32>(rect.height())}},
		cv_count, cv};

	vkCmdBeginRenderPass(GetCurrentCommandBuffer(), &begin_info, VK_SUBPASS_CONTENTS_INLINE);
#ifdef ORBIS_VULKAN
	OrbisGpStampPass(true); // vk-285-94
#endif
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, u32 clear_color)
{
	alignas(16) VkClearValue cv;
	GSVector4::store<true>((void*)cv.color.float32, GSVector4::unorm8(clear_color));
	BeginClearRenderPass(rp, rect, &cv, 1);
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, float depth, u8 stencil)
{
	VkClearValue cv;
	cv.depthStencil.depth = depth;
	cv.depthStencil.stencil = stencil;
	BeginClearRenderPass(rp, rect, &cv, 1);
}

void GSDeviceVK::EndRenderPass()
{
	if (m_current_render_pass == VK_NULL_HANDLE)
		return;

	m_current_render_pass = VK_NULL_HANDLE;
	g_perfmon.Put(GSPerfMon::RenderPasses, 1);

	vkCmdEndRenderPass(GetCurrentCommandBuffer());
}

void GSDeviceVK::SetViewport(const VkViewport& viewport)
{
	if (std::memcmp(&viewport, &m_viewport, sizeof(VkViewport)) == 0)
		return;

	std::memcpy(&m_viewport, &viewport, sizeof(VkViewport));
	m_dirty_flags |= DIRTY_FLAG_VIEWPORT;
}

void GSDeviceVK::SetScissor(const GSVector4i& scissor)
{
	if (m_scissor.eq(scissor))
		return;

	m_scissor = scissor;
	m_dirty_flags |= DIRTY_FLAG_SCISSOR;
}

void GSDeviceVK::SetPipeline(VkPipeline pipeline)
{
	if (m_current_pipeline == pipeline)
		return;

	m_current_pipeline = pipeline;
	m_dirty_flags |= DIRTY_FLAG_PIPELINE;
}

void GSDeviceVK::SetInitialState(VkCommandBuffer cmdbuf)
{
	VkBuffer buffer = *m_vertex_stream_buffer.GetBufferPtr();
	if (buffer != VK_NULL_HANDLE)
	{
		constexpr VkDeviceSize buffer_offset = 0;
		vkCmdBindVertexBuffers(cmdbuf, 0, 1, &buffer, &buffer_offset);
	}
}

__ri void GSDeviceVK::ApplyBaseState(u32 flags, VkCommandBuffer cmdbuf)
{
	if (flags & DIRTY_FLAG_INDEX_BUFFER)
		vkCmdBindIndexBuffer(cmdbuf, m_index_buffer, 0, VK_INDEX_TYPE_UINT16);

	if (flags & DIRTY_FLAG_PIPELINE)
		vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_current_pipeline);

	if (flags & DIRTY_FLAG_VIEWPORT)
		vkCmdSetViewport(cmdbuf, 0, 1, &m_viewport);

	if (flags & DIRTY_FLAG_SCISSOR)
	{
		const VkRect2D vscissor{
			{m_scissor.x, m_scissor.y}, {static_cast<u32>(m_scissor.width()), static_cast<u32>(m_scissor.height())}};
		vkCmdSetScissor(cmdbuf, 0, 1, &vscissor);
	}

	if (flags & DIRTY_FLAG_BLEND_CONSTANTS)
	{
		const GSVector4 col(static_cast<float>(m_blend_constant_color) / 128.0f);
		vkCmdSetBlendConstants(cmdbuf, col.v);
	}

	if (flags & DIRTY_FLAG_LINE_WIDTH)
		vkCmdSetLineWidth(cmdbuf, m_current_line_width);
}

bool GSDeviceVK::ApplyTFXState(bool already_execed)
{
	if (m_current_pipeline_layout == PipelineLayout::TFX && m_dirty_flags == 0)
		return true;

	const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
	u32 flags = m_dirty_flags;
	m_dirty_flags &= ~(DIRTY_TFX_STATE | DIRTY_CONSTANT_BUFFER_STATE | DIRTY_FLAG_TFX_UBO);

	// do cbuffer first, because it's the most likely to cause an exec
	if (flags & DIRTY_FLAG_VS_CONSTANT_BUFFER)
	{
		if (!m_vertex_uniform_stream_buffer.ReserveMemory(
			sizeof(m_vs_cb_cache), static_cast<u32>(m_device_properties.limits.minUniformBufferOffsetAlignment)))
		{
			if (already_execed)
			{
				Console.Error("VK: Failed to reserve vertex uniform space");
				return false;
			}

			ExecuteCommandBufferAndRestartRenderPass(false, "Ran out of vertex uniform space");
			return ApplyTFXState(true);
		}

#ifdef ORBIS_VULKAN
		OrbisUploadUniform(m_vertex_uniform_stream_buffer.GetCurrentHostPointer(), m_vs_cb_cache); // vk-285-98, 102, 106
#else
		std::memcpy(m_vertex_uniform_stream_buffer.GetCurrentHostPointer(), &m_vs_cb_cache, sizeof(m_vs_cb_cache));
#endif
		m_tfx_dynamic_offsets[0] = m_vertex_uniform_stream_buffer.GetCurrentOffset();
		m_vertex_uniform_stream_buffer.CommitMemory(sizeof(m_vs_cb_cache));
		flags |= DIRTY_FLAG_TFX_UBO;
	}

	if (flags & DIRTY_FLAG_PS_CONSTANT_BUFFER)
	{
		if (!m_fragment_uniform_stream_buffer.ReserveMemory(
				sizeof(m_ps_cb_cache), static_cast<u32>(m_device_properties.limits.minUniformBufferOffsetAlignment)))
		{
			if (already_execed)
			{
				Console.Error("VK: Failed to reserve pixel uniform space");
				return false;
			}

			ExecuteCommandBufferAndRestartRenderPass(false, "Ran out of pixel uniform space");
			return ApplyTFXState(true);
		}

#ifdef ORBIS_VULKAN
		OrbisUploadUniform(m_fragment_uniform_stream_buffer.GetCurrentHostPointer(), m_ps_cb_cache); // vk-285-98, 102, 106
#else
		std::memcpy(m_fragment_uniform_stream_buffer.GetCurrentHostPointer(), &m_ps_cb_cache, sizeof(m_ps_cb_cache));
#endif
		m_tfx_dynamic_offsets[1] = m_fragment_uniform_stream_buffer.GetCurrentOffset();
		m_fragment_uniform_stream_buffer.CommitMemory(sizeof(m_ps_cb_cache));
		flags |= DIRTY_FLAG_TFX_UBO;
	}

	Vulkan::DescriptorSetUpdateBuilder dsub;
	if (m_current_pipeline_layout != PipelineLayout::TFX)
	{
		m_current_pipeline_layout = PipelineLayout::TFX;
		flags |= DIRTY_FLAG_TFX_UBO | DIRTY_FLAG_TFX_TEXTURES | DIRTY_FLAG_VS_PUSH_CONSTANTS;

		// Clear out the RT/DS binding if feedback loop isn't on, because it'll be in the wrong state and make
		// the validation layer cranky. Not a big deal since we need to write it anyway.
		std::array<TFX_TEXTURES, 2> texture_types = { TFX_TEXTURE_RT, TFX_TEXTURE_DEPTH };
		for (u32 texture_type : texture_types)
		{
			const GSTextureVK::Layout tex_layout = m_tfx_textures[texture_type]->GetLayout();
			if (tex_layout != GSTextureVK::Layout::FeedbackLoop && tex_layout != GSTextureVK::Layout::ShaderReadOnly)
				m_tfx_textures[texture_type] = m_null_texture.get();
		}
	}

	if (flags & DIRTY_FLAG_TFX_UBO)
	{
		// Still need to bind the UBO descriptor set.
		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tfx_pipeline_layout, 0, 1,
			&m_tfx_ubo_descriptor_set, NUM_TFX_DYNAMIC_OFFSETS, m_tfx_dynamic_offsets.data());
	}

	if (m_features.vs_expand && (flags & DIRTY_FLAG_VS_PUSH_CONSTANTS))
		SetVSPushConstants(m_vs_pc_cache.base_vertex, m_vs_pc_cache.base_index, true);

	if (flags & DIRTY_FLAG_TFX_TEXTURES)
	{
		if (flags & DIRTY_FLAG_TFX_TEXTURE_TEX)
		{
			dsub.AddCombinedImageSamplerDescriptorWrite(VK_NULL_HANDLE, TFX_TEXTURE_TEXTURE,
				m_tfx_textures[TFX_TEXTURE_TEXTURE]->GetView(), m_tfx_sampler,
				m_tfx_textures[TFX_TEXTURE_TEXTURE]->GetVkLayout());
		}
		if (flags & DIRTY_FLAG_TFX_TEXTURE_PALETTE)
		{
			dsub.AddImageDescriptorWrite(VK_NULL_HANDLE, TFX_TEXTURE_PALETTE,
				m_tfx_textures[TFX_TEXTURE_PALETTE]->GetView(), m_tfx_textures[TFX_TEXTURE_PALETTE]->GetVkLayout());
		}
		if (flags & DIRTY_FLAG_TFX_TEXTURE_RT)
		{
			if (m_features.texture_barrier && !UseFeedbackLoopLayout())
			{
				dsub.AddInputAttachmentDescriptorWrite(
					VK_NULL_HANDLE, TFX_TEXTURE_RT, m_tfx_textures[TFX_TEXTURE_RT]->GetView(), VK_IMAGE_LAYOUT_GENERAL);
			}
			else
			{
				dsub.AddImageDescriptorWrite(VK_NULL_HANDLE, TFX_TEXTURE_RT, m_tfx_textures[TFX_TEXTURE_RT]->GetView(),
					m_tfx_textures[TFX_TEXTURE_RT]->GetVkLayout());
			}
		}
		if (flags & DIRTY_FLAG_TFX_TEXTURE_PRIMID)
		{
			dsub.AddImageDescriptorWrite(VK_NULL_HANDLE, TFX_TEXTURE_PRIMID,
				m_tfx_textures[TFX_TEXTURE_PRIMID]->GetView(), m_tfx_textures[TFX_TEXTURE_PRIMID]->GetVkLayout());
		}
		if (flags & DIRTY_FLAG_TFX_TEXTURE_DEPTH)
		{
			if (m_features.texture_barrier && !UseFeedbackLoopLayout())
			{
				dsub.AddInputAttachmentDescriptorWrite(
					VK_NULL_HANDLE, TFX_TEXTURE_DEPTH, m_tfx_textures[TFX_TEXTURE_DEPTH]->GetView(), VK_IMAGE_LAYOUT_GENERAL);
			}
			else
			{
				dsub.AddImageDescriptorWrite(VK_NULL_HANDLE, TFX_TEXTURE_DEPTH, m_tfx_textures[TFX_TEXTURE_DEPTH]->GetView(),
					m_tfx_textures[TFX_TEXTURE_DEPTH]->GetVkLayout());
			}
		}
		if (flags & DIRTY_FLAG_TFX_TEXTURE_RT_ROV)
		{
			dsub.AddImageDescriptorWrite(VK_NULL_HANDLE, TFX_TEXTURE_RT_ROV, m_tfx_textures[TFX_TEXTURE_RT_ROV]->GetView(),
				m_tfx_textures[TFX_TEXTURE_RT_ROV]->GetVkLayout(), true);
		}
		if (flags & DIRTY_FLAG_TFX_TEXTURE_DEPTH_ROV)
		{
			dsub.AddImageDescriptorWrite(VK_NULL_HANDLE, TFX_TEXTURE_DEPTH_ROV, m_tfx_textures[TFX_TEXTURE_DEPTH_ROV]->GetView(),
				m_tfx_textures[TFX_TEXTURE_DEPTH_ROV]->GetVkLayout(), true);
		}

		dsub.PushUpdate(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tfx_pipeline_layout, TFX_DESCRIPTOR_SET_TEXTURES);
	}

	ApplyBaseState(flags, cmdbuf);
	return true;
}

bool GSDeviceVK::ApplyUtilityState(bool already_execed)
{
	if (m_current_pipeline_layout == PipelineLayout::Utility && m_dirty_flags == 0)
		return true;

	const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
	u32 flags = m_dirty_flags;
	m_dirty_flags &= ~DIRTY_UTILITY_STATE;

	if (m_current_pipeline_layout != PipelineLayout::Utility || flags & DIRTY_FLAG_UTILITY_TEXTURE)
	{
		m_current_pipeline_layout = PipelineLayout::Utility;

		Vulkan::DescriptorSetUpdateBuilder dsub;
		dsub.AddCombinedImageSamplerDescriptorWrite(
			VK_NULL_HANDLE, 0, m_utility_texture->GetView(), m_utility_sampler, m_utility_texture->GetVkLayout());
		dsub.PushUpdate(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_utility_pipeline_layout, 0, false);
	}


	ApplyBaseState(flags, cmdbuf);
	return true;
}

void GSDeviceVK::SetVSConstantBuffer(const GSHWDrawConfig::VSConstantBuffer& cb)
{
	if (m_vs_cb_cache.Update(cb))
		m_dirty_flags |= DIRTY_FLAG_VS_CONSTANT_BUFFER;
}

void GSDeviceVK::SetPSConstantBuffer(const GSHWDrawConfig::PSConstantBuffer& cb)
{
	if (m_ps_cb_cache.Update(cb))
		m_dirty_flags |= DIRTY_FLAG_PS_CONSTANT_BUFFER;
}

void GSDeviceVK::SetVSPushConstants(u32 base_vertex, u32 base_index, bool force_update)
{
	GSHWDrawConfig::VSPushConstants pc;
	pc.base_vertex = base_vertex;
	pc.base_index = base_index;

	if (m_vs_pc_cache.Update(pc) || force_update)
	{
		vkCmdPushConstants(GetCurrentCommandBuffer(), m_tfx_pipeline_layout,
			VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
	}
}

void GSDeviceVK::SetupDATE(GSTexture* rt, GSTexture* ds, SetDATM datm, const GSVector4i& bbox)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	GL_PUSH("SetupDATE {%d,%d} %dx%d", bbox.left, bbox.top, bbox.width(), bbox.height());

	const GSVector2i size(ds->GetSize());
	const GSVector4 src = GSVector4(bbox) / GSVector4(size).xyxy();
	const GSVector4 dst = src * 2.0f - 1.0f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
	};

	// sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii shadows, rumble roses shadows, persona4 shadows
	EndRenderPass();
	SetUtilityTexture(rt, m_point_sampler);
	OMSetRenderTargets(nullptr, ds, bbox);
	IASetVertexBuffer(vertices, sizeof(vertices[0]), 4);
	SetPipeline(GetConvertPipeline(SetDATMShader(datm)));
	BeginClearRenderPass(m_date_setup_render_pass, bbox, 0.0f, 0);
	if (ApplyUtilityState())
		DrawPrimitive();

	EndRenderPass();
}

GSTextureVK* GSDeviceVK::SetupPrimitiveTrackingDATE(GSHWDrawConfig& config)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	// How this is done:
	// - can't put a barrier for the image in the middle of the normal render pass, so that's out
	// - so, instead of just filling the int texture with INT_MAX, we sample the RT and use -1 for failing values
	// - then, instead of sampling the RT with DATE=1/2, we just do a min() without it, the -1 gets preserved
	// - then, the DATE=3 draw is done as normal
	GL_INS("Setup DATE Primitive ID Image for {%d,%d}-{%d,%d}", config.drawarea.left, config.drawarea.top,
		config.drawarea.right, config.drawarea.bottom);

	const GSVector2i rtsize(config.rt->GetSize());
	GSTextureVK* image =
		static_cast<GSTextureVK*>(CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::PrimID, false));
	if (!image)
		return nullptr;

	EndRenderPass();

	// setup the fill quad to prefill with existing alpha values
	SetUtilityTexture(config.rt, m_point_sampler);
	OMSetRenderTargets(image, config.ds, config.drawarea);

	// if the depth target has been cleared, we need to preserve that clear
	const VkAttachmentLoadOp ds_load_op = GetLoadOpForTexture(static_cast<GSTextureVK*>(config.ds));
	const u32 ds = (config.ds ? 1 : 0);

	if (ds_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
	{
		VkClearValue cv[2] = {};
		cv[1].depthStencil.depth = static_cast<GSTextureVK*>(config.ds)->GetClearDepth();
		cv[1].depthStencil.stencil = 1;
		BeginClearRenderPass(m_primid_image_setup_render_passes[ds][1], GSVector4i::loadh(rtsize), cv, 2);
	}
	else
	{
		BeginRenderPass(m_primid_image_setup_render_passes[ds][0], config.drawarea);
	}

	// draw the quad to prefill the image
	const GSVector4 src = GSVector4(config.drawarea) / GSVector4(rtsize).xyxy();
	const GSVector4 dst = src * 2.0f - 1.0f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
	};
	const VkPipeline pipeline = m_primid_image_setup_pipelines[ds][static_cast<u8>(config.datm)];
	SetPipeline(pipeline);
	IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));
	if (ApplyUtilityState())
		DrawPrimitive();

	// image is now filled with either -1 or INT_MAX, so now we can do the prepass
	UploadHWDrawVerticesAndIndices(config);

	// primid texture will get re-bound, so clear it since we're using push descriptors
	PSSetShaderResource(3, m_null_texture.get(), false);

	// cut down the configuration for the prepass, we don't need blending or any feedback loop
	PipelineSelector& pipe = m_pipeline_selector;
	UpdateHWPipelineSelector(config, pipe);
	pipe.dss.zwe = false;
	pipe.cms.wrgba = 0;
	pipe.bs = {};
	pipe.feedback_loop_flags = FeedbackLoopFlag_None;
	pipe.rt = true;
	pipe.ps.blend_a = pipe.ps.blend_b = pipe.ps.blend_c = pipe.ps.blend_d = false;
	pipe.ps.no_color = false;
	pipe.ps.no_color1 = true;
	if (BindDrawPipeline(pipe))
		Draw(config);

	// image is initialized/prepass is done, so finish up and get ready to do the "real" draw
	EndRenderPass();

	// .. by setting it to DATE=3
	config.ps.date = 3;
	config.alpha_second_pass.ps.date = 3;

	// and bind the image to the primitive sampler
	image->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	PSSetShaderResource(3, image, false);
	return image;
}

void GSDeviceVK::RenderHW(GSHWDrawConfig& config)
{
#ifdef ORBIS_VULKAN
	// vk-285-113 (AI-assisted): the PS5 driver copies every command buffer of one vkQueueSubmit into a 2 MiB stream and refuses more
	// than 523,503 words (ps5vk_queue.c:1528; a draw is ~50-90 words, an in-stream feedback barrier 17). SendHWDraw makes one barrier +
	// one draw per drawlist group of a full-barrier draw, so one frame can hold thousands of driver draws; vk-285-108..112's Sims,
	// Stuntman and Area 51 died on it ("a submission of more than 523503 words", then VK_ERROR_DEVICE_LOST). Submit early instead, here at
	// a draw boundary before this draw has touched any state (the uniform-space flush in ApplyTFXState is the precedent).
	// 3500 draws x 90 words (the driver's per-draw maximum) = 315k words, leaving room for uploads, clears and the present pass.
	{
		u32 groups = 1; // a full-barrier draw is one barrier + one driver draw per drawlist group (SendHWDraw)
		if (config.require_full_barrier && config.drawlist && !config.drawlist->empty())
			groups = static_cast<u32>(config.drawlist->size());
		const u32 draws = groups + (config.alpha_second_pass.enable ? groups : 0) + (config.blend_multi_pass.enable ? 1 : 0);
		// vk-285-115: ps5vk only. RADV has no such limit (it splits a long stream itself), so an early submit would only cost it a
		// render pass restart.
		if (m_orbis_submit_draws != 0 && m_orbis_submit_draws + draws > ORBIS_SUBMIT_DRAW_BUDGET && !OrbisDriverIsRADV())
		{
			static u32 s_budget_flushes = 0;
			if (++s_budget_flushes <= 3 || (s_budget_flushes % 500) == 0)
				Console.Warning("VK: %u draws in this submission and %u more coming, submitting early (%u times)",
					m_orbis_submit_draws, draws, s_budget_flushes);
			ExecuteCommandBuffer(false);
		}
	}
	OrbisVkTraceDraw(config);
#endif
	const GSVector2i rtsize(config.rt ? config.rt->GetSize() : config.ds->GetSize());
	GSTextureVK* draw_rt = config.ps.HasColorROV() ? nullptr : static_cast<GSTextureVK*>(config.rt);
	GSTextureVK* draw_ds = config.ps.HasDepthROV() ? nullptr : static_cast<GSTextureVK*>(config.ds);
	GSTextureVK* draw_rt_rov = config.ps.HasColorROV() ? static_cast<GSTextureVK*>(config.rt) : nullptr;
	GSTextureVK* draw_ds_rov = config.ps.HasDepthROV() ? static_cast<GSTextureVK*>(config.ds) : nullptr;
	GSTextureVK* draw_rt_clone = nullptr;
	GSTextureVK* colclip_rt = static_cast<GSTextureVK*>(g_gs_device->GetColorClipTexture());

	// stream buffer in first, in case we need to exec
	SetVSConstantBuffer(config.cb_vs);
	SetPSConstantBuffer(config.cb_ps);

	// bind textures before checking the render pass, in case we need to transition them
	if (config.tex)
	{
		PSSetShaderResource(TFX_TEXTURE_TEXTURE, config.tex, config.tex != config.rt && config.tex != config.ds);
		PSSetSampler(config.sampler);
	}
	if (config.pal)
		PSSetShaderResource(TFX_TEXTURE_PALETTE, config.pal, true);

	if (config.blend.constant_enable)
		SetBlendConstants(config.blend.constant);

	if (config.topology == GSHWDrawConfig::Topology::Line)
		SetLineWidth(config.line_expand ? config.cb_ps.ScaleFactor.z : 1.0f);

	// Primitive ID tracking DATE setup.
	// Needs to be done before
	GSTextureVK* date_image = nullptr;
	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		// If we have a colclip in progress, we need to use the colclip texture, but we can't check this later as there's a chicken/egg problem with the pipe setup.
		GSTexture* backup_rt = config.rt;

		if (colclip_rt)
			config.rt = colclip_rt;

		date_image = SetupPrimitiveTrackingDATE(config);
		if (!date_image)
		{
			Console.Warning("VK: Failed to allocate DATE image, aborting draw.");
			return;
		}

		config.rt = backup_rt;
	}

	// figure out the pipeline
	PipelineSelector& pipe = m_pipeline_selector;
	UpdateHWPipelineSelector(config, pipe);

	// now blit the colclip texture back to the original target
	if (colclip_rt)
	{
		if (config.colclip_mode == GSHWDrawConfig::ColClipMode::EarlyResolve)
		{
			GL_PUSH("Blit ColorClip back to RT");

			EndRenderPass();
			colclip_rt->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);

			draw_rt = static_cast<GSTextureVK*>(config.rt);
			OMSetRenderTargets(draw_rt, draw_ds, GSVector4i::loadh(rtsize), static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));

			// if this target was cleared and never drawn to, perform the clear as part of the resolve here.
			if (draw_rt->GetState() == GSTexture::State::Cleared)
			{
				alignas(16) VkClearValue cvs[2];
				u32 cv_count = 0;
				GSVector4::store<true>(&cvs[cv_count++].color, draw_rt->GetClearForFormat());
				if (draw_ds)
					cvs[cv_count++].depthStencil = {draw_ds->GetClearDepth(), 1};

				BeginClearRenderPass(GetTFXRenderPass(true, pipe.ds, false, false, pipe.IsRTFeedbackLoop(),
										 pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_CLEAR,
										 pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
					draw_rt->GetRect(), cvs, cv_count);
				draw_rt->SetState(GSTexture::State::Dirty);
			}
			else
			{
				BeginRenderPass(GetTFXRenderPass(true, pipe.ds, false, false, pipe.IsRTFeedbackLoop(),
									pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_LOAD,
									pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
					draw_rt->GetRect());
			}

			const GSVector4 drawareaf = GSVector4(config.colclip_update_area);
			const GSVector4 sRect(drawareaf / GSVector4(rtsize).xyxy());
			SetPipeline(m_colclip_finish_pipelines[pipe.ds][pipe.IsRTFeedbackLoop()]);
			SetUtilityTexture(colclip_rt, m_point_sampler);
			DrawStretchRect(sRect, drawareaf, rtsize);

			Recycle(colclip_rt);
			g_gs_device->SetColorClipTexture(nullptr);

			colclip_rt = nullptr;
			draw_rt = config.ps.HasColorROV() ? nullptr : static_cast<GSTextureVK*>(config.rt);
		}
		else
		{
			pipe.ps.colclip_hw = 1;
			draw_rt = colclip_rt;
		}
	}

	// Destination Alpha Setup
	const bool need_barrier = config.require_one_barrier || (config.require_full_barrier && m_features.texture_barrier);
	switch (config.destination_alpha)
	{
		case GSHWDrawConfig::DestinationAlphaMode::Off: // No setup
		case GSHWDrawConfig::DestinationAlphaMode::Full: // No setup
		case GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking: // Setup is done below
			break;
		case GSHWDrawConfig::DestinationAlphaMode::StencilOne: // setup is done below
		{
			// we only need to do the setup here if we don't have barriers, in which case do full DATE.
			if (!need_barrier)
			{
				SetupDATE(draw_rt, config.ds, config.datm, config.drawarea);
				config.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Stencil;
			}
		}
		break;

		case GSHWDrawConfig::DestinationAlphaMode::Stencil:
			SetupDATE(draw_rt, config.ds, config.datm, config.drawarea);
			break;
	}

	// Switch to colclip target for colclip hw rendering
	if (pipe.ps.colclip_hw)
	{
		if (!colclip_rt)
		{
			config.colclip_update_area = config.drawarea;
			EndRenderPass();
			colclip_rt = static_cast<GSTextureVK*>(CreateFeedbackTarget(rtsize.x, rtsize.y, GSTexture::Format::ColorClip, false));
			if (!colclip_rt)
			{
				Console.Warning("VK: Failed to allocate ColorClip render target, aborting draw.");

				if (date_image)
					Recycle(date_image);

				GL_POP();
				return;
			}
			g_gs_device->SetColorClipTexture(static_cast<GSTexture*>(colclip_rt));

			// propagate clear value through if the colclip render is the first
			if (draw_rt->GetState() == GSTexture::State::Cleared)
			{
				colclip_rt->SetState(GSTexture::State::Cleared);
				colclip_rt->SetClearColor(draw_rt->GetClearColor());

				// If depth is cleared, we need to commit it, because we're only going to draw to the active part of the FB.
				if (draw_ds && draw_ds->GetState() == GSTexture::State::Cleared && !config.drawarea.eq(GSVector4i::loadh(rtsize)))
					draw_ds->CommitClear(m_current_command_buffer);
			}
			else if (draw_rt->GetState() == GSTexture::State::Dirty)
			{
				GL_PUSH_("ColorClip Render Target Setup");
				draw_rt->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
			}

			// we're not drawing to the RT, so we can use it as a source
			if (config.require_one_barrier && !m_features.texture_barrier)
				PSSetShaderResource(2, draw_rt, true);
		}
		draw_rt = colclip_rt;
	}

	// clear texture binding when it's bound to RT or DS.
	if (!config.tex && ((config.rt && static_cast<GSTextureVK*>(config.rt) == m_tfx_textures[0]) ||
						   (config.ds && static_cast<GSTextureVK*>(config.ds) == m_tfx_textures[0])))
	{
		PSSetShaderResource(0, nullptr, false);
	}

	// render pass restart optimizations
	if (colclip_rt && (config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly))
	{
		// colclip hw requires blitting.
		EndRenderPass();
	}
	else if (InRenderPass() &&
		((draw_rt && m_current_render_target == draw_rt) ||
		(draw_ds && m_current_depth_target == draw_ds)))
	{
		// avoid restarting the render pass just to switch from rt+depth to rt and vice versa
		// keep the depth even if doing colclip hw draws, because the next draw will probably re-enable depth.
		if (!(draw_rt || draw_rt_rov) && m_current_render_target && config.tex != m_current_render_target &&
			draw_ds && m_current_render_target->GetSize() == draw_ds->GetSize())
		{
			draw_rt = m_current_render_target;
			m_pipeline_selector.rt = true;
		}
		else if (!(draw_ds || draw_ds_rov) && m_current_depth_target && config.tex != m_current_depth_target &&
			draw_rt && m_current_depth_target->GetSize() == draw_rt->GetSize())
		{
			draw_ds = m_current_depth_target;
			m_pipeline_selector.ds = true;
		}

		// Prefer keeping feedback loop enabled, that way we're not constantly restarting render passes
		if (draw_rt)
			pipe.feedback_loop_flags |= m_current_framebuffer_feedback_loop & FeedbackLoopFlag_ReadAndWriteRT;
		if (draw_ds)
			pipe.feedback_loop_flags |= (m_current_framebuffer_feedback_loop &
				(FeedbackLoopFlag_ReadAndWriteDepth | FeedbackLoopFlag_ReadDepth));
	}

	if (draw_rt && ((config.require_one_barrier && (config.IsFeedbackLoopRT(config.ps) || config.IsFeedbackLoopRT(config.alpha_second_pass.ps)))) && !m_features.texture_barrier)
	{
		// Requires a copy of the RT.
		draw_rt_clone = static_cast<GSTextureVK*>(CreateTexture(rtsize.x, rtsize.y, 1, draw_rt->GetFormat(), true));
		if (draw_rt_clone)
		{
			GL_PUSH("VK: Copy RT to temp texture {%d,%d %dx%d}",
				config.drawarea.left, config.drawarea.top,
				config.drawarea.width(), config.drawarea.height());
			EndRenderPass();
			if (config.tex_hazard)
			{
				const GSVector4i union_rect = config.drawarea.runion(config.samplearea);
				const u32 size_union = union_rect.width() * union_rect.height();
				const u32 size_indiv = config.drawarea.width() * config.drawarea.height() +
				                       config.samplearea.width() * config.samplearea.height();

				// Do an individual copy if the union is larger than the sum of individual areas.
				if (size_union > size_indiv)
				{
					const GSVector4i snapped_drawarea = ProcessCopyArea(GSVector4i(0, 0, rtsize.x, rtsize.y), config.drawarea);
					const GSVector4i snapped_samplearea = ProcessCopyArea(GSVector4i(0, 0, rtsize.x, rtsize.y), config.samplearea);
					CopyRect(draw_rt, draw_rt_clone, snapped_drawarea, snapped_drawarea.left, snapped_drawarea.top);
					CopyRect(draw_rt, draw_rt_clone, snapped_samplearea, snapped_samplearea.left, snapped_samplearea.top);
				}
				else
				{
					CopyRect(draw_rt, draw_rt_clone, union_rect, union_rect.left, union_rect.top);
				}
			}
			else
			{
				CopyRect(draw_rt, draw_rt_clone, config.drawarea, config.drawarea.left, config.drawarea.top);
			}

			if (config.require_one_barrier)
				PSSetShaderResource(2, draw_rt_clone, true);
			if (config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_RT)
				PSSetShaderResource(0, draw_rt_clone, true);
		}
		else
			Console.Warning("VK: Failed to allocate temp texture for RT copy.");
	}

	if (pipe.IsRTFeedbackLoop() && draw_rt)
	{
		pxAssertMsg(m_features.texture_barrier, "Texture barriers enabled");
		PSSetShaderResource(TFX_TEXTURE_RT, draw_rt, false);

		if (m_tfx_textures[TFX_TEXTURE_RT_ROV] == draw_rt)
		{
			// Unbind to avoid conflicts.
			PSSetShaderResource(TFX_TEXTURE_RT_ROV, nullptr, false);
			m_dirty_flags |= static_cast<u32>(DIRTY_FLAG_TFX_TEXTURE_RT_ROV);
		}
	}
	else if (!draw_rt_clone)
	{
		// Unbind to avoid conflicts with framebuffer
		PSSetShaderResource(TFX_TEXTURE_RT, nullptr, false);
	}
	
	if (pipe.IsDepthFeedbackLoop() && draw_ds)
	{
		pxAssertMsg(m_features.texture_barrier, "Texture barriers enabled");
		PSSetShaderResource(TFX_TEXTURE_DEPTH, draw_ds, false);

		if (m_tfx_textures[TFX_TEXTURE_DEPTH_ROV] == draw_ds)
		{
			// Unbind to avoid conflicts.
			PSSetShaderResource(TFX_TEXTURE_DEPTH_ROV, nullptr, false);
			m_dirty_flags |= static_cast<u32>(DIRTY_FLAG_TFX_TEXTURE_DEPTH_ROV);
		}
	}
	else
	{
		// Unbind to avoid conflicts with framebuffer
		PSSetShaderResource(TFX_TEXTURE_DEPTH, nullptr, false);
	}
	
	PSSetROVs(draw_rt_rov, draw_ds_rov, config.ps.HasColorOutput(), config.ps.HasDepthROVWrite());

	OMSetRenderTargets(draw_rt, draw_ds, config.scissor, static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags), rtsize);

	// Begin render pass if new target or out of the area.
	if (!InRenderPass())
	{
		const VkAttachmentLoadOp rt_op = GetLoadOpForTexture(draw_rt);
		const VkAttachmentLoadOp ds_op = GetLoadOpForTexture(draw_ds);
		const VkRenderPass rp = GetTFXRenderPass(pipe.rt, pipe.ds, pipe.ps.colclip_hw,
			config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::Stencil, pipe.IsRTFeedbackLoop(),
			pipe.IsTestingAndSamplingDepth(), rt_op, ds_op);
		const bool is_clearing_rt = (rt_op == VK_ATTACHMENT_LOAD_OP_CLEAR || ds_op == VK_ATTACHMENT_LOAD_OP_CLEAR);

		// Only draw to the active area of the colclip hw target. Except when depth is cleared, we need to use the full
		// buffer size, otherwise it'll only clear the draw part of the depth buffer.
		const GSVector4i render_area = (pipe.ps.colclip_hw && (config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve) && ds_op != VK_ATTACHMENT_LOAD_OP_CLEAR)
		                             ? config.drawarea
		                             : GSVector4i::loadh(rtsize);

		if (is_clearing_rt)
		{
			// when we're clearing, we set the draw area to the whole fb, otherwise part of it will be undefined
			alignas(16) VkClearValue cvs[2];
			u32 cv_count = 0;
			if (draw_rt)
			{
				GSVector4 clear_color = draw_rt->GetClearForFormat();
				if (pipe.ps.colclip_hw)
				{
					// Denormalize clear color for hw colclip.
					clear_color *= GSVector4::cxpr(255.0f / 65535.0f, 255.0f / 65535.0f, 255.0f / 65535.0f, 1.0f);
				}
				GSVector4::store<true>(&cvs[cv_count++].color, clear_color);
			}
			if (draw_ds)
				cvs[cv_count++].depthStencil = {draw_ds->GetClearDepth(), 0};

			BeginClearRenderPass(rp, render_area, cvs, cv_count);
		}
		else
		{
			BeginRenderPass(rp, render_area);
		}
	}

	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::StencilOne)
	{
		const VkClearAttachment ca = {VK_IMAGE_ASPECT_STENCIL_BIT, 0u, {.depthStencil = {0.0f, 1u}}};
		const VkClearRect rc = {{{config.drawarea.left, config.drawarea.top},
									{static_cast<u32>(config.drawarea.width()), static_cast<u32>(config.drawarea.height())}},
			0u, 1u};
		vkCmdClearAttachments(m_current_command_buffer, 1, &ca, 1, &rc);
	}

	// rt -> colclip hw blit if enabled
	if (colclip_rt && (config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve) && config.rt->GetState() == GSTexture::State::Dirty)
	{
		OMSetRenderTargets(draw_rt, draw_ds, GSVector4i::loadh(rtsize), static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));
		SetUtilityTexture(static_cast<GSTextureVK*>(config.rt), m_point_sampler);
		SetPipeline(m_colclip_setup_pipelines[pipe.ds][pipe.IsRTFeedbackLoop()]);

		const GSVector4 drawareaf = GSVector4((config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly) ? GSVector4i::loadh(rtsize) : config.drawarea);
		const GSVector4 sRect(drawareaf / GSVector4(rtsize).xyxy());
		DrawStretchRect(sRect, drawareaf, rtsize);

		GL_POP();
		OMSetRenderTargets(draw_rt, draw_ds, config.scissor, static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));
	}

	// VB/IB upload, if we did DATE setup and it's not colclip hw this has already been done
	if (!date_image || colclip_rt)
		UploadHWDrawVerticesAndIndices(config);

	// now we can do the actual draw
	if (BindDrawPipeline(pipe))
		SendHWDraw(config, pipe.IsRTFeedbackLoop() ? draw_rt : nullptr, pipe.IsDepthFeedbackLoop() ? draw_ds : nullptr,
			config.require_one_barrier, config.require_full_barrier);

	// blend second pass
	if (config.blend_multi_pass.enable)
	{
		if (config.blend_multi_pass.blend.constant_enable)
			SetBlendConstants(config.blend_multi_pass.blend.constant);

		pipe.bs = config.blend_multi_pass.blend;
		pipe.ps.no_color1 = config.blend_multi_pass.no_color1;
		pipe.ps.blend_hw = config.blend_multi_pass.blend_hw;
		pipe.ps.dither = config.blend_multi_pass.dither;
		if (BindDrawPipeline(pipe))
		{
			// TODO: This probably should have barriers, in case we want to use it conditionally.
			Draw(config);
		}
	}

	// and the alpha pass
	if (config.alpha_second_pass.enable)
	{
		// cbuffer will definitely be dirty if aref changes, no need to check it
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			SetPSConstantBuffer(config.cb_ps);
		}

		pipe.ps = config.alpha_second_pass.ps;
		pipe.cms = config.alpha_second_pass.colormask;
		pipe.dss = config.alpha_second_pass.depth;
		pipe.bs = config.blend;
		if (BindDrawPipeline(pipe))
		{
			SendHWDraw(config, pipe.IsRTFeedbackLoop() ? draw_rt : nullptr, pipe.IsDepthFeedbackLoop() ? draw_ds : nullptr,
				config.alpha_second_pass.require_one_barrier, config.alpha_second_pass.require_full_barrier);
		}
	}

	if (draw_rt_clone)
		Recycle(draw_rt_clone);

	if (date_image)
		Recycle(date_image);

	// now blit the colclip texture back to the original target
	if (colclip_rt)
	{
		config.colclip_update_area = config.colclip_update_area.runion(config.drawarea);

		if ((config.colclip_mode == GSHWDrawConfig::ColClipMode::ResolveOnly || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve))
		{
			GL_PUSH("Blit ColorClip back to RT");

			EndRenderPass();

			colclip_rt->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);

			draw_rt = static_cast<GSTextureVK*>(config.rt);
			OMSetRenderTargets(draw_rt, draw_ds, (config.colclip_mode == GSHWDrawConfig::ColClipMode::ResolveOnly) ? GSVector4i::loadh(rtsize) : config.scissor, static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));

			// if this target was cleared and never drawn to, perform the clear as part of the resolve here.
			if (draw_rt->GetState() == GSTexture::State::Cleared)
			{
				alignas(16) VkClearValue cvs[2];
				u32 cv_count = 0;
				GSVector4::store<true>(&cvs[cv_count++].color, draw_rt->GetClearForFormat());
				if (draw_ds)
					cvs[cv_count++].depthStencil = {draw_ds->GetClearDepth(), 1};

				BeginClearRenderPass(GetTFXRenderPass(true, pipe.ds, false, false, pipe.IsRTFeedbackLoop(),
										 pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_CLEAR,
										 pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
					draw_rt->GetRect(), cvs, cv_count);
				draw_rt->SetState(GSTexture::State::Dirty);
			}
			else
			{
				BeginRenderPass(GetTFXRenderPass(true, pipe.ds, false, false, pipe.IsRTFeedbackLoop(),
									pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_LOAD,
									pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
					draw_rt->GetRect());
			}

			const GSVector4 drawareaf = GSVector4(config.colclip_update_area);
			const GSVector4 sRect(drawareaf / GSVector4(rtsize).xyxy());
			SetPipeline(m_colclip_finish_pipelines[pipe.ds][pipe.IsRTFeedbackLoop()]);
			SetUtilityTexture(colclip_rt, m_point_sampler);
			DrawStretchRect(sRect, drawareaf, rtsize);

			Recycle(colclip_rt);
			g_gs_device->SetColorClipTexture(nullptr);
		}
	}

	config.colclip_mode = GSHWDrawConfig::ColClipMode::NoModify;
#ifdef ORBIS_VULKAN
	OrbisGpStampDraw(config); // vk-285-94
#endif
}

void GSDeviceVK::UpdateHWPipelineSelector(GSHWDrawConfig& config, PipelineSelector& pipe)
{
	pipe.vs.key = config.vs.key;
	pipe.ps.key_hi = config.ps.key_hi;
	pipe.ps.key_lo = config.ps.key_lo;
	pipe.dss.key = config.ps.HasDepthROV() ? GSHWDrawConfig::DepthStencilSelector::NoDepth().key : config.depth.key;
	pipe.bs.key = config.ps.HasColorROV() ? GSHWDrawConfig::BlendState().key : config.blend.key;
	pipe.bs.constant = 0; // don't dupe states with different alpha values
	pipe.cms.key = config.ps.HasColorROV() ? GSHWDrawConfig::ColorMaskSelector().key : config.colormask.key;
	pipe.topology = static_cast<u32>(config.topology);
	pipe.rt = config.rt != nullptr && !config.ps.HasColorROV();
	pipe.ds = config.ds != nullptr && !config.ps.HasDepthROV();
	pipe.line_width = config.line_expand;
	pipe.feedback_loop_flags = FeedbackLoopFlag_None;
	if (m_features.texture_barrier && (config.require_one_barrier || config.require_full_barrier))
	{
		if (config.IsFeedbackLoopRT(config.ps))
			pipe.feedback_loop_flags |= FeedbackLoopFlag_ReadAndWriteRT;

		if (config.IsFeedbackLoopDepth(config.ps))
			pipe.feedback_loop_flags |= FeedbackLoopFlag_ReadAndWriteDepth;
	}
	if (pipe.ds && !(pipe.feedback_loop_flags & FeedbackLoopFlag_ReadAndWriteDepth))
	{
		pipe.feedback_loop_flags |= (config.tex && config.tex == config.ds) ? FeedbackLoopFlag_ReadDepth : FeedbackLoopFlag_None;
	}

	// enable point size in the vertex shader if we're rendering points regardless of upscaling.
	pipe.vs.point_size |= (config.topology == GSHWDrawConfig::Topology::Point);
}

void GSDeviceVK::UploadHWDrawVerticesAndIndices(GSHWDrawConfig& config)
{
	IASetVertexBuffer(config.verts, sizeof(GSVertex), config.nverts, GetVertexAlignment(config.vs.expand));

	if (config.vs.UseFixedExpandIndexBuffer())
	{
		m_index.start = 0;
		m_index.count = config.nindices;
		SetIndexBuffer(m_expand_index_buffer);
	}
	else if (config.vs.UseVSExpandIndexBuffer())
	{
		VSSetIndexBuffer(config.indices, config.nindices);
		SetVSConstantBuffer(config.cb_vs);
	}
	else
	{
		IASetIndexBuffer(config.indices, config.nindices);
	}
}

VkImageMemoryBarrier GSDeviceVK::GetColorBufferFeedbackBarrier(GSTextureVK* rt) const
{
	const VkImageLayout layout =
		UseFeedbackLoopLayout() ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
	const VkAccessFlags dst_access =
		UseFeedbackLoopLayout() ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
	return {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, dst_access, layout, layout,
		VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, rt->GetImage(), {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
}

VkImageMemoryBarrier GSDeviceVK::GetDepthStencilBufferFeedbackBarrier(GSTextureVK* ds) const
{
	const VkImageLayout layout =
		UseFeedbackLoopLayout() ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
	const VkAccessFlags dst_access =
		UseFeedbackLoopLayout() ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
	return {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, dst_access, layout, layout,
		VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, ds->GetImage(),
		{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0u, 1u, 0u, 1u}};
}

VkDependencyFlags GSDeviceVK::GetFeedbackBarrierDependencyFlags() const
{
	return UseFeedbackLoopLayout() ? (VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT) :
	                                 VK_DEPENDENCY_BY_REGION_BIT;
}

void GSDeviceVK::SendHWDraw(const GSHWDrawConfig& config, GSTextureVK* draw_rt, GSTextureVK* draw_ds,
	bool one_barrier, bool full_barrier)
{
	if (!m_features.texture_barrier) [[unlikely]]
	{
		Draw(config);
		return;
	}

#ifdef PCSX2_DEVBUILD
	if ((one_barrier || full_barrier) && !(config.IsFeedbackLoopRT(m_pipeline_selector.ps) || config.IsFeedbackLoopDepth(m_pipeline_selector.ps))) [[unlikely]]
		Console.Warning("VK: Possible unnecessary barrier detected.");
#endif
	VkDependencyFlags barrier_flags = GetFeedbackBarrierDependencyFlags();

	std::array<VkImageMemoryBarrier, 2> barriers;
	u32 n_barriers = 0;
	if (full_barrier || one_barrier)
	{
		if (draw_rt)
		{
			barriers[0] = GetColorBufferFeedbackBarrier(draw_rt);
			n_barriers++;
		}
		if (draw_ds)
		{
			barriers[1] = GetDepthStencilBufferFeedbackBarrier(draw_ds);
			n_barriers++;
		}
	}

	const auto IssueBarriers = [&]() {
		if (draw_rt)
		{
			vkCmdPipelineBarrier(GetCurrentCommandBuffer(),
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, barrier_flags, 0, nullptr, 0, nullptr, 1, &barriers[0]);
		}
		if (draw_ds)
		{
			vkCmdPipelineBarrier(GetCurrentCommandBuffer(),
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, barrier_flags, 0, nullptr, 0, nullptr, 1, &barriers[1]);
		}
	};

	if (full_barrier)
	{
		pxAssert(config.drawlist && !config.drawlist->empty());

		const u32 indices_per_prim = config.indices_per_prim;
		const u32 draw_list_size = static_cast<u32>(config.drawlist->size());

		GL_PUSH("Split the draw");
		g_perfmon.Put(GSPerfMon::Barriers, n_barriers * static_cast<u32>(draw_list_size));

#ifdef ORBIS_VULKAN
		// vk-285-113 (AI-assisted): one full-barrier draw is one barrier + one driver draw per drawlist group, and a big one (a
		// screen of overlapping sprites, thousands of groups) holds more words than the driver's 2 MiB submission stream takes,
		// however early RenderHW submitted before it: the driver refuses the submission and the queue is lost (Sims, Stuntman, Area 51 in
		// vk-285-108..112; Zatch Bell Mamodo Fury probably too: three hangs, ~6 s after its start). So when the command buffer has reached the budget
		// between two groups, submit it and go on in a fresh one, restarting like ExecuteCommandBufferAndRestartRenderPass does
		// (without its warning per call), and putting back what InvalidateCachedState() wipes: the texture bindings.
		const auto SubmitBetweenGroups = [&]() -> bool {
			const VkRenderPass render_pass = m_current_render_pass;
			if (render_pass == VK_NULL_HANDLE)
				return true; // no render pass, nothing to restart (not seen; the draws below would not record either)
			const std::array<GSTextureVK*, NUM_TFX_TEXTURES> textures = m_tfx_textures;
			const VkPipeline pipeline = m_current_pipeline;
			const GSVector4i render_pass_area = m_current_render_pass_area;
			const GSVector4i scissor = m_scissor;
			GSTexture* const current_rt = m_current_render_target;
			GSTexture* const current_ds = m_current_depth_target;
			const FeedbackLoopFlag current_feedback_loop = m_current_framebuffer_feedback_loop;

			static u32 s_mid_draw_submits = 0;
			if (++s_mid_draw_submits <= 3 || (s_mid_draw_submits % 500) == 0)
				Console.Warning("VK: %u draws in this submission, in the middle of one draw of %u groups: submitting early (%u times)",
					m_orbis_submit_draws, draw_list_size, s_mid_draw_submits);

			EndRenderPass();
			ExecuteCommandBuffer(GetWaitType(false, GSConfig.HWSpinCPUForReadbacks));
			if (m_last_submit_failed)
				return false;

			// The new command buffer samples the same images; their use counters name the one just submitted, and a texture update that
			// sees an older counter would go into the init buffer, which runs before this command buffer's remaining groups.
			m_tfx_textures = textures;
			for (GSTextureVK* tex : m_tfx_textures)
			{
				if (tex)
					tex->SetUseFenceCounter(GetCurrentFenceCounter());
			}
			m_dirty_flags |= DIRTY_FLAG_TFX_TEXTURES;
			OMSetRenderTargets(current_rt, current_ds, scissor, current_feedback_loop);
			BeginRenderPass(GetRenderPassForRestarting(render_pass), render_pass_area);
			SetPipeline(pipeline);
			return ApplyTFXState();
		};
#endif

		for (u32 n = 0, p = 0; n < draw_list_size; n++)
		{
#ifdef ORBIS_VULKAN
			if (n != 0 && m_orbis_submit_draws >= ORBIS_SUBMIT_DRAW_BUDGET && !OrbisDriverIsRADV() && !SubmitBetweenGroups())
				break; // ps5vk only (vk-285-115), as in RenderHW
#endif
			IssueBarriers();

			const u32 count = config.drawlist->at(n) * indices_per_prim;
			Draw(config, p, count);
			p += count;
		}

		return;
	}

	if (one_barrier)
	{
		g_perfmon.Put(GSPerfMon::Barriers, n_barriers);
		IssueBarriers();
	}

	Draw(config);

	if (config.ps.HasColorROV() || config.ps.HasDepthROV())
		g_perfmon.Put(GSPerfMon::DrawCallsROV, 1);
}
