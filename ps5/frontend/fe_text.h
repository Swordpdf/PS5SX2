// PS5 port frontend: fonts. A signed-distance-field atlas for the UI (any size, one texture) and
// plain coverage rasterization for the textures the frontend paints on the CPU (spines and
// placeholder covers).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fe
{
// One vertex of the UI pipeline (ui.vert): pixels, atlas uv (or pixels from a shape's centre),
// RGBA8 colour (0xAABBGGRR) and the shader's parameters.
struct UiVertex
{
	float x, y;
	float u, v;
	uint32_t color;
	float p0, p1, p2, p3;
};

// Decodes one UTF-8 code point at *s and advances it (invalid bytes become U+FFFD).
uint32_t NextCodepoint(const char*& s);

class Fonts
{
public:
	struct Glyph
	{
		float u0, v0, u1, v1;   // atlas
		float x0, y0, x1, y1;   // quad relative to the pen, in base pixels (y down)
		float advance;          // base pixels
		int font;
	};

	// `text_font`/`icon_font`: TrueType/OpenType data that stays alive with this object.
	// `brand_font` (optional, vk-285-50): Font Awesome's brands, for the author's handles.
	bool Init(const uint8_t* text_font, size_t text_size, const uint8_t* icon_font, size_t icon_size,
		const uint8_t* brand_font = nullptr, size_t brand_size = 0);

	// The SDF atlas (R8), built by Init.
	const std::vector<uint8_t>& AtlasPixels() const { return m_atlas; }
	int AtlasWidth() const { return m_atlas_w; }
	int AtlasHeight() const { return m_atlas_h; }

	// Width in pixels of `utf8` at `px` (the em size in pixels).
	float Measure(const char* utf8, float px) const;

	// True when the atlas has the first character of `utf8` (the brand icons are optional).
	bool Has(const char* utf8) const;

	enum Align
	{
		Left,
		Center,
		Right
	};

	// Appends `utf8` at (x, baseline) as UI triangles. `weight`: 0 regular .. 1 bold (a shift of the
	// field's edge); returns the width.
	float AddText(std::vector<UiVertex>& out, const char* utf8, float x, float baseline, float px, uint32_t color,
		float weight = 0.0f, Align align = Left) const;

	// Appends a filled rounded rectangle (top-left x, y).
	static void AddRoundedRect(std::vector<UiVertex>& out, float x, float y, float w, float h, float radius, uint32_t color);

	// Ascent and descent (positive) of the text font at `px`.
	float Ascent(float px) const;
	float Descent(float px) const;

	// Plain coverage raster of `utf8` for CPU painting: an 8-bit alpha image `w` x `h` sized to the
	// text (the baseline at `baseline_out` from its top). `px` is the em size.
	void Raster(const char* utf8, float px, std::vector<uint8_t>& alpha, int& w, int& h, int& baseline_out) const;

private:
	const Glyph* Find(uint32_t cp) const;
	bool AddGlyph(int font, uint32_t cp);

	struct FontData;
	std::vector<FontData*> m_fonts;
	std::unordered_map<uint32_t, Glyph> m_glyphs;
	std::vector<uint8_t> m_atlas;
	int m_atlas_w = 0, m_atlas_h = 0;
	int m_pen_x = 0, m_pen_y = 0, m_row_h = 0;

public:
	~Fonts();
	static constexpr float kBasePx = 64.0f;   // the SDF's em size
	static constexpr int kPad = 8;            // field padding in atlas pixels
	static constexpr float kSpread = 8.0f;    // field distance (pixels) that spans half the range
};

// Test build 1 (vk-285-55): the testing builds' watermark over the game: `line1` big (bold),
// `line2` under it and `line3` (vk-285-105, may be null) smaller under that, white with a soft
// shadow at the opacities given (line3 at line2's), sized for a 2160-line screen. RGBA8 pixels
// with R in the low byte and straight alpha, `w` x `h`.
void RasterWatermark(const Fonts& fonts, const char* line1, const char* line2, const char* line3, float alpha1,
	float alpha2, std::vector<uint32_t>& rgba, int& w, int& h);

// PromptFont's PlayStation glyphs (as PCSX2's IconsPromptFont.h names them).
namespace icon
{
constexpr const char* Cross = "\xE2\x87\xA3";
constexpr const char* Circle = "\xE2\x87\xA2";
constexpr const char* Triangle = "\xE2\x87\xA1";
constexpr const char* Square = "\xE2\x87\xA0";
constexpr const char* L1 = "\xE2\x86\xB0";
constexpr const char* R1 = "\xE2\x86\xB1";
constexpr const char* DpadLeftRight = "\xE2\x86\xA2";
constexpr const char* DpadUpDown = "\xE2\x86\xA3"; // vk-285-114: the options sheet's hints
constexpr const char* Options = "\xE2\x88\x88";
// vk-285-116: the Controls tab's D-pad rows and values (the face buttons use the four above; the shoulder, trigger and
// stick glyphs carry their name inside, so those rows show just the name).
constexpr const char* DpadLeft = "\xE2\x86\x9E";
constexpr const char* DpadUp = "\xE2\x86\x9F";
constexpr const char* DpadRight = "\xE2\x86\xA0";
constexpr const char* DpadDown = "\xE2\x86\xA1";
// Not a glyph: an em space (U+2003, not in the atlas) that keeps a symbol's room on the sheet's rows with nothing drawn.
constexpr const char* Blank = "\xE2\x80\x83";
// Font Awesome Free 7 brands (SIL OFL 1.1; the icons CC BY 4.0), present when Init got the font.
constexpr const char* Discord = "\xEF\x8E\x92";   // U+F392
constexpr const char* XTwitter = "\xEE\x98\x9B";  // U+E61B
} // namespace icon
} // namespace fe
