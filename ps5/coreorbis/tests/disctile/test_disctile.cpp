// PS5SX2 (fix180, AI-assisted), PC test: orbis-shims/OrbisDiscTileStart.h, what a press on a PS2 disc tile starts (owner: never the question; the dump
// when there is one, by serial; else the disc; a half-finished copy doesn't count).
//   ps5/coreorbis/tests/disctile/test-disctile.sh
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "OrbisDiscTileStart.h"

#include <cstdio>
#include <set>

using namespace orbis_disc;

static int fails = 0, checks = 0;
static void Check(bool ok, const char* what)
{
	checks++;
	if (!ok)
	{
		fails++;
		std::printf("FAIL  %s\n", what);
	}
}

int main()
{
	std::set<std::string> files;
	const auto exists = [&](const std::string& p) { return files.count(p) != 0; };
	const std::string kSerial = "SLUS-20864";

	// no dump: the disc
	{
		const std::vector<TileCandidate> games = {{"/data/PCSX2/games/Other (SLUS-20147).iso", "SLUS-20147"}};
		const std::string copy = PickTileCopy(games, kSerial, exists);
		Check(copy.empty(), "no game of that serial: no copy");
		Check(DecideTileStart(kSerial, copy) == TileStart::Disc, "no dump -> play straight from the disc");
	}
	// a dump of that serial, any name, any format, any folder: the dump
	{
		const std::vector<TileCandidate> games = {
			{"/data/PCSX2/games/Other (SLUS-20147).iso", "SLUS-20147"},
			{"/mnt/usb0/PS2/punisher.chd", kSerial},
		};
		const std::string copy = PickTileCopy(games, kSerial, exists);
		Check(copy == "/mnt/usb0/PS2/punisher.chd", "dump found by serial (a .chd on a USB drive)");
		Check(DecideTileStart(kSerial, copy) == TileStart::Copy, "dump -> the dump starts");
	}
	// a half-finished copy doesn't count
	{
		const std::vector<TileCandidate> games = {{"/data/PCSX2/games/The Punisher (SLUS-20864).iso.part", kSerial}};
		Check(PickTileCopy(games, kSerial, exists).empty(), "a .part (copy still being written) is no dump");
		Check(DecideTileStart(kSerial, PickTileCopy(games, kSerial, exists)) == TileStart::Disc, "-> the disc");
	}
	{
		const std::vector<TileCandidate> games = {{"/data/PCSX2/games/X.ISO.PART", kSerial}, {"/data/PCSX2/games/X.iso.state", kSerial},
			{"/data/PCSX2/games/X.iso.tmp", kSerial}};
		Check(PickTileCopy(games, kSerial, exists).empty(), "upper-case .PART, a .state and a .tmp are no dumps");
	}
	// a copy with unreadable sectors (.incomplete marker) doesn't count; a whole one after it does
	{
		files.insert("/data/PCSX2/games/The Punisher (SLUS-20864).iso.incomplete");
		const std::vector<TileCandidate> games = {
			{"/data/PCSX2/games/The Punisher (SLUS-20864).iso", kSerial},
			{"/data/PCSX2/games/DVD/The Punisher.iso", kSerial},
		};
		Check(PickTileCopy(games, kSerial, exists) == "/data/PCSX2/games/DVD/The Punisher.iso", "the copy with holes is skipped, the whole one taken");
		files.clear();
		Check(PickTileCopy(games, kSerial, exists) == "/data/PCSX2/games/The Punisher (SLUS-20864).iso", "without the marker the first one is taken");
	}
	// an image the shelf can't read doesn't count
	{
		TileCandidate bad{"/data/PCSX2/games/broken.chd", kSerial};
		bad.unreadable = true;
		Check(PickTileCopy({bad}, kSerial, exists).empty(), "an unreadable image is no dump");
	}
	// no serial (not a PS2 game): nothing; an empty serial never matches a game without one
	{
		const std::vector<TileCandidate> games = {{"/data/PCSX2/games/noserial.iso", ""}};
		Check(PickTileCopy(games, "", exists).empty(), "no serial: no copy");
		Check(DecideTileStart("", "") == TileStart::Nothing, "no serial: the shelf opens");
	}
	// never a question: the decision has no third answer for a disc with a serial
	{
		Check(DecideTileStart(kSerial, "") != TileStart::Nothing && DecideTileStart(kSerial, "/x.iso") != TileStart::Nothing, "a PS2 disc always starts");
	}
	std::printf("disctile test: %d checks, %s\n", checks, fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
