// PS5 port, PC test (gamedb, 2026-10-30, AI-assisted): the eboot's own GameIndex.yaml behind the resources folder's
// (include-orbis/OrbisGameDb.h, orbis-shims/orbis_gamedb.cpp, built here as it is, with the tree's real database embedded).
// The #bugs scan of 2026-10-30: Black, NFS Carbon and Burnout Revenge ran without the database on consoles with no resources folder.
//   ps5/coreorbis/tests/gamedb/test-gamedb.sh
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisGameDb.h"

#include <cstdio>
#include <cstdlib>
#include <string>

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
		char buf[65536];
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

// The entry of `serial` (up to the next entry) holds `line`.
static bool EntryHas(const std::string& db, const char* serial, const char* line)
{
	const size_t at = db.find("\n" + std::string(serial) + ":\n");
	if (at == std::string::npos)
		return false;
	size_t end = at + 1;
	while (true)
	{
		end = db.find('\n', end);
		if (end == std::string::npos || end + 1 >= db.size() || (db[end + 1] != ' ' && db[end + 1] != '#'))
			break;
		end++;
	}
	return db.substr(at, end == std::string::npos ? std::string::npos : end - at).find(line) != std::string::npos;
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: gamedb_test <work folder> <the embedded GameIndex.yaml>\n");
		return 2;
	}
	const std::string work = argv[1];
	const std::string real = Read(argv[2]);
	Check(real.size() > 1000000, "the tree's GameIndex.yaml (" + std::to_string(real.size()) + " bytes)");

	// Before any read (the shelf comes up first): a look at the folder only.
	std::string why;
	Check(OrbisGameDbFolderProblem(work + "/none", &why) && why == work + "/none/GameIndex.yaml not there",
		"before a read, no folder: a problem (" + why + ")");
	std::system(("mkdir -p '" + work + "/ok' '" + work + "/empty' '" + work + "/cut' '" + work + "/other' '" + work + "/dir/GameIndex.yaml'").c_str());
	Write(work + "/ok/GameIndex.yaml", real);
	Check(!OrbisGameDbFolderProblem(work + "/ok", &why) && why.empty(), "before a read, a whole file: no problem");
	Write(work + "/empty/GameIndex.yaml", "");
	Check(OrbisGameDbFolderProblem(work + "/empty", &why) && why.find("is empty") != std::string::npos, "before a read, an empty file: " + why);

	// The eboot's copy is the tree's file, byte for byte.
	const char* data = nullptr;
	size_t size = 0;
	OrbisBuiltinGameDb(&data, &size);
	Check(size == real.size() && std::string(data, size) == real, "the built-in copy is the tree's GameIndex.yaml");
	const std::string builtin(data, size);
	// The three games of the scan: each one's own fix is in it.
	Check(EntryHas(builtin, "SLES-53886", "vuClampMode: 3"), "Black (SLES-53886): vuClampMode 3");
	Check(EntryHas(builtin, "SLES-54492", "eeClampMode: 3"), "NFS Carbon (SLES-54492): eeClampMode 3");
	Check(EntryHas(builtin, "SLES-53507", "halfPixelOffset: 2"), "Burnout Revenge (SLES-53507): halfPixelOffset 2");
	Check(EntryHas(builtin, "SLUS-20047", "recommendedBlendingLevel: 5"), "Gauntlet: Dark Legacy (SLUS-20047): its blending level (no CPU fix to miss)");

	// What counts as the database.
	Check(OrbisGameDbTextUsable(real), "the real database is usable");
	std::string crlf;
	for (char c : real.substr(0, 300000))
	{
		if (c == '\n')
			crlf += '\r';
		crlf += c;
	}
	Check(OrbisGameDbTextUsable(crlf), "with Windows line ends too");
	Check(!OrbisGameDbTextUsable(real.substr(0, 1000)), "a cut-off one (1000 bytes) is not");
	Check(!OrbisGameDbTextUsable(std::string(200000, 'x')), "200 KB of something else is not");
	Check(!OrbisGameDbTextUsable(std::string()), "nothing is not");

	// The reads.
	std::string text, problem;
	Check(OrbisReadGameDb(work + "/ok", &text, &problem) == OrbisGameDbSource::Folder && text == real && problem.empty(),
		"a whole file in the folder: the folder's");
	Check(!OrbisGameDbFolderProblem(work + "/elsewhere", &why), "then the status is the read's (no problem), whatever folder is asked");
	std::string edited = real;
	edited.replace(edited.find("halfPixelOffset: 2", edited.find("\nSLES-53507:")), 18, "halfPixelOffset: 4");
	Write(work + "/ok/GameIndex.yaml", edited);
	Check(OrbisReadGameDb(work + "/ok", &text, &problem) == OrbisGameDbSource::Folder && EntryHas(text, "SLES-53507", "halfPixelOffset: 4"),
		"an edited file in the folder: the edit is used");
	Check(OrbisReadGameDb(work + "/none", &text, &problem) == OrbisGameDbSource::Builtin && text == real &&
			  problem == work + "/none/GameIndex.yaml not there",
		"no folder: the built-in copy (" + problem + ")");
	Check(OrbisGameDbFolderProblem(work + "/none", &why) && why == problem, "and the status says so");
	Check(OrbisReadGameDb(work + "/empty", &text, &problem) == OrbisGameDbSource::Builtin && text == real &&
			  problem.find("is empty") != std::string::npos,
		"an empty file: the built-in copy (" + problem + ")");
	Write(work + "/cut/GameIndex.yaml", real.substr(0, 4096));
	Check(OrbisReadGameDb(work + "/cut", &text, &problem) == OrbisGameDbSource::Builtin && text == real &&
			  problem.find("isn't the game database (4096 bytes)") != std::string::npos,
		"a cut-off file: the built-in copy (" + problem + ")");
	Write(work + "/other/GameIndex.yaml", std::string(300000, '#'));
	Check(OrbisReadGameDb(work + "/other", &text, &problem) == OrbisGameDbSource::Builtin && text == real,
		"another file under the name: the built-in copy");
	Check(OrbisReadGameDb(work + "/dir", &text, &problem) == OrbisGameDbSource::Builtin && text == real,
		"a folder under the name: the built-in copy (" + problem + ")");
	OrbisReadGameDb(work + "/ok", &text, &problem);
	Check(!OrbisGameDbFolderProblem(work + "/ok", &why), "the folder's again: no problem");
	OrbisGameDbNoteFolderUnparsable(work + "/ok");
	Check(OrbisGameDbFolderProblem(work + "/ok", &why) && why == work + "/ok/GameIndex.yaml can't be parsed",
		"PCSX2 couldn't parse the folder's: the status says so");
	Check(OrbisReadBuiltinGameDb(&text) && text == real, "and the built-in copy is there for it");

	std::printf(g_fails ? "%d FAILED\n" : "all passed\n", g_fails);
	return g_fails ? 1 : 0;
}
