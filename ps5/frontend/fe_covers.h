// PS5 port frontend: covers. A worker thread paints each game's spine and placeholder front, then
// finds its cover -- a file the user put in the covers folder, one downloaded before (the cache),
// or a download -- decodes it and picks the glow colour from it. The main thread polls the results
// and turns them into textures.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_games.h"
#include "fe_text.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <pthread.h>
#include <unordered_map>
#include <vector>

namespace fe
{
struct CoverImage
{
	enum Kind
	{
		Spine,
		Placeholder,
		Cover
	};
	int game = -1;
	Kind kind = Spine;
	int width = 0, height = 0;
	std::vector<uint8_t> rgba;
	float glow[3] = {0, 0, 0};
	bool has_glow = false;
	const char* source = ""; // "manual", "cache", "download"
};

struct CoverConfig
{
	std::string manual_dir; // covers the user supplies: <serial>.jpg/.png or <file name>.jpg/.png
	std::string cache_dir;  // downloads: <serial>.jpg, and <serial>.missing after a 404
	std::string url_template; // "${serial}" is replaced
	bool allow_download = true;
	// vk-285-110: the shelf downloads only for games on USB drives (the prefetch before the jailbreak
	// can't see those); every other cover comes from the prefetch, as before. vk-285-156: and for games on
	// NFS shares, which are mounted only after the jailbreak too (OnNetworkShare).
	bool download_usb_only = false;
};

// Fetches `url` into `out`; returns the HTTP status (200 on success), or a negative value when the
// network failed.
using DownloadFn = std::function<int(const std::string& url, std::vector<uint8_t>& out)>;

// vk-285-110: a cover file on disk, and which kind of place it was found in.
struct CoverFile
{
	std::string path;
	const char* source = ""; // "manual", "beside", "drive", "cache" or "art"
};

// vk-285-110: where a game's cover can be, best first:
//   1. manual: the covers folder, <serial>, <file name> or <title> .jpg/.png/.jpeg (as before);
//   2. beside: next to the disc image, under the image's name ("Game.iso" -> "Game.jpg", "Game.png");
//   3. drive:  a "covers" folder at the root of the game's USB drive, names as in 1;
//   4. cache:  a download (<serial>.jpg);
//   5. art:    Open PS2 Loader's ART folder at the root of the game's USB drive, "<ELF name>_COV"
//              .jpg/.png ("SLUS_213.51_COV.jpg"). These are often small, so a download still replaces
//              them.
// 2, 3 and 5 match names in any letter case (USB drives are FAT32 or exFAT). Only files that aren't
// empty count. Each folder is listed once per finder: keep one finder for a whole pass over the
// games, on one thread.
class CoverFinder
{
public:
	CoverFinder(std::string manual_dir, std::string cache_dir);

	// The cover files `g` has, best first (empty when none).
	std::vector<CoverFile> Find(const GameInfo& g);

	// The first of them, or an empty path (stops looking at the first one found).
	CoverFile Best(const GameInfo& g);

private:
	using Names = std::unordered_map<std::string, std::string>; // lower-case name -> the name on disk
	std::vector<CoverFile> Search(const GameInfo& g, bool first_only);
	const Names& List(const std::string& dir);
	std::string Subdir(const std::string& dir, const char* lower_name);
	bool Lookup(const std::string& dir, const std::string& base, const char* source, std::vector<CoverFile>& out);

	std::string m_manual_dir, m_cache_dir;
	std::unordered_map<std::string, Names> m_lists;
};

// vk-285-110: "/mnt/usb3" for a path on USB drive 3, else "".
std::string UsbDriveRoot(const std::string& path);

// vk-285-156 (AI-assisted; testers: NFS games had no covers): a path on one of the app's NFS shares ("/nfs/<host>/...",
// OrbisNfs.cpp), which only exist after the jailbreak, like USB drives.
bool OnNetworkShare(const std::string& path);

// vk-285-156: the NFS shares' folders ("/nfs/192.168.1.10/volume1/PS2"), so a share's root works like a USB drive's for
// covers: its covers/ folder and Open PS2 Loader's ART/ folder. Set by the shelf before it starts the covers.
void SetShareRoots(const std::vector<std::string>& roots);

// vk-285-156: the root of the drive or share a game is on (UsbDriveRoot, else the longest share root holding it), or "".
std::string DriveRoot(const std::string& path);

// vk-285-110: Open PS2 Loader's name for a game, "SLUS_213.51" for "SLUS-21351"; "" for a serial of
// another shape.
std::string OplGameId(const std::string& serial);

class CoverService
{
public:
	~CoverService();
	void Start(const std::vector<GameInfo>& games, const Fonts* fonts, const CoverConfig& cfg, DownloadFn download);

	// Asks the worker to stop and waits up to `timeout_ms` (forever when negative); false if it is
	// still busy then (a download that hasn't timed out), in which case the object must stay alive.
	bool Stop(int timeout_ms = -1);

	// Tells the worker to finish after the step it is in; Stop then waits for it.
	void RequestStop() { m_stop = true; }

	// Main thread: the next finished image, if any.
	bool Poll(CoverImage& out);

	// Main thread: games nearer `selected` go first.
	void SetSelected(int selected);

	// A line for the UI: "" when idle, else e.g. "Downloading covers  3 / 11".
	std::string Status() const;

	// Paints a placeholder front / a spine (also used before the worker runs).
	static void PaintSpine(const Fonts& fonts, const GameInfo& g, CoverImage& out);
	static void PaintPlaceholder(const Fonts& fonts, const GameInfo& g, CoverImage& out);

	// Decodes an image file's bytes to RGBA8, at most `max_h` rows tall; false if it isn't one.
	static bool Decode(const std::vector<uint8_t>& bytes, int max_h, CoverImage& out, int max_dimension = 0);

	// The games whose cover is none of: the user's own file (CoverFinder's manual, beside or drive), a
	// cached download, a 404 in the last two weeks. For fetching them before the HEN jailbreak
	// (fe_ps5.cpp, vk-285-44). A game with only an OPL ART cover counts as missing (vk-285-110).
	static std::vector<int> MissingCovers(const std::vector<GameInfo>& games, const CoverConfig& cfg);

	// Downloads those covers into the cache, one after another, until `budget_s` seconds have
	// passed or the network fails. Only images that decode are kept. Returns how many were saved.
	static int Prefetch(const std::vector<GameInfo>& games, const std::vector<int>& which, const CoverConfig& cfg,
		const DownloadFn& download, double budget_s);

private:
	static void* ThreadMain(void* self);
	void Run();
	// vk-285-110: the covers on disk for every game first, then the downloads, so a slow or failing
	// download never holds back a cover that is already there. `upgrade`: a download is still worth
	// trying (no cover, or only an OPL ART one).
	bool FindLocalCover(CoverFinder& finder, int index, CoverImage& out, bool& upgrade);
	bool MayDownload(int index) const;
	bool DownloadCover(int index, CoverImage& out);
	int NextGame(const std::vector<bool>& done) const;

	std::vector<GameInfo> m_games;
	const Fonts* m_fonts = nullptr;
	CoverConfig m_cfg;
	DownloadFn m_download;
	pthread_t m_thread{};
	bool m_thread_started = false;
	std::atomic<bool> m_stop{false};
	std::atomic<bool> m_finished{false};
	std::atomic<int> m_selected{0};
	mutable std::mutex m_mutex;
	std::deque<CoverImage> m_results;
	int m_download_total = 0, m_download_done = 0;
	bool m_offline = false;
	bool m_busy = false;
};
} // namespace fe
