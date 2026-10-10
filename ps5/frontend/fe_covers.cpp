// PS5 port frontend: covers (see fe_covers.h).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_covers.h"
#include "fe_i18n.h"

#include <algorithm>
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
bool RecentlyMissing(const std::string& cache_dir, const std::string& serial)
{
	struct stat st = {};
	return stat((cache_dir + "/" + serial + ".missing").c_str(), &st) == 0 &&
		   std::time(nullptr) - st.st_mtime < 14 * 24 * 3600;
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
	return path.compare(0, 5, "/nfs/") == 0;
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

bool CoverService::Decode(const std::vector<uint8_t>& bytes, int max_h, CoverImage& out, int max_dimension)
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
	if (h > max_h)
	{
		const int nw = std::max(1, static_cast<int>(std::lround(static_cast<double>(w) * max_h / h)));
		Fill(out, nw, max_h);
		stbir_resize_uint8_srgb(px, w, h, 0, out.rgba.data(), nw, max_h, 0, STBIR_RGBA);
	}
	else
	{
		out.width = w;
		out.height = h;
		out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
	}
	stbi_image_free(px);
	for (size_t i = 3; i < out.rgba.size(); i += 4)
		out.rgba[i] = 255;
	out.has_glow = GlowFrom(out, out.glow);
	return true;
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
	m_finished = false;
	if (!m_cfg.cache_dir.empty())
		MakeDirs(m_cfg.cache_dir);
	// A thread of its own with a roomy stack (image decoding and font rasterizing run on it).
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 1024 * 1024);
	m_thread_started = pthread_create(&m_thread, &attr, &CoverService::ThreadMain, this) == 0;
	pthread_attr_destroy(&attr);
	if (!m_thread_started)
		m_finished = true;
}

void* CoverService::ThreadMain(void* self)
{
	CoverService* s = static_cast<CoverService*>(self);
	s->Run();
	s->m_finished = true;
	return nullptr;
}

bool CoverService::Stop(int timeout_ms)
{
	m_stop = true;
	if (!m_thread_started)
		return true;
	if (timeout_ms >= 0)
	{
		for (int waited = 0; !m_finished && waited < timeout_ms; waited += 10)
			usleep(10000);
		if (!m_finished)
			return false;
	}
	pthread_join(m_thread, nullptr);
	m_thread_started = false;
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
		if (ReadFile(f.path, bytes) && Decode(bytes, 1024, out))
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
// downloads go: m_offline.)
bool CoverService::MayDownload(int index) const
{
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	return !g.serial.empty() && m_cfg.allow_download && m_download && !m_cfg.url_template.empty() &&
		   (!m_cfg.download_usb_only || !UsbDriveRoot(g.path).empty() || OnNetworkShare(g.path)) && !RecentlyMissing(m_cfg.cache_dir, g.serial);
}

bool CoverService::DownloadCover(int index, CoverImage& out)
{
	const GameInfo& g = m_games[static_cast<size_t>(index)];
	if (m_offline || m_stop)
		return false;
	const std::string cached = m_cfg.cache_dir + "/" + g.serial + ".jpg";
	const std::string missing = m_cfg.cache_dir + "/" + g.serial + ".missing";
	std::vector<uint8_t> bytes;
	std::string url = m_cfg.url_template;
	const size_t at = url.find("${serial}");
	if (at != std::string::npos)
		url.replace(at, 9, g.serial);
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_busy = true;
	}
	bytes.clear();
	const int status = m_download(url, bytes);
	bool ok = false;
	if (status == 200 && Decode(bytes, 1024, out))
	{
		WriteFileAtomic(cached, bytes);
		out.source = "download";
		ok = true;
	}
	else if (status == 404)
		WriteFileAtomic(missing, std::vector<uint8_t>{'4', '0', '4', '\n'});
	else if (status < 0)
		m_offline = true;
	std::printf("[frontend] cover %s: %s -> %d%s\n", g.serial.c_str(), url.c_str(), status,
		ok ? " (saved)" :
		status >= 0 ? "" :
		m_stop ? " (stopped: the shelf closed)" :
				 " (network down; no more downloads this session)");
	std::fflush(stdout);
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
		if (!have && !RecentlyMissing(cfg.cache_dir, g.serial))
			missing.push_back(static_cast<int>(i));
	}
	return missing;
}

int CoverService::Prefetch(const std::vector<GameInfo>& games, const std::vector<int>& which, const CoverConfig& cfg,
	const DownloadFn& download, double budget_s)
{
	if (which.empty() || !download || cfg.url_template.empty())
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
	for (int index : which)
	{
		if (elapsed() >= budget_s)
		{
			std::printf("[frontend] prefetch: out of time after %.1f s; the rest next time\n", elapsed());
			break;
		}
		const GameInfo& g = games[static_cast<size_t>(index)];
		std::string url = cfg.url_template;
		const size_t at = url.find("${serial}");
		if (at != std::string::npos)
			url.replace(at, 9, g.serial);
		std::vector<uint8_t> bytes;
		const int status = download(url, bytes);
		CoverImage probe;
		bool ok = false;
		if (status == 200 && Decode(bytes, 1024, probe))
			ok = WriteFileAtomic(cfg.cache_dir + "/" + g.serial + ".jpg", bytes);
		else if (status == 404)
			WriteFileAtomic(cfg.cache_dir + "/" + g.serial + ".missing", std::vector<uint8_t>{'4', '0', '4', '\n'});
		std::printf("[frontend] prefetch %s: %d%s\n", g.serial.c_str(), status,
			ok ? " (saved)" : (status == 200 ? " (not an image; not kept)" : ""));
		std::fflush(stdout);
		saved += ok ? 1 : 0;
		if (status < 0)
			break; // the network is down: no point in trying the rest now
	}
	return saved;
}

void CoverService::Run()
{
	const size_t n = m_games.size();
	// Spines and placeholders first: they make every box presentable at once.
	// vk-285-157 (AI-assisted): throttle the queue so a large NFS library (e.g. 2287 games) doesn't
	// dump ~2.8 GB of undrained spine+placeholder images into RAM before the main thread can consume
	// them. Each spine is 96×1304 (~490 KB) and each placeholder is 364×512 (~730 KB); without a cap
	// a 2000-game library fills ~2.8 GB at once → memory pressure → frame stalls → watchdog KP.
	// The main thread drains 4 images per frame; keep at most 24 queued so it stays ahead.
	// needs proper testing
	static const size_t kMaxQueue = 24;
	std::vector<bool> done(n, false);
	for (size_t k = 0; k < n && !m_stop; k++)
	{
		const int i = NextGame(done);
		done[static_cast<size_t>(i)] = true;
		CoverImage spine, hold;
		PaintSpine(*m_fonts, m_games[static_cast<size_t>(i)], spine);
		PaintPlaceholder(*m_fonts, m_games[static_cast<size_t>(i)], hold);
		spine.game = hold.game = i;
		// Wait until the main thread has drained enough before pushing more.
		for (;;)
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (m_results.size() < kMaxQueue || m_stop)
				{
					m_results.push_back(std::move(spine));
					m_results.push_back(std::move(hold));
					break;
				}
			}
			usleep(5000); // 5 ms; yields CPU to the render thread
		}
	}
	// Then the covers already on disk, nearest the selection first (vk-285-110: all of them before any
	// download, so a slow or failing download never holds one back).
	auto deliver = [this](int i, CoverImage& cover) {
		cover.game = i;
		cover.kind = CoverImage::Cover;
		std::lock_guard<std::mutex> lock(m_mutex);
		m_results.push_back(std::move(cover));
	};
	CoverFinder finder(m_cfg.manual_dir, m_cfg.cache_dir);
	std::vector<bool> want(n, false); // a download is worth a try
	int found = 0, wanted = 0;
	std::string sources; // " (cache 11, beside 1)" for the log
	std::vector<std::pair<std::string, int>> by_source;
	done.assign(n, false);
	for (size_t k = 0; k < n && !m_stop; k++)
	{
		const int i = NextGame(done);
		done[static_cast<size_t>(i)] = true;
		CoverImage cover;
		bool upgrade = true;
		if (FindLocalCover(finder, i, cover, upgrade))
		{
			found++;
			auto it = std::find_if(by_source.begin(), by_source.end(), [&](const auto& s) { return s.first == cover.source; });
			if (it == by_source.end())
				by_source.emplace_back(cover.source, 1);
			else
				it->second++;
			deliver(i, cover);
		}
		want[static_cast<size_t>(i)] = upgrade && MayDownload(i);
		wanted += want[static_cast<size_t>(i)] ? 1 : 0;
	}
	for (const auto& s : by_source)
		sources += (sources.empty() ? " (" : ", ") + s.first + " " + std::to_string(s.second);
	if (!sources.empty())
		sources += ")";
	std::printf("[frontend] covers on disk: %d of %zu%s; %d to download%s\n", found, n, sources.c_str(), wanted,
		m_cfg.allow_download ? "" : " (downloads off here)");
	std::fflush(stdout);
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_download_total = wanted;
	}
	// Then the downloads, nearest the selection first, until the network fails. After the HEN jailbreak
	// HTTPS failed on vk-285-41/42; when it still does, the next start's prefetch fetches these.
	for (size_t i = 0; i < n; i++)
		done[i] = !want[i];
	for (int k = 0; k < wanted && !m_stop && !m_offline; k++)
	{
		const int i = NextGame(done);
		if (i < 0)
			break;
		done[static_cast<size_t>(i)] = true;
		CoverImage cover;
		if (DownloadCover(i, cover))
			deliver(i, cover);
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	m_busy = false;
}
} // namespace fe
