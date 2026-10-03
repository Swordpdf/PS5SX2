// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ImGui/FullscreenUI.h"
#include "OrbisEEDiag.h" // vk-285-100
#include "ImGui/ImGuiManager.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/Common/GSFunctionMap.h" // vk-285-28: GSCodeReserve (the [rec] line)
extern "C" int sceKernelAvailableFlexibleMemorySize(unsigned long long* size); // vk-285-28
#include "GS/GSCapture.h"
#include "GS/GSDump.h"
#include "GS/GSGL.h"
#include "GS/GSPerfMon.h"
#include "GS/GSUtil.h"
#include "GS/Renderers/Vulkan/VKOrbisTiming.h" // vk-285-113: the readback counters (OrbisPerfMinute)
#ifdef ORBIS_VULKAN
#include "OrbisGSShaders.h" // vk-285-115: shadeboost.glsl from the folder or the eboot
#endif
#include "GSDumpReplayer.h"
#include "Host.h"
#include "PerformanceMetrics.h"
#include "pcsx2/Config.h"
#include "VMManager.h"

#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#include "fmt/format.h"
#include "IconsFontAwesome.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <cmath>  // test build 1 (vk-285-55)
#include <vector>

static void DumpGSPrivRegs(const GSPrivRegSet& r, const std::string& filename);

// Orbis: GL/GS readback dump implemented in GSDeviceOGL.cpp.
void OrbisPresentGLFrame();
void OrbisSampleWindow();

static constexpr std::array<PresentShader, 8> s_tv_shader_indices = {
	PresentShader::COPY, PresentShader::SCANLINE,
	PresentShader::DIAGONAL_FILTER, PresentShader::TRIANGULAR_FILTER,
	PresentShader::COMPLEX_FILTER, PresentShader::LOTTES_FILTER,
	PresentShader::SUPERSAMPLE_4xRGSS, PresentShader::SUPERSAMPLE_AUTO};

// ---- eerec-278 (PS5 port): live present tuning + FPS box for the GL presenter ----
// Modes cycle when L3+R3 are held (the pad thread bumps g_orbis_filter_cycle) or are set from
// /data/PCSX2/live.ini (re-read every 30 vsyncs when its content changes). There is no ImGui
// on Orbis, so the FPS / mode label is a small CPU-drawn texture presented top-right.
extern std::atomic<int> g_orbis_filter_cycle;
extern "C" void orbis_text_rgba(u32* buf, unsigned w, unsigned h, unsigned x, unsigned y, const char* s, unsigned scale, u32 color);
float g_orbis_present_param[4] = {0.5f, 0.0f, 0.0f, 0.0f}; // u_orbis_param (GSDeviceOGL::PresentRect)
int g_orbis_swtex = 2; // eerec-280 default (eerec-279: 1): SW output texture kind (GSRendererSW::GetOutput)
int g_orbis_testpat = 0; // eerec-279: SW output replaced by a test pattern
int g_orbis_upload_mode = 0; // eerec-279: GSTextureOGL::Update path (0 PCSX2, 1 direct DSA, 2 direct bind)
char g_orbis_osd_text[24]; // eerec-282: one-shot OSD label (savestates)
std::atomic<int> g_orbis_osd_text_frames{0};
void OrbisOSDLabel(const char* text)
{
	snprintf(g_orbis_osd_text, sizeof(g_orbis_osd_text), "%s", text);
	g_orbis_osd_text_frames.store(120, std::memory_order_release);
}
int g_orbis_diag = 0; // eerec-280: periodic GL readback diagnostics (live.ini diag=1)
int g_orbis_perf = 0; // eerec-280: perf OSD + [perf] klog line every second (live.ini perf=1)
// vk-285-113: PS5SX2/Overlay and PS5SX2/FpsGraph from gs.ini or the game's ini (main-boot.cpp orbis_ps5opts_from), and
// the settings page's QR code over the game (the pad thread: L2 + D-pad down held for 2 s).
std::atomic<int> g_orbis_overlay_mode{-1}; // -1: live.ini's fps= and perf= decide; 0: no box; 1: FPS; 2: FPS and the EE/GS/VU loads
std::atomic<int> g_orbis_fps_graph{0}; // 1: a blue graph of the last minute's frame rate, top right
std::atomic<int> g_orbis_qr_show{0}; // 1: the settings page's QR code and address over the game
// The page's address as a QR code (fe_ps5.cpp): its modules, one byte each row by row, the side length, and the
// "192.168.1.20:8844" it is shown as; false without a web server or a network.
extern bool orbis_web_qr(std::vector<unsigned char>& modules, int& size, std::string& shown);
// ---- eerec-285: live gs.ini reload flags; CPU placement sampling and pinning ----
#include <pthread.h>
#include <sys/param.h> // vk-285-87: cpuset_setaffinity (pin_all)
#include <sys/cpuset.h>
#include <cerrno>
#include <x86intrin.h> // vk-285-106: __rdtsc for [vsslow]
#include "OrbisPaths.h" // vk-285-33 (the port's include-orbis)
#ifdef ORBIS_VULKAN
#include "GS/Renderers/Vulkan/VKOrbisTiming.h" // vk-285-106: the Vulkan waits inside a slow vsync
#endif
// PS5 port (vk-285-104): this file's printf/fflush(stdout) go to the deferred log (OrbisDeferredLog.h); the
// ticker thread writes them out, so the GS thread never waits on /data or on stdout's lock.
#include "OrbisDeferredLog.h"
#define printf OrbisDeferredPrintf
#define fflush OrbisDeferredFlush
std::atomic<int> g_orbis_gsini_reload{0}; // GS thread saw gs.ini change -> the CPU thread applies it
std::atomic<int> g_orbis_live_reapply{0}; // the CPU thread applied gs.ini -> apply live.ini again
std::atomic<int> g_orbis_pin_request{-1}; // live.ini pin= -> the CPU thread (OrbisApplyPinning)
extern "C" int sceKernelGetCurrentCpu(void);
extern "C" int sceKernelGetCpumode(void);
extern "C" int scePthreadGetaffinity(pthread_t thread, unsigned long long* mask);
extern "C" int scePthreadSetaffinity(pthread_t thread, unsigned long long mask);
// slots: 0 EE, 1 GS, 2 VU1, 3..7 SW workers 0..4
static constexpr int ORBIS_CPU_SLOTS = 8;
// vk-285-87: pin=3's layout from live.ini, a CPU each (-1: any CPU the others leave free) for the EE, GS
// and VU threads and the Vulkan driver's recorder and queue worker; pin_all=1 first restricts every thread
// of the process to the CPUs the layout leaves free (cpuset_setaffinity), so no unnamed thread shares the
// EE's or the VU's core.
enum { ORBIS_PIN_EE, ORBIS_PIN_GS, ORBIS_PIN_VU, ORBIS_PIN_REC, ORBIS_PIN_QUEUE, ORBIS_PIN_COUNT };
static std::atomic<int> s_orbis_pin_cpu[ORBIS_PIN_COUNT] = {-1, -1, -1, -1, -1};
static std::atomic<int> s_orbis_pin_all{0};
#ifdef ORBIS_VULKAN
void* OrbisVkDeviceHandle(); // GSDeviceVK.cpp
extern "C" uint32_t ps5vk_debug_threads(void* device, pthread_t* recorder, pthread_t* queue_worker)
	__attribute__((weak)); // the driver (ps5vk_debug.h)
#endif
static pthread_t s_orbis_thr[ORBIS_CPU_SLOTS];
static unsigned s_orbis_cpu_hist[ORBIS_CPU_SLOTS][16];
static unsigned long long s_orbis_cpu_last[ORBIS_CPU_SLOTS];
static int s_orbis_pin_mode = 0;
void OrbisCpuSample(int slot)
{
	if (slot < 0 || slot >= ORBIS_CPU_SLOTS)
		return;
	const unsigned long long t = __builtin_ia32_rdtsc();
	if (t - s_orbis_cpu_last[slot] < 16000000ull) // about 5 ms
		return;
	s_orbis_cpu_last[slot] = t;
	if (s_orbis_thr[slot] == pthread_t{})
	{
		s_orbis_thr[slot] = pthread_self();
		if (s_orbis_pin_mode != 0) // a thread that started after pinning is pinned too
			g_orbis_pin_request.store(s_orbis_pin_mode, std::memory_order_release);
	}
	const int c = sceKernelGetCurrentCpu();
	if (c >= 0 && c < 16)
		s_orbis_cpu_hist[slot][c]++;
}
void OrbisCpuForget(int slot)
{
	if (slot >= 0 && slot < ORBIS_CPU_SLOTS)
		s_orbis_thr[slot] = pthread_t{};
}
// vk-285-108: helper threads (the ticker, the disc reader) get the CPUs the pin layout leaves free. A thread
// starts with its creator's CPU set, and the ticker is made by the CPU thread after the first Execute
// returns, by when pin=3 has put that thread on the EE's CPU alone: vk-285-107's ticker shared CPU 2 with
// an EE that never slept and stopped at t+8s (its log lines waited in memory until the app closed, and an FTP
// listing of logs/ hung, likely on a file lock it held); vk-285-98 to 106's ticked only when the EE waited.
// A registered thread gets the layout's `rest` CPUs now and whenever the layout changes; unregister before
// the thread ends. 0: no layout applied yet (a thread keeps its own set).
static std::mutex s_orbis_helper_mutex;
static pthread_t s_orbis_helpers[8];
static int s_orbis_helper_n = 0;
static unsigned long long s_orbis_helper_mask = 0;
static int OrbisPinHelpersLocked()
{
	int failed = 0;
	for (int i = 0; i < s_orbis_helper_n; i++)
		failed += scePthreadSetaffinity(s_orbis_helpers[i], s_orbis_helper_mask) != 0 ? 1 : 0;
	return failed;
}
static void OrbisPinHelpers(unsigned long long mask)
{
	std::lock_guard<std::mutex> lock(s_orbis_helper_mutex);
	s_orbis_helper_mask = mask;
	const int failed = OrbisPinHelpersLocked();
	printf("[pin] helpers: %d thread(s) to %#llx%s\n", s_orbis_helper_n, mask, failed ? " (some failed)" : "");
}
void OrbisHelperThreadAdd(pthread_t thread)
{
	std::lock_guard<std::mutex> lock(s_orbis_helper_mutex);
	if (s_orbis_helper_n < static_cast<int>(std::size(s_orbis_helpers)))
		s_orbis_helpers[s_orbis_helper_n++] = thread;
	if (s_orbis_helper_mask != 0)
		scePthreadSetaffinity(thread, s_orbis_helper_mask);
}
void OrbisHelperThreadRemove(pthread_t thread)
{
	std::lock_guard<std::mutex> lock(s_orbis_helper_mutex);
	for (int i = 0; i < s_orbis_helper_n; i++)
	{
		if (pthread_equal(s_orbis_helpers[i], thread))
		{
			s_orbis_helpers[i] = s_orbis_helpers[--s_orbis_helper_n];
			break;
		}
	}
}
// vk-285-108: the ticker's heartbeat (main-boot.cpp), which the GS thread watches once a second
// (OrbisTickerWatch below): the TSC at the end of its last pass, the step it is in and the CPU it was on.
std::atomic<unsigned long long> g_orbis_ticker_beat{0};
std::atomic<int> g_orbis_ticker_step{0}, g_orbis_ticker_cpu{-1};
pthread_t g_orbis_ticker_thread{};
// No pass for 3 s: a deferred [ticker] line (it goes out with the ticker's next drain) naming its step and CPU,
// and the ticker moved off that CPU (the layout's helper CPUs without it, or every CPU of the process but it).
static void OrbisTickerWatch()
{
	static unsigned long long s_last_warn = 0;
	static unsigned long long s_orig_mask = 0;
	const unsigned long long beat = g_orbis_ticker_beat.load(std::memory_order_acquire);
	if (beat == 0 || g_orbis_ticker_thread == pthread_t{})
		return;
	const unsigned long long now = __rdtsc();
	const double stale_s = static_cast<double>(now - beat) / 1596300000.0;
	if (stale_s < 3.0 || (s_last_warn != 0 && now - s_last_warn < 5ull * 1596300000ull))
		return;
	s_last_warn = now;
	static const char* const steps[] = {"sleep", "flags", "drain", "lines", "overlay"};
	const int step = g_orbis_ticker_step.load(std::memory_order_relaxed);
	const int cpu = g_orbis_ticker_cpu.load(std::memory_order_relaxed);
	unsigned long long mask;
	{
		std::lock_guard<std::mutex> lock(s_orbis_helper_mutex);
		mask = s_orbis_helper_mask;
	}
	if (mask == 0)
	{
		if (s_orig_mask == 0 && scePthreadGetaffinity(g_orbis_ticker_thread, &s_orig_mask) != 0)
			s_orig_mask = 0;
		mask = s_orig_mask;
	}
	if (cpu >= 0 && cpu < 64 && (mask & ~(1ull << cpu)) != 0)
		mask &= ~(1ull << cpu);
	const int rc = mask ? scePthreadSetaffinity(g_orbis_ticker_thread, mask) : -1;
	printf("[ticker] no pass for %.1f s: in step %s, last on CPU %d; moved to %#llx (rc=%d)\n", stale_s,
		(step >= 0 && step < 5) ? steps[step] : "?", cpu, mask, rc);
}
// vk-285-87: pin=3, the explicit layout (s_orbis_pin_cpu). A named CPU outside the process mask counts as
// unnamed. The EE's and the VU's SMT siblings are left free as well, so those two threads have their cores
// alone; every unnamed thread gets the CPUs left over.
static void OrbisApplyLayout(unsigned long long orig)
{
	int cpu[ORBIS_PIN_COUNT];
	unsigned long long reserved = 0;
	for (int i = 0; i < ORBIS_PIN_COUNT; i++)
	{
		cpu[i] = s_orbis_pin_cpu[i].load(std::memory_order_acquire);
		if (cpu[i] < 0 || cpu[i] > 15 || !((orig >> cpu[i]) & 1ull))
			cpu[i] = -1;
		if (cpu[i] >= 0)
			reserved |= 1ull << cpu[i];
	}
	if (cpu[ORBIS_PIN_EE] >= 0)
		reserved |= 1ull << (cpu[ORBIS_PIN_EE] ^ 1);
	if (cpu[ORBIS_PIN_VU] >= 0)
		reserved |= 1ull << (cpu[ORBIS_PIN_VU] ^ 1);
	unsigned long long rest = orig & ~reserved;
	if (rest == 0)
		rest = orig;
	auto mask_of = [&](int which) { return cpu[which] >= 0 ? (1ull << cpu[which]) : rest; };
	// Every thread of the process first, when asked: the named ones get their own CPUs right after.
	int rc_all = 1;
	if (s_orbis_pin_all.load(std::memory_order_acquire))
	{
		cpuset_t set;
		CPU_ZERO(&set);
		for (int c = 0; c < 64; c++)
			if ((rest >> c) & 1ull)
				CPU_SET(c, &set);
		// vk-285-88: the console's kernel refused sizeof(cpuset_t) (32 bytes) with ERANGE in vk-285-87, so the
		// set's size is whatever it takes: 8, 16 or 32 bytes (the first 64 bits hold every CPU there is).
		for (const size_t size : {size_t{8}, size_t{16}, sizeof(set)})
		{
			rc_all = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, size, &set) == 0 ? 0 : errno;
			if (rc_all != ERANGE)
				break;
		}
	}
	s_orbis_pin_mode = 3;
	int rc[ORBIS_CPU_SLOTS];
	for (int i = 0; i < ORBIS_CPU_SLOTS; i++)
	{
		const unsigned long long m = (i == 0) ? mask_of(ORBIS_PIN_EE) : (i == 1) ? mask_of(ORBIS_PIN_GS) :
			(i == 2) ? mask_of(ORBIS_PIN_VU) : rest;
		rc[i] = (s_orbis_thr[i] != pthread_t{}) ? scePthreadSetaffinity(s_orbis_thr[i], m) : 1;
	}
	int rc_rec = 1, rc_queue = 1;
#ifdef ORBIS_VULKAN
	if (ps5vk_debug_threads)
	{
		if (void* const device = OrbisVkDeviceHandle())
		{
			pthread_t recorder{}, queue{};
			const uint32_t found = ps5vk_debug_threads(device, &recorder, &queue);
			if (found & 1u)
				rc_rec = scePthreadSetaffinity(recorder, mask_of(ORBIS_PIN_REC));
			if (found & 2u)
				rc_queue = scePthreadSetaffinity(queue, mask_of(ORBIS_PIN_QUEUE));
		}
	}
#endif
	printf("[pin] mode=3 ee=%d gs=%d vu=%d rec=%d queue=%d rest=%#llx all=%d rc ee=%d gs=%d vu=%d rec=%d queue=%d\n",
		cpu[ORBIS_PIN_EE], cpu[ORBIS_PIN_GS], cpu[ORBIS_PIN_VU], cpu[ORBIS_PIN_REC], cpu[ORBIS_PIN_QUEUE], rest,
		rc_all, rc[0], rc[1], rc[2], rc_rec, rc_queue);
	OrbisPinHelpers(rest); // vk-285-108
	fflush(stdout);
	OrbisOSDLabel("PIN 3");
}

// CPU thread (Host::PumpMessagesOnCPUThread). SMT siblings are taken to be CPUs 2k and 2k+1.
void OrbisApplyPinning(int mode)
{
	static unsigned long long s_orig = 0;
	const pthread_t self = pthread_self();
	s_orbis_thr[0] = self;
	if (s_orig == 0)
	{
		unsigned long long m = 0;
		const int rc = scePthreadGetaffinity(self, &m);
		printf("[pin] process mask %#llx (rc=%d) cpumode=%d\n", m, rc, sceKernelGetCpumode());
		if (rc != 0 || m == 0)
		{
			fflush(stdout);
			OrbisOSDLabel("PIN: NO MASK");
			return;
		}
		s_orig = m;
	}
	int pairs[8], np = 0;
	for (int k = 7; k >= 0; k--)
		if (((s_orig >> (2 * k)) & 3ull) == 3ull)
			pairs[np++] = k;
	if (np < 3 && mode != 3)
		mode = 0;
	unsigned long long ee = s_orig, gs = s_orig, rest = s_orig;
	if (mode == 3)
	{
		OrbisApplyLayout(s_orig);
		return;
	}
	if (mode >= 1)
	{
		ee = 1ull << (2 * pairs[0]);
		rest &= ~(3ull << (2 * pairs[0]));
		gs = rest;
	}
	if (mode >= 2 && np >= 4)
	{
		gs = 1ull << (2 * pairs[1]);
		rest &= ~(3ull << (2 * pairs[1]));
	}
	s_orbis_pin_mode = mode;
	int rc[ORBIS_CPU_SLOTS];
	for (int i = 0; i < ORBIS_CPU_SLOTS; i++)
	{
		const unsigned long long m = (i == 0) ? ee : (i == 1) ? gs : rest;
		rc[i] = (s_orbis_thr[i] != pthread_t{}) ? scePthreadSetaffinity(s_orbis_thr[i], m) : 1;
	}
	printf("[pin] mode=%d pairs=%d ee=%#llx gs=%#llx rest=%#llx rc ee=%d gs=%d vu=%d sw=%d/%d/%d/%d/%d\n", mode, np, ee,
		gs, rest, rc[0], rc[1], rc[2], rc[3], rc[4], rc[5], rc[6], rc[7]);
	OrbisPinHelpers(rest); // vk-285-108 (mode 0: every CPU of the process)
	fflush(stdout);
	char label[24];
	snprintf(label, sizeof(label), "PIN %d", mode);
	OrbisOSDLabel(label);
}
// Appended to the [load] line: each thread's two most frequent CPUs and their share of the samples.
static void OrbisPrintCpu()
{
	static const char* const names[ORBIS_CPU_SLOTS] = {"ee", "gs", "vu", "sw0", "sw1", "sw2", "sw3", "sw4"};
	printf(" | cpu");
	for (int i = 0; i < ORBIS_CPU_SLOTS; i++)
	{
		unsigned h[16];
		memcpy(h, s_orbis_cpu_hist[i], sizeof(h));
		memset(s_orbis_cpu_hist[i], 0, sizeof(h));
		unsigned tot = 0;
		int a = -1, b = -1;
		for (int c = 0; c < 16; c++)
		{
			tot += h[c];
			if (h[c] == 0)
				continue;
			if (a < 0 || h[c] > h[a])
			{
				b = a;
				a = c;
			}
			else if (b < 0 || h[c] > h[b])
				b = c;
		}
		if (tot == 0)
			continue;
		printf(" %s=%d:%u", names[i], a, h[a] * 100 / tot);
		if (b >= 0)
			printf(",%d:%u", b, h[b] * 100 / tot);
	}
}
extern double GetVerticalFrequency();
extern unsigned long long g_orbis_gs_idle_ticks, g_orbis_ee_waitgs_ticks, g_orbis_ee_stall_ticks; // eerec-174
extern unsigned long long g_orbis_ee_vsyncq_ticks, g_orbis_vu_idle_ticks, g_orbis_ee_waitvu_ticks, g_orbis_ee_vuring_ticks,
	g_orbis_ee_throttle_ticks, g_orbis_gs_swsync_ticks, g_orbis_sw_busy_ticks[16]; // eerec-281
#ifdef ORBIS_VULKAN
extern std::atomic<int> g_orbis_widescreen, g_orbis_ws_active; // vk-285-12 (pcsx2/OrbisWidescreen.cpp)
void OrbisEEProfMark(); // vk-285-8 (the port's orbis_eeprof.cpp)
void OrbisGSProfStart(); // vk-285-24: the same profiler on the GS thread (/data/PCSX2/gsprof)
void OrbisAutoProfSecond(float speed, float ee, float gs, float vu); // vk-285-118 (orbis_eeprof.cpp)
std::atomic<int> g_orbis_rb_auto_request{0}; // vk-285-118: StubHost.cpp hands it to main-boot.cpp's OrbisReadbackAutoCpu
bool OrbisFlag(const char* name);

// vk-285-118 (AI-assisted): the readback stall of older firmware. The testers' logs (2026-09-30..10-03, six consoles) put
// a GPU readback's wait at 20-36 ms on firmware 4.03, 4.50, 6.00 and 7.40, and at 0.3-6 ms on 10.40, 12.00 and 13.x,
// for the same kind of tiny reads (0.0 MB): the wait runs to a vblank or two, so a game that reads back once a frame runs
// at half speed (Crash Nitro Kart and Valkyrie Profile 2 at 50% and 33.3 ms a frame, Fatal Frame, The Matrix: Path of Neo
// at 29%). When that is what is happening -- the GS thread waits 150 ms a second or more for readbacks, each one 10 ms or
// more, the game is under 92% speed, for 3 seconds in a row -- GPU readbacks switch to Don't wait (HWDownloadMode 3) for
// the rest of the game, unless gs.ini or the game's file sets HWDownloadMode itself (OrbisReadbackAutoCpu, on the CPU
// thread). The flag noautoreadback turns it off.
// vk-285-119 (AI-assisted): the same switch for many short waits. The vk-285-118 logs (firmware 13.x) had games that
// read back ~90-170 times a second, each wait only 2.6-5 ms but together 435-500 ms of every second on the GS thread:
// The Punisher (77% speed), Max Payne 2, Guitar Hero World Tour. So: 300 ms a second or more of readback waits, at 20
// readbacks or more, under 92% speed, for 5 seconds in a row (the 10 ms-each rule still needs 3) -- and at least 1.5
// readbacks a displayed frame: a wait also covers the GPU work queued before it, so a game held back by the GPU that
// reads back once a frame would reach 300 ms too, and Don't wait would buy it little (review of vk-285-119). The three
// games above read back 2 to 3.4 times a frame.
std::atomic<int> g_orbis_rb_auto_kind{0}; // 1: each wait 10 ms or more (old firmware), 2: many short waits (main-boot.cpp)
static void OrbisReadbackAutoSecond(float speed, unsigned fps)
{
	static int s_checked = -1;
	static bool s_done = false;
	static int s_streak = 0, s_streak_many = 0;
	static unsigned long long s_n0 = 0, s_ns0 = 0;
	const unsigned long long n = g_orbis_readback_wait_n - s_n0, ns = g_orbis_readback_wait_ns - s_ns0;
	s_n0 = g_orbis_readback_wait_n;
	s_ns0 = g_orbis_readback_wait_ns;
	if (s_done)
		return;
	if (s_checked < 0)
		s_checked = OrbisFlag("noautoreadback") ? 1 : 0;
	if (s_checked == 1 || GSConfig.HWDownloadMode > GSHardwareDownloadMode::EnabledForceFull)
		return;
	const double ms = static_cast<double>(ns) / 1e6;
	if (n >= 3 && ms >= 150.0 && ms / static_cast<double>(n) >= 10.0 && speed < 92.0f)
		s_streak++;
	else
		s_streak = 0;
	if (n >= 20 && ms >= 300.0 && speed < 92.0f && 2ull * n >= 3ull * fps)
		s_streak_many++;
	else
		s_streak_many = 0;
	const int kind = (s_streak >= 3) ? 1 : (s_streak_many >= 5) ? 2 : 0;
	if (kind == 0)
		return;
	s_done = true;
	printf("[readbacks] the GS thread waited %.0f ms in the last second for %llu readbacks (%.1f ms each) at %.0f%% speed, "
		   "%d s in a row: asking for GPU readbacks Don't wait (%s, %u fps)\n", ms, n, ms / static_cast<double>(n), speed,
		kind == 1 ? s_streak : s_streak_many, kind == 1 ? "each wait 10 ms or more" : "many waits, 300 ms a second or more", fps);
	fflush(stdout);
	g_orbis_rb_auto_kind.store(kind, std::memory_order_relaxed);
	g_orbis_rb_auto_request.store(1, std::memory_order_release);
}
#endif
// vk-285-72: the EE, GS and VU threads' loads over the last second, in percent, from the [load] line's
// wait counters (1000 ms less the ms each thread waited). The perf line in settings.log, the [perf] line
// and the FPS box showed PerformanceMetrics' per-thread CPU times, and on the console those gave EE, GS
// and VU one and the same number every time (probably no per-thread CPU clock behind
// pthread_getcpuclockid there; not checked). Valid once OrbisMeasureLoad has measured a second.
static float s_orbis_load_ee = 0.0f, s_orbis_load_gs = 0.0f, s_orbis_load_vu = 0.0f;
static bool s_orbis_load_valid = false;

// eerec-281: ms per second each thread spent waiting (TSC, calibrated against steady_clock every second).
// vk-285-72: measured once a second on the GS thread whether or not the [load] line prints.
struct OrbisLoadMeasure
{
	bool measured = false;
	double k = 0.0; // ms of the second per TSC tick
	double v[10] = {};
	double ee_wait = 0.0;
	double sw_busy[16] = {};
	u32 nsw = 0;
	double vu_mcycles = 0.0; // vk-285-74: VU1 cycles the MTVU thread ran this second, in millions
	double vu_runs = 0.0; // and VU1 program runs
	double sw_sync_n[8] = {}, sw_sync_ms[8] = {}; // vk-285-119: the GS thread's SW syncs by reason, per second
};
extern unsigned long long g_orbis_sw_sync_n[8], g_orbis_sw_sync_ticks[8]; // vk-285-119 (GSRasterizer.cpp)
extern std::atomic<u64> g_orbis_vu1_cycles, g_orbis_vu1_runs; // vk-285-74 (MTVU.cpp)
extern std::atomic<int> g_orbis_vu1_dump_request;
static OrbisLoadMeasure s_orbis_load_measure;

static void OrbisMeasureLoad()
{
	static unsigned long long s_tsc = 0, s_prev[26] = {};
	static auto s_t = std::chrono::steady_clock::now();
	const unsigned long long tsc = __builtin_ia32_rdtsc();
	const auto now = std::chrono::steady_clock::now();
	const double sec = std::chrono::duration<double>(now - s_t).count();
	const unsigned long long cur[10] = {g_orbis_ee_waitgs_ticks, g_orbis_ee_stall_ticks, g_orbis_ee_vsyncq_ticks,
		g_orbis_ee_waitvu_ticks, g_orbis_ee_vuring_ticks, g_orbis_ee_throttle_ticks, g_orbis_gs_idle_ticks,
		g_orbis_gs_swsync_ticks, g_orbis_vu_idle_ticks, 0};
	OrbisLoadMeasure& m = s_orbis_load_measure;
	OrbisTickerWatch(); // vk-285-108
	m.nsw = std::min<u32>(PerformanceMetrics::GetGSSWThreadCount(), 16);
	m.measured = s_tsc != 0 && sec > 0.2;
	if (m.measured)
	{
		m.k = 1000.0 / static_cast<double>(tsc - s_tsc);
		for (int i = 0; i < 9; i++)
			m.v[i] = static_cast<double>(cur[i] - s_prev[i]) * m.k;
		m.ee_wait = m.v[0] + m.v[1] + m.v[2] + m.v[3] + m.v[4] + m.v[5];
		for (u32 i = 0; i < m.nsw; i++)
			m.sw_busy[i] = static_cast<double>(g_orbis_sw_busy_ticks[i] - s_prev[10 + i]) * m.k;
		const auto percent = [](double busy_ms) { return static_cast<float>(std::clamp(busy_ms / 10.0, 0.0, 100.0)); };
		s_orbis_load_ee = percent(1000.0 - m.ee_wait);
		s_orbis_load_gs = percent(1000.0 - m.v[6]);
		s_orbis_load_vu = THREAD_VU1 ? percent(1000.0 - m.v[8]) : 0.0f;
		s_orbis_load_valid = true;
	}
	for (int i = 0; i < 9; i++)
		s_prev[i] = cur[i];
	for (u32 i = 0; i < m.nsw; i++)
		s_prev[10 + i] = g_orbis_sw_busy_ticks[i];
	{
		// vk-285-119: the software renderer's syncs this second, by reason (GSRendererSW::Sync).
		static unsigned long long s_sync_n[8] = {}, s_sync_ticks[8] = {};
		for (int i = 0; i < 8; i++)
		{
			const unsigned long long n = g_orbis_sw_sync_n[i], t = g_orbis_sw_sync_ticks[i];
			m.sw_sync_n[i] = m.measured ? static_cast<double>(n - s_sync_n[i]) / sec : 0.0;
			m.sw_sync_ms[i] = m.measured ? static_cast<double>(t - s_sync_ticks[i]) * m.k : 0.0;
			s_sync_n[i] = n;
			s_sync_ticks[i] = t;
		}
	}
	s_tsc = tsc;
	s_t = now;

	// vk-285-74: VU1 cycles and program runs this second (MTVU.cpp).
	{
		static u64 s_cycles = 0, s_runs = 0;
		const u64 cycles = g_orbis_vu1_cycles.load(std::memory_order_relaxed);
		const u64 runs = g_orbis_vu1_runs.load(std::memory_order_relaxed);
		if (m.measured)
		{
			m.vu_mcycles = static_cast<double>(cycles - s_cycles) / 1e6 / sec;
			m.vu_runs = static_cast<double>(runs - s_runs) / sec;
		}
		s_cycles = cycles;
		s_runs = runs;
	}
	// vk-285-74: /data/PCSX2/flags/vudump appearing while the game runs asks the MTVU thread for a dump
	// of the VU1 recompiler (microVU.cpp OrbisVU1Dump). A flag already there at launch does nothing: move
	// it out and back in at the spot to study.
	{
		static int s_prev_flag = -1;
		const int flag = OrbisFlag("vudump") ? 1 : 0;
		if (s_prev_flag == 0 && flag == 1)
		{
			if (THREAD_VU1)
			{
				g_orbis_vu1_dump_request.store(1, std::memory_order_release);
				printf("[vudump] flag seen: the MTVU thread dumps after its current program\n");
			}
			else
				printf("[vudump] flag seen, but the dump needs the VU thread (MTVU) on\n");
			fflush(stdout);
		}
		s_prev_flag = flag;
	}
}

// The [load] line of the second OrbisMeasureLoad last measured, and the [rec]/[mcd] lines after it.
static void OrbisPrintLoad()
{
	const OrbisLoadMeasure& m = s_orbis_load_measure;
	if (!m.measured)
		return;
	const double* const v = m.v;
	// vk-285-74: the VU part adds the VU1 cycles (millions) and program runs of the second.
	printf("[load] ms/s ee: busy=%.0f waitgs=%.0f ringfull=%.0f vsyncq=%.0f waitvu=%.0f vuring=%.0f throttle=%.0f | gs: busy=%.0f swsync=%.0f | vu: busy=%.0f cyc=%.2fM runs=%.0f | sw busy=",
		1000.0 - m.ee_wait, v[0], v[1], v[2], v[3], v[4], v[5], 1000.0 - v[6], v[7], 1000.0 - v[8], m.vu_mcycles, m.vu_runs);
	for (u32 i = 0; i < m.nsw; i++)
		printf("%s%.0f", i ? "/" : "", m.sw_busy[i]);
	OrbisPrintCpu(); // eerec-285
	printf("\n");
	if (m.nsw != 0)
	{
		// vk-285-119: why the GS thread waited for the raster workers (count and ms this second, GSRendererSW::Sync):
		// at vsync, for the output, a texture the queued draws render into, a target they sample, a transfer or a
		// readback touching pages in use, anything else.
		double total = 0.0;
		for (int i = 0; i < 8; i++)
			total += m.sw_sync_n[i];
		if (total > 0.0)
			printf("[swsync] per s count/ms: vsync=%.0f/%.0f output=%.0f/%.0f texture=%.0f/%.0f target=%.0f/%.0f transfer=%.0f/%.0f "
				   "readback=%.0f/%.0f other=%.0f/%.0f\n",
				m.sw_sync_n[0], m.sw_sync_ms[0], m.sw_sync_n[1], m.sw_sync_ms[1], m.sw_sync_n[4], m.sw_sync_ms[4],
				m.sw_sync_n[5], m.sw_sync_ms[5], m.sw_sync_n[6], m.sw_sync_ms[6], m.sw_sync_n[7], m.sw_sync_ms[7],
				m.sw_sync_n[2], m.sw_sync_ms[2]);
	}
	{
		// vk-285-77: VU1 program lengths this second (MTVU.cpp).
		extern std::atomic<u32> g_orbis_vu1_run_hist[8];
		extern std::atomic<u32> g_orbis_vu1_run_max, g_orbis_vu1_long_pc, g_orbis_vu1_long_top, g_orbis_vu1_long_tpc;
		u32 h[8];
		u32 total = 0;
		for (int i = 0; i < 8; i++)
		{
			h[i] = g_orbis_vu1_run_hist[i].exchange(0, std::memory_order_relaxed);
			total += h[i];
		}
		if (total)
		{
			const u32 max = g_orbis_vu1_run_max.exchange(0, std::memory_order_relaxed);
			printf("[vuruns] cycles <1k/<4k/<16k/<64k/<256k/<1M/<2.9M/budget: %u/%u/%u/%u/%u/%u/%u/%u max=%uk", h[0], h[1], h[2],
				h[3], h[4], h[5], h[6], h[7], max / 1000);
			if (h[6] || h[7])
				printf(" last long: start %04x top %04x stopped at %04x", g_orbis_vu1_long_pc.load(std::memory_order_relaxed),
					g_orbis_vu1_long_top.load(std::memory_order_relaxed), g_orbis_vu1_long_tpc.load(std::memory_order_relaxed));
			// vk-285-80: MSCAL 0x00b0 runs (SotC's lighting program) and how many started with an odd count.
			extern std::atomic<u32> g_orbis_vu1_b0_runs, g_orbis_vu1_b0_odd;
			printf(" | b0 runs %u odd/0 %u", g_orbis_vu1_b0_runs.exchange(0, std::memory_order_relaxed),
				g_orbis_vu1_b0_odd.exchange(0, std::memory_order_relaxed));
			printf("\n");
		}
	}
	{
		// vk-285-90: EE-thread event rates this second (MTVU.cpp OrbisEEStats): event tests, VIF1 DMA
		// interrupts, VU1 programs' MTVU packets through the GIF fast path / through Execute, VIF1 unpacks
		// handed to the VU thread, and kicks of the VU thread. vk-285-93: and the MTVU ring's wraps, and the
		// wraps that had to wait for the VU thread to leave the lap's first packet (each one a ring
		// overwritten before vk-285-93).
		extern void OrbisEEStats(u64 out[8]);
		static u64 s_prev[8] = {};
		u64 cur[8];
		OrbisEEStats(cur);
		printf("[eestat] per s: evtests=%llu vif1int=%llu gif fast/slow=%llu/%llu unpacks=%llu kicks=%llu | mtvu wraps=%llu lapwaits=%llu (total %llu)\n",
			static_cast<unsigned long long>(cur[0] - s_prev[0]), static_cast<unsigned long long>(cur[1] - s_prev[1]),
			static_cast<unsigned long long>(cur[2] - s_prev[2]), static_cast<unsigned long long>(cur[3] - s_prev[3]),
			static_cast<unsigned long long>(cur[4] - s_prev[4]), static_cast<unsigned long long>(cur[5] - s_prev[5]),
			static_cast<unsigned long long>(cur[6] - s_prev[6]), static_cast<unsigned long long>(cur[7] - s_prev[7]),
			static_cast<unsigned long long>(cur[7]));
		for (int i = 0; i < 8; i++)
			s_prev[i] = cur[i];
		// vk-285-99: VIF1 DMA tags read this second, by ID (MTVU.cpp OrbisEETagStats).
		extern void OrbisEETagStats(u64 out[8]);
		static u64 s_tag_prev[8] = {};
		u64 tags[8];
		OrbisEETagStats(tags);
		printf("[eetags] per s: refe=%llu cnt=%llu next=%llu ref=%llu refs=%llu call=%llu ret=%llu end=%llu\n",
			static_cast<unsigned long long>(tags[0] - s_tag_prev[0]), static_cast<unsigned long long>(tags[1] - s_tag_prev[1]),
			static_cast<unsigned long long>(tags[2] - s_tag_prev[2]), static_cast<unsigned long long>(tags[3] - s_tag_prev[3]),
			static_cast<unsigned long long>(tags[4] - s_tag_prev[4]), static_cast<unsigned long long>(tags[5] - s_tag_prev[5]),
			static_cast<unsigned long long>(tags[6] - s_tag_prev[6]), static_cast<unsigned long long>(tags[7] - s_tag_prev[7]));
		for (int i = 0; i < 8; i++)
			s_tag_prev[i] = tags[i];
	}
	{
		// vk-285-27: the EE recompiler's churn this second (iR5900.cpp, vtlb.cpp), and the
		// rec_nocount switch.
		extern std::atomic<u32> g_orbis_rec_compiles, g_orbis_rec_discards, g_orbis_rec_page_resets,
			g_orbis_rec_full_resets, g_orbis_rec_faults;
		extern std::atomic<int> g_orbis_rec_nocount;
		static u32 s_rec_prev[5] = {};
		const u32 rec[5] = {g_orbis_rec_compiles.load(std::memory_order_relaxed),
			g_orbis_rec_discards.load(std::memory_order_relaxed), g_orbis_rec_page_resets.load(std::memory_order_relaxed),
			g_orbis_rec_faults.load(std::memory_order_relaxed), g_orbis_rec_full_resets.load(std::memory_order_relaxed)};
		g_orbis_rec_nocount.store(OrbisFlag("rec_nocount") ? 1 : 0, std::memory_order_relaxed);
		// vk-285-28: and how full each code cache is (MiB used/size), and the flexible memory left.
		extern size_t OrbisRecEEUsed(size_t* size);
		extern size_t OrbisRecIOPUsed(size_t* size);
		extern size_t OrbisMVUUsed(int vu, size_t* size);
		size_t ee_size = 0, iop_size = 0, vu0_size = 0, vu1_size = 0;
		const size_t ee_used = OrbisRecEEUsed(&ee_size), iop_used = OrbisRecIOPUsed(&iop_size);
		const size_t vu0_used = OrbisMVUUsed(0, &vu0_size), vu1_used = OrbisMVUUsed(1, &vu1_size);
		unsigned long long flex = 0;
		sceKernelAvailableFlexibleMemorySize(&flex);
		const double mib = 1.0 / 1048576.0;
		printf("[rec] compiles=%u discards=%u page_resets=%u faults=%u full_resets=%u nocount=%d | MiB ee %.1f/%.0f "
			   "iop %.1f/%.0f vu0 %.1f/%.0f vu1 %.1f/%.0f sw %.1f | flex free %.0f\n",
			rec[0] - s_rec_prev[0], rec[1] - s_rec_prev[1], rec[2] - s_rec_prev[2], rec[3] - s_rec_prev[3],
			rec[4] - s_rec_prev[4], g_orbis_rec_nocount.load(std::memory_order_relaxed), ee_used * mib,
			ee_size * mib, iop_used * mib, iop_size * mib, vu0_used * mib, vu0_size * mib, vu1_used * mib,
			vu1_size * mib, GSCodeReserve::GetMemoryUsed() * mib, flex * mib);
		std::memcpy(s_rec_prev, rec, sizeof(rec));
	}
	{
		// vk-285-31: memory card activity this second (MemoryCardFile.cpp), when there was any.
		extern std::atomic<u32> g_orbis_mcd_reads, g_orbis_mcd_writes, g_orbis_mcd_erases, g_orbis_mcd_flush_kib;
		extern std::atomic<unsigned long long> g_orbis_mcd_ticks;
		extern std::atomic<int> g_orbis_mcd_cached;
		static u32 s_mcd_prev[4] = {};
		static unsigned long long s_mcd_ticks_prev = 0;
		const u32 mcd[4] = {g_orbis_mcd_reads.load(std::memory_order_relaxed),
			g_orbis_mcd_writes.load(std::memory_order_relaxed), g_orbis_mcd_erases.load(std::memory_order_relaxed),
			g_orbis_mcd_flush_kib.load(std::memory_order_relaxed)};
		const unsigned long long mcd_ticks = g_orbis_mcd_ticks.load(std::memory_order_relaxed);
		if (std::memcmp(mcd, s_mcd_prev, sizeof(mcd)) != 0 || mcd_ticks != s_mcd_ticks_prev)
		{
			printf("[mcd] reads=%u writes=%u erases=%u written_back=%u KiB | %.1f ms | cached slots %#x\n",
				mcd[0] - s_mcd_prev[0], mcd[1] - s_mcd_prev[1], mcd[2] - s_mcd_prev[2], mcd[3] - s_mcd_prev[3],
				static_cast<double>(mcd_ticks - s_mcd_ticks_prev) * m.k, g_orbis_mcd_cached.load(std::memory_order_relaxed));
			std::memcpy(s_mcd_prev, mcd, sizeof(mcd));
			s_mcd_ticks_prev = mcd_ticks;
		}
	}
#ifdef ORBIS_VULKAN
	OrbisGSProfStart(); // vk-285-24: starts sampling this (GS) thread once /data/PCSX2/gsprof exists
	OrbisEEProfMark(); // vk-285-8: the profiler's sample count at this [load] line
	extern void OrbisVkGpuProfSecond(); // GSDeviceVK.cpp
	OrbisVkGpuProfSecond(); // vk-285-94: the GPU profiler's lines (flags/gpuprof)
#endif
}
void OrbisDiagTexture(const char* tag, GSTexture* t);

namespace
{
	struct OrbisPresentMode
	{
		const char* name;
		int tv_shader; // index into s_tv_shader_indices
		int presharp; // native-res unsharp mask (ShadeBoost slot), percent, 0 = off
		int sharp; // FSR sharpening (RCAS) amount, percent, TVShader 6 only
	};
	OrbisPresentMode s_orbis_modes[] = {
		{"FSR", 6, 50, 50},
		{"FSR SOFT", 7, 0, 0},
		{"CLASSIC", 0, 60, 0},
		{"CRT", 5, 0, 0},
	};
	constexpr int ORBIS_NUM_MODES = static_cast<int>(sizeof(s_orbis_modes) / sizeof(s_orbis_modes[0]));
	int s_orbis_mode = 0;
	int s_orbis_label_frames = 0;
	bool s_orbis_fps_box = true;
	bool s_orbis_gl = false;
	int s_orbis_res[4] = {}; // vk-285-12: source w/h and on-screen w/h of the last presented frame (GS thread)
} // namespace

#ifdef ORBIS_VULKAN
// vk-285-12: the native-resolution pre-sharpen needs the port's Vulkan shadeboost.glsl (it
// carries the ORBIS_PRESHARP marker). With PCSX2's stock one, ShadeBoost would desaturate.
static bool OrbisVkPresharpShader()
{
	static int s_ok = -1;
	if (s_ok < 0)
	{
		s_ok = 0;
		// vk-285-115 (AI-assisted): the file, else the eboot's built-in copy (OrbisGSShaders.h), as the device reads it.
		std::string text;
		if (OrbisReadShaderSource(EmuFolders::Resources, "shaders/vulkan/shadeboost.glsl", &text))
			s_ok = text.find("ORBIS_PRESHARP") != std::string::npos ? 1 : 0;
		printf("[present] Vulkan shadeboost.glsl: %s\n", s_ok ? "native pre-sharpen (ORBIS_PRESHARP)" : "stock, pre-sharpen off");
	}
	return s_ok == 1;
}
#endif

static void OrbisApplyMode(int m, bool announce)
{
	m = ((m % ORBIS_NUM_MODES) + ORBIS_NUM_MODES) % ORBIS_NUM_MODES;
	s_orbis_mode = m;
	const OrbisPresentMode& pm = s_orbis_modes[m];
	GSConfig.TVShader = pm.tv_shader;
	GSConfig.LinearPresent = GSPostBilinearMode::BilinearSmooth;
	GSConfig.ShadeBoost = (pm.presharp > 0);
	GSConfig.ShadeBoost_Saturation = static_cast<u8>(std::clamp(pm.presharp / 2, 0, 100));
#ifdef ORBIS_VULKAN
	// The port's shadeboost.glsl reads the saturation as a pre-sharpen amount. On Vulkan
	// that needs the port's shader (vk-285-12); PCSX2's stock one would desaturate.
	if (g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::Vulkan && !OrbisVkPresharpShader())
		GSConfig.ShadeBoost = false;
#endif
	g_orbis_present_param[0] = static_cast<float>(pm.sharp) / 100.0f;
	if (announce)
		s_orbis_label_frames = 120;
	printf("[present] mode=%d %s tv=%d presharp=%d sharp=%d split=%.0f aspect=%d\n", m, pm.name, pm.tv_shader,
		pm.presharp, pm.sharp, g_orbis_present_param[1], static_cast<int>(EmuConfig.CurrentAspectRatio));
	fflush(stdout);
}

static void OrbisLiveTune()
{
	OrbisCpuSample(1); // eerec-285: GS thread
#ifdef ORBIS_VULKAN
	// PS5 Vulkan build: the same present modes, live.ini tuning, FPS box and
	// [perf]/[load] lines on GSDeviceVK.
	if (!g_gs_device || (g_gs_device->GetRenderAPI() != RenderAPI::OpenGL &&
							g_gs_device->GetRenderAPI() != RenderAPI::Vulkan))
		return;
#else
	if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::OpenGL)
		return;
#endif
	static bool s_init = false;
	static int s_seen_cycle = 0;
	if (!s_init)
	{
		s_init = true;
		s_orbis_gl = true;
		s_seen_cycle = g_orbis_filter_cycle.load(std::memory_order_relaxed);
		int m = 0;
		for (int i = 0; i < ORBIS_NUM_MODES; i++)
		{
			if (s_orbis_modes[i].tv_shader == static_cast<int>(GSConfig.TVShader))
			{
				m = i;
				break;
			}
		}
		s_orbis_fps_box = !OrbisFlag("nofps");
#ifdef ORBIS_VULKAN
		// vk-285-12: on Vulkan the driver's queue submit waits for the GPU, so GPU time is
		// GS-thread time. FSR starts as native pre-sharpen + EASU; the output RCAS (four more
		// EASU evaluations per pixel at 4K) is opt-in with live.ini sharp=.
		if (g_gs_device->GetRenderAPI() == RenderAPI::Vulkan)
			s_orbis_modes[0].sharp = 0;
#endif
		OrbisApplyMode(m, false);
	}
	const int cyc = g_orbis_filter_cycle.load(std::memory_order_relaxed);
	if (cyc != s_seen_cycle)
	{
		s_seen_cycle = cyc;
		OrbisApplyMode(s_orbis_mode + 1, true);
	}

	static unsigned s_poll = 0;
	if ((s_poll++ % 30) != 0)
		return;
	{
		// eerec-285: gs.ini applies live once a changed file reads the same twice (a half-written one is skipped)
		static bool s_gsini_init = false;
		static std::string s_gsini_applied, s_gsini_pending;
		// vk-285-105: the files as main-boot's ticker last read them (OrbisCachedRead, once a second), so this
		// thread doesn't open files on /data (an open there now and then takes 30+ ms).
		std::string g;
		{
			std::string b;
			if (OrbisCachedRead("/data/PCSX2/gs.ini", b))
				g.assign(b, 0, std::min<size_t>(b.size(), 2048));
		}
		// vk-285-32: and the game's own settings file (main-boot.cpp), so editing it applies live too.
		extern const char* OrbisGameIniPath();
		if (const char* gp = OrbisGameIniPath(); gp && *gp)
		{
			std::string b;
			if (OrbisCachedRead(gp, b))
			{
				g.append("\n#game\n");
				g.append(b, 0, std::min<size_t>(b.size(), 2048));
			}
		}
		if (!s_gsini_init)
		{
			s_gsini_init = true;
			s_gsini_applied = s_gsini_pending = g;
			unsigned long long m = 0;
			const int rc = scePthreadGetaffinity(pthread_self(), &m);
			printf("[cpu] GS thread on cpu %d, affinity %#llx (rc=%d), cpumode=%d\n", sceKernelGetCurrentCpu(), m, rc,
				sceKernelGetCpumode());
			fflush(stdout);
		}
		else if (g != s_gsini_applied)
		{
			if (g == s_gsini_pending)
			{
				s_gsini_applied = g;
				g_orbis_gsini_reload.store(1, std::memory_order_release);
				printf("[gsini] gs.ini or the game's settings changed (%zu bytes): applying at the next vsync\n", g.size());
				fflush(stdout);
			}
			else
				s_gsini_pending = g;
		}
	}
	static std::string s_last;
	std::string cur;
	if (g_orbis_live_reapply.exchange(0, std::memory_order_acq_rel))
		s_last = "\x01"; // eerec-285: gs.ini was applied, so apply live.ini again (present mode, pin)
	{
		std::string b; // vk-285-105: as the ticker last read it (see gs.ini above)
		if (OrbisCachedRead("/data/PCSX2/live.ini", b))
			cur.assign(b, 0, std::min<size_t>(b.size(), 1024));
	}
	if (cur == s_last)
		return;
	s_last = cur;
	int mode = -1;
	int pin_request = -1; // vk-285-87: issued once every key is read
	size_t pos = 0;
	while (pos < cur.size())
	{
		size_t eol = cur.find('\n', pos);
		if (eol == std::string::npos)
			eol = cur.size();
		const std::string line = cur.substr(pos, eol - pos);
		pos = eol + 1;
		char key[32] = {};
		float v = 0.0f;
		if (sscanf(line.c_str(), " %31[a-z_] = %f", key, &v) != 2)
			continue;
		const std::string k(key);
		if (k == "mode")
			mode = static_cast<int>(v);
		else if (k == "sharp")
			s_orbis_modes[0].sharp = std::clamp(static_cast<int>(v * 100.0f + 0.5f), 0, 100);
		else if (k == "presharp")
			s_orbis_modes[0].presharp = std::clamp(static_cast<int>(v * 100.0f + 0.5f), 0, 200);
		else if (k == "classic_sharp")
			s_orbis_modes[2].presharp = std::clamp(static_cast<int>(v * 100.0f + 0.5f), 0, 200);
		else if (k == "split")
			g_orbis_present_param[1] = (v > 0.0f && v <= 1.0f) ? v * static_cast<float>(g_gs_device->GetWindowWidth()) : std::max(v, 0.0f);
		else if (k == "fps")
			s_orbis_fps_box = (v != 0.0f);
		else if (k == "deinterlace")
			GSConfig.InterlaceMode = static_cast<GSInterlaceMode>(std::clamp(static_cast<int>(v), 0, static_cast<int>(GSInterlaceMode::Count) - 1));
		else if (k == "fxaa")
			GSConfig.FXAA = (v != 0.0f);
		else if (k == "antiblur")
			GSConfig.PCRTCAntiBlur = (v != 0.0f);
		else if (k == "swtex")
			g_orbis_swtex = std::clamp(static_cast<int>(v), 0, 2);
		else if (k == "testpat")
			g_orbis_testpat = (v != 0.0f);
		else if (k == "upload")
			g_orbis_upload_mode = std::clamp(static_cast<int>(v), 0, 2);
		else if (k == "diag")
			g_orbis_diag = (v != 0.0f);
		else if (k == "perf")
			g_orbis_perf = (v != 0.0f);
		else if (k == "pin") // eerec-285; vk-285-87: 3, the layout below (applied after the whole file)
			pin_request = std::clamp(static_cast<int>(v), 0, 3);
		else if (k == "pin_ee") // vk-285-87
			s_orbis_pin_cpu[ORBIS_PIN_EE].store(static_cast<int>(v), std::memory_order_release);
		else if (k == "pin_gs")
			s_orbis_pin_cpu[ORBIS_PIN_GS].store(static_cast<int>(v), std::memory_order_release);
		else if (k == "pin_vu")
			s_orbis_pin_cpu[ORBIS_PIN_VU].store(static_cast<int>(v), std::memory_order_release);
		else if (k == "pin_rec")
			s_orbis_pin_cpu[ORBIS_PIN_REC].store(static_cast<int>(v), std::memory_order_release);
		else if (k == "pin_q")
			s_orbis_pin_cpu[ORBIS_PIN_QUEUE].store(static_cast<int>(v), std::memory_order_release);
		else if (k == "pin_all")
			s_orbis_pin_all.store(v != 0.0f ? 1 : 0, std::memory_order_release);
#ifdef ORBIS_VULKAN
		else if (k == "widescreen") // vk-285-12: R&C PAL 16:9 patch (pcsx2/OrbisWidescreen.cpp)
			g_orbis_widescreen.store(v != 0.0f ? 1 : 0, std::memory_order_release);
#endif
		else if (k == "aspect")
		{
			const int a = static_cast<int>(v);
			const AspectRatioType t = (a == 0) ? AspectRatioType::Stretch : (a == 4) ? AspectRatioType::R4_3 :
				(a == 16) ? AspectRatioType::R16_9 : AspectRatioType::RAuto4_3_3_2;
			EmuConfig.CurrentAspectRatio = t;
			GSConfig.AspectRatio = t;
		}
	}
	if (pin_request >= 0)
		g_orbis_pin_request.store(pin_request, std::memory_order_release);
	printf("[present] live.ini (%zu bytes) applied: deinterlace=%d fxaa=%d antiblur=%d\n", cur.size(),
		static_cast<int>(GSConfig.InterlaceMode), static_cast<int>(GSConfig.FXAA), static_cast<int>(GSConfig.PCRTCAntiBlur));
	printf("[present] swtex=%d testpat=%d upload=%d diag=%d perf=%d\n", g_orbis_swtex, g_orbis_testpat, g_orbis_upload_mode,
		g_orbis_diag, g_orbis_perf); // eerec-280
	OrbisApplyMode(mode >= 0 ? mode : s_orbis_mode, mode >= 0);
}

// Test build 1 (vk-285-55): main-boot.cpp's build number (0: not a testing build) and the watermark
// it rasterized once the game was picked; the settings log's writer (frontend/fe_ps5.cpp).
extern "C" int g_orbis_test_build;
extern std::vector<u32> g_orbis_watermark;
extern int g_orbis_watermark_w, g_orbis_watermark_h;
extern "C" void orbis_event_log(const char* line) __attribute__((weak));

// Test build 1: once a minute, the minute's frame rate, speed and thread loads on one line of the
// settings log (logs/settings.log), so a report shows how a game ran without reading the per-second
// [perf] lines. Fed once a second from OrbisGLOSD.
static void OrbisPerfMinute(unsigned fps, float speed, float ee, float gs, float vu)
{
	static unsigned s_n = 0, s_min = ~0u, s_max = 0, s_slow = 0;
	static double s_fps = 0, s_speed = 0, s_ee = 0, s_gs = 0, s_vu = 0;
	s_n++;
	s_fps += fps;
	s_speed += speed;
	s_ee += ee;
	s_gs += gs;
	s_vu += vu;
	s_min = std::min(s_min, fps);
	s_max = std::max(s_max, fps);
	s_slow += speed < 95.0f ? 1 : 0;
	if (s_n < 60)
		return;
	if (orbis_event_log)
	{
		char line[256];
		snprintf(line, sizeof(line),
			"perf, last %u s: %.1f fps (min %u, max %u), speed %.0f%%, below 95%% for %u s; CPU thread load EE %.0f%% GS %.0f%% VU %.0f%%; %dx",
			s_n, s_fps / s_n, s_min, s_max, s_speed / s_n, s_slow, s_ee / s_n, s_gs / s_n, s_vu / s_n,
			static_cast<int>(GSConfig.UpscaleMultiplier));
		OrbisDeferredEvent(line); // vk-285-107: the ticker writes it (a file write here held the GS thread)
#ifdef ORBIS_VULKAN
		// vk-285-113: the minute's GPU readbacks (GSDownloadTextureVK), on a line of their own so the line above keeps
		// its shape: how many a second, the megabytes, what the GS thread waited for them, and the longest wait.
		static unsigned long long s_rb_n0 = 0, s_rb_bytes0 = 0, s_rb_wait0 = 0;
		const unsigned long long rb_n = g_orbis_readback_n - s_rb_n0, rb_bytes = g_orbis_readback_bytes - s_rb_bytes0,
								 rb_wait = g_orbis_readback_wait_ns - s_rb_wait0;
		s_rb_n0 = g_orbis_readback_n;
		s_rb_bytes0 = g_orbis_readback_bytes;
		s_rb_wait0 = g_orbis_readback_wait_ns;
		const double rb_max_ms = g_orbis_readback_wait_max_min_ns / 1e6;
		g_orbis_readback_wait_max_min_ns = 0;
		if (rb_n != 0)
		{
			char rb_line[192];
			snprintf(rb_line, sizeof(rb_line), "readbacks, last %u s: %.1f/s (%.1f MB/s), the GS thread waited %.0f ms/s for them (longest %.1f ms), mode %d",
				s_n, static_cast<double>(rb_n) / s_n, static_cast<double>(rb_bytes) / 1048576.0 / s_n,
				static_cast<double>(rb_wait) / 1e6 / s_n, rb_max_ms, static_cast<int>(GSConfig.HWDownloadMode));
			OrbisDeferredEvent(rb_line);
		}
#endif
	}
	s_n = s_slow = s_max = 0;
	s_min = ~0u;
	s_fps = s_speed = s_ee = s_gs = s_vu = 0;
}

#ifdef ORBIS_VULKAN
void OrbisVkPresentBlend(GSTexture* tex, const GSVector4& sRect, const GSVector4& dRect); // GSDeviceVK.cpp

// Test build 1: the TESTING watermark in the middle of the game's picture, faint (its opacity is in
// the image), sized for the display's height. The texture is made once per GS device.
static void OrbisWatermark()
{
	if (g_orbis_test_build <= 0 || g_orbis_watermark.empty() || !g_gs_device ||
		g_gs_device->GetRenderAPI() != RenderAPI::Vulkan)
		return;
	static GSTexture* s_tex = nullptr;
	static GSDevice* s_dev = nullptr;
	const int w = g_orbis_watermark_w, h = g_orbis_watermark_h;
	if (s_dev != g_gs_device.get())
	{
		s_dev = g_gs_device.get();
		s_tex = s_dev->CreateTexture(w, h, 1, GSTexture::Format::Color);
		if (s_tex)
			s_tex->Update(GSVector4i(0, 0, w, h), g_orbis_watermark.data(), w * 4);
		printf("[present] testing watermark %dx%d: %s\n", w, h, s_tex ? "on" : "no texture");
		fflush(stdout);
	}
	if (!s_tex)
		return;
	const float ww = static_cast<float>(g_gs_device->GetWindowWidth());
	const float wh = static_cast<float>(g_gs_device->GetWindowHeight());
	const float k = wh / 2160.0f;
	const float dw = static_cast<float>(w) * k, dh = static_cast<float>(h) * k;
	const float x0 = std::floor((ww - dw) * 0.5f), y0 = std::floor((wh - dh) * 0.5f);
	OrbisVkPresentBlend(s_tex, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), GSVector4(x0, y0, x0 + dw, y0 + dh));
}
#endif

// vk-285-113: the settings page's address as a QR code over the game while g_orbis_qr_show is set (the pad thread
// toggles it: L2 + D-pad down held for 2 s). A white panel with black modules and the address under it, right of
// the middle of the screen; the game goes on behind it. Made once per showing, at a size for the display's height.
static void OrbisDrawQrPanel()
{
	static GSTexture* s_tex = nullptr;
	static GSDevice* s_dev = nullptr;
	static GSDevice* s_tex_dev = nullptr; // the device s_tex came from
	static int s_w = 0, s_h = 0, s_for_height = 0;
	static bool s_showing = false, s_ok = false;
	if (!g_orbis_qr_show.load(std::memory_order_relaxed))
	{
		s_showing = false;
		return;
	}
	if (!g_gs_device)
		return;
	const int wh = static_cast<int>(g_gs_device->GetWindowHeight());
	if (!s_showing || s_dev != g_gs_device.get() || s_for_height != wh)
	{
		s_showing = true;
		s_dev = g_gs_device.get();
		s_for_height = wh;
		s_ok = false;
		std::vector<unsigned char> modules;
		int size = 0;
		std::string shown;
		const bool have = orbis_web_qr(modules, size, shown);
		const float k = static_cast<float>(wh) / 2160.0f;
		const int mp = std::max(3, static_cast<int>(10.0f * k + 0.5f)); // pixels a module
		const int ts = std::max(2, static_cast<int>(4.0f * k + 0.5f)); // text scale (a glyph is 5x7 units, 6 wide with its gap)
		const int quiet = 4 * mp;
		const int qr_px = have ? size * mp : 0;
		const char* const title = "PS5SX2 SETTINGS";
		const char* const hint = "HOLD L2 + DOWN TO CLOSE";
		const std::string addr = have ? shown : std::string("NO NETWORK ADDRESS");
		const auto text_w = [](const char* str, int scale) { return static_cast<int>(std::strlen(str)) * 6 * scale; };
		const int w = std::max({qr_px + 2 * quiet, text_w(title, ts) + 2 * quiet, text_w(addr.c_str(), ts) + 2 * quiet,
			text_w(hint, std::max(1, ts - 1)) + 2 * quiet});
		const int line = 9 * ts;
		const int h = quiet + line + (have ? qr_px + quiet / 2 : 0) + line + std::max(1, ts - 1) * 9 + quiet;
		s_w = w;
		s_h = h;
		std::vector<u32> pix(static_cast<size_t>(w) * h, 0xFFFFFFFFu);
		const auto centered = [&](int y, const char* str, int scale, u32 color) {
			orbis_text_rgba(pix.data(), static_cast<unsigned>(w), static_cast<unsigned>(h),
				static_cast<unsigned>(std::max(0, (w - text_w(str, scale)) / 2)), static_cast<unsigned>(y), str, static_cast<unsigned>(scale), color);
		};
		int y = quiet / 2;
		centered(y, title, ts, 0xFF303030u);
		y += line;
		if (have)
		{
			const int x0 = (w - qr_px) / 2;
			for (int my = 0; my < size; my++)
				for (int mx = 0; mx < size; mx++)
					if (modules[static_cast<size_t>(my) * size + mx])
						for (int py = 0; py < mp; py++)
							for (int px = 0; px < mp; px++)
								pix[static_cast<size_t>(y + my * mp + py) * w + x0 + mx * mp + px] = 0xFF000000u;
			y += qr_px + quiet / 2;
		}
		centered(y, addr.c_str(), ts, 0xFF000000u);
		y += line;
		centered(y, hint, std::max(1, ts - 1), 0xFF606060u);
		if (s_tex && s_tex_dev == s_dev)
			s_dev->Recycle(s_tex); // back to the device's pool (a new showing may need another size)
		s_tex = s_dev->CreateTexture(w, h, 1, GSTexture::Format::Color);
		s_tex_dev = s_dev;
		if (s_tex)
		{
			s_tex->Update(GSVector4i(0, 0, w, h), pix.data(), w * 4);
			s_ok = true;
		}
		printf("[present] settings QR %dx%d (%s): %s\n", w, h, have ? shown.c_str() : "no address", s_tex ? "on" : "no texture");
		fflush(stdout);
	}
	if (!s_ok || !s_tex)
		return;
	const float ww = static_cast<float>(g_gs_device->GetWindowWidth());
	const float whf = static_cast<float>(wh);
	const float x1 = ww - 60.0f * whf / 2160.0f, x0 = x1 - static_cast<float>(s_w);
	const float y0 = std::floor((whf - static_cast<float>(s_h)) * 0.5f), y1 = y0 + static_cast<float>(s_h);
	g_gs_device->PresentRect(s_tex, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), nullptr, GSVector4(x0, y0, x1, y1), PresentShader::COPY, 0.0f, Nearest);
}

static void OrbisGLOSD()
{
	if (!s_orbis_gl || !g_gs_device)
		return;
	static u64 s_count = 0;
	static auto s_t0 = std::chrono::steady_clock::now();
	static unsigned s_fps = 0;
	s_count++;
	const auto now = std::chrono::steady_clock::now();
	const double dt = std::chrono::duration<double>(now - s_t0).count();
	if (dt >= 1.0)
	{
		s_fps = static_cast<unsigned>(static_cast<double>(s_count) / dt + 0.5);
		// vk-285-72: the loads come from the [load] line's counters (s_orbis_load_*), measured first; the
		// line itself still prints after [perf].
		const bool print = g_orbis_perf || g_orbis_test_build > 0; // eerec-280; test build 1: always in testing builds
		OrbisMeasureLoad();
		const float ee = s_orbis_load_valid ? s_orbis_load_ee : static_cast<float>(PerformanceMetrics::GetCPUThreadUsage());
		const float gs = s_orbis_load_valid ? s_orbis_load_gs : PerformanceMetrics::GetGSThreadUsage();
		const float vu = s_orbis_load_valid ? s_orbis_load_vu : PerformanceMetrics::GetVUThreadUsage();
		OrbisPerfMinute(s_fps, PerformanceMetrics::GetSpeed(), ee, gs, vu); // test build 1
		OrbisAutoProfSecond(PerformanceMetrics::GetSpeed(), ee, gs, vu); // vk-285-118: a slow stretch samples the busy thread
		OrbisReadbackAutoSecond(PerformanceMetrics::GetSpeed(), s_fps); // vk-285-118: older firmware's readback stall
		if (print)
		{
			printf("[perf] fps=%u vfreq=%.2f speed=%.0f ee=%.0f gs=%.0f vu=%.0f ft=%.1f/%.1f/%.1f sw=", s_fps,
				GetVerticalFrequency(), PerformanceMetrics::GetSpeed(), ee, gs, vu,
				PerformanceMetrics::GetMinimumFrameTime(), PerformanceMetrics::GetAverageFrameTime(),
				PerformanceMetrics::GetMaximumFrameTime());
			for (u32 i = 0; i < PerformanceMetrics::GetGSSWThreadCount(); i++)
				printf("%s%.0f", i ? "/" : "", PerformanceMetrics::GetGSSWThreadUsage(i));
			printf("\n");
			OrbisPrintLoad(); // eerec-281
		}
		// vk-285-100: the EE switches' flag files, [cpuclk] every 5 s and the eediag lines (OrbisEEDiag.cpp);
		// the flags are read whether or not the lines print.
		OrbisEEDiagSecond(print);
		if (print)
			fflush(stdout);
		s_count = 0;
		s_t0 = now;
	}
	// vk-285-113: the frame rate four times a second for the graph: 240 samples, a minute, two pixels each.
	constexpr int GRAPH_N = 240;
	static float s_hist[GRAPH_N] = {};
	static int s_hist_n = 0;
	static u64 s_q_count = 0;
	static auto s_q_t0 = now;
	static bool s_hist_dirty = false;
	s_q_count++;
	{
		const double qdt = std::chrono::duration<double>(now - s_q_t0).count();
		if (qdt >= 0.25)
		{
			const float sample = static_cast<float>(static_cast<double>(s_q_count) / qdt);
			const int copies = std::min(8, std::max(1, static_cast<int>(qdt / 0.25))); // a stall shows as a flat stretch
			for (int i = 0; i < copies; i++)
			{
				std::memmove(s_hist, s_hist + 1, (GRAPH_N - 1) * sizeof(float));
				s_hist[GRAPH_N - 1] = sample;
			}
			s_hist_n = std::min(GRAPH_N, s_hist_n + copies);
			s_q_count = 0;
			s_q_t0 = now;
			s_hist_dirty = true;
		}
	}

	// vk-285-113: PS5SX2/Overlay (0 none, 1 FPS, 2 FPS and loads) wins over live.ini's fps= and perf= when set.
	const int overlay = g_orbis_overlay_mode.load(std::memory_order_relaxed);
	const bool show_fps = overlay >= 0 ? overlay >= 1 : s_orbis_fps_box;
	const bool show_loads = overlay >= 0 ? overlay >= 2 : g_orbis_perf != 0;
	const bool graph = g_orbis_fps_graph.load(std::memory_order_relaxed) != 0;

	char text[48] = {};
	bool have_text = true;
	if (g_orbis_osd_text_frames.load(std::memory_order_acquire) > 0) // eerec-282
	{
		g_orbis_osd_text_frames.fetch_sub(1, std::memory_order_relaxed);
		snprintf(text, sizeof(text), "%s", g_orbis_osd_text);
	}
	else if (s_orbis_label_frames > 0)
	{
		s_orbis_label_frames--;
		snprintf(text, sizeof(text), "%s", s_orbis_modes[s_orbis_mode].name);
	}
	else if (show_fps && show_loads) // eerec-280; vk-285-72: the [load] line's loads
		snprintf(text, sizeof(text), "%u FPS EE%u GS%u VU%u", s_fps,
			static_cast<unsigned>((s_orbis_load_valid ? s_orbis_load_ee : PerformanceMetrics::GetCPUThreadUsage()) + 0.5),
			static_cast<unsigned>((s_orbis_load_valid ? s_orbis_load_gs : PerformanceMetrics::GetGSThreadUsage()) + 0.5f),
			static_cast<unsigned>((s_orbis_load_valid ? s_orbis_load_vu : PerformanceMetrics::GetVUThreadUsage()) + 0.5f));
	else if (show_fps)
		snprintf(text, sizeof(text), "FPS %u", s_fps);
	else
		have_text = false;
	if (!have_text && !graph)
	{
		OrbisDrawQrPanel();
		return;
	}

	// vk-285-12: second line, the game's frame size and the size it is drawn at on screen.
	char text2[48] = {};
	if (have_text && show_fps && s_orbis_res[0] > 0)
		snprintf(text2, sizeof(text2), "%dx%d > %dx%d", s_orbis_res[0], s_orbis_res[1], s_orbis_res[2], s_orbis_res[3]);

	// vk-285-113: the box is the text (one line 50 px, two 80) and, when the graph is on, the graph under it.
	constexpr int TW = 480, TEXT_H = 80, SCALE = 3, LINE2_Y = 45, GRAPH_H = 96, TH = TEXT_H + GRAPH_H;
	static GSTexture* s_tex = nullptr;
	static GSDevice* s_dev = nullptr;
	static char s_last[96] = {};
	static int s_box_w = TW, s_box_h = TEXT_H;
	if (s_dev != g_gs_device.get())
	{
		s_dev = g_gs_device.get();
		s_tex = s_dev->CreateTexture(TW, TH, 1, GSTexture::Format::Color);
		s_last[0] = 0;
	}
	if (!s_tex)
		return;
	char key[96];
	snprintf(key, sizeof(key), "%s|%s|%d", have_text ? text : "", text2, graph ? 1 : 0);
	if (strcmp(key, s_last) != 0 || (graph && s_hist_dirty))
	{
		static u32 s_buf[TW * TH];
		const auto width = [](const char* str) {
			int tw = 0;
			for (const char* c = str; *c; c++)
			{
				const bool known = (*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
								   *c == '>' || *c == ':' || *c == '.' || *c == '-' || *c == '/' || *c == '%';
				tw += known ? 6 * SCALE : 2 * SCALE;
			}
			return tw;
		};
		const int text_h = !have_text ? 0 : (text2[0] ? TEXT_H : 50);
		s_box_w = graph ? TW : std::min(TW, std::max(width(text), width(text2)) + 30 - SCALE);
		s_box_h = text_h + (graph ? GRAPH_H : 0);
		std::fill(std::begin(s_buf), std::end(s_buf), 0xFF202020u);
		if (have_text)
		{
			orbis_text_rgba(s_buf, TW, TH, 15, 12, text, SCALE, 0xFF00FFFFu);
			if (text2[0])
				orbis_text_rgba(s_buf, TW, TH, 15, LINE2_Y, text2, SCALE, 0xFFE0E0E0u);
		}
		if (graph)
		{
			// The bars in blue on the dark box; the scale is 0-60 fps, or 30 fps steps above that. Faint lines at 30 and 60.
			constexpr u32 BLUE = 0xFFFF7A1Eu; // R 1E, G 7A, B FF (0xAABBGGRR)
			constexpr u32 GRID = 0xFF484848u;
			const int gy0 = text_h + 4, gh = GRAPH_H - 8; // the graph's rows: gy0 .. gy0 + gh - 1
			float peak = 60.0f;
			for (int i = GRAPH_N - s_hist_n; i < GRAPH_N; i++)
				peak = std::max(peak, s_hist[i]);
			const float scale_max = std::ceil(peak / 30.0f) * 30.0f;
			const auto row_of = [&](float fps) { return gy0 + gh - 1 - static_cast<int>(std::min(fps, scale_max) / scale_max * (gh - 1) + 0.5f); };
			for (const float mark : {30.0f, 60.0f})
			{
				if (mark > scale_max)
					continue;
				const int y = row_of(mark);
				for (int x = 0; x < TW; x++)
					s_buf[y * TW + x] = GRID;
			}
			for (int i = 0; i < GRAPH_N; i++)
			{
				if (i < GRAPH_N - s_hist_n)
					continue;
				const int top = row_of(s_hist[i]);
				for (int y = top; y < gy0 + gh; y++)
					for (int x = i * 2; x < i * 2 + 2; x++)
						s_buf[y * TW + x] = BLUE;
			}
			char label[8];
			snprintf(label, sizeof(label), "%d", static_cast<int>(scale_max));
			orbis_text_rgba(s_buf, TW, TH, 3, static_cast<unsigned>(gy0 + 1), label, 2, 0xFFC0C0C0u);
		}
		s_tex->Update(GSVector4i(0, 0, TW, TH), s_buf, TW * 4);
		snprintf(s_last, sizeof(s_last), "%s", key);
		s_hist_dirty = false;
	}
	const float ww = static_cast<float>(g_gs_device->GetWindowWidth());
	const float wh = static_cast<float>(g_gs_device->GetWindowHeight());
	const float x1 = ww - 20.0f, x0 = x1 - static_cast<float>(s_box_w);
#ifdef ORBIS_VULKAN
	// Vulkan's window origin is the top-left corner: 40 px below the top edge.
	(void)wh;
	const float y0 = 40.0f, y1 = y0 + static_cast<float>(s_box_h);
#else
	const float y1 = wh - 40.0f, y0 = y1 - static_cast<float>(s_box_h); // GL lower-left origin: 40 px below the top edge
#endif
	g_gs_device->PresentRect(s_tex, GSVector4(0.0f, 0.0f, static_cast<float>(s_box_w) / TW, static_cast<float>(s_box_h) / TH),
		nullptr, GSVector4(x0, y0, x1, y1), PresentShader::COPY, 0.0f, Nearest);
	OrbisDrawQrPanel();
}
// ---- end eerec-278 ----

static std::deque<std::thread> s_screenshot_threads;
static std::mutex s_screenshot_threads_mutex;

std::unique_ptr<GSRenderer> g_gs_renderer;

// Since we read this on the EE thread, we can't put it in the renderer, because
// we might be switching while the other thread reads it.
static GSVector4 s_last_draw_rect;

// Last time we reset the renderer due to a GPU crash, if any.
static Common::Timer::Value s_last_gpu_reset_time;

// Screen alignment
static GSDisplayAlignment s_display_alignment = GSDisplayAlignment::Center;

GSRenderer::GSRenderer()
	: m_shader_time_start(Common::Timer::GetCurrentValue())
{
	s_last_draw_rect = GSVector4::zero();
}

GSRenderer::~GSRenderer() = default;

void GSRenderer::Reset(bool hardware_reset)
{
	// Clear the current display texture.
	// Orbis: Null/SW renderers have no GSDevice (g_gs_device == nullptr).
	if (hardware_reset && g_gs_device)
		g_gs_device->ClearCurrent();

	GSState::Reset(hardware_reset);
}

void GSRenderer::Destroy()
{
	GSCapture::EndCapture();
}

void GSRenderer::UpdateRenderFixes()
{
}

// PS5 port (vk-285-106): a slow vsync's time, phase by phase. vk-285-101 to 105 logged a ~33 ms GS-thread stall
// in ~6% of the [vstime] windows (and a ~16.7 ms one in ~9%), inside the merge or the present, which no
// per-second counter explained; vk-285-104's deferred log did not remove it. VSync stamps the TSC at the end of
// each phase and keeps the GS thread's Vulkan waits by kind (VKOrbisTiming.h, cumulative) from its start; a vsync
// over 20 ms prints a [vsslow] line: its total, the gap since the previous vsync ended (the frame's GS work, and
// any wait for the EE), each phase's time, and each Vulkan wait kind's time (calls) inside it. A phase that
// didn't run shows no time. Cheap when nothing is slow: ~12 TSC reads and two 160-byte copies a vsync.
// Needs proper testing.
namespace
{
enum OrbisVsPhase : int
{
	ORBIS_VS_TUNE, // OrbisLiveTune (live.ini, gs.ini, the game's ini)
	ORBIS_VS_DUMP, // the dump checks and the duplicate-frame check
	ORBIS_VS_OUTPUT, // Merge's GetOutput calls (the texture cache's lookups for the displayed frame)
	ORBIS_VS_MERGE, // the rest of Merge (the merge draws)
	ORBIS_VS_CAPTURE, // OrbisFrameCapture and the diagnostics
	ORBIS_VS_PREP, // AgePool, the perfmon, CAS
	ORBIS_VS_BEGIN, // BeginPresentFrame (the swapchain acquire)
	ORBIS_VS_RECT, // PresentRect
	ORBIS_VS_OSD, // the watermark and the OSD
	ORBIS_VS_END, // EndPresentFrame (the submit and the present)
	ORBIS_VS_AFTER, // the GPU time, [vstime] and PerformanceMetrics::Update
	ORBIS_VS_PHASES
};
constexpr const char* s_orbis_vs_names[ORBIS_VS_PHASES] = {
	"tune", "dump", "output", "merge", "capture", "prep", "begin", "rect", "osd", "end", "after"};
#ifdef ORBIS_VULKAN
constexpr const char* s_orbis_vkw_names[ORBIS_VKW_KINDS] = {
	"reuse", "sync", "counter", "acquire", "submit", "present", "idle", "pipelines", "glsl", "k9"};
#endif
struct OrbisVsTrace
{
	u64 t0 = 0, prev_end = 0;
	u64 cpu0 = 0; // the GS thread's CPU time at the start, us (Threading::GetThreadCpuTime; 0 if unknown)
	u64 mark[ORBIS_VS_PHASES] = {};
	u64 vsyncs = 0, slow = 0;
#ifdef ORBIS_VULKAN
	unsigned long long vkw_ns[ORBIS_VKW_KINDS] = {}, vkw_n[ORBIS_VKW_KINDS] = {};
#endif
};
OrbisVsTrace s_orbis_vs;
constexpr double kOrbisTscPerMs = 1596300.0; // the PS5's TSC, 1596.30 MHz (vk-285-101's [cpuclk])

__fi void OrbisVsMark(int phase)
{
	s_orbis_vs.mark[phase] = __rdtsc();
}

void OrbisVsBegin()
{
	OrbisVsTrace& v = s_orbis_vs;
	v.t0 = __rdtsc();
	v.cpu0 = Threading::GetThreadCpuTime();
	std::memset(v.mark, 0, sizeof(v.mark));
#ifdef ORBIS_VULKAN
	std::memcpy(v.vkw_ns, g_orbis_vkw_ns, sizeof(v.vkw_ns));
	std::memcpy(v.vkw_n, g_orbis_vkw_n, sizeof(v.vkw_n));
#endif
}

void OrbisVsEnd()
{
	OrbisVsTrace& v = s_orbis_vs;
	const u64 end = __rdtsc();
	const double gap_ms = v.prev_end ? static_cast<double>(v.t0 - v.prev_end) / kOrbisTscPerMs : 0.0;
	v.prev_end = end;
	v.vsyncs++;
	const double total_ms = static_cast<double>(end - v.t0) / kOrbisTscPerMs;
	if (total_ms < 20.0)
		return;
	v.slow++;
	// The thread's own CPU time over the vsync: near the total when it ran (work or a spin), well under it when
	// it slept in a wait or wasn't scheduled.
	const u64 cpu1 = v.cpu0 ? Threading::GetThreadCpuTime() : 0;
	char cpu[32] = "n/a";
	if (cpu1 > v.cpu0)
		std::snprintf(cpu, sizeof(cpu), "%.1f ms", static_cast<double>(cpu1 - v.cpu0) / 1000.0);
	char phases[320], waits[320];
	int pl = 0, wl = 0;
	phases[0] = waits[0] = '\0';
	u64 prev = v.t0;
	for (int p = 0; p < ORBIS_VS_PHASES; p++)
	{
		const u64 m = (v.mark[p] && v.mark[p] >= prev) ? v.mark[p] : prev;
		const double ms = static_cast<double>(m - prev) / kOrbisTscPerMs;
		if (ms >= 0.05 && pl < static_cast<int>(sizeof(phases)) - 24)
			pl += std::snprintf(phases + pl, sizeof(phases) - pl, " %s=%.1f", s_orbis_vs_names[p], ms);
		prev = m;
	}
#ifdef ORBIS_VULKAN
	for (int k = 0; k < ORBIS_VKW_KINDS; k++)
	{
		const unsigned long long n = g_orbis_vkw_n[k] - v.vkw_n[k];
		if (!n || wl >= static_cast<int>(sizeof(waits)) - 32)
			continue;
		wl += std::snprintf(waits + wl, sizeof(waits) - wl, " %s=%.1f(%llu)", s_orbis_vkw_names[k],
			static_cast<double>(g_orbis_vkw_ns[k] - v.vkw_ns[k]) / 1e6, n);
	}
#endif
	printf("[vsslow] #%llu %.1f ms (thread cpu %s), gap before %.1f ms:%s | vk:%s\n",
		static_cast<unsigned long long>(v.vsyncs), total_ms, cpu, gap_ms, phases, wl ? waits : " none");
}
} // namespace
#define ORBIS_VS_MARK(phase) OrbisVsMark(phase)

bool GSRenderer::Merge(int field)
{
	{
		static bool logged = false;
		if (!logged) { logged = true; printf("[dbg] merge called field=%d\n", field); fflush(stdout); }
	}
	GSVector2i fs(0, 0);
	GSTexture* tex[3] = { nullptr, nullptr, nullptr };
	float tex_scale[3] = { 0.0f, 0.0f, 0.0f };
	int y_offset[3] = { 0, 0, 0 };
	const bool feedback_merge = m_regs->EXTWRITE.WRITE == 1;

	// Need to do this here, if the user has Anti-Blur enabled, these offsets can get wiped out/changed.
	const bool game_deinterlacing = (PCRTCDisplays.PCRTCDisplays[0].prevFramebufferOffsets.y != PCRTCDisplays.PCRTCDisplays[0].framebufferOffsets.y) !=
	                                (PCRTCDisplays.PCRTCDisplays[1].prevFramebufferOffsets.y != PCRTCDisplays.PCRTCDisplays[1].framebufferOffsets.y);

	// Only need to check the right/bottom on software renderer, hardware always gets the full texture then cuts a bit out later.
	if (PCRTCDisplays.FrameRectMatch() && !PCRTCDisplays.FrameWrap() && !feedback_merge)
	{
		tex[0] = GetOutput(-1, tex_scale[0], y_offset[0]);
		tex[1] = tex[0]; // saves one texture fetch
		y_offset[1] = y_offset[0];
		tex_scale[1] = tex_scale[0];
	}
	else
	{
		const bool use_rc1 =
			PCRTCDisplays.PCRTCDisplays[0].enabled &&                    // RC1 enabled.
				(!(m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 0) || // Blend RC1 with non-zero alpha.
				(m_regs->PMODE.AMOD == 0) ||                             // Use alpha of RC1.
				(feedback_merge && m_regs->EXTBUF.FBIN == 0));           // Use RC1 for feedback merge.

		// The following two flags determine if RC1 output completely overwrites RC2 output
		// due to the alpha used for blending and the respective rectangles of the outputs.
		const bool rc1_contains_rc2 =
			PCRTCDisplays.PCRTCDisplays[0].displayRect.rcontains(PCRTCDisplays.PCRTCDisplays[1].displayRect);

		const bool rc1_overwrites_rc2 = use_rc1 && rc1_contains_rc2 && m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 255;

		const bool use_rc2 =
			PCRTCDisplays.PCRTCDisplays[1].enabled &&                // RC2 enabled.
				((m_regs->PMODE.SLBG == 0 && !rc1_overwrites_rc2) || // Blending RC2 and not overwritten by RC1.
				(m_regs->PMODE.AMOD == 1) ||                         // Use alpha of RC2.
				(feedback_merge && m_regs->EXTBUF.FBIN == 1));       // Use RC2 for feedback merge.

		if (use_rc1)
			tex[0] = GetOutput(0, tex_scale[0], y_offset[0]);
		if (use_rc2)
			tex[1] = GetOutput(1, tex_scale[1], y_offset[1]);
		if (feedback_merge)
			tex[2] = GetFeedbackOutput(tex_scale[2]);
	}

	ORBIS_VS_MARK(ORBIS_VS_OUTPUT); // vk-285-106

	if (!tex[0] && !tex[1])
	{
		// Clear out the MAD buffer as some remnants of the previously shown frame came be left over, causing a flash for one frame.
		if (GSConfig.InterlaceMode == GSInterlaceMode::Automatic || GSConfig.InterlaceMode >= GSInterlaceMode::AdaptiveTFF)
		{
			GSTexture* mad_tex = g_gs_device->GetMAD();

			if (mad_tex)
			{
				g_gs_device->ClearRenderTarget(mad_tex, 0);
				mad_tex = nullptr;
			}
		}

		// Both circuits off still outputs BGCOLOR on real hardware.
		if (PCRTCDisplays.PCRTCDisplays[0].enabled || PCRTCDisplays.PCRTCDisplays[1].enabled)
		{
			// Orbis: bring-up - why is the frame blank?
			{
				static unsigned long long blanks = 0;
				unsigned long long n = blanks++;
				if (n < 3 || (n % 200) == 0)
				{
					printf("[gsmerge] blank #%llu en0=%d en1=%d fb0=%u fb1=%u\n", n,
						(int)PCRTCDisplays.PCRTCDisplays[0].enabled, (int)PCRTCDisplays.PCRTCDisplays[1].enabled,
						(unsigned)PCRTCDisplays.PCRTCDisplays[0].FBW, (unsigned)PCRTCDisplays.PCRTCDisplays[1].FBW);
					fflush(stdout);
				}
			}
			m_real_size = GSVector2i(0, 0);
			return false;
		}
	}

	s_n++;

	GSVector4 src_gs_read[2] = {};
	GSVector4 dst[3] = {};

	// Use offset for bob deinterlacing always, extra offset added later for FFMD mode.
	const bool scanmask_frame = m_scanmask_used && abs(PCRTCDisplays.PCRTCDisplays[0].displayRect.y - PCRTCDisplays.PCRTCDisplays[1].displayRect.y) != 1;
	int field2 = 0;
	int mode = 3; // If the game is manually deinterlacing then we need to bob (if we want to get away with no deinterlacing).
	bool is_bob = GSConfig.InterlaceMode == GSInterlaceMode::BobTFF || GSConfig.InterlaceMode == GSInterlaceMode::BobBFF;

	// FFMD (half frames) requires blend deinterlacing, so automatically use that. Same when SCANMSK is used but not blended in the merge circuit (Alpine Racer 3).
	if (GSConfig.InterlaceMode != GSInterlaceMode::Automatic || (!game_deinterlacing && !m_regs->SMODE2.FFMD && !scanmask_frame))
	{
		field2 = ((static_cast<int>(GSConfig.InterlaceMode) - 2) & 1);
		mode = ((static_cast<int>(GSConfig.InterlaceMode) - 2) >> 1);
	}

	for (int i = 0; i < 2; i++)
	{
		 const GSPCRTCRegs::PCRTCDisplay& curCircuit = PCRTCDisplays.PCRTCDisplays[i];

		if (!curCircuit.enabled || !tex[i])
			continue;

		const GSVector4 scale = GSVector4(tex_scale[i]);

		// dst is the final destination rect with offset on the screen.
		dst[i] = scale * GSVector4(curCircuit.displayRect);

		// src_gs_read is the size which we're really reading from GS memory.
		src_gs_read[i] = ((GSVector4(curCircuit.framebufferRect) + GSVector4(0, y_offset[i], 0, y_offset[i])) * scale) / GSVector4(tex[i]->GetSize()).xyxy();

		float interlace_offset = 0.0f;
		if (isReallyInterlaced() && m_regs->SMODE2.FFMD && !is_bob && !GSConfig.DisableInterlaceOffset && GSConfig.InterlaceMode != GSInterlaceMode::Off)
		{
			interlace_offset = (scale.y) * static_cast<float>(field ^ field2);
		}
		// Scanmask frame offsets. It's gross, I'm sorry but it sucks.
		if (m_scanmask_used)
		{
			int displayIntOffset = PCRTCDisplays.PCRTCDisplays[i].displayRect.y - PCRTCDisplays.PCRTCDisplays[1 - i].displayRect.y;

			if (displayIntOffset > 0)
			{
				displayIntOffset &= 1;
				dst[i].y -= displayIntOffset * scale.y;
				dst[i].w -= displayIntOffset * scale.y;
				interlace_offset += displayIntOffset;
			}
		}

		dst[i] += GSVector4(0.0f, interlace_offset, 0.0f, interlace_offset);
	}

	if (feedback_merge && tex[2])
	{
		const GSVector4 scale = GSVector4(tex_scale[2]);
		GSVector4i feedback_rect;

		feedback_rect.left = m_regs->EXTBUF.WDX;
		feedback_rect.right = feedback_rect.left + ((m_regs->EXTDATA.WW + 1) / ((m_regs->EXTDATA.SMPH - m_regs->DISP[m_regs->EXTBUF.FBIN].DISPLAY.MAGH) + 1));
		feedback_rect.top = m_regs->EXTBUF.WDY;
		feedback_rect.bottom = ((m_regs->EXTDATA.WH + 1) * (2 - m_regs->EXTBUF.WFFMD)) / ((m_regs->EXTDATA.SMPV - m_regs->DISP[m_regs->EXTBUF.FBIN].DISPLAY.MAGV) + 1);

		dst[2] = GSVector4(scale * GSVector4(feedback_rect.rsize()));
	}

	const GSVector2i resolution = PCRTCDisplays.GetResolution();
	fs = GSVector2i(static_cast<int>(static_cast<float>(resolution.x) * GetUpscaleMultiplier()),
		static_cast<int>(static_cast<float>(resolution.y) * GetUpscaleMultiplier()));
	{
		static unsigned long long mc = 0;
		if ((mc++ % 100) == 0)
		{
			printf("[dbg] merge[%llu]: res=%d,%d fs=%d,%d mult=%.2f\n", mc, resolution.x, resolution.y, fs.x, fs.y, GetUpscaleMultiplier());
			fflush(stdout);
		}
	}

	m_real_size = GSVector2i(fs.x, fs.y);

	if ((tex[0] || tex[1]) && (tex[0] == tex[1]) && (src_gs_read[0] == src_gs_read[1]).alltrue() && (dst[0] == dst[1]).alltrue() &&
		(PCRTCDisplays.PCRTCDisplays[0].displayRect == PCRTCDisplays.PCRTCDisplays[1].displayRect).alltrue() &&
		(PCRTCDisplays.PCRTCDisplays[0].framebufferRect == PCRTCDisplays.PCRTCDisplays[1].framebufferRect).alltrue() &&
		!feedback_merge && !m_regs->PMODE.SLBG)
	{
		// the two outputs are identical, skip drawing one of them (the one that is alpha blended)
		tex[0] = nullptr;
	}

	const u32 c = (m_regs->BGCOLOR.U32[0] & 0x00FFFFFFu) | (m_regs->PMODE.ALP << 24);
	g_gs_device->Merge(tex, src_gs_read, dst, fs, m_regs->PMODE, m_regs->EXTBUF, c);

	if ((tex[0] || tex[1]) && isReallyInterlaced() && GSConfig.InterlaceMode != GSInterlaceMode::Off)
	{
		const float offset = is_bob ? (tex[1] ? tex_scale[1] : tex_scale[0]) : 0.0f;

		g_gs_device->Interlace(fs, field ^ field2, mode, offset);
	}

	if (GSConfig.ShadeBoost)
		g_gs_device->ShadeBoost();

	if (GSConfig.FXAA)
		g_gs_device->FXAA();

	// Sharpens biinear at lower resolutions, almost nearest but with more uniform pixels.
	if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp && (g_gs_device->GetWindowWidth() > fs.x || g_gs_device->GetWindowHeight() > fs.y))
	{
		g_gs_device->Resize(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight());
	}

	if (m_scanmask_used)
		m_scanmask_used--;

	return true;
}

GSVector2i GSRenderer::GetInternalResolution()
{
	return m_real_size;
}

float GSRenderer::GetModXYOffset()
{
	if (GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::Normal)
	{
		float mod_xy = GetUpscaleMultiplier();
		const int rounded_mod_xy = static_cast<int>(std::round(mod_xy));
		if (rounded_mod_xy > 1)
		{
			if (!(rounded_mod_xy & 1))
				return mod_xy += 0.2f;
			else if (!(rounded_mod_xy & 2))
				return mod_xy += 0.3f;
			else
				return mod_xy += 0.1f;
		}
	}

	return 0.0f;
}

static float GetCurrentAspectRatioFloat(bool is_progressive)
{
	switch (GSConfig.AspectRatio)
	{
		default:
		// We don't know the AR of the display here, nor we care about it
		case AspectRatioType::Stretch:
		case AspectRatioType::RAuto4_3_3_2:
			if (EmuConfig.CurrentCustomAspectRatio > 0.f)
				return EmuConfig.CurrentCustomAspectRatio;
			else if (is_progressive)
				return 3.0f / 2.0f;
			else
				return 4.0f / 3.0f;
		case AspectRatioType::R4_3:
			return 4.0f / 3.0f;
		case AspectRatioType::R16_9:
			return 16.0f / 9.0f;
		case AspectRatioType::R10_7:
			return 10.0f / 7.0f;
	}
}

static GSVector4 CalculateDrawDstRect(s32 window_width, s32 window_height, const GSVector4i& src_rect, const GSVector2i& src_size, GSDisplayAlignment alignment, bool flip_y, bool is_progressive)
{
	const float f_width = static_cast<float>(window_width);
	const float f_height = static_cast<float>(window_height);
	const float clientAr = f_width / f_height;

	float targetAr = clientAr;
	if (EmuConfig.CurrentAspectRatio == AspectRatioType::RAuto4_3_3_2)
	{
		if (is_progressive)
			targetAr = 3.0f / 2.0f;
		else
			targetAr = 4.0f / 3.0f;
		// Fall back on the custom aspect ratio set by patches (e.g. 16:9, 21:9)
		if (EmuConfig.CurrentCustomAspectRatio > 0.f)
			targetAr = EmuConfig.CurrentCustomAspectRatio;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R4_3)
	{
		targetAr = 4.0f / 3.0f;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R16_9)
	{
		targetAr = 16.0f / 9.0f;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R10_7)
	{
		targetAr = 10.0f / 7.0f;
	}

	const float crop_adjust = (static_cast<float>(src_rect.width()) / static_cast<float>(src_size.x)) /
		(static_cast<float>(src_rect.height()) / static_cast<float>(src_size.y));

	const double arr = (targetAr * crop_adjust) / clientAr;
	float target_width = f_width;
	float target_height = f_height;
	if (arr < 1)
		target_width = std::floor(f_width * arr + 0.5f);
	else if (arr > 1)
		target_height = std::floor(f_height / arr + 0.5f);

	target_height *= GSConfig.StretchY / 100.0f;

	if (GSConfig.IntegerScaling)
	{
		// make target width/height an integer multiple of the texture width/height
		float t_width = static_cast<double>(src_rect.width());
		float t_height = static_cast<double>(src_rect.height());

		// If using Bilinear (Shape) the image will be prescaled to larger than the window, so we need to unscale it.
		if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp && src_rect.width() > 0 && src_rect.height() > 0)
		{
			const GSVector2i resolution = g_gs_renderer->PCRTCDisplays.GetResolution();
			const GSVector2i fs = GSVector2i(static_cast<int>(static_cast<float>(resolution.x) * g_gs_renderer->GetUpscaleMultiplier()),
				static_cast<int>(static_cast<float>(resolution.y) * g_gs_renderer->GetUpscaleMultiplier()));

			if (g_gs_device->GetWindowWidth() > fs.x || g_gs_device->GetWindowHeight() > fs.y)
			{
				t_width *= static_cast<float>(fs.x) / src_rect.width();
				t_height *= static_cast<float>(fs.y) / src_rect.height();
			}
		}

		float scale;
		if ((t_width / t_height) >= 1.0)
			scale = target_width / t_width;
		else
			scale = target_height / t_height;

		if (scale > 1.0)
		{
			const float adjust = std::floor(scale) / scale;
			target_width = target_width * adjust;
			target_height = target_height * adjust;
		}
	}

	float target_x, target_y;
	if (target_width >= f_width)
	{
		target_x = -((target_width - f_width) * 0.5f);
	}
	else
	{
		switch (alignment)
		{
			case GSDisplayAlignment::Center:
				target_x = (f_width - target_width) * 0.5f;
				break;
			case GSDisplayAlignment::RightOrBottom:
				target_x = (f_width - target_width);
				break;
			case GSDisplayAlignment::LeftOrTop:
			default:
				target_x = 0.0f;
				break;
		}
	}
	if (target_height >= f_height)
	{
		target_y = -((target_height - f_height) * 0.5f);
	}
	else
	{
		switch (alignment)
		{
			case GSDisplayAlignment::Center:
				target_y = (f_height - target_height) * 0.5f;
				break;
			case GSDisplayAlignment::RightOrBottom:
				target_y = (f_height - target_height);
				break;
			case GSDisplayAlignment::LeftOrTop:
			default:
				target_y = 0.0f;
				break;
		}
	}

	GSVector4 ret(target_x, target_y, target_x + target_width, target_y + target_height);

	if (flip_y)
	{
		const float height = ret.w - ret.y;
		ret.y = static_cast<float>(window_height) - ret.w;
		ret.w = ret.y + height;
	}

	return ret;
}

static GSVector4i CalculateDrawSrcRect(const GSTexture* src, const GSVector2i real_size)
{
	const GSVector2i size(src->GetSize());
	const GSVector2 scale = GSVector2(size.x, size.y) / GSVector2(real_size.x, real_size.y).max(GSVector2(0.1f, 0.1f));
	const float upscale = GSIsHardwareRenderer() ? GSConfig.UpscaleMultiplier : 1;
	const int left = static_cast<int>(static_cast<float>(GSConfig.Crop[0] * scale.x) * upscale);
	const int top = static_cast<int>(static_cast<float>(GSConfig.Crop[1] * scale.y) * upscale);
	const int right =  size.x - static_cast<int>(static_cast<float>(GSConfig.Crop[2] * scale.x) * upscale);
	const int bottom = size.y - static_cast<int>(static_cast<float>(GSConfig.Crop[3] * scale.y) * upscale);
	return GSVector4i(left, top, right, bottom);
}

static const char* GetScreenshotSuffix()
{
	static constexpr const char* suffixes[static_cast<u8>(GSScreenshotFormat::Count)] = {
		"png", "jpg", "webp"};
	return suffixes[static_cast<u8>(GSConfig.ScreenshotFormat)];
}

static void CompressAndWriteScreenshot(std::string filename, u32 width, u32 height, std::vector<u32> pixels)
{
	RGBA8Image image;
	image.SetPixels(width, height, std::move(pixels));

	std::string key(fmt::format("GSScreenshot_{}", filename));

	if (!GSDumpReplayer::IsRunner())
	{
		Host::AddIconOSDMessage(key, ICON_FA_CAMERA,
			fmt::format(TRANSLATE_FS("GS", "Saving screenshot to '{}'."), Path::GetFileName(filename)), 60.0f);
	}

	// maybe std::async would be better here.. but it's definitely worth threading, large screenshots take a while to compress.
	std::unique_lock lock(s_screenshot_threads_mutex);
	s_screenshot_threads.emplace_back([key = std::move(key), filename = std::move(filename), image = std::move(image),
										  quality = GSConfig.ScreenshotQuality]() {
		if (image.SaveToFile(filename.c_str(), quality))
		{
			if (!GSDumpReplayer::IsRunner())
			{
				Host::AddIconOSDMessage(std::move(key), ICON_FA_CAMERA,
					fmt::format(TRANSLATE_FS("GS", "Saved screenshot to '{}'."), Path::GetFileName(filename)),
					Host::OSD_INFO_DURATION);
			}
		}
		else
		{
			Host::AddIconOSDMessage(std::move(key), ICON_FA_CAMERA,
				fmt::format(TRANSLATE_FS("GS", "Failed to save screenshot to '{}'."), Path::GetFileName(filename),
					Host::OSD_ERROR_DURATION));
		}

		// remove ourselves from the list, if the GS thread is waiting for us, we won't be in there
		const auto this_id = std::this_thread::get_id();
		std::unique_lock lock(s_screenshot_threads_mutex);
		for (auto it = s_screenshot_threads.begin(); it != s_screenshot_threads.end(); ++it)
		{
			if (it->get_id() == this_id)
			{
				it->detach();
				s_screenshot_threads.erase(it);
				break;
			}
		}
	});
}

void GSJoinSnapshotThreads()
{
	std::unique_lock lock(s_screenshot_threads_mutex);
	while (!s_screenshot_threads.empty())
	{
		std::thread save_thread(std::move(s_screenshot_threads.front()));
		s_screenshot_threads.pop_front();
		lock.unlock();
		save_thread.join();
		lock.lock();
	}
}

bool GSRenderer::BeginPresentFrame(bool frame_skip)
{
	Host::BeginPresentFrame();

	const GSDevice::PresentResult res = g_gs_device->BeginPresent(frame_skip);
	if (res == GSDevice::PresentResult::FrameSkipped)
	{
		// If we're skipping a frame, we need to reset imgui's state, since
		// we won't be calling EndPresentFrame().
		ImGuiManager::SkipFrame();
		return false;
	}
	else if (res == GSDevice::PresentResult::OK)
	{
		// All good!
		return true;
	}

	// If we're constantly crashing on something in particular, we don't want to end up in an
	// endless reset loop.. that'd probably end up leaking memory and/or crashing us for other
	// reasons. So just abort in such case.
	const Common::Timer::Value current_time = Common::Timer::GetCurrentValue();
	if (s_last_gpu_reset_time != 0 &&
		Common::Timer::ConvertValueToSeconds(current_time - s_last_gpu_reset_time) < 15.0f)
	{
		pxFailRel("Host GPU lost too many times, device is probably completely wedged.");
	}
	s_last_gpu_reset_time = current_time;

	// Device lost, something went really bad.
	// Let's just toss out everything, and try to hobble on.
	if (!GSreopen(true, false, GSGetCurrentRenderer(), std::nullopt))
	{
		pxFailRel("Failed to recreate GS device after loss.");
		return false;
	}

	// First frame after reopening is definitely going to be trash, so skip it.
	Host::AddIconOSDMessage("GSDeviceLost", ICON_FA_TRIANGLE_EXCLAMATION,
		TRANSLATE_SV("GS", "Host GPU device encountered an error and was recovered. This may have broken rendering."),
		Host::OSD_CRITICAL_ERROR_DURATION);
	return false;
}

void GSRenderer::EndPresentFrame()
{
	if (GSDumpReplayer::IsReplayingDump())
		GSDumpReplayer::RenderUI();

	FullscreenUI::Render();
	ImGuiManager::RenderOSD();
	g_gs_device->EndPresent();
	ImGuiManager::NewFrame();
}

// ---- vk-285-25: the frame capture ----
// /data/PCSX2/framecap (content optional: "secs=20 every=2 x0=0.5 x1=1 y0=0 y1=1 half=1") records the
// displayed frame -- the merge output, before the present's scaling -- for `secs` seconds, every `every`th
// vsync, cropped to [x0,x1) x [y0,y1) of the frame (fractions) and halved on the GPU with half=1, into
// /data/PCSX2/framecap.bin: "PS5FCAP1", u32 version 1, u32 header size 16, then per frame twelve u32
// (magic 'FRM1', vsync, w, h, frame w, frame h, crop x0 y0 x1 y1, microseconds lo, hi) and w*h*3 bytes RGB,
// rows top down. Frame N's copy is read at vsync N+1, by which time the GPU has normally finished it.
namespace
{
	struct OrbisCap
	{
		FILE* f = nullptr;
		int left = 0;
		int every = 2;
		int n = 0;
		float x0 = 0.5f, x1 = 1.0f, y0 = 0.0f, y1 = 1.0f;
		int half = 1;
		unsigned frames = 0;
		unsigned long long bytes = 0;
		std::chrono::steady_clock::time_point t0;
		GSTexture* rt = nullptr;
		std::unique_ptr<GSDownloadTexture> dl;
		bool pending = false;
		u32 hdr[12] = {};
		std::vector<u8> rgb;
	};
	OrbisCap s_orbis_cap;
	unsigned s_orbis_cap_vsync = 0;
} // namespace

static void OrbisCapFinishPending()
{
	OrbisCap& c = s_orbis_cap;
	if (!c.pending)
		return;
	c.pending = false;
	const u32 w = c.hdr[2], h = c.hdr[3];
	const GSVector4i rc(0, 0, static_cast<int>(w), static_cast<int>(h));
	c.dl->Flush();
	if (!c.dl->Map(rc))
	{
		printf("[framecap] map failed\n");
		return;
	}
	const u8* const p = c.dl->GetMapPointer();
	const u32 pitch = c.dl->GetMapPitch();
	c.rgb.resize(static_cast<size_t>(w) * h * 3);
	u8* o = c.rgb.data();
	for (u32 y = 0; y < h; y++)
	{
		const u8* r = p + static_cast<size_t>(y) * pitch;
		for (u32 x = 0; x < w; x++, r += 4, o += 3)
		{
			o[0] = r[0];
			o[1] = r[1];
			o[2] = r[2];
		}
	}
	c.dl->Unmap();
	fwrite(c.hdr, sizeof(c.hdr), 1, c.f);
	fwrite(c.rgb.data(), 1, c.rgb.size(), c.f);
	c.frames++;
	c.bytes += sizeof(c.hdr) + c.rgb.size();
}

static void OrbisCapClose()
{
	OrbisCap& c = s_orbis_cap;
	if (c.f)
		OrbisCapFinishPending();
	c.pending = false;
	if (c.f)
	{
		fclose(c.f);
		c.f = nullptr;
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - c.t0).count();
		printf("[framecap] done: %u frames, %.1f MB in %.1f s -> /data/PCSX2/framecap.bin\n", c.frames,
			static_cast<double>(c.bytes) / 1048576.0, secs);
		fflush(stdout);
		OrbisOSDLabel("CAPTURE DONE");
	}
	if (c.rt)
	{
		g_gs_device->Recycle(c.rt);
		c.rt = nullptr;
	}
	c.dl.reset();
	std::vector<u8>().swap(c.rgb);
	c.left = 0;
}

static void OrbisFrameCapture(GSTexture* current)
{
	OrbisCap& c = s_orbis_cap;
	s_orbis_cap_vsync++;
	if (c.f)
		OrbisCapFinishPending();
	if (!c.f)
	{
		if ((s_orbis_cap_vsync % 25) != 0 || !OrbisFlag("framecap"))
			return;
		char buf[256] = {};
		if (FILE* pf = fopen(OrbisFlagPath("framecap").c_str(), "rb"))
		{
			const size_t n = fread(buf, 1, sizeof(buf) - 1, pf);
			buf[n] = '\0';
			fclose(pf);
		}
		unlink(OrbisFlagPath("framecap").c_str());
		float secs = 20.0f, every = 2.0f, half = 1.0f, x0 = 0.5f, x1 = 1.0f, y0 = 0.0f, y1 = 1.0f;
		char* save = nullptr;
		for (char* tok = strtok_r(buf, " \t\r\n,", &save); tok; tok = strtok_r(nullptr, " \t\r\n,", &save))
		{
			char key[16] = {};
			float v = 0.0f;
			if (sscanf(tok, "%15[a-z0-9]=%f", key, &v) != 2)
				continue;
			const std::string k(key);
			if (k == "secs")
				secs = v;
			else if (k == "every")
				every = v;
			else if (k == "half")
				half = v;
			else if (k == "x0")
				x0 = v;
			else if (k == "x1")
				x1 = v;
			else if (k == "y0")
				y0 = v;
			else if (k == "y1")
				y1 = v;
		}
		c.x0 = std::clamp(x0, 0.0f, 1.0f);
		c.x1 = std::clamp(x1, 0.0f, 1.0f);
		c.y0 = std::clamp(y0, 0.0f, 1.0f);
		c.y1 = std::clamp(y1, 0.0f, 1.0f);
		if (c.x1 <= c.x0 || c.y1 <= c.y0)
		{
			printf("[framecap] empty crop, not recording\n");
			fflush(stdout);
			return;
		}
		c.f = fopen(OrbisLogPath("framecap.bin").c_str(), "wb");
		if (!c.f)
		{
			printf("[framecap] cannot open /data/PCSX2/framecap.bin\n");
			fflush(stdout);
			return;
		}
		setvbuf(c.f, nullptr, _IOFBF, 4u << 20);
		static const char magic[8] = {'P', 'S', '5', 'F', 'C', 'A', 'P', '1'};
		const u32 ver[2] = {1, 16};
		fwrite(magic, sizeof(magic), 1, c.f);
		fwrite(ver, sizeof(ver), 1, c.f);
		c.left = static_cast<int>(std::clamp(secs, 0.1f, 120.0f) * 50.0f + 0.5f);
		c.every = std::clamp(static_cast<int>(every), 1, 50);
		c.half = half != 0.0f ? 1 : 0;
		c.n = 0;
		c.frames = 0;
		c.bytes = 16;
		c.pending = false;
		c.t0 = std::chrono::steady_clock::now();
		printf("[framecap] armed: %d vsyncs, every %d, crop x %.3f-%.3f y %.3f-%.3f, %s\n", c.left, c.every,
			static_cast<double>(c.x0), static_cast<double>(c.x1), static_cast<double>(c.y0), static_cast<double>(c.y1),
			c.half ? "halved" : "full size");
		fflush(stdout);
		OrbisOSDLabel("CAPTURE");
	}
	if (c.left <= 0)
	{
		OrbisCapClose();
		return;
	}
	c.left--;
	if ((c.n++ % c.every) != 0 || !current)
		return;
	const int W = current->GetWidth(), H = current->GetHeight();
	const int cx0 = std::clamp(static_cast<int>(c.x0 * W) & ~1, 0, W), cx1 = std::clamp(static_cast<int>(c.x1 * W) & ~1, 0, W);
	const int cy0 = std::clamp(static_cast<int>(c.y0 * H) & ~1, 0, H), cy1 = std::clamp(static_cast<int>(c.y1 * H) & ~1, 0, H);
	if (cx1 - cx0 < 2 || cy1 - cy0 < 2)
		return;
	const int w = (cx1 - cx0) >> c.half, h = (cy1 - cy0) >> c.half;
	if (!c.rt || c.rt->GetWidth() != w || c.rt->GetHeight() != h)
	{
		if (c.rt)
			g_gs_device->Recycle(c.rt);
		c.rt = g_gs_device->CreateRenderTarget(w, h, GSTexture::Format::Color, false, false);
		c.dl = g_gs_device->CreateDownloadTexture(static_cast<u32>(w), static_cast<u32>(h), GSTexture::Format::Color);
		if (!c.rt || !c.dl)
		{
			printf("[framecap] no %dx%d target or download buffer\n", w, h);
			OrbisCapClose();
			return;
		}
	}
	const GSVector4 sRect(static_cast<float>(cx0) / W, static_cast<float>(cy0) / H, static_cast<float>(cx1) / W,
		static_cast<float>(cy1) / H);
	g_gs_device->StretchRect(current, sRect, c.rt, GSVector4(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)),
		ShaderConvert::COPY, c.half ? Biln : Nearest);
	c.dl->CopyFromTexture(GSVector4i(0, 0, w, h), c.rt, GSVector4i(0, 0, w, h), 0, true);
	c.pending = true;
	const unsigned long long us = static_cast<unsigned long long>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - c.t0).count());
	const u32 hdr[12] = {0x314D5246u, s_orbis_cap_vsync, static_cast<u32>(w), static_cast<u32>(h), static_cast<u32>(W),
		static_cast<u32>(H), static_cast<u32>(cx0), static_cast<u32>(cy0), static_cast<u32>(cx1), static_cast<u32>(cy1),
		static_cast<u32>(us), static_cast<u32>(us >> 32)};
	std::memcpy(c.hdr, hdr, sizeof(hdr));
}


void GSRenderer::VSync(u32 field, bool registers_written, bool idle_frame)
{
	// Orbis: Null renderer has no GSDevice; skip presentation/merge entirely.
	if (!g_gs_device)
		return;

	OrbisVsBegin(); // vk-285-106
	OrbisLiveTune(); // eerec-278
	ORBIS_VS_MARK(ORBIS_VS_TUNE);
	const auto orbis_vs_t0 = std::chrono::steady_clock::now();
	double orbis_merge_ms = 0.0;
	double orbis_present_ms = 0.0;

	if (GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()))
	{
		if (GSConfig.SaveInfo)
		{
			DumpGSPrivRegs(*m_regs, GetDrawDumpPath("%05lld_f%05lld_vsync_gs_reg.txt", s_n, g_perfmon.GetFrame()));

			DumpDrawInfo(false, false, true);
		}

		if (GSConfig.SaveTransferImages)
			DumpTransferImages();

		if (GSConfig.SaveFrameStats)
		{
			m_perfmon_frame = g_perfmon - m_perfmon_frame;
			m_perfmon_frame.Dump(GetDrawDumpPath("%05lld_f%05lld_frame_stats.txt", s_n, g_perfmon.GetFrame()), GSIsHardwareRenderer());
			m_perfmon_frame = g_perfmon;
		}
	}

	const int fb_sprite_blits = g_perfmon.GetDisplayFramebufferSpriteBlits();
	const bool fb_sprite_frame = (fb_sprite_blits > 0);

	bool skip_frame = false;
	if (GSConfig.SkipDuplicateFrames && !GSCapture::IsCapturingVideo())
	{
		bool is_unique_frame;
		switch (PerformanceMetrics::GetInternalFPSMethod())
		{
		case PerformanceMetrics::InternalFPSMethod::GSPrivilegedRegister:
			is_unique_frame = registers_written;
			break;
		case PerformanceMetrics::InternalFPSMethod::DISPFBBlit:
			is_unique_frame = fb_sprite_frame;
			break;
		default:
			is_unique_frame = true;
			break;
		}

		if (!is_unique_frame && m_skipped_duplicate_frames < MAX_SKIPPED_DUPLICATE_FRAMES)
		{
			m_skipped_duplicate_frames++;
			skip_frame = true;
		}
		else
		{
			m_skipped_duplicate_frames = 0;
		}
	}

	ORBIS_VS_MARK(ORBIS_VS_DUMP);
	const bool blank_frame = !Merge(field);
	ORBIS_VS_MARK(ORBIS_VS_MERGE);
	OrbisFrameCapture(blank_frame ? nullptr : g_gs_device->GetCurrent()); // vk-285-25
	{
		static unsigned s_d = 0;
		if (g_orbis_diag && (s_d++ % 250) == 7 && s_orbis_gl) // eerec-280
			OrbisDiagTexture("current", g_gs_device->GetCurrent());
	} // eerec-279
	ORBIS_VS_MARK(ORBIS_VS_CAPTURE);
	orbis_merge_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - orbis_vs_t0).count();

	// Orbis: bring-up - which presentation branch is taken?
	{
		static unsigned long long vs = 0;
		unsigned long long n = vs++;
		if (n < 3 || (n % 100) == 0)
		{
			printf("[gsvs] #%llu skip=%d throttle_skip=%d blank=%d current=%p win=%dx%d\n", n,
				(int)skip_frame, (int)g_gs_device->ShouldSkipPresentingFrame(), (int)blank_frame,
				(void*)g_gs_device->GetCurrent(), (int)g_gs_device->GetWindowWidth(), (int)g_gs_device->GetWindowHeight());
			fflush(stdout);
		}
	}

	m_last_draw_n = s_n;
	m_last_transfer_n = s_transfer_n;

	// Skip presentation when running uncapped while vsync is on.
	if (skip_frame || g_gs_device->ShouldSkipPresentingFrame())
	{
		const bool began = BeginPresentFrame(true);
		ORBIS_VS_MARK(ORBIS_VS_BEGIN);
		if (began)
		{
			EndPresentFrame();
			ORBIS_VS_MARK(ORBIS_VS_END);
		}

		PerformanceMetrics::Update(registers_written, fb_sprite_frame, skip_frame);
		ORBIS_VS_MARK(ORBIS_VS_AFTER);
	}
	else
	{
		if (!idle_frame)
			g_gs_device->AgePool();

		g_perfmon.EndFrame(idle_frame);

		if ((g_perfmon.GetFrame() & 0x1f) == 0)
			g_perfmon.Update();

		// Little bit ugly, but we can't do CAS inside the render pass.
		GSVector4i src_rect;
		GSVector4 src_uv, draw_rect;
		GSTexture* current = g_gs_device->GetCurrent();
		if (current && !blank_frame)
		{
			src_rect = CalculateDrawSrcRect(current, m_real_size);
			src_uv = GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy();
			draw_rect = CalculateDrawDstRect(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight(),
				src_rect, current->GetSize(), s_display_alignment, g_gs_device->UsesLowerLeftOrigin(),
				GetVideoMode() == GSVideoMode::SDTV_480P);
			s_last_draw_rect = draw_rect;
			s_orbis_res[0] = src_rect.width(); // vk-285-12: OSD resolution line
			s_orbis_res[1] = src_rect.height();
			s_orbis_res[2] = static_cast<int>(draw_rect.z - draw_rect.x + 0.5f);
			s_orbis_res[3] = static_cast<int>(std::abs(draw_rect.w - draw_rect.y) + 0.5f);

			if (GSConfig.CASMode != GSCASMode::Disabled)
			{
				static bool cas_log_once = false;
				if (g_gs_device->Features().cas_sharpening)
				{
					// sharpen only if the IR is higher than the display resolution
					const bool sharpen_only = (GSConfig.CASMode == GSCASMode::SharpenOnly ||
					                           (current->GetWidth() > g_gs_device->GetWindowWidth() &&
					                            current->GetHeight() > g_gs_device->GetWindowHeight()));
					g_gs_device->CAS(current, src_rect, src_uv, draw_rect, sharpen_only);
				}
				else if (!cas_log_once)
				{
					Host::AddIconOSDMessage("CASUnsupported", ICON_FA_TRIANGLE_EXCLAMATION,
						TRANSLATE_SV("GS", "CAS is not available, your graphics driver does not support the required functionality."),
						10.0f);
					cas_log_once = true;
				}
			}
		}

		ORBIS_VS_MARK(ORBIS_VS_PREP);
		const bool began = BeginPresentFrame(false);
		ORBIS_VS_MARK(ORBIS_VS_BEGIN);
		if (began)
		{
			if (current && !blank_frame)
			{
				const u64 current_time = Common::Timer::GetCurrentValue();
				const float shader_time = static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - m_shader_time_start));

				{ static unsigned s_geo = 0; if ((s_geo++ % 600) == 0) { printf("[present] tex=%dx%d src=%.3f,%.3f,%.3f,%.3f dst=%.0f,%.0f,%.0f,%.0f\n", current->GetWidth(), current->GetHeight(), src_uv.x, src_uv.y, src_uv.z, src_uv.w, draw_rect.x, draw_rect.y, draw_rect.z, draw_rect.w); fflush(stdout); } } // eerec-278
				g_gs_device->PresentRect(current, src_uv, nullptr, draw_rect,
					s_tv_shader_indices[GSConfig.TVShader], shader_time, BilnIf(GSConfig.LinearPresent != GSPostBilinearMode::Off));
			}
			ORBIS_VS_MARK(ORBIS_VS_RECT);

#ifdef ORBIS_VULKAN
			OrbisWatermark(); // test build 1 (vk-285-55): under the FPS box
#endif
			OrbisGLOSD(); // eerec-278
			if (s_orbis_gl && g_orbis_diag) // eerec-280
				OrbisSampleWindow(); // eerec-279 (every 120th call)
			ORBIS_VS_MARK(ORBIS_VS_OSD);
			EndPresentFrame();
			ORBIS_VS_MARK(ORBIS_VS_END);

			const float gpu_time = g_gs_device->GetAndResetAccumulatedGPUTime();
			GPUPipelineStatistics gpu_stats = g_gs_device->GetAndResetAccumulatedGPUPipelineStatistics();
			PerformanceMetrics::OnGPUPresent(gpu_time, gpu_stats.vs_invocations, gpu_stats.ps_invocations);
		}

		{
			orbis_present_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - orbis_vs_t0).count() - orbis_merge_ms;
			static unsigned long long orbis_vsn = 0;
			static double orbis_vs_sum = 0, orbis_vs_max = 0, orbis_mg_sum = 0, orbis_pr_sum = 0;
			const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - orbis_vs_t0).count();
			orbis_vs_sum += total;
			orbis_mg_sum += orbis_merge_ms;
			orbis_pr_sum += orbis_present_ms;
			if (total > orbis_vs_max)
				orbis_vs_max = total;
			if ((++orbis_vsn % 50) == 0)
			{
				printf("[vstime] n=%llu vsync avg=%.1fms max=%.1fms merge_avg=%.1fms present_avg=%.1fms\n",
					orbis_vsn, orbis_vs_sum / 50.0, orbis_vs_max, orbis_mg_sum / 50.0, orbis_pr_sum / 50.0);
				fflush(stdout);
				orbis_vs_sum = orbis_mg_sum = orbis_pr_sum = 0;
				orbis_vs_max = 0;
			}
		}

		PerformanceMetrics::Update(registers_written, fb_sprite_frame, false);
		ORBIS_VS_MARK(ORBIS_VS_AFTER);
	}
	OrbisVsEnd(); // vk-285-106: [vsslow]

	// snapshot
	if (!m_snapshot.empty())
	{
		u32 screenshot_width, screenshot_height;
		std::vector<u32> screenshot_pixels;

		if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp)
		{
			const GSTexture* current = g_gs_device->GetCurrent();
			const GSVector2i internal_res = GetInternalResolution();

			if (current && (current->GetWidth() > internal_res.x || current->GetHeight() > internal_res.y))
				g_gs_device->Resize(internal_res.x, internal_res.y);
		}

		if (!m_dump && m_dump_frames > 0)
		{
			if (GSConfig.UserHacks_ReadTCOnClose)
				ReadbackTextureCache();

			freezeData fd = {0, nullptr};
			Freeze(&fd, true);
			fd.data = new u8[fd.size];
			Freeze(&fd, false);

			// keep the screenshot relatively small so we don't bloat the dump
			static constexpr u32 DUMP_SCREENSHOT_WIDTH = 640;
			static constexpr u32 DUMP_SCREENSHOT_HEIGHT = 480;
			SaveSnapshotToMemory(DUMP_SCREENSHOT_WIDTH, DUMP_SCREENSHOT_HEIGHT, true, false,
				&screenshot_width, &screenshot_height, &screenshot_pixels);

			std::string_view compression_str;
			if (GSConfig.GSDumpCompression == GSDumpCompressionMethod::Uncompressed)
			{
				m_dump = GSDumpBase::CreateUncompressedDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with no compression");
			}
			else if (GSConfig.GSDumpCompression == GSDumpCompressionMethod::LZMA)
			{
				m_dump = GSDumpBase::CreateXzDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with LZMA compression");
			}
			else
			{
				m_dump = GSDumpBase::CreateZstDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with Zstandard compression");
			}

			delete[] fd.data;

			Host::AddKeyedOSDMessage("GSDump",
				fmt::format(TRANSLATE_FS("GS", "Saving {0} GS dump {1} to '{2}'"),
					(m_dump_frames == 1) ? TRANSLATE_SV("GS", "single frame") : TRANSLATE_SV("GS", "multi-frame"), compression_str,
					Path::GetFileName(m_dump->GetPath())),
				Host::OSD_INFO_DURATION);
		}

		const bool internal_resolution = (GSConfig.ScreenshotSize >= GSScreenshotSize::InternalResolution);
		const bool aspect_correct = (GSConfig.ScreenshotSize != GSScreenshotSize::InternalResolutionUncorrected);

		if (g_gs_device->GetCurrent() && SaveSnapshotToMemory(
			internal_resolution ? 0 : g_gs_device->GetWindowWidth(),
			internal_resolution ? 0 : g_gs_device->GetWindowHeight(),
			aspect_correct, true,
			&screenshot_width, &screenshot_height, &screenshot_pixels))
		{
			CompressAndWriteScreenshot(fmt::format("{}.{}", m_snapshot, GetScreenshotSuffix()),
				screenshot_width, screenshot_height, std::move(screenshot_pixels));
		}
		else
		{
			Host::AddIconOSDMessage("GSScreenshot", ICON_FA_CAMERA,
				TRANSLATE_SV("GS", "Failed to render/download screenshot."), Host::OSD_ERROR_DURATION);
		}

		m_snapshot = {};
	}
	else if (m_dump)
	{
		const bool last = (m_dump_frames == 0);
		if (m_dump->VSync(field, last, m_regs))
		{
			Host::AddKeyedOSDMessage("GSDump",
				fmt::format(TRANSLATE_FS("GS", "Saved GS dump to '{}'."), Path::GetFileName(m_dump->GetPath())),
				Host::OSD_INFO_DURATION);
			m_dump.reset();
		}
		else if (!last)
		{
			m_dump_frames--;
		}
	}

	// capture
	if (GSCapture::IsCapturingVideo())
	{
		const GSVector2i size = GSCapture::GetSize();
		if (GSTexture* current = g_gs_device->GetCurrent())
		{
			// TODO: Maybe avoid this copy in the future? We can use swscale to fix it up on the dumping thread..
			if (current->GetSize() != size)
			{
				GSTexture* temp = g_gs_device->CreateRenderTarget(size.x, size.y, GSTexture::Format::Color, false);
				if (temp)
				{
					g_gs_device->StretchRect(current, temp, GSVector4(0, 0, size.x, size.y), ShaderConvert::COPY, Biln);
					GSCapture::DeliverVideoFrame(temp);
					g_gs_device->Recycle(temp);
				}
			}
			else
			{
				GSCapture::DeliverVideoFrame(current);
			}
		}
		else
		{
			// Bit janky, but unless we want to make variable frame rate files, we need to deliver *a* frame to
			// the video file, so just grab a blank RT.
			GSTexture* temp = g_gs_device->CreateRenderTarget(size.x, size.y, GSTexture::Format::Color, true);
			if (temp)
			{
				GSCapture::DeliverVideoFrame(temp);
				g_gs_device->Recycle(temp);
			}
		}
	}

	if (GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()) && GSConfig.SaveTransferImages)
		DumpTransferImages();
}

void GSRenderer::QueueSnapshot(const std::string& path, const u32 gsdump_frames)
{
	if (!m_snapshot.empty())
		return;

	// Allows for providing a complete path
	if (path.size() > 4 && StringUtil::EndsWithNoCase(path, ".png"))
		m_snapshot = path.substr(0, path.size() - 4);
	else
		m_snapshot = GSGetBaseSnapshotFilename();

	// this is really gross, but wx we get the snapshot request after shift...
	m_dump_frames = gsdump_frames;
}

static std::string GSGetBaseFilename()
{
	std::string filename;

	// append the game serial and title
	if (std::string name(VMManager::GetTitle(true)); !name.empty())
	{
		Path::SanitizeFileName(&name);
		if (name.length() > 219)
			name.resize(219);
		filename += name;
	}
	if (std::string serial = VMManager::GetDiscSerial(); !serial.empty())
	{
		Path::SanitizeFileName(&serial);
		filename += '_';
		filename += serial;
	}

	const time_t cur_time = time(nullptr);
	char local_time[16];

	if (strftime(local_time, sizeof(local_time), "%Y%m%d%H%M%S", localtime(&cur_time)))
	{
		static time_t prev_snap;
		// The variable 'n' is used for labelling the screenshots when multiple screenshots are taken in
		// a single second, we'll start using this variable for naming when a second screenshot request is detected
		// at the same time as the first one. Hence, we're initially setting this counter to 2 to imply that
		// the captured image is the 2nd image captured at this specific time.
		static int n = 2;

		filename += '_';

		if (cur_time == prev_snap)
			filename += fmt::format("{0}_({1})", local_time, n++);
		else
		{
			n = 2;
			filename += fmt::format("{}", local_time);
		}
		prev_snap = cur_time;
	}

	return filename;
}

std::string GSGetBaseSnapshotFilename()
{
	// If organize by game is enabled, use or create a game-specific folder.
	if (GSConfig.OrganizeSnapshotsByGame)
	{
		const bool prefer_english = Host::GetBaseBoolSettingValue("UI", "PreferEnglishGameList", false);
		std::string game_name = VMManager::GetTitle(prefer_english);
		if (!game_name.empty())
		{
			Path::SanitizeFileName(&game_name);
			const std::string game_dir = Path::Combine(EmuFolders::Snapshots, game_name);

			// Make sure the per-game directory exists or that we can successfully create it.
			if (FileSystem::DirectoryExists(game_dir.c_str()) || FileSystem::CreateDirectoryPath(game_dir.c_str(), false))
				return Path::Combine(game_dir, GSGetBaseFilename());
		}
	}

	return Path::Combine(EmuFolders::Snapshots, GSGetBaseFilename());
}

std::string GSGetBaseVideoFilename()
{
	// If organize by game is enabled, use or create a game-specific folder.
	if (GSConfig.OrganizeVideoCaptureByGame)
	{
		const bool prefer_english = Host::GetBaseBoolSettingValue("UI", "PreferEnglishGameList", false);
		std::string game_name = VMManager::GetTitle(prefer_english);
		if (!game_name.empty())
		{
			Path::SanitizeFileName(&game_name);
			const std::string game_dir = Path::Combine(EmuFolders::Videos, game_name);

			// Make sure the per-game directory exists or that we can successfully create it.
			if (FileSystem::DirectoryExists(game_dir.c_str()) || FileSystem::CreateDirectoryPath(game_dir.c_str(), false))
				return Path::Combine(game_dir, GSGetBaseFilename());
		}
	}
	// prepend video directory
	return Path::Combine(EmuFolders::Videos, GSGetBaseFilename());
}

void GSRenderer::StopGSDump()
{
	m_snapshot = {};
	m_dump_frames = 0;
}

void GSRenderer::PresentCurrentFrame()
{
	if (BeginPresentFrame(false))
	{
		GSTexture* current = g_gs_device->GetCurrent();
		if (current)
		{
			const GSVector4i src_rect(CalculateDrawSrcRect(current, m_real_size));
			const GSVector4 src_uv(GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy());
			const GSVector4 draw_rect(CalculateDrawDstRect(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight(),
				src_rect, current->GetSize(), s_display_alignment, g_gs_device->UsesLowerLeftOrigin(),
				GetVideoMode() == GSVideoMode::SDTV_480P));
			s_last_draw_rect = draw_rect;

			const u64 current_time = Common::Timer::GetCurrentValue();
			const float shader_time = static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - m_shader_time_start));

			g_gs_device->PresentRect(current, src_uv, nullptr, draw_rect,
				s_tv_shader_indices[GSConfig.TVShader], shader_time, BilnIf(GSConfig.LinearPresent != GSPostBilinearMode::Off));
		}
			EndPresentFrame();

			// Orbis: periodic GL/GS readback dump (bring-up diagnosis).
			{
				static unsigned long long orbis_rb = 0;
				if (g_orbis_diag && (orbis_rb++ % 120) == 0) // eerec-280
				{
					OrbisPresentGLFrame();
					OrbisSampleWindow();
				}
			}

	}
}

void GSTranslateWindowToDisplayCoordinates(float window_x, float window_y, float* display_x, float* display_y)
{
	const float draw_width = s_last_draw_rect.z - s_last_draw_rect.x;
	const float draw_height = s_last_draw_rect.w - s_last_draw_rect.y;
	const float rel_x = window_x - s_last_draw_rect.x;
	const float rel_y = window_y - s_last_draw_rect.y;
	if (rel_x < 0 || rel_x > draw_width || rel_y < 0 || rel_y > draw_height)
	{
		*display_x = -1.0f;
		*display_y = -1.0f;
		return;
	}

	*display_x = rel_x / draw_width;
	*display_y = rel_y / draw_height;
}

void GSSetDisplayAlignment(GSDisplayAlignment alignment)
{
	s_display_alignment = alignment;
}

bool GSRenderer::BeginCapture(std::string filename, const GSVector2i& size)
{
	const GSVector2i capture_resolution = (size.x != 0 && size.y != 0) ?
											  size :
											  (GSConfig.VideoCaptureAutoResolution ?
													  GetInternalResolution() :
													  GSVector2i(GSConfig.VideoCaptureWidth, GSConfig.VideoCaptureHeight));

	return GSCapture::BeginCapture(GetTvRefreshRate(), capture_resolution,
		GetCurrentAspectRatioFloat(GetVideoMode() == GSVideoMode::SDTV_480P),
		std::move(filename));
}

void GSRenderer::EndCapture()
{
	GSCapture::EndCapture();
}

GSTexture* GSRenderer::LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size)
{
	return nullptr;
}

bool GSRenderer::IsIdleFrame() const
{
	return (m_last_draw_n == s_n && m_last_transfer_n == s_transfer_n);
}

bool GSRenderer::SaveSnapshotToMemory(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
	u32* width, u32* height, std::vector<u32>* pixels)
{
	GSTexture* const current = g_gs_device->GetCurrent();
	if (!current)
	{
		*width = 0;
		*height = 0;
		pixels->clear();
		return false;
	}

	const GSVector4i src_rect(CalculateDrawSrcRect(current, m_real_size));
	const GSVector4 src_uv(GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy());

	const bool is_progressive = (GetVideoMode() == GSVideoMode::SDTV_480P);
	GSVector4 draw_rect;
	if (window_width == 0 || window_height == 0)
	{
		if (apply_aspect)
		{
			// use internal resolution of the texture
			const float aspect = GetCurrentAspectRatioFloat(is_progressive);
			const int tex_width = current->GetWidth();
			const int tex_height = current->GetHeight();

			// expand to the larger dimension
			const float tex_aspect = static_cast<float>(tex_width) / static_cast<float>(tex_height);
			if (tex_aspect >= aspect)
				draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(tex_width), static_cast<float>(tex_width) / aspect);
			else
				draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(tex_height) * aspect, static_cast<float>(tex_height));
		}
		else
		{
			// uncorrected aspect is only available at internal resolution
			draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(current->GetWidth()), static_cast<float>(current->GetHeight()));
		}
	}
	else
	{
		draw_rect = CalculateDrawDstRect(window_width, window_height, src_rect, current->GetSize(),
			GSDisplayAlignment::LeftOrTop, false, is_progressive);
	}
	const u32 draw_width = static_cast<u32>(draw_rect.z - draw_rect.x);
	const u32 draw_height = static_cast<u32>(draw_rect.w - draw_rect.y);
	const u32 image_width = crop_borders ? draw_width : std::max(draw_width, window_width);
	const u32 image_height = crop_borders ? draw_height : std::max(draw_height, window_height);

	// We're not expecting screenshots to be fast, so just allocate a download texture on demand.
	GSTexture* rt = g_gs_device->CreateRenderTarget(draw_width, draw_height, GSTexture::Format::Color, false);
	if (rt)
	{
		std::unique_ptr<GSDownloadTexture> dl(g_gs_device->CreateDownloadTexture(draw_width, draw_height, GSTexture::Format::Color));
		if (dl)
		{
			const GSVector4i rc(0, 0, draw_width, draw_height);
			g_gs_device->StretchRect(current, src_uv, rt, GSVector4(rc), ShaderConvert::TRANSPARENCY_FILTER, Biln);
			dl->CopyFromTexture(rc, rt, rc, 0);
			dl->Flush();

			if (dl->Map(rc))
			{
				const u32 pad_x = (image_width - draw_width) / 2;
				const u32 pad_y = (image_height - draw_height) / 2;
				pixels->clear();
				pixels->resize(image_width * image_height, 0);
				*width = image_width;
				*height = image_height;
				StringUtil::StrideMemCpy(pixels->data() + pad_y * image_width + pad_x, image_width * sizeof(u32), dl->GetMapPointer(),
					dl->GetMapPitch(), draw_width * sizeof(u32), draw_height);

				g_gs_device->Recycle(rt);
				return true;
			}
		}

		g_gs_device->Recycle(rt);
	}

	*width = 0;
	*height = 0;
	pixels->clear();
	return false;
}

void DumpGSPrivRegs(const GSPrivRegSet& r, const std::string& filename)
{
	auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "wt");
	if (!fp)
		return;

	for (int i = 0; i < 2; i++)
	{
		if (i == 0 && !r.PMODE.EN1)
			continue;
		if (i == 1 && !r.PMODE.EN2)
			continue;

		std::fprintf(fp.get(), "DISPFB%d: { BP: 0x%05x, BW: %u, PSM: %u, DBX: %u, DBY: %u }\n",
			i,
			r.DISP[i].DISPFB.Block(),
			r.DISP[i].DISPFB.FBW,
			r.DISP[i].DISPFB.PSM,
			r.DISP[i].DISPFB.DBX,
			r.DISP[i].DISPFB.DBY);

		std::fprintf(fp.get(), "DISPLAY%d: { DX: %u, DY: %u, DW: %u, DH: %u, MAGH: %u, MAGV: %u }\n",
			i,
			r.DISP[i].DISPLAY.DX,
			r.DISP[i].DISPLAY.DY,
			r.DISP[i].DISPLAY.DW,
			r.DISP[i].DISPLAY.DH,
			r.DISP[i].DISPLAY.MAGH,
			r.DISP[i].DISPLAY.MAGV);
	}

	std::fprintf(fp.get(), "PMODE: { EN1: %u, EN2: %u, CRTMD: %u, MMOD: %u, AMOD: %u, SLBG: %u, ALP: %u }\n",
		r.PMODE.EN1,
		r.PMODE.EN2,
		r.PMODE.CRTMD,
		r.PMODE.MMOD,
		r.PMODE.AMOD,
		r.PMODE.SLBG,
		r.PMODE.ALP);

	std::fprintf(fp.get(),
		"SMODE1: { CLKSEL: %u, CMOD: %u, EX: %u, GCONT: %u, LC: %u, NVCK: %u, PCK2: %u, PEHS: %u, PEVS: %u, PHS: %u, PRST: %u, PVS: %u, RC: %u, SINT: %u, SLCK: %u, SLCK2: %u, SPML: %u, T1248: %u, VCKSEL: %u, VHP: %u, XPCK: %u }\n",
		r.SMODE1.CLKSEL,
		r.SMODE1.CMOD,
		r.SMODE1.EX,
		r.SMODE1.GCONT,
		r.SMODE1.LC,
		r.SMODE1.NVCK,
		r.SMODE1.PCK2,
		r.SMODE1.PEHS,
		r.SMODE1.PEVS,
		r.SMODE1.PHS,
		r.SMODE1.PRST,
		r.SMODE1.PVS,
		r.SMODE1.RC,
		r.SMODE1.SINT,
		r.SMODE1.SLCK,
		r.SMODE1.SLCK2,
		r.SMODE1.SPML,
		r.SMODE1.T1248,
		r.SMODE1.VCKSEL,
		r.SMODE1.VHP,
		r.SMODE1.XPCK);

	std::fprintf(fp.get(), "SMODE2: { INT: %u, FFMD: %u, DPMS: %u }\n",
		r.SMODE2.INT,
		r.SMODE2.FFMD,
		r.SMODE2.DPMS);

	std::fprintf(fp.get(), "SRFSH: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SRFSH.U32[0],
		r.SRFSH.U32[1]);

	std::fprintf(fp.get(), "SYNCH1: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SYNCH1.U32[0],
		r.SYNCH1.U32[1]);

	std::fprintf(fp.get(), "SYNCH2: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SYNCH2.U32[0],
		r.SYNCH2.U32[1]);

	std::fprintf(fp.get(), "SYNCV: { VBP: %u, VBPE: %u, VDP: %u, VFP: %u, VFPE: %u, VS: %u }\n",
		r.SYNCV.VBP,
		r.SYNCV.VBPE,
		r.SYNCV.VDP,
		r.SYNCV.VFP,
		r.SYNCV.VFPE,
		r.SYNCV.VS);

	std::fprintf(fp.get(), "CSR: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.CSR.U32[0],
		r.CSR.U32[1]);

	std::fprintf(fp.get(), "BGCOLOR: { B: %u, G: %u, R: %u }\n",
		r.BGCOLOR.B,
		r.BGCOLOR.G,
		r.BGCOLOR.R);

	std::fprintf(fp.get(), "EXTBUF: { BP: 0x%05x, BW: %u, FBIN: %u, WFFMD: %u, EMODA: %u, EMODC: %u, WDX: %u, WDY: %u }\n",
		r.EXTBUF.EXBP, r.EXTBUF.EXBW, r.EXTBUF.FBIN, r.EXTBUF.WFFMD,
		r.EXTBUF.EMODA, r.EXTBUF.EMODC, r.EXTBUF.WDX, r.EXTBUF.WDY);

	std::fprintf(fp.get(), "EXTDATA: { SX: %u, SY: %u, SMPH: %u, SMPV: %u, WW: %u, WH: %u }\n",
		r.EXTDATA.SX, r.EXTDATA.SY, r.EXTDATA.SMPH, r.EXTDATA.SMPV, r.EXTDATA.WW, r.EXTDATA.WH);

	std::fprintf(fp.get(), "EXTWRITE: { EN: %u }\n", r.EXTWRITE.WRITE);
}
