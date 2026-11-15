// PS5 port frontend (318, AI-assisted; the owner: "make it pretty", "make the in-game menu match this style too"): the settings
// sheet's cards, drawn by one piece of code for the shelf's sheet (fe_app.cpp) and the in-game menu's (fe_ingame.cpp), so the two
// cannot drift apart. 2.41's first cards were written twice and already differed.
//
// What it draws, inside the rows' window of a sheet whose Settings tab has pages (OptionsSheet::Paged):
//   - runs of rows on cards (rounded panels, a hairline between rows), a group's name in small capitals over its card;
//   - the home's page rows as tiles, two a line (OptionsSheet::GridColumns): a coloured icon, the page's name and under it what is
//     set on the page, or what the page is for when nothing is;
//   - a page's title with its icon and a line saying what it holds;
//   - each value as a chip whose colour says where it comes from (this game, all games), plain text for PCSX2's default;
//   - the focus as an outline in the shelf's colour on a lighter row or tile;
//   - the help for the focused row on a card of its own, and on a game's sheet what the two chip colours mean.
// The callers keep what is theirs: the panel, the title, the tabs, the buttons, the status line, and through `look` the rows only
// they know (the texture pack, the home screen tile, the update, "next start" in a running game).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_options.h"
#include "fe_sheeticons.h"
#include "fe_text.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace fe::sheetcards
{
// Measures, in 2160-line pixels (drawn scaled by Context::k).
constexpr float kRowH = 104.0f;   // a setting
constexpr float kHeroH = 120.0f;  // the update card's row
constexpr float kTileH = 144.0f;  // a page's tile on the home
constexpr float kTileGap = 16.0f; // between two tiles, either way
constexpr float kGapH = 28.0f;    // between two cards
constexpr float kHeaderH = 72.0f; // a group's name over its card
constexpr float kTitleH = 136.0f; // a page's title and its line
constexpr float kSymbolScale = 1.35f, kSymbolDrop = 0.12f; // a button's symbol against the text beside it (fe_app.cpp's)

inline uint32_t Col(float r, float g, float b, float a = 1.0f)
{
	const auto c = [](float v) { return static_cast<uint32_t>(std::min(1.0f, std::max(0.0f, v)) * 255.0f + 0.5f); };
	return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

inline std::string Utf8(uint32_t cp)
{
	std::string s;
	if (cp < 0x80)
		s += static_cast<char>(cp);
	else if (cp < 0x800)
	{
		s += static_cast<char>(0xC0 | (cp >> 6));
		s += static_cast<char>(0x80 | (cp & 0x3F));
	}
	else
	{
		s += static_cast<char>(0xE0 | (cp >> 12));
		s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		s += static_cast<char>(0x80 | (cp & 0x3F));
	}
	return s;
}

// `text` cut to `width` with an ellipsis (whole UTF-8 characters).
inline std::string FitText(const Fonts& fonts, std::string text, float px, float width)
{
	if (fonts.Measure(text.c_str(), px) <= width)
		return text;
	while (!text.empty() && fonts.Measure((text + "\xE2\x80\xA6").c_str(), px) > width)
	{
		text.pop_back();
		while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
			text.pop_back();
		if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0xC0)
			text.pop_back();
	}
	return text + "\xE2\x80\xA6";
}

// `text` in lines of `width`, at most `max_lines` (the last one cut with an ellipsis).
inline std::vector<std::string> WrapText(const Fonts& fonts, const std::string& text, float px, float width, int max_lines)
{
	std::vector<std::string> lines;
	std::string line, word;
	const auto flush_word = [&]() {
		if (word.empty())
			return;
		const std::string tryline = line.empty() ? word : line + " " + word;
		if (!line.empty() && fonts.Measure(tryline.c_str(), px) > width)
		{
			lines.push_back(line);
			line = word;
		}
		else
			line = tryline;
		word.clear();
	};
	for (const char ch : text)
	{
		if (ch == ' ' || ch == '\n')
		{
			flush_word();
			if (ch == '\n' && !line.empty())
			{
				lines.push_back(line);
				line.clear();
			}
		}
		else
			word += ch;
	}
	flush_word();
	if (!line.empty())
		lines.push_back(line);
	if (static_cast<int>(lines.size()) > max_lines)
	{
		lines.resize(static_cast<size_t>(max_lines));
		lines.back() = FitText(fonts, lines.back() + " \xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6", px, width);
	}
	return lines;
}

// "<symbol>  Cross" (fe_options.cpp Sym: a PromptFont glyph, two spaces, the name) into its two parts.
inline bool SplitSymbol(const std::string& s, std::string& glyph, std::string& name)
{
	if (s.size() < 6 || static_cast<unsigned char>(s[0]) != 0xE2 || s.compare(3, 2, "  ") != 0)
		return false;
	glyph = s.substr(0, 3);
	name = s.substr(5);
	return true;
}

// How the caller draws: a filled rounded rectangle (top-left x, y) and a line of text at a baseline (returns its width).
struct Painter
{
	std::function<void(float x, float y, float w, float h, float radius, uint32_t color)> rect;
	std::function<float(const char* text, float x, float baseline, float px, uint32_t color, float weight, Fonts::Align align)> text;
};

// Where a value comes from, which is its chip's colour.
enum class Tone
{
	Plain,  // PCSX2's default: no chip
	Own,    // set for this game
	Global, // from the settings for all games
	Set,    // set on the sheet for all games
	Good,   // done, on, up to date
	Warn,   // failed
	Action, // press: the shelf's colour, filled
};

// One row as it is drawn. Filled from the sheet, then handed to the caller (Context::look) for the rows only it knows.
struct RowLook
{
	std::string value;
	Tone tone = Tone::Plain;
	bool arrows = true;     // left and right change it: arrows either side of the value while the row has the focus
	bool chevron = false;   // Cross opens something
	bool wide = false;      // a long value beside a short label (the texture pack)
	float progress = -1.0f; // 0..1: a thin bar along the row's foot
	std::string tag;        // a short amber note after the label ("next start")
	std::string note;       // a second, smaller line under the label
};

struct Context
{
	Painter paint;
	const Fonts* fonts = nullptr;
	const OptionsSheet* sheet = nullptr;
	int focus = -1;
	float x = 0, y = 0, sw = 0, sh = 0, k = 1.0f; // the sheet's panel, in pixels; k: pixels a 2160-line pixel
	float list_top = 0, list_bottom = 0;          // the rows' window, in pixels
	float scroll = 0;                              // how far the rows are scrolled (2160-line pixels)
	float* scroll_target = nullptr;                // where the scroll should go, kept so that the focused row is in the window
	float accent[3] = {0.72f, 0.62f, 1.00f};       // the shelf's colour
	float panel[3] = {0.045f, 0.050f, 0.105f};     // the panel's own colour (the focused row is filled with a lighter one)
	bool icons = true;                             // the icon font is there
	double now = 0;
	const char* press_again = "Press again";
	const char* legend_own = "set for this game";
	const char* legend_global = "from all games";
	// A mark some rows carry after their label (RowLook::tag) and what it means, for the legend (in a running game: a clock, "next
	// start"); empty and null: none.
	std::string legend_tag;
	const char* legend_tag_text = nullptr;
	// The highlight glides in the in-game menu: where it is drawn now (x, y, w, h in pixels; null or h 0: on the row at once), and
	// where it belongs (h 0 while the focused row is outside the window).
	const float* focus_drawn = nullptr;
	float* focus_goal = nullptr;
	std::function<void(size_t index, const OptionsSheet::Row&, bool focused, RowLook&)> look; // may be empty
	std::function<std::string(const OptionsSheet::Row&)> help;                                 // may be empty: the sheet's
};

// A row's place: from the cards' left edge and the list's top, in 2160-line pixels.
struct Place
{
	float x = 0, y = 0, w = 0, h = 0;
	bool tile = false;
};

// Every row's place for cards `width` wide; returns the list's height.
inline float Layout(const OptionsSheet& sheet, float width, std::vector<Place>& out)
{
	const auto& rows = sheet.rows();
	out.assign(rows.size(), Place());
	const bool home = sheet.Paged() && sheet.Page() < 0;
	float acc = 0;
	for (size_t i = 0; i < rows.size();)
	{
		const OptionsSheet::Row& r = rows[i];
		if (r.kind == OptionsSheet::Kind::Category && sheet.GridColumns() > 1)
		{
			size_t j = i;
			while (j + 1 < rows.size() && rows[j + 1].kind == OptionsSheet::Kind::Category)
				j++;
			const float tw = (width - kTileGap) * 0.5f;
			for (size_t t = i; t <= j; t++)
			{
				const size_t n = t - i;
				Place& p = out[t];
				p.x = static_cast<float>(n % 2) * (tw + kTileGap);
				p.y = acc + static_cast<float>(n / 2) * (kTileH + kTileGap);
				p.w = tw;
				p.h = kTileH;
				p.tile = true;
			}
			const float lines = static_cast<float>((j - i) / 2 + 1);
			acc += lines * kTileH + (lines - 1.0f) * kTileGap;
			i = j + 1;
			continue;
		}
		float h = kRowH;
		if (r.kind == OptionsSheet::Kind::Header)
			h = (i == 0 && sheet.Page() >= 0) ? kTitleH : r.label.empty() ? kGapH : kHeaderH;
		else if (home && r.kind == OptionsSheet::Kind::SystemUpdate)
			h = kHeroH;
		Place& p = out[i];
		p.y = acc;
		p.w = width;
		p.h = h;
		acc += h;
		i++;
	}
	return acc;
}

// An icon in a rounded tile of its colour.
inline void IconTile(const Painter& paint, float x, float y, float size, const float rgb[3], uint32_t glyph, bool lit)
{
	paint.rect(x, y, size, size, size * 0.30f, Col(rgb[0], rgb[1], rgb[2], lit ? 0.32f : 0.20f));
	if (glyph == 0)
		return;
	const float px = size * 0.46f;
	const auto up = [](float v) { return v + (1.0f - v) * 0.34f; };
	paint.text(Utf8(glyph).c_str(), x + size * 0.5f, y + size * 0.5f + px * 0.375f, px, Col(up(rgb[0]), up(rgb[1]), up(rgb[2])), 0.4f,
		Fonts::Center);
}

// The focus: an outline in the shelf's colour (`accent`) with a faint band round it, on a fill a little lighter than the panel.
inline void FocusRing(const Painter& paint, const float accent[3], const float panel[3], float x, float y, float w, float h, float radius,
	float k)
{
	const auto lift = [](float v, float t) { return v + (1.0f - v) * t; };
	paint.rect(x - 9 * k, y - 9 * k, w + 18 * k, h + 18 * k, radius + 9 * k, Col(accent[0], accent[1], accent[2], 0.14f));
	paint.rect(x - 3 * k, y - 3 * k, w + 6 * k, h + 6 * k, radius + 3 * k,
		Col(lift(accent[0], 0.2f), lift(accent[1], 0.2f), lift(accent[2], 0.2f), 0.96f));
	paint.rect(x, y, w, h, radius, Col(lift(panel[0], 0.15f) * 0.94f + accent[0] * 0.06f, lift(panel[1], 0.15f) * 0.94f + accent[1] * 0.06f,
		lift(panel[2], 0.15f) * 0.94f + accent[2] * 0.06f));
}

inline void Draw(const Context& c)
{
	using Kind = OptionsSheet::Kind;
	const OptionsSheet& sheet = *c.sheet;
	const Fonts& f = *c.fonts;
	const auto& rows = sheet.rows();
	const float k = c.k;
	const uint32_t hi = Col(1, 1, 1), mid = Col(0.74f, 0.72f, 0.84f), lo = Col(0.52f, 0.50f, 0.64f);
	const uint32_t accent = Col(c.accent[0], c.accent[1], c.accent[2]);
	const auto lift = [](float v, float t) { return v + (1.0f - v) * t; };
	const uint32_t own = Col(lift(c.accent[0], 0.45f), lift(c.accent[1], 0.45f), lift(c.accent[2], 0.45f));
	const uint32_t amber = Col(1.0f, 0.77f, 0.45f);
	const float bx = c.x + 32 * k, bw = c.sw - 64 * k; // the cards
	const float view_h = (c.list_bottom - c.list_top) / k;
	const bool home = sheet.Paged() && sheet.Page() < 0;
	const bool global = sheet.is_global();
	const int n = static_cast<int>(rows.size());

	std::vector<Place> at;
	const float total = Layout(sheet, bw / k, at);

	// The scroll keeps the focused row in the window, with the header over it (over the tiles' first line too).
	if (c.scroll_target && c.focus >= 0 && c.focus < n)
	{
		const size_t fi = static_cast<size_t>(c.focus);
		float top = at[fi].y;
		const float bottom = at[fi].y + at[fi].h;
		size_t first = fi;
		while (at[fi].tile && first > 0 && at[first - 1].tile && at[first - 1].y >= at[fi].y)
			first--;
		if (first > 0 && rows[first - 1].kind == Kind::Header && (!at[fi].tile || first == fi || at[first].y == at[fi].y))
			top = std::min(top, at[first - 1].y);
		float t = *c.scroll_target;
		if (top < t + 12.0f)
			t = std::max(0.0f, top - 12.0f);
		if (bottom > t + view_h - 12.0f)
			t = bottom - view_h + 12.0f;
		*c.scroll_target = std::max(0.0f, std::min(t, std::max(0.0f, total - view_h)));
	}
	const auto py = [&](float units) { return c.list_top + (units - c.scroll) * k; };
	const auto inside = [&](const Place& p) { return py(p.y) >= c.list_top - 2 * k && py(p.y + p.h) <= c.list_bottom + 2 * k; };

	// The cards: each run of rows between headers (the part in the window), and each tile.
	const uint32_t card = Col(1, 1, 1, 0.055f);
	for (size_t i = 0; i < rows.size();)
	{
		if (rows[i].kind == Kind::Header || at[i].tile)
		{
			if (at[i].tile && inside(at[i]))
				c.paint.rect(bx + at[i].x * k, py(at[i].y), at[i].w * k, at[i].h * k, 30 * k, card);
			i++;
			continue;
		}
		size_t j = i;
		while (j + 1 < rows.size() && rows[j + 1].kind != Kind::Header && !at[j + 1].tile)
			j++;
		// The card covers the run's rows that are in the window, whole rows only: it never ends in an empty stub at the window's edge.
		size_t a = i, b = i;
		bool any = false;
		for (size_t t = i; t <= j; t++)
			if (inside(at[t]))
			{
				if (!any)
					a = t;
				b = t;
				any = true;
			}
		if (any)
			c.paint.rect(bx, py(at[a].y), bw, py(at[b].y + at[b].h) - py(at[a].y), 30 * k, card);
		i = j + 1;
	}

	// The focus: an outline in the shelf's colour on a lighter fill.
	float goal[4] = {0, 0, 0, 0};
	bool focus_tile = false;
	if (c.focus >= 0 && c.focus < n && sheet.Selectable(rows[static_cast<size_t>(c.focus)]) && inside(at[static_cast<size_t>(c.focus)]))
	{
		const Place& p = at[static_cast<size_t>(c.focus)];
		focus_tile = p.tile;
		if (p.tile)
		{
			goal[0] = bx + p.x * k;
			goal[1] = py(p.y);
			goal[2] = p.w * k;
			goal[3] = p.h * k;
		}
		else
		{
			goal[0] = bx + 6 * k;
			goal[1] = py(p.y) + 5 * k;
			goal[2] = bw - 12 * k;
			goal[3] = p.h * k - 10 * k;
		}
	}
	if (c.focus_goal)
		std::copy(goal, goal + 4, c.focus_goal);
	const float* const fr = (c.focus_drawn && c.focus_drawn[3] > 0 && goal[3] > 0) ? c.focus_drawn : goal;
	if (fr[3] > 0)
		FocusRing(c.paint, c.accent, c.panel, fr[0], fr[1], fr[2], fr[3], (focus_tile ? 30.0f : 26.0f) * k, k);

	for (size_t i = 0; i < rows.size(); i++)
	{
		const OptionsSheet::Row& r = rows[i];
		const Place& p = at[i];
		if (!inside(p))
			continue;
		const float rx = bx + p.x * k, ry = py(p.y), rw = p.w * k, rh = p.h * k;
		const bool focused = static_cast<int>(i) == c.focus;

		if (r.kind == Kind::Header)
		{
			if (i == 0 && sheet.Page() >= 0) // the page: a way back, its icon, its name, what it holds
			{
				const int page = sheet.Page();
				float tx = bx + 10 * k;
				if (c.icons && page < OptionsSheet::kPageCount)
				{
					c.paint.text(Utf8(sheeticons::kBack).c_str(), bx + 22 * k, ry + 68 * k, 30 * k, lo, 0.4f, Fonts::Center);
					const float s = 76 * k, ix = bx + 54 * k;
					IconTile(c.paint, ix, ry + 16 * k, s, sheeticons::kPageColor[page], sheeticons::PageIcon(page, global), true);
					tx = ix + s + 26 * k;
				}
				c.paint.text(OptionsSheet::PageName(page, global), tx, ry + 58 * k, 48 * k, hi, 0.5f, Fonts::Left);
				c.paint.text(FitText(f, OptionsSheet::PageTagline(page, global), 28 * k, bx + bw - tx - 10 * k).c_str(), tx, ry + 98 * k, 28 * k, mid,
					0.2f, Fonts::Left);
			}
			else if (!r.label.empty() && i + 1 < rows.size() && inside(at[i + 1])) // a group's name over its card (with its first row in view)
			{
				std::string upper = r.label;
				for (char& ch : upper)
					ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
				c.paint.text(upper.c_str(), bx + 16 * k, ry + 52 * k, 24 * k, lo, 0.5f, Fonts::Left);
			}
			continue;
		}

		if (p.tile) // a page of the home: its icon, its name, what is set on it (else what it is for)
		{
			const int page = std::max(0, std::min(r.slot, OptionsSheet::kPageCount - 1));
			float tx = rx + 30 * k;
			if (c.icons)
			{
				const float s = 76 * k, ix = rx + 26 * k;
				IconTile(c.paint, ix, ry + (rh - s) * 0.5f, s, sheeticons::kPageColor[page], sheeticons::PageIcon(page, global), focused);
				tx = ix + s + 24 * k;
			}
			const float room = rx + rw - 22 * k - tx;
			const std::string summary = sheet.Value(r);
			c.paint.text(FitText(f, r.label, 38 * k, room).c_str(), tx, ry + rh * 0.5f - 6 * k, 38 * k, hi, 0.3f, Fonts::Left);
			c.paint.text(FitText(f, summary.empty() ? std::string(OptionsSheet::PageTagline(page, global)) : summary, 27 * k, room).c_str(), tx,
				ry + rh * 0.5f + 34 * k, 27 * k, summary.empty() ? lo : Col(0.86f, 0.84f, 0.96f), 0.2f, Fonts::Left);
			continue;
		}

		const bool action = r.kind == Kind::Recommended || r.kind == Kind::ResetAll || r.kind == Kind::SystemMenu;
		RowLook look;
		look.value = sheet.Value(r);
		if (action)
		{
			look.value.clear();
			look.arrows = false;
		}
		else
		{
			const OptionsSheet::From from = sheet.Source(r);
			look.tone = from == OptionsSheet::From::Own ? (global ? Tone::Set : Tone::Own) : from == OptionsSheet::From::Global ? Tone::Global : Tone::Plain;
		}
		// The home's Frame generation row names the method that makes the frames when it is not PS5SX2's own.
		if (home && r.kind == Kind::Option && r.def && r.def->key == "PS5SX2/FrameGeneration" && sheet.EffectiveValue(r.def->key) == "true")
		{
			const std::string method = sheet.EffectiveValue("PS5SX2/FrameGenMethod");
			if (!method.empty() && method != "PS5SX2")
				look.value = sheet.EffectiveLabel("PS5SX2/FrameGenMethod");
		}
		if (c.look)
			c.look(i, r, focused, look);
		if (sheet.Armed(r, c.now))
		{
			look.value = c.press_again;
			look.tone = Tone::Action;
			look.arrows = false;
		}
		else if (r.kind == Kind::NewCard && focused)
			look.value += "  \xC2\xB7  Create";

		// The row's icon: the home's everyday rows and the update, and a reset.
		const bool hero = home && r.kind == Kind::SystemUpdate;
		uint32_t glyph = 0;
		if (c.icons)
			glyph = hero ? sheeticons::kUpdate : (home && r.kind == Kind::Option) ? sheeticons::HomeIcon(r.def) :
			        (r.kind == Kind::Recommended || r.kind == Kind::ResetAll) ? sheeticons::kReset : 0u;
		float lx = rx + 40 * k;
		if (glyph)
		{
			const float s = (hero ? 76.0f : 64.0f) * k, ix = rx + 26 * k;
			IconTile(c.paint, ix, ry + (rh - s) * 0.5f, s, c.accent, glyph, focused);
			lx = ix + s + 26 * k;
		}
		const bool prev_row = i > 0 && rows[i - 1].kind != Kind::Header && !at[i - 1].tile;
		if (prev_row && !focused && static_cast<int>(i) - 1 != c.focus)
			c.paint.rect(lx, ry - 1 * k, rx + rw - 36 * k - lx, 2 * k, 0, Col(1, 1, 1, 0.07f));

		// The value: a chip in its tone's colour, right-aligned; arrows either side while it can be changed.
		const float lpx = 40 * k, vpx = 34 * k, pad = 22 * k;
		const float base = ry + rh * 0.5f + 13 * k, vbase = ry + rh * 0.5f + 12 * k;
		const float right = rx + rw - 34 * k;
		const bool arrows = focused && look.arrows && !look.value.empty();
		std::string vglyph, vname;
		const bool vsym = SplitSymbol(look.value, vglyph, vname);
		const float sym_w = vsym && vglyph != icon::Blank ? f.Measure(vglyph.c_str(), vpx * kSymbolScale) + 14 * k : 0.0f;
		const std::string shown = FitText(f, vsym ? vname : look.value, vpx, rw * (look.wide ? 0.66f : 0.46f) - sym_w);
		const float text_w = shown.empty() ? 0.0f : f.Measure(shown.c_str(), vpx) + sym_w;
		const float edge = right - (arrows ? 44 * k : 0.0f) - (look.chevron ? 40 * k : 0.0f); // the chip's right edge
		const float chip_w = text_w + 2 * pad, chip_x = edge - chip_w;
		const float value_w = shown.empty() ? (look.chevron ? 40 * k : 0.0f) : right - chip_x + (arrows ? 44 * k : 0.0f);

		std::string lglyph, lname;
		const bool lsym = SplitSymbol(r.label, lglyph, lname);
		const float slot = lsym ? 66 * k : 0.0f;
		const float tag_w = look.tag.empty() ? 0.0f : f.Measure(look.tag.c_str(), 25 * k) + 20 * k;
		const std::string label = FitText(f, lsym ? lname : r.label, lpx, right - lx - slot - value_w - tag_w - 28 * k);
		const uint32_t lc = action ? own : hi;
		const float lbase = look.note.empty() ? base : base - 17 * k;
		if (lsym && lglyph != icon::Blank)
			c.paint.text(lglyph.c_str(), lx + slot * 0.42f, lbase + kSymbolDrop * lpx, lpx * kSymbolScale, lc, 0.25f, Fonts::Center);
		const float lw = c.paint.text(label.c_str(), lx + slot, lbase, lpx, lc, action ? 0.4f : 0.25f, Fonts::Left);
		if (!look.tag.empty())
			c.paint.text(look.tag.c_str(), lx + slot + lw + 20 * k, lbase - 1 * k, 25 * k, amber, 0.4f, Fonts::Left);
		if (!look.note.empty())
			c.paint.text(FitText(f, look.note, 27 * k, right - lx - value_w - 28 * k).c_str(), lx, base + 25 * k, 27 * k, lo, 0.2f, Fonts::Left);

		if (!shown.empty())
		{
			uint32_t fill = 0, ink = mid;
			switch (look.tone)
			{
				case Tone::Plain:
					break;
				case Tone::Own:
					fill = Col(1.00f, 0.40f, 0.66f, 0.27f);
					ink = Col(1.00f, 0.84f, 0.91f);
					break;
				case Tone::Global:
					fill = Col(0.34f, 0.60f, 1.00f, 0.27f);
					ink = Col(0.82f, 0.90f, 1.00f);
					break;
				case Tone::Set:
					fill = Col(1, 1, 1, 0.11f);
					ink = hi;
					break;
				case Tone::Good:
					fill = Col(0.30f, 0.86f, 0.56f, 0.20f);
					ink = Col(0.74f, 0.98f, 0.84f);
					break;
				case Tone::Warn:
					fill = Col(1.00f, 0.46f, 0.42f, 0.22f);
					ink = Col(1.00f, 0.80f, 0.78f);
					break;
				case Tone::Action:
					fill = Col(lift(c.accent[0], 0.25f), lift(c.accent[1], 0.25f), lift(c.accent[2], 0.25f), 0.96f);
					ink = Col(0.07f, 0.06f, 0.16f);
					break;
			}
			const float ch = 58 * k;
			if (look.tone != Tone::Plain)
				c.paint.rect(chip_x, ry + (rh - ch) * 0.5f, chip_w, ch, ch * 0.5f, fill);
			const float tw = c.paint.text(shown.c_str(), edge - pad, vbase, vpx, ink, 0.5f, Fonts::Right);
			if (sym_w > 0)
				c.paint.text(vglyph.c_str(), edge - pad - tw - 14 * k, vbase + kSymbolDrop * vpx, vpx * kSymbolScale, ink, 0.5f, Fonts::Right);
			if (arrows)
			{
				c.paint.text("\xE2\x80\xB9", chip_x - 20 * k, vbase + 2 * k, 42 * k, hi, 0.4f, Fonts::Center);
				c.paint.text("\xE2\x80\xBA", edge + 20 * k, vbase + 2 * k, 42 * k, hi, 0.4f, Fonts::Center);
			}
		}
		if (look.chevron)
			c.paint.text("\xE2\x80\xBA", right - 10 * k, base + 1 * k, 46 * k, focused ? hi : lo, 0.4f, Fonts::Center);
		if (look.progress >= 0.0f) // something on its way: a thin bar along the row's foot
		{
			const float fx = lx, fw = right - lx, fy = ry + rh - 15 * k;
			const float done = std::min(1.0f, std::max(0.0f, look.progress));
			c.paint.rect(fx, fy, fw, 5 * k, 2.5f * k, Col(1, 1, 1, 0.10f));
			if (done > 0)
				c.paint.rect(fx, fy, std::max(5 * k, fw * done), 5 * k, 2.5f * k, accent);
		}
	}

	// More above or below.
	if (c.scroll > 4.0f)
		c.paint.text("\xE2\x80\xA2 \xE2\x80\xA2 \xE2\x80\xA2", c.x + c.sw * 0.5f, c.list_top - 10 * k, 22 * k, lo, 0.3f, Fonts::Center);
	if (c.scroll + view_h < total - 4.0f)
		c.paint.text("\xE2\x80\xA2 \xE2\x80\xA2 \xE2\x80\xA2", c.x + c.sw * 0.5f, c.list_bottom + 20 * k, 22 * k, lo, 0.3f, Fonts::Center);

	// What the focused row does, on a card of its own: as tall as its text, standing on the sheet's foot.
	if (c.focus >= 0 && c.focus < n)
	{
		const OptionsSheet::Row& r = rows[static_cast<size_t>(c.focus)];
		const std::string help = c.help ? c.help(r) : sheet.Help(r);
		const float tx = bx + (c.icons ? 96.0f : 36.0f) * k;
		const float hb = c.y + c.sh - 88 * k, most = hb - (c.list_bottom + 30 * k);
		const int room = std::max(1, std::min(4, static_cast<int>((most - 42 * k) / (44 * k))));
		const std::vector<std::string> lines = WrapText(f, help, 30 * k, bx + bw - 36 * k - tx, room);
		if (!lines.empty())
		{
			const float hh = (42.0f + 44.0f * static_cast<float>(lines.size())) * k, hy = hb - hh;
			c.paint.rect(bx, hy, bw, hh, 28 * k, Col(1, 1, 1, 0.035f));
			if (c.icons)
				c.paint.text(Utf8(sheeticons::kInfo).c_str(), bx + 54 * k, hy + 60 * k, 32 * k, lo, 0.3f, Fonts::Center);
			float line_y = hy + 56 * k;
			for (const std::string& line : lines)
			{
				c.paint.text(line.c_str(), tx, line_y, 30 * k, mid, 0.1f, Fonts::Left);
				line_y += 44 * k;
			}
		}
	}
	// At the foot, beside the status line: what a row's mark means, and on a game's sheet what the two chip colours mean.
	const float ly = c.y + c.sh - 40 * k, dot = 16 * k;
	float lx = c.x + c.sw - 64 * k;
	if (c.legend_tag_text && !c.legend_tag.empty())
	{
		lx -= c.paint.text(c.legend_tag_text, lx, ly, 24 * k, lo, 0.3f, Fonts::Right) + 10 * k;
		lx -= c.paint.text(c.legend_tag.c_str(), lx, ly, 22 * k, amber, 0.3f, Fonts::Right) + 34 * k;
	}
	if (!global && c.legend_own && c.legend_global)
	{
		lx -= c.paint.text(c.legend_global, lx, ly, 24 * k, lo, 0.3f, Fonts::Right) + 12 * k + dot;
		c.paint.rect(lx, ly - 18 * k, dot, dot, dot * 0.5f, Col(0.50f, 0.70f, 1.00f, 0.90f));
		lx -= 34 * k;
		lx -= c.paint.text(c.legend_own, lx, ly, 24 * k, lo, 0.3f, Fonts::Right) + 12 * k + dot;
		c.paint.rect(lx, ly - 18 * k, dot, dot, dot * 0.5f, Col(1.00f, 0.56f, 0.76f, 0.90f));
	}
}
} // namespace fe::sheetcards
