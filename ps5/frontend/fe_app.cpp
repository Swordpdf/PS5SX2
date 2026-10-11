// PS5 port frontend: the cover-flow shelf (see fe_app.h).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_app.h"
#include "fe_i18n.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>

extern "C" {
#include "third_party/qrcodegen/qrcodegen.h"
}

namespace fe
{
namespace
{
// The camera and the shelf's layout (world units: 1 = 100 mm).
const Vec3 kEye(0.0f, 0.30f, 7.8f);
const Vec3 kTarget(0.0f, -0.16f, 0.0f);
constexpr float kFovY = 30.0f;
constexpr float kFloorY = -kBoxHalfH;
constexpr float kCenterZ = 1.05f;   // the selected case, pulled forward
constexpr float kSideX = 1.86f;     // the first neighbour's centre
constexpr float kStepX = 0.56f;     // each further one
constexpr float kSideZ = -0.55f;
constexpr float kStepZ = 0.10f;
constexpr float kSideTurn = 58.0f;  // degrees the neighbours turn towards the centre
constexpr int kVisible = 7;         // cases drawn on each side
// 2026-10-10 (AI-assisted; a tester with ~2,600 NFS games had no covers): textures only for the games near the selection.
// Every game used to keep a spine, a placeholder and a cover (~2.7 MB): gigabytes for a large library, and past ~1,020
// games the renderer's descriptor pool (1,024 sets) ran out, so boxes weren't drawn. Needs proper testing on the console.
constexpr int kCoverKeep = 24; // games within this many places of the selection are asked for
constexpr int kCoverDrop = 32; // and dropped beyond this many (from the selection and the scroll), so going back and forth doesn't repaint

// The author's handles, under the wordmark (vk-285-50, the user's request).
constexpr const char* kDiscordHandle = "sword.pdf";
constexpr const char* kXHandle = "@sword_pdf";

uint32_t Rgba(float r, float g, float b, float a = 1.0f)
{
	auto c = [](float v) { return static_cast<uint32_t>(Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
	return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

// vk-285-110: in the PS5's language ("4,2 Go"; fe_i18n.cpp).
std::string SizeText(uint64_t bytes)
{
	return Size(bytes);
}

// live-22 (AI-assisted): a disc being copied is a shelf entry whose Init-list index is this (below every real index, 0..N-1),
// so it sorts to the front and the cover service, which keys on real indices, never touches it.
constexpr int kDumpIndex = -1;

// live-22 (AI-assisted, graphics approach from the dropped frontend of PR #34): a spinning disc drawn on the front of a box
// (model `model`), as UI triangles whose corners are the box's front points projected, so it lies on the box at any angle.
// `alpha` fades it on the boxes beside the selected one. Here it marks the game currently being dumped; the percentage is
// drawn over it by Build.
void AddDiscSpinner(std::vector<UiVertex>& ui, const Mat4& view_proj, const Mat4& model, float t, float alpha, float W, float H)
{
	if (alpha <= 0.01f)
		return;
	constexpr int kSegments = 72;
	constexpr float kPi = 3.14159265f;
	const float cx = 0.0f, cy = 0.18f, z = kBoxHalfD + 0.004f; // a little above centre; the percentage sits below it
	auto put = [&](float r, float a, uint32_t color) {
		const Vec4 w = model * Vec4(cx + r * std::cos(a), cy + r * std::sin(a), z, 1.0f);
		const Vec4 c = view_proj * w;
		UiVertex v = {((c.x / c.w) * 0.5f + 0.5f) * W, ((c.y / c.w) * 0.5f + 0.5f) * H, 0, 0, color, 1e5f, 1e5f, 0, 1};
		return v;
	};
	auto ring = [&](float r0, float r1, float from, float to, const auto& colour) {
		const int n = std::max(2, static_cast<int>(kSegments * (to - from) / (2 * kPi)));
		for (int k = 0; k < n; k++)
		{
			const float a0 = from + (to - from) * k / n, a1 = from + (to - from) * (k + 1) / n;
			const UiVertex p0 = put(r0, a0, colour(a0, 0.0f)), p1 = put(r1, a0, colour(a0, 1.0f));
			const UiVertex p2 = put(r1, a1, colour(a1, 1.0f)), p3 = put(r0, a1, colour(a1, 0.0f));
			ui.insert(ui.end(), {p0, p1, p2, p0, p2, p3});
		}
	};
	const float spin = t * 5.0f;
	const auto a8 = [&](float a) { return static_cast<uint32_t>(Clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f) << 24; };
	// The disc: silver with two glints that turn with it and a faint rainbow, the clear hub, and the hole.
	ring(0.13f, 0.40f, 0.0f, 2 * kPi, [&](float a, float outer) {
		const float g1 = std::pow(std::max(0.0f, std::cos(a - spin)), 10.0f), g2 = std::pow(std::max(0.0f, std::cos(a - spin - kPi)), 10.0f);
		const float glint = 0.55f * (g1 + g2) * (0.6f + 0.4f * outer);
		const float hue = a * 2.0f - spin;
		const float r = 0.62f + 0.10f * std::sin(hue) + glint, g = 0.64f + 0.10f * std::sin(hue + 2.1f) + glint,
					bl = 0.72f + 0.10f * std::sin(hue + 4.2f) + glint;
		return a8(alpha * 0.92f) | Rgba(Clamp(r, 0.0f, 1.0f), Clamp(g, 0.0f, 1.0f), Clamp(bl, 0.0f, 1.0f), 0);
	});
	ring(0.055f, 0.13f, 0.0f, 2 * kPi, [&](float, float) { return a8(alpha * 0.55f) | Rgba(0.85f, 0.88f, 0.95f, 0); });
	// The loading arc around it, a quarter turn long, going round faster than the disc.
	const float head = t * 3.2f;
	ring(0.44f, 0.465f, head, head + kPi * 0.6f, [&](float a, float) {
		const float tail = (a - head) / (kPi * 0.6f);
		return a8(alpha * (0.15f + 0.85f * tail)) | Rgba(1.0f, 1.0f, 1.0f, 0);
	});
}

// vk-285-114: multiplies the alpha of UI vertices [begin, end) by `f` (what the options sheet covers fades out).
void FadeRange(std::vector<UiVertex>& ui, size_t begin, size_t end, float f)
{
	if (f >= 1.0f)
		return;
	for (size_t i = begin; i < end && i < ui.size(); i++)
	{
		const uint32_t c = ui[i].color;
		const uint32_t a = static_cast<uint32_t>(static_cast<float>(c >> 24) * Clamp(f, 0.0f, 1.0f) + 0.5f);
		ui[i].color = (c & 0x00FFFFFFu) | (a << 24);
	}
}

// Screen position (0..1) of a world point.
Vec2 Project(const Mat4& view_proj, const Vec3& p)
{
	const Vec4 c = view_proj * Vec4(p.x, p.y, p.z, 1.0f);
	Vec2 out;
	out.x = (c.x / c.w) * 0.5f + 0.5f;
	out.y = (c.y / c.w) * 0.5f + 0.5f;
	return out;
}

std::string Fit(const Fonts& fonts, std::string text, float px, float width); // below; vk-285-134: Build uses it too
} // namespace

bool App::Init(Renderer* renderer, const Fonts* fonts, std::vector<GameInfo> games, CoverService* covers, const AppConfig& cfg)
{
	m_renderer = renderer;
	m_fonts = fonts;
	m_covers = covers;
	m_cfg = cfg;
	m_games = std::move(games);
	m_slots.assign(m_games.size(), Slot());
	m_selected = m_games.empty() ? 0 : std::max(0, std::min(cfg.preselect, static_cast<int>(m_games.size()) - 1));
	// vk-285-137: the games hidden from the shelf wait off it (ApplyHidden); the preselected one may be one of them.
	m_index.resize(m_games.size());
	for (size_t i = 0; i < m_index.size(); i++)
		m_index[i] = static_cast<int>(i);
	m_shelved.clear();
	m_show_hidden = !cfg.options.gs_ini.empty() && ShowHiddenGames(cfg.options.gs_ini);
	if (!m_games.empty())
	{
		const size_t all = m_games.size();
		const size_t hidden = static_cast<size_t>(std::count_if(m_games.begin(), m_games.end(), [](const GameInfo& g) { return g.hidden; }));
		ApplyHidden(m_selected);
		if (hidden > 0)
			std::printf("[frontend] %zu of %zu games hidden from the shelf%s\n", hidden, all,
				m_show_hidden ? " (shown, dimmed: Show hidden games is on)" : "");
	}
	m_scroll = static_cast<float>(m_selected);
	m_atlas = m_renderer->CreateTexture(static_cast<uint32_t>(fonts->AtlasWidth()), static_cast<uint32_t>(fonts->AtlasHeight()),
		VK_FORMAT_R8_UNORM, fonts->AtlasPixels().data());
	if (!m_atlas)
		return false;
	m_renderer->SetAtlas(m_atlas);
	if (m_covers && !m_games.empty())
		m_covers->SetSelected(m_index[static_cast<size_t>(m_selected)]);
	// vk-285-134: main-boot.cpp looked for the BIOS just before the shelf; what it found is shown until a pick looks again.
	m_bios_problem = m_cfg.bios_problem ? m_cfg.bios_problem() : std::string();
	m_bios_missing = !m_bios_problem.empty();
	return true;
}

// vk-285-134 (AI-assisted): a game (or the PS2 system menu) starts only with a BIOS. Looked for again at each pick, as a BIOS
// may have been copied over meanwhile (the settings page, FTP); without one the pick stays on the shelf and the line says why.
bool App::BiosReady()
{
	if (!m_cfg.bios_present)
		return true;
	if (m_cfg.bios_present())
	{
		m_bios_missing = false;
		m_bios_problem.clear();
		return true;
	}
	m_bios_missing = true;
	m_bios_problem = m_cfg.bios_problem ? m_cfg.bios_problem() : std::string();
	m_bios_refused = m_time;
	std::printf("[frontend] no PS2 BIOS: the game stays on the shelf (%s)\n", m_bios_problem.c_str());
	std::fflush(stdout);
	Sound(Sfx::Edge, 0.0f);
	return false;
}


void App::SetWebUrl(const std::string& url, const std::string& shown)
{
	m_web_known = true;
	if (url == m_web_url && shown == m_web_shown)
		return;
	m_web_url = url;
	m_web_shown = shown;
	m_qr.clear();
	m_qr_size = 0;
	if (url.empty())
		return;
	// Byte mode up to version 10 (57 modules) is plenty for "http://255.255.255.255:65535/" (vk-285-118: no key
	// after it any more); medium error correction.
	uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
	if (!qrcodegen_encodeText(url.c_str(), tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true))
	{
		std::printf("[frontend] QR: \"%s\" does not fit\n", url.c_str());
		return;
	}
	m_qr_size = qrcodegen_getSize(qr);
	m_qr.resize(static_cast<size_t>(m_qr_size) * m_qr_size);
	for (int y = 0; y < m_qr_size; y++)
		for (int x = 0; x < m_qr_size; x++)
			m_qr[static_cast<size_t>(y) * m_qr_size + x] = qrcodegen_getModule(qr, x, y) ? 1 : 0;
}

void App::Shutdown()
{
	if (m_cfg.game_achievements.cancel)
		m_cfg.game_achievements.cancel();
	ClearAchievementBadges();
	m_account.Close();
	for (Shelved& h : m_shelved) // vk-285-137
		m_slots.push_back(h.slot);
	m_shelved.clear();
	for (Slot& s : m_slots)
	{
		m_renderer->FreeTextureSet(s.set);
		m_renderer->DestroyTexture(s.cover);
		m_renderer->DestroyTexture(s.placeholder);
		m_renderer->DestroyTexture(s.spine);
		s = Slot();
	}
	m_renderer->SetAtlas(nullptr);
	m_renderer->DestroyTexture(m_atlas);
	m_atlas = nullptr;
}

bool App::Step(int dir)
{
	if (m_games.empty())
		return false;
	const int n = static_cast<int>(m_games.size());
	const int next = std::max(0, std::min(n - 1, m_selected + dir));
	if (next == m_selected)
		return false;
	m_selected = next;
	m_select_time = m_time;
	if (m_covers && m_index[static_cast<size_t>(m_selected)] >= 0) // live-22: the dump entry (kDumpIndex) isn't the service's
		m_covers->SetSelected(m_index[static_cast<size_t>(m_selected)]);
	return true;
}

int App::PositionOf(int index) const
{
	const auto it = std::lower_bound(m_index.begin(), m_index.end(), index); // m_index keeps Init's order
	return it != m_index.end() && *it == index ? static_cast<int>(it - m_index.begin()) : -1;
}

bool App::ApplyHidden(int keep)
{
	struct Entry
	{
		GameInfo game;
		Slot slot;
		int index;
	};
	std::vector<Entry> all;
	all.reserve(m_games.size() + m_shelved.size());
	for (size_t i = 0; i < m_games.size(); i++)
		all.push_back({std::move(m_games[i]), m_slots[i], m_index[i]});
	for (Shelved& h : m_shelved)
		all.push_back({std::move(h.game), h.slot, h.index});
	std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) { return a.index < b.index; });
	const std::vector<int> was = m_index;
	m_games.clear();
	m_slots.clear();
	m_index.clear();
	m_shelved.clear();
	int at = -1, after = -1, before = -1;
	for (Entry& e : all)
	{
		if (e.game.hidden && !m_show_hidden)
		{
			m_shelved.push_back({std::move(e.game), e.slot, e.index});
			continue;
		}
		const int pos = static_cast<int>(m_games.size());
		if (e.index == keep)
			at = pos;
		else if (e.index > keep && after < 0)
			after = pos;
		else if (e.index < keep)
			before = pos;
		m_games.push_back(std::move(e.game));
		m_slots.push_back(e.slot);
		m_index.push_back(e.index);
	}
	m_selected = at >= 0 ? at : after >= 0 ? after : before >= 0 ? before : 0;
	if (m_index == was)
		return false;
	m_scroll = static_cast<float>(m_selected);
	m_scroll_vel = 0;
	m_select_time = m_time;
	if (m_covers && !m_games.empty() && m_index[static_cast<size_t>(m_selected)] >= 0) // live-22: skip the dump entry
		m_covers->SetSelected(m_index[static_cast<size_t>(m_selected)]);
	return true;
}

void App::Sound(Sfx sfx, float pan)
{
	if (m_cfg.sound)
		m_cfg.sound->Play(sfx, pan);
}

void App::DropCovers(Slot& s, int index)
{
	m_renderer->FreeTextureSet(s.set);
	m_renderer->DestroyTexture(s.cover);
	m_renderer->DestroyTexture(s.placeholder);
	m_renderer->DestroyTexture(s.spine);
	s = Slot();
	m_covers->Forget(index);
}

void App::KeepCoversNear()
{
	const int n = static_cast<int>(m_games.size());
	if (n == 0)
		return;
	const int scroll = static_cast<int>(std::lround(m_scroll));
	for (int pos = 0; pos < n; pos++)
		if (m_index[static_cast<size_t>(pos)] >= 0 && m_slots[static_cast<size_t>(pos)].wanted &&
			std::abs(pos - m_selected) > kCoverDrop && std::abs(pos - scroll) > kCoverDrop) // live-22: never the dump entry
			DropCovers(m_slots[static_cast<size_t>(pos)], m_index[static_cast<size_t>(pos)]);
	for (Shelved& h : m_shelved) // vk-285-137: off the shelf, not drawn
		if (h.slot.wanted)
			DropCovers(h.slot, h.index);
	// Nearest first, so the service (which also goes nearest first) has the visible ones at the front.
	for (int d = 0; d <= kCoverKeep; d++)
		for (int pos : {m_selected - d, m_selected + d})
			if (pos >= 0 && pos < n && m_index[static_cast<size_t>(pos)] >= 0 && !m_slots[static_cast<size_t>(pos)].wanted) // live-22
			{
				m_slots[static_cast<size_t>(pos)].wanted = true;
				m_covers->Want(m_index[static_cast<size_t>(pos)]);
			}
}

void App::PollCovers()
{
	if (!m_covers)
		return;
	KeepCoversNear();
	CoverImage img;
	int budget = 4; // textures a frame
	while (budget-- > 0 && m_covers->Poll(img))
	{
		// vk-285-137: img.game is an index in Init's list; the game may be off the shelf (its slot keeps the covers).
		Slot* slot = nullptr;
		const GameInfo* game = nullptr;
		if (const int pos = PositionOf(img.game); pos >= 0)
		{
			slot = &m_slots[static_cast<size_t>(pos)];
			game = &m_games[static_cast<size_t>(pos)];
		}
		else
			for (Shelved& h : m_shelved)
				if (h.index == img.game)
				{
					slot = &h.slot;
					game = &h.game;
					break;
				}
		if (!slot || !slot->wanted) // 2026-10-10: dropped since (KeepCoversNear)
			continue;
		Slot& s = *slot;
		Texture* t = m_renderer->CreateTexture(static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height),
			VK_FORMAT_R8G8B8A8_UNORM, img.rgba.data());
		if (!t)
			continue;
		Texture** dst = img.kind == CoverImage::Spine ? &s.spine : img.kind == CoverImage::Placeholder ? &s.placeholder : &s.cover;
		m_renderer->DestroyTexture(*dst);
		*dst = t;
		if (img.kind == CoverImage::Cover)
		{
			s.has_cover = true;
			if (img.has_glow)
			{
				s.glow[0] = img.glow[0];
				s.glow[1] = img.glow[1];
				s.glow[2] = img.glow[2];
				s.has_glow = true;
			}
			std::printf("[frontend] cover for %s (%s, %dx%d)\n", game->title.c_str(), img.source, img.width, img.height);
		}
		s.dirty = true;
	}
	for (Slot& s : m_slots)
		if (s.dirty && s.spine && s.placeholder)
		{
			m_renderer->FreeTextureSet(s.set);
			s.set = m_renderer->AllocTextureSet(s.cover ? s.cover : s.placeholder, s.placeholder, s.spine);
			s.dirty = !s.set; // 2026-10-10: the pool full for now (sets freed are given back a frame later): try again
		}
}

// live-22 (AI-assisted): the dump entry's position on the shelf (its index is kDumpIndex), or -1.
int App::DumpPos() const
{
	for (size_t i = 0; i < m_index.size(); i++)
		if (m_index[i] == kDumpIndex)
			return static_cast<int>(i);
	return -1;
}

// live-22 (AI-assisted): a synthetic shelf entry for the disc being copied, at the front, selected, with its box painted
// (no title: the spinning disc goes there) and its spine carrying the title. Its index is kDumpIndex, so the cover service
// leaves it alone. The percentage is drawn over the box by Build.
void App::InsertDumpEntry(const std::string& serial, const std::string& title, int pct)
{
	const std::string disp = title.empty() ? std::string("PS2 disc") : title;
	GameInfo g;
	g.path = "@disc-dump"; // a sentinel path: this entry is never launched
	g.serial = serial;
	g.title = disp;
	Slot s;
	CoverImage ph, sp;
	GameInfo front = g;
	front.title.clear(); // the box front holds the spinning disc, not the title
	CoverService::PaintPlaceholder(*m_fonts, front, ph);
	CoverService::PaintSpine(*m_fonts, g, sp);
	s.placeholder = m_renderer->CreateTexture(static_cast<uint32_t>(ph.width), static_cast<uint32_t>(ph.height),
		VK_FORMAT_R8G8B8A8_UNORM, ph.rgba.data());
	s.spine = m_renderer->CreateTexture(static_cast<uint32_t>(sp.width), static_cast<uint32_t>(sp.height),
		VK_FORMAT_R8G8B8A8_UNORM, sp.rgba.data());
	if (s.placeholder && s.spine)
	{
		s.set = m_renderer->AllocTextureSet(s.placeholder, s.placeholder, s.spine);
		s.dirty = !s.set; // the descriptor pool may be full for a frame; PollCovers retries
	}
	else
		s.dirty = true;
	m_games.insert(m_games.begin(), std::move(g));
	m_slots.insert(m_slots.begin(), std::move(s));
	m_index.insert(m_index.begin(), kDumpIndex);
	// Select the new entry so its spinner and percentage are front and centre; the shelf springs over from where it was.
	m_scroll += 1.0f;
	m_selected = 0;
	m_scroll_vel = 0;
	m_select_time = m_time;
	m_dump_on = true;
	m_dump_serial = serial;
	m_dump_title = disp;
	m_dump_pct = pct;
	Sound(Sfx::Move, 0.0f);
	std::printf("[frontend] disc dump: %s (%s) %d%% on the shelf\n", disp.c_str(), serial.c_str(), pct);
	std::fflush(stdout);
}

// live-22 (AI-assisted): drop the dump entry (the copy ended). The game either launches at once (the dumper's launch
// request) or appears on the shelf at the next open, as it did before.
void App::RemoveDumpEntry()
{
	const int pos = DumpPos();
	m_dump_on = false;
	if (pos < 0)
		return;
	Slot& s = m_slots[static_cast<size_t>(pos)];
	m_renderer->FreeTextureSet(s.set);
	m_renderer->DestroyTexture(s.cover);
	m_renderer->DestroyTexture(s.placeholder);
	m_renderer->DestroyTexture(s.spine);
	m_games.erase(m_games.begin() + pos);
	m_slots.erase(m_slots.begin() + pos);
	m_index.erase(m_index.begin() + pos);
	if (m_selected > pos)
		m_selected--;
	const int n = static_cast<int>(m_games.size());
	m_selected = n == 0 ? 0 : std::max(0, std::min(m_selected, n - 1));
	m_scroll = static_cast<float>(m_selected);
	m_scroll_vel = 0;
	m_select_time = m_time;
	if (m_covers && n > 0 && m_index[static_cast<size_t>(m_selected)] >= 0)
		m_covers->SetSelected(m_index[static_cast<size_t>(m_selected)]);
	std::printf("[frontend] disc dump: entry removed\n");
	std::fflush(stdout);
}

// live-22 (AI-assisted): read the dumper's status file (throttled) and keep the dump entry in step with it.
void App::PollDiscDump()
{
	if (m_cfg.disc_dump_progress.empty() || m_time - m_dump_poll < 0.5)
		return;
	m_dump_poll = m_time;
	std::string serial, title;
	int pct = -1;
	bool active = false;
	struct stat st;
	// A file last written in the last 20 s: a live copy (every ~1% write refreshes its mtime). An older one is a copy that
	// crashed; it is treated as finished so the entry doesn't linger.
	if (stat(m_cfg.disc_dump_progress.c_str(), &st) == 0 && std::time(nullptr) - st.st_mtime < 20)
	{
		if (FILE* f = std::fopen(m_cfg.disc_dump_progress.c_str(), "r"))
		{
			char ser[80] = {0}, line[256] = {0};
			if (std::fscanf(f, "%79s %d ", ser, &pct) == 2 && pct >= 0 && pct <= 100)
			{
				serial = ser;
				if (std::fgets(line, sizeof(line), f))
				{
					size_t len = std::strlen(line);
					while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
						line[--len] = 0;
					title = line;
				}
				active = true;
			}
			std::fclose(f);
		}
	}
	if (active)
	{
		if (m_dump_on && serial != m_dump_serial)
			RemoveDumpEntry(); // a different disc is being copied now: start fresh
		if (!m_dump_on)
			InsertDumpEntry(serial, title, pct);
		else
			m_dump_pct = pct;
	}
	else if (m_dump_on)
		RemoveDumpEntry();
}

void App::Update(double dt, const Input& in)
{
	m_time += dt;
	m_account.Poll(m_cfg.achievements);
	PollGameAchievements();
	const float fdt = static_cast<float>(std::min(dt, 0.1));
	m_dump_spin += fdt; // live-22: the dump disc's rotation, advanced whether or not a dump is on

	// 2026-10-05: the account panel fades in and out, and its keyboard slides up under it (AI-assisted).
	{
		const float fade = fdt / 0.16f;
		m_account_anim = m_account.open ? std::min(1.0f, m_account_anim + fade) : std::max(0.0f, m_account_anim - fade);
		m_account_kb_anim += ((m_account.open && m_account.editing ? 1.0f : 0.0f) - m_account_kb_anim) * (1.0f - std::exp(-fdt * 16.0f));
	}

	// vk-285-114: the options sheet slides in and out; while it is open it has the buttons.
	{
		const float target = m_sheet_open ? 1.0f : 0.0f;
		const float step = fdt / 0.22f;
		m_sheet_anim = m_sheet_anim < target ? std::min(target, m_sheet_anim + step) : std::max(target, m_sheet_anim - step);
		const float a = 1.0f - std::exp(-fdt * 14.0f);
		m_sheet_scroll += (m_sheet_scroll_target - m_sheet_scroll) * a;
	}

	if (m_launching)
	{
		if (m_game_achievements.busy)
			m_launch_time = m_time;
		if (m_time - m_launch_time > 0.6 && !m_game_achievements.busy)
			m_done = true;
	}
	else if (m_account.open)
	{
		UpdateAccount(in);
		m_prev = in;
	}
	else if (m_sheet_open && m_cfg.achievements.state && in.square && in.l1 && !m_prev.l1 && m_prev.square &&
	         m_time - m_chord_square_time < kChordWindow)
	{
		// pr9n: Square, then L1 while Square is still held: the account chord, not the sheet's This game / All games.
		CloseSheet();
		m_sheet_anim = 0;
		m_account.Open();
		m_ime_field = -1;
		m_held = 0;
		m_prev = in;
	}
	else if (m_sheet_open)
	{
		UpdateSheet(dt, in);
		m_prev = in;
	}
	else if (m_released && !m_qr_big && m_sheet_anim < 0.05f && m_cfg.achievements.state &&
	         in.l1 && in.square && !(m_prev.l1 && m_prev.square))
	{
		// Handle the account chord before shelf navigation or settings (AI-assisted).
		// pr9n: L1 pressed first already jumped 5 games: put the selection back.
		if (m_prev.l1 && !m_prev.square && m_chord_from >= 0 && m_time - m_chord_l1_time < kChordWindow)
			Step(std::min(m_chord_from, static_cast<int>(m_games.size()) - 1) - m_selected);
		m_chord_from = -1;
		m_account.Open();
		m_ime_field = -1;
		m_held = 0;
		m_prev = in;
	}
	else
	{
		const bool any = in.left || in.right || in.cross || in.options || in.l1 || in.r1 || in.up || in.down || in.square || in.triangle ||
		                 in.circle;
		if (!m_released)
			m_released = !any;
		else
		{
			const int dir = in.left ? -1 : in.right ? 1 : 0;
			const bool fresh = (in.left && !m_prev.left) || (in.right && !m_prev.right);
			// The sounds lean a little towards the side pressed.
			const float pan = 0.18f * static_cast<float>(dir);
			if (dir != 0 && fresh)
			{
				Sound(Step(dir) ? Sfx::Move : Sfx::Edge, pan);
				m_held = dir;
				m_held_for = 0;
				m_next_repeat = 0.36;
			}
			else if (dir != 0 && dir == m_held)
			{
				m_held_for += dt;
				bool moved = false; // one sound a frame, however many steps a slow frame took
				while (m_held_for >= m_next_repeat)
				{
					moved |= Step(dir);
					m_next_repeat += m_held_for > 1.5 ? 0.055 : 0.11;
				}
				if (moved)
					Sound(Sfx::MoveRepeat, pan);
			}
			else
				m_held = 0;
			if (in.l1 && !m_prev.l1)
			{
				m_chord_from = m_selected; // pr9n: undone if Square follows (the account chord)
				m_chord_l1_time = m_time;
				Sound(Step(-5) ? Sfx::JumpLeft : Sfx::Edge, -0.25f);
			}
			if (in.r1 && !m_prev.r1)
				Sound(Step(5) ? Sfx::JumpRight : Sfx::Edge, 0.25f);
			if (((in.cross && !m_prev.cross) || (in.options && !m_prev.options)) && !m_games.empty() && !m_qr_big)
			{
				const GameInfo& picked = m_games[static_cast<size_t>(m_selected)];
				if (m_index[static_cast<size_t>(m_selected)] == kDumpIndex) // live-22: the disc is still being copied
				{
					m_refused_text = "This disc is still being copied (" + std::to_string(m_dump_pct) + "%).";
					m_refused_time = m_time;
					Sound(Sfx::Edge, 0.0f);
				}
				else if (!picked.damaged.empty()) // vk-285-134: an image that can't be read stays on the shelf
				{
					m_refused_text = "This image can't be read (" + picked.damaged +
					                 "): copy it again, or make the CHD again with chdman from a good copy.";
					m_refused_time = m_time;
					std::printf("[frontend] %s can't be read (%s): not started\n", picked.file.c_str(), picked.damaged.c_str());
					std::fflush(stdout);
					Sound(Sfx::Edge, 0.0f);
				}
				else if (BiosReady()) // vk-285-134
				{
					if (m_cfg.game_achievements.cancel)
						m_cfg.game_achievements.cancel();
					m_launching = true;
					m_launch_time = m_time;
					Sound(Sfx::Launch, 0.0f);
				}
			}
			// Triangle keeps the upstream QR view; Circle opens the RA account (AI-assisted).
			else if (m_qr_big && ((in.triangle && !m_prev.triangle) || (in.circle && !m_prev.circle)))
			{
				m_qr_big = false;
				Sound(Sfx::Move, -0.3f);
			}
			else if (!m_qr_big && (in.triangle && !m_prev.triangle) && m_qr_size > 0 && m_sheet_anim < 0.05f)
			{
				m_qr_big = true;
				Sound(Sfx::Move, 0.3f);
			}
			// vk-285-114: Square opens the selected game's options sheet (the settings for all games when there is no game).
			else if (!m_qr_big && in.square && !m_prev.square && m_sheet_anim < 0.05f)
			{
				OpenSheet(m_games.empty());
				m_chord_square_time = m_time; // pr9n: L1 right after makes it the account chord
				Sound(Sfx::Move, 0.3f);
			}
		}
		m_prev = in;
	}

	m_qr_big_anim += (m_qr_big ? 1.0f : -1.0f) * fdt / 0.18f; // vk-285-118
	m_qr_big_anim = std::min(1.0f, std::max(0.0f, m_qr_big_anim));
	if (m_qr_size <= 0)
		m_qr_big = false;

	// The shelf follows the selection on a critically damped spring.
	const float k = 150.0f, c = 2.0f * std::sqrt(k);
	for (int i = 0; i < 4; i++)
	{
		const float h = fdt / 4;
		const float acc = k * (static_cast<float>(m_selected) - m_scroll) - c * m_scroll_vel;
		m_scroll_vel += acc * h;
		m_scroll += m_scroll_vel * h;
	}

	PollDiscDump(); // live-22: the disc being copied shows as a spinning-disc entry at the front
	PollCovers();
	for (Slot& s : m_slots)
		if (s.has_cover && s.cover_mix < 1.0f)
			s.cover_mix = std::min(1.0f, s.cover_mix + fdt / 0.35f);

	const float house[3] = {0.55f, 0.42f, 1.0f};
	float want[3] = {house[0], house[1], house[2]};
	if (!m_slots.empty() && m_slots[static_cast<size_t>(m_selected)].has_glow)
		for (int i = 0; i < 3; i++)
			want[i] = Mix(m_slots[static_cast<size_t>(m_selected)].glow[i], house[i], 0.35f);
	const float a = 1.0f - std::exp(-fdt * 5.0f);
	for (int i = 0; i < 3; i++)
		m_glow[i] += (want[i] - m_glow[i]) * a;
}

void App::Pose(float d, float t, Mat4& model, float& brightness) const
{
	const float ad = std::fabs(d);
	const float s = d < 0 ? -1.0f : 1.0f;
	const float e = Smoothstep(0.0f, 1.0f, std::min(ad, 1.0f));
	const float far_ = std::max(ad - 1.0f, 0.0f);
	const float side_x = kSideX + far_ * kStepX;
	const float side_z = kSideZ - far_ * kStepZ;
	const float sway = (1.0f - e) * 0.10f * std::sin(t * 0.7f);
	// The picked case hovers: it lifts off the floor as it is picked, then bobs between 0.03 and
	// 0.054 above it. Resting on the floor at the bottom of the bob (vk-285-46) still put the
	// halo's bottom line, drawn 0.016 outside the case, into the reflection.
	const float bob = (1.0f - e) * (0.030f + 0.012f * (1.0f + std::sin(t * 1.3f)));
	float x = s * Mix(0.0f, side_x, e);
	float z = Mix(kCenterZ, side_z, e);
	const float turn = Mix(sway, -s * Radians(kSideTurn), e);
	if (m_launching && !m_system_menu && ad < 0.5f) // 2026-10-08: the system menu isn't the selected game
	{
		const float l = Smoothstep(0.0f, 0.6f, static_cast<float>(m_time - m_launch_time));
		z += l * 2.2f;
	}
	model = Mat4::Translate(x, bob, z) * Mat4::RotateY(turn);
	brightness = Mix(1.0f, 0.74f, e) - 0.05f * std::min(far_, 5.0f);
}

void App::Build(FrameDesc& f, const std::string& clock)
{
	m_achievement_images.clear();
	f.ui_images.clear();
	const float W = static_cast<float>(m_renderer->width()), H = static_cast<float>(m_renderer->height());
	const float k = H / 2160.0f;
	const float t = static_cast<float>(m_time);
	const Mat4 proj = Mat4::Perspective(Radians(kFovY), W / H, 0.1f, 60.0f);
	// vk-285-114: with the options sheet open on the right, the shelf slides left (the camera moves right), so the
	// picked case stays in view beside its settings.
	const float slide = Smoothstep(0.0f, 1.0f, m_sheet_anim);
	const Vec3 shift(1.62f * slide, 0.0f, 0.0f); // 2026-10-05: 1.45 before the sheet widened
	const Mat4 view = Mat4::LookAt(kEye + shift, kTarget + shift, Vec3(0, 1, 0));
	f.view_proj = proj * view;
	f.cam_pos = kEye + shift;
	f.time = t;
	f.glow[0] = m_glow[0];
	f.glow[1] = m_glow[1];
	f.glow[2] = m_glow[2];
	f.glow[3] = 1.0f;
	f.floor_y = kFloorY;
	f.reflect_strength = 0.24f;
	f.reflect_falloff = 3.6f;
	f.boxes.clear();
	f.reflections.clear();
	f.ui.clear();
	f.halo = HaloDraw();

	// Background: the glow sits behind the selected case, the floor glow under it.
	const Vec2 centre = Project(f.view_proj, Vec3(0, 0.1f, kCenterZ - 0.6f));
	const Vec2 floor_pt = Project(f.view_proj, Vec3(0, kFloorY, kCenterZ));
	BgParams& bg = f.bg;
	bg.glow_color[0] = m_glow[0];
	bg.glow_color[1] = m_glow[1];
	bg.glow_color[2] = m_glow[2];
	bg.glow_color[3] = 0.50f;
	bg.glow_pos[0] = centre.x;
	bg.glow_pos[1] = centre.y;
	bg.glow_pos[2] = 0.58f;
	bg.glow_pos[3] = 0.40f;
	bg.floor_glow[0] = floor_pt.x;
	bg.floor_glow[1] = floor_pt.y;
	bg.floor_glow[2] = 0.30f;
	bg.floor_glow[3] = 0.035f;
	const float top[4] = {0.030f, 0.034f, 0.095f, 1}, mid[4] = {0.055f, 0.055f, 0.155f, 1},
				bottom[4] = {0.010f, 0.010f, 0.026f, 1};
	std::copy(top, top + 4, bg.top);
	std::copy(mid, mid + 4, bg.mid);
	std::copy(bottom, bottom + 4, bg.bottom);
	bg.misc[0] = W / H;
	bg.misc[1] = floor_pt.y;
	bg.misc[2] = 0.55f;
	bg.misc[3] = t;

	// The cases, nearest the selection last so the halo lands on top of its neighbours.
	const int n = static_cast<int>(m_games.size());
	const int first = std::max(0, static_cast<int>(std::floor(m_scroll)) - kVisible);
	const int last = std::min(n - 1, static_cast<int>(std::ceil(m_scroll)) + kVisible);
	const float since = static_cast<float>(m_time - m_select_time);
	for (int i = first; i <= last; i++)
	{
		const Slot& s = m_slots[static_cast<size_t>(i)];
		if (!s.set)
			continue;
		const float d = static_cast<float>(i) - m_scroll;
		BoxDraw b;
		Pose(d, t, b.model, b.brightness);
		if (m_games[static_cast<size_t>(i)].hidden) // vk-285-137: shown while Show hidden games is on, dimmed
			b.brightness *= 0.4f;
		b.set = s.set;
		b.cover_mix = s.cover_mix;
		b.selected = std::max(0.0f, 1.0f - std::fabs(d) * 2.0f);
		if (i == m_selected && since < 1.2f)
		{
			b.sheen_pos = -0.3f + since * 1.6f;
			b.sheen_strength = 0.16f * (1.0f - Smoothstep(0.6f, 1.2f, since));
		}
		f.boxes.push_back(b);
		BoxDraw r = b;
		r.model = Mat4::Translate(0, 2 * kFloorY, 0) * Mat4::Scale(1, -1, 1) * b.model;
		r.sheen_strength = 0;
		f.reflections.push_back(r);
		// live-22: the disc being copied: a spinning disc and the live percentage over its box.
		if (m_index[static_cast<size_t>(i)] == kDumpIndex)
		{
			const float a = std::max(0.0f, 1.0f - std::fabs(d) * 1.6f) * b.brightness;
			AddDiscSpinner(f.ui, f.view_proj, b.model, m_dump_spin, a, W, H);
			const Vec4 wp = b.model * Vec4(0.0f, -0.30f, kBoxHalfD + 0.01f, 1.0f);
			const Vec4 cp = f.view_proj * wp;
			const float sx = ((cp.x / cp.w) * 0.5f + 0.5f) * W, sy = ((cp.y / cp.w) * 0.5f + 0.5f) * H;
			char pctbuf[8];
			std::snprintf(pctbuf, sizeof(pctbuf), "%d%%", m_dump_pct);
			const float ppx = (54.0f + 60.0f * b.selected) * k;
			m_fonts->AddText(f.ui, pctbuf, sx, sy, ppx, Rgba(1.0f, 1.0f, 1.0f, std::min(1.0f, a * 1.4f)), 0.6f, Fonts::Center);
		}
	}

	// The outline glow on the selected case once the shelf settles on it.
	const float settle = 1.0f - Clamp(std::fabs(m_scroll - static_cast<float>(m_selected)) * 2.5f, 0.0f, 1.0f);
	if (n > 0 && settle > 0.0f && m_slots[static_cast<size_t>(m_selected)].set && !m_launching)
	{
		float bright;
		Pose(static_cast<float>(m_selected) - m_scroll, t, f.halo.model, bright);
		f.halo.enabled = true;
		f.halo.half_w = kBoxHalfW;
		f.halo.half_h = kBoxHalfH;
		f.halo.z = kBoxHalfD - 0.004f;
		f.halo.margin = 0.22f;
		f.halo.radius = 0.035f;
		f.halo.line = 0.0065f;
		f.halo.glow_width = 0.035f;
		f.halo.color[0] = Mix(m_glow[0], 1.0f, 0.8f);
		f.halo.color[1] = Mix(m_glow[1], 1.0f, 0.8f);
		f.halo.color[2] = Mix(m_glow[2], 1.0f, 0.8f);
		f.halo.color[3] = settle * (0.50f + 0.08f * std::sin(t * 2.2f));
	}

	// Fade in at start, out when launching.
	f.fade = std::min(1.0f, t / 0.35f);
	if (m_launching)
		f.fade *= 1.0f - Smoothstep(0.1f, 0.6f, static_cast<float>(m_time - m_launch_time));

	// ---- UI ----
	const uint32_t white = Rgba(1, 1, 1), dim = Rgba(0.72f, 0.69f, 0.82f), faint = Rgba(0.55f, 0.53f, 0.66f);
	const uint32_t accent = Rgba(Mix(m_glow[0], 1.0f, 0.3f), Mix(m_glow[1], 1.0f, 0.3f), Mix(m_glow[2], 1.0f, 0.3f));
	const float margin = 110.0f * k;
	std::vector<UiVertex>& ui = f.ui;

	// Wordmark (vk-285-50: PS5SX2, "SX2" in the glow's colour) and the author's handles under it.
	float x = margin;
	x += m_fonts->AddText(ui, "PS5", x, 150.0f * k, 60.0f * k, white, 0.8f);
	m_fonts->AddText(ui, "SX2", x + 2.0f * k, 150.0f * k, 60.0f * k, accent, 0.8f);
	{
		const float hpx = 32.0f * k, hy2 = 214.0f * k;
		float hx2 = margin + 2.0f * k;
		const bool icons = m_fonts->Has(icon::Discord) && m_fonts->Has(icon::XTwitter);
		if (icons)
			hx2 += m_fonts->AddText(ui, icon::Discord, hx2, hy2 + 2.0f * k, hpx * 1.05f, faint) + 12.0f * k;
		hx2 += m_fonts->AddText(ui, kDiscordHandle, hx2, hy2, hpx, faint, 0.1f) + 40.0f * k;
		if (icons)
			hx2 += m_fonts->AddText(ui, icon::XTwitter, hx2, hy2 + 2.0f * k, hpx * 0.95f, faint) + 12.0f * k;
		m_fonts->AddText(ui, kXHandle, hx2, hy2, hpx, faint, 0.1f);
	}
	if (!clock.empty())
		m_fonts->AddText(ui, clock.c_str(), W - margin, 150.0f * k, 58.0f * k, white, 0.2f, Fonts::Right);
	if (m_cfg.achievements.state)
	{
		std::string label = "RetroAchievements - ";
		label += m_account.account.saved ? m_account.account.username : Tr(Str::AccountSignIn);
		m_fonts->AddText(ui, label.c_str(), margin, 290.0f * k, 34.0f * k, dim);
	}
	BuildTexturePackActivity(ui, margin, (m_cfg.achievements.state ? 350.0f : 290.0f) * k, k, accent); // 2026-10-05
	BuildBiosLine(ui, W, k); // vk-285-134

	// The settings page's QR tile, bottom right (vk-285-50). The code is dark on a light tile, as
	// cameras expect; neighbouring dark modules are merged into runs and grown by a pixel so the
	// rectangles' anti-aliased edges leave no seams between them. vk-285-51: smaller and dimmer (the
	// user's choice: "less"); it still scans from a metre or so, and a phone keeps the page's key
	// once it has opened it, so the code is mostly needed once.
	const bool qr_shown = m_qr_size > 0;
	const size_t qr_begin = ui.size();
	if (qr_shown)
	{
		const float tile = 220.0f * k;
		const int quiet = 4; // the standard quiet zone
		const float mod = std::floor(tile / static_cast<float>(m_qr_size + 2 * quiet));
		const float side = mod * static_cast<float>(m_qr_size + 2 * quiet);
		const float tx = std::floor(W - margin - side), ty = std::floor(H - 150.0f * k - side);
		Fonts::AddRoundedRect(ui, tx, ty, side, side, 12.0f * k, Rgba(0.60f, 0.59f, 0.66f));
		const uint32_t ink = Rgba(0.07f, 0.06f, 0.16f);
		const float grow = 0.4f; // enough to close the seams, little enough to keep the modules their size
		for (int y = 0; y < m_qr_size; y++)
			for (int x0 = 0; x0 < m_qr_size;)
			{
				if (!m_qr[static_cast<size_t>(y) * m_qr_size + x0])
				{
					x0++;
					continue;
				}
				int x1 = x0;
				while (x1 < m_qr_size && m_qr[static_cast<size_t>(y) * m_qr_size + x1])
					x1++;
				Fonts::AddRoundedRect(ui, tx + (x0 + quiet) * mod - grow, ty + (y + quiet) * mod - grow,
					(x1 - x0) * mod + 2 * grow, mod + 2 * grow, 0.0f, ink);
				x0 = x1;
			}
		// vk-285-52: no caption, the user's call ("qr is enough"); 51 had "Settings on your phone" and
		// the address beside it.
	}
	else if (m_web_known)
		m_fonts->AddText(ui, Tr(Str::NoNetwork), W - margin, H - 170.0f * k, 32.0f * k, faint, 0.1f, Fonts::Right);
	const float sheet_e = Smoothstep(0.0f, 1.0f, m_sheet_anim);
	FadeRange(ui, qr_begin, ui.size(), 1.0f - sheet_e); // vk-285-114: under the options sheet

	const size_t title_begin = ui.size();
	if (n > 0)
	{
		const GameInfo& g = m_games[static_cast<size_t>(m_selected)];
		// Title, shrunk to fit (narrower beside the QR tile, which sits bottom right).
		const float title_w = W * (qr_shown ? 0.72f : 0.8f);
		float px = 96.0f * k;
		while (px > 64.0f * k && m_fonts->Measure(g.title.c_str(), px) > title_w)
			px -= 4.0f * k;
		m_fonts->AddText(ui, g.title.c_str(), W * 0.5f, H * 0.842f, px, white, 0.55f, Fonts::Center);

		// Details and badges on one centred row.
		std::string info;
		auto add = [&](const std::string& s) {
			if (s.empty())
				return;
			if (!info.empty())
				info += "   \xC2\xB7   ";
			info += s;
		};
		add(g.serial);
		add(Region(g.region)); // vk-285-110: the region names in the PS5's language
		if (m_index[static_cast<size_t>(m_selected)] == kDumpIndex) // live-22: the disc being copied, not a sized image
			add("Copying " + std::to_string(m_dump_pct) + "%");
		else
			add(SizeText(g.bytes));
		if (!g.damaged.empty())
			add("can't be read"); // vk-285-134
		if (g.hidden)
			add("hidden"); // vk-285-137
		const float info_px = 44.0f * k, badge_px = 36.0f * k;
		const float pad = 20.0f * k, gap = 16.0f * k;
		float row_w = m_fonts->Measure(info.c_str(), info_px);
		for (const std::string& b : g.badges)
			row_w += gap + m_fonts->Measure(b.c_str(), badge_px) + 2 * pad;
		float rx = W * 0.5f - row_w * 0.5f;
		const float row_y = H * 0.905f;
		rx += m_fonts->AddText(ui, info.c_str(), rx, row_y, info_px, dim, 0.1f);
		for (const std::string& b : g.badges)
		{
			const float bw = m_fonts->Measure(b.c_str(), badge_px) + 2 * pad;
			rx += gap;
			Fonts::AddRoundedRect(ui, rx, row_y - 40.0f * k, bw, 54.0f * k, 27.0f * k, Rgba(1, 1, 1, 0.12f));
			m_fonts->AddText(ui, b.c_str(), rx + pad, row_y - 2.0f * k, badge_px, white, 0.35f);
			rx += bw;
		}
	}
	else
		m_fonts->AddText(ui, Tr(Str::NoGames), W * 0.5f, H * 0.5f, 64.0f * k, white, 0.4f, Fonts::Center);
	// vk-285-137: a note (a game hidden, hidden games shown or not), in the same place, unless a refusal came after it.
	if (!m_note_text.empty() && m_time - m_note_time < 7.0 && m_note_time >= m_refused_time)
	{
		const float a = static_cast<float>(std::min(1.0, (7.0 - (m_time - m_note_time)) / 0.5));
		m_fonts->AddText(ui, Fit(*m_fonts, m_note_text, 36.0f * k, W * 0.86f).c_str(), W * 0.5f, H * 0.775f, 36.0f * k,
			Rgba(0.82f, 0.88f, 1.0f, a), 0.3f, Fonts::Center);
	}
	// vk-285-134: a pick refused for its image, for six seconds, just above the title.
	else if (!m_refused_text.empty() && m_time - m_refused_time < 6.0)
	{
		const float a = static_cast<float>(std::min(1.0, (6.0 - (m_time - m_refused_time)) / 0.5));
		m_fonts->AddText(ui, Fit(*m_fonts, m_refused_text, 36.0f * k, W * 0.8f).c_str(), W * 0.5f, H * 0.775f, 36.0f * k,
			Rgba(1.0f, 0.78f, 0.55f, a), 0.3f, Fonts::Center);
	}
	FadeRange(ui, title_begin, ui.size(), 1.0f - sheet_e); // vk-285-114: the sheet shows the title itself

	// vk-285-114: the options sheet, over a dimmed shelf.
	if (m_sheet_anim > 0.001f)
	{
		Fonts::AddRoundedRect(ui, 0, 0, W, H, 0.0f, Rgba(0.01f, 0.01f, 0.03f, 0.42f * sheet_e));
		BuildSheet(ui, W, H, k, accent);
		f.ui_images = m_achievement_images;
	}

	// 2026-10-05: the account panel's dim goes here, under the button hints (they are the panel's while it is open); the
	// panel itself is drawn last (BuildAccount).
	const float account_e = Smoothstep(0.0f, 1.0f, m_account_anim);
	if (m_account_anim > 0.001f)
		Fonts::AddRoundedRect(ui, 0, 0, W, H, 0.0f, Rgba(0.01f, 0.01f, 0.03f, 0.86f * account_e));

	// Button hints.
	const float hy = H - 88.0f * k, ipx = 64.0f * k, tpx = 40.0f * k;
	float hx = margin;
	// A PromptFont glyph, or (names starting with '#') a shoulder button drawn as a small pill.
	auto key = [&](const char* name) {
		if (name[0] != '#')
		{
			hx += m_fonts->AddText(ui, name, hx, hy + 6.0f * k, ipx, white);
			return;
		}
		const float kpx = 30.0f * k, kw = m_fonts->Measure(name + 1, kpx) + 26.0f * k, kh = 44.0f * k;
		Fonts::AddRoundedRect(ui, hx, hy - 36.0f * k, kw, kh, 12.0f * k, Rgba(1, 1, 1, 0.9f));
		m_fonts->AddText(ui, name + 1, hx + kw * 0.5f, hy - 4.0f * k, kpx, Rgba(0.08f, 0.08f, 0.14f), 0.6f, Fonts::Center);
		hx += kw;
	};
	auto hint = [&](const char* key_a, const char* key_b, const char* label) {
		key(key_a);
		if (key_b)
		{
			hx += 8.0f * k;
			key(key_b);
		}
		hx += 16.0f * k;
		hx += m_fonts->AddText(ui, label, hx, hy, tpx, dim, 0.1f);
		hx += 56.0f * k;
	};
	if (m_account.open)
	{
		// 2026-10-05: the account panel's buttons (none while the PS5's keyboard is up: it shows its own).
		const bool on_password = m_account.row == AchievementAccountPanel::RowPassword && !m_account.SignedIn();
		const char* const show = Tr(m_account.show_password ? Str::HintHidePassword : Str::HintShowPassword);
		if (m_ime_field >= 0 || m_account.account.busy)
		{
			// nothing to press
		}
		else if (m_account.editing)
		{
			hint(icon::DpadUpDown, nullptr, Tr(Str::HintMove));
			hint(icon::Cross, nullptr, Tr(Str::HintSelect));
			hint(icon::Triangle, nullptr, Tr(Str::HintDelete));
			if (on_password)
				hint(icon::Square, nullptr, show);
			hint(icon::Circle, nullptr, Tr(Str::HintDone));
		}
		else
		{
			if (m_account.RowCount() > 1)
				hint(icon::DpadUpDown, nullptr, Tr(Str::HintMove));
			const bool field = !m_account.SignedIn() && m_account.row != AchievementAccountPanel::RowSignIn;
			hint(icon::Cross, nullptr, Tr(field ? Str::HintEdit : Str::HintSelect));
			if (on_password && !m_account.password.empty())
				hint(icon::Square, nullptr, show);
			hint(icon::Circle, nullptr, Tr(Str::HintBack));
		}
	}
	else if ((m_sheet_open || m_sheet_anim > 0.5f) && m_sheet.tab() != kTabAchievements && m_cfg.texture_packs && !m_games.empty() &&
			 m_sheet_row >= 0 && m_sheet_row < static_cast<int>(m_sheet.rows().size()) &&
			 m_sheet.rows()[static_cast<size_t>(m_sheet_row)].kind == OptionsSheet::Kind::TexturePack)
	{
		// 2026-10-05: the HD texture pack row's buttons, for what it is doing now.
		using TS = TexturePackStatus::State;
		const TexturePackStatus st = m_cfg.texture_packs.status(m_games[static_cast<size_t>(m_selected)].serial);
		const bool busy = st.state == TS::Queued || st.state == TS::Checking || st.state == TS::Downloading || st.state == TS::Verifying ||
		                  st.state == TS::Unpacking;
		const bool startable = !st.packs.empty() && (st.state == TS::Available || st.state == TS::Failed || st.state == TS::NeedSpace);
		hint(icon::DpadUpDown, nullptr, Tr(Str::HintMove));
		if (startable && st.packs.size() > 1)
			hint(icon::DpadLeftRight, nullptr, Tr(Str::HintChange));
		if (startable || st.state == TS::Unavailable)
			hint(icon::Cross, nullptr, Tr(Str::HintDownload));
		if (busy || st.state == TS::Failed || st.state == TS::NeedSpace)
			hint(icon::Triangle, nullptr, Tr(Str::HintCancel));
		else if (st.state == TS::Installed && st.ours)
			hint(icon::Triangle, nullptr, Tr(Str::HintDelete));
		hint(icon::Circle, nullptr, Tr(Str::HintBack));
	}
	else if (m_sheet_open || m_sheet_anim > 0.5f)
	{
		// vk-285-114: the options sheet's buttons.
		hint(icon::DpadUpDown, nullptr, Tr(Str::HintMove));
		hint(icon::DpadLeftRight, nullptr, Tr(Str::HintChange));
		hint(icon::Triangle, nullptr, m_sheet.tab() == kTabAchievements ? Tr(Str::AchievementRefresh) : Tr(Str::HintReset));
		hint(icon::Circle, nullptr, Tr(Str::HintBack));
		if (n > 0)
		{
			const std::string scope = std::string(Tr(Str::SheetThisGame)) + " / " + Tr(Str::SheetAllGames);
			hint("#L1", "#R1", scope.c_str());
		}
		const std::string tabs = std::string(Tr(Str::HintSettings)) + " / " + Tr(Str::SheetControls) + " / " + Tr(Str::Achievements); // vk-285-116
		hint("#L2", "#R2", tabs.c_str());
	}
	else
	{
		hint(icon::Cross, nullptr, Tr(Str::HintPlay)); // vk-285-110: hints in the PS5's language
		// vk-285-69: the shelf opens with one game too (its QR code leads to the settings page and the
		// logs); browsing needs two.
		if (n > 1)
		{
			hint(icon::DpadLeftRight, nullptr, Tr(Str::HintBrowse));
			hint("#L1", "#R1", Tr(Str::HintJump));
		}
		hint(icon::Square, nullptr, Tr(Str::HintSettings)); // vk-285-114
		if (m_cfg.achievements.state && !m_qr_big)
			hint("#L1", icon::Square, "RetroAchievements");
		if (m_qr_size > 0)
			hint(icon::Triangle, nullptr, Tr(m_qr_big ? Str::HintBack : Str::HintQrCode));
	}

	std::string status = m_covers ? m_covers->Status() : std::string();
	if (status.empty() && n > 0)
	{
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%d / %d", m_selected + 1, n);
		status = buf;
		if (!m_cfg.build_tag.empty())
			status = m_cfg.build_tag + "   \xC2\xB7   " + status;
	}
	if (sheet_e < 0.5f && account_e < 0.5f)
		m_fonts->AddText(ui, status.c_str(), W - margin, hy, 36.0f * k, faint, 0.1f, Fonts::Right);

	// Test build 1 (vk-285-55): testing builds say so across the middle of the shelf, over the
	// cases, with the build under it, so every photo and video of one names the build.
	if (m_cfg.test_build > 0)
	{
		const float px = 300.0f * k, sub_px = 64.0f * k;
		const float base = H * 0.5f + m_fonts->Ascent(px) * 0.36f;
		m_fonts->AddText(ui, "TESTING", W * 0.5f + 5.0f * k, base + 5.0f * k, px, Rgba(0, 0, 0, 0.30f), 0.95f, Fonts::Center);
		m_fonts->AddText(ui, "TESTING", W * 0.5f, base, px, Rgba(1, 1, 1, 0.42f), 0.95f, Fonts::Center);
		float below = base + m_fonts->Descent(px); // the bottom of the last line drawn
		if (!m_cfg.build_label.empty())
		{
			const float sub_base = below + sub_px * 1.1f;
			m_fonts->AddText(ui, m_cfg.build_label.c_str(), W * 0.5f + 3.0f * k, sub_base + 3.0f * k, sub_px, Rgba(0, 0, 0, 0.45f),
				0.5f, Fonts::Center);
			m_fonts->AddText(ui, m_cfg.build_label.c_str(), W * 0.5f, sub_base, sub_px, Rgba(1, 1, 1, 0.75f), 0.5f, Fonts::Center);
			below = sub_base + m_fonts->Descent(sub_px);
		}
		// vk-285-105: the Discord note, smaller, under the build.
		if (!m_cfg.test_note.empty())
		{
			const float note_px = 44.0f * k, note_base = below + note_px * 1.25f;
			m_fonts->AddText(ui, m_cfg.test_note.c_str(), W * 0.5f + 2.0f * k, note_base + 2.0f * k, note_px, Rgba(0, 0, 0, 0.45f),
				0.5f, Fonts::Center);
			m_fonts->AddText(ui, m_cfg.test_note.c_str(), W * 0.5f, note_base, note_px, Rgba(1, 1, 1, 0.75f), 0.5f, Fonts::Center);
		}
	}

	// vk-285-118 (AI-assisted): the QR code large in the middle, black on white for any phone camera (Triangle).
	if (m_qr_big_anim > 0.0f && m_qr_size > 0)
	{
		const float e = Smoothstep(0.0f, 1.0f, m_qr_big_anim);
		const size_t begin = ui.size();
		Fonts::AddRoundedRect(ui, 0, 0, W, H, 0.0f, Rgba(0.01f, 0.01f, 0.03f, 0.80f));
		const int quiet = 4;
		const float mod = std::floor(H * 0.62f / static_cast<float>(m_qr_size + 2 * quiet));
		const float side = mod * static_cast<float>(m_qr_size + 2 * quiet);
		const float tx = std::floor((W - side) * 0.5f), ty = std::floor((H - side) * 0.5f - 40.0f * k);
		Fonts::AddRoundedRect(ui, tx, ty, side, side, 16.0f * k, Rgba(1.0f, 1.0f, 1.0f));
		const uint32_t ink = Rgba(0.0f, 0.0f, 0.0f);
		for (int y = 0; y < m_qr_size; y++)
			for (int x0 = 0; x0 < m_qr_size;)
			{
				if (!m_qr[static_cast<size_t>(y) * m_qr_size + x0])
				{
					x0++;
					continue;
				}
				int x1 = x0;
				while (x1 < m_qr_size && m_qr[static_cast<size_t>(y) * m_qr_size + x1])
					x1++;
				Fonts::AddRoundedRect(ui, tx + (x0 + quiet) * mod - 0.4f, ty + (y + quiet) * mod - 0.4f, (x1 - x0) * mod + 0.8f, mod + 0.8f, 0.0f, ink);
				x0 = x1;
			}
		FadeRange(ui, begin, ui.size(), e);
	}
	if (m_account_anim > 0.001f)
		BuildAccount(ui, W, H, k, accent);
}

// ---- vk-285-114: the options sheet ---------------------------------------------------------------------------------
// Square on the shelf opens it for the picked game; L1/R1 switch between that game and all games; D-pad up/down move,
// left/right (or Cross) change the value, Triangle puts the row back to what it follows, Circle, Square or Options close
// it. Every change is saved at once, as the settings page saves (fe_options.cpp).

namespace
{
	// `text` in lines of at most `width` pixels at `px`, at most `max_lines` (the last one ends in an ellipsis when cut).
	std::vector<std::string> Wrap(const Fonts& fonts, const std::string& text, float px, float width, int max_lines)
	{
		std::vector<std::string> lines;
		std::string line, word;
		auto flush_word = [&]() {
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
		for (char c : text)
		{
			if (c == ' ' || c == '\n')
			{
				flush_word();
				if (c == '\n' && !line.empty())
				{
					lines.push_back(line);
					line.clear();
				}
			}
			else
				word += c;
		}
		flush_word();
		if (!line.empty())
			lines.push_back(line);
		if (static_cast<int>(lines.size()) > max_lines)
		{
			lines.resize(static_cast<size_t>(max_lines));
			std::string& last = lines.back();
			while (!last.empty() && fonts.Measure((last + "\xE2\x80\xA6").c_str(), px) > width)
			{
				// Whole UTF-8 characters only (the summaries have middle dots).
				while (last.size() > 1 && (static_cast<unsigned char>(last.back()) & 0xC0) == 0x80)
					last.pop_back();
				last.pop_back();
			}
			last += "\xE2\x80\xA6";
		}
		return lines;
	}

	// The shortest string with `text`'s start that fits `width` (an ellipsis marks the cut).
	std::string Fit(const Fonts& fonts, std::string text, float px, float width)
	{
		if (fonts.Measure(text.c_str(), px) <= width)
			return text;
		while (!text.empty() && fonts.Measure((text + "\xE2\x80\xA6").c_str(), px) > width)
		{
			text.pop_back();
			while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) // a whole UTF-8 character
				text.pop_back();
			if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0xC0)
				text.pop_back();
		}
		return text + "\xE2\x80\xA6";
	}

	// vk-285-116: "<symbol>  Cross" (fe_options.cpp Sym: a PromptFont glyph, two spaces, the name) into its two parts.
	bool SplitSymbol(const std::string& s, std::string& glyph, std::string& name)
	{
		if (s.size() < 6 || static_cast<unsigned char>(s[0]) != 0xE2 || s.compare(3, 2, "  ") != 0)
			return false;
		glyph = s.substr(0, 3);
		name = s.substr(5);
		return true;
	}
	// The symbol's size against the text's, and how far its baseline drops (of the text's size) to sit centred on the text.
	constexpr float kSymbolScale = 1.35f, kSymbolDrop = 0.12f;

	// The sheet's measures, in 2160-line pixels (scaled by k when drawn).
	constexpr float kSheetW = 1520.0f, kSheetTop = 206.0f, kSheetBottomGap = 222.0f; // 2026-10-05: 1260 before
	constexpr float kSheetListTop = 308.0f; // from the sheet's top (vk-285-117: the top is one line shorter)
	constexpr float kSheetHelpH = 352.0f; // the help box at the bottom
	constexpr float kRowH = 100.0f, kHeaderH = 84.0f; // 2026-10-05: 94 and 76 before
} // namespace

void App::OpenSheet(bool global)
{
	const GameInfo* g = (!global && !m_games.empty()) ? &m_games[static_cast<size_t>(m_selected)] : nullptr;
	m_sheet_global = g == nullptr;
	if (m_sheet_global && m_cfg.game_achievements.cancel)
		m_cfg.game_achievements.cancel();
	m_sheet.SetTexturePackRow(static_cast<bool>(m_cfg.texture_packs)); // 2026-10-05
	m_sheet.SetOnlinePatchRow(static_cast<bool>(m_cfg.online_patches)); // 2026-10-08
	m_sheet.SetSystemMenuRow(m_cfg.system_menu);                        // 2026-10-08
	m_sheet.SetFolderRows(!m_cfg.folder_places.empty());                // vk-285-135
	m_picker.open = false;
	OptionsPaths paths = m_cfg.options;
	if (!g || IsElfName(g->file.c_str())) // 2026-10-08: an ELF's Disc image row lists the shelf's disc images; vk-285-139: the Cheat disc row
	{
		for (const GameInfo& other : m_games)
			if (!IsElfName(other.file.c_str()))
				paths.disc_images.emplace_back(other.file, other.title);
		for (const Shelved& h : m_shelved) // vk-285-137: hidden ones too
			if (!IsElfName(h.game.file.c_str()))
				paths.disc_images.emplace_back(h.game.file, h.game.title);
	}
	m_sheet.Open(paths, g);
	m_sheet_saved_at_open = 0;
	if (!m_sheet_open)
	{
		m_sheet_open = true;
		m_sheet_scroll = m_sheet_scroll_target = 0;
	}
	// The first row that can be picked.
	m_sheet_row = 0;
	const auto& rows = m_sheet.rows();
	while (m_sheet_row < static_cast<int>(rows.size()) && !m_sheet.Selectable(rows[static_cast<size_t>(m_sheet_row)]))
		m_sheet_row++;
	m_sheet_scroll_target = 0;
	m_sheet_status.clear();
	m_sheet_held_v = m_sheet_held_h = 0;
	if (m_sheet.tab() == kTabAchievements)
		LoadGameAchievements();
	std::printf("[options] sheet for %s (%s)\n", m_sheet.title().c_str(), m_sheet.file_label().c_str());
	std::fflush(stdout);
}

// vk-285-116: another tab of the same sheet, from its first row.
void App::SheetTab(int tab)
{
	m_sheet.SetTab(tab);
	if (tab != kTabAchievements && m_cfg.game_achievements.cancel)
		m_cfg.game_achievements.cancel();
	if (tab == kTabAchievements)
		LoadGameAchievements();
	m_sheet_row = 0;
	const auto& rows = m_sheet.rows();
	while (m_sheet_row < static_cast<int>(rows.size()) && !m_sheet.Selectable(rows[static_cast<size_t>(m_sheet_row)]))
		m_sheet_row++;
	m_sheet_scroll = m_sheet_scroll_target = 0;
	m_sheet_held_v = m_sheet_held_h = 0;
	std::printf("[options] %s tab\n", tab == kTabAchievements ? "achievements" : tab == kTabControls ? "controls" :
																									   "settings");
	std::fflush(stdout);
}

void App::RefreshBadges()
{
	if (m_cfg.refresh_game && !m_games.empty())
	{
		if (m_sheet_global)
			for (GameInfo& g : m_games)
				m_cfg.refresh_game(g);
		else
			m_cfg.refresh_game(m_games[static_cast<size_t>(m_selected)]);
	}
	// vk-285-137: a game hidden on its sheet leaves the shelf now; Show hidden games (the sheet for all games) brings the hidden
	// ones back, dimmed, or takes them off again.
	const bool show = !m_cfg.options.gs_ini.empty() && ShowHiddenGames(m_cfg.options.gs_ini);
	const bool show_changed = show != m_show_hidden;
	m_show_hidden = show;
	const bool hid = !m_sheet_global && !m_games.empty() && m_games[static_cast<size_t>(m_selected)].hidden && !show;
	const std::string title = m_games.empty() ? std::string() : m_games[static_cast<size_t>(m_selected)].title;
	const std::string file = m_games.empty() ? std::string() : m_games[static_cast<size_t>(m_selected)].file;
	const int keep = !m_games.empty() ? m_index[static_cast<size_t>(m_selected)] : m_shelved.empty() ? 0 : m_shelved.front().index;
	const size_t shown_before = m_games.size();
	if (!ApplyHidden(keep) && !show_changed)
		return;
	const size_t hidden_count = m_shelved.size() + static_cast<size_t>(std::count_if(m_games.begin(), m_games.end(),
		[](const GameInfo& g) { return g.hidden; }));
	if (hid)
	{
		m_note_text = title + " is hidden from the shelf. Show hidden games, on the sheet for all games (Square, then R1), brings it back.";
		std::printf("[frontend] hidden from the shelf: %s (%zu hidden)\n", file.c_str(), hidden_count);
	}
	else if (show_changed)
	{
		m_note_text = show ? std::to_string(hidden_count) + (hidden_count == 1 ? " hidden game is" : " hidden games are") +
		                         " back on the shelf, dimmed. To keep one there, turn Hide from the shelf off on its sheet."
		                   : std::to_string(hidden_count) + (hidden_count == 1 ? " hidden game is" : " hidden games are") + " off the shelf again.";
		std::printf("[frontend] hidden games %s (%zu; the shelf had %zu, now %zu)\n", show ? "shown, dimmed" : "off the shelf", hidden_count,
			shown_before, m_games.size());
	}
	else
		return;
	m_note_time = m_time;
	std::fflush(stdout);
}

void App::CloseSheet()
{
	if (!m_sheet_open)
		return;
	m_sheet_open = false;
	m_picker.open = false; // vk-285-135
	if (m_cfg.game_achievements.cancel)
		m_cfg.game_achievements.cancel();
	if (m_sheet.saved() > 0)
		RefreshBadges();
	std::printf("[options] sheet closed (%d change(s) saved)\n", m_sheet.saved());
	std::fflush(stdout);
}

bool App::SheetMove(int dir)
{
	const auto& rows = m_sheet.rows();
	int r = m_sheet_row;
	for (;;)
	{
		r += dir;
		if (r < 0 || r >= static_cast<int>(rows.size()))
			return false;
		if (m_sheet.Selectable(rows[static_cast<size_t>(r)]))
			break;
	}
	m_sheet_row = r;
	return true;
}

void App::UpdateSheet(double dt, const Input& in)
{
	auto pressed = [&](bool now, bool before) { return now && !before; };
	const double now = m_time;
	const auto& rows = m_sheet.rows();
	if (rows.empty() && m_sheet.tab() != kTabAchievements)
	{
		CloseSheet();
		return;
	}
	m_sheet_row = std::max(0, std::min(m_sheet_row, static_cast<int>(rows.size()) - 1));

	// 2026-10-05: a texture pack that came in while the sheet is open: the game's file has Texture replacements on now.
	if (m_cfg.texture_packs && !m_sheet_global && !m_games.empty())
	{
		const std::string& serial = m_games[static_cast<size_t>(m_selected)].serial;
		const TexturePackStatus::State st = m_cfg.texture_packs.status(serial).state;
		if (serial == m_texpack_seen_serial && st != m_texpack_seen && st == TexturePackStatus::State::Installed)
		{
			m_sheet.Reload();
			RefreshBadges();
		}
		m_texpack_seen_serial = serial;
		m_texpack_seen = st;
	}
	// 2026-10-08: online patches that came in while the sheet is open: their rows.
	if (m_cfg.online_patches && !m_sheet_global && !m_games.empty())
	{
		const std::string& serial = m_games[static_cast<size_t>(m_selected)].serial;
		const OnlinePatchStatus::State st = m_cfg.online_patches.status(serial).state;
		if (serial == m_online_seen_serial && st != m_online_seen && st == OnlinePatchStatus::State::Done)
		{
			m_sheet.Reload();
			RefreshBadges();
		}
		m_online_seen_serial = serial;
		m_online_seen = st;
	}

	// vk-285-135: the folder picker has the buttons while it is open (Circle goes back a folder, not out of the sheet).
	if (m_picker.open)
	{
		UpdatePicker(dt, in);
		return;
	}
	if (pressed(in.circle, m_prev.circle) || pressed(in.square, m_prev.square) || pressed(in.options, m_prev.options))
	{
		CloseSheet();
		Sound(Sfx::Move, 0.3f);
		return;
	}
	if ((pressed(in.l1, m_prev.l1) || pressed(in.r1, m_prev.r1)) && !m_games.empty())
	{
		const bool global = pressed(in.r1, m_prev.r1);
		if (global != m_sheet_global)
		{
			const int saved = m_sheet.saved();
			if (saved > 0)
				RefreshBadges();
			OpenSheet(global);
			Sound(global ? Sfx::JumpRight : Sfx::JumpLeft, global ? 0.3f : -0.1f);
		}
		else
			Sound(Sfx::Edge, 0.3f);
		return;
	}
	// vk-285-116: L2 the settings, R2 the controls.
	// pr9n (AI-assisted), PR #9 review item 11: no wrapping. From Settings, L2 went round to Achievements, which starts
	// network traffic; the tabs stop at either end now.
	if (pressed(in.l2, m_prev.l2) || pressed(in.r2, m_prev.r2))
	{
		const int direction = pressed(in.r2, m_prev.r2) ? 1 : -1;
		const int tab = std::max(0, std::min(kTabCount - 1, m_sheet.tab() + direction));
		if (tab != m_sheet.tab())
		{
			SheetTab(tab);
			Sound(direction > 0 ? Sfx::JumpRight : Sfx::JumpLeft, direction > 0 ? 0.3f : -0.1f);
		}
		else
			Sound(Sfx::Edge, 0.3f);
		return;
	}

	if (m_sheet.tab() == kTabAchievements)
	{
		const int direction = in.up ? -1 : in.down ? 1 :
			                                         0;
		const bool fresh = (in.up && !m_prev.up) || (in.down && !m_prev.down);
		if (direction && (fresh || m_time >= m_sheet_next_repeat))
		{
			m_achievement_row = std::clamp(m_achievement_row + direction, 0,
				std::max(0, static_cast<int>(m_game_achievements.entries.size()) - 1));
			m_sheet_next_repeat = m_time + (fresh ? 0.32 : 0.10);
		}
		if (pressed(in.triangle, m_prev.triangle) && !m_game_achievements.busy)
			LoadGameAchievements();
		return;
	}

	// Up and down, repeating while held.
	const int v = in.up ? -1 : in.down ? 1 : 0;
	const bool v_fresh = (in.up && !m_prev.up) || (in.down && !m_prev.down);
	if (v != 0 && v_fresh)
	{
		Sound(SheetMove(v) ? Sfx::Move : Sfx::Edge, 0.3f);
		m_sheet_held_v = v;
		m_sheet_held_for = 0;
		m_sheet_next_repeat = 0.34;
	}
	else if (v != 0 && v == m_sheet_held_v)
	{
		m_sheet_held_for += dt;
		bool moved = false;
		while (m_sheet_held_for >= m_sheet_next_repeat)
		{
			moved |= SheetMove(v);
			m_sheet_next_repeat += 0.085;
		}
		if (moved)
			Sound(Sfx::MoveRepeat, 0.3f);
	}
	else
		m_sheet_held_v = 0;

	const OptionsSheet::Row& row = rows[static_cast<size_t>(m_sheet_row)];
	auto after = [&](bool changed) {
		const std::string& st = m_sheet.status();
		if (!st.empty())
		{
			m_sheet_status = st;
			m_sheet_status_time = now;
		}
		Sound(changed ? Sfx::Move : Sfx::Edge, 0.3f);
	};

	// 2026-10-05: the HD texture pack row runs on its own (left and right pick a pack, Cross fetches it, Triangle twice
	// cancels or deletes).
	if (row.kind == OptionsSheet::Kind::TexturePack)
	{
		UpdateTexturePackRow(in, now);
		return;
	}
	// 2026-10-08: "Get patches and cheats": Cross fetches the game's files (fe_patchdl.h); the rows come when it's done.
	if (row.kind == OptionsSheet::Kind::OnlinePatches)
	{
		if (pressed(in.cross, m_prev.cross) && m_cfg.online_patches && !m_games.empty())
		{
			const GameInfo& g = m_games[static_cast<size_t>(m_selected)];
			const bool started = m_cfg.online_patches.begin(g);
			m_sheet_status = started ? "Looking online for " + g.title + "'s patches and cheats" : "Already looking";
			m_sheet_status_time = now;
			std::printf("[options] online patches for %s (%s): %s\n", g.title.c_str(), g.serial.c_str(), started ? "asked" : "already asked");
			std::fflush(stdout);
			Sound(started ? Sfx::Move : Sfx::Edge, 0.3f);
		}
		return;
	}

	// vk-285-135: the Folders rows: Cross opens the picker (game folders, the BIOS folder) or the share list; Triangle sets the
	// BIOS folder back to its default.
	if (row.kind == OptionsSheet::Kind::GameFolders || row.kind == OptionsSheet::Kind::BiosFolder ||
		row.kind == OptionsSheet::Kind::NfsShares)
	{
		if (pressed(in.cross, m_prev.cross))
		{
			m_sheet.Activate(row, now);
			const OptionsSheet::Kind asked = m_sheet.TakeFolderRequest();
			if (asked != OptionsSheet::Kind::Header)
			{
				OpenPicker(asked);
				Sound(Sfx::Move, 0.3f);
			}
		}
		else if (pressed(in.triangle, m_prev.triangle))
			after(m_sheet.Reset(row));
		return;
	}

	// 2026-10-08: "PS2 system menu" (the sheet for all games): Cross twice starts the PS2's own menu with no disc; the shelf
	// closes as for a game (SystemMenuChosen).
	if (row.kind == OptionsSheet::Kind::SystemMenu)
	{
		if (pressed(in.cross, m_prev.cross))
		{
			after(m_sheet.Activate(row, now));
			if (m_sheet.TakeSystemMenu())
			{
				CloseSheet();
				if (BiosReady()) // vk-285-134: the PS2's own menu is the BIOS
				{
					m_system_menu = true;
					m_launching = true;
					m_launch_time = m_time;
					Sound(Sfx::Launch, 0.0f);
				}
			}
		}
		return;
	}

	// Left and right change the value (not on the action rows, which Cross runs).
	const bool action = row.kind == OptionsSheet::Kind::Recommended || row.kind == OptionsSheet::Kind::ResetAll;
	const int h = in.left ? -1 : in.right ? 1 : 0;
	const bool h_fresh = (in.left && !m_prev.left) || (in.right && !m_prev.right);
	if (h != 0 && h_fresh && !action)
	{
		after(m_sheet.Step(row, h));
		m_sheet_held_h = h;
		m_sheet_held_for = 0;
		m_sheet_next_repeat = 0.45;
		return; // the rows may have been made again
	}
	if (h == 0)
		m_sheet_held_h = 0;

	if (pressed(in.cross, m_prev.cross))
	{
		after(action || row.kind == OptionsSheet::Kind::NewCard ? m_sheet.Activate(row, now) : m_sheet.Step(row, 1));
		return;
	}
	if (pressed(in.triangle, m_prev.triangle))
	{
		after(m_sheet.Reset(row));
		return;
	}
}

// ---- 2026-10-08: the sheet's "Get patches and cheats" row (AI-assisted; see fe_patchdl.h) ---------------------------

std::string App::OnlinePatchValue() const
{
	if (!m_cfg.online_patches || m_games.empty())
		return {};
	const OnlinePatchStatus s = m_cfg.online_patches.status(m_games[static_cast<size_t>(m_selected)].serial);
	switch (s.state)
	{
		case OnlinePatchStatus::State::Idle:
			return "Download";
		case OnlinePatchStatus::State::Working:
			return s.message.empty() ? std::string("Working") : s.message;
		case OnlinePatchStatus::State::Done:
			return s.message;
		case OnlinePatchStatus::State::Failed:
			return "Failed: " + s.message;
	}
	return {};
}

// ---- 2026-10-05: the sheet's HD texture pack row (AI-assisted; see fe_texpacks.h) -----------------------------------
// The sheet's other rows are in English; so is this one. The shelf's line and the popup are in the PS5's language.

void App::UpdateTexturePackRow(const Input& in, double now)
{
	using State = TexturePackStatus::State;
	auto pressed = [&](bool down, bool before) { return down && !before; };
	if (m_games.empty())
		return;
	const GameInfo& g = m_games[static_cast<size_t>(m_selected)];
	const TexturePackStatus s = m_cfg.texture_packs.status(g.serial);
	const int n = static_cast<int>(s.packs.size());
	// The pack shown first: the one a download was for, else the first.
	auto found = m_texpack_pick.find(g.serial);
	if (found == m_texpack_pick.end())
		found = m_texpack_pick.emplace(g.serial, s.job_pack >= 0 ? s.job_pack : 0).first;
	int& pick = found->second;
	pick = n > 0 ? std::clamp(pick, 0, n - 1) : 0;
	const bool can_start = n > 0 && (s.state == State::Available || s.state == State::Failed || s.state == State::NeedSpace);
	auto status = [&](const std::string& text) {
		m_sheet_status = text;
		m_sheet_status_time = now;
	};

	const int h = in.left ? -1 : in.right ? 1 : 0;
	if (h != 0 && ((in.left && !m_prev.left) || (in.right && !m_prev.right)))
	{
		if (can_start && n > 1)
		{
			pick = (pick + h + n) % n;
			Sound(Sfx::Move, 0.3f);
		}
		else
			Sound(Sfx::Edge, 0.3f);
		return;
	}
	if (pressed(in.cross, m_prev.cross))
	{
		bool ok = false;
		if (s.state == State::Unavailable && m_cfg.texture_packs.retry)
		{
			m_cfg.texture_packs.retry();
			status("Looking on archive.org again");
			ok = true;
		}
		else if (can_start)
		{
			const std::string path = m_cfg.options.settings_dir + "/" + g.stem + ".ini";
			const std::string header = "# " + g.title + (g.serial.empty() ? std::string() : " (" + g.serial + ")");
			ok = m_cfg.texture_packs.begin(g.serial, pick, path, header, g.title);
			const TexturePack& p = s.packs[static_cast<size_t>(pick)];
			status(ok ? "Asked for " + p.label + " (" + FormatBytes(p.bytes) + ")" : "Couldn't start the download");
		}
		Sound(ok ? Sfx::Move : Sfx::Edge, 0.3f);
		return;
	}
	if (pressed(in.triangle, m_prev.triangle))
	{
		const bool busy = s.state == State::Queued || s.state == State::Checking || s.state == State::Downloading ||
		                  s.state == State::Verifying || s.state == State::Unpacking;
		const bool cancel = busy || s.state == State::Failed || s.state == State::NeedSpace;
		const bool remove = s.state == State::Installed && s.ours;
		if (!cancel && !remove)
		{
			Sound(Sfx::Edge, 0.3f);
			return;
		}
		if (m_texpack_armed != g.serial || now > m_texpack_armed_until)
		{
			m_texpack_armed = g.serial;
			m_texpack_armed_until = now + 4.0;
			status(remove ? "Press Triangle again to delete this pack" : "Press Triangle again to cancel the download");
			Sound(Sfx::Move, 0.3f);
			return;
		}
		m_texpack_armed.clear();
		const bool ok = remove ? m_cfg.texture_packs.remove(g.serial) : m_cfg.texture_packs.cancel(g.serial);
		status(!ok ? std::string("Couldn't do that now") : remove ? "Deleting the pack" : "Download cancelled");
		Sound(ok ? Sfx::Move : Sfx::Edge, 0.3f);
	}
}

std::string App::TexturePackValue(const TexturePackStatus& s, int pick) const
{
	using State = TexturePackStatus::State;
	const int n = static_cast<int>(s.packs.size());
	auto percent = [&] {
		const int pc = s.total > 0 ? static_cast<int>(100.0 * static_cast<double>(s.done) / static_cast<double>(s.total)) : 0;
		return std::to_string(std::clamp(pc, 0, 100)) + "%";
	};
	switch (s.state)
	{
		case State::Loading: return "Looking on archive.org";
		case State::Unavailable: return "Can't reach archive.org";
		case State::None: return "None on archive.org";
		case State::Available:
		{
			const TexturePack& p = s.packs[static_cast<size_t>(std::clamp(pick, 0, n - 1))];
			return (n > 1 ? p.label : std::string("Download")) + "  \xC2\xB7  " + FormatBytes(p.bytes);
		}
		case State::Queued: return "Waiting";
		case State::Checking: return "Checking " + percent();
		case State::Downloading:
		{
			std::string v = percent() + "  \xC2\xB7  " + FormatBytes(s.done) + " of " + FormatBytes(s.total);
			if (s.rate > 1000)
				v += "  \xC2\xB7  " + FormatBytes(static_cast<uint64_t>(s.rate)) + "/s";
			return v;
		}
		case State::Verifying: return "Verifying";
		case State::Unpacking: return "Unpacking " + percent();
		case State::Installing: return "Installing";
		case State::Installed:
			if (s.ours)
				return "Installed";
			return s.where.compare(0, 8, "/mnt/usb") == 0 ? "On a USB drive" : "Your own pack";
		case State::Removing: return "Deleting";
		case State::Failed: return "Failed";
		case State::NeedSpace: return "Needs more space";
	}
	return {};
}

std::string App::TexturePackHelp(const TexturePackStatus& s, int pick, const std::string& serial) const
{
	using State = TexturePackStatus::State;
	const int n = static_cast<int>(s.packs.size());
	const std::string folder = "textures/" + serial;
	const std::string carry_on = " It downloads while the shelf is open: a game pauses it, and it carries on when you're back here.";
	switch (s.state)
	{
		case State::Loading: return "Looking up this game in archive.org's PCSX2 HD Texture Packs.";
		case State::Unavailable: return s.message + ". Cross looks again.";
		case State::None:
			return "archive.org's PCSX2 HD Texture Packs has none for " + serial + ". A pack you put in " + folder +
			       "/replacements still works (turn Texture replacements on).";
		case State::Available:
		{
			const TexturePack& p = s.packs[static_cast<size_t>(std::clamp(pick, 0, n - 1))];
			std::string h = "Cross downloads " + p.label + " (" + FormatBytes(p.bytes) +
			                (p.files > 0 ? ", " + std::to_string(p.files) + " files" : std::string()) +
			                ") from archive.org's PCSX2 HD Texture Packs, puts it in " + folder +
			                " and turns Texture replacements on for this game." + carry_on;
			if (n > 1)
				h += " Left and right pick another version.";
			return h;
		}
		case State::Queued: return "Waiting for the pack before it." + carry_on + " Triangle twice cancels it.";
		case State::Checking: return "Checking what was downloaded before." + carry_on + " Triangle twice cancels it.";
		case State::Downloading: return "Downloading from archive.org." + carry_on + " Triangle twice cancels it.";
		case State::Verifying: return "Checking the download against archive.org's checksum.";
		case State::Unpacking: return "Unpacking into " + folder + ". Triangle twice cancels it.";
		case State::Installing: return "Moving the pack into " + folder + ".";
		case State::Installed:
			if (s.ours)
			{
				std::string name = s.installed.empty() ? std::string("The pack") : s.installed;
				if (name.size() > 4 && (name.compare(name.size() - 4, 4, ".rar") == 0 || name.compare(name.size() - 4, 4, ".zip") == 0))
					name.resize(name.size() - 4);
				return name + " is in " + folder + ". It shows in the game while Texture replacements is on (installing turned it on). "
				                                   "Triangle twice deletes it.";
			}
			return "There is a texture pack for this game in " + s.where + " already, so archive.org's isn't offered.";
		case State::Removing: return "Deleting the pack's files.";
		case State::Failed: return s.message + ". Cross tries again; Triangle twice forgets it.";
		case State::NeedSpace: return s.message + ". Cross tries again once there's room; Triangle twice forgets it.";
	}
	return {};
}

// The shelf's line while a pack is on its way: "HD textures: God of War · 37%", and a thin bar under it.
// vk-285-134: the line while there's no BIOS, centred under the clock's row: the headline in the PS5's language, what was
// found instead under it. It lights up for a moment when a pick was refused.
void App::BuildBiosLine(std::vector<UiVertex>& ui, float W, float k)
{
	if (!m_bios_missing)
		return;
	const float since = static_cast<float>(m_time - m_bios_refused);
	const float flash = since >= 0.0f && since < 1.6f ? 1.0f - since / 1.6f : 0.0f;
	const float w = 2000.0f * k, x = W * 0.5f - w * 0.5f, y = 222.0f * k, h = m_bios_problem.empty() ? 96.0f * k : 150.0f * k;
	Fonts::AddRoundedRect(ui, x, y, w, h, 24.0f * k, Rgba(0.42f + 0.25f * flash, 0.20f + 0.08f * flash, 0.04f, 0.88f));
	char head[320];
	std::snprintf(head, sizeof(head), Tr(Str::ShelfNoBios), m_cfg.bios_dir.c_str());
	const float hpx = 40.0f * k, dpx = 32.0f * k;
	m_fonts->AddText(ui, Fit(*m_fonts, head, hpx, w - 60.0f * k).c_str(), W * 0.5f, y + 60.0f * k, hpx, Rgba(1, 1, 1), 0.4f, Fonts::Center);
	if (!m_bios_problem.empty())
		m_fonts->AddText(ui, Fit(*m_fonts, m_bios_problem, dpx, w - 60.0f * k).c_str(), W * 0.5f, y + 116.0f * k, dpx,
			Rgba(1.0f, 0.86f, 0.62f), 0.2f, Fonts::Center);
}

void App::BuildTexturePackActivity(std::vector<UiVertex>& ui, float x, float y, float k, uint32_t accent)
{
	using State = TexturePackStatus::State;
	if (!m_cfg.texture_packs || !m_cfg.texture_packs.activity)
		return;
	const TexturePackActivity a = m_cfg.texture_packs.activity();
	if (!a.active)
		return;
	const bool measured = a.state == State::Checking || a.state == State::Downloading || a.state == State::Unpacking;
	std::string line = std::string(Tr(Str::HdTextures)) + ": " + a.title;
	if (measured)
		line += "  \xC2\xB7  " + std::to_string(static_cast<int>(a.fraction * 100.0)) + "%";
	if (a.waiting > 0)
		line += "  (+" + std::to_string(a.waiting) + ")";
	const float px = 30.0f * k, w = 460.0f * k;
	m_fonts->AddText(ui, Fit(*m_fonts, line, px, 900.0f * k).c_str(), x, y, px, Rgba(0.72f, 0.69f, 0.82f), 0.1f);
	Fonts::AddRoundedRect(ui, x, y + 16.0f * k, w, 6.0f * k, 3.0f * k, Rgba(1, 1, 1, 0.10f));
	if (measured && a.fraction > 0)
		Fonts::AddRoundedRect(ui, x, y + 16.0f * k, std::max(6.0f * k, w * static_cast<float>(a.fraction)), 6.0f * k, 3.0f * k, accent);
}

// Achievement data and textures are owned by the shelf, never by the VM. (AI-assisted)
void App::ClearAchievementBadges()
{
	for (auto& badge : m_achievement_badges)
	{
		m_renderer->FreeTextureSet(badge.set);
		m_renderer->DestroyTexture(badge.texture);
	}
	m_achievement_badges.clear();
}
void App::LoadGameAchievements()
{
	if (m_sheet_global || m_games.empty())
		return;
	if (m_cfg.game_achievements.load)
		m_cfg.game_achievements.load(m_games[static_cast<size_t>(m_selected)].path);
	m_achievement_row = 0;
}
void App::PollGameAchievements()
{
	if (!m_cfg.game_achievements.state)
		return;
	auto state = m_cfg.game_achievements.state();
	if (state.revision != m_game_achievements.revision)
	{
		bool changed = state.path != m_game_achievements.path || state.entries.size() != m_game_achievements.entries.size();
		if (!changed)
			for (size_t i = 0; i < state.entries.size(); ++i)
				if (state.entries[i].id != m_game_achievements.entries[i].id ||
					state.entries[i].unlocked != m_game_achievements.entries[i].unlocked)
				{
					changed = true;
					break;
				}
		if (changed)
		{
			ClearAchievementBadges();
			m_achievement_row = 0;
		}
		m_game_achievements = std::move(state);
	}
	m_achievement_badges.resize(m_game_achievements.entries.size());
	// At most one decode/upload per frame, keeping the controller responsive.
	for (size_t i = 0; i < m_achievement_badges.size(); ++i)
	{
		auto& badge = m_achievement_badges[i];
		const auto& entry = m_game_achievements.entries[i];
		if (std::abs(static_cast<int>(i) - m_achievement_row) > 20)
		{
			if (badge.texture)
			{
				m_renderer->FreeTextureSet(badge.set);
				m_renderer->DestroyTexture(badge.texture);
				badge = {};
			}
			continue;
		}
		if (!entry.image || badge.attempted)
			continue;
		badge.attempted = true;
		CoverImage decoded;
		if (CoverService::Decode(*entry.image, 64, decoded, 512))
		{
			badge.texture = m_renderer->CreateTexture(decoded.width, decoded.height, VK_FORMAT_R8G8B8A8_UNORM, decoded.rgba.data());
			if (badge.texture)
				badge.set = m_renderer->AllocTextureSet(badge.texture, badge.texture, badge.texture);
		}
		break;
	}
}
// 2026-10-05 (AI-assisted; with the wider sheet): the list a size up, as the other tabs' rows (bigger badges, titles and
// descriptions, seven rows in place of eight), the focused row lit with the cover's colour at its edge as theirs.
void App::BuildGameAchievements(std::vector<UiVertex>& ui, float x, float y, float width, float height, float k, uint32_t accent)
{
	const uint32_t white = Rgba(1, 1, 1), muted = Rgba(0.7f, 0.7f, 0.8f), green = Rgba(0.45f, 1, 0.65f);
	std::string message;
	if (m_sheet_global || m_games.empty())
		message = Tr(Str::AchievementSelectGame);
	else if (!m_cfg.game_achievements.state)
		message = Tr(Str::AchievementUnavailable);
	else if (m_game_achievements.path != m_games[static_cast<size_t>(m_selected)].path)
		message = Tr(Str::AchievementPending);
	else if (m_game_achievements.entries.empty())
		message = m_game_achievements.message;
	if (!message.empty())
	{
		float baseline = y + 80 * k;
		for (const auto& line : Wrap(*m_fonts, message, 36 * k, width, 4))
		{
			m_fonts->AddText(ui, line.c_str(), x, baseline, 36 * k, muted);
			baseline += 48 * k;
		}
		return;
	}
	unsigned unlocked = 0, points = 0, earned = 0;
	for (const auto& entry : m_game_achievements.entries)
	{
		points += entry.points;
		if (entry.unlocked)
		{
			++unlocked;
			earned += entry.points;
		}
	}
	const std::string summary = std::to_string(unlocked) + " / " + std::to_string(m_game_achievements.entries.size()) +
		                        std::string(" ") + Tr(Str::AchievementUnlocked) + "   |   " + std::to_string(earned) + " / " + std::to_string(points) + " " + Tr(Str::AchievementPoints);
	m_fonts->AddText(ui, Fit(*m_fonts, m_game_achievements.title, 38 * k, width).c_str(), x, y + 44 * k, 38 * k, white);
	m_fonts->AddText(ui, summary.c_str(), x, y + 96 * k, 30 * k, green);
	constexpr float list_top = 132, row_height = 160, box_height = 148; // 115, 142 and 130 before
	const int visible = std::max(1, static_cast<int>((height / k - list_top - 60) / row_height));
	const int count = static_cast<int>(m_game_achievements.entries.size());
	const int first = std::clamp(m_achievement_row - visible / 2, 0, std::max(0, count - visible));
	for (int i = first; i < std::min(count, first + visible); ++i)
	{
		const auto& entry = m_game_achievements.entries[static_cast<size_t>(i)];
		const float top = y + (list_top + (i - first) * row_height) * k;
		const bool focused = i == m_achievement_row;
		Fonts::AddRoundedRect(ui, x - 12 * k, top, width + 24 * k, box_height * k, 24 * k,
			focused ? Rgba(1, 1, 1, 0.11f) : Rgba(1, 1, 1, 0.035f));
		if (focused)
			Fonts::AddRoundedRect(ui, x + 2 * k, top + 26 * k, 6 * k, (box_height - 52) * k, 3 * k, accent);
		const auto& badge = m_achievement_badges[static_cast<size_t>(i)];
		const float bx = x + 24 * k, by = top + 18 * k, size = 112 * k;
		if (badge.set)
		{
			const uint32_t first_vertex = static_cast<uint32_t>(ui.size());
			const UiVertex a{bx, by, 0, 0, white, 0, 0, 0, 2}, b{bx + size, by, 1, 0, white, 0, 0, 0, 2};
			const UiVertex c{bx + size, by + size, 1, 1, white, 0, 0, 0, 2}, d{bx, by + size, 0, 1, white, 0, 0, 0, 2};
			ui.insert(ui.end(), {a, b, c, a, c, d});
			m_achievement_images.push_back({first_vertex, 6, badge.set});
		}
		else
		{
			Fonts::AddRoundedRect(ui, bx, by, size, size, 14 * k, Rgba(1, 1, 1, 0.1f));
			m_fonts->AddText(ui, "RA", bx + size * 0.5f, by + size * 0.5f + 11 * k, 30 * k, muted, 0.2f, Fonts::Center);
		}
		const float tx = bx + size + 32 * k, room = x + width - tx - 8 * k;
		const std::string status = (entry.unlocked ? Tr(Str::AchievementStatusUnlocked) : Tr(Str::AchievementStatusLocked)) + std::string("  |  ") +
			                       std::to_string(entry.points) + " " + Tr(Str::AchievementPoints);
		const float status_width = m_fonts->Measure(status.c_str(), 27 * k);
		m_fonts->AddText(ui, Fit(*m_fonts, entry.title, 35 * k, room - status_width - 32 * k).c_str(), tx, top + 52 * k, 35 * k, white);
		m_fonts->AddText(ui, status.c_str(), x + width - 8 * k, top + 52 * k, 27 * k, entry.unlocked ? green : muted, 0.2f, Fonts::Right);
		float baseline = top + 96 * k;
		for (const auto& line : Wrap(*m_fonts, entry.description, 29 * k, room, 2))
		{
			m_fonts->AddText(ui, line.c_str(), tx, baseline, 29 * k, muted);
			baseline += 36 * k;
		}
	}
	const std::string footer = std::to_string(count ? m_achievement_row + 1 : 0) + " / " + std::to_string(count) +
		                       "   |   " + (m_game_achievements.busy ? Tr(Str::AchievementImages) : Tr(Str::AchievementTriangleRefresh));
	m_fonts->AddText(ui, footer.c_str(), x, y + height - 10 * k, 28 * k, muted);
}

void App::BuildSheet(std::vector<UiVertex>& ui, float W, float H, float k, uint32_t accent)
{
	const float e = Smoothstep(0.0f, 1.0f, m_sheet_anim);
	const float sw = kSheetW * k, top = kSheetTop * k, sh = H - top - kSheetBottomGap * k;
	const float margin = 110.0f * k;
	const float x = W - margin - sw + (1.0f - e) * (sw + margin + 60.0f * k);
	const float y = top;
	const size_t begin = ui.size();

	const uint32_t hi = Rgba(1, 1, 1), mid = Rgba(0.74f, 0.72f, 0.84f), lo = Rgba(0.52f, 0.50f, 0.64f);
	const uint32_t own = Rgba(Mix(m_glow[0], 1.0f, 0.45f), Mix(m_glow[1], 1.0f, 0.45f), Mix(m_glow[2], 1.0f, 0.45f));

	// The panel: a soft shadow, a hairline edge, the body.
	Fonts::AddRoundedRect(ui, x - 6 * k, y + 10 * k, sw + 12 * k, sh + 12 * k, 48 * k, Rgba(0, 0, 0, 0.35f));
	Fonts::AddRoundedRect(ui, x - 2 * k, y - 2 * k, sw + 4 * k, sh + 4 * k, 44 * k, Rgba(1, 1, 1, 0.16f));
	Fonts::AddRoundedRect(ui, x, y, sw, sh, 42 * k, Rgba(0.045f, 0.050f, 0.105f, 0.985f)); // the shelf's text must not show through

	const float cx = x + 64 * k, inner = sw - 128 * k;
	// 2026-10-05 (AI-assisted; swordpdf: "the square ui in general is a little cramped"): a wider sheet, and its top in two
	// lines that each have room: the title with this game / all games (L1 / R1) on the right, then the tabs (L2 / R2) as one
	// strip across the sheet, the shown tab a white pill as the shown scope. vk-285-117's title kept its line to itself;
	// the strip below now holds the three tabs that used to share a line with the scope.
	const char* const this_label = Tr(Str::SheetThisGame);
	const char* const all_label = Tr(Str::SheetAllGames);
	const float ph = 60 * k, ppx = 30 * k, pad = 50 * k, pgap = 14 * k;
	const float this_w = m_fonts->Measure(this_label, ppx) + pad, all_w = m_fonts->Measure(all_label, ppx) + pad;
	const float pills_w = this_w + pgap + all_w;
	{
		const float room = inner - pills_w - 48 * k;
		float px = 52 * k;
		const std::string& t = m_sheet.title();
		while (px > 40 * k && m_fonts->Measure(t.c_str(), px) > room)
			px -= 2 * k;
		m_fonts->AddText(ui, Fit(*m_fonts, t, px, room).c_str(), cx, y + 112 * k, px, hi, 0.55f);
	}
	{
		const float py = y + 66 * k;
		float px = cx + inner - pills_w;
		auto pill = [&](const char* label, float w, bool on, bool enabled) {
			if (on)
				Fonts::AddRoundedRect(ui, px, py, w, ph, ph * 0.5f, Rgba(1, 1, 1, 0.92f));
			else
				Fonts::AddRoundedRect(ui, px, py, w, ph, ph * 0.5f, Rgba(1, 1, 1, enabled ? 0.10f : 0.04f));
			m_fonts->AddText(ui, label, px + w * 0.5f, py + ph * 0.5f + 11 * k, ppx, on ? Rgba(0.07f, 0.06f, 0.16f) : (enabled ? mid : lo),
				0.5f, Fonts::Center);
			px += w + pgap;
		};
		pill(this_label, this_w, !m_sheet_global, !m_games.empty());
		pill(all_label, all_w, m_sheet_global, true);
	}
	{
		// The tab strip: [L2] Settings | Controls | Achievements [R2], each tab a third of the room between the keys.
		const float ty = y + 168 * k, th = 74 * k, kpx = 24 * k, tpx = 32 * k;
		Fonts::AddRoundedRect(ui, cx, ty, inner, th, th * 0.5f, Rgba(1, 1, 1, 0.05f));
		const float kh = 40 * k;
		auto key_pill = [&](const char* name, float kx) {
			const float kw = m_fonts->Measure(name, kpx) + 20 * k;
			Fonts::AddRoundedRect(ui, kx, ty + (th - kh) * 0.5f, kw, kh, 10 * k, Rgba(1, 1, 1, 0.82f));
			m_fonts->AddText(ui, name, kx + kw * 0.5f, ty + th * 0.5f + 9 * k, kpx, Rgba(0.08f, 0.08f, 0.14f), 0.6f, Fonts::Center);
			return kw;
		};
		const float l2 = key_pill("L2", cx + 16 * k);
		const float r2w = m_fonts->Measure("R2", kpx) + 20 * k;
		key_pill("R2", cx + inner - 16 * k - r2w);
		const float sx0 = cx + 16 * k + l2 + 14 * k, sx1 = cx + inner - 16 * k - r2w - 14 * k;
		const float seg = (sx1 - sx0) / 3.0f;
		const int shown = m_sheet.tab() == kTabControls ? 1 : m_sheet.tab() == kTabAchievements ? 2 : 0;
		const char* const names[3] = {Tr(Str::HintSettings), Tr(Str::SheetControls), Tr(Str::Achievements)};
		for (int t = 0; t < 3; t++)
		{
			const float sx = sx0 + t * seg;
			const bool on = t == shown;
			if (on)
				Fonts::AddRoundedRect(ui, sx + 4 * k, ty + 7 * k, seg - 8 * k, th - 14 * k, (th - 14 * k) * 0.5f, Rgba(1, 1, 1, 0.92f));
			const std::string label = Fit(*m_fonts, names[t], tpx, seg - 36 * k);
			m_fonts->AddText(ui, label.c_str(), sx + seg * 0.5f, ty + th * 0.5f + 11 * k, tpx, on ? Rgba(0.07f, 0.06f, 0.16f) : mid,
				on ? 0.5f : 0.3f, Fonts::Center);
		}
	}
	Fonts::AddRoundedRect(ui, cx, y + 280 * k, inner, 2 * k, 0, Rgba(1, 1, 1, 0.12f));

	if (m_sheet.tab() == kTabAchievements)
	{
		BuildGameAchievements(ui, cx, y + 310 * k, inner, sh - 370 * k, k, accent);
		FadeRange(ui, begin, ui.size(), e);
		return;
	}

	// vk-285-135: the folder picker, in the rows' place.
	if (m_picker.open)
	{
		BuildPicker(ui, x, sw, y, sh, k, accent);
		FadeRange(ui, begin, ui.size(), Clamp(e * 1.4f, 0.0f, 1.0f));
		if (m_picker.osk) // vk-285-139: the panel's keyboard for a share's address, over the shelf
			BuildPickerKeyboard(ui, W, H, k, accent);
		return;
	}

	// The rows: a list that scrolls to keep the focused row in view.
	const auto& rows = m_sheet.rows();
	const float list_top = y + kSheetListTop * k, list_bottom = y + sh - kSheetHelpH * k;
	const float view_h = (list_bottom - list_top) / k;
	std::vector<float> offs(rows.size());
	float acc = 0, focus_top = 0, focus_bottom = 0;
	for (size_t i = 0; i < rows.size(); i++)
	{
		offs[i] = acc;
		const float hgt = rows[i].kind == OptionsSheet::Kind::Header ? (rows[i].label.empty() ? 30.0f : kHeaderH) : kRowH;
		if (static_cast<int>(i) == m_sheet_row)
		{
			// The header just above the focused row comes into view with it.
			focus_top = (i > 0 && rows[i - 1].kind == OptionsSheet::Kind::Header) ? offs[i - 1] : acc;
			focus_bottom = acc + hgt;
		}
		acc += hgt;
	}
	const float total = acc;
	if (focus_top < m_sheet_scroll_target + 20.0f)
		m_sheet_scroll_target = std::max(0.0f, focus_top - 20.0f);
	if (focus_bottom > m_sheet_scroll_target + view_h - 20.0f)
		m_sheet_scroll_target = focus_bottom - view_h + 20.0f;
	m_sheet_scroll_target = std::max(0.0f, std::min(m_sheet_scroll_target, std::max(0.0f, total - view_h)));

	for (size_t i = 0; i < rows.size(); i++)
	{
		const OptionsSheet::Row& r = rows[i];
		const float ry = list_top + (offs[i] - m_sheet_scroll) * k;
		const bool header = r.kind == OptionsSheet::Kind::Header;
		const float rh = (header ? (r.label.empty() ? 30.0f : kHeaderH) : kRowH) * k;
		if (ry < list_top - 2 * k || ry + rh > list_bottom + 2 * k)
			continue; // outside the list's window
		if (header)
		{
			if (!r.label.empty())
			{
				std::string upper = r.label;
				for (char& c : upper)
					c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
				m_fonts->AddText(ui, upper.c_str(), cx, ry + 54 * k, 26 * k, lo, 0.45f);
			}
			continue;
		}
		const bool focused = static_cast<int>(i) == m_sheet_row;
		const float bx = x + 32 * k, bw = sw - 64 * k, bh = rh - 10 * k;
		if (focused)
		{
			Fonts::AddRoundedRect(ui, bx, ry, bw, bh, 26 * k, Rgba(1, 1, 1, 0.11f));
			Fonts::AddRoundedRect(ui, bx + 14 * k, ry + 24 * k, 6 * k, bh - 48 * k, 3 * k, accent);
		}
		const float mid_y = ry + bh * 0.5f + 13 * k; // the text's baseline, centred in the row
		const float lpx = 40 * k, vpx = 38 * k; // 2026-10-05: 38 and 36 before
		const bool action = r.kind == OptionsSheet::Kind::Recommended || r.kind == OptionsSheet::Kind::ResetAll ||
		                    r.kind == OptionsSheet::Kind::SystemMenu; // 2026-10-08

		std::string value = m_sheet.Value(r);
		const bool armed = m_sheet.Armed(r, m_time);
		if (armed)
			value = "Press again";
		else if (action && r.kind == OptionsSheet::Kind::ResetAll)
			value.clear();
		if (r.kind == OptionsSheet::Kind::NewCard && focused)
			value += "  \xC2\xB7  Create";
		if (r.kind == OptionsSheet::Kind::OnlinePatches) // 2026-10-08
			value = OnlinePatchValue();
		// 2026-10-05: the HD texture pack row shows the pack, the download's progress or the installed pack.
		const bool pack_row = r.kind == OptionsSheet::Kind::TexturePack && m_cfg.texture_packs && !m_games.empty();
		TexturePackStatus pack;
		bool pick_arrows = true;
		if (pack_row)
		{
			const std::string& serial = m_games[static_cast<size_t>(m_selected)].serial;
			pack = m_cfg.texture_packs.status(serial);
			const auto it = m_texpack_pick.find(serial);
			const int pick = it != m_texpack_pick.end() ? it->second : std::max(0, pack.job_pack);
			value = TexturePackValue(pack, pick);
			using TS = TexturePackStatus::State;
			pick_arrows = pack.packs.size() > 1 && (pack.state == TS::Available || pack.state == TS::Failed || pack.state == TS::NeedSpace);
			if (m_texpack_armed == serial && m_time <= m_texpack_armed_until)
				value = "Press again";
		}
		// vk-285-135: the Folders rows open a list (Cross); left and right don't change them.
		const bool folder_row = r.kind == OptionsSheet::Kind::GameFolders || r.kind == OptionsSheet::Kind::BiosFolder ||
		                        r.kind == OptionsSheet::Kind::NfsShares;
		if (folder_row && focused)
			value += "  \xE2\x80\xBA";
		const bool arrows = focused && !action && pick_arrows && !folder_row;

		const float value_room = bw * (pack_row ? 0.68f : 0.46f); // 2026-10-05: the pack row's label is short, its value long
		// vk-285-116: a label or a value that starts with a button's symbol ("<glyph>  Cross", the Controls tab): the symbol
		// a size up, the label's in a slot of its own so the names line up.
		std::string lglyph, lname, vglyph, vname;
		const bool lsym = SplitSymbol(r.label, lglyph, lname), vsym = SplitSymbol(value, vglyph, vname);
		const float slot = lsym ? 66 * k : 0.0f;
		const std::string label = Fit(*m_fonts, lsym ? lname : r.label, lpx,
			bw - 80 * k - slot - (value.empty() ? 0.0f : std::min(value_room, m_fonts->Measure(value.c_str(), vpx)) + 90 * k));
		const uint32_t lc = action ? (focused ? hi : mid) : hi;
		if (lsym && lglyph != icon::Blank)
			m_fonts->AddText(ui, lglyph.c_str(), bx + 48 * k + slot * 0.42f, mid_y + kSymbolDrop * lpx, lpx * kSymbolScale, lc, 0.25f,
				Fonts::Center);
		m_fonts->AddText(ui, label.c_str(), bx + 48 * k + slot, mid_y, lpx, lc, 0.25f);

		if (!value.empty())
		{
			const std::string shown = Fit(*m_fonts, vsym ? vname : value, vpx, value_room - (vsym ? 64 * k : 0.0f));
			const OptionsSheet::From from = m_sheet.Source(r);
			uint32_t vc = from == OptionsSheet::From::Own ? own : mid;
			if (armed || (action && focused))
				vc = own;
			if (pack_row)
			{
				using TS = TexturePackStatus::State;
				vc = pack.state == TS::Installed ? own : (pack.state == TS::Failed || pack.state == TS::NeedSpace) ? Rgba(1.0f, 0.62f, 0.60f) : mid;
			}
			const float vx = bx + bw - 44 * k - (arrows ? 30 * k : 0.0f);
			float vw = m_fonts->AddText(ui, shown.c_str(), vx, mid_y, vpx, vc, 0.5f, Fonts::Right);
			if (vsym && vglyph != icon::Blank)
				vw += 14 * k + m_fonts->AddText(ui, vglyph.c_str(), vx - vw - 14 * k, mid_y + kSymbolDrop * vpx, vpx * kSymbolScale, vc, 0.5f,
								   Fonts::Right);
			if (arrows)
			{
				// The arrows either side of a value that left and right change.
				m_fonts->AddText(ui, "\xE2\x80\xB9", vx - vw - 26 * k, mid_y + 1 * k, 44 * k, hi, 0.4f, Fonts::Center);
				m_fonts->AddText(ui, "\xE2\x80\xBA", vx + 26 * k, mid_y + 1 * k, 44 * k, hi, 0.4f, Fonts::Center);
			}
			// A dot beside a value this file sets itself (not what it follows).
			if (from == OptionsSheet::From::Own && !action && !focused)
				Fonts::AddRoundedRect(ui, vx - vw - 30 * k, mid_y - 18 * k, 12 * k, 12 * k, 6 * k, own);
		}
		// 2026-10-05: a pack on its way: a thin bar along the row's foot.
		if (pack_row && pack.total > 0)
		{
			using TS = TexturePackStatus::State;
			if (pack.state == TS::Checking || pack.state == TS::Downloading || pack.state == TS::Unpacking)
			{
				const float fx = bx + 48 * k, fw = bw - 92 * k, fy = ry + bh - 16 * k;
				const float f = std::clamp(static_cast<float>(pack.done) / static_cast<float>(pack.total), 0.0f, 1.0f);
				Fonts::AddRoundedRect(ui, fx, fy, fw, 5 * k, 2.5f * k, Rgba(1, 1, 1, 0.10f));
				if (f > 0)
					Fonts::AddRoundedRect(ui, fx, fy, std::max(5 * k, fw * f), 5 * k, 2.5f * k, accent);
			}
		}
	}
	// A hint that there is more above or below.
	if (m_sheet_scroll > 4.0f)
		m_fonts->AddText(ui, "\xE2\x80\xA2 \xE2\x80\xA2 \xE2\x80\xA2", x + sw * 0.5f, list_top - 10 * k, 22 * k, lo, 0.3f, Fonts::Center);
	if (m_sheet_scroll + view_h < total - 4.0f)
		m_fonts->AddText(ui, "\xE2\x80\xA2 \xE2\x80\xA2 \xE2\x80\xA2", x + sw * 0.5f, list_bottom + 22 * k, 22 * k, lo, 0.3f, Fonts::Center);

	// What the focused row does, and what the last change did.
	Fonts::AddRoundedRect(ui, cx, list_bottom + 44 * k, inner, 2 * k, 0, Rgba(1, 1, 1, 0.12f));
	if (m_sheet_row >= 0 && m_sheet_row < static_cast<int>(rows.size()))
	{
		const OptionsSheet::Row& focus_row = rows[static_cast<size_t>(m_sheet_row)];
		std::string help = m_sheet.Help(focus_row);
		if (focus_row.kind == OptionsSheet::Kind::TexturePack && m_cfg.texture_packs && !m_games.empty())
		{
			const std::string& serial = m_games[static_cast<size_t>(m_selected)].serial;
			const TexturePackStatus pack = m_cfg.texture_packs.status(serial);
			const auto it = m_texpack_pick.find(serial);
			help = TexturePackHelp(pack, it != m_texpack_pick.end() ? it->second : std::max(0, pack.job_pack), serial);
		}
		float hy = list_bottom + 110 * k;
		for (const std::string& line : Wrap(*m_fonts, help, 32 * k, inner, 4))
		{
			m_fonts->AddText(ui, line.c_str(), cx, hy, 32 * k, mid, 0.1f);
			hy += 46 * k;
		}
	}
	if (!m_sheet_status.empty() && m_time - m_sheet_status_time < 3.0)
	{
		const float a = 1.0f - Smoothstep(2.2f, 3.0f, static_cast<float>(m_time - m_sheet_status_time));
		const uint32_t c = (own & 0x00FFFFFFu) | (static_cast<uint32_t>(a * 255.0f) << 24);
		m_fonts->AddText(ui, Fit(*m_fonts, m_sheet_status, 30 * k, inner).c_str(), cx, y + sh - 40 * k, 30 * k, c, 0.4f);
	}

	FadeRange(ui, begin, ui.size(), Clamp(e * 1.4f, 0.0f, 1.0f));
}
// ---- vk-285-135: the sheet's folder picker and NFS share list (AI-assisted) ------------------------------------------
// swordpdf: "also where is the option to select what dir is set for games and bios?", "i also want to pick a folder though the
// browser in the shelf". The sheet for all games' Folders rows open it in the sheet's place: the places first (the folders
// added already, /data/PCSX2, the drives, the NFS shares), then a folder's folders, "Use this folder" at the top with the
// games found there. The choice goes into gs.ini (the sheet's own file), which main-boot.cpp reads when PS5SX2 starts.

namespace
{
bool IsFolder(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// A folder's folders (sorted, hidden and system ones left out, at most 500) and how many disc images are in it.
void ListFolder(const std::string& dir, std::vector<std::string>& folders, int& images)
{
	folders.clear();
	images = 0;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (const dirent* e = readdir(d))
		{
			if (e->d_name[0] == '.' || e->d_name[0] == '$')
				continue;
			if (IsDiscImageName(e->d_name))
			{
				images++;
				continue;
			}
			bool folder = e->d_type == DT_DIR;
			if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK)
				folder = IsFolder(dir + "/" + e->d_name);
			if (folder && folders.size() < 500)
				folders.emplace_back(e->d_name);
		}
		closedir(d);
	}
	std::sort(folders.begin(), folders.end(), [](const std::string& a, const std::string& b) {
		const size_t n = std::min(a.size(), b.size());
		for (size_t i = 0; i < n; i++)
		{
			const int ca = std::tolower(static_cast<unsigned char>(a[i])), cb = std::tolower(static_cast<unsigned char>(b[i]));
			if (ca != cb)
				return ca < cb;
		}
		return a.size() < b.size();
	});
}

std::string Parent(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos || slash == 0 ? std::string("/") : path.substr(0, slash);
}

bool StartsWithNoCase(const std::string& s, const char* prefix)
{
	for (size_t i = 0; prefix[i]; i++)
		if (i >= s.size() || std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
			return false;
	return true;
}
} // namespace

void App::OpenPicker(OptionsSheet::Kind kind)
{
	m_picker = FolderPicker();
	m_picker.open = true;
	m_picker.kind = kind;
	PickerList("");
	std::printf("[options] %s\n", kind == OptionsSheet::Kind::NfsShares ? "NFS share list" :
	                              kind == OptionsSheet::Kind::BiosFolder ? "folder picker for the BIOS folder" : "folder picker for game folders");
	std::fflush(stdout);
}

void App::PickerList(const std::string& dir, const std::string& focus)
{
	FolderPicker& p = m_picker;
	p.dir = dir;
	p.items.clear();
	using T = PickerItem::Type;
	if (p.kind == OptionsSheet::Kind::NfsShares)
	{
		p.items.push_back({T::Add, "Add a share", "", "nfs://"});
		for (const std::string& share : SplitFolderList(m_sheet.OwnValue(kNfsSharesKey)))
			p.items.push_back({T::Share, share, share, ""});
	}
	else if (dir.empty())
	{
		if (p.kind == OptionsSheet::Kind::GameFolders)
			for (const std::string& f : SplitFolderList(m_sheet.OwnValue(kGameFoldersKey)))
				p.items.push_back({T::Listed, f, f, "Added"});
		for (const auto& [label, path] : m_cfg.folder_places)
			if (IsFolder(path))
				p.items.push_back({T::Place, label, path, path});
	}
	else
	{
		std::vector<std::string> folders;
		int images = 0;
		ListFolder(dir, folders, images);
		std::string value;
		if (p.kind == OptionsSheet::Kind::GameFolders)
		{
			// The games the shelf would list from it: its own, and those one folder down (as main-boot.cpp lists folders).
			int below = 0;
			for (size_t i = 0; i < folders.size() && i < 64; i++)
			{
				std::vector<std::string> sub;
				int n = 0;
				ListFolder(dir + "/" + folders[i], sub, n);
				below += n;
			}
			const int games = images + below;
			value = games == 0 ? "No games" : games == 1 ? "1 game" : std::to_string(games) + " games";
		}
		p.items.push_back({T::Use, "Use this folder", dir, value});
		for (const std::string& f : folders)
			p.items.push_back({T::Folder, f, dir + "/" + f, ""});
	}
	p.row = 0;
	for (size_t i = 0; i < p.items.size(); i++)
		if (!focus.empty() && p.items[i].path == focus)
			p.row = static_cast<int>(i);
	p.scroll = p.scroll_target = 0;
}

// vk-285-139: what a keyboard (the PS5's or the panel's) gave for a share's address: r > 0 typed, else cancelled.
void App::FinishShareText(int r, std::string text)
{
	FolderPicker& p = m_picker;
	auto status = [&](const std::string& t) {
		m_sheet_status = t;
		m_sheet_status_time = m_time;
	};
	while (!text.empty() && (text.back() == ' ' || text.back() == ';'))
		text.pop_back();
	while (!text.empty() && text.front() == ' ')
		text.erase(0, 1);
	// vk-285-138 (AI-assisted; swordpdf: "i cant edit my nfs path"): Cross on a listed share opens the keyboard with it, and
	// what comes back takes its place.
	const std::string editing = p.editing;
	p.editing.clear();
	if (r > 0 && text.size() > 6 && StartsWithNoCase(text, "nfs://") && text.find(';') == std::string::npos)
	{
		std::vector<std::string> list = SplitFolderList(m_sheet.OwnValue(kNfsSharesKey));
		const auto old = std::find(list.begin(), list.end(), editing);
		if (!editing.empty() && old != list.end())
		{
			*old = text;
			list.erase(std::unique(list.begin(), list.end()), list.end());
		}
		else if (std::find(list.begin(), list.end(), text) == list.end())
			list.push_back(text);
		const bool changed = text != editing;
		if (changed && m_sheet.SetOwn(kNfsSharesKey, JoinFolderList(list), (editing.empty() ? "NFS share added: " : "NFS share changed: ") + text))
			status((editing.empty() ? "Added " : "Changed to ") + text + ": restart PS5SX2 to mount it");
		else if (!changed)
			status("Unchanged");
		PickerList("", text);
		Sound(Sfx::Move, 0.3f);
	}
	else
	{
		status(r > 0 && !text.empty() && text != "nfs://" ? std::string("Not an nfs:// address: ") + (editing.empty() ? "nothing added" : "the share stays as it was")
		                                                   : std::string(editing.empty() ? "Nothing added" : "Unchanged"));
		Sound(Sfx::Edge, 0.3f);
	}
}

void App::UpdatePicker(double dt, const Input& in)
{
	FolderPicker& p = m_picker;
	auto pressed = [&](bool now, bool before) { return now && !before; };
	auto status = [&](const std::string& text) {
		m_sheet_status = text;
		m_sheet_status_time = m_time;
	};
	using T = PickerItem::Type;
	using K = OptionsSheet::Kind;

	// The PS5's keyboard is up for a share's address: it has the controller until it closes.
	if (p.typing)
	{
		std::string text;
		const int r = m_cfg.text_entry.poll ? m_cfg.text_entry.poll(text) : -1;
		if (r == 0)
			return;
		p.typing = false;
		FinishShareText(r, text);
		return;
	}
	// vk-285-139: the panel's own keyboard: D-pad to a key, Cross types it, Square deletes, Options done, Circle cancels.
	if (p.osk)
	{
		using Panel = AchievementAccountPanel;
		const bool up = pressed(in.up, m_prev.up), down = pressed(in.down, m_prev.down), left = pressed(in.left, m_prev.left),
				   right = pressed(in.right, m_prev.right);
		if (up || down)
		{
			const int to = std::clamp(p.osk_row + (down ? 1 : -1), 0, Panel::FnRow);
			if (to != p.osk_row)
			{
				const float x = Panel::KeyCentre(p.osk_page, p.osk_row, p.osk_col);
				int best = 0;
				for (int c = 1; c < Panel::RowKeys(p.osk_page, to); c++)
					if (std::abs(Panel::KeyCentre(p.osk_page, to, c) - x) < std::abs(Panel::KeyCentre(p.osk_page, to, best) - x))
						best = c;
				p.osk_row = to;
				p.osk_col = best;
				Sound(Sfx::Move, 0.3f);
			}
		}
		if (left || right)
		{
			const int to = std::clamp(p.osk_col + (right ? 1 : -1), 0, Panel::RowKeys(p.osk_page, p.osk_row) - 1);
			Sound(to != p.osk_col ? Sfx::Move : Sfx::Edge, 0.3f);
			p.osk_col = to;
		}
		auto erase = [&] {
			if (!p.osk_text.empty())
				p.osk_text.pop_back();
		};
		if (pressed(in.square, m_prev.square))
			erase();
		if (pressed(in.circle, m_prev.circle))
		{
			p.osk = false;
			FinishShareText(-1, {});
			return;
		}
		bool done = pressed(in.options, m_prev.options);
		if (pressed(in.cross, m_prev.cross))
		{
			Sound(Sfx::Move, 0.3f);
			if (p.osk_row < Panel::FnRow)
			{
				if (p.osk_text.size() < 255)
					p.osk_text += Panel::PageRows[p.osk_page][p.osk_row][p.osk_col];
			}
			else
				switch (p.osk_col)
				{
					case Panel::FnShift: p.osk_page = p.osk_page == 0 ? 1 : 0; break;
					case Panel::FnSymbols: p.osk_page = p.osk_page == 2 ? 0 : 2; break;
					case Panel::FnSpace:
						if (p.osk_text.size() < 255)
							p.osk_text += ' ';
						break;
					case Panel::FnDelete: erase(); break;
					case Panel::FnDone: done = true; break;
				}
			p.osk_col = std::min(p.osk_col, Panel::RowKeys(p.osk_page, p.osk_row) - 1);
		}
		if (done)
		{
			p.osk = false;
			FinishShareText(1, p.osk_text);
		}
		return;
	}
	// Circle: up a folder, from a place's top back to the places, from the places back to the sheet.
	if (pressed(in.circle, m_prev.circle) || pressed(in.options, m_prev.options))
	{
		if (p.dir.empty() || pressed(in.options, m_prev.options))
			p.open = false;
		else
		{
			bool place = false;
			for (const auto& pl : m_cfg.folder_places)
				place = place || pl.second == p.dir;
			const std::string from = p.dir;
			PickerList(place ? std::string() : Parent(p.dir), from);
		}
		Sound(Sfx::Move, 0.3f);
		return;
	}
	if (p.items.empty())
		return;

	// Up and down, repeating while held (as the sheet's rows).
	const int v = in.up ? -1 : in.down ? 1 : 0;
	const bool fresh = (in.up && !m_prev.up) || (in.down && !m_prev.down);
	auto move = [&](int d) {
		const int to = std::clamp(p.row + d, 0, static_cast<int>(p.items.size()) - 1);
		const bool moved = to != p.row;
		p.row = to;
		return moved;
	};
	if (v != 0 && fresh)
	{
		Sound(move(v) ? Sfx::Move : Sfx::Edge, 0.3f);
		p.held = v;
		p.held_for = 0;
		p.next_repeat = 0.34;
	}
	else if (v != 0 && v == p.held)
	{
		p.held_for += dt;
		bool moved = false;
		while (p.held_for >= p.next_repeat)
		{
			moved |= move(v);
			p.next_repeat += 0.085;
		}
		if (moved)
			Sound(Sfx::MoveRepeat, 0.3f);
	}
	else
		p.held = 0;

	const PickerItem item = p.items[static_cast<size_t>(p.row)];
	if (pressed(in.cross, m_prev.cross))
	{
		switch (item.type)
		{
			case T::Folder:
			case T::Place:
				PickerList(item.path);
				Sound(Sfx::Move, 0.3f);
				break;
			case T::Use:
				if (p.kind == K::BiosFolder)
				{
					m_sheet.SetOwn(kBiosFolderKey, item.path, "BIOS folder picked");
					status("BIOS folder: " + item.path + " (restart PS5SX2)");
				}
				else
				{
					std::vector<std::string> list = SplitFolderList(m_sheet.OwnValue(kGameFoldersKey));
					if (std::find(list.begin(), list.end(), item.path) != list.end())
						status(item.path + " is listed already");
					else
					{
						list.push_back(item.path);
						if (m_sheet.SetOwn(kGameFoldersKey, JoinFolderList(list), "game folder added"))
							status("Added " + item.path + ": restart PS5SX2 to list its games");
					}
				}
				std::printf("[options] %s picked: %s\n", p.kind == K::BiosFolder ? "BIOS folder" : "game folder", item.path.c_str());
				std::fflush(stdout);
				p.open = false;
				Sound(Sfx::Move, 0.3f);
				break;
			case T::Share: // vk-285-138: change it on the keyboard
			case T::Add:
				p.editing = item.type == T::Share ? item.path : std::string();
				if (m_cfg.text_entry.open && m_cfg.text_entry.open("NFS share: nfs://server/shared folder", item.type == T::Share ? item.path : item.value,
						false, 255))
					p.typing = true;
				else
				{
					// vk-285-139: the panel's own keyboard instead (swordpdf's 138: "the keyboard didnt open").
					p.osk = true;
					p.osk_text = item.type == T::Share ? item.path : item.value;
					p.osk_page = 0;
					p.osk_row = 1;
					p.osk_col = 0;
					Sound(Sfx::Move, 0.3f);
				}
				break;
			case T::Listed:
				status("Triangle removes it");
				Sound(Sfx::Edge, 0.3f);
				break;
		}
		return;
	}
	if (pressed(in.triangle, m_prev.triangle) && (item.type == T::Listed || item.type == T::Share))
	{
		const char* const key = item.type == T::Share ? kNfsSharesKey : kGameFoldersKey;
		std::vector<std::string> list = SplitFolderList(m_sheet.OwnValue(key));
		list.erase(std::remove(list.begin(), list.end(), item.path), list.end());
		if (m_sheet.SetOwn(key, JoinFolderList(list), std::string(item.type == T::Share ? "NFS share" : "game folder") + " removed: " + item.path))
			status("Removed " + item.path + " (restart PS5SX2)");
		const int keep = p.row;
		PickerList(p.dir);
		p.row = std::min(keep, std::max(0, static_cast<int>(p.items.size()) - 1));
		Sound(Sfx::Move, 0.3f);
	}
}

void App::BuildPicker(std::vector<UiVertex>& ui, float x, float sw, float y, float sh, float k, uint32_t accent)
{
	const FolderPicker& p = m_picker;
	using T = PickerItem::Type;
	using K = OptionsSheet::Kind;
	const uint32_t hi = Rgba(1, 1, 1), mid = Rgba(0.74f, 0.72f, 0.84f), lo = Rgba(0.52f, 0.50f, 0.64f);
	const uint32_t own = Rgba(Mix(m_glow[0], 1.0f, 0.45f), Mix(m_glow[1], 1.0f, 0.45f), Mix(m_glow[2], 1.0f, 0.45f));
	const float cx = x + 64 * k, inner = sw - 128 * k;
	const float list_top = y + kSheetListTop * k, list_bottom = y + sh - kSheetHelpH * k;

	// Where it is: what for, and the folder shown.
	const char* what = p.kind == K::NfsShares ? "NFS SHARES" : p.kind == K::BiosFolder ? "THE BIOS FOLDER" : "A GAME FOLDER";
	const std::string head = p.kind == K::NfsShares ? std::string(what) : std::string("PICK ") + what;
	m_fonts->AddText(ui, head.c_str(), cx, list_top + 40 * k, 26 * k, lo, 0.45f);
	if (!p.dir.empty())
		m_fonts->AddText(ui, Fit(*m_fonts, p.dir, 30 * k, inner).c_str(), cx, list_top + 84 * k, 30 * k, mid, 0.3f);
	else if (p.kind != K::NfsShares)
		m_fonts->AddText(ui, "Where to look", cx, list_top + 84 * k, 30 * k, mid, 0.3f);

	const float rows_top = list_top + 110 * k;
	const float view_h = (list_bottom - rows_top) / k;
	const float focus_top = p.row * kRowH, focus_bottom = focus_top + kRowH;
	FolderPicker& mp = m_picker;
	if (focus_top < mp.scroll_target + 20.0f)
		mp.scroll_target = std::max(0.0f, focus_top - 20.0f);
	if (focus_bottom > mp.scroll_target + view_h - 20.0f)
		mp.scroll_target = focus_bottom - view_h + 20.0f;
	mp.scroll_target = std::clamp(mp.scroll_target, 0.0f, std::max(0.0f, p.items.size() * kRowH - view_h));
	mp.scroll += (mp.scroll_target - mp.scroll) * 0.35f;

	if (p.items.empty())
		m_fonts->AddText(ui, "Nothing here", cx, rows_top + 60 * k, 34 * k, lo, 0.3f);
	for (size_t i = 0; i < p.items.size(); i++)
	{
		const PickerItem& it = p.items[i];
		const float ry = rows_top + (i * kRowH - mp.scroll) * k;
		const float rh = kRowH * k;
		if (ry < rows_top - 2 * k || ry + rh > list_bottom + 2 * k)
			continue;
		const bool focused = static_cast<int>(i) == p.row;
		const float bx = x + 32 * k, bw = sw - 64 * k, bh = rh - 10 * k;
		if (focused)
		{
			Fonts::AddRoundedRect(ui, bx, ry, bw, bh, 26 * k, Rgba(1, 1, 1, 0.11f));
			Fonts::AddRoundedRect(ui, bx + 14 * k, ry + 24 * k, 6 * k, bh - 48 * k, 3 * k, accent);
		}
		const float mid_y = ry + bh * 0.5f + 13 * k;
		const bool action = it.type == T::Use || it.type == T::Add;
		const char* const mark = it.type == T::Folder || it.type == T::Place ? "\xE2\x80\xBA" : ""; // a folder to go into
		const float vpx = 34 * k, lpx = 40 * k;
		const float value_w = it.value.empty() ? 0.0f : std::min(bw * 0.42f, m_fonts->Measure(it.value.c_str(), vpx));
		const std::string label = Fit(*m_fonts, it.label, lpx, bw - 120 * k - (value_w > 0 ? value_w + 60 * k : 0.0f));
		m_fonts->AddText(ui, label.c_str(), bx + 48 * k, mid_y, lpx, action ? own : hi, 0.25f);
		float vx = bx + bw - 44 * k;
		if (*mark)
		{
			m_fonts->AddText(ui, mark, vx, mid_y + 1 * k, 44 * k, focused ? hi : lo, 0.4f, Fonts::Right);
			vx -= 46 * k;
		}
		if (!it.value.empty())
			m_fonts->AddText(ui, Fit(*m_fonts, it.value, vpx, bw * 0.42f).c_str(), vx, mid_y, vpx,
				it.type == T::Listed ? own : mid, 0.4f, Fonts::Right);
	}

	// What the buttons do here.
	Fonts::AddRoundedRect(ui, cx, list_bottom + 44 * k, inner, 2 * k, 0, Rgba(1, 1, 1, 0.12f));
	std::string help;
	const PickerItem* it = p.items.empty() ? nullptr : &p.items[static_cast<size_t>(p.row)];
	if (p.osk)
		help = "Type the share's address, nfs://<server>/<shared folder>: Cross types the key, Square deletes, Options is done, Circle "
		       "cancels. Add ?version=4 for an NFS v4 server.";
	else if (p.typing)
		help = "Type the share's address on the PS5's keyboard: nfs://<server>/<shared folder>, as the server shares it (a folder "
		       "inside it works too). Add ?version=4 for an NFS v4 server.";
	else if (it && it->type == T::Use)
		help = p.kind == K::BiosFolder ? "Cross: look for the BIOS here first. Circle: back."
		                               : "Cross: list the games in this folder (and in the folders in it). Circle: back.";
	else if (it && (it->type == T::Listed || it->type == T::Share))
		help = it->type == T::Share ? std::string("Cross: change it on the PS5's keyboard. Triangle: remove it from the shares. Circle: back to the sheet.")
		                            : std::string("Triangle: remove it from the game folders. Circle: back to the sheet.");
	else if (it && it->type == T::Add)
		help = "Cross: type a share's address on the PS5's keyboard (nfs://<server>/<shared folder>). Circle: back to the sheet.";
	else
		help = std::string("Cross: open the folder. ") + (p.dir.empty() ? "Circle: back to the sheet." : "Circle: up a folder.");
	if (!p.typing && p.kind != K::NfsShares)
		help += " Changes take effect when PS5SX2 starts.";
	float hy = list_bottom + 110 * k;
	for (const std::string& line : Wrap(*m_fonts, help, 32 * k, inner, 4))
	{
		m_fonts->AddText(ui, line.c_str(), cx, hy, 32 * k, mid, 0.1f);
		hy += 46 * k;
	}
	if (!m_sheet_status.empty() && m_time - m_sheet_status_time < 3.0)
	{
		const float a = 1.0f - Smoothstep(2.2f, 3.0f, static_cast<float>(m_time - m_sheet_status_time));
		const uint32_t c = (own & 0x00FFFFFFu) | (static_cast<uint32_t>(a * 255.0f) << 24);
		m_fonts->AddText(ui, Fit(*m_fonts, m_sheet_status, 30 * k, inner).c_str(), cx, y + sh - 40 * k, 30 * k, c, 0.4f);
	}
}

// ---- 2026-10-05: the RetroAchievements account panel (L1 + Square), redesigned (AI-assisted) -------------------------
// swordpdf: "we gotta redesign the ra login screen, it now looks trash. use the shell keyboard and make the ui for the inputs
// match our current ui". The panel now has the options sheet's look: a centred panel with its shadow, edge and body over a
// dimmed shelf, the fields drawn as the sheet's rows (the focused one lit, the cover's colour at its edge), and a white
// pill for the button in focus. Cross on a field opens the PS5's own keyboard (TextEntryService) or, where the app can't
// open it, the panel's keyboard, which slides up under the panel. While the PS5's keyboard is up it has the controller.

void App::UpdateAccount(const Input& in)
{
	using Panel = AchievementAccountPanel;
	auto pressed = [](bool now, bool before) { return now && !before; };
	if (m_ime_field >= 0)
	{
		std::string text;
		const int r = m_cfg.text_entry.poll ? m_cfg.text_entry.poll(text) : -1;
		if (r != 0)
		{
			if (r > 0)
			{
				m_account.SetField(m_ime_field, text);
				// As the panel's Done: on to the next field, then to Sign in.
				if (m_ime_field < Panel::RowSignIn)
					m_account.row = m_ime_field + 1;
			}
			m_ime_field = -1;
		}
		std::fill(text.begin(), text.end(), '\0'); // it may hold the password
		return;
	}
	m_account.Move(pressed(in.right, m_prev.right) - pressed(in.left, m_prev.left),
		pressed(in.down, m_prev.down) - pressed(in.up, m_prev.up));
	if (pressed(in.circle, m_prev.circle))
		m_account.Back();
	else if (pressed(in.triangle, m_prev.triangle))
		m_account.Erase();
	else if (pressed(in.square, m_prev.square))
	{
		if (!m_account.SignedIn() && m_account.row == Panel::RowPassword)
			m_account.TogglePasswordVisibility();
	}
	else if (pressed(in.cross, m_prev.cross) && m_account.Accept(m_cfg.achievements, m_time))
	{
		// A field: the PS5's keyboard if the app can open it, the panel's otherwise.
		const int field = m_account.row;
		const bool password = field == Panel::RowPassword;
		if (m_cfg.text_entry.open &&
			m_cfg.text_entry.open(Tr(password ? Str::AccountPassword : Str::AccountUsername), m_account.Field(field), password,
				password ? Panel::MaxPassword : Panel::MaxUsername))
			m_ime_field = field;
		else
			m_account.StartKeyboard();
	}
}

void App::BuildAccount(std::vector<UiVertex>& ui, float W, float H, float k, uint32_t accent)
{
	using Panel = AchievementAccountPanel;
	const float e = Smoothstep(0.0f, 1.0f, m_account_anim);
	// (The shelf behind is dimmed in Build, under the button hints, so far that its covers and titles don't compete.)
	const size_t begin = ui.size();

	const uint32_t hi = Rgba(1, 1, 1), mid = Rgba(0.74f, 0.72f, 0.84f), lo = Rgba(0.52f, 0.50f, 0.64f);
	const uint32_t own = Rgba(Mix(m_glow[0], 1.0f, 0.45f), Mix(m_glow[1], 1.0f, 0.45f), Mix(m_glow[2], 1.0f, 0.45f));
	const uint32_t ink = Rgba(0.07f, 0.06f, 0.16f); // text on a white pill
	const AchievementAccountState& acc = m_account.account;
	const bool signed_in = m_account.SignedIn();
	const bool busy = acc.busy;

	// The panel's size: the header, then the two fields and Sign in, or the account and Sign out.
	const float pw = 1760 * k, inner = pw - 2 * 88 * k;
	const float intro_px = 32 * k, intro_lh = 46 * k, note_px = 32 * k, note_lh = 46 * k;
	// Signed in, the header says who ("Signed in as <name>", the name in the cover's colour: the format's words either
	// side of its %s), and the body only what that means and Sign out.
	const bool show_account = signed_in && !busy;
	std::vector<std::string> intro_lines, note_lines;
	std::string signed_in_before, signed_in_after;
	if (show_account)
	{
		const std::string format = Tr(Str::AccountSignedIn);
		const size_t at = format.find("%s");
		signed_in_before = format.substr(0, at);
		signed_in_after = at == std::string::npos ? std::string() : format.substr(at + 2);
		intro_lines.emplace_back(); // one line, drawn in parts
		note_lines = Wrap(*m_fonts, Tr(Str::AccountSignedInNote), note_px, inner, 3);
	}
	else
		intro_lines = Wrap(*m_fonts, Tr(busy ? Str::AccountSigningIn : Str::AccountIntro), intro_px, inner, 2);
	const float header_h = 210 * k + intro_lines.size() * intro_lh; // the separator's place
	const float field_block = 52 * k + 116 * k + 46 * k;             // a field's label, box and the gap after it
	const float body_h = signed_in ? 226 * k + note_lines.size() * note_lh : 2 * field_block + 210 * k;
	const float ph = header_h + 40 * k + body_h;
	const float kb_h = 5 * 108 * k + 4 * 16 * k + 2 * 48 * k + 64 * k; // the panel's keyboard (BuildAccountKeyboard)
	const float kbe = Smoothstep(0.0f, 1.0f, m_account_kb_anim);
	const float centred = (H - ph) * 0.5f - 40 * k;
	const float raised = std::max(150 * k, H - 200 * k - kb_h - 40 * k - ph); // room for the keyboard under it
	const float px = (W - pw) * 0.5f, py = centred + (raised - centred) * kbe;
	const float cx = px + 88 * k;

	Fonts::AddRoundedRect(ui, px - 6 * k, py + 10 * k, pw + 12 * k, ph + 12 * k, 48 * k, Rgba(0, 0, 0, 0.35f));
	Fonts::AddRoundedRect(ui, px - 2 * k, py - 2 * k, pw + 4 * k, ph + 4 * k, 44 * k, Rgba(1, 1, 1, 0.16f));
	Fonts::AddRoundedRect(ui, px, py, pw, ph, 42 * k, Rgba(0.045f, 0.050f, 0.105f, 0.985f));

	m_fonts->AddText(ui, "RetroAchievements", cx, py + 124 * k, 60 * k, hi, 0.55f);
	float iy = py + 194 * k;
	if (show_account)
	{
		const float apx = 36 * k;
		const float name_room = inner - m_fonts->Measure(signed_in_before.c_str(), apx) - m_fonts->Measure(signed_in_after.c_str(), apx);
		float tx = cx + m_fonts->AddText(ui, signed_in_before.c_str(), cx, iy, apx, hi, 0.15f);
		tx += m_fonts->AddText(ui, Fit(*m_fonts, acc.username, apx, name_room).c_str(), tx, iy, apx, own, 0.5f);
		m_fonts->AddText(ui, signed_in_after.c_str(), tx, iy, apx, hi, 0.15f);
	}
	else
		for (const std::string& line : intro_lines)
		{
			m_fonts->AddText(ui, line.c_str(), cx, iy, intro_px, mid, 0.15f);
			iy += intro_lh;
		}
	const float sep = py + header_h;
	Fonts::AddRoundedRect(ui, cx, sep, inner, 2 * k, 0, Rgba(1, 1, 1, 0.12f));

	// A small "<button> label" on the right of a field, in the hint bar's words.
	auto field_hint = [&](float right, float baseline, const char* glyph, const char* label) {
		const float tpx = 30 * k;
		const float w = m_fonts->AddText(ui, label, right, baseline, tpx, mid, 0.1f, Fonts::Right);
		m_fonts->AddText(ui, glyph, right - w - 12 * k, baseline + 0.12f * tpx, tpx * 1.35f, mid, 0.1f, Fonts::Right);
		return w + 12 * k + m_fonts->Measure(glyph, tpx * 1.35f);
	};
	// A pill button: white with dark text when focused, faint otherwise (the sheet's this game / all games pills).
	auto button = [&](float x0, float y0, const char* label, bool focused, bool enabled, uint32_t focused_text) {
		const float bpx = 36 * k, bh = 96 * k;
		const float bw = std::max(440 * k, m_fonts->Measure(label, bpx) + 140 * k);
		if (focused && enabled)
			Fonts::AddRoundedRect(ui, x0, y0, bw, bh, bh * 0.5f, Rgba(1, 1, 1, 0.92f));
		else
			Fonts::AddRoundedRect(ui, x0, y0, bw, bh, bh * 0.5f, Rgba(1, 1, 1, enabled ? 0.10f : 0.04f));
		m_fonts->AddText(ui, label, x0 + bw * 0.5f, y0 + bh * 0.5f + 13 * k, bpx,
			focused && enabled ? focused_text : (enabled ? mid : lo), 0.5f, Fonts::Center);
	};

	float y = sep + 40 * k;
	if (!signed_in)
	{
		const char* const labels[2] = {Tr(Str::AccountUsername), Tr(Str::AccountPassword)};
		const char* const hints[2] = {Tr(Str::AccountUsernameHint), Tr(Str::AccountPasswordHint)};
		const bool caret_on = std::fmod(m_time, 1.0) < 0.6;
		for (int f = 0; f < 2; f++)
		{
			const bool focused = m_account.row == f && !busy;
			const bool typing = (m_account.editing && m_account.row == f) || m_ime_field == f;
			// The label, as the sheet's group headers.
			std::string upper = labels[f];
			for (char& c : upper)
				c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
			m_fonts->AddText(ui, upper.c_str(), cx, y + 36 * k, 26 * k, lo, 0.45f);
			const float by = y + 52 * k, bh = 116 * k, bx = px + 48 * k, bw = pw - 96 * k;
			// The box: an edge, the body (lit when focused), the cover's colour at the edge of the focused one.
			Fonts::AddRoundedRect(ui, bx, by, bw, bh, 28 * k, Rgba(1, 1, 1, focused ? 0.24f : 0.10f));
			Fonts::AddRoundedRect(ui, bx + 2 * k, by + 2 * k, bw - 4 * k, bh - 4 * k, 26 * k,
				focused ? Rgba(0.105f, 0.108f, 0.170f) : Rgba(0.062f, 0.066f, 0.125f));
			if (focused)
				Fonts::AddRoundedRect(ui, bx + 14 * k, by + 26 * k, 6 * k, bh - 52 * k, 3 * k, accent);
			const float base = by + bh * 0.5f + 14 * k, vpx = 40 * k;
			const std::string& value = m_account.Field(f);
			// What's typed (dots for a hidden password), or the field's hint in the low colour.
			float hint_w = 0;
			if (focused && !typing)
			{
				const float right = bx + bw - 44 * k;
				if (f == Panel::RowPassword && !value.empty())
					hint_w = field_hint(right, base, icon::Square,
								 Tr(m_account.show_password ? Str::HintHidePassword : Str::HintShowPassword)) + 40 * k;
				hint_w += field_hint(right - hint_w, base, icon::Cross, Tr(Str::HintEdit)) + 40 * k;
			}
			const float room = bw - 96 * k - hint_w;
			std::string shown;
			if (f == Panel::RowPassword && !m_account.show_password)
				for (size_t i = 0; i < value.size() && i < 64; i++)
					shown += "\xE2\x80\xA2"; // a bullet per character
			else
				shown = value;
			float tx = bx + 48 * k;
			if (shown.empty() && !typing)
				m_fonts->AddText(ui, Fit(*m_fonts, hints[f], vpx, room).c_str(), tx, base, vpx, lo, 0.2f);
			else
			{
				// Typing: the text's end stays in view, so a long one shows its last characters.
				while (!shown.empty() && m_fonts->Measure(shown.c_str(), vpx) > room - 30 * k)
				{
					size_t cut = 1;
					while (cut < shown.size() && (static_cast<unsigned char>(shown[cut]) & 0xC0) == 0x80)
						cut++;
					shown.erase(0, cut);
				}
				tx += m_fonts->AddText(ui, shown.c_str(), tx, base, vpx, hi, 0.2f);
				if (typing && caret_on)
					Fonts::AddRoundedRect(ui, tx + 6 * k, by + 30 * k, 4 * k, bh - 60 * k, 2 * k, accent);
			}
			y = by + bh + 46 * k;
		}
		// What the last sign-in said (a failure in a warm red), then Sign in.
		if (!acc.message.empty() && !busy)
			m_fonts->AddText(ui, Fit(*m_fonts, acc.message, 30 * k, inner).c_str(), cx, y + 6 * k, 30 * k,
				acc.authenticated ? own : Rgba(1.0f, 0.62f, 0.60f), 0.15f);
		const char* const sign_in = busy ? Tr(Str::AccountSigningIn) : Tr(Str::AccountSignIn);
		button(cx, y + 50 * k, sign_in, m_account.row == Panel::RowSignIn || busy, m_account.CanSignIn() || busy, ink);
	}
	else
	{
		float ny = y + 50 * k;
		for (const std::string& line : note_lines)
		{
			m_fonts->AddText(ui, line.c_str(), cx, ny, note_px, mid, 0.1f);
			ny += note_lh;
		}
		const bool armed = m_account.SignOutArmed(m_time);
		button(cx, ny + 8 * k, armed ? Tr(Str::AccountSignOutAgain) : Tr(Str::AccountSignOut), !busy, !busy,
			armed ? Rgba(0.55f, 0.16f, 0.20f) : ink);
	}
	FadeRange(ui, begin, ui.size(), e);

	if (m_account_kb_anim > 0.01f)
	{
		const size_t kb_begin = ui.size();
		const float ky = py + ph + 40 * k + (1.0f - kbe) * 120 * k;
		BuildAccountKeyboard(ui, px, ky, pw, k);
		FadeRange(ui, kb_begin, ui.size(), e * kbe);
	}
}

// The panel's own keyboard (where the app can't open the PS5's): a page of four rows of keys and the function row, in
// the panel's look; the focused key is a white pill with dark text, as the focused button.
void App::BuildAccountKeyboard(std::vector<UiVertex>& ui, float x, float y, float w, float k)
{
	using Panel = AchievementAccountPanel;
	const bool password = m_account.row == Panel::RowPassword;
	BuildPanelKeyboard(ui, x, y, w, k, Tr(password ? Str::AccountPassword : Str::AccountUsername), m_account.page, m_account.key_row,
		m_account.key_col);
}

// vk-285-139: the share's address as typed, over the panel's keyboard, at the bottom of the screen.
void App::BuildPickerKeyboard(std::vector<UiVertex>& ui, float W, float H, float k, uint32_t accent)
{
	using Panel = AchievementAccountPanel;
	const FolderPicker& p = m_picker;
	const float key_w = 152 * k, gap = 16 * k, pad = 48 * k, key_h = 108 * k;
	const float w = Panel::Columns * key_w + (Panel::Columns - 1) * gap + 2 * pad;
	const float h = 5 * key_h + 4 * gap + 2 * pad + 64 * k;
	const float x = (W - w) * 0.5f, ky = H - h - 150 * k;
	// The field: what's typed so far (its end, when it's long) and a caret.
	const float fy = ky - 170 * k, fh = 130 * k;
	Fonts::AddRoundedRect(ui, x - 2 * k, fy - 2 * k, w + 4 * k, fh + 4 * k, 34 * k, Rgba(1, 1, 1, 0.16f));
	Fonts::AddRoundedRect(ui, x, fy, w, fh, 32 * k, Rgba(0.045f, 0.050f, 0.105f, 0.985f));
	Fonts::AddRoundedRect(ui, x + 28 * k, fy + 30 * k, 6 * k, fh - 60 * k, 3 * k, accent);
	std::string shown = p.osk_text;
	const float px = 44 * k, room = w - 120 * k;
	while (shown.size() > 1 && m_fonts->Measure(shown.c_str(), px) > room)
		shown.erase(0, 1);
	const bool caret = std::fmod(m_time, 1.0) < 0.6;
	m_fonts->AddText(ui, (shown + (caret ? "|" : " ")).c_str(), x + 60 * k, fy + fh * 0.5f + px * 0.36f, px, Rgba(1, 1, 1), 0.3f);
	BuildPanelKeyboard(ui, x, ky, w, k, "NFS share", p.osk_page,
		p.osk_row, p.osk_col);
}

void App::BuildPanelKeyboard(std::vector<UiVertex>& ui, float x, float y, float w, float k, const std::string& field_title, int page,
	int key_row, int key_col)
{
	using Panel = AchievementAccountPanel;
	const uint32_t hi = Rgba(1, 1, 1), lo = Rgba(0.52f, 0.50f, 0.64f);
	const uint32_t ink = Rgba(0.07f, 0.06f, 0.16f);
	const float key_w = 152 * k, key_h = 108 * k, gap = 16 * k, pad = 48 * k;
	const float grid_w = Panel::Columns * key_w + (Panel::Columns - 1) * gap;
	const float h = 5 * key_h + 4 * gap + 2 * pad + 64 * k;
	Fonts::AddRoundedRect(ui, x - 6 * k, y + 10 * k, w + 12 * k, h + 12 * k, 48 * k, Rgba(0, 0, 0, 0.35f));
	Fonts::AddRoundedRect(ui, x - 2 * k, y - 2 * k, w + 4 * k, h + 4 * k, 44 * k, Rgba(1, 1, 1, 0.16f));
	Fonts::AddRoundedRect(ui, x, y, w, h, 42 * k, Rgba(0.045f, 0.050f, 0.105f, 0.985f));

	// The field being typed into, and the page.
	std::string title = field_title;
	for (char& c : title)
		c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
	const float gx = x + (w - grid_w) * 0.5f;
	m_fonts->AddText(ui, title.c_str(), gx, y + pad + 26 * k, 26 * k, lo, 0.45f);
	static const char* const kPageNames[Panel::Pages] = {"abc", "ABC", "#+="};
	m_fonts->AddText(ui, kPageNames[page], gx + grid_w, y + pad + 26 * k, 26 * k, lo, 0.45f, Fonts::Right);

	const float top = y + pad + 64 * k;
	auto key = [&](float kx, float ky, float kw, const char* label, bool focused, bool function) {
		Fonts::AddRoundedRect(ui, kx, ky, kw, key_h, 22 * k,
			focused ? Rgba(1, 1, 1, 0.92f) : Rgba(1, 1, 1, function ? 0.12f : 0.07f));
		const float lpx = function ? 34 * k : 46 * k;
		m_fonts->AddText(ui, label, kx + kw * 0.5f, ky + key_h * 0.5f + lpx * 0.36f, lpx, focused ? ink : hi, function ? 0.4f : 0.3f,
			Fonts::Center);
	};
	for (int r = 0; r < Panel::CharRows; r++)
	{
		const int n = Panel::RowKeys(page, r);
		const float row_x = gx + (Panel::Columns - n) * 0.5f * (key_w + gap);
		for (int c = 0; c < n; c++)
		{
			const char text[2] = {Panel::PageRows[page][r][c], '\0'};
			key(row_x + c * (key_w + gap), top + r * (key_h + gap), key_w, text, key_row == r && key_col == c,
				false);
		}
	}
	const char* const fn_labels[Panel::FnCount] = {page == 0 ? "ABC" : "abc", page == 2 ? "abc" : "#+=",
		Tr(Str::KeySpace), Tr(Str::HintDelete), Tr(Str::HintDone)};
	float fx = gx;
	const float fy = top + Panel::CharRows * (key_h + gap);
	for (int c = 0; c < Panel::FnCount; c++)
	{
		// A function key spans its width in keys, with the gaps between them.
		const float fw = Panel::FnWidth[c] * (key_w + gap) - gap;
		key(fx, fy, fw, fn_labels[c], key_row == Panel::FnRow && key_col == c, true);
		fx += fw + gap;
	}
}
} // namespace fe
