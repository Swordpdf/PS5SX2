// PS5 port frontend: the shelf on a PC, for looking at it without a console. It runs fe::App and fe::Renderer
// on the machine's Vulkan (SwiftShader from Chromium does), drives them with a script of controller presses and
// writes the frames it is told to as PNG files. Nothing of this goes into the eboot.
//
//   build-host.sh && ./fe_host --data <folder> --out <folder> [--size 1920x1080] [--lang <ps5 language id>] [--ime]
//   [--no-bios "<what was found instead>"] (vk-285-134) [--ime-text "<what the keyboard types>"] (vk-285-135: with --ime)
//   [--open-bios] (bioscheck: with --no-bios, the build ships the open BIOS: the BIOS question offers it, and once gs.ini says
//   PS5SX2/OpenBios=true the stand-in finds a BIOS) [--bios-pending <path>] (a tile's or a disc's start refused before the shelf came up)
//   Steps: "outside <path>" (a start asked of the shelf from outside it: Play from disc, a finished copy; expect outsideok, approved).
//   [--games <file>] [--build-tag <text>] [--record <out.mp4> [--record-fps 30|60]] (2026-10-08: the showcase video)
//                               [--script <file>] [step ...]
// --games: one game a line, "title|region|serial|size in MB" (the shelf's order); its cover is <data>/covers/<serial>.jpg.
// --record: every frame the steps run (at --record-fps, default 30) goes to ffmpeg, which writes out.mp4 (H.264, near
// lossless); "shot" steps still write their PNGs. "rec off" / "rec on" steps leave frames out (they still run).
// --virtual-clock: the texture packs' clock is the shelf's frames and --texpacks-rate counts in it (a recorded download).
// --places "Label=/path|...": the folder picker's places (a video shows the console's own: /data/PCSX2, /mnt/usb0).
// --tiles: a game's sheet gets the Home screen tile row (third selectable row), answered by a stand-in for the PS5SX2 Disc Helper in <data>/tiles (FakeHelper: steps "helper ok|slow|
//          stuck|mute|none|stale|fail <reason>|clear|seed ...", "tilepollsreset"; expect tilephase, tilerow, tilefocus, tilepolls, tilepending, tilereq, tilefile, tileentries).
//          --own-tile <path>: this run stands for a game tile of that game (its own tile is not taken off from inside it).
// --record-audio <out.wav>: the shelf's own sounds (fe_sound's key taps) for the frames recorded, 48 kHz stereo, in step
// with --record's video; without --record nothing is drawn (a quick pass for a clip recorded before).
//
// <folder> for --data is a stand-in for /data/PCSX2: settings/, gs.ini, patches/, memcards/ (made when missing).
// Steps (from --script, one a line, and/or the command line, in that order):
//   wait <s>               the shelf runs <s> seconds with no button down
//   press <button> [n]     the button goes down for a frame and up for a frame, n times (default 1)
//   hold <button> <s>      the button stays down for <s> seconds
//   shot <name>            <out>/<name>.png of the current frame
//   game <n>               (before any other step) the shelf starts on game n
// Buttons: left right up down cross circle square triangle options l1 r1 l2 r2; "a+b" presses them together.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../fe_app.h"
#include "../fe_games.h"
#include "../fe_i18n.h"
#include "../fe_discsettings.h" // gamedb (2026-10-30)

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <chrono>
#include <memory>
#include <ctime>
#include <mutex>
#include <thread>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace fe;

namespace
{
// installer-dl: the stand-in release's PS5SX2Installer.elf (--updates-no-installer)
std::vector<uint8_t> HostInstallerElf()
{
	std::vector<uint8_t> b(8000, 0x5a);
	b[0] = 0x7f;
	b[1] = 'E';
	b[2] = 'L';
	b[3] = 'F';
	return b;
}
std::string HostInstallerSha()
{
	const std::vector<uint8_t> b = HostInstallerElf();
	return Sha256Hex(b.data(), b.size());
}

bool ReadAll(const std::string& path, std::vector<uint8_t>& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

// A PNG writer (zlib's compress2 for the IDAT, one filter byte of 0 a row).
void PutBe32(std::vector<uint8_t>& v, uint32_t x)
{
	v.push_back(static_cast<uint8_t>(x >> 24));
	v.push_back(static_cast<uint8_t>(x >> 16));
	v.push_back(static_cast<uint8_t>(x >> 8));
	v.push_back(static_cast<uint8_t>(x));
}

void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data)
{
	PutBe32(png, static_cast<uint32_t>(data.size()));
	const size_t start = png.size();
	png.insert(png.end(), type, type + 4);
	png.insert(png.end(), data.begin(), data.end());
	PutBe32(png, static_cast<uint32_t>(crc32(0, png.data() + start, static_cast<uInt>(png.size() - start))));
}

bool WritePng(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h)
{
	std::vector<uint8_t> raw;
	raw.reserve(static_cast<size_t>(w * 3 + 1) * h);
	for (uint32_t y = 0; y < h; y++)
	{
		raw.push_back(0);
		const uint8_t* row = rgba.data() + static_cast<size_t>(y) * w * 4;
		for (uint32_t x = 0; x < w; x++)
			raw.insert(raw.end(), row + x * 4, row + x * 4 + 3); // no alpha: the display ignores it
	}
	uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
	std::vector<uint8_t> z(zlen);
	if (compress2(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
		return false;
	z.resize(zlen);
	std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	std::vector<uint8_t> ihdr;
	PutBe32(ihdr, w);
	PutBe32(ihdr, h);
	ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
	Chunk(png, "IHDR", ihdr);
	Chunk(png, "IDAT", z);
	Chunk(png, "IEND", {});
	std::ofstream f(path, std::ios::binary);
	f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
	return static_cast<bool>(f);
}

struct Gpu
{
	void* lib = nullptr;
	Vk vk;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	uint32_t qf = 0;
	VkQueue queue = VK_NULL_HANDLE;
	std::vector<VkImage> images;
	std::vector<VkDeviceMemory> memory;

	bool Init(uint32_t w, uint32_t h)
	{
		lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
		if (!lib)
			return Fail("no libvulkan.so.1");
		auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
		const char* missing = nullptr;
		if (!gipa || !vk.LoadGlobal(gipa, &missing))
			return Fail(missing ? missing : "vkGetInstanceProcAddr");
		VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
		ai.pApplicationName = "fe_host";
		ai.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ici.pApplicationInfo = &ai;
		if (vk.vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS || !vk.LoadInstance(instance, false, &missing))
			return Fail("instance");
		uint32_t n = 1;
		if (vk.vkEnumeratePhysicalDevices(instance, &n, &pd) < 0 || n == 0)
			return Fail("no physical device (set VK_ICD_FILENAMES to SwiftShader's json)");
		VkPhysicalDeviceProperties props;
		vk.vkGetPhysicalDeviceProperties(pd, &props);
		std::printf("[host] device: %s\n", props.deviceName);
		uint32_t nq = 0;
		vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
		std::vector<VkQueueFamilyProperties> qs(nq);
		vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qs.data());
		for (qf = 0; qf < nq && !(qs[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT); qf++)
			;
		if (qf == nq)
			return Fail("no graphics queue");
		const float prio = 1.0f;
		VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qci.queueFamilyIndex = qf;
		qci.queueCount = 1;
		qci.pQueuePriorities = &prio;
		VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		if (vk.vkCreateDevice(pd, &dci, nullptr, &device) != VK_SUCCESS || !vk.LoadDevice(device, false, &missing))
			return Fail("device");
		vk.vkGetDeviceQueue(device, qf, 0, &queue);

		// Two display images, as the console's swapchain has.
		VkPhysicalDeviceMemoryProperties mp;
		vk.vkGetPhysicalDeviceMemoryProperties(pd, &mp);
		for (int i = 0; i < 2; i++)
		{
			VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			ci.imageType = VK_IMAGE_TYPE_2D;
			ci.format = VK_FORMAT_B8G8R8A8_UNORM;
			ci.extent = {w, h, 1};
			ci.mipLevels = ci.arrayLayers = 1;
			ci.samples = VK_SAMPLE_COUNT_1_BIT;
			ci.tiling = VK_IMAGE_TILING_OPTIMAL;
			ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			VkImage img;
			if (vk.vkCreateImage(device, &ci, nullptr, &img) != VK_SUCCESS)
				return Fail("image");
			VkMemoryRequirements req;
			vk.vkGetImageMemoryRequirements(device, img, &req);
			uint32_t type = 0;
			while (type < mp.memoryTypeCount &&
				   !((req.memoryTypeBits & (1u << type)) && (mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
				type++;
			VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			mai.allocationSize = req.size;
			mai.memoryTypeIndex = type;
			VkDeviceMemory mem;
			if (type == mp.memoryTypeCount || vk.vkAllocateMemory(device, &mai, nullptr, &mem) != VK_SUCCESS)
				return Fail("image memory");
			vk.vkBindImageMemory(device, img, mem, 0);
			images.push_back(img);
			memory.push_back(mem);
		}
		return true;
	}

	bool Fail(const char* what)
	{
		std::fprintf(stderr, "[host] Vulkan: %s\n", what);
		return false;
	}
};

struct Step
{
	std::string op, arg, text;
	double seconds = 0;
	int count = 1;
};

bool SetButton(Input& in, const std::string& b, bool down)
{
	// "l1+square": a chord, every button down (and up) in the same frame (2026-10-05, the account panel's chord).
	if (const size_t plus = b.find('+'); plus != std::string::npos)
		return SetButton(in, b.substr(0, plus), down) && SetButton(in, b.substr(plus + 1), down);
	bool* p = b == "left" ? &in.left : b == "right" ? &in.right : b == "up" ? &in.up : b == "down" ? &in.down :
	          b == "cross" ? &in.cross : b == "circle" ? &in.circle : b == "square" ? &in.square :
	          b == "triangle" ? &in.triangle : b == "options" ? &in.options : b == "l1" ? &in.l1 : b == "r1" ? &in.r1 :
	          b == "l2" ? &in.l2 : b == "r2" ? &in.r2 : b == "r3" ? &in.r3 : nullptr; // vk-285-116; 2.11: the right stick's click
	if (!p)
		return false;
	*p = down;
	return true;
}

bool ParseStep(const std::string& line, std::vector<Step>& out)
{
	std::istringstream s(line);
	Step st;
	if (!(s >> st.op) || st.op[0] == '#')
		return true;
	if (st.op == "wait")
		s >> st.seconds;
	else if (st.op == "press")
	{
		s >> st.arg;
		if (!(s >> st.count))
			st.count = 1;
	}
	else if (st.op == "hold")
		s >> st.arg >> st.seconds;
	else if (st.op == "down" || st.op == "up") // pr9n: a button stays down until its "up" (chords pressed one after the other)
		s >> st.arg;
	else if (st.op == "expect") // pr9n: "expect selected=4", "account=1", "sheet=0", "tab=2"; a mismatch fails the run (vk-285-137: "shelf=4"; 2.11: also "flip100>=95", "flip100<=12")
		s >> st.arg;
	else if (st.op == "flipreset") // 2.11: forget the fastest the case has turned so far ("expect flipspeed100<=270" is the fastest since the last one of these)
	{
	}
	else if (st.op == "expectwhile") // R3: "expectwhile <condition> <must> [<must> ...]": frame by frame while the condition holds; the others must hold at every one of them
	{
		s >> st.arg;
		std::getline(s, st.text);
		while (!st.text.empty() && st.text.front() == ' ')
			st.text.erase(0, 1);
	}
	else if (st.op == "sleep") // 2026-10-05: real seconds, for the texture pack worker
		s >> st.seconds;
	else if (st.op == "expectsheetfile") // gamedb (2026-10-30): "expectsheetfile settings/<stem>.ini": the file the open sheet writes
	{
		std::getline(s, st.arg);
		while (!st.arg.empty() && st.arg.front() == ' ')
			st.arg.erase(0, 1);
	}
	else if (st.op == "shot" || st.op == "game" || st.op == "lastdisc") // fix180: "lastdisc <serial>": the last game played was that disc from the drive
		s >> st.arg;
	else if (st.op == "scan") // chdscan: "scan <done>/<total>": the shelf came up while the scan runs, this far; "scan done": the games come (App::SetGames)
		s >> st.arg;
	else if (st.op == "dump") // live-22: "dump <pct>" writes the dumper's progress file, "dump off" removes it
	s >> st.arg;
	else if (st.op == "drive") // 2.11: "drive <serial> <title>" puts a PS2 disc in the drive (the watcher's report), "drive off" takes it out
	{
	s >> st.arg;
	std::getline(s, st.text);
	while (!st.text.empty() && st.text.front() == ' ')
	st.text.erase(0, 1);
	}
	else if (st.op == "helper") // 2.12 (--tiles): "helper ok|slow|stuck|mute|none|stale|fail <reason>|clear|seed <path>|<tile>|<serial>|<title>|<status>": the stand-in helper
	{
		s >> st.arg;
		std::getline(s, st.text);
		while (!st.text.empty() && st.text.front() == ' ')
			st.text.erase(0, 1);
	}
	else if (st.op == "tilepollsreset") // 2.12: zero the count of looks at the helper's state ("expect tilepolls=3")
	{
	}
	else if (st.op == "rec") // 2026-10-08: "rec on" / "rec off": what --record keeps (from the start when no step says)
		s >> st.arg;
	else if (st.op == "stick") // 2.11: "stick <x> <y>": the right stick, held there until the next "stick"
		s >> st.arg >> st.seconds;
	else if (st.op == "outside") // bioscheck: "outside <path>": a start asked of the shelf from outside it (the rest of the line, spaces kept)
	{
		std::getline(s, st.arg);
		while (!st.arg.empty() && st.arg.front() == ' ')
			st.arg.erase(0, 1);
	}
	else if (st.op == "remote") // 2.11: "remote <text>": the settings page's search box sets the shelf's search ("remote" alone clears it)
	{
		std::getline(s, st.arg);
		while (!st.arg.empty() && st.arg.front() == ' ')
			st.arg.erase(0, 1);
	}
	else
	{
		std::fprintf(stderr, "[host] unknown step: %s\n", line.c_str());
		return false;
	}
	out.push_back(st);
	return true;
}

void MakeDir(const std::string& p)
{
	mkdir(p.c_str(), 0755);
}

bool Exists(const std::string& p)
{
	struct stat st;
	return stat(p.c_str(), &st) == 0;
}

void WriteText(const std::string& path, const std::string& text)
{
	std::ofstream f(path, std::ios::binary);
	f << text;
}

// The stand-in for /data/PCSX2 when it is empty: gs.ini as the Recommended button leaves it, two memory cards and a
// patch file with two groups for the first game (the codes are dummies: nothing runs them here).
void SeedData(const std::string& data, const std::string& presets_text)
{
	MakeDir(data);
	for (const char* d : {"/settings", "/patches", "/memcards"})
		MakeDir(data + d);
	if (!Exists(data + "/gs.ini"))
	{
		std::string section;
		settings::PresetSection(presets_text, "@global", section);
		WriteText(data + "/gs.ini", "# All games\n" + section);
	}
	for (const char* card : {"/memcards/Mcd001.ps2", "/memcards/Mcd002.ps2"})
		if (!Exists(data + card))
		{
			FILE* f = std::fopen((data + card).c_str(), "wb");
			if (f)
			{
				std::fseek(f, static_cast<long>(settings::kCardMb * 8 - 1), SEEK_SET);
				std::fputc(0, f);
				std::fclose(f);
			}
		}
	if (!Exists(data + "/patches/SCUS-97399_2F123FD8.pnach"))
		WriteText(data + "/patches/SCUS-97399_2F123FD8.pnach",
			"gametitle=God of War (US)\n\n[Widescreen 16:9]\ndescription=Renders the game in 16:9 (placeholder for the preview)\n"
			"patch=1,EE,00100000,word,00000000\n\n[60 FPS]\ndescription=Runs the game at 60 fps (placeholder for the preview)\n"
			"patch=1,EE,00100004,word,00000000\n");
}

GameInfo Game(const char* title, const char* region, const char* serial, uint64_t mb)
{
	GameInfo g;
	g.title = title;
	g.region = region;
	g.serial = serial;
	g.stem = std::string(title) + " (" + region + ")";
	g.file = g.stem + ".iso";
	g.path = "/data/PCSX2/games/" + g.file;
	g.bytes = mb << 20;
	return g;
}

// 2.12 (--tiles): a stand-in for the PS5SX2 Disc Helper's game tile code (tile6/gametile.c), ticked every frame in the shelf's own (virtual) time. It reads and writes the
// files of plans/gametiles_contract.md with a tiny splitter of its own -- NOT fe_gametile.h -- so a bug in the shelf's writer or reader is not hidden by the same bug here:
// alive.txt every 20 s, state.txt (no header, one line per tile, then the failures, whole file by tmp + rename), ONE request-*.txt a second (name order; the file is
// removed, its lines go to <data>/helper-took.txt), a tile `installing` at once and `ok` 4 s later, `removing` at once and gone 1.5 s later. Modes (the "helper" step):
//   ok      as above                                 slow   installing and removing for ever (the console: until PS5SX2 is closed)
//   stuck   takes the files, writes nothing          mute   alive, but never takes a file
//   none    not running (no alive.txt)               stale  not running, alive.txt 2 minutes old
//   fail <reason>  takes the files and answers `-<TAB>path...<TAB>failed:<reason>` for an add
struct FakeHelper
{
	enum class Mode
	{
		Ok,
		Slow,
		Stuck,
		Mute,
		None,
		Stale,
		Fail,
	};
	struct Tile
	{
		std::string id, path, serial, title, status;
		double at = 0;
	};
	std::string dir, took_file;
	Mode mode = Mode::Ok;
	std::string fail_reason = "nogame";
	double now = 0, next_tick = 1.0, next_alive = 20.0;
	int took = 0; // request files taken
	std::vector<Tile> tiles, deferred; // deferred: an add that waits for the removal of the same game
	std::vector<std::string> failures; // state lines
	bool dirty = false;

	static std::vector<std::string> Split(const std::string& s, char sep)
	{
		std::vector<std::string> out;
		size_t at = 0;
		for (;;)
		{
			const size_t p = s.find(sep, at);
			if (p == std::string::npos)
			{
				out.push_back(s.substr(at));
				return out;
			}
			out.push_back(s.substr(at, p - at));
			at = p + 1;
		}
	}
	static std::string Norm(const std::string& p)
	{
		std::string o;
		for (char c : p)
			if (!(c == '/' && !o.empty() && o.back() == '/'))
				o += c;
		while (o.size() > 1 && o.back() == '/')
			o.pop_back();
		return o;
	}
	static bool IsId(const std::string& s)
	{
		return s.size() == 9 && s.compare(0, 7, "PPSA992") == 0 && s[7] >= '5' && s[7] <= '6' && std::isdigit(static_cast<unsigned char>(s[8]));
	}
	void Atomic(const char* name, const std::string& text)
	{
		const std::string tmp = dir + "/" + name + ".tmp";
		WriteText(tmp, text);
		std::rename(tmp.c_str(), (dir + "/" + name).c_str());
	}
	void Alive() { Atomic("alive.txt", std::to_string(static_cast<long long>(std::time(nullptr))) + " " + std::to_string(static_cast<int>(getpid())) + " gametile 1\n"); }
	void Flush()
	{
		std::string text;
		for (const Tile& t : tiles)
			text += t.id + "\t" + t.path + "\t" + t.serial + "\t" + t.title + "\t" + t.status + "\n";
		for (const std::string& f : failures)
			text += f + "\n";
		Atomic("state.txt", text);
		dirty = false;
	}
	void Start()
	{
		Alive();
		Flush();
	}
	void Command(const std::string& what, const std::string& rest)
	{
		if (what == "seed") // "<path>|<tile>|<serial>|<title>|<status>"
		{
			const std::vector<std::string> f = Split(rest, '|');
			if (f.size() >= 5)
			{
				if (f[4].compare(0, 7, "failed:") == 0)
					failures.push_back("-\t" + Norm(f[0]) + "\t" + f[2] + "\t" + f[3] + "\t" + f[4]);
				else
					tiles.push_back({f[1], Norm(f[0]), f[2], f[3], f[4], now});
				Flush();
			}
			return;
		}
		if (what == "clear")
		{
			tiles.clear();
			deferred.clear();
			failures.clear();
			Flush();
			return;
		}
		if (what == "ok" || what == "slow" || what == "stuck" || what == "mute" || what == "fail")
		{
			mode = what == "ok" ? Mode::Ok : what == "slow" ? Mode::Slow : what == "stuck" ? Mode::Stuck : what == "mute" ? Mode::Mute : Mode::Fail;
			if (what == "fail")
				fail_reason = rest.empty() ? "nogame" : rest;
			Alive();
			next_alive = now + 20;
		}
		else if (what == "none")
		{
			mode = Mode::None;
			std::remove((dir + "/alive.txt").c_str());
		}
		else if (what == "stale")
		{
			mode = Mode::Stale;
			Alive();
			const struct timeval old[2] = {{static_cast<time_t>(std::time(nullptr) - 120), 0}, {static_cast<time_t>(std::time(nullptr) - 120), 0}};
			utimes((dir + "/alive.txt").c_str(), old);
		}
		else
			std::fprintf(stderr, "[host] unknown helper mode: %s\n", what.c_str());
	}
	std::vector<std::string> RequestNames() const
	{
		std::vector<std::string> names;
		if (DIR* d = opendir(dir.c_str()))
		{
			while (const dirent* e = readdir(d))
			{
				const std::string n = e->d_name;
				if (n.compare(0, 8, "request-") == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".txt") == 0)
					names.push_back(n);
			}
			closedir(d);
		}
		std::sort(names.begin(), names.end());
		return names;
	}
	int RequestFiles() const { return static_cast<int>(RequestNames().size()); }
	int StateLines() const
	{
		std::ifstream f(dir + "/state.txt");
		int n = 0;
		for (std::string l; std::getline(f, l);)
			n += l.empty() ? 0 : 1;
		return n;
	}
	Tile* Find(const std::string& key)
	{
		for (Tile& t : tiles)
			if (t.id == key || t.path == key)
				return &t;
		return nullptr;
	}
	void Add(const std::string& path_in, const std::string& serial, const std::string& title)
	{
		const std::string path = Norm(path_in);
		for (auto it = failures.begin(); it != failures.end();) // the old failure of this game goes when its request is read
			it = Split(*it, '\t').size() > 1 && Split(*it, '\t')[1] == path ? failures.erase(it) : it + 1;
		dirty = true;
		if (mode == Mode::Fail)
		{
			failures.push_back("-\t" + path + "\t" + serial + "\t" + title + "\tfailed:" + fail_reason);
			return;
		}
		if (Tile* t = Find(path))
		{
			if (t->status == "removing")
				deferred.push_back({"", path, serial, title, "installing", now});
			return; // a second add of a game that has its tile: nothing
		}
		Build(path, serial, title);
	}
	void Build(const std::string& path, const std::string& serial, const std::string& title)
	{
		for (int s = 51; s <= 60; s++)
		{
			const std::string id = "PPSA992" + std::to_string(s);
			if (!Find(id))
			{
				tiles.push_back({id, path, serial, title, "installing", now});
				std::stable_sort(tiles.begin(), tiles.end(), [](const Tile& a, const Tile& b) { return a.id < b.id; });
				dirty = true;
				return;
			}
		}
		failures.push_back("-\t" + path + "\t" + serial + "\t" + title + "\tfailed:limit");
		dirty = true;
	}
	void Take()
	{
		const std::vector<std::string> names = RequestNames();
		if (names.empty())
			return;
		const std::string file = dir + "/" + names[0];
		std::ifstream in(file, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		in.close();
		std::remove(file.c_str());
		took++;
		if (mode == Mode::Stuck)
			return;
		std::vector<std::string> lines = Split(ss.str(), '\n');
		bool first = true;
		for (std::string l : lines)
		{
			if (!l.empty() && l.back() == '\r')
				l.pop_back();
			if (l.empty())
				continue;
			if (first)
			{
				first = false;
				if (l != "v1")
					return; // not a request of this contract: dropped unread
				continue;
			}
			{
				std::ofstream log(took_file, std::ios::app | std::ios::binary);
				log << l << "\n";
			}
			const std::vector<std::string> f = Split(l, '\t');
			if (f[0] == "add" && f.size() >= 4)
				Add(f[1], f[2], f[3]);
			else if (f[0] == "remove" && f.size() >= 2)
			{
				if (Tile* t = Find(IsId(f[1]) ? f[1] : Norm(f[1])))
					if (t->status != "removing")
					{
						t->status = "removing";
						t->at = now;
						dirty = true;
					}
			}
		}
	}
	void Tick(double dt)
	{
		now += dt;
		if (mode == Mode::None || mode == Mode::Stale)
			return;
		if (now >= next_alive)
		{
			Alive();
			next_alive = now + 20;
		}
		if (now < next_tick)
			return;
		next_tick += 1.0;
		for (size_t i = 0; i < tiles.size();)
		{
			Tile& t = tiles[i];
			if (mode == Mode::Ok && t.status == "installing" && now - t.at >= 4.0)
			{
				t.status = "ok";
				dirty = true;
			}
			if (mode == Mode::Ok && t.status == "removing" && now - t.at >= 1.5)
			{
				const std::string gone = t.path;
				tiles.erase(tiles.begin() + static_cast<std::ptrdiff_t>(i));
				dirty = true;
				for (size_t k = 0; k < deferred.size(); k++) // an add that waited for this removal
					if (deferred[k].path == gone)
					{
						Build(deferred[k].path, deferred[k].serial, deferred[k].title);
						deferred.erase(deferred.begin() + static_cast<std::ptrdiff_t>(k));
						break;
					}
				continue;
			}
			i++;
		}
		if (mode != Mode::Mute)
			Take();
		if (dirty)
			Flush();
	}
};
} // namespace

int main(int argc, char** argv)
{
	std::string data = "fe_host_data", out = "fe_host_shots", script, root;
	uint32_t w = 1920, h = 1080;
	int lang = 1; // English
	bool achievements_preview = false;
	bool ime = false;
	std::string ime_text = "PreviewPlayer"; // vk-285-135: what the stand-in keyboard types (not for a password)
	std::vector<Step> steps;
	std::string texpacks;      // 2026-10-05: a folder with metadata.json (archive.org's list) and pack files
	double texpacks_rate = 0;  // KB a second for the fake downloads (0: as fast as the disk)
	bool texpacks_fake = false; // fake downloads send zeros instead of reading the files (for the progress previews)
	// 2026-10-08 (the showcase video): --virtual-clock: the texture packs' clock is the shelf's frames (1/60 s each), and
	// --texpacks-rate counts in that time, so a recording shows a download at the rate given however long a frame takes to
	// draw here. At the end the clock jumps to real time so the manager's Stop can time out.
	bool virtual_clock = false;
	std::atomic<double> vclock{1000.0};
	std::atomic<bool> vclock_real{false};
	std::atomic<int> vabort{0};
	// A download in its request: 1 while it reads and stores bytes already due, 2 while it waits for the clock, else 0.
	// The shelf's clock waits out the 1s: the download keeps step with it (so a run with frames left out can't outpace it).
	std::atomic<int> vdownload{0};
	{
		const std::string self = argv[0];
		const size_t slash = self.find_last_of('/');
		root = (slash == std::string::npos ? std::string(".") : self.substr(0, slash)) + "/..";
	}
	std::string web;
	bool no_bios = false; // vk-285-134
	std::string no_bios_problem;
	std::string gamedb_missing; // gamedb (2026-10-30)
	bool open_bios = false;      // bioscheck
	std::string bios_pending;    // bioscheck
	std::string games_file, build_tag = "vk-285-114 (host)", record, record_audio; // 2026-10-08: the showcase video
	std::string places; // 2026-10-08: --places "Label=/path|Label=/path": the folder picker's places (else the data folder's)
	bool tiles = false; // 2.12: --tiles: the sheet's Home screen tile row, with the stand-in helper (FakeHelper) in <data>/tiles; --own-tile <path>: this run is a game tile's
	std::string own_tile;
	// 180: --updates <tag>: the sheet for all games' PS5SX2 group, with a stand-in for GitHub naming release <tag> ("fail" or "error"
	// as <tag>: no answer / HTTP 500) and a stand-in installer (--updates-start <rc>: what starting it answers, 0 started).
	std::string updates;
	int updates_start = 0;
	bool updates_no_installer = false;
	// b185: --reload-tile: the sheet for all games' "Reload PS2 Disc tile" switch, over <data>/flags/disctile_noreload (the helper's flag on the PS5).
	bool reload_tile = false;
	int record_fps = 30;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
		if (a == "--data")
			data = next();
		else if (a == "--out")
			out = next();
		else if (a == "--script")
			script = next();
		else if (a == "--root")
			root = next();
		else if (a == "--web") // vk-285-118: the settings page's address, so the shelf shows its QR code
			web = next();
		else if (a == "--lang")
			lang = std::atoi(next().c_str());
		else if (a == "--achievements-preview")
			achievements_preview = true;
		else if (a == "--ime") // 2026-10-05: a stand-in for the PS5's keyboard
			ime = true;
		else if (a == "--ime-text" && i + 1 < argc) // vk-285-135
		{
			ime = true;
			ime_text = argv[++i];
		}
		else if (a == "--texpacks") // 2026-10-05: texture packs from a folder standing in for archive.org
			texpacks = next();
		else if (a == "--texpacks-rate")
			texpacks_rate = std::atof(next().c_str());
		else if (a == "--texpacks-fake")
			texpacks_fake = true;
		else if (a == "--virtual-clock")
			virtual_clock = true;
		else if (a == "--no-bios") // vk-285-134: no PS2 BIOS, and what was found instead (the shelf's line, picks refused)
		{
			no_bios = true;
			no_bios_problem = next();
		}
		else if (a == "--open-bios") // bioscheck
			open_bios = true;
		else if (a == "--no-gamedb") // gamedb (2026-10-30): resources/GameIndex.yaml is missing (the shelf's note names this file)
			gamedb_missing = next();
		else if (a == "--bios-pending") // bioscheck
			bios_pending = next();
		else if (a == "--games")
			games_file = next();
		else if (a == "--build-tag")
			build_tag = next();
		else if (a == "--record")
			record = next();
		else if (a == "--places")
			places = next();
		else if (a == "--tiles")
			tiles = true;
		else if (a == "--own-tile")
			own_tile = next();
		else if (a == "--updates")
			updates = next();
		else if (a == "--updates-start")
			updates_start = std::atoi(next().c_str());
		else if (a == "--updates-no-installer") // installer-dl: the app has no installer; the stand-in release has one, fetched in 0.6 s
			updates_no_installer = true;
		else if (a == "--reload-tile") // b185
			reload_tile = true;
		else if (a == "--record-audio")
			record_audio = next();
		else if (a == "--record-fps")
			record_fps = std::atoi(next().c_str()) == 60 ? 60 : 30;
		else if (a == "--size")
		{
			const std::string s = next();
			if (std::sscanf(s.c_str(), "%ux%u", &w, &h) != 2)
				return 2;
		}
		else if (!ParseStep(a, steps))
			return 2;
	}
	if (!script.empty())
	{
		std::ifstream f(script);
		std::string line;
		std::vector<Step> from_file;
		while (std::getline(f, line))
			if (!ParseStep(line, from_file))
				return 2;
		steps.insert(steps.begin(), from_file.begin(), from_file.end());
	}
	MakeDir(out);

	std::vector<uint8_t> text_font, icon_font, brand_font, presets;
	const std::string pcsx2 = root + "/../..";
	if (!ReadAll(pcsx2 + "/bin/resources/fonts/Roboto-Regular.ttf", text_font) ||
		!ReadAll(pcsx2 + "/bin/resources/fonts/promptfont.otf", icon_font) ||
		!ReadAll(root + "/assets/fonts/fa-brands-400.otf", brand_font) || !ReadAll(root + "/assets/presets.ini", presets))
	{
		std::fprintf(stderr, "[host] fonts or presets.ini not found under %s (--root is ps5/frontend)\n", root.c_str());
		return 1;
	}
	const std::string presets_text(presets.begin(), presets.end());
	SeedData(data, presets_text);
	SetLanguage(lang, "");
	SetGameDbFile(pcsx2 + "/bin/resources/GameIndex.yaml"); // 2026-10-08: the games' own hardware fixes (the Hardware fixes rows)

	Gpu gpu;
	if (!gpu.Init(w, h))
		return 1;
	Fonts* fonts = new Fonts();
	Renderer renderer;
	if (!fonts->Init(text_font.data(), text_font.size(), icon_font.data(), icon_font.size(), brand_font.data(), brand_font.size()) ||
		!renderer.Init(&gpu.vk, gpu.pd, gpu.device, gpu.qf, gpu.queue, w, h, VK_FORMAT_B8G8R8A8_UNORM, gpu.images,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))
	{
		std::fprintf(stderr, "[host] renderer: %s\n", renderer.error().c_str());
		return 1;
	}

	std::vector<GameInfo> games = {
		Game("God of War", "USA", "SCUS-97399", 3712),
		Game("Ratchet & Clank", "Europe", "SCES-50916", 3350),
		Game("Gran Turismo 4", "USA", "SCUS-97328", 7680),
		Game("Castlevania - Lament of Innocence", "USA", "SLUS-20733", 2860),
		Game("The Lord of the Rings - The Two Towers", "USA", "SLUS-20578", 2410),
	};
	if (!games_file.empty()) // 2026-10-08: the showcase's library
	{
		std::ifstream f(games_file);
		std::vector<GameInfo> list;
		for (std::string line; std::getline(f, line);)
		{
			if (line.empty() || line[0] == '#')
				continue;
			std::string field[4];
			size_t at = 0;
			for (int k = 0; k < 4; k++)
			{
				const size_t bar = line.find('|', at);
				field[k] = line.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
				at = bar == std::string::npos ? line.size() : bar + 1;
			}
			list.push_back(Game(field[0].c_str(), field[1].c_str(), field[2].c_str(), std::strtoull(field[3].c_str(), nullptr, 10)));
		}
		if (list.empty())
		{
			std::fprintf(stderr, "[host] no games in %s\n", games_file.c_str());
			return 2;
		}
		games = std::move(list);
	}
	OptionsPaths op;
	op.settings_dir = data + "/settings";
	op.gs_ini = data + "/gs.ini";
	op.patches_dir = data + "/patches";
	op.memcards_dir = data + "/memcards";
	op.presets = presets_text;
	op.log = [](const std::string& line) { std::printf("[settings.log] %s\n", line.c_str()); };
	for (GameInfo& g : games)
		ReadBadges(g, op.settings_dir, op.gs_ini, op.patches_dir);

	CoverService* covers = new CoverService();
	CoverConfig cc;
	cc.manual_dir = data + "/covers";
	cc.cache_dir = data + "/cache-covers";
	cc.allow_download = false;
	// chdscan: with a "scan" step the shelf comes up without its games, as fe_ps5.cpp's does while images not seen before are read; the
	// cover service starts with the games ("scan done").
	const bool scan_mode = std::any_of(steps.begin(), steps.end(), [](const Step& st) { return st.op == "scan"; });
	bool scanning = scan_mode;
	int scan_done = 0, scan_total = 0;
	const auto no_download = [](const std::string&, std::vector<uint8_t>&) { return -1; };
	if (!scan_mode)
		covers->Start(games, fonts, cc, no_download);

	App app;
	AppConfig acfg;
	AchievementAccountState preview_account;
	GameAchievementsState preview_game;
	preview_account.available = true;
	// 2026-10-05: --ime stands in for the PS5's keyboard: it opens, and the next poll returns a made-up name or password.
	std::string ime_pending;
	if (ime)
	{
		acfg.text_entry.open = [&](const std::string& title, const std::string&, bool password, unsigned) {
			ime_pending = password ? "hunter2-preview" : ime_text;
			std::printf("[host] the PS5's keyboard (stand-in) for %s\n", title.c_str());
			return true;
		};
		acfg.text_entry.poll = [&](std::string& text) {
			text = ime_pending;
			return 1;
		};
	}
	if (achievements_preview)
	{
		// A local UI fixture. It never authenticates or stores the typed credentials.
		acfg.achievements = {[&] { return preview_account; },
			[&](const std::string& username, const std::string&) {
				preview_account.username = username;
				preview_account.saved = preview_account.authenticated = true;
				return true;
			},
			[&] { preview_account = {}; }};
		acfg.game_achievements = {[&] { return preview_game; }, [&](const std::string& path) {
            preview_game.path = path;
            preview_game.title = "Achievement browser preview";
            ++preview_game.revision;
            preview_game.entries.clear();
            auto image = std::make_shared<std::vector<uint8_t>>();
            std::ifstream badge(data + "/badge.png", std::ios::binary);
            image->assign(std::istreambuf_iterator<char>(badge), std::istreambuf_iterator<char>());
            // 2026-10-08 (the showcase video): <data>/achievements.txt, "title|description|points|1 when earned|badge file", if there.
            std::ifstream list(data + "/achievements.txt");
            for (std::string line; std::getline(list, line);) {
                if (line.compare(0, 6, "title=") == 0) { // the game's name over the list
                    preview_game.title = line.substr(6);
                    continue;
                }
                std::string f[5];
                size_t at = 0;
                for (int k = 0; k < 5; k++) {
                    const size_t bar = line.find('|', at);
                    f[k] = line.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
                    at = bar == std::string::npos ? line.size() : bar + 1;
                }
                if (f[0].empty())
                    continue;
                GameAchievement entry; entry.id = static_cast<uint32_t>(preview_game.entries.size() + 1);
                entry.points = static_cast<uint32_t>(std::atoi(f[2].c_str()));
                entry.unlocked = f[3] == "1";
                entry.title = f[0];
                entry.description = f[1];
                auto own = std::make_shared<std::vector<uint8_t>>();
                std::ifstream b(data + "/" + f[4], std::ios::binary);
                own->assign(std::istreambuf_iterator<char>(b), std::istreambuf_iterator<char>());
                entry.image = own->empty() ? image : own;
                preview_game.entries.push_back(std::move(entry));
            }
            for (int i = 0; preview_game.entries.empty() && i < 24; ++i) {
                GameAchievement entry; entry.id = i+1; entry.points = 5 + i*5;
                entry.unlocked = i < 7;
                entry.title = "Achievement " + std::to_string(i+1);
                entry.description = "Explore the game and complete this objective to earn the achievement.";
                entry.image = image;
                preview_game.entries.push_back(std::move(entry));
            }
            return true; }, [] {}};
	}
	// 2026-10-05: the texture packs, with a folder standing in for archive.org (its list and the pack files).
	TexturePackManager* packs = nullptr;
	if (!texpacks.empty())
	{
		TexturePackPlatform tp;
		tp.get_text = [texpacks](const std::string& url, std::string& body) {
			if (url != TexturePackMetadataUrl())
				return 404;
			std::ifstream f(texpacks + "/metadata.json", std::ios::binary);
			std::stringstream ss;
			ss << f.rdbuf();
			body = ss.str();
			return body.empty() ? -1 : 200;
		};
		tp.get_range = [texpacks, texpacks_rate, texpacks_fake, virtual_clock, &vclock, &vabort, &vdownload](const std::string& url,
						   uint64_t offset,
						   uint64_t length, const std::function<bool(const void*, size_t)>& sink) {
			const std::string prefix = std::string("https://archive.org/download/") + kTexturePackItem + "/";
			const std::string path = texpacks + "/" + PercentDecode(url.substr(prefix.size()));
			FILE* f = texpacks_fake ? nullptr : std::fopen(path.c_str(), "rb");
			if (!texpacks_fake && !f)
				return 404;
			if (f)
				std::fseek(f, static_cast<long>(offset), SEEK_SET);
			std::vector<char> buf(64 * 1024, 0);
			const double t0 = vclock.load();
			const int generation = vabort.load();
			uint64_t sent = 0;
			struct Busy
			{
				std::atomic<int>& state;
				~Busy() { state = 0; }
			} busy{vdownload};
			vdownload = virtual_clock && texpacks_rate > 0 ? 1 : 0;
			for (uint64_t left = length; left > 0;)
			{
				const size_t want = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
				const size_t n = f ? std::fread(buf.data(), 1, want, f) : want;
				if (n == 0)
					break;
				if (texpacks_rate > 0 && virtual_clock)
				{
					// These bytes are due once the shelf has run long enough for them at the rate (in its own time).
					const double due = t0 + static_cast<double>(sent + n) / (texpacks_rate * 1024.0);
					if (vclock.load() < due)
					{
						vdownload = 2;
						while (vclock.load() < due && vabort.load() == generation)
							std::this_thread::sleep_for(std::chrono::microseconds(200));
						vdownload = 1;
					}
					if (vabort.load() != generation)
					{
						if (f)
							std::fclose(f);
						return -2;
					}
				}
				else if (texpacks_rate > 0)
					std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(n / (texpacks_rate * 1024.0) * 1e6)));
				sent += n;
				if (!sink(buf.data(), n))
				{
					if (f)
						std::fclose(f);
					return -2;
				}
				left -= n;
			}
			if (f)
				std::fclose(f);
			return 206;
		};
		tp.free_bytes = [texpacks_fake](const std::string& dir) {
			if (texpacks_fake)
				return UINT64_MAX; // the previews' fake downloads stop long before the disk would matter
			struct statvfs v;
			return statvfs(dir.c_str(), &v) == 0 ? static_cast<uint64_t>(v.f_bavail) * v.f_frsize : UINT64_MAX;
		};
		if (virtual_clock)
		{
			tp.now = [&vclock, &vclock_real] {
				if (vclock_real.load())
					return 1e9 + std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
				return vclock.load();
			};
			tp.abort = [&vabort] { vabort++; };
		}
		tp.log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
		tp.notify = [](const std::string& game, bool ok, const std::string& detail) {
			std::printf("[host] popup: HD textures %s: %s (%s)\n", ok ? "ready" : "failed", game.c_str(), detail.c_str());
		};
		MakeDir(data + "/textures");
		packs = new TexturePackManager(tp, data + "/textures", data + "/textures/.ps5sx2-downloads", data + "/cache/texture-packs.json");
		packs->Start();
		acfg.texture_packs = packs->Service();
	}
	// 2026-10-08: the shelf's sounds for --record-audio, mixed a frame at a time while recording.
	Mixer* mixer = nullptr;
	if (!record_audio.empty())
	{
		mixer = new Mixer();
		mixer->Build();
		acfg.sound = mixer;
	}
	acfg.build_tag = build_tag;
	acfg.options = op;
	acfg.system_menu = true; // 2026-10-08: the sheet for all games' "PS2 system menu" row
	// 2.12: --tiles: the sheet's Home screen tile row, and the stand-in helper that answers it. Without --tiles there is no row (the counted presses of the other scripts stay).
	FakeHelper helper;
	if (tiles)
	{
		helper.dir = data + "/tiles";
		helper.took_file = data + "/helper-took.txt";
		MakeDir(helper.dir);
		std::remove(helper.took_file.c_str());
		helper.Start();
		acfg.tiles_dir = helper.dir;
		acfg.own_tile_game = own_tile;
		acfg.tile_cover = [data](const GameInfo& g) { // the host's covers: <data>/covers/<serial>.jpg
			const std::string c = data + "/covers/" + g.serial + ".jpg";
			return gametile::CoverFileOk(c) ? c : std::string();
		};
	}
	// 180: --updates: the PS5SX2 group, over a stand-in GitHub and installer (fe_update.h). The build tag must look like a release's
	// ("vk-285-180") for the comparison; files go to <data>/installer (its no-log-upload switch).
	std::unique_ptr<Updater> updater;
	int upd_started = 0, upd_quit = 0;
	if (!updates.empty())
	{
		MakeDir(data + "/installer");
		UpdatePlatform up;
		const std::string tag = updates;
		up.get_text = [tag](const std::string&, std::string& body, std::string& error) {
			if (tag == "fail")
			{
				error = "no connection: is the console online?";
				return -1;
			}
			if (tag == "error")
				return 500;
			const std::string name = "PS5SX2-" + tag + ".zip";
			body = "{\"tag_name\":\"" + tag + "\",\"name\":\"PS5SX2 9.99\",\"draft\":false,\"assets\":[{\"name\":\"" + name +
				"\",\"state\":\"uploaded\",\"size\":41000000,\"digest\":\"sha256:" + std::string(64, 'a') +
				"\",\"browser_download_url\":\"" + kUpdateDownloadPrefix + tag + "/" + name + "\"}" +
				// installer-dl: and its installer (HostInstallerElf: 8000 bytes)
				",{\"name\":\"PS5SX2Installer.elf\",\"state\":\"uploaded\",\"size\":8000,\"digest\":\"sha256:" + HostInstallerSha() +
				"\",\"browser_download_url\":\"" + kUpdateDownloadPrefix + tag + "/PS5SX2Installer.elf\"}]}";
			return 200;
		};
		up.start_installer = [&upd_started, updates_start](const std::string& expect, std::string& detail) {
			detail = "host stand-in for " + expect;
			if (updates_start == 0)
				upd_started++;
			return updates_start;
		};
		up.exists = [](const std::string& p) { return access(p.c_str(), F_OK) == 0; };
		up.set_file = [](const std::string& p, bool on) {
			if (!on)
				return std::remove(p.c_str()) == 0 || errno == ENOENT;
			FILE* f = std::fopen(p.c_str(), "w");
			if (f)
				std::fclose(f);
			return f != nullptr;
		};
		up.log = [](const std::string& line) { std::printf("[update] %s\n", line.c_str()); };
		up.quit = [&upd_quit] { upd_quit++; };
		// installer-dl: the installer goes to <data>/installer/PS5SX2Installer.elf
		up.installer_dest = data + "/installer/PS5SX2Installer.elf";
		up.installer_present = [updates_no_installer, dest = up.installer_dest] { return !updates_no_installer || access(dest.c_str(), F_OK) == 0; };
		up.get_file = [](const std::string&, uint64_t, std::vector<uint8_t>& out, std::string&) {
			std::this_thread::sleep_for(std::chrono::milliseconds(600));
			out = HostInstallerElf();
			return 200;
		};
		up.write_file = [](const std::string& path, const std::vector<uint8_t>& bytes, std::string& error) {
			FILE* f = std::fopen(path.c_str(), "wb");
			if (!f)
			{
				error = "can't create " + path;
				return false;
			}
			std::fwrite(bytes.data(), 1, bytes.size(), f);
			std::fclose(f);
			return true;
		};
		up.rename_file = [](const std::string& from, const std::string& to) { return std::rename(from.c_str(), to.c_str()) == 0; };
		updater = std::make_unique<Updater>(up, build_tag, data + "/installer");
		acfg.updates = updater->Service();
	}
	// b185: --reload-tile: the switch writes the flag file as fe_ps5.cpp does on the PS5 (there: /data/PCSX2/flags/disctile_noreload).
	const std::string noreload = data + "/flags/disctile_noreload";
	if (reload_tile)
	{
		acfg.reload_tile_on = [noreload] { return access(noreload.c_str(), F_OK) != 0; };
		acfg.set_reload_tile = [noreload, data](bool on) {
			if (on)
				return unlink(noreload.c_str()) == 0 || errno == ENOENT;
			mkdir((data + "/flags").c_str(), 0777);
			FILE* f = std::fopen(noreload.c_str(), "wb");
			if (f)
				std::fclose(f);
			return f != nullptr;
		};
	}
	// 2.11: the settings page's search box (the "remote" step)
	std::mutex remote_mutex;
	std::string remote_text;
	bool remote_pending = false;
	acfg.search_remote = [&remote_mutex, &remote_text, &remote_pending](std::string& text) {
		std::lock_guard<std::mutex> lock(remote_mutex);
		if (!remote_pending)
			return false;
		remote_pending = false;
		text = remote_text;
		return true;
	};
	// vk-285-135: the folder picker's places: the data folder (as /data/PCSX2) and two "drives" in it, when they are folders.
	acfg.folder_places = {{"PS5SX2's folder", data}, {"USB drive 1", data + "/usb0"}, {"Extended storage", data + "/ext0"}};
	if (!places.empty())
	{
		acfg.folder_places.clear();
		for (size_t at = 0; at <= places.size();)
		{
			const size_t bar = std::min(places.find('|', at), places.size());
			const std::string one = places.substr(at, bar - at);
			const size_t eq = one.find('=');
			if (eq != std::string::npos)
				acfg.folder_places.push_back({one.substr(0, eq), one.substr(eq + 1)});
			at = bar + 1;
		}
	}
	// SMB share passwords: "<url>\t<password>" lines in <data>/smb-passwords.txt (the console's OrbisSmb has its own file).
	const std::string smb_passwords = data + "/smb-passwords.txt";
	auto smb_lines = [smb_passwords] {
		std::vector<std::string> lines;
		std::ifstream in(smb_passwords);
		for (std::string line; std::getline(in, line);)
			lines.push_back(line);
		return lines;
	};
	acfg.smb_set_password = [smb_passwords, smb_lines](const std::string& url, const std::string& password, std::string& error) {
		if (url.find('@') == std::string::npos)
		{
			error = "a password needs a user name";
			return false;
		}
		std::ofstream out(smb_passwords, std::ios::trunc);
		for (const std::string& line : smb_lines())
			if (line.compare(0, url.size() + 1, url + "\t") != 0)
				out << line << "\n";
		if (!password.empty())
			out << url << "\t" << password << "\n";
		return true;
	};
	acfg.smb_has_password = [smb_lines](const std::string& url) {
		for (const std::string& line : smb_lines())
			if (line.compare(0, url.size() + 1, url + "\t") == 0)
				return true;
		return false;
	};
	// bioscheck: the stand-in for main-boot.cpp's orbis_bios_usable: no BIOS of the player's own; with --open-bios, the open one once
	// gs.ini's [PS5SX2] says OpenBios = true (the BIOS question writes it, the sheet's switch turns it off).
	int bios_looks = 0;
	auto open_chosen = [gs = op.gs_ini]() {
		std::string text;
		if (!settings::ReadFile(gs, text))
			return false;
		for (const settings::IniLine& l : settings::ParseIni(text))
			if (settings::CanonKey(l.key) == settings::CanonKey("PS5SX2/OpenBios"))
				return settings::Lower(settings::Trim(l.value)) == "true";
		return false;
	};
	if (no_bios)
	{
		acfg.bios_present = [&bios_looks, open_bios, open_chosen] {
			bios_looks++;
			return open_bios && open_chosen();
		};
		acfg.bios_problem = [no_bios_problem, open_bios, open_chosen] { return open_bios && open_chosen() ? std::string() : no_bios_problem; };
		acfg.bios_dir = "/data/PCSX2/bios";
		acfg.open_bios_bundled = open_bios;
		acfg.bios_open_in_use = [open_bios, open_chosen] { return open_bios && open_chosen(); };
		acfg.bios_pending_launch = bios_pending;
	}
	int outside_ok = -1;
	std::string approved;
	acfg.refresh_game = [op](GameInfo& g) {
		g.badges.clear();
		ReadBadges(g, op.settings_dir, op.gs_ini, op.patches_dir);
	};
	// live-22: the disc dumper's progress file (ProsperoDiscDump.cpp on the console); the "dump" step writes it.
	const std::string dump_progress = data + "/disc-dump-progress.txt";
	acfg.disc_dump_progress = dump_progress;
	// 2.11: the disc in the drive (not copied): the "drive" step sets it; picking the entry counts as a request to be asked again.
	std::string drive_serial, drive_title;
	int reasks = 0;
	acfg.drive_disc_poll = [&](std::string& title, std::string& serial) {
	if (drive_serial.empty())
	return false;
	title = drive_title;
	serial = drive_serial;
	return true;
	};
	acfg.drive_disc_reask = [&] { reasks++; };
	if (scan_mode)
		acfg.scan_progress = [&](int& done, int& total) {
			done = scan_done;
			total = scan_total;
			return scanning;
		};
	acfg.gamedb_missing = gamedb_missing; // gamedb (2026-10-30)
	// gamedb (2026-10-30): the disc entries' settings file, by the console's rule (fe_discsettings.h) with the drive's title as the copy's name
	acfg.disc_settings_stem = [&, settings_dir = op.settings_dir](const std::string& serial) {
		if (serial.size() < 5)
			return std::string(); // a serial unfit for a file name (the console: OrbisDiscDrive::SafeSerial)
		const std::string by_copy = (serial == drive_serial && !drive_title.empty() ? drive_title : std::string("PS2 disc")) + " (" + serial + ")";
		return fe::DiscSettingsStem(serial, by_copy, [&](const std::string& stem) { return access((settings_dir + "/" + stem + ".ini").c_str(), F_OK) == 0; });
	};
	for (const Step& s : steps)
		if (s.op == "game")
			acfg.preselect = std::atoi(s.arg.c_str());
		else if (s.op == "lastdisc")
			acfg.preselect_disc_serial = s.arg;
	if (!app.Init(&renderer, fonts, scan_mode ? std::vector<GameInfo>() : games, covers, acfg))
	{
		std::fprintf(stderr, "[host] app: %s\n", renderer.error().c_str());
		return 1;
	}
	if (!web.empty())
	{
		std::string shown = web.substr(web.find("://") == std::string::npos ? 0 : web.find("://") + 3);
		shown = shown.substr(0, shown.find('/'));
		app.SetWebUrl(web, shown);
	}

	const double dt = 1.0 / 60.0;
	Input in;
	FrameDesc frame;
	uint32_t index = 0;
	// 2026-10-08: --record pipes every frame (every other one at 30 fps) to ffmpeg.
	FILE* rec = nullptr;
	unsigned rec_step = 0, rec_frames = 0;
	bool rec_on = std::none_of(steps.begin(), steps.end(), [](const Step& st) { return st.op == "rec"; });
	std::vector<int16_t> audio; // interleaved stereo at SoundBank::kRate
	std::vector<float> mix(static_cast<size_t>(SoundBank::kRate / 60) * 2);
	if (!record.empty())
	{
		const std::string cmd = "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s " + std::to_string(w) + "x" + std::to_string(h) +
		                        " -r " + std::to_string(record_fps) + " -i - -c:v libx264 -preset slow -crf 10 -pix_fmt yuv420p '" + record + "'";
		rec = popen(cmd.c_str(), "w");
		if (!rec)
		{
			std::fprintf(stderr, "[host] could not start ffmpeg for %s\n", record.c_str());
			return 1;
		}
	}
	// 2.11: how fast the picked case has turned over, at most, since the last "flipreset" (flips a second; the turning is capped, App kFlipMaxSpeed).
	// (The pose repeats every two flips: angles are counted as the pose they make, -1 .. 1, and a change of the angle as the shorter way between two poses.)
	// flip_top / flip_low: how far over it has been at most / at least, 0 .. 1, since the last "flipreset".
	const auto pose = [](float angle) { return angle - 2.0f * std::round(angle / 2.0f); };
	float flip_prev = 0.0f, flip_peak = 0.0f, flip_top = 0.0f, flip_low = 1.0f;
	int held_peak = 0, count_peak = 0; // the most backheld / backcount have been since the last "flipreset"
	// R3: what "expect" and "expectwhile" read: the shelf's state by name. backat<N>: game N holds a real back cover (1/0, -1 off the shelf); turn<N>: how far
	// the flip turns game N's case now (degrees); setat<N>: its case has a texture set, so it is drawn (1/0); backheld: the cases that are not the picked
	// one that still hold their back cover; backcount: all the cases (off the shelf too) that hold one; heldpeak / countpeak: the most of either since flipreset.
	auto value_of = [&](const std::string& key) -> int {
		return key.compare(0, 6, "backat") == 0 ? app.BackRealOf(std::atoi(key.c_str() + 6)) :
		       key.compare(0, 4, "turn") == 0 ? app.TurnOf(std::atoi(key.c_str() + 4)) :
		       key.compare(0, 5, "setat") == 0 ? app.SetOf(std::atoi(key.c_str() + 5)) :
		       key == "backheld" ? app.BackHeld() :
		       key == "backcount" ? app.BackRealCount() :
		       key == "heldpeak" ? held_peak :
		       key == "scanning" ? (scanning ? 1 : 0) : // chdscan: the shelf is waiting for the scan's games
		       key == "countpeak" ? count_peak :
		       key == "selected" ? app.Chosen() : key == "account" ? app.AccountOpen() : key == "sheet" ? app.SheetOpen() :
			                key == "tab"      ? app.SheetTab() :
			                key == "done"     ? app.Done() : // 2026-10-08: the shelf closed (a game, or the PS2 system menu)
			                key == "systemmenu" ? app.SystemMenuChosen() :
			                key == "picker"   ? app.PickerOpen() : // vk-285-135
			                key == "shelf"    ? app.ShelfCount() : // vk-285-137: the games on the shelf
			                key == "typing"   ? app.SearchTyping() : // 2.11: the search keyboard is up
				                key == "flip100"  ? static_cast<int>(std::lround(app.FlipAmount() * 100.0f)) : // 2.11: the case turned over, in percent
				                key == "searchlen" ? static_cast<int>(app.SearchText().size()) :
				                key == "dumppct"  ? app.DumpPercent() : // live-22: the disc dump entry's percentage (-1 when none)
				                				                key == "driveentry" ? app.DriveEntry() : // 2.11: the disc in the drive is listed
				                				                key == "discart"  ? app.DiscArt() : // 2.11: the disc art arrived
				                				                key == "reasks"   ? reasks : // 2.11: how often the drive entry was picked
				                key == "updstate" ? (updater ? static_cast<int>(updater->Status().state) : -1) : // 180: fe_update.h UpdateStatus::State
				                key == "updstarted" ? upd_started : // 180: installers started
				                key == "updquit" ? upd_quit :       // 180: the app asked to close
				                key == "updinstaller" ? (access((data + "/installer/PS5SX2Installer.elf").c_str(), F_OK) == 0 ? 1 : 0) : // installer-dl: fetched
				                key == "logupload" ? (updater ? (updater->LogUpload() ? 1 : 0) : -1) : // 180: session logs on
				                key == "reloadtile" ? (reload_tile ? (access(noreload.c_str(), F_OK) != 0 ? 1 : 0) : -1) : // b185: the Reload tile kept (no flag file)
				                key == "reloadfocus" ? (app.ReloadTileFocused() ? 1 : 0) : // b185: the focused row is the Reload tile switch
				                key == "tilephase" ? app.TilePhase() : // 2.12: the Home screen tile row's phase (fe_gametile.h; -1: no row on the sheet)
				                key == "tilerow"   ? app.TileRowShown() : // 2.12: the sheet has the row
				                key == "tilefocus" ? app.TileFocused() : // 2.12: the focused row is it
				                key == "tilepolls" ? app.TilePolls() : // 2.12: looks at the helper's state since the last tilepollsreset
				                key == "tilepending" ? app.TilePending() : // 2.12: asks still followed closely
				                key == "tilereq"   ? helper.took : // 2.12: request files the stand-in helper took
				                key == "tilefile"  ? helper.RequestFiles() : // 2.12: request files in the folder now
				                key == "tileentries" ? helper.StateLines() : // 2.12: lines in state.txt
				                key == "backreal" ? app.BackReal() : // 2.11: the picked case's real back cover has arrived
				                key == "ringcount" ? app.RingCount() : // 2.11: how many neighbours' backs were handed over to be fetched ahead
				                key == "ringfirst" ? app.RingFirst() : // 2.11: the first of them (an index of the host's game list)
				                key == "flipangle100" ? static_cast<int>(std::lround(pose(app.FlipAngle()) * 100.0f)) : // 2.11: the same with its side (-100 .. 100)
				                key == "flippeak100" ? static_cast<int>(std::lround(flip_top * 100.0f)) : // 2.11: the furthest over it got since flipreset, in percent
				                key == "flipbottom100" ? static_cast<int>(std::lround(flip_low * 100.0f)) : // 2.11: the nearest to the front it got since flipreset
				                key == "flipspeed100" ? static_cast<int>(std::lround(flip_peak * 100.0f)) :
				key == "biosask"   ? (app.BiosChoiceOpen() ? 1 : 0) : // bioscheck: the BIOS question is up
				key == "biosrow"   ? app.BiosChoiceRow() :            // bioscheck: its focused card (-1: not up)
				key == "biosmissing" ? (app.BiosMissing() ? 1 : 0) :  // bioscheck
				key == "gamedbnote" ? (app.GameDbNoteShown() ? 1 : 0) : // gamedb (2026-10-30): the shelf's note about the missing database
				key == "bioslooks" ? bios_looks :                     // bioscheck: how often the stand-in was asked
				key == "openchosen" ? (open_chosen() ? 1 : 0) :       // bioscheck: gs.ini PS5SX2/OpenBios=true
				key == "outsideok" ? outside_ok :                     // bioscheck: the last "outside" step was taken (1), refused (0)
				key == "approved"  ? (approved += app.TakeApprovedLaunch(), approved.empty() ? 0 : 1) :     // bioscheck: an outside start the BIOS question let go
				-99;
	};
	// Splits "key>=N" into its parts; false when it does not parse.
	auto split_expect = [&](const std::string& arg, std::string& key, char& cmp, int& want) {
		const size_t eq = arg.find_first_of("=<>");
		if (eq == std::string::npos)
			return false;
		key = arg.substr(0, eq);
		cmp = '=';
		size_t num = eq + 1;
		if (arg[eq] != '=')
		{
			cmp = arg[eq];
			num = eq + 1 < arg.size() && arg[eq + 1] == '=' ? eq + 2 : std::string::npos;
			if (num == std::string::npos)
				return false;
		}
		want = std::atoi(arg.c_str() + num);
		return true;
	};
	auto holds = [&](int got, char cmp, int want) { return cmp == '=' ? got == want : cmp == '>' ? got >= want : got <= want; };
	// Update and Build every frame, as the console's loop does (the sheet's scroll target is worked out in Build).
	auto run = [&](double seconds) {
		for (double t = 0; t < seconds - 1e-9; t += dt)
		{
			if (virtual_clock)
			{
				while (vdownload.load() == 1)
					std::this_thread::sleep_for(std::chrono::microseconds(200));
				vclock.store(vclock.load() + dt);
			}
			app.Update(dt, in);
			if (tiles)
				helper.Tick(dt); // 2.12
			held_peak = std::max(held_peak, app.BackHeld());
			count_peak = std::max(count_peak, app.BackRealCount());
			flip_peak = std::max(flip_peak, static_cast<float>(std::fabs(pose(app.FlipAngle() - flip_prev)) / dt));
			flip_prev = app.FlipAngle();
			flip_top = std::max(flip_top, std::fabs(pose(flip_prev)));
			flip_low = std::min(flip_low, std::fabs(pose(flip_prev)));
			app.Build(frame, "21:47");
			if (mixer)
			{
				// Every frame's 1/60 s of sound, kept while recording (the voices play on either way).
				mixer->Mix(mix.data(), SoundBank::kRate / 60);
				if (rec_on)
					for (float x : mix)
						audio.push_back(static_cast<int16_t>(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f)));
			}
			if (rec && rec_on && (rec_step++ % (60 / record_fps)) == 0)
			{
				std::vector<uint8_t> rgba;
				if (renderer.Render(frame, index, VK_NULL_HANDLE, VK_NULL_HANDLE) && renderer.ReadPixels(index, rgba) &&
					rgba.size() == static_cast<size_t>(w) * h * 4)
				{
					std::fwrite(rgba.data(), 1, rgba.size(), rec);
					rec_frames++;
				}
				index ^= 1;
			}
		}
	};
	int failures = 0;
	for (const Step& s : steps)
	{
		if (s.op == "wait")
			run(s.seconds);
		else if (s.op == "press")
			for (int n = 0; n < s.count; n++)
			{
				if (!SetButton(in, s.arg, true))
				{
					std::fprintf(stderr, "[host] unknown button %s\n", s.arg.c_str());
					return 2;
				}
				run(dt);
				SetButton(in, s.arg, false);
				run(dt * 2);
			}
		else if (s.op == "stick")
		{
			in.rx = static_cast<float>(std::atof(s.arg.c_str()));
			in.ry = static_cast<float>(s.seconds);
			run(dt);
		}
		else if (s.op == "remote")
		{
			{
				std::lock_guard<std::mutex> lock(remote_mutex);
				remote_text = s.arg;
				remote_pending = true;
			}
			run(dt * 2);
		}
		else if (s.op == "rec")
		{
			rec_on = s.arg == "on";
			rec_step = 0;
			if (rec)
				std::printf("[host] rec %s at frame %u\n", rec_on ? "on" : "off", rec_frames); // where the cuts are
		}
		else if (s.op == "helper") // 2.12: the stand-in helper's mode or state (see FakeHelper)
		{
			if (!tiles)
			{
				std::fprintf(stderr, "[host] the helper step needs --tiles\n");
				return 2;
			}
			helper.Command(s.arg, s.text);
			run(dt);
		}
		else if (s.op == "tilepollsreset") // 2.12
			app.ResetTilePolls();
		else if (s.op == "dump") // live-22: stand in for the console's disc dumper writing its progress file
		{
			if (s.arg == "off")
				std::remove(dump_progress.c_str());
			else
				WriteText(dump_progress, "SLUS-00000 " + s.arg + "\nHost Dump Game\n");
			run(dt); // let the shelf poll it (PollDiscDump is throttled to ~0.5 s)
		}
		else if (s.op == "scan") // chdscan
		{
			if (s.arg == "done" && scanning)
			{
				scanning = false;
				covers->Start(games, fonts, cc, no_download);
				app.SetGames(games, acfg.preselect);
			}
			else
				std::sscanf(s.arg.c_str(), "%d/%d", &scan_done, &scan_total);
			run(dt);
		}
		else if (s.op == "expectsheetfile") // gamedb (2026-10-30)
		{
			const std::string got = app.SheetOpen() ? app.SheetFileLabel() : std::string("(no sheet)");
			if (got != s.arg)
			{
				std::printf("[host] expect FAILED: the sheet's file is %s, wanted %s\n", got.c_str(), s.arg.c_str());
				failures++;
			}
			else
				std::printf("[host] expect ok: the sheet's file is %s\n", got.c_str());
		}
		else if (s.op == "drive") // 2.11: stand in for the disc watcher's report of the disc in the drive
		{
		if (s.arg == "off")
		drive_serial.clear();
		else
		{
		drive_serial = s.arg;
		drive_title = s.text;
		}
		run(dt);
		}
		else if (s.op == "outside") // bioscheck: a start asked of the shelf from outside it, as fe_ps5.cpp's loop asks
		{
			outside_ok = app.OfferOutsideLaunch(s.arg) ? 1 : 0;
			std::printf("[host] outside %s: %s\n", s.arg.c_str(), outside_ok ? "taken" : "not taken (no BIOS)");
			run(dt);
		}
		else if (s.op == "sleep")
		{
			const double until = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() + s.seconds;
			while (std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() < until)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				run(dt);
			}
		}
		else if (s.op == "flipreset") // 2.11
		{
			flip_peak = 0.0f;
			held_peak = app.BackHeld();
			count_peak = app.BackRealCount();
			flip_prev = app.FlipAngle();
			flip_top = flip_low = std::fabs(pose(flip_prev));
		}
		else if (s.op == "expectwhile") // R3: runs frame by frame while the condition holds (4 s at most), the must-hold line checked before every frame
		{
			struct Must
			{
				std::string key;
				char cmp = '=';
				int want = 0;
			};
			std::string ck, word;
			char cc = '=';
			int cw = 0;
			std::vector<Must> musts;
			std::istringstream list(s.text);
			bool parsed = split_expect(s.arg, ck, cc, cw) && value_of(ck) != -99;
			while (parsed && list >> word)
			{
				Must m;
				parsed = split_expect(word, m.key, m.cmp, m.want) && value_of(m.key) != -99;
				musts.push_back(m);
			}
			if (!parsed || musts.empty())
			{
				std::fprintf(stderr, "[host] unknown expectwhile: %s %s\n", s.arg.c_str(), s.text.c_str());
				return 2;
			}
			int frames = 0, bad = 0, first_bad = -1, got_bad = 0;
			std::string key_bad;
			while (frames < 240 && holds(value_of(ck), cc, cw))
			{
				for (const Must& m : musts)
				{
					const int got = value_of(m.key);
					if (!holds(got, m.cmp, m.want) && bad++ == 0)
					{
						first_bad = frames;
						got_bad = got;
						key_bad = m.key;
					}
				}
				run(dt);
				frames++;
			}
			if (frames < 3 || frames >= 240 || bad > 0)
			{
				std::printf("[host] expect FAILED: expectwhile %s -> %s: %d frames, %d bad (the first at frame %d: %s is %d)%s\n", s.arg.c_str(), s.text.c_str(), frames, bad,
					first_bad, key_bad.c_str(), got_bad, frames < 3 ? " (the condition hardly held)" : frames >= 240 ? " (it never ended)" : "");
				failures++;
			}
			else
				std::printf("[host] expect ok: expectwhile %s -> %s over %d frames\n", s.arg.c_str(), s.text.c_str(), frames);
		}
		else if (s.op == "expect")
		{
			// 2.11: "key=N" (as always), or "key>=N" / "key<=N"
			const size_t eq = s.arg.find_first_of("=<>");
			const std::string key = s.arg.substr(0, eq);
			char cmp = '=';
			size_t num = eq == std::string::npos ? eq : eq + 1;
			if (eq != std::string::npos && s.arg[eq] != '=')
			{
				cmp = s.arg[eq];
				num = eq + 1 < s.arg.size() && s.arg[eq + 1] == '=' ? eq + 2 : std::string::npos;
				if (num == std::string::npos)
					cmp = '?';
			}
			const int want = num == std::string::npos ? -1 : std::atoi(s.arg.c_str() + num);
			const int got = value_of(key);
			if (got == -99 || eq == std::string::npos || cmp == '?')
			{
				std::fprintf(stderr, "[host] unknown expect: %s\n", s.arg.c_str());
				return 2;
			}
			if (cmp == '=' ? got != want : cmp == '>' ? got < want : got > want)
			{
				std::printf("[host] expect FAILED: %s is %d, wanted %s%d\n", key.c_str(), got, cmp == '=' ? "" : cmp == '>' ? ">= " : "<= ", want);
				failures++;
			}
			else
				std::printf("[host] expect ok: %s%s%d (is %d)\n", key.c_str(), cmp == '=' ? "=" : cmp == '>' ? ">=" : "<=", want, got);
		}
		else if (s.op == "down" || s.op == "up")
		{
			if (!SetButton(in, s.arg, s.op == "down"))
			{
				std::fprintf(stderr, "[host] unknown button %s\n", s.arg.c_str());
				return 2;
			}
			run(dt);
		}
		else if (s.op == "hold")
		{
			SetButton(in, s.arg, true);
			run(s.seconds);
			SetButton(in, s.arg, false);
			run(dt);
		}
		else if (s.op == "shot")
		{
			app.Build(frame, "21:47");
			if (!renderer.Render(frame, index, VK_NULL_HANDLE, VK_NULL_HANDLE))
			{
				std::fprintf(stderr, "[host] render: %s\n", renderer.error().c_str());
				return 1;
			}
			// A second frame: textures made during the first (covers, the atlas) are uploaded by then.
			app.Build(frame, "21:47");
			renderer.Render(frame, index ^ 1, VK_NULL_HANDLE, VK_NULL_HANDLE);
			std::vector<uint8_t> rgba;
			const std::string path = out + "/" + s.arg + ".png";
			if (!renderer.ReadPixels(index ^ 1, rgba) || !WritePng(path, rgba, w, h))
			{
				std::fprintf(stderr, "[host] could not write %s\n", path.c_str());
				failures++;
			}
			else
				std::printf("[host] %s\n", path.c_str());
		}
	}
	if (rec)
	{
		const int rc = pclose(rec);
		std::printf("[host] recorded %u frames at %d fps to %s (ffmpeg %d)\n", rec_frames, record_fps, record.c_str(), rc);
		if (rc != 0)
			failures++;
	}
	if (mixer)
	{
		// A plain 16-bit PCM WAV.
		std::vector<uint8_t> wav;
		auto u32 = [&](uint32_t v) { for (int b = 0; b < 4; b++) wav.push_back(static_cast<uint8_t>(v >> (8 * b))); };
		auto u16 = [&](uint16_t v) { wav.push_back(static_cast<uint8_t>(v)); wav.push_back(static_cast<uint8_t>(v >> 8)); };
		const uint32_t bytes = static_cast<uint32_t>(audio.size() * 2);
		wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
		u32(36 + bytes);
		wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
		u32(16);
		u16(1);
		u16(2);
		u32(SoundBank::kRate);
		u32(SoundBank::kRate * 4);
		u16(4);
		u16(16);
		wav.insert(wav.end(), {'d', 'a', 't', 'a'});
		u32(bytes);
		const uint8_t* pcm = reinterpret_cast<const uint8_t*>(audio.data());
		wav.insert(wav.end(), pcm, pcm + bytes);
		std::ofstream f(record_audio, std::ios::binary);
		f.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
		std::printf("[host] recorded %.2f s of the shelf's sounds to %s\n", static_cast<double>(audio.size() / 2) / SoundBank::kRate,
			record_audio.c_str());
		if (!f)
			failures++;
	}
	covers->Stop();
	vclock_real = true;
	if (packs && packs->Stop(3000))
		delete packs;
	app.Shutdown();
	renderer.Shutdown();
	return failures ? 1 : 0;
}
