// PS5 port frontend, PC test (2026-10-08, AI-assisted): the Hardware fixes rows (PCSX2's manual hardware fixes) on the
// shelf's sheet and the settings page. Checks the game database reader's gsHWFixes (fe_games.cpp GameDbHwFixes) on the
// real resources/GameIndex.yaml, the settings they become (fe_settings.cpp ManualFixSettings), the sheet (the rows on a
// game's sheet only, the game's own fixes shown while manual fixes are off, a changed fix turning them on with the game's
// own fixes written first, Reset all taking them out) and the page's settings answer ("gamefixes", only for a game that has
// such fixes).
//   ps5/frontend/host/test-hwfixes.sh
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_options.h"
#include "fe_web.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_fails = 0;

static void Check(bool ok, const std::string& what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
	if (!ok)
		g_fails++;
}

static std::string Read(const std::string& path)
{
	std::string out;
	if (FILE* f = std::fopen(path.c_str(), "rb"))
	{
		char buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
			out.append(buf, n);
		std::fclose(f);
	}
	return out;
}

static void Write(const std::string& path, const std::string& text)
{
	if (FILE* f = std::fopen(path.c_str(), "wb"))
	{
		std::fwrite(text.data(), 1, text.size(), f);
		std::fclose(f);
	}
}

// An ISO 9660 image just big enough for the serial reader (as settings_test.cpp makes them).
static void MakeIso(const std::string& path, const std::string& boot_elf)
{
	std::vector<uint8_t> img(20 * 2048, 0);
	auto le32 = [&](size_t at, uint32_t v) {
		for (int i = 0; i < 4; i++)
			img[at + i] = static_cast<uint8_t>(v >> (8 * i));
	};
	uint8_t* pvd = &img[16 * 2048];
	pvd[0] = 1;
	std::memcpy(pvd + 1, "CD001", 5);
	pvd[6] = 1;
	const size_t root = 16 * 2048 + 156;
	img[root] = 34;
	le32(root + 2, 18);
	le32(root + 10, 2048);
	img[root + 25] = 2;
	img[root + 32] = 1;
	uint8_t* term = &img[17 * 2048];
	term[0] = 255;
	std::memcpy(term + 1, "CD001", 5);
	const std::string cnf = "BOOT2 = cdrom0:\\" + boot_elf + ";1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
	const std::string name = "SYSTEM.CNF;1";
	const size_t rec = 18 * 2048;
	img[rec] = static_cast<uint8_t>(33 + name.size() + ((33 + name.size()) & 1));
	le32(rec + 2, 19);
	le32(rec + 10, static_cast<uint32_t>(cnf.size()));
	img[rec + 32] = static_cast<uint8_t>(name.size());
	std::memcpy(&img[rec + 33], name.data(), name.size());
	std::memcpy(&img[19 * 2048], cnf.data(), cnf.size());
	Write(path, std::string(reinterpret_cast<const char*>(img.data()), img.size()));
}

static std::string Http(uint16_t port, const std::string& target)
{
	const int s = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
		return "connect failed";
	std::string enc;
	for (char c : target)
		enc += c == ' ' ? std::string("%20") : std::string(1, c);
	const std::string req = "GET " + enc + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
	send(s, req.data(), req.size(), 0);
	std::string resp;
	char buf[8192];
	ssize_t n;
	while ((n = recv(s, buf, sizeof(buf), 0)) > 0)
		resp.append(buf, static_cast<size_t>(n));
	close(s);
	const size_t body = resp.find("\r\n\r\n");
	return body == std::string::npos ? resp : resp.substr(body + 4);
}

static std::string Fixes(const std::vector<std::pair<std::string, std::string>>& v)
{
	std::string out;
	for (const auto& kv : v)
		out += (out.empty() ? "" : " ") + kv.first + "=" + kv.second;
	return out;
}

static const fe::OptionsSheet::Row* FindRow(const fe::OptionsSheet& sheet, const std::string& label)
{
	for (const auto& r : sheet.rows())
		if (r.label == label)
			return &r;
	return nullptr;
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: hwfix_test <work folder> <GameIndex.yaml>\n");
		return 2;
	}
	const std::string root = argv[1];
	std::system(("rm -rf '" + root + "' && mkdir -p '" + root + "/games' '" + root + "/settings' '" + root + "/patches' '" + root + "/covers' '" +
				 root + "/cache' '" + root + "/logs'")
					.c_str());
	fe::SetGameDbFile(argv[2]);

	// The database reader: gsHWFixes in the file's order, a fix without a value is 1, function names left out.
	{
		const auto kh = fe::GameDbHwFixes("SLUS-20370");
		Check(kh.size() == 1 && kh[0].first == "nativeScaling" && kh[0].second == 3, "Kingdom Hearts (SLUS-20370): nativeScaling 3");
		const auto kh2 = fe::GameDbHwFixes("SLUS-21005");
		std::string names;
		for (const auto& f : kh2)
			names += f.first + "=" + std::to_string(f.second) + " ";
		Check(names == "autoFlush=1 roundSprite=1 halfPixelOffset=5 nativeScaling=4 ", "Kingdom Hearts II (SLUS-21005): " + names);
		const auto namco = fe::GameDbHwFixes("SCAJ-10015");
		bool skip_count = false;
		for (const auto& f : namco)
			skip_count = skip_count || f.first == "getSkipCount";
		Check(!namco.empty() && !skip_count, "SCAJ-10015: its fixes without getSkipCount (a function's name)");
		Check(fe::GameDbHwFixes("SLES-99999").empty(), "a serial the database lacks has none");
		fe::GameInfo g;
		g.serial = "SLUS-20370";
		g.title = "SLUS_203.70";
		fe::ApplyGameDbTitle(g);
		Check(g.title == "Kingdom Hearts", "the database's names still read (" + g.title + ")");
	}

	// The settings that set them by hand.
	{
		Check(Fixes(fe::settings::ManualFixSettings("SLUS-20370")) == "UserHacks_native_scaling=3", "KH: UserHacks_native_scaling=3");
		const std::string kh2 = Fixes(fe::settings::ManualFixSettings("SLUS-21005"));
		Check(kh2 == "UserHacks_AutoFlushLevel=1 UserHacks_round_sprite_offset=1 UserHacks_HalfPixelOffset=5 UserHacks_native_scaling=4",
			"KH II: " + kh2);
		const std::string sled = Fixes(fe::settings::ManualFixSettings("SLED-52031"));
		Check(sled.find("UserHacks_CPU_FB_Conversion=true") != std::string::npos && sled.find("UserHacks_TextureInsideRt=1") != std::string::npos,
			"an on/off fix is true or false: " + sled);
		const std::string papx = Fixes(fe::settings::ManualFixSettings("PAPX-90524"));
		Check(papx.find("paltex") == std::string::npos && papx.find("UserHacks_ForceEvenSpritePosition=true") != std::string::npos,
			"gpuPaletteConversion 2 (depends on preloading) is left to PCSX2: " + papx);
		const std::string slaj = Fixes(fe::settings::ManualFixSettings("SLAJ-25053"));
		Check(slaj.find("Blending") == std::string::npos && slaj.find("UserHacks_GPUTargetCLUTMode=1") != std::string::npos &&
				  slaj.find("UserHacks_BilinearHack=2") != std::string::npos,
			"fixes PCSX2 applies either way (blending levels) aren't in it: " + slaj);
	}

	// The sheet.
	const std::string gs_ini = root + "/gs.ini";
	Write(gs_ini, "# All games\nupscale_multiplier=6\n");
	fe::OptionsPaths paths;
	paths.settings_dir = root + "/settings";
	paths.gs_ini = gs_ini;
	paths.patches_dir = root + "/patches";
	{
		fe::OptionsSheet all;
		all.Open(paths, nullptr);
		Check(!FindRow(all, "Hardware fixes") && !FindRow(all, "Texture offset X"), "the sheet for all games has no Hardware fixes");
	}
	fe::GameInfo kh;
	kh.stem = "Kingdom Hearts (USA)";
	kh.title = "Kingdom Hearts";
	kh.serial = "SLUS-20370";
	const std::string kh_ini = root + "/settings/Kingdom Hearts (USA).ini";
	{
		fe::OptionsSheet sheet;
		sheet.Open(paths, &kh);
		const auto* header = FindRow(sheet, "Hardware fixes");
		const auto* manual = FindRow(sheet, "Manual hardware fixes");
		const auto* native = FindRow(sheet, "Native scaling");
		const auto* tcx = FindRow(sheet, "Texture offset X");
		const auto* crop = FindRow(sheet, "Crop");
		Check(header && manual && native && tcx, "a game's sheet has the Hardware fixes rows");
		Check(header && crop && header < crop, "and Crop stays the last group");
		if (!(manual && native && tcx))
			return 1;
		Check(sheet.Value(*manual) == "Off", "manual fixes start off");
		Check(sheet.Help(*manual).find("This game's own fixes: Native scaling Normal (keep upscale).") != std::string::npos,
			"the Manual row names the game's own fixes: " + sheet.Help(*manual));
		Check(sheet.Value(*native) == "Normal (keep upscale)", "while off, Native scaling shows the game's own fix: " + sheet.Value(*native));
		Check(sheet.Help(*native).find("PCSX2's own fix for this game.") != std::string::npos, "and says so");
		Check(sheet.Value(*tcx) == "Off" && sheet.Help(*tcx).find("PCSX2's default.") != std::string::npos, "a fix the game hasn't: PCSX2's default");
		// Texture offset X, one step: manual fixes on, the game's native scaling written, the offset set.
		sheet.Step(*tcx, 1);
		const std::string file = Read(kh_ini);
		Check(file.find("UserHacks_TCOffsetX=100\n") != std::string::npos && file.find("UserHacks=true\n") != std::string::npos &&
				  file.find("UserHacks_native_scaling=3\n") != std::string::npos,
			"changing a fix turns manual fixes on and writes the game's own first:\n" + file);
		native = FindRow(sheet, "Native scaling");
		manual = FindRow(sheet, "Manual hardware fixes");
		tcx = FindRow(sheet, "Texture offset X");
		Check(sheet.Value(*manual) == "On" && sheet.Value(*native) == "Normal (keep upscale)" &&
				  sheet.Help(*native).find("Set for this game.") != std::string::npos,
			"then the row is this game's own setting");
		// Step the offset on to 525 (100 -> 200 -> 250 -> 300 -> 400 -> 500 -> 525).
		for (int i = 0; i < 6; i++)
		{
			tcx = FindRow(sheet, "Texture offset X");
			sheet.Step(*tcx, 1);
		}
		tcx = FindRow(sheet, "Texture offset X");
		Check(sheet.Value(*tcx) == "525" && Read(kh_ini).find("UserHacks_TCOffsetX=525\n") != std::string::npos, "the tester's 525 is a step");
		// Manual fixes off: the rows stay set, and say they count only while it's on.
		manual = FindRow(sheet, "Manual hardware fixes");
		sheet.Step(*manual, 1);
		native = FindRow(sheet, "Native scaling");
		Check(Read(kh_ini).find("UserHacks=false\n") != std::string::npos &&
				  sheet.Help(*native).find("counts while Manual hardware fixes is on") != std::string::npos,
			"manual fixes off: the rows say they count only while it's on");
		// On again from the Manual row: nothing more to write (the game's fix is already set).
		manual = FindRow(sheet, "Manual hardware fixes");
		sheet.Step(*manual, 1);
		Check(Read(kh_ini).find("UserHacks=true\n") != std::string::npos, "manual fixes on again");
		// Follow all games: every line gone, the game's own fixes too.
		for (const auto& r : sheet.rows())
			if (r.kind == fe::OptionsSheet::Kind::ResetAll)
			{
				sheet.Activate(r, 1.0);
				sheet.Activate(r, 1.5);
				break;
			}
		const std::string after = Read(kh_ini);
		Check(after.find("UserHacks") == std::string::npos, "Follow the settings for all games takes them all out:\n" + after);
	}
	// A game whose fixes have no row of their own (KH II's auto flush has one, SLED-52031's CPU framebuffer conversion hasn't):
	// turning manual fixes on from the Manual row writes them all.
	{
		fe::GameInfo g;
		g.stem = "Shield Game (Europe)";
		g.title = "Shield Game";
		g.serial = "SLED-52031";
		fe::OptionsSheet sheet;
		sheet.Open(paths, &g);
		const auto* manual = FindRow(sheet, "Manual hardware fixes");
		sheet.Step(*manual, 1);
		const std::string file = Read(root + "/settings/Shield Game (Europe).ini");
		Check(file.find("UserHacks=true\n") != std::string::npos && file.find("UserHacks_CPU_FB_Conversion=true\n") != std::string::npos &&
				  file.find("UserHacks_AutoFlushLevel=2\n") != std::string::npos && file.find("UserHacks_HalfPixelOffset=3\n") != std::string::npos,
			"the Manual row on writes every fix of the game's:\n" + file);
		for (const auto& r : sheet.rows())
			if (r.kind == fe::OptionsSheet::Kind::ResetAll)
			{
				sheet.Activate(r, 1.0);
				sheet.Activate(r, 1.5);
				break;
			}
		Check(Read(root + "/settings/Shield Game (Europe).ini").find("UserHacks") == std::string::npos, "and Reset all takes even those out");
	}

	// 2026-10-08: an ELF's sheet: the Disc image row first, cycling through the shelf's disc images by file name.
	{
		fe::GameInfo elf;
		elf.file = "Homebrew.elf";
		elf.stem = "Homebrew";
		elf.title = "Homebrew";
		fe::OptionsPaths ep = paths;
		ep.disc_images = {{"Kingdom Hearts (USA).iso", "Kingdom Hearts"}, {"God of War (USA).chd", "God of War"}};
		fe::OptionsSheet sheet;
		sheet.Open(ep, &elf);
		const auto* disc = FindRow(sheet, "Disc image");
		Check(disc && sheet.rows().size() > 2 && &sheet.rows()[2] == disc, "an ELF's sheet starts with its Disc image row");
		if (!disc)
			return 1;
		Check(sheet.Value(*disc) == "No disc", "no disc to start with");
		sheet.Step(*disc, 1);
		disc = FindRow(sheet, "Disc image");
		const std::string ini = root + "/settings/Homebrew.ini";
		Check(sheet.Value(*disc) == "Kingdom Hearts" && Read(ini).find("PS5SX2/ElfDisc=Kingdom Hearts (USA).iso\n") != std::string::npos,
			"a step picks the first image, saved by its file name");
		sheet.Step(*disc, 1);
		disc = FindRow(sheet, "Disc image");
		Check(sheet.Value(*disc) == "God of War", "the next one");
		sheet.Step(*disc, 1);
		disc = FindRow(sheet, "Disc image");
		Check(sheet.Value(*disc) == "No disc" && Read(ini).find("ElfDisc") == std::string::npos, "round to no disc: the line goes");
		sheet.Step(*disc, -1);
		disc = FindRow(sheet, "Disc image");
		sheet.Reset(*disc);
		disc = FindRow(sheet, "Disc image");
		Check(sheet.Value(*disc) == "No disc", "Triangle takes it off");
		fe::OptionsSheet game;
		game.Open(ep, &kh);
		Check(!FindRow(game, "Disc image"), "a disc image's sheet has no such row");
	}

	// The page's answer: "gamefixes" for a game that has such fixes, nothing new for one that hasn't.
	{
		MakeIso(root + "/games/Kingdom Hearts (USA).iso", "SLUS_203.70");
		MakeIso(root + "/games/Sample Game (Europe).iso", "SLES_123.45");
		fe::WebServer web;
		fe::WebConfig cfg;
		cfg.game_dirs = {root + "/games"};
		cfg.settings_dir = root + "/settings";
		cfg.gs_ini = gs_ini;
		cfg.patches_dir = root + "/patches";
		cfg.covers_dir = root + "/covers";
		cfg.cache_dir = root + "/cache";
		cfg.build_tag = "test";
		cfg.port = static_cast<uint16_t>(20000 + getpid() % 20000);
		cfg.top_dir = root;
		cfg.change_log = root + "/logs/settings.log";
		if (!web.Start(cfg))
		{
			Check(false, "the web server starts");
			return 1;
		}
		const std::string kh_json = Http(web.Port(), "/api/settings?id=Kingdom Hearts (USA).iso");
		const std::string other = Http(web.Port(), "/api/settings?id=Sample Game (Europe).iso");
		const std::string all = Http(web.Port(), "/api/settings?id=@global");
		web.Stop();
		Check(kh_json.find("\"gamefixes\":{\"UserHacks_native_scaling\":\"3\"}") != std::string::npos, "the page gets KH's own fixes");
		Check(other.find("gamefixes") == std::string::npos && other.find("\"id\"") != std::string::npos, "and nothing for a game without");
		Check(all.find("gamefixes") == std::string::npos, "or for all games");
	}

	std::printf(g_fails ? "%d FAILED\n" : "all passed\n", g_fails);
	return g_fails ? 1 : 0;
}
