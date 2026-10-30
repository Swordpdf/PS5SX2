// PS5SX2 (vk-285-180, AI-assisted): see OrbisInstallerLaunch.h. The app starts the PS5SX2 Installer it ships (/app0/PS5SX2Installer.elf)
// through the console's ELF loader: at every start as the session logger, and from the shelf's System update row for an update. So the
// user loads neither the helper nor the installer by hand any more. Every step goes to boot.log.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisHelperAutoload.h"
#include "OrbisInstallerLaunch.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// From orbis_paths.cpp.
bool OrbisFlag(const char* name) __attribute__((weak));

namespace
{
constexpr const char* kWork = "/data/PS5SX2-Installer"; // the installer's own folder (ps5/installer/src/config.h P_WORK)

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...)
{
	char line[600];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	std::printf("[installer] %s\n", line);
	std::fflush(stdout);
}

bool Exists(const std::string& path)
{
	struct stat s;
	return stat(path.c_str(), &s) == 0;
}

// The installer the app ships, then one a tester put beside the settings by hand.
// fix180: and the app's folder by its /data path: /app0 is the app's folder only before the jailbreak, and the logger and the System update row look
// after it (all 51 vk-285-180 reports: "logger: skip (no installer shipped with the app)", though the package has PPSA99203/PS5SX2Installer.elf).
std::string FindInstaller()
{
	static const char* const kPaths[] = {"/app0/PS5SX2Installer.elf", "/data/homebrew/PPSA99203/PS5SX2Installer.elf", "/data/PCSX2/PS5SX2Installer.elf"};
	for (const char* p : kPaths)
	{
		struct stat s;
		if (stat(p, &s) == 0 && S_ISREG(s.st_mode) && s.st_size > 4096)
			return p;
	}
	return {};
}

bool ReadAll(const std::string& path, std::vector<uint8_t>& out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out.clear();
	uint8_t buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < (64u << 20))
		out.insert(out.end(), buf, buf + n);
	const bool ok = !std::ferror(f);
	std::fclose(f);
	return ok && !out.empty();
}

// Whether another process holds logger.lock now (the installer's flock, fsx.c fs_lock). Tried without waiting and let go at once;
// a file that is not there, or cannot be opened, is "not held".
bool LoggerLockHeld()
{
	const std::string path = std::string(kWork) + "/logger.lock";
	const int fd = open(path.c_str(), O_RDWR | O_NOFOLLOW);
	if (fd < 0)
		return false;
	bool held = false;
	if (flock(fd, LOCK_EX | LOCK_NB) != 0)
		held = errno == EWOULDBLOCK || errno == EAGAIN;
	else
		flock(fd, LOCK_UN);
	close(fd);
	return held;
}

orbis_installer::StartResult Send(const std::string& path, const orbis_installer::LaunchArgs& args, std::string& detail)
{
	using orbis_installer::StartResult;
	std::vector<uint8_t> elf;
	if (path.empty() || !ReadAll(path, elf))
	{
		detail = path.empty() ? "no PS5SX2Installer.elf in the app's folder" : "cannot read " + path;
		return StartResult::NoInstaller;
	}
	std::string error;
	if (!orbis_installer::FillLaunchBlock(elf, args, error))
	{
		detail = path + ": " + error;
		return StartResult::BadInstaller;
	}
	bool no_loader = false;
	const std::vector<unsigned char>& bytes = elf;
	if (!OrbisLoaderSend(bytes, detail, &no_loader))
		return no_loader ? StartResult::NoLoader : StartResult::SendFailed;
	return StartResult::Started;
}
} // namespace

void OrbisLoggerAutoStart(const char* build_tag)
{
	using namespace orbis_installer;
	const bool flag = OrbisFlag && OrbisFlag("logger_noautoload");
	const bool off = Exists(std::string(kWork) + "/no-log-upload");
	const std::string elf = flag || off ? std::string() : FindInstaller();
	const bool locked = !flag && !off && !elf.empty() && LoggerLockHeld();
	const LoggerDecision d = DecideLogger(flag, off, !elf.empty(), locked);
	Log("logger: %s", LoggerDecisionName(d));
	if (d != LoggerDecision::Send)
		return;
	LaunchArgs a;
	a.flags = kFlagLogger;
	a.app_pid = static_cast<int32_t>(getpid());
	a.app_tag = build_tag && TagFits(build_tag) ? build_tag : "";
	std::string detail;
	const StartResult r = Send(elf, a, detail);
	Log("logger: %s %s: %s", r == StartResult::Started ? "sent" : "could not send", elf.c_str(), detail.c_str());
}

orbis_installer::StartResult OrbisStartUpdateInstaller(const std::string& build_tag, const std::string& expect_tag, std::string& detail)
{
	using namespace orbis_installer;
	LaunchArgs a;
	a.flags = kFlagRelaunch;
	a.app_pid = static_cast<int32_t>(getpid());
	a.app_tag = TagFits(build_tag) ? build_tag : "";
	a.expect_tag = TagFits(expect_tag) ? expect_tag : "";
	const std::string elf = FindInstaller();
	const StartResult r = Send(elf, a, detail);
	Log("update: %s %s for %s -> %s: %s", r == StartResult::Started ? "sent" : "could not send", elf.empty() ? "(no installer)" : elf.c_str(),
		a.app_tag.c_str(), a.expect_tag.empty() ? "(latest)" : a.expect_tag.c_str(), detail.c_str());
	return r;
}
