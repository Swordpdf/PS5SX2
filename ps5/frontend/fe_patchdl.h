// PS5 port frontend: a game's patches and cheats from the internet (2026-10-08, AI-assisted).
//
// Testers asked for "an option to download patches from https://github.com/PCSX2/pcsx2_patches and
// https://github.com/Gabominated/PCSX2 directly from the app", and for cheats per game. On a game's sheet, the "Get patches
// and cheats" row (Cross) fetches, for that game's serial and CRC, the file each source has:
//   PCSX2      github.com/PCSX2/pcsx2_patches             patches/<serial>_<crc>.pnach      -> patches/<serial>_<crc>.pnach
//   Gabominated github.com/Gabominated/PCSX2              PCSX2 Patches/<serial>_<crc>.pnach -> patches/<serial>_<crc>_gabominated.pnach
//   cheats     github.com/xs1l3n7x/pcsx2_cheats_collection cheats/<serial>_<crc>.pnach       -> cheats/<serial>_<crc>.pnach
// The CRC is PCSX2's (the XOR of the boot executable's 32-bit words, ElfObject::GetCRC), worked out from the disc image.
// Gabominated's groups are renamed "<name> (Gabominated)" and its unnamed lines put in a "[Gabominated]" group, so that
// nothing applies by itself and nothing clashes with PCSX2's groups of the same name (PCSX2 loads every
// <serial>_<crc>*.pnach and enables groups by name). A file this app didn't write (the user's own, or one a release
// ships) is never replaced; one it wrote is replaced when the source changed it (cache/online-patches.txt keeps what it
// wrote). The sheet then lists the groups under Patches and Cheats, each off until turned on. Needs proper testing on the
// console.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_games.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fe
{
struct OnlinePatchStatus
{
	enum class State
	{
		Idle,    // nothing asked yet
		Working, // `message` says what
		Done,    // `message` says what came
		Failed,  // `message` says why
	};
	State state = State::Idle;
	std::string message;
};

// What the app asks of the downloader (AppConfig::online_patches); unset on a build without one.
struct OnlinePatchService
{
	std::function<OnlinePatchStatus(const std::string& serial)> status;
	std::function<bool(const GameInfo& game)> begin;
	explicit operator bool() const { return static_cast<bool>(status); }
};

struct OnlinePatchPlatform
{
	// GET a small file: the HTTP status, or < 0 when the request failed.
	std::function<int(const std::string& url, std::string& body)> get_text;
	std::function<void(const std::string& line)> log;
	std::function<void()> thread_start; // first thing on the worker
};

// One source of files.
struct OnlinePatchSource
{
	const char* name;     // "PCSX2"
	const char* url;      // with "%s" for "<serial>_<crc>"
	bool cheats;          // into the cheats folder (else the patches folder)
	const char* suffix;   // the local file's name after "<serial>_<crc>" ("" or "_gabominated")
	const char* tag;      // groups renamed "<name> (<tag>)" ("" leaves them)
};
const std::vector<OnlinePatchSource>& OnlinePatchSources();
// A source's URL for "<serial>_<crc>": its first "%s" replaced by `key`. vk-285-134b: not through snprintf, since the URL
// isn't a format: Gabominated's holds "%20" ("PCSX2%20Patches"), which the console's snprintf read as a conversion, and
// every Gabominated request failed ("no connection") while PC builds (glibc prints an unknown conversion as it is) worked.
std::string OnlinePatchUrl(const OnlinePatchSource& src, const std::string& key);

// PCSX2's game CRC of a boot executable: the XOR of its little-endian 32-bit words (a tail of 1-3 bytes left out).
uint32_t ElfCrc(const std::vector<uint8_t>& elf);
// A pnach file's text: no NUL bytes, under 1 MB, and at least one patch= line (an HTML error page isn't one).
bool LooksLikePnach(const std::string& text);
// vk-285-135: what an answer is. PCSX2's file for Ratchet & Clank (SCUS-97199_CE4933D0) has every line commented out
// ("//patch=", its widescreen patch breaks textures): a pnach with nothing to apply (Empty), which the sheet now reports
// as nothing online for the game, not as a file that "isn't a patch file" (NotPnach: no pnach lines at all).
enum class PnachKind
{
	Patches, // at least one patch line PCSX2 applies
	Empty,   // pnach text (gametitle=, a group, a commented-out patch line), but nothing to apply
	NotPnach,
};
PnachKind ClassifyPnach(const std::string& text);
// The "[name]" groups renamed "[name (tag)]", and patch lines before the first group put under "[unnamed]".
std::string TagPnachGroups(const std::string& text, const std::string& tag, const std::string& unnamed);
// How many groups ("[...]" lines) a pnach text has, and whether it has patch lines outside any group.
int CountPnachGroups(const std::string& text, bool* unnamed = nullptr);

class OnlinePatches
{
public:
	// manifest: what this app wrote (cache/online-patches.txt).
	OnlinePatches(OnlinePatchPlatform platform, std::string patches_dir, std::string cheats_dir, std::string manifest);
	~OnlinePatches();
	OnlinePatches(const OnlinePatches&) = delete;
	OnlinePatches& operator=(const OnlinePatches&) = delete;

	OnlinePatchStatus Status(const std::string& serial) const;
	bool Begin(const GameInfo& game);
	// Waits up to `wait_ms` for the job in flight; true when the worker ended.
	bool Stop(int wait_ms);
	OnlinePatchService Service();

	// One job, on the calling thread (the worker's body; the PC test calls it directly): the game's CRC, then each source.
	OnlinePatchStatus Fetch(const GameInfo& game);
	// The CRC worked out from a disc image (0 with `error` when its executable can't be read).
	static uint32_t DiscCrc(const std::string& image, std::string& error);
	// Fetch with a CRC already known (the test's games have no executable).
	OnlinePatchStatus FetchWithCrc(const std::string& serial, uint32_t crc);

private:
	void Run();
	void SetStatus(const std::string& serial, OnlinePatchStatus::State state, const std::string& message);
	void Log(const std::string& line) const;
	void LoadManifest();
	void SaveManifest() const;

	OnlinePatchPlatform m_platform;
	std::string m_patches, m_cheats, m_manifest_path;
	std::map<std::string, std::string> m_manifest; // path -> MD5 of what this app wrote there
	std::thread m_thread;
	mutable std::mutex m_mutex;
	std::condition_variable m_cv;
	bool m_stop = false, m_running = false;
	std::deque<GameInfo> m_queue;
	std::map<std::string, OnlinePatchStatus> m_status; // by serial
};
} // namespace fe
