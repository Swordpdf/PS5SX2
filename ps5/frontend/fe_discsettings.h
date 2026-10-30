// PS5 port frontend (gamedb, 2026-10-30, AI-assisted): the settings file of a PS2 disc played from the drive, one rule for the emulator
// (main-boot.cpp, the file a disc start reads) and the shelf (the sheet of the disc's entry, the file it writes).
//
// #bugs, 2026-10-25 (MGS2 PAL from the disc, 2.15): "if I put in his own settings x6 and framegen on ... it will only use the all games
// settings"; games from files didn't have it. The shelf's entry for the disc in the drive (and for a disc being copied) had no settings
// file name: Square opened the all-games sheet, and L1 there opened a "this game" sheet that wrote settings/.ini, a file no start reads.
// A disc start reads settings/<Title> (<serial>).ini (2.11: the name the disc's copy gets, so the copy starts with the disc's settings).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>

namespace fe
{
// The stem of settings/<stem>.ini for disc `serial`, whose copy is named `by_copy` ("Metal Gear Solid 2 - Sons of Liberty (SLES-50383)",
// ProsperoDiscDump.cpp OrbisDiscSettingsStem). That file when it is there; else a file an earlier build named: settings/<serial>.ini
// (2.04 to 2.10) or "PS2 disc (<serial>)" (the copy's name while the database was missing, before gamedb); else `by_copy` (a new file).
inline std::string DiscSettingsStem(const std::string& serial, const std::string& by_copy,
	const std::function<bool(const std::string& stem)>& exists)
{
	if (serial.empty() || exists(by_copy))
		return by_copy;
	if (exists(serial))
		return serial;
	const std::string unnamed = "PS2 disc (" + serial + ")";
	if (unnamed != by_copy && exists(unnamed))
		return unnamed;
	return by_copy;
}
} // namespace fe
