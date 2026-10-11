// PS5 port frontend (vk-285-134, AI-assisted): the PS2 BIOS, before a game starts. See fe_bios.h.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_bios.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fe
{
namespace
{
struct Entry
{
	std::string path, name, ext; // ext lower case, without the dot
	uint64_t size = 0;
	bool dir = false;
	int depth = 0;
};

std::string Lower(std::string s)
{
	for (char& c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::string ExtOf(const std::string& name)
{
	const size_t dot = name.find_last_of('.');
	return dot == std::string::npos || dot == 0 ? std::string() : Lower(name.substr(dot + 1));
}

bool IsDir(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// The entries of `dir` and of its folders down to `max_depth` (0: its own only), at most `limit`, sorted by name in each
// folder. Hidden names and the system's ($RECYCLE.BIN, ...) are left out.
void Walk(const std::string& dir, int depth, int max_depth, size_t limit, std::vector<Entry>* out)
{
	std::vector<std::string> names;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (const dirent* e = readdir(d))
		{
			if (e->d_name[0] != '.' && e->d_name[0] != '$')
				names.emplace_back(e->d_name);
		}
		closedir(d);
	}
	std::sort(names.begin(), names.end());
	for (const std::string& name : names)
	{
		if (out->size() >= limit)
			return;
		Entry e;
		e.path = dir + "/" + name;
		e.name = name;
		e.depth = depth;
		struct stat st = {};
		if (stat(e.path.c_str(), &st) != 0)
			continue;
		e.dir = S_ISDIR(st.st_mode);
		if (!e.dir && !S_ISREG(st.st_mode))
			continue;
		e.size = static_cast<uint64_t>(st.st_size);
		e.ext = e.dir ? std::string() : ExtOf(name);
		out->push_back(e);
		if (e.dir && depth < max_depth)
			Walk(e.path, depth + 1, max_depth, limit, out);
	}
}

bool IsArchive(const std::string& ext)
{
	return ext == "zip" || ext == "rar" || ext == "7z";
}

// A name that isn't there yet in `dir`: `name`, else "name (from the archive)".
std::string FreeName(const std::string& dir, const std::string& name)
{
	std::string path = dir + "/" + name;
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0)
		return path;
	const size_t dot = name.find_last_of('.');
	const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
	const std::string ext = dot == std::string::npos ? std::string() : name.substr(dot);
	for (int i = 1; i < 100; i++)
	{
		path = dir + "/" + stem + " (from the archive" + (i > 1 ? " " + std::to_string(i) : std::string()) + ")" + ext;
		if (stat(path.c_str(), &st) != 0)
			return path;
	}
	return std::string();
}

// One archive: its entries of a BIOS's size, written to `dest_dir` and checked. The path kept, or "".
std::string FromArchive(const std::string& archive_path, const std::string& dest_dir, const std::function<bool(const std::string&)>& is_bios)
{
	struct archive* a = archive_read_new();
	if (!a)
		return std::string();
	// The vendored libarchive reads zip and RAR (fe_texpacks.cpp's packs); a .7z doesn't open, and the shelf's sentence then
	// asks for it unpacked.
	archive_read_support_format_zip(a);
	archive_read_support_format_rar(a);
	archive_read_support_format_rar5(a);
	std::string kept;
	if (archive_read_open_filename(a, archive_path.c_str(), 64 * 1024) != ARCHIVE_OK)
	{
		std::printf("[bios] %s doesn't open: %s\n", archive_path.c_str(), archive_error_string(a) ? archive_error_string(a) : "?");
		std::fflush(stdout);
		archive_read_free(a);
		return kept;
	}
	struct archive_entry* entry = nullptr;
	int looked = 0;
	while (kept.empty() && looked < 4096 && archive_read_next_header(a, &entry) == ARCHIVE_OK)
	{
		looked++;
		const la_int64_t size = archive_entry_size(entry);
		const char* const pathname = archive_entry_pathname(entry);
		if (archive_entry_filetype(entry) != AE_IFREG || !pathname || size < static_cast<la_int64_t>(kBiosMinSize) ||
			size > static_cast<la_int64_t>(kBiosMaxSize))
		{
			archive_read_data_skip(a);
			continue;
		}
		std::string name = pathname;
		if (const size_t slash = name.find_last_of("/\\"); slash != std::string::npos)
			name = name.substr(slash + 1);
		if (name.empty())
			continue;
		mkdir(dest_dir.c_str(), 0777);
		const std::string out = FreeName(dest_dir, name);
		if (out.empty())
			break;
		const std::string tmp = out + ".part";
		FILE* f = std::fopen(tmp.c_str(), "wb");
		if (!f)
		{
			std::printf("[bios] can't write %s (errno %d)\n", tmp.c_str(), errno);
			std::fflush(stdout);
			break;
		}
		bool ok = true;
		std::vector<char> buf(64 * 1024); // the heap: the shelf's threads keep small stacks
		for (;;)
		{
			const la_ssize_t n = archive_read_data(a, buf.data(), buf.size());
			if (n == 0)
				break;
			if (n < 0 || std::fwrite(buf.data(), 1, static_cast<size_t>(n), f) != static_cast<size_t>(n))
			{
				ok = false;
				break;
			}
		}
		ok = (std::fclose(f) == 0) && ok;
		if (!ok || std::rename(tmp.c_str(), out.c_str()) != 0)
		{
			std::printf("[bios] %s in %s couldn't be taken out: %s (errno %d)\n", name.c_str(), archive_path.c_str(),
				archive_error_string(a) ? archive_error_string(a) : "write error", errno);
			std::fflush(stdout);
			std::remove(tmp.c_str());
			continue;
		}
		if (is_bios(out))
			kept = out;
		else
			std::remove(out.c_str());
	}
	archive_read_free(a);
	return kept;
}
} // namespace

std::string ExtractBiosFromArchives(const std::vector<std::string>& dirs, const std::string& dest_dir,
	const std::function<bool(const std::string&)>& is_bios, std::set<std::string>* tried, std::string* from)
{
	for (const std::string& dir : dirs)
	{
		std::vector<Entry> entries;
		Walk(dir, 0, 3, 2000, &entries);
		for (const Entry& e : entries)
		{
			// An archive the size of a BIOS pack (a game's would be far bigger), not seen before.
			if (e.dir || !IsArchive(e.ext) || e.size > 256u * 1024u * 1024u)
				continue;
			if (tried && !tried->insert(e.path).second)
				continue;
			const std::string kept = FromArchive(e.path, dest_dir, is_bios);
			if (!kept.empty())
			{
				if (from)
					*from = e.path;
				return kept;
			}
		}
	}
	return std::string();
}

std::string DescribeBiosProblem(const std::string& bios_dir, const std::string& top_dir, const std::vector<std::string>& dirs,
	const std::function<bool(const std::string&)>& is_bios)
{
	const std::string wanted = top_dir + "/bios";
	std::vector<Entry> entries;
	const bool no_folder = bios_dir == top_dir || !IsDir(bios_dir);
	if (!no_folder)
		Walk(bios_dir, 0, 3, 500, &entries);
	for (const std::string& dir : dirs)
	{
		if (dir == bios_dir)
			continue;
		std::vector<Entry> more;
		Walk(dir, 0, 0, 200, &more);
		for (Entry& e : more)
		{
			// Of the other folders only files named like a BIOS (a drive's root holds all sorts of 4 MB files).
			const std::string lower = Lower(e.name);
			const bool named = lower.find("scph") != std::string::npos || lower.find("bios") != std::string::npos ||
			                   lower.find("ps2") != std::string::npos || lower.find("psx") != std::string::npos;
			if (!e.dir && named)
			{
				e.depth = 9; // not in the BIOS folder
				entries.push_back(e);
			}
		}
	}
	const Entry *big = nullptr, *part = nullptr, *small = nullptr, *empty = nullptr, *archive = nullptr, *game = nullptr;
	size_t files = 0, folders = 0;
	for (const Entry& e : entries)
	{
		if (e.dir)
		{
			folders += e.depth == 0 ? 1 : 0;
			continue;
		}
		files++;
		const bool parts_ext = e.ext == "rom1" || e.ext == "rom2" || e.ext == "erom" || e.ext == "nvm" || e.ext == "mec";
		if (e.size == 0)
			empty = empty ? empty : &e;
		else if (e.size >= kBiosMinSize && e.size <= kBiosMaxSize && !IsArchive(e.ext))
		{
			if (!big && !is_bios(e.path))
				big = &e;
		}
		else if (parts_ext)
			part = part ? part : &e;
		else if (e.size == 524288 || (e.size > 200u * 1024u && e.size < kBiosMinSize && (e.ext == "bin" || e.ext == "rom")))
			small = small ? small : &e;
		else if (IsArchive(e.ext))
			archive = archive ? archive : &e;
		else if (e.size > 50u * 1024u * 1024u && (e.ext == "iso" || e.ext == "chd" || e.ext == "cso" || e.ext == "bin" || e.ext == "img" ||
													 e.ext == "zso" || e.ext == "gz" || e.ext == "mdf"))
			game = game ? game : &e;
	}
	const std::string where = no_folder ? wanted : bios_dir;
	if (big)
		return big->name + " is the size of a PS2 BIOS but isn't one (damaged, or not a BIOS): copy the BIOS again.";
	if (part)
		return "Only parts of a PS2 BIOS are there (" + part->name + "): the main file, 4 MB (.bin or .ROM0), is missing.";
	if (small)
		return small->name + " (512 KB) is a PS1 BIOS or a part of a PS2 one: PS5SX2 needs the PS2 BIOS file, 4 MB.";
	if (archive)
		return "No PS2 BIOS could be taken out of " + archive->name + ": unpack it on a computer and copy the BIOS file to " + where + ".";
	if (empty)
		return empty->name + " is empty (0 bytes): copy it again.";
	if (game)
		return game->name + " is a game, not a BIOS: games go in " + top_dir + "/games.";
	if (no_folder)
		return "There is no bios folder: make " + wanted + " and copy your BIOS file into it.";
	if (files == 0 && folders == 0)
		return where + " is empty.";
	if (files == 0)
		return "The folders in " + where + " have no BIOS file in them.";
	return "Nothing in " + where + " is a PS2 BIOS (" + std::to_string(files) + (files == 1 ? " file)." : " files).");
}
} // namespace fe
