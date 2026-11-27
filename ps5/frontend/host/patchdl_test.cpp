// PS5 port frontend, PC test (2026-10-08, AI-assisted): fe_patchdl.cpp, a game's patches and cheats from GitHub, against a
// stand-in for raw.githubusercontent.com. Checks the CRC, the pnach checks, Gabominated's renamed groups, where each file
// goes, that a file this app didn't write is never replaced, that one it wrote is replaced when its source changes, and the
// sheet's Cheats rows (on, off, EmuCore/EnableCheats with them).
//   ps5/frontend/host/test-patchdl.sh
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_options.h"
#include "fe_patchdl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int g_fails = 0;

static void Check(bool ok, const std::string& what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
	if (!ok)
		g_fails++;
}

static std::string Read(const std::string& path)
{
	std::string out;
	if (FILE* f = std::fopen(path.c_str(), "rb"))
	{
		char buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
			out.append(buf, n);
		std::fclose(f);
	}
	return out;
}

static void Write(const std::string& path, const std::string& text)
{
	if (FILE* f = std::fopen(path.c_str(), "wb"))
	{
		std::fwrite(text.data(), 1, text.size(), f);
		std::fclose(f);
	}
}

static bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}

int main(int argc, char** argv)
{
	const std::string root = argc > 1 ? argv[1] : "/tmp/patchdl-test";
	const std::string patches = root + "/patches", cheats = root + "/cheats", manifest = root + "/cache/online-patches.txt";
	std::system(("rm -rf '" + root + "' && mkdir -p '" + patches + "' '" + root + "/settings'").c_str());

	// The CRC: the XOR of the little-endian words, a short tail left out.
	{
		std::vector<uint8_t> elf = {0x7f, 'E', 'L', 'F', 1, 0, 0, 0, 0x10, 0x20, 0x30, 0x40, 0xAA};
		const uint32_t want = 0x464c457fu ^ 0x00000001u ^ 0x40302010u;
		Check(fe::ElfCrc(elf) == want, "ElfCrc is the XOR of the 32-bit words");
	}
	// vk-285-134b: the URL is made by replacing "%s", never by printf: a "%20" (or "%20s") in a source's URL stays as it is.
	{
		const fe::OnlinePatchSource odd = {"odd", "https://example.com/a%20s%20P/%s.pnach", false, "", ""};
		Check(fe::OnlinePatchUrl(odd, "SLUS-20265_79646C72") == "https://example.com/a%20s%20P/SLUS-20265_79646C72.pnach",
			"a source's URL keeps its %20s and gets the key in place of its %s");
		bool gab_ok = false;
		for (const fe::OnlinePatchSource& src : fe::OnlinePatchSources())
			if (std::string(src.name) == "Gabominated")
				gab_ok = fe::OnlinePatchUrl(src, "SLUS-20265_79646C72") ==
				         "https://raw.githubusercontent.com/Gabominated/PCSX2/main/PCSX2%20Patches/SLUS-20265_79646C72.pnach";
		Check(gab_ok, "Gabominated's URL is its folder \"PCSX2 Patches\" with the key");
	}
	Check(fe::LooksLikePnach("gametitle=X\n[60 FPS]\npatch=1,EE,00100000,word,00000000\n"), "a pnach text is one");
	Check(!fe::LooksLikePnach("<!DOCTYPE html><html>404</html>"), "an HTML page is not");
	Check(!fe::LooksLikePnach(""), "nothing is not");
	// vk-285-135: PCSX2's Ratchet & Clank file (SCUS-97199_CE4933D0) has every line commented out: a pnach with nothing in it.
	{
		const std::string rc = "//gametitle=Ratchet & Clank (NTSC-U)\n\n//[Widescreen 16:9]\n//gsaspectratio=16:9\n"
		                       "// DWORD Code patching routine\n//patch=1,EE,200C0000,extended,3C1B000C\n";
		Check(fe::ClassifyPnach(rc) == fe::PnachKind::Empty, "a pnach with every line commented out has nothing in it");
		Check(fe::ClassifyPnach("gametitle=X\n[60 FPS]\npatch=1,EE,1,word,2\n") == fe::PnachKind::Patches, "one with a patch line has patches");
		Check(fe::ClassifyPnach("<!DOCTYPE html><html>\n<body>Not Found</body></html>\n") == fe::PnachKind::NotPnach, "an HTML page isn't one");
	}
	{
		const std::string in = "gametitle=Game\n// comment\npatch=1,EE,1,word,2\n[60 FPS]\nauthor=me\npatch=1,EE,3,word,4\n[]\npatch=1,EE,5,word,6\n";
		const std::string out = fe::TagPnachGroups(in, "Gabominated", "Gabominated");
		Check(out.find("[Gabominated]\npatch=1,EE,1,word,2") != std::string::npos, "loose lines go under [Gabominated]");
		Check(out.find("[60 FPS (Gabominated)]") != std::string::npos, "a group is renamed with the source");
		Check(out.find("[60 FPS]") == std::string::npos, "the old name is gone");
		Check(out.find("gametitle=Game\n// comment\n") == 0, "the lines before stay as they were");
		bool loose = true;
		Check(fe::CountPnachGroups(out, &loose) == 3 && !loose, "the renamed file has 3 groups and nothing loose");
		Check(out.find("[Gabominated 2]\npatch=1,EE,5,word,6") != std::string::npos, "a second unnamed group gets its own name");
		bool loose_in = false;
		fe::CountPnachGroups(in, &loose_in);
		Check(loose_in, "the original had loose lines");
	}

	// The stand-in for GitHub.
	const std::string key = "SLUS-20265_79646C72";
	std::map<std::string, std::pair<int, std::string>> server;
	const std::string pcsx2_text = "gametitle=Game (SLUS-20265)\n[Widescreen 16:9]\npatch=1,EE,00100000,word,00000001\n";
	const std::string gab_text = "gametitle=Game\npatch=1,EE,00200000,word,00000002\n[60 FPS]\npatch=1,EE,00300000,word,00000003\n";
	const std::string cheat_text = "gametitle=Game\n[Infinite Health]\npatch=1,EE,00400000,word,00000004\n[Max Money]\npatch=1,EE,00500000,word,00000005\n";
	server["https://raw.githubusercontent.com/PCSX2/pcsx2_patches/main/patches/" + key + ".pnach"] = {200, pcsx2_text};
	server["https://raw.githubusercontent.com/Gabominated/PCSX2/main/PCSX2%20Patches/" + key + ".pnach"] = {200, gab_text};
	server["https://raw.githubusercontent.com/xs1l3n7x/pcsx2_cheats_collection/main/cheats/" + key + ".pnach"] = {200, cheat_text};
	std::vector<std::string> asked;

	fe::OnlinePatchPlatform platform;
	platform.get_text = [&](const std::string& url, std::string& body) {
		asked.push_back(url);
		const auto it = server.find(url);
		if (it == server.end())
		{
			body = "404: Not Found";
			return 404;
		}
		body = it->second.second;
		return it->second.first;
	};
	platform.log = [](const std::string& line) { std::printf("      %s\n", line.c_str()); };

	// A file the user (or a release) put there already.
	const std::string user_file = patches + "/" + key + ".pnach";
	Write(user_file, "gametitle=mine\n[My patch]\npatch=1,EE,1,word,1\n");

	{
		fe::OnlinePatches op(platform, patches, cheats, manifest);
		const fe::OnlinePatchStatus s = op.FetchWithCrc("SLUS-20265", 0x79646C72);
		Check(asked.size() == 3, "three sources asked (" + std::to_string(asked.size()) + ")");
		Check(s.state == fe::OnlinePatchStatus::State::Done, "the fetch is done: " + s.message);
		Check(Read(user_file).find("gametitle=mine") == 0, "the user's own patch file is left as it was");
		const std::string gab = Read(patches + "/" + key + "_gabominated.pnach");
		Check(gab.find("[60 FPS (Gabominated)]") != std::string::npos && gab.find("[Gabominated]\npatch=1,EE,00200000") != std::string::npos,
			"Gabominated's file is written with its groups renamed");
		Check(Read(cheats + "/" + key + ".pnach") == cheat_text, "the cheats go into the cheats folder as they are");
		const std::string m = Read(manifest);
		Check(m.find(key + "_gabominated.pnach") != std::string::npos && m.find(cheats + "/" + key + ".pnach") != std::string::npos &&
				  m.find(patches + "/" + key + ".pnach\t") == std::string::npos,
			"the manifest names the two files written, not the user's");
		Check(s.message.find("your own files kept") != std::string::npos, "the message says the user's file was kept");
	}

	// The source changes Gabominated's file: replaced (this app wrote it). The user edits the cheats file: kept from then on.
	server["https://raw.githubusercontent.com/Gabominated/PCSX2/main/PCSX2%20Patches/" + key + ".pnach"].second =
		gab_text + "[50 FPS]\npatch=1,EE,00600000,word,00000006\n";
	Write(cheats + "/" + key + ".pnach", cheat_text + "// my own line\n");
	{
		fe::OnlinePatches op(platform, patches, cheats, manifest);
		op.FetchWithCrc("SLUS-20265", 0x79646C72);
		Check(Read(patches + "/" + key + "_gabominated.pnach").find("[50 FPS (Gabominated)]") != std::string::npos,
			"a file this app wrote is replaced when its source changed");
		Check(Read(cheats + "/" + key + ".pnach").find("// my own line") != std::string::npos, "a file changed since is left alone");
	}

	// Nothing for a game: 404 everywhere.
	{
		fe::OnlinePatches op(platform, patches, cheats, manifest);
		const fe::OnlinePatchStatus s = op.FetchWithCrc("SLES-99999", 0x12345678);
		Check(s.state == fe::OnlinePatchStatus::State::Done && s.message == "Nothing online for this game", "nothing online: " + s.message);
		Check(!Exists(patches + "/SLES-99999_12345678.pnach"), "and no file made");
	}
	// A broken answer (HTML with 200) is no patch file.
	server["https://raw.githubusercontent.com/PCSX2/pcsx2_patches/main/patches/SLES-11111_11111111.pnach"] = {200, "<html>rate limited</html>"};
	{
		fe::OnlinePatches op(platform, patches, cheats, manifest);
		const fe::OnlinePatchStatus s = op.FetchWithCrc("SLES-11111", 0x11111111);
		Check(s.state == fe::OnlinePatchStatus::State::Failed && !Exists(patches + "/SLES-11111_11111111.pnach"),
			"an HTML answer fails and writes nothing: " + s.message);
	}
	// vk-285-135: a file with every patch commented out (PCSX2's Ratchet & Clank): nothing online, no failure, no file.
	server["https://raw.githubusercontent.com/PCSX2/pcsx2_patches/main/patches/SCUS-97199_CE4933D0.pnach"] = {200,
		"//gametitle=Ratchet & Clank (NTSC-U)\n\n//[Widescreen 16:9]\n//gsaspectratio=16:9\n//patch=1,EE,200C0000,extended,3C1B000C\n"};
	{
		fe::OnlinePatches op(platform, patches, cheats, manifest);
		const fe::OnlinePatchStatus s = op.FetchWithCrc("SCUS-97199", 0xCE4933D0);
		Check(s.state == fe::OnlinePatchStatus::State::Done && s.message.find("Nothing online for this game") == 0 &&
				  s.message.find("turned off") != std::string::npos && !Exists(patches + "/SCUS-97199_CE4933D0.pnach"),
			"a file with its patches commented out is nothing online, not a failure: " + s.message);
	}

	// 2.16 (AI-assisted): the serials an executable names its memory card folders after.
	{
		const std::string s = std::string("\x7f" "ELF....BASLUS-20500\0..BESLES-52478\0BASLUS-20500\0BASLUS-205001\0BXSLUS-11111\0", 79);
		const std::vector<uint8_t> elf(s.begin(), s.end());
		const std::vector<std::string> got = fe::ExecutableSerials(elf);
		Check(got.size() == 2 && got[0] == "SLUS-20500" && got[1] == "SLES-52478",
			"ExecutableSerials finds BASLUS-20500 and BESLES-52478 once each, in order, and nothing else");
		const std::string tree = R"({"sha":"x","tree":[{"path":"SLUS-20500_E169BAF8.pnach","mode":"100644"},)"
		                         R"({"path":"SLUS-20500_E169BAF8.txt"},{"path":"SLUS-205001_00000000.pnach"},{"path":"SLES-52478_E2E67E23.pnach"}]})";
		const std::vector<std::string> files = fe::TreeFilesForSerial(tree, "SLUS-20500");
		Check(files.size() == 1 && files[0] == "SLUS-20500_E169BAF8.pnach", "TreeFilesForSerial takes only <serial>_<crc>.pnach");
		// GitHub's own layout: a space after each colon, one key a line
		const std::string github = "{\n  \"tree\": [\n    {\n      \"path\": \"SLUS-21423_7EA439F5.pnach\",\n      \"mode\": \"100644\"\n    }\n  ]\n}\n";
		const std::vector<std::string> gh = fe::TreeFilesForSerial(github, "SLUS-21423");
		Check(gh.size() == 1 && gh[0] == "SLUS-21423_7EA439F5.pnach", "and reads GitHub's answer as GitHub writes it");
	}
	// 2.16: nothing for the disc's own CRC: the fallback by serial.
	{
		const std::string pcsx2_tree = "https://api.github.com/repos/PCSX2/pcsx2_patches/git/trees/main:patches";
		const std::string gab_tree = "https://api.github.com/repos/Gabominated/PCSX2/git/trees/main:PCSX2%20Patches";
		const std::string cheats_tree = "https://api.github.com/repos/xs1l3n7x/pcsx2_cheats_collection/git/trees/main:cheats";
		server[pcsx2_tree] = {200, R"({"tree":[{"path":"SLUS-21423_7EA439F5.pnach"},{"path":"SLUS-20500_E169BAF8.pnach"},)"
		                           R"({"path":"SLES-52478_E2E67E23.pnach"},{"path":"SLUS-20946_AAAAAAAA.pnach"},{"path":"SLUS-20946_BBBBBBBB.pnach"}]})"};
		server[gab_tree] = {200, R"({"tree":[{"path":"SLES-52478_E2E67E23.pnach"}]})"};
		server[cheats_tree] = {200, R"({"tree":[]})"};
		const std::string base = "https://raw.githubusercontent.com/PCSX2/pcsx2_patches/main/patches/";
		server[base + "SLUS-21423_7EA439F5.pnach"] = {200, "gametitle=LCS\n[Widescreen 16:9]\npatch=1,EE,002918d0,word,468010a0\n"};
		server[base + "SLUS-20500_E169BAF8.pnach"] = {200, "gametitle=RDR\n[Force 60FPS]\npatch=1,EE,201018AC,word,24030001\n"};
		server[base + "SLES-52478_E2E67E23.pnach"] = {200, "gametitle=RDR PAL\n[50/60 FPS]\npatch=1,EE,001018AC,word,24030001\n"};

		// A translation that kept the serial (GTA LCS, SLUS-21423, CRC 3AF039F8 against PCSX2's 7EA439F5).
		const std::string lcs_elf = "\x7f" "ELF...BASLUS-21423";
		{
			asked.clear();
			fe::OnlinePatches op(platform, patches, cheats, manifest);
			const fe::OnlinePatchStatus s = op.FetchWithExecutable("SLUS-21423", 0x3AF039F8, std::vector<uint8_t>(lcs_elf.begin(), lcs_elf.end()));
			const std::string file = Read(patches + "/SLUS-21423_3AF039F8_SLUS-21423.pnach");
			Check(s.state == fe::OnlinePatchStatus::State::Done && s.message.find("from SLUS-21423_7EA439F5") != std::string::npos,
				"same serial, other CRC: the serial's one file is found: " + s.message);
			Check(file.find("[Widescreen 16:9 (from SLUS-21423_7EA439F5)]") != std::string::npos && file.find("// PS5SX2: ") == 0,
				"it is kept under the disc's CRC, its group named after where it came from");
			int trees = 0;
			for (const std::string& u : asked)
				trees += u.find("api.github.com") != std::string::npos;
			Check(trees == 3, "each source's list asked once (" + std::to_string(trees) + ")");
		}
		// A rebuild with no serial of its own (Red Dead Revolver NORG: "NORG-RUS00"), its executable naming SLUS-20500 first,
		// then SLES-52478: only the first serial found is taken (not the PAL release's too).
		const std::string rdr = std::string("\x7f" "ELF..BASLUS-20500\0BESLES-52478", 30);
		{
			fe::OnlinePatches op(platform, patches, cheats, manifest);
			const fe::OnlinePatchStatus s = op.FetchWithExecutable("NORG-RUS00", 0x87844524, std::vector<uint8_t>(rdr.begin(), rdr.end()));
			Check(s.state == fe::OnlinePatchStatus::State::Done && Exists(patches + "/NORG-RUS00_87844524_SLUS-20500.pnach") &&
					  !Exists(patches + "/NORG-RUS00_87844524_SLES-52478.pnach") &&
					  !Exists(patches + "/NORG-RUS00_87844524_gabominated_SLES-52478.pnach"),
				"no serial of its own: the executable's first serial wins: " + s.message);
			const fe::OnlinePatchStatus again = op.FetchWithExecutable("NORG-RUS00", 0x87844524, std::vector<uint8_t>(rdr.begin(), rdr.end()));
			Check(again.state == fe::OnlinePatchStatus::State::Done, "asking again keeps it (the file is this app's)");
		}
		// Two files for one serial (two releases): which one the disc was made from isn't known, so none is taken.
		{
			const std::string two = "\x7f" "ELF...BASLUS-20946";
			fe::OnlinePatches op(platform, patches, cheats, manifest);
			const fe::OnlinePatchStatus s = op.FetchWithExecutable("NORG-00000", 0x12121212, std::vector<uint8_t>(two.begin(), two.end()));
			Check(s.message == "Nothing online for this game" && !Exists(patches + "/NORG-00000_12121212_SLUS-20946.pnach"),
				"two releases of the serial: none taken: " + s.message);
		}
		// The lists can't be had (GitHub's hourly limit): said so, nothing written, and the next game asks again.
		{
			const auto saved = server[pcsx2_tree];
			server[pcsx2_tree] = {403, R"({"message":"API rate limit exceeded"})"};
			server[gab_tree] = {403, R"({"message":"API rate limit exceeded"})"};
			server[cheats_tree] = {403, R"({"message":"API rate limit exceeded"})"};
			fe::OnlinePatches op(platform, patches, cheats, manifest);
			const std::string other = "\x7f" "ELF...BASLUS-21423";
			const fe::OnlinePatchStatus s = op.FetchWithExecutable("SLUS-21423", 0x01020304, std::vector<uint8_t>(other.begin(), other.end()));
			Check(s.state == fe::OnlinePatchStatus::State::Failed && s.message.find("HTTP 403") != std::string::npos &&
					  !Exists(patches + "/SLUS-21423_01020304_SLUS-21423.pnach"),
				"no file list (HTTP 403): a failure that says why, no file: " + s.message);
			server[pcsx2_tree] = saved;
			server[gab_tree] = {200, R"({"tree":[]})"};
			server[cheats_tree] = {200, R"({"tree":[]})"};
			const fe::OnlinePatchStatus again = op.FetchWithExecutable("SLUS-21423", 0x01020304, std::vector<uint8_t>(other.begin(), other.end()));
			Check(again.state == fe::OnlinePatchStatus::State::Done && Exists(patches + "/SLUS-21423_01020304_SLUS-21423.pnach"),
				"a list refused is not kept: asked again, it is found: " + again.message);
		}
		// The disc's own file is there: no fallback at all.
		{
			asked.clear();
			fe::OnlinePatches op(platform, patches, cheats, manifest);
			op.FetchWithExecutable("SLUS-20265", 0x79646C72, std::vector<uint8_t>(lcs_elf.begin(), lcs_elf.end()));
			bool tree_asked = false;
			for (const std::string& u : asked)
				tree_asked |= u.find("api.github.com") != std::string::npos;
			Check(!tree_asked && !Exists(patches + "/SLUS-20265_79646C72_SLUS-21423.pnach"), "a disc with its own file found asks no list");
		}
	}

	// The sheet: the game's Cheats rows, on and off, and EmuCore/EnableCheats with them.
	{
		fe::OptionsPaths paths;
		paths.settings_dir = root + "/settings";
		paths.gs_ini = root + "/gs.ini";
		paths.patches_dir = patches;
		paths.cheats_dir = cheats;
		fe::GameInfo g;
		g.stem = "Game (USA)";
		g.title = "Game";
		g.serial = "SLUS-20265";
		fe::OptionsSheet sheet;
		sheet.SetOnlinePatchRow(true);
		sheet.Open(paths, &g);
		int cheat_rows = 0, online_rows = 0, patch_rows = 0;
		const fe::OptionsSheet::Row* first_cheat = nullptr;
		for (const auto& r : sheet.rows())
		{
			if (r.kind == fe::OptionsSheet::Kind::Cheat)
			{
				cheat_rows++;
				if (!first_cheat)
					first_cheat = &r;
			}
			online_rows += r.kind == fe::OptionsSheet::Kind::OnlinePatches;
			patch_rows += r.kind == fe::OptionsSheet::Kind::Patch;
		}
		Check(online_rows == 1, "the sheet has the Get patches and cheats row");
		Check(cheat_rows == 2, "the sheet lists the 2 cheat groups (" + std::to_string(cheat_rows) + ")");
		Check(patch_rows >= 4, "and the patch groups of both patch files (" + std::to_string(patch_rows) + ")");
		const std::string name = first_cheat ? first_cheat->label : "";
		Check(first_cheat && sheet.Value(*first_cheat) == "Off", "a cheat starts off");
		if (first_cheat)
			sheet.Step(*first_cheat, 1);
		std::string file = Read(paths.settings_dir + "/Game (USA).ini");
		Check(file.find("Cheats/Enable=" + name) != std::string::npos && file.find("EmuCore/EnableCheats=true") != std::string::npos,
			"turning a cheat on writes its line and turns on the game's cheats");
		for (const auto& r : sheet.rows())
			if (r.kind == fe::OptionsSheet::Kind::Cheat && r.label == name)
			{
				Check(sheet.Value(r) == "On", "the row says On");
				sheet.Step(r, 1);
				break;
			}
		file = Read(paths.settings_dir + "/Game (USA).ini");
		Check(file.find("Cheats/Enable=") == std::string::npos && file.find("EnableCheats") == std::string::npos,
			"turning the last cheat off takes both lines out");
	}

	std::printf(g_fails ? "%d FAILED\n" : "all passed\n", g_fails);
	return g_fails ? 1 : 0;
}
