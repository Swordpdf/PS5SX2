// PS5 port frontend: the settings files (see fe_settings.h). The first part is fe_web.cpp's code as it was (vk-285-113),
// moved here unchanged so the shelf's options sheet uses the same reader and writer as the settings page.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_settings.h"
#include "fe_games.h" // 2026-10-08: GameDbHwFixes

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace fe::settings
{
std::string Lower(std::string s)
{
	for (char& c : s)
		c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
	return s;
}

std::string Trim(const std::string& s)
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
		b++;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
		e--;
	return s.substr(b, e - b);
}


bool ReadFile(const std::string& path, std::string& out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out.clear();
	char buf[4096]; // small: this runs on the server thread's stack
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	std::fclose(f);
	return true;
}

// Written beside, then renamed over: the GS thread polls these files and must never read half of one.
// vk-285-121 (AI-assisted): a missing folder is made first, one level -- nothing made settings/, so on a console set up
// without it every save of a game's settings failed with errno 2 (two testers' 1.7 logs: 36 page saves, 9 sheet saves).
bool WriteFileAtomic(const std::string& path, const std::string& data)
{
	const std::string tmp = path + ".tmp";
	FILE* f = std::fopen(tmp.c_str(), "wb");
	if (!f && errno == ENOENT)
	{
		const size_t slash = path.rfind('/');
		if (slash != std::string::npos && slash > 0)
		{
			const std::string dir = path.substr(0, slash);
			if (mkdir(dir.c_str(), 0777) == 0)
				std::printf("[settings] made %s for %s\n", dir.c_str(), path.c_str() + slash + 1);
			f = std::fopen(tmp.c_str(), "wb"); // made now, or by another thread meanwhile
		}
	}
	if (!f)
		return false;
	const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
	if (std::fclose(f) != 0 || !ok)
	{
		unlink(tmp.c_str());
		return false;
	}
	if (std::rename(tmp.c_str(), path.c_str()) != 0)
	{
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}


// ---- settings files --------------------------------------------------------------------------
// The port's ini lines are "key=value": a bare key is in EmuCore/GS, others carry their section
// ("EmuCore/Speedhacks/EECycleRate"). Patches/Enable lines repeat, one group name each
// (main-boot.cpp orbis_apply_ini_file).


std::string CanonKey(std::string key)
{
	key = Trim(key);
	if (key.compare(0, 11, "EmuCore/GS/") == 0)
		key.erase(0, 11);
	return key;
}

std::vector<IniLine> ParseIni(const std::string& text)
{
	std::vector<IniLine> lines;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos)
			nl = text.size();
		IniLine l;
		l.raw = text.substr(pos, nl - pos);
		if (!l.raw.empty() && l.raw.back() == '\r')
			l.raw.pop_back();
		const std::string t = Trim(l.raw);
		const size_t eq = t.find('=');
		if (!t.empty() && t[0] != '#' && t[0] != ';' && eq != std::string::npos)
		{
			l.kv = true;
			l.key = CanonKey(t.substr(0, eq));
			l.value = Trim(t.substr(eq + 1));
		}
		lines.push_back(std::move(l));
		pos = nl + 1;
	}
	return lines;
}

bool IsListKey(const std::string& key)
{
	return key == "Patches/Enable" || key == "Patches/Disable" || key == "Cheats/Enable" || key == "Cheats/Disable";
}

bool SafeKey(const std::string& key)
{
	if (key.empty() || key.size() > 80 || IsListKey(key))
		return false;
	for (char c : key)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '/'))
			return false;
	// pr9n (AI-assisted), PR #9 review item 4: nothing in [Achievements] from the page or the sheet. The page answers anyone
	// on the LAN, and Achievements/Host pointed at another https server would have had the next sign-in send the user's
	// token there (Token and Username would replace the account). The account comes from the sign-in only.
	std::string first = key.substr(0, key.find('/'));
	for (char& c : first)
		c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
	if (first == "achievements")
		return false;
	return true;
}

bool SafeValue(const std::string& v)
{
	if (v.size() > 200)
		return false;
	for (char c : v)
		if (c == '\n' || c == '\r' || c == '#')
			return false;
	return true;
}

// ---- memory cards (vk-285-113) ---------------------------------------------------------------
// The page lists the cards PCSX2 offers from memcards/ (its own rule: a file ending .ps2, .mcr, .mcd, .bin or .mc2 that is
// at least a PS1 card's size) and makes new blank PS2 cards of 8, 16, 32 or 64 MB the way the core's FileMcd_CreateNewCard
// does: a file of 0xFF bytes, a card's raw size with its ECC bytes (1024 * 528 * 2 a MB). Which card sits in a slot is an
// ordinary setting (MemoryCards/Slot1_Filename), for all games or one.


bool HasCardExtension(const std::string& name)
{
	for (const char* ext : {".ps2", ".mcr", ".mcd", ".bin", ".mc2"})
	{
		const size_t n = std::strlen(ext);
		if (name.size() > n && name.compare(name.size() - n, n, ext) == 0)
			return true;
	}
	return false;
}

// A name the page makes: letters, digits, space, '_', '-', '.', '(' and ')', starting with a letter or digit, ending in
// ".ps2" after at least one character that is not a space or a dot, 64 characters at most.
bool NewCardNameOk(const std::string& name)
{
	if (name.size() < 5 || name.size() > 64 || name.compare(name.size() - 4, 4, ".ps2") != 0)
		return false;
	for (char c : name)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-' ||
				c == '.' || c == '(' || c == ')'))
			return false;
	const char first = name[0], last = name[name.size() - 5];
	return ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || (first >= '0' && first <= '9')) && last != ' ' && last != '.';
}

std::vector<CardFile> ListCards(const std::string& dir)
{
	std::vector<CardFile> out;
	DIR* d = dir.empty() ? nullptr : opendir(dir.c_str());
	if (!d)
		return out;
	while (dirent* e = readdir(d))
	{
		const std::string name = e->d_name;
		// Names the settings file can hold (no '#', no line breaks) and the page can show.
		if (name.empty() || name[0] == '.' || !HasCardExtension(name) || !SafeValue(name))
			continue;
		struct stat st = {};
		if (stat((dir + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode) || static_cast<uint64_t>(st.st_size) < kPs1CardBytes)
			continue;
		out.push_back({name, static_cast<uint64_t>(st.st_size)});
	}
	closedir(d);
	std::sort(out.begin(), out.end(), [](const CardFile& a, const CardFile& b) {
		const std::string la = Lower(a.name), lb = Lower(b.name);
		return la != lb ? la < lb : a.name < b.name;
	});
	return out;
}


// The groups ("[60 FPS]") of the game's patch files, <serial>_<crc>.pnach.

std::vector<PatchGroup> PatchGroups(const std::string& dir, const std::string& serial)
{
	std::vector<PatchGroup> out;
	if (serial.empty())
		return out;
	DIR* d = opendir(dir.c_str());
	if (!d)
		return out;
	const std::string prefix = Lower(serial) + "_";
	std::vector<std::string> files;
	while (dirent* e = readdir(d))
	{
		const std::string name = e->d_name, low = Lower(name);
		if (low.compare(0, prefix.size(), prefix) == 0 && low.size() > 6 && low.compare(low.size() - 6, 6, ".pnach") == 0)
			files.push_back(name);
	}
	closedir(d);
	std::sort(files.begin(), files.end());
	for (const std::string& file : files)
	{
		std::string text;
		if (!ReadFile(dir + "/" + file, text))
			continue;
		PatchGroup* current = nullptr;
		for (const IniLine& l : ParseIni(text))
		{
			const std::string t = Trim(l.raw);
			if (t.size() > 2 && t.front() == '[' && t.back() == ']')
			{
				const std::string name = Trim(t.substr(1, t.size() - 2));
				current = nullptr;
				if (name.empty())
					continue;
				auto it = std::find_if(out.begin(), out.end(), [&](const PatchGroup& g) { return g.name == name; });
				if (it == out.end())
				{
					out.push_back({name, file, {}});
					current = &out.back();
				}
			}
			else if (current && l.kv && Lower(l.key) == "description" && current->description.empty())
				current->description = l.value;
		}
	}
	return out;
}


// ---- recommended settings (vk-285-51) -------------------------------------------------------

// What a settings text sets, as the port's reader applies it: a key set twice counts with its last
// line; Patches/Enable lines add up.

IniState ReadState(const std::string& text)
{
	IniState st;
	for (const IniLine& l : ParseIni(text))
	{
		if (!l.kv)
			continue;
		if (l.key == "Patches/Enable")
		{
			if (std::find(st.enabled.begin(), st.enabled.end(), l.value) == st.enabled.end())
				st.enabled.push_back(l.value);
			continue;
		}
		if (l.key == "Cheats/Enable") // 2026-10-08
		{
			if (std::find(st.cheats.begin(), st.cheats.end(), l.value) == st.cheats.end())
				st.cheats.push_back(l.value);
			continue;
		}
		if (IsListKey(l.key))
			continue;
		auto it = std::find_if(st.kv.begin(), st.kv.end(), [&](const std::pair<std::string, std::string>& p) { return p.first == l.key; });
		if (it != st.kv.end())
			it->second = l.value;
		else
			st.kv.emplace_back(l.key, l.value);
	}
	return st;
}


// vk-285-110: PS5SX2/ keys (the game language) are the player's own choices, not tuning: the Recommended
// button keeps them, and they don't stop a file from counting as the recommended one. (vk-285-113: so are the
// MemoryCards/ keys, the cards in the slots.)
bool IsPlayerKey(const std::string& key)
{
	return key.compare(0, 7, "PS5SX2/") == 0 || key.compare(0, 12, "MemoryCards/") == 0; // vk-285-113: the cards in the slots too
}

bool SameState(IniState a, IniState b)
{
	for (IniState* st : {&a, &b})
		st->kv.erase(std::remove_if(st->kv.begin(), st->kv.end(),
						 [](const std::pair<std::string, std::string>& p) { return IsPlayerKey(p.first); }),
			st->kv.end());
	std::sort(a.kv.begin(), a.kv.end());
	std::sort(b.kv.begin(), b.kv.end());
	std::sort(a.enabled.begin(), a.enabled.end());
	std::sort(b.enabled.begin(), b.enabled.end());
	return a.kv == b.kv && a.enabled == b.enabled;
}

// "key=value (was old)"-style notes for the settings log: what `after` sets differently from
// `before`, joined with "; ".
std::string DescribeChanges(const IniState& before, const IniState& after)
{
	std::string out;
	auto add = [&](const std::string& s) { out += (out.empty() ? "" : "; ") + s; };
	auto find = [](const IniState& st, const std::string& k) -> const std::string* {
		for (const auto& p : st.kv)
			if (p.first == k)
				return &p.second;
		return nullptr;
	};
	for (const auto& p : after.kv)
	{
		const std::string* was = find(before, p.first);
		if (!was)
			add(p.first + "=" + p.second + " (was unset)");
		else if (*was != p.second)
			add(p.first + "=" + p.second + " (was " + *was + ")");
	}
	for (const auto& p : before.kv)
		if (!find(after, p.first))
			add("unset " + p.first + " (was " + p.second + ")");
	for (const std::string& e : after.enabled)
		if (std::find(before.enabled.begin(), before.enabled.end(), e) == before.enabled.end())
			add("patch on: " + e);
	for (const std::string& e : before.enabled)
		if (std::find(after.enabled.begin(), after.enabled.end(), e) == after.enabled.end())
			add("patch off: " + e);
	for (const std::string& e : after.cheats) // 2026-10-08
		if (std::find(before.cheats.begin(), before.cheats.end(), e) == before.cheats.end())
			add("cheat on: " + e);
	for (const std::string& e : before.cheats)
		if (std::find(after.cheats.begin(), after.cheats.end(), e) == after.cheats.end())
			add("cheat off: " + e);
	return out;
}

// One section of presets.ini ("[@global]", "[SLUS-20733]"): its lines as they are, comments
// included. False when there is none.
bool PresetSection(const std::string& presets, const std::string& id, std::string& out)
{
	out.clear();
	if (id.empty())
		return false;
	bool found = false, in = false;
	size_t pos = 0;
	while (pos < presets.size())
	{
		size_t nl = presets.find('\n', pos);
		if (nl == std::string::npos)
			nl = presets.size();
		std::string line = presets.substr(pos, nl - pos);
		pos = nl + 1;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const std::string t = Trim(line);
		if (t.size() > 2 && t.front() == '[' && t.back() == ']')
		{
			in = Lower(Trim(t.substr(1, t.size() - 2))) == Lower(id);
			found = found || in;
			continue;
		}
		if (in)
			out += line + "\n";
	}
	// The blank lines between sections are not part of either.
	while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
		out.pop_back();
	if (out == "\n")
		out.clear();
	return found;
}

// ---- 2026-10-08 (AI-assisted): the game database's fixes that PCSX2's manual hardware fixes switch off ------------------

std::vector<std::pair<std::string, std::string>> ManualFixSettings(const std::string& serial)
{
	// GameDatabase.cpp's gsHWFixes names (isUserHackHWFix: every fix but deinterlace, mipmap, texture preloading, trilinear
	// filtering, the blending levels, the PCRTC ones and the CRC hacks) and the GS settings that set each by hand
	// (Pcsx2Config.cpp's keys); `flag`: an on/off setting (value > 0). The recommended-only ones (recommendedAccurateAlphaTest,
	// recommendedHWAA1) only show a message, so they're left out.
	static const struct
	{
		const char* fix;
		const char* key;
		bool flag;
	} kFixes[] = {
		{"autoFlush", "UserHacks_AutoFlushLevel", false},
		{"cpuFramebufferConversion", "UserHacks_CPU_FB_Conversion", true},
		{"readTCOnClose", "UserHacks_ReadTCOnClose", true},
		{"disableDepthSupport", "UserHacks_DisableDepthSupport", true},
		{"preloadFrameData", "preload_frame_with_gs_data", true},
		{"disablePartialInvalidation", "UserHacks_DisablePartialInvalidation", true},
		{"textureInsideRT", "UserHacks_TextureInsideRt", false},
		{"limit24BitDepth", "UserHacks_Limit24BitDepth", false},
		{"alignSprite", "UserHacks_align_sprite_X", true},
		{"mergeSprite", "UserHacks_merge_pp_sprite", true},
		{"accurateAlphaTest", "HWAccurateAlphaTest", true},
		{"forceEvenSpritePosition", "UserHacks_ForceEvenSpritePosition", true},
		{"bilinearUpscale", "UserHacks_BilinearHack", false},
		{"nativePaletteDraw", "UserHacks_NativePaletteDraw", true},
		{"estimateTextureRegion", "UserHacks_EstimateTextureRegion", true},
		{"drawBuffering", "UserHacks_DrawBuffering", true},
		{"rewriteLargeST", "UserHacks_RewriteLargeST", true},
		{"skipDrawStart", "UserHacks_SkipDraw_Start", false},
		{"skipDrawEnd", "UserHacks_SkipDraw_End", false},
		{"halfPixelOffset", "UserHacks_HalfPixelOffset", false},
		{"roundSprite", "UserHacks_round_sprite_offset", false},
		{"nativeScaling", "UserHacks_native_scaling", false},
		{"cpuSpriteRenderBW", "UserHacks_CPUSpriteRenderBW", false},
		{"cpuSpriteRenderLevel", "UserHacks_CPUSpriteRenderLevel", false},
		{"cpuCLUTRender", "UserHacks_CPUCLUTRender", false},
		{"gpuTargetCLUT", "UserHacks_GPUTargetCLUTMode", false},
		{"gpuPaletteConversion", "paltex", true},
	};
	std::vector<std::pair<std::string, std::string>> out;
	for (const auto& [fix, value] : GameDbHwFixes(serial))
		for (const auto& f : kFixes)
		{
			if (fix != f.fix)
				continue;
			// gpuPaletteConversion 2 means "on when texture preloading is Full", which depends on the other settings: left to
			// PCSX2 (it then counts as switched off by manual fixes, as on a PC).
			if (fix == "gpuPaletteConversion" && value > 1)
				break;
			std::string v = f.flag ? (value > 0 ? "true" : "false") : std::to_string(value);
			bool seen = false;
			for (auto& kv : out)
				if (kv.first == f.key)
				{
					kv.second = v; // a fix listed twice: the last one, as PCSX2 applies them in order
					seen = true;
				}
			if (!seen)
				out.emplace_back(f.key, std::move(v));
			break;
		}
	return out;
}

// ---- vk-285-114: the page's steps as functions, for the shelf's options sheet ---------------------------------------

bool EditSettingsFile(const std::string& path, const std::string& header_if_new, const std::vector<Change>& changes, std::string& what,
	std::string& error)
{
	what.clear();
	error.clear();
	std::string text;
	const bool existed = ReadFile(path, text);
	const IniState before = ReadState(text);
	std::vector<IniLine> lines = ParseIni(text);
	if (!existed && !header_if_new.empty())
	{
		IniLine header;
		header.raw = header_if_new;
		lines.push_back(header);
	}
	for (const Change& c : changes)
	{
		if (c.kind == Change::Set)
		{
			const std::string key = CanonKey(c.key), value = Trim(c.value);
			if (!SafeKey(key) || !SafeValue(value))
			{
				error = "bad setting: " + c.key;
				return false;
			}
			bool placed = false;
			for (size_t i = 0; i < lines.size();)
			{
				if (lines[i].kv && lines[i].key == key)
				{
					if (!placed)
					{
						lines[i].raw = key + "=" + value;
						lines[i].value = value;
						placed = true;
						i++;
					}
					else
						lines.erase(lines.begin() + static_cast<long>(i)); // one line a key
				}
				else
					i++;
			}
			if (!placed)
			{
				IniLine l;
				l.raw = key + "=" + value;
				l.key = key;
				l.value = value;
				l.kv = true;
				lines.push_back(l);
			}
		}
		else if (c.kind == Change::Unset)
		{
			const std::string key = CanonKey(c.key);
			if (!SafeKey(key))
			{
				error = "bad setting: " + c.key;
				return false;
			}
			lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const IniLine& l) { return l.kv && l.key == key; }), lines.end());
		}
		else
		{
			const std::string& group = c.key;
			if (group.empty() || group.size() > 100 || !SafeValue(group))
			{
				error = "bad patch name";
				return false;
			}
			// 2026-10-08: a cheat group the same way, on Cheats/Enable lines.
			const bool cheat = c.kind == Change::CheatOn || c.kind == Change::CheatOff;
			const std::string list_key = cheat ? "Cheats/Enable" : "Patches/Enable";
			const bool turn_on = c.kind == Change::PatchOn || c.kind == Change::CheatOn;
			bool present = false;
			for (const IniLine& l : lines)
				present = present || (l.kv && l.key == list_key && l.value == group);
			if (turn_on && !present)
			{
				IniLine l;
				l.raw = list_key + "=" + group;
				l.key = list_key;
				l.value = group;
				l.kv = true;
				lines.push_back(l);
			}
			else if (!turn_on)
				lines.erase(std::remove_if(lines.begin(), lines.end(),
								[&](const IniLine& l) { return l.kv && l.key == list_key && l.value == group; }),
					lines.end());
		}
	}
	std::string out;
	for (const IniLine& l : lines)
		out += l.raw + "\n";
	if (!WriteFileAtomic(path, out))
	{
		error = "could not write " + path + " (errno " + std::to_string(errno) + ")";
		return false;
	}
	what = DescribeChanges(before, ReadState(out));
	return true;
}

bool ApplyRecommended(const std::string& path, const std::string& presets, const std::string& id, const std::string& header, std::string& what,
	std::string& error)
{
	what.clear();
	error.clear();
	std::string preset;
	const bool has = PresetSection(presets, id, preset);
	if (id == "@global" && !has)
	{
		error = "this build has no recommended settings for all games";
		return false;
	}
	std::string text = header + (has ? ": PS5SX2's recommended settings, restored from the shelf" :
	                                   ": no settings of its own, so it follows the settings for all games (restored from the shelf)");
	text += "\n" + preset;
	std::string old;
	const bool existed = ReadFile(path, old);
	if (existed && !WriteFileAtomic(path + ".before-recommended", old))
		std::printf("[shelf] could not keep a copy of %s (errno %d)\n", path.c_str(), errno);
	// The player's own choices (PS5SX2/, MemoryCards/) stay, as the page keeps them.
	if (existed)
	{
		std::string keep;
		for (const auto& kv : ReadState(old).kv)
			if (IsPlayerKey(kv.first))
				keep += kv.first + "=" + kv.second + "\n";
		if (!keep.empty())
			text += (text.empty() || text.back() == '\n' ? "" : "\n") + keep;
	}
	if (!WriteFileAtomic(path, text))
	{
		error = "could not write " + path + " (errno " + std::to_string(errno) + ")";
		return false;
	}
	what = DescribeChanges(ReadState(old), ReadState(text));
	return true;
}

bool CreateCard(const std::string& dir, uint64_t mb, std::string name, std::string& made, std::string& error)
{
	made.clear();
	error.clear();
	if (dir.empty())
	{
		error = "no memory cards folder";
		return false;
	}
	if (mb != 8 && mb != 16 && mb != 32 && mb != 64)
	{
		error = "a card is 8, 16, 32 or 64 MB";
		return false;
	}
	name = Trim(name);
	if (name.size() < 4 || Lower(name.substr(name.size() - 4)) != ".ps2")
		name += ".ps2";
	else
		name = name.substr(0, name.size() - 4) + ".ps2"; // ".PS2" too, as the core lists lower case only
	if (!NewCardNameOk(name))
	{
		error = "use letters, digits, spaces and - _ . ( ) in the name";
		return false;
	}
	size_t count = 0;
	bool taken = false;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (dirent* e = readdir(d))
		{
			const std::string other = e->d_name;
			count++;
			taken = taken || Lower(other) == Lower(name); // a name that differs in case is confusing too
		}
		closedir(d);
	}
	else if (mkdir(dir.c_str(), 0777) != 0 && errno != EEXIST)
	{
		error = "could not make the memory cards folder";
		return false;
	}
	if (taken)
	{
		error = "a card called " + name + " is there already";
		return false;
	}
	if (count > 400)
	{
		error = "too many files in the memory cards folder";
		return false;
	}
	const std::string final_path = dir + "/" + name, tmp_path = dir + "/." + name + ".tmp";
	FILE* f = std::fopen(tmp_path.c_str(), "wb");
	if (!f)
	{
		error = "could not create the file (errno " + std::to_string(errno) + ")";
		return false;
	}
	const std::vector<unsigned char> block(static_cast<size_t>(kCardMb / 2), 0xFF); // 1024 * 528 bytes: a page run, 2 to the MB
	uint64_t left = mb * kCardMb;
	bool ok = true;
	while (ok && left > 0)
	{
		const size_t n = static_cast<size_t>(std::min<uint64_t>(left, block.size()));
		ok = std::fwrite(block.data(), 1, n, f) == n;
		left -= n;
	}
	ok = std::fflush(f) == 0 && ok;
	ok = std::fclose(f) == 0 && ok;
	if (ok && std::rename(tmp_path.c_str(), final_path.c_str()) != 0)
		ok = false;
	if (!ok)
	{
		unlink(tmp_path.c_str());
		error = "the card could not be written: is the disk full?";
		return false;
	}
	made = name;
	return true;
}
} // namespace fe::settings
