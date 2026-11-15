// PS5 port frontend (2.41, AI-assisted; flag newsheet): the game sheet's icons (Font Awesome Free solid), shared by the shelf's sheet
// (fe_app.cpp) and the in-game menu's (fe_ingame.cpp), so both draw the same page, the same everyday rows and the same reset.
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
// One a page, in OptionsSheet::Page order: Picture, Speed, Fixes, Patches and cheats, Memory cards, Game options.
constexpr uint32_t kPage[OptionsSheet::kPageCount] = {0xF03E /* image */, 0xF625 /* gauge-high */, 0xF0AD /* wrench */,
	0xF12E /* puzzle-piece */, 0xF7C2 /* sd-card */, 0xF11B /* gamepad */};
constexpr uint32_t kBack = 0xF053;  // chevron-left: before a page's title
constexpr uint32_t kReset = 0xF2EA; // rotate-left: "Reset to defaults"

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
	cps.push_back(kBack);
	cps.push_back(kReset);
	cps.insert(cps.end(), std::begin(kHome), std::end(kHome));
	return cps;
}
} // namespace fe::sheeticons
