// PS5 port frontend: the folders the shelf, the settings page and the cover prefetch list games from.
//
// PS5SX2/GameFolders unset: the usual places (games/, /data/PCSX2, every USB or extended-storage drive) and the NFS shares,
// as before. Set: the player's folders that are there, and the NFS shares, and nothing else. PS5SX2/ScanUsb off leaves the
// drives out of the usual places, PS5SX2/ScanSubfolders off lists only the files directly in each folder. One pure function
// decides, so every consumer (main-boot.cpp's orbis_scan_usb, fe_ps5.cpp's three scans, the disc finder) sees the same list
// and ps5/frontend/host/roots_test.cpp checks it on a PC.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace fe
{
// gs.ini's switches next to the folders (the settings page's and the sheet's Folders group); both default on = what the app always did.
constexpr const char* kScanUsbKey = "PS5SX2/ScanUsb";               // the drives (/mnt/usb0-7, /mnt/ext0-1) are looked through
constexpr const char* kScanSubfoldersKey = "PS5SX2/ScanSubfolders"; // each folder's own folders, one level down, are listed too

struct RootOptions
{
	bool scan_usb = true;
	bool scan_subfolders = true;
};

struct GameRoots
{
	std::vector<std::string> dirs;    // what ScanGames is given, in order (a folder, and with scan_subfolders one level of folders in it, each)
	bool only_set = false;            // PS5SX2/GameFolders was set: `dirs` holds those and the NFS shares only
	bool scan_usb = true;             // the usual places included the drives (never in only_set mode)
	bool scan_subfolders = true;      // ScanGames also opens the folders in each of `dirs`
	std::vector<std::string> missing; // set folders that are no folder here (boot.log, and the empty shelf says so)
	std::string signature;            // "default", or "set:" + the set folders joined with '|', and the switches: what usb-games.txt is keyed to
	std::string copy_to;              // where a copied disc goes: the first set folder that is no NFS or SMB share; "" = games/ as before
	std::string first_set;            // the first folder named in PS5SX2/GameFolders, there or not: what the empty shelf names
};

inline bool RootIsFolder(const std::string& path)
{
	struct stat st = {};
	return path.size() > 1 && path[0] == '/' && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// A folder on an NFS share (OrbisNfs mounts them under /nfs/<host>/...): read-only, so a copied disc is never put there.
inline bool UnderNfs(const std::string& path) { return path == "/nfs" || path.compare(0, 5, "/nfs/") == 0; }

// A folder on an SMB share (OrbisSmb mounts them under /smb/<server>/<share>/...): read-only too.
inline bool UnderSmb(const std::string& path) { return path == "/smb" || path.compare(0, 5, "/smb/") == 0; }

// `set`: PS5SX2/GameFolders as SplitFolderList gives it. `defaults`: games_dir and top_dir. `drives`: the drives' folders
// (orbis_usb_game_dirs). Both are read only while `set` is empty, and `drives` only while options.scan_usb. `shares`: the NFS
// and SMB shares' mount points (the player added them: always, and they exist by now, so a set folder under /nfs can be checked).
inline GameRoots ResolveGameRoots(const std::vector<std::string>& set, const std::vector<std::string>& defaults,
	const std::vector<std::string>& drives, const std::vector<std::string>& shares, const RootOptions& options = RootOptions())
{
	GameRoots r;
	r.scan_subfolders = options.scan_subfolders;
	const auto add = [&](const std::string& d) {
		if (!d.empty() && std::find(r.dirs.begin(), r.dirs.end(), d) == r.dirs.end())
			r.dirs.push_back(d);
	};
	if (set.empty())
	{
		r.scan_usb = options.scan_usb;
		for (const std::string& d : defaults)
			add(d);
		if (options.scan_usb)
			for (const std::string& d : drives)
				add(d);
	}
	else
	{
		r.only_set = true;
		r.scan_usb = false;
		r.first_set = set[0];
		for (const std::string& d : set)
		{
			if (RootIsFolder(d))
			{
				add(d);
				if (r.copy_to.empty() && !UnderNfs(d) && !UnderSmb(d))
					r.copy_to = d;
			}
			else
				r.missing.push_back(d);
		}
	}
	for (const std::string& d : shares)
		add(d);
	if (r.only_set)
	{
		// What was asked for, not what exists: before the jailbreak a drive's folder is not there yet, and the list the shelf
		// wrote after it must still be the one the cover prefetch reads. The shares are left out for the same reason.
		r.signature = "set:";
		for (size_t i = 0; i < set.size(); i++)
			r.signature += (i ? "|" : "") + set[i];
	}
	else
		r.signature = options.scan_usb ? "default" : "default-nousb";
	if (!options.scan_subfolders)
		r.signature += " flat";
	return r;
}

// cache/usb-games.txt: its first line names the roots it was written for. A list written for other roots is not used.
inline std::string UsbListHeader(const std::string& signature) { return "#roots " + signature + "\n"; }
inline bool UsbListMatches(const std::string& text, const std::string& signature)
{
	if (text.compare(0, 7, "#roots ") != 0)
		return signature == "default"; // a list from before the header: the default places wrote it
	const size_t nl = text.find('\n');
	return text.compare(0, nl == std::string::npos ? text.size() : nl, "#roots " + signature) == 0;
}
} // namespace fe
