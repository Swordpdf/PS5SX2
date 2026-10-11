// PS5 port frontend: the shelf's options sheet (vk-285-114). The settings page's options (assets/web/index.html GROUPS,
// its memory cards and patches) on the console itself: Square on the shelf opens the selected game's sheet, like the
// PS3 app's. What it changes goes into the same files the page writes (settings/<image>.ini for one game, gs.ini for
// all of them), through the same code (fe_settings.cpp), and into logs/settings.log.
//
// Copyright (C) 2026 swordpdf
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
	// 2026-10-08: one of PCSX2's manual hardware fixes, used only while EmuCore/GS/UserHacks is on; changing it on a game's
	// sheet turns that on, starting from the game's own fixes (settings::ManualFixSettings).
	bool manual_fix = false;
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
	bool game_only = false; // 2026-10-08: on a game's sheet only, not the one for all games (the page's "game: true")
	bool global_only = false; // vk-285-137: on the sheet for all games only (the page's "global: true")
};

// 2026-10-08: the key that turns PCSX2's manual hardware fixes on (ManualUserHacks) for the rows with manual_fix.
constexpr const char* kManualFixesKey = "UserHacks";

const std::vector<OptionGroup>& OptionGroups();

struct OptionsPaths
{
	std::string settings_dir; // settings/<image>.ini
	std::string gs_ini;       // the settings every game starts from
	std::string patches_dir;  // <serial>_<crc>.pnach
	std::string cheats_dir;   // 2026-10-08: <serial>_<crc>.pnach of cheats ("" for none: no cheat rows)
	std::string memcards_dir; // memcards/ ("" for none: no card rows)
	std::string presets;      // assets/presets.ini's text (the Recommended row)
	// Where each change goes: the settings log (fe_ps5.cpp: AppendSettingsLog). May be empty.
	std::function<void(const std::string&)> log;
	// 2026-10-08: the shelf's disc images (file name, title), for an ELF's Disc image row.
	std::vector<std::pair<std::string, std::string>> disc_images;
};

// 2026-10-08: an ELF's disc image, by its file name as the shelf lists it (main-boot.cpp finds it in the game folders).
constexpr const char* kElfDiscKey = "PS5SX2/ElfDisc";
constexpr const char* kCheatDiscKey = "PS5SX2/CheatDisc"; // vk-285-139: gs.ini, a disc image the shelf lists

// vk-285-135 (AI-assisted; swordpdf: "i also want to pick a folder though the browser in the shelf"): the folders main-boot.cpp
// reads when PS5SX2 starts, in gs.ini, set from the sheet for all games (the app's folder picker) as from the settings page.
constexpr const char* kGameFoldersKey = "PS5SX2/GameFolders"; // folders separated by ';'
constexpr const char* kBiosFolderKey = "PS5SX2/BiosFolder";
constexpr const char* kNfsSharesKey = "PS5SX2/NfsShares"; // nfs:// addresses separated by ';'
// A list setting's items (';' or '|' between them, spaces and a folder's last '/' off), and the list again.
std::vector<std::string> SplitFolderList(const std::string& list);
std::string JoinFolderList(const std::vector<std::string>& items);

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
		TexturePack, // 2026-10-05: the game's HD texture pack from archive.org (the app draws and runs it: fe_texpacks.h)
		Cheat,       // 2026-10-08: one of the game's cheat groups (cheats/<serial>_<crc>.pnach)
		OnlinePatches, // 2026-10-08: "Get patches and cheats" (the app runs it: fe_patchdl.h)
		SystemMenu,  // 2026-10-08: the sheet for all games: start the PS2's own menu with no disc (the app starts it)
		ElfDisc,     // 2026-10-08: an ELF's sheet: the disc image it runs with (kElfDiscKey)
		GameFolders, // vk-285-135: the sheet for all games: more game folders (the app's folder picker)
		BiosFolder,  // vk-285-135: where the BIOS is looked for first (the folder picker)
		NfsShares,   // vk-285-135: NFS shares' addresses (the app's list, the PS5's keyboard)
		CheatDisc,   // vk-285-139: the sheet for all games: the cheat disc (kCheatDiscKey, one of the shelf's images)
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
	// 2026-10-05: a game's sheet gets the HD texture pack row (after Graphics) when the app has texture packs.
	void SetTexturePackRow(bool on) { m_texture_pack_row = on; }
	// 2026-10-08: a game's sheet gets the "Get patches and cheats" row (under Patches) when the app can fetch them.
	void SetOnlinePatchRow(bool on) { m_online_patch_row = on; }
	// 2026-10-08: the sheet for all games gets the "PS2 system menu" row when the app can start it. TakeSystemMenu: true
	// once, after the row was confirmed (Cross twice), for the app to start it.
	void SetSystemMenuRow(bool on) { m_system_menu_row = on; }
	bool TakeSystemMenu()
	{
		const bool asked = m_system_menu;
		m_system_menu = false;
		return asked;
	}
	// vk-285-135: the sheet for all games gets the Folders rows (game folders, BIOS folder, NFS shares). Cross on one asks
	// the app for its picker: TakeFolderRequest gives that row's kind once (else Kind::Header).
	void SetFolderRows(bool on) { m_folder_rows = on; }
	Kind TakeFolderRequest()
	{
		const Kind k = m_folder_request;
		m_folder_request = Kind::Header;
		return k;
	}
	// vk-285-135: the sheet's own file's value of a key ("" when it isn't set), and a key set (an empty value: unset) and
	// saved as the rows' changes are, `note` saying what for in the settings log.
	std::string OwnValue(const std::string& key) const;
	bool SetOwn(const std::string& key, const std::string& value, const std::string& note);
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
	bool CheatOn(const std::string& name) const; // 2026-10-08
	bool SetCheat(const std::string& name, bool on);
	// 2026-10-08: manual hardware fixes. On for this sheet's file? The value a fix row shows while they're off (the game's
	// own fix, else what the row follows), and the changes that turn them on, starting from the game's own fixes.
	bool ManualFixesOn() const;
	Effective FixValue(const OptionDef& d) const;
	std::vector<settings::Change> ManualFixesOnChanges(const std::string& except_key) const;
	const std::string* GameFix(const std::string& key) const;
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
	std::vector<settings::PatchGroup> m_cheats; // 2026-10-08: the cheats folder's groups for this game
	std::vector<std::pair<std::string, std::string>> m_game_fixes; // 2026-10-08: settings::ManualFixSettings for the game
	std::vector<settings::CardFile> m_cards;
	bool m_has_preset = false;
	std::string m_preset;
	std::vector<Row> m_rows;
	int m_new_card_mb = 8;
	int m_tab = kTabSettings;
	bool m_texture_pack_row = false;
	bool m_online_patch_row = false; // 2026-10-08
	bool m_system_menu_row = false, m_system_menu = false; // 2026-10-08
	bool m_folder_rows = false; // vk-285-135
	Kind m_folder_request = Kind::Header;
	bool m_elf = false; // 2026-10-08: this sheet's game is an ELF (the Disc image row)
	int m_armed_row = -1;
	double m_armed_until = 0;
	std::string m_status;
	int m_saved = 0;
};
} // namespace fe
