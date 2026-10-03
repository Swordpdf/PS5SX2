// PS5 port frontend: the shelf's options sheet (vk-285-114). The settings page's options (assets/web/index.html GROUPS,
// its memory cards and patches) on the console itself: Square on the shelf opens the selected game's sheet, like the
// PS3 app's. What it changes goes into the same files the page writes (settings/<image>.ini for one game, gs.ini for
// all of them), through the same code (fe_settings.cpp), and into logs/settings.log.
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_games.h"
#include "fe_settings.h"

#include <functional>
#include <string>
#include <vector>

namespace fe
{
struct OptionChoice
{
	std::string value, label;
};

// One option, as the settings page lists it (keep the two lists the same: index.html GROUPS).
struct OptionDef
{
	std::string key;   // the ini key ("upscale_multiplier" is in EmuCore/GS)
	std::string label;
	std::string def;   // PCSX2's own default, shown when neither the game nor gs.ini sets it
	std::string hint;
	std::string short_fmt; // the Recommended row's summary: "%" is the value's label ("Widescreen %" -> "Widescreen on")
	bool toggle = false;
	bool restart = false; // read when a game starts
	std::vector<OptionChoice> choices;
};

// vk-285-116: the sheet's tabs (L2 and R2), as the page's: the settings (with the memory cards and the patches) and the
// controls (the controller's buttons, sticks, rumble, save state buttons, the keyboard and mouse).
constexpr int kTabSettings = 0, kTabControls = 1, kTabAchievements = 2, kTabCount = 3;

struct OptionGroup
{
	std::string title;
	std::vector<OptionDef> items;
	int tab = kTabSettings;
};

const std::vector<OptionGroup>& OptionGroups();

struct OptionsPaths
{
	std::string settings_dir; // settings/<image>.ini
	std::string gs_ini;       // the settings every game starts from
	std::string patches_dir;  // <serial>_<crc>.pnach
	std::string memcards_dir; // memcards/ ("" for none: no card rows)
	std::string presets;      // assets/presets.ini's text (the Recommended row)
	// Where each change goes: the settings log (fe_ps5.cpp: AppendSettingsLog). May be empty.
	std::function<void(const std::string&)> log;
};

class OptionsSheet
{
public:
	enum class Kind
	{
		Header,      // a group's title
		Option,      // one of OptionGroups()
		Card,        // memory card slot 1 or 2
		NewCard,     // make a blank card
		Patch,       // one of the game's patch groups
		Recommended, // PS5SX2's recommended settings
		ResetAll,    // every option of the sheet back to what it follows
	};

	enum class From
	{
		Own,     // this file sets it
		Global,  // gs.ini sets it (a game's sheet)
		Default, // PCSX2's default
	};

	struct Row
	{
		Kind kind = Kind::Header;
		std::string label;
		const OptionDef* def = nullptr;
		int slot = 0;           // Card: 1 or 2
		std::string patch_desc; // Patch: its description
	};

	// `game` null: the sheet for all games (gs.ini). The tab stays as it was.
	void Open(const OptionsPaths& paths, const GameInfo* game);
	void Reload();
	// vk-285-116: the rows of one tab (kTabSettings or kTabControls).
	void SetTab(int tab);
	int tab() const { return m_tab; }

	bool is_global() const { return m_global; }
	const std::string& title() const { return m_title; }
	const std::string& file_label() const { return m_file_label; } // "settings/<image>.ini", "gs.ini"
	const std::vector<Row>& rows() const { return m_rows; }

	// The value the row shows ("4x", "On", "Mcd001.ps2", ...), where it comes from, and what the row does.
	std::string Value(const Row& r) const;
	From Source(const Row& r) const;
	std::string Help(const Row& r) const;
	bool Selectable(const Row& r) const { return r.kind != Kind::Header; }

	// Left/right (dir -1/+1) or Cross (+1): the next choice, saved at once. False when nothing changed.
	bool Step(const Row& r, int dir);
	// Triangle: the row follows all games (a game's sheet) or PCSX2's default (gs.ini) again.
	bool Reset(const Row& r);
	// Cross on an action row (Recommended, ResetAll: the first press arms, a second within 4 s does it; NewCard makes a card).
	bool Activate(const Row& r, double now);
	bool Armed(const Row& r, double now) const;

	// What the last action did ("Saved", "Made Card 1.ps2", an error), and how many changes this sheet saved.
	const std::string& status() const { return m_status; }
	int saved() const { return m_saved; }

private:
	struct Effective
	{
		std::string value;
		From from = From::Default;
	};

	Effective Get(const std::string& key, const std::string& def) const;
	bool Save(const std::vector<settings::Change>& changes, const std::string& note = {});
	void BuildRows();
	std::string CardValue(int slot, bool* own_out) const;
	int ChoiceIndex(const OptionDef& d, const std::string& v) const;

	OptionsPaths m_paths;
	bool m_global = true;
	std::string m_id;    // "@global" or the game's serial (the presets section)
	std::string m_title, m_file_label, m_path, m_header;
	std::string m_serial;
	settings::IniState m_own, m_globals;
	std::vector<settings::PatchGroup> m_patches;
	std::vector<settings::CardFile> m_cards;
	bool m_has_preset = false;
	std::string m_preset;
	std::vector<Row> m_rows;
	int m_new_card_mb = 8;
	int m_tab = kTabSettings;
	int m_armed_row = -1;
	double m_armed_until = 0;
	std::string m_status;
	int m_saved = 0;
};
} // namespace fe
