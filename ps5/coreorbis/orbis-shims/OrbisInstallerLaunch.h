// PS5SX2 (vk-285-180, AI-assisted): the app starts the PS5SX2 Installer itself, the way PS5SX3 starts its own: it reads the
// installer ELF it ships (/app0/PS5SX2Installer.elf), fills in the ELF's launch block and sends it to the console's ELF loader
// (127.0.0.1:9021, the same loader the helper goes to: OrbisHelperAutoload.cpp). Two reasons:
//   - at every start, as the session logger (kFlagLogger): the installer never installs then; it stays running and sends the
//     session's logs when PS5SX2 closes (or leaves at once when a logger already runs or no-log-upload is there). So the user no
//     longer loads PS5SX2Installer.elf by hand for the log upload;
//   - from the shelf's System update row (kFlagRelaunch): it waits until this app is gone, installs the latest release (size and
//     SHA-256 checked, journalled: ps5/installer) and starts PS5SX2 again.
//
// The block's layout is ps5/installer/src/launch.h's (change both, and the marker's number, together): the marker, then zeros to the
// block's end, exactly once in the file. Everything here is pure (no file, no socket), so the PC test (tests/updater) drives it.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace orbis_installer
{
constexpr char kLaunchMarker[] = "PS5SX2-INSTALLER-LAUNCH-1";
constexpr uint32_t kLaunchMagic = 0x32495850; // "PXI2"
constexpr uint32_t kFlagRelaunch = 1;         // start PS5SX2 again once the install is done (or there was none)
constexpr uint32_t kFlagLogger = 2;           // only the session logger; never install
constexpr std::size_t kLaunchSize = 176;
constexpr std::size_t kTagLen = 64;
// Offsets in the block (launch.h's struct installer_launch).
constexpr std::size_t kOffMagic = 32, kOffSize = 36, kOffFlags = 40, kOffPid = 44, kOffAppTag = 48, kOffExpectTag = 112;

struct LaunchArgs
{
	uint32_t flags = 0;
	int32_t app_pid = 0;
	std::string app_tag;    // this build ("vk-285-180")
	std::string expect_tag; // the release the shelf showed, "" for none
};

// Where the block is in the ELF: the marker, then zeros to the block's end, exactly once. npos when it is not there once.
inline std::size_t FindLaunchBlock(const std::vector<uint8_t>& elf)
{
	const std::size_t marker = sizeof(kLaunchMarker) - 1;
	std::size_t found = std::string_view::npos;
	for (std::size_t i = 0; i + kLaunchSize <= elf.size(); i++)
	{
		if (elf[i] != static_cast<uint8_t>(kLaunchMarker[0]) || std::memcmp(elf.data() + i, kLaunchMarker, marker) != 0)
			continue;
		bool zeros = true;
		for (std::size_t j = i + marker; j < i + kLaunchSize && zeros; j++)
			zeros = elf[j] == 0;
		if (!zeros)
			continue;
		if (found != std::string_view::npos)
			return std::string_view::npos; // twice: not the installer this was made for
		found = i;
	}
	return found;
}

inline bool TagFits(const std::string& t)
{
	if (t.size() >= kTagLen)
		return false;
	for (const char c : t)
		if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7f)
			return false;
	return true;
}

// Fills the block in place; false with `error` (English, for boot.log) when the file is not an installer with one block.
inline bool FillLaunchBlock(std::vector<uint8_t>& elf, const LaunchArgs& args, std::string& error)
{
	if (elf.size() < 64 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0)
	{
		error = "the installer isn't an ELF file";
		return false;
	}
	if (!TagFits(args.app_tag) || !TagFits(args.expect_tag))
	{
		error = "a build tag is too long for the installer's block";
		return false;
	}
	const std::size_t at = FindLaunchBlock(elf);
	if (at == std::string_view::npos)
	{
		error = "the installer has no launch block (an installer older than 1.4?)";
		return false;
	}
	uint8_t* const b = elf.data() + at;
	const auto put32 = [&](std::size_t offset, uint32_t v) {
		for (int i = 0; i < 4; i++)
			b[offset + static_cast<std::size_t>(i)] = static_cast<uint8_t>(v >> (8 * i)); // little-endian, as the console
	};
	put32(kOffMagic, kLaunchMagic);
	put32(kOffSize, static_cast<uint32_t>(kLaunchSize));
	put32(kOffFlags, args.flags);
	put32(kOffPid, static_cast<uint32_t>(args.app_pid));
	std::memcpy(b + kOffAppTag, args.app_tag.data(), args.app_tag.size());
	std::memcpy(b + kOffExpectTag, args.expect_tag.data(), args.expect_tag.size());
	return true;
}

// ---- the logger at start ----

enum class LoggerDecision
{
	Send,       // send the installer as the logger
	SkipFlag,   // flags/logger_noautoload: the user loads the installer by hand, as before
	SkipOff,    // /data/PS5SX2-Installer/no-log-upload: the person switched the upload off
	SkipNoElf,  // no installer shipped with the app
	SkipRunning // a logger already holds logger.lock (one the user loaded by hand, or the one sent at an earlier start)
};

inline const char* LoggerDecisionName(LoggerDecision d)
{
	switch (d)
	{
		case LoggerDecision::Send: return "send";
		case LoggerDecision::SkipFlag: return "skip (flag logger_noautoload)";
		case LoggerDecision::SkipOff: return "skip (no-log-upload: the upload is switched off)";
		case LoggerDecision::SkipNoElf: return "skip (no installer shipped with the app)";
		case LoggerDecision::SkipRunning: return "skip (a logger already runs)";
	}
	return "?";
}

// `logger_locked`: logger.lock is held by another process right now (the app tried it without waiting).
inline LoggerDecision DecideLogger(bool noautoload_flag, bool no_log_upload, bool have_elf, bool logger_locked)
{
	if (noautoload_flag)
		return LoggerDecision::SkipFlag;
	if (no_log_upload)
		return LoggerDecision::SkipOff;
	if (!have_elf)
		return LoggerDecision::SkipNoElf;
	if (logger_locked)
		return LoggerDecision::SkipRunning;
	return LoggerDecision::Send;
}

// What starting the installer for an update came to (OrbisStartUpdateInstaller).
enum class StartResult
{
	Started,      // the loader took the whole ELF: the app should close now
	NoInstaller,  // the app has no installer (or it cannot be read)
	BadInstaller, // it is not an installer this app can fill in (`detail` says why)
	NoLoader,     // nothing answered on 127.0.0.1:9021
	SendFailed    // the loader stopped taking it
};
} // namespace orbis_installer

// The console's side (OrbisInstallerStart.cpp). Both log to boot.log ("[installer] ...").
// At start, after the helper (main-boot.cpp): the installer as the session logger, unless DecideLogger says not.
void OrbisLoggerAutoStart(const char* build_tag);
// From the shelf's System update row: the installer for an update of this app (build_tag) to the release the shelf showed.
orbis_installer::StartResult OrbisStartUpdateInstaller(const std::string& build_tag, const std::string& expect_tag, std::string& detail);
