// PS5 port frontend: the cover-flow shelf itself: input, animation and the frame it draws.
// Platform code owns the Vulkan device and the display; it calls Update and Build once a frame.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_covers.h"
#include "fe_achievements.h"
#include "fe_game_achievements.h"
#include "fe_games.h"
#include "fe_options.h"
#include "fe_patchdl.h"
#include "fe_renderer.h"
#include "fe_sound.h"
#include "fe_texpacks.h"
#include "fe_text.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fe
{
struct Input
{
	bool left = false, right = false, cross = false, options = false, l1 = false, r1 = false;
	// vk-285-114: the options sheet's buttons.
	bool up = false, down = false, square = false, triangle = false, circle = false;
	bool l2 = false, r2 = false; // vk-285-116: the sheet's tabs
};

struct AppConfig
{
	std::string build_tag;   // shown small in the corner
	int preselect = 0;       // the game selected at start
	SoundSink* sound = nullptr; // told about steps and the launch (may be null)
	int test_build = 0;          // test build 1 (vk-285-55): > 0 draws TESTING and build_label mid-screen
	std::string build_label;     // "Test build 1 · vk-285-55"
	std::string test_note;       // vk-285-105: a smaller line under build_label (the testers' Discord)
	// vk-285-114: the options sheet (Square): where the settings files are, and a way to read a game's badges again
	// after its settings changed (fe_games.cpp ReadBadges; null: the badges stay as they were).
	OptionsPaths options;
	std::function<void(GameInfo&)> refresh_game;
	AchievementAccountService achievements;
	GameAchievementsService game_achievements;
	TextEntryService text_entry; // 2026-10-05: the PS5's own keyboard for the account panel (fe_ps5.cpp)
	TexturePackService texture_packs; // 2026-10-05: HD texture packs from archive.org (fe_texpacks.h); unset: no row
	OnlinePatchService online_patches; // 2026-10-08: a game's patches and cheats from GitHub (fe_patchdl.h); unset: no row
	bool system_menu = false; // 2026-10-08: the sheet for all games offers the PS2 system menu (App::SystemMenuChosen)
	// vk-285-134 (AI-assisted): the PS2 BIOS. bios_present looks again and says whether there is one (null: never asked);
	// while there isn't, the shelf says so (bios_problem: what was found instead) and a game picked stays on the shelf.
	std::function<bool()> bios_present;
	std::function<std::string()> bios_problem;
	std::string bios_dir;
	// vk-285-135 (AI-assisted): the sheet for all games' Folders rows and their picker start from these places (label, path),
	// those that are folders when it opens (the drives come and go); empty: no Folders rows.
	std::vector<std::pair<std::string, std::string>> folder_places;
	// A disc drive's entry on the shelf (fe_games.h IsDrivePath): asked once a frame for each such game, it says
	// whether the disc in the drive changed since it last answered, with `g` described again and its pictures (spine,
	// placeholder, and the cover when one is on disk) in `images`. Null: the entries stay as Init got them.
	std::function<bool(GameInfo& g, std::vector<CoverImage>& images)> drive_changed;
};

class App
{
public:
	bool Init(Renderer* renderer, const Fonts* fonts, std::vector<GameInfo> games, CoverService* covers, const AppConfig& cfg);
	void Shutdown();

	// Advances `dt` seconds with the pad's current buttons.
	void Update(double dt, const Input& in);

	// Fills `f` for this moment; `clock` is the time of day to show ("" for none).
	void Build(FrameDesc& f, const std::string& clock);

	// The settings page's address for the QR tile (vk-285-50): "http://<ip>:<port>/" (vk-285-118: no key), or
	// empty when there is no network (the tile then says so). Call it again when the address changes.
	void SetWebUrl(const std::string& url, const std::string& shown);

	// True once a game was picked and its launch animation has played.
	bool Done() const { return m_done; }
	// vk-285-137: an index in the list Init got (games hidden from the shelf are still in it).
	int Chosen() const { return m_index.empty() ? m_selected : m_index[static_cast<size_t>(m_selected)]; }
	// 2026-10-08: true when Done() came from the sheet's "PS2 system menu", not a game.
	bool SystemMenuChosen() const { return m_system_menu; }

	const std::vector<GameInfo>& games() const { return m_games; }

	// pr9n: what the host tests check (fe_host's "expect" step).
	bool AccountOpen() const { return m_account.open; }
	bool SheetOpen() const { return m_sheet_open; }
	int SheetTab() const { return m_sheet.tab(); }
	bool PickerOpen() const { return m_picker.open; } // vk-285-135
	int ShelfCount() const { return static_cast<int>(m_games.size()); } // vk-285-137: the games on the shelf

private:
	struct Slot
	{
		Texture* cover = nullptr;
		Texture* placeholder = nullptr;
		Texture* spine = nullptr;
		VkDescriptorSet set = VK_NULL_HANDLE;
		bool dirty = false;
		float cover_mix = 0;     // animates to 1 once the cover arrives
		bool has_cover = false;
		float glow[3] = {0.55f, 0.42f, 1.0f};
		bool has_glow = false;
	};

	bool Step(int dir); // true when the selection moved
	// vk-285-137 (AI-assisted; swordpdf: "add an option to hide games from the shelf"): m_games is the shelf; a game whose
	// settings hide it (GameInfo::hidden) waits in m_shelved, with its slot (its covers keep coming), unless Show hidden
	// games (gs.ini) is on. m_index[i] is m_games[i]'s index in the list Init got, which the cover service and the caller use.
	// ApplyHidden sorts the games back into the two, keeps `keep` (such an index) selected or else the next game on the
	// shelf, and says whether the shelf changed.
	bool ApplyHidden(int keep);
	int PositionOf(int index) const; // m_games position of an index from Init's list; -1 when shelved
	void Sound(Sfx sfx, float pan);

	// vk-285-114: the options sheet.
	void OpenSheet(bool global);
	void SheetTab(int tab); // vk-285-116
	void CloseSheet();
	void UpdateSheet(double dt, const Input& in);
	bool SheetMove(int dir); // false at either end
	void BuildSheet(std::vector<UiVertex>& ui, float W, float H, float k, uint32_t accent);
	void RefreshBadges();
	// 2026-10-05: the sheet's HD texture pack row (fe_texpacks.h): its buttons, what it shows, its help line, and the
	// shelf's line while a pack is on its way.
	void UpdateTexturePackRow(const Input& in, double now);
	std::string OnlinePatchValue() const; // 2026-10-08: the "Get patches and cheats" row's value
	std::string TexturePackValue(const TexturePackStatus& s, int pick) const;
	std::string TexturePackHelp(const TexturePackStatus& s, int pick, const std::string& serial) const;
	void BuildTexturePackActivity(std::vector<UiVertex>& ui, float x, float y, float k, uint32_t accent);
	void PollCovers();
	// A picture for a slot (PollCovers' images and the disc drives'), and the disc drives' changes (drive_changed).
	void ApplyImage(Slot& s, const GameInfo& g, CoverImage& img);
	void PollDrives();
	int m_disc_selected = -1, m_before_disc = -1; // the disc drive selected when its disc went in, and the game before it
	// vk-285-135 (AI-assisted; swordpdf: "i also want to pick a folder though the browser in the shelf"): the folder picker of the
	// sheet's Folders rows (game folders, the BIOS folder) and the NFS share list, in the sheet's place while it is open.
	struct PickerItem
	{
		enum class Type
		{
			Use,    // "Use this folder" (the folder shown)
			Folder, // a folder in it: Cross goes in
			Place,  // a drive, /data/PCSX2, the NFS shares: Cross goes in
			Listed, // a game folder added already (Triangle removes it)
			Add,    // "Add a share" (the PS5's keyboard)
			Share,  // an NFS share's address (Triangle removes it)
		};
		Type type = Type::Folder;
		std::string label, path, value;
	};
	struct FolderPicker
	{
		bool open = false;
		OptionsSheet::Kind kind = OptionsSheet::Kind::Header;
		std::string dir; // "" the places (the share list for NFS)
		std::vector<std::string> trail; // the folders gone into, to come back to their rows
		std::vector<PickerItem> items;
		int row = 0;
		float scroll = 0, scroll_target = 0;
		bool typing = false; // the PS5's keyboard is open for a share's address
		std::string editing;  // vk-285-138: the share being changed (empty: a new one)
		// vk-285-139: the panel's own keyboard for the address, where the PS5's won't open (libSceImeDialog doesn't load in
		// every jailbreak's process: swordpdf's 138 log, "load 0x80020063").
		bool osk = false;
		std::string osk_text;
		int osk_page = 0, osk_row = 1, osk_col = 0;
		int held = 0;
		double held_for = 0, next_repeat = 0;
	};
	void OpenPicker(OptionsSheet::Kind kind);
	void PickerList(const std::string& dir, const std::string& focus = {});
	void UpdatePicker(double dt, const Input& in);
	void BuildPicker(std::vector<UiVertex>& ui, float x, float sw, float y, float sh, float k, uint32_t accent);
	FolderPicker m_picker;
	void UpdateAccount(const Input& in);
	void BuildAccount(std::vector<UiVertex>& ui, float W, float H, float k, uint32_t accent);
	void BuildAccountKeyboard(std::vector<UiVertex>& ui, float x, float y, float w, float k);
	// vk-285-139: the panel's keyboard for any field: its title, page and focused key.
	void BuildPanelKeyboard(std::vector<UiVertex>& ui, float x, float y, float w, float k, const std::string& title, int page, int key_row,
		int key_col);
	void BuildPickerKeyboard(std::vector<UiVertex>& ui, float W, float H, float k, uint32_t accent);
	void FinishShareText(int result, std::string text);
	void Pose(float d, float t, Mat4& model, float& brightness) const;

	Renderer* m_renderer = nullptr;
	const Fonts* m_fonts = nullptr;
	CoverService* m_covers = nullptr;
	AppConfig m_cfg;
	std::vector<GameInfo> m_games;
	std::vector<Slot> m_slots;
	// vk-285-137: see ApplyHidden.
	struct Shelved
	{
		GameInfo game;
		Slot slot;
		int index = -1;
	};
	std::vector<Shelved> m_shelved;
	std::vector<int> m_index;
	bool m_show_hidden = false;
	// vk-285-137: a line above the title for a few seconds (a game hidden, hidden games shown or not).
	std::string m_note_text;
	double m_note_time = -10;

	int m_selected = 0;
	bool m_system_menu = false; // 2026-10-08: the PS2 system menu was started from the sheet
	float m_scroll = 0, m_scroll_vel = 0; // the shelf's position (a game index), sprung to m_selected
	double m_time = 0;
	double m_select_time = -10;            // when the selection last changed (for the sheen)
	float m_glow[3] = {0.55f, 0.42f, 1.0f};

	// Held-direction repeat.
	int m_held = 0;
	double m_held_for = 0, m_next_repeat = 0;
	Input m_prev;
	bool m_released = false; // buttons held at start count only once released
	// pr9n (AI-assisted), PR #9 review item 11: L1 + Square in either order. L1 first jumps 5 games at once (undone when
	// Square follows while L1 is held, within kChordWindow); Square first opens the sheet (closed again when L1 follows).
	static constexpr double kChordWindow = 0.5; // seconds between the chord's two presses
	int m_chord_from = -1; // the selection before L1's jump
	double m_chord_l1_time = -10, m_chord_square_time = -10;

	// The QR tile (vk-285-50): the code's modules, row by row, 1 = dark.
	std::string m_web_url, m_web_shown;
	std::vector<uint8_t> m_qr;
	int m_qr_size = 0;
	// vk-285-118 (AI-assisted): Triangle shows the QR code large in the middle (a tester's phone couldn't read the small tile).
	bool m_qr_big = false;
	float m_qr_big_anim = 0.0f;
	bool m_web_known = false; // SetWebUrl was called (the host preview may never call it)

	// vk-285-114: the options sheet: open or not, its slide (0 closed .. 1 open), the focused row, the list's scroll (in
	// 2160-line pixels), the held-button repeats and the status line's time.
	OptionsSheet m_sheet;
	bool m_sheet_open = false;
	bool m_sheet_global = false;
	float m_sheet_anim = 0;
	int m_sheet_row = 0;
	float m_sheet_scroll = 0, m_sheet_scroll_target = 0;
	int m_sheet_held_v = 0, m_sheet_held_h = 0;
	double m_sheet_held_for = 0, m_sheet_next_repeat = 0;
	std::string m_sheet_status;
	double m_sheet_status_time = -10;
	int m_sheet_saved_at_open = 0;
	// 2026-10-05: the texture pack picked on each game's row (left and right), Triangle's first press (cancel, delete),
	// and the state seen last (a pack that comes in reloads the sheet: its game's file has Texture replacements on then).
	std::map<std::string, int> m_texpack_pick;
	std::string m_texpack_armed;
	double m_texpack_armed_until = -1;
	std::string m_texpack_seen_serial;
	TexturePackStatus::State m_texpack_seen = TexturePackStatus::State::Loading;
	// 2026-10-08: the online patches' state last seen for the sheet's game (a finished fetch reloads the sheet's rows).
	std::string m_online_seen_serial;
	OnlinePatchStatus::State m_online_seen = OnlinePatchStatus::State::Idle;

	// vk-285-134: no PS2 BIOS (AppConfig::bios_present said so at start or at the last pick), what was found instead,
	// and when a pick was refused for it (the line lights up).
	bool m_bios_missing = false;
	std::string m_bios_problem;
	double m_bios_refused = -10;
	bool BiosReady(); // looks again; false (and the line lit) when there's still none
	void BuildBiosLine(std::vector<UiVertex>& ui, float W, float k);
	// vk-285-134: a pick refused for its image (GameInfo::damaged): the line under the title says why, for a few seconds.
	std::string m_refused_text;
	double m_refused_time = -10;

	bool m_launching = false;
	double m_launch_time = 0;
	bool m_done = false;
	Texture* m_atlas = nullptr;
	AchievementAccountPanel m_account;
	// 2026-10-05: the account panel's fade (0 closed .. 1 open), its keyboard's slide, and the field the PS5's keyboard
	// is typing into (-1: none open).
	float m_account_anim = 0, m_account_kb_anim = 0;
	int m_ime_field = -1;
	GameAchievementsState m_game_achievements;
	struct Badge
	{
		Texture* texture = nullptr;
		VkDescriptorSet set = VK_NULL_HANDLE;
		bool attempted = false;
	};
	std::vector<Badge> m_achievement_badges;
	std::vector<UiImageRange> m_achievement_images;
	int m_achievement_row = 0;
	void PollGameAchievements();
	void ClearAchievementBadges();
	void LoadGameAchievements();
	void BuildGameAchievements(std::vector<UiVertex>& ui, float x, float y, float width, float height, float k, uint32_t accent);
};
} // namespace fe
