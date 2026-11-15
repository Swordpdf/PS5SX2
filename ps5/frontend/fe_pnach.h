// PS5SX2 2.41 (AI-assisted; the owner: "we need to improve how pnatches are handled - the naming is too strict"): which .pnach files
// of patches/ or cheats/ belong to a game. One rule for the core (pcsx2/Patch.cpp, which loads them), the shelf's sheet and the
// settings page (fe_settings.cpp PatchGroups), so what is listed is what is loaded. Header only, the C++ library and POSIX only.
//
// A file is the game's when (letter case and the separators - _ . and space never matter):
//   1. it lies in a folder named after the game: patches/<serial>/ or cheats/<serial>/ (SLUS-20552, slus_20552, SLUS 205.52 ...), or
//      a folder with the game's file name (cheats/Grand Theft Auto - Vice City (USA)/), whatever the file inside is called;
//   2. its name holds the game's serial anywhere ("SLUS-20552 60fps.pnach", PCSX2's own "SLUS-20552_20B19E49.pnach") or the game's
//      CRC as a word of its own ("GTA VC (20B19E49).pnach", PCSX2 1.x's "20B19E49.pnach");
//   3. its name is the game's file name ("Grand Theft Auto - Vice City (USA).pnach"), as settings/<file name>.ini is;
//   4. its first lines hold a gametitle= line with the serial (PCSX2 1.x files: gametitle=Grand Theft Auto - Vice City [SLUS 20552] (U)).
// Version check: patches write fixed addresses, so one made for another version of the game can crash it. A file that names CRCs (in
// its name, or a crc= line) and none of them is the game's is "for another version": the core leaves it out and says so, the shelf
// shows it as such. A file that names no CRC is taken (most hand-made files name none).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

namespace fe::pnach
{
// The game a file is matched against. crc 0: not known (the shelf before the game's first start); stem: the game's file name
// without its extension ("" when there is none, a disc in the drive).
struct Game
{
	std::string serial;
	uint32_t crc = 0;
	std::string stem;
};

enum class Why
{
	None,       // not this game's
	Folder,     // rule 1
	Serial,     // rule 2, the serial in the name
	Crc,        // rule 2, the CRC in the name
	FileName,   // rule 3
	GameTitle,  // rule 4
};

inline const char* WhyText(Why w)
{
	switch (w)
	{
		case Why::Folder: return "its folder is named after the game";
		case Why::Serial: return "its name holds the serial";
		case Why::Crc: return "its name holds the CRC";
		case Why::FileName: return "its name is the game's file name";
		case Why::GameTitle: return "its gametitle line holds the serial";
		default: return "not this game's";
	}
}

struct Match
{
	Why why = Why::None;
	bool other_version = false;     // the game's, but it names CRCs and none is the game's
	std::vector<uint32_t> crcs;     // the CRCs it names
	bool Ours() const { return why != Why::None; }
	bool Load() const { return Ours() && !other_version; }
};

inline std::string Lower(std::string_view s)
{
	std::string out(s);
	for (char& c : out)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

// Letters and digits only, lower case: "Grand Theft Auto - Vice City (USA)" and "grand_theft_auto_vice_city_(usa)" are the same name.
inline std::string Squash(std::string_view s)
{
	std::string out;
	for (const char c : s)
		if (std::isalnum(static_cast<unsigned char>(c)))
			out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	return out;
}

// Every serial in `text`: 4 letters (not right after a letter), an optional - _ . or space, 3 digits, an optional . _ or space, 2 digits
// (not followed by a digit). Upper case, no separators: "SLUS20552".
inline std::vector<std::string> SerialsIn(std::string_view t)
{
	std::vector<std::string> out;
	auto alpha = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; };
	auto digit = [](char c) { return c >= '0' && c <= '9'; };
	auto sep = [](char c) { return c == '-' || c == '_' || c == '.' || c == ' '; };
	for (size_t i = 0; i + 9 <= t.size(); i++)
	{
		if (i > 0 && alpha(t[i - 1]))
			continue;
		if (!alpha(t[i]) || !alpha(t[i + 1]) || !alpha(t[i + 2]) || !alpha(t[i + 3]))
			continue;
		size_t j = i + 4;
		if (j < t.size() && sep(t[j]))
			j++;
		if (j + 3 > t.size() || !digit(t[j]) || !digit(t[j + 1]) || !digit(t[j + 2]))
			continue;
		std::string s;
		for (size_t k = i; k < i + 4; k++)
			s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(t[k]))));
		s.append(t.substr(j, 3));
		j += 3;
		if (j < t.size() && (t[j] == '.' || t[j] == '_' || t[j] == ' ') && j + 1 < t.size() && digit(t[j + 1]))
			j++;
		if (j + 2 > t.size() || !digit(t[j]) || !digit(t[j + 1]) || (j + 2 < t.size() && digit(t[j + 2])))
			continue;
		s.append(t.substr(j, 2));
		if (std::find(out.begin(), out.end(), s) == out.end())
			out.push_back(s);
	}
	return out;
}

// "SLUS-20552", "slus_205.52", "SLUS 20552", "SLUS20552" -> "SLUS20552"; "" when `s` is not one serial.
inline std::string NormSerial(std::string_view s)
{
	const std::vector<std::string> found = SerialsIn(s);
	if (found.size() != 1)
		return {};
	// the whole of `s` must be the serial (and separators): "SLUS-20552 widescreen" is a name with a serial, not a serial
	std::string rest;
	for (const char c : s)
		if (std::isalnum(static_cast<unsigned char>(c)))
			rest.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
	return rest == found[0] ? found[0] : std::string();
}

// Every CRC in `text`: 8 hex digits as a word of their own (no letter or digit right before or after). Hex words of 8 that are
// plain decimal numbers count too (a CRC can be 20552000); "0x" before one is allowed.
inline std::vector<uint32_t> CrcsIn(std::string_view t)
{
	std::vector<uint32_t> out;
	auto hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
	auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
	for (size_t i = 0; i + 8 <= t.size(); i++)
	{
		size_t start = i;
		if (i + 10 <= t.size() && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X') && (i == 0 || !word(t[i - 1])))
			start = i + 2;
		else if (i > 0 && word(t[i - 1]))
			continue;
		if (start + 8 > t.size())
			continue;
		bool ok = true;
		for (size_t k = start; k < start + 8; k++)
			ok = ok && hex(t[k]);
		if (!ok || (start + 8 < t.size() && word(t[start + 8])))
			continue;
		const uint32_t v = static_cast<uint32_t>(std::stoul(std::string(t.substr(start, 8)), nullptr, 16));
		if (std::find(out.begin(), out.end(), v) == out.end())
			out.push_back(v);
		i = start + 7;
	}
	return out;
}

// The serials of the gametitle= lines and the CRCs of crc= lines in the file's first lines (`head`).
inline void ReadHead(std::string_view head, std::vector<std::string>& serials, std::vector<uint32_t>& crcs)
{
	size_t at = 0;
	while (at < head.size())
	{
		size_t end = head.find('\n', at);
		if (end == std::string_view::npos)
			end = head.size();
		std::string_view line = head.substr(at, end - at);
		at = end + 1;
		while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
			line.remove_prefix(1);
		const std::string low = Lower(line.substr(0, 10));
		if (low.compare(0, 9, "gametitle") == 0)
		{
			for (const std::string& s : SerialsIn(line))
				if (std::find(serials.begin(), serials.end(), s) == serials.end())
					serials.push_back(s);
		}
		else if (low.compare(0, 3, "crc") == 0 && line.size() > 3)
		{
			size_t k = 3;
			while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
				k++;
			if (k < line.size() && (line[k] == '=' || line[k] == ':'))
				for (const uint32_t c : CrcsIn(line.substr(k + 1)))
					if (std::find(crcs.begin(), crcs.end(), c) == crcs.end())
						crcs.push_back(c);
		}
	}
}

inline bool IsPnachName(std::string_view name)
{
	return name.size() > 6 && Lower(name.substr(name.size() - 6)) == ".pnach";
}

// Whether the file `name` (in the subfolder `folder` of patches/ or cheats/, "" for the folder itself; `head`: its first lines,
// "" when not read: then rule 4 can't match and a crc= line is not seen) is the game's.
inline Match Classify(const Game& game, std::string_view folder, std::string_view name, std::string_view head)
{
	Match m;
	std::string_view base = name;
	if (IsPnachName(base))
		base.remove_suffix(6);
	const std::string serial = NormSerial(game.serial);
	const std::vector<std::string> name_serials = SerialsIn(base);
	m.crcs = CrcsIn(base);
	std::vector<std::string> title_serials;
	ReadHead(head, title_serials, m.crcs);
	const bool crc_known = game.crc != 0;
	const bool names_our_crc = crc_known && std::find(m.crcs.begin(), m.crcs.end(), game.crc) != m.crcs.end();
	if (!folder.empty())
	{
		if ((!serial.empty() && NormSerial(folder) == serial) || (!game.stem.empty() && Squash(folder) == Squash(game.stem)))
			m.why = Why::Folder;
	}
	else if (!serial.empty() && std::find(name_serials.begin(), name_serials.end(), serial) != name_serials.end())
		m.why = Why::Serial;
	else if (names_our_crc && name_serials.empty())
		m.why = Why::Crc;
	else if (!game.stem.empty() && Squash(base) == Squash(game.stem))
		m.why = Why::FileName;
	else if (!serial.empty() && name_serials.empty() && std::find(title_serials.begin(), title_serials.end(), serial) != title_serials.end())
		m.why = Why::GameTitle;
	if (m.Ours() && crc_known && !m.crcs.empty() && !names_our_crc)
		m.other_version = true;
	return m;
}

// A file no game can be matched to by its name or head alone: in the folder itself, no serial and no CRC in its name and no gametitle
// serial. (Rule 3, the game's file name, can still match one: the callers check that against the games they know.)
inline bool NamesNoGame(std::string_view name, std::string_view head)
{
	std::string_view base = name;
	if (IsPnachName(base))
		base.remove_suffix(6);
	std::vector<std::string> title_serials;
	std::vector<uint32_t> head_crcs;
	ReadHead(head, title_serials, head_crcs);
	return SerialsIn(base).empty() && CrcsIn(base).empty() && title_serials.empty();
}

// The first `max` bytes of a file ("" when it can't be read).
inline std::string ReadHeadOf(const std::string& path, size_t max = 4096)
{
	std::string out;
	if (FILE* f = std::fopen(path.c_str(), "rb"))
	{
		out.resize(max);
		out.resize(std::fread(out.data(), 1, max, f));
		std::fclose(f);
	}
	return out;
}

struct Found
{
	std::string path;    // the whole path
	std::string folder;  // the subfolder ("" for the folder itself)
	std::string name;    // the file's name
	Match match;
};

// Every .pnach of `dir` and of its folders (one level down; hidden ones and "." ".." left out), with its match for `game`, sorted by
// path. `head_all`: read every file's first lines (the core); else only the ones whose name and folder can't decide (the shelf, which
// lists many games' files). `unmatched` (may be null) gets the files of `dir` itself that name no game at all.
inline std::vector<Found> List(const std::string& dir, const Game& game, bool head_all, std::vector<std::string>* unmatched = nullptr)
{
	std::vector<Found> out;
	auto walk = [&](const std::string& d, const std::string& folder, auto& self) -> void {
		DIR* h = opendir(d.c_str());
		if (!h)
			return;
		std::vector<std::string> names, dirs;
		while (dirent* e = readdir(h))
		{
			const std::string n = e->d_name;
			if (n.empty() || n[0] == '.')
				continue;
			struct stat st = {};
			if (stat((d + "/" + n).c_str(), &st) != 0)
				continue;
			if (S_ISDIR(st.st_mode))
			{
				if (folder.empty())
					dirs.push_back(n);
			}
			else if (S_ISREG(st.st_mode) && IsPnachName(n))
				names.push_back(n);
		}
		closedir(h);
		for (const std::string& n : names)
		{
			const std::string path = d + "/" + n;
			std::string head;
			std::string_view base = n;
			base.remove_suffix(6);
			const bool decided = !folder.empty() || !SerialsIn(base).empty() || !CrcsIn(base).empty();
			if (head_all || !decided)
				head = ReadHeadOf(path);
			Found f{path, folder, n, Classify(game, folder, n, head)};
			if (unmatched && folder.empty() && !f.match.Ours() && NamesNoGame(n, head))
				unmatched->push_back(n);
			out.push_back(std::move(f));
		}
		for (const std::string& sub : dirs)
			self(d + "/" + sub, sub, self);
	};
	walk(dir, std::string(), walk);
	std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return a.path < b.path; });
	if (unmatched)
		std::sort(unmatched->begin(), unmatched->end());
	return out;
}

// cache/game-crcs.txt: "<serial> <crc>" lines, the CRC each serial last started with (the core writes it; the shelf, which doesn't
// read the game's executable, takes the CRC from it for the version check and rule 2's CRC names).
inline uint32_t KnownCrc(const std::string& file, const std::string& serial)
{
	const std::string want = NormSerial(serial);
	uint32_t crc = 0;
	if (want.empty())
		return 0;
	if (FILE* f = std::fopen(file.c_str(), "r"))
	{
		char s[64] = {}, c[16] = {};
		while (std::fscanf(f, "%63s %15s", s, c) == 2)
			if (NormSerial(s) == want)
				crc = static_cast<uint32_t>(std::strtoul(c, nullptr, 16));
		std::fclose(f);
	}
	return crc;
}

inline void RememberCrc(const std::string& file, const std::string& serial, uint32_t crc)
{
	const std::string want = NormSerial(serial);
	if (want.empty() || crc == 0 || KnownCrc(file, serial) == crc)
		return;
	std::vector<std::string> keep;
	if (FILE* f = std::fopen(file.c_str(), "r"))
	{
		char s[64] = {}, c[16] = {};
		while (std::fscanf(f, "%63s %15s", s, c) == 2)
			if (NormSerial(s) != want)
				keep.push_back(std::string(s) + " " + c);
		std::fclose(f);
	}
	char line[48];
	std::snprintf(line, sizeof(line), "%s %08X", want.c_str(), crc);
	keep.push_back(line);
	const std::string tmp = file + ".tmp";
	if (FILE* f = std::fopen(tmp.c_str(), "w"))
	{
		for (const std::string& l : keep)
			std::fprintf(f, "%s\n", l.c_str());
		std::fclose(f);
		std::rename(tmp.c_str(), file.c_str());
	}
}
} // namespace fe::pnach
