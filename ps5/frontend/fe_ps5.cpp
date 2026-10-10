// PS5 port frontend: the console side. The shelf runs on its own Vulkan device (the driver linked
// into the eboot) with a VK_KHR_display swapchain on VideoOut, reads the DualSense, plays its key
// sounds on an audio port of its own, and downloads missing covers over HTTPS with the console's
// own libSceHttp2 (before the jailbreak) and HD texture packs with our own (fe_https.h). Everything
// is torn down again before PCSX2 opens its device, VideoOut and audio.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_ps5.h"

#include "fe_app.h"
#include "fe_covers.h"
#include "fe_games.h"
#include "fe_i18n.h"
#include "fe_renderer.h"
#include "fe_sound.h"
#include "fe_text.h"
#include "fe_https.h"
#include "fe_texpacks.h"
#include "fe_patchdl.h" // 2026-10-08
#include "fe_vk.h"
#include "fe_web.h"
#include "ps5/coreorbis/orbis-shims/ProsperoNotify.h" // 2026-10-05: the texture packs' popup
#ifdef PS5SX2_ACHIEVEMENTS
#include "ps5/coreorbis/orbis-shims/ProsperoAchievements.h"
#include "pcsx2/Achievements.h"
#endif
#include "third_party/qrcodegen/qrcodegen.h" // vk-285-113: orbis_web_qr

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <ctime>
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/mount.h> // statfs: the texture packs' free space
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
int sceKernelGetModuleList(int32_t* handles, size_t count, size_t* actual);
int sceKernelGetModuleInfo(int32_t handle, void* info);
int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags, void* opt, int* res);
int sceKernelDlsym(int handle, const char* symbol, void** addrp);
// 2026-10-05: where the texture packs' threads run (libkernel; orbis-shims/orbis_eeprof.cpp uses the same calls).
int scePthreadGetaffinity(pthread_t thread, unsigned long long* mask);
int scePthreadSetaffinity(pthread_t thread, unsigned long long mask);
int scePthreadGetprio(pthread_t thread, int* prio);
int sceKernelGetCurrentCpu(void);
#ifdef PS5SX2_IME_IMPORT
int sceImeDialogInit(void* param, void* extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(void* result);
int sceImeDialogTerm(void);
#endif

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

// main-boot.cpp (2026-10-05): the folder the emulator would take a game's texture pack from (a USB drive, the settings
// page's folder, /data), for the HD texture pack row.
std::string OrbisTexturesGameDir(const std::string& serial, std::string& how);

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

// ---- 2026-10-05 (AI-assisted): the HD texture packs' downloads (fe_texpacks.h) -----------------------------------------
// archive.org's file list and the packs, in ranges of up to 32 MB that the manager asks for one after another, over our
// own HTTPS (fe_https.h). The console's libSceHttp2 can't do it after the jailbreak: libSceSsl can't reach its certificate
// store there, and with roots handed to it, it still can't follow archive.org's cross-signed chain (texnet3-5 on the
// console). mbedTLS checks each server's chain against Mozilla's roots, and its name; texnet6 fetched the list and a whole
// pack through the download redirect this way on the console. Needs proper testing in the app.
struct TexHttp
{
	explicit TexHttp(const char* name = "pcsx2-texpacks", const char* tag = "texpacks")
		: pool_name(name)
		, log_tag(tag)
	{
	}
	const char* pool_name; // 2026-10-08: a second client for the online patches (its own pool and log tag)
	const char* log_tag;
	std::mutex mutex; // the client's setup
	int pool = -1;
	std::unique_ptr<HttpsClient> client;

	// The client, made once the console is online; null (and tried again next time) when it isn't.
	HttpsClient* Client()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (client)
			return client.get();
		// NetCtl stays up once started (the covers and DEV9's DNS lookup use it too): no sceNetCtlTerm here.
		(void)sceNetCtlInit();
		int state[4] = {-1, 0, 0, 0};
		const int gs = sceNetCtlGetState(state);
		if (gs == 0 && state[0] >= 0 && state[0] < 3)
		{
			std::printf("[%s] the console is not connected (netctl state %d)\n", log_tag, state[0]);
			std::fflush(stdout);
			return nullptr;
		}
		sceNetInit();
		if (pool < 0)
			pool = sceNetPoolCreate(pool_name, 64 * 1024, 0);
		if (pool < 0)
		{
			std::printf("[%s] no libnet pool (%#x)\n", log_tag, static_cast<unsigned>(pool));
			std::fflush(stdout);
			return nullptr;
		}
		const char* const tag = log_tag;
		auto made = std::make_unique<HttpsClient>(MakeConsoleHttpsPlatform(pool, [tag](const std::string& line) {
			std::printf("[%s] %s\n", tag, line.c_str());
			std::fflush(stdout);
		}));
		std::string error;
		if (!made->Init(error))
			return nullptr; // the client said why
		client = std::move(made);
		return client.get();
	}

	void Term()
	{
		std::lock_guard<std::mutex> lock(mutex);
		client.reset();
		if (pool >= 0)
			sceNetPoolDestroy(pool);
		pool = -1;
	}

	// Fails the request in flight (another thread: Cancel, the shelf's end).
	void Abort()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (client)
			client->Abort();
	}

	// One GET: the whole answer (ranged false) or bytes [offset, offset + length). The HTTP status, or < 0 (-3: the
	// server ignored a range after the file's start, and nothing was delivered).
	int Get(const std::string& url, bool ranged, uint64_t offset, uint64_t length, const std::function<bool(const void*, size_t)>& sink)
	{
		HttpsClient* c = Client();
		if (!c)
			return -1;
		// A 32 MB range: 15 minutes at most; the list: a minute.
		return c->Get(url, ranged, offset, length, sink, ranged ? 900 * 1000 : 60 * 1000);
	}
};
TexHttp g_texhttp;
TexHttp g_patchhttp("pcsx2-patches", "patches"); // 2026-10-08: the online patches' own client (fe_patchdl.h)
TexHttp g_coverhttp("pcsx2-covers", "covers");   // 2026-10-10: the shelf's cover downloads (CoverGet)

// 2026-10-10 (AI-assisted): the shelf's cover downloads, after the jailbreak, over our own HTTPS. libSceHttp2 fails there
// (0x8095F00C; texnet6 fetched raw.githubusercontent.com with this client on the console), so the shelf's first download
// failed, the network counted as down for the session, and NFS and USB games waited for the prefetch before the
// jailbreak: 30 s a start, about 85 covers. Needs proper testing on the console.
int CoverGet(const std::string& url, std::vector<uint8_t>& out)
{
	HttpsClient* c = g_coverhttp.Client();
	if (!c)
		return -1;
	out.clear();
	return c->Get(url, false, 0, 0, [&out](const void* p, size_t n) {
		const uint8_t* b = static_cast<const uint8_t*>(p);
		out.insert(out.end(), b, b + n);
		return out.size() <= (16u << 20);
	}, 20 * 1000);
}

// The free space of a folder's disk for the texture packs, or UINT64_MAX when the console won't say (the manager then
// skips its room check; a full disk still fails the writes, which it reports). Two ways crashed the app on the console:
// statvfs (pr9f: libSceLibcInternal's jumps through a libkernel import an app doesn't get; klog: SIGSEGV at an address
// with nothing there) and the SDK libc's statfs stub (pr9g: a system call from outside libkernel; no report at all).
// This asks libkernel's own _fstatfs, looked up by name in the loaded modules as orbis-shims/ProsperoKbdMouse.cpp does,
// so a console without it gets a null and an "unknown", never a jump to nowhere. Needs testing on the console.
// Where a thread runs: "cpu N, priority P, cpus MASK".
std::string ThreadPlace()
{
	unsigned long long mask = 0;
	int prio = -1;
	const int got = scePthreadGetaffinity(pthread_self(), &mask);
	scePthreadGetprio(pthread_self(), &prio);
	char buf[96];
	std::snprintf(buf, sizeof(buf), "cpu %d, priority %d, cpus %s%llx", sceKernelGetCurrentCpu(), prio, got == 0 ? "0x" : "? ", mask);
	return buf;
}

// The start of each texture-pack thread (TexturePackPlatform::thread_start). A thread takes the CPUs of the thread that
// made it, and the console's first unpack ran some 25 times slower than the same work in a payload (pr9h: 13,200 files
// in over 30 minutes; a payload wrote about 190 files a second): if the shelf's thread is held to one CPU, the
// texture packs' threads share it with a thread that waits for every frame. So they get all 13 of the title's CPUs (0 to
// 12, as measured for the RPCS3 port). Logged, with what the system answered. Needs testing on the console.
void TexturePackThreadStart(const char* role)
{
	constexpr unsigned long long kTitleCpus = 0x1fffull;
	unsigned long long mask = 0;
	const std::string before = ThreadPlace();
	std::string what;
	if (scePthreadGetaffinity(pthread_self(), &mask) == 0 && mask != 0 && (mask & kTitleCpus) != kTitleCpus)
	{
		const int rc = scePthreadSetaffinity(pthread_self(), kTitleCpus);
		char buf[64];
		std::snprintf(buf, sizeof(buf), "; all the title's CPUs: %s", rc == 0 ? "yes" : "refused");
		what = buf;
		if (rc != 0)
		{
			std::snprintf(buf, sizeof(buf), " (%#x)", static_cast<unsigned>(rc));
			what += buf;
		}
	}
	std::printf("[texpacks] %s thread: %s%s\n", role, before.c_str(), what.c_str());
	std::fflush(stdout);
}

uint64_t TexturePackFreeBytes(const std::string& dir)
{
	using FstatfsFn = int (*)(int, struct statfs*);
	static const FstatfsFn fstatfs_fn = [] {
		int32_t handles[256];
		size_t count = 0;
		FstatfsFn found = nullptr;
		if (sceKernelGetModuleList(handles, 256, &count) == 0)
		{
			for (size_t i = 0; i < count && i < 256 && !found; i++)
			{
				void* address = nullptr;
				if (sceKernelDlsym(handles[i], "_fstatfs", &address) == 0 && address)
					found = reinterpret_cast<FstatfsFn>(address);
			}
		}
		std::printf("[texpacks] free space: %s\n", found ? "libkernel's _fstatfs" : "no _fstatfs in this process (no room check)");
		std::fflush(stdout);
		return found;
	}();
	if (!fstatfs_fn)
		return UINT64_MAX;
	const int fd = open(dir.c_str(), O_RDONLY);
	if (fd < 0)
		return UINT64_MAX;
	// Room to spare in case the kernel's struct is bigger than the header's.
	union
	{
		struct statfs s;
		char room[sizeof(struct statfs) + 1024];
	} v;
	std::memset(&v, 0, sizeof(v));
	const int rc = fstatfs_fn(fd, &v.s);
	close(fd);
	if (rc != 0 || v.s.f_bsize == 0 || v.s.f_bavail < 0)
		return UINT64_MAX;
	const uint64_t bytes = static_cast<uint64_t>(v.s.f_bavail) * v.s.f_bsize;
	static std::atomic<bool> said{false};
	if (!said.exchange(true))
	{
		std::printf("[texpacks] free space in %s: %llu MB\n", dir.c_str(), static_cast<unsigned long long>(bytes >> 20));
		std::fflush(stdout);
	}
	return bytes;
}


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
	return path.compare(0, 8, "/mnt/usb") == 0 || path.compare(0, 8, "/mnt/ext") == 0; // 2026-10-08: and the ext/M.2 drives
}

// cache/usb-games.txt, one "<serial>\t<stem>\t<title>" line per USB game: before the jailbreak,
// where covers download, the app may not see the drives, so the covers of the games found on them
// after the jailbreak are fetched at the next start from this list. vk-285-156: NFS shares' games too
// (mounted only after the jailbreak; testers: their covers never came).
void WriteUsbList(const std::string& path, const std::vector<GameInfo>& games)
{
	if (path.empty())
		return;
	std::string text;
	for (const GameInfo& g : games)
		if ((OnUsb(g.path) || OnNetworkShare(g.path)) && !g.serial.empty())
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

// 2026-10-05 (AI-assisted): the PS5's own keyboard for the account panel (libSceImeDialog), swordpdf: "use the shell
// keyboard". The parameter block is the PS4 SDK's SceImeDialogParam as shadPS4's reimplementation lays it out (96 bytes);
// its checks there: a password needs the BasicLatin type, the reserved bytes must be zero, the position is in 1920x1080.
// This process may not load the library itself: firmware 11.40 refuses libSceNotification, libSceKeyboard and libSceMouse
// with 0x80020063. So the library is used when it is already in the process (the list of loaded modules is logged once,
// to boot.log), or when the load is allowed; otherwise TextEntryService::open fails and the panel shows its own keyboard.
// With PS5SX2_IME_IMPORT the functions are imported instead (the system loads the library at the app's start, if it will):
// an experiment, since an app whose import is refused doesn't start.
namespace
{
struct ImeDialogParam
{
	int32_t user_id;
	uint32_t type; // 0 default, 1 basic latin, 2 URL, 3 mail, 4 number
	uint64_t supported_languages;
	uint32_t enter_label;
	uint32_t input_method;
	void* filter;
	uint32_t option;
	uint32_t max_text_length;
	char16_t* input_text_buffer;
	float posx, posy;
	uint32_t horizontal_alignment, vertical_alignment; // 0 left/top, 1 centre, 2 right/bottom
	const char16_t* placeholder;
	const char16_t* title;
	int8_t reserved[16];
};
static_assert(sizeof(ImeDialogParam) == 96, "SceImeDialogParam is 96 bytes");
struct ImeDialogResult
{
	uint32_t end_status; // 0 OK, 1 cancelled, 2 aborted
	// The SDKs disagree on the size of what follows (12 bytes, or shadPS4's 12 ints); room for either, so a library
	// that writes all of it can't write past this on the stack.
	int32_t reserved[16];
};
constexpr uint32_t kImeTypeBasicLatin = 1;
constexpr uint32_t kImeOptionNoAutoCapitalization = 0x2, kImeOptionPassword = 0x4, kImeOptionNoLearning = 0x20;
using ImeInitFn = int (*)(ImeDialogParam*, void*);
using ImeStatusFn = int (*)();
using ImeResultFn = int (*)(ImeDialogResult*);
using ImeTermFn = int (*)();

struct SystemKeyboard
{
	bool tried = false;
	ImeInitFn init = nullptr;
	ImeStatusFn status = nullptr;
	ImeResultFn result = nullptr;
	ImeTermFn term = nullptr;
	bool running = false;
	int32_t user = -1;
	char16_t buffer[512 + 1] = {};
	char16_t title[96] = {};
};
SystemKeyboard g_ime;

std::u16string Utf8To16(const std::string& s)
{
	std::u16string out;
	for (size_t i = 0; i < s.size();)
	{
		const unsigned char c = static_cast<unsigned char>(s[i]);
		uint32_t cp = c;
		size_t n = 1;
		if (c >= 0xF0 && i + 3 < s.size())
			cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu), n = 4;
		else if (c >= 0xE0 && i + 2 < s.size())
			cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu), n = 3;
		else if (c >= 0xC0 && i + 1 < s.size())
			cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu), n = 2;
		i += n;
		if (cp >= 0x10000)
		{
			cp -= 0x10000;
			out += static_cast<char16_t>(0xD800 + (cp >> 10));
			out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
		}
		else
			out += static_cast<char16_t>(cp);
	}
	return out;
}

std::string Utf16To8(const char16_t* s)
{
	std::string out;
	for (size_t i = 0; s[i]; i++)
	{
		uint32_t cp = s[i];
		if (cp >= 0xD800 && cp < 0xDC00 && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
			cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
		if (cp < 0x80)
			out += static_cast<char>(cp);
		else if (cp < 0x800)
			out += static_cast<char>(0xC0 | (cp >> 6)), out += static_cast<char>(0x80 | (cp & 0x3F));
		else if (cp < 0x10000)
			out += static_cast<char>(0xE0 | (cp >> 12)), out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
				out += static_cast<char>(0x80 | (cp & 0x3F));
		else
			out += static_cast<char>(0xF0 | (cp >> 18)), out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)),
				out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
	}
	return out;
}

// The modules in this process, by name (logged once), and the handle of `want` among them (-1 if absent).
int LoadedModule(const char* want)
{
	struct ModuleInfo
	{
		uint64_t size;
		char name[256];
		struct
		{
			uint64_t address;
			uint32_t size;
			int32_t prot;
		} segments[4];
		uint32_t num_segments;
		uint8_t fingerprint[20];
	};
	int32_t handles[256];
	size_t count = 0;
	const int rc = sceKernelGetModuleList(handles, 256, &count);
	static bool s_logged = false;
	std::string names;
	int found = -1;
	for (size_t i = 0; rc == 0 && i < count && i < 256; i++)
	{
		ModuleInfo info = {};
		info.size = sizeof(info);
		if (sceKernelGetModuleInfo(handles[i], &info) != 0)
			continue;
		info.name[sizeof(info.name) - 1] = '\0';
		if (!names.empty())
			names += ' ';
		names += info.name;
		if (std::strcmp(info.name, want) == 0)
			found = handles[i];
	}
	if (!s_logged)
	{
		s_logged = true;
		std::printf("[ime] modules in this process (list rc %#x, %zu): %s\n", static_cast<unsigned>(rc), count, names.c_str());
	}
	return found;
}

bool SystemKeyboardReady()
{
	SystemKeyboard& k = g_ime;
	if (k.tried)
		return k.init != nullptr;
	k.tried = true;
#ifdef PS5SX2_IME_IMPORT
	k.init = reinterpret_cast<ImeInitFn>(&sceImeDialogInit);
	k.status = reinterpret_cast<ImeStatusFn>(&sceImeDialogGetStatus);
	k.result = reinterpret_cast<ImeResultFn>(&sceImeDialogGetResult);
	k.term = reinterpret_cast<ImeTermFn>(&sceImeDialogTerm);
	std::printf("[ime] libSceImeDialog imported at start\n");
#else
	int module = LoadedModule("libSceImeDialog.sprx");
	if (module >= 0)
		std::printf("[ime] libSceImeDialog is already loaded (%#x)\n", static_cast<unsigned>(module));
	else
	{
		int res = 0;
		module = sceKernelLoadStartModule("/system/common/lib/libSceImeDialog.sprx", 0, nullptr, 0, nullptr, &res);
		std::printf("[ime] /system/common/lib/libSceImeDialog.sprx: load %#x (start result %d)\n", static_cast<unsigned>(module), res);
	}
	if (module >= 0)
	{
		void* a[4] = {};
		const char* const names[4] = {"sceImeDialogInit", "sceImeDialogGetStatus", "sceImeDialogGetResult", "sceImeDialogTerm"};
		bool ok = true;
		for (int i = 0; i < 4; i++)
			ok = sceKernelDlsym(module, names[i], &a[i]) == 0 && a[i] && ok;
		if (ok)
		{
			k.init = reinterpret_cast<ImeInitFn>(a[0]);
			k.status = reinterpret_cast<ImeStatusFn>(a[1]);
			k.result = reinterpret_cast<ImeResultFn>(a[2]);
			k.term = reinterpret_cast<ImeTermFn>(a[3]);
		}
		else
			std::printf("[ime] libSceImeDialog: a function is missing\n");
	}
#endif
	std::printf("[ime] the PS5's keyboard is %s\n", k.init ? "available" : "not available: the panel's own keyboard is used");
	std::fflush(stdout);
	return k.init != nullptr;
}

bool SystemKeyboardOpen(const std::string& title, const std::string& text, bool password, unsigned max_length)
{
	SystemKeyboard& k = g_ime;
	if (k.running || k.user < 0 || !SystemKeyboardReady())
		return false;
	max_length = std::min<unsigned>(max_length, 512);
	const std::u16string start = Utf8To16(text), t = Utf8To16(title);
	std::fill(std::begin(k.buffer), std::end(k.buffer), u'\0');
	std::copy_n(start.data(), std::min<size_t>(start.size(), max_length), k.buffer);
	std::fill(std::begin(k.title), std::end(k.title), u'\0');
	std::copy_n(t.data(), std::min<size_t>(t.size(), 95), k.title);
	ImeDialogParam p = {};
	p.user_id = k.user;
	p.type = kImeTypeBasicLatin; // a password needs it, and RetroAchievements names are plain letters and digits
	p.option = kImeOptionNoAutoCapitalization | kImeOptionNoLearning | (password ? kImeOptionPassword : 0);
	p.max_text_length = max_length;
	p.input_text_buffer = k.buffer;
	p.posx = 960.0f;
	p.posy = 300.0f;
	p.horizontal_alignment = 1;
	p.vertical_alignment = 1;
	p.title = k.title;
	const int rc = k.init(&p, nullptr);
	std::printf("[ime] keyboard for %s: init %#x\n", password ? "the password" : "the username", static_cast<unsigned>(rc));
	std::fflush(stdout);
	if (rc < 0)
		return false;
	k.running = true;
	return true;
}

int SystemKeyboardPoll(std::string& text)
{
	SystemKeyboard& k = g_ime;
	if (!k.running)
		return -1;
	const int status = k.status();
	if (status == 1)
		return 0; // still typing
	ImeDialogResult r = {};
	const int rrc = status == 2 ? k.result(&r) : -1;
	const int trc = k.term();
	k.running = false;
	const bool ok = status == 2 && rrc == 0 && r.end_status == 0;
	if (ok)
		text = Utf16To8(k.buffer);
	std::fill(std::begin(k.buffer), std::end(k.buffer), u'\0'); // it may hold the password
	std::printf("[ime] keyboard closed: status %d, result %#x, end %u, term %#x%s\n", status, static_cast<unsigned>(rrc), r.end_status,
		static_cast<unsigned>(trc), ok ? ", text taken" : "");
	std::fflush(stdout);
	return ok ? 1 : -1;
}
} // namespace

std::vector<std::string> orbis_usb_game_dirs(const char* when)
{
	std::vector<std::string> dirs;
	int drives = 0;
	// 2026-10-08 (AI-assisted; testers: "some people want ext/usb or m2"): the PS5's extended storage and M.2 drives
	// (/mnt/ext0, /mnt/ext1) the same way as the USB drives, after them.
	for (int i = 0; i < 10; i++)
	{
		char root[16];
		if (i < 8)
			std::snprintf(root, sizeof(root), "/mnt/usb%d", i);
		else
			std::snprintf(root, sizeof(root), "/mnt/ext%d", i - 8);
		const char* const tag = i < 8 ? "usb" : "drive";
		DIR* d = opendir(root);
		if (!d)
		{
			if (when && errno != ENOENT)
				std::printf("[%s] %s: %s: can't open (errno %d)\n", tag, when, root, errno);
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
			if (lower == "dvd" || lower == "cd" || lower == "ps5sx2" || lower == "games")
				subs.push_back(e->d_name);
			// vk-285-118 (AI-assisted): a drive laid out like /data/PCSX2 (testers: "games in the games/ folder on the external
			// drive are NOT detected", while PCSX2/textures there works): PCSX2/games/, and PS5SX2/games/, also DVD/ and CD/ in them.
			if (lower == "pcsx2" || lower == "ps5sx2")
			{
				const std::string parent = std::string(root) + "/" + e->d_name;
				if (DIR* pd = opendir(parent.c_str()))
				{
					while (const dirent* pe = readdir(pd))
					{
						const std::string l2 = LowerAscii(pe->d_name);
						if (l2 == "games" || l2 == "dvd" || l2 == "cd")
							subs.push_back(std::string(e->d_name) + "/" + pe->d_name);
					}
					closedir(pd);
				}
			}
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
			std::printf("[%s] %s: %s: %d entries; disc images in %s\n", tag, when, root, entries, found.c_str());
	}
	if (when && drives == 0)
		std::printf("[usb] %s: no USB drive with files at /mnt/usb0-7 (nor at /mnt/ext0-1)\n", when);
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
	SetShareRoots(paths.share_roots);       // vk-285-156
	WebConfig cfg;
	cfg.game_dirs = {paths.games_dir, paths.top_dir};
	cfg.game_dirs.insert(cfg.game_dirs.end(), paths.usb_dirs.begin(), paths.usb_dirs.end()); // test build 1
	cfg.settings_dir = paths.settings_dir;
	cfg.gs_ini = paths.gs_ini;
	cfg.patches_dir = paths.patches_dir;
	cfg.cheats_dir = paths.online_patches ? paths.cheats_dir : std::string(); // 2026-10-08
	cfg.covers_dir = paths.covers_dir;
	cfg.cache_dir = paths.cache_dir;
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
	cfg.current_disc = orbis_current_disc; // vk-285-139 (main-boot.cpp)
	cfg.disc_set = orbis_disc_set;
	cfg.change_disc = orbis_request_disc;
#ifdef PS5SX2_ACHIEVEMENTS
	cfg.achievements = Achievements::GetPS5GameAchievements;
	cfg.achievement_badge = Achievements::GetPS5AchievementBadge;
#endif
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
		g.serial = ReadSerial(g); // 2.02: through the serial cache, keyed by what ScanGames already has
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

// vk-285-144: a launch asked for from outside the shelf (a disc in the drive), taken by the shelf's loop.
static std::mutex g_launch_lock;
static std::string g_launch_request; // under g_launch_lock
static bool g_shelf_up = false;      // under g_launch_lock

uint64_t orbis_frontend_free_bytes(const std::string& dir)
{
	return TexturePackFreeBytes(dir);
}

bool orbis_frontend_request_launch(const std::string& path)
{
	std::lock_guard<std::mutex> lock(g_launch_lock);
	if (!g_shelf_up)
		return false;
	g_launch_request = path;
	return true;
}

static std::string TakeLaunchRequest()
{
	std::lock_guard<std::mutex> lock(g_launch_lock);
	std::string r;
	r.swap(g_launch_request);
	return r;
}

static void SetShelfUp(bool up)
{
	std::lock_guard<std::mutex> lock(g_launch_lock);
	g_shelf_up = up;
	if (!up)
		g_launch_request.clear();
}

std::string orbis_frontend_run(const OrbisFrontendPaths& paths, const char* build_tag, bool* ran)
{
	*ran = false;
	const double t0 = Now();
	SetSerialCacheFile(paths.serial_cache); // vk-285-108
	SetGameDbFile(paths.gamedb_file);       // vk-285-113
	SetShareRoots(paths.share_roots);       // vk-285-156
	std::vector<std::string> dirs = {paths.games_dir, paths.top_dir};
	dirs.insert(dirs.end(), paths.usb_dirs.begin(), paths.usb_dirs.end()); // test build 1: USB drives
	std::vector<GameInfo> games = ScanGames(dirs);
	int on_usb = 0, on_nfs = 0;
	for (GameInfo& g : games)
	{
		g.serial = ReadSerial(g); // 2.02: through the serial cache, keyed by what ScanGames already has
		// vk-285-113: a CHD (or an ISO) named after its file shows the game's name from the game database.
		const std::string file_title = g.title;
		const bool renamed = ApplyGameDbTitle(g);
		ReadBadges(g, paths.settings_dir, paths.gs_ini, paths.patches_dir);
		on_usb += OnUsb(g.path) ? 1 : 0;
		on_nfs += OnNetworkShare(g.path) ? 1 : 0;
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
			// vk-285-134: libchdr's "invalid file/data" (or a version it doesn't read) is a damaged image; a CHD that needs
			// its parent isn't (PCSX2 finds the parent itself).
			const std::string cant = "libchdr can't open it: ";
			if (what.rfind(cant, 0) == 0 && what.find("parent") == std::string::npos)
				g.damaged = what.substr(cant.size());
		}
	}
	SortGames(games); // vk-285-113: by the titles the game database may have changed
	std::printf("[frontend] %zu disc image(s), %d on USB, %d on NFS shares, scanned in %.0f ms\n", games.size(), on_usb, on_nfs,
		(Now() - t0) * 1000.0);
	std::fflush(stdout);
	WriteUsbList(paths.usb_list, games);
	// Keep the shelf available without games too, for settings and account sign-in.
	const std::string last = ReadLastGame(paths.top_dir);
	int preselect = 0;
	for (size_t i = 0; i < games.size(); i++)
		if (games[i].file == last)
			preselect = static_cast<int>(i);
	// vk-285-69: one game opens the shelf too, not the game straight away: the shelf's QR code is how
	// testers reach the settings page and its Download logs (swordpdf, 2026-09-26).

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
	covers->Start(games, fonts, cc, CoverGet); // 2026-10-10: was libSceHttp2 (g_http), which fails after the jailbreak

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
#ifdef PS5SX2_ACHIEVEMENTS
	acfg.achievements = OrbisAchievementsAccountService();
	acfg.game_achievements = OrbisAchievementsBrowserService();
	g_ime.user = user;
	acfg.text_entry.open = SystemKeyboardOpen;
	acfg.text_entry.poll = SystemKeyboardPoll;
#endif
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
	// 2026-10-05 (AI-assisted): HD texture packs from archive.org, on the sheet's row (fe_texpacks.h). The worker runs while
	// the shelf is up; a download left unfinished goes on at the next shelf.
	TexturePackManager* texpacks = nullptr;
	if (paths.texture_packs && !paths.textures_dir.empty())
	{
		TexturePackPlatform tp;
		tp.get_text = [](const std::string& url, std::string& body) {
			body.clear();
			return g_texhttp.Get(url, false, 0, 0, [&body](const void* d, size_t n) {
				if (body.size() + n > (8u << 20))
					return false;
				body.append(static_cast<const char*>(d), n);
				return true;
			});
		};
		tp.get_range = [](const std::string& url, uint64_t offset, uint64_t length, const std::function<bool(const void*, size_t)>& sink) {
			return g_texhttp.Get(url, true, offset, length, sink);
		};
		tp.abort = [] { g_texhttp.Abort(); };
		tp.free_bytes = [](const std::string& dir) { return TexturePackFreeBytes(dir); };
		tp.thread_start = [](const char* role) { TexturePackThreadStart(role); };
		tp.existing_pack = [](const std::string& serial) {
			std::string how;
			return OrbisTexturesGameDir(serial, how);
		};
		tp.log = [](const std::string& line) {
			std::printf("%s\n", line.c_str());
			std::fflush(stdout);
		};
		tp.notify = [](const std::string& game, bool ok, const std::string& detail) {
			const std::string sub = detail.empty() ? game : game + " \xC2\xB7 " + detail;
			OrbisNotifyRich(Tr(ok ? Str::HdTexturesReady : Str::HdTexturesFailed), sub.c_str(), "");
		};
		tp.now = [] { return Now(); };
		const std::string textures = paths.textures_dir;
		texpacks = new TexturePackManager(tp, textures, textures + "/.ps5sx2-downloads", paths.texture_pack_list);
		texpacks->Start();
		acfg.texture_packs = texpacks->Service();
	}
	// 2026-10-08 (AI-assisted): a game's patches and cheats from GitHub, on the sheet's "Get patches and cheats" row
	// (fe_patchdl.h), over its own HTTPS client. One small file a source; the worker ends when it has nothing to do.
	OnlinePatches* online = nullptr;
	if (paths.online_patches && !paths.patches_dir.empty() && !paths.cheats_dir.empty())
	{
		OnlinePatchPlatform op;
		op.get_text = [](const std::string& url, std::string& body) {
			body.clear();
			return g_patchhttp.Get(url, false, 0, 0, [&body](const void* d, size_t n) {
				if (body.size() + n > (2u << 20))
					return false;
				body.append(static_cast<const char*>(d), n);
				return true;
			});
		};
		op.log = [](const std::string& line) {
			std::printf("%s\n", line.c_str());
			std::fflush(stdout);
		};
		op.thread_start = [] { TexturePackThreadStart("online patches"); };
		online = new OnlinePatches(op, paths.patches_dir, paths.cheats_dir, paths.online_patch_manifest);
		acfg.online_patches = online->Service();
		acfg.options.cheats_dir = paths.cheats_dir;
	}
	acfg.system_menu = true; // 2026-10-08: the sheet for all games offers the PS2 system menu (main-boot.cpp boots it)
	// vk-285-135: the places the sheet's folder picker starts from (those that are folders when it opens).
	acfg.folder_places = {{"PS5SX2's folder", "/data/PCSX2"}};
	for (int i = 0; i < 8; i++)
		acfg.folder_places.push_back({"USB drive " + std::to_string(i + 1), "/mnt/usb" + std::to_string(i)});
	acfg.folder_places.push_back({"Extended storage", "/mnt/ext0"});
	acfg.folder_places.push_back({"Extended storage 2", "/mnt/ext1"});
	acfg.folder_places.push_back({"NFS shares", "/nfs"}); // OrbisNfs.cpp: a folder while shares are set
	acfg.bios_present = paths.bios_check; // vk-285-134
	acfg.bios_problem = paths.bios_problem;
	acfg.bios_dir = paths.bios_dir;
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

	std::printf("[frontend] shelf thread: %s\n", ThreadPlace().c_str()); // 2026-10-05: see TexturePackThreadStart
	FrameDesc frame;
	double last_t = Now(), report_t = last_t;
	unsigned frames = 0, slot = 0;
	double worst = 0;
	bool first_shown = false;
	std::string launch_path; // vk-285-144: a disc's game, asked for from outside
	SetShelfUp(ok);
	while (ok && !app.Done())
	{
		const double now = Now();
		launch_path = TakeLaunchRequest();
		if (!launch_path.empty())
		{
			std::printf("[frontend] launch asked for from outside the shelf: %s\n", launch_path.c_str());
			std::fflush(stdout);
			break;
		}
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
			// 2026-10-05: not while a texture pack is being unpacked, installed or removed: its thousands of files keep the
			// disk busy, and this write from the menu's thread then waited for the file system's commits (frames of up to
			// 0.95 s during pr9h's unpack, in a pattern that followed a ~20 s commit cycle).
			bool disk_busy = false;
			if (texpacks)
			{
				const TexturePackActivity a = texpacks->Activity();
				disk_busy = a.active && (a.state == TexturePackStatus::State::Unpacking || a.state == TexturePackStatus::State::Installing ||
											a.state == TexturePackStatus::State::Removing);
			}
			if (!disk_busy)
			{
				std::printf("[frontend] %u frames in %.1f s, worst %.1f ms\n", frames, now - report_t, worst * 1000.0);
				std::fflush(stdout);
			}
			frames = 0;
			worst = 0;
			report_t = now;
		}
	}

	SetShelfUp(false);
	const int chosen = app.Chosen();
	const bool picked = ok && (app.Done() || !launch_path.empty());
	const bool system_menu = app.SystemMenuChosen(); // 2026-10-08
	renderer.WaitIdle();
	app.Shutdown();
	// The worker may be inside a download: stop it from starting another, fail the one in flight,
	// give it a moment, then leave it behind if need be.
	covers->RequestStop();
	g_http.Abort();
	g_coverhttp.Abort(); // 2026-10-10
	// 2026-10-05: the texture packs stop with the shelf: a download keeps its part for the next one.
	if (texpacks)
	{
		if (texpacks->Stop(1500))
		{
			delete texpacks;
			g_texhttp.Term();
		}
		else
			std::printf("[frontend] a texture pack job is still stopping; leaving it to end on its own\n");
	}
	// 2026-10-08: the online patches stop with the shelf (a request in flight is failed; the job is asked again later).
	if (online)
	{
		g_patchhttp.Abort();
		if (online->Stop(1500))
		{
			delete online;
			g_patchhttp.Term();
		}
		else
			std::printf("[frontend] an online patch request is still stopping; leaving it to end on its own\n");
	}
	const bool stopped = covers->Stop(1500);
	renderer.Shutdown();
	display->Destroy();
	delete display;
	if (stopped)
	{
		delete covers;
		delete fonts;
		g_http.Term();
		g_coverhttp.Term(); // 2026-10-10
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
	if (!launch_path.empty())
	{
		const size_t slash = launch_path.rfind('/');
		WriteLastGame(paths.top_dir, slash == std::string::npos ? launch_path : launch_path.substr(slash + 1));
		std::printf("[frontend] started %s (a disc in the drive)\n", launch_path.c_str());
		std::fflush(stdout);
		return launch_path;
	}
	if (system_menu)
	{
		// 2026-10-08: the PS2's own menu, no disc (main-boot.cpp boots the BIOS for kOrbisSystemMenuPath). The last game stays.
		std::printf("[frontend] picked the PS2 system menu (no disc)\n");
		std::fflush(stdout);
		return kOrbisSystemMenuPath;
	}
	const GameInfo& g = games[static_cast<size_t>(chosen)];
	WriteLastGame(paths.top_dir, g.file);
	std::printf("[frontend] picked %s (%s)\n", g.file.c_str(), g.serial.c_str());
	std::fflush(stdout);
	return g.path;
}
