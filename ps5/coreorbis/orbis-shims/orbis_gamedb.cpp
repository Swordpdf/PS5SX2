// PS5 port (gamedb, 2026-10-30, AI-assisted): the eboot's own copy of PCSX2's game database (include-orbis/OrbisGameDb.h).
//
// The file is bin/resources/GameIndex.yaml of the tree the eboot is built from, embedded with .incbin (as the GS shaders are,
// orbis_gs_shaders.cpp), so it matches this build's GameDatabase.cpp: the same text the release's resources folder holds.
// Plain C++ and stdio: the host test (tests/gamedb) builds this file as it is, with a small database of its own.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisGameDb.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <sys/stat.h>

#ifndef ORBIS_GAMEDB_FILE
#error "ORBIS_GAMEDB_FILE must name PCSX2's bin/resources/GameIndex.yaml"
#endif

__asm__(".section .rodata.orbis_gamedb_yaml,\"a\",@progbits\n"
		".balign 16\n"
		".global orbis_gamedb_yaml\n"
		"orbis_gamedb_yaml:\n"
		".incbin \"" ORBIS_GAMEDB_FILE "\"\n"
		".global orbis_gamedb_yaml_end\n"
		"orbis_gamedb_yaml_end:\n"
		".byte 0\n"
		".previous\n");

extern "C" const char orbis_gamedb_yaml[], orbis_gamedb_yaml_end[];

namespace
{
std::mutex s_mutex;
bool s_read = false;           // a read has run (OrbisReadGameDb)
bool s_folder_problem = false; // ... and the folder's copy wasn't used
std::string s_why;

// The file's text; false with errno's reason in `why` when it can't be read.
bool ReadFile(const std::string& path, std::string* out, std::string* why)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
	{
		*why = errno == ENOENT ? std::string("not there") : std::string("can't be opened (") + std::strerror(errno) + ")";
		return false;
	}
	out->clear();
	char buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		out->append(buf, n);
	const bool ok = !std::ferror(f);
	std::fclose(f);
	if (!ok)
		*why = "can't be read";
	return ok;
}

void Remember(bool problem, const std::string& why)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	s_read = true;
	s_folder_problem = problem;
	s_why = why;
}
} // namespace

void OrbisBuiltinGameDb(const char** data, size_t* size)
{
	*data = orbis_gamedb_yaml;
	*size = static_cast<size_t>(orbis_gamedb_yaml_end - orbis_gamedb_yaml);
}

bool OrbisGameDbTextUsable(const std::string& text)
{
	// A real GameIndex.yaml: 2.6 MB, ~12800 entries, each a serial at the line's start ("SLUS-20047:") with a "  name:" under it.
	if (text.size() < 64 * 1024)
		return false;
	const size_t probe = std::min<size_t>(text.size(), 256 * 1024);
	int entries = 0;
	for (size_t at = 0; at < probe && entries < 8;)
	{
		const size_t eol = text.find('\n', at);
		const size_t end = eol == std::string::npos ? text.size() : eol;
		// "XXXX-NNNNN:" with letters before the dash and digits after it
		size_t i = at;
		while (i < end && text[i] >= 'A' && text[i] <= 'Z')
			i++;
		if (i > at && i < end && text[i] == '-')
		{
			size_t j = i + 1;
			while (j < end && text[j] >= '0' && text[j] <= '9')
				j++;
			if (j > i + 1 && j < end && text[j] == ':' && eol != std::string::npos && text.compare(end + 1, 7, "  name:") == 0)
				entries++;
		}
		if (eol == std::string::npos)
			break;
		at = eol + 1;
	}
	return entries >= 8;
}

OrbisGameDbSource OrbisReadGameDb(const std::string& resources_dir, std::string* out, std::string* problem)
{
	const std::string path = resources_dir + "/GameIndex.yaml";
	std::string why;
	if (ReadFile(path, out, &why))
	{
		if (OrbisGameDbTextUsable(*out))
		{
			Remember(false, std::string());
			if (problem)
				problem->clear();
			return OrbisGameDbSource::Folder;
		}
		why = out->empty() ? std::string("is empty") : "isn't the game database (" + std::to_string(out->size()) + " bytes)";
	}
	why = path + " " + why;
	if (problem)
		*problem = why;
	Remember(true, why);
	const char* data;
	size_t size;
	OrbisBuiltinGameDb(&data, &size);
	if (size == 0)
	{
		out->clear();
		return OrbisGameDbSource::None;
	}
	out->assign(data, size);
	return OrbisGameDbSource::Builtin;
}

bool OrbisReadBuiltinGameDb(std::string* out)
{
	const char* data;
	size_t size;
	OrbisBuiltinGameDb(&data, &size);
	out->assign(data, size);
	return size > 0;
}

void OrbisGameDbNoteFolderUnparsable(const std::string& resources_dir)
{
	Remember(true, resources_dir + "/GameIndex.yaml can't be parsed");
}

bool OrbisGameDbFolderProblem(const std::string& resources_dir, std::string* why)
{
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (s_read)
		{
			if (why)
				*why = s_why;
			return s_folder_problem;
		}
	}
	// Not read yet (the shelf, before the first game): a look at the folder's file, its size only (the emulator's own read, which
	// checks the text, decides later; the shelf comes up on every start and the file is 2.6 MB).
	const std::string path = resources_dir + "/GameIndex.yaml";
	std::string reason;
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
		reason = errno == ENOENT ? std::string("not there") : std::string("can't be looked at (") + std::strerror(errno) + ")";
	else if (!S_ISREG(st.st_mode))
		reason = "isn't a file";
	else if (st.st_size < 64 * 1024)
		reason = st.st_size == 0 ? std::string("is empty") : "isn't the game database (" + std::to_string(st.st_size) + " bytes)";
	if (why)
		*why = reason.empty() ? std::string() : path + " " + reason;
	return !reason.empty();
}
