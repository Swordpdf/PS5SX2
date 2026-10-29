// PS5 port frontend, PC harness (2.12, AI-assisted; swordpdf: "add an option to add a game from the shelf to the xmb ... we should also be able to delete it, again, from
// the shelf"): the settings page's copy of the sheet's Home screen tile row, /api/tile. test-gametile.sh builds this against fe_web.cpp and runs it (the server runs in the
// process, the requests go over 127.0.0.1). THIS TEST plays the PS5SX2 Disc Helper by hand over real files (alive.txt, state.txt, taking the request file), so every
// step of the contract is seen from the page's side: the page asks, the helper's files answer, and the page says what the shelf's sheet says.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../fe_web.h"
#include "../fe_gametile.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace fe;

namespace
{
int fails = 0;

void Check(bool ok, const char* what, const std::string& detail = std::string())
{
	std::printf("%s: %s%s%s\n", ok ? "ok  " : "FAIL", what, detail.empty() || ok ? "" : "  -- ", ok ? "" : detail.c_str());
	if (!ok)
		fails++;
}

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

bool Exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

// An ISO 9660 image just big enough for the serial reader (the same as discgame_web_test.cpp's).
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

// The status, a space, the body.
std::string Http(uint16_t port, const std::string& method, const std::string& target, const std::string& body)
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
	const std::string req = method + " " + enc + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) +
	                        "\r\nConnection: close\r\n\r\n" + body;
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

bool Has(const std::string& text, const std::string& part)
{
	return text.find(part) != std::string::npos;
}

// A string field of the reply's JSON ("phase":"on" -> on), no escapes in what these tests look at.
std::string Field(const std::string& json, const std::string& key)
{
	const std::string k = "\"" + key + "\":";
	const size_t at = json.find(k);
	if (at == std::string::npos)
		return "(none)";
	size_t from = at + k.size();
	if (json[from] == '"')
	{
		const size_t end = json.find('"', from + 1);
		return json.substr(from + 1, end - from - 1);
	}
	return json.substr(from, json.find_first_of(",}", from) - from);
}

std::vector<std::string> Requests(const std::string& dir)
{
	std::vector<std::string> out;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (const dirent* e = readdir(d))
		{
			const std::string n = e->d_name;
			if (n.compare(0, 8, "request-") == 0)
				out.push_back(n);
		}
		closedir(d);
	}
	std::sort(out.begin(), out.end());
	return out;
}

// The helper's alive.txt, as it writes it (every 20 s; here once).
void Alive(const std::string& dir)
{
	Write(dir + "/alive.tmp", std::to_string(static_cast<long long>(time(nullptr))) + " 1234 gametile 1\n");
	std::rename((dir + "/alive.tmp").c_str(), (dir + "/alive.txt").c_str());
}

void State(const std::string& dir, const std::string& text)
{
	Write(dir + "/state.tmp", text);
	std::rename((dir + "/state.tmp").c_str(), (dir + "/state.txt").c_str());
}

void Sleep(double s)
{
	std::this_thread::sleep_for(std::chrono::duration<double>(s));
}
} // namespace

int main(int argc, char** argv)
{
	const std::string data = argc > 1 ? argv[1] : "/tmp/gametile-web-test";
	std::system(("rm -rf '" + data + "' && mkdir -p '" + data + "'").c_str());
	for (const char* d : {"/games", "/settings", "/patches", "/memcards", "/logs", "/covers", "/cache", "/tiles"})
		MakeDir(data + d);
	// The games live in a temporary folder, which /data and /mnt cannot be here: the test adds it to the folders a game may be in (nothing in the app does).
	gametile::GameRoots().push_back(data + "/");
	MakeIso(data + "/games/God of War (USA).iso", "SLUS_200.01");
	MakeIso(data + "/games/Okami (USA).iso", "SLUS_213.09");
	Write(data + "/gs.ini", "# All games\nupscale_multiplier=4\n");
	const std::string tiles = data + "/tiles";
	const std::string game = data + "/games/God of War (USA).iso";
	const std::string id = "God of War (USA).iso", id2 = "Okami (USA).iso";
	const std::string tile_get = "/api/tile?id=" + id;

	auto start = [&](WebServer& web, const std::string& tiles_dir, const std::string& own) {
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
		cfg.change_log = data + "/logs/settings.log";
		cfg.logs_dir = data + "/logs";
		cfg.top_dir = data;
		cfg.memcards_dir = data + "/memcards";
		cfg.tiles_dir = tiles_dir;
		cfg.own_tile_game = own;
		return web.Start(cfg);
	};
	WebServer web;
	if (!start(web, tiles, ""))
	{
		std::printf("web server did not start\n");
		return 1;
	}
	const uint16_t port = web.Port();

	// No helper yet (no alive.txt): the row is there and says so; nothing is written.
	std::string r = Http(port, "GET", tile_get, "");
	Check(r.rfind("200", 0) == 0 && Field(r, "available") == "true" && Field(r, "phase") == "nohelper" && Field(r, "value") == "No helper" && Field(r, "can") == "none" &&
			  Field(r, "busy") == "false",
		"no alive.txt: the row says No helper and offers nothing", r);
	Check(Has(r, "171k") && Has(r, "by hand"), "...and its help names the helper revision and the manual-load fallback", r);
	r = Http(port, "POST", "/api/tile", "add\n" + id);
	Check(r.rfind("409", 0) == 0 && Requests(tiles).empty(), "POST add with no helper: 409 and no file written", r);

	// A stale alive.txt (two minutes old) is no helper either.
	Alive(tiles);
	{
		const struct timeval old[2] = {{static_cast<time_t>(time(nullptr) - 120), 0}, {static_cast<time_t>(time(nullptr) - 120), 0}};
		utimes((tiles + "/alive.txt").c_str(), old);
	}
	r = Http(port, "GET", tile_get, "");
	Check(Field(r, "phase") == "nohelper", "a 2-minute-old alive.txt: still No helper", r);

	// The helper is there (alive.txt, an empty state.txt).
	Alive(tiles);
	State(tiles, "");
	r = Http(port, "GET", tile_get, "");
	Check(Field(r, "phase") == "off" && Field(r, "value") == "Off" && Field(r, "can") == "add" && Field(r, "id") == id, "alive and no line: Off, can add", r);
	Check(Has(r, "Puts God of War on the PS5 home screen as its own tile"), "...the help is the sheet's", r);

	// ADD.
	r = Http(port, "POST", "/api/tile", "add\n" + id);
	Check(r.rfind("200", 0) == 0 && Field(r, "phase") == "asking" && Field(r, "busy") == "true" && Field(r, "value") == "Adding..." && Field(r, "can") == "none", "POST add: Asking, busy", r);
	std::vector<std::string> files = Requests(tiles);
	Check(files.size() == 1 && files[0].compare(0, 8, "request-") == 0 && files[0].size() == std::string("request-1760611234567-000.txt").size(), "one request file, named request-<ms13>-<nnn>.txt",
		files.empty() ? std::string("none") : files[0]);
	const std::string body = files.empty() ? std::string() : Read(tiles + "/" + files[0]);
	Check(body == "v1\nadd\t" + game + "\tSLUS-20001\tGod of War\n", "the request is exactly v1, add, the path, the serial and the title, TAB-separated", Read(tiles + "/" + (files.empty() ? "" : files[0])));
	r = Http(port, "POST", "/api/tile", "add\n" + id);
	Check(r.rfind("409", 0) == 0 && Has(r, "still working") && Requests(tiles).size() == 1, "a second POST while busy: 409, no second file", r);
	r = Http(port, "POST", "/api/tile", "remove\n" + id);
	Check(r.rfind("409", 0) == 0 && Requests(tiles).size() == 1, "POST remove while busy: 409 too", r);
	// The helper takes the file.
	std::remove((tiles + "/" + files[0]).c_str());
	r = Http(port, "GET", tile_get, "");
	Check(Field(r, "phase") == "waiting" && Field(r, "busy") == "true" && Field(r, "value") == "Adding...", "the file is gone: Waiting", r);
	State(tiles, "PPSA99251\t" + game + "\tSLUS-20001\tGod of War\tinstalling\n");
	r = Http(port, "GET", tile_get, "");
	Check(Field(r, "phase") == "installing" && Field(r, "value") == "Installing..." && Field(r, "tile") == "PPSA99251" && Field(r, "busy") == "true", "a line installing: Installing, with the tile id", r);
	Check(Has(r, "about 30 seconds after you close PS5SX2"), "...and the help says when it appears", r);
	State(tiles, "PPSA99251\t" + game + "\tSLUS-20001\tGod of War\tok\n");
	r = Http(port, "GET", tile_get, "");
	Check(Field(r, "phase") == "on" && Field(r, "value") == "On" && Field(r, "can") == "remove" && Field(r, "busy") == "false", "a line ok: On, can remove", r);
	r = Http(port, "POST", "/api/tile", "add\n" + id);
	Check(r.rfind("409", 0) == 0 && Requests(tiles).empty(), "POST add of a game that is On: 409, nothing written", r);
	// Another game is not this game.
	r = Http(port, "GET", "/api/tile?id=" + id2, "");
	Check(Field(r, "phase") == "off" && Field(r, "id") == id2, "another game: Off", r);

	// REMOVE.
	r = Http(port, "POST", "/api/tile", "remove\n" + id);
	Check(r.rfind("200", 0) == 0 && Field(r, "phase") == "asking" && Field(r, "value") == "Removing...", "POST remove: Asking, shown as Removing...", r);
	files = Requests(tiles);
	Check(files.size() == 1 && Read(tiles + "/" + files[0]) == "v1\nremove\t" + game + "\n", "the request is v1 and one remove line, no add line", files.empty() ? std::string("none") : Read(tiles + "/" + files[0]));
	std::remove((tiles + "/" + files[0]).c_str());
	r = Http(port, "GET", tile_get, "");
	Check(Field(r, "phase") == "removing" && Field(r, "busy") == "true", "taken, the line still ok: Removing", r);
	State(tiles, "PPSA99251\t" + game + "\tSLUS-20001\tGod of War\tremoving\n");
	r = Http(port, "GET", tile_get, "");
	Check(Field(r, "phase") == "removing" && Has(r, "taken away about 30 seconds after you close PS5SX2"), "a line removing: Removing, with its help", r);
	State(tiles, "");
	r = Http(port, "GET", tile_get, "");
	Check(Field(r, "phase") == "off" && Field(r, "can") == "add", "the line is gone: Off", r);

	// A failure and its retry.
	r = Http(port, "POST", "/api/tile", "add\n" + id);
	files = Requests(tiles);
	std::remove((tiles + "/" + files[0]).c_str());
	State(tiles, "-\t" + game + "\tSLUS-20001\tGod of War\tfailed:nogame\n");
	r = Http(port, "GET", tile_get, "");
	Check(Field(r, "phase") == "failed" && Field(r, "value") == "Failed" && Field(r, "reason") == "nogame" && Field(r, "can") == "add" &&
			  Has(r, "The helper cannot find the game file"),
		"a failed line: Failed, the reason's sentence, can retry", r);
	r = Http(port, "POST", "/api/tile", "add\n" + id);
	Check(r.rfind("200", 0) == 0 && Field(r, "phase") == "asking", "retry: Asking (the old failure is not the answer)", r);
	files = Requests(tiles);
	std::remove((tiles + "/" + files[0]).c_str());
	r = Http(port, "GET", tile_get, "");
	Check(Field(r, "phase") == "waiting", "retry, taken: Waiting while the old failure's text is still there (the grace)", r);
	State(tiles, "PPSA99251\t" + game + "\tSLUS-20001\tGod of War\tinstalling\n");
	r = Http(port, "GET", tile_get, "");
	Check(Field(r, "phase") == "installing", "retry: the helper's new line is the answer", r);
	State(tiles, "");
	Http(port, "GET", tile_get, ""); // forgets nothing: the ask is still in the books at Installing; the line left
	r = Http(port, "GET", tile_get, "");
	Check(Field(r, "phase") == "waiting", "the line left again: Waiting (the helper is building it again)", r);

	// The cover: the shelf's cover for the game is the request's fifth field.
	Write(data + "/covers/SLUS-20001.jpg", "not really a jpeg, but a file");
	State(tiles, "-\t" + game + "\tSLUS-20001\tGod of War\tfailed:busy\n");
	r = Http(port, "GET", tile_get, "");
	Check(Field(r, "phase") == "failed" && Field(r, "reason") == "busy", "a second failure: Failed(busy)", r);
	Http(port, "POST", "/api/tile", "add\n" + id);
	files = Requests(tiles);
	Check(files.size() == 1 && Read(tiles + "/" + files[0]) == "v1\nadd\t" + game + "\tSLUS-20001\tGod of War\t" + data + "/covers/SLUS-20001.jpg\n",
		"the request carries the cover the shelf shows as its fifth field", files.empty() ? std::string("none") : Read(tiles + "/" + files[0]));
	std::remove((tiles + "/" + files[0]).c_str());
	State(tiles, "");
	Http(port, "GET", tile_get, "");
	Http(port, "GET", tile_get, "");

	// A request nobody takes is taken back after 15 s (a real wait: this is the page's own clock).
	r = Http(port, "POST", "/api/tile", "add\n" + id2);
	Check(Field(r, "phase") == "asking", "another game: Asking", r);
	Sleep(15.5);
	Alive(tiles); // the helper is still there, but did not take the file
	r = Http(port, "GET", "/api/tile?id=" + id2, "");
	Check(Field(r, "phase") == "noanswer" && Field(r, "value") == "No answer" && Field(r, "can") == "add" && Has(r, "did not take the request within 15 seconds"),
		"nobody took the request in 15 s: No answer, can retry", r);
	Check(Requests(tiles).empty(), "...and the file was taken back (a helper loaded later does not act on it)", std::to_string(Requests(tiles).size()));

	// The unknown, the not-offered.
	r = Http(port, "GET", "/api/tile?id=No%20Such%20Game.iso", "");
	Check(r.rfind("404", 0) == 0 && Has(r, "no such game"), "an unknown game: 404", r);
	r = Http(port, "GET", "/api/tile", "");
	Check(r.rfind("400", 0) == 0, "no id: 400", r);
	r = Http(port, "POST", "/api/tile", "launch\n" + id);
	Check(r.rfind("400", 0) == 0, "an unknown verb: 400", r);
	r = Http(port, "POST", "/api/tile", "add");
	Check(r.rfind("400", 0) == 0 || r.rfind("404", 0) == 0, "no game named: refused", r);
	web.SetNowPlaying("/dev/cd0", "Disc drive cd0", "SLUS-21453");
	r = Http(port, "GET", "/api/tile?id=cd0", "");
	Check(r.rfind("200", 0) == 0 && Field(r, "available") == "false" && Field(r, "why") == "not a plain file", "a game played from a disc drive: no tile row", r);
	r = Http(port, "POST", "/api/tile", "add\ncd0");
	Check(r.rfind("409", 0) == 0, "...and no tile for it", r);
	web.SetNowPlaying("", "", "");

	// The tile this process runs from is not taken off from inside it; and with game tiles off there is no row at all; and the page's other answers are the same bytes.
	WebServer own, off;
	if (!own.Start([&] {
			WebConfig cfg;
			cfg.game_dirs = {data + "/games"};
			cfg.settings_dir = data + "/settings";
			cfg.gs_ini = data + "/gs.ini";
			cfg.patches_dir = data + "/patches";
			cfg.covers_dir = data + "/covers";
			cfg.cache_dir = data + "/cache";
			cfg.token_path = data + "/token.txt";
			cfg.build_tag = "test";
			cfg.port = static_cast<uint16_t>(21000 + getpid() % 20000);
			cfg.change_log = data + "/logs/settings.log";
			cfg.top_dir = data;
			cfg.memcards_dir = data + "/memcards";
			cfg.tiles_dir = tiles;
			cfg.own_tile_game = game;
			return cfg;
		}()))
	{
		std::printf("second web server did not start\n");
		return 1;
	}
	State(tiles, "PPSA99251\t" + game + "\tSLUS-20001\tGod of War\tok\n");
	Alive(tiles);
	r = Http(own.Port(), "GET", tile_get, "");
	Check(Field(r, "phase") == "on" && Field(r, "can") == "none" && Has(r, "started from"), "own tile: On, but nothing to take it off with", r);
	r = Http(own.Port(), "POST", "/api/tile", "remove\n" + id);
	Check(r.rfind("409", 0) == 0 && Has(r, "running from its tile") && Requests(tiles).empty(), "own tile: POST remove is refused (409), nothing written", r);
	r = Http(own.Port(), "GET", "/api/tile?id=" + id2, "");
	Check(Field(r, "phase") == "off" && Field(r, "can") == "add", "own tile: another game's row works as always", r);
	own.Stop();

	if (!off.Start([&] {
			WebConfig cfg;
			cfg.game_dirs = {data + "/games"};
			cfg.settings_dir = data + "/settings";
			cfg.gs_ini = data + "/gs.ini";
			cfg.patches_dir = data + "/patches";
			cfg.covers_dir = data + "/covers";
			cfg.cache_dir = data + "/cache";
			cfg.token_path = data + "/token.txt";
			cfg.build_tag = "test";
			cfg.port = static_cast<uint16_t>(22000 + getpid() % 20000);
			cfg.change_log = data + "/logs/settings.log";
			cfg.top_dir = data;
			cfg.memcards_dir = data + "/memcards";
			return cfg; // tiles_dir empty: the flag file is not there
		}()))
	{
		std::printf("third web server did not start\n");
		return 1;
	}
	r = Http(off.Port(), "GET", tile_get, "");
	Check(r.rfind("200", 0) == 0 && Field(r, "available") == "false" && Field(r, "why") == "off", "game tiles off: available false (off)", r);
	r = Http(off.Port(), "POST", "/api/tile", "add\n" + id);
	Check(r.rfind("409", 0) == 0 && Requests(tiles).empty(), "game tiles off: POST add is refused, nothing written", r);
	// The other endpoints answer the same bytes with and without tiles_dir (the 1.50 page's diff stays green).
	for (const char* path : {"/api/games", "/api/settings?id=God%20of%20War%20(USA).iso", "/api/settings?id=@global", "/api/memcards", "/api/discs"})
	{
		const std::string with = Http(port, "GET", path, ""), without = Http(off.Port(), "GET", path, "");
		Check(with == without && with.rfind("200", 0) == 0, (std::string(path) + ": the same with and without game tiles").c_str(), with.substr(0, 200));
	}
	{
		std::string with = Http(port, "GET", "/api/state", ""), without = Http(off.Port(), "GET", "/api/state", "");
		Check(with.rfind("200", 0) == 0 && Has(with, "notify.tile_game_missing") && Has(with, "notify.tile_game_invalid"), "/api/state exports the two new text ids", with.substr(0, 100));
		Check(with == without, "/api/state: the same with and without game tiles", with.substr(0, 200));
	}
	off.Stop();
	web.Stop();
	std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
