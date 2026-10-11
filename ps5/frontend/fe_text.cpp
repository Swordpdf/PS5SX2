// PS5 port frontend: fonts (see fe_text.h).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_text.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"

namespace fe
{
struct Fonts::FontData
{
	stbtt_fontinfo info;
	float base_scale = 0; // font units -> base pixels
	int ascent = 0, descent = 0, line_gap = 0;
};

uint32_t NextCodepoint(const char*& s)
{
	const unsigned char c = static_cast<unsigned char>(*s);
	if (c < 0x80)
	{
		s++;
		return c;
	}
	int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : -1;
	if (extra < 0)
	{
		s++;
		return 0xFFFD;
	}
	uint32_t cp = c & (0x3F >> extra);
	s++;
	for (int i = 0; i < extra; i++)
	{
		const unsigned char cc = static_cast<unsigned char>(*s);
		if ((cc & 0xC0) != 0x80)
			return 0xFFFD;
		cp = (cp << 6) | (cc & 0x3F);
		s++;
	}
	return cp;
}

Fonts::~Fonts()
{
	for (FontData* f : m_fonts)
		delete f;
}

bool Fonts::Init(const uint8_t* text_font, size_t text_size, const uint8_t* icon_font, size_t icon_size,
	const uint8_t* brand_font, size_t brand_size)
{
	(void)text_size;
	(void)icon_size;
	(void)brand_size;
	const uint8_t* datas[3] = {text_font, icon_font, brand_font};
	for (const uint8_t* data : datas)
	{
		if (!data && data == brand_font && m_fonts.size() == 2)
			break; // the brands are optional
		FontData* f = new FontData();
		if (!data || !stbtt_InitFont(&f->info, data, stbtt_GetFontOffsetForIndex(data, 0)))
		{
			delete f;
			if (m_fonts.size() == 2)
				break; // an unreadable brands font only loses the two icons
			return false;
		}
		f->base_scale = stbtt_ScaleForMappingEmToPixels(&f->info, kBasePx);
		stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->line_gap);
		m_fonts.push_back(f);
	}

	m_atlas_w = 2048;
	m_atlas_h = 1024;
	m_atlas.assign(static_cast<size_t>(m_atlas_w) * m_atlas_h, 0);
	m_pen_x = m_pen_y = m_row_h = 0;

	for (uint32_t cp = 32; cp < 127; cp++)
		AddGlyph(0, cp);
	for (uint32_t cp = 160; cp < 256; cp++)
		AddGlyph(0, cp);
	// vk-285-114: 0x2039 and 0x203A, the options sheet's arrows either side of a value.
	const uint32_t extra[] = {0x2022, 0x2026, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2122, 0x2190,
		0x2192, 0x25B8, 0x2039, 0x203A, 0xFFFD};
	for (uint32_t cp : extra)
		AddGlyph(0, cp);
	const char* icons[] = {icon::Cross, icon::Circle, icon::Triangle, icon::Square, icon::L1, icon::R1,
		icon::DpadLeftRight, icon::DpadUpDown, icon::Options,
		// vk-285-116: the Controls tab's.
		icon::DpadLeft, icon::DpadUp, icon::DpadRight, icon::DpadDown};
	for (const char* s : icons)
	{
		const char* p = s;
		AddGlyph(1, NextCodepoint(p));
	}
	if (m_fonts.size() > 2)
		for (const char* s : {icon::Discord, icon::XTwitter})
		{
			const char* p = s;
			AddGlyph(2, NextCodepoint(p));
		}
	return true;
}

namespace
{
// stb_truetype's field decides inside and outside by counting crossings, and gets some CFF glyphs
// wrong: Font Awesome 7's Discord mark came out with nothing inside (vk-285-50). Those glyphs get
// their field from a 4x coverage raster instead, which fills by winding: the distance to the
// nearest edge between covered and uncovered samples, positive inside.
unsigned char* SdfFromCoverage(const stbtt_fontinfo* info, float scale, int glyph, int pad, float dist_scale, int* w, int* h,
	int* xoff, int* yoff)
{
	const int ss = 4;
	int bw = 0, bh = 0, bx = 0, by = 0;
	unsigned char* cov = stbtt_GetGlyphBitmap(info, scale * ss, scale * ss, glyph, &bw, &bh, &bx, &by);
	if (!cov)
		return nullptr;
	int ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
	stbtt_GetGlyphBitmapBox(info, glyph, scale, scale, &ix0, &iy0, &ix1, &iy1);
	const int ow = ix1 - ix0 + 2 * pad, oh = iy1 - iy0 + 2 * pad;
	auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < bw && y < bh && cov[y * bw + x] >= 128; };
	std::vector<float> ex, ey; // edge points, in 4x pixels
	for (int y = -1; y <= bh; y++)
		for (int x = -1; x <= bw; x++)
		{
			const bool c = in(x, y);
			if (c != in(x + 1, y))
			{
				ex.push_back(x + 1.0f);
				ey.push_back(y + 0.5f);
			}
			if (c != in(x, y + 1))
			{
				ex.push_back(x + 0.5f);
				ey.push_back(y + 1.0f);
			}
		}
	unsigned char* out = static_cast<unsigned char*>(STBTT_malloc(static_cast<size_t>(ow) * oh, nullptr));
	for (int py = 0; py < oh; py++)
		for (int px = 0; px < ow; px++)
		{
			// This pixel's centre in the 4x raster.
			const float X = (px + 0.5f + ix0 - pad) * ss - bx, Y = (py + 0.5f + iy0 - pad) * ss - by;
			float best = 1e30f;
			for (size_t i = 0; i < ex.size(); i++)
			{
				const float dx = ex[i] - X, dy = ey[i] - Y;
				best = std::min(best, dx * dx + dy * dy);
			}
			const float d = std::sqrt(best) / ss * (in(static_cast<int>(std::floor(X)), static_cast<int>(std::floor(Y))) ? 1.0f : -1.0f);
			out[py * ow + px] = static_cast<unsigned char>(std::clamp(128.0f + d * dist_scale, 0.0f, 255.0f));
		}
	stbtt_FreeBitmap(cov, nullptr);
	*w = ow;
	*h = oh;
	*xoff = ix0 - pad;
	*yoff = iy0 - pad;
	return out;
}
} // namespace

bool Fonts::AddGlyph(int font, uint32_t cp)
{
	FontData& f = *m_fonts[static_cast<size_t>(font)];
	const int index = stbtt_FindGlyphIndex(&f.info, static_cast<int>(cp));
	if (index == 0 && cp != 32)
		return false;
	int advance = 0, lsb = 0;
	stbtt_GetGlyphHMetrics(&f.info, index, &advance, &lsb);

	Glyph g = {};
	g.font = font;
	g.advance = advance * f.base_scale;
	int w = 0, h = 0, xoff = 0, yoff = 0;
	unsigned char* sdf = stbtt_GetGlyphSDF(&f.info, f.base_scale, index, kPad, 128, 128.0f / kSpread, &w, &h, &xoff, &yoff);
	if (sdf && w > 0 && h > 0 && *std::max_element(sdf, sdf + static_cast<size_t>(w) * h) < 128)
	{
		stbtt_FreeSDF(sdf, nullptr);
		sdf = SdfFromCoverage(&f.info, f.base_scale, index, kPad, 128.0f / kSpread, &w, &h, &xoff, &yoff);
	}
	if (sdf && w > 0 && h > 0)
	{
		if (m_pen_x + w + 1 > m_atlas_w)
		{
			m_pen_x = 0;
			m_pen_y += m_row_h + 1;
			m_row_h = 0;
		}
		if (m_pen_y + h + 1 > m_atlas_h)
		{
			stbtt_FreeSDF(sdf, nullptr);
			return false; // atlas full
		}
		for (int y = 0; y < h; y++)
			std::memcpy(&m_atlas[static_cast<size_t>(m_pen_y + y) * m_atlas_w + m_pen_x], sdf + y * w, static_cast<size_t>(w));
		g.u0 = static_cast<float>(m_pen_x) / m_atlas_w;
		g.v0 = static_cast<float>(m_pen_y) / m_atlas_h;
		g.u1 = static_cast<float>(m_pen_x + w) / m_atlas_w;
		g.v1 = static_cast<float>(m_pen_y + h) / m_atlas_h;
		g.x0 = static_cast<float>(xoff);
		g.y0 = static_cast<float>(yoff);
		g.x1 = static_cast<float>(xoff + w);
		g.y1 = static_cast<float>(yoff + h);
		m_pen_x += w + 1;
		m_row_h = std::max(m_row_h, h);
	}
	if (sdf)
		stbtt_FreeSDF(sdf, nullptr);
	m_glyphs[cp] = g;
	return true;
}

bool Fonts::Has(const char* utf8) const
{
	const char* p = utf8;
	return m_glyphs.count(NextCodepoint(p)) != 0;
}

const Fonts::Glyph* Fonts::Find(uint32_t cp) const
{
	auto it = m_glyphs.find(cp);
	if (it != m_glyphs.end())
		return &it->second;
	it = m_glyphs.find('?');
	return it != m_glyphs.end() ? &it->second : nullptr;
}

float Fonts::Ascent(float px) const
{
	const FontData& f = *m_fonts[0];
	return f.ascent * f.base_scale * (px / kBasePx);
}

float Fonts::Descent(float px) const
{
	const FontData& f = *m_fonts[0];
	return -f.descent * f.base_scale * (px / kBasePx);
}

float Fonts::Measure(const char* utf8, float px) const
{
	const float s = px / kBasePx;
	float x = 0;
	uint32_t prev = 0;
	for (const char* p = utf8; *p;)
	{
		const uint32_t cp = NextCodepoint(p);
		const Glyph* g = Find(cp);
		if (!g)
			continue;
		if (prev && g->font == 0)
			x += stbtt_GetCodepointKernAdvance(&m_fonts[0]->info, static_cast<int>(prev), static_cast<int>(cp)) *
			     m_fonts[0]->base_scale * s;
		x += g->advance * s;
		prev = g->font == 0 ? cp : 0;
	}
	return x;
}

float Fonts::AddText(std::vector<UiVertex>& out, const char* utf8, float x, float baseline, float px, uint32_t color,
	float weight, Align align) const
{
	const float width = Measure(utf8, px);
	if (align == Center)
		x -= width * 0.5f;
	else if (align == Right)
		x -= width;
	const float s = px / kBasePx;
	// The field spans 128/255 over kSpread base pixels; one screen pixel is kBasePx/px base pixels.
	const float field_per_base_px = (128.0f / kSpread) / 255.0f;
	const float soft = std::max(0.5f * (kBasePx / px) * field_per_base_px, 0.004f);
	const float edge = 128.0f / 255.0f - weight * 0.09f;
	uint32_t prev = 0;
	for (const char* p = utf8; *p;)
	{
		const uint32_t cp = NextCodepoint(p);
		const Glyph* g = Find(cp);
		if (!g)
			continue;
		if (prev && g->font == 0)
			x += stbtt_GetCodepointKernAdvance(&m_fonts[0]->info, static_cast<int>(prev), static_cast<int>(cp)) *
			     m_fonts[0]->base_scale * s;
		if (g->x1 > g->x0)
		{
			const float qx0 = x + g->x0 * s, qy0 = baseline + g->y0 * s;
			const float qx1 = x + g->x1 * s, qy1 = baseline + g->y1 * s;
			const UiVertex a = {qx0, qy0, g->u0, g->v0, color, edge, soft, 0, 0};
			const UiVertex b = {qx1, qy0, g->u1, g->v0, color, edge, soft, 0, 0};
			const UiVertex c = {qx1, qy1, g->u1, g->v1, color, edge, soft, 0, 0};
			const UiVertex d = {qx0, qy1, g->u0, g->v1, color, edge, soft, 0, 0};
			out.insert(out.end(), {a, b, c, a, c, d});
		}
		x += g->advance * s;
		prev = g->font == 0 ? cp : 0;
	}
	return width;
}

void Fonts::AddRoundedRect(std::vector<UiVertex>& out, float x, float y, float w, float h, float radius, uint32_t color)
{
	const float hw = w * 0.5f, hh = h * 0.5f;
	const float r = std::min(radius, std::min(hw, hh));
	const UiVertex a = {x, y, -hw, -hh, color, hw, hh, r, 1};
	const UiVertex b = {x + w, y, hw, -hh, color, hw, hh, r, 1};
	const UiVertex c = {x + w, y + h, hw, hh, color, hw, hh, r, 1};
	const UiVertex d = {x, y + h, -hw, hh, color, hw, hh, r, 1};
	out.insert(out.end(), {a, b, c, a, c, d});
}

void Fonts::Raster(const char* utf8, float px, std::vector<uint8_t>& alpha, int& w, int& h, int& baseline_out) const
{
	const FontData& f = *m_fonts[0];
	const float scale = stbtt_ScaleForMappingEmToPixels(&f.info, px);
	const int ascent = static_cast<int>(std::ceil(f.ascent * scale));
	const int descent = static_cast<int>(std::ceil(-f.descent * scale));
	w = static_cast<int>(std::ceil(Measure(utf8, px))) + 4;
	h = ascent + descent + 2;
	baseline_out = ascent + 1;
	alpha.assign(static_cast<size_t>(w) * h, 0);
	float x = 1.0f;
	uint32_t prev = 0;
	for (const char* p = utf8; *p;)
	{
		const uint32_t cp = NextCodepoint(p);
		const Glyph* g = Find(cp);
		if (!g || g->font != 0)
			continue;
		if (prev)
			x += stbtt_GetCodepointKernAdvance(&f.info, static_cast<int>(prev), static_cast<int>(cp)) * scale;
		int x0, y0, x1, y1;
		const float sub = x - std::floor(x);
		stbtt_GetCodepointBitmapBoxSubpixel(&f.info, static_cast<int>(cp), scale, scale, sub, 0, &x0, &y0, &x1, &y1);
		const int gw = x1 - x0, gh = y1 - y0;
		if (gw > 0 && gh > 0)
		{
			std::vector<uint8_t> tmp(static_cast<size_t>(gw) * gh);
			stbtt_MakeCodepointBitmapSubpixel(&f.info, tmp.data(), gw, gh, gw, scale, scale, sub, 0, static_cast<int>(cp));
			const int ox = static_cast<int>(std::floor(x)) + x0, oy = baseline_out + y0;
			for (int yy = 0; yy < gh; yy++)
				for (int xx = 0; xx < gw; xx++)
				{
					const int tx = ox + xx, ty = oy + yy;
					if (tx < 0 || ty < 0 || tx >= w || ty >= h)
						continue;
					uint8_t& dst = alpha[static_cast<size_t>(ty) * w + tx];
					dst = static_cast<uint8_t>(std::min(255, dst + tmp[static_cast<size_t>(yy) * gw + xx]));
				}
		}
		int advance, lsb;
		stbtt_GetCodepointHMetrics(&f.info, static_cast<int>(cp), &advance, &lsb);
		x += advance * scale;
		prev = cp;
	}
}

// Test build 1 (vk-285-55): see fe_text.h.
void RasterWatermark(const Fonts& fonts, const char* line1, const char* line2, const char* line3, float alpha1,
	float alpha2, std::vector<uint32_t>& rgba, int& w, int& h)
{
	// The text in white; bold made by thickening Roboto Regular's coverage (a small max filter).
	struct Line
	{
		std::vector<uint8_t> a;
		int w = 0, h = 0, base = 0;
	};
	auto raster = [&](const char* text, float px, int bold) {
		Line l;
		if (!text || !*text)
			return l;
		fonts.Raster(text, px, l.a, l.w, l.h, l.base);
		if (bold > 0)
		{
			// Grow the image by `bold` on each side, then spread each row and column by `bold`.
			const int W = l.w + 2 * bold, H = l.h + 2 * bold;
			std::vector<uint8_t> g(static_cast<size_t>(W) * H, 0), t(g.size(), 0);
			for (int y = 0; y < l.h; y++)
				std::memcpy(&g[static_cast<size_t>(y + bold) * W + bold], &l.a[static_cast<size_t>(y) * l.w], static_cast<size_t>(l.w));
			for (int y = 0; y < H; y++)
				for (int x = 0; x < W; x++)
				{
					uint8_t m = 0;
					for (int dx = -bold; dx <= bold; dx++)
						if (x + dx >= 0 && x + dx < W)
							m = std::max(m, g[static_cast<size_t>(y) * W + x + dx]);
					t[static_cast<size_t>(y) * W + x] = m;
				}
			for (int y = 0; y < H; y++)
				for (int x = 0; x < W; x++)
				{
					uint8_t m = 0;
					for (int dy = -bold; dy <= bold; dy++)
						if (y + dy >= 0 && y + dy < H)
							m = std::max(m, t[static_cast<size_t>(y + dy) * W + x]);
					g[static_cast<size_t>(y) * W + x] = m;
				}
			l.a.swap(g);
			l.w = W;
			l.h = H;
			l.base += bold;
		}
		return l;
	};
	// vk-285-105: line3, the Discord note, smaller under the build.
	const Line a = raster(line1, 300.0f, 5), b = raster(line2, 76.0f, 1), c = raster(line3, 48.0f, 1);
	const int shadow = 5, gap = 18, gap3 = 14, pad = 12;
	w = std::max(std::max(a.w, b.w), c.w) + 2 * pad + shadow;
	h = a.h + (b.h ? gap + b.h : 0) + (c.h ? gap3 + c.h : 0) + 2 * pad + shadow;
	// Coverage of the text and of its shadow, then straight-alpha RGBA: white text over black shadow.
	std::vector<float> text(static_cast<size_t>(w) * h, 0.0f), shade(text.size(), 0.0f);
	auto put = [&](const Line& l, int ox, int oy, float alpha, int sh) {
		for (int y = 0; y < l.h; y++)
			for (int x = 0; x < l.w; x++)
			{
				const float c = l.a[static_cast<size_t>(y) * l.w + x] / 255.0f * alpha;
				if (c <= 0.0f)
					continue;
				float& t0 = text[static_cast<size_t>(oy + y) * w + ox + x];
				t0 = std::max(t0, c);
				float& s0 = shade[static_cast<size_t>(oy + y + sh) * w + ox + x + sh];
				s0 = std::max(s0, c * 0.6f);
			}
	};
	put(a, pad + (w - 2 * pad - shadow - a.w) / 2, pad, alpha1, shadow);
	if (b.h)
		put(b, pad + (w - 2 * pad - shadow - b.w) / 2, pad + a.h + gap, alpha2, shadow);
	if (c.h) // a closer shadow for the small line
		put(c, pad + (w - 2 * pad - shadow - c.w) / 2, pad + a.h + (b.h ? gap + b.h : 0) + gap3, alpha2, 3);
	rgba.assign(static_cast<size_t>(w) * h, 0u);
	for (size_t i = 0; i < rgba.size(); i++)
	{
		const float t = text[i], s = shade[i];
		const float out_a = t + s * (1.0f - t);
		if (out_a <= 0.0f)
			continue;
		const uint32_t v = static_cast<uint32_t>(std::lround(255.0f * t / out_a)); // white over black
		const uint32_t a8 = static_cast<uint32_t>(std::lround(255.0f * std::min(out_a, 1.0f)));
		rgba[i] = (a8 << 24) | (v << 16) | (v << 8) | v;
	}
}
} // namespace fe
