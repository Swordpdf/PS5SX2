// PS5 port frontend: the console side. The shelf runs on its own Vulkan device (the driver linked
// into the eboot) with a VK_KHR_display swapchain on VideoOut, reads the DualSense, plays its key
// sounds on an audio port of its own, and downloads missing covers over HTTPS with the console's
// own libSceHttp2. Everything is torn down again before PCSX2 opens its device, VideoOut and audio.
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_ps5.h"

#include "fe_app.h"
#include "fe_covers.h"
#include "fe_games.h"
#include "fe_i18n.h"
#include "fe_renderer.h"
#include "fe_sound.h"
#include "fe_text.h"
#include "fe_vk.h"
#include "fe_web.h"
#include "third_party/qrcodegen/qrcodegen.h" // vk-285-113: orbis_web_qr
#include "OrbisPaths.h" // OrbisRoot(), OrbisSetRoot() for orbis_pick_root_dir

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

// What sceKernelConvertUtcToLocaltime fills in: 16 bytes (as the open-source KytyPS5 emulator
// implements it), not a struct timezone. vk-285-40 passed an 8-byte struct timezone and an int for
// the DST offset; the call wrote the zone offsets (7200, 3600) over the next stack slot, which held
// the swapchain pointer, and the first vkQueuePresentKHR faulted on it.
struct KernelTimesec
{
	int64_t t;
	uint32_t west_sec; // seconds east of UTC, despite the name
	uint32_t dst_sec;
};

extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);
int scePadInit(void);
int scePadOpen(int32_t userId, int32_t type, int32_t index, const void* param);
int scePadClose(int32_t handle);
int scePadReadState(int32_t handle, void* data);
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(int32_t* userId);
int sceKernelConvertUtcToLocaltime(time_t utc, time_t* local, KernelTimesec* sec, uint64_t* dst_sec);
int sceSystemServiceParamGetInt(int param, int* value);
int sceSystemServiceHideSplashScreen(void);

// libSceNet, libSceSsl and libSceHttp2 as the payload SDK's http2_get sample declares them. The
// system loads all three into every app already, so linking them adds nothing at start-up.
int sceNetInit(void);
int sceNetPoolCreate(const char* name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceSslInit(size_t pool_size);
int sceSslTerm(int ctx);
int sceHttp2Init(int net_pool, int ssl_ctx, size_t pool_size, int max_requests);
int sceHttp2Term(int ctx);
int sceHttp2CreateTemplate(int ctx, const char* user_agent, int http_version, int auto_proxy);
int sceHttp2DeleteTemplate(int tmpl);
int sceHttp2CreateRequestWithURL(int tmpl, const char* method, const char* url, uint64_t content_length);
int sceHttp2DeleteRequest(int req);
int sceHttp2SendRequest(int req, const void* data, size_t size);
int sceHttp2GetStatusCode(int req, int* status);
int sceHttp2ReadData(int req, void* data, size_t size);
int sceHttp2SetResolveTimeOut(int id, uint32_t usec);
int sceHttp2SetConnectTimeOut(int id, uint32_t usec);
int sceHttp2SetSendTimeOut(int id, uint32_t usec);
int sceHttp2SetRecvTimeOut(int id, uint32_t usec);
int sceHttp2AbortRequest(int req);
int sceHttp2SetTimeOut(int id, uint32_t usec);
int sceHttp2SetAutoRedirect(int id, int enable);
int sceNetCtlInit(void);
void sceNetCtlTerm(void);
int sceNetCtlGetState(int* state);

// libSceAudioOut, as orbis-shims/ProsperoAudio.cpp declares it for PCSX2's own output.
int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutOutput(int handle, const void* p);
int sceAudioOutClose(int handle);
}

// The fonts, embedded: PCSX2's Roboto (Apache-2.0) and PromptFont (OFL-1.1), from its resources.
#ifndef FE_FONT_DIR
#error "FE_FONT_DIR must name PCSX2's bin/resources/fonts"
#endif
#define FE_INCBIN(sym, file) \
	__asm__(".section .rodata." #sym ",\"a\",@progbits\n" \
			".balign 16\n" \
			".global " #sym "\n" #sym ":\n" \
			".incbin \"" FE_FONT_DIR "/" file "\"\n" \
			".global " #sym "_end\n" #sym "_end:\n" \
			".byte 0\n" \
			".previous\n")
FE_INCBIN(fe_font_text, "Roboto-Regular.ttf");
FE_INCBIN(fe_font_icons, "promptfont.otf");
extern "C" const uint8_t fe_font_text[], fe_font_text_end[], fe_font_icons[], fe_font_icons_end[];

// vk-285-50: the frontend's own files (frontend/assets): Font Awesome Free's brands font (SIL OFL
// 1.1) for the handles' icons, and the settings page with its two icons (Font Awesome, CC BY 4.0).
#ifndef FE_ASSET_DIR
#error "FE_ASSET_DIR must name frontend/assets"
#endif
#define FE_INCBIN_ASSET(sym, file) \
	__asm__(".section .rodata." #sym ",\"a\",@progbits\n" \
			".balign 16\n" \
			".global " #sym "\n" #sym ":\n" \
			".incbin \"" FE_ASSET_DIR "/" file "\"\n" \
			".global " #sym "_end\n" #sym "_end:\n" \
			".byte 0\n" \
			".previous\n")
FE_INCBIN_ASSET(fe_font_brands, "fonts/fa-brands-400.otf");
FE_INCBIN_ASSET(fe_web_page, "web/index.html");
FE_INCBIN_ASSET(fe_web_discord, "web/discord.svg");
FE_INCBIN_ASSET(fe_web_x, "web/x-twitter.svg");
// vk-285-51: the recommended settings the page's Recommended button restores.
FE_INCBIN_ASSET(fe_presets, "presets.ini");
extern "C" const uint8_t fe_font_brands[], fe_font_brands_end[], fe_web_page[], fe_web_page_end[], fe_web_discord[],
	fe_web_discord_end[], fe_web_x[], fe_web_x_end[], fe_presets[], fe_presets_end[];

namespace
{
using namespace fe;

// xlenore/ps2-covers, the default set: <serial>.jpg, 512x736 (the same source as the user's Twiso).
constexpr const char* kCoverUrl = "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/${serial}.jpg";

// The key sounds' output: a port on the main output, float stereo at 48 kHz in 256-frame grains,
// opened the way PCSX2's own output opens it once the game runs (system user, main port, format 4),
// and a thread that mixes whatever is playing into each grain. sceAudioOutOutput blocks until the
// previous grain has been consumed, which paces the thread.
class AudioOut
{
public:
	bool Start(Mixer* mixer)
	{
		const int init = sceAudioOutInit(); // PCSX2's output calls it again later and ignores the error
		m_handle = sceAudioOutOpen(255, 0, 0, kGrain, SoundBank::kRate, 4);
		std::printf("[frontend] sound: sceAudioOutInit %x, port %x\n", static_cast<unsigned>(init), static_cast<unsigned>(m_handle));
		if (m_handle < 0)
			return false;
		m_mixer = mixer;
		m_thread = std::thread([this]() { Run(); });
		return true;
	}

	void Stop()
	{
		m_quit.store(true);
		if (m_thread.joinable())
			m_thread.join();
		if (m_handle >= 0)
			sceAudioOutClose(m_handle);
		m_handle = -1;
	}

private:
	static constexpr unsigned kGrain = 256;

	void Run()
	{
		alignas(64) float buf[kGrain * 2];
		while (!m_quit.load(std::memory_order_relaxed))
		{
			m_mixer->Mix(buf, static_cast<int>(kGrain));
			sceAudioOutOutput(m_handle, buf);
		}
	}

	Mixer* m_mixer = nullptr;
	int m_handle = -1;
	std::atomic<bool> m_quit{false};
	std::thread m_thread;
};

// libScePad's state, as game_select.cpp and main-boot.cpp read it.
struct PadData
{
	uint32_t buttons;
	uint8_t lx, ly, rx, ry, l2, r2, pad0, pad1;
	uint8_t rest[256];
};
constexpr uint32_t kPadRight = 0x20, kPadLeft = 0x80, kPadCross = 0x4000, kPadOptions = 0x8, kPadL1 = 0x400,
				   kPadR1 = 0x800;
// vk-285-114: the options sheet's buttons.
constexpr uint32_t kPadUp = 0x10, kPadDown = 0x40, kPadTriangle = 0x1000, kPadCircle = 0x2000, kPadSquare = 0x8000;
constexpr uint32_t kPadL2 = 0x100, kPadR2 = 0x200; // vk-285-116: the sheet's tabs

double Now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
}

// The time of day in the console's time zone and clock format.
std::string Clock()
{
	const time_t utc = time(nullptr);
	time_t local = utc;
	// Room to spare after both outputs, so a firmware that writes more cannot reach our locals.
	KernelTimesec sec[2] = {};
	uint64_t dst[2] = {};
	if (sceKernelConvertUtcToLocaltime(utc, &local, &sec[0], &dst[0]) != 0)
		local = utc;
	struct tm tm = {};
	gmtime_r(&local, &tm);
	static int s_format = -1;
	if (s_format < 0)
	{
		int v = 1;
		s_format = sceSystemServiceParamGetInt(3 /* time format */, &v) == 0 ? v : 1;
	}
	char buf[16];
	if (s_format == 0) // 12-hour
		std::snprintf(buf, sizeof(buf), "%d:%02d %s", (tm.tm_hour + 11) % 12 + 1, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
	else
		std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
	return buf;
}

// ---- HTTPS through libSceHttp2, on the cover worker's thread only ----
struct Http
{
	bool tried = false, ok = false, netctl = false;
	int pool = -1, ssl = -1, ctx = -1, tmpl = -1;
	std::atomic<int> active{-1}; // the request in flight, for Abort from the main thread
	// vk-285-110: set by Abort, so a request that was still being set up when it ran doesn't start
	// (Abort only reaches a request once `active` holds it).
	std::atomic<bool> stopping{false};
	// vk-285-110: Abort's call and the worker's delete of the same request never overlap.
	std::mutex abort_mutex;

	// The worker, done with `req`: out of `active` (after any Abort call on it), then deleted.
	void Release(int req)
	{
		{
			std::lock_guard<std::mutex> lock(abort_mutex);
			active = -1;
		}
		sceHttp2DeleteRequest(req);
	}

	bool Init()
	{
		if (tried)
			return ok;
		tried = true;
		// Is there a network at all? vk-285-41's first download never came back (100+ s): an
		// offline console must not wait on the resolver. State 3 is "IP address obtained"; a state
		// the call cannot tell leaves the decision to the request itself.
		const int nc = sceNetCtlInit();
		netctl = nc == 0;
		int state[4] = {-1, 0, 0, 0}; // room to spare, as with the time call
		const int gs = sceNetCtlGetState(state);
		std::printf("[frontend] netctl: init %#x, state %d (%#x)\n", static_cast<unsigned>(nc), state[0],
			static_cast<unsigned>(gs));
		std::fflush(stdout);
		if (gs == 0 && state[0] >= 0 && state[0] < 3)
		{
			std::printf("[frontend] the console is not connected; no cover downloads this time\n");
			std::fflush(stdout);
			return false;
		}
		const int net = sceNetInit(); // an error here only means it was up already
		pool = sceNetPoolCreate("pcsx2-frontend", 64 * 1024, 0);
		ssl = pool >= 0 ? sceSslInit(256 * 1024) : -1;
		ctx = ssl >= 0 ? sceHttp2Init(pool, ssl, 256 * 1024, 1) : -1;
		tmpl = ctx >= 0 ? sceHttp2CreateTemplate(ctx, "PS5SX2/1.0", 3, 1) : -1;
		std::printf("[frontend] https: net %#x pool %#x ssl %#x http2 %#x template %#x\n", static_cast<unsigned>(net),
			static_cast<unsigned>(pool), static_cast<unsigned>(ssl), static_cast<unsigned>(ctx), static_cast<unsigned>(tmpl));
		std::fflush(stdout);
		if (tmpl < 0)
			return false;
		ok = true;
		return true;
	}

	// Main thread, at shutdown: fails the request in flight so the worker can finish.
	void Abort()
	{
		stopping.store(true); // before reading `active`: Get stores `active`, then reads this
		std::lock_guard<std::mutex> lock(abort_mutex);
		const int req = active.load();
		if (req >= 0)
		{
			const int r = sceHttp2AbortRequest(req);
			std::printf("[frontend] aborted request %#x: %#x\n", static_cast<unsigned>(req), static_cast<unsigned>(r));
			std::fflush(stdout);
		}
	}

	// After the worker has stopped: gives the pools back.
	void Term()
	{
		if (tmpl >= 0)
			sceHttp2DeleteTemplate(tmpl);
		if (ctx >= 0)
			sceHttp2Term(ctx);
		if (ssl >= 0)
			sceSslTerm(ssl);
		if (pool >= 0)
			sceNetPoolDestroy(pool);
		if (netctl)
			sceNetCtlTerm();
		tmpl = ctx = ssl = pool = -1;
		tried = ok = netctl = false;
		stopping = false;
	}

	// The HTTP status, or -1 when the request could not be made at all. Every step is logged with
	// its time: vk-285-41's first request never returned, and the log could not say where.
	int Get(const std::string& url, std::vector<uint8_t>& out)
	{
		if (stopping || !Init())
			return -1;
		const double t0 = Now();
		auto ms = [&] { return (Now() - t0) * 1000.0; };
		const int req = sceHttp2CreateRequestWithURL(tmpl, "GET", url.c_str(), 0);
		std::printf("[frontend] get %s: request %#x\n", url.c_str(), static_cast<unsigned>(req));
		std::fflush(stdout);
		if (req < 0)
			return -1;
		// On the request, as Swordpdf/Twiso's working cover fetcher sets them (microseconds): each
		// phase 10 s, the whole request 20 s, redirects followed.
		const int t_resolve = sceHttp2SetResolveTimeOut(req, 10 * 1000 * 1000);
		const int t_connect = sceHttp2SetConnectTimeOut(req, 10 * 1000 * 1000);
		const int t_send = sceHttp2SetSendTimeOut(req, 10 * 1000 * 1000);
		const int t_recv = sceHttp2SetRecvTimeOut(req, 10 * 1000 * 1000);
		const int t_total = sceHttp2SetTimeOut(req, 20 * 1000 * 1000);
		const int redirect = sceHttp2SetAutoRedirect(req, 1);
		std::printf("[frontend] get: timeouts %#x %#x %#x %#x %#x, redirect %#x\n", static_cast<unsigned>(t_resolve),
			static_cast<unsigned>(t_connect), static_cast<unsigned>(t_send), static_cast<unsigned>(t_recv),
			static_cast<unsigned>(t_total), static_cast<unsigned>(redirect));
		std::fflush(stdout);
		active = req;
		if (stopping) // Abort ran while this request was being set up, before `active` held it
		{
			Release(req);
			std::printf("[frontend] get: stopped before sending\n");
			std::fflush(stdout);
			return -1;
		}
		int status = -1;
		const int sent = sceHttp2SendRequest(req, nullptr, 0);
		const int got = sent == 0 ? sceHttp2GetStatusCode(req, &status) : -1;
		std::printf("[frontend] get: send %#x, status %d (%#x) after %.0f ms\n", static_cast<unsigned>(sent), status,
			static_cast<unsigned>(got), ms());
		std::fflush(stdout);
		if (sent != 0 || got != 0)
			status = -1;
		else if (status == 200)
		{
			std::vector<uint8_t> buf(64 * 1024);
			for (;;)
			{
				const int n = sceHttp2ReadData(req, buf.data(), buf.size());
				if (n < 0)
				{
					std::printf("[frontend] get: read %#x after %zu bytes\n", static_cast<unsigned>(n), out.size());
					status = -1;
					break;
				}
				if (n == 0)
					break;
				out.insert(out.end(), buf.begin(), buf.begin() + n);
				if (out.size() > (16u << 20))
				{
					status = -1;
					break;
				}
			}
			std::printf("[frontend] get: %zu bytes in %.0f ms\n", out.size(), ms());
			std::fflush(stdout);
		}
		Release(req);
		return status;
	}
};
Http g_http;

// ---- The display ----
struct Display
{
	Vk vk;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	uint32_t qf = 0;
	VkQueue queue = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	std::vector<VkImage> images;
	VkExtent2D extent = {};
	VkSemaphore acquired[2] = {}, rendered[2] = {};

	bool Fail(const char* what, VkResult r)
	{
		std::printf("[frontend] %s failed: %d\n", what, static_cast<int>(r));
		std::fflush(stdout);
		return false;
	}

	bool Init()
	{
		const char* missing = nullptr;
		if (!vk.LoadGlobal(vk_icdGetInstanceProcAddr, &missing))
		{
			std::printf("[frontend] no %s\n", missing);
			return false;
		}
		const char* inst_ext[2] = {"VK_KHR_surface", "VK_KHR_display"};
		VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
		ai.pApplicationName = "PS5SX2 frontend";
		ai.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ici.pApplicationInfo = &ai;
		ici.enabledExtensionCount = 2;
		ici.ppEnabledExtensionNames = inst_ext;
		VkResult r = vk.vkCreateInstance(&ici, nullptr, &instance);
		if (r != VK_SUCCESS)
			return Fail("vkCreateInstance", r);
		if (!vk.LoadInstance(instance, true, &missing))
		{
			std::printf("[frontend] no %s\n", missing);
			return false;
		}
		uint32_t count = 1;
		r = vk.vkEnumeratePhysicalDevices(instance, &count, &pd);
		if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0)
			return Fail("vkEnumeratePhysicalDevices", r);
		uint32_t qcount = 0;
		vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
		std::vector<VkQueueFamilyProperties> qfs(qcount);
		vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, qfs.data());
		qf = UINT32_MAX;
		for (uint32_t i = 0; i < qcount; i++)
			if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			{
				qf = i;
				break;
			}
		if (qf == UINT32_MAX)
			return Fail("a graphics queue", VK_ERROR_UNKNOWN);
		const float prio = 1.0f;
		VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qi.queueFamilyIndex = qf;
		qi.queueCount = 1;
		qi.pQueuePriorities = &prio;
		const char* dev_ext[1] = {"VK_KHR_swapchain"};
		VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qi;
		dci.enabledExtensionCount = 1;
		dci.ppEnabledExtensionNames = dev_ext;
		r = vk.vkCreateDevice(pd, &dci, nullptr, &device);
		if (r != VK_SUCCESS)
			return Fail("vkCreateDevice", r);
		if (!vk.LoadDevice(device, true, &missing))
		{
			std::printf("[frontend] no %s\n", missing);
			return false;
		}
		vk.vkGetDeviceQueue(device, qf, 0, &queue);
		return CreateSurface() && CreateSwapchain();
	}

	// As PCSX2's VKSwapChain does on the PS5: the first display with a mode, its largest mode (of equally
	// large ones the one nearest 60 Hz, vk-285-115), and a plane that can drive it.
	bool CreateSurface()
	{
		uint32_t display_count = 0;
		if (vk.vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &display_count, nullptr) != VK_SUCCESS || !display_count)
			return Fail("vkGetPhysicalDeviceDisplayPropertiesKHR", VK_ERROR_UNKNOWN);
		std::vector<VkDisplayPropertiesKHR> displays(display_count);
		vk.vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &display_count, displays.data());
		uint32_t plane_count = 0;
		if (vk.vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &plane_count, nullptr) != VK_SUCCESS || !plane_count)
			return Fail("vkGetPhysicalDeviceDisplayPlanePropertiesKHR", VK_ERROR_UNKNOWN);
		std::vector<VkDisplayPlanePropertiesKHR> planes(plane_count);
		vk.vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &plane_count, planes.data());
		VkDisplayKHR display = VK_NULL_HANDLE;
		VkDisplayModePropertiesKHR mode = {};
		for (const VkDisplayPropertiesKHR& d : displays)
		{
			uint32_t mode_count = 0;
			if (vk.vkGetDisplayModePropertiesKHR(pd, d.display, &mode_count, nullptr) != VK_SUCCESS || !mode_count)
				continue;
			std::vector<VkDisplayModePropertiesKHR> modes(mode_count);
			vk.vkGetDisplayModePropertiesKHR(pd, d.display, &mode_count, modes.data());
			auto off_60hz = [](const VkDisplayModePropertiesKHR& x) {
				const int64_t mhz = static_cast<int64_t>(x.parameters.refreshRate);
				return mhz > 60000 ? mhz - 60000 : 60000 - mhz;
			};
			for (const VkDisplayModePropertiesKHR& m : modes)
			{
				const uint64_t area = static_cast<uint64_t>(m.parameters.visibleRegion.width) * m.parameters.visibleRegion.height;
				const uint64_t best = static_cast<uint64_t>(mode.parameters.visibleRegion.width) * mode.parameters.visibleRegion.height;
				if (!display || area > best || (area == best && off_60hz(m) < off_60hz(mode)))
				{
					display = d.display;
					mode = m;
				}
			}
			if (display)
				break;
		}
		if (!display)
			return Fail("a display mode", VK_ERROR_UNKNOWN);
		uint32_t plane = UINT32_MAX;
		for (uint32_t i = 0; i < plane_count; i++)
			if (planes[i].currentDisplay == VK_NULL_HANDLE || planes[i].currentDisplay == display)
			{
				plane = i;
				break;
			}
		if (plane == UINT32_MAX)
			return Fail("a display plane", VK_ERROR_UNKNOWN);
		const VkDisplaySurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR, nullptr, 0,
			mode.displayMode, plane, planes[plane].currentStackIndex, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, 1.0f,
			VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR, mode.parameters.visibleRegion};
		const VkResult r = vk.vkCreateDisplayPlaneSurfaceKHR(instance, &ci, nullptr, &surface);
		if (r != VK_SUCCESS)
			return Fail("vkCreateDisplayPlaneSurfaceKHR", r);
		extent = mode.parameters.visibleRegion;
		std::printf("[frontend] display %ux%u at %.2f Hz, plane %u\n", extent.width, extent.height, mode.parameters.refreshRate / 1000.0,
			plane);
		return true;
	}

	bool CreateSwapchain()
	{
		VkSurfaceCapabilitiesKHR caps = {};
		VkResult r = vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
		if (r != VK_SUCCESS)
			return Fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
		if (caps.currentExtent.width != UINT32_MAX)
			extent = caps.currentExtent;
		uint32_t images_wanted = std::max(2u, caps.minImageCount);
		if (caps.maxImageCount)
			images_wanted = std::min(images_wanted, caps.maxImageCount);
		VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
		sci.surface = surface;
		sci.minImageCount = images_wanted;
		sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
		sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		sci.imageExtent = extent;
		sci.imageArrayLayers = 1;
		sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
		sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
		sci.clipped = VK_TRUE;
		r = vk.vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain);
		if (r != VK_SUCCESS)
			return Fail("vkCreateSwapchainKHR", r);
		uint32_t n = 0;
		vk.vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
		images.resize(n);
		vk.vkGetSwapchainImagesKHR(device, swapchain, &n, images.data());
		VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		for (int i = 0; i < 2; i++)
			if (vk.vkCreateSemaphore(device, &si, nullptr, &acquired[i]) != VK_SUCCESS ||
				vk.vkCreateSemaphore(device, &si, nullptr, &rendered[i]) != VK_SUCCESS)
				return Fail("vkCreateSemaphore", VK_ERROR_UNKNOWN);
		std::printf("[frontend] swapchain: %u images %ux%u\n", n, extent.width, extent.height);
		return true;
	}

	void Destroy()
	{
		if (device)
		{
			vk.vkDeviceWaitIdle(device);
			for (int i = 0; i < 2; i++)
			{
				if (acquired[i])
					vk.vkDestroySemaphore(device, acquired[i], nullptr);
				if (rendered[i])
					vk.vkDestroySemaphore(device, rendered[i], nullptr);
				acquired[i] = rendered[i] = VK_NULL_HANDLE;
			}
			if (swapchain)
				vk.vkDestroySwapchainKHR(device, swapchain, nullptr); // closes VideoOut for PCSX2
			swapchain = VK_NULL_HANDLE;
			vk.vkDestroyDevice(device, nullptr);
			device = VK_NULL_HANDLE;
		}
		if (surface)
			vk.vkDestroySurfaceKHR(instance, surface, nullptr);
		surface = VK_NULL_HANDLE;
		if (instance)
			vk.vkDestroyInstance(instance, nullptr);
		instance = VK_NULL_HANDLE;
	}
};

std::string ReadLastGame(const std::string& dir)
{
	std::string name;
	if (FILE* f = std::fopen((dir + "/lastgame.txt").c_str(), "r"))
	{
		char buf[512] = {};
		if (std::fgets(buf, sizeof(buf), f))
			name = buf;
		std::fclose(f);
	}
	while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
		name.pop_back();
	return name;
}

void WriteLastGame(const std::string& dir, const std::string& file)
{
	if (FILE* f = std::fopen((dir + "/lastgame.txt").c_str(), "w"))
	{
		std::fprintf(f, "%s\n", file.c_str());
		std::fclose(f);
	}
}

// ---- Test build 1 (vk-285-55): games on USB drives.

std::string LowerAscii(std::string s)
{
	for (char& c : s)
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	return s;
}

// The disc images (.iso, .chd) straight in `dir`, or -1 when it can't be opened as a folder.
int CountImages(const std::string& dir)
{
	DIR* d = opendir(dir.c_str());
	if (!d)
		return -1;
	int n = 0;
	while (const dirent* e = readdir(d))
		n += IsDiscImageName(e->d_name) ? 1 : 0;
	closedir(d);
	return n;
}

bool OnUsb(const std::string& path)
{
	return path.compare(0, 8, "/mnt/usb") == 0;
}

// cache/usb-games.txt, one "<serial>\t<stem>\t<title>" line per USB game: before the jailbreak,
// where covers download, the app may not see the drives, so the covers of the games found on them
// after the jailbreak are fetched at the next start from this list.
void WriteUsbList(const std::string& path, const std::vector<GameInfo>& games)
{
	if (path.empty())
		return;
	std::string text;
	for (const GameInfo& g : games)
		if (OnUsb(g.path) && !g.serial.empty())
			text += g.serial + "\t" + g.stem + "\t" + g.title + "\n";
	std::string old;
	if (FILE* f = std::fopen(path.c_str(), "rb"))
	{
		char buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
			old.append(buf, n);
		std::fclose(f);
	}
	if (text.empty() || text == old)
		return;
	const size_t slash = path.rfind('/');
	if (slash != std::string::npos)
		mkdir(path.substr(0, slash).c_str(), 0777);
	if (FILE* f = std::fopen(path.c_str(), "wb"))
	{
		std::fwrite(text.data(), 1, text.size(), f);
		std::fclose(f);
	}
}

// Adds the listed USB games that `games` doesn't hold (by serial), with no image path: enough for the
// cover prefetch, which works from serials.
int AddUsbListGames(const std::string& path, std::vector<GameInfo>& games)
{
	FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
	if (!f)
		return 0;
	int added = 0;
	char line[1024];
	while (std::fgets(line, sizeof(line), f))
	{
		std::string l = line;
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
			l.pop_back();
		const size_t t1 = l.find('\t'), t2 = t1 == std::string::npos ? t1 : l.find('\t', t1 + 1);
		if (t2 == std::string::npos || t1 == 0)
			continue;
		GameInfo g;
		g.serial = l.substr(0, t1);
		g.stem = l.substr(t1 + 1, t2 - t1 - 1);
		g.title = l.substr(t2 + 1);
		if (std::any_of(games.begin(), games.end(), [&](const GameInfo& x) { return x.serial == g.serial; }))
			continue;
		games.push_back(g);
		added++;
	}
	std::fclose(f);
	return added;
}
} // namespace

std::vector<std::string> orbis_usb_game_dirs(const char* when)
{
	std::vector<std::string> dirs;
	int drives = 0;
	for (int i = 0; i < 8; i++)
	{
		char root[16];
		std::snprintf(root, sizeof(root), "/mnt/usb%d", i);
		DIR* d = opendir(root);
		if (!d)
		{
			if (when && errno != ENOENT)
				std::printf("[usb] %s: %s: can't open (errno %d)\n", when, root, errno);
			continue;
		}
		int entries = 0, isos = 0;
		std::vector<std::string> subs;
		while (const dirent* e = readdir(d))
		{
			if (e->d_name[0] == '.')
				continue;
			entries++;
			isos += IsDiscImageName(e->d_name) ? 1 : 0;
			const std::string lower = LowerAscii(e->d_name);
			if (lower == "dvd" || lower == "cd" || lower == "ps5sx2")
				subs.push_back(e->d_name);
		}
		closedir(d);
		if (entries == 0)
			continue; // no drive in this slot (the folder is there either way)
		drives++;
		dirs.push_back(root);
		std::string found = "/ (" + std::to_string(isos) + ")";
		for (const std::string& sub : subs)
		{
			const std::string dir = std::string(root) + "/" + sub;
			const int n = CountImages(dir);
			if (n < 0)
				continue;
			dirs.push_back(dir);
			found += ", " + sub + "/ (" + std::to_string(n) + ")";
		}
		if (when)
			std::printf("[usb] %s: %s: %d entries; disc images in %s\n", when, root, entries, found.c_str());
	}
	if (when && drives == 0)
		std::printf("[usb] %s: no USB drive with files at /mnt/usb0-7\n", when);
	if (when)
		std::fflush(stdout);
	return dirs;
}

bool orbis_frontend_watermark(const char* line1, const char* line2, const char* line3, float alpha1, float alpha2,
	std::vector<uint32_t>& rgba, int& w, int& h)
{
	Fonts fonts;
	if (!fonts.Init(fe_font_text, static_cast<size_t>(fe_font_text_end - fe_font_text), fe_font_icons,
			static_cast<size_t>(fe_font_icons_end - fe_font_icons)))
		return false;
	RasterWatermark(fonts, line1, line2, line3, alpha1, alpha2, rgba, w, h);
	return true;
}

// vk-285-50: the settings page's server; it lives until the app ends.
static fe::WebServer* g_web = nullptr;

// vk-285-51: the settings log's clock, in the console's time zone (as Clock() above).
static long long SettingsLogLocalTime(long long utc)
{
	time_t local = static_cast<time_t>(utc);
	KernelTimesec sec[2] = {};
	uint64_t dst[2] = {};
	if (sceKernelConvertUtcToLocaltime(static_cast<time_t>(utc), &local, &sec[0], &dst[0]) != 0)
		local = static_cast<time_t>(utc);
	return static_cast<long long>(local);
}

// vk-285-53: sceSystemServiceHideSplashScreen once per process (later calls return the first result).
int orbis_hide_splash()
{
	static std::atomic<int> s_result{1};
	int expected = 1;
	if (s_result.compare_exchange_strong(expected, 2))
		s_result.store(sceSystemServiceHideSplashScreen());
	return s_result.load();
}

int orbis_ps5_language()
{
	static int s_lang = -2;
	if (s_lang == -2)
	{
		int v = 1;
		const int rc = sceSystemServiceParamGetInt(1 /* language */, &v);
		s_lang = rc == 0 ? v : 1;
		std::printf("[i18n] system language: rc=%x value=%d\n", static_cast<unsigned>(rc), v);
		std::fflush(stdout);
	}
	return s_lang;
}

void orbis_frontend_set_language(const std::string& lang_dir)
{
	fe::SetLanguage(orbis_ps5_language(), lang_dir);
}

// vk-285-113: the settings log on a console that shows /data only after the jailbreak. The path given at the start
// is then the top folder's (logs/ isn't visible yet) and every line written before the jailbreak was lost, the
// "app start" line with them (v112's console ...575 on firmware 10.20: sessions with no build and no game). Now:
//   - logs/ is looked for at every write, so the file is logs/settings.log as soon as that folder is there;
//   - a line that can't be written yet waits in a small buffer and goes out, in order, with the next one that can;
//   - the first write of a run first looks at the end of the file for the previous run's closing line and, when
//     there is none, says so: the app was closed from the PS button, the system killed it, the console lost power,
//     or it crashed without a signal.
// All of it is open()/pread()/write() on stack buffers, so the crash handler and the exit hooks can still call it.
static char g_event_log_path[256];     // as given
static char g_event_log_dir_logs[256]; // <dir>/logs, checked at each write ("" when the path already is inside a logs folder)
static char g_event_log_in_logs[256];  // <dir>/logs/<name>
static char g_event_pending[8192];     // formatted lines waiting for a folder
static size_t g_event_pending_len = 0;
static std::atomic<bool> g_event_busy{false};
static bool g_event_previous_checked = false;

void orbis_event_log_init(const std::string& path)
{
	std::snprintf(g_event_log_path, sizeof(g_event_log_path), "%s", path.c_str());
	g_event_log_dir_logs[0] = g_event_log_in_logs[0] = 0;
	const size_t slash = path.rfind('/');
	if (slash != std::string::npos && slash > 0)
	{
		const std::string dir = path.substr(0, slash);
		const size_t dslash = dir.rfind('/');
		const std::string last = dslash == std::string::npos ? dir : dir.substr(dslash + 1);
		if (last != "logs")
		{
			std::snprintf(g_event_log_dir_logs, sizeof(g_event_log_dir_logs), "%s/logs", dir.c_str());
			std::snprintf(g_event_log_in_logs, sizeof(g_event_log_in_logs), "%s/logs/%s", dir.c_str(), path.c_str() + slash + 1);
		}
	}
	fe::g_utc_to_local = &SettingsLogLocalTime;
}

// Whether a log line's text (after the time) says the run that wrote it is over. The lines the app itself writes
// as it ends: main-boot.cpp's exits, the crash printer, the GPU-hang exit and the new signal and exit hooks.
static bool EventEndsARun(const char* text)
{
	static const char* const kEnds[] = {"back to the menu", "the app closed", "GPU hang", "crash:", "no game to start",
		"the game didn't start", "the system ended the app", "app exit"};
	for (const char* e : kEnds)
		if (std::strstr(text, e))
			return true;
	return false;
}

// The note about the previous run, or 0 when it closed itself properly (or there is no earlier log).
static int EventPreviousRunNote(int fd, char* out, size_t out_size)
{
	struct stat st = {};
	if (fstat(fd, &st) != 0 || st.st_size <= 0)
		return 0;
	char tail[2048];
	const off_t from = st.st_size > static_cast<off_t>(sizeof(tail)) ? st.st_size - static_cast<off_t>(sizeof(tail)) : 0;
	const ssize_t got = pread(fd, tail, sizeof(tail) - 1, from);
	if (got <= 0)
		return 0;
	tail[got] = 0;
	// the last line: after the last newline that isn't the final character
	ssize_t end = got;
	while (end > 0 && (tail[end - 1] == '\n' || tail[end - 1] == '\r'))
		end--;
	if (end <= 0)
		return 0;
	ssize_t begin = end;
	while (begin > 0 && tail[begin - 1] != '\n')
		begin--;
	tail[end] = 0;
	const char* last = tail + begin;
	// "YYYY-MM-DD HH:MM:SS  text"
	const bool stamped = end - begin > 21 && last[4] == '-' && last[7] == '-' && last[10] == ' ' && last[13] == ':' && last[16] == ':';
	if (stamped && EventEndsARun(last + 21))
		return 0;
	if (!stamped)
		return 0; // not a line of ours
	return std::snprintf(out, out_size,
		"previous run: its log ends without a closing line (last entry %.19s: \"%.50s\"): the app was closed from the PS button or "
		"by the system, the console lost power, or the app crashed without a signal",
		last, last + 21);
}

extern "C" void orbis_event_log(const char* line)
{
	if (!g_event_log_path[0] || !line)
		return;
	const time_t local = static_cast<time_t>(SettingsLogLocalTime(static_cast<long long>(time(nullptr))));
	struct tm tm = {};
	gmtime_r(&local, &tm);
	char buf[1280];
	int n = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d  %s\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec, line);
	if (n <= 0)
		return;
	if (n >= static_cast<int>(sizeof(buf)))
	{
		n = static_cast<int>(sizeof(buf)) - 1;
		buf[n - 1] = '\n';
	}
	// logs/ once it is there, else the path as given.
	const char* path = g_event_log_path;
	if (g_event_log_dir_logs[0])
	{
		struct stat st = {};
		if (stat(g_event_log_dir_logs, &st) == 0 && S_ISDIR(st.st_mode))
			path = g_event_log_in_logs;
	}
	// One thread at a time in the buffer; a caller that finds it taken (another thread, or a signal that interrupted
	// it) writes its line by itself and leaves the buffer alone.
	const bool own = !g_event_busy.exchange(true, std::memory_order_acquire);
	const int fd = open(path, O_RDWR | O_APPEND | O_CREAT, 0666);
	if (fd < 0)
	{
		if (own)
		{
			if (g_event_pending_len + static_cast<size_t>(n) <= sizeof(g_event_pending))
			{
				std::memcpy(g_event_pending + g_event_pending_len, buf, static_cast<size_t>(n));
				g_event_pending_len += static_cast<size_t>(n);
			}
			g_event_busy.store(false, std::memory_order_release);
		}
		return;
	}
	if (own && !g_event_previous_checked)
	{
		g_event_previous_checked = true;
		char note[400];
		const int m = EventPreviousRunNote(fd, note + 0, sizeof(note) - 1);
		if (m > 0)
		{
			char stamped[480];
			const int k = std::snprintf(stamped, sizeof(stamped), "%04d-%02d-%02d %02d:%02d:%02d  %s\n", tm.tm_year + 1900, tm.tm_mon + 1,
				tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, note);
			if (k > 0)
				(void)!write(fd, stamped, static_cast<size_t>(k < static_cast<int>(sizeof(stamped)) ? k : sizeof(stamped) - 1));
		}
	}
	if (own && g_event_pending_len)
	{
		(void)!write(fd, g_event_pending, g_event_pending_len);
		g_event_pending_len = 0;
	}
	(void)!write(fd, buf, static_cast<size_t>(n));
	close(fd);
	if (own)
		g_event_busy.store(false, std::memory_order_release);
}

bool orbis_web_start(const OrbisFrontendPaths& paths, const char* build_tag)
{
	if (g_web)
		return true;
	SetSerialCacheFile(paths.serial_cache); // vk-285-108
	SetGameDbFile(paths.gamedb_file);       // vk-285-113
	WebConfig cfg;
	cfg.game_dirs = {paths.games_dir, paths.top_dir};
	cfg.game_dirs.insert(cfg.game_dirs.end(), paths.usb_dirs.begin(), paths.usb_dirs.end()); // test build 1
	cfg.settings_dir = paths.settings_dir;
	cfg.gs_ini = paths.gs_ini;
	cfg.patches_dir = paths.patches_dir;
	cfg.covers_dir = paths.covers_dir;
	cfg.cache_dir = paths.cache_dir;
	cfg.token_path = paths.top_dir + "/webui_token.txt";
	cfg.build_tag = build_tag ? build_tag : "";
	cfg.port = 8844;
	cfg.presets.assign(reinterpret_cast<const char*>(fe_presets), static_cast<size_t>(fe_presets_end - fe_presets));
	cfg.change_log = paths.settings_log;
	// Test build 1 (vk-285-55): the logs download.
	cfg.logs_dir = paths.logs_dir;
	cfg.top_dir = paths.top_dir;
	cfg.memcards_dir = paths.memcards_dir; // vk-285-113
	cfg.report_header = paths.report_header;
	cfg.test_build = paths.test_build;
	cfg.root_config_path = "/data/ps5sx2_root.txt"; // configurable data root
	fe::g_utc_to_local = &SettingsLogLocalTime;
	cfg.assets = {
		{"/", "text/html; charset=utf-8", fe_web_page, static_cast<size_t>(fe_web_page_end - fe_web_page)},
		{"/fonts/roboto.ttf", "font/ttf", fe_font_text, static_cast<size_t>(fe_font_text_end - fe_font_text)},
		{"/icons/discord.svg", "image/svg+xml", fe_web_discord, static_cast<size_t>(fe_web_discord_end - fe_web_discord)},
		{"/icons/x-twitter.svg", "image/svg+xml", fe_web_x, static_cast<size_t>(fe_web_x_end - fe_web_x)},
	};
	fe::WebServer* server = new fe::WebServer();
	if (!server->Start(cfg))
	{
		std::printf("[web] the settings page could not start\n");
		std::fflush(stdout);
		delete server;
		return false;
	}
	g_web = server;
	std::string url, shown;
	std::printf("[web] %s\n", g_web->Address(url, shown) ? ("http://" + shown).c_str() : "no network address yet");
	std::fflush(stdout);
	return true;
}

void orbis_web_now_playing(const std::string& image_path)
{
	if (g_web)
		g_web->SetNowPlaying(image_path);
}

// vk-285-113: the page's address as a QR code (the one the shelf shows) for the panel drawn over the game.
bool orbis_web_qr(std::vector<unsigned char>& modules, int& size, std::string& shown)
{
	if (!g_web)
		return false;
	std::string url;
	if (!g_web->Address(url, shown))
		return false;
	uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
	if (!qrcodegen_encodeText(url.c_str(), tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true))
		return false;
	size = qrcodegen_getSize(qr);
	modules.assign(static_cast<size_t>(size) * size, 0);
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++)
			modules[static_cast<size_t>(y) * size + x] = qrcodegen_getModule(qr, x, y) ? 1 : 0;
	return true;
}

bool orbis_web_browser_url(std::string& url, std::string& shown)
{
	if (!g_web)
		return false;
	if (g_web->Address(url, shown))
		return true;
	if (g_web->Port() == 0)
		return false;
	url = g_web->LoopbackUrl();
	shown = "127.0.0.1:" + std::to_string(g_web->Port());
	return true;
}

void orbis_web_request_stats(uint64_t& count, double& age_s)
{
	count = 0;
	age_s = -1.0;
	if (g_web)
		g_web->RequestStats(count, age_s);
}

void orbis_web_log_next_requests(int n)
{
	if (g_web)
		g_web->LogNextRequests(n);
}

int orbis_frontend_prefetch_covers(const OrbisFrontendPaths& paths, double budget_s, void (*notify)(const char*))
{
	if (!paths.allow_download)
		return 0;
	const double t0 = Now();
	SetSerialCacheFile(paths.serial_cache); // vk-285-108
	SetGameDbFile(paths.gamedb_file);       // vk-285-113
	std::vector<std::string> dirs = {paths.games_dir, paths.top_dir};
	dirs.insert(dirs.end(), paths.usb_dirs.begin(), paths.usb_dirs.end()); // test build 1: USB drives, when visible here
	std::vector<GameInfo> games = ScanGames(dirs);
	for (GameInfo& g : games)
		g.serial = ReadSerial(g.path);
	// Test build 1: the USB games seen after the jailbreak last time (the drives may not be visible here).
	const int listed = AddUsbListGames(paths.usb_list, games);
	CoverConfig cc;
	cc.manual_dir = paths.covers_dir;
	cc.cache_dir = paths.cache_dir;
	cc.url_template = kCoverUrl;
	const std::vector<int> missing = CoverService::MissingCovers(games, cc);
	std::printf("[frontend] prefetch: %zu of %zu covers to fetch (%d from the USB list; checked in %.0f ms)\n", missing.size(),
		games.size(), listed, (Now() - t0) * 1000.0);
	std::fflush(stdout);
	if (missing.empty())
		return 0;
	if (notify)
	{
		char buf[96];
		std::snprintf(buf, sizeof(buf), fe::Tr(missing.size() == 1 ? fe::Str::NotifyCoversOne : fe::Str::NotifyCoversMany),
			static_cast<int>(missing.size())); // vk-285-110
		notify(buf);
	}
	const int saved = CoverService::Prefetch(games, missing, cc,
		[](const std::string& url, std::vector<uint8_t>& out) { return g_http.Get(url, out); }, budget_s);
	g_http.Term();
	std::printf("[frontend] prefetch: %d saved in %.1f s\n", saved, Now() - t0);
	std::fflush(stdout);
	return saved;
}

std::string orbis_frontend_run(const OrbisFrontendPaths& paths, const char* build_tag, bool* ran)
{
	*ran = false;
	const double t0 = Now();
	SetSerialCacheFile(paths.serial_cache); // vk-285-108
	SetGameDbFile(paths.gamedb_file);       // vk-285-113
	std::vector<std::string> dirs = {paths.games_dir, paths.top_dir};
	dirs.insert(dirs.end(), paths.usb_dirs.begin(), paths.usb_dirs.end()); // test build 1: USB drives
	std::vector<GameInfo> games = ScanGames(dirs);
	int on_usb = 0;
	for (GameInfo& g : games)
	{
		g.serial = ReadSerial(g.path);
		// vk-285-113: a CHD (or an ISO) named after its file shows the game's name from the game database.
		const std::string file_title = g.title;
		const bool renamed = ApplyGameDbTitle(g);
		ReadBadges(g, paths.settings_dir, paths.gs_ini, paths.patches_dir);
		on_usb += OnUsb(g.path) ? 1 : 0;
		if (renamed)
			std::printf("[frontend] %s: titled \"%s\" (the file name gave \"%s\")\n", g.file.c_str(), g.title.c_str(), file_title.c_str());
		// Test build 1: the size too (a disc image of an odd size is often a bad dump), and the folder.
		std::printf("[frontend] %s | %s | %s | %llu bytes | %s\n", g.file.c_str(), g.serial.empty() ? "no serial" : g.serial.c_str(),
			g.title.c_str(), static_cast<unsigned long long>(g.bytes), g.path.substr(0, g.path.rfind('/')).c_str());
		// vk-285-109: a CHD without a serial says what it is.
		if (g.serial.empty())
		{
			const std::string what = DescribeImage(g.path);
			if (!what.empty())
				std::printf("[frontend]   chd: %s\n", what.c_str());
		}
	}
	SortGames(games); // vk-285-113: by the titles the game database may have changed
	std::printf("[frontend] %zu disc image(s), %d on USB, scanned in %.0f ms\n", games.size(), on_usb, (Now() - t0) * 1000.0);
	std::fflush(stdout);
	WriteUsbList(paths.usb_list, games);
	if (games.empty())
	{
		*ran = true;
		return {};
	}
	const std::string last = ReadLastGame(paths.top_dir);
	int preselect = 0;
	for (size_t i = 0; i < games.size(); i++)
		if (games[i].file == last)
			preselect = static_cast<int>(i);
	// vk-285-69: one game opens the shelf too, not the game straight away: the shelf's QR code is how
	// testers reach the settings page and its Download logs (Spyros, 2026-09-26).

	int32_t user = -1;
	(void)sceUserServiceInitialize(nullptr);
	(void)sceUserServiceGetInitialUser(&user);
	(void)scePadInit();
	const int pad = user >= 0 ? scePadOpen(user, 0, 0, nullptr) : -1;
	if (pad < 0)
	{
		std::printf("[frontend] no controller (%d); the plain list takes over\n", pad);
		return {};
	}

	// Heap objects, so a download still running at the end can be left to finish on its own.
	Display* display = new Display();
	if (!display->Init())
	{
		display->Destroy();
		delete display;
		scePadClose(pad);
		return {};
	}
	Fonts* fonts = new Fonts();
	Renderer renderer;
	if (!fonts->Init(fe_font_text, static_cast<size_t>(fe_font_text_end - fe_font_text), fe_font_icons,
			static_cast<size_t>(fe_font_icons_end - fe_font_icons), fe_font_brands,
			static_cast<size_t>(fe_font_brands_end - fe_font_brands)) ||
		!renderer.Init(&display->vk, display->pd, display->device, display->qf, display->queue, display->extent.width,
			display->extent.height, VK_FORMAT_B8G8R8A8_UNORM, display->images, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR))
	{
		std::printf("[frontend] renderer: %s\n", renderer.error().c_str());
		renderer.Shutdown();
		display->Destroy();
		delete display;
		delete fonts;
		scePadClose(pad);
		return {};
	}

	CoverService* covers = new CoverService();
	CoverConfig cc;
	cc.manual_dir = paths.covers_dir;
	cc.cache_dir = paths.cache_dir;
	cc.url_template = kCoverUrl;
	cc.allow_download = paths.allow_download;
	cc.download_usb_only = true; // vk-285-110: the rest came from the prefetch, before the jailbreak
	covers->Start(games, fonts, cc, [](const std::string& url, std::vector<uint8_t>& out) { return g_http.Get(url, out); });

	// The key sounds (vk-285-47). Without an audio port the shelf is simply quiet.
	Mixer* mixer = nullptr;
	AudioOut* audio = nullptr;
	if (paths.sound)
	{
		const double s0 = Now();
		mixer = new Mixer();
		mixer->Build();
		audio = new AudioOut();
		if (!audio->Start(mixer))
		{
			delete audio;
			audio = nullptr;
			delete mixer;
			mixer = nullptr;
		}
		std::printf("[frontend] sound %s (%.0f ms)\n", audio ? "on" : "off", (Now() - s0) * 1000.0);
	}
	else
		std::printf("[frontend] sound off (nomenusound)\n");

	App app;
	AppConfig acfg;
	acfg.build_tag = build_tag ? build_tag : "";
	acfg.test_build = paths.test_build;   // test build 1: the TESTING watermark
	acfg.build_label = paths.build_label;
	acfg.test_note = paths.test_note; // vk-285-105
	acfg.preselect = preselect;
	acfg.sound = mixer;
	// vk-285-114: the options sheet (Square) edits the files the settings page does, and logs as it does.
	acfg.options.settings_dir = paths.settings_dir;
	acfg.options.gs_ini = paths.gs_ini;
	acfg.options.patches_dir = paths.patches_dir;
	acfg.options.memcards_dir = paths.memcards_dir;
	acfg.options.presets.assign(reinterpret_cast<const char*>(fe_presets), static_cast<size_t>(fe_presets_end - fe_presets));
	{
		const std::string log_path = paths.settings_log;
		acfg.options.log = [log_path](const std::string& line) { AppendSettingsLog(log_path, line); };
	}
	acfg.refresh_game = [paths](GameInfo& g) {
		g.badges.clear();
		ReadBadges(g, paths.settings_dir, paths.gs_ini, paths.patches_dir);
	};
	bool ok = app.Init(&renderer, fonts, games, covers, acfg);
	std::printf("[frontend] up in %.0f ms (%s)\n", (Now() - t0) * 1000.0, ok ? "ok" : renderer.error().c_str());
	std::fflush(stdout);
	// vk-285-50: the QR tile's address, looked up again every few seconds in case the IP changes.
	double web_checked = -1e9;
	auto refresh_web = [&](double now) {
		if (!g_web || now - web_checked < 3.0)
			return;
		web_checked = now;
		std::string url, shown;
		g_web->Address(url, shown);
		app.SetWebUrl(url, shown);
	};

	FrameDesc frame;
	double last_t = Now(), report_t = last_t;
	unsigned frames = 0, slot = 0;
	double worst = 0;
	bool first_shown = false;
	while (ok && !app.Done())
	{
		const double now = Now();
		refresh_web(now);
		const double dt = now - last_t;
		last_t = now;
		worst = std::max(worst, dt);
		PadData pd;
		std::memset(&pd, 0, sizeof(pd));
		pd.lx = pd.ly = 128;
		Input in;
		if (scePadReadState(pad, &pd) == 0)
		{
			in.left = (pd.buttons & kPadLeft) || pd.lx < 48;
			in.right = (pd.buttons & kPadRight) || pd.lx > 208;
			in.cross = pd.buttons & kPadCross;
			in.options = pd.buttons & kPadOptions;
			in.l1 = pd.buttons & kPadL1;
			in.r1 = pd.buttons & kPadR1;
			in.up = (pd.buttons & kPadUp) || pd.ly < 48; // vk-285-114
			in.down = (pd.buttons & kPadDown) || pd.ly > 208;
			in.square = pd.buttons & kPadSquare;
			in.triangle = pd.buttons & kPadTriangle;
			in.circle = pd.buttons & kPadCircle;
			in.l2 = (pd.buttons & kPadL2) || pd.l2 > 160; // vk-285-116
			in.r2 = (pd.buttons & kPadR2) || pd.r2 > 160;
		}
		app.Update(dt, in);
		app.Build(frame, Clock());
		if (!renderer.WaitForSlot())
		{
			ok = false;
			break;
		}
		uint32_t index = 0;
		VkResult r = display->vk.vkAcquireNextImageKHR(display->device, display->swapchain, UINT64_MAX,
			display->acquired[slot], VK_NULL_HANDLE, &index);
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
		{
			std::printf("[frontend] vkAcquireNextImageKHR: %d\n", static_cast<int>(r));
			ok = false;
			break;
		}
		if (!renderer.Render(frame, index, display->acquired[slot], display->rendered[slot]))
		{
			std::printf("[frontend] render: %s\n", renderer.error().c_str());
			ok = false;
			break;
		}
		VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &display->rendered[slot];
		pi.swapchainCount = 1;
		pi.pSwapchains = &display->swapchain;
		pi.pImageIndices = &index;
		r = display->vk.vkQueuePresentKHR(display->queue, &pi);
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
		{
			std::printf("[frontend] vkQueuePresentKHR: %d\n", static_cast<int>(r));
			ok = false;
			break;
		}
		slot ^= 1;
		frames++;
		if (!first_shown)
		{
			first_shown = true;
			// vk-285-53: the launch screen (sce_sys/pic1.dds) covers the app until it is hidden; with
			// it, 52 stayed on the launch screen with the shelf running behind it. Hidden once the
			// shelf's first frame is up, so the screen goes from one straight to the other.
			const int hide = orbis_hide_splash();
			std::printf("[frontend] first frame on screen %.0f ms after start; launch screen hidden (%d)\n", (Now() - t0) * 1000.0,
				hide);
			std::fflush(stdout);
		}
		if (now - report_t >= 5.0)
		{
			std::printf("[frontend] %u frames in %.1f s, worst %.1f ms\n", frames, now - report_t, worst * 1000.0);
			std::fflush(stdout);
			frames = 0;
			worst = 0;
			report_t = now;
		}
	}

	const int chosen = app.Chosen();
	const bool picked = ok && app.Done();
	renderer.WaitIdle();
	app.Shutdown();
	// The worker may be inside a download: stop it from starting another, fail the one in flight,
	// give it a moment, then leave it behind if need be.
	covers->RequestStop();
	g_http.Abort();
	const bool stopped = covers->Stop(1500);
	renderer.Shutdown();
	display->Destroy();
	delete display;
	if (stopped)
	{
		delete covers;
		delete fonts;
		g_http.Term();
	}
	else
		std::printf("[frontend] a cover download is still running; leaving it to finish\n");
	scePadClose(pad);
	if (audio)
	{
		// Let the launch sound ring out: it lasts about 0.8 s, the shelf closes 0.6 s after the press.
		const double s0 = Now();
		while (!mixer->Idle() && Now() - s0 < 0.4)
			usleep(5000);
		audio->Stop();
		delete audio;
		delete mixer;
	}

	if (!picked)
	{
		std::printf("[frontend] stopped without a pick; the plain list takes over\n");
		std::fflush(stdout);
		return {};
	}
	*ran = true;
	const GameInfo& g = games[static_cast<size_t>(chosen)];
	WriteLastGame(paths.top_dir, g.file);
	std::printf("[frontend] picked %s (%s)\n", g.file.c_str(), g.serial.c_str());
	std::fflush(stdout);
	return g.path;
}

// ---- Configurable data root: directory browser (orbis_pick_root_dir) ----
//
// Called from main-boot.cpp when the active OrbisRoot() directory does not exist.
// Opens a Vulkan display, renders a simple list UI and lets the user navigate the
// filesystem with the D-pad, enter folders with Cross and confirm with Triangle.
// Writes the chosen path via OrbisSetRoot() and returns true. Returns false when
// the user cancels (Circle at the top level) or the display cannot be opened.

namespace
{
// ---- helpers ---------------------------------------------------------------

struct RootCandidate
{
	std::string path;
	int games = 0;   // disc images in <path>/games/ (or in path itself)
	bool has_bios = false;
};

static bool DirExists(const std::string& p)
{
	struct stat st = {};
	return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Count .iso/.chd/.cso/.zso files directly inside `dir` (-1 = can't open).
static int CountDirImages(const std::string& dir)
{
	DIR* d = opendir(dir.c_str());
	if (!d)
		return -1;
	int n = 0;
	while (const dirent* e = readdir(d))
		if (IsDiscImageName(e->d_name))
			n++;
	closedir(d);
	return n;
}

static RootCandidate MakeCandidate(const std::string& path)
{
	RootCandidate c;
	c.path = path;
	// games in <path>/games/ or, for the old flat layout, directly in <path>
	const int in_games = CountDirImages(path + "/games");
	c.games = (in_games >= 0) ? in_games : std::max(0, CountDirImages(path));
	c.has_bios = DirExists(path + "/bios");
	return c;
}

// Enumerate top-level candidate roots: internal /data, each /mnt/usb0-7, /mnt/ext0/1.
static std::vector<std::string> TopLevelRoots()
{
	std::vector<std::string> roots;
	// Internal storage
	if (DirExists("/data"))
		roots.push_back("/data");
	// USB drives (usb0-7)
	for (int i = 0; i < 8; i++)
	{
		char p[16];
		std::snprintf(p, sizeof(p), "/mnt/usb%d", i);
		if (DirExists(p))
			roots.push_back(p);
	}
	// External M.2 / USB HDD (ext0, ext1)
	for (int i = 0; i <= 1; i++)
	{
		char p[16];
		std::snprintf(p, sizeof(p), "/mnt/ext%d", i);
		if (DirExists(p))
			roots.push_back(p);
	}
	return roots;
}

// List the subdirectories of `dir` (no hidden entries).
static std::vector<std::string> ListSubdirs(const std::string& dir)
{
	std::vector<std::string> out;
	DIR* d = opendir(dir.c_str());
	if (!d)
		return out;
	while (const dirent* e = readdir(d))
	{
		if (e->d_name[0] == '.')
			continue;
		const std::string full = dir + "/" + e->d_name;
		if (DirExists(full))
			out.push_back(full);
	}
	closedir(d);
	std::sort(out.begin(), out.end());
	return out;
}

// ---- rendering helpers -----------------------------------------------------

static constexpr uint32_t kColorWhite  = 0xFFFFFFFF;
static constexpr uint32_t kColorDim    = 0xFFB0A8CC;
static constexpr uint32_t kColorAccent = 0xFFFFAA66;
static constexpr uint32_t kColorBg     = 0xFF1A1630;
static constexpr uint32_t kColorSel    = 0x33FFFFFF;
static constexpr uint32_t kColorGreen  = 0xFF88EEB0;
static constexpr uint32_t kColorWarn   = 0xFF66AAFF;

// Screen is always 3840x2160; UI is laid out in logical pixels at 1/2 scale (1920x1080 grid).
static constexpr float kScale  = 2.0f;
static constexpr float kW      = 1920.0f;
static constexpr float kH      = 1080.0f;
static constexpr float kMargin = 60.0f;
static constexpr float kRowH   = 64.0f;
static constexpr float kPxBig  = 36.0f;
static constexpr float kPxMid  = 26.0f;
static constexpr float kPxSm   = 20.0f;

static void AddBg(std::vector<UiVertex>& ui)
{
	Fonts::AddRoundedRect(ui, 0, 0, kW * kScale, kH * kScale, 0, kColorBg);
}

// `sel` row is highlighted; `rows_start` = y of first row, `sel_idx` is 0-based within visible.
static void AddSelHighlight(std::vector<UiVertex>& ui, float rows_start, int sel_idx)
{
	const float y = (rows_start + sel_idx * kRowH - 4) * kScale;
	Fonts::AddRoundedRect(ui, kMargin * kScale, y,
	                      (kW - kMargin * 2) * kScale, (kRowH - 4) * kScale,
	                      12.0f * kScale, kColorSel);
}

struct TextRow
{
	std::string label;
	std::string sub;  // secondary info (games count, bios status)
	bool is_back = false;
};

static std::vector<TextRow> BuildRows(const std::vector<std::string>& entries,
                                      const std::string& current_dir,
                                      bool can_go_up)
{
	std::vector<TextRow> rows;
	if (can_go_up)
	{
		TextRow r;
		r.label = "..  (go up)";
		r.is_back = true;
		rows.push_back(r);
	}
	for (const std::string& p : entries)
	{
		TextRow r;
		// Show just the last path component
		const size_t sl = p.rfind('/');
		r.label = (sl != std::string::npos) ? p.substr(sl + 1) : p;
		// Quick stats
		const RootCandidate c = MakeCandidate(p);
		if (c.games > 0 || c.has_bios)
		{
			r.sub = "";
			if (c.games > 0)
				r.sub += std::to_string(c.games) + " game(s)";
			if (c.has_bios)
				r.sub += (r.sub.empty() ? "" : "  ·  ") + std::string("BIOS found");
		}
		rows.push_back(r);
	}
	return rows;
}

// ---- main browser loop -----------------------------------------------------

// Maximum visible rows on screen at once.
static constexpr int kMaxVisible = 12;

// Renders one frame of the directory browser. Returns false if the display fails.
static void RenderBrowserFrame(Renderer& renderer, const Fonts& fonts, Display& display,
                               const std::string& current_dir,
                               const std::vector<TextRow>& rows, int sel,
                               int scroll, int slot_idx,
                               const std::string& warning)
{
	FrameDesc f;
	f.fade = 1.0f;
	std::vector<UiVertex>& ui = f.ui;

	AddBg(ui);

	// Title
	const std::string title = Tr(Str::PickRootTitle);
	fonts.AddText(ui, title.c_str(),
	              kMargin * kScale,
	              (kMargin + kPxBig) * kScale,
	              kPxBig * kScale, kColorAccent, 0.35f);

	// Current path
	char cur_buf[520];
	std::snprintf(cur_buf, sizeof(cur_buf), Tr(Str::PickRootCurrent), current_dir.c_str());
	fonts.AddText(ui, cur_buf,
	              kMargin * kScale,
	              (kMargin + kPxBig + kPxSm + 8) * kScale,
	              kPxSm * kScale, kColorDim);

	// Warning (e.g. previous root not found)
	if (!warning.empty())
	{
		fonts.AddText(ui, warning.c_str(),
		              kMargin * kScale,
		              (kMargin + kPxBig + kPxSm * 2 + 20) * kScale,
		              kPxSm * kScale, kColorWarn);
	}

	const float rows_start = kMargin + kPxBig + kPxSm * 2 + 56;

	// Selection highlight
	const int vis_sel = sel - scroll;
	AddSelHighlight(ui, rows_start, vis_sel);

	// Rows
	const int visible = std::min(static_cast<int>(rows.size()), kMaxVisible);
	for (int i = 0; i < visible; i++)
	{
		const TextRow& row = rows[static_cast<size_t>(scroll + i)];
		const float base_y = (rows_start + i * kRowH + kPxMid) * kScale;
		const uint32_t col = (scroll + i == sel) ? kColorWhite : kColorDim;

		fonts.AddText(ui, row.label.c_str(),
		              (kMargin + 16) * kScale, base_y,
		              kPxMid * kScale, col);

		if (!row.sub.empty())
		{
			fonts.AddText(ui, row.sub.c_str(),
			              (kMargin + 16 + 420) * kScale, base_y,
			              kPxSm * kScale, kColorGreen);
		}
	}

	// Hint bar at the bottom
	const float hint_y = (kH - kMargin) * kScale;
	fonts.AddText(ui, Tr(Str::PickRootHint),
	              kMargin * kScale, hint_y,
	              kPxSm * kScale, kColorDim);

	// Acquire swapchain image and render
	uint32_t img_idx = 0;
	if (vkAcquireNextImageKHR(display.device, display.swapchain, UINT64_MAX,
	                          display.acquired[slot_idx], VK_NULL_HANDLE, &img_idx) != VK_SUCCESS)
		return;

	renderer.Render(f, img_idx, display.acquired[slot_idx], display.rendered[slot_idx]);

	VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores    = &display.rendered[slot_idx];
	pi.swapchainCount     = 1;
	pi.pSwapchains        = &display.swapchain;
	pi.pImageIndices      = &img_idx;
	vkQueuePresentKHR(display.queue, &pi);
}

} // namespace (anonymous)

// Public entry point called from main-boot.cpp (declared extern bool orbis_pick_root_dir()).
bool orbis_pick_root_dir()
{
	std::printf("[rootpick] opening directory browser\n");
	std::fflush(stdout);

	// --- Init display & renderer ------------------------------------------------
	Display* display = new Display();
	if (!display->Init())
	{
		std::printf("[rootpick] display init failed\n");
		display->Destroy();
		delete display;
		return false;
	}

	Fonts* fonts = new Fonts();
	Renderer renderer;
	if (!fonts->Init(fe_font_text, static_cast<size_t>(fe_font_text_end - fe_font_text),
	                 fe_font_icons, static_cast<size_t>(fe_font_icons_end - fe_font_icons)) ||
	    !renderer.Init(&display->vk, display->pd, display->device, display->qf, display->queue,
	                   display->extent.width, display->extent.height,
	                   VK_FORMAT_B8G8R8A8_UNORM, display->images,
	                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR))
	{
		std::printf("[rootpick] renderer init failed: %s\n", renderer.error().c_str());
		renderer.Shutdown();
		display->Destroy();
		delete display;
		delete fonts;
		return false;
	}

	// Upload font atlas
	Texture* atlas = renderer.CreateTexture(
	    static_cast<uint32_t>(fonts->AtlasWidth()),
	    static_cast<uint32_t>(fonts->AtlasHeight()),
	    VK_FORMAT_R8_UNORM, fonts->AtlasPixels().data());
	if (atlas)
		renderer.SetAtlas(atlas);

	// --- Controller input -------------------------------------------------------
	int32_t user = -1;
	(void)sceUserServiceInitialize(nullptr);
	(void)sceUserServiceGetInitialUser(&user);
	(void)scePadInit();
	const int pad = user >= 0 ? scePadOpen(user, 0, 0, nullptr) : -1;

	// --- Build warning string ---------------------------------------------------
	const std::string& prev_root = OrbisRoot();
	std::string warning;
	{
		struct stat st = {};
		if (stat(prev_root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
		{
			char buf[520];
			std::snprintf(buf, sizeof(buf), Tr(Str::PickRootNotFound), prev_root.c_str());
			warning = buf;
		}
	}

	// --- Navigation state -------------------------------------------------------
	// Navigation stack: each level holds the directory being browsed.
	std::vector<std::string> dir_stack;        // stack of directories entered
	std::string current_dir = "/";             // the directory displayed now
	std::vector<std::string> entries;          // subdirs of current_dir
	std::vector<TextRow> rows;
	int sel    = 0;
	int scroll = 0;

	auto refresh = [&]() {
		if (dir_stack.empty())
		{
			// Top level: enumerate candidate mount points
			entries = TopLevelRoots();
			current_dir = "/";
		}
		else
		{
			entries = ListSubdirs(current_dir);
			if (entries.empty() && dir_stack.empty())
				entries = TopLevelRoots();
		}
		rows = BuildRows(entries, current_dir, !dir_stack.empty());
		if (sel >= static_cast<int>(rows.size()))
			sel = std::max(0, static_cast<int>(rows.size()) - 1);
		scroll = 0;
	};
	refresh();

	// Pad state for edge-detection
	struct PadSnapshot { uint32_t buttons = 0; };
	PadSnapshot prev_pad{};
	bool confirmed = false;
	bool cancelled = false;
	std::string chosen_path;

	// --- Main loop --------------------------------------------------------------
	int slot_idx = 0;
	while (!confirmed && !cancelled)
	{
		// Input
		if (pad >= 0)
		{
			ScePadData pd{};
			scePadReadState(pad, &pd);
			const uint32_t pressed = pd.buttons & ~prev_pad.buttons;
			prev_pad.buttons = pd.buttons;

			const int total = static_cast<int>(rows.size());

			if (pressed & SCE_PAD_BUTTON_UP)
			{
				if (sel > 0) sel--;
				else sel = std::max(0, total - 1);
			}
			if (pressed & SCE_PAD_BUTTON_DOWN)
			{
				if (sel < total - 1) sel++;
				else sel = 0;
			}
			// Adjust scroll window
			if (sel < scroll)
				scroll = sel;
			else if (sel >= scroll + kMaxVisible)
				scroll = sel - kMaxVisible + 1;

			if (pressed & SCE_PAD_BUTTON_CROSS)
			{
				// Enter directory (or go up)
				if (!rows.empty())
				{
					if (rows[static_cast<size_t>(sel)].is_back)
					{
						// Go up
						if (!dir_stack.empty())
						{
							current_dir = dir_stack.back();
							dir_stack.pop_back();
							sel = 0;
							refresh();
						}
					}
					else
					{
						// Determine which entry this is
						const int entry_idx = sel - (dir_stack.empty() ? 0 : 1);
						if (entry_idx >= 0 && entry_idx < static_cast<int>(entries.size()))
						{
							dir_stack.push_back(current_dir);
							current_dir = entries[static_cast<size_t>(entry_idx)];
							sel = 0;
							refresh();
						}
					}
				}
			}
			if (pressed & SCE_PAD_BUTTON_TRIANGLE)
			{
				// Confirm current_dir as the new root
				if (current_dir != "/")
				{
					chosen_path = current_dir;
					confirmed = true;
				}
			}
			if (pressed & SCE_PAD_BUTTON_CIRCLE)
			{
				if (!dir_stack.empty())
				{
					// Go up instead of cancelling
					current_dir = dir_stack.back();
					dir_stack.pop_back();
					sel = 0;
					refresh();
				}
				else
				{
					cancelled = true;
				}
			}
		}

		// Render
		RenderBrowserFrame(renderer, *fonts, *display, current_dir, rows, sel, scroll, slot_idx, warning);
		slot_idx ^= 1;
	}

	// --- Teardown ---------------------------------------------------------------
	renderer.WaitIdle();
	if (atlas)
		renderer.DestroyTexture(atlas);
	renderer.Shutdown();
	display->Destroy();
	delete display;
	delete fonts;

	if (pad >= 0)
		scePadClose(pad);

	if (!confirmed || chosen_path.empty())
	{
		std::printf("[rootpick] cancelled\n");
		std::fflush(stdout);
		return false;
	}

	std::printf("[rootpick] chosen: %s\n", chosen_path.c_str());
	std::fflush(stdout);

	if (!OrbisSetRoot(chosen_path))
	{
		std::printf("[rootpick] OrbisSetRoot failed (can't write /data/ps5sx2_root.txt)\n");
		std::fflush(stdout);
		return false;
	}
	return true;
}
