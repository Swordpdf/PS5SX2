// PS5 port frontend: HD texture packs from archive.org (2026-10-05, AI-assisted).
//
// swordpdf: "i wanna be able to press square on a game and download the texture pack, unzip it if needed and place it on
// the appropriate folder, automatically", archive.org first. archive.org's "PCSX2 HD Texture Packs" item
// (pcsx2-hd-texture-packs) holds one RAR 5 file a game, named "<title> (<region>) [<serial>] <edition>.rar", with
// "<pack>/<serial>/replacements/..." inside (some also have the maker's dumps/ beside replacements/). Its file list
// (archive.org's metadata API) gives each file's size and MD5.
//
// On the sheet's "HD texture pack" row (Square on a game): Cross downloads the game's pack while the shelf is up, in
// 32 MB ranges that pick up where they stopped (a game starts the app over, so a download goes on at the next shelf),
// checks it against archive.org's MD5, unpacks it with libarchive (ps5/third_party/libarchive) into
// textures/.ps5sx2-unpack, moves <serial>/ into the textures folder, writes a marker file there and turns on Texture
// replacements in the game's settings file. Triangle twice cancels a download or removes a pack this app installed.
// The platform (HTTP, free space, the popup) is behind TexturePackPlatform: fe_ps5.cpp on the console, fe_host.cpp
// and host/texpacks_test.cpp on a PC. Needs proper testing on the console.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fe
{
// The archive.org item and where its files are.
constexpr const char* kTexturePackItem = "pcsx2-hd-texture-packs";

struct TexturePack
{
	std::string name;                 // the file's name in the item
	std::string title;                // the name without its serials and extension
	std::string label;                // the edition ("HD Remaster Definitive Edition"); the title when there is none
	std::vector<std::string> serials; // every "[SLUS-20776]" in the name (a two-disc pack names both)
	uint64_t bytes = 0;
	std::string md5; // lowercase hex, from the item's file list
	uint32_t files = 0; // the archive's file count, from the list (0 when it doesn't say)
};

// The item's file list (https://archive.org/metadata/<item>) -> its packs: original .rar and .zip files whose name holds
// a serial. False (and `error`) when the text isn't that list.
bool ParseTexturePackCatalog(const std::string& json, std::vector<TexturePack>& out, std::string& error);
std::string TexturePackMetadataUrl();
std::string TexturePackUrl(const TexturePack& pack);
std::string PercentEncode(const std::string& text);
std::string PercentDecode(const std::string& text);
// "SLUS-20776" for a well-formed serial (any case), else "".
std::string NormalSerial(const std::string& text);
// Where an archive entry goes, relative to the textures folder ("SLUS-20776/replacements/01/x.png"), or "" to leave it
// out: the part from a serial folder on (not its dumps/), else from a replacements/ folder on (put under `serial`).
// Paths with "..", absolute paths and control characters give "".
std::string TexturePackTarget(const std::string& entry, const std::string& serial);
// "1.2 GB", "356 MB", "12 KB".
std::string FormatBytes(uint64_t bytes);
// The MD5 of `data`, lowercase hex (the check a download goes through; here for the tests).
std::string TexturePackMd5(const void* data, size_t size);

struct TexturePackStatus
{
	enum class State
	{
		Loading,     // the list from archive.org is on its way
		Unavailable, // no list (offline, archive.org down): `message`; Cross tries again
		None,        // archive.org has no pack for this serial
		Available,   // `packs`
		Queued,      // waiting behind another game's pack
		Checking,    // reading back a part downloaded before
		Downloading,
		Verifying, // the MD5 of the whole file
		Unpacking,
		Installing,
		Installed, // `installed` names it
		Removing,
		Failed,    // `message`; Cross tries again
		NeedSpace, // `message` ("Needs 12.3 GB free")
	};
	State state = State::Loading;
	std::vector<TexturePack> packs; // archive.org's packs for this serial
	int job_pack = -1;              // the one being fetched, as an index into `packs` (-1 when it isn't there)
	std::string installed;          // Installed: the pack's file name, "" for one this app didn't put there
	std::string where;              // Installed: its folder
	bool ours = false;              // Installed by this app (its marker file): Triangle can remove it
	uint64_t done = 0, total = 0;   // bytes so far and in all (checking, downloading, verifying, unpacking)
	double rate = 0;                // download speed, bytes a second
	std::string message;            // Unavailable, Failed, NeedSpace: why
};

// What runs now, for the shelf's line ("HD textures: God of War · 37%").
struct TexturePackActivity
{
	bool active = false;
	std::string title; // the game
	TexturePackStatus::State state = TexturePackStatus::State::Queued;
	double fraction = 0;
	int waiting = 0; // other packs queued behind it
};

// What the app asks of a manager (AppConfig::texture_packs); unset on a build without one.
struct TexturePackService
{
	std::function<TexturePackStatus(const std::string& serial)> status;
	std::function<bool(const std::string& serial, int pack, const std::string& settings_path, const std::string& settings_header,
		const std::string& title)>
		begin;
	std::function<bool(const std::string& serial)> cancel;
	std::function<bool(const std::string& serial)> remove;
	std::function<void()> retry; // fetch the list again
	std::function<TexturePackActivity()> activity;
	explicit operator bool() const { return static_cast<bool>(status); }
};

struct TexturePackPlatform
{
	// GET a small text (the item's file list): the HTTP status, or < 0 when the request failed.
	std::function<int(const std::string& url, std::string& body)> get_text;
	// GET bytes [offset, offset + length) of `url`, each block to `sink` (false stops it): the HTTP status (206; 200 when
	// the whole file came from 0), or < 0.
	std::function<int(const std::string& url, uint64_t offset, uint64_t length, const std::function<bool(const void*, size_t)>& sink)>
		get_range;
	std::function<void()> abort; // fails the request in flight (Stop, Cancel), from another thread
	std::function<uint64_t(const std::string& dir)> free_bytes; // UINT64_MAX when unknown
	// A pack for this serial somewhere else the emulator looks first (a USB drive, the settings page's folder): its folder.
	std::function<std::string(const std::string& serial)> existing_pack;
	std::function<void(const std::string& line)> log;
	std::function<void(const std::string& game, bool ok, const std::string& detail)> notify; // the popup when a pack is in or failed
	std::function<double()> now; // seconds, monotonic
	// Called first thing on each thread the manager starts (the worker, "worker"; the unpacker's file writers, "writer"):
	// the console logs where it runs and lets it use all the title's CPUs (a thread made by the shelf's takes its CPU).
	std::function<void(const char* role)> thread_start;
};

class TexturePackManager
{
public:
	// textures_dir: the emulator's textures folder (<serial>/replacements); work_dir: downloads in progress and their
	// job files; catalog_cache: the item's file list, used for a day.
	TexturePackManager(TexturePackPlatform platform, std::string textures_dir, std::string work_dir, std::string catalog_cache);
	~TexturePackManager();
	TexturePackManager(const TexturePackManager&) = delete;
	TexturePackManager& operator=(const TexturePackManager&) = delete;

	// The worker: the list, then the downloads left from last time, then whatever Begin asks for.
	void Start();
	// Stops the worker: a download keeps its part for the next start, an unpack starts over then. True when it ended within
	// `wait_ms`; false leaves it running (and this object must then be left alive).
	bool Stop(int wait_ms);

	TexturePackStatus Status(const std::string& serial) const;
	bool Begin(const std::string& serial, int pack, const std::string& settings_path, const std::string& settings_header,
		const std::string& title);
	bool Cancel(const std::string& serial);
	bool Remove(const std::string& serial);
	void Retry();
	TexturePackActivity Activity() const;

	TexturePackService Service();

	// The marker file in a pack's folder ("ps5sx2-pack.txt"): which pack, from where, when.
	static constexpr const char* kMarker = "ps5sx2-pack.txt";
	static constexpr uint64_t kChunk = 32ull << 20;

private:
	struct Job
	{
		std::string serial, name, md5, title, settings, header;
		uint64_t bytes = 0;
	};
	struct Progress
	{
		TexturePackStatus::State state = TexturePackStatus::State::Queued;
		uint64_t done = 0, total = 0;
		double rate = 0;
		std::string message;
		std::string name; // the pack
		std::string title;
	};
	struct Installed
	{
		double checked = -1;
		bool present = false, ours = false;
		std::string name, where;
	};

	void Run();
	void MoveFromParent();
	// pr9l: a folder to delete is renamed out of the way first (.ps5sx2-old-...), and the worker deletes what was set
	// aside when it has nothing else to do: a folder of thousands of files takes the console minutes to delete.
	bool SetAside(const std::string& path);
	void EmptySetAside();
	void LoadCatalog();
	void ResumeJobs();
	void Process(const Job& job);
	bool Download(const Job& job, const std::string& part, const std::string& full);
	bool Unpack(const Job& job, const std::string& full, const std::string& staging, uint32_t& files, bool& no_space, std::string& error);
	void Finish(const Job& job, uint32_t files);
	void SetState(const std::string& serial, TexturePackStatus::State state, uint64_t done = 0, uint64_t total = 0,
		const std::string& message = {});
	void SetProgress(const std::string& serial, uint64_t done);
	bool Stopping(const std::string& serial) const;
	void Log(const std::string& line) const;
	void DeleteJobFiles(const Job& job);
	bool WriteJob(const Job& job);
	Installed LookAtFolder(const std::string& serial) const;
	std::vector<TexturePack> PacksFor(const std::string& serial) const;
	double Now() const;

	TexturePackPlatform m_platform;
	std::string m_textures, m_work, m_cache;
	std::thread m_thread;
	std::shared_ptr<bool> m_finished; // set by the worker as it ends (Stop waits on it)
	mutable std::mutex m_mutex;
	std::condition_variable m_cv;
	bool m_stop = false;
	bool m_started = false;
	bool m_retry = false;
	enum class Catalog
	{
		Loading,
		Ready,
		Failed,
	} m_catalog_state = Catalog::Loading;
	std::string m_catalog_error;
	std::vector<TexturePack> m_catalog;
	std::deque<Job> m_queue;
	std::deque<std::string> m_removals;
	std::map<std::string, Progress> m_progress; // by serial: queued, running, failed
	std::string m_current;                      // the serial being worked on
	bool m_cancel_current = false;
	mutable std::map<std::string, Installed> m_installed; // a cache: the folder is looked at every few seconds
	// The download speed: bytes and time at the last measure.
	uint64_t m_rate_bytes = 0;
	double m_rate_time = 0;
};
} // namespace fe
