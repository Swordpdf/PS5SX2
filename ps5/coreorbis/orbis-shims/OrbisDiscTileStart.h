// PS5SX2 (fix180, AI-assisted): what a press on a PS2 DISC TILE starts. Kept apart from the PS5 calls so a PC test runs it.
//
// Owner, 2026-10-30: "when the player presses a PS2 disc tile in the XMB, never show the 'Play from disc or dump game' question. If a dump (copy) of that
// disc's game already exists (match by serial: the shelf's games, a finished disc copy), start the dump; if not, play straight from the disc. The 'When a
// PS2 disc goes in' setting (Ask me / Copy / Play) still applies to inserting a disc / the shelf's disc entry, just not to a tile press."
//
// main-boot.cpp asks ProsperoDiscDump's OrbisFindDiscCopy first (the copy's own name, or an .iso of the same serial at about the disc's size). When that
// finds nothing, every game of the shelf's folders (fe::ScanGames, any format: .iso, .chd, .cso, .bin, ...; any size) whose serial is the disc's counts
// (PickTileCopy). A half-finished copy never does: it is "<name>.iso.part" until it is whole (not even listed), and a copy that finished with unreadable
// sectors has a "<image>.incomplete" marker beside it (it may hang; it stays on the shelf to be tried by hand). Neither does an image the shelf marks
// unreadable. In a disc tile's process the disc watcher never asks either (ProsperoDiscDump.cpp): when the tile ends on the shelf after all (a BIOS
// missing, a disc not ready), the disc is its entry there and nothing pops up.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace orbis_disc
{
struct TileCandidate
{
	std::string path;
	std::string serial;
	bool unreadable = false; // the shelf can't read it (a damaged CHD): no start from it
};

inline bool EndsWithNoCase(const std::string& s, const char* tail)
{
	size_t n = 0;
	while (tail[n])
		n++;
	if (s.size() < n)
		return false;
	for (size_t i = 0; i < n; i++)
	{
		char a = s[s.size() - n + i], b = tail[i];
		if (a >= 'A' && a <= 'Z')
			a = static_cast<char>(a - 'A' + 'a');
		if (a != b)
			return false;
	}
	return true;
}

// A copy that is not whole: still being written (.part), its resume state, a temporary file.
inline bool IsUnfinishedCopyName(const std::string& path)
{
	return EndsWithNoCase(path, ".part") || EndsWithNoCase(path, ".state") || EndsWithNoCase(path, ".tmp");
}

// The first candidate of the disc's serial that is a whole copy, "" when none. `exists` says whether a file is there (the ".incomplete" marker).
inline std::string PickTileCopy(const std::vector<TileCandidate>& games, const std::string& serial, const std::function<bool(const std::string&)>& exists)
{
	if (serial.empty())
		return {};
	for (const TileCandidate& g : games)
	{
		if (g.serial != serial || g.unreadable || g.path.empty() || IsUnfinishedCopyName(g.path))
			continue;
		if (exists && exists(g.path + ".incomplete"))
			continue;
		return g.path;
	}
	return {};
}

enum class TileStart
{
	Copy,    // the dump of the disc's game starts
	Disc,    // no dump: the game starts straight from the disc
	Nothing, // the drive holds no PS2 game (no serial): the shelf opens
};

// Never Ask: a tile press has no question.
inline TileStart DecideTileStart(const std::string& serial, const std::string& copy)
{
	if (serial.empty())
		return TileStart::Nothing;
	return copy.empty() ? TileStart::Disc : TileStart::Copy;
}
} // namespace orbis_disc
