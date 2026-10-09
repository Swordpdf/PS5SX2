// pcsx2-orbis boot smoke: VMManager init + Execute on worker + pc sampling.
#include <atomic>
#include <cstdarg>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <thread>
#include <chrono>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <strings.h> // 2026-10-08: strcasecmp (an .elf from the shelf)
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "Config.h"
#include "GameDatabase.h" // vk-285-115: the GameDB's MTVU for the live apply
#include "VMManager.h"
#include "R5900.h"
#include "R3000A.h"
#include "Memory.h"
#include "GS/GS.h"
#include "GS/Renderers/Vulkan/VKOrbisTiming.h"
#ifdef ORBIS_VULKAN
#include "../frontend/fe_ps5.h"
#include "../frontend/fe_i18n.h" // vk-285-110: the notifications' text
#include "../frontend/fe_bios.h" // vk-285-134: the BIOS before the shelf
#include "../frontend/fe_games.h" // vk-285-139: fe::DiscSet
#include "ps2/BiosTools.h"       // vk-285-134: IsBIOS, IsBIOSAvailable
#endif // vk-285-36/38: [vkwait], [shaders]
extern volatile unsigned long long g_orbis_map_addr;
#include "vtlb.h"
#include "Host.h"
#ifdef PS5SX2_ACHIEVEMENTS
#include "Achievements.h" // pr9n: OrbisFlushBeforeExit
#include "orbis-shims/ProsperoAchievements.h"
#include "orbis-shims/ProsperoNotify.h" // 2026-10-05: OrbisNotifyHold
#endif
#include "common/Error.h"
#include "common/HostSys.h"
#include "common/CrashHandler.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h" // eerec-285
#include "debug_overlay.h"
#include "OrbisPaths.h" // vk-285-33: the /data/PCSX2 folder layout
#include "OrbisDriver.h" // vk-285-115: ps5vk or RADV
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "SIO/Memcard/MemoryCardFile.h" // vk-285-48: FileMcd_EmuClose/Open
#include "CDVD/CDVD.h"                  // vk-285-48: cdvdSaveNVRAM
#include "SPU2/spu2.h"                  // vk-285-48: SPU2::SetOutputPaused
#include "VUmicro.h"                    // vk-285-76: CpuVU0/CpuVU1->Reset() after a VU codegen switch
#include "OrbisEEDiag.h"                // vk-285-100: OrbisCoreCyclesPerTsc
#include "OrbisDeferredLog.h"            // vk-285-104: orbis_log_drain (the ticker)
#include "orbis-shims/OrbisTextureRoots.h" // vk-285-113: a game's texture pack on a USB drive
#include "orbis-shims/ProsperoKbdMouse.h" // vk-285-72, vk-285-113: the PS5's USB keyboard and mouse
#include "orbis-shims/OrbisPadMap.h"     // vk-285-116: the controller remapping
#include "OrbisNfs.h"                     // vk-285-135: games on NFS shares
#include "orbis-shims/OrbisForward.h"    // --rom from a home screen forwarder
#include <mutex>
#include <set> // vk-285-134
// vk-285-108 (GSRenderer.cpp): the helper threads' CPUs and the ticker's heartbeat for the GS thread's watchdog.
void OrbisHelperThreadAdd(pthread_t thread);
extern "C" const int orbis_ps5vk_war_shim __attribute__((weak)); // vk-285-119 (orbis-shims/orbis_ps5vk_war.c)
extern std::atomic<unsigned long long> g_orbis_ticker_beat;
extern std::atomic<int> g_orbis_ticker_step, g_orbis_ticker_cpu;
extern pthread_t g_orbis_ticker_thread;
extern "C" int sceKernelGetCurrentCpu(void);
#include <dlfcn.h>

// Orbis: DualSense -> PCSX2 port 1 DualShock2 via libScePad (polled on its own thread).
// eerec-278: SW renderer frames presented by GSDeviceOGL (GPU upscale) when /data/PCSX2/swgl exists.
bool g_orbis_sw_on_gl = false;
// eerec-278: bumped by the pad thread when L3+R3 are held ~0.4 s (cycles the present filter).
std::atomic<int> g_orbis_filter_cycle{0};
extern std::atomic<int> g_orbis_state_request; // eerec-282 (StubHost.cpp)
// vk-285-48: back to the menu. The pad thread asks with request 3: since vk-285-49 on a touchpad
// click with L1+R1 held (48 used L3+R3 + D-pad Left). StubHost's PumpMessagesOnCPUThread then calls
// OrbisBackToMenuCpu() at vsync on the CPU thread, which stops the VM; main() writes the memory
// cards and the NVRAM back and re-executes the eboot, whose startup ends in the frontend again.
static std::atomic<bool> g_orbis_menu_request{false};

// vk-285-51: the settings log (frontend/fe_ps5.h orbis_event_log): what the app did, beside what
// the settings page changed, so a setting that breaks a game shows up in one place.
static void orbis_eventf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void orbis_eventf(const char* fmt, ...)
{
#ifdef ORBIS_VULKAN
  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  orbis_event_log(line);
#else
  (void)fmt;
#endif
}

// vk-285-51: the last sessions' logs are kept: <name> is this session's, <name>.1 the one before,
// up to <name>.<keep-1> (boot.1.log, stderr.1.log, emulog.1.txt). 285..50 truncated them at every
// start, so a crash's log was gone once the app was started again.
static void orbis_rotate_log(const std::string& path, int keep)
{
  const size_t slash = path.rfind('/'), dot = path.rfind('.');
  const bool ext = dot != std::string::npos && (slash == std::string::npos || dot > slash);
  const std::string stem = ext ? path.substr(0, dot) : path, suffix = ext ? path.substr(dot) : std::string();
  for (int i = keep - 1; i >= 1; i--)
  {
    const std::string from = i == 1 ? path : stem + "." + std::to_string(i - 1) + suffix;
    rename(from.c_str(), (stem + "." + std::to_string(i) + suffix).c_str());
  }
}

// vk-285-113: boot.log on a console that shows /data only after the jailbreak (v112's console ...575 on firmware
// 10.20: its Helper's hook on the mount didn't take, and the sandbox has no /data before the jailbreak). The
// freopen() of stdout onto a file that can't be created closed it, so the whole boot log of that session was empty:
// nothing was kept of what the app printed until it could write. Now, when boot.log can't be created at the start,
// stdout and stderr go into a pipe that a thread empties into memory (8 MiB at most, then it just throws the rest
// away), and orbis_boot_log_release() writes what was held at the top of boot.log as soon as /data is there.
static bool orbis_boot_log_open(bool rotate)
{
  const std::string boot_log = OrbisLogPath("boot.log");
  // vk-285-51: keep the last 8 sessions' logs (orbis_rotate_log): a crash's log survives a restart.
  if (rotate)
    for (const char* name : {"boot.log", "stderr.log", "emulog.txt"})
      orbis_rotate_log(OrbisLogPath(name), 8);
  // vk-285-51: both streams append, so neither writes over the other. With "w" they still had
  // separate offsets on the console despite the dup2 below: vk-285-50's boot logs start with
  // "[boot] stderr-ok" and the driver's first stderr lines, written over the boot header, and on
  // 2026-09-25 the "[boot] game:"/settings lines vanished under the shelf's driver profile.
  FILE* t = fopen(boot_log.c_str(), "w");
  if (!t)
    return false; // stdout and stderr are as they were: no freopen() that would close them
  fclose(t);
  freopen(boot_log.c_str(), "a", stdout);
  freopen(boot_log.c_str(), "a", stderr);
  // Orbis: the two freopen()s give independent file offsets, so stdout and
  // stderr overwrite each other (crash-time stderr peeks land on top of the
  // boot header). Merge them onto one description: everything appends in
  // order. NOTE: use fileno(), not assumed 1/2 (no console here, so the fds
  // are whatever was free - dup2(1,2) aliased the wrong pair).
  dup2(fileno(stdout), fileno(stderr));
  return true;
}

namespace
{
struct OrbisBootLogHold
{
  std::mutex mutex;
  std::string text;
  size_t dropped = 0;
  int read_fd = -1;
  bool done = false; // the pump saw the end of the pipe
};
OrbisBootLogHold* g_boot_hold = nullptr; // never freed: the pump thread may still be running at exit
std::atomic<bool> g_boot_held{false};
constexpr size_t kBootHoldMax = 8u << 20;
} // namespace

static void* orbis_boot_log_pump(void* arg)
{
  OrbisBootLogHold* h = static_cast<OrbisBootLogHold*>(arg);
  const int fd = h->read_fd;
  char buf[4096];
  for (;;)
  {
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    std::lock_guard<std::mutex> lock(h->mutex);
    if (h->text.size() + static_cast<size_t>(n) <= kBootHoldMax)
      h->text.append(buf, static_cast<size_t>(n));
    else
      h->dropped += static_cast<size_t>(n);
  }
  close(fd);
  std::lock_guard<std::mutex> lock(h->mutex);
  h->done = true;
  return nullptr;
}

static void orbis_boot_log_hold()
{
  int p[2];
  if (pipe(p) != 0)
    return;
  // Both ends well above 0-2: those numbers are free in this process (no console), and stdout and stderr
  // are about to be dup2()ed onto them.
  const int rd = fcntl(p[0], F_DUPFD, 64), wr = fcntl(p[1], F_DUPFD, 64);
  close(p[0]);
  close(p[1]);
  if (rd < 0 || wr < 0)
  {
    if (rd >= 0)
      close(rd);
    if (wr >= 0)
      close(wr);
    return;
  }
  // A full pipe must never stop the app: the write end doesn't block, the pump empties the other end.
  fcntl(wr, F_SETFL, fcntl(wr, F_GETFL, 0) | O_NONBLOCK);
  OrbisBootLogHold* h = new OrbisBootLogHold;
  h->read_fd = rd;
  pthread_t pump;
  if (pthread_create(&pump, nullptr, &orbis_boot_log_pump, h) == 0)
    pthread_detach(pump);
  else
    close(rd); // nobody reads: writes now fail (SIGPIPE is ignored) instead of filling the pipe and blocking
  dup2(wr, fileno(stdout));
  dup2(wr, fileno(stderr));
  close(wr);
  g_boot_hold = h;
  g_boot_held.store(true, std::memory_order_release);
}

// Called after the jailbreak, and again at the later steps of the start: when boot.log can be created now, the held
// lines go to the top of it and stdout and stderr are the file's, as they are at a normal start.
static void orbis_boot_log_release(const char* when)
{
  if (!g_boot_held.load(std::memory_order_acquire))
    return;
  struct stat st = {};
  if (stat("/data/PCSX2", &st) != 0 || !S_ISDIR(st.st_mode))
    return; // still not there
  OrbisBootLogHold* h = g_boot_hold;
  fflush(stdout);
  fflush(stderr);
  if (!orbis_boot_log_open(true))
    return; // the folder is there but boot.log can't be made: keep holding
  g_boot_held.store(false, std::memory_order_release);
  setvbuf(stdout, nullptr, _IOFBF, 1 << 20);
  // The pipe's write ends are gone now (freopen() closed them): the pump reads what is left and stops.
  for (int i = 0; i < 100; i++)
  {
    {
      std::lock_guard<std::mutex> lock(h->mutex);
      if (h->done)
        break;
    }
    usleep(10000);
  }
  std::string text;
  size_t dropped;
  {
    std::lock_guard<std::mutex> lock(h->mutex);
    text.swap(h->text);
    dropped = h->dropped;
  }
  printf("[boot] the next %zu bytes were printed before /data could be written to and are written here (%s)\n", text.size(), when);
  if (dropped)
    printf("[boot] (%zu more bytes of them were dropped: the hold is 8 MiB)\n", dropped);
  fwrite(text.data(), 1, text.size(), stdout);
  printf("[boot] end of the held lines; boot.log goes on directly from here (%s)\n", when);
  // pf.log: the page-fault handler's file, named in logs/ now that it's visible; a fresh one, as at a normal start.
  snprintf(g_orbis_pf_log, sizeof(g_orbis_pf_log), "%s", OrbisLogPath("pf.log").c_str());
  if (FILE* pflog = fopen(g_orbis_pf_log, "w"))
    fclose(pflog);
  fflush(stdout);
}

// vk-285-113: signals. The crash printer (orbis-shims/ProsperoCrash.cpp) covered SIGABRT, SIGILL and SIGFPE (SIGSEGV is the
// page-fault handler's, which hands what it can't map to the printer). Many of v112's sessions ended with no closing
// line at all; they could have died of any of the others: SIGBUS (a file mapped past its end, a misaligned access),
// SIGTRAP (int3), SIGSYS (a system call the kernel refuses), SIGXCPU and SIGXFSZ, or of the system asking the app to end
// (SIGTERM, SIGHUP, SIGINT, SIGQUIT). The first group goes to the printer; the second writes one line to the settings log
// and to boot.log and then ends the app as it would have ended (the handler resets itself, and the signal is raised
// again). SIGKILL can't be caught: the next start says the log had no closing line (fe_ps5.cpp, orbis_event_log).
static int g_orbis_stdout_fd = -1;

static void orbis_quit_signal(int sig)
{
#ifdef ORBIS_VULKAN
  char line[96];
  snprintf(line, sizeof(line), "the system ended the app (signal %d)", sig);
  orbis_event_log(line);
#endif
  if (g_orbis_stdout_fd >= 0)
  {
    static const char msg[] = "[boot] the system ended the app (a signal)\n"; // a raw write: stdio's lock may be held by the thread this interrupted
    (void)!write(g_orbis_stdout_fd, msg, sizeof(msg) - 1);
  }
  raise(sig); // SA_RESETHAND: the default action, once this returns
}

static void orbis_exit_hook()
{
#ifdef ORBIS_VULKAN
  orbis_event_log("app exit (the process is ending)");
#endif
}

static void orbis_install_signal_handlers()
{
  g_orbis_stdout_fd = fileno(stdout);
  struct sigaction fatal;
  memset(&fatal, 0, sizeof(fatal));
  fatal.sa_flags = SA_SIGINFO;
  fatal.sa_sigaction = &CrashHandler::CrashSignalHandler;
  for (const int sig : {SIGABRT, SIGILL, SIGFPE, SIGBUS, SIGTRAP, SIGSYS, SIGXCPU, SIGXFSZ})
    sigaction(sig, &fatal, nullptr);
  // 2026-10-05 (AI-assisted): SIGSEGV too, for the shelf: the page-fault handler (which hands what it can't map to the
  // printer) is installed only after the shelf, so a crash there left nothing but the kernel's report in klog (pr9f).
  // Only while nothing else has SIGSEGV: this function runs again after PageFaultHandler::Install (just before the
  // game), and pr9h set the printer there over the page-fault handler, so a game's first fastmem fault (its first
  // write to a GS register, a second in) went to the printer and ended every game.
  struct sigaction current;
  memset(&current, 0, sizeof(current));
  if (sigaction(SIGSEGV, nullptr, &current) == 0 && !(current.sa_flags & SA_SIGINFO) && current.sa_handler == SIG_DFL)
    sigaction(SIGSEGV, &fatal, nullptr);
  struct sigaction quit;
  memset(&quit, 0, sizeof(quit));
  quit.sa_flags = SA_RESETHAND;
  quit.sa_handler = &orbis_quit_signal;
  for (const int sig : {SIGTERM, SIGHUP, SIGINT, SIGQUIT})
    sigaction(sig, &quit, nullptr);
  signal(SIGPIPE, SIG_IGN);
  static bool s_exit_hook = false;
  if (!s_exit_hook)
  {
    s_exit_hook = true;
    atexit(&orbis_exit_hook);
  }
}

// vk-285-51: "key=value, key=value" from a settings file, for the settings log's game start line.
static std::string orbis_ini_summary(const std::string& path)
{
  FILE* f = fopen(path.c_str(), "r");
  if (!f)
    return "no file";
  std::string out;
  char line[256];
  while (fgets(line, sizeof(line), f))
  {
    std::string l(line);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ' || l.back() == '\t'))
      l.pop_back();
    size_t b = 0;
    while (b < l.size() && (l[b] == ' ' || l[b] == '\t'))
      b++;
    l.erase(0, b);
    if (l.empty() || l[0] == '#' || l[0] == ';' || l.find('=') == std::string::npos)
      continue;
    if (l.compare(0, 11, "EmuCore/GS/") == 0)
      l.erase(0, 11);
    out += (out.empty() ? "" : ", ") + l;
  }
  fclose(f);
  return out.empty() ? "nothing set" : out;
}

// vk-285-113: rumble. PCSX2's DualShock 2 hands its two motors (the big one's strength, the small one on or off) to
// InputManager::SetPadVibrationIntensity, which went nowhere with no input source bound. Input/InputManager.cpp now
// passes the values of PS2 ports 1 and 2 to orbis_pad_vibration; the pad thread sends the changes to the controllers
// with scePadSetVibration. PS5SX2/Rumble=false (the settings page's Controller group) turns it off.
static std::atomic<u32> g_orbis_rumble_state[8]; // (big << 8) | small, each 0..255; by PCSX2's pad index (vk-285-118: all 8)
static std::atomic<int> g_orbis_rumble_on{1};
// vk-285-118: PS5SX2/RumbleStrength (0.25..2, the Controller group's Strength): each motor's value times this, at most full.
static std::atomic<int> g_orbis_rumble_pct{100};
// vk-285-118: PS5SX2/FastSpeed, how fast fast forward runs: 0 as fast as it can, else that many times full speed (StubHost.cpp).
std::atomic<int> g_orbis_fast_speed{0};
// vk-285-116 (AI-assisted): the controller remapping (orbis-shims/OrbisPadMap.h: PS5SX2/Button*, SwapSticks, InvertLeft,
// InvertRight, LeftStickDpad; vk-285-117: the save and load state combos, SaveButton1/2, LoadButton1/2, StateHold; the
// Controls tab). orbis_ps5opts_from sets it at boot and at every live apply; the pad thread copies it when the version
// moves.
static std::mutex g_orbis_padmap_mutex;
static orbis_padmap::Config g_orbis_padmap;
static std::atomic<unsigned> g_orbis_padmap_version{0};
extern "C" void orbis_pad_vibration(unsigned pad_index, float large, float small)
{
  if (pad_index > 7)
    return;
  const auto to_byte = [](float v) { return static_cast<u32>(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f); };
  g_orbis_rumble_state[pad_index].store((to_byte(large) << 8) | to_byte(small), std::memory_order_relaxed);
}
// vk-285-113: the settings page's QR code over the game (GSRenderer.cpp): the fallback when the PS5's browser can't be opened.
extern std::atomic<int> g_orbis_qr_show;

// vk-285-113: L2 + D-pad down held for 2 s opens the settings page in the PS5's own web browser (SceShellCore's launcher,
// libSceSystemService). The game keeps running behind it (the page is served by this app's own web thread). The thread
// below logs what the console answers and then whether the page loads and how often it asks; if the console refuses to
// open the browser, the page's QR code goes over the game instead (the same hold hides it again).
extern "C" int sceSystemServiceLaunchWebBrowser(const char* url, const void* param);
static std::atomic<bool> g_orbis_browser_busy{false};

static double orbis_mono_seconds()
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void orbis_browser_qr_fallback(const char* why)
{
  printf("[browser] %s: the settings page's QR code goes over the game instead\n", why);
  fflush(stdout);
  g_orbis_qr_show.store(1, std::memory_order_relaxed);
}

static void* orbis_browser_thread(void*)
{
  std::string url, shown;
  if (!orbis_web_browser_url(url, shown))
  {
    orbis_browser_qr_fallback("no web server or no network address");
    g_orbis_browser_busy.store(false);
    return nullptr;
  }
  orbis_web_log_next_requests(40); // who loads the page (the console's browser comes from its own address or 127.0.0.1)
  uint64_t base = 0;
  double age = 0.0;
  orbis_web_request_stats(base, age);
  const double t0 = orbis_mono_seconds();
  printf("[browser] opening http://%s/ in the PS5's browser\n", shown.c_str());
  fflush(stdout);
  const int rc = sceSystemServiceLaunchWebBrowser(url.c_str(), nullptr);
  printf("[browser] sceSystemServiceLaunchWebBrowser returned 0x%x after %.0f ms\n", static_cast<unsigned>(rc),
    (orbis_mono_seconds() - t0) * 1000.0);
  fflush(stdout);
  if (rc != 0)
  {
    orbis_browser_qr_fallback("the console refused to open its browser");
    g_orbis_browser_busy.store(false);
    return nullptr;
  }
  // Watch for a minute: does the page load, and does this app keep running while the browser is in front? A gap
  // between these lines longer than 5 s means the system stopped the app.
  double prev = orbis_mono_seconds();
  for (int i = 0; i < 12; i++)
  {
    usleep(5000000);
    const double now = orbis_mono_seconds();
    uint64_t count = 0;
    orbis_web_request_stats(count, age);
    printf("[browser] +%.0f s: %llu page requests since, the last %.0f s ago%s\n", now - t0,
      static_cast<unsigned long long>(count - base), age, now - prev > 8.0 ? " (this app was stopped for a while)" : "");
    fflush(stdout);
    prev = now;
  }
  g_orbis_browser_busy.store(false);
  return nullptr;
}

static void orbis_open_settings_browser()
{
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  const int rc = pthread_create(&th, &attr, &orbis_browser_thread, nullptr);
  pthread_attr_destroy(&attr);
  if (rc != 0)
  {
    orbis_browser_qr_fallback("the browser thread could not start");
    g_orbis_browser_busy.store(false);
  }
}

namespace {
struct OrbisPadData { uint32_t buttons; uint8_t lx, ly, rx, ry, l2, r2, pad0, pad1; uint8_t rest[256]; };

static void orbis_pad_axis(u32 port, u32 pos, u32 neg, int v, int center)
{
  // v: 0..255, center ~128. Small deadzone; PCSX2 applies its own on top.
  const int d = v - center;
  const float dz = 10.0f;
  float fp = 0.0f, fn = 0.0f;
  if (d > dz) fp = std::min(1.0f, (d - dz) / (127.0f - dz));
  if (d < -dz) fn = std::min(1.0f, (-d - dz) / (128.0f - dz));
  Pad::SetControllerState(port, pos, fp);
  Pad::SetControllerState(port, neg, fn);
}

extern "C" {
int scePadInit(void);
int scePadOpen(int32_t userId, int32_t type, int32_t index, const void *param);
int scePadGetHandle(int32_t userId, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void *data);
int scePadSetVibration(int32_t handle, const void *param); // vk-285-113: { uint8_t big, small }
int scePadSetVibrationMode(int32_t handle, int32_t mode);   // vk-285-117: see orbis_pad_rumble_mode
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *userId);
// vk-285-109: the users logged in on the PS5, four ids, -1 for none (how the PS5 SDL port's joystick code
// reads it: ps5-payload-dev/SDL src/joystick/ps5/SDL_ps5joystick.c).
int sceUserServiceGetLoginUserIdList(int32_t *userIds);
}

// ---- Orbis sampling profiler (flag /data/PCSX2/prof): ITIMER_PROF -> SIGPROF on the running thread,
// histogram of interrupted RIP (PS5 mcontext: rip at ctx+0xe0). Reports every 20 s.
#include <sys/time.h>
static constexpr unsigned long long kProfElfBase = 0x400000ULL, kProfElfSize = 0x2000000ULL;
static std::atomic<unsigned> g_prof_elf[kProfElfSize >> 8];
static std::atomic<unsigned> g_prof_jit[256];   // 0x900000000 + i*1MB
static std::atomic<unsigned> g_prof_other, g_prof_lib, g_prof_total;
static void orbis_prof_handler(int, siginfo_t*, void* ctx)
{
  if (!ctx) return;
  const unsigned long long rip = static_cast<const unsigned long long*>(ctx)[0xe0 / 8];
  g_prof_total.fetch_add(1, std::memory_order_relaxed);
  if (rip >= kProfElfBase && rip < kProfElfBase + kProfElfSize)
    g_prof_elf[(rip - kProfElfBase) >> 8].fetch_add(1, std::memory_order_relaxed);
  else if (rip >= 0x900000000ULL && rip < 0x910000000ULL)
    g_prof_jit[(rip - 0x900000000ULL) >> 20 & 255].fetch_add(1, std::memory_order_relaxed);
  else if (rip >= 0x800000000ULL && rip < 0x880000000ULL)
    g_prof_lib.fetch_add(1, std::memory_order_relaxed);
  else
    g_prof_other.fetch_add(1, std::memory_order_relaxed);
}
static void* orbis_prof_report_thread(void*)
{
  for (;;)
  {
    std::this_thread::sleep_for(std::chrono::seconds(20));
    const unsigned total = g_prof_total.exchange(0);
    std::vector<std::pair<unsigned, unsigned>> top;
    for (unsigned i = 0; i < (kProfElfSize >> 8); i++)
    {
      const unsigned c = g_prof_elf[i].exchange(0);
      if (c) top.emplace_back(c, i);
    }
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
    unsigned elf_sum = 0;
    for (auto& t : top) elf_sum += t.first;
    printf("[prof] total=%u elf=%u lib=%u other=%u\n", total, elf_sum, g_prof_lib.exchange(0), g_prof_other.exchange(0));
    for (unsigned i = 0; i < 256; i++)
    {
      const unsigned c = g_prof_jit[i].exchange(0);
      if (c) printf("[prof] jit 0x%llx-MB c=%u\n", 0x900000000ULL + ((unsigned long long)i << 20), c);
    }
    for (size_t i = 0; i < top.size() && i < 60; i++)
      printf("[prof] elf 0x%llx c=%u\n", kProfElfBase + ((unsigned long long)top[i].second << 8), top[i].first);
    fflush(stdout);
  }
  return nullptr;
}
static void orbis_prof_start()
{
  if (!OrbisFlag("prof")) return; // vk-285-33
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = orbis_prof_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  const int r1 = sigaction(SIGPROF, &sa, nullptr);
  struct itimerval it;
  it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 1000;
  it.it_value = it.it_interval;
  const int r2 = setitimer(ITIMER_PROF, &it, nullptr);
  printf("[prof] start sigaction=%d setitimer=%d errno=%d\n", r1, r2, errno);
  fflush(stdout);
  pthread_t t;
  if (pthread_create(&t, nullptr, orbis_prof_report_thread, nullptr) == 0) pthread_detach(t);
}

// vk-285-109: player 2. The PS5 ties each controller to a logged-in user; when a second user is logged in
// at game start, PS2 port 2 gets a DualShock 2 and the pad thread reads that user's controller too (a
// tester: "No second controller recognized in Fifa Street 2"). -1: one user, port 2 stays empty.
static int32_t g_orbis_second_user = -1;
// vk-285-118 (AI-assisted): players 2..4, PS5SX2/Multitap (0 off, 1 a multitap in PS2 port 1, 2 in port 2; read at game
// start). Off: player 2 on port 2, as vk-285-109. Port 1: players 1..4 on 1A..1D (PCSX2's pad indices 0, 2, 3, 4). Port 2:
// player 1 on port 1, players 2..4 on 2A..2C (1, 5, 6). Each extra player is the next logged-in user, with their controller.
static int g_orbis_multitap = 0;
static int g_orbis_extra_count = 0;
static int32_t g_orbis_extra_user[3] = {-1, -1, -1};
static u32 g_orbis_extra_index[3] = {1, 0, 0};

static int32_t orbis_find_second_user(int32_t first)
{
  int32_t ids[16]; // four used; room to spare in case the list is longer on some firmware
  for (int32_t &id : ids)
    id = -1;
  if (sceUserServiceGetLoginUserIdList(ids) != 0)
    return -1;
  for (int i = 0; i < 4; i++)
    if (ids[i] != -1 && ids[i] != first)
      return ids[i];
  return -1;
}

// vk-285-118: the logged-in users other than the first, in the PS5's order (up to 3).
static int orbis_find_other_users(int32_t first, int32_t out[3])
{
  int32_t ids[16];
  for (int32_t &id : ids)
    id = -1;
  if (sceUserServiceGetLoginUserIdList(ids) != 0)
    return 0;
  int n = 0;
  for (int i = 0; i < 4 && n < 3; i++)
    if (ids[i] != -1 && ids[i] != first)
      out[n++] = ids[i];
  return n;
}

// vk-285-109: one controller's buttons, triggers and sticks into a PS2 port (port 0 was the only one).
struct OrbisPadPort
{
  u32 port = 0;
  int32_t handle = -1;
  // vk-285-116: what each PS2 input was last set to (released at the start) and the sticks; only changes are sent.
  float last_value[orbis_padmap::T_COUNT] = {};
  uint8_t last_stick[4] = {128, 128, 128, 128};
};

// vk-285-109: whether the controller behind a handle is connected: the byte 76 bytes into the pad data (after
// the buttons, sticks, triggers, padding, quaternion, velocity, acceleration and touch data; ScePadData's
// "connected", which the PS5 SDL port reads as PS5_PadData.connected). A second user often stays logged in
// with the controller off: player 2 then gets a neutral pad rather than whatever the read returns.
static_assert(offsetof(OrbisPadData, rest) == 12, "OrbisPadData: the fields before rest are 12 bytes");
static bool orbis_pad_connected(const OrbisPadData &d)
{
  return d.rest[76 - 12] != 0;
}

// vk-285-116 (AI-assisted): the buttons go through the remapping (orbis-shims/OrbisPadMap.h); with nothing set it is
// what vk-285-109..115 did: each button on its own, the touchpad's click and the keyboard's Backspace on Select, the
// triggers analog, the sticks as they are. Each PS2 input is set when its value changes (the pressure modifier and the
// analog button act on a change only).
// The pad thread's copy of the controls (it is the only reader: the combos and both ports).
static const orbis_padmap::Config &orbis_padmap_current()
{
  static orbis_padmap::Config s_cfg;
  static unsigned s_version = 0;
  const unsigned version = g_orbis_padmap_version.load(std::memory_order_acquire);
  if (version != s_version)
  {
    std::lock_guard<std::mutex> lock(g_orbis_padmap_mutex);
    s_cfg = g_orbis_padmap;
    s_version = version;
  }
  return s_cfg;
}

static void orbis_pad_apply(OrbisPadPort &p, const OrbisPadData &d)
{
  using namespace orbis_padmap;
  static const u32 bind[T_COUNT] = {
    PadDualshock2::Inputs::PAD_CROSS, PadDualshock2::Inputs::PAD_CIRCLE, PadDualshock2::Inputs::PAD_SQUARE,
    PadDualshock2::Inputs::PAD_TRIANGLE, PadDualshock2::Inputs::PAD_L1, PadDualshock2::Inputs::PAD_R1,
    PadDualshock2::Inputs::PAD_L2, PadDualshock2::Inputs::PAD_R2, PadDualshock2::Inputs::PAD_L3,
    PadDualshock2::Inputs::PAD_R3, PadDualshock2::Inputs::PAD_START, PadDualshock2::Inputs::PAD_SELECT,
    PadDualshock2::Inputs::PAD_UP, PadDualshock2::Inputs::PAD_DOWN, PadDualshock2::Inputs::PAD_LEFT,
    PadDualshock2::Inputs::PAD_RIGHT, PadDualshock2::Inputs::PAD_ANALOG, PadDualshock2::Inputs::PAD_PRESSURE,
  };
  const Config &cfg = orbis_padmap_current();
  State st;
  st.buttons = d.buttons;
  st.l2 = d.l2;
  st.r2 = d.r2;
  st.lx = d.lx;
  st.ly = d.ly;
  st.rx = d.rx;
  st.ry = d.ry;
  const Out o = Apply(cfg, st);
  for (int t = 0; t < T_COUNT; t++)
  {
    if (o.value[t] != p.last_value[t])
    {
      Pad::SetControllerState(p.port, bind[t], o.value[t]);
      p.last_value[t] = o.value[t];
    }
  }
  uint8_t *const last = p.last_stick;
  if (o.lx != last[0]) orbis_pad_axis(p.port, PadDualshock2::Inputs::PAD_L_RIGHT, PadDualshock2::Inputs::PAD_L_LEFT, o.lx, 128);
  if (o.ly != last[1]) orbis_pad_axis(p.port, PadDualshock2::Inputs::PAD_L_DOWN, PadDualshock2::Inputs::PAD_L_UP, o.ly, 128);
  if (o.rx != last[2]) orbis_pad_axis(p.port, PadDualshock2::Inputs::PAD_R_RIGHT, PadDualshock2::Inputs::PAD_R_LEFT, o.rx, 128);
  if (o.ry != last[3]) orbis_pad_axis(p.port, PadDualshock2::Inputs::PAD_R_DOWN, PadDualshock2::Inputs::PAD_R_UP, o.ry, 128);
  last[0] = o.lx; last[1] = o.ly; last[2] = o.rx; last[3] = o.ry;
}

// vk-285-113: one controller's rumble: what the game last asked for (or nothing: Rumble off, or leaving for the menu),
// sent when it changes. The first few changes and any failure are logged (a DualSense in PS4 mode takes the same
// two-motor call).
struct OrbisRumbleOut
{
  u32 sent = 0;
  unsigned logged = 0;
};
static void orbis_rumble_send(int index, int32_t handle, OrbisRumbleOut &out)
{
  if (handle < 0)
    return;
  const bool on = g_orbis_rumble_on.load(std::memory_order_relaxed) != 0 && !g_orbis_menu_request.load(std::memory_order_relaxed);
  u32 want = on ? g_orbis_rumble_state[index].load(std::memory_order_relaxed) : 0u;
  const u32 pct = static_cast<u32>(g_orbis_rumble_pct.load(std::memory_order_relaxed));
  if (want != 0u && pct != 100u)
  {
    const u32 big = std::min<u32>(255u, ((want >> 8) * pct + 50u) / 100u), small = std::min<u32>(255u, ((want & 0xFFu) * pct + 50u) / 100u);
    want = (big << 8) | small;
  }
  if (want == out.sent)
    return;
  const uint8_t param[2] = {static_cast<uint8_t>(want >> 8), static_cast<uint8_t>(want & 0xFFu)};
  const int rc = scePadSetVibration(handle, param);
  if (out.logged < 8u || rc != 0)
  {
    out.logged++;
    if (out.logged <= 12u)
    {
      printf("[pad] rumble port %d: big %u small %u -> rc=%x\n", index + 1, want >> 8, want & 0xFFu, static_cast<unsigned>(rc));
      fflush(stdout);
    }
  }
  out.sent = want;
}

// vk-285-117 (AI-assisted): the DualSense didn't rumble. Every scePadSetVibration answered 0 (4,355 "[pad] rumble" lines in
// the testers' reports) and nothing was felt: a PS5 title's controllers start in haptics mode, where the actuators play
// the vibration audio port and the two-motor values go nowhere, until scePadSetVibrationMode puts the pad in rumble mode
// (libScePad's modes as the PC libScePad has them: 1 haptics, 2 rumble). Needs checking on a console with a DualSense and
// a DualShock 4: flags rumble_mode0 to rumble_mode3 try another value, rumble_mode_off leaves the pad as it is.
static void orbis_pad_rumble_mode(int32_t handle, const char *who)
{
  if (handle < 0)
    return;
  if (OrbisFlag("rumble_mode_off"))
  {
    printf("[pad] %s: vibration mode left as it is (flag rumble_mode_off)\n", who);
    fflush(stdout);
    return;
  }
  int mode = 2;
  for (int m = 0; m <= 3; m++)
  {
    char name[32];
    snprintf(name, sizeof(name), "rumble_mode%d", m);
    if (OrbisFlag(name))
      mode = m;
  }
  const int rc = scePadSetVibrationMode(handle, mode);
  printf("[pad] %s: vibration mode %d%s -> rc=%x\n", who, mode, mode == 2 ? " (rumble)" : "", static_cast<unsigned>(rc));
  fflush(stdout);
}

static void *orbis_pad_thread(void *)
{
  auto p_read = [](int32_t h, OrbisPadData *d) { return scePadReadState(h, d); };
  void *pad = (void *)&scePadInit, *us = (void *)&sceUserServiceInitialize;
  int32_t user = -1;
  int rc_ui = sceUserServiceInitialize(nullptr);
  int rc_uu = sceUserServiceGetInitialUser(&user);
  int rc_pi = scePadInit();
  int handle = user >= 0 ? scePadOpen(user, 0, 0, nullptr) : -999;
  if (handle < 0 && user >= 0)
    handle = scePadGetHandle(user, 0, 0);
  printf("[pad] lib=%p us=%p ui=%x uu=%x user=%d init=%x handle=%d\n", pad, us, rc_ui, rc_uu, user, rc_pi, handle);
  fflush(stdout);
  if (handle < 0)
    return nullptr;
  orbis_pad_rumble_mode(handle, "player 1"); // vk-285-117
  OrbisPadPort port1;
  port1.port = 0;
  port1.handle = handle;
  // vk-285-109: player 2's controller, when a second user was logged in at game start (main() gave PS2
  // port 2 a DualShock 2 then). vk-285-118: players 3 and 4 too with a multitap (g_orbis_extra_*).
  OrbisPadPort extra[3];
  for (int i = 0; i < g_orbis_extra_count; i++)
  {
    extra[i].port = g_orbis_extra_index[i];
    extra[i].handle = scePadOpen(g_orbis_extra_user[i], 0, 0, nullptr);
    if (extra[i].handle < 0)
      extra[i].handle = scePadGetHandle(g_orbis_extra_user[i], 0, 0);
    char who[16];
    snprintf(who, sizeof(who), "player %d", i + 2);
    printf("[pad] %s: user=%d handle=%d (PCSX2 pad %u)\n", who, g_orbis_extra_user[i], extra[i].handle, extra[i].port + 1);
    fflush(stdout);
    orbis_pad_rumble_mode(extra[i].handle, who); // vk-285-117
  }
  unsigned reads = 0;
  for (;;)
  {
    OrbisPadData d;
    memset(&d, 0, sizeof(d));
    int rc = p_read(handle, &d);
    // vk-285-113: the PS5's USB keyboard and mouse as the PS2 controller (orbis-shims/ProsperoKbdMouse.cpp): the keys held,
    // the mouse's buttons and its aim are added to what the DualSense reports (a button held on either is held; a stick the
    // keyboard or mouse moves is theirs while they move it). With no DualSense data at all they stand alone.
    {
      static bool s_kbm_was_active = false;
      OrbisKbdMousePad kp;
      const bool kbm = OrbisKbdMousePadState(kp);
      const bool active = kbm && (kp.buttons != 0 || kp.l2 != 0 || kp.r2 != 0 || kp.lx != 128 || kp.ly != 128 ||
                                  kp.rx != 128 || kp.ry != 128);
      if (rc != 0 && (active || s_kbm_was_active))
      {
        memset(&d, 0, sizeof(d));
        d.lx = d.ly = d.rx = d.ry = 128;
        rc = 0;
      }
      if (kbm)
      {
        d.buttons |= kp.buttons;
        d.l2 = std::max(d.l2, kp.l2);
        d.r2 = std::max(d.r2, kp.r2);
        if (kp.lx != 128) d.lx = kp.lx;
        if (kp.ly != 128) d.ly = kp.ly;
        if (kp.rx != 128) d.rx = kp.rx;
        if (kp.ry != 128) d.ry = kp.ry;
      }
      s_kbm_was_active = active;
      if (const int hotkey = OrbisKbdMouseTakeHotkey())
      {
        g_orbis_state_request.store(hotkey, std::memory_order_release);
        printf("[pad] keyboard: %s\n", hotkey == 1 ? "F1, save the state" : hotkey == 2 ? "F3, load the state" : "Esc held, back to the menu");
        fflush(stdout);
      }
    }
    if (rc == 0)
    {
      // vk-285-117 (AI-assisted): save and load the state (slot 1) with the combos the Controls tab chose (two buttons each,
      // held together for PS5SX2/StateHold; orbis-shims/OrbisPadMap.h ComboWatch). By default L3+R3 + D-pad up saves and
      // + D-pad down loads, at once, as since eerec-282 (now the D-pad press that does it is kept from the game). The
      // touchpad's sides are eerec-284's zones (1.51's touch + Cross is TouchLeft/TouchRight + Cross here). They read the
      // controller's real buttons (and the keyboard's), before the remapping.
      bool state_fired = false;
      {
        const orbis_padmap::Config &cfg = orbis_padmap_current();
        static orbis_padmap::ComboWatch s_save, s_load, s_fast, s_disc;
        orbis_padmap::ComboState cs;
        cs.buttons = d.buttons;
        cs.l2 = d.l2;
        cs.r2 = d.r2;
        const unsigned touches = d.rest[40];
        uint16_t tx = 0xffffu;
        memcpy(&tx, &d.rest[48], sizeof(tx));
        cs.touch = (touches == 0 || tx >= 4096u) ? 0 : (tx < 640u) ? 1 : (tx >= 1280u) ? 2 : 0;
        const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
        uint32_t block = 0;
        const bool save = s_save.Update(cfg.save, cfg.hold_ms, cs, now_ms, block);
        const bool load = s_load.Update(cfg.load, cfg.hold_ms, cs, now_ms, block) && !save;
        // vk-285-118: the fast forward combo (StubHost.cpp turns it on or off on the CPU thread).
        const bool fast = s_fast.Update(cfg.fast, cfg.hold_ms, cs, now_ms, block) && !save && !load;
        // vk-285-139: the change disc combo (the next disc of the game's set; StubHost.cpp runs it on the CPU thread).
        const bool disc = s_disc.Update(cfg.disc, cfg.hold_ms, cs, now_ms, block) && !save && !load && !fast;
        if (disc)
        {
          g_orbis_state_request.store(6, std::memory_order_release);
          state_fired = true;
          printf("[pad] change disc: %s\n", orbis_padmap::DescribeCombo(cfg.disc, cfg.hold_ms).c_str());
          fflush(stdout);
        }
        if (fast)
        {
          g_orbis_state_request.store(4, std::memory_order_release);
          state_fired = true;
          printf("[pad] fast forward on/off: %s\n", orbis_padmap::DescribeCombo(cfg.fast, cfg.hold_ms).c_str());
          fflush(stdout);
        }
        if (save || load)
        {
          g_orbis_state_request.store(save ? 1 : 2, std::memory_order_release);
          state_fired = true;
          printf("[pad] %s the state (slot 1): %s\n", save ? "save" : "load",
            orbis_padmap::DescribeCombo(save ? cfg.save : cfg.load, cfg.hold_ms).c_str());
          fflush(stdout);
        }
        d.buttons &= ~block;
        if (block & 0x100u)
          d.l2 = 0;
        if (block & 0x200u)
          d.r2 = 0;
      }
      // eerec-282: L3+R3 held ~0.4 s and let go without the D-pad (or a save or load) cycles the present filter
      // (eerec-278 fired while held). (vk-285-73's notification test on D-pad Right/Left is gone in vk-285-74.)
      {
        static unsigned combo = 0;
        static bool used = false;
        static uint32_t prev_dpad = 0;
        const uint32_t dpad = d.buttons & 0xF0u; // Up 0x10, Right 0x20, Down 0x40, Left 0x80
        if ((d.buttons & 0x6u) == 0x6u)
        {
          combo++;
          if (((dpad & ~prev_dpad) & 0x50u) || state_fired)
            used = true;
        }
        else
        {
          if (combo >= 100u && !used)
            g_orbis_filter_cycle.fetch_add(1, std::memory_order_relaxed);
          combo = 0;
          used = false;
        }
        prev_dpad = dpad;
      }
      // vk-285-49: L1+R1 held and a touchpad click = back to the menu (the user's choice). The click
      // that triggers it is held back from the game (it is Select there) until it is released.
      {
        static bool s_prev_click = false, s_click_consumed = false;
        const bool click = (d.buttons & 0x00100000u) != 0; // the touchpad button
        if (click && !s_prev_click && (d.buttons & 0x00000C00u) == 0x00000C00u) // L1 0x400, R1 0x800
        {
          g_orbis_state_request.store(3, std::memory_order_release);
          s_click_consumed = true;
          printf("[pad] L1+R1+touchpad click: back to the menu\n");
          fflush(stdout);
        }
        if (!click)
          s_click_consumed = false;
        s_prev_click = click;
        if (s_click_consumed)
          d.buttons &= ~0x00100000u;
      }
      // vk-285-113: L2 + D-pad down held for 2 s: the settings page in the PS5's own browser (or, with the QR code up,
      // takes the QR code down). The game keeps running (and sees the buttons: a 2 s hold is nobody's move).
      {
        static bool s_holding = false, s_fired = false;
        static std::chrono::steady_clock::time_point s_since;
        const bool l2 = (d.buttons & 0x00000100u) != 0 || d.l2 >= 200u;
        const bool down = (d.buttons & 0x00000040u) != 0;
        if (l2 && down)
        {
          const auto now = std::chrono::steady_clock::now();
          if (!s_holding)
          {
            s_holding = true;
            s_fired = false;
            s_since = now;
          }
          else if (!s_fired && now - s_since >= std::chrono::seconds(2))
          {
            s_fired = true;
            if (g_orbis_qr_show.load(std::memory_order_relaxed))
            {
              g_orbis_qr_show.store(0, std::memory_order_relaxed);
              printf("[pad] L2 + D-pad down held for 2 s: settings QR hidden\n");
              fflush(stdout);
              orbis_eventf("settings page QR code hidden (L2 + D-pad down held for 2 s)");
            }
            else if (!g_orbis_browser_busy.exchange(true))
            {
              printf("[pad] L2 + D-pad down held for 2 s: opening the settings page in the PS5's browser\n");
              fflush(stdout);
              orbis_eventf("settings page: opening it in the PS5's browser (L2 + D-pad down held for 2 s)");
              orbis_open_settings_browser();
            }
          }
        }
        else
          s_holding = false;
      }
      orbis_pad_apply(port1, d);
    }
    {
      static OrbisRumbleOut s_rumble1, s_rumble_extra[3];
      orbis_rumble_send(0, handle, s_rumble1);
      for (int i = 0; i < g_orbis_extra_count; i++)
        orbis_rumble_send(static_cast<int>(extra[i].port), extra[i].handle, s_rumble_extra[i]);
    }
    if (++reads == 250u || (rc != 0 && reads % 1000u == 0u))
      printf("[pad] read rc=%x buttons=%08x lx=%u ly=%u\n", rc, d.buttons, d.lx, d.ly);
    for (int i = 0; i < g_orbis_extra_count; i++)
    {
      OrbisPadPort &px = extra[i];
      if (px.handle < 0)
        continue;
      OrbisPadData d2;
      memset(&d2, 0, sizeof(d2));
      const int rc2 = p_read(px.handle, &d2);
      const bool on = rc2 == 0 && orbis_pad_connected(d2);
      if (on)
        orbis_pad_apply(px, d2);
      else
      {
        // Released buttons, centred sticks: nothing held on this port while its controller is off.
        OrbisPadData idle;
        memset(&idle, 0, sizeof(idle));
        idle.lx = idle.ly = idle.rx = idle.ry = 128;
        orbis_pad_apply(px, idle);
      }
      static int s_on[3] = {-1, -1, -1};
      if (s_on[i] != (on ? 1 : 0))
      {
        printf("[pad] player %d controller %s (rc=%x)\n", i + 2, on ? "connected" : "not connected", rc2);
        s_on[i] = on ? 1 : 0;
      }
      if (reads == 250u || (rc2 != 0 && reads % 1000u == 0u))
        printf("[pad] player %d read rc=%x buttons=%08x lx=%u ly=%u connected=%u\n", i + 2, rc2, d2.buttons, d2.lx, d2.ly,
          static_cast<unsigned>(d2.rest[76 - 12]));
    }
    usleep(4000);
  }
  return nullptr;
}
} // namespace

extern "C" volatile unsigned long long orbis_fault_count;
volatile unsigned long long orbis_fault_count = 0;

extern "C" int g_jailbreak_ok = 0;
#ifndef ORBIS_VULKAN
extern "C" void orbis_gl_probe();
#endif
// The renderer the GPU device runs: OpenGL in the GL build, Vulkan (GSDeviceVK on
// the linked Swordpdf/PS5HB_Vulkan driver) in the Vulkan build (Makefile.vk).
#ifdef ORBIS_VULKAN
static constexpr GSRendererType ORBIS_GPU_RENDERER = GSRendererType::VK;
#else
static constexpr GSRendererType ORBIS_GPU_RENDERER = GSRendererType::OGL;
#endif
// Orbis: true = PCSX2 OpenGL renderer (GPU) + no CPU overlay; false = SW renderer + overlay.
// Orbis: SW renderer + CPU overlay is the known-good demo path (correct output,
// slow). The OpenGL path (GPU) needs the ps5-opengl driver's per-draw cost
// fixed; eerec-83 tests the driver patch that removes the per-draw pipeline
// drain and the 1ms poll floor.
static bool g_use_gl_renderer = true;
// Orbis test switches (files in /data/PCSX2, read once at boot):
//   sw_renderer  -> PCSX2 software rasterizer (GL device still presents)
//   nospeedhacks -> vu1Instant/vuFlagHack off
// vk-285-32: per-game settings. /data/PCSX2/settings/<the disc image's name>.ini (e.g.
// "settings/Ratchet & Clank 3.ini") holds lines like gs.ini's and is applied after it, so its values
// win. Its path is set once the game selector has picked the image; GSRenderer.cpp's live reload
// watches it next to gs.ini (OrbisGameIniPath).
static std::string s_game_ini_path;
const char* OrbisGameIniPath() { return s_game_ini_path.c_str(); }

// Orbis: lines "Section/Key=value" (Section defaults to EmuCore/GS); '#' or ';' starts a comment.
static void orbis_apply_ini_file(MemorySettingsInterface& si, const char* path, const char* tag, bool quiet = false)
{
  FILE* f = fopen(path, "r");
  if (!f) return;
  const auto trim = [](std::string& t) {
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ' || t.back() == '\t')) t.pop_back();
    size_t b = 0;
    while (b < t.size() && (t[b] == ' ' || t[b] == '\t')) b++;
    t.erase(0, b);
  };
  char line[256];
  while (fgets(line, sizeof(line), f))
  {
    std::string l(line);
    trim(l); // vk-285-32: spaces around the line and around '=' are fine
    if (l.empty() || l[0] == '#' || l[0] == ';') continue;
    const size_t eq = l.find('=');
    if (eq == std::string::npos) continue;
    std::string key = l.substr(0, eq), val = l.substr(eq + 1), sec = "EmuCore/GS";
    trim(key);
    trim(val);
    const size_t sl = key.rfind('/');
    if (sl != std::string::npos) { sec = key.substr(0, sl); key = key.substr(sl + 1); }
    // pr9n (AI-assisted), PR #9 review item 4: nothing in [Achievements] from gs.ini or a game's file (the settings page
    // writes these files for anyone on the LAN): Achievements/Host would have had the next sign-in send the token to
    // another server. The account comes from the sign-in and achievements-secrets.ini only.
    {
      std::string first = sec.substr(0, sec.find('/'));
      for (char& c : first)
        c = static_cast<char>(tolower((unsigned char)c));
      if (first == "achievements")
      {
        printf("[boot] %s %s/%s ignored: RetroAchievements settings come from the sign-in only\n", tag, sec.c_str(), key.c_str());
        continue;
      }
    }
    // vk-285-34: PCSX2's patch and cheat lists ("Patches/Enable=60 FPS" turns on a pnach's [60 FPS]
    // group): each line adds one name, so a game can list several.
    if ((sec == "Patches" || sec == "Cheats") && (key == "Enable" || key == "Disable"))
    {
      si.AddToStringList(sec.c_str(), key.c_str(), val.c_str());
      if (!quiet)
        printf("[boot] %s %s/%s += %s\n", tag, sec.c_str(), key.c_str(), val.c_str());
      continue;
    }
    if (val == "true" || val == "false") si.SetBoolValue(sec.c_str(), key.c_str(), val == "true");
    else if (!val.empty() && (isdigit((unsigned char)val[0]) || val[0] == '-') && val.find_first_not_of("-0123456789") == std::string::npos)
      si.SetIntValue(sec.c_str(), key.c_str(), atoi(val.c_str()));
    // vk-285-54: one dot at most, so an IP address (DEV9/Eth/DNS1=67.222.156.250) stays a string;
    // as a float it became 67.222 and PCSX2 read no address.
    else if (!val.empty() && val.find_first_not_of("-0123456789.") == std::string::npos &&
             std::count(val.begin(), val.end(), '.') <= 1)
      si.SetFloatValue(sec.c_str(), key.c_str(), (float)atof(val.c_str()));
    else si.SetStringValue(sec.c_str(), key.c_str(), val.c_str());
    if (!quiet)
      printf("[boot] %s %s/%s=%s\n", tag, sec.c_str(), key.c_str(), val.c_str());
  }
  fclose(f);
  fflush(stdout);
}

static void orbis_apply_gs_ini(MemorySettingsInterface& si, bool quiet = false)
{
  orbis_apply_ini_file(si, "/data/PCSX2/gs.ini", "gs.ini", quiet);
  if (!s_game_ini_path.empty()) // vk-285-32: then the game's own
    orbis_apply_ini_file(si, s_game_ini_path.c_str(), "game ini", quiet);
#ifdef PS5SX2_ACHIEVEMENTS
  OrbisAchievementsRestoreAccount(si); // pr9n: the account's keys as the secrets file has them, whatever a merge did
#endif
}

// vk-285-64: pcsx2/Memory.cpp's choice of memory for the recompilers' code (the flag file jitdirect).
extern "C" int g_orbis_code_direct;

static bool orbis_flag(const char *name)
{
  const bool on = OrbisFlag(name); // vk-285-33: flags/<name>, or the top folder
  printf("[boot] flag %s=%d\n", name, (int)on);
  fflush(stdout);
  return on;
}
static bool g_sw_renderer = false;
static bool g_no_speedhacks = false;
static bool g_vu_interp = false;
static bool g_ee_interp = false;
static bool g_iop_interp = false;
extern "C" int sceSystemServiceLoadExec(const char* path, const char* argv[]);
// 2026-10-08 (AI-assisted): frame generation's 120 Hz output outlives the process that asked for it (PS5_Vulkan R51), so
// every way out of the process that leaves the GS's swapchain open (the menu, a restart, the exit, a new build) puts the
// default output back first (the driver's ps5vk_output_restore_default, vk-285-131; nothing without it or at 60 Hz).
extern "C" bool ps5vk_output_restore_default(void) __attribute__((weak));
static void orbis_output_default()
{
  if (ps5vk_output_restore_default)
    ps5vk_output_restore_default();
}

// etaHEN non-whitelist jailbreak: send JAILBREAK_CMD to the legacy CMD server
// (127.0.0.1:9028). Grants the process full ucred/caps (JIT, direct memory...).
static void orbis_try_jailbreak()
{
    struct HijackerCommand
    {
        int magic = 0xDEADBEEF;
        int cmd;
        int PID;
        int ret = -1337;
        char msg1[0x500];
        char msg2[0x500];
    } cmd;

    cmd.magic = 0xDEADBEEF;
    cmd.cmd = 5; // JAILBREAK_CMD
    cmd.PID = (int)getpid();
    memset(cmd.msg1, 0, sizeof(cmd.msg1));
    memset(cmd.msg2, 0, sizeof(cmd.msg2));

    // Try the etaHEN legacy CMD server (9028) and the SharpProspero unjail
    // daemon (9069). Both use the same 0xDEADBEEF / cmd 5 protocol.
    for (int port : {9028, 9069})
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            printf("[jailbreak] socket failed errno=%d\n", errno);
            fflush(stdout);
            return;
        }
        sockaddr_in sa = {};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(0x7F000001); // 127.0.0.1
        if (connect(fd, (sockaddr*)&sa, sizeof(sa)) < 0)
        {
            printf("[jailbreak] connect port %d failed errno=%d\n", port, errno);
            fflush(stdout);
            close(fd);
            continue;
        }
        if (send(fd, (const void*)&cmd, sizeof(cmd), 0) != (int)sizeof(cmd))
        {
            printf("[jailbreak] send failed errno=%d\n", errno);
            fflush(stdout);
            close(fd);
            continue;
        }
        int got = 0;
        while (got < (int)sizeof(cmd))
        {
            int r = recv(fd, (char*)&cmd + got, sizeof(cmd) - got, 0);
            if (r <= 0)
                break;
            got += r;
        }
        close(fd);
        printf("[jailbreak] port %d ret=%d got=%d\n", port, cmd.ret, got);
        fflush(stdout);
        if (cmd.ret == 0 || cmd.ret == -1337)
        {
            g_jailbreak_ok = 1;
            return;
        }
    }
    printf("[jailbreak] all ports failed\n");
    fflush(stdout);
}

extern "C" void orbis_stage(const char* stage)
{
    ps5::debug::set_line(5, "stage: %s", stage);
}

static u8 g_worker_stack[16 * 1024 * 1024];
static void* worker_main(void*)
{
    printf("[boot] Execute begin\n");
    fflush(stdout);
    ps5::debug::set_line(2, "Execute begin");
    ps5::debug::set_line(3, "booting...");
    VMManager::Execute();
    printf("[boot] Execute returned\n");
    fflush(stdout);
    ps5::debug::set_line(2, "Execute returned");
    return nullptr;
}

static MemorySettingsInterface s_base_si;
static MemorySettingsInterface s_game_si;
static MemorySettingsInterface s_input_si;

// vk-285-75: EmuCore/Speedhacks/OrbisVU1Speed (percent, 25..800, default 100) from the settings as
// gs.ini and the game's file left them, into pcsx2/MTVU.cpp's g_orbis_vu1_speed. It only matters with
// Instant VU1 off: the EE then sees VU1 busy for the recent programs' cycles divided by it.
extern std::atomic<u32> g_orbis_vu1_speed;
// vk-285-76: EmuCore/CPU/Recompiler/OrbisVUFastMinMax (bool, default off): VU MAX/MINI as SSE float
// max/min (pcsx2/x86/microVU_Misc.inl). Returns true when it changed, so a live apply can recompile.
extern std::atomic<int> g_orbis_vu_fast_minmax;
static bool orbis_vu1_speed_from(const SettingsInterface& si)
{
  int v = 100;
  si.GetIntValue("EmuCore/Speedhacks", "OrbisVU1Speed", &v);
  const u32 speed = static_cast<u32>(std::clamp(v, 25, 800));
  if (speed != g_orbis_vu1_speed.load(std::memory_order_relaxed))
    printf("[boot] VU1 speed %u%% (EmuCore/Speedhacks/OrbisVU1Speed; with Instant VU1 off)\n", speed);
  g_orbis_vu1_speed.store(speed, std::memory_order_relaxed);

  // vk-285-79: EmuCore/Gamefixes/OrbisVIF1ExecEarly (bool, default off): a program queued by VIF1's MSCAL
  // runs before the next unpack instead of after up to 3 (pcsx2/Vif_Unpack.cpp). Takes effect at once.
  {
    extern std::atomic<int> g_orbis_vif1_exec_early;
    bool early = false;
    si.GetBoolValue("EmuCore/Gamefixes", "OrbisVIF1ExecEarly", &early);
    if (g_orbis_vif1_exec_early.exchange(early ? 1 : 0, std::memory_order_relaxed) != (early ? 1 : 0))
      printf("[boot] VIF1 queued program before the next unpack: %s (EmuCore/Gamefixes/OrbisVIF1ExecEarly)\n", early ? "on" : "off");
  }

  // vk-285-84: EmuCore/Speedhacks/OrbisMTVUSpin (default on: the VU thread spins ~50 us for work before
  // it sleeps) and OrbisMTVUBatch (default off: a VIF1 transfer wakes the VU thread once, at its end).
  // pcsx2/MTVU.cpp; both take effect at once.
  {
    extern std::atomic<int> g_orbis_mtvu_spin;
    extern std::atomic<int> g_orbis_mtvu_batch;
    bool spin = true, batch = false;
    si.GetBoolValue("EmuCore/Speedhacks", "OrbisMTVUSpin", &spin);
    si.GetBoolValue("EmuCore/Speedhacks", "OrbisMTVUBatch", &batch);
    const int old_spin = g_orbis_mtvu_spin.exchange(spin ? 1 : 0, std::memory_order_relaxed);
    const int old_batch = g_orbis_mtvu_batch.exchange(batch ? 1 : 0, std::memory_order_relaxed);
    if (old_spin != (spin ? 1 : 0) || old_batch != (batch ? 1 : 0))
      printf("[boot] MTVU: VU thread spin %s, kick batching %s (EmuCore/Speedhacks/OrbisMTVUSpin, OrbisMTVUBatch)\n",
        spin ? "on" : "off", batch ? "on" : "off");
    // vk-285-90: EmuCore/Speedhacks/OrbisMTVURingKB (256..16384, default 16384 = the whole buffer): the
    // EE-to-VU ring's length, taken at its next wrap (pcsx2/MTVU.cpp).
    extern std::atomic<u32> g_orbis_mtvu_ring_kb;
    int ring_kb = 16384;
    si.GetIntValue("EmuCore/Speedhacks", "OrbisMTVURingKB", &ring_kb);
    const u32 ring = static_cast<u32>(std::clamp(ring_kb, 256, 16384));
    if (g_orbis_mtvu_ring_kb.exchange(ring, std::memory_order_relaxed) != ring)
      printf("[boot] MTVU ring %u KB (EmuCore/Speedhacks/OrbisMTVURingKB)\n", ring);
  }

  bool fast = false;
  si.GetBoolValue("EmuCore/CPU/Recompiler", "OrbisVUFastMinMax", &fast);
  const int prev = g_orbis_vu_fast_minmax.exchange(fast ? 1 : 0, std::memory_order_relaxed);
  if (prev != (fast ? 1 : 0))
  {
    printf("[boot] fast VU MIN/MAX %s (EmuCore/CPU/Recompiler/OrbisVUFastMinMax)\n", fast ? "on" : "off");
    return true;
  }
  return false;
}

// vk-285-113: PS5SX2/Overlay, PS5SX2/FpsGraph and PS5SX2/Rumble from gs.ini and the game's file, live (the settings
// page's Overlay and Controller groups). Overlay: 0 no box, 1 FPS, 2 FPS and the EE/GS/VU loads; unset, or anything
// else: live.ini's fps= and perf= decide, as before. GSRenderer.cpp draws the box and the graph; the pad thread sends
// the rumble.
extern std::atomic<int> g_orbis_overlay_mode;
extern std::atomic<int> g_orbis_fps_graph;
extern std::atomic<int> g_orbis_ra_challenge; // 2026-10-08: PS5SX2/RAChallengeIcons (GSRenderer.cpp OrbisDrawChallengeIcons)
// 2026-10-08 (AI-assisted): the overlay picture (GSRenderer.cpp OrbisDrawBezel): PS5SX2/Bezel and PS5SX2/BezelDir, live.
extern std::atomic<int> g_orbis_bezel;
extern std::atomic<u32> g_orbis_bezel_gen;
static std::mutex g_orbis_bezel_mutex;
static std::string g_orbis_bezel_dir;
std::string OrbisBezelDirectory()
{
  std::lock_guard<std::mutex> lock(g_orbis_bezel_mutex);
  return g_orbis_bezel_dir;
}
static void orbis_set_bezel(bool on, const std::string& dir)
{
  {
    std::lock_guard<std::mutex> lock(g_orbis_bezel_mutex);
    if (on == (g_orbis_bezel.load(std::memory_order_relaxed) != 0) && dir == g_orbis_bezel_dir)
      return;
    g_orbis_bezel_dir = dir;
    g_orbis_bezel.store(on ? 1 : 0, std::memory_order_relaxed);
  }
  g_orbis_bezel_gen.fetch_add(1, std::memory_order_release);
  printf("[boot] overlay picture %s (PS5SX2/Bezel), from %s\n", on ? "on" : "off",
    dir.empty() ? "/data/PCSX2/overlays" : (dir + ", then /data/PCSX2/overlays (PS5SX2/BezelDir)").c_str());
  fflush(stdout);
}

// vk-285-113: a game's texture pack outside /data/PCSX2/textures: PS5SX2/TexturesDir/<serial>, or a USB drive's
// PS5SX2/textures, PCSX2/textures or textures folder (orbis-shims/OrbisTextureRoots.h; GSTextureReplacements.cpp asks
// when a game starts). The setting is kept here under a lock: the GS thread asks, the CPU thread reads the files.
static std::mutex g_orbis_textures_mutex;
static std::string g_orbis_textures_dir;
static void orbis_set_textures_dir(const std::string& dir)
{
  std::lock_guard<std::mutex> lock(g_orbis_textures_mutex);
  if (dir == g_orbis_textures_dir)
    return;
  g_orbis_textures_dir = dir;
  printf("[boot] PS5SX2/TexturesDir=%s\n", dir.empty() ? "(not set: USB drives, then /data/PCSX2/textures)" : dir.c_str());
  fflush(stdout);
}
std::string OrbisTexturesGameDir(const std::string& serial, std::string& how)
{
  std::string manual;
  {
    std::lock_guard<std::mutex> lock(g_orbis_textures_mutex);
    manual = g_orbis_textures_dir;
  }
  return OrbisTextures::FindGameDir(OrbisTextures::UsbRoots("/mnt"), manual, serial, &how);
}

static void orbis_ps5opts_from(const SettingsInterface& si)
{
  s32 overlay = -1;
  if (!si.GetIntValue("PS5SX2", "Overlay", &overlay) || overlay < 0 || overlay > 2)
    overlay = -1;
  bool graph = false, rumble = true, challenge = true;
  si.GetBoolValue("PS5SX2", "FpsGraph", &graph);
  si.GetBoolValue("PS5SX2", "Rumble", &rumble);
  si.GetBoolValue("PS5SX2", "RAChallengeIcons", &challenge); // 2026-10-08 (AI-assisted): the RetroAchievements challenge icons
  {
    bool bezel = false; // 2026-10-08 (AI-assisted): the overlay picture
    std::string bezel_dir;
    si.GetBoolValue("PS5SX2", "Bezel", &bezel);
    si.GetStringValue("PS5SX2", "BezelDir", &bezel_dir);
    orbis_set_bezel(bezel, bezel_dir);
  }
  if (g_orbis_ra_challenge.exchange(challenge ? 1 : 0, std::memory_order_relaxed) != (challenge ? 1 : 0))
  {
    printf("[boot] RetroAchievements challenge icons %s (PS5SX2/RAChallengeIcons)\n", challenge ? "on" : "off");
    fflush(stdout);
  }
  const int old_overlay = g_orbis_overlay_mode.exchange(overlay, std::memory_order_relaxed);
  const int old_graph = g_orbis_fps_graph.exchange(graph ? 1 : 0, std::memory_order_relaxed);
  const int old_rumble = g_orbis_rumble_on.exchange(rumble ? 1 : 0, std::memory_order_relaxed);
  int rumble_pct = 100;
  {
    std::string v;
    if (si.GetStringValue("PS5SX2", "RumbleStrength", &v))
    {
      char *end = nullptr;
      const double k = std::strtod(v.c_str(), &end);
      if (end != v.c_str() && k >= 0.1 && k <= 3.0)
        rumble_pct = static_cast<int>(k * 100.0 + 0.5);
    }
  }
  const int old_rumble_pct = g_orbis_rumble_pct.exchange(rumble_pct, std::memory_order_relaxed);
  {
    const int fast = si.GetIntValue("PS5SX2", "FastSpeed", 0);
    g_orbis_fast_speed.store(fast >= 2 && fast <= 8 ? fast : 0, std::memory_order_relaxed);
  }
  std::string textures_dir; // vk-285-113: PS5SX2/TexturesDir, where a game's texture packs are besides USB drives
  si.GetStringValue("PS5SX2", "TexturesDir", &textures_dir);
  orbis_set_textures_dir(textures_dir);
  // vk-285-113: the USB keyboard and mouse as the PS2 controller (orbis-shims/ProsperoKbdMouse.cpp): PS5SX2/KeyboardMouse
  // 0 Auto, 1 Controller only, 2 USB devices only, 3 Off; MouseAim 0 nothing, 1 right stick, 2 left stick; MouseSpeed 1..4;
  // MouseButtons 0 R1 and L1, 1 R2 and L2.
  OrbisKbdMouseConfigure(si.GetIntValue("PS5SX2", "KeyboardMouse", 0), si.GetIntValue("PS5SX2", "MouseAim", 1),
    si.GetIntValue("PS5SX2", "MouseSpeed", 2), si.GetIntValue("PS5SX2", "MouseButtons", 0));
  // vk-285-116 (AI-assisted): the remapping and (vk-285-117) the save and load combos (the Controls tab).
  {
    const orbis_padmap::Config map = orbis_padmap::FromSettings([&](const char* key, std::string& value) {
      return si.GetStringValue("PS5SX2", key, &value);
    });
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(g_orbis_padmap_mutex);
      if (map != g_orbis_padmap)
      {
        g_orbis_padmap = map;
        changed = true;
      }
    }
    if (changed)
    {
      g_orbis_padmap_version.fetch_add(1, std::memory_order_release);
      printf("[boot] controls: %s (PS5SX2/Button*, SwapSticks, InvertLeft, InvertRight, LeftStickDpad, DeadzoneLeft, DeadzoneRight, SaveButton*, LoadButton*, StateHold)\n",
        orbis_padmap::Describe(map).c_str());
      fflush(stdout);
    }
  }
  if (old_overlay != overlay || old_graph != (graph ? 1 : 0) || old_rumble != (rumble ? 1 : 0) || old_rumble_pct != rumble_pct)
  {
    printf("[boot] on-screen box %s, FPS graph %s, rumble %s at %d%% (PS5SX2/Overlay, FpsGraph, Rumble, RumbleStrength)\n",
      overlay < 0 ? "as live.ini says" : overlay == 0 ? "off" : overlay == 1 ? "FPS" : "FPS and loads", graph ? "on" : "off",
      rumble ? "on" : "off", rumble_pct);
    fflush(stdout);
  }
}

// vk-285-113: PS5SX2/KeyboardMouse 1 (Controller only) and 3 (Off) take the PS2's USB keyboard and mouse off the ports
// (0 Auto and 2 USB devices only leave them where boot put them: USB1 hidkbd, USB2 hidmouse, unless the game's own file
// says otherwise). Applied to the base settings at boot and to every live reload, so the setting plugs them in and out of
// a running game.
static void orbis_usb_kbm_for_mode(SettingsInterface& si)
{
  const int mode = si.GetIntValue("PS5SX2", "KeyboardMouse", 0);
  if (mode != 1 && mode != 3)
    return;
  std::string type;
  if (si.GetStringValue("USB1", "Type", &type) && type == "hidkbd")
    si.SetStringValue("USB1", "Type", "None");
  if (si.GetStringValue("USB2", "Type", &type) && type == "hidmouse")
    si.SetStringValue("USB2", "Type", "None");
}

// eerec-285: gs.ini re-read while running (GSRenderer.cpp OrbisLiveTune sees the change, StubHost's
// PumpMessagesOnCPUThread calls this at vsync on the CPU thread).
static MemorySettingsInterface s_base_pre_gsini; // the base layer as it was before boot applied gs.ini
extern std::atomic<int> g_orbis_live_reapply;
void OrbisOSDLabel(const char* text);

// vk-285-115 (AI-assisted): MTVU as the running game will have it: the game database's mtvu speed hack wins over the
// settings while game fixes are on (VMManager::ApplyGameFixes). Comparing the settings' value alone made every live
// change in such a game (PCSX2's 67 "mtvu: 0" entries, and now PS5SX2's own for Contra, GT3 PAL and Castlevania) read
// as an MTVU switch: "nothing applied ... a relaunch applies them".
static bool orbis_mtvu_after_gamedb(bool from_settings)
{
  if (!EmuConfig.EnableGameFixes)
    return from_settings;
  const GameDatabaseSchema::GameEntry* game = GameDatabase::findGame(VMManager::GetDiscSerial());
  if (game)
  {
    for (const auto& hack : game->speedHacks)
    {
      if (hack.first == SpeedHack::MTVU)
        return hack.second != 0;
    }
  }
  return from_settings;
}

#ifdef ORBIS_VULKAN
// 2026-10-08 (AI-assisted): frame generation's latency hold (see the boot's "frame generation" block): the game runs 0.08% slow so that
// two presents a frame at 120 Hz never fill the swapchain's queue. Applied to the settings layer at the boot and at each live reload
// (which rebuilds the layer from the files).
extern bool g_orbis_fg_wanted;
static void orbis_fg_hold(MemorySettingsInterface& si)
{
  if (!g_orbis_fg_wanted)
    return;
  const float scalar = si.GetFloatValue("Framerate", "NominalScalar", 1.0f);
  si.SetFloatValue("Framerate", "NominalScalar", scalar * 0.9992f);
}
#endif

void orbis_reload_gs_ini_cpu();
// vk-285-118 (AI-assisted): GPU readbacks to Don't wait for the rest of the game, asked for by GSRenderer.cpp's
// OrbisReadbackAutoSecond (older firmware's readback stall). Not when gs.ini or the game's file sets HWDownloadMode: that
// choice stays. Put under gs.ini in the base layer, so a live reload keeps it and a later HWDownloadMode in those files
// still wins.
extern std::atomic<int> g_orbis_rb_auto_kind; // vk-285-119 (GSRenderer.cpp): 1 each wait 10 ms+, 2 many short waits
void OrbisReadbackAutoCpu()
{
  const bool many = g_orbis_rb_auto_kind.load(std::memory_order_relaxed) == 2;
  MemorySettingsInterface peek;
  orbis_apply_gs_ini(peek, true);
  if (peek.ContainsValue("EmuCore/GS", "HWDownloadMode"))
  {
    printf("[readbacks] not switched: HWDownloadMode=%d is set in gs.ini or the game's file\n",
      peek.GetIntValue("EmuCore/GS", "HWDownloadMode", 0));
    orbis_eventf(many ? "readbacks: they take 300 ms a second or more and the game is slow, but HWDownloadMode is set in "
                        "gs.ini or the game's file: left as it is" :
                        "readbacks: each one takes 10 ms or more and the game is slow, but HWDownloadMode is set in gs.ini or the "
                        "game's file: left as it is");
    fflush(stdout);
    return;
  }
  s_base_pre_gsini.SetIntValue("EmuCore/GS", "HWDownloadMode", static_cast<int>(GSHardwareDownloadMode::Unsynchronized));
  orbis_reload_gs_ini_cpu();
  OrbisOSDLabel("READBACKS: DON'T WAIT (AUTO)");
  orbis_eventf(many ? "readbacks: waiting for them took 300 ms a second or more and slowed the game, so GPU readbacks are on "
                      "Don't wait (auto) for this game; set GPU readbacks to Accurate to keep them, or add the flag noautoreadback" :
                      "readbacks: each one took 10 ms or more and slowed the game, so GPU readbacks are on Don't wait (auto) "
                      "for this game; set GPU readbacks to Accurate to keep them, or add the flag noautoreadback");
  printf("[readbacks] GPU readbacks set to Don't wait (HWDownloadMode 3) for this game\n");
  fflush(stdout);
}

void orbis_reload_gs_ini_cpu()
{
  MemorySettingsInterface trial = s_base_pre_gsini;
  orbis_apply_gs_ini(trial);
#ifdef ORBIS_VULKAN
  orbis_fg_hold(trial); // 2026-10-08
#endif
  orbis_usb_kbm_for_mode(trial); // vk-285-113
  Pcsx2Config next;
  {
    SettingsLoadWrapper slw(trial);
    next.LoadSave(slw);
  }
  bool kept = false;
  if (next.GS.Renderer != EmuConfig.GS.Renderer || !next.GS.RestartOptionsAreEqual(EmuConfig.GS) ||
      orbis_mtvu_after_gamedb(next.Speedhacks.vuThread) != EmuConfig.Speedhacks.vuThread)
  {
    // vk-285-50: the settings page can change a relaunch-only option (MTVU, the renderer, a GS
    // device option) together with live ones. Those keep their running values and the rest
    // applies now; 285..49 refused the whole change, so a pending MTVU switch blocked every other
    // live change to the game until the next launch.
    static const char* const restart_keys[][2] = {
      {"EmuCore/GS", "Renderer"}, {"EmuCore/GS", "Adapter"}, {"EmuCore/GS", "UseDebugDevice"},
      {"EmuCore/GS", "UseBlitSwapChain"}, {"EmuCore/GS", "DisableShaderCache"},
      {"EmuCore/GS", "DisableFramebufferFetch"}, {"EmuCore/GS", "DisableVertexShaderExpand"},
      {"EmuCore/GS", "OverrideTextureBarriers"}, {"EmuCore/GS", "DepthFeedbackMode"}, {"EmuCore/GS", "HWAA1"},
      {"EmuCore/GS", "ExclusiveFullscreenControl"}, {"EmuCore/Speedhacks", "vuThread"}};
    for (const auto& k : restart_keys)
    {
      std::string v;
      if (s_base_si.GetStringValue(k[0], k[1], &v))
        trial.SetStringValue(k[0], k[1], v.c_str());
      else
        trial.DeleteValue(k[0], k[1]);
    }
    next = Pcsx2Config();
    {
      SettingsLoadWrapper slw(trial);
      next.LoadSave(slw);
    }
    if (next.GS.Renderer != EmuConfig.GS.Renderer || !next.GS.RestartOptionsAreEqual(EmuConfig.GS) ||
        orbis_mtvu_after_gamedb(next.Speedhacks.vuThread) != EmuConfig.Speedhacks.vuThread)
    {
      printf("[gsini] not applied: the renderer, a GS device option or MTVU changed - those need a relaunch\n");
      fflush(stdout);
      orbis_eventf("live apply: nothing applied, the renderer, a GS device option or MTVU changed (a relaunch applies them)");
      OrbisOSDLabel("GS.INI: RELAUNCH");
      return;
    }
    printf("[gsini] the renderer, GS device options and MTVU keep their running values until the next launch\n");
    kept = true;
  }
  {
    std::unique_lock<std::mutex> lock = Host::GetSettingsLock();
    s_base_si = trial;
  }
  const bool vu_codegen_changed = orbis_vu1_speed_from(trial); // vk-285-75, vk-285-76
  orbis_ps5opts_from(trial); // vk-285-113
  VMManager::ApplySettings();
  if (vu_codegen_changed)
  {
    // vk-285-76: recompile the VU programs with the new MAX/MINI code (we are on the CPU thread at vsync;
    // recMicroVU1::Reset waits for the VU thread first).
    CpuVU0->Reset();
    CpuVU1->Reset();
    printf("[gsini] VU recompilers reset (fast VU MIN/MAX %s)\n", g_orbis_vu_fast_minmax.load() ? "on" : "off");
  }
  // vk-285-34: a changed patch list (Patches/Enable) or a new pnach applies live too; PCSX2 only
  // re-reads them on its own when one of the patch switches changes.
  VMManager::ReloadPatches(true, true, false, true);
  g_orbis_live_reapply.store(1, std::memory_order_release);
  {
    extern std::atomic<int> g_orbis_mtvu_spin;
    extern std::atomic<int> g_orbis_mtvu_batch;
    extern std::atomic<u32> g_orbis_mtvu_ring_kb;
    printf("[gsini] applied: EECycleRate=%d FrameratePAL=%.2f extrathreads=%d TVShader=%d vuThread=%d vu1Instant=%d vu1Speed=%u mtvuSpin=%d mtvuBatch=%d mtvuRingKB=%u\n",
      (int)EmuConfig.Speedhacks.EECycleRate, (double)EmuConfig.GS.FrameratePAL, (int)EmuConfig.GS.SWExtraThreads,
      (int)EmuConfig.GS.TVShader, (int)EmuConfig.Speedhacks.vuThread, (int)EmuConfig.Speedhacks.vu1Instant,
      g_orbis_vu1_speed.load(std::memory_order_relaxed), g_orbis_mtvu_spin.load(std::memory_order_relaxed),
      g_orbis_mtvu_batch.load(std::memory_order_relaxed), g_orbis_mtvu_ring_kb.load(std::memory_order_relaxed)); // vk-285-84/90
  }
  fflush(stdout);
  orbis_eventf(kept ? "live apply: applied in the running game; MTVU and the other relaunch-only options wait for the next launch" :
                      "live apply: applied in the running game");
  OrbisOSDLabel(kept ? "APPLIED (SOME AT NEXT LAUNCH)" : "GS.INI APPLIED");
}

typedef struct notify_request
{
  char useless1[45];
  char message[3075];
} notify_request_t;

extern "C" int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
extern "C" int sceKernelConfiguredFlexibleMemorySize(unsigned long long* size);
extern "C" int sceKernelAvailableFlexibleMemorySize(unsigned long long* size);
extern "C" int sceKernelAvailableDirectMemorySize(unsigned long long start, unsigned long long end,
                                                  unsigned long long alignment, long long* phys,
                                                  unsigned long long* size);
extern "C" void* mmap(void*, unsigned long, int, int, int, long);
extern "C" int munmap(void*, unsigned long);

static void sys_notify(const char* msg)
{
  notify_request_t req;
  memset(&req, 0, sizeof(req));
  strncpy(req.message, msg, sizeof(req.message) - 1);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

#ifndef ORBIS_BUILD_TAG
#ifndef ORBIS_BUILD_TAG
#define ORBIS_BUILD_TAG "eerec-285-live"
#endif
#endif

// Test build 1 (vk-285-55): link-vk.sh's ORBIS_TEST_BUILD=N makes testing build N. It says so on
// the shelf and over the game (the TESTING watermark), in the logs and on the settings page, which
// also offers the logs as one file for the testers' Discord. 0: a normal build.
#ifndef ORBIS_TEST_BUILD
#define ORBIS_TEST_BUILD 0
#endif
extern "C" int g_orbis_test_build = ORBIS_TEST_BUILD;
// The in-game watermark (GSRenderer.cpp OrbisWatermark), made once the game is picked.
std::vector<uint32_t> g_orbis_watermark;
int g_orbis_watermark_w = 0, g_orbis_watermark_h = 0;

// "Test build 1 · vk-285-55", or the plain tag.
static std::string orbis_build_label()
{
  if (g_orbis_test_build <= 0)
    return ORBIS_BUILD_TAG;
  return "Test build " + std::to_string(g_orbis_test_build) + " \xC2\xB7 " ORBIS_BUILD_TAG;
}

// vk-285-105 (swordpdf): testing builds point at the testers' Discord, in smaller text under the build
// on the shelf and over the game. Empty in a normal build.
static std::string orbis_test_note()
{
  if (g_orbis_test_build <= 0)
    return std::string();
  return "Join: https://discord.gg/7gZxXt4PM";
}

// Test build 1: what the console is, for the logs. Only calls every app may make; the SoC id
// call gets a pointer too, so it works whichever of the two forms it has (the value comes back
// either as the result or through the pointer).
extern "C" {
int sceKernelGetProsperoSystemSwVersion(void* version);
unsigned long long sceKernelGetCpuFrequency(void);
int sceKernelGetMainSocId(unsigned int* id);
int sceKernelGetCpumode(void);
int sysctlbyname(const char* name, void* oldp, size_t* oldlenp, const void* newp, size_t newlen);
}

// vk-285-101: the CPU clocks, for the question whether the PS5 Pro's 3.85 GHz CPU mode is on for this app. The
// TSC measured against steady_clock over 50 ms, and this thread's core clock from a chain of dependent adds
// (OrbisCoreCyclesPerTsc, the best of 3): user-mode timing only. vk-285-100 also called five kernel mode getters
// (sceKernelIsNeoMode and others, with guessed signatures) here; one of them faulted at boot on the Pro, and the
// console panicked after tearing the app down. No kernel calls here. The EE and GS threads' cores are measured
// again in play ([cpuclk] every 5 s, OrbisEEDiag.cpp).
static void orbis_clock_survey()
{
  const auto t0 = std::chrono::steady_clock::now();
  const unsigned long long c0 = __builtin_ia32_rdtsc();
  auto t1 = t0;
  while ((t1 = std::chrono::steady_clock::now()) - t0 < std::chrono::milliseconds(50))
  {
  }
  const unsigned long long c1 = __builtin_ia32_rdtsc();
  const double sec = std::chrono::duration<double>(t1 - t0).count();
  const double tsc_mhz = sec > 0.0 ? static_cast<double>(c1 - c0) / sec / 1e6 : 0.0;
  double ratio = 0.0;
  for (int i = 0; i < 3; i++)
    ratio = std::max(ratio, OrbisCoreCyclesPerTsc());
  printf("[cpuclk] boot: measured TSC %.2f MHz, main-thread core %.0f MHz (%.4f cycles/tick)\n", tsc_mhz,
    ratio * tsc_mhz, ratio);
}
static std::string s_console_info; // "firmware 11.40 · SoC ... · CPU ..." (one line)

// pr9n (AI-assisted): the product part of RetroAchievements' User-Agent (pcsx2/Host.cpp, PR #9 review item 10):
// "PS5SX2/vk-285-128-pr9n (PS5 11.40)". The firmware is the survey's (orbis_console_survey runs before any RA call).
std::string OrbisUserAgentProduct()
{
  std::string fw = "?";
  const size_t at = s_console_info.find("firmware ");
  if (at != std::string::npos)
  {
    fw = s_console_info.substr(at + 9);
    fw = fw.substr(0, fw.find(' '));
  }
  return std::string("PS5SX2/") + ORBIS_BUILD_TAG + " (PS5 " + fw + ")";
}

static std::string orbis_console_survey()
{
  std::string info;
  // The firmware: {size_t size = 0x28; char text[0x1C]; uint32 version}, as on the PS4. The buffer
  // is bigger than that in case the console's structure is.
  alignas(8) unsigned char sw[256] = {};
  *reinterpret_cast<unsigned long long*>(sw) = 0x28;
  const int sw_rc = sceKernelGetProsperoSystemSwVersion(sw);
  char fw_text[0x1D] = {};
  memcpy(fw_text, sw + 8, 0x1C);
  unsigned int fw_ver = 0;
  memcpy(&fw_ver, sw + 0x24, 4);
  printf("[console] firmware: rc=%#x text=\"%s\" version=%#x\n", (unsigned)sw_rc, fw_text, fw_ver);
  char buf[160];
  if (sw_rc == 0 && fw_text[0])
  {
    // "11.40", from "11.400.001" or similar.
    std::string t = fw_text;
    const size_t dot = t.find('.');
    if (dot != std::string::npos && t.size() >= dot + 3)
      t = t.substr(0, dot + 3);
    while (t.size() > 1 && t[0] == '0' && t[1] != '.')
      t.erase(0, 1);
    info += "firmware " + t;
  }
  else
  {
    snprintf(buf, sizeof(buf), "firmware ? (rc %#x)", (unsigned)sw_rc);
    info += buf;
  }
  unsigned int soc_out = 0;
  const int soc_rc = sceKernelGetMainSocId(&soc_out);
  printf("[console] main SoC id: result=%#x out=%#x\n", (unsigned)soc_rc, soc_out);
  snprintf(buf, sizeof(buf), " \xC2\xB7 SoC %#x/%#x", (unsigned)soc_rc, soc_out);
  info += buf;
  const unsigned long long hz = sceKernelGetCpuFrequency();
  const int cpumode = sceKernelGetCpumode();
  printf("[console] CPU: %llu Hz, cpumode %d\n", hz, cpumode);
  orbis_clock_survey(); // vk-285-100
  if (hz != 0)
    snprintf(buf, sizeof(buf), " \xC2\xB7 CPU %.2f GHz, mode %d", static_cast<double>(hz) / 1e9, cpumode);
  else
  {
    // vk-285-72: some consoles answer 0 here (every 12.00 one in the test reports, and some at 8.40
    // and 10.01), which the header showed as "CPU 0.00 GHz". The kernel's own clock numbers go to
    // boot.log for comparison; the header leaves the clock out rather than show a wrong one.
    unsigned long long tsc = 0;
    size_t len = sizeof(tsc);
    const int tsc_rc = sysctlbyname("machdep.tsc_freq", &tsc, &len, nullptr, 0);
    int clockrate = 0;
    len = sizeof(clockrate);
    const int rate_rc = sysctlbyname("hw.clockrate", &clockrate, &len, nullptr, 0);
    printf("[console] CPU clock not reported; machdep.tsc_freq rc=%d %llu Hz, hw.clockrate rc=%d %d MHz\n", tsc_rc,
           tsc_rc == 0 ? tsc : 0ULL, rate_rc, rate_rc == 0 ? clockrate : 0);
    snprintf(buf, sizeof(buf), " \xC2\xB7 CPU mode %d", cpumode);
  }
  info += buf;
  {
    char model[128] = {};
    size_t len = sizeof(model) - 1;
    if (sysctlbyname("hw.model", model, &len, nullptr, 0) == 0 && model[0])
    {
      printf("[console] hw.model: %s\n", model);
      info += std::string(" (") + model + ")";
    }
    else
      printf("[console] hw.model: unavailable (errno %d)\n", errno);
    int sdk = 0;
    len = sizeof(sdk);
    if (sysctlbyname("kern.sdk_version", &sdk, &len, nullptr, 0) == 0)
      printf("[console] kern.sdk_version: %#x\n", (unsigned)sdk);
  }
  printf("[console] %s\n", info.c_str());
  fflush(stdout);
  return info;
}

// A home screen forwarder's --exit-after-game (orbis-shims/OrbisForward.h): back to the menu closes the app.
static bool s_forward_exit_after_game = false;

// vk-285-48: at vsync on the CPU thread (StubHost.cpp, request 3): stop the VM. Execute returns at
// the next event test and main() takes it from there (orbis_back_to_menu).
void OrbisBackToMenuCpu()
{
  if (g_orbis_menu_request.exchange(true))
    return;
  printf("[menu] back to the menu: stopping the VM\n");
  fflush(stdout);
  orbis_eventf("back to the menu");
  OrbisOSDLabel("BACK TO MENU");
  VMManager::SetState(VMState::Stopping);
}

// vk-285-109: the end of the process without the static destructors and exit handlers, which abort when
// PCSX2 is only half started (a failed start, or no game): the logs out first.
// vk-285-115 (AI-assisted): through the system, not _exit(). On the console a title's _exit() (and a return from
// main, exit()) ends in SIGSYS inside libkernel: 1.50 (vk-285-113), the first build with a SIGSYS handler, reported
// a crash for every start that found no game (1,537 of the 1,554 crash reports of 2026-09-28..10-01: "signal 12 at
// eboot+0x7ffc003ac", right after "[boot] no game to start: closing"); 112 died of the same signal without a line.
// sceSystemServiceLoadExec("exit") ends the app properly (RPCS3-PS5 does it: its klog shows sceApplicationExitSpawn3,
// "Kill for LoadExec", "Terminating pid"). It works asynchronously: wait for it, and _exit() only if it never comes.
[[noreturn]] void OrbisExitApp(int status)
{
  orbis_output_default();
  orbis_log_drain();
  fflush(stdout);
  fflush(stderr);
  const int rc = sceSystemServiceLoadExec("exit", nullptr);
  printf("[boot] exit %d: sceSystemServiceLoadExec(\"exit\") returned 0x%08x; waiting for the system to close the app\n", status,
    (unsigned)rc);
  orbis_log_drain();
  fflush(stdout);
  if (rc == 0)
    for (int i = 0; i < 100; i++)
      usleep(100000);
  printf("[boot] exit %d: the system didn't close the app in 10 s; _exit()\n", status);
  orbis_log_drain();
  fflush(stdout);
  _exit(status);
}

[[noreturn]] static void orbis_exit_quietly(int status)
{
  OrbisExitApp(status);
}

// vk-285-109: our own eboot again, into the shelf, after a start that failed (the VM never ran, so no memory
// card or NVRAM to write back). Returns when LoadExec doesn't take.
static void orbis_restart_to_menu()
{
  const char* path = "/data/homebrew/PPSA99203/eboot.bin";
  struct stat st{};
  if (stat(path, &st) != 0)
    path = "/app0/eboot.bin";
  printf("[menu] the game didn't start: re-executing %s\n", path);
  orbis_output_default();
  orbis_log_drain();
  fflush(stdout);
  fflush(stderr);
  const int rc = sceSystemServiceLoadExec(path, nullptr);
  if (rc == 0)
    for (int i = 0; i < 100; i++)
      usleep(100000);
  printf("[menu] LoadExec(%s) returned %x: closing instead\n", path, (unsigned)rc);
}

// vk-285-48: the VM has stopped for the menu. The memory cards' RAM cache and the NVRAM go back to
// disk, then our own eboot is executed again: its startup (the jailbreak included, as for the eboot
// watcher's new builds) ends in the frontend, with this game preselected. When LoadExec fails, the
// game carries on where it stopped.
static void orbis_back_to_menu()
{
  printf("[menu] VM stopped; writing the memory cards and the NVRAM back\n");
  fflush(stdout);
  SPU2::SetOutputPaused(true);
  FileMcd_EmuClose();
  cdvdSaveNVRAM();
#ifdef PS5SX2_ACHIEVEMENTS
  // pr9n (AI-assisted), PR #9 review item 9: unlocks and leaderboard entries still being sent went with the process.
  // Up to 5 s for them (a retry of one that failed included), on this thread, the one that polled them in the game;
  // a notification says so when they don't make it. Returns at once when nothing is waiting.
  Achievements::OrbisFlushBeforeExit(5000);
#endif
  if (s_forward_exit_after_game)
  {
    // A forwarder's game with --exit-after-game: the cards and the NVRAM are on disk, so out to the home screen.
    printf("[menu] the forwarded game ended (--exit-after-game): closing\n");
    orbis_eventf("the forwarded game ended: the app closed (--exit-after-game)");
    OrbisExitApp(0);
  }
  const char* path = "/data/homebrew/PPSA99203/eboot.bin";
  struct stat st{};
  if (stat(path, &st) != 0)
    path = "/app0/eboot.bin";
  printf("[menu] re-executing %s\n", path);
  orbis_output_default();
  fflush(stdout);
  fflush(stderr);
  const int rc = sceSystemServiceLoadExec(path, nullptr);
  // It doesn't come back when it works; allow for one that returns first and ends the process later.
  if (rc == 0)
    for (int i = 0; i < 100; i++)
      usleep(100000);
  printf("[menu] LoadExec(%s) returned %x and we're still here: back to the game\n", path, (unsigned)rc);
  fflush(stdout);
  sys_notify(fe::Tr(fe::Str::NotifyMenuFailed));
  FileMcd_EmuOpen();
  SPU2::SetOutputPaused(false);
  g_orbis_menu_request.store(false);
  VMManager::SetState(VMState::Running);
}

#ifdef ORBIS_VULKAN
// Test build 1 (vk-285-55): the USB folders games are listed from, looked up before the jailbreak
// (for the cover prefetch; the sandbox may not show the drives yet) and again after it.
static std::vector<std::string> s_usb_dirs;

// 2026-10-08 (AI-assisted; testers: "have users select their bios and games scan path (across the entire possible paths)
// some people want ext/usb or m2"): gs.ini's PS5SX2/GameFolders, folders separated by ';' (the settings page, all games),
// listed like the drives' (with one level of folders in them), and PS5SX2/BiosFolder, where the BIOS is looked for first.
// The drives themselves (/mnt/usb0-7, /mnt/ext0-1) are looked through without them (orbis_usb_game_dirs, BiosTools.cpp).
static std::string orbis_gs_ini_string(const char* key)
{
  MemorySettingsInterface peek;
  orbis_apply_ini_file(peek, "/data/PCSX2/gs.ini", "gs.ini", true);
  std::string value;
  peek.GetStringValue("PS5SX2", key, &value);
  return value;
}

static std::vector<std::string> orbis_extra_game_folders(const char* when)
{
  std::vector<std::string> dirs;
  const std::string list = orbis_gs_ini_string("GameFolders");
  size_t at = 0;
  while (at <= list.size())
  {
    size_t end = list.find_first_of(";|", at);
    if (end == std::string::npos)
      end = list.size();
    std::string dir = list.substr(at, end - at);
    while (!dir.empty() && (dir.back() == ' ' || dir.back() == '/'))
      dir.pop_back();
    while (!dir.empty() && dir.front() == ' ')
      dir.erase(0, 1);
    at = end + 1;
    if (dir.empty())
      continue;
    struct stat st = {};
    const bool ok = dir[0] == '/' && stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    if (when)
      printf("[boot] %s: game folder %s (PS5SX2/GameFolders)%s\n", when, dir.c_str(), ok ? "" : ": not a folder here, skipped");
    if (ok && std::find(dirs.begin(), dirs.end(), dir) == dirs.end())
      dirs.push_back(dir);
  }
  if (when && !dirs.empty())
    fflush(stdout);
  return dirs;
}

// vk-285-135 (AI-assisted; testers: "nfs mounting"): gs.ini's PS5SX2/NfsShares, nfs:// addresses separated by ';' (the
// settings page's Folders group, the shelf's folder picker), mounted after the jailbreak (orbis-shims/OrbisNfs.cpp: the
// app's own NFS client) and listed like the drives: each share's folder under /nfs/<host>/<path>. All at once, each try
// giving up after 5 s; a share that didn't mount is said in boot.log and left out, and the shelf starts without it.
static std::vector<std::string> orbis_nfs_game_folders(const char* when)
{
  std::vector<std::string> dirs;
  const std::string list = orbis_gs_ini_string("NfsShares");
  if (list.find_first_not_of(" \t\r\n;") == std::string::npos)
  {
    OrbisNfs::SetShares(std::string());
    return dirs;
  }
  std::vector<std::string> problems;
  OrbisNfs::SetShares(list, &problems);
  for (const std::string& p : problems)
    printf("[boot] %s: NFS share left out (PS5SX2/NfsShares): %s\n", when, p.c_str());
  const auto t0 = std::chrono::steady_clock::now();
  for (const OrbisNfs::ShareInfo& s : OrbisNfs::MountAll(5000))
  {
    printf("[boot] %s: NFS share %s: %s%s\n", when, s.url.c_str(), s.state.c_str(),
      s.mounted ? (", its games listed from " + s.mount_point).c_str() : "");
    if (s.mounted)
      dirs.push_back(s.mount_point);
  }
  printf("[boot] %s: NFS shares looked at in %.2f s\n", when,
    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  fflush(stdout);
  return dirs;
}

static void orbis_scan_usb(const char* when)
{
  s_usb_dirs = orbis_usb_game_dirs(when);
  for (const std::string& dir : orbis_extra_game_folders(when)) // 2026-10-08
  {
    if (std::find(s_usb_dirs.begin(), s_usb_dirs.end(), dir) == s_usb_dirs.end())
      s_usb_dirs.push_back(dir);
  }
  // vk-285-135: the NFS shares, once the jailbreak gave the app the network's sockets and /data.
  if (std::strcmp(when, "after the jailbreak") == 0)
  {
    for (const std::string& dir : orbis_nfs_game_folders(when))
    {
      if (std::find(s_usb_dirs.begin(), s_usb_dirs.end(), dir) == s_usb_dirs.end())
        s_usb_dirs.push_back(dir);
    }
  }
}

// Test build 1: the logs report's first lines (the settings page's Download logs).
static std::string orbis_report_header()
{
  std::string h = "Build: " + orbis_build_label();
#if defined(ORBIS_DRIVER_REV) && defined(ORBIS_PCSX2_REV)
  h += " (sources: driver " ORBIS_DRIVER_REV ", pcsx2 " ORBIS_PCSX2_REV ")";
#endif
  h += "\nConsole: " + (s_console_info.empty() ? std::string("not surveyed yet") : s_console_info) + "\n";
  return h;
}

// The frontend's folders (vk-285-44: used twice, for the cover prefetch and for the shelf).
// vk-285-134 (AI-assisted): the BIOS before the shelf (build 130's logs: 961 starts on 235 consoles without one). When PCSX2
// finds none, a BIOS in a .zip/.rar/.7z in the BIOS folder, /data/PCSX2 or a drive is taken out into the BIOS folder; when
// there's still none, s_bios_problem says what was found instead, and the shelf shows it and won't start a game.
static std::string s_bios_problem;
static std::set<std::string> s_bios_archives_tried;
static bool orbis_is_bios(const std::string& path)
{
  u32 version = 0, region = 0;
  std::string description, zone;
  return IsBIOS(path.c_str(), version, description, region, zone);
}
static std::vector<std::string> orbis_bios_places()
{
  std::vector<std::string> dirs = {EmuFolders::Bios};
  if (EmuFolders::Bios != "/data/PCSX2")
    dirs.push_back("/data/PCSX2");
  for (int i = 0; i < 10; i++)
  {
    const std::string root = i < 8 ? "/mnt/usb" + std::to_string(i) : "/mnt/ext" + std::to_string(i - 8);
    struct stat st = {};
    if (stat(root.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
      dirs.push_back(root);
  }
  return dirs;
}
static bool orbis_check_bios()
{
  if (IsBIOSAvailable(std::string()))
  {
    s_bios_problem.clear();
    return true;
  }
  std::string from;
  const std::string taken = fe::ExtractBiosFromArchives(orbis_bios_places(), EmuFolders::Bios, orbis_is_bios,
    &s_bios_archives_tried, &from);
  if (!taken.empty())
  {
    printf("[bios] %s taken out of %s\n", taken.c_str(), from.c_str());
    fflush(stdout);
    if (IsBIOSAvailable(std::string()))
    {
      s_bios_problem.clear();
      return true;
    }
  }
  s_bios_problem = fe::DescribeBiosProblem(EmuFolders::Bios, "/data/PCSX2", orbis_bios_places(), orbis_is_bios);
  printf("[bios] no PS2 BIOS: %s\n", s_bios_problem.c_str());
  fflush(stdout);
  return false;
}

static OrbisFrontendPaths orbis_frontend_paths(bool allow_download)
{
  OrbisFrontendPaths fe;
  // vk-285-134: the BIOS, for the shelf's line and its check at each start.
  fe.bios_check = []() { return orbis_check_bios(); };
  fe.bios_dir = EmuFolders::Bios == "/data/PCSX2" ? "/data/PCSX2/bios" : EmuFolders::Bios;
  fe.bios_problem = [](){ return s_bios_problem; };
  // Test build 1 (vk-285-55): USB drives, the testing label and the logs download.
  fe.usb_dirs = s_usb_dirs;
  fe.usb_list = OrbisDir("cache") + "/usb-games.txt";
  fe.serial_cache = OrbisDir("cache") + "/chd-serials.txt"; // vk-285-108
  fe.gamedb_file = OrbisDir("resources") + "/GameIndex.yaml"; // vk-285-113: the games' names, for the shelf and the page
  fe.memcards_dir = OrbisDir("memcards"); // vk-285-113: the settings page's memory card list and creator
  fe.test_build = g_orbis_test_build;
  fe.build_label = orbis_build_label();
  fe.test_note = orbis_test_note(); // vk-285-105
  fe.logs_dir = OrbisDir("logs");
  fe.report_header = orbis_report_header();
  fe.games_dir = OrbisDir("games");
  fe.top_dir = "/data/PCSX2";
  fe.settings_dir = "/data/PCSX2/settings";
  fe.gs_ini = "/data/PCSX2/gs.ini";
  fe.patches_dir = OrbisDir("patches");
  fe.covers_dir = OrbisDir("covers");
  fe.cache_dir = OrbisDir("cache") + "/covers";
  fe.allow_download = allow_download;
  fe.sound = !orbis_flag("nomenusound"); // vk-285-47
  fe.settings_log = OrbisLogPath("settings.log"); // vk-285-51
  // 2026-10-05: HD texture packs from archive.org, in the folder PCSX2 reads them from (EmuFolders::Textures, set below
  // to /data/PCSX2/textures). Not OrbisDir("textures"): that answers /data/PCSX2 itself when the folder isn't there yet
  // (swordpdf's PS5 had none), and pr9h installed a pack as /data/PCSX2/<serial> where no game looks. The manager makes
  // the folder when it first needs it.
  fe.textures_dir = "/data/PCSX2/textures";
  fe.texture_pack_list = OrbisDir("cache") + "/texture-packs.json";
  fe.texture_packs = !orbis_flag("notexpacks");
  // 2026-10-08 (AI-assisted): a game's patches and cheats from GitHub (the sheet's "Get patches and cheats"), into the
  // folders PCSX2 reads them from (EmuFolders::Patches and ::Cheats below).
  fe.cheats_dir = OrbisDir("cheats");
  fe.online_patch_manifest = OrbisDir("cache") + "/online-patches.txt";
  fe.online_patches = !orbis_flag("noonlinepatches");
  return fe;
}

// 2026-10-08 (AI-assisted): an ELF's disc image (PS5SX2/ElfDisc in its settings file): a full path, or a file name the shelf
// lists, looked for in the folders it lists games from (games/, /data/PCSX2, the drives' and the extra folders) and one folder
// down in each, the first found winning as on the shelf. "" when unset or not found.
static std::string orbis_find_image(const std::string& want, const char* what);
static std::string orbis_elf_disc()
{
  if (s_game_ini_path.empty())
    return {};
  MemorySettingsInterface peek;
  orbis_apply_ini_file(peek, s_game_ini_path.c_str(), "game ini", true);
  std::string want;
  if (!peek.GetStringValue("PS5SX2", "ElfDisc", &want) || want.empty())
    return {};
  return orbis_find_image(want, "the ELF's disc");
}

// A disc image by the name the shelf's rows store (its file name: found in the game folders and one folder down), or a
// full path. "" when it isn't there (and the log says so).
static std::string orbis_find_image(const std::string& want, const char* what)
{
  struct stat st = {};
  const auto is_file = [&](const std::string& p) { return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); };
  if (want.find('/') != std::string::npos)
  {
    if (is_file(want))
      return want;
    printf("[boot] %s %s isn't there\n", what, want.c_str());
    return {};
  }
  std::vector<std::string> dirs = {OrbisDir("games"), "/data/PCSX2"};
  dirs.insert(dirs.end(), s_usb_dirs.begin(), s_usb_dirs.end());
  for (const std::string& dir : dirs)
  {
    if (is_file(dir + "/" + want))
      return dir + "/" + want;
    DIR* d = opendir(dir.c_str());
    if (!d)
      continue;
    std::string found;
    int subdirs = 0;
    while (const dirent* e = readdir(d))
    {
      if (e->d_name[0] == '.' || ++subdirs > 64)
        continue;
      const std::string p = dir + "/" + e->d_name + "/" + want;
      if (is_file(p))
      {
        found = p;
        break;
      }
    }
    closedir(d);
    if (!found.empty())
      return found;
  }
  printf("[boot] %s %s isn't in the game folders\n", what, want.c_str());
  return {};
}

// vk-285-139 (AI-assisted; swordpdf: "multi-disc and disc-change support (to even support gameshark iso)", the cheat disc "would
// have to be disc 1 at all times"). The discs the running game can change to: the cheat disc first when the game starts from
// it (PS5SX2/CheatDiscStart, its own file or gs.ini; PS5SX2/CheatDisc in gs.ini), then the game's own (fe::DiscSet: "(Disc N)"
// beside it, or an .m3u). The change disc combo (request 6) puts the next one in, the settings page any image (request 5).
// Needs proper testing on the console (a GameShark's disc swap, a two-disc game).
static std::mutex s_disc_mutex;
static std::vector<std::string> s_disc_set;
static std::string s_disc_current, s_disc_request;

std::string orbis_current_disc()
{
  std::lock_guard<std::mutex> lock(s_disc_mutex);
  return s_disc_current;
}

std::vector<std::string> orbis_disc_set()
{
  std::lock_guard<std::mutex> lock(s_disc_mutex);
  return s_disc_set;
}

bool orbis_request_disc(const std::string& path)
{
  {
    std::lock_guard<std::mutex> lock(s_disc_mutex);
    if (s_disc_current.empty())
      return false; // no game (or the PS2 menu, or an ELF with no disc)
    s_disc_request = path;
  }
  g_orbis_state_request.store(5, std::memory_order_release);
  return true;
}

// The CPU thread (StubHost.cpp's PumpMessagesOnCPUThread), between frames.
void OrbisChangeDiscCpu(int req)
{
  std::string to;
  int number = 0, count = 0;
  {
    std::lock_guard<std::mutex> lock(s_disc_mutex);
    count = static_cast<int>(s_disc_set.size());
    if (req == 6)
    {
      if (count < 2)
      {
        OrbisOSDLabel("ONE DISC");
        printf("[disc] change disc: this game has one disc\n");
        fflush(stdout);
        return;
      }
      const auto at = std::find(s_disc_set.begin(), s_disc_set.end(), s_disc_current);
      const int i = at == s_disc_set.end() ? -1 : static_cast<int>(at - s_disc_set.begin());
      to = s_disc_set[static_cast<size_t>((i + 1) % count)];
    }
    else
    {
      to.swap(s_disc_request);
    }
    const auto in = std::find(s_disc_set.begin(), s_disc_set.end(), to);
    number = in == s_disc_set.end() ? 0 : static_cast<int>(in - s_disc_set.begin()) + 1;
  }
  if (to.empty())
    return;
  const bool ok = VMManager::ChangeDisc(CDVD_SourceType::Iso, to);
  if (ok)
  {
    std::lock_guard<std::mutex> lock(s_disc_mutex);
    s_disc_current = to;
  }
  char label[48];
  if (!ok)
    std::snprintf(label, sizeof(label), "DISC CHANGE FAILED");
  else if (number > 0 && count > 1)
    std::snprintf(label, sizeof(label), "DISC %d OF %d", number, count);
  else
    std::snprintf(label, sizeof(label), "DISC CHANGED");
  OrbisOSDLabel(label);
  printf("[disc] %s: %s (%s)\n", req == 6 ? "change disc combo" : "settings page", to.c_str(), ok ? label : "failed");
  orbis_eventf("change disc: %s%s", to.substr(to.rfind('/') + 1).c_str(), ok ? "" : " (failed)");
  fflush(stdout);
}

// At the game's start: the set, and the disc it starts from (the cheat disc, when it's on and found).
static std::string orbis_disc_boot(const std::string& game)
{
  std::vector<std::string> set = fe::DiscSet(game);
  std::string start = game;
  MemorySettingsInterface peek;
  orbis_apply_ini_file(peek, "/data/PCSX2/gs.ini", "gs.ini", true);
  if (!s_game_ini_path.empty())
    orbis_apply_ini_file(peek, s_game_ini_path.c_str(), "game ini", true);
  std::string on, want;
  if (peek.GetStringValue("PS5SX2", "CheatDiscStart", &on) && (on == "true" || on == "1"))
  {
    MemorySettingsInterface global;
    orbis_apply_ini_file(global, "/data/PCSX2/gs.ini", "gs.ini", true);
    if (!global.GetStringValue("PS5SX2", "CheatDisc", &want) || want.empty())
      printf("[disc] Start from the cheat disc is on, but no cheat disc is set (Cheat disc, the sheet for all games)\n");
    else
    {
      const std::string cheat = orbis_find_image(want, "the cheat disc");
      if (!cheat.empty() && cheat != game)
      {
        set.erase(std::remove(set.begin(), set.end(), cheat), set.end());
        set.insert(set.begin(), cheat);
        start = cheat;
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(s_disc_mutex);
    s_disc_set = set;
    s_disc_current = start;
  }
  std::string list;
  for (size_t i = 0; i < set.size(); i++)
    list += (i ? " | " : "") + set[i].substr(set[i].rfind('/') + 1);
  printf("[disc] %zu disc(s): %s; starts from %s\n", set.size(), list.c_str(), start.substr(start.rfind('/') + 1).c_str());
  fflush(stdout);
  return start;
}

// vk-285-45: the flags folder before and after the jailbreak. Before it, the sandbox answers
// access() with EPERM for a file that exists (stat() works), so vk-285-43/44 read every flag there
// as absent ("flag vk_renderer=0"), set the driver's environment from that, and PCSX2 ran without
// PS5VK_FULL_STATE, the WAR barrier, the live flags and the 8192 extent -- the GPU hang in Return
// of the King about 5 s in, and 3x instead of 6x. vk-285-46 reads flags with stat(); this still
// logs what the app can see.
static void orbis_log_flag_access(const char* when)
{
  struct stat st{};
  const int rc = stat("/data/PCSX2/flags", &st);
  const int stat_errno = rc ? errno : 0;
  const int acc = access("/data/PCSX2/flags", R_OK | X_OK);
  const int acc_errno = acc ? errno : 0;
  printf("[boot] flags folder %s: stat rc=%d errno=%d mode=%o uid=%u, access rc=%d errno=%d\n", when, rc,
         stat_errno, rc ? 0u : (unsigned)(st.st_mode & 07777), rc ? 0u : (unsigned)st.st_uid, acc, acc_errno);
  fflush(stdout);
}

// vk-285-115 (AI-assisted): the release's switch files when the flags folder is missing or empty. The installer writes
// them on a first install only, so a console set up by hand (the eboot and the folders copied over) ran with none: the
// recompilers' code in JIT shared memory (105 MiB of the ~190 MiB flexible budget), the 8192 texture limit, two
// swapchain images, no wide memory and the driver's conservative live flags. 1.50's logs: "Failed to initialize GS."
// in 285 sessions on 23 such consoles (the GS device ran out of memory at its last pipelines), and Need for Speed:
// Most Wanted's recompiler crash. Not when flags/ holds any file (a tester's own set, even a single file: that is
// also how to keep a folder of no switches) or when a release switch sits in the top folder (the layout before
// vk-285-33, which OrbisFlag still reads). games/ is created too, for the disc images; bios/ is not, since OrbisDir
// falls back to the top folder only while there is no bios/ (older setups keep their BIOS there).
static const char* const kOrbisReleaseFlags[] = {"fastmem", "jitdirect", "sw_renderer", "vk_16k", "vk_async",
                                                 "vk_deferflush", "vk_depthwait", "vk_fastpoll", "vk_gpuwait",
                                                 "vk_latewait", "vk_nocrumbs", "vk_noevict", "vk_renderer",
                                                 "vk_triple", "vk_widemem"};

static void orbis_ensure_data_layout()
{
  static const char kRoot[] = "/data/PCSX2";
  struct stat st{};
  if (stat(kRoot, &st) != 0 || !S_ISDIR(st.st_mode))
  {
    printf("[boot] data layout: no %s folder (errno %d)\n", kRoot, errno);
    fflush(stdout);
    return;
  }
  const std::string games = std::string(kRoot) + "/games";
  if (stat(games.c_str(), &st) != 0)
  {
    const int rc = mkdir(games.c_str(), 0777);
    printf("[boot] data layout: created %s (rc=%d errno=%d)\n", games.c_str(), rc, rc ? errno : 0);
    if (rc == 0)
      orbis_eventf("created /data/PCSX2/games/ for the disc images");
  }
  // vk-285-121 (AI-assisted): settings/ as well. The shelf's sheet and the settings page write a game's own settings
  // there and nothing made the folder: two testers' 1.7 logs had every save fail with errno 2 ("[web] writing
  // /data/PCSX2/settings/... failed (errno 2)"). The writers make it too now (fe_settings.cpp, WriteFileAtomic).
  const std::string settings = std::string(kRoot) + "/settings";
  if (stat(settings.c_str(), &st) != 0)
  {
    const int rc = mkdir(settings.c_str(), 0777);
    printf("[boot] data layout: created %s (rc=%d errno=%d)\n", settings.c_str(), rc, rc ? errno : 0);
    if (rc == 0)
      orbis_eventf("created /data/PCSX2/settings/ for the games' own settings");
  }
  const std::string flags = std::string(kRoot) + "/flags";
  const bool have_dir = stat(flags.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  int entries = 0;
  if (have_dir)
  {
    DIR* d = opendir(flags.c_str());
    if (!d)
    {
      printf("[boot] data layout: can't list %s (errno %d); switch files left as they are\n", flags.c_str(), errno);
      fflush(stdout);
      return;
    }
    while (const dirent* e = readdir(d))
      entries += e->d_name[0] != '.' ? 1 : 0;
    closedir(d);
  }
  if (entries > 0)
  {
    fflush(stdout);
    return;
  }
  for (const char* name : kOrbisReleaseFlags)
  {
    if (stat((std::string(kRoot) + "/" + name).c_str(), &st) == 0)
    {
      printf("[boot] data layout: flags/ is %s but %s/%s is there (the older layout): switch files left as they are\n",
             have_dir ? "empty" : "missing", kRoot, name);
      fflush(stdout);
      return;
    }
  }
  if (!have_dir && mkdir(flags.c_str(), 0777) != 0)
  {
    printf("[boot] data layout: can't create %s (errno %d)\n", flags.c_str(), errno);
    fflush(stdout);
    return;
  }
  int made = 0;
  for (const char* name : kOrbisReleaseFlags)
  {
    const int fd = open((flags + "/" + name).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
      close(fd);
      made++;
    }
  }
  const int total = static_cast<int>(sizeof(kOrbisReleaseFlags) / sizeof(kOrbisReleaseFlags[0]));
  printf("[boot] data layout: flags/ was %s: wrote the release's switch files (%d of %d)\n", have_dir ? "empty" : "missing",
         made, total);
  fflush(stdout);
  orbis_eventf("flags/ was %s: PS5SX2 wrote the release's %d switch files (as the installer does)",
               have_dir ? "empty" : "missing", made);
}

// vk-285-115 (AI-assisted): PCSX2's Vulkan shader and pipeline caches renamed to *.bad after a GS device that didn't
// start, so the next start makes new ones (VKShaderCache keeps the first index entry of a shader, so one bad blob came
// back at every start). Renamed, not deleted: the last set stays for a report.
static void orbis_set_aside_shader_caches()
{
  static const char* const kNames[] = {"vulkan_shaders.idx", "vulkan_shaders.bin", "vulkan_pipelines.bin"};
  int moved = 0;
  for (const char* name : kNames)
  {
    const std::string path = EmuFolders::Cache + "/" + name;
    struct stat st{};
    if (stat(path.c_str(), &st) != 0)
      continue;
    const int rc = rename(path.c_str(), (path + ".bad").c_str());
    printf("[boot] GS didn't start: %s set aside as %s.bad (rc=%d errno=%d)\n", path.c_str(), name, rc, rc ? errno : 0);
    moved += rc == 0 ? 1 : 0;
  }
  fflush(stdout);
  if (moved > 0)
    orbis_eventf("the GS didn't start: PCSX2's shader caches set aside (*.bad); the next start makes new ones");
}

// The Vulkan driver's environment: only setenv()s of flags, after the jailbreak, where they can
// be read (vk-285-43 made this a function).
//
// vk-285-113 (AI-assisted): the HW renderer's variables follow the renderer that was CHOSEN (!g_sw_renderer), not
// the flag file vk_renderer. A console with no flags/ folder (a manual copy of the eboot: the installer writes the
// flags only on a first install) picks the HW renderer -- sw_renderer is absent -- but got none of these, i.e. the
// driver ran without PS5VK_FULL_STATE, the state-leak fix of vk-285-16. vk-285-112's Hitman: Contracts (5x) and GTA
// Vice City (2x) hung that way ("completion marker not written within 2 s", the vk-285-13..15 signature) on the one
// console with no flags. The opt-in flags (vk_widemem, vk_triple, vk_recordthread, vk_fullstatecopy, vk_16k and the
// vk_no* switches) still need their files.
// vk-285-133 (AI-assisted): the title's param.json for ps5vk's 120 Hz mode (frame generation). The driver offers the 119.88 Hz
// mode only when the title's param.json declares high frame rates (attribute3 0x80040), and it read /app0/sce_sys/param.json,
// which after the jailbreak doesn't lead to our folder: vk-285-131 on the console never offered the mode, and the TV stayed at
// 60 Hz. PS5VK_PARAM_JSON names the first of these that opens instead: the running app's own folder as the sandbox mounts it
// (what the system sees), /app0, then the install folder. What it declares is logged here, since the driver's own lines go to
// stderr.log.
// vk-285-134 (AI-assisted): build 130's logs had ~27 starts that failed with "Requested filename '...' does not exist" for a
// file the shelf had just listed (and the same game started before and after on the same console, from internal storage and
// USB drives alike). Before the VM looks, the file is stat'ed here: when it isn't seen, up to 3 s of retries, and boot.log
// gets errno, whether its folder opens and whether a file of that name is in it.
static void orbis_wait_for_game_file(const std::string& path)
{
  if (path.empty() || path[0] != '/')
    return;
  struct stat st = {};
  if (stat(path.c_str(), &st) == 0)
    return;
  const int first = errno;
  const size_t slash = path.find_last_of('/');
  const std::string dir = slash == 0 ? "/" : path.substr(0, slash), name = path.substr(slash + 1);
  bool listed = false;
  int dir_errno = 0;
  if (DIR* d = opendir(dir.c_str()))
  {
    while (const dirent* e = readdir(d))
      listed |= name == e->d_name;
    closedir(d);
  }
  else
    dir_errno = errno;
  printf("[boot] game file not seen: stat errno %d; its folder %s %s%s; retrying for up to 3 s\n", first, dir.c_str(),
    dir_errno ? "doesn't open, errno " : "opens", dir_errno ? std::to_string(dir_errno).c_str() : (listed ? ", the name is in it" : ", the name isn't in it"));
  fflush(stdout);
  for (int i = 1; i <= 15; i++)
  {
    usleep(200000);
    if (stat(path.c_str(), &st) == 0)
    {
      printf("[boot] game file seen after %d ms\n", i * 200);
      fflush(stdout);
      return;
    }
  }
  printf("[boot] game file still not seen (errno %d)\n", errno);
  fflush(stdout);
}

static void orbis_vk_param_json()
{
  static const char* const paths[] = {"/mnt/sandbox/PPSA99203_000/app0/sce_sys/param.json", "/app0/sce_sys/param.json",
                                      "/data/homebrew/PPSA99203/sce_sys/param.json"};
  std::string tried;
  for (const char* path : paths)
  {
    FILE* const file = fopen(path, "rb");
    if (!file)
    {
      const int error = errno;
      tried += std::string(tried.empty() ? "" : ", ") + path + " (errno " + std::to_string(error) + ")";
      continue;
    }
    static char text[16384];
    const size_t length = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[length] = '\0';
    const char* at = strstr(text, "\"attribute3\"");
    if (at)
      at = strchr(at, ':');
    const unsigned long attribute3 = at ? strtoul(at + 1, nullptr, 0) : 0;
    const bool declared = at && (attribute3 & 0x80040ul) == 0x80040ul;
    setenv("PS5VK_PARAM_JSON", path, 0);
    printf("[boot] param.json for the 120 Hz mode: %s, attribute3 %s0x%lx: %s\n", path, at ? "" : "absent, ", attribute3,
      declared ? "high frame rates declared, so the driver offers 119.88 Hz (frame generation asks for it)"
               : "high frame rates (0x80040) not declared, so the display stays at 60 Hz (the param.json that comes "
                 "with this build declares them)");
    fflush(stdout);
    return;
  }
  printf("[boot] param.json for the 120 Hz mode: none opened (%s); the display stays at 60 Hz\n", tried.c_str());
  fflush(stdout);
}

static void orbis_vk_environment()
{
  const bool hw = !g_sw_renderer;
  // vk-285-115 (AI-assisted): the RADV eboot (link-radv.sh). RADV reads none of the PS5VK_* variables below. Its own: the shader
  // cache, which RADV puts in /app0/radv-shader-cache unless told otherwise (next to PCSX2's caches instead, as ps5vk's), and its
  // threaded recording for the vk_recordthread flag (RADV_THREADED_RECORDING; a RADV build older than the threaded layer ignores it).
  if (OrbisDriverIsRADV())
  {
    setenv("MESA_SHADER_CACHE_DIR", (OrbisDir("cache") + "/radv-shader-cache").c_str(), 0);
    if (hw && orbis_flag("vk_recordthread")) setenv("RADV_THREADED_RECORDING", "1", 0);
    const char* const cache = getenv("MESA_SHADER_CACHE_DIR");
    const char* const threaded = getenv("RADV_THREADED_RECORDING");
    printf("[boot] RADV environment (hardware renderer %s): MESA_SHADER_CACHE_DIR=%s RADV_THREADED_RECORDING=%s\n", hw ? "yes" : "no",
      cache ? cache : "-", threaded ? threaded : "-");
    fflush(stdout);
    return;
  }
  // The driver keeps its compiled shaders next to PCSX2's caches, not in /app0.
  setenv("PS5VK_SHADER_CACHE_DIR", (OrbisDir("cache") + "/ps5vk-shader-cache").c_str(), 0); // vk-285-33: cache/
  orbis_vk_param_json(); // vk-285-133: before the frontend's swapchain, the driver's first look at the modes
  // The driver's queue profile (a stderr.log line every 10 s) unless novkprof.
  if (!orbis_flag("novkprof")) setenv("PS5VK_PROFILE", "1", 0);
  // vk-285-14: on a GPU hang the driver writes the hung step's words and register tables here
  // (ps5vk-hang.bin/.txt) before the device is marked lost.
  setenv("PS5VK_HANG_DUMP", OrbisLogPath("ps5vk-hang").c_str(), 0); // vk-285-33: logs/
  // vk-285-15: GPU breadcrumbs (the driver writes each draw's serial after it completes) while
  // the HW renderer is being brought up, so a hang dump says which draw the GPU stopped at.
  if (hw) setenv("PS5VK_BREADCRUMBS", "1", 0);
  // vk-285-16: full per-draw state in the driver for the HW renderer: every draw programs its
  // blend word (no more inheriting the last draw's dual-source blend), a colour-only rendering
  // unbinds the depth surface, and a depth rendering ends its submission step so the next pass
  // starts on a drained GPU -- the fix for the vk-285-14/15 hang at the first gameplay frame.
  // The flag file vk_nofullstate turns it off (A/B).
  if (hw && !orbis_flag("vk_nofullstate")) setenv("PS5VK_FULL_STATE", "1", 0);
  // vk-285-17: the driver evicts the render targets' CPU cache lines only at a submission's ends
  // and around CPU work, not around each of the ~230 GPU syncs per frame (6.4 GiB, 97 ms a frame
  // in vk-285-16). The flag file vk_eagerflush restores the per-step eviction (A/B).
  if (hw && !orbis_flag("vk_eagerflush")) setenv("PS5VK_LAZY_TARGET_FLUSH", "1", 0);
  // vk-285-22: live flag files, read by the driver about once a second while the game runs:
  // vk_gpuwait turns on the in-stream colour barrier (a GPU-side wait for the colour flush in
  // place of the ~220 CPU round trips a frame PCSX2's full-barrier draws caused, vk-285-21), and
  // vk_nocrumbs turns the per-draw breadcrumbs off. A barrier wait that never passes is released
  // by the driver after ~200 ms, which also turns the barrier off for the rest of the run.
  if (hw) setenv("PS5VK_LIVE_DIR", OrbisDir("flags").c_str(), 0); // vk-285-33: flags/
  // vk-285-23: the driver reports 8192 for its image, framebuffer and viewport extent (4096 by
  // default): PCSX2 caps upscale_multiplier at maxImageDimension2D / 1280, so 4096 stopped it at
  // 3x and 6x ("4K") needs 7680.
  // vk-285-66: 16384 with the flag file vk_16k, so PCSX2 offers 8x (16384 / 1280 = 12.8; at 8192 it
  // stops at 6x). The descriptor and target size fields hold 14 bits (the driver's ps5vk_max_extent_2d).
  // vk-285-134d (swordpdf: "also have a selectable option for 8x native"): 16384 by default, so the Resolution row's 8x
  // applies (vk-285-66 ran R&C at 8x and 60 fps with it, and the 12 GiB heap it wants is the default since); the flag
  // file vk_8k keeps 8192 (PCSX2 then stops at 6x).
  if (hw) setenv("PS5VK_MAX_EXTENT_2D", orbis_flag("vk_8k") ? "8192" : "16384", 0);
  // vk-285-66: VkDeviceMemory outside the 4 GiB window and a 12 GiB heap (Mihawk's R86-R88) with the
  // flag file vk_widemem.
  if (hw && orbis_flag("vk_widemem")) setenv("PS5VK_WIDE_MEMORY", "1", 0);
  // vk-285-24: two more live flag files the driver reads from PS5VK_LIVE_DIR: vk_noevict stops the
  // per-frame CPU-cache eviction of every render target (0.65 ms a frame at 1x, 9.5 ms at 6x), and
  // vk_async runs the queue on a worker thread, so the GS thread no longer waits for the GPU, and
  // vk_deferflush evicts the draws' register/descriptor tables once per submission (on that worker)
  // instead of 5-6 clflush+mfence runs per draw on the GS thread.
  // vk-285-25: the driver's write-after-read drain. A draw that renders into an image an earlier
  // draw sampled (since the GPU last drained) waits for those samples first: R&C's frame starts by
  // copying the back buffer out and then clearing it, and at 3x+ the clear overtook the copy's last
  // strip, so 32x32 blocks of the displayed frame showed the clear (sky) colour. The live flag file
  // vk_nowar switches it off again for comparison.
  if (hw && !orbis_flag("vk_nowarenv")) setenv("PS5VK_WAR_BARRIER", "1", 0);
  // vk-285-62: three swapchain images with the flag file vk_triple (read when the swapchain is
  // created, so it applies from the next start). The original PS5 at firmware 4.03 started about a
  // third of R&C's frames ~15.7 ms late (vk-285-61's GPU trace: ~4.3 ms of GPU work, then the wait
  // for the display); with two images every late start cost a vblank, with three it has slack.
  if (hw && orbis_flag("vk_triple")) setenv("PS5VK_SWAPCHAIN_IMAGES", "3", 0);
  // vk-285-72: texture uploads as GPU copies in the frame (CP DMA) instead of CPU copies at a
  // submission split. PCSX2 records an update of a texture the frame already drew with into the
  // frame's own command buffer; each such split was a step, and on a tester's original PS5 at 8.40
  // every step waited about a vblank (Gran Turismo 4: 2.4-3 steps a frame, 25-29 ms). The live flag
  // file vk_cpuupload puts the uploads back on the CPU while a game runs.
  if (hw) setenv("PS5VK_GPU_UPLOAD", "1", 0);
  // vk-285-86: the flag file vk_recordthread moves the driver's command encoding off the GS thread:
  // PCSX2's vkCmd* calls copy their arguments and return, and the driver's own thread encodes them
  // (~22% of the GS thread in Shadow of the Colossus's heavy views, vk-285-84). Read when the
  // device is created, so it applies from the next start; the live flag file vk_recordsync keeps
  // new recordings on the GS thread for an A/B within a run. Needs proper testing.
  if (hw && orbis_flag("vk_recordthread")) setenv("PS5VK_RECORD_THREAD", "1", 0);
  // vk-285-86: the flag file vk_fullstatecopy binds pipelines with Mesa's whole dynamic-state copy
  // instead of the driver's per-group copy (A/B for the per-draw state cost).
  if (hw && orbis_flag("vk_fullstatecopy")) setenv("PS5VK_FULL_STATE_COPY", "1", 0);
  // vk-285-113: which of them the driver will see, so a report shows whether a console ran with the environment.
  {
    static const char* const names[] = {"PS5VK_FULL_STATE", "PS5VK_WAR_BARRIER", "PS5VK_LAZY_TARGET_FLUSH", "PS5VK_LIVE_DIR",
                                        "PS5VK_MAX_EXTENT_2D", "PS5VK_GPU_UPLOAD", "PS5VK_BREADCRUMBS", "PS5VK_WIDE_MEMORY",
                                        "PS5VK_SWAPCHAIN_IMAGES", "PS5VK_HANG_DUMP", "PS5VK_PARAM_JSON"};
    std::string line;
    for (const char* name : names)
    {
      const char* value = getenv(name);
      line += std::string(" ") + name + "=" + (value ? value : "-");
    }
    printf("[boot] ps5vk environment (hardware renderer %s):%s\n", hw ? "yes" : "no", line.c_str());
    fflush(stdout);
  }
}
#endif


// argc and argv: a home screen forwarder's --rom and --exit-after-game (orbis-shims/OrbisForward.h); the CRT
// (proto/native/tooling/native/app_crt.cpp) passes the launch arguments. The re-executions of this eboot (back to the
// menu, a failed start, a new build) pass none, so they open the shelf.
int main(int argc, char** argv)
{
  const orbis_forward::Args forward = orbis_forward::Parse(argc, argv);
  // Bigapp: no elfldr socket. Log to file (read back over FTP) + notify.
  // vk-285-33: in logs/ when that folder exists (OrbisPaths.h).
  // vk-285-113: a broken pipe (a network peer that went away) is an error return, not the end of the app.
  signal(SIGPIPE, SIG_IGN);
  // stdout and stderr go to boot.log; when /data isn't visible yet (a console whose Helper's mount hook didn't take),
  // into memory until it is (orbis_boot_log_release, after the jailbreak).
  if (!orbis_boot_log_open(true))
    orbis_boot_log_hold();
  // Orbis: fresh fault history per run (pf.log otherwise appends forever).
  {
    snprintf(g_orbis_pf_log, sizeof(g_orbis_pf_log), "%s", OrbisLogPath("pf.log").c_str()); // vk-285-33
    FILE* pflog = fopen(g_orbis_pf_log, "w");
    if (pflog)
      fclose(pflog);
  }
  // Orbis: was _IONBF (every printf = storage write, idled the CPU), then
  // _IOFBF 1MB (fast but hid the log tail on silent death), then _IOLBF
  // (bench: 0.72ms PER LINE - block compiles cost 1.44ms in prints alone,
  // tens of percent of wall time). Back to _IOFBF 1MB: the 1s ticker thread
  // below printf+fflushes every second (periodic flush), and the crash
  // handler flushes on death, so worst case we lose ~1s of hot-path lines.
  setvbuf(stdout, nullptr, _IOFBF, 1 << 20);
  fprintf(stderr, "[boot] stderr-ok\n");
  printf("[boot] main=%p\n", (void*)&main);
  printf("[boot] build=" ORBIS_BUILD_TAG "\n");
  {
    // vk-285-91: what VZEROUPPER costs on this CPU. The GS thread's profiles (vk-285-89/90) had 5-7% of
    // their samples on the instruction right after one, so the eboot is now built with -mno-vzeroupper
    // (no SSE/AVX transition penalty to avoid on Zen 2, which keeps the upper halves apart). Two short
    // loops of a 256-bit add, one with a VZEROUPPER after each add; the best of three, in TSC ticks.
    const auto loop = [](bool vzu) {
      unsigned long long best = ~0ull;
      for (int rep = 0; rep < 3; rep++)
      {
        const unsigned long long t0 = __builtin_ia32_rdtsc();
        if (vzu)
          for (int i = 0; i < 1000000; i++)
            asm volatile("vpaddd %%ymm1, %%ymm1, %%ymm1\n\tvzeroupper" ::: "xmm0", "xmm1", "xmm2", "xmm3", "xmm4",
              "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory");
        else
          for (int i = 0; i < 1000000; i++)
            asm volatile("vpaddd %%ymm1, %%ymm1, %%ymm1" ::: "xmm1", "memory");
        const unsigned long long dt = __builtin_ia32_rdtsc() - t0;
        if (dt < best)
          best = dt;
      }
      return static_cast<double>(best) / 1e6;
    };
    const double with = loop(true), without = loop(false);
    printf("[vzubench] ticks per iteration: 256-bit add + vzeroupper %.2f, 256-bit add alone %.2f (TSC)\n", with, without);
  }
  if (g_orbis_test_build > 0)
    printf("[boot] testing build: %s\n", orbis_build_label().c_str()); // test build 1 (vk-285-55)
  if (!forward.rom.empty())
    printf("[boot] forwarder: --rom %s%s\n", forward.rom.c_str(), forward.exit_after_game ? " --exit-after-game" : "");
#ifdef ORBIS_VULKAN
  orbis_event_log_init(OrbisLogPath("settings.log")); // vk-285-51
#endif
  orbis_eventf("app start: %s (pid %d)", orbis_build_label().c_str(), (int)getpid());
  {
    // vk-285-51: the crash printer (orbis-shims/ProsperoCrash.cpp) from the start, so the shelf and
    // the settings page's thread are covered too; it was installed only just before PCSX2 started.
    orbis_install_signal_handlers(); // vk-285-113: more signals, and the ones that end the app
  }
#if defined(ORBIS_DRIVER_REV) && defined(ORBIS_PCSX2_REV)
  // vk-285-35: the exact sources (link-vk.sh; a trailing + marks uncommitted changes).
  printf("[boot] sources: driver %s, pcsx2 %s\n", ORBIS_DRIVER_REV, ORBIS_PCSX2_REV);
#endif
  printf("[boot] Vulkan driver: %s\n", OrbisDriverName()); // vk-285-115: link-vk.sh links ps5vk, link-radv.sh RADV
  if (&orbis_ps5vk_war_shim != nullptr) // vk-285-119: link-vk.sh linked orbis-shims/orbis_ps5vk_war.c
    printf("[boot] ps5vk write-after-read list: AVX2 lookup (orbis_ps5vk_war.c)\n");
  fflush(stdout);
  printf("[boot] main tid=%llu\n", (unsigned long long)pthread_self());
  {
    void* sp = nullptr;
#if defined(__x86_64__)
    asm volatile("mov %%rsp, %0" : "=r"(sp));
#endif
    printf("[boot] main rsp=%p\n", sp);
    ps5::debug::set_line(6, "main rsp=%p", sp);
  }
  fflush(stdout);
  {
    FILE* f = fopen("/data/PCSX2/pid.txt", "w");
    if (f) { fprintf(f, "%d", (int)getpid()); fclose(f); }
    printf("[boot] pid=%d\n", (int)getpid());
    fflush(stdout);
  }

  // Claim flexible memory before PCSX2 allocates its direct/code regions so
  // pthread stacks remain outside the emulation maps.
  void* guard = mmap(nullptr, 0x8000000, PROT_READ | PROT_WRITE, 0 /*ANON*/, -1, 0);
  printf("[boot] stack guard=%p (errno=%d)\n", guard, (guard == (void*)-1) ? errno : 0);
#ifdef ORBIS_VULKAN
  {
    extern void orbis_vk_window_survey(const char* when);
    orbis_vk_window_survey("at start");
  }
#endif
  fflush(stdout);

  // On-screen debug overlay (VideoOut canvas, separate thread).
  ps5::debug::set_line(1, "main started");
  // vk-285-110: the shelf's and the notifications' text in the PS5's language (fe_i18n.cpp), with testers'
  // fixes from lang/<code>.txt when /data is readable here (again after the jailbreak).
  orbis_frontend_set_language(OrbisDir("lang"));
  sys_notify(fe::Tr(g_orbis_test_build > 0 ? fe::Str::NotifyStartingTest : fe::Str::NotifyStarting)); // vk-285-50: the new name
  // vk-285-44: only the cover downloads run before the HEN jailbreak. HTTPS from the frontend
  // failed after it (vk-285-41/42: the handshake to raw.githubusercontent.com hung, or ended in
  // 0x8095F00C, "unknown CA") and worked before it (vk-285-43, and the user's Twiso, which fetches
  // covers before it asks for the jailbreak). vk-285-43 ran the whole frontend there, and the
  // first game after it hung the GPU (cause not established), so the frontend and its Vulkan
  // device are back after the jailbreak, where they ran in vk-285-40..42; they read the covers
  // from the cache this fills. At most 30 s, and only when covers are missing.
  static std::string s_game_path;
  bool frontend_ran = false;
#ifdef ORBIS_VULKAN
  orbis_log_flag_access("before the jailbreak");
  orbis_scan_usb("before the jailbreak"); // test build 1: whether the sandbox shows USB drives yet
  // A forwarder's game starts without the shelf, so no covers to wait for (the shelf fetches them when it opens).
  if (forward.rom.empty() && !orbis_flag("nofrontend") && !orbis_flag("nomenu") && !orbis_flag("nocoverdl"))
    orbis_frontend_prefetch_covers(orbis_frontend_paths(true), 30.0, sys_notify);
#endif

  // Primary: etaHEN/OnionHEN download0 file-broker jailbreak (needs PPSA99203
  // in the HEN app_jailbreak allowlist). Falls back to the legacy CMD ports.
  extern bool orbis_hen_jailbreak();
  extern bool orbis_probe_jit();
  extern void orbis_log_hen_config();
  orbis_log_hen_config();
  if (orbis_hen_jailbreak())
    g_jailbreak_ok = 1;
  else
    orbis_try_jailbreak();
#ifdef ORBIS_VULKAN
  orbis_log_flag_access("after the jailbreak");
#endif
  orbis_boot_log_release("after the jailbreak"); // vk-285-113: boot.log's first lines, when /data wasn't there yet
#ifdef ORBIS_VULKAN
  orbis_ensure_data_layout(); // vk-285-115: the release's switch files on a console set up by hand
#endif
  orbis_frontend_set_language(OrbisDir("lang")); // vk-285-110: lang/<code>.txt may only be readable now
  // Test build 1 (vk-285-55): what the console is, in boot.log and the settings log.
  s_console_info = orbis_console_survey();
  orbis_eventf("console: %s", s_console_info.c_str());
  // geteuid() may keep reporting 1 even with working creds; the JIT page
  // probe is the ground truth for privilege on this firmware.
  const bool jit_probe_ok = orbis_probe_jit();
  if (jit_probe_ok)
    g_jailbreak_ok = 1;
  // vk-285-62: the memory probe (orbis-shims/orbis_memprobe.cpp): what direct memory can hold
  // (executable code, aliased guest pages, fixed mappings), with the flag file memprobe.
  if (orbis_flag("memprobe"))
  {
    extern void orbis_memprobe();
    orbis_memprobe();
  }
  // vk-285-64: the recompilers' code in direct memory (pcsx2/Memory.cpp, g_orbis_code_direct) with
  // the flag file jitdirect; without it, JIT shared memory out of the flexible budget, as before.
  //
  // vk-285-115 (AI-assisted): direct memory without the flag too. 1.50's sessions ran it on 5,892 of 6,325 (the
  // release's flags have jitdirect); the 433 in JIT shared memory had 105 MiB less of the ~190 MiB flexible budget
  // and were most of the "Failed to initialize GS." starts. JIT shared memory now only with the flag file jitshared,
  // and only when the JIT probe worked (where it fails, that memory can't hold code at all).
  {
    const bool want_shared = orbis_flag("jitshared");
    g_orbis_code_direct = (want_shared && jit_probe_ok) ? 0 : 1;
    const char* why = !g_orbis_code_direct ? "JIT shared memory (jitshared)"
                    : want_shared          ? "direct memory (jitshared asked, but the JIT probe failed)"
                    : orbis_flag("jitdirect") ? "direct memory (jitdirect)"
                                              : "direct memory (the default)";
    printf("[boot] recompiler code in %s\n", why);
    fflush(stdout);
  }
  // Orbis dev-loop auto-restart: watch our own eboot; when a new build is
  // uploaded (size/mtime change, stable across two polls so partial FTP
  // writes don't trigger), re-exec into it via sceSystemServiceLoadExec.
  // The new image re-runs this whole startup (HEN jailbreak included).
  {
    std::thread([]() {
      const char* path = "/data/homebrew/PPSA99203/eboot.bin";
      struct stat st0{};
      int rc0 = stat(path, &st0);
      printf("[boot] eboot watcher: path=%s rc=%d errno=%d size=%lld mtime=%lld\n", path, rc0,
             rc0 ? errno : 0, (long long)st0.st_size, (long long)st0.st_mtime);
      fflush(stdout);
      if (rc0 != 0)
      {
        path = "/app0/eboot.bin";
        rc0 = stat(path, &st0);
        printf("[boot] eboot watcher: fallback path=%s rc=%d errno=%d size=%lld\n", path, rc0,
               rc0 ? errno : 0, (long long)st0.st_size);
        fflush(stdout);
        if (rc0 != 0)
          return;
      }
      for (;;)
      {
        sleep(2);
        struct stat st1{};
        if (stat(path, &st1) != 0)
          continue;
        if (st1.st_mtime == st0.st_mtime && st1.st_size == st0.st_size)
          continue;
        printf("[boot] eboot watcher: change size %lld->%lld mtime %lld->%lld\n", (long long)st0.st_size,
               (long long)st1.st_size, (long long)st0.st_mtime, (long long)st1.st_mtime);
        fflush(stdout);
        sleep(2); // stability: ignore in-flight uploads
        struct stat st2{};
        if (stat(path, &st2) != 0)
          continue;
        if (st2.st_mtime != st1.st_mtime || st2.st_size != st1.st_size || st2.st_size == 0)
          continue; // still being written: keep the original baseline and re-check
        printf("[boot] new eboot detected (%lld bytes), restarting into it\n", (long long)st2.st_size);
        fflush(stdout);
        sys_notify("PS5SX2: new build, restarting");
        ps5::debug::set_line(2, "NEW BUILD - RESTARTING");
        sleep(1);
        orbis_output_default();
        int rc = sceSystemServiceLoadExec(path, nullptr);
        printf("[boot] LoadExec failed rc=%d, staying on current build\n", rc);
        fflush(stdout);
        st0 = st2;
      }
    }).detach();
  }
  ps5::debug::set_line(2, "setting up folders...");
  EmuFolders::AppRoot = "/data/PCSX2";
  EmuFolders::DataRoot = "/data/PCSX2";
  // vk-285-33: sub-folders when they exist, else the top folder as before (OrbisPaths.h).
  EmuFolders::Bios = OrbisDir("bios");
  {
    // 2026-10-08 (AI-assisted): PS5SX2/BiosFolder (gs.ini, the settings page), when it is a folder, is looked through first.
    std::string dir = orbis_gs_ini_string("BiosFolder");
    while (dir.size() > 1 && dir.back() == '/')
      dir.pop_back();
    struct stat st = {};
    if (!dir.empty())
    {
      const bool ok = dir[0] == '/' && stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
      printf("[boot] BIOS folder %s (PS5SX2/BiosFolder)%s\n", dir.c_str(), ok ? "" : ": not a folder here, /data/PCSX2/bios instead");
      if (ok)
        EmuFolders::Bios = dir;
      fflush(stdout);
    }
  }
  EmuFolders::Settings = "/data/PCSX2";
  EmuFolders::Logs = OrbisDir("logs");
  EmuFolders::MemoryCards = OrbisDir("memcards");
  EmuFolders::Snapshots = OrbisDir("snapshots");
  EmuFolders::Savestates = OrbisDir("savestates");
  EmuFolders::Cheats = OrbisDir("cheats");
  EmuFolders::Patches = OrbisDir("patches");
  EmuFolders::Cache = OrbisDir("cache");
  EmuFolders::Covers = OrbisDir("covers");
  EmuFolders::GameSettings = "/data/PCSX2";
  EmuFolders::Textures = "/data/PCSX2/textures"; // vk-285-12: packs in textures/<serial>/replacements (HW renderer only)
  EmuFolders::InputProfiles = "/data/PCSX2";
  // Orbis: GL renderer loads shaders from <Resources>/shaders/opengl/*.glsl.
  EmuFolders::Resources = OrbisDir("resources"); // vk-285-33: GameIndex.yaml, shaders/
  printf("[boot] folders: bios %s | memcards %s | savestates %s | patches %s | resources %s | cache %s | logs %s | flags %s\n",
    EmuFolders::Bios.c_str(), EmuFolders::MemoryCards.c_str(), EmuFolders::Savestates.c_str(), EmuFolders::Patches.c_str(),
    EmuFolders::Resources.c_str(), EmuFolders::Cache.c_str(), EmuFolders::Logs.c_str(), OrbisDir("flags").c_str());
  fflush(stdout);

  // Orbis: OpenGL renderer (GPU rasterization via ps5-opengl) when enabled,
  // else SW renderer + CPU GSDeviceOrbis presenting through the overlay.
  g_sw_renderer = orbis_flag("sw_renderer");
#ifdef ORBIS_VULKAN
  // Vulkan build: vk_renderer selects PCSX2's hardware renderer on GSDeviceVK even
  // when sw_renderer is there for the GL build (both builds read these flags).
  // Without it, sw_renderer + swgl is the SW renderer presented by GSDeviceVK.
  if (orbis_flag("vk_renderer")) g_sw_renderer = false;
  // vk-285-45: after the jailbreak again (vk-285-43/44 set it before, where every flag read as
  // absent: no PS5VK_FULL_STATE, WAR barrier, live flags or 8192 extent -- see
  // orbis_log_flag_access).
  orbis_vk_environment();
#endif
  // vk-285-30: the game selector (orbis-shims/game_select.cpp): the disc images in /data/PCSX2 on a
  // screen of their own, before PCSX2 opens the display. Ratchet & Clank when there are none.
  // vk-285-40: after the driver's environment above, because the frontend runs on the same driver.
  extern std::string orbis_select_game(const char* games_dir, const char* top_dir, const char* build_tag);
#ifdef PS5SX2_ACHIEVEMENTS
  OrbisAchievementsInit(s_base_si, "/data/PCSX2");
#endif
  ps5::debug::set_line(2, "game selector...");
#ifdef ORBIS_VULKAN
  // vk-285-40: the frontend (../frontend/fe_ps5.cpp): the disc images as PS2 cases on a cover-flow
  // shelf, drawn through the Vulkan driver, with covers found by serial -- the user's own in
  // covers/, else the ones the prefetch above downloaded into cache/covers/ (vk-285-44: it no
  // longer downloads itself). The plain list below takes over when it can't start, and always
  // with the nofrontend flag.
  // vk-285-50: the settings page (frontend/fe_web.cpp) for phones and PCs, before the shelf that
  // shows its QR code; it keeps running in the game. The nowebui flag leaves it off.
  orbis_scan_usb("after the jailbreak"); // test build 1: games on USB drives
  orbis_check_bios(); // vk-285-134: after the drives are seen, before the shelf
#endif
  // A home screen forwarder's game (--rom): looked for now that /data and the USB drives can be read. Found, it starts
  // without the shelf or the plain list; not found, the shelf opens as usual, with a notification.
  bool forwarded = false;
  if (!forward.rom.empty())
  {
    for (const std::string& path : orbis_forward::Candidates(forward.rom, {OrbisDir("games"), "/data/PCSX2"}))
    {
      struct stat st{};
      if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
      {
        s_game_path = path;
        forwarded = true;
        break;
      }
    }
    s_forward_exit_after_game = forwarded && forward.exit_after_game;
    printf("[boot] forwarded game %s: %s\n", forward.rom.c_str(), forwarded ? s_game_path.c_str() : "not found");
    fflush(stdout);
    orbis_eventf("forwarded game %s: %s", forward.rom.c_str(), forwarded ? "found" : "not found, the shelf opens");
    if (!forwarded)
      sys_notify(("PS5SX2: game not found: " + forward.rom).c_str());
  }
#ifdef ORBIS_VULKAN
  orbis_boot_log_release("before the settings page starts"); // vk-285-113: still holding? (the folder can show up late)
  if (!orbis_flag("nowebui"))
    orbis_web_start(orbis_frontend_paths(false), orbis_build_label().c_str());
  // vk-285-110: the shelf tries the missing covers of games on USB drives (the prefetch above can't see
  // the drives) in the background, after the covers on disk. Nothing else downloads after the jailbreak.
  // HTTPS failed there on vk-285-41/42, with etaHEN's jailbreak; with the PS5SX2 Helper's it hasn't been
  // tried, and boot.log's "[frontend] cover ..." lines will show it. When it fails, the next start's
  // prefetch fetches them from cache/usb-games.txt, as before.
  if (!forwarded && !orbis_flag("nofrontend") && !orbis_flag("nomenu"))
    s_game_path = orbis_frontend_run(orbis_frontend_paths(!orbis_flag("nocoverdl")), ORBIS_BUILD_TAG, &frontend_ran);
#endif
#ifdef ORBIS_VULKAN
  // vk-285-53: the system's launch screen (sce_sys/pic1.dds) covers the app until it is hidden. The
  // shelf hides it on its first frame; without the shelf this does, before the plain list or the game.
  orbis_hide_splash();
#endif
  orbis_boot_log_release("after the shelf"); // vk-285-113
#ifdef PS5SX2_ACHIEVEMENTS
  OrbisAchievementsWaitForLogin();
  OrbisAchievementsStopBrowser(); // pr9n: the shelf's achievement list isn't needed now (was: wait out its requests)
#endif
  if (!forwarded && !frontend_ran)
    s_game_path = orbis_select_game(OrbisDir("games").c_str(), "/data/PCSX2", ORBIS_BUILD_TAG); // vk-285-33: games/ too
  if (s_game_path.empty())
  {
    // vk-285-109: no game to start (none found, or none picked). This fell back to games/Ratchet & Clank.iso,
    // which failed wherever that file isn't ("VM init failed"), and the process then aborted on its way out:
    // the signal 6 crashes in the testers' logs (vk-285-71 to 105, consoles with no image visible at start,
    // e.g. games only on a USB drive not mounted yet). Now it says so and closes without the crash.
    printf("[boot] no game to start: closing\n");
    orbis_eventf("no game to start (no disc image found, or none picked): the app closed");
    sys_notify(fe::Tr(fe::Str::NotifyNoGame));
    orbis_exit_quietly(0);
  }
  // 2026-10-08 (AI-assisted; testers asked for the PS2's own boot and menu): the sheet's "PS2 system menu" (fe_ps5.h
  // kOrbisSystemMenuPath): no disc, so VMManager boots the BIOS into its menu (AutoDetectSource: an empty file name is
  // CDVD_SourceType::NoDisc); gs.ini's settings, no game's file. Needs proper testing on the console.
  const bool system_menu = s_game_path == kOrbisSystemMenuPath;
  if (system_menu)
  {
    s_game_path.clear();
    s_game_ini_path.clear();
    printf("[boot] the PS2 system menu (no disc); settings: gs.ini only\n");
    orbis_eventf("game start: the PS2 system menu (no disc) | all games (gs.ini): %s", orbis_ini_summary("/data/PCSX2/gs.ini").c_str());
    fflush(stdout);
  }
  else
  {
    // vk-285-32: its settings file, named after the image without the extension.
    std::string stem = s_game_path.substr(s_game_path.rfind('/') + 1);
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos && dot > 0)
      stem.erase(dot);
    s_game_ini_path = "/data/PCSX2/settings/" + stem + ".ini";
  }
#ifdef ORBIS_VULKAN
  if (!system_menu)
    orbis_web_now_playing(s_game_path); // vk-285-50: the page marks it and applies its changes live
#endif
  if (!system_menu)
  {
    printf("[boot] game: %s\n[boot] game settings: %s (%s)\n", s_game_path.c_str(), s_game_ini_path.c_str(),
      access(s_game_ini_path.c_str(), F_OK) == 0 ? "found" : "none");
    fflush(stdout);
    // Test build 1 (vk-285-55): where the image is, when it isn't in games/ (a USB drive, say).
    const std::string dir = s_game_path.substr(0, s_game_path.rfind('/'));
    const std::string from = dir == OrbisDir("games") ? std::string() : " (from " + dir + ")";
    orbis_eventf("game start: %s%s | its settings: %s | all games (gs.ini): %s", // vk-285-51
      s_game_path.substr(s_game_path.rfind('/') + 1).c_str(), from.c_str(), orbis_ini_summary(s_game_ini_path).c_str(),
      orbis_ini_summary("/data/PCSX2/gs.ini").c_str());
  }
  // 2026-10-05 (AI-assisted): rich toasts (RetroAchievements' login and game summary) wait for the game's first 12 s:
  // sent in its first seconds, they showed without their pictures (orbis-shims/ProsperoNotify.cpp, the pacing).
  OrbisNotifyHold(12);
#ifdef ORBIS_VULKAN
  if (g_orbis_test_build > 0)
  {
    // Test build 1 (vk-285-55): the watermark over the game (GSRenderer.cpp OrbisWatermark):
    // TESTING and the build, faint, in the middle of the screen; vk-285-105: the Discord note under them.
    const std::string label = orbis_build_label(), note = orbis_test_note();
    const bool wm = orbis_frontend_watermark("TESTING", label.c_str(), note.c_str(), 0.20f, 0.45f, g_orbis_watermark,
      g_orbis_watermark_w, g_orbis_watermark_h);
    printf("[boot] testing watermark: %s (%dx%d)\n", wm ? "ready" : "no font", g_orbis_watermark_w, g_orbis_watermark_h);
    fflush(stdout);
  }
#endif

  // vk-285-118 (AI-assisted): PS5SX2/Renderer per game (or for all games in gs.ini): Hardware or Software, read at game
  // start (the renderer can't change while a game runs here). Software renders on the CPU and GSDeviceVK presents it, as
  // swgl does; PCSX2's own EmuCore/GS/Renderer=13 in those files means the same. Unset: the flags decide, as before.
  bool sw_by_setting = false;
  {
    MemorySettingsInterface peek;
    orbis_apply_gs_ini(peek, true);
    std::string r;
    peek.GetStringValue("PS5SX2", "Renderer", &r);
    const int pcsx2_renderer = peek.GetIntValue("EmuCore/GS", "Renderer", -1);
    if (r == "Software" || r == "software" || r == "sw" || (r.empty() && pcsx2_renderer == static_cast<int>(GSRendererType::SW)))
    {
      g_sw_renderer = true;
      sw_by_setting = true;
    }
    else if ((r == "Hardware" || r == "hardware" || r == "hw") && g_sw_renderer)
    {
      g_sw_renderer = false;
#ifdef ORBIS_VULKAN
      orbis_vk_environment(); // the hardware renderer's driver variables, which the sw_renderer flag left out
#endif
    }
    if (!r.empty() || pcsx2_renderer >= 0)
    {
      printf("[boot] renderer: %s (PS5SX2/Renderer=%s, EmuCore/GS/Renderer=%d)\n", g_sw_renderer ? "software" : "hardware",
        r.empty() ? "(unset)" : r.c_str(), pcsx2_renderer);
      fflush(stdout);
    }
  }
  g_orbis_sw_on_gl = g_sw_renderer && (orbis_flag("swgl") || sw_by_setting);
  // SW path: CPU GSDeviceOrbis + debug overlay presents, unless swgl (GSDeviceOGL presents the SW frames).
  if (g_sw_renderer && !g_orbis_sw_on_gl) g_use_gl_renderer = false;
  g_no_speedhacks = orbis_flag("nospeedhacks");
  g_vu_interp = orbis_flag("vuinterp");
  g_ee_interp = orbis_flag("eeinterp");
  g_iop_interp = orbis_flag("iopinterp");
  EmuConfig.GS.Renderer = (g_use_gl_renderer && !g_sw_renderer) ? ORBIS_GPU_RENDERER : GSRendererType::SW;
  EmuConfig.GS.SWExtraThreads = g_sw_renderer ? 4 : 2;

  {
    std::unique_lock<std::mutex> lock = Host::GetSettingsLock();
#ifndef PS5SX2_ACHIEVEMENTS
    Host::Internal::SetBaseSettingsLayer(&s_base_si);
#endif
    Host::Internal::SetGameSettingsLayer(&s_game_si, lock);
    Host::Internal::SetInputSettingsLayer(&s_input_si, lock);
  }
  printf("[boot] settings layers installed\n");
  fflush(stdout);
  ps5::debug::set_line(2, "settings layers installed");

  // Orbis: run the interpreters for now. The x86 JIT dispatcher generation
  // crashes in the bigapp (likely JIT/RWX or code-cache allocation), so boot
  // the VM with the interpreter paths first. These must be set through the
  // settings interface because VMManager::Initialize -> ApplySettings()
  // reloads EmuConfig from the settings layers (defaults re-enable rec).
    s_base_si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", !g_ee_interp);
    s_base_si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableIOP", !g_iop_interp);
    s_base_si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", !g_vu_interp);
    s_base_si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", !g_vu_interp);
    // Orbis: fastmem crashes in memReset on PS5 (pc=0 in a spawned thread;
    // likely mprotect/VirtualAlloc-style reservation or a startup race).
    // Keep off until that init path is diagnosed. See eerec-58.
    // vk-285-64: the cause was the area itself -- views of the guest pages need memory that can be
    // mapped twice, and the area was flexible memory (or nothing). With the guest's data in direct
    // memory the area is a reservation filled with direct mappings (common/Linux/LnxHostSys.cpp);
    // on with the flag file fastmem.
    s_base_si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableFastmem", orbis_flag("fastmem"));
    printf("[boot] fastmem %s\n", orbis_flag("fastmem") ? "on (flag fastmem)" : "off");
    s_base_si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableEECache", false);
    s_base_si.SetBoolValue("EmuCore/Speedhacks", "WaitLoop", true);
    s_base_si.SetBoolValue("EmuCore/Speedhacks", "IntcStat", true);
    s_base_si.SetBoolValue("EmuCore/Speedhacks", "vu1Instant", !g_no_speedhacks);
    s_base_si.SetBoolValue("EmuCore/Speedhacks", "vuFlagHack", !g_no_speedhacks);
    s_base_si.SetBoolValue("EmuCore/Speedhacks", "vuThread", false);
  s_base_si.SetIntValue("EmuCore/GS", "Renderer", (s32)((g_use_gl_renderer && !g_sw_renderer) ? ORBIS_GPU_RENDERER : GSRendererType::SW));
  s_base_si.SetIntValue("EmuCore/GS", "extrathreads", g_sw_renderer ? 4 : 2); // eerec-285: PCSX2's key; "SWExtraThreads" never applied
  s_base_si.SetStringValue("EmuCore", "Filename", s_game_path.c_str()); // vk-285-30: the selector's pick
  // vk-285-72: a USB keyboard on the PS2's USB port 1 and a USB mouse on port 2, fed from the PS5's own
  // (orbis-shims/ProsperoKbdMouse.cpp), for the games that take them (Half-Life, Unreal Tournament, the
  // online games' chat). The flag file nousbkbm leaves both ports empty; a game's settings file can set
  // USB1/Type or USB2/Type itself (None to take one off).
  if (!orbis_flag("nousbkbm"))
  {
    s_base_si.SetStringValue("USB1", "Type", "hidkbd");
    s_base_si.SetStringValue("USB2", "Type", "hidmouse");
  }
  printf("[boot] USB keyboard and mouse %s\n", orbis_flag("nousbkbm") ? "off (flag nousbkbm)" : "on ports 1 and 2");
  // 2026-10-05 (AI-assisted; swordpdf: the network adapter on for every game, after a tester went online in Resident Evil
  // Outbreak with SOCOM II's lines): the PS2's network adapter as SOCOM II's file sets it (claude/socom2-online.md):
  // PCSX2's sockets backend on the console's own connection, its DHCP server giving the game an address, and the DNS the
  // PS5 uses (pcsx2/DEV9/AdapterUtils.cpp GetDNS). DEV9's receive thread starts only once a game sends (net.cpp), so games
  // that never use the network don't pay for it. gs.ini, a game's file or the sheet's Network adapter row
  // (DEV9/Eth/EthEnable=false) turn it off; so does the flag file nonetwork, for every game.
  if (!orbis_flag("nonetwork"))
  {
    s_base_si.SetBoolValue("DEV9/Eth", "EthEnable", true);
    s_base_si.SetStringValue("DEV9/Eth", "EthApi", "Sockets");
    s_base_si.SetStringValue("DEV9/Eth", "EthDevice", "Auto");
    s_base_si.SetBoolValue("DEV9/Eth", "InterceptDHCP", true);
  }
  printf("[boot] PS2 network adapter %s\n", orbis_flag("nonetwork") ? "off (flag nonetwork)" : "on by default (sockets, DHCP)");
  {
    // vk-285-109: player 2 (see g_orbis_second_user).
    int32_t first = -1;
    (void)sceUserServiceInitialize(nullptr);
    (void)sceUserServiceGetInitialUser(&first);
    g_orbis_second_user = orbis_find_second_user(first);
    // vk-285-118 (AI-assisted): PS5SX2/Multitap from gs.ini and the game's file (a copy: the base keeps them out, as the
    // live reload expects), or PCSX2's own Pad/MultitapPort1 or MultitapPort2 there. Pad types and the multitap go into
    // the base, so they stay through live reloads; a change takes a restart of the game.
    {
      MemorySettingsInterface peek = s_base_si;
      orbis_apply_gs_ini(peek, true);
      int mt = peek.GetIntValue("PS5SX2", "Multitap", 0);
      if (mt < 0 || mt > 2)
        mt = 0;
      if (mt == 0 && peek.GetBoolValue("Pad", "MultitapPort1", false))
        mt = 1;
      if (mt == 0 && peek.GetBoolValue("Pad", "MultitapPort2", false))
        mt = 2;
      g_orbis_multitap = mt;
    }
    int32_t others[3] = {-1, -1, -1};
    const int n_others = orbis_find_other_users(first, others);
    static const u32 slots[3][3] = {{1, 0, 0}, {2, 3, 4}, {1, 5, 6}};
    g_orbis_extra_count = g_orbis_multitap == 0 ? std::min(n_others, 1) : n_others;
    for (int i = 0; i < 3; i++)
    {
      g_orbis_extra_user[i] = i < g_orbis_extra_count ? others[i] : -1;
      g_orbis_extra_index[i] = slots[g_orbis_multitap][i];
    }
    if (g_orbis_multitap != 0)
    {
      s_base_si.SetBoolValue("Pad", g_orbis_multitap == 1 ? "MultitapPort1" : "MultitapPort2", true);
      // the multitap's other slots answer as DualShock 2s with no controller behind them yet, so games count 4 ports
      for (int i = 0; i < 3; i++)
        s_base_si.SetStringValue(("Pad" + std::to_string(slots[g_orbis_multitap][i] + 1)).c_str(), "Type", "DualShock2");
    }
    else if (g_orbis_extra_count > 0)
      s_base_si.SetStringValue("Pad2", "Type", "DualShock2");
    std::string who;
    for (int i = 0; i < g_orbis_extra_count; i++)
      who += " player " + std::to_string(i + 2) + "=user " + std::to_string(g_orbis_extra_user[i]) + " on pad " +
             std::to_string(g_orbis_extra_index[i] + 1);
    printf("[boot] users: first %d, %d more; multitap %s (PS5SX2/Multitap);%s\n", first, n_others,
      g_orbis_multitap == 0 ? "off" : g_orbis_multitap == 1 ? "in port 1" : "in port 2",
      who.empty() ? " PS2 port 2 stays empty" : who.c_str());
  }
  s_base_pre_gsini = s_base_si; // eerec-285
  orbis_apply_gs_ini(s_base_si);
#ifdef PS5SX2_ACHIEVEMENTS
  OrbisAchievementsConfigure(s_base_si);
#endif
  orbis_usb_kbm_for_mode(s_base_si); // vk-285-113
  orbis_vu1_speed_from(s_base_si); // vk-285-75
  orbis_ps5opts_from(s_base_si); // vk-285-113
#ifdef ORBIS_VULKAN
  {
    // 2026-10-08 (AI-assisted): PS5SX2/FrameGeneration, per game (or in gs.ini): a frame generated between two of the game's and
    // presented before the second (GSDeviceVK.cpp, "frame generation"), with the TV in its 120 Hz mode for the game (VKSwapChain.cpp
    // picks the mode when the swapchain is made, so it is read here, before the GS opens). Needs proper testing.
    g_orbis_fg_wanted = s_base_si.GetBoolValue("PS5SX2", "FrameGeneration", false) && !g_sw_renderer;
    // The latency hold (RPCS3-PS5 m74): at 120 Hz each game frame brings two presents, which fill every refresh; a game a hair
    // faster than half the display's rate would fill the swapchain's queue and show every frame later from then on. The game
    // runs 0.08% slow instead (59.89 fps for NTSC's 59.94): the queue stays empty, and once in a while a refresh repeats.
    orbis_fg_hold(s_base_si);
    printf("[boot] frame generation: %s\n", g_orbis_fg_wanted ? "on (PS5SX2/FrameGeneration; the display asked for at 120 Hz)" :
      "off");
    fflush(stdout);
  }
#endif
  {
    // vk-285-110: the PS2 system language games are told (pcsx2/CDVD/CDVD.cpp): the PS5SX2/GameLanguage
    // setting (0 Japanese .. 7 Portuguese, from gs.ini or the game's settings file), else the PS5's own
    // language, mapped by fe::Ps2LanguageFor. PAL games with several languages start in it.
    extern int g_orbis_ps2_language;
    const int ps5 = orbis_ps5_language();
    const int set = s_base_si.GetIntValue("PS5SX2", "GameLanguage", -1);
    const bool fixed = set >= 0 && set <= 7;
    g_orbis_ps2_language = fixed ? set : fe::Ps2LanguageFor(ps5);
    printf("[boot] game language: %s (%s; PS5 language %d)\n", fe::Ps2LanguageName(g_orbis_ps2_language),
      fixed ? "PS5SX2/GameLanguage" : "the PS5's", ps5);
  }

  {
    Error pf_err;
    const bool pf_ok = PageFaultHandler::Install(&pf_err);
    printf("[boot] PageFaultHandler::Install=%d %s\n", (int)pf_ok, pf_err.GetDescription().c_str());
    fflush(stdout);
  }

  VMBootParameters params;
  params.filename = s_game_path; // vk-285-30
  // vk-285-139: a game's discs (and the cheat disc as disc 1 when the game starts from it).
  if (!s_game_path.empty() && !(s_game_path.size() > 4 && strcasecmp(s_game_path.c_str() + s_game_path.size() - 4, ".elf") == 0))
  {
    params.filename = orbis_disc_boot(s_game_path);
    if (params.filename != s_game_path)
      params.source_type = CDVD_SourceType::Iso;
  }
  // 2026-10-08 (AI-assisted; testers: "mount ELFs", "ELF properties: disc path"): an .elf from the shelf boots as PCSX2 boots
  // an ELF (VMBootParameters::elf_override, always fast booted; host: is its folder when EmuCore/HostFs is on), with the disc
  // image its settings file names (PS5SX2/ElfDisc: a file name the shelf lists, or a full path), else with no disc. Needs
  // proper testing on the console.
  if (s_game_path.size() > 4 && strcasecmp(s_game_path.c_str() + s_game_path.size() - 4, ".elf") == 0)
  {
    params.elf_override = s_game_path;
    const std::string disc = orbis_elf_disc();
    if (!disc.empty())
    {
      params.filename = disc;
      params.source_type = CDVD_SourceType::Iso;
      std::lock_guard<std::mutex> lock(s_disc_mutex); // vk-285-139: the page can change it
      s_disc_set = {disc};
      s_disc_current = disc;
    }
    else
    {
      params.filename.clear();
      params.source_type = CDVD_SourceType::NoDisc;
    }
    printf("[boot] ELF %s, %s\n", s_game_path.c_str(), disc.empty() ? "no disc (PS5SX2/ElfDisc not set or not found)" : ("disc " + disc).c_str());
    fflush(stdout);
  }

  // The PS5 account service uses native notifications instead of ImGui overlays.
#ifndef PS5SX2_ACHIEVEMENTS
  EmuConfig.Achievements.Enabled = false;
#endif
  GSConfig.Renderer = (g_use_gl_renderer && !g_sw_renderer) ? ORBIS_GPU_RENDERER : GSRendererType::SW;
  GSConfig.SWExtraThreads = g_sw_renderer ? 4 : 2;
  // EE+IOP+VU recompilers (JIT memory + emitter >4GB fixes in).
  EmuConfig.Cpu.Recompiler.EnableEE = !g_ee_interp;
  EmuConfig.Cpu.Recompiler.EnableIOP = !g_iop_interp;
  EmuConfig.Cpu.Recompiler.EnableVU0 = !g_vu_interp;
  EmuConfig.Cpu.Recompiler.EnableVU1 = !g_vu_interp;
  EmuConfig.Speedhacks.WaitLoop = true;
  EmuConfig.Speedhacks.IntcStat = true;
  EmuConfig.Speedhacks.vu1Instant = !g_no_speedhacks;
  EmuConfig.Speedhacks.vuFlagHack = !g_no_speedhacks;
  EmuConfig.Speedhacks.vuThread = false;
#ifdef PS5SX2_ACHIEVEMENTS
  printf("[boot] achievements available (softcore, native notifications)\n");
#else
  printf("[boot] achievements disabled (no ImGui backend)\n");
#endif
  fflush(stdout);
  printf("[boot] initializing...\n");
  fflush(stdout);
  ps5::debug::set_line(2, "VMManager::Initialize running...");

  std::atomic<bool> init_done{false};
  std::thread watchdog([&init_done]() {
    for (int i = 0; i < 40 && !init_done.load(); i++)
    {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      if (!init_done.load())
      {
        printf("[boot] watchdog t+%ds: still in Initialize\n", (i + 1) * 5);
        fflush(stdout);
      }
    }
  });
  watchdog.detach();

  {
    printf("[boot] BIOS dir scan:\n");
    fflush(stdout);
    {
      DIR* d = opendir("/data/PCSX2");
      printf("[boot]   opendir(/data/PCSX2)=%p errno=%d\n", (void*)d, errno);
      if (d)
      {
        int n = 0;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr && n < 20)
        {
          printf("[boot]     entry: %s\n", e->d_name);
          n++;
        }
        closedir(d);
      }
      fflush(stdout);
    }
    {
      DIR* d = opendir("/data");
      printf("[boot]   opendir(/data)=%p errno=%d\n", (void*)d, errno);
      if (d) { closedir(d); }
      fflush(stdout);
    }
    FileSystem::FindResultsArray results;
    if (FileSystem::FindFiles("/data/PCSX2", "*", FILESYSTEM_FIND_FILES, &results))
    {
      for (const auto& fd : results)
        printf("[boot]   found: %s (%lld bytes)\n", fd.FileName.c_str(), (long long)fd.Size);
    }
    else
    {
      printf("[boot]   FindFiles FAILED\n");
    }
    fflush(stdout);
    printf("[boot] BIOS exists: %d\n", (int)FileSystem::FileExists((EmuFolders::Bios + "/SCPH-90001_BIOS_V18_USA_230.ROM0").c_str()));
    fflush(stdout);
  }

  // Catch assert/abort/ud2 crashes: log PC then die.
  {
    freopen(OrbisLogPath("stderr.log").c_str(), "w", stderr); // vk-285-33: logs/
#ifdef ORBIS_VULKAN
    // The driver writes its refusals and its queue profile to stderr. Line-buffered:
    // each finished line is one write, so an abort still keeps it. Not _IONBF (vk-285-6):
    // unbuffered, the profile's 177-character line held the present 128-133 ms every
    // 10 s, the 47-48 fps dips -- 0.72 ms a character, the cost of one write to /data
    // (see stdout above), so the libc writes an unbuffered stream a character at a time.
    setvbuf(stderr, nullptr, _IOLBF, 4096);
#endif
    orbis_install_signal_handlers();
    printf("[boot] crash handlers installed\n");
    fflush(stdout);
  }

  // Memory budget probe: measure what the bigapp actually grants.
  {
    unsigned long long cfg = 0, avail = 0, dphys = 0, dsize = 0;
    sceKernelConfiguredFlexibleMemorySize(&cfg);
    sceKernelAvailableFlexibleMemorySize(&avail);
    long long pa = 0;
    sceKernelAvailableDirectMemorySize(0, ~0ULL, 0x1000, &pa, &dsize);
    printf("[boot] mem: configured_flexible=%llu avail_flexible=%llu direct=%llu\n", cfg, avail, dsize);
    fflush(stdout);
  }

  Error err;
  // Init CPU providers (rec LUTs etc.) + allocate the host memory map, as the
  // normal CPU-thread path does. The recompiler's rec reset (ClearCPUExecutionCaches)
  // needs both BEFORE VMManager::Initialize's SysMemory::Reset runs.
  {
    printf("[boot] CPUThreadInitialize (pre-init)\n");
    fflush(stdout);
    if (VMManager::Internal::CPUThreadInitialize())
      printf("[boot] CPUThreadInitialize OK\n");
    else
      printf("[boot] CPUThreadInitialize FAILED\n");
    fflush(stdout);
    // Orbis: the CPU overlay owns VideoOut and presents the frame the GL
    // renderer reads back (GSDeviceOGL::PresentRect -> orbis_present_frame).
    // ps5-opengl's own flip path is not used (it kernel-panicked when driven
    // from the MTGS thread). The capability probe is GL-renderer-only.
#ifndef ORBIS_VULKAN
    if (!g_use_gl_renderer && !OrbisFlag("glprobe.off"))
    {
      ps5::debug::set_line(1, "GL probe running");
      orbis_gl_probe();
    }
#endif
    // Orbis: the GL renderer's runtime owns VideoOut (MAIN bus) and presents
    // via eglSwapBuffers; the CPU overlay would fight it for the display. The
    // overlay is only used by the SW renderer path.
    if (!g_use_gl_renderer && !OrbisFlag("nooverlay"))
    {
      ps5::debug::start_overlay();
      ps5::debug::set_build(ORBIS_BUILD_TAG);
      ps5::debug::set_line(1, "overlay started " ORBIS_BUILD_TAG);
    }
    else
    {
      printf("[boot] overlay disabled (GL renderer owns VideoOut)\n");
      fflush(stdout);
    }
  }
  // Orbis: VMManager::Initialize's SysMemory::Reset (memReset -> vtlb_Init) is
  // skipped/reached too late in this flow; the recompiler's first memory access
  // then calls an uninitialized RWFT handler (0x55). Do the full memory reset
  // (vtlb init + RAM/ROM mappings + handlers) up front, before any recompile.
  printf("[boot] SysMemory::Reset (pre-initialize)\n");
  fflush(stdout);
  SysMemory::Reset();
  printf("[boot] SysMemory::Reset done\n");
  fflush(stdout);
  orbis_wait_for_game_file(params.filename); // vk-285-134
  const VMBootResult res = VMManager::Initialize(params, &err);
  init_done = true;
  printf("[boot] Initialize=%d err=%s\n", (int)res, err.GetDescription().c_str());
  fflush(stdout);
  // vk-285-47: the game's name and how it runs, instead of "VM init OK".
  if (res == VMBootResult::StartupSuccess)
  {
    std::string title = VMManager::GetTitle(true);
    if (title.empty() && system_menu)
      title = "PS2 system menu"; // 2026-10-08
    if (title.empty())
    {
      title = s_game_path.substr(s_game_path.rfind('/') + 1);
      const size_t dot = title.rfind('.');
      if (dot != std::string::npos && dot > 0)
        title.erase(dot);
    }
    const GSRendererType renderer = EmuConfig.GS.Renderer;
    const char* api = renderer == GSRendererType::VK ? "Vulkan" : renderer == GSRendererType::OGL ? "OpenGL" : nullptr;
    char how[64];
    // vk-285-110: in the PS5's language (fe_i18n.cpp).
    if (!api)
      snprintf(how, sizeof(how), "%s", fe::Tr(fe::Str::HowSoftware));
    else if (EmuConfig.GS.UpscaleMultiplier > 1.0f)
      snprintf(how, sizeof(how), "%gx %s", EmuConfig.GS.UpscaleMultiplier, api);
    else
      snprintf(how, sizeof(how), fe::Tr(fe::Str::HowNative), api);
    char msg[512];
    snprintf(msg, sizeof(msg), fe::Tr(fe::Str::NotifyNowPlaying), title.c_str(), how);
    sys_notify(msg);
  }
  else
  {
    // vk-285-109: the reason too (a missing or unreadable image, an unknown disc type).
    const std::string why = err.GetDescription();
    char msg[512];
    // vk-285-115 (AI-assisted): the two commonest failures of 1.50's starts said briefly, in the PS5's language: no
    // BIOS (689 failed starts on 146 consoles) and the GS device (439 on 28). After a GS failure the shader caches are
    // set aside, so the next start builds them again (one console's cache held a blob the driver refused, 30 failed
    // starts in a row).
    if (why.find("requires a PlayStation 2 BIOS") != std::string::npos)
    {
      snprintf(msg, sizeof(msg), fe::Tr(fe::Str::NotifyNoBios), (EmuFolders::Bios + "/").c_str());
    }
    else if (why.find("Failed to initialize GS") != std::string::npos)
    {
      orbis_set_aside_shader_caches();
      snprintf(msg, sizeof(msg), "%s", fe::Tr(fe::Str::NotifyGsFailed));
    }
    else
    {
      snprintf(msg, sizeof(msg), fe::Tr(fe::Str::NotifyNotStarted), why.c_str()); // vk-285-110
    }
    sys_notify(msg);
    orbis_eventf("the game didn't start: VM init failed (%s)", why.c_str()); // vk-285-51
  }
  ps5::debug::set_line(1, "Initialize=%d", (int)res);
  ps5::debug::set_line(2, "%s", res == VMBootResult::StartupSuccess ? "VM INIT OK" : "VM INIT FAILED");
  ps5::debug::set_line(3, "%s", err.GetDescription().c_str());
  if (res != VMBootResult::StartupSuccess)
  {
    // vk-285-109: back to the shelf rather than out: returning from main here aborted (signal 6) with PCSX2
    // half started, a crash on top of the failed start. Only when the game came from the shelf, which waits
    // for a pick: the plain list's automatic picks (the only image, the nomenu flag, no controller or
    // display) would choose the same image again, and fail again, in a loop.
    // A forwarder's game too: the re-execution has no arguments, so it opens the shelf rather than this image again.
    if (frontend_ran || forwarded)
      orbis_restart_to_menu();
    orbis_exit_quietly(1);
  }

  // Must be Running before Execute: IsExecutionInterrupted() returns true for any
  // other state, which makes the first event test exit the interpreter immediately.
  VMManager::SetState(VMState::Running);
  {
    pthread_t pad_thread;
    if (pthread_create(&pad_thread, nullptr, orbis_pad_thread, nullptr) == 0)
      pthread_detach(pad_thread);
  }
  // vk-285-72: the PS5's USB keyboard and mouse, read for the PS2's (orbis-shims/ProsperoKbdMouse.cpp).
  if (!orbis_flag("nousbkbm"))
    OrbisKbdMouseStart();
  orbis_prof_start();
  // Benchmark raw memory reads + arithmetic (isolates interpreter slowness).
  {
    volatile unsigned long long acc = 0;
    auto b0 = std::chrono::steady_clock::now();
    for (unsigned long long i = 0; i < 10000000ULL; i++)
      acc += i;
    auto b1 = std::chrono::steady_clock::now();
    printf("[boot] bench: 10M adds = %.1fms\n",
      std::chrono::duration<double, std::milli>(b1 - b0).count());
    fflush(stdout);

    volatile u32* buf = (volatile u32*)malloc(4096 * 4);
    u32 sum = 0;
    auto r0 = std::chrono::steady_clock::now();
    for (unsigned long long i = 0; i < 10000000ULL; i++)
      sum += buf[i & 0x3FF];
    auto r1 = std::chrono::steady_clock::now();
    printf("[boot] bench: 10M host reads = %.1fms (sum=%u)\n",
      std::chrono::duration<double, std::milli>(r1 - r0).count(), sum);
    fflush(stdout);
    free((void*)buf);

    // vmap probe: 0x9FC42 (KSEG1 ROM) vs 0x1FC42 (phys ROM)
    if (vtlb_private::vtlbdata.vmap)
    {
      auto e1 = vtlb_private::vtlbdata.vmap[0x1FC42];
      auto e9 = vtlb_private::vtlbdata.vmap[0x9FC42];
      printf("[boot] vmap[0x1FC42]=%llx h1=%d vmap[0x9FC42]=%llx h9=%d\n",
        (unsigned long long)e1.raw(), (int)e1.isHandler(0x1FC42BE0),
        (unsigned long long)e9.raw(), (int)e9.isHandler(0x9FC42BE0));
      fflush(stdout);
      u32 sumA = 0, sumB = 0;
      auto a0 = std::chrono::steady_clock::now();
      for (unsigned long long i = 0; i < 10000000ULL; i++)
        sumA += memRead32(0x1FC42BE0ULL + ((u32)i & 0x3FF) * 4);
      auto a1 = std::chrono::steady_clock::now();
      for (unsigned long long i = 0; i < 10000000ULL; i++)
        sumB += memRead32(0x9FC42BE0ULL + ((u32)i & 0x3FF) * 4);
      auto a2 = std::chrono::steady_clock::now();
      printf("[boot] bench: 10M phys-rom reads = %.1fms, 10M kseg1-rom reads = %.1fms\n",
        std::chrono::duration<double, std::milli>(a1 - a0).count(),
        std::chrono::duration<double, std::milli>(a2 - a1).count());
      fflush(stdout);
    }

    u32 sum2 = 0;
    auto v0 = std::chrono::steady_clock::now();
    for (unsigned long long i = 0; i < 10000000ULL; i++)
      sum2 += memRead32(0x1FC42BE0ULL + ((u32)i & 0x3FF) * 4);
    auto v1 = std::chrono::steady_clock::now();
    printf("[boot] bench: 10M vtlb memRead32 = %.1fms (sum=%u)\n",
      std::chrono::duration<double, std::milli>(v1 - v0).count(), sum2);
    fflush(stdout);
    // Orbis: clock granularity (quantized per-slice timings?) + log write cost.
    {
      auto c0 = std::chrono::steady_clock::now();
      unsigned long long cmin = ~0ULL, cmax = 0, csum = 0;
      for (int i = 0; i < 2000; i++)
      {
        auto t1 = std::chrono::steady_clock::now();
        auto t2 = std::chrono::steady_clock::now();
        unsigned long long d = (unsigned long long)std::chrono::duration<double, std::nano>(t2 - t1).count();
        if (d < cmin) cmin = d; if (d > cmax) cmax = d; csum += d;
      }
      auto c1 = std::chrono::steady_clock::now();
      // Consecutive-clock delta distribution: how many distinct nonzero values?
      unsigned long long distinct = 0, lastd = ~0ULL;
      for (int i = 0; i < 2000; i++)
      {
        auto t1 = std::chrono::steady_clock::now();
        auto t2 = std::chrono::steady_clock::now();
        unsigned long long d = (unsigned long long)std::chrono::duration<double, std::nano>(t2 - t1).count();
        if (d != lastd) { distinct++; lastd = d; }
      }
      printf("[boot] bench: 2000x now() min=%lluns max=%lluns avg=%lluns wall=%.1fms distinct=%llu\n",
        cmin, cmax, csum / 2000,
        std::chrono::duration<double, std::milli>(c1 - c0).count(), distinct);
      fflush(stdout);
      auto p0 = std::chrono::steady_clock::now();
      for (int i = 0; i < 500; i++)
      {
        printf("[boot] bench: printf probe line %d abcdefghijklmnopqrstuvwxyz 0123456789\n", i);
        fflush(stdout);
      }
      auto p1 = std::chrono::steady_clock::now();
      printf("[boot] bench: 500x printf+fflush = %.1fms (%.3fms each)\n",
        std::chrono::duration<double, std::milli>(p1 - p0).count(),
        std::chrono::duration<double, std::milli>(p1 - p0).count() / 500.0);
      fflush(stdout);
    }
  }

  printf("[boot] state=%d pc=%08x\n", (int)VMManager::GetState(), cpuRegs.pc);
  fflush(stdout);
  ps5::debug::set_line(1, "state=%d pc=%08x", (int)VMManager::GetState(), cpuRegs.pc);

  // Run Execute on the MAIN thread: its stack is the process stack (safe, not
  // flexible) so the EE RAM writes can't clobber it (worker pthread stacks were
  // placed inside the memory map -> stack corruption -> NULL calls).
  ps5::debug::set_line(2, "Execute begin");
  ps5::debug::set_line(3, "booting...");
  {
    unsigned long long stk = 0;
    asm volatile("mov %%rsp, %0" : "=r"(stk));
    printf("[boot] Execute: self=%p rsp=%p\n", (void*)pthread_self(), (void*)stk);
    fflush(stdout);
  }
  VMManager::Execute();
  printf("[boot] Execute returned (first tenure)\n");
  fflush(stdout);
  ps5::debug::set_line(2, "Execute returned");

  // Orbis: Cpu->Execute() runs until the first JIT exit (reset, exception,
  // SIF LoadELF...) then RETURNS. Stock re-enters via its emu-thread loop;
  // without re-entry the VM silently stops after the ELF-load reset (pc and
  // cycles freeze, state stays Running). Ticker thread samples, main thread
  // drives re-entry until the smoke window ends.
  std::thread ticker([]() {
    // Runs for the whole session: 1s sampling + doubles as the periodic
    // log flusher (stdout is fully buffered; crash handler flushes on death).
    // vk-285-108: each pass's steps are timed (a [ticker] line when a pass runs long) and published for the
    // GS thread's watchdog (GSRenderer.cpp OrbisTickerWatch): the step, the CPU and the time of the last pass.
    using TickClock = std::chrono::steady_clock;
    const auto ms_between = [](TickClock::time_point a, TickClock::time_point b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    for (int i = 0; ; i++)
    {
      g_orbis_ticker_step.store(0, std::memory_order_relaxed);
      const TickClock::time_point t_sleep = TickClock::now();
      std::this_thread::sleep_for(std::chrono::seconds(1));
      const int cpu_start = sceKernelGetCurrentCpu();
      g_orbis_ticker_cpu.store(cpu_start, std::memory_order_relaxed);
      g_orbis_ticker_step.store(1, std::memory_order_relaxed);
      const TickClock::time_point t_flags = TickClock::now();
      // vk-285-105: the flags folder and the polled settings files, read here for the emulation threads
      // (OrbisFlag and OrbisCachedRead answer from this snapshot; OrbisPaths.h).
      OrbisFlagsRefresh();
      g_orbis_ticker_step.store(2, std::memory_order_relaxed);
      const TickClock::time_point t_drain = TickClock::now();
      // vk-285-104: the GS thread's deferred lines (OrbisDeferredLog.h) out first, from this thread.
      orbis_log_drain();
      g_orbis_ticker_step.store(3, std::memory_order_relaxed);
      const TickClock::time_point t_lines = TickClock::now();
      {
        extern unsigned long long g_orbis_gs_idle_ticks, g_orbis_ee_waitgs_ticks, g_orbis_ee_stall_ticks, g_orbis_ee_waitgs_n, g_orbis_ee_stall_n;
        extern unsigned long long g_orbis_hwdraw_n;
        static unsigned long long p0, p1, p2, p3, p4, p5;
        printf("[threads] draws=%llu gs_idle_ms=%.0f ee_waitgs_ms=%.0f(%llu) ee_ringfull_ms=%.0f(%llu) ticker_cpu=%d\n",
          g_orbis_hwdraw_n - p5, (g_orbis_gs_idle_ticks - p0) / 1596000.0, (g_orbis_ee_waitgs_ticks - p1) / 1596000.0, g_orbis_ee_waitgs_n - p3,
          (g_orbis_ee_stall_ticks - p2) / 1596000.0, g_orbis_ee_stall_n - p4, cpu_start);
        p0 = g_orbis_gs_idle_ticks; p1 = g_orbis_ee_waitgs_ticks; p2 = g_orbis_ee_stall_ticks; p3 = g_orbis_ee_waitgs_n; p4 = g_orbis_ee_stall_n; p5 = g_orbis_hwdraw_n;
      }
#ifdef ORBIS_VULKAN
      {
        // vk-285-36: where the GS thread waited on the Vulkan side this second (VKOrbisTiming.h,
        // g_orbis_vkw_*): a command buffer's fence before reuse, an explicit wait after a
        // submission, a fence counter (stream buffers, texture copies), the swapchain acquire,
        // vkQueueSubmit, vkQueuePresentKHR and vkDeviceWaitIdle; milliseconds (calls).
        static unsigned long long prev_ns[ORBIS_VKW_KINDS], prev_n[ORBIS_VKW_KINDS];
        double ms[ORBIS_VKW_KINDS], max_ms[ORBIS_VKW_KINDS];
        unsigned long long calls[ORBIS_VKW_KINDS];
        for (int k = 0; k < ORBIS_VKW_KINDS; k++)
        {
          const unsigned long long ns = g_orbis_vkw_ns[k], n = g_orbis_vkw_n[k];
          ms[k] = (ns - prev_ns[k]) / 1e6;
          calls[k] = n - prev_n[k];
          prev_ns[k] = ns;
          prev_n[k] = n;
          max_ms[k] = g_orbis_vkw_max_ns[k] / 1e6;
          g_orbis_vkw_max_ns[k] = 0;
        }
        printf("[vkwait] reuse=%.1fms(%llu) sync=%.1fms(%llu) counter=%.1fms(%llu) acquire=%.1fms(%llu) "
               "submit=%.1fms(%llu) present=%.1fms(%llu) idle=%.1fms(%llu)\n",
               ms[0], calls[0], ms[1], calls[1], ms[2], calls[2], ms[3], calls[3], ms[4], calls[4], ms[5], calls[5],
               ms[6], calls[6]);
        // vk-285-38: new shaders this second, only when there were any -- pipelines created
        // (the driver's cache lookups, and its compiles and stores on a miss: its own
        // "[ps5vk] shader timing" lines split those) and GLSL compiled to SPIR-V; the longest
        // single one of each is the hitch it made.
        if (calls[7] || calls[8])
          printf("[shaders] pipelines=%llu %.1fms (max %.1fms) | glsl=%llu %.1fms (max %.1fms)\n",
                 calls[7], ms[7], max_ms[7], calls[8], ms[8], max_ms[8]);
      }
      {
        // vk-285-39: GSDeviceVK::CopyRect's copies this second (VKOrbisTiming.h), only when there
        // were any: the ones made as convert draws, then the image copies left, which the driver
        // runs on the CPU, by kind; count (MB).
        static unsigned long long prev_n[ORBIS_COPY_KINDS], prev_bytes[ORBIS_COPY_KINDS];
        unsigned long long n[ORBIS_COPY_KINDS];
        double mb[ORBIS_COPY_KINDS];
        bool any = false;
        for (int k = 0; k < ORBIS_COPY_KINDS; k++)
        {
          n[k] = g_orbis_copy_n[k] - prev_n[k];
          mb[k] = (g_orbis_copy_bytes[k] - prev_bytes[k]) / 1048576.0;
          prev_n[k] = g_orbis_copy_n[k];
          prev_bytes[k] = g_orbis_copy_bytes[k];
          any = any || n[k] != 0;
        }
        if (any)
          printf("[copies] draw=%llu(%.0fMB) | image rt>rt=%llu(%.0fMB) rt>tex=%llu(%.0fMB) ds>ds=%llu(%.0fMB) "
                 "ds>tex=%llu(%.0fMB) other=%llu(%.0fMB)\n",
                 n[ORBIS_COPY_DRAW], mb[ORBIS_COPY_DRAW], n[ORBIS_COPY_RT_RT], mb[ORBIS_COPY_RT_RT],
                 n[ORBIS_COPY_RT_TEX], mb[ORBIS_COPY_RT_TEX], n[ORBIS_COPY_DS_DS], mb[ORBIS_COPY_DS_DS],
                 n[ORBIS_COPY_DS_TEX], mb[ORBIS_COPY_DS_TEX], n[ORBIS_COPY_OTHER], mb[ORBIS_COPY_OTHER]);
      }
      {
        // vk-285-113: the GS's readbacks this second, only when there were any: copies recorded (MB), and the GS thread's
        // waits for them -- how many, their total and the longest (GSDownloadTextureVK, VKOrbisTiming.h).
        static unsigned long long prev_n, prev_bytes, prev_wait_ns, prev_wait_n;
        const unsigned long long n = g_orbis_readback_n - prev_n, wn = g_orbis_readback_wait_n - prev_wait_n;
        const double mb = (g_orbis_readback_bytes - prev_bytes) / 1048576.0, wms = (g_orbis_readback_wait_ns - prev_wait_ns) / 1e6;
        const double max_ms = g_orbis_readback_wait_max_ns / 1e6;
        prev_n = g_orbis_readback_n;
        prev_bytes = g_orbis_readback_bytes;
        prev_wait_ns = g_orbis_readback_wait_ns;
        prev_wait_n = g_orbis_readback_wait_n;
        g_orbis_readback_wait_max_ns = 0;
        if (n || wn)
          printf("[readbacks] copies=%llu(%.1fMB) waits=%llu %.1fms (max %.1fms) mode=%d\n", n, mb, wn, wms, max_ms,
                 static_cast<int>(EmuConfig.GS.HWDownloadMode));
      }
#endif
      printf("[boot] t+%ds state=%d eepc=%08x eecy=%llu ioppc=%08x iopcy=%llu rec=%d/%d\n", i + 1,
        (int)VMManager::GetState(), cpuRegs.pc, (unsigned long long)cpuRegs.cycle,
        psxRegs.pc, (unsigned long long)psxRegs.cycle,
        (int)EmuConfig.Cpu.Recompiler.EnableEE, (int)EmuConfig.Cpu.Recompiler.EnableIOP);
      fflush(stdout);
      g_orbis_ticker_step.store(4, std::memory_order_relaxed);
      const TickClock::time_point t_overlay = TickClock::now();
      ps5::debug::set_line(4, "t+%ds st=%d ee=%08x/%llu io=%08x rec=%d%d", i + 1, (int)VMManager::GetState(),
        cpuRegs.pc, (unsigned long long)cpuRegs.cycle, psxRegs.pc,
        (int)EmuConfig.Cpu.Recompiler.EnableEE, (int)EmuConfig.Cpu.Recompiler.EnableIOP);
      const TickClock::time_point t_end = TickClock::now();
      const int cpu_end = sceKernelGetCurrentCpu();
      // vk-285-108: a pass whose sleep overran by 250 ms or whose work took 250 ms, step by step.
      const double sleep_ms = ms_between(t_sleep, t_flags), work_ms = ms_between(t_flags, t_end);
      if (sleep_ms > 1250.0 || work_ms > 250.0)
      {
        printf("[ticker] slow pass %d: sleep %.0f flags %.0f drain %.0f lines %.0f overlay %.0f ms (cpu %d -> %d)\n",
          i + 1, sleep_ms, ms_between(t_flags, t_drain), ms_between(t_drain, t_lines), ms_between(t_lines, t_overlay),
          ms_between(t_overlay, t_end), cpu_start, cpu_end);
        fflush(stdout);
      }
      g_orbis_ticker_cpu.store(cpu_end, std::memory_order_relaxed);
      g_orbis_ticker_beat.store(__builtin_ia32_rdtsc(), std::memory_order_release);
    }
  });
  // vk-285-108: this (the CPU) thread may already be pinned to the EE's CPU (pin=3 applies during the first
  // Execute), and a new thread starts with its creator's CPUs: the ticker goes to the layout's helper CPUs
  // (GSRenderer.cpp OrbisHelperThreadAdd), and the GS thread's watchdog starts from now.
  g_orbis_ticker_thread = ticker.native_handle();
  OrbisHelperThreadAdd(ticker.native_handle());
  g_orbis_ticker_beat.store(__builtin_ia32_rdtsc(), std::memory_order_release);
  ticker.detach();

  {
    auto t0 = std::chrono::steady_clock::now();
    unsigned exits = 0;
    (void)t0;
    // Orbis: re-enter forever. Execute() returns on every JIT exit (game reset, new ELF, pause);
    // stopping after a fixed window froze the game on New Game / load.
    for (;;)
    {
      const VMState st = VMManager::GetState();
      if (st == VMState::Running)
      {
        VMManager::Execute();
        exits++;
        if (exits < 200 || (exits % 1000) == 0)
          printf("[boot] re-enter #%u state=%d eepc=%08x eecy=%llu\n", exits,
            (int)VMManager::GetState(), cpuRegs.pc, (unsigned long long)cpuRegs.cycle);
        continue;
      }
      if (st == VMState::Stopping && g_orbis_menu_request.load())
      {
        orbis_back_to_menu(); // returns only when the re-exec failed, with the VM running again
        continue;
      }
      if (st == VMState::Shutdown || st == VMState::Stopping)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10)); // paused / resetting
    }
    printf("[boot] execute loop done exits=%u\n", exits);
    fflush(stdout);
  }

  printf("[boot] smoke test window done - keeping emulator running\n");
  fflush(stdout);
  sys_notify(fe::Tr(fe::Str::NotifyStopped));
  ps5::debug::set_line(2, "GAME RUNNING");
  ps5::debug::set_line(3, "keep-alive (no shutdown)");
  // Keep the VM running: VMManager::Shutdown would stop the emulation.
  for (;;)
  {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    printf("[boot] running - still alive\n");
    fflush(stdout);
  }
}
