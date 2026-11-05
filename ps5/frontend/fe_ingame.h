// PS5 port frontend: the in-game menu (2.31, AI-assisted; swordpdf: "rework the ps5sx2 in game menu (the one where we usually
// use L2+Dpad down to open the webui). I want the web ui options, but made in a way to fit the current ps5sx2 style and theme.
// make it glorious and intuitive").
//
// The settings page combo (PS5SX2/WebButton1/2, L2 + D-pad down by default, held for a second) pauses the game and opens this
// over it: the game's picture glides into a card on the left (the paused frame, blurred, behind everything), the shelf's options
// sheet slides in on the right. Its tabs (L2 / R2):
//   Game          resume, save and load states (ten slots, each with the screenshot PCSX2 keeps in the state), fast forward,
//                 change disc (a game on more than one disc), the settings page on a phone (its address and QR code), report a
//                 problem (a note in the session log), back to the shelf;
//   Settings      the shelf's sheet and the settings page's rows (fe_options.h), this game or all games (L1 / R1), saved at once
//   Controls      and applied when the game goes on (a row read when a game starts says "next start");
//   Achievements  the game's RetroAchievements, when signed in, each with its badge (build 190: the host loads them,
//                 WantedBadges / BadgeDone, into pictures of their own).
// Circle (or Options) goes back to the game.
//
// Drawn with the shelf's own text and shapes (fe::Fonts' UiVertex) plus three more shapes the GS's UI shader draws
// (bin/resources/shaders/vulkan/orbis_ui.glsl): a soft rectangle (glows, shadows), an image with rounded corners (the game's
// picture, the save states' screenshots) and a rounded outline. This file is the menu's state and drawing only: no Vulkan, no
// PCSX2. The emulator (main-boot.cpp) gives it the controller, runs what it asks for (InGameRequest) and hands what it builds
// to the GS (orbis-shims/OrbisInGameUi.h). ps5/frontend/host/ingame_preview.cpp draws it on a PC.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_game_achievements.h"
#include "fe_options.h"
#include "fe_text.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace fe
{
// The controller's buttons as OrbisPadData reports them (orbis-shims/OrbisPadMap.h).
namespace ingame_pad
{
constexpr uint32_t L3 = 0x2u, R3 = 0x4u, Options = 0x8u, Up = 0x10u, Right = 0x20u, Down = 0x40u, Left = 0x80u, L2 = 0x100u,
	R2 = 0x200u, L1 = 0x400u, R1 = 0x800u, Triangle = 0x1000u, Circle = 0x2000u, Cross = 0x4000u, Square = 0x8000u,
	Touch = 0x100000u;
}

// The UI shader's shapes beyond fe::Fonts' (UiVertex::p3): 0 text, 1 rounded rectangle, 2 RGBA image (the shelf's three), then
// 3 + s/4096: a rounded rectangle whose edge fades over s pixels; 4: an image with rounded corners (u, v 0..1 over it; p0, p1
// half size, p2 radius); 5 + t/4096: a rounded outline t pixels thick (u, v pixels from the centre, as 1).
namespace ui_mode
{
constexpr float Text = 0, Rect = 1, Image = 2, Soft = 3, RoundImage = 4, Outline = 5;
}

// What the vertices of a range are drawn with.
enum class InGameImage : int
{
	Atlas = 0, // fe::Fonts' atlas: the text (and the shapes, which don't read it)
	Blur = 1,  // the paused picture, small and blurred: the background
	Frame = 2, // the paused picture: u, v 0..1 over the game's picture (the GS maps them to where it is in its texture)
	Slot0 = 3, // Slot0 + i: save state slot i + 1's screenshot (InGameSlot::shot); Slot0 + kInGameSaveSlots + j: badge picture j
};

// The pictures after the atlas, the blur and the frame (orbis_ui::kSlots, kBadges): the save states' screenshots, then the
// Achievements tab's badges. A row's badge is picture (its row % kInGameBadges): more than the rows shown and those around them.
constexpr int kInGameSaveSlots = 10;
constexpr int kInGameBadges = 24;

struct InGameRange
{
	uint32_t first = 0, count = 0;
	int image = 0; // InGameImage, or Slot0 + slot index
};

// One frame of the menu: the vertices, in screen pixels, in ranges by what they are drawn with.
struct InGameFrame
{
	std::vector<UiVertex> ui;
	std::vector<InGameRange> ranges;
};

// The controller as the menu sees it: what is held, and what was pressed since the last Update (so that a tap between two
// updates is not lost).
struct InGamePad
{
	uint32_t held = 0;
	uint32_t pressed = 0;
	uint8_t lx = 128, ly = 128;
};

// What the menu asks the emulator for. TakeRequest hands each over once; Done says how it went.
struct InGameRequest
{
	enum class Kind
	{
		None,
		SaveState,   // slot (1..)
		LoadState,   // slot; the menu closes when it worked
		FastForward, // switch it, then the game goes on
		ChangeDisc,  // text: the image's path; the game goes on
		BackToShelf, // quit the game for the shelf
		Note,        // text: the note for the session log
	};
	Kind kind = Kind::None;
	int slot = 0;
	std::string text;
};

struct InGameSlot
{
	bool used = false;
	int64_t mtime = 0;          // UTC seconds
	bool frame_shot = false;    // its picture is the paused frame (saved just now)
	int shot_w = 0, shot_h = 0; // its screenshot's size, once the host has it (it hands the pixels to the GS: InGameImage::Slot0 + index)
};

// A badge the Achievements tab wants (InGameMenu::WantedBadges): achievement `id`'s picture (its grey one when `unlocked` is false)
// into badge picture `index` (0 .. kInGameBadges-1).
struct InGameBadgeWant
{
	int index = 0;
	uint32_t id = 0;
	bool unlocked = false;
};

// The running game and what the emulator can do, given before each Open.
struct InGameSession
{
	GameInfo game;   // title, serial, region, the settings file's stem
	bool has_game = true; // false: no game file (the PS2's own menu, an ELF without a disc): the sheets are for all games only
	std::vector<std::string> discs; // the game's set: two or more images give the Change disc row
	std::string disc;               // the one in now
	std::vector<InGameSlot> slots;  // slots 1..N
	bool fast_forward = false;
	bool hardcore = false;          // RetroAchievements' hardcore mode: no loading states
	std::string web_address;        // the settings page, "192.168.1.20:8844" ("" without a network)
	std::vector<unsigned char> qr;  // its QR code: qr_size x qr_size modules, 1 dark
	int qr_size = 0;
	std::vector<std::string> chips; // under the title: "4x", "16:9", "60 FPS"
	double played_s = -1;           // how long this game has run (< 0: not shown)
};

class InGameMenu
{
public:
	struct Config
	{
		const Fonts* fonts = nullptr;
		int icon_font = -1;   // fe::Fonts::AddFont's index of Font Awesome Free solid (-1: rows without icons)
		OptionsPaths paths;   // the sheets' files, as the shelf's
		std::function<GameAchievementsState()> achievements; // the running game's (empty: no Achievements tab)
		std::function<std::string()> clock;                   // "21:47"
		std::function<std::string(int64_t utc)> when;         // a save's time: "Today 21:47"
	};

	// The code points the menu's icons need (for Fonts::Preload with the icon font).
	static std::vector<uint32_t> IconCodepoints();

	void Configure(const Config& cfg);
	// The game, before Open. The sheets read their files here: call it where file reads may take a while (not on the GS thread).
	void SetSession(const InGameSession& s);
	// `held`: the buttons down now (the combo that opened it): ignored until they are let go.
	void Open(double now, uint32_t held);
	bool Active() const { return m_state != State::Closed; }
	bool Closed() const { return m_state == State::Closed; }
	// 0 (no menu) .. 1 (open), eased: how far the opening has gone.
	float Openness() const { return m_open; }
	void Update(double now, const InGamePad& pad);
	// `W` x `H`: the window. `frame`: where the present draws the game's picture without the menu (x0, y0, x1, y1).
	void Build(InGameFrame& out, float W, float H, const float frame[4]);
	bool TakeRequest(InGameRequest& out);
	// The last request is done: `status` is shown (the colour of a failure when !ok); `close` closes the menu.
	void Done(bool ok, const std::string& status, bool close);
	// Starts closing (the host resumes the game once Closed()).
	void Close();
	// Save state slot `slot` (1..) was written just now: shown with the paused picture and this time.
	void SlotSaved(int slot, int64_t utc);
	// Slot `slot`'s screenshot has come (the host read it from the state after the menu opened): drawn from now on.
	void SlotShot(int slot, int w, int h);
	// Build 190: the badges of the Achievements tab's rows on screen and a few around them (the first rows before the tab is shown)
	// that no picture has yet, the focused row's first, at most four, each asked again a second after "not there yet". The host
	// loads each (its PNG), hands the pixels to the GS (picture Slot0 + kInGameSaveSlots + index) and says how it went: `ok` it is
	// drawn from now on; otherwise not there yet, or never (`never`: the row keeps its icon).
	std::vector<InGameBadgeWant> WantedBadges(double now) const;
	void BadgeDone(const InGameBadgeWant& w, bool ok, bool never, double now);

	// The ring while the combo is held in the game, before the menu opens: `progress` 0 .. 1. Any thread.
	static void BuildHold(const Fonts& fonts, InGameFrame& out, float W, float H, float progress);

private:
	enum class State
	{
		Closed,
		Opening,
		Open,
		Closing,
	};
	enum class Tab
	{
		Game,
		Settings,
		Controls,
		Achievements,
	};
	enum class Action
	{
		Header,
		Resume,
		Save,
		Load,
		Fast,
		Disc,
		Phone,
		Report,
		Shelf,
	};
	struct ActionRow
	{
		Action action;
		std::string label;
	};

	void BuildActions();
	void OpenSheet(bool global);
	std::vector<Tab> Tabs() const;
	int TabIndex() const;
	void SwitchTab(int dir);
	void UpdateGame(double now, const InGamePad& pad, int v, int h);
	void UpdateSheet(double now, const InGamePad& pad, int v, int h);
	void UpdateAchievements(double now, int v);
	bool MoveGame(int dir);
	bool MoveSheet(int dir);
	void SetStatus(const std::string& s, bool ok);
	void Ask(const InGameRequest& r);
	std::string SlotValue(int slot) const;
	std::string ActionValue(const ActionRow& r) const;
	std::string ActionHelp(const ActionRow& r) const;
	int UsedSlotNear(int from, int dir) const;
	std::string PlayedText() const;

	void BuildLeft(InGameFrame& out, float W, float H, float k, const float frame[4], float e);
	void BuildPanel(InGameFrame& out, float W, float H, float k, float e);
	void BuildGameRows(InGameFrame& out, float x, float y, float sw, float sh, float k);
	void BuildSheetRows(InGameFrame& out, float x, float y, float sw, float sh, float k);
	void BuildAchievementRows(InGameFrame& out, float x, float y, float sw, float sh, float k);
	void BuildSlotStrip(InGameFrame& out, float x, float y, float w, float k);
	void BuildQr(InGameFrame& out, float x, float y, float side, float k);
	void BuildHints(InGameFrame& out, float W, float H, float k, float e);

	Config m_cfg;
	InGameSession m_s;
	State m_state = State::Closed;
	double m_now = 0, m_state_time = 0;
	float m_open = 0;
	uint32_t m_ignore = 0; // buttons held at Open, until let go
	uint32_t m_prev = 0;

	Tab m_tab = Tab::Game;
	float m_tab_pos = 0; // the shown tab's pill, gliding
	double m_tab_time = -10;

	std::vector<ActionRow> m_actions;
	int m_action_row = 0;
	int m_save_slot = 1, m_load_slot = 1, m_disc_pick = 0, m_problem = 0;
	int m_armed_action = -1;
	double m_armed_until = 0;

	OptionsSheet m_sheet;
	bool m_sheet_open = false, m_sheet_global = false;
	int m_sheet_row = 0;
	float m_sheet_scroll = 0, m_sheet_scroll_target = 0;

	GameAchievementsState m_ach;
	double m_ach_time = -10;
	int m_ach_row = 0;
	float m_ach_scroll = 0;
	int m_ach_first = 0, m_ach_last = 9; // the rows Build last drew (the first rows before the tab is shown)
	struct BadgePic
	{
		bool asked = false; // the host has said how this picture went (BadgeDone)
		uint32_t id = 0;
		bool unlocked = false;
		bool loaded = false; // the picture is that badge
		bool never = false;  // it can't be had: the icon stays
		double tried = -10;
	};
	BadgePic m_badges[kInGameBadges];

	float m_focus_y = -1;      // the focused row's highlight, gliding to m_focus_target (screen pixels; Update moves it)
	float m_focus_target = -1; // where Build last put the focused row
	int v_held = 0, h_held = 0;
	double v_next = 0, h_next = 0;

	std::string m_status;
	bool m_status_ok = true;
	double m_status_time = -10;
	bool m_busy = false;

	mutable std::mutex m_req_mutex;
	std::vector<InGameRequest> m_requests;
};
} // namespace fe
