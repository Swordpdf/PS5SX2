// PS5 port (vk-285-33): the /data/PCSX2 folder layout (include-orbis/OrbisPaths.h).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisPaths.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <vector>

namespace
{
constexpr const char* kRoot = "/data/PCSX2";

bool IsDir(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
} // namespace

std::string OrbisDir(const char* sub)
{
	std::string dir = std::string(kRoot) + "/" + sub;
	return IsDir(dir) ? dir : std::string(kRoot);
}

// vk-285-46: stat(), not access(). Before the HEN jailbreak the app's sandbox answers access() on a
// file that exists with EPERM (vk-285-45's log: the flags folder stats fine, mode 777, and access
// fails with errno 1), so every flag read as absent there -- in vk-285-43/44 that set the driver's
// environment without vk_renderer. stat() works on both sides of the jailbreak.
namespace
{
bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}
} // namespace

std::string OrbisFlagPath(const char* name)
{
	std::string in_flags = std::string(kRoot) + "/flags/" + name;
	if (Exists(in_flags))
		return in_flags;
	return std::string(kRoot) + "/" + name;
}

// vk-285-105: the flags folder and the top folder's names, and the small files the GS thread polls (gs.ini,
// live.ini, the game's settings file), refreshed once a second by main-boot's ticker thread
// (OrbisFlagsRefresh). OrbisFlag and OrbisCachedRead answer from memory once the first refresh is in, so the
// emulation threads make no system call on /data: a stat() or an open there takes ~0.05 ms, and now and then
// 30 ms -- vk-285-104's GS thread stalled ~33 ms every few seconds in Shadow of the Colossus's heavy view (the
// frame capture flag's check every 25 frames, the settings files every 30, the Vulkan driver's ~20 live flags
// every second from its submit). Before the first refresh (the boot) they read the files directly.
namespace
{
std::mutex s_snap_mutex;
bool s_snap_ready = false;
std::vector<std::string> s_snap_flags; // names in flags/
std::vector<std::string> s_snap_root; // names in the top folder
struct OrbisCachedFile
{
	std::string path;
	std::string data;
	bool ok = false;
	bool fresh = false; // read by a refresh
};
std::vector<OrbisCachedFile> s_cached_files;

std::vector<std::string> ListNames(const char* dir)
{
	std::vector<std::string> names;
	if (DIR* d = opendir(dir))
	{
		while (const dirent* e = readdir(d))
			if (e->d_name[0] != '.')
				names.emplace_back(e->d_name);
		closedir(d);
	}
	std::sort(names.begin(), names.end());
	return names;
}

bool ReadSmall(const std::string& path, std::string& out)
{
	out.clear();
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char b[4096];
	const size_t n = fread(b, 1, sizeof(b), f);
	fclose(f);
	out.assign(b, n);
	return true;
}

bool Has(const std::vector<std::string>& names, const char* name)
{
	return std::binary_search(names.begin(), names.end(), std::string(name));
}
} // namespace

void OrbisFlagsRefresh()
{
	std::vector<std::string> flags = ListNames((std::string(kRoot) + "/flags").c_str());
	// vk-285-118 (AI-assisted): switch names in any letter case. 2026-10-02's reports had a console whose flags/ held
	// FASTMEM, VK_16K and VK_ASYNC: it ran without them (12% of its minutes below full speed). Every switch is lower case.
	for (std::string& f : flags)
		for (char& c : f)
			c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	std::vector<std::string> root = ListNames(kRoot);
	std::vector<std::string> paths;
	{
		std::lock_guard<std::mutex> lock(s_snap_mutex);
		for (const OrbisCachedFile& c : s_cached_files)
			paths.push_back(c.path);
	}
	std::vector<std::string> data(paths.size());
	std::vector<bool> ok(paths.size());
	for (size_t i = 0; i < paths.size(); i++)
		ok[i] = ReadSmall(paths[i], data[i]);
	std::lock_guard<std::mutex> lock(s_snap_mutex);
	s_snap_flags.swap(flags);
	s_snap_root.swap(root);
	for (size_t i = 0; i < paths.size(); i++)
	{
		for (OrbisCachedFile& c : s_cached_files)
		{
			if (c.path == paths[i])
			{
				c.data.swap(data[i]);
				c.ok = ok[i];
				c.fresh = true;
				break;
			}
		}
	}
	s_snap_ready = true;
}

bool OrbisFlag(const char* name)
{
	{
		std::lock_guard<std::mutex> lock(s_snap_mutex);
		if (s_snap_ready)
			return Has(s_snap_flags, name) || Has(s_snap_root, name);
	}
	return Exists(OrbisFlagPath(name));
}

bool OrbisCachedRead(const char* path, std::string& out)
{
	{
		std::lock_guard<std::mutex> lock(s_snap_mutex);
		bool known = false;
		for (const OrbisCachedFile& c : s_cached_files)
		{
			if (c.path == path)
			{
				known = true;
				if (c.fresh)
				{
					out = c.data;
					return c.ok;
				}
				break;
			}
		}
		if (!known)
			s_cached_files.push_back(OrbisCachedFile{path});
	}
	return ReadSmall(path, out); // not refreshed yet: this once, directly
}

// The Vulkan driver's live flags (ps5vk_device.c, weak there): answered for files in the flags folder.
extern "C" bool ps5vk_live_flag_hook(const char* path, bool* on)
{
	static const std::string prefix = std::string(kRoot) + "/flags/";
	if (std::strncmp(path, prefix.c_str(), prefix.size()) != 0)
		return false;
	const char* name = path + prefix.size();
	std::lock_guard<std::mutex> lock(s_snap_mutex);
	if (!s_snap_ready)
		return false;
	*on = Has(s_snap_flags, name);
	return true;
}

extern "C" {
char g_orbis_pf_log[160] = "/data/PCSX2/pf.log";
}

std::string OrbisLogPath(const char* name)
{
	return OrbisDir("logs") + "/" + name;
}
