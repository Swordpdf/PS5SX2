// PS5SX2 2.41 (AI-assisted): fe_pnach.h, which .pnach files belong to a game (ps5/frontend/host/test-pnach.sh).
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fe_pnach.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

using namespace fe::pnach;

static int s_failed = 0, s_passed = 0;
static void Check(bool ok, const std::string& what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
	(ok ? s_passed : s_failed)++;
}

static void Write(const std::string& path, const std::string& text)
{
	if (FILE* f = std::fopen(path.c_str(), "w"))
	{
		std::fputs(text.c_str(), f);
		std::fclose(f);
	}
}

int main(int argc, char** argv)
{
	const std::string work = argc > 1 ? argv[1] : "/tmp/pnach-test";
	mkdir(work.c_str(), 0755);

	// ---- the scanners
	Check(SerialsIn("SLUS-20552_20B19E49") == std::vector<std::string>{"SLUS20552"}, "SerialsIn: PCSX2's own name");
	Check(SerialsIn("slus_205.52 60fps") == std::vector<std::string>{"SLUS20552"}, "SerialsIn: lower case, _ and the dot");
	Check(SerialsIn("SLUS 20552") == std::vector<std::string>{"SLUS20552"}, "SerialsIn: a space");
	Check(SerialsIn("SLUS20552") == std::vector<std::string>{"SLUS20552"}, "SerialsIn: no separator");
	Check(SerialsIn("XSLUS-20552").empty(), "SerialsIn: not inside a longer word");
	Check(SerialsIn("SLUS-205521").empty(), "SerialsIn: not with a sixth digit");
	Check(SerialsIn("[SLUS 20552] (U)") == std::vector<std::string>{"SLUS20552"}, "SerialsIn: in brackets");
	Check(NormSerial("SLUS-20552") == "SLUS20552" && NormSerial("slus_205.52") == "SLUS20552", "NormSerial: the forms");
	Check(NormSerial("SLUS-20552 widescreen").empty(), "NormSerial: a name with a serial is not a serial");
	Check(CrcsIn("GTA VC (20B19E49)") == std::vector<uint32_t>{0x20B19E49u}, "CrcsIn: in brackets");
	Check(CrcsIn("20b19e49") == std::vector<uint32_t>{0x20B19E49u}, "CrcsIn: lower case");
	Check(CrcsIn("0x20B19E49") == std::vector<uint32_t>{0x20B19E49u}, "CrcsIn: 0x");
	Check(CrcsIn("deadbeefcafe").empty(), "CrcsIn: not part of a longer hex word");
	Check(CrcsIn("Widescreen").empty() && CrcsIn("60fps hud").empty(), "CrcsIn: ordinary words are no CRC");

	// ---- Classify
	Game vc{"SLUS-20552", 0x20B19E49u, "Grand Theft Auto - Vice City (USA)"};
	auto why = [&](const Game& g, const char* folder, const char* name, const char* head = "") { return Classify(g, folder, name, head); };
	Check(why(vc, "", "SLUS-20552_20B19E49.pnach").why == Why::Serial && why(vc, "", "SLUS-20552_20B19E49.pnach").Load(), "PCSX2's name: loaded");
	Check(why(vc, "", "slus_20552 widescreen HUD.pnach").Load(), "rule 2: the serial anywhere, any case and separator");
	Check(why(vc, "", "GTA VC (20B19E49).pnach").why == Why::Crc, "rule 2: the CRC as a word");
	Check(why(vc, "", "20b19e49.pnach").Load(), "rule 2: PCSX2 1.x's <crc>.pnach, lower case");
	Check(why(vc, "", "grand theft auto - vice city (usa).PNACH").why == Why::FileName, "rule 3: the game's file name, any case");
	Check(why(vc, "", "Grand_Theft_Auto_Vice_City_USA.pnach").why == Why::FileName, "rule 3: separators don't matter");
	Check(why(vc, "SLUS-20552", "anything.pnach").why == Why::Folder, "rule 1: a folder named after the serial");
	Check(why(vc, "slus 205.52", "anything.pnach").why == Why::Folder, "rule 1: the folder's serial in any form");
	Check(why(vc, "Grand Theft Auto - Vice City (USA)", "60 fps.pnach").why == Why::Folder, "rule 1: a folder with the game's file name");
	Check(why(vc, "", "hud fix.pnach", "gametitle=Grand Theft Auto - Vice City [SLUS 20552] (U)\npatch=1,EE,0,word,0\n").why == Why::GameTitle,
		"rule 4: the gametitle line");
	Check(!why(vc, "", "hud fix.pnach").Ours(), "no rule: a name with nothing in it is not the game's");
	Check(!why(vc, "", "SLES-50000 widescreen.pnach").Ours(), "another game's serial in the name: not ours");
	Check(!why(vc, "", "SLES-50000 (20B19E49).pnach").Ours(), "our CRC beside another game's serial: not ours");
	Check(!why(vc, "SLES-50000", "SLUS-20552.pnach").Ours(), "in another game's folder: not ours, whatever the name");
	Check(!why(vc, "", "hud.pnach", "gametitle=Something [SLES 50000]\n").Ours(), "another game's gametitle: not ours");

	// ---- the version check
	Check(why(vc, "", "SLUS-20552_11111111.pnach").other_version, "version: the serial with another CRC = another version");
	Check(!why(vc, "", "SLUS-20552_11111111.pnach").Load(), "version: not loaded");
	Check(why(vc, "", "SLUS-20552_11111111_20B19E49.pnach").Load(), "version: one of the named CRCs is ours: loaded");
	Check(why(vc, "", "SLUS-20552.pnach", "crc=11111111\n").other_version, "version: a crc= line names another version");
	Check(why(vc, "", "SLUS-20552.pnach", "crc = 20B19E49\n").Load(), "version: a crc= line with ours");
	Game vc_nocrc{"SLUS-20552", 0, "Grand Theft Auto - Vice City (USA)"};
	Check(why(vc_nocrc, "", "SLUS-20552_11111111.pnach").Load(), "version: CRC not known (the shelf before a start): taken");
	Check(!why(vc_nocrc, "", "20B19E49.pnach").Ours(), "CRC not known: a <crc>.pnach can't be told");
	Check(why(vc, "", "SLUS-20552 60fps.pnach").Load(), "version: a file that names no CRC is taken");

	// ---- List over real files
	const std::string dir = work + "/cheats";
	mkdir(dir.c_str(), 0755);
	mkdir((dir + "/SLUS-20552").c_str(), 0755);
	mkdir((dir + "/SLES-50000").c_str(), 0755);
	Write(dir + "/SLUS-20552_20B19E49.pnach", "[Money]\npatch=1,EE,0,word,0\n");
	Write(dir + "/gta vc hud.pnach", "gametitle=Grand Theft Auto - Vice City [SLUS 20552] (U)\npatch=1,EE,0,word,0\n");
	Write(dir + "/SLUS-20552_11111111.pnach", "[Old]\n");
	Write(dir + "/random.pnach", "patch=1,EE,0,word,0\n");
	Write(dir + "/notes.txt", "x");
	Write(dir + "/SLUS-20552/widescreen.pnach", "[WS]\n");
	Write(dir + "/SLES-50000/x.pnach", "[X]\n");
	std::vector<std::string> unmatched;
	const std::vector<Found> found = List(dir, vc, true, &unmatched);
	int load = 0, other = 0, ours = 0;
	for (const Found& f : found)
	{
		load += f.match.Load();
		other += f.match.other_version;
		ours += f.match.Ours();
	}
	Check(found.size() == 6, "List: every .pnach, one folder down, not notes.txt (" + std::to_string(found.size()) + ")");
	Check(load == 3 && other == 1 && ours == 4, "List: 3 loaded, 1 for another version (" + std::to_string(load) + ", " + std::to_string(other) + ")");
	Check(unmatched == std::vector<std::string>{"random.pnach"}, "List: random.pnach names no game");

	// ---- the CRC cache
	const std::string cache = work + "/game-crcs.txt";
	std::remove(cache.c_str());
	Check(KnownCrc(cache, "SLUS-20552") == 0, "KnownCrc: none yet");
	RememberCrc(cache, "slus_20552", 0x20B19E49u);
	RememberCrc(cache, "SCUS-97399", 0x12345678u);
	Check(KnownCrc(cache, "SLUS-20552") == 0x20B19E49u && KnownCrc(cache, "SCUS-97399") == 0x12345678u, "RememberCrc/KnownCrc: two games");
	RememberCrc(cache, "SLUS-20552", 0x0BADF00Du);
	Check(KnownCrc(cache, "SLUS 20552") == 0x0BADF00Du && KnownCrc(cache, "SCUS-97399") == 0x12345678u, "RememberCrc: the newest replaces the old one");

	std::printf("%s: %d passed, %d failed\n", s_failed ? "SOME FAILED" : "all passed", s_passed, s_failed);
	return s_failed ? 1 : 0;
}
