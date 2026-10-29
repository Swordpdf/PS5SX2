// PS5 port (bioscheck, 2026-10-29, AI-assisted): the PS2 BIOS before every start, and the open-source BIOS PS5SX2 ships.
//
// 2.15's logs: 367 no-starts on 128 consoles came from disc starts (a disc tile, the disc helper, Play from disc); 56 of 60 sampled
// no-starts were "PCSX2 requires a PlayStation 2 BIOS", 52 of them disc starts that never went through the shelf's own BIOS check.
// One check now runs before every start (main-boot.cpp orbis_bios_usable), and with no BIOS of the player's own PS5SX2 offers
// OpenPS2BIOS (Tamariba Devs, MIT; frontend/assets/openps2bios), an experimental open-source BIOS for emulators: the player
// picks it once (gs.ini PS5SX2/OpenBios), a BIOS of their own always wins. It starts PS2 discs only: no PS1 discs, no DVD video.
// Plain C++ (no PCSX2, no PS5 headers): coreorbis/tests/openbios builds it on a PC.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace OrbisBios
{
// How the open BIOS is named wherever PS5SX2 names the BIOS (PCSX2's BIOS description, the logs, the shelf, the settings).
inline constexpr const char kOpenBiosName[] = "OpenPS2BIOS (experimental)";
// The bundled image's file name, and the folder it is written to (the cache folder's openps2bios/, never the player's bios/).
inline constexpr const char kOpenBiosFile[] = "OpenPS2BIOS-0.5.bin";
inline constexpr const char kOpenBiosDir[] = "openps2bios";
// gs.ini [PS5SX2] OpenBios=true: the player chose the open BIOS for when there is no BIOS of their own.
inline constexpr const char kOpenBiosKey[] = "OpenBios";

struct OpenBiosInfo
{
	bool open = false;   // the image is OpenPS2BIOS
	std::string version; // "0.5" ("" when the image doesn't say)
};

// OpenPS2BIOS's EE kernel prints "[openbios:ee] OpenPS2BIOS <version> ready" and its boot screen draws "OpenPS2BIOS"; no Sony
// BIOS holds either. (Its ROMVER, "0005AD20261028" for 0.5, reads as a USA "Devel" BIOS v00.05 to PCSX2, and its EXTINFO is empty.)
inline OpenBiosInfo IdentifyOpenBios(const unsigned char* data, size_t n)
{
	OpenBiosInfo info;
	static const char kName[] = "OpenPS2BIOS";
	static const char kTag[] = "[openbios:ee] ";
	const size_t name_len = sizeof(kName) - 1, tag_len = sizeof(kTag) - 1;
	for (size_t i = 0; i + name_len <= n; i++)
	{
		if (data[i] != 'O' || std::memcmp(data + i, kName, name_len) != 0)
			continue;
		info.open = true;
		if (i >= tag_len && std::memcmp(data + i - tag_len, kTag, tag_len) == 0 && i + name_len < n && data[i + name_len] == ' ')
		{
			for (size_t v = i + name_len + 1; v < n && v < i + name_len + 17 && ((data[v] >= '0' && data[v] <= '9') || data[v] == '.'); v++)
				info.version += static_cast<char>(data[v]);
			return info;
		}
	}
	return info;
}

// The same for a file (its first 8 MB, the largest BIOS PCSX2 takes).
inline OpenBiosInfo IdentifyOpenBiosFile(const char* path)
{
	OpenBiosInfo info;
	FILE* f = std::fopen(path, "rb");
	if (!f)
		return info;
	std::vector<unsigned char> buf(8u * 1024u * 1024u);
	const size_t n = std::fread(buf.data(), 1, buf.size(), f);
	std::fclose(f);
	return IdentifyOpenBios(buf.data(), n);
}

// "OpenPS2BIOS (experimental)", with the version after it for the logs ("OpenPS2BIOS (experimental) 0.5").
inline std::string OpenBiosLogName(const OpenBiosInfo& info)
{
	return info.version.empty() ? std::string(kOpenBiosName) : std::string(kOpenBiosName) + " " + info.version;
}

// Which BIOS a start gets: the player's own always wins; the open one only when the player chose it and its image is ready.
enum class Use
{
	Own,
	Open,
	None,
};
inline Use Choose(bool own_found, bool open_chosen, bool open_ready)
{
	if (own_found)
		return Use::Own;
	if (open_chosen && open_ready)
		return Use::Open;
	return Use::None;
}
inline const char* UseName(Use u)
{
	return u == Use::Own ? "a BIOS of the player's own" : u == Use::Open ? kOpenBiosName : "no BIOS";
}

// Where a start came from. Shelf: the shelf's pick (and the system menu row); ShelfOutside: a launch asked of the shelf from outside
// it (Play from disc, a copy that finished); the rest start without the shelf (a game tile, a disc tile, the disc helper's
// disc-launch.txt, the bootdisc flag, the plain list of the nomenu/nofrontend flags).
enum class StartFrom
{
	Shelf,
	ShelfOutside,
	GameTile,
	DiscTile,
	DiscLaunch,
	BootDisc,
	PlainList,
};
inline const char* StartFromName(StartFrom s)
{
	switch (s)
	{
		case StartFrom::Shelf: return "the shelf";
		case StartFrom::ShelfOutside: return "a launch asked of the shelf (play from disc, a finished copy)";
		case StartFrom::GameTile: return "a game tile";
		case StartFrom::DiscTile: return "a disc tile";
		case StartFrom::DiscLaunch: return "the disc helper (disc-launch.txt)";
		case StartFrom::BootDisc: return "the bootdisc flag";
		case StartFrom::PlainList: return "the plain list";
	}
	return "?";
}

// What happens to a start: it starts; or it doesn't, and the shelf opens with the BIOS choice (the start waits there: picking the
// open BIOS starts it), or, with no shelf to open (the nofrontend/nomenu flags), a notification says why and the app closes.
struct Verdict
{
	bool start = false;
	bool to_shelf = false; // the shelf opens (or stays up) with the BIOS choice
	bool notify = false;   // a system notification says why (the shelf isn't up yet, or won't be)
};
inline Verdict Gate(StartFrom from, Use use, bool shelf_can_open)
{
	Verdict v;
	if (use != Use::None)
	{
		v.start = true;
		return v;
	}
	const bool on_shelf = from == StartFrom::Shelf || from == StartFrom::ShelfOutside;
	v.to_shelf = on_shelf || (shelf_can_open && from != StartFrom::PlainList);
	v.notify = !on_shelf; // the shelf says it itself while it is up
	return v;
}

// OpenPS2BIOS's boot screen starts PS2 discs only (its README): no PS1 discs, no audio CDs, no DVD video.
enum class Disc
{
	Ps2,
	Ps1,
	DvdVideo,
	AudioCd,
	Other,
};
inline bool OpenBiosCanStart(Disc d)
{
	return d == Disc::Ps2 || d == Disc::Other; // Other: an ELF, the system menu (no disc): nothing to refuse for
}

// Writes the bundled image to `path` (its folder made when missing) unless an identical file is there already. False when it
// can't be written.
inline bool WriteIfDifferent(const std::string& path, const unsigned char* data, size_t n)
{
	if (FILE* f = std::fopen(path.c_str(), "rb"))
	{
		std::vector<unsigned char> have(n + 1);
		const size_t got = std::fread(have.data(), 1, have.size(), f);
		std::fclose(f);
		if (got == n && std::memcmp(have.data(), data, n) == 0)
			return true;
	}
	const size_t slash = path.rfind('/');
	if (slash != std::string::npos && slash > 0)
	{
		const std::string dir = path.substr(0, slash);
		struct stat st = {};
		if (stat(dir.c_str(), &st) != 0)
			mkdir(dir.c_str(), 0777);
	}
	const std::string tmp = path + ".part";
	FILE* f = std::fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(data, 1, n, f) == n;
	if (std::fclose(f) != 0 || !ok)
	{
		std::remove(tmp.c_str());
		return false;
	}
	if (std::rename(tmp.c_str(), path.c_str()) != 0)
	{
		std::remove(tmp.c_str());
		return false;
	}
	return true;
}
} // namespace OrbisBios
