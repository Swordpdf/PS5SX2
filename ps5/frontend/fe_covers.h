// PS5 port frontend: covers. A worker thread paints each game's spine and placeholder front, then
// finds its cover -- a file the user put in the covers folder, one downloaded before (the cache),
// or a download -- decodes it and picks the glow colour from it. The main thread polls the results
// and turns them into textures. 2026-10-10 (AI-assisted): only for the games the shelf asks for (Want: those near the
// selection), so memory follows the shelf's window rather than the library (a tester's ~2,600 NFS games); a second
// thread downloads the missing covers into the cache, nearest the selection first.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_coverfit.h"
#include "fe_games.h"
#include "fe_launchbox.h"
#include "fe_text.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
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
		Cover,
		Back,     // 2.11: the case's back, painted (title and serial) until RealBack replaces it
		RealBack, // 2.11: a back cover the user (or a download) supplied
		Disc      // 2.11: the disc art of a disc being copied (game -1): a round label with a transparent surround, alpha kept
	};
	int game = -1;
	Kind kind = Spine;
	int width = 0, height = 0;
	std::vector<uint8_t> rgba;
	float glow[3] = {0, 0, 0};
	std::string serial; // 2.11: Disc: the serial it is for
	bool has_glow = false;
	const char* source = ""; // "manual", "cache", "download"
	// 2.11: Back: painted with the note "No back cover for this game" (true: none can come) or without it (false: one may still arrive, the placeholder
	// says nothing about it).
	bool none = false;
};

struct CoverConfig
{
	std::string manual_dir; // covers the user supplies: <serial>.jpg/.png or <file name>.jpg/.png
	std::string cache_dir;  // downloads: <serial>.jpg, and <serial>.missing after a 404
	std::string url_template; // "${serial}" is replaced
	// 2.11: where back covers come from, when the player has a source ("" for none: only files the player puts in covers/ are used);
	// the same "${serial}"; a download is kept as cache/<serial>_back.jpg.
	std::string back_url_template;
	bool allow_download = true;
	// vk-285-110: the shelf downloads only for games on USB drives (the prefetch before the jailbreak
	// can't see those); every other cover comes from the prefetch, as before. vk-285-156: and for games on
	// NFS shares, which are mounted only after the jailbreak too (OnNetworkShare).
	bool download_usb_only = false;
	// 2.11: LaunchBox's art by serial (fe_launchbox.h), null for none. Its region-exact scans come before url_template, the other regions' after it.
	std::shared_ptr<const LaunchboxIndex> launchbox;
	// 2.11: a cached cover the index has a sure match for (LaunchboxIndex::CanUpgrade) is replaced by LaunchBox's once, when its game comes near
	// the selection; the old file is kept as <serial>.jpg.old. Off with the flag file flags/nolbupgrade (fe_ps5.cpp).
	bool launchbox_upgrade = true;
	// 186 (the owner, 2026-11-02: "switch back to the old covers (only for the front cover)"): false = the configured source (url_template, xlenore's
	// ps2-covers) comes first for fronts again and LaunchBox only for a game it has none for; a cached front that is LaunchBox's (cache/<serial>.lb) is
	// switched back once when its game comes near the selection (RevertCover). Backs, spines and disc art stay LaunchBox's. True (the flag file
	// flags/lbfront, fe_ps5.cpp): 2.11's order, LaunchBox's own-region scan first, with its upgrades.
	bool launchbox_front_first = false;
	// 2.11: a front, back or spine picture is made the shape of its face of the case (fe_coverfit.h) as it is decoded. Off with the flag file
	// flags/nocoverfit (fe_ps5.cpp): every picture then fills its face as it did before, whatever its proportions.
	bool fit_covers = true;
	// 2.11 (swordpdf: "back covers are not loading instantly"): the back covers of the cases beside the picked one are fetched into the cache ahead of
	// the flip (PrefetchBacks, a few files at a time, one request at a time). Off with the flag file flags/nobackprefetch (fe_ps5.cpp).
	bool back_prefetch = true;
	// 2.11: after a network failure downloads wait before they are tried again (30 s, doubling to 5 min; LaunchBox: 2 min to 10 min, a rate limit 15 min).
	// 0: those times; the tests set a few hundred milliseconds.
	int net_retry_ms = 0;
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

	// 2.11: the back covers a game has, best first: the front's places and names with "_back", "-back", " back" or "_b" after them
	// ("SLUS-21351_back.jpg" in covers/, "Game_back.jpg" beside "Game.iso", the drive's covers/, a download in the cache).
	std::vector<CoverFile> FindBack(const GameInfo& g);

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
// OrbisNfs.cpp) or SMB shares ("/smb/...", OrbisSmb.cpp), which only exist after the jailbreak, like USB drives.
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

	// 2026-10-10 (AI-assisted): main thread. Want: the shelf wants the game's spine, placeholder and cover (it came near
	// the selection). Forget: it no longer does (its textures are gone); what was painted for it and not yet polled is
	// dropped, and a download for it only goes to the cache. Needs proper testing on the console with a large library.
	void Want(int index);
	void Forget(int index);
	// 2.11: main thread. The shelf looks at this game's back (it has been the picked one a moment): its back cover is looked for
	// (files, then a download) and comes as a RealBack image; nothing when there is none. `download` is the permission to use the network: false asks
	// for the files on disk (the cache included) only; a game still waiting for that permission is fetched when the call comes again with true (the
	// shelf gives it after the case has been picked 0.35 s, or when the player starts to turn it over, so scrolling past games costs no request).
	// Asking again for what was asked already does nothing. When the game has no back and none can come (no source, downloads off, a 404 in the last two
	// weeks) the painted back is sent again with its note ("No back cover for this game"; CoverImage::none). ForgetBack takes the request back (the
	// textures are the shelf's to free).
	void WantBack(int index, bool download = false);
	void ForgetBack(int index);
	// 2.11: main thread. The backs of these games (nearest the picked one first, at most 8) are fetched into the cache, for the flip to find them there:
	// the download thread takes them after the picked game's back and before the spines, one request at a time, skipping what is on disk, a 404 in
	// the last two weeks and a source that is paused. No image is made. Replaces the earlier list. Nothing with CoverConfig::back_prefetch off.
	void PrefetchBacks(const std::vector<int>& nearest_first);
	// 2.11: main thread. A disc is being copied: its disc art (a round label, transparent around it) is looked for (covers/<serial>_disc.png, the
	// cache, then LaunchBox) and comes as a Disc image (game -1, alpha kept); nothing when there is none.
	void RequestDisc(const std::string& serial);

	// 2.11: the addresses to try for a game's front cover, back cover and disc art, best first: LaunchBox's art from the serial's own region, the
	// configured source (url_template / back_url_template), then LaunchBox's art from other regions. Empty when there is no source.
	static std::vector<std::string> FrontUrls(const CoverConfig& cfg, const std::string& serial);
	static std::vector<std::string> BackUrls(const CoverConfig& cfg, const std::string& serial);
	static std::vector<std::string> DiscUrls(const CoverConfig& cfg, const std::string& serial);
	// 2.11 (swordpdf: "what about the side cover? that also exists on the site"): LaunchBox's spine for the case's side, when it has one.
	static std::vector<std::string> SpineUrls(const CoverConfig& cfg, const std::string& serial);
	// Like Decode, but the alpha channel is kept and no glow colour is worked out: the disc art.
	static bool DecodeWithAlpha(const std::vector<uint8_t>& bytes, int max_h, CoverImage& out, int max_dimension = 0);

	// A line for the UI: "" when idle, else e.g. "Downloading covers  3 / 11".
	std::string Status() const;

	// Paints a placeholder front / a spine (also used before the worker runs).
	static void PaintSpine(const Fonts& fonts, const GameInfo& g, CoverImage& out);
	static void PaintPlaceholder(const Fonts& fonts, const GameInfo& g, CoverImage& out);
	// 2.11: `none`: the note "No back cover for this game / Put <serial>_back.jpg in covers/" is painted (no back can come); else the card says only the
	// title and serial, for the moment before a back that may exist arrives.
	static void PaintBackPlaceholder(const Fonts& fonts, const GameInfo& g, CoverImage& out, bool none = true);

	// Decodes an image file's bytes to RGBA8, at most `max_h` rows tall; false if it isn't one. 2.11: `fit` makes the result the shape of the case face it goes
	// on (fe_coverfit.h: left alone when nearly right, else cropped or padded, never stretched).
	static bool Decode(const std::vector<uint8_t>& bytes, int max_h, CoverImage& out, int max_dimension = 0, CoverFit fit = CoverFit::None);

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
	static void* DownloadThreadMain(void* self);
	void Run();
	void RunDownloads(); // 2026-10-10: the downloads, on a thread of their own so a slow one never holds up the painting
	int NextWanted() const; // m_mutex held: the wanted game nearest the selection still to paint, or -1
	int NextBack() const;   // m_mutex held: the game whose back cover the shelf asked for (WantBack) and is still to look for, or -1
	bool FindLocalBack(CoverFinder& finder, int index, CoverImage& out); // 2.11
	// 2.11 (painting thread): the answer to a back cover asked for: the file on disk as a RealBack image, else the painted back with its note when no back
	// can come, else (a download may fetch it) the request goes on to the download thread, at once when the shelf gave the permission (WantBack's
	// `download`), else when it does. The painting pass calls AnswerNextBack between its stages, so a back waits for a decode, not for a whole game.
	void AnswerBack(CoverFinder& finder, int back);
	bool AnswerNextBack(CoverFinder& finder); // the nearest back waiting for its answer; false when there is none
	bool BackKnownNone(int index) const;      // no back cover can come for this game: no source, downloads off, or a 404 for it in the last two weeks
	// 2.11: `keep` gets the bytes to write to the cache (after the image is handed over: the flash write stays out of the way to the screen); `none`: the
	// game was marked as having no back (the painting thread then paints the note). `no_answer`: a request got no answer (the connection failed), so the
	// picked game's back is worth asking for again (each such failure counts towards a pause, NoteResult, so this cannot go on for ever).
	bool DownloadBack(int index, CoverImage& out, std::vector<uint8_t>& keep, bool& none, bool& no_answer);
	bool PrefetchBack(int index); // 2.11: the back into the cache, no image
	bool Push(CoverImage& img); // to the results once there's room; false (dropped) when the game is forgotten or stopping
	bool PushDisc(CoverImage& img); // 2.11: the disc art (no game): false when stopping
	void NoteResult(const std::string& url, int status, bool good); // 2.11: what a download's outcome means for the sources (see m_offline_until_ms)
	// 2.11 (swordpdf: "back covers are not loading instantly"): downloads are paused for a while, not for the session, after a failure. Offline: the
	// network or the configured source; LbPaused: LaunchBox.
	static int64_t NowMs();
	bool Offline() const { return m_offline_until_ms.load() > NowMs(); }
	bool LbPaused() const { return m_lb_until_ms.load() > NowMs(); }
	// 2.11: the download thread's other jobs, one per call, between the bulk covers: the disc art of a disc being copied, then the back cover of
	// the picked game, then the backs of its neighbours (PrefetchBacks), the spine, then a cached cover LaunchBox can improve. False when there was
	// nothing to do (or the downloads are paused).
	bool ServeRequest();
	bool DownloadDisc(const std::string& serial, CoverImage& out);
	bool FindLocalSpine(int index, CoverImage& out); // 2.11: covers/<serial>_spine.jpg|png, or one downloaded before
	bool DownloadSpine(int index, CoverImage& out);   // 2.11
	int NextSpineDownload() const;                    // m_mutex held
	bool FindLocalDisc(const std::string& serial, CoverImage& out);
	bool WantsUpgrade(int index, const char* source) const;
	bool UpgradeCover(int index, CoverImage& out);
	bool WantsRevert(int index, const char* source) const; // 186: the same pass, the other way (launchbox_front_first false)
	bool RevertCover(int index, CoverImage& out);
	int NextBackDownload() const; // m_mutex held: the game nearest the selection whose back cover is to be downloaded, or -1
	int NextUpgrade() const;      // m_mutex held: the same for a cached cover to improve
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
	pthread_t m_thread{}, m_dl_thread{};
	bool m_thread_started = false, m_dl_thread_started = false;
	std::atomic<bool> m_stop{false};
	std::atomic<int> m_running{0}; // threads not finished yet
	std::condition_variable m_wake; // Want, Stop
	std::vector<uint8_t> m_live, m_todo; // m_mutex: per game, wanted by the shelf / still to paint
	std::vector<uint8_t> m_back_todo;    // m_mutex: per game, its back cover asked for (WantBack) and not yet looked for
	// 2.11, m_mutex: per game, what the shelf asked of its back (0 nothing, 1 the files on disk, 2 and the network too), and whether the lookup missed and
	// waits for the permission to download.
	std::vector<uint8_t> m_back_ask, m_back_miss;
	std::vector<uint8_t> m_back_note; // 2.11, m_mutex: per game, the painted back with its note ("No back cover") is out (a picked game is not given it again)
	std::vector<int> m_back_ring; // 2.11, m_mutex: PrefetchBacks' list, nearest first
	std::unique_ptr<CoverFinder> m_dl_finder; // 2.11: the download thread's own finder (RunDownloads): PrefetchBack asks it for the backs on disk
	std::vector<uint8_t> m_spine_dl; // 2.11, m_mutex: per game, a real spine to download (none on disk)
	std::vector<uint8_t> m_back_dl, m_upgrade_todo; // 2.11, m_mutex: per game, a back cover to download / a cached cover LaunchBox can improve
	std::string m_disc_serial;                      // 2.11, m_mutex: RequestDisc
	bool m_disc_todo = false, m_disc_dl = false;    // 2.11, m_mutex: to look for on disk (paint thread) / to download (download thread)
	std::atomic<int> m_selected{0};
	mutable std::mutex m_mutex;
	std::deque<CoverImage> m_results;
	int m_download_total = 0, m_download_done = 0;
	// 2.11 (review): which source failed decides what stops. LaunchBox failing three times running (or answering 429) pauses LaunchBox only; any other
	// source failing is the old "network down" and pauses every download. 2.11 (swordpdf: "back covers are not loading instantly"): a pause ends by
	// itself (until the steady clock reads these milliseconds) and doubles with each failure in a row, instead of lasting the session; a download that
	// delivers a picture ends the run of failures.
	std::atomic<int64_t> m_offline_until_ms{0}, m_lb_until_ms{0};
	std::atomic<int> m_offline_strikes{0}, m_lb_strikes{0};
	std::atomic<int> m_lb_failures{0};
	bool m_busy = false;
};
} // namespace fe
