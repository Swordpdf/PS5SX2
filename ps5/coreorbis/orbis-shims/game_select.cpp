// PS5 port (vk-285-30): the game selector.
//
// At start, before PCSX2 owns the display, the port lists the disc images in /data/PCSX2 (*.iso, *.chd)
// and lets the DualSense pick one: D-pad or left stick to move (held: repeats), L1/R1 a page,
// X or OPTIONS to start. It draws on the VideoOut OVERLAY bus with the boot overlay's CPU canvas
// (demo_renderer.cpp) and gives the bus and its memory back before the Vulkan device opens the
// MAIN bus. The game started last is kept in /data/PCSX2/lastgame.txt and preselected next time.
// One image: no menu. None: the caller's default. The nomenu flag: the last game, no menu.
// vk-285-33: the images are looked for in games/ and then in the top folder (older setups).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "demo_renderer.hpp"
#include "OrbisPaths.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" {
int scePadInit(void);
int scePadOpen(int32_t userId, int32_t type, int32_t index, const void* param);
int scePadClose(int32_t handle);
int scePadReadState(int32_t handle, void* data);
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(int32_t* userId);
int sceKernelUsleep(uint32_t microseconds);
}

std::string orbis_select_game(const char* games_dir, const char* top_dir, const char* build_tag);

namespace
{
using ps5::demo::Canvas;
using ps5::demo::Color;

// libScePad's state (main-boot.cpp reads the same layout).
struct PadData
{
	uint32_t buttons;
	uint8_t lx, ly, rx, ry, l2, r2, pad0, pad1;
	uint8_t rest[256];
};
constexpr uint32_t kUp = 0x10, kDown = 0x40, kCross = 0x4000, kOptions = 0x8, kL1 = 0x400, kR1 = 0x800;

struct Game
{
	std::string dir; // vk-285-33: the folder it is in
	std::string file; // the name in the directory
	std::string title; // the name without its extension
	unsigned long long bytes;
};

// .iso, .chd (vk-285-108), .cso or .zso (vk-285-113), in any case.
bool IsDiscImage(const char* name)
{
	const size_t n = std::strlen(name);
	if (n < 5 || name[0] == '.')
		return false;
	const char* ext = name + n - 4;
	const char e1 = static_cast<char>(ext[1] | 0x20), e2 = static_cast<char>(ext[2] | 0x20), e3 = static_cast<char>(ext[3] | 0x20);
	return ext[0] == '.' &&
	       ((e1 == 'i' && e2 == 's' && e3 == 'o') || (e1 == 'c' && e2 == 'h' && e3 == 'd') ||
	        (e1 == 'c' && e2 == 's' && e3 == 'o') || (e1 == 'z' && e2 == 's' && e3 == 'o'));
}

std::string Lower(const std::string& s)
{
	std::string out(s);
	for (char& c : out)
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	return out;
}

// Adds the disc images in `dir` (an image already found under the same name is skipped).
void ScanGames(const char* dir, std::vector<Game>& games)
{
	DIR* d = opendir(dir);
	if (!d)
		return;
	while (const dirent* e = readdir(d))
	{
		if (!IsDiscImage(e->d_name))
			continue;
		const std::string path = std::string(dir) + "/" + e->d_name;
		struct stat st = {};
		if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		const std::string file = e->d_name;
		if (std::any_of(games.begin(), games.end(), [&](const Game& g) { return g.file == file; }))
			continue;
		games.push_back({dir, file, file.substr(0, file.size() - 4), static_cast<unsigned long long>(st.st_size)});
	}
	closedir(d);
}

std::vector<Game> ScanAll(const char* games_dir, const char* top_dir)
{
	std::vector<Game> games;
	ScanGames(games_dir, games);
	if (std::strcmp(games_dir, top_dir) != 0)
		ScanGames(top_dir, games);
	std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
		const std::string la = Lower(a.title), lb = Lower(b.title);
		return la != lb ? la < lb : a.title < b.title;
	});
	return games;
}

std::string ReadLastGame(const char* dir)
{
	const std::string path = std::string(dir) + "/lastgame.txt";
	std::string name;
	if (FILE* f = std::fopen(path.c_str(), "r"))
	{
		char buf[512] = {};
		if (std::fgets(buf, sizeof(buf), f))
		{
			name = buf;
			while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
				name.pop_back();
		}
		std::fclose(f);
	}
	return name;
}

void WriteLastGame(const char* dir, const std::string& file)
{
	const std::string path = std::string(dir) + "/lastgame.txt";
	if (FILE* f = std::fopen(path.c_str(), "w"))
	{
		std::fprintf(f, "%s\n", file.c_str());
		std::fclose(f);
	}
}

// ---- drawing (colours are 0xAABBGGRR) ----
constexpr Color kBackground = static_cast<Color>(0xff1e140eu);
constexpr Color kRow = static_cast<Color>(0xff463224u);
constexpr Color kAccent = static_cast<Color>(0xfff0b040u);
constexpr Color kTitle = static_cast<Color>(0xffffffffu);
constexpr Color kText = static_cast<Color>(0xffdcd6d0u);
constexpr Color kSelected = static_cast<Color>(0xffffffffu);
constexpr Color kDim = static_cast<Color>(0xff9a8e84u);

constexpr unsigned kLeft = 160, kRight = 1760; // inside the TV's safe area
constexpr unsigned kListTop = 290, kRowHeight = 62, kVisible = 11;
constexpr unsigned kNameScale = 4, kSizeScale = 3;

struct Menu
{
	const std::vector<Game>* games;
	int selected;
	int top;
	const char* tag;
	bool starting; // the last frame: "STARTING ..."
};

std::string SizeText(unsigned long long bytes)
{
	char buf[32];
	const double gb = static_cast<double>(bytes) / 1e9;
	if (gb >= 1.0)
		std::snprintf(buf, sizeof(buf), "%.1f GB", gb);
	else
		std::snprintf(buf, sizeof(buf), "%.0f MB", static_cast<double>(bytes) / 1e6);
	return buf;
}

// `text` cut to `width` pixels at `scale`, with "..." when it doesn't fit.
std::string Fit(const std::string& text, unsigned width, unsigned scale)
{
	if (ps5::demo::label_width(text, scale) <= width)
		return text;
	std::string cut = text;
	while (!cut.empty() && ps5::demo::label_width(cut + "...", scale) > width)
		cut.pop_back();
	while (!cut.empty() && cut.back() == ' ')
		cut.pop_back();
	return cut + "...";
}

// An arrow of `height` rows centred on `cx`: pointing up (apex at `top`) or down.
void Arrow(Canvas& c, unsigned cx, unsigned top, unsigned half_width, unsigned height, bool up, Color color)
{
	for (unsigned r = 0; r < height; ++r)
	{
		const unsigned row = up ? r : height - 1 - r;
		const unsigned half = row * half_width / height;
		c.rectangle(cx - half, top + r, 2 * half + 1, 1, color);
	}
}

void DrawMenu(Canvas& c, const void* user) noexcept
{
	const Menu& m = *static_cast<const Menu*>(user);
	const std::vector<Game>& games = *m.games;
	const int n = static_cast<int>(games.size());
	c.fill_screen(kBackground);

	c.label(kLeft, 96, "PS5", 10, kTitle); // vk-285-50: PS5SX2
	c.label(kLeft + ps5::demo::label_width("PS5", 10), 96, "SX2", 10, kAccent);
	char count[32];
	std::snprintf(count, sizeof(count), "%d / %d", m.selected + 1, n);
	c.label(kRight - ps5::demo::label_width(count, 4), 138, count, 4, kDim);
	c.label(kLeft, 206, m.starting ? "STARTING..." : "SELECT A GAME", 4, m.starting ? kAccent : kDim);
	c.rectangle(kLeft, 256, kRight - kLeft, 4, kAccent);

	for (int v = 0; v < static_cast<int>(kVisible); ++v)
	{
		const int i = m.top + v;
		if (i >= n)
			break;
		const unsigned y = kListTop + static_cast<unsigned>(v) * kRowHeight;
		const bool sel = i == m.selected;
		if (sel)
		{
			c.rectangle(kLeft - 20, y, kRight - kLeft + 40, kRowHeight - 8, kRow);
			c.rectangle(kLeft - 20, y, 10, kRowHeight - 8, kAccent);
		}
		const std::string size = SizeText(games[i].bytes);
		const unsigned size_w = ps5::demo::label_width(size, kSizeScale);
		const std::string name = Fit(games[i].title, kRight - kLeft - size_w - 60, kNameScale);
		c.label(kLeft + 20, y + (kRowHeight - 8 - 7 * kNameScale) / 2, name, kNameScale, sel ? kSelected : kText);
		c.label(kRight - size_w, y + (kRowHeight - 8 - 7 * kSizeScale) / 2, size, kSizeScale, sel ? kText : kDim);
	}
	if (m.top > 0)
		Arrow(c, 960, 266, 18, 16, true, kDim);
	if (m.top + static_cast<int>(kVisible) < n)
		Arrow(c, 960, kListTop + kVisible * kRowHeight + 2, 18, 16, false, kDim);

	c.rectangle(kLeft, 996, kRight - kLeft, 2, kRow);
	c.label(kLeft, 1012, "X  START      UP / DOWN  MOVE      L1 / R1  PAGE", 3, kDim);
	if (m.tag)
		c.label(kRight - ps5::demo::label_width(m.tag, 3), 1012, m.tag, 3, kDim);
}

void Clamp(Menu& m)
{
	const int n = static_cast<int>(m.games->size());
	m.selected = std::max(0, std::min(m.selected, n - 1));
	if (m.selected < m.top)
		m.top = m.selected;
	if (m.selected >= m.top + static_cast<int>(kVisible))
		m.top = m.selected - static_cast<int>(kVisible) + 1;
	m.top = std::max(0, std::min(m.top, std::max(0, n - static_cast<int>(kVisible))));
}

// The pad's buttons with the left stick folded into the D-pad's up/down.
uint32_t Buttons(const PadData& d)
{
	uint32_t b = d.buttons;
	if (d.ly < 48)
		b |= kUp;
	else if (d.ly > 208)
		b |= kDown;
	return b;
}

// One pad step: returns true once X or OPTIONS picks the selection. `held`/`ticks` keep the
// repeat of a held direction (first repeat after ~400 ms, then every ~100 ms at 60 steps/s).
bool Step(Menu& m, uint32_t buttons, uint32_t& prev, int& held, unsigned& ticks, bool& changed)
{
	const int n = static_cast<int>(m.games->size());
	const uint32_t pressed = buttons & ~prev;
	prev = buttons;
	const int before = m.selected;
	const int dir = (buttons & kUp) ? -1 : (buttons & kDown) ? 1 : 0;
	if (dir != 0 && (pressed & (dir < 0 ? kUp : kDown)))
	{
		held = dir;
		ticks = 0;
		m.selected = (m.selected + dir + n) % n; // a fresh press wraps around
	}
	else if (dir != 0 && dir == held)
	{
		if (++ticks >= 24 && (ticks - 24) % 6 == 0)
			m.selected = std::max(0, std::min(n - 1, m.selected + dir)); // repeats stop at the ends
	}
	else
		held = 0;
	if (pressed & kL1)
		m.selected -= static_cast<int>(kVisible);
	if (pressed & kR1)
		m.selected += static_cast<int>(kVisible);
	Clamp(m);
	changed = changed || m.selected != before;
	return (pressed & (kCross | kOptions)) != 0;
}
} // namespace

std::string orbis_select_game(const char* games_dir, const char* top_dir, const char* build_tag)
{
	const char* const dir = top_dir; // lastgame.txt stays in the top folder
	const std::vector<Game> games = ScanAll(games_dir, top_dir);
	std::printf("[menu] %zu disc image(s) in %s%s%s\n", games.size(), games_dir,
		std::strcmp(games_dir, top_dir) != 0 ? " and " : "", std::strcmp(games_dir, top_dir) != 0 ? top_dir : "");
	for (const Game& g : games)
		std::printf("[menu]   %s/%s (%llu bytes)\n", g.dir.c_str(), g.file.c_str(), g.bytes);
	std::fflush(stdout);
	if (games.empty())
		return {};

	const std::string last = ReadLastGame(dir);
	Menu m = {&games, 0, 0, build_tag, false};
	for (size_t i = 0; i < games.size(); ++i)
		if (games[i].file == last)
			m.selected = static_cast<int>(i);
	Clamp(m);
	const auto chosen = [&](const char* why) {
		const Game& g = games[static_cast<size_t>(m.selected)];
		WriteLastGame(dir, g.file);
		std::printf("[menu] %s: %s\n", why, g.file.c_str());
		std::fflush(stdout);
		return g.dir + "/" + g.file;
	};
	if (games.size() == 1)
		return chosen("the only disc image");
	if (OrbisFlag("nomenu")) // vk-285-33: flags/ or the top folder
		return chosen("nomenu flag, the last game");

	int32_t user = -1;
	(void)sceUserServiceInitialize(nullptr);
	(void)sceUserServiceGetInitialUser(&user);
	(void)scePadInit();
	const int pad = user >= 0 ? scePadOpen(user, 0, 0, nullptr) : -1;
	std::printf("[menu] user %d pad %d\n", static_cast<int>(user), pad);
	std::fflush(stdout);
	if (pad < 0)
		return chosen("no controller, the last game");

	ps5::demo::MenuDisplay display;
	if (!display.open())
	{
		scePadClose(pad);
		return chosen("no display, the last game");
	}
	display.present(DrawMenu, &m);
	display.present(DrawMenu, &m);

	// Buttons held at start (the X that launched the app) count only once released.
	uint32_t prev = ~0u;
	int held = 0;
	unsigned ticks = 0;
	for (;;)
	{
		PadData d;
		std::memset(&d, 0, sizeof(d));
		d.ly = 128;
		uint32_t buttons = 0;
		if (scePadReadState(pad, &d) == 0)
			buttons = Buttons(d);
		bool changed = false;
		if (Step(m, buttons, prev, held, ticks, changed))
			break;
		if (changed)
			display.present(DrawMenu, &m);
		else
			sceKernelUsleep(16000);
	}
	m.starting = true;
	display.present(DrawMenu, &m);
	sceKernelUsleep(300000); // long enough to see which game starts
	display.close();
	scePadClose(pad);
	return chosen("picked");
}
