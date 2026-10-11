// PS5 port frontend: the settings page's web server (vk-285-50). A small HTTP/1.1 server on the
// LAN: a phone or PC opens the page the shelf's QR code points at, lists the games with their
// covers and edits each game's settings file (settings/<image>.ini) or the shared gs.ini. The
// running game picks the change up by itself: PCSX2's GS thread polls both files and applies them
// at the next vsync (GSRenderer.cpp OrbisLiveTune -> main-boot.cpp orbis_reload_gs_ini_cpu).
//
// Plain BSD sockets, one thread, one request per connection; the same code runs on the PC in
// fe_host for testing. Every /api/ request uses the same-site checks; the console address is sufficient.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_covers.h"
#include "fe_games.h"
#include "fe_game_achievements.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

namespace fe
{
// A file served as it is (the page, its fonts and icons).
struct WebAsset
{
	std::string path; // "/", "/fonts/roboto.ttf", ...
	std::string type; // "text/html; charset=utf-8", ...
	const uint8_t* data = nullptr;
	size_t size = 0;
};

struct WebConfig
{
	std::vector<std::string> game_dirs; // the images: games/, then the top folder
	std::string settings_dir;           // settings/<image stem>.ini
	std::string gs_ini;                 // the settings every game shares
	std::string patches_dir;            // <serial>_<crc>.pnach
	std::string cheats_dir;             // 2026-10-08: cheats' <serial>_<crc>.pnach ("" leaves cheats off the page)
	std::string covers_dir;             // the user's covers: <serial|stem|title>.jpg/.png
	std::string cache_dir;              // downloaded covers: <serial>.jpg
	std::string token_path;             // unused since vk-285-118 (no key); kept for the host harness's build of the 1.50 page
	std::string build_tag;
	uint16_t port = 8844;               // the first port tried; the next seven if it is taken
	std::vector<WebAsset> assets;
	// vk-285-51: the recommended settings (assets/presets.ini: "[@global]" and "[<serial>]"
	// sections) that the page's Recommended button writes, and the log every change goes to.
	std::string presets;
	std::string change_log;             // "" for none
	// Test build 1 (vk-285-55): the logs download (/api/report) and the tester's notes (/api/note).
	std::string logs_dir;               // logs/: boot.log, emulog.txt, stderr.log and their earlier sessions
	std::string top_dir;                // /data/PCSX2: gs.ini, live.ini, flags/, settings/, patches/, cheats/
	std::string memcards_dir;           // vk-285-113: memcards/, the cards PCSX2 uses ("" for none: no cards page)
	std::string report_header;          // the report's first lines: build, sources, console
	std::function<GameAchievementsState()> achievements; // Runtime snapshot, no client pointers (AI-assisted).
	std::function<std::vector<uint8_t>(uint32_t)> achievement_badge;
	int test_build = 0;                 // names the report file "PS5SX2-test<N>-..."
	// vk-285-139: changing discs while a game runs (/api/discs, /api/disc). The disc in now and the game's set ("" and empty
	// in the menu); change_disc asks for one (any image the page lists), false when no game runs.
	std::function<std::string()> current_disc;
	std::function<std::vector<std::string>()> disc_set;
	std::function<bool(const std::string& path)> change_disc;
};

// vk-285-51: appends "<date time>  <line>" to the settings log (logs/settings.log), which keeps
// what changed, when and from where, and what the app did next (app and game starts, live
// applies, crashes). At 512 KiB it moves to <name>.1.log and starts again. Thread-safe.
void AppendSettingsLog(const std::string& path, const std::string& line);

// The settings log's clock: UTC -> the console's local time. Null means the C library's own
// localtime (the PC harness); fe_ps5.cpp sets the PS5's.
extern long long (*g_utc_to_local)(long long utc);

class WebServer
{
public:
	~WebServer();

	bool Start(const WebConfig& cfg);
	void Stop();

	uint16_t Port() const { return m_port; }
	const std::string& Token() const { return m_token; } // "" since vk-285-118 (the host harness sends it to the 1.50 page)

	// The disc image PCSX2 runs ("" while the shelf is up).
	void SetNowPlaying(const std::string& image_path);

	// The QR code's address, "http://<ip>:<port>/" (vk-285-118: no key), and the short "<ip>:<port>" shown under it.
	// False (both empty) without a network.
	bool Address(std::string& url, std::string& shown) const;

	// vk-285-113: for a browser on the console itself, "http://127.0.0.1:<port>/".
	std::string LoopbackUrl() const;

	// vk-285-113: how many requests came in and how long ago the last one was (-1 while none has). The browser
	// launch (main-boot.cpp) watches this to see the page load and to know when the browser is closed.
	void RequestStats(uint64_t& count, double& age_s) const;
	// Log the next `n` requests ("[web] GET /api/state from <ip>") to see who loads the page.
	void LogNextRequests(int n) { m_log_requests = n; }

private:
	struct Request
	{
		std::string method, path, query, body, token;
		std::string host, origin, fetch_site; // vk-285-118: the Host, Origin and Sec-Fetch-Site headers (see Route)
		std::string peer; // the client's address, for the settings log
	};
	struct Response
	{
		int status = 200;
		std::string type = "application/json";
		std::string body;
		const uint8_t* data = nullptr; // a static asset instead of body
		size_t size = 0;
		std::string cache = "no-store";
		std::string disposition;      // test build 1: "attachment; filename=..." for the logs download
		int send_timeout_s = 0;       // a longer send timeout than the usual 3 s (the logs download)
	};

	void Run();
	void Serve(int fd, const char* peer);
	void Route(const Request& req, Response& res);
	bool LoadToken();

	const GameInfo* FindGame(const std::string& id, std::vector<GameInfo>& games);
	std::vector<GameInfo> Games();
	// vk-285-110: the covers' lookup (CoverFinder), kept between requests so the page's one request per
	// cover doesn't list the same USB folders again each time; made again after 10 s, like the list.
	CoverFinder& Covers();

	void ApiState(Response& res);
	void ApiGames(Response& res);
	void ApiAchievements(Response& res);
	void ApiAchievementBadge(const Request& req, Response& res);
	void ApiCover(const Request& req, Response& res);
	void ApiSettings(const Request& req, Response& res);
	void ApiSave(const Request& req, Response& res);
	void ApiRecommended(const Request& req, const GameInfo* g, const std::string& path, Response& res);
	void ApiReport(const Request& req, Response& res); // test build 1: one text file with the logs
	void ApiNote(const Request& req, Response& res);   // test build 1: a tester's note, into settings.log
	void ApiDiscs(Response& res);                      // vk-285-139: the disc in, the game's discs and every image
	void ApiDisc(const Request& req, Response& res);   // vk-285-139: put one in (the body: its path)
	void ApiMemcards(Response& res);                   // vk-285-113: the cards in memcards/
	void ApiMemcardCreate(const Request& req, Response& res); // vk-285-113: "create <8|16|32|64> <name>"
	void Log(const Request& req, const std::string& what);

	WebConfig m_cfg;
	int m_listen = -1;
	uint16_t m_port = 0;
	std::string m_token;
	pthread_t m_thread{};          // vk-285-113: a thread of its own with a roomy stack (see Start)
	bool m_thread_started = false;
	static void* ThreadMain(void* self);
	std::atomic<bool> m_stop{false};
	std::atomic<uint64_t> m_requests{0};
	std::atomic<double> m_last_request{-1.0}; // steady clock seconds
	std::atomic<int> m_log_requests{0};
	std::unique_ptr<CoverFinder> m_covers; // the server thread only (vk-285-110)
	double m_covers_time = -1e9;

	mutable std::mutex m_mutex; // the fields below
	std::string m_now_playing;
	std::vector<GameInfo> m_games;
	double m_games_time = -1e9;
	std::map<std::string, std::pair<uint64_t, std::string>> m_serials; // path -> (size, serial): USB drives are slow
};

// The IPv4 address other devices reach this machine on: the one the default route uses, else the
// one a private-range route uses; "" without a network.
std::string LocalAddress();
} // namespace fe
