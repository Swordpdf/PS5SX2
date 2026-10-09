// PS5 port frontend: the console side (fe_ps5.cpp).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct OrbisFrontendPaths
{
	std::string games_dir;    // /data/PCSX2/games
	std::string top_dir;      // /data/PCSX2 (older setups keep images here; lastgame.txt lives here)
	std::string settings_dir; // per-game settings, <image name>.ini
	std::string gs_ini;       // the shared settings
	std::string patches_dir;  // *.pnach, for the 16:9 badge
	std::string covers_dir;   // covers the user supplies (PCSX2's covers folder)
	std::string cache_dir;    // downloaded covers
	// The prefetch's downloads; on the shelf (vk-285-110), a try at the covers of games on USB drives
	// that are still missing after the jailbreak. Off with the nocoverdl flag.
	bool allow_download = true;
	bool sound = true;        // the shelf's key sounds (vk-285-47; off with the nomenusound flag)
	std::string settings_log; // vk-285-51: logs/settings.log, what the settings page changed
	// Test build 1 (vk-285-55):
	std::vector<std::string> usb_dirs; // folders on USB drives to list games from (orbis_usb_game_dirs)
	std::string usb_list;              // cache/usb-games.txt: the USB games seen, for the next start's cover prefetch
	std::vector<std::string> share_roots; // vk-285-156: the NFS shares' folders (OrbisNfs mount points), for covers
	int test_build = 0;                // > 0: a testing build; the shelf shows TESTING and this label
	std::string build_label;           // "Test build 1 · vk-285-55", or the plain tag
	std::string test_note;             // vk-285-105: testing builds, under the label (the testers' Discord)
	std::string logs_dir;              // logs/: the settings page's logs download reads the sessions there
	std::string report_header;         // that download's first lines: build, sources, console
	std::string serial_cache;          // vk-285-108: cache/chd-serials.txt, the CHD serials found (fe::SetSerialCacheFile)
	std::string gamedb_file;           // vk-285-113: resources/GameIndex.yaml, the games' names by serial (fe::SetGameDbFile)
	std::string memcards_dir;          // vk-285-113: memcards/, for the settings page's memory card list and creator
	// 2026-10-05: HD texture packs from archive.org (fe_texpacks.h): the textures folder they go into, where archive.org's
	// list is kept, and whether the sheet offers them at all (off with the notexpacks flag).
	std::string textures_dir;
	std::string texture_pack_list;
	bool texture_packs = true;
	// 2026-10-08: a game's patches and cheats from GitHub (fe_patchdl.h): the cheats folder, what was downloaded
	// (cache/online-patches.txt), and whether the sheet offers it (off with the noonlinepatches flag).
	std::string cheats_dir;
	std::string online_patch_manifest;
	bool online_patches = true;
	// vk-285-134 (AI-assisted): the PS2 BIOS (build 130's logs: 961 starts on 235 consoles without one). bios_check looks
	// again (a BIOS in an archive is taken out) and says whether PCSX2 finds one; bios_problem is then what was found
	// instead (a sentence); bios_dir where the BIOS goes. Unset: the shelf doesn't check.
	std::function<bool()> bios_check;
	std::function<std::string()> bios_problem;
	std::string bios_dir;
};

// 2026-10-08 (AI-assisted): what orbis_frontend_run returns for the sheet's "PS2 system menu": main-boot.cpp then boots the
// BIOS with no disc (the PS2's own menu: memory card browser, clock, language).
inline constexpr const char kOrbisSystemMenuPath[] = "@ps2-system-menu";

// Test build 1 (vk-285-55): the folders on USB drives PS5SX2 lists games from: each drive's root
// (/mnt/usb0 to /mnt/usb7) and, when present, its DVD/ and CD/ (Open PS2 Loader's layout) and
// PS5SX2/ folders, matched without regard to case. `when` ("before the jailbreak", ...) labels the
// [usb] lines it prints (null: silent).
std::vector<std::string> orbis_usb_game_dirs(const char* when);

// Test build 1 (vk-285-55): the watermark drawn over the game in testing builds: `line1` big,
// `line2` under it and `line3` (vk-285-105, may be null) smaller under that, white with a soft
// shadow, at the given opacities (line3 at line2's), sized for a 2160-line screen. RGBA8 pixels (R in
// the low byte, straight alpha), `w` x `h`. False if the font didn't load.
bool orbis_frontend_watermark(const char* line1, const char* line2, const char* line3, float alpha1, float alpha2,
	std::vector<uint32_t>& rgba, int& w, int& h);

// Before the HEN jailbreak: downloads the covers the cache lacks, for at most `budget_s` seconds.
// HTTPS works there and failed after the jailbreak (vk-285-41/42). `notify` (may be null) is told
// how many are coming before the first request. Returns how many it saved (0 when none were
// missing, or with downloads off). The covers of the games on USB drives come from
// cache/usb-games.txt, the list the shelf wrote last time: the drives appear only after the jailbreak.
int orbis_frontend_prefetch_covers(const OrbisFrontendPaths& paths, double budget_s, void (*notify)(const char*));

// vk-285-50: the settings page's web server (fe_web.cpp), for phones and PCs on the LAN. Start it
// once, after the jailbreak; it runs until the app ends, through the shelf and the game. The shelf
// shows its address as a QR code. The access token is kept in /data/PCSX2/webui_token.txt.
bool orbis_web_start(const OrbisFrontendPaths& paths, const char* build_tag);
// The disc image PCSX2 runs, for the page's "now playing".
void orbis_web_now_playing(const std::string& image_path);
// vk-285-139 (main-boot.cpp): the running game's disc, its discs, and a disc the settings page asks for (false: no game).
std::string orbis_current_disc();
std::vector<std::string> orbis_disc_set();
bool orbis_request_disc(const std::string& path);
// vk-285-113: the page's address as the QR code the shelf shows: `modules` (one byte a module, row by row), its
// side length in `size`, and the short "<ip>:<port>" in `shown`. GSRenderer.cpp draws it over the game when the
// PS5's browser can't be opened (L2 + D-pad down held for 2 s). False without a web server or a network.
bool orbis_web_qr(std::vector<unsigned char>& modules, int& size, std::string& shown);
// vk-285-113: the page's address for the console's own web browser (L2 + D-pad down held for 2 s opens it there):
// the LAN one, else the loopback one; `shown` is the short "<ip>:<port>" (no access token, safe to log). False
// without a web server.
bool orbis_web_browser_url(std::string& url, std::string& shown);
// How many requests the page's server has had, and how long ago the last one came (-1 while none has).
void orbis_web_request_stats(uint64_t& count, double& age_s);
// Log the next `n` requests to boot.log (who loads the page).
void orbis_web_log_next_requests(int n);

// vk-285-51: the settings log, logs/settings.log. The settings page writes what it changed there
// (fe_web.cpp) and the app what it did around it: starts, games, live applies, crashes and GPU
// hangs. Set its path once at startup; orbis_event_log() then appends "<local time>  <line>" with
// open()/write() only, so the crash handler and the GPU-hang exit can use it too.
void orbis_event_log_init(const std::string& path);

// vk-285-53: hides the system's launch screen (sce_sys/pic1.dds), which covers the app until then.
// The shelf calls it on its first frame; main() calls it again before the plain list or the game
// in case the shelf didn't run. Only the first call does anything; it returns that call's result.
int orbis_hide_splash();
extern "C" void orbis_event_log(const char* line);

// vk-285-110: the PS5's system language (system parameter 1: 0 Japanese, 1 English, 2 French, 3 Spanish,
// ...; English when the call fails), read once.
int orbis_ps5_language();

// vk-285-110: the shelf's and the notifications' text in that language (fe_i18n.h). `lang_dir` holds
// testers' fixes (<code>.txt); empty reads none (before the jailbreak /data isn't readable).
void orbis_frontend_set_language(const std::string& lang_dir);

// Shows the shelf and returns the picked image's path. *ran is false when the frontend could not
// start (no Vulkan display, say): the caller then shows the plain list instead. With no images the
// result is empty. One image opens the shelf too (vk-285-69, for its QR code); the caller's nomenu
// flag skips the shelf.
std::string orbis_frontend_run(const OrbisFrontendPaths& paths, const char* build_tag, bool* ran);

// vk-285-144 (AI-assisted): starts a game from outside the shelf (a PS2 disc put in the drive, orbis-shims/ProsperoDiscDump.cpp):
// the running shelf closes as if the image at `path` had been picked. False when no shelf is up to take it.
bool orbis_frontend_request_launch(const std::string& path);

// vk-285-145 (AI-assisted): free bytes in `dir` through libkernel's _fstatfs (libc's statfs makes the system call from the
// app's code, which the PS5 kills the app for). UINT64_MAX when it can't tell.
uint64_t orbis_frontend_free_bytes(const std::string& dir);
