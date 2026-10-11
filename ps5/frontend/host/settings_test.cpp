// PS5 port frontend, PC harness: the settings files the settings page (fe_web.cpp) and the shelf's options sheet
// (fe_options.cpp through fe_settings.cpp) write, for the same changes. test-settings.sh builds it twice, against this
// tree's fe_web.cpp and against an older one, and compares:
//   settings_test <folder> web    the page's API over HTTP (127.0.0.1): responses and the files it leaves
//   settings_test <folder> sheet  the same changes through fe::settings, as the sheet makes them (this tree only)
//   settings_test <folder> nosettings  vk-285-121: both save into a missing settings/ folder (this tree only)
// The files must match line for line apart from comment lines (each side names itself in the file's first line).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../fe_web.h"
#ifdef FE_HAVE_SETTINGS
#include "../fe_settings.h"
#endif

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace fe;

namespace
{
std::string g_root;

void MakeDir(const std::string& p)
{
	mkdir(p.c_str(), 0755);
}

void Write(const std::string& path, const std::string& text)
{
	std::ofstream f(path, std::ios::binary);
	f << text;
}

std::string Read(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}

// An ISO 9660 image just big enough for the shelf's serial reader: the volume descriptor at sector 16, the root
// directory at 18 with SYSTEM.CNF, its text at 19.
void MakeIso(const std::string& path, const std::string& boot_elf)
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
	std::ofstream f(path, std::ios::binary);
	f.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
}

void Seed(const std::string& data)
{
	MakeDir(data);
	for (const char* d : {"/games", "/settings", "/patches", "/memcards", "/logs", "/covers", "/cache"})
		MakeDir(data + d);
	MakeIso(data + "/games/God of War (USA).iso", "SCUS_973.99");
	MakeIso(data + "/games/Sample Game (Europe).iso", "SLES_123.45");
	Write(data + "/gs.ini", "# All games\nupscale_multiplier=4\nEmuCore/EnableWideScreenPatches=true\n");
	Write(data + "/patches/SCUS-97399_2F123FD8.pnach",
		"gametitle=God of War (US)\n\n[Widescreen 16:9]\ndescription=placeholder\npatch=1,EE,00100000,word,00000000\n\n"
		"[60 FPS]\ndescription=placeholder\npatch=1,EE,00100004,word,00000000\n");
	FILE* f = std::fopen((data + "/memcards/Mcd001.ps2").c_str(), "wb");
	std::fseek(f, 8650752 - 1, SEEK_SET);
	std::fputc(0, f);
	std::fclose(f);
}

// Every file under data/ that a change can touch, sorted, as text (a card: its size and a checksum).
std::string Dump(const std::string& data, bool drop_comments)
{
	std::vector<std::string> names;
	for (const char* d : {"/settings", "/memcards"})
		if (DIR* dir = opendir((data + d).c_str()))
		{
			while (dirent* e = readdir(dir))
				if (e->d_name[0] != '.')
					names.push_back(std::string(d + 1) + "/" + e->d_name);
			closedir(dir);
		}
	names.push_back("gs.ini");
	std::sort(names.begin(), names.end());
	std::string out;
	for (const std::string& n : names)
	{
		const std::string text = Read(data + "/" + n);
		out += "=== " + n + "\n";
		if (n.rfind("memcards/", 0) == 0)
		{
			uint32_t sum = 0;
			for (unsigned char c : text)
				sum = sum * 31 + c;
			out += std::to_string(text.size()) + " bytes, sum " + std::to_string(sum) + "\n";
			continue;
		}
		std::istringstream s(text);
		std::string line;
		while (std::getline(s, line))
			if (!(drop_comments && !line.empty() && line[0] == '#'))
				out += line + "\n";
	}
	return out;
}

std::string Http(uint16_t port, const std::string& token, const std::string& method, const std::string& target, const std::string& body)
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
	const std::string req = method + " " + enc + " HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Token: " + token + "\r\nContent-Type: text/plain\r\n"
	                        "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
	send(s, req.data(), req.size(), 0);
	std::string resp;
	char buf[8192];
	ssize_t n;
	while ((n = recv(s, buf, sizeof(buf), 0)) > 0)
		resp.append(buf, static_cast<size_t>(n));
	close(s);
	const size_t sp = resp.find(' ');
	const size_t hdr_end = resp.find("\r\n\r\n");
	return resp.substr(sp + 1, 3) + " " + (hdr_end == std::string::npos ? std::string() : resp.substr(hdr_end + 4));
}

// The changes, as the page sends them.
const char* const kGod = "God of War (USA).iso";
const char* const kSample = "Sample Game (Europe).iso";

int RunWeb(const std::string& data, const std::string& presets)
{
	WebServer web;
	WebConfig cfg;
	cfg.game_dirs = {data + "/games"};
	cfg.settings_dir = data + "/settings";
	cfg.gs_ini = data + "/gs.ini";
	cfg.patches_dir = data + "/patches";
	cfg.covers_dir = data + "/covers";
	cfg.cache_dir = data + "/cache";
	cfg.token_path = data + "/token.txt";
	cfg.build_tag = "test";
	cfg.port = static_cast<uint16_t>(20000 + getpid() % 20000);
	cfg.presets = presets;
	cfg.change_log = data + "/logs/settings.log";
	cfg.logs_dir = data + "/logs";
	cfg.top_dir = data;
	cfg.memcards_dir = data + "/memcards";
	if (!web.Start(cfg))
	{
		std::printf("web server did not start\n");
		return 1;
	}
	const std::string t = web.Token();
	auto call = [&](const char* m, const std::string& target, const std::string& body) {
		std::printf(">>> %s %s %s\n%s\n", m, target.c_str(), body.c_str(), Http(web.Port(), t, m, target, body).c_str());
	};
	const std::string god = std::string("/api/settings?id=") + kGod, sample = std::string("/api/settings?id=") + kSample;
	call("GET", "/api/games", "");
	call("GET", god, "");
	call("POST", god, "set upscale_multiplier=6\nset EmuCore/EnableWideScreenPatches=false\npatch+ Widescreen 16:9\n");
	call("POST", god, "unset upscale_multiplier\nset MemoryCards/Slot1_Filename=Mcd001.ps2\npatch+ 60 FPS\npatch- Widescreen 16:9\n");
	call("POST", "/api/settings?id=@global", "set upscale_multiplier=3\nset TVShader=5\n");
	call("POST", god, "recommended");
	call("POST", sample, "set PS5SX2/GameLanguage=2\nset filter=1\n");
	// vk-285-116: the Controls tab's settings (the remapping, vk-285-117: the save combo and its hold time).
	call("POST", sample, "set PS5SX2/ButtonCross=Circle\nset PS5SX2/ButtonCircle=Cross\nset PS5SX2/SaveButton1=Touchpad\nset PS5SX2/StateHold=1.5\n"
		"set PS5SX2/SwapSticks=true\n");
	call("POST", sample, "recommended");
	call("POST", "/api/memcards", "create 16 My Card");
	call("GET", "/api/memcards", "");
	call("POST", "/api/settings?id=@global", "recommended");
	call("POST", god, "set bad key=1\n");
	call("GET", "/api/settings?id=@global", "");
	web.Stop();
	std::printf("%s", Dump(data, false).c_str());
	return 0;
}

#ifdef FE_HAVE_SETTINGS
int RunSheet(const std::string& data, const std::string& presets)
{
	using namespace settings;
	std::string what, error, made;
	const std::string god = data + "/settings/God of War (USA).ini", sample = data + "/settings/Sample Game (Europe).ini";
	const std::string hg = "# God of War (SCUS-97399): written from the PS5SX2 shelf", hs = "# Sample Game (SLES-12345): written from the PS5SX2 shelf";
	bool ok = true;
	ok &= EditSettingsFile(god, hg,
		{{Change::Set, "upscale_multiplier", "6"}, {Change::Set, "EmuCore/EnableWideScreenPatches", "false"},
			{Change::PatchOn, "Widescreen 16:9", ""}},
		what, error);
	ok &= EditSettingsFile(god, hg,
		{{Change::Unset, "upscale_multiplier", ""}, {Change::Set, "MemoryCards/Slot1_Filename", "Mcd001.ps2"},
			{Change::PatchOn, "60 FPS", ""}, {Change::PatchOff, "Widescreen 16:9", ""}},
		what, error);
	ok &= EditSettingsFile(data + "/gs.ini", "# All games", {{Change::Set, "upscale_multiplier", "3"}, {Change::Set, "TVShader", "5"}}, what, error);
	ok &= ApplyRecommended(god, presets, "SCUS-97399", "# God of War (SCUS-97399)", what, error);
	ok &= EditSettingsFile(sample, hs, {{Change::Set, "PS5SX2/GameLanguage", "2"}, {Change::Set, "filter", "1"}}, what, error);
	ok &= EditSettingsFile(sample, hs,
		{{Change::Set, "PS5SX2/ButtonCross", "Circle"}, {Change::Set, "PS5SX2/ButtonCircle", "Cross"}, {Change::Set, "PS5SX2/SaveButton1", "Touchpad"},
			{Change::Set, "PS5SX2/StateHold", "1.5"}, {Change::Set, "PS5SX2/SwapSticks", "true"}},
		what, error); // vk-285-116, vk-285-117
	ok &= ApplyRecommended(sample, presets, "SLES-12345", "# Sample Game (SLES-12345)", what, error);
	ok &= CreateCard(data + "/memcards", 16, "My Card.ps2", made, error);
	ok &= ApplyRecommended(data + "/gs.ini", presets, "@global", "# All games", what, error);
	const bool bad = EditSettingsFile(god, hg, {{Change::Set, "bad key", "1"}}, what, error);
	std::printf("sheet: %s; the bad key %s (%s)\n", ok ? "all changes saved" : ("FAILED: " + error).c_str(), bad ? "WAS SAVED" : "was refused",
		error.c_str());
	// pr9n (PR #9 review item 4): the RetroAchievements account's keys, in any case, never from a settings file edit.
	bool account_key = false;
	for (const char* k : {"Achievements/Host", "Achievements/Token", "achievements/username", "ACHIEVEMENTS/Enabled", "Achievements"})
		account_key |= SafeKey(k) || EditSettingsFile(god, hg, {{Change::Set, k, "https://elsewhere.example"}}, what, error);
	std::printf("sheet: the RetroAchievements keys %s\n", account_key ? "WERE ACCEPTED" : "were refused");
	std::printf("%s", Dump(data, true).c_str());
	return ok && !bad && !account_key ? 0 : 1;
}

// vk-285-121: a console without settings/ (two testers' 1.7 logs: every save failed with errno 2). The sheet's save
// and the page's must each make the folder and write the file.
int RunNoSettings(const std::string& data, const std::string& presets)
{
	using namespace settings;
	const std::string dir = data + "/settings", god = dir + "/God of War (USA).ini";
	rmdir(dir.c_str());
	std::string what, error;
	const bool sheet = EditSettingsFile(god, "# God of War (SCUS-97399): written from the PS5SX2 shelf",
		{{Change::Set, "upscale_multiplier", "6"}}, what, error);
	const bool sheet_file = Read(god).find("upscale_multiplier=6") != std::string::npos;
	std::printf("%s: the sheet's save without settings/ (%s)\n", sheet && sheet_file ? "PASS" : "FAIL", sheet ? "saved" : error.c_str());
	unlink(god.c_str());
	rmdir(dir.c_str());
	WebServer web;
	WebConfig cfg;
	cfg.game_dirs = {data + "/games"};
	cfg.settings_dir = dir;
	cfg.gs_ini = data + "/gs.ini";
	cfg.patches_dir = data + "/patches";
	cfg.covers_dir = data + "/covers";
	cfg.cache_dir = data + "/cache";
	cfg.token_path = data + "/token.txt";
	cfg.build_tag = "test";
	cfg.port = static_cast<uint16_t>(20000 + (getpid() + 7) % 20000);
	cfg.presets = presets;
	cfg.change_log = data + "/logs/settings.log";
	cfg.logs_dir = data + "/logs";
	cfg.top_dir = data;
	cfg.memcards_dir = data + "/memcards";
	if (!web.Start(cfg))
	{
		std::printf("FAIL: the web server did not start\n");
		return 1;
	}
	const std::string answer = Http(web.Port(), web.Token(), "POST", std::string("/api/settings?id=") + kGod, "set upscale_multiplier=5\n");
	web.Stop();
	const bool page_file = Read(god).find("upscale_multiplier=5") != std::string::npos;
	std::printf("%s: the page's save without settings/\n", page_file ? "PASS" : "FAIL");
	if (!page_file)
		std::printf("%s\n", answer.c_str());
	return sheet && sheet_file && page_file ? 0 : 1;
}
#endif
} // namespace

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		std::fprintf(stderr, "usage: settings_test <folder> web|sheet|files <presets.ini>\n");
		return 2;
	}
	g_root = argv[1];
	const std::string mode = argv[2], presets = Read(argv[3]);
	const std::string data = g_root + "/data-" + mode;
	MakeDir(g_root);
	Seed(data);
	if (mode == "web")
		return RunWeb(data, presets);
#ifdef FE_HAVE_SETTINGS
	if (mode == "sheet")
		return RunSheet(data, presets);
	if (mode == "nosettings")
		return RunNoSettings(data, presets);
#endif
	std::fprintf(stderr, "mode %s is not in this build\n", mode.c_str());
	return 2;
}
