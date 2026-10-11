// PS5 port frontend: the settings files (vk-285-114). What the settings page (fe_web.cpp) and the shelf's options sheet
// (fe_options.cpp) share: the port's ini format (main-boot.cpp orbis_apply_ini_file reads it), the memory cards in
// memcards/, the games' patch groups and the recommended settings (assets/presets.ini). Moved here from fe_web.cpp
// unchanged; the editing, card and recommended helpers at the end are the page's own steps, for the sheet.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fe::settings
{
std::string Lower(std::string s);
std::string Trim(const std::string& s);

bool ReadFile(const std::string& path, std::string& out);
// Written beside, then renamed over: the GS thread polls these files and must never read half of one.
bool WriteFileAtomic(const std::string& path, const std::string& data);
bool Exists(const std::string& path);

// ---- settings files: "key=value" lines; a bare key is in EmuCore/GS, others carry their section
// ("EmuCore/Speedhacks/EECycleRate"). Patches/Enable lines repeat, one group name each.
struct IniLine
{
	std::string raw;
	std::string key; // canonical: EmuCore/GS/ dropped
	std::string value;
	bool kv = false;
};

std::string CanonKey(std::string key);
std::vector<IniLine> ParseIni(const std::string& text);
bool IsListKey(const std::string& key);
bool SafeKey(const std::string& key);
bool SafeValue(const std::string& v);

// ---- memory cards (vk-285-113): the cards PCSX2 offers from memcards/ (a file ending .ps2, .mcr, .mcd, .bin or .mc2
// that is at least a PS1 card's size); a new blank PS2 card is a file of 0xFF bytes, 1024 * 528 * 2 bytes a MB.
constexpr uint64_t kCardMb = 1024ull * 528 * 2;
constexpr uint64_t kPs1CardBytes = 1024ull * 8 * 16;

struct CardFile
{
	std::string name;
	uint64_t bytes = 0;
};

bool HasCardExtension(const std::string& name);
bool NewCardNameOk(const std::string& name);
std::vector<CardFile> ListCards(const std::string& dir);

// ---- the groups ("[60 FPS]") of a game's patch files, <serial>_<crc>.pnach.
struct PatchGroup
{
	std::string name, file, description;
};

std::vector<PatchGroup> PatchGroups(const std::string& dir, const std::string& serial);

// ---- recommended settings (vk-285-51). What a settings text sets, as the port's reader applies it: a key set twice
// counts with its last line; Patches/Enable lines add up.
struct IniState
{
	std::vector<std::pair<std::string, std::string>> kv;
	std::vector<std::string> enabled;
	std::vector<std::string> cheats; // 2026-10-08: Cheats/Enable lines (the sheet's Cheats rows)
};

IniState ReadState(const std::string& text);
// PS5SX2/ and MemoryCards/ keys are the player's own choices, not tuning.
bool IsPlayerKey(const std::string& key);
bool SameState(IniState a, IniState b);
// "key=value (was old)"-style notes for the settings log.
std::string DescribeChanges(const IniState& before, const IniState& after);
// One section of presets.ini ("[@global]", "[SLUS-20733]"). False when there is none.
bool PresetSection(const std::string& presets, const std::string& id, std::string& out);

// ---- 2026-10-08 (AI-assisted): PCSX2's manual hardware fixes (EmuCore/GS/UserHacks=true). While they are on, PCSX2
// leaves out the game database's hardware-renderer fixes for the game (GameDatabase.cpp applyGSHardwareFixes) unless the
// settings give the same value, as on a PC. ManualFixSettings: those fixes as the settings that set them by hand, e.g.
// ("UserHacks_native_scaling", "3") for Kingdom Hearts (SLUS-20370), from the database file (fe_games.h GameDbHwFixes).
// The sheet and the page write them when manual fixes are turned on for a game, so that turning it on changes nothing
// until a fix is changed. Fixes PCSX2 applies either way (mipmapping, blending levels, the CRC hacks) aren't in it.
std::vector<std::pair<std::string, std::string>> ManualFixSettings(const std::string& serial);

// ---- vk-285-114: the page's steps as functions, for the shelf's options sheet.

// One change: set `key` to `value`, or unset it (`value` empty with `unset` true); "patch+"/"patch-" for a patch group.
struct Change
{
	enum Kind
	{
		Set,
		Unset,
		PatchOn,
		PatchOff,
		CheatOn,  // 2026-10-08: a Cheats/Enable line for a cheat group
		CheatOff,
	} kind = Set;
	std::string key;   // the setting, or the patch (or cheat) group's name
	std::string value; // for Set
};

// Applies `changes` to the settings file at `path` the way the settings page's save does (one line a key, comments and
// other lines kept; a new file starts with `header_if_new` when it is not empty). `what` gets the settings log's note
// ("key=value (was old); ..." or ""). False (and `error`) when a key or value is not one the file can hold, or the file
// could not be written.
bool EditSettingsFile(const std::string& path, const std::string& header_if_new, const std::vector<Change>& changes, std::string& what,
	std::string& error);

// The Recommended button: the file becomes the `id` section of `presets` ("@global" for gs.ini; a game without a
// section gets a file with no values, so it follows gs.ini); the player's own keys stay; what was there is kept in
// <path>.before-recommended. False for "@global" without a section, or when the file could not be written.
bool ApplyRecommended(const std::string& path, const std::string& presets, const std::string& id, const std::string& header, std::string& what,
	std::string& error);

// A new blank PS2 card of `mb` (8, 16, 32 or 64) MB named `name` (".ps2" added when missing) in `dir`, made as the
// page's "Create card" makes it. `made` gets the file name. False (and `error`) when the name is not one the page
// accepts, the name is taken, or the file could not be written.
bool CreateCard(const std::string& dir, uint64_t mb, std::string name, std::string& made, std::string& error);
} // namespace fe::settings
