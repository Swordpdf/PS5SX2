// PS5 port (vk-285-113): where a game's texture pack is.
//
// PCSX2 looks for replacement textures in <textures folder>/<serial>/replacements, with the textures folder
// /data/PCSX2/textures. Packs are big, so they can sit on a USB drive (or any folder the PS5 mounts) instead:
// FindGameDir looks there first and gives the game's folder, or "" to leave it to PCSX2's own place.
//
//   1. the PS5SX2/TexturesDir setting (gs.ini or the game's settings file), when <it>/<serial> is there;
//   2. each USB drive, /mnt/usb0 to /mnt/usb7: <drive>/PS5SX2/textures/<serial>, <drive>/PCSX2/textures/<serial>,
//      <drive>/textures/<serial>, and <drive>/PS5SX2/<serial>, <drive>/<serial> when that folder has a
//      "replacements" folder in it (a pack copied to the drive as it is).
//
// Folder names match without regard to case (USB drives are FAT32 or exFAT, other file systems care). The
// answer is a path with the names as they are on the drive. Header only, so ps5/coreorbis/tests/textures/ builds it
// on a PC.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

namespace OrbisTextures
{
inline bool IsDir(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline std::string LowerAscii(std::string s)
{
	for (char& c : s)
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	return s;
}

// The folder called `name` in `dir` (any case; an exact match wins), as it is named on the disk; "" if none.
inline std::string ChildDir(const std::string& dir, const std::string& name)
{
	DIR* d = opendir(dir.c_str());
	if (!d)
		return std::string();
	const std::string want = LowerAscii(name);
	std::string found;
	while (const dirent* e = readdir(d))
	{
		if (e->d_name[0] == '.')
			continue;
		const std::string entry = e->d_name;
		if (LowerAscii(entry) != want)
			continue;
		if (!IsDir(dir + "/" + entry))
			continue;
		if (entry == name)
		{
			found = entry;
			break;
		}
		if (found.empty())
			found = entry;
	}
	closedir(d);
	return found;
}

// dir/<a>/<b>/... with every name found in any case; "" when one is missing.
inline std::string WalkDirs(const std::string& dir, const std::vector<std::string>& names)
{
	std::string path = dir;
	for (const std::string& name : names)
	{
		const std::string child = ChildDir(path, name);
		if (child.empty())
			return std::string();
		path += "/" + child;
	}
	return path;
}

// The game's texture folder outside /data/PCSX2/textures, or "". `how` (may be null) says which rule found it.
inline std::string FindGameDir(const std::vector<std::string>& drive_roots, const std::string& manual_dir, const std::string& serial,
	std::string* how = nullptr)
{
	if (serial.empty())
		return std::string();
	if (!manual_dir.empty())
	{
		const std::string dir = WalkDirs(manual_dir, {serial});
		if (!dir.empty())
		{
			if (how)
				*how = "the PS5SX2/TexturesDir setting";
			return dir;
		}
	}
	for (const std::string& drive : drive_roots)
	{
		static const char* const kRoots[][2] = {{"PS5SX2", "textures"}, {"PCSX2", "textures"}, {"textures", nullptr}};
		for (const auto& r : kRoots)
		{
			std::vector<std::string> names = {r[0]};
			if (r[1])
				names.push_back(r[1]);
			names.push_back(serial);
			const std::string dir = WalkDirs(drive, names);
			if (!dir.empty())
			{
				if (how)
					*how = "a texture folder on " + drive;
				return dir;
			}
		}
		// A pack copied over as it is: <drive>/PS5SX2/<serial>/replacements or <drive>/<serial>/replacements.
		for (const char* top : {"PS5SX2", ""})
		{
			const std::string base = top[0] ? WalkDirs(drive, {top}) : drive;
			if (base.empty())
				continue;
			const std::string dir = WalkDirs(base, {serial, "replacements"});
			if (!dir.empty())
			{
				if (how)
					*how = "a texture pack on " + drive;
				return base + "/" + ChildDir(base, serial);
			}
		}
	}
	return std::string();
}

// The mounted USB drives' roots: /mnt/usb0 to /mnt/usb7 that hold anything (2026-10-08: then /mnt/ext0 and /mnt/ext1, the
// extended storage and M.2 drives).
inline std::vector<std::string> UsbRoots(const std::string& mount_dir = "/mnt")
{
	std::vector<std::string> out;
	for (int i = 0; i < 10; i++)
	{
		const std::string root = i < 8 ? mount_dir + "/usb" + std::to_string(i) : mount_dir + "/ext" + std::to_string(i - 8);
		DIR* d = opendir(root.c_str());
		if (!d)
			continue;
		bool any = false;
		while (const dirent* e = readdir(d))
			if (e->d_name[0] != '.')
			{
				any = true;
				break;
			}
		closedir(d);
		if (any)
			out.push_back(root);
	}
	return out;
}
} // namespace OrbisTextures
