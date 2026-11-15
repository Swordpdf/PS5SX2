// PS5 port frontend (2.41, AI-assisted; flag newsheet): the game sheet's icons (Font Awesome Free solid), shared by the shelf's sheet
// (fe_app.cpp) and the in-game menu's (fe_ingame.cpp), so both draw the same page, the same everyday rows and the same reset.
// 318: a colour a page (its tile on the home and its title), the sheet for all games' pages, the update card and the help card's mark.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_options.h"

#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

namespace fe::sheeticons
{
// One a page, in OptionsSheet::Page order: Picture, Speed, Fixes, Patches and cheats, Memory cards, Game options, Library, PS5SX2.
constexpr uint32_t kPage[OptionsSheet::kPageCount] = {0xF03E /* image */, 0xF625 /* gauge-high */, 0xF0AD /* wrench */,
	0xF12E /* puzzle-piece */, 0xF7C2 /* sd-card */, 0xF11B /* gamepad */, 0xF07C /* folder-open */, 0xF013 /* gear */};
constexpr uint32_t kConsole = 0xF2DB; // microchip: the sheet for all games' "PS2 console" (its kPageFixes)
constexpr uint32_t kBack = 0xF053;    // chevron-left: before a page's title
constexpr uint32_t kReset = 0xF2EA;   // rotate-left: "Reset to defaults"
constexpr uint32_t kUpdate = 0xF019;  // download: PS5SX2's update, the first card of the sheet for all games
constexpr uint32_t kInfo = 0xF05A;    // circle-info: the help card
constexpr uint32_t kNextStart = 0xF017; // clock: in a running game, a row read when a game starts

// 318: a page's colour (red, green, blue): its icon's tile on the home and beside its title.
constexpr float kPageColor[OptionsSheet::kPageCount][3] = {
	{0.64f, 0.52f, 1.00f}, // Picture: violet
	{1.00f, 0.66f, 0.28f}, // Speed: amber
	{0.26f, 0.80f, 0.74f}, // Fixes: teal
	{0.42f, 0.84f, 0.48f}, // Patches and cheats: green
	{0.38f, 0.66f, 1.00f}, // Memory cards: blue
	{0.98f, 0.50f, 0.68f}, // Game options: rose
	{0.98f, 0.80f, 0.34f}, // Library: gold
	{0.72f, 0.62f, 1.00f}, // PS5SX2: the shelf's own lilac
};

inline uint32_t PageIcon(int page, bool global)
{
	if (page < 0 || page >= OptionsSheet::kPageCount)
		return 0;
	return global && page == OptionsSheet::kPageFixes ? kConsole : kPage[page];
}

// The four everyday rows of the home (OptionsSheet::IsHomeKey), in its order, and their icons.
constexpr const char* kHomeKeys[4] = {"upscale_multiplier", "AspectRatio", "EmuCore/EnableWideScreenPatches", "PS5SX2/FrameGeneration"};
constexpr uint32_t kHome[4] = {0xF065 /* expand */, 0xF125 /* crop */, 0xF26C /* tv */, 0xF04E /* forward */};

inline uint32_t HomeIcon(const OptionDef* d)
{
	for (int i = 0; d && i < 4; i++)
		if (d->key == kHomeKeys[i])
			return kHome[i];
	return 0;
}

// Every icon above, for Fonts::Preload with the icon font.
inline std::vector<uint32_t> Codepoints()
{
	std::vector<uint32_t> cps(std::begin(kPage), std::end(kPage));
	cps.push_back(kConsole);
	cps.push_back(kBack);
	cps.push_back(kReset);
	cps.push_back(kUpdate);
	cps.push_back(kInfo);
	cps.push_back(kNextStart);
	cps.insert(cps.end(), std::begin(kHome), std::end(kHome));
	return cps;
}
} // namespace fe::sheeticons
