// PS5 port frontend: the cover-flow shelf itself: input, animation and the frame it draws.
// Platform code owns the Vulkan device and the display; it calls Update and Build once a frame.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_covers.h"
#include "fe_achievements.h"
#include "fe_game_achievements.h"
#include "fe_gametile.h"
#include "fe_games.h"
#include "fe_options.h"
#include "fe_patchdl.h"
#include "fe_renderer.h"
#include "fe_sound.h"
#include "fe_texpacks.h"
#include "fe_text.h"
#include "fe_update.h"

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
	// 2.11: the right stick (-1..1, right and down positive; 0 for a pad without one, and in the host's scripts unless they move it) and
	// its click: on the shelf the stick turns the picked case over to show its back, the click holds it there.
	float rx = 0, ry = 0;
	bool r3 = false;
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
	// gamedb (2026-10-30, AI-assisted): resources/GameIndex.yaml is missing or unusable (main-boot.cpp, OrbisGameDb.h): the shelf names
	// the file under the BIOS line (games still get their fixes from the eboot's copy). Empty: nothing to say.
	std::string gamedb_missing;
	// bioscheck (2026-10-29, AI-assisted): a pick refused for want of a BIOS asks what to do (the BIOS question): the open-source BIOS this
	// build ships (open_bios_bundled; picking it writes gs.ini PS5SX2/OpenBios=true, then bios_present looks again and the pick starts), or
	// the player's own in bios_dir. bios_open_in_use: games start with the open BIOS now (the shelf names it). bios_pending_launch: a start
	// from outside the shelf (a game or disc tile, the disc helper, bootdisc) refused for want of a BIOS before the shelf came up: the
	// question is asked for it at once, and picking the open BIOS starts it (App::TakeApprovedLaunch).
	bool open_bios_bundled = false;
	std::function<bool()> bios_open_in_use;
	std::string bios_pending_launch;
	// 2026-10-16: the games folder the player set (the first of them) while only those are listed; "" = the usual places. The empty
	// shelf names it where it says /data/PCSX2/games (a typo or an unplugged drive then explains itself).
	std::string games_folder_note;
	// chdscan (AI-assisted): true while the images not seen before are being read (`done` of `total`): the shelf came up without its games
	// (App::SetGames brings them) and says so where it would say there are none. Unset: no scan.
	std::function<bool(int& done, int& total)> scan_progress;
	// vk-285-135 (AI-assisted): the sheet for all games' Folders rows and their picker start from these places (label, path),
	// those that are folders when it opens (the drives come and go); empty: no Folders rows.
	std::vector<std::pair<std::string, std::string>> folder_places;
	// live-22 (AI-assisted): the disc dumper's progress file (orbis-shims/ProsperoDiscDump.cpp, logs/disc-dump-progress.txt).
	// While a PS2 disc is being copied, the shelf polls it and shows a spinning-disc entry with the live percentage, until
	// the copy ends (the file is removed, and the game launches). Empty: no such entry (the host sets it for its test).
	std::string disc_dump_progress;
	// 2.04: a PS2 disc put in the drive that is not copied yet: the shelf asks what to do with it (play it straight from the disc, or
	// copy it first). disc_ask_poll gives the pending question (false when there is none); disc_ask_answer takes the answer:
	// 1 copy it, 2 play from the disc, 3 not now. Unset: no question is shown.
	std::function<bool(std::string& title, std::string& serial)> disc_ask_poll;
	std::function<void(int)> disc_ask_answer;
	// 2.11: the PS2 disc in the drive that is not copied (orbis-shims/ProsperoDiscDump.cpp): the shelf lists it first. drive_disc_poll gives its title
	// and serial (false when none); drive_disc_reask asks the watcher about it again (the question comes back as disc_ask_poll's).
	std::function<bool(std::string& title, std::string& serial)> drive_disc_poll;
	std::function<void()> drive_disc_reask;
	// gamedb (2026-10-30, AI-assisted; #bugs: a disc game ignored its own settings): the settings file stem of disc `serial` (fe_discsettings.h, the
	// file a start from the drive reads), for the entries of the disc in the drive and of a disc being copied: their sheet is the disc's own.
	// Unset or "": those entries open only the all-games sheet.
	std::function<std::string(const std::string& serial)> disc_settings_stem;
	// fix180 (tester on 180: "started a game from disc brings you back ... into the shelf to the first game entry"): the last game played was the disc in
	// the drive (lastgame.txt "disc:<serial>"): when the drive's entry for that serial comes (the watcher reports it a few seconds after the start), it is
	// picked, unless the player has moved the shelf by then.
	std::string preselect_disc_serial;
	// 2.11: the shelf's search, from the settings page on a phone or PC. search_remote gives the text the page set since the last
	// call (true; "" clears the search); search_report tells the page what the shelf is searching for (it shows it in its box).
	std::function<bool(std::string& text)> search_remote;
	std::function<void(const std::string& text)> search_report;
	// 2.12 (swordpdf: "add a game from the shelf to the xmb"): a game's sheet gets the "Home screen tile" row, which asks the PS5SX2 Disc Helper to put the
	// game on the PS5 home screen as a tile of its own (or to take it off; fe_gametile.h). tiles_dir is the folder the two talk through (/data/PCSX2/tiles); empty:
	// no row (the flag file flags/gametiles turns it on: fe_ps5.cpp). own_tile_game: the game this process was started from a tile for ("" outside a tile): its tile
	// is not taken off from inside it. tile_cover: the shelf's cover file for a game (an absolute path, "" for none), sent with the request for the tile's art.
	std::string tiles_dir;
	std::string own_tile_game;
	std::function<std::string(const GameInfo&)> tile_cover;
	// 180 (AI-assisted): PS5SX2 updating itself and the session-log switch: the sheet for all games' PS5SX2 group (fe_update.h). Unset: no group.
	UpdateService updates;
};

class App
{
public:
	bool Init(Renderer* renderer, const Fonts* fonts, std::vector<GameInfo> games, CoverService* covers, const AppConfig& cfg);
	void Shutdown();

	// chdscan (AI-assisted): the games, once the scan that Init's empty shelf waited for is done (the cover service was started with the
	// same list). `preselect` is picked unless the player has moved on the shelf meanwhile (a disc drive's entry may be there).
	void SetGames(std::vector<GameInfo> games, int preselect);

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
	// bioscheck: a start asked of the shelf from outside it (Play from disc, a finished copy): true when it may go (there is a BIOS);
	// false when there is none: the BIOS question comes up for it (once a path), and the request is not taken.
	bool OfferOutsideLaunch(const std::string& path);
	// bioscheck: a start from outside the shelf that waited on the BIOS question and may go now (the open BIOS was picked); "" for none.
	std::string TakeApprovedLaunch();
	bool BiosMissing() const { return m_bios_missing; }
	bool GameDbNoteShown() const { return m_gamedb_note_shown; } // gamedb (2026-10-30): the note was drawn on the last frame
	bool BiosChoiceOpen() const { return m_bios_ask; }
	int BiosChoiceRow() const { return m_bios_ask ? m_bios_sel : -1; } // 0 the open BIOS, 1 your own (0 is your own without the open BIOS)

	const std::vector<GameInfo>& games() const { return m_games; }

	// pr9n: what the host tests check (fe_host's "expect" step).
	bool AccountOpen() const { return m_account.open; }
	bool SheetOpen() const { return m_sheet_open; }
	int SheetTab() const { return m_sheet.tab(); }
	const std::string& SheetFileLabel() const { return m_sheet.file_label(); } // gamedb (2026-10-30): "settings/<stem>.ini" or "gs.ini"
	bool PickerOpen() const { return m_picker.open; } // vk-285-135
	int ShelfCount() const { return static_cast<int>(m_games.size()); } // vk-285-137: the games on the shelf
	// live-22: the disc dump entry's percentage (0-100) while one is on the shelf, else -1 (the host test checks it).
	int DumpPercent() const { return m_dump_on ? m_dump_pct : -1; }
	// 2.11: the disc in the drive is listed (1) or not (0); the disc art for the copy's or the drive's entry has arrived (1) or not (0). For the host tests.
	int DriveEntry() const { return m_drive_on ? 1 : 0; }
	int DiscArt() const { return m_dump_disc_set ? 1 : 0; }
	// 2.11: the shelf's search (the host tests check them): what it is searching for, whether its keyboard is up, and how far the
	// picked case is turned over (0 front .. 1 back).
	const std::string& SearchText() const { return m_search.text; }
	bool SearchTyping() const { return m_search.typing; }
	float FlipAmount() const { return m_flip < 0 ? -m_flip : m_flip; }
	// 2.11: the same with the side it is turned towards (-1 .. 1; the pose repeats every 2), and whether the picked case's real back cover has arrived (1) or not (0).
	float FlipAngle() const { return m_flip * m_flip_dir; }
	int BackReal() const { return !m_games.empty() && m_slots[static_cast<size_t>(m_selected)].back_real ? 1 : 0; }
	// 2.11 (the host tests): whether the case of a game (an index of Init's list) holds a real back cover now (1/0, -1 when it is not on the shelf), how far
	// the flip turns its pose at this moment (degrees, 0 when it is not on the shelf), and how many cases that are not the picked one still hold their back
	// because they are turned over on screen; and whether the case has its texture set to be drawn with (1/0, -1 when it is not on the shelf).
	int BackRealOf(int game) const;
	int TurnOf(int game) const;
	int SetOf(int game) const;
	int BackHeld() const { return static_cast<int>(m_back_held.size()); }
	int BackRealCount() const; // every slot, the ones off the shelf too, that holds a real back cover (the leak check of the hold)
	// 2.11: the last list of neighbours whose backs were handed to the service for fetching ahead: how many, and the first (an index of Init's list; -1 none).
	int RingCount() const { return m_ring_count; }
	int RingFirst() const { return m_ring_first; }
	// 2.12 (the host tests check them): the Home screen tile row's phase (fe_gametile.h Phase; -1 when the sheet shows no such row), whether the row is in the
	// sheet (1) or not (0), whether it is the focused one, how many times the state was looked at (ResetTilePolls zeroes it) and how many asks are pending.
	int TilePhase() const { return m_sheet_open && m_tile_row ? static_cast<int>(m_tile_view.phase) : -1; }
	int TileRowShown() const { return m_sheet_open && m_tile_row ? 1 : 0; }
	int TileFocused() const;
	int TilePolls() const { return m_tile_polls; }
	void ResetTilePolls() { m_tile_polls = 0; }
	int TilePending() const { return m_tile.PendingCount(); }

private:
	struct Slot
	{
		Texture* cover = nullptr;
		Texture* placeholder = nullptr;
		Texture* spine = nullptr;
		Texture* back_ph = nullptr;   // 2.11: the case's back, painted (the cover service)
		Texture* back_real = nullptr; // 2.11: a back cover the player supplied, held while the case is the picked one (and while it is still turned over after it)
		bool back_wanted = false;     // 2.11: its back cover was asked of the service
		bool back_net = false;        // 2.11: ... with the permission to download it
		double back_asked = -1;       // 2.11: when (App::m_time), for the log
		double back_shown = -1;       // 2.11: when (App::m_time) it was last drawn turned over, after the selection left it (m_back_held)
		double back_left = -1;        // 2.11: when (App::m_time) the selection left it with its back held (m_back_held), for the log
		VkDescriptorSet set = VK_NULL_HANDLE;
		bool dirty = false;
		float cover_mix = 0;     // animates to 1 once the cover arrives
		bool has_cover = false;
		float glow[3] = {0.55f, 0.42f, 1.0f};
		bool has_glow = false;
		bool wanted = false; // 2026-10-10: asked of the cover service (near the selection); textures only while it is
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
	// 180: the PS5SX2 group's rows (fe_update.h): a press on either, what they show and say, and the close after the installer started.
	void UpdateRowPress(const OptionsSheet::Row& row, double now);
	std::string UpdateRowValue(const OptionsSheet::Row& row) const;
	std::string UpdateRowHelp(const OptionsSheet::Row& row) const;
	void UpdateQuitTick();
	// 2.12: the sheet's Home screen tile row (fe_gametile.h): whether the shown game can have one, the state looked at (throttled; only with the row shown or an ask
	// pending), a press (Cross, Left or Right add; Triangle or Cross remove), and what the row shows and says. HomeTileValue and HomeTileHelp read what PollTiles
	// found: no file access per frame.
	bool HomeTileEligible(const GameInfo* g) const;
	void PollTiles(bool force);
	bool DiskBusy() const;
	void HomeTileToggle(int how); // 0 Cross, 1 Left or Right, 2 Triangle
	void HomeTileSay(const std::string& text);
	std::string HomeTileValue() const;
	std::string HomeTileHelp() const;
	std::string TexturePackValue(const TexturePackStatus& s, int pick) const;
	std::string TexturePackHelp(const TexturePackStatus& s, int pick, const std::string& serial) const;
	void BuildTexturePackActivity(std::vector<UiVertex>& ui, float x, float y, float k, uint32_t accent);
	void PollCovers();
	// live-22 (AI-assisted): a disc being copied shows as a spinning-disc entry at the front of the shelf with its live
	// percentage (AppConfig::disc_dump_progress). PollDiscDump reads the dumper's status file (throttled), inserts the
	// synthetic entry when a copy starts, keeps its percent up to date, and drops it when the copy ends. The entry is a
	// normal shelf entry whose Init-list index is kDumpIndex (< 0), so the cover service never touches it; its box carries
	// a spinning disc and the percentage drawn over it.
	void PollDiscDump();
	void InsertDumpEntry(const std::string& serial, const std::string& title, int pct);
	void RemoveDumpEntry();
	int DumpPos() const; // the dump entry's position on the shelf, or -1
	bool m_dump_on = false;
	std::string m_dump_serial, m_dump_title;
	int m_dump_pct = 0;
	// 179: the live copy speed shown on the dump entry: smoothed MB/s, seconds left (0 = unknown), and the drive speed taken ("8x"/"3.2x"/"").
	double m_dump_mbps = 0.0;
	int m_dump_eta = 0;
	std::string m_dump_speed;
	float m_dump_spin = 0;      // the disc's rotation, advanced each frame
	double m_dump_poll = -10;   // when the status file was last read
	// 2.11 (swordpdf: "we can also use the circular disc cover for when a game is dumping"): the disc being copied shows its own disc art (LaunchBox, or
	// covers/<serial>_disc.png) instead of the painted disc, once the cover service has it.
	void TakeDiscArt(const CoverImage& img);
	void EnsureDiscArt(const std::string& serial); // asks for the art of this disc unless it has it (or asked) already
	// 2.11 (swordpdf: "since the emulator knows what game is inside the drive, it would be cool to have the disc in the shelf as well, maybe always on
	// first place"): the PS2 disc in the drive that is not copied is a shelf entry at the front (Init-list index kDriveIndex), with its disc art on the
	// box. Picking it asks the disc watcher about the disc again (play from it, or copy it), which is how the question comes back after "not now".
	void PollDriveDisc();
	// 2.11 (review): a shelf entry that moves the selection (the copy's) or the list (the drive's) waits while a pick is launching or a panel holds the pad.
	bool ShelfBusy() const { return m_launching || m_sheet_open || m_sheet_anim > 0.05f || m_account.open || m_picker.open || m_search.typing; }
	void InsertDriveEntry(const std::string& serial, const std::string& title);
	void RemoveDriveEntry();
	int DrivePos() const; // the entry's position on the shelf, or -1
	bool m_drive_on = false;
	std::string m_drive_serial, m_drive_title;
	std::string m_disc_art_serial; // the disc the art (m_dump_disc) is for, or was asked for
	double m_art_idle = -1;        // since when no entry wants the art (it is let go after a moment: the drive's entry and the copy's hand it over)
	void FreeDiscArt();
	Texture* m_dump_disc = nullptr;
	VkDescriptorSet m_dump_disc_set = VK_NULL_HANDLE;
	float m_dump_disc_aspect = 1.0f; // width / height of the picture
	void KeepCoversNear();                // 2026-10-10: asks for the games near the selection, drops the rest's textures
	void DropCovers(Slot& s, int index); // its textures and set freed, the service told
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
			TypePath, // 2.11: "Type a folder path" (the keyboard): any folder, however deep, without browsing to it
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
		bool path_entry = false; // 2.11: the keyboard (either) is for a folder's path, not a share's address
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
	// 2.04: the question about a disc just put in (AppConfig::disc_ask_poll), a panel over the shelf.
	void PollDiscAsk();
	void UpdateDiscAsk(const Input& in);
	void BuildDiscAsk(std::vector<UiVertex>& ui, float W, float H, float k, uint32_t accent);
	bool m_ask_on = false;
	std::string m_ask_title, m_ask_serial;
	int m_ask_sel = 0;     // 0 play from the disc, 1 copy it first
	float m_ask_anim = 0;  // the panel's fade
	void BuildAccount(std::vector<UiVertex>& ui, float W, float H, float k, uint32_t accent);
	void BuildAccountKeyboard(std::vector<UiVertex>& ui, float x, float y, float w, float k);
	// vk-285-139: the panel's keyboard for any field: its title, page and focused key.
	void BuildPanelKeyboard(std::vector<UiVertex>& ui, float x, float y, float w, float k, const std::string& title, int page, int key_row,
		int key_col);
	void BuildPickerKeyboard(std::vector<UiVertex>& ui, float W, float H, float k, uint32_t accent);
	void FinishShareText(int result, std::string text);
	// 2.11 (swordpdf: "add a search section to the shelf, make it sleek"): R2 on the shelf opens a keyboard (the PS5's when the app can
	// open it, else the panel's own, docked at the bottom) and the shelf narrows to the games that match as each key is typed: every
	// word of the search must be in the title, serial, region or file name (any letter case). Circle clears it. The settings page's
	// search box does the same from a phone (AppConfig::search_remote).
	struct Search
	{
		bool typing = false;  // the keyboard is up
		bool system = false;  // ... and it is the PS5's (it has the pad until it closes)
		std::string text;     // as typed
		std::string applied;  // lower case: what the shelf is narrowed by ("" for none)
		int page = 0, row = 1, col = 0;
		int held_x = 0, held_y = 0;
		double held_for = 0, next_repeat = 0;
		float kb_anim = 0;    // the keyboard's slide
		float pill_anim = 0;  // the search line's fade
	};
	Search m_search;
	int m_search_return = -1; // the game picked when a search first narrowed the shelf (an index of Init's list)
	bool SearchMatches(const GameInfo& g) const;
	void SetSearch(const std::string& text); // the shelf narrowed to it ("" shows every game again)
	void OpenSearch();
	void CloseSearch(); // the keyboard goes; the search stays
	void UpdateSearch(const Input& in, double dt);
	void BuildSearch(std::vector<UiVertex>& ui, float W, float H, float k, uint32_t accent);
	// 2.11 (swordpdf: "moving the right stick would allow you to slickly flip it and see the back in the shelf"): how far the picked
	// case is turned over, 0 front to 1 back, sprung to what the right stick (or its click, which holds it) asks.
	float m_flip = 0, m_flip_vel = 0;
	float m_flip_dir = 1.0f; // which way it turns: the stick's side
	bool m_flip_latched = false;
	// 2.11 (swordpdf: "back covers are not loading instantly"): the player is turning the case over -- the stick is past its dead zone or the click holds it
	// -- so its back is wanted at once, network included. (m_flip itself is no sign of that: it stays above zero for a third of a second after the stick is
	// let go.)
	bool m_flip_wanted = false;
	bool m_r3_prev = false;
	int m_back_for = -2; // the game (an index of Init's list) whose back cover was asked for last
	int m_prefetch_for = -2; // the game whose neighbours' backs were handed to the service (PrefetchBacks)
	// 2.11 (swordpdf, build 175: "the back cover gets deloaded before the case has turned back to the front, and the user can see a black spot for a
	// second"): m_flip turns every case near the scroll position (Pose), so the case the selection just left is still turned over -- showing its back --
	// while it slides aside and the flip comes back. Its back cover (1.7 MB) is kept for that time: the games, not the picked one, that hold theirs.
	std::vector<int> m_back_held; // indices of Init's list
	float FlipTurn(float d) const;    // what the flip adds to the pose of the case d places from the scroll position (radians)
	bool BackOnShow(int pos) const;   // the case at this position is drawn turned over by the flip (so its back may be in view)
	void ReleaseBack(Slot& s, int index); // its back cover let go, the service told
	void PollBackHolds(int sel_index);  // each frame: the held backs of cases that are turned back to the front go
	int m_ring_count = 0, m_ring_first = -1;
	int m_last_dir = 1;      // which way the selection last moved (the neighbours that way come first)
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
	bool m_moved = false; // fix180: the player (or a chord) moved the shelf since it opened
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
	// 180: System update: the first Cross on an available release arms (until then), and the time PS5SX2 closes once the installer runs (< 0: not yet).
	double m_update_armed_until = -1;
	double m_update_quit_at = -1;
	bool m_update_quit_done = false;
	// 2.12: the Home screen tile row: every ask made (also for games whose sheet has closed), the lines of the helper's state.txt, what the sheet's game shows,
	// and the game it is for (its normalized path, title and serial as the request carries them).
	gametile::Tracker m_tile;
	gametile::StateReader m_tile_state;
	gametile::View m_tile_view;
	bool m_tile_row = false;        // the sheet shows the row (set at OpenSheet)
	bool m_tile_view_known = false; // m_tile_view was looked at since the sheet opened (a tile turning On is told only after that)
	std::string m_tile_path, m_tile_title, m_tile_serial;
	double m_tile_poll = -10;       // when the state was looked at last
	int m_tile_polls = 0;

	// vk-285-134: no PS2 BIOS (AppConfig::bios_present said so at start or at the last pick), what was found instead,
	// and when a pick was refused for it (the line lights up).
	bool m_bios_missing = false;
	bool m_gamedb_note_shown = false; // gamedb (2026-10-30)
	std::string m_bios_problem;
	double m_bios_refused = -10;
	// bioscheck: what waits on the BIOS question: a pick of the shelf, the system menu, or a start from outside the shelf.
	enum class BiosWait
	{
		None,
		Pick,
		SystemMenu,
		Outside,
	};
	bool BiosReady(BiosWait wait = BiosWait::None, const std::string& path = std::string()); // looks again; false (the line lit, the question up) when there's still none
	void BuildBiosLine(std::vector<UiVertex>& ui, float W, float k);
	void BuildGameDbLine(std::vector<UiVertex>& ui, float W, float k); // gamedb (2026-10-30)
	std::string DiscStem(const std::string& serial) const;           // gamedb (2026-10-30)
	// bioscheck: the BIOS question (Left/Right choose, Cross confirms, Circle closes it).
	bool m_bios_ask = false;
	int m_bios_sel = 0;
	float m_bios_anim = 0;
	BiosWait m_bios_wait = BiosWait::None;
	std::string m_bios_wait_path;      // BiosWait::Outside: the start that waits
	std::string m_bios_outside_asked;  // the outside start the question last came up for (a request asked again doesn't reopen it)
	std::string m_approved_launch;     // an outside start the open BIOS let go
	bool m_bios_open_in_use = false;   // the shelf names the open BIOS
	void OpenBiosChoice(BiosWait wait, const std::string& path);
	void UpdateBiosChoice(const Input& in);
	void BuildBiosChoice(std::vector<UiVertex>& ui, float W, float H, float k, uint32_t accent);
	bool UseOpenBios(); // gs.ini PS5SX2/OpenBios=true, then looks again
	void RefreshBios(); // looks again quietly (the sheet closed: the OpenBios switch may have changed)
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
