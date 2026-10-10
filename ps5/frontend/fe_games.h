// PS5 port frontend: the game list: disc images (.iso, .chd, .cso, .zso, and since 2026-10-08 .bin and .img, raw or plain),
// their serials (read from the disc's SYSTEM.CNF),
// display titles made from Redump-style file names, and badges from the game's settings.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <utility>
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
	// vk-285-134 (AI-assisted): why the image can't be read ("invalid data": a CHD libchdr can't open, damaged or cut short;
	// build 130's logs: 10 such starts on 4 consoles); empty when it reads. The shelf marks it and won't start it.
	std::string damaged;
	// vk-285-137 (AI-assisted; swordpdf: "add an option to hide games from the shelf"): PS5SX2/HideGame=true in its own settings
	// file (ReadBadges). The shelf leaves it out unless gs.ini's PS5SX2/ShowHiddenGames is on, and then shows it dimmed.
	bool hidden = false;
	// A disc drive's entry (IsDrivePath): `absent` while it holds no PS2 disc (the shelf leaves it out, whatever
	// ShowHiddenGames says), `reading` while a disc that went in is spun up and checked (its box shows a spinning disc,
	// and it won't start until it's known).
	bool absent = false;
	bool reading = false;
};

// A disc image's file name: .iso, (vk-285-108) .chd, (vk-285-113) .cso or .zso, (2026-10-08) .bin or .img, in any case,
// not hidden. ScanGames leaves out a .bin or .img under 16 MB or without an ISO 9660 volume (an audio track).
bool IsDiscImageName(const char* name);
// 2026-10-08: an .elf (a PS2 executable). ScanGames lists one whose first bytes are an ELF header; it has no serial.
bool IsElfName(const char* name);

// A disc drive's device, "/dev/cd0" to "/dev/cd7": a USB DVD or BD drive on a PS5 without a disc drive of its
// own. The shelf lists the drive (fe_ps5.cpp) and the game boots from the disc in it (main-boot.cpp, CDVD_SourceType::Disc).
bool IsDrivePath(const std::string& path);
// The disc in drive `device`, for the shelf: its serial (SYSTEM.CNF), the volume's size, a title (the volume's name, else "PS2
// disc"; ApplyGameDbTitle names it better) and the settings file's name (the serial, or "Disc drive cd1" without one).
// False, with `g` still describing the drive, when no data disc reads: none in it, still spinning up, not an ISO 9660 disc;
// `why` then says which, for the shelf.
bool ReadDriveDisc(const std::string& device, GameInfo& g, std::string* why);

// A disc drive's entry for the shelf while it holds no PS2 disc (absent) and while a disc that went in is read (reading:
// "Reading the disc" on its box). ReadDriveDisc describes it once the disc is known.
GameInfo EmptyDrive(const std::string& device);
GameInfo ReadingDrive(const std::string& device);

// The region a PS2 serial's prefix stands for ("SLUS-21351": "USA"; SLES/SCES Europe, SLPS/SLPM/SCPS Japan, SLKA/SCKA
// Korea, SCAJ/SLAJ Asia), in the names MakeTitle gives; empty for a prefix it doesn't know.
std::string RegionFromSerial(const std::string& serial);

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
// 2026-10-08 (AI-assisted): the database's hardware-renderer fixes for a serial (its gsHWFixes, "nativeScaling" -> 3), in
// the file's order; none when the serial isn't there or no database file is set. Fixes whose value is a function's name
// (getSkipCount and the like) aren't in it.
std::vector<std::pair<std::string, int>> GameDbHwFixes(const std::string& serial);

// The shelf's order: by title (case-insensitive), then by file name. ScanGames sorts this way; a list whose
// titles ApplyGameDbTitle changed sorts again.
void SortGames(std::vector<GameInfo>& games);

// Badges from settings/<stem>.ini, gs.ini and the patches folder.
void ReadBadges(GameInfo& g, const std::string& settings_dir, const std::string& gs_ini, const std::string& patches_dir);

// vk-285-137: gs.ini's PS5SX2/ShowHiddenGames (the sheet for all games): hidden games stay on the shelf, dimmed.
bool ShowHiddenGames(const std::string& gs_ini);

// vk-285-139 (AI-assisted): a game's discs, for changing discs in the game. The images an .m3u in the image's folder lists
// (in its order, relative to it or full paths) when one lists this image; else the disc images beside it whose names
// differ only in "(Disc N)" or "(Disc N of M)" (Redump's naming; any image type), by N; else just the image.
std::vector<std::string> DiscSet(const std::string& path);
// N of "(Disc N)" / "(Disc N of M)" in a file name (any case), 0 when there's none; `rest`: the name without it and its
// extension, for comparing.
int DiscNumber(const std::string& name, std::string* rest);
} // namespace fe
