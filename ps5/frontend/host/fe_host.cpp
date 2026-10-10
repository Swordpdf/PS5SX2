// PS5 port frontend: the shelf on a PC, for looking at it without a console. It runs fe::App and fe::Renderer
// on the machine's Vulkan (SwiftShader from Chromium does), drives them with a script of controller presses and
// writes the frames it is told to as PNG files. Nothing of this goes into the eboot.
//
//   build-host.sh && ./fe_host --data <folder> --out <folder> [--size 1920x1080] [--lang <ps5 language id>] [--ime]
//   [--no-bios "<what was found instead>"] (vk-285-134) [--ime-text "<what the keyboard types>"] (vk-285-135: with --ime)
//   [--games <file>] [--build-tag <text>] [--record <out.mp4> [--record-fps 30|60]] (2026-10-08: the showcase video)
//                               [--script <file>] [step ...]
// --drive: a disc drive (/dev/cd1) at the front of the shelf, empty until a "drive" step puts a disc in it.
// --games: one game a line, "title|region|serial|size in MB" (the shelf's order); its cover is <data>/covers/<serial>.jpg.
// --record: every frame the steps run (at --record-fps, default 30) goes to ffmpeg, which writes out.mp4 (H.264, near
// lossless); "shot" steps still write their PNGs. "rec off" / "rec on" steps leave frames out (they still run).
// --virtual-clock: the texture packs' clock is the shelf's frames and --texpacks-rate counts in it (a recorded download).
// --places "Label=/path|...": the folder picker's places (a video shows the console's own: /data/PCSX2, /mnt/usb0).
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
//   drive <state> [s|t|r|mb] (with --drive) the disc drive's disc: "empty", "reading", "ready SLUS-20370|Kingdom
//                          Hearts|USA|2900" (serial|title|region|size in MB), or "other" (a disc that isn't a PS2 game)
// Buttons: left right up down cross circle square triangle options l1 r1 l2 r2; "a+b" presses them together.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../fe_app.h"
#include "../fe_games.h"
#include "../fe_i18n.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
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
	std::string op, arg, rest;
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
	          b == "l2" ? &in.l2 : b == "r2" ? &in.r2 : nullptr; // vk-285-116
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
	else if (st.op == "expect") // pr9n: "expect selected=4", "account=1", "sheet=0", "tab=2"; a mismatch fails the run (vk-285-137: "shelf=4")
		s >> st.arg;
	else if (st.op == "sleep") // 2026-10-05: real seconds, for the texture pack worker
		s >> st.seconds;
	else if (st.op == "shot" || st.op == "game")
		s >> st.arg;
	else if (st.op == "drive") // the disc drive's disc (--drive)
	{
		s >> st.arg;
		std::getline(s >> std::ws, st.rest);
	}
	else if (st.op == "rec") // 2026-10-08: "rec on" / "rec off": what --record keeps (from the start when no step says)
		s >> st.arg;
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
	bool drive = false; // --drive
	std::string games_file, build_tag = "vk-285-114 (host)", record, record_audio; // 2026-10-08: the showcase video
	std::string places; // 2026-10-08: --places "Label=/path|Label=/path": the folder picker's places (else the data folder's)
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
		else if (a == "--games")
			games_file = next();
		else if (a == "--drive")
			drive = true;
		else if (a == "--build-tag")
			build_tag = next();
		else if (a == "--record")
			record = next();
		else if (a == "--places")
			places = next();
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
	if (drive)
		games.insert(games.begin(), EmptyDrive("/dev/cd1"));
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
	covers->Start(games, fonts, cc, [](const std::string&, std::vector<uint8_t>&) { return -1; });

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
	if (no_bios)
	{
		acfg.bios_present = [] { return false; };
		acfg.bios_problem = [no_bios_problem] { return no_bios_problem; };
		acfg.bios_dir = "/data/PCSX2/bios";
	}
	acfg.refresh_game = [op](GameInfo& g) {
		g.badges.clear();
		ReadBadges(g, op.settings_dir, op.gs_ini, op.patches_dir);
	};
	// --drive: the disc a "drive" step put in the drive, handed to the shelf at its next frame with its pictures (as
	// fe_ps5.cpp's DriveWatch paints them).
	bool drive_pending = false;
	GameInfo drive_next;
	if (drive)
		acfg.drive_changed = [&](GameInfo& g, std::vector<CoverImage>& images) {
			if (!drive_pending)
				return false;
			drive_pending = false;
			g = drive_next;
			if (g.absent)
				return true;
			images.resize(2);
			CoverService::PaintSpine(*fonts, g, images[0]);
			GameInfo front = g;
			if (g.reading)
				front.title.clear(); // the spinning disc goes there
			CoverService::PaintPlaceholder(*fonts, front, images[1]);
			images[0].kind = CoverImage::Spine;
			images[1].kind = CoverImage::Placeholder;
			CoverFinder finder(cc.manual_dir, cc.cache_dir);
			const CoverFile f = finder.Best(g);
			std::ifstream in(f.path, std::ios::binary);
			const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			CoverImage cover;
			if (!f.path.empty() && CoverService::Decode(bytes, 1024, cover))
			{
				cover.kind = CoverImage::Cover;
				cover.source = f.source;
				images.push_back(std::move(cover));
			}
			return true;
		};
	for (const Step& s : steps)
		if (s.op == "game")
			acfg.preselect = std::atoi(s.arg.c_str());
	if (!app.Init(&renderer, fonts, games, covers, acfg))
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
		else if (s.op == "drive")
		{
			if (!drive)
			{
				std::fprintf(stderr, "[host] a drive step needs --drive\n");
				return 2;
			}
			if (s.arg == "empty" || s.arg == "other")
				drive_next = EmptyDrive("/dev/cd1");
			else if (s.arg == "reading")
				drive_next = ReadingDrive("/dev/cd1");
			else if (s.arg == "ready")
			{
				std::string field[4];
				size_t at = 0;
				for (int k = 0; k < 4; k++)
				{
					const size_t bar = s.rest.find('|', at);
					field[k] = s.rest.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
					at = bar == std::string::npos ? s.rest.size() : bar + 1;
				}
				drive_next = EmptyDrive("/dev/cd1");
				drive_next.absent = false;
				drive_next.serial = field[0];
				drive_next.stem = field[0];
				drive_next.title = field[1];
				drive_next.region = field[2].empty() ? RegionFromSerial(field[0]) : field[2];
				drive_next.bytes = std::strtoull(field[3].c_str(), nullptr, 10) << 20;
			}
			else
			{
				std::fprintf(stderr, "[host] unknown drive state: %s\n", s.arg.c_str());
				return 2;
			}
			drive_pending = true;
			run(dt);
		}
		else if (s.op == "rec")
		{
			rec_on = s.arg == "on";
			rec_step = 0;
			if (rec)
				std::printf("[host] rec %s at frame %u\n", rec_on ? "on" : "off", rec_frames); // where the cuts are
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
		else if (s.op == "expect")
		{
			const size_t eq = s.arg.find('=');
			const std::string key = s.arg.substr(0, eq);
			const int want = eq == std::string::npos ? -1 : std::atoi(s.arg.c_str() + eq + 1);
			const int got = key == "selected" ? app.Chosen() : key == "account" ? app.AccountOpen() : key == "sheet" ? app.SheetOpen() :
			                key == "tab"      ? app.SheetTab() :
			                key == "done"     ? app.Done() : // 2026-10-08: the shelf closed (a game, or the PS2 system menu)
			                key == "systemmenu" ? app.SystemMenuChosen() :
			                key == "picker"   ? app.PickerOpen() : // vk-285-135
			                key == "shelf"    ? app.ShelfCount() : -99; // vk-285-137: the games on the shelf
			if (got == -99 || eq == std::string::npos)
			{
				std::fprintf(stderr, "[host] unknown expect: %s\n", s.arg.c_str());
				return 2;
			}
			if (got != want)
			{
				std::printf("[host] expect FAILED: %s is %d, wanted %d\n", key.c_str(), got, want);
				failures++;
			}
			else
				std::printf("[host] expect ok: %s=%d\n", key.c_str(), got);
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
