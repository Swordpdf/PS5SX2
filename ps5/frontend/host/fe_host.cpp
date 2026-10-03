// PS5 port frontend: the shelf on a PC, for looking at it without a console. It runs fe::App and fe::Renderer
// on the machine's Vulkan (SwiftShader from Chromium does), drives them with a script of controller presses and
// writes the frames it is told to as PNG files. Nothing of this goes into the eboot.
//
//   build-host.sh && ./fe_host --data <folder> --out <folder> [--size 1920x1080] [--lang <ps5 language id>]
//                               [--script <file>] [step ...]
//
// <folder> for --data is a stand-in for /data/PCSX2: settings/, gs.ini, patches/, memcards/ (made when missing).
// Steps (from --script, one a line, and/or the command line, in that order):
//   wait <s>               the shelf runs <s> seconds with no button down
//   press <button> [n]     the button goes down for a frame and up for a frame, n times (default 1)
//   hold <button> <s>      the button stays down for <s> seconds
//   shot <name>            <out>/<name>.png of the current frame
//   game <n>               (before any other step) the shelf starts on game n
// Buttons: left right up down cross circle square triangle options l1 r1 l2 r2.
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../fe_app.h"
#include "../fe_games.h"
#include "../fe_i18n.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <zlib.h>

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
	std::string op, arg;
	double seconds = 0;
	int count = 1;
};

bool SetButton(Input& in, const std::string& b, bool down)
{
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
	else if (st.op == "shot" || st.op == "game")
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
	std::vector<Step> steps;
	{
		const std::string self = argv[0];
		const size_t slash = self.find_last_of('/');
		root = (slash == std::string::npos ? std::string(".") : self.substr(0, slash)) + "/..";
	}
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
		else if (a == "--lang")
			lang = std::atoi(next().c_str());
		else if (a == "--achievements-preview")
			achievements_preview = true;
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
            for (int i = 0; i < 24; ++i) {
                GameAchievement entry; entry.id = i+1; entry.points = 5 + i*5;
                entry.unlocked = i < 7;
                entry.title = "Achievement " + std::to_string(i+1);
                entry.description = "Explore the game and complete this objective to earn the achievement.";
                entry.image = image;
                preview_game.entries.push_back(std::move(entry));
            }
            return true; }, [] {}};
	}
	acfg.build_tag = "vk-285-114 (host)";
	acfg.options = op;
	acfg.refresh_game = [op](GameInfo& g) {
		g.badges.clear();
		ReadBadges(g, op.settings_dir, op.gs_ini, op.patches_dir);
	};
	for (const Step& s : steps)
		if (s.op == "game")
			acfg.preselect = std::atoi(s.arg.c_str());
	if (!app.Init(&renderer, fonts, games, covers, acfg))
	{
		std::fprintf(stderr, "[host] app: %s\n", renderer.error().c_str());
		return 1;
	}

	const double dt = 1.0 / 60.0;
	Input in;
	FrameDesc frame;
	uint32_t index = 0;
	// Update and Build every frame, as the console's loop does (the sheet's scroll target is worked out in Build).
	auto run = [&](double seconds) {
		for (double t = 0; t < seconds - 1e-9; t += dt)
		{
			app.Update(dt, in);
			app.Build(frame, "21:47");
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
	covers->Stop();
	app.Shutdown();
	renderer.Shutdown();
	return failures ? 1 : 0;
}
