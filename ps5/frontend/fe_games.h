// PS5 port frontend: the game list: disc images (.iso, .chd, .cso, .zso), their serials (read from the disc's SYSTEM.CNF),
// display titles made from Redump-style file names, and badges from the game's settings.
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fe
{
struct GameInfo
{
	std::string path;   // full path of the image
	std::string file;   // its file name
	std::string stem;   // the file name without the extension (the settings file's name)
	std::string title;  // display title
	std::string region; // "USA", "Europe", ... (may be empty)
	std::string extra;  // the other bracketed parts of the name, e.g. "En,Ja"
	std::string serial; // "SLUS-21351" (empty when the disc could not be read)
	uint64_t bytes = 0;
	std::vector<std::string> badges; // "6x", "16:9", "60 FPS"
};

// A disc image's file name: .iso, (vk-285-108) .chd, (vk-285-113) .cso or .zso, in any case, not hidden.
bool IsDiscImageName(const char* name);

// Lists the disc images in `dirs` (the first folder wins for a name found twice), sorted by title.
std::vector<GameInfo> ScanGames(const std::vector<std::string>& dirs);

// "SLUS-21351" from the image's SYSTEM.CNF (ISO 9660); empty if unreadable. A .cso or .zso is decompressed here
// (zlib, LZ4). A .chd is read through libchdr
// (a DVD's 2048-byte units or a CD's raw frames); one that needs a parent CHD reads as empty.
std::string ReadSerial(const std::string& image_path);

// Read the BOOT2 executable for the RA hash. Supports the shelf image formats. (AI-assisted)
bool ReadAchievementExecutable(const std::string& path, std::string& name, std::vector<uint8_t>& bytes);

// vk-285-109: what a CHD is (header, codecs, first track, data offset, or why libchdr can't open it), for
// the log when ReadSerial found no serial in it; empty for other images.
std::string DescribeImage(const std::string& image_path);

// vk-285-108: where ReadSerial keeps the CHD serials it found (opening a CHD costs tens of milliseconds);
// empty: none kept.
void SetSerialCacheFile(const std::string& path);

// Display title and region from a file name stem ("Lord of the Rings, The - The Two Towers (USA)").
void MakeTitle(const std::string& stem, std::string& title, std::string& region, std::string& extra);

// vk-285-113: PCSX2's game database (resources/GameIndex.yaml) by serial. A disc image named after its file
// ("SLUS_200.39.chd", "rac1.iso") shows as the game's name, like the emulator's own window does: ApplyGameDbTitle
// gives `g` the database's English name (and its region when the file name has none) unless the title made
// from the file name already holds that name (a Redump-style file name keeps its own spelling, region and
// language tags). False when nothing changed. Read once, on first use; an empty path reads none.
void SetGameDbFile(const std::string& path);
bool ApplyGameDbTitle(GameInfo& g);

// The shelf's order: by title (case-insensitive), then by file name. ScanGames sorts this way; a list whose
// titles ApplyGameDbTitle changed sorts again.
void SortGames(std::vector<GameInfo>& games);

// Badges from settings/<stem>.ini, gs.ini and the patches folder.
void ReadBadges(GameInfo& g, const std::string& settings_dir, const std::string& gs_ini, const std::string& patches_dir);
} // namespace fe
