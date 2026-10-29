// PS5 port frontend: updating PS5SX2 itself from the shelf, and the session-log switch (vk-285-180, AI-assisted).
//
// The owner: an in-app updater like PS5SX3's. The sheet for all games (Square on the shelf) gets a "PS5SX2" group:
//   System update  Cross looks at the latest release of Swordpdf/PS5SX2 on GitHub (the API the PS5SX2 Installer reads,
//                  ps5/installer/src/github.c, with its rules: a PS5SX2-*.zip asset, uploaded, a plausible size, GitHub's
//                  SHA-256 "digest", the repository's own download address) and compares its tag with this build's
//                  ("vk-285-181" > "vk-285-180"). When a newer one is out, Cross twice starts the installer the app ships
//                  (/app0/PS5SX2Installer.elf, through the ELF loader: OrbisInstallerStart.cpp) and closes PS5SX2; the
//                  installer waits until PS5SX2 is gone, downloads the zip, checks its size and SHA-256, installs it with
//                  its journal (a cut-off install is undone at its next run; the old files are kept until the new ones are
//                  all in place, eboot.bin last) and starts PS5SX2 again. A journal left by a cut-off install shows here as
//                  "Interrupted": Cross runs the installer, which undoes or finishes it first.
//   Session logs   On: the installer, sent by the app at each start as the session logger, sends a session's logs when
//                  PS5SX2 closes (ps5/installer/src/logger.c). Off writes the installer's own switch
//                  /data/PS5SX2-Installer/no-log-upload (the opt-out every earlier logger already honours).
// One look at GitHub when the shelf opens (flags/noupdatecheck: none) says on the screen when a newer build is out.
//
// The parsing and the decision are pure (no network, no file): ps5/coreorbis/tests/updater runs them on a PC, with a fake
// platform for the Updater.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace fe
{
// The repository the releases come from (ps5/installer/src/config.h GH_REPO: change both together).
constexpr const char* kUpdateApiUrl = "https://api.github.com/repos/Swordpdf/PS5SX2/releases/latest";
constexpr const char* kUpdateDownloadPrefix = "https://github.com/Swordpdf/PS5SX2/releases/download/";
constexpr uint64_t kUpdateMaxZipBytes = 512ull << 20; // the installer's MAX_ZIP_BYTES

struct ReleaseInfo
{
	std::string tag;        // "vk-285-181"
	std::string title;      // "PS5SX2 2.20" (the release's name; the tag when it has none)
	std::string asset_name; // "PS5SX2-vk-285-181.zip"
	std::string asset_url;  // <prefix><tag>/<asset_name>
	uint64_t size = 0;
	std::string sha256;     // lowercase hex
	std::string published_at;
};

// GitHub's answer for the latest release, checked as the installer checks it; false with `error` (English, for boot.log
// and the row) when it is not a release the installer would install.
bool ParseLatestRelease(const std::string& json, const std::string& download_prefix, ReleaseInfo& out, std::string& error);

// Build tags of the form <prefix>-<number>[-<number>...] ("vk-285-180"): < 0, 0, > 0 as a is older than, the same as, or
// newer than b; 2 when they can't be compared (another prefix or count of numbers). The installer's tag_compare (app.c).
int CompareBuildTags(const std::string& a, const std::string& b);

struct UpdateStatus
{
	enum class State
	{
		Idle,        // not looked yet
		Checking,    // asking GitHub
		Current,     // this build is the latest release
		Newer,       // this build is newer than the latest release (a test build)
		Available,   // `latest` is newer: Cross twice installs it
		Failed,      // the look failed (`error`)
		Interrupted, // an install was cut off (the installer's journal is there): Cross lets the installer undo or finish it
		Starting,    // the installer is being sent
		Started,     // the installer runs: the app closes now
		StartFailed, // the installer couldn't be started (`error`)
	};
	State state = State::Idle;
	ReleaseInfo latest;
	std::string error;
	bool automatic = false; // the last look was the one at the shelf's start
};

const char* UpdateStateName(UpdateStatus::State s);

// What the GitHub answer means for this build. `http_status` < 0: no answer (`fetch_error` says why).
UpdateStatus JudgeRelease(const std::string& this_tag, int http_status, const std::string& body, const std::string& fetch_error,
	const std::string& download_prefix);

// What the app asks of the updater (AppConfig::updates); unset on a build without one (no rows).
struct UpdateService
{
	std::function<UpdateStatus()> status;
	std::function<bool(bool automatic)> check; // false when a look already runs
	std::function<bool()> install;             // Available or Interrupted: starts the installer; false otherwise or on failure
	std::function<bool()> log_upload;          // the session logs are sent (no no-log-upload switch)
	std::function<bool(bool on)> set_log_upload;
	std::function<void()> quit;                // after Started: close PS5SX2 (the installer waits for it)
	std::string this_tag;
	explicit operator bool() const { return static_cast<bool>(status); }
};

struct UpdatePlatform
{
	// GET a small document over HTTPS: the HTTP status (body filled whatever it is), or < 0 with `error`.
	std::function<int(const std::string& url, std::string& body, std::string& error)> get_text;
	// Sends the installer for an update to `expect_tag` (empty: the latest): 0 started, 1 no installer, 2 not an installer this
	// app can fill in, 3 no ELF loader, 4 the send broke; `detail` says what happened.
	std::function<int(const std::string& expect_tag, std::string& detail)> start_installer;
	std::function<bool(const std::string& path)> exists;
	// Creates (on) or removes (off) an empty switch file; false when that failed.
	std::function<bool(const std::string& path, bool on)> set_file;
	std::function<void(const std::string& text)> notify; // a popup (the automatic look's "newer build" line)
	std::function<void(const std::string& line)> log;
	std::function<void()> quit;
	std::function<void()> thread_start; // first thing on the worker
};

class Updater
{
public:
	// `work_dir`: the installer's folder (/data/PS5SX2-Installer): journal.txt, no-log-upload.
	Updater(UpdatePlatform platform, std::string this_tag, std::string work_dir, std::string api_url = kUpdateApiUrl,
		std::string download_prefix = kUpdateDownloadPrefix);
	~Updater();
	Updater(const Updater&) = delete;
	Updater& operator=(const Updater&) = delete;

	UpdateStatus Status() const;
	bool Check(bool automatic);
	bool Install();
	bool LogUpload() const;
	bool SetLogUpload(bool on);
	// Waits up to `wait_ms` for a look in flight; true when the worker ended.
	bool Stop(int wait_ms);
	UpdateService Service();

	// One look, on the calling thread (the worker's body; the PC test calls it directly).
	UpdateStatus Look(bool automatic);

private:
	void Log(const std::string& line) const;

	UpdatePlatform m_platform;
	std::string m_tag, m_work, m_api, m_prefix;
	mutable std::mutex m_mutex;
	std::condition_variable m_cv;
	std::thread m_thread;
	bool m_running = false;
	UpdateStatus m_status;
};
} // namespace fe
