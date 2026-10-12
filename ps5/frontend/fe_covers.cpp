// PS5 port frontend: covers (see fe_covers.h).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_covers.h"
#include "fe_i18n.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#include "third_party/stb_image_resize2.h"

namespace fe
{
namespace
{
bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out.clear();
	uint8_t buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
	{
		out.insert(out.end(), buf, buf + n);
		if (out.size() > (32u << 20))
			break;
	}
	std::fclose(f);
	return !out.empty();
}

bool WriteFileAtomic(const std::string& path, const std::vector<uint8_t>& data)
{
	const std::string tmp = path + ".part";
	FILE* f = std::fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
	std::fclose(f);
	if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0)
	{
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

void MakeDirs(const std::string& path)
{
	std::string cur;
	for (size_t i = 0; i < path.size(); i++)
	{
		cur += path[i];
		if ((path[i] == '/' && i > 0) || i + 1 == path.size())
			mkdir(cur.c_str(), 0777);
	}
}

// vk-285-110: for CoverFinder.
constexpr const char* kImageExts[] = {".jpg", ".png", ".jpeg"};

std::string LowerAscii(std::string s)
{
	for (char& c : s)
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	return s;
}

// A file with something in it (not a folder, not an empty file from a copy that failed).
bool IsCoverFile(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

// A 404 from the cover server in the last two weeks (<serial>.missing in the cache).
bool RecentlyMissing(const std::string& cache_dir, const std::string& serial, bool lb_candidate = false)
{
	struct stat st = {};
	const std::string path = cache_dir + "/" + serial + ".missing";
	if (stat(path.c_str(), &st) != 0 || std::time(nullptr) - st.st_mtime >= 14 * 24 * 3600)
		return false;
	if (lb_candidate)
	{
		// 2.11 (review): a 404 remembered by 2.10 only says the old source hasn't the cover; LaunchBox was not asked. The marks made since say "lb".
		char buf[16] = {};
		if (FILE* f = std::fopen(path.c_str(), "rb"))
		{
			const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
			std::fclose(f);
			if (n == 0 || std::strstr(buf, "lb") == nullptr)
				return false;
		}
	}
	return true;
}

// The glow colour: the dominant saturated hue of the cover, brightened.
bool GlowFrom(const CoverImage& img, float out[3])
{
	constexpr int kBins = 36;
	double weight[kBins] = {}, sr[kBins] = {}, sg[kBins] = {}, sb[kBins] = {};
	double total = 0;
	const int step = 3;
	for (int y = 0; y < img.height; y += step)
		for (int x = 0; x < img.width; x += step)
		{
			const uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
			const float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
			const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
			if (mx < 0.18f)
				continue;
			const float s = (mx - mn) / mx;
			if (s < 0.2f)
				continue;
			float h;
			if (mx == r)
				h = std::fmod((g - b) / (mx - mn), 6.0f);
			else if (mx == g)
				h = (b - r) / (mx - mn) + 2.0f;
			else
				h = (r - g) / (mx - mn) + 4.0f;
			if (h < 0)
				h += 6.0f;
			const int bin = std::min(kBins - 1, static_cast<int>(h / 6.0f * kBins));
			const double w = s * s * mx;
			weight[bin] += w;
			sr[bin] += r * w;
			sg[bin] += g * w;
			sb[bin] += b * w;
			total += w;
		}
	if (total <= 0)
		return false;
	int best = 0;
	double best_w = -1;
	for (int i = 0; i < kBins; i++)
	{
		const double w = weight[i] + 0.5 * (weight[(i + 1) % kBins] + weight[(i + kBins - 1) % kBins]);
		if (w > best_w)
		{
			best_w = w;
			best = i;
		}
	}
	if (weight[best] <= 0 || best_w < total * 0.08)
		return false;
	float r = static_cast<float>(sr[best] / weight[best]), g = static_cast<float>(sg[best] / weight[best]),
		  b = static_cast<float>(sb[best] / weight[best]);
	// Push it towards a luminous, fairly saturated version of itself.
	const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
	const float s = mx > 0 ? (mx - mn) / mx : 0;
	const float want_s = std::min(0.82f, std::max(0.5f, s * 1.1f));
	const float scale = 0.95f / std::max(mx, 1e-3f);
	r *= scale;
	g *= scale;
	b *= scale;
	const float v = 0.95f;
	auto resat = [&](float c) { return v - (v - c) * (want_s / std::max(s, 1e-3f)); };
	out[0] = std::min(1.0f, std::max(0.0f, resat(r)));
	out[1] = std::min(1.0f, std::max(0.0f, resat(g)));
	out[2] = std::min(1.0f, std::max(0.0f, resat(b)));
	return true;
}

void Fill(CoverImage& img, int w, int h)
{
	img.width = w;
	img.height = h;
	img.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
}

void Blend(CoverImage& img, int x, int y, uint32_t rgb, float a)
{
	if (x < 0 || y < 0 || x >= img.width || y >= img.height || a <= 0)
		return;
	uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
	const float c[3] = {static_cast<float>(rgb & 0xff), static_cast<float>((rgb >> 8) & 0xff),
		static_cast<float>((rgb >> 16) & 0xff)};
	for (int i = 0; i < 3; i++)
		p[i] = static_cast<uint8_t>(p[i] + (c[i] - p[i]) * a + 0.5f);
	p[3] = 255;
}

// Draws raster text `alpha` (w x h) at (x, y), upright or turned a quarter clockwise (reading down,
// the letters' tops towards +x).
void DrawAlpha(CoverImage& img, const std::vector<uint8_t>& alpha, int w, int h, int x, int y, uint32_t rgb, bool turned)
{
	for (int sy = 0; sy < h; sy++)
		for (int sx = 0; sx < w; sx++)
		{
			const float a = alpha[static_cast<size_t>(sy) * w + sx] / 255.0f;
			if (a <= 0)
				continue;
			if (turned)
				Blend(img, x + (h - 1 - sy), y + sx, rgb, a);
			else
				Blend(img, x + sx, y + sy, rgb, a);
		}
}

std::vector<std::string> Wrap(const Fonts& fonts, const std::string& text, float px, float max_w, size_t max_lines)
{
	std::vector<std::string> words;
	std::string cur;
	for (char c : text)
	{
		if (c == ' ')
		{
			if (!cur.empty())
				words.push_back(cur);
			cur.clear();
		}
		else
			cur += c;
	}
	if (!cur.empty())
		words.push_back(cur);
	std::vector<std::string> lines;
	std::string line;
	for (const std::string& w : words)
	{
		const std::string trial = line.empty() ? w : line + " " + w;
		if (!line.empty() && fonts.Measure(trial.c_str(), px) > max_w)
		{
			lines.push_back(line);
			line = w;
		}
		else
			line = trial;
	}
	if (!line.empty())
		lines.push_back(line);
	if (lines.size() > max_lines)
	{
		lines.resize(max_lines);
		lines.back() += "\xE2\x80\xA6";
	}
	return lines;
}
} // namespace

// ---- vk-285-110: covers on USB drives ----

std::string UsbDriveRoot(const std::string& path)
{
	// "/mnt/usb<n>/..." (fe_ps5.cpp lists /mnt/usb0 to /mnt/usb7), 2026-10-08: and "/mnt/ext<n>/..." (extended storage, M.2)
	if (path.compare(0, 8, "/mnt/usb") != 0 && path.compare(0, 8, "/mnt/ext") != 0)
		return {};
	size_t i = 8;
	while (i < path.size() && path[i] >= '0' && path[i] <= '9')
		i++;
	if (i == 8 || i >= path.size() || path[i] != '/')
		return {};
	return path.substr(0, i);
}

namespace
{
std::mutex g_share_roots_lock;
std::vector<std::string> g_share_roots;
} // namespace

bool OnNetworkShare(const std::string& path)
{
	return path.compare(0, 5, "/nfs/") == 0 || path.compare(0, 5, "/smb/") == 0;
}

void SetShareRoots(const std::vector<std::string>& roots)
{
	std::vector<std::string> keep;
	for (std::string r : roots)
	{
		while (r.size() > 1 && r.back() == '/')
			r.pop_back();
		if (OnNetworkShare(r) && std::find(keep.begin(), keep.end(), r) == keep.end())
			keep.push_back(r);
	}
	std::lock_guard<std::mutex> lock(g_share_roots_lock);
	g_share_roots = std::move(keep);
}

std::string DriveRoot(const std::string& path)
{
	std::string root = UsbDriveRoot(path);
	if (!root.empty() || !OnNetworkShare(path))
		return root;
	std::lock_guard<std::mutex> lock(g_share_roots_lock);
	for (const std::string& r : g_share_roots)
		if (r.size() > root.size() && path.size() > r.size() && path.compare(0, r.size(), r) == 0 && path[r.size()] == '/')
			root = r;
	return root;
}

std::string OplGameId(const std::string& serial)
{
	// SYSTEM.CNF's "SLUS_213.51" is the serial "SLUS-21351" (fe_games.cpp); OPL names ART files after the former.
	if (serial.size() != 10 || serial[4] != '-')
		return {};
	for (size_t i = 0; i < 4; i++)
		if (serial[i] < 'A' || serial[i] > 'Z')
			return {};
	for (size_t i = 5; i < 10; i++)
		if (serial[i] < '0' || serial[i] > '9')
			return {};
	return serial.substr(0, 4) + "_" + serial.substr(5, 3) + "." + serial.substr(8, 2);
}

CoverFinder::CoverFinder(std::string manual_dir, std::string cache_dir)
	: m_manual_dir(std::move(manual_dir))
	, m_cache_dir(std::move(cache_dir))
{
}

// The folder's names, listed on first use. A folder that can't be opened lists as empty.
const CoverFinder::Names& CoverFinder::List(const std::string& dir)
{
	const auto it = m_lists.find(dir);
	if (it != m_lists.end())
		return it->second;
	Names& names = m_lists[dir]; // references into an unordered_map survive later insertions
	if (DIR* d = opendir(dir.c_str()))
	{
		while (const dirent* e = readdir(d))
			if (e->d_name[0] != '.')
				names.emplace(LowerAscii(e->d_name), e->d_name);
		closedir(d);
	}
	return names;
}

// `dir`'s entry named `lower_name` in any letter case, as a path, or "".
std::string CoverFinder::Subdir(const std::string& dir, const char* lower_name)
{
	if (dir.empty())
		return {};
	const Names& names = List(dir);
	const auto it = names.find(lower_name);
	return it == names.end() ? std::string() : dir + "/" + it->second;
}

// Adds `dir`/<base>.jpg/.png/.jpeg, in any letter case, when it's there.
bool CoverFinder::Lookup(const std::string& dir, const std::string& base, const char* source, std::vector<CoverFile>& out)
{
	if (dir.empty() || base.empty())
		return false;
	const Names& names = List(dir);
	if (names.empty())
		return false;
	const std::string lower = LowerAscii(base);
	for (const char* ext : kImageExts)
	{
		const auto it = names.find(lower + ext);
		if (it != names.end() && IsCoverFile(dir + "/" + it->second))
		{
			out.push_back({dir + "/" + it->second, source});
			return true;
		}
	}
	return false;
}

std::vector<CoverFile> CoverFinder::Find(const GameInfo& g)
{
	return Search(g, false);
}

CoverFile CoverFinder::Best(const GameInfo& g)
{
	std::vector<CoverFile> first = Search(g, true);
	return first.empty() ? CoverFile() : std::move(first.front());
}

std::vector<CoverFile> CoverFinder::Search(const GameInfo& g, bool first_only)
{
	std::vector<CoverFile> out;
	auto done = [&] { return first_only && !out.empty(); };
	// The names a cover may have in a covers folder: the serial, the image's name, the title.
	std::vector<std::string> names;
	for (const std::string& n : {g.serial, g.stem, g.title})
		if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end())
			names.push_back(n);
	// 1. The covers folder, exact names (as before vk-285-110).
	if (!m_manual_dir.empty())
		for (const std::string& n : names)
			for (const char* ext : kImageExts)
				if (!done() && IsCoverFile(m_manual_dir + "/" + n + ext))
					out.push_back({m_manual_dir + "/" + n + ext, "manual"});
	if (done())
		return out;
	// 2. Beside the disc image, under its name. (A game from cache/usb-games.txt has no path.)
	const size_t slash = g.path.rfind('/');
	if (slash != std::string::npos && slash > 0 && Lookup(g.path.substr(0, slash), g.stem, "beside", out) && first_only)
		return out;
	// 3. A covers folder at the root of the game's USB drive (vk-285-156: or NFS share).
	const std::string drive = DriveRoot(g.path);
	const std::string drive_covers = Subdir(drive, "covers");
	if (!drive_covers.empty())
		for (const std::string& n : names)
			if (!done())
				Lookup(drive_covers, n, "drive", out);
	if (done())
		return out;
	// 4. A download.
	if (!g.serial.empty() && !m_cache_dir.empty() && IsCoverFile(m_cache_dir + "/" + g.serial + ".jpg"))
		out.push_back({m_cache_dir + "/" + g.serial + ".jpg", "cache"});
	if (done())
		return out;
	// 5. Open PS2 Loader's ART folder on the drive.
	const std::string opl = OplGameId(g.serial);
	const std::string art = opl.empty() ? std::string() : Subdir(drive, "art");
	if (!art.empty())
		Lookup(art, opl + "_COV", "art", out);
	return out;
}

namespace
{
// 2.11: the fit a case face's picture is decoded with: `want` (Face, Spine), or none with the flag file flags/nocoverfit (CoverConfig::fit_covers).
CoverFit FitFor(const CoverConfig& cfg, CoverFit want)
{
	return cfg.fit_covers ? want : CoverFit::None;
}
} // namespace

bool CoverService::Decode(const std::vector<uint8_t>& bytes, int max_h, CoverImage& out, int max_dimension, CoverFit fit)
{
	if (max_dimension > 0)
	{
		int w, h, channels;
		if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels) ||
			w > max_dimension || h > max_dimension)
			return false;
	}
	int w = 0, h = 0, n = 0;
	unsigned char* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 4);
	if (!px || w < 16 || h < 16)
	{
		if (px)
			stbi_image_free(px);
		return false;
	}
	// 2.11: the texture is made the shape of the case face it goes on (fe_coverfit.h); the part of the picture that is used is resampled once, straight
	// into place. With CoverFit::None (badges) this is what it was: scaled down to `max_h` rows when taller.
	const FitPlan plan = PlanFit(w, h, max_h, fit);
	if (plan.mode == FitPlan::Pad)
	{
		out.width = plan.cw;
		out.height = plan.ch;
		out.rgba.resize(static_cast<size_t>(plan.cw) * plan.ch * 4);
		for (size_t i = 0; i < out.rgba.size(); i += 4) // the case's plastic (the painted spine's colour) round the picture
		{
			out.rgba[i] = 12;
			out.rgba[i + 1] = 12;
			out.rgba[i + 2] = 15;
			out.rgba[i + 3] = 255;
		}
	}
	else
		Fill(out, plan.cw, plan.ch);
	const unsigned char* src = px + (static_cast<size_t>(plan.sy) * w + plan.sx) * 4;
	uint8_t* dst = out.rgba.data() + (static_cast<size_t>(plan.dy) * plan.cw + plan.dx) * 4;
	if (plan.sw == plan.dw && plan.sh == plan.dh)
		for (int y = 0; y < plan.dh; y++)
			std::memcpy(dst + static_cast<size_t>(y) * plan.cw * 4, src + static_cast<size_t>(y) * w * 4, static_cast<size_t>(plan.dw) * 4);
	else
		stbir_resize_uint8_srgb(src, plan.sw, plan.sh, w * 4, dst, plan.dw, plan.dh, plan.cw * 4, STBIR_RGBA);
	stbi_image_free(px);
	for (size_t i = 3; i < out.rgba.size(); i += 4)
		out.rgba[i] = 255;
	out.has_glow = GlowFrom(out, out.glow);
	return true;
}

namespace
{
// 2.11 (swordpdf: the covers "need to be proportionate to the case"): the disc art as a square picture of exactly the disc, at most `max_side` across, so
// the shelf's circle of a fixed size on the case is the disc whatever the scan was: the transparent surround is cut off (a disc with a wide margin drew
// small), and a scan whose disc touches all four sides without the picture being square (an ellipse) is stretched round. A picture that is not one round
// disc (a disc beside a case, a banner: its opaque part is far from square; a picture with no transparent surround at all: its opaque part fills its box,
// where a disc fills 78 % of it) keeps its proportions and is padded with transparency to a square. The cut is made on the picture as it was decoded, so a
// disc scanned with a wide margin keeps its resolution. False: nothing opaque.
bool SquareDisc(const unsigned char* px, int w, int h, int max_side, CoverImage& out)
{
	int x0 = w, y0 = h, x1 = -1, y1 = -1;
	size_t opaque = 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			if (px[(static_cast<size_t>(y) * w + x) * 4 + 3] >= 128)
			{
				opaque++;
				x0 = std::min(x0, x);
				x1 = std::max(x1, x);
				y0 = std::min(y0, y);
				y1 = std::max(y1, y);
			}
	if (x1 < x0 || y1 < y0)
		return false;
	const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
	const double q = static_cast<double>(bw) / bh;
	const double fill = static_cast<double>(opaque) / (static_cast<double>(bw) * bh); // a disc: pi/4 minus its hole; a rectangle: 1
	const int side = std::max(16, std::min(max_side, std::max(bw, bh)));
	int dw = side, dh = side;
	if (q < 0.7 || q > 1.43 || fill > 0.9) // not a round disc: keep the proportions
	{
		if (q > 1.0)
			dh = std::max(1, static_cast<int>(std::lround(side / q)));
		else
			dw = std::max(1, static_cast<int>(std::lround(side * q)));
	}
	Fill(out, side, side); // transparent
	// linear: the surround's colour must not bleed in
	stbir_resize_uint8_linear(px + (static_cast<size_t>(y0) * w + x0) * 4, bw, bh, w * 4,
		out.rgba.data() + (static_cast<size_t>((side - dh) / 2) * side + (side - dw) / 2) * 4, dw, dh, side * 4, STBIR_RGBA);
	return true;
}
} // namespace

// 2.11: Decode for the disc art: the alpha channel is the round label's surround, so it is kept (as is, not made opaque) and no glow is worked out. The
// result is a square picture of the disc, at most `max_h` across (SquareDisc).
bool CoverService::DecodeWithAlpha(const std::vector<uint8_t>& bytes, int max_h, CoverImage& out, int max_dimension)
{
	if (max_dimension > 0)
	{
		int w, h, channels;
		if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels) || w > max_dimension || h > max_dimension)
			return false;
	}
	int w = 0, h = 0, n = 0;
	unsigned char* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 4);
	if (!px || w < 16 || h < 16)
	{
		if (px)
			stbi_image_free(px);
		return false;
	}
	const bool ok = SquareDisc(px, w, h, max_h, out);
	stbi_image_free(px);
	return ok;
}

void CoverService::PaintSpine(const Fonts& fonts, const GameInfo& g, CoverImage& out)
{
	const int W = 96, H = 1304;
	Fill(out, W, H);
	out.kind = CoverImage::Spine;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
		{
			const float e = std::fabs(x / (W - 1.0f) * 2.0f - 1.0f);
			const float shade = 1.0f - 0.45f * e * e * e;
			uint8_t* p = &out.rgba[(static_cast<size_t>(y) * W + x) * 4];
			p[0] = static_cast<uint8_t>(13 * shade);
			p[1] = static_cast<uint8_t>(13 * shade);
			p[2] = static_cast<uint8_t>(16 * shade);
			p[3] = 255;
		}
	std::vector<uint8_t> a;
	int tw, th, base;
	// "PlayStation 2" at the top, the title, and the serial at the foot, all reading downwards.
	fonts.Raster("PlayStation\xC2\xAE" "2", 25.0f, a, tw, th, base);
	DrawAlpha(out, a, tw, th, (W - th) / 2, 40, 0xe8e8e8, true);
	const int title_top = 60 + tw + 70, title_bottom = H - 190;
	float px = 44.0f;
	std::string title = g.title;
	for (;;)
	{
		fonts.Raster(title.c_str(), px, a, tw, th, base);
		if (tw <= title_bottom - title_top || px <= 30.0f)
			break;
		px -= 2.0f;
	}
	while (tw > title_bottom - title_top && title.size() > 4)
	{
		title.resize(title.size() - 2);
		while (!title.empty() && title.back() == ' ')
			title.pop_back();
		fonts.Raster((title + "\xE2\x80\xA6").c_str(), px, a, tw, th, base);
	}
	DrawAlpha(out, a, tw, th, (W - th) / 2, title_top, 0xffffff, true);
	if (!g.serial.empty())
	{
		fonts.Raster(g.serial.c_str(), 22.0f, a, tw, th, base);
		DrawAlpha(out, a, tw, th, (W - th) / 2, H - 40 - tw, 0x9a9aa6, true);
	}
}

void CoverService::PaintPlaceholder(const Fonts& fonts, const GameInfo& g, CoverImage& out)
{
	const int W = 364, H = 512;
	Fill(out, W, H);
	out.kind = CoverImage::Placeholder;
	for (int y = 0; y < H; y++)
	{
		const float t = y / (H - 1.0f);
		const uint8_t r = static_cast<uint8_t>(40 - 26 * t), gg = static_cast<uint8_t>(44 - 30 * t),
					  b = static_cast<uint8_t>(86 - 58 * t);
		for (int x = 0; x < W; x++)
		{
			uint8_t* p = &out.rgba[(static_cast<size_t>(y) * W + x) * 4];
			const bool header = y < 36;
			p[0] = header ? 6 : r;
			p[1] = header ? 6 : gg;
			p[2] = header ? 8 : b;
			p[3] = 255;
		}
	}
	std::vector<uint8_t> a;
	int tw, th, base;
	fonts.Raster("PlayStation\xC2\xAE" "2", 19.0f, a, tw, th, base);
	DrawAlpha(out, a, tw, th, 12, 18 - base + 6, 0xffffff, false);
	const std::vector<std::string> lines = Wrap(fonts, g.title, 38.0f, W - 44.0f, 5);
	const float line_h = 46.0f;
	float y = H * 0.46f - (lines.size() * line_h) * 0.5f;
	for (const std::string& l : lines)
	{
		fonts.Raster(l.c_str(), 38.0f, a, tw, th, base);
		DrawAlpha(out, a, tw, th, (W - tw) / 2, static_cast<int>(y), 0xffffff, false);
		y += line_h;
	}
	std::string foot = g.serial;
	if (!g.region.empty())
		foot += (foot.empty() ? "" : "  \xC2\xB7  ") + Region(g.region); // vk-285-110: in the PS5's language
	if (!foot.empty())
	{
		fonts.Raster(foot.c_str(), 18.0f, a, tw, th, base);
		DrawAlpha(out, a, tw, th, (W - tw) / 2, H - 44, 0xb0acd0, false);
	}
}

CoverService::~CoverService()
{
	Stop();
}



void CoverService::Start(const std::vector<GameInfo>& games, const Fonts* fonts, const CoverConfig& cfg, DownloadFn download)
{
	m_games = games;
	m_fonts = fonts;
	m_cfg = cfg;
	m_download = std::move(download);
	m_stop = false;
	m_offline_until_ms = m_lb_until_ms = 0; // 2.11
	m_offline_strikes = m_lb_strikes = m_lb_failures = 0;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_live.assign(m_games.size(), 0);
		m_todo.assign(m_games.size(), 0);
		m_back_todo.assign(m_games.size(), 0); // 2.11
		m_back_ask.assign(m_games.size(), 0);
		m_back_miss.assign(m_games.size(), 0);
		m_back_note.assign(m_games.size(), 0);
		m_back_ring.clear();
		m_back_dl.assign(m_games.size(), 0);
		m_spine_dl.assign(m_games.size(), 0);
		m_upgrade_todo.assign(m_games.size(), 0);
		m_disc_serial.clear();
		m_disc_todo = m_disc_dl = false;
	}
	if (!m_cfg.cache_dir.empty())
		MakeDirs(m_cfg.cache_dir);
	// Threads of their own with a roomy stack (image decoding and font rasterizing run on them). 2026-10-10: the
	// painting and the downloads apart, so a download (up to 20 s) never holds back the boxes the shelf is showing.
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 1024 * 1024);
	m_running = 2;
	m_thread_started = pthread_create(&m_thread, &attr, &CoverService::ThreadMain, this) == 0;
	if (!m_thread_started)
		m_running--;
	m_dl_thread_started = pthread_create(&m_dl_thread, &attr, &CoverService::DownloadThreadMain, this) == 0;
	if (!m_dl_thread_started)
		m_running--;
	pthread_attr_destroy(&attr);
}

void* CoverService::ThreadMain(void* self)
{
	CoverService* s = static_cast<CoverService*>(self);
	s->Run();
	s->m_running--;
	return nullptr;
}

void* CoverService::DownloadThreadMain(void* self)
{
	CoverService* s = static_cast<CoverService*>(self);
	s->RunDownloads();
	s->m_running--;
	return nullptr;
}

bool CoverService::Stop(int timeout_ms)
{
	m_stop = true;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_wake.notify_all();
	}
	if (!m_thread_started && !m_dl_thread_started)
		return true;
	if (timeout_ms >= 0)
	{
		for (int waited = 0; m_running > 0 && waited < timeout_ms; waited += 10)
			usleep(10000);
		if (m_running > 0)
			return false;
	}
	if (m_thread_started)
		pthread_join(m_thread, nullptr);
	if (m_dl_thread_started)
		pthread_join(m_dl_thread, nullptr);
	m_thread_started = m_dl_thread_started = false;
	return true;
}

bool CoverService::Poll(CoverImage& out)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_results.empty())
		return false;
	out = std::move(m_results.front());
	m_results.pop_front();
	return true;
}

void CoverService::SetSelected(int selected)
{
	m_selected = selected;
}

void CoverService::Want(int index)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (index < 0 || static_cast<size_t>(index) >= m_live.size() || m_live[static_cast<size_t>(index)])
		return;
	m_live[static_cast<size_t>(index)] = 1;
	m_todo[static_cast<size_t>(index)] = 1;
	m_wake.notify_all();
}

void CoverService::Forget(int index)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (index < 0 || static_cast<size_t>(index) >= m_live.size())
		return;
	m_live[static_cast<size_t>(index)] = 0;
	m_todo[static_cast<size_t>(index)] = 0;
	if (static_cast<size_t>(index) < m_back_todo.size())
		m_back_todo[static_cast<size_t>(index)] = m_back_dl[static_cast<size_t>(index)] = m_back_ask[static_cast<size_t>(index)] = m_back_miss[static_cast<size_t>(index)] =
			m_back_note[static_cast<size_t>(index)] = 0;
	if (static_cast<size_t>(index) < m_upgrade_todo.size())
		m_upgrade_todo[static_cast<size_t>(index)] = m_spine_dl[static_cast<size_t>(index)] = 0;
}

int CoverService::NextWanted() const
{
	const int sel = m_selected;
	int best = -1, best_d = 1 << 30;
	for (int i = 0; i < static_cast<int>(m_todo.size()); i++)
		if (m_todo[static_cast<size_t>(i)] && std::abs(i - sel) < best_d)
		{
			best_d = std::abs(i - sel);
			best = i;
		}
	return best;
}

// vk-285-157's throttle, kept: at most 24 images (< 18 MB) wait for the main thread, which takes 4 a frame.
bool CoverService::Push(CoverImage& img)
{
	static const size_t kMaxQueue = 24;
	for (;;)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_stop || img.game < 0 || static_cast<size_t>(img.game) >= m_live.size() || !m_live[static_cast<size_t>(img.game)])
				return false;
			if (m_results.size() < kMaxQueue)
			{
				m_results.push_back(std::move(img));
				return true;
			}
		}
		usleep(5000); // 5 ms; yields CPU to the render thread
	}
}

std::string CoverService::Status() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_busy && m_download_total > 0)
	{
		char buf[96];
		std::snprintf(buf, sizeof(buf), Tr(Str::DownloadingCovers), m_download_done, m_download_total); // vk-285-110
		return buf;
	}
	return {};
}

int CoverService::NextGame(const std::vector<bool>& done) const
{
	const int sel = m_selected;
	int best = -1, best_d = 1 << 30;
	for (int i = 0; i < static_cast<int>(done.size()); i++)
		if (!done[static_cast<size_t>(i)])
		{
			const int d = std::abs(i - sel);
			if (d < best_d)
			{
				best_d = d;
				best = i;
			}
		}
	return best;
}

bool CoverService::FindLocalCover(CoverFinder& finder, int index, CoverImage& out, bool& upgrade)
{
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	std::vector<uint8_t> bytes;
	// The first file that decodes, best place first (CoverFinder).
	for (const CoverFile& f : finder.Find(g))
		if (ReadFile(f.path, bytes) && Decode(bytes, 1024, out, 0, FitFor(m_cfg, CoverFit::Face)))
		{
			out.source = f.source;
			upgrade = std::strcmp(f.source, "art") == 0;
			return true;
		}
	upgrade = true;
	return false;
}

// A download for the game is worth a try: a serial, downloads allowed (on the shelf only for games on
// USB drives and, vk-285-156, NFS shares: the prefetch before the jailbreak sees neither), and no 404 for it in the last two weeks. (The network being down is checked as the
// downloads go: Offline().)
// ---- 2.11: LaunchBox art (fe_launchbox.h) ---------------------------------------------------------------------------------------------------
namespace
{
std::string FillSerial(std::string tmpl, const std::string& serial)
{
	const size_t at = tmpl.find("${serial}");
	if (at != std::string::npos)
		tmpl.replace(at, 9, serial);
	return tmpl;
}

bool IsLaunchboxUrl(const std::string& url)
{
	return url.rfind("https://images.launchbox-app.com/", 0) == 0 || url.rfind("https://gamesdb-images.launchbox.gg/", 0) == 0;
}

bool LbFileExists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

// cache/<serial>.lb says the cached cover is LaunchBox's: the shelf then leaves it alone (UpgradeCover). Written with a LaunchBox download, removed
// by any other.
void MarkLaunchbox(const std::string& cache_dir, const std::string& serial, bool from_launchbox)
{
	const std::string mark = cache_dir + "/" + serial + ".lb";
	if (from_launchbox)
		WriteFileAtomic(mark, std::vector<uint8_t>{'1', '\n'});
	else
		unlink(mark.c_str());
}

void Mark404(const std::string& path)
{
	WriteFileAtomic(path, std::vector<uint8_t>{'4', '0', '4', ' ', 'l', 'b', '\n'}); // "lb": made since LaunchBox was among the sources
}

// A source that answered but has nothing usable for the game: the file is gone (404, 403, 410) or it is a picture the app can't take (a 200 that
// doesn't decode). Not a transient failure (5xx, no connection, 429).
bool Gone(int status)
{
	return status == 404 || status == 403 || status == 410;
}

// A picture whose header says it is wider or taller than `max_side` (Decode refuses those); for the log.
bool OverMaxSide(const std::vector<uint8_t>& bytes, int max_side)
{
	int w = 0, h = 0, n = 0;
	return stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n) && (w > max_side || h > max_side);
}
} // namespace

int64_t CoverService::NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 2.11 (swordpdf: "back covers are not loading instantly"): a failure pauses the downloads for a while instead of switching them off for the session (one
// timeout, or a console not yet online at the shelf's start, used to take away every back, spine, disc and cover upgrade until the shelf was started
// again). The pause doubles with each one in a row and ends with the first picture that arrives. Which source failed decides what pauses: LaunchBox
// three requests in a row (2 minutes, up to 10) or one rate limit answer (15 minutes) pauses LaunchBox only; the configured source or the network
// itself (30 seconds, up to 5 minutes) pauses all of them.
void CoverService::NoteResult(const std::string& url, int status, bool good)
{
	const bool lb = IsLaunchboxUrl(url);
	if (good)
	{
		if (lb)
		{
			m_lb_failures = 0;
			m_lb_strikes = 0;
		}
		m_offline_strikes = 0; // a picture arrived: the network works
		return;
	}
	if (status >= 0 && status != 429)
		return; // an answer (not found, a server error): that is the file's story, not the source's
	const int retry = m_cfg.net_retry_ms; // 0: the times above; the tests shorten them
	// `base` doubled for each pause in a row before this one, at most `cap`
	const auto pause_ms = [](int base, int cap, std::atomic<int>& strikes) {
		const int doublings = std::min(strikes.fetch_add(1), 6);
		return std::min<int64_t>(cap, static_cast<int64_t>(base) << doublings);
	};
	if (lb)
	{
		if (status == 429 || ++m_lb_failures >= 3)
		{
			const int64_t ms = status == 429 ? (retry ? retry : 15 * 60 * 1000)
											  : pause_ms(retry ? retry : 2 * 60 * 1000, retry ? retry * 8 : 10 * 60 * 1000, m_lb_strikes);
			m_lb_failures = 0;
			m_lb_until_ms = NowMs() + ms;
			std::printf("[frontend] LaunchBox art is paused for %.1f s (%s)\n", static_cast<double>(ms) / 1000.0,
				status == 429 ? "rate limited" : "three requests in a row failed");
			std::fflush(stdout);
		}
	}
	else
	{
		// the configured source or the network itself
		const int64_t ms = pause_ms(retry ? retry : 30 * 1000, retry ? retry * 8 : 5 * 60 * 1000, m_offline_strikes);
		m_offline_until_ms = NowMs() + ms;
		std::printf("[frontend] network downloads paused for %.1f s (%s)\n", static_cast<double>(ms) / 1000.0, status == 429 ? "rate limited" : "no answer");
		std::fflush(stdout);
	}
}

std::vector<std::string> CoverService::FrontUrls(const CoverConfig& cfg, const std::string& serial)
{
	std::vector<std::string> urls;
	const LaunchboxArt* art = cfg.launchbox && !serial.empty() ? cfg.launchbox->Find(serial) : nullptr;
	const bool lb = art && !art->front.empty();
	// 186: LaunchBox's own-region scan first only with launchbox_front_first; else after the configured source, as for the other regions'
	const bool lb_first = lb && art->front_exact && cfg.launchbox_front_first;
	if (lb_first)
		urls.push_back(LaunchboxIndex::Url(art->front));
	if (!cfg.url_template.empty())
		urls.push_back(FillSerial(cfg.url_template, serial));
	if (lb && !lb_first)
		urls.push_back(LaunchboxIndex::Url(art->front));
	return urls;
}

std::vector<std::string> CoverService::BackUrls(const CoverConfig& cfg, const std::string& serial)
{
	std::vector<std::string> urls;
	const LaunchboxArt* art = cfg.launchbox && !serial.empty() ? cfg.launchbox->Find(serial) : nullptr;
	const bool lb = art && !art->back.empty();
	if (lb && art->back_exact)
		urls.push_back(LaunchboxIndex::Url(art->back));
	if (!cfg.back_url_template.empty())
		urls.push_back(FillSerial(cfg.back_url_template, serial));
	if (lb && !art->back_exact)
		urls.push_back(LaunchboxIndex::Url(art->back));
	return urls;
}

std::vector<std::string> CoverService::SpineUrls(const CoverConfig& cfg, const std::string& serial)
{
	std::vector<std::string> urls;
	const LaunchboxArt* art = cfg.launchbox && !serial.empty() ? cfg.launchbox->Find(serial) : nullptr;
	if (art && !art->spine.empty())
		urls.push_back(LaunchboxIndex::Url(art->spine));
	return urls;
}

std::vector<std::string> CoverService::DiscUrls(const CoverConfig& cfg, const std::string& serial)
{
	std::vector<std::string> urls;
	const LaunchboxArt* art = cfg.launchbox && !serial.empty() ? cfg.launchbox->Find(serial) : nullptr;
	if (art && !art->disc.empty())
		urls.push_back(LaunchboxIndex::Url(art->disc));
	return urls;
}

// A download for the game is worth a try: a serial, downloads allowed (on the shelf only for games on
// USB drives and, vk-285-156, NFS shares: the prefetch before the jailbreak sees neither), a source, and no 404 for it in the last two
// weeks. (The network being down is checked as the downloads go: Offline().)
bool CoverService::MayDownload(int index) const
{
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	const LaunchboxArt* art = m_cfg.launchbox && !g.serial.empty() ? m_cfg.launchbox->Find(g.serial) : nullptr;
	return !g.serial.empty() && m_cfg.allow_download && m_download && !FrontUrls(m_cfg, g.serial).empty() &&
		   (!m_cfg.download_usb_only || !UsbDriveRoot(g.path).empty() || OnNetworkShare(g.path)) &&
		   !RecentlyMissing(m_cfg.cache_dir, g.serial, art && !art->front.empty());
}

bool CoverService::DownloadCover(int index, CoverImage& out)
{
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	if (Offline() || m_stop)
		return false;
	const std::string cached = m_cfg.cache_dir + "/" + g.serial + ".jpg";
	const std::string missing = m_cfg.cache_dir + "/" + g.serial + ".missing";
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_busy = true;
	}
	// 2.11: the sources in order (LaunchBox's own-region scan, the configured source, LaunchBox's other regions'): the first that answers with an
	// image wins; every source having nothing usable is remembered for two weeks, a failure of any other kind is not.
	const std::vector<std::string> urls = FrontUrls(m_cfg, g.serial);
	bool ok = false, all_gone = true;
	int tried = 0, skipped = 0;
	for (const std::string& url : urls)
	{
		if (LbPaused() && IsLaunchboxUrl(url))
		{
			skipped++; // not asked: nothing may be remembered about its answer
			continue;
		}
		tried++;
		std::vector<uint8_t> bytes;
		const int status = m_download(url, bytes);
		const bool lb = IsLaunchboxUrl(url);
		const bool decoded = status == 200 && Decode(bytes, 1024, out, 4096, FitFor(m_cfg, CoverFit::Face));
		if (decoded)
		{
			WriteFileAtomic(cached, bytes);
			MarkLaunchbox(m_cfg.cache_dir, g.serial, lb);
			out.source = lb ? "launchbox" : "download";
			ok = true;
		}
		else if (!Gone(status) && status != 200)
			all_gone = false;
		NoteResult(url, status, decoded); // 2026-10-10: the cover host's rate limit: stop rather than hammer it
		std::printf("[frontend] cover %s: %s -> %d%s\n", g.serial.c_str(), url.c_str(), status,
			ok ? " (saved)" :
			status == 429 ? " (rate limited)" :
			status >= 0 ? (status == 200 ? " (not a picture the shelf can use)" : "") :
			m_stop ? " (stopped: the shelf closed)" :
					 " (no answer)");
		std::fflush(stdout);
		if (ok || Offline() || m_stop)
			break;
	}
	if (!ok && all_gone && tried > 0 && skipped == 0 && !Offline() && !m_stop)
		Mark404(missing);
	if (ok || !Offline()) // a game the network failed on is tried again when the pause is over (RunDownloads): not counted yet
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_download_done++;
	}
	return ok;
}

std::vector<int> CoverService::MissingCovers(const std::vector<GameInfo>& games, const CoverConfig& cfg)
{
	std::vector<int> missing;
	CoverFinder finder(cfg.manual_dir, cfg.cache_dir);
	for (size_t i = 0; i < games.size(); i++)
	{
		const GameInfo& g = games[i];
		if (g.serial.empty())
			continue;
		bool have = false;
		for (const CoverFile& f : finder.Find(g))
			have = have || std::strcmp(f.source, "art") != 0; // an OPL ART cover is worth replacing
		const LaunchboxArt* art = cfg.launchbox ? cfg.launchbox->Find(g.serial) : nullptr;
		if (!have && !RecentlyMissing(cfg.cache_dir, g.serial, art && !art->front.empty()))
			missing.push_back(static_cast<int>(i));
	}
	return missing;
}

int CoverService::Prefetch(const std::vector<GameInfo>& games, const std::vector<int>& which, const CoverConfig& cfg,
	const DownloadFn& download, double budget_s)
{
	if (which.empty() || !download || (cfg.url_template.empty() && !cfg.launchbox))
		return 0;
	MakeDirs(cfg.cache_dir);
	timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	auto elapsed = [&] {
		timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		return static_cast<double>(now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) * 1e-9;
	};
	int saved = 0;
	int lb_failures = 0; // 2.11 (review): LaunchBox failing three times running leaves it out for the rest; another source failing is the network down
	for (int index : which)
	{
		if (elapsed() >= budget_s)
		{
			std::printf("[frontend] prefetch: out of time after %.1f s; the rest next time\n", elapsed());
			break;
		}
		const GameInfo& g = games[static_cast<size_t>(index)];
		// 2.11: LaunchBox's scan of the serial's own region first, then the configured source, then LaunchBox's other regions.
		const std::vector<std::string> urls = FrontUrls(cfg, g.serial);
		bool ok = false, all_gone = true, net_down = false;
		int tried = 0;
		for (const std::string& url : urls)
		{
			const bool lb = IsLaunchboxUrl(url);
			if (lb && lb_failures >= 3)
				continue;
			tried++;
			std::vector<uint8_t> bytes;
			const int status = download(url, bytes);
			CoverImage probe;
			const bool decoded = status == 200 && Decode(bytes, 1024, probe, 4096);
			if (decoded)
			{
				ok = WriteFileAtomic(cfg.cache_dir + "/" + g.serial + ".jpg", bytes);
				MarkLaunchbox(cfg.cache_dir, g.serial, ok && lb);
				if (lb)
					lb_failures = 0;
			}
			else if (!Gone(status) && status != 200)
				all_gone = false;
			if (!decoded && (status < 0 || status == 429))
			{
				if (lb)
					lb_failures = status == 429 ? 3 : lb_failures + 1;
				else
					net_down = true; // the network is down: no point in trying the rest now
			}
			std::printf("[frontend] prefetch %s: %d%s%s\n", g.serial.c_str(), status, ok ? " (saved)" : (status == 200 ? " (not a usable picture; not kept)" : ""),
				lb ? " (LaunchBox)" : "");
			std::fflush(stdout);
			if (ok || net_down)
				break;
		}
		if (!ok && all_gone && tried > 0 && !net_down)
			Mark404(cfg.cache_dir + "/" + g.serial + ".missing");
		saved += ok ? 1 : 0;
		if (net_down)
			break;
	}
	return saved;
}

// 2026-10-10 (AI-assisted; a tester with ~2,600 games on an NFS share got no covers): the games the shelf wants, nearest
// the selection first: spine and placeholder, then the cover on disk (CoverFinder). Before, every game was painted and
// every cover on disk decoded before any download, and the shelf kept a texture for each: about 1.2 MB a game for the
// spine and placeholder, plus the cover's (~1.5 MB), which ran a 2,600-game shelf out of memory, and past ~1,020 games
// out of the renderer's descriptor sets, so boxes stopped being drawn (the selected one too, as its cover came).
void CoverService::Run()
{
	CoverFinder finder(m_cfg.manual_dir, m_cfg.cache_dir);
	while (!m_stop)
	{
		int i;
		int back = -1;
		bool disc = false;
		std::string disc_serial;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			if (m_disc_todo) // 2.11: a disc being copied wants its disc art
			{
				m_disc_todo = false;
				disc = true;
				disc_serial = m_disc_serial;
			}
			back = disc ? -1 : NextBack(); // 2.11
			if (disc)
				i = -3;
			else if (back >= 0)
			{
				m_back_todo[static_cast<size_t>(back)] = 0;
				i = -2;
			}
			else
				i = NextWanted();
			if (i == -1)
			{
				m_wake.wait_for(lock, std::chrono::milliseconds(100));
				continue;
			}
			if (i >= 0)
				m_todo[static_cast<size_t>(i)] = 0;
		}
		if (disc)
		{
			CoverImage art;
			if (FindLocalDisc(disc_serial, art))
			{
				art.kind = CoverImage::Disc;
				art.game = -1;
				art.serial = disc_serial;
				PushDisc(art);
				}
				else if (m_cfg.allow_download && m_download && !DiscUrls(m_cfg, disc_serial).empty())
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_disc_dl = true; // the download thread fetches it
			}
			continue;
		}
		if (back >= 0)
		{
			AnswerBack(finder, back);
			continue;
		}
		const GameInfo& g = m_games[static_cast<size_t>(i)];
		CoverImage spine, hold;
		PaintSpine(*m_fonts, g, spine);
		PaintPlaceholder(*m_fonts, g, hold);
		CoverImage back_ph;
		const bool noted = BackKnownNone(i);
		PaintBackPlaceholder(*m_fonts, g, back_ph, noted); // 2.11: the painted back comes with the others; it says "none" only where none can come
		spine.game = hold.game = back_ph.game = i;
		if (!Push(spine) || !Push(hold) || !Push(back_ph))
			continue;
		if (noted)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_back_note[static_cast<size_t>(i)] = m_live[static_cast<size_t>(i)];
		}
		// 2.11: a back the shelf asked for meanwhile is answered between the stages of this game's pass (a decode or two each), not after the whole pass.
		// Never `continue` out of the pass for it: this game's todo flag is cleared already and Want is a no-op while it is live, so the game would never
		// get its cover and spine.
		AnswerNextBack(finder);
		CoverImage cover;
		bool upgrade = true;
		if (FindLocalCover(finder, i, cover, upgrade))
		{
			cover.game = i;
			cover.kind = CoverImage::Cover;
			const bool improve = WantsUpgrade(i, cover.source); // 2.11: a cached cover LaunchBox has a sure match for
			Push(cover);
			if (improve)
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_upgrade_todo[static_cast<size_t>(i)] = m_live[static_cast<size_t>(i)] ? 1 : 0;
			}
		}
		AnswerNextBack(finder);
		// 2.11: the case's real side: a file on disk, else LaunchBox's spine from the download thread; the painted one stays until then
		{
			CoverImage side;
			if (FindLocalSpine(i, side))
			{
				side.game = i;
				side.kind = CoverImage::Spine;
				Push(side);
			}
			else if (m_cfg.allow_download && m_download && !SpineUrls(m_cfg, g.serial).empty())
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_spine_dl[static_cast<size_t>(i)] = m_live[static_cast<size_t>(i)] ? 1 : 0;
			}
		}
	}
}

// The missing covers (none on disk, or only an OPL ART one; no 404 in the last two weeks), downloaded into the cache
// nearest the selection first until the network fails. A cover for a game the shelf still shows goes to it too; the
// rest wait in the cache for when the shelf gets to them. Nothing is decoded here but the downloads.
void CoverService::RunDownloads()
{
	const size_t n = m_games.size();
	timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	m_dl_finder = std::make_unique<CoverFinder>(m_cfg.manual_dir, m_cfg.cache_dir); // this thread's own (CoverFinder is not thread safe); PrefetchBack uses it too
	CoverFinder& finder = *m_dl_finder;
	std::vector<bool> done(n, true);
	int have = 0, wanted = 0;
	for (size_t i = 0; i < n && !m_stop; i++)
	{
		bool on_disk = false;
		for (const CoverFile& f : finder.Find(m_games[i]))
			on_disk = on_disk || std::strcmp(f.source, "art") != 0; // an OPL ART cover is worth replacing
		have += on_disk ? 1 : 0;
		if (!on_disk && MayDownload(static_cast<int>(i)))
		{
			done[i] = false;
			wanted++;
		}
		if ((i & 31) == 31)
			ServeRequest(); // 2.11: a back the shelf asks for during the scan of a big library (thousands of games) is not kept waiting for its end
	}
	timespec t1;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	std::printf("[frontend] covers on disk: %d of %zu; %d to download%s%s (checked in %.0f ms)\n", have, n, wanted,
		m_cfg.allow_download ? "" : " (downloads off here)", m_cfg.launchbox ? (m_cfg.launchbox_front_first ? " (LaunchBox first)" : " (xlenore first, LaunchBox where it has none)") : "",
		static_cast<double>(t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
	std::fflush(stdout);
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_download_total = wanted;
	}
	for (int k = 0; k < wanted && !m_stop;)
	{
		if (Offline()) // 2.11: paused (NoteResult): wait for the pause to end; no game is used up meanwhile, and the thread stays
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_busy = false; // nothing is downloading, the shelf's "Downloading covers" line goes
			}
			usleep(100000);
			continue;
		}
		while (!m_stop && !Offline() && ServeRequest()) // 2.11: what the shelf is showing first
		{
		}
		if (m_stop || Offline())
			continue;
		const int i = NextGame(done);
		if (i < 0)
			break;
		done[static_cast<size_t>(i)] = true;
		CoverImage cover;
		if (DownloadCover(i, cover))
		{
			cover.game = i;
			cover.kind = CoverImage::Cover;
			Push(cover); // dropped when the shelf isn't showing the game: it's in the cache for when it is
		}
		else if (Offline())
			done[static_cast<size_t>(i)] = false; // the network failed on it: it is tried again when the pause is over
		if (!Offline())
			k++;
	}
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_busy = false;
	}
	// 2.11: then this thread stays for the other jobs (a disc being copied, a picked game's back cover, the covers beside it, a cover to improve) until
	// the shelf closes; it looks again every 20 ms, so a request waits only for the job in flight.
	while (!m_stop)
		if (!ServeRequest())
			usleep(20000);
}

// ---- 2.11: back covers (swordpdf: "can you research adding back cover support and moving the right stick would allow you slickly flip it") ----
// No free source has back covers by serial (xlenore's ps2-covers has "default" and "3d" only), so the shelf takes the ones the player has: the
// front cover's places and names with "_back" (or "-back", " back", "_b") after them, and a download when back_url_template names a source.
// Until one is found the back is painted (title and serial; with a line saying where a real one goes where none can come), so turning a case over
// always shows something. The picked game's back is asked for after 0.20 s (the files) and 0.35 s (the network) of being picked, or when the player
// starts to turn the case, and the backs of the cases beside it are fetched ahead (PrefetchBacks), so the flip finds it already there.

std::vector<CoverFile> CoverFinder::FindBack(const GameInfo& g)
{
	std::vector<CoverFile> out;
	static const char* const kSuffix[] = {"_back", "-back", " back", "_b"};
	std::vector<std::string> names;
	for (const std::string& n : {g.serial, g.stem, g.title})
		if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end())
			names.push_back(n);
	auto in_dir = [&](const std::string& dir, const char* source) {
		for (const std::string& n : names)
			for (const char* suffix : kSuffix)
				if (Lookup(dir, n + suffix, source, out))
					break;
	};
	in_dir(m_manual_dir, "manual");
	const size_t slash = g.path.rfind('/');
	if (slash != std::string::npos && slash > 0)
		for (const char* suffix : kSuffix)
			if (Lookup(g.path.substr(0, slash), g.stem + suffix, "beside", out))
				break;
	in_dir(Subdir(DriveRoot(g.path), "covers"), "drive");
	if (!g.serial.empty() && !m_cache_dir.empty() && IsCoverFile(m_cache_dir + "/" + g.serial + "_back.jpg"))
		out.push_back({m_cache_dir + "/" + g.serial + "_back.jpg", "cache"});
	return out;
}

void CoverService::PaintBackPlaceholder(const Fonts& fonts, const GameInfo& g, CoverImage& out, bool none)
{
	const int W = 256, H = 360;
	Fill(out, W, H);
	out.kind = CoverImage::Back;
	out.none = none;
	for (int y = 0; y < H; y++)
	{
		const float t = y / (H - 1.0f);
		for (int x = 0; x < W; x++)
		{
			uint8_t* p = &out.rgba[(static_cast<size_t>(y) * W + x) * 4];
			const bool header = y < 26;
			p[0] = static_cast<uint8_t>(header ? 6 : 24 - 16 * t);
			p[1] = static_cast<uint8_t>(header ? 6 : 26 - 18 * t);
			p[2] = static_cast<uint8_t>(header ? 8 : 50 - 36 * t);
			p[3] = 255;
		}
	}
	std::vector<uint8_t> a;
	int tw, th, base;
	fonts.Raster("PlayStation\xC2\xAE" "2", 14.0f, a, tw, th, base);
	DrawAlpha(out, a, tw, th, 9, 13 - base + 4, 0xffffff, false);
	const std::vector<std::string> lines = Wrap(fonts, g.title, 25.0f, W - 36.0f, 4);
	const float line_h = 31.0f;
	float y = H * 0.40f - (lines.size() * line_h) * 0.5f;
	for (const std::string& l : lines)
	{
		fonts.Raster(l.c_str(), 25.0f, a, tw, th, base);
		DrawAlpha(out, a, tw, th, (W - tw) / 2, static_cast<int>(y), 0xe8e8f4, false);
		y += line_h;
	}
	std::string foot = g.serial;
	if (!g.region.empty())
		foot += (foot.empty() ? "" : "  \xC2\xB7  ") + Region(g.region);
	if (!foot.empty())
	{
		fonts.Raster(foot.c_str(), 14.0f, a, tw, th, base);
		DrawAlpha(out, a, tw, th, (W - tw) / 2, static_cast<int>(H * 0.66f), 0xb0acd0, false);
	}
	// A thin rule, and where a real back cover goes (only where none can come: a card that is shown while a back may still arrive says nothing about it).
	for (int x = W / 2 - 40; x < W / 2 + 40; x++)
		Blend(out, x, static_cast<int>(H * 0.76f), 0xb0acd0, 0.35f);
	if (!none)
		return;
	const std::string name = (g.serial.empty() ? g.stem : g.serial) + "_back.jpg";
	fonts.Raster("No back cover for this game", 13.0f, a, tw, th, base);
	DrawAlpha(out, a, tw, th, (W - tw) / 2, static_cast<int>(H * 0.82f), 0x8c88aa, false);
	fonts.Raster(("Put " + name + " in covers/").c_str(), 12.0f, a, tw, th, base);
	DrawAlpha(out, a, tw, th, (W - tw) / 2, static_cast<int>(H * 0.87f), 0x8c88aa, false);
}

int CoverService::NextBack() const
{
	const int sel = m_selected;
	int best = -1, best_d = 1 << 30;
	for (int i = 0; i < static_cast<int>(m_back_todo.size()); i++)
		if (m_back_todo[static_cast<size_t>(i)] && std::abs(i - sel) < best_d)
		{
			best_d = std::abs(i - sel);
			best = i;
		}
	return best;
}

void CoverService::WantBack(int index, bool download)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (index < 0 || static_cast<size_t>(index) >= m_back_todo.size() || !m_live[static_cast<size_t>(index)])
		return;
	const size_t i = static_cast<size_t>(index);
	const uint8_t level = download ? 2 : 1; // what is asked: the files, or the files and the network
	if (m_back_ask[i] >= level)
		return; // asked already (a download in flight or done included): asking again costs nothing
	const uint8_t before = m_back_ask[i];
	m_back_ask[i] = level;
	if (before == 0)
		m_back_todo[i] = 1; // the lookup on disk, on the painting thread; it sees the level when it has its answer
	else if (m_back_miss[i])
	{
		m_back_miss[i] = 0; // looked up and missed, waiting for exactly this permission
		m_back_dl[i] = 1;
	}
	m_wake.notify_all();
}

void CoverService::ForgetBack(int index)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (index >= 0 && static_cast<size_t>(index) < m_back_todo.size())
	{
		const size_t i = static_cast<size_t>(index);
		m_back_todo[i] = m_back_dl[i] = m_back_ask[i] = m_back_miss[i] = 0;
	}
}

void CoverService::PrefetchBacks(const std::vector<int>& nearest_first)
{
	if (!m_cfg.back_prefetch)
		return;
	static const size_t kMaxRing = 8;
	std::lock_guard<std::mutex> lock(m_mutex);
	m_back_ring.clear();
	for (const int index : nearest_first)
		if (index >= 0 && static_cast<size_t>(index) < m_games.size() && m_back_ring.size() < kMaxRing)
			m_back_ring.push_back(index);
}

// No back can come for the game: nothing to fetch it from (no serial, downloads off, no LaunchBox match and no source of the player's), or the sources
// have none (a 404 or a picture the shelf can't use in the last two weeks). Not a pause: that ends.
bool CoverService::BackKnownNone(int index) const
{
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	return g.serial.empty() || !m_cfg.allow_download || !m_download || BackUrls(m_cfg, g.serial).empty() ||
		   RecentlyMissing(m_cfg.cache_dir, g.serial + "_back");
}

// The answer to a back cover the shelf asked for (painting thread; the request is taken off the list already): the file on disk, or the painted back with
// its note when none can come, or else the request goes to the download thread -- at once when the shelf has given the permission, else it waits for it
// (WantBack).
void CoverService::AnswerBack(CoverFinder& finder, int back)
{
	CoverImage real;
	if (FindLocalBack(finder, back, real))
	{
		real.game = back;
		real.kind = CoverImage::RealBack;
		Push(real);
		return;
	}
	if (BackKnownNone(back))
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_back_note[static_cast<size_t>(back)])
				return; // the card with the note is out already
		}
		CoverImage note;
		PaintBackPlaceholder(*m_fonts, m_games[static_cast<size_t>(back)], note, true); // replaces the card that says nothing
		note.game = back;
		if (Push(note))
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_back_note[static_cast<size_t>(back)] = m_live[static_cast<size_t>(back)];
		}
		return;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	const size_t i = static_cast<size_t>(back);
	if (!m_live[i] || m_back_ask[i] == 0)
		return; // forgotten meanwhile
	if (m_back_ask[i] >= 2)
		m_back_dl[i] = 1; // the download thread fetches it
	else
		m_back_miss[i] = 1;
}

bool CoverService::AnswerNextBack(CoverFinder& finder)
{
	int back;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		back = NextBack();
		if (back >= 0)
			m_back_todo[static_cast<size_t>(back)] = 0;
	}
	if (back < 0)
		return false;
	AnswerBack(finder, back);
	return true;
}

bool CoverService::FindLocalBack(CoverFinder& finder, int index, CoverImage& out)
{
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	std::vector<uint8_t> bytes;
	for (const CoverFile& f : finder.FindBack(g))
		if (ReadFile(f.path, bytes) && Decode(bytes, 768, out, 0, FitFor(m_cfg, CoverFit::Face))) // smaller than a front: only the picked case's back is held
		{
			out.source = f.source;
			return true;
		}
	return false;
}

// The back from the network, decoded for the shelf. The bytes to keep in the cache come back in `keep` (ServeRequest writes them once the image is handed
// over: the flash write stays out of the way to the screen). `none`: the sources have none and the game was marked (the painting thread then paints
// the note). The log line says how long the request took, which is what a console log needs to tell the network from the rest.
bool CoverService::DownloadBack(int index, CoverImage& out, std::vector<uint8_t>& keep, bool& none, bool& no_answer)
{
	none = false;
	no_answer = false;
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	if (Offline() || m_stop || g.serial.empty() || !m_cfg.allow_download || !m_download || RecentlyMissing(m_cfg.cache_dir, g.serial + "_back"))
		return false;
	const std::vector<std::string> urls = BackUrls(m_cfg, g.serial);
	bool ok = false, all_gone = true;
	int tried = 0, skipped = 0;
	for (const std::string& url : urls)
	{
		if (LbPaused() && IsLaunchboxUrl(url))
		{
			skipped++; // not asked: nothing may be remembered about its answer
			continue;
		}
		tried++;
		std::vector<uint8_t> bytes;
		const int64_t t0 = NowMs();
		const int status = m_download(url, bytes);
		const int ms = static_cast<int>(NowMs() - t0);
		const size_t size = bytes.size();
		const bool too_big = status == 200 && OverMaxSide(bytes, 4096);
		const bool decoded = status == 200 && Decode(bytes, 768, out, 4096, FitFor(m_cfg, CoverFit::Face));
		if (decoded)
		{
			keep = std::move(bytes);
			out.source = IsLaunchboxUrl(url) ? "launchbox" : "download";
			ok = true;
		}
		else if (!Gone(status) && status != 200)
			all_gone = false;
		no_answer = no_answer || (status < 0 && !m_stop);
		NoteResult(url, status, decoded);
		std::printf("[frontend] back cover %s: %s -> %d (%zu KB in %d ms)%s\n", g.serial.c_str(), url.c_str(), status, size / 1024, ms,
			ok ? " (saved)" :
			status == 429 ? " (rate limited)" :
			status >= 0 ? (status == 200 ? (too_big ? " (over 4096 px: not used)" : " (not a picture the shelf can use)") : "") :
			m_stop ? " (stopped: the shelf closed)" :
					 " (no answer)");
		std::fflush(stdout);
		if (ok || Offline() || m_stop)
			break;
	}
	if (!ok && all_gone && tried > 0 && skipped == 0 && !Offline() && !m_stop)
	{
		Mark404(m_cfg.cache_dir + "/" + g.serial + "_back.missing");
		none = RecentlyMissing(m_cfg.cache_dir, g.serial + "_back"); // (a full disk leaves no mark: then it is not "none")
	}
	return ok;
}

// 2.11 (swordpdf: "back covers are not loading instantly"): the back of a case near the picked one, into the cache and nowhere else -- no decode (the size
// is read from the picture's header), no image, no texture. The flip then finds the file where the shelf looks for one (CoverFinder::FindBack) and decodes
// it as any back is decoded. Nothing is asked when the back is on disk already, none can come (BackKnownNone), or the sources are paused; the answers are
// remembered as for the picked game's back (a 404 for two weeks). The download thread's own finder is asked, never the painting thread's.
bool CoverService::PrefetchBack(int index)
{
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	if (!m_cfg.back_prefetch || Offline() || m_stop || BackKnownNone(index))
		return false;
	if (m_dl_finder && !m_dl_finder->FindBack(g).empty())
		return false;
	bool ok = false, all_gone = true;
	int tried = 0, skipped = 0;
	for (const std::string& url : BackUrls(m_cfg, g.serial))
	{
		if (LbPaused() && IsLaunchboxUrl(url))
		{
			skipped++;
			continue;
		}
		tried++;
		std::vector<uint8_t> bytes;
		const int64_t t0 = NowMs();
		const int status = m_download(url, bytes);
		const int ms = static_cast<int>(NowMs() - t0);
		int w = 0, h = 0, n = 0;
		const bool usable = status == 200 && stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n) && w >= 16 && h >= 16 &&
							w <= 4096 && h <= 4096;
		bool wrote = true;
		if (usable)
		{
			wrote = WriteFileAtomic(m_cfg.cache_dir + "/" + g.serial + "_back.jpg", bytes);
			ok = true; // (a full disk is no reason to remember a 404)
		}
		else if (!Gone(status) && status != 200)
			all_gone = false;
		NoteResult(url, status, usable);
		std::printf("[frontend] back cover %s (ahead): %s -> %d (%zu KB in %d ms)%s\n", g.serial.c_str(), url.c_str(), status, bytes.size() / 1024, ms,
			ok ? (wrote ? " (saved)" : " (could not write the file)") :
			status == 429 ? " (rate limited)" :
			status >= 0 ? (status == 200 ? " (not a picture the shelf can use)" : "") :
			m_stop ? " (stopped: the shelf closed)" :
					 " (no answer)");
		std::fflush(stdout);
		if (ok || Offline() || m_stop)
			break;
	}
	if (!ok && all_gone && tried > 0 && skipped == 0 && !Offline() && !m_stop)
		Mark404(m_cfg.cache_dir + "/" + g.serial + "_back.missing");
	return ok;
}

// ---- 2.11: the download thread's other jobs --------------------------------------------------------------------------------------------------

void CoverService::RequestDisc(const std::string& serial)
{
	if (serial.empty())
		return;
	std::lock_guard<std::mutex> lock(m_mutex);
	m_disc_serial = serial;
	m_disc_todo = true;
	m_disc_dl = false;
	m_wake.notify_all();
}

bool CoverService::PushDisc(CoverImage& img)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_stop)
		return false;
	m_results.push_back(std::move(img));
	return true;
}

// The player's own disc art (covers/<serial>_disc.png) or one downloaded before (cache/<serial>_disc.png).
bool CoverService::FindLocalDisc(const std::string& serial, CoverImage& out)
{
	std::vector<uint8_t> bytes;
	for (const std::string& dir : {m_cfg.manual_dir, m_cfg.cache_dir})
	{
		if (dir.empty())
			continue;
		if (ReadFile(dir + "/" + serial + "_disc.png", bytes) && DecodeWithAlpha(bytes, 512, out, 2600))
		{
			out.source = dir == m_cfg.manual_dir ? "manual" : "cache";
			return true;
		}
	}
	return false;
}

bool CoverService::DownloadDisc(const std::string& serial, CoverImage& out)
{
	if (Offline() || m_stop || serial.empty() || !m_cfg.allow_download || !m_download || RecentlyMissing(m_cfg.cache_dir, serial + "_disc"))
		return false;
	const std::vector<std::string> urls = DiscUrls(m_cfg, serial);
	bool ok = false, all_gone = true;
	int tried = 0, skipped = 0;
	for (const std::string& url : urls)
	{
		if (LbPaused() && IsLaunchboxUrl(url))
		{
			skipped++; // not asked: nothing may be remembered about its answer
			continue;
		}
		tried++;
		std::vector<uint8_t> bytes;
		const int status = m_download(url, bytes);
		const bool decoded = status == 200 && DecodeWithAlpha(bytes, 512, out, 2600);
		if (decoded)
		{
			if (bytes.size() <= (2u << 20)) // kept for the next copy of the game, unless it is one of the multi-megabyte ones
				WriteFileAtomic(m_cfg.cache_dir + "/" + serial + "_disc.png", bytes);
			out.source = "launchbox";
			ok = true;
		}
		else if (!Gone(status) && status != 200)
			all_gone = false;
		NoteResult(url, status, decoded);
		std::printf("[frontend] disc art %s: %s -> %d%s\n", serial.c_str(), url.c_str(), status, ok ? " (taken)" : "");
		std::fflush(stdout);
		if (ok || Offline() || m_stop)
			break;
	}
	if (!ok && all_gone && tried > 0 && skipped == 0 && !Offline() && !m_stop)
		Mark404(m_cfg.cache_dir + "/" + serial + "_disc.missing");
	return ok;
}

// A cached cover the index has a sure match for (title matched as it stands, the front from the serial's own region), not LaunchBox's yet.
bool CoverService::WantsUpgrade(int index, const char* source) const
{
	if (!m_cfg.launchbox_front_first)
		return WantsRevert(index, source); // 186: the old fronts first: a LaunchBox one goes back instead
	if (!m_cfg.launchbox || !m_cfg.launchbox_upgrade || !m_cfg.allow_download || !m_download || std::strcmp(source, "cache") != 0)
		return false;
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	const LaunchboxArt* art = g.serial.empty() ? nullptr : m_cfg.launchbox->Find(g.serial);
	return art && LaunchboxIndex::CanUpgrade(*art) && !LbFileExists(m_cfg.cache_dir + "/" + g.serial + ".lb") &&
		   !RecentlyMissing(m_cfg.cache_dir, g.serial + "_lb");
}

// Replaces the cached cover with LaunchBox's scan; the old file is kept once as <serial>.jpg.old. A scan that is gone or not an image is not asked
// for again for two weeks.
bool CoverService::UpgradeCover(int index, CoverImage& out)
{
	if (!m_cfg.launchbox_front_first)
		return RevertCover(index, out); // 186
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	const LaunchboxArt* art = m_cfg.launchbox ? m_cfg.launchbox->Find(g.serial) : nullptr;
	if (!art || !LaunchboxIndex::CanUpgrade(*art) || Offline() || m_stop || LbPaused())
		return false;
	const std::string url = LaunchboxIndex::Url(art->front);
	const std::string cached = m_cfg.cache_dir + "/" + g.serial + ".jpg";
	const std::string fresh = cached + ".lbnew";
	std::vector<uint8_t> bytes;
	const int status = m_download(url, bytes);
	bool ok = false;
	const bool decoded = status == 200 && Decode(bytes, 1024, out, 4096, FitFor(m_cfg, CoverFit::Face));
	if (decoded)
	{
		// The new file is written beside the old one first; only then the old one is kept once as .old and the new one takes its place (a full disk or a
		// kill half way leaves the cover as it was).
		if (WriteFileAtomic(fresh, bytes))
		{
			const std::string old = cached + ".old";
			const bool had = LbFileExists(cached);
			if (had && !LbFileExists(old))
				rename(cached.c_str(), old.c_str());
			if (rename(fresh.c_str(), cached.c_str()) == 0)
			{
				ok = true;
				MarkLaunchbox(m_cfg.cache_dir, g.serial, true);
				out.source = "launchbox";
			}
			else if (had && !LbFileExists(cached) && LbFileExists(old))
				rename(old.c_str(), cached.c_str()); // put the old one back
		}
		if (!ok)
		{
			unlink(fresh.c_str());
			Mark404(m_cfg.cache_dir + "/" + g.serial + "_lb.missing"); // not again for two weeks
		}
	}
	else if (status == 200 || Gone(status))
		Mark404(m_cfg.cache_dir + "/" + g.serial + "_lb.missing"); // gone, or not a picture the shelf can use
	NoteResult(url, status, decoded);
	std::printf("[frontend] cover upgrade %s: %s -> %d%s\n", g.serial.c_str(), url.c_str(), status, ok ? " (replaced; the old one is kept as .jpg.old)" : "");
	std::fflush(stdout);
	return ok;
}

// 186: a cached front that is LaunchBox's (cache/<serial>.lb) while LaunchBox no longer comes first for fronts: switched back once (RevertCover), from the
// file it replaced when that is still there, else from the configured source unless that had none for it in the last two weeks.
bool CoverService::WantsRevert(int index, const char* source) const
{
	if (std::strcmp(source, "cache") != 0)
		return false;
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	if (g.serial.empty() || !LbFileExists(m_cfg.cache_dir + "/" + g.serial + ".lb"))
		return false;
	if (LbFileExists(m_cfg.cache_dir + "/" + g.serial + ".jpg.old"))
		return true;
	return m_cfg.allow_download && m_download && !m_cfg.url_template.empty() && !RecentlyMissing(m_cfg.cache_dir, g.serial + "_x");
}

// 186: switches a cached LaunchBox front back: the cover UpgradeCover replaced (<serial>.jpg.old) when it is there, else the configured source's
// (xlenore's). LaunchBox's is kept once as <serial>.jpg.lbfront. A source with none for the game (gone, or not a picture): LaunchBox's stays, and the
// source is not asked again for two weeks (<serial>_x.missing).
bool CoverService::RevertCover(int index, CoverImage& out)
{
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	const std::string cached = m_cfg.cache_dir + "/" + g.serial + ".jpg";
	const std::string old = cached + ".old", kept = cached + ".lbfront";
	std::vector<uint8_t> bytes;
	if (LbFileExists(old))
	{
		rename(cached.c_str(), kept.c_str());
		if (rename(old.c_str(), cached.c_str()) != 0)
		{
			rename(kept.c_str(), cached.c_str()); // put LaunchBox's back: nothing changed
			return false;
		}
		MarkLaunchbox(m_cfg.cache_dir, g.serial, false);
		const bool ok = ReadFile(cached, bytes) && Decode(bytes, 1024, out, 0, FitFor(m_cfg, CoverFit::Face));
		if (ok)
			out.source = "cache";
		std::printf("[frontend] cover %s: back to the one LaunchBox's had replaced (.jpg.old; LaunchBox's kept as .jpg.lbfront)%s\n", g.serial.c_str(),
			ok ? "" : ", which does not decode");
		std::fflush(stdout);
		return ok;
	}
	if (Offline() || m_stop || m_cfg.url_template.empty())
		return false;
	const std::string url = FillSerial(m_cfg.url_template, g.serial);
	const std::string fresh = cached + ".xnew";
	const int status = m_download(url, bytes);
	const bool decoded = status == 200 && Decode(bytes, 1024, out, 4096, FitFor(m_cfg, CoverFit::Face));
	bool ok = false;
	if (decoded && WriteFileAtomic(fresh, bytes))
	{
		// LaunchBox's is kept first; only then the new one takes its place (a full disk or a kill half way leaves a cover there)
		rename(cached.c_str(), kept.c_str());
		if (rename(fresh.c_str(), cached.c_str()) == 0)
		{
			ok = true;
			MarkLaunchbox(m_cfg.cache_dir, g.serial, false);
			out.source = "download";
		}
		else
		{
			rename(kept.c_str(), cached.c_str());
			unlink(fresh.c_str());
		}
	}
	else if (status == 200 || Gone(status))
		Mark404(m_cfg.cache_dir + "/" + g.serial + "_x.missing"); // none there: LaunchBox's stays, not asked again for two weeks
	NoteResult(url, status, decoded);
	std::printf("[frontend] cover %s back to the old source: %s -> %d%s\n", g.serial.c_str(), url.c_str(), status,
		ok ? " (replaced; LaunchBox's kept as .jpg.lbfront)" : decoded ? " (could not be written)" : " (LaunchBox's stays)");
	std::fflush(stdout);
	return ok;
}

// The player's own spine (covers/<serial>_spine.jpg or .png) or one downloaded before (cache/<serial>_spine.jpg). Decoded at the painted spine's height.
bool CoverService::FindLocalSpine(int index, CoverImage& out)
{
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	if (g.serial.empty())
		return false;
	std::vector<uint8_t> bytes;
	const std::string stem = g.serial + "_spine";
	for (const std::string& dir : {m_cfg.manual_dir, m_cfg.cache_dir})
		for (const char* ext : {".jpg", ".png", ".jpeg"})
			if (!dir.empty() && ReadFile(dir + "/" + stem + ext, bytes) && Decode(bytes, 1304, out, 4096, FitFor(m_cfg, CoverFit::Spine)))
			{
				out.source = dir == m_cfg.manual_dir ? "manual" : "cache";
				return true;
			}
	return false;
}

bool CoverService::DownloadSpine(int index, CoverImage& out)
{
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	if (Offline() || m_stop || g.serial.empty() || !m_cfg.allow_download || !m_download || RecentlyMissing(m_cfg.cache_dir, g.serial + "_spine"))
		return false;
	const std::vector<std::string> urls = SpineUrls(m_cfg, g.serial);
	bool ok = false, all_gone = true;
	int tried = 0, skipped = 0;
	for (const std::string& url : urls)
	{
		if (LbPaused() && IsLaunchboxUrl(url))
		{
			skipped++; // not asked: nothing may be remembered about its answer
			continue;
		}
		tried++;
		std::vector<uint8_t> bytes;
		const int status = m_download(url, bytes);
		const bool decoded = status == 200 && Decode(bytes, 1304, out, 4096, FitFor(m_cfg, CoverFit::Spine));
		if (decoded)
		{
			WriteFileAtomic(m_cfg.cache_dir + "/" + g.serial + "_spine.jpg", bytes);
			out.source = "launchbox";
			ok = true;
		}
		else if (!Gone(status) && status != 200)
			all_gone = false;
		NoteResult(url, status, decoded);
		std::printf("[frontend] spine %s: %s -> %d%s\n", g.serial.c_str(), url.c_str(), status, ok ? " (saved)" : "");
		std::fflush(stdout);
		if (ok || Offline() || m_stop)
			break;
	}
	if (!ok && all_gone && tried > 0 && skipped == 0 && !Offline() && !m_stop)
		Mark404(m_cfg.cache_dir + "/" + g.serial + "_spine.missing");
	return ok;
}

int CoverService::NextSpineDownload() const
{
	const int sel = m_selected;
	int best = -1, best_d = 1 << 30;
	for (int i = 0; i < static_cast<int>(m_spine_dl.size()); i++)
		if (m_spine_dl[static_cast<size_t>(i)] && std::abs(i - sel) < best_d)
		{
			best_d = std::abs(i - sel);
			best = i;
		}
	return best;
}

int CoverService::NextBackDownload() const
{
	const int sel = m_selected;
	int best = -1, best_d = 1 << 30;
	for (int i = 0; i < static_cast<int>(m_back_dl.size()); i++)
		if (m_back_dl[static_cast<size_t>(i)] && std::abs(i - sel) < best_d)
		{
			best_d = std::abs(i - sel);
			best = i;
		}
	return best;
}

int CoverService::NextUpgrade() const
{
	const int sel = m_selected;
	int best = -1, best_d = 1 << 30;
	for (int i = 0; i < static_cast<int>(m_upgrade_todo.size()); i++)
		if (m_upgrade_todo[static_cast<size_t>(i)] && std::abs(i - sel) < best_d)
		{
			best_d = std::abs(i - sel);
			best = i;
		}
	return best;
}

bool CoverService::ServeRequest()
{
	if (Offline() || m_stop)
		return false;
	std::string disc;
	int back = -1, ring = -1, improve = -1, side = -1;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		// A job whose every source is paused (LaunchBox, for the art only it has) stays queued for when the pause ends, which is soon; the others go on.
		const bool lb_paused = LbPaused();
		const auto servable = [lb_paused](const std::vector<std::string>& urls) {
			if (!lb_paused || urls.empty()) // (no address at all: the job ends at once, it is not waiting for LaunchBox)
				return true;
			for (const std::string& url : urls)
				if (!IsLaunchboxUrl(url))
					return true;
			return false;
		};
		// The order: a disc being copied, the picked game's back, the backs beside it (ahead of the spines: on a fresh install the painting thread queues a
		// spine for every game it paints, and the flip is what the player is waiting for), the nearest spine, a cover to improve.
		const int next_back = NextBackDownload();
		if (m_disc_dl && servable(DiscUrls(m_cfg, m_disc_serial)))
		{
			m_disc_dl = false;
			disc = m_disc_serial;
		}
		else if (next_back >= 0 && servable(BackUrls(m_cfg, m_games[static_cast<size_t>(next_back)].serial)))
		{
			back = next_back;
			m_back_dl[static_cast<size_t>(back)] = 0;
		}
		else if (!m_back_ring.empty() && servable(BackUrls(m_cfg, m_games[static_cast<size_t>(m_back_ring.front())].serial)))
		{
			ring = m_back_ring.front();
			m_back_ring.erase(m_back_ring.begin());
		}
		else if (!lb_paused && (side = NextSpineDownload()) >= 0)
			m_spine_dl[static_cast<size_t>(side)] = 0;
		else if (!lb_paused && (improve = NextUpgrade()) >= 0)
			m_upgrade_todo[static_cast<size_t>(improve)] = 0;
		else
			return false;
	}
	if (!disc.empty())
	{
		CoverImage art;
		if (DownloadDisc(disc, art))
		{
			art.kind = CoverImage::Disc;
			art.game = -1;
			art.serial = disc;
			PushDisc(art);
		}
	}
	else if (back >= 0)
	{
		CoverImage real;
		std::vector<uint8_t> keep;
		bool none = false, no_answer = false;
		// A prefetch of this very back may have landed since the painting thread looked: then it is only decoded (no second request).
		if (m_dl_finder && FindLocalBack(*m_dl_finder, back, real))
		{
			real.game = back;
			real.kind = CoverImage::RealBack;
			Push(real);
		}
		else if (DownloadBack(back, real, keep, none, no_answer))
		{
			real.game = back;
			real.kind = CoverImage::RealBack;
			Push(real);
			// the file is written after the picture is on its way to the screen
			if (!WriteFileAtomic(m_cfg.cache_dir + "/" + m_games[static_cast<size_t>(back)].serial + "_back.jpg", keep))
			{
				std::printf("[frontend] back cover %s: could not write it to the cache\n", m_games[static_cast<size_t>(back)].serial.c_str());
				std::fflush(stdout);
			}
		}
		else if (none)
		{
			// The sources have none (and the game is marked): the painting thread finds the mark and paints the note.
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_live[static_cast<size_t>(back)] && m_back_ask[static_cast<size_t>(back)])
				m_back_todo[static_cast<size_t>(back)] = 1;
		}
		else if (Offline() || LbPaused() || no_answer)
		{
			// This request was the one that began a pause (no answer, a rate limit), or got no answer at all (a connection that failed, the first or second
			// time: a pause comes with the third): the picked game's back is tried again when the pause is over rather than dropped (the shelf asks only once
			// for a game it stays on). Only where it cannot spin: all downloads paused, a failure (each one counts towards a pause), or every address LaunchBox's.
			bool only_lb = true;
			for (const std::string& url : BackUrls(m_cfg, m_games[static_cast<size_t>(back)].serial))
				only_lb = only_lb && IsLaunchboxUrl(url);
			std::lock_guard<std::mutex> lock(m_mutex);
			if ((Offline() || only_lb || no_answer) && m_live[static_cast<size_t>(back)] && m_back_ask[static_cast<size_t>(back)] >= 2)
				m_back_dl[static_cast<size_t>(back)] = 1;
		}
	}
	else if (ring >= 0)
		PrefetchBack(ring);
	else if (side >= 0)
	{
		CoverImage spine;
		if (DownloadSpine(side, spine))
		{
			spine.game = side;
			spine.kind = CoverImage::Spine;
			Push(spine);
		}
	}
	else
	{
		CoverImage cover;
		if (UpgradeCover(improve, cover))
		{
			cover.game = improve;
			cover.kind = CoverImage::Cover;
			Push(cover);
		}
	}
	return true;
}

} // namespace fe
