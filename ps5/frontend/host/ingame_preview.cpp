// PS5 port frontend, host tool: the in-game menu (fe_ingame.h) drawn on a PC, for its design and its tests. It builds the menu's
// vertices with the real code and rasterizes them on the CPU the way the GS's UI shader draws them
// (bin/resources/shaders/vulkan/orbis_ui.glsl: the same six shapes, bilinear sampling, straight alpha), over a game picture.
// Nothing of this goes into the eboot.
//
//   ingame_preview <fonts dir> <frontend assets dir> <game picture .png/.jpg> <data dir> <out dir> [W H]
//
// Writes <out>/<name>.png for each step of the script below, and open-NN.png frames of the opening.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../fe_i18n.h"
#include "../fe_ingame.h"
#include "../fe_settings.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "../third_party/stb_image.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{
struct Image
{
	int w = 0, h = 0;
	std::vector<float> px; // RGBA 0..1
	bool r8 = false;       // one channel (the atlas): px holds w*h values, sampled as the GS's UNorm8 is: (0, 0, 0, value)
	float At(int x, int y, int c) const
	{
		x = std::clamp(x, 0, w - 1);
		y = std::clamp(y, 0, h - 1);
		return r8 ? px[static_cast<size_t>(y) * w + x] : px[(static_cast<size_t>(y) * w + x) * 4 + c];
	}
	// Bilinear, clamped to the edge (the GS's linear sampler).
	void Sample(float u, float v, float out[4]) const
	{
		const float fx = u * w - 0.5f, fy = v * h - 0.5f;
		const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
		const float tx = fx - x0, ty = fy - y0;
		for (int c = 0; c < (r8 ? 1 : 4); c++)
		{
			const float a = At(x0, y0, c) * (1 - tx) + At(x0 + 1, y0, c) * tx;
			const float b = At(x0, y0 + 1, c) * (1 - tx) + At(x0 + 1, y0 + 1, c) * tx;
			out[c] = a * (1 - ty) + b * ty;
		}
		if (r8)
		{
			// GSTextureVK views a UNorm8 texture with the swizzle (0, 0, 0, R): the value in alpha, red green blue 0
			out[3] = out[0];
			out[0] = out[1] = out[2] = 0.0f;
		}
	}
};

// A made-up 64 x 64 badge for the preview (build 190): a medal in a colour of its own, grey for a locked achievement.
Image MakeBadge(uint32_t id, bool unlocked)
{
	Image img;
	img.w = img.h = 64;
	img.px.resize(64 * 64 * 4);
	const float hue = static_cast<float>((id * 47u) % 360u) / 60.0f;
	const float x = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
	float r = 0, g = 0, b = 0;
	switch (static_cast<int>(hue) % 6)
	{
		case 0: r = 1, g = x; break;
		case 1: r = x, g = 1; break;
		case 2: g = 1, b = x; break;
		case 3: g = x, b = 1; break;
		case 4: r = x, b = 1; break;
		default: r = 1, b = x; break;
	}
	for (int py = 0; py < 64; py++)
		for (int px = 0; px < 64; px++)
		{
			const float dx = px - 31.5f, dy = py - 31.5f, d = std::sqrt(dx * dx + dy * dy);
			float shade = 0.35f + 0.25f * (1.0f - py / 63.0f);       // the background
			if (d < 22.0f)
				shade = 0.95f - 0.012f * d + (dx + dy < 0 ? 0.08f : 0.0f); // the medal
			if (std::fabs(d - 22.0f) < 1.6f)
				shade = 0.25f;                                        // its rim
			if (d < 9.0f && std::fabs(dx) + std::fabs(dy) < 9.0f)
				shade = 1.15f;                                        // the gem
			float c[3] = {r * shade, g * shade, b * shade};
			if (!unlocked)
			{
				const float l = (0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]) * 0.7f;
				c[0] = c[1] = c[2] = l;
			}
			float* o = &img.px[(static_cast<size_t>(py) * 64 + px) * 4];
			o[0] = std::min(c[0], 1.0f);
			o[1] = std::min(c[1], 1.0f);
			o[2] = std::min(c[2], 1.0f);
			o[3] = 1.0f;
		}
	return img;
}

bool LoadImage(const std::string& path, Image& img)
{
	int w, h, n;
	unsigned char* d = stbi_load(path.c_str(), &w, &h, &n, 4);
	if (!d)
		return false;
	img.w = w;
	img.h = h;
	img.px.resize(static_cast<size_t>(w) * h * 4);
	for (size_t i = 0; i < img.px.size(); i++)
		img.px[i] = d[i] / 255.0f;
	stbi_image_free(d);
	return true;
}

Image Crop(const Image& src, int x, int y, int w, int h)
{
	Image o;
	o.w = w;
	o.h = h;
	o.px.resize(static_cast<size_t>(w) * h * 4);
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			for (int c = 0; c < 4; c++)
				o.px[(static_cast<size_t>(j) * w + i) * 4 + c] = src.At(x + i, y + j, c);
	return o;
}

// The GS's blur: the picture made 1/16 the window's size (box-filtered here; the GS's two bilinear steps are close to it), then
// stretched back by bilinear sampling.
Image Shrink(const Image& src, int w, int h)
{
	Image o;
	o.w = w;
	o.h = h;
	o.px.assign(static_cast<size_t>(w) * h * 4, 0.0f);
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
		{
			const int x0 = i * src.w / w, x1 = std::max(x0 + 1, (i + 1) * src.w / w);
			const int y0 = j * src.h / h, y1 = std::max(y0 + 1, (j + 1) * src.h / h);
			float acc[4] = {0, 0, 0, 0};
			for (int y = y0; y < y1; y++)
				for (int x = x0; x < x1; x++)
					for (int c = 0; c < 4; c++)
						acc[c] += src.At(x, y, c);
			const float n = static_cast<float>((x1 - x0) * (y1 - y0));
			for (int c = 0; c < 4; c++)
				o.px[(static_cast<size_t>(j) * w + i) * 4 + c] = acc[c] / n;
		}
	return o;
}

float Smoothstep(float e0, float e1, float x)
{
	const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return t * t * (3 - 2 * t);
}

// The UI shader (orbis_ui.glsl ps_main), for one pixel: uv as interpolated, the texture's sample at the transformed uv.
void Shade(const float uv[2], const float color[4], const float p[4], const float s[4], float out[4])
{
	const float mode = p[3];
	auto rect_d = [&](float qx_in, float qy_in, float hw, float hh, float r) {
		const float qx = std::fabs(qx_in) - hw + r, qy = std::fabs(qy_in) - hh + r;
		const float mx = std::max(qx, 0.0f), my = std::max(qy, 0.0f);
		return std::sqrt(mx * mx + my * my) + std::min(std::max(qx, qy), 0.0f) - r;
	};
	float a = 1.0f;
	out[0] = color[0];
	out[1] = color[1];
	out[2] = color[2];
	if (mode < 0.5f)
		a = Smoothstep(p[0] - p[1], p[0] + p[1], s[3]); // the atlas's value is in alpha (orbis_ui.glsl)
	else if (mode < 1.5f)
		a = 1.0f - Smoothstep(-0.75f, 0.75f, rect_d(uv[0], uv[1], p[0], p[1], p[2]));
	else if (mode < 2.5f)
	{
		for (int c = 0; c < 3; c++)
			out[c] = s[c] * color[c];
		out[3] = s[3] * color[3];
		return;
	}
	else if (mode < 3.5f)
	{
		const float soft = std::max((mode - 3.0f) * 4096.0f, 1.0f);
		const float d = rect_d(uv[0], uv[1], p[0], p[1], p[2]);
		const float t = std::clamp(d / soft, 0.0f, 1.0f);
		a = (1.0f - t) * (1.0f - t);
	}
	else if (mode < 4.5f)
	{
		const float px = (uv[0] - 0.5f) * 2.0f * p[0], py = (uv[1] - 0.5f) * 2.0f * p[1];
		a = 1.0f - Smoothstep(-0.75f, 0.75f, rect_d(px, py, p[0], p[1], p[2]));
		for (int c = 0; c < 3; c++)
			out[c] = s[c] * color[c];
	}
	else
	{
		const float t = (mode - 5.0f) * 4096.0f;
		const float d = rect_d(uv[0], uv[1], p[0], p[1], p[2]);
		a = 1.0f - Smoothstep(-0.75f, 0.75f, std::fabs(d + t * 0.5f) - t * 0.5f);
	}
	out[3] = color[3] * a;
}

struct Raster
{
	int W, H;
	std::vector<float> rgb;
	Raster(int w, int h) : W(w), H(h), rgb(static_cast<size_t>(w) * h * 3, 0.0f) {}

	void Draw(const fe::InGameFrame& f, const std::map<int, const Image*>& images)
	{
		for (const fe::InGameRange& r : f.ranges)
		{
			const auto it = images.find(r.image);
			const Image* tex = it != images.end() ? it->second : nullptr;
			for (uint32_t q = r.first; q + 5 < r.first + r.count + 0u; q += 6)
				Quad(&f.ui[q], tex);
		}
	}

	// The six vertices a, b, c, a, c, d of an axis-aligned quad (every shape the menu makes is one).
	void Quad(const fe::UiVertex* v, const Image* tex)
	{
		const fe::UiVertex& a = v[0];
		const fe::UiVertex& b = v[1];
		const fe::UiVertex& c = v[2];
		const fe::UiVertex& d = v[5];
		const float x0 = a.x, y0 = a.y, x1 = c.x, y1 = c.y;
		if (!(x1 > x0) || !(y1 > y0))
			return;
		const int ix0 = std::max(0, static_cast<int>(std::ceil(x0 - 0.5f))), ix1 = std::min(W, static_cast<int>(std::ceil(x1 - 0.5f)));
		const int iy0 = std::max(0, static_cast<int>(std::ceil(y0 - 0.5f))), iy1 = std::min(H, static_cast<int>(std::ceil(y1 - 0.5f)));
		auto unpack = [](uint32_t col, float o[4]) {
			for (int i = 0; i < 4; i++)
				o[i] = ((col >> (8 * i)) & 0xFF) / 255.0f;
		};
		float ca[4], cb[4], cc[4], cd[4];
		unpack(a.color, ca);
		unpack(b.color, cb);
		unpack(c.color, cc);
		unpack(d.color, cd);
		const float p[4] = {a.p0, a.p1, a.p2, a.p3};
		for (int y = iy0; y < iy1; y++)
		{
			const float t = (y + 0.5f - y0) / (y1 - y0);
			for (int x = ix0; x < ix1; x++)
			{
				const float s = (x + 0.5f - x0) / (x1 - x0);
				float uv[2], col[4];
				// uv: a (top-left), b (top-right), c (bottom-right), d (bottom-left)
				const float top_u = a.u + (b.u - a.u) * s, bot_u = d.u + (c.u - d.u) * s;
				const float top_v = a.v + (b.v - a.v) * s, bot_v = d.v + (c.v - d.v) * s;
				uv[0] = top_u + (bot_u - top_u) * t;
				uv[1] = top_v + (bot_v - top_v) * t;
				for (int i = 0; i < 4; i++)
				{
					const float top = ca[i] + (cb[i] - ca[i]) * s, bot = cd[i] + (cc[i] - cd[i]) * s;
					col[i] = top + (bot - top) * t;
				}
				float smp[4] = {0, 0, 0, 1};
				if (tex)
					tex->Sample(uv[0], uv[1], smp);
				float o[4];
				Shade(uv, col, p, smp, o);
				const float al = std::clamp(o[3], 0.0f, 1.0f);
				float* dst = &rgb[(static_cast<size_t>(y) * W + x) * 3];
				for (int i = 0; i < 3; i++)
					dst[i] = o[i] * al + dst[i] * (1 - al);
			}
		}
	}
};

void PutBe32(std::vector<uint8_t>& v, uint32_t x)
{
	v.push_back(static_cast<uint8_t>(x >> 24));
	v.push_back(static_cast<uint8_t>(x >> 16));
	v.push_back(static_cast<uint8_t>(x >> 8));
	v.push_back(static_cast<uint8_t>(x));
}

void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data)
{
	PutBe32(png, static_cast<uint32_t>(data.size()));
	const size_t start = png.size();
	png.insert(png.end(), type, type + 4);
	png.insert(png.end(), data.begin(), data.end());
	PutBe32(png, static_cast<uint32_t>(crc32(0, png.data() + start, static_cast<uInt>(png.size() - start))));
}

bool WritePng(const std::string& path, const Raster& r)
{
	std::vector<uint8_t> raw;
	raw.reserve(static_cast<size_t>(r.W * 3 + 1) * r.H);
	for (int y = 0; y < r.H; y++)
	{
		raw.push_back(0);
		for (int x = 0; x < r.W * 3; x++)
			raw.push_back(static_cast<uint8_t>(std::clamp(r.rgb[static_cast<size_t>(y) * r.W * 3 + x], 0.0f, 1.0f) * 255.0f + 0.5f));
	}
	uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
	std::vector<uint8_t> z(zlen);
	if (compress2(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
		return false;
	z.resize(zlen);
	std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	std::vector<uint8_t> ihdr;
	PutBe32(ihdr, static_cast<uint32_t>(r.W));
	PutBe32(ihdr, static_cast<uint32_t>(r.H));
	ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
	Chunk(png, "IHDR", ihdr);
	Chunk(png, "IDAT", z);
	Chunk(png, "IEND", {});
	std::ofstream o(path, std::ios::binary);
	o.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
	return o.good();
}

std::vector<uint8_t> ReadAll(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void WriteText(const std::string& path, const std::string& text)
{
	std::ofstream(path, std::ios::binary) << text;
}

Image SlotShot(const Image& game, float fx, float fy, float fw)
{
	const int w = static_cast<int>(game.w * fw), h = w * 9 / 16;
	return Crop(game, static_cast<int>(game.w * fx), static_cast<int>(game.h * fy), w, std::min(h, game.h));
}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 6)
	{
		std::fprintf(stderr, "usage: %s <fonts dir> <assets dir> <game picture> <data dir> <out dir> [W H]\n", argv[0]);
		return 2;
	}
	const std::string fonts_dir = argv[1], assets = argv[2], picture = argv[3], data = argv[4], out = argv[5];
	const int W = argc > 7 ? std::atoi(argv[6]) : 1920, H = argc > 7 ? std::atoi(argv[7]) : 1080;
	mkdir(out.c_str(), 0755);

	const std::vector<uint8_t> text_font = ReadAll(fonts_dir + "/Roboto-Regular.ttf"), prompt = ReadAll(fonts_dir + "/promptfont.otf");
	const std::vector<uint8_t> brands = ReadAll(assets + "/fonts/fa-brands-400.otf"), solid = ReadAll(fonts_dir + "/fa-solid-900.ttf");
	fe::Fonts fonts;
	if (text_font.empty() || prompt.empty() || !fonts.Init(text_font.data(), text_font.size(), prompt.data(), prompt.size(),
												 brands.empty() ? nullptr : brands.data(), brands.size()))
	{
		std::fprintf(stderr, "fonts failed\n");
		return 1;
	}
	const int icon_font = fonts.AddFont(solid.empty() ? nullptr : solid.data());
	const int icons = fonts.Preload(icon_font, fe::InGameMenu::IconCodepoints());
	std::printf("icon font %d, %d icons of %zu\n", icon_font, icons, fe::InGameMenu::IconCodepoints().size());

	Image game;
	if (!LoadImage(picture, game))
	{
		std::fprintf(stderr, "can't read %s\n", picture.c_str());
		return 1;
	}
	if (game.w >= 3 * game.h) // a strip of three captures side by side: the first one
		game = Crop(game, 0, 0, game.w / 3, game.h);
	const Image blur = Shrink(game, std::max(8, W / 16), std::max(8, H / 16));
	Image atlas;
	atlas.w = fonts.AtlasWidth();
	atlas.h = fonts.AtlasHeight();
	atlas.r8 = true;
	atlas.px.resize(fonts.AtlasPixels().size());
	for (size_t i = 0; i < atlas.px.size(); i++)
		atlas.px[i] = fonts.AtlasPixels()[i] / 255.0f;

	// A game's files, as the shelf and the page keep them.
	const std::string stem = "Grand Theft Auto - San Andreas (USA)";
	mkdir(data.c_str(), 0755);
	mkdir((data + "/settings").c_str(), 0755);
	mkdir((data + "/patches").c_str(), 0755);
	mkdir((data + "/cheats").c_str(), 0755);
	mkdir((data + "/memcards").c_str(), 0755);
	WriteText(data + "/settings/" + stem + ".ini", "upscale_multiplier=4\nAspectRatio=16:9\nEmuCore/EnableWideScreenPatches=true\n");
	WriteText(data + "/gs.ini", "TVShader=6\nMaxAnisotropy=16\n");
	fe::OptionsPaths paths;
	paths.settings_dir = data + "/settings";
	paths.gs_ini = data + "/gs.ini";
	paths.patches_dir = data + "/patches";
	paths.cheats_dir = data + "/cheats";
	paths.memcards_dir = data + "/memcards";
	{
		const std::vector<uint8_t> p = ReadAll(assets + "/presets.ini");
		paths.presets.assign(p.begin(), p.end());
	}

	fe::GameAchievementsState ach;
	for (int i = 0; i < 48; i++)
	{
		fe::GameAchievement a;
		a.id = static_cast<uint32_t>(i);
		static const char* names[] = {"Welcome to Los Santos", "Grove Street 4 Life", "Lowrider Champion", "Wheels of Fortune",
			"Big Smoke's Order", "Taking Flight", "Seventh Heaven", "Fresh Prince"};
		static const char* descs[] = {"Finish the first mission", "Take back a gang territory", "Win the lowrider meet",
			"Win a race in every class", "Order the whole meal", "Pass the flight school", "Reach seven gym sessions in a day",
			"Buy every outfit in one shop"};
		a.title = names[i % 8];
		a.description = descs[i % 8];
		a.points = static_cast<uint32_t>(i % 5 == 0 ? 25 : i % 3 == 0 ? 10 : 5);
		a.unlocked = i < 12;
		ach.entries.push_back(a);
	}

	fe::InGameMenu menu;
	fe::InGameMenu::Config cfg;
	cfg.fonts = &fonts;
	cfg.icon_font = icon_font;
	cfg.paths = paths;
	cfg.achievements = [&]() { return ach; };
	cfg.clock = []() { return std::string("21:47"); };
	constexpr int64_t kNow = 1793914020; // 2026-11-05 21:27 UTC: the preview's "now"
	cfg.when = [](int64_t utc) {
		const std::time_t tt = static_cast<std::time_t>(utc);
		const std::tm tm = *std::gmtime(&tt);
		char hm[16], b[64];
		std::snprintf(hm, sizeof(hm), "%02d:%02d", tm.tm_hour, tm.tm_min);
		std::snprintf(b, sizeof(b), fe::Tr(kNow - utc < 86400 ? fe::Str::IngameToday : fe::Str::IngameYesterday), hm);
		return std::string(b);
	};
	menu.Configure(cfg);

	fe::InGameSession s;
	s.game.title = "Grand Theft Auto: San Andreas";
	s.game.serial = "SLUS-20946";
	s.game.region = "USA";
	s.game.stem = stem;
	s.game.file = stem + ".iso";
	s.game.path = data + "/" + s.game.file;
	s.has_game = true;
	s.slots.resize(10);
	std::vector<Image> shots;
	shots.push_back(SlotShot(game, 0.0f, 0.0f, 1.0f));
	shots.push_back(SlotShot(game, 0.25f, 0.28f, 0.55f));
	for (int i = 0; i < 2; i++)
	{
		s.slots[static_cast<size_t>(i)].used = true;
		s.slots[static_cast<size_t>(i)].mtime = kNow - (i == 0 ? 1200 : 4600);
		const Image& sh = shots[static_cast<size_t>(i)];
		s.slots[static_cast<size_t>(i)].shot_w = sh.w;
		s.slots[static_cast<size_t>(i)].shot_h = sh.h;
	}
	s.fast_forward = false;
	s.web_address = "192.168.1.20:8844";
	{
		// A stand-in QR pattern of the right size (the real one comes from the web server's address, qrcodegen).
		s.qr_size = 25;
		s.qr.assign(25 * 25, 0);
		unsigned seed = 7;
		for (int y = 0; y < 25; y++)
			for (int x = 0; x < 25; x++)
			{
				seed = seed * 1103515245u + 12345u;
				const bool finder = (x < 7 && y < 7) || (x >= 18 && y < 7) || (x < 7 && y >= 18);
				bool on = (seed >> 16) & 1;
				if (finder)
				{
					const int fx = x >= 18 ? x - 18 : x, fy = y >= 18 ? y - 18 : y;
					on = fx == 0 || fx == 6 || fy == 0 || fy == 6 || (fx >= 2 && fx <= 4 && fy >= 2 && fy <= 4);
				}
				if ((x == 7 || y == 7) && (x < 8 && y < 8))
					on = false;
				s.qr[static_cast<size_t>(y) * 25 + x] = on ? 1 : 0;
			}
	}
	s.chips = {"4x", "16:9", "60 FPS"};
	s.played_s = 72 * 60;

	std::map<int, const Image*> images = {{static_cast<int>(fe::InGameImage::Atlas), &atlas},
		{static_cast<int>(fe::InGameImage::Blur), &blur}, {static_cast<int>(fe::InGameImage::Frame), &game}};
	for (size_t i = 0; i < shots.size(); i++)
		images[static_cast<int>(fe::InGameImage::Slot0) + static_cast<int>(i)] = &shots[i];
	// Build 190: the badges, made up (a coloured medal; the grey one for a locked achievement), handed over as the console's loop
	// does (WantedBadges, then BadgeDone) a few a frame.
	std::vector<Image> badges(fe::kInGameBadges);
	auto feed_badges = [&](double now) {
		for (const fe::InGameBadgeWant& w : menu.WantedBadges(now))
		{
			badges[static_cast<size_t>(w.index)] = MakeBadge(w.id, w.unlocked);
			images[static_cast<int>(fe::InGameImage::Slot0) + fe::kInGameSaveSlots + w.index] = &badges[static_cast<size_t>(w.index)];
			menu.BadgeDone(w, true, false, now);
		}
	};

	// Where the present draws the game without the menu: the whole screen for a 16:9 game.
	const float frame[4] = {0, 0, static_cast<float>(W), static_cast<float>(H)};
	auto shot = [&](const std::string& name) {
		fe::InGameFrame f;
		menu.Build(f, static_cast<float>(W), static_cast<float>(H), frame);
		Raster r(W, H);
		r.Draw(f, images);
		const std::string path = out + "/" + name + ".png";
		std::printf("%s: %zu vertices in %zu ranges -> %s\n", name.c_str(), f.ui.size(), f.ranges.size(), WritePng(path, r) ? path.c_str() : "FAILED");
	};

	double t = 100.0;
	// One frame, as the console's loop runs it: Update, then Build (the highlight's glide reads where Build put the row).
	auto step = [&](double dt, uint32_t held, uint32_t pressed = 0) {
		t += dt;
		fe::InGamePad p;
		p.held = held;
		p.pressed = pressed;
		menu.Update(t, p);
		fe::InGameFrame unused;
		menu.Build(unused, static_cast<float>(W), static_cast<float>(H), frame);
		feed_badges(t);
	};
	auto press = [&](uint32_t b) {
		step(1.0 / 60, b, b);
		step(1.0 / 60, 0);
		for (int i = 0; i < 20; i++)
			step(1.0 / 60, 0); // settle the glides
	};
	using namespace fe::ingame_pad;

	// The opening, frame by frame (L2 + D-pad down still held), then the Game tab.
	menu.SetSession(s);
	menu.Open(t, L2 | Down);
	for (int i = 0; i <= 24; i++)
	{
		char name[32];
		std::snprintf(name, sizeof(name), "open-%02d", i);
		shot(name);
		step(1.0 / 60, i < 16 ? (L2 | Down) : 0);
	}
	for (int i = 0; i < 30; i++)
		step(1.0 / 60, 0);
	shot("game-resume");
	press(Down);
	shot("game-save");
	press(Right);
	shot("game-save-slot4");
	press(Down);
	shot("game-load");
	press(Down);
	press(Down);
	shot("game-phone");
	press(Down);
	shot("game-report");
	press(Down);
	press(Cross);
	shot("game-shelf-armed");
	press(R2);
	shot("settings");
	press(Down);
	press(Right);
	shot("settings-changed");
	press(R1);
	shot("settings-all-games");
	press(R2);
	shot("controls");
	press(R2);
	shot("achievements");
	for (int i = 0; i < 5; i++)
		press(Down);
	shot("achievements-scrolled");

	// The hold ring in the game.
	for (float p : {0.15f, 0.5f, 0.9f})
	{
		fe::InGameFrame f;
		fe::InGameMenu::BuildHold(fonts, f, static_cast<float>(W), static_cast<float>(H), p);
		Raster r(W, H);
		// The game behind it, as the present draws it.
		fe::InGameFrame bg;
		{
			const fe::UiVertex a = {0, 0, 0, 0, 0xFFFFFFFFu, W * 0.5f, H * 0.5f, 0, fe::ui_mode::RoundImage};
			const fe::UiVertex b = {static_cast<float>(W), 0, 1, 0, 0xFFFFFFFFu, W * 0.5f, H * 0.5f, 0, fe::ui_mode::RoundImage};
			const fe::UiVertex c = {static_cast<float>(W), static_cast<float>(H), 1, 1, 0xFFFFFFFFu, W * 0.5f, H * 0.5f, 0, fe::ui_mode::RoundImage};
			const fe::UiVertex d = {0, static_cast<float>(H), 0, 1, 0xFFFFFFFFu, W * 0.5f, H * 0.5f, 0, fe::ui_mode::RoundImage};
			bg.ui = {a, b, c, a, c, d};
			bg.ranges = {{0, 6, static_cast<int>(fe::InGameImage::Frame)}};
		}
		r.Draw(bg, images);
		r.Draw(f, images);
		char name[32];
		std::snprintf(name, sizeof(name), "hold-%02d", static_cast<int>(p * 100));
		std::printf("%s: %s\n", name, WritePng(out + "/" + name + ".png", r) ? "ok" : "FAILED");
	}

	// Closing.
	press(Circle);
	int closing_frames = 0;
	while (menu.Active() && closing_frames < 60)
	{
		step(1.0 / 60, 0);
		closing_frames++;
	}
	std::printf("closed after %d frames: %s\n", closing_frames, menu.Closed() ? "yes" : "NO");
	if (!menu.Closed())
		return 1;

	// FE_TOUR=1: a session as a player would have it, every frame at 60 Hz (tour-NNNN.png), for a video of the menu in use.
	if (const char* tour = std::getenv("FE_TOUR"); tour && *tour == '1')
	{
		int n = 0, tick = 0;
		const char* every_env = std::getenv("FE_TOUR_EVERY");
		const int every = every_env ? std::max(1, std::atoi(every_env)) : 4; // a picture every this many 60 Hz frames
		auto frame_out = [&]() {
			if (tick++ % every != 0)
				return;
			fe::InGameFrame f;
			menu.Build(f, static_cast<float>(W), static_cast<float>(H), frame);
			Raster r(W, H);
			if (menu.Active())
				r.Draw(f, images);
			else
			{
				fe::InGameFrame bg;
				const fe::UiVertex a = {0, 0, 0, 0, 0xFFFFFFFFu, W * 0.5f, H * 0.5f, 0, fe::ui_mode::RoundImage};
				const fe::UiVertex b = {static_cast<float>(W), 0, 1, 0, 0xFFFFFFFFu, W * 0.5f, H * 0.5f, 0, fe::ui_mode::RoundImage};
				const fe::UiVertex c = {static_cast<float>(W), static_cast<float>(H), 1, 1, 0xFFFFFFFFu, W * 0.5f, H * 0.5f, 0, fe::ui_mode::RoundImage};
				const fe::UiVertex d = {0, static_cast<float>(H), 0, 1, 0xFFFFFFFFu, W * 0.5f, H * 0.5f, 0, fe::ui_mode::RoundImage};
				bg.ui = {a, b, c, a, c, d};
				bg.ranges = {{0, 6, static_cast<int>(fe::InGameImage::Frame)}};
				r.Draw(bg, images);
			}
			char name[64];
			std::snprintf(name, sizeof(name), "%s/tour-%04d.png", out.c_str(), n++);
			WritePng(name, r);
		};
		auto run = [&](double seconds, uint32_t held, uint32_t pressed_first = 0) {
			const int frames = static_cast<int>(seconds * 60.0 + 0.5);
			for (int i = 0; i < frames; i++)
			{
				step(1.0 / 60, held, i == 0 ? pressed_first : 0);
				frame_out();
			}
		};
		auto tap = [&](uint32_t b, double after) {
			run(1.0 / 60, b, b);
			run(after, 0);
		};
		// The game, then the combo held: the ring fills (the game's own present draws it), then the menu opens.
		menu.SetSession(s);
		for (int i = 0; i < 30; i++)
			frame_out();
		for (int i = 0; i <= 60; i++)
		{
			if (tick++ % every != 0)
				continue;
			fe::InGameFrame f, ring;
			fe::InGameMenu::BuildHold(fonts, ring, static_cast<float>(W), static_cast<float>(H), i / 60.0f);
			Raster r(W, H);
			fe::InGameFrame bg;
			const fe::UiVertex a = {0, 0, 0, 0, 0xFFFFFFFFu, W * 0.5f, H * 0.5f, 0, fe::ui_mode::RoundImage};
			const fe::UiVertex b = {static_cast<float>(W), 0, 1, 0, 0xFFFFFFFFu, W * 0.5f, H * 0.5f, 0, fe::ui_mode::RoundImage};
			const fe::UiVertex c = {static_cast<float>(W), static_cast<float>(H), 1, 1, 0xFFFFFFFFu, W * 0.5f, H * 0.5f, 0, fe::ui_mode::RoundImage};
			const fe::UiVertex d = {0, static_cast<float>(H), 0, 1, 0xFFFFFFFFu, W * 0.5f, H * 0.5f, 0, fe::ui_mode::RoundImage};
			bg.ui = {a, b, c, a, c, d};
			bg.ranges = {{0, 6, static_cast<int>(fe::InGameImage::Frame)}};
			r.Draw(bg, images);
			r.Draw(ring, images);
			char name[64];
			std::snprintf(name, sizeof(name), "%s/tour-%04d.png", out.c_str(), n++);
			WritePng(name, r);
		}
		menu.Open(t, L2 | Down);
		run(0.25, L2 | Down);
		run(0.9, 0);
		tap(Down, 0.7);  // Save state
		tap(Right, 0.6); // slot 4
		tap(Left, 0.7);  // slot 3
		tap(Down, 0.7);  // Load state
		tap(Down, 0.4);  // Fast forward
		tap(Down, 0.4);  // Settings on your phone
		run(0.8, 0);
		tap(R2, 0.9);    // Settings
		tap(Right, 0.5); // Resolution 5x
		tap(Down, 0.4);
		tap(Down, 0.6);
		tap(R1, 0.9);    // all games
		tap(R2, 0.9);    // Controls
		tap(R2, 0.8);    // Achievements
		tap(Down, 0.25);
		tap(Down, 0.25);
		tap(Down, 0.6);
		tap(L2, 0.3);
		tap(L2, 0.3);
		tap(L2, 0.8);    // back to the Game tab
		tap(Circle, 0.6); // resume: the card grows back into the game
		std::printf("tour: %d frames\n", n);
	}
	return 0;
}
