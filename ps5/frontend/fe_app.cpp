// PS5 port frontend: the cover-flow shelf (see fe_app.h).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_app.h"
#include "fe_i18n.h"
#include "fe_roots.h" // 2026-10-16: kScanSubfoldersKey (the picker's game count)
#include "fe_settings.h" // bioscheck: EditSettingsFile (the BIOS question writes PS5SX2/OpenBios)

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
// fe_coverfit.h makes a cover the shape of the case face it goes on: the faces of the box mesh (fe_renderer.cpp CreateBoxMesh) must be the shape it assumes.
static_assert(kFaceAspectFront - kBoxHalfW / kBoxHalfH < 0.001f && kBoxHalfW / kBoxHalfH - kFaceAspectFront < 0.001f, "front/back face shape");
static_assert(kFaceAspectSpine - kBoxHalfD / kBoxHalfH < 0.001f && kBoxHalfD / kBoxHalfH - kFaceAspectSpine < 0.001f, "spine face shape");
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

// 2.11 (swordpdf: "back covers are not loading instantly"): when the picked case's back cover is asked for. The files on disk after 0.20 s of being picked,
// which is above the 0.11 s a held D-pad steps at (Step runs before PollCovers in a frame), so scrolling never starts a lookup; the network after 0.35 s, or
// at once when the player starts to turn the case over; the backs of the cases beside it are fetched ahead after 0.45 s, above the 0.36 s a held button
// waits before it repeats.
constexpr double kBackLocalDwell = 0.20, kBackNetDwell = 0.35, kBackRingDwell = 0.45;
constexpr int kBackRing = 3; // cases on each side of the picked one whose backs are fetched ahead
// 2.11 (swordpdf, build 175: "viewing the back cover and moving to the next game: the cover gets deloaded before it turns to the front, a black spot for a
// second"): a case the selection has left keeps its back cover for as long as the flip still turns it over on screen (more than kBackShownDeg of its pose,
// well past the 90 degrees at which the back is in view), and kBackGrace longer: the next case's back is only the picked one's, but what is drawn is not.
constexpr float kBackShownDeg = 10.0f;
constexpr double kBackGrace = 0.20;

// 2.11 (swordpdf: "the rotating of the disc cover with the stick is very sensitive and small stick movements make the rotation go fast. make it harder"):
// the right stick turns the picked case over. Nothing happens below kFlipDead; the whole way is reached only near a full push (kFlipFull); between, the
// push (0..1 after those two) is bent by a power curve, so a small push turns it a little. A soft spring follows it, with hardly any overshoot, and the
// turning speed is capped (flips a second: one flip is the whole way over), so a flick of the stick cannot spin the case.
constexpr float kFlipDead = 0.35f, kFlipFull = 0.95f, kFlipCurve = 2.2f;
constexpr float kFlipSpring = 60.0f, kFlipDamping = 0.85f, kFlipMaxSpeed = 2.6f;

// 2.11 (swordpdf: "proportionate to the case"): the disc drawn on a case front, to the case scale: the front is 135 mm wide and a PS2 disc is 120 mm
// across, so its circle is most of the front's width (the loading arc goes round it, the percentage and the captions under it).
constexpr float kDiscRadius = 0.54f, kDiscCy = 0.17f;
constexpr float kDiscTextY = kDiscCy - kDiscRadius - 0.17f; // the text's centre: clear of the disc's lower edge, above the serial at the foot

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
// 2.11: the disc in the drive, not copied: a shelf entry too, in front of everything (an index below kDumpIndex: both are never up at once).
constexpr int kDriveIndex = -2;

// live-22 (AI-assisted, graphics approach from the dropped frontend of PR #34): a spinning disc drawn on the front of a box
// (model `model`), as UI triangles whose corners are the box's front points projected, so it lies on the box at any angle.
// `alpha` fades it on the boxes beside the selected one. Here it marks the game currently being dumped; the percentage is
// drawn over it by Build.
void AddDiscSpinner(std::vector<UiVertex>& ui, const Mat4& view_proj, const Mat4& model, float t, float alpha, float W, float H, bool with_disc = true,
	bool with_arc = true)
{
	if (alpha <= 0.01f)
		return;
	constexpr int kSegments = 72;
	constexpr float kPi = 3.14159265f;
	const float cx = 0.0f, cy = kDiscCy, z = kBoxHalfD + 0.004f; // a little above centre; the percentage sits below it
	const float s = kDiscRadius / 0.40f; // the radii below were drawn for a disc of 0.40
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
	// The disc: silver with two glints that turn with it and a faint rainbow, the clear hub, and the hole. (2.11: not when the disc's own art is drawn.)
	if (with_disc)
	ring(0.13f * s, 0.40f * s, 0.0f, 2 * kPi, [&](float a, float outer) {
		const float g1 = std::pow(std::max(0.0f, std::cos(a - spin)), 10.0f), g2 = std::pow(std::max(0.0f, std::cos(a - spin - kPi)), 10.0f);
		const float glint = 0.55f * (g1 + g2) * (0.6f + 0.4f * outer);
		const float hue = a * 2.0f - spin;
		const float r = 0.62f + 0.10f * std::sin(hue) + glint, g = 0.64f + 0.10f * std::sin(hue + 2.1f) + glint,
					bl = 0.72f + 0.10f * std::sin(hue + 4.2f) + glint;
		return a8(alpha * 0.92f) | Rgba(Clamp(r, 0.0f, 1.0f), Clamp(g, 0.0f, 1.0f), Clamp(bl, 0.0f, 1.0f), 0);
	});
	if (with_disc)
	ring(0.055f * s, 0.13f * s, 0.0f, 2 * kPi, [&](float, float) { return a8(alpha * 0.55f) | Rgba(0.85f, 0.88f, 0.95f, 0); });
	// The loading arc around it, a quarter turn long, going round faster than the disc.
	const float head = t * 3.2f;
	if (with_arc)
	ring(0.44f * s, 0.465f * s, head, head + kPi * 0.6f, [&](float a, float) {
		const float tail = (a - head) / (kPi * 0.6f);
		return a8(alpha * (0.15f + 0.85f * tail)) | Rgba(1.0f, 1.0f, 1.0f, 0);
	});
}

// 2.11 (AI-assisted; swordpdf: "we can also use the circular disc cover for when a game is dumping"): the disc's own art, a picture with a round label and a
// transparent surround, turned by `angle` about the disc's centre on the box front. A grid of small quads whose corners are the box's front points
// projected (the UI shader has no perspective correction, so a few cells keep the picture from swimming when the box is at an angle). `images` gets
// the texture change for them (the renderer binds `set` for those vertices).
void AddDiscArt(std::vector<UiVertex>& ui, std::vector<UiImageRange>& images, VkDescriptorSet set, float aspect, const Mat4& view_proj, const Mat4& model,
	float angle, float alpha, float W, float H)
{
	if (alpha <= 0.01f || !set)
		return;
	constexpr int kGrid = 8;
	const float cx = 0.0f, cy = kDiscCy, z = kBoxHalfD + 0.0035f, radius = kDiscRadius; // as the spinner's disc
	const float hw = aspect >= 1.0f ? radius : radius * aspect, hh = aspect >= 1.0f ? radius / aspect : radius;
	const float ca = std::cos(angle), sa = std::sin(angle);
	const uint32_t colour = Rgba(1.0f, 1.0f, 1.0f, Clamp(alpha, 0.0f, 1.0f));
	auto vert = [&](float s, float tt) {
		const float lx = (s - 0.5f) * 2.0f * hw, ly = (0.5f - tt) * 2.0f * hh; // the picture's top is up on the box
		const Vec4 w = model * Vec4(cx + lx * ca - ly * sa, cy + lx * sa + ly * ca, z, 1.0f);
		const Vec4 c = view_proj * w;
		return UiVertex{((c.x / c.w) * 0.5f + 0.5f) * W, ((c.y / c.w) * 0.5f + 0.5f) * H, s, tt, colour, 0, 0, 0, 2};
	};
	const uint32_t first = static_cast<uint32_t>(ui.size());
	for (int gy = 0; gy < kGrid; gy++)
		for (int gx = 0; gx < kGrid; gx++)
		{
			const float s0 = static_cast<float>(gx) / kGrid, s1 = static_cast<float>(gx + 1) / kGrid;
			const float t0 = static_cast<float>(gy) / kGrid, t1 = static_cast<float>(gy + 1) / kGrid;
			const UiVertex a = vert(s0, t0), b = vert(s1, t0), c = vert(s1, t1), d = vert(s0, t1);
			ui.insert(ui.end(), {a, b, c, a, c, d});
		}
	images.push_back({first, static_cast<uint32_t>(ui.size()) - first, set});
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
	m_bios_open_in_use = !m_bios_missing && m_cfg.bios_open_in_use && m_cfg.bios_open_in_use();
	// bioscheck: a tile's or a disc's start was refused for want of a BIOS before the shelf came up: the question comes up for it at once.
	if (!m_cfg.bios_pending_launch.empty())
	{
		m_bios_missing = true;
		OpenBiosChoice(BiosWait::Outside, m_cfg.bios_pending_launch);
	}
	return true;
}

void App::SetGames(std::vector<GameInfo> games, int preselect)
{
	const size_t all = games.size();
	const size_t hidden = static_cast<size_t>(std::count_if(games.begin(), games.end(), [](const GameInfo& g) { return g.hidden; }));
	// The entries already on the shelf (a disc drive's, a copy's: indices below 0) stay; the games join them as Init's list would be.
	const int keep = m_moved && !m_games.empty() ? m_index[static_cast<size_t>(m_selected)] : preselect;
	for (size_t i = 0; i < games.size(); i++)
		m_shelved.push_back({std::move(games[i]), Slot(), static_cast<int>(i)});
	ApplyHidden(keep);
	m_scroll = static_cast<float>(m_selected);
	m_scroll_vel = 0;
	m_select_time = m_time;
	if (m_covers && !m_games.empty() && m_index[static_cast<size_t>(m_selected)] >= 0)
		m_covers->SetSelected(m_index[static_cast<size_t>(m_selected)]);
	std::printf("[frontend] the scan is done: %zu games on the shelf%s\n", m_games.size(),
		hidden > 0 ? (" (" + std::to_string(hidden) + " of " + std::to_string(all) + " hidden)").c_str() : "");
	std::fflush(stdout);
}

// vk-285-134 (AI-assisted): a game (or the PS2 system menu) starts only with a BIOS. Looked for again at each pick, as a BIOS
// may have been copied over meanwhile (the settings page, FTP); without one the pick stays on the shelf and the line says why.
bool App::BiosReady(BiosWait wait, const std::string& path)
{
	if (!m_cfg.bios_present)
		return true;
	if (m_cfg.bios_present())
	{
		m_bios_missing = false;
		m_bios_problem.clear();
		m_bios_open_in_use = m_cfg.bios_open_in_use && m_cfg.bios_open_in_use();
		return true;
	}
	m_bios_missing = true;
	m_bios_open_in_use = false;
	m_bios_problem = m_cfg.bios_problem ? m_cfg.bios_problem() : std::string();
	m_bios_refused = m_time;
	std::printf("[frontend] no PS2 BIOS: the game stays on the shelf (%s)\n", m_bios_problem.c_str());
	std::fflush(stdout);
	Sound(Sfx::Edge, 0.0f);
	if (wait != BiosWait::None) // bioscheck: and asks what to do
		OpenBiosChoice(wait, path);
	return false;
}

// bioscheck (2026-10-29, AI-assisted): the BIOS question. A pick (or the system menu, or a start from outside the shelf) refused for want of a
// BIOS asks: the open-source BIOS PS5SX2 ships (OpenPS2BIOS, experimental), which is remembered (gs.ini PS5SX2/OpenBios) and starts what
// waited at once, or the player's own BIOS in the BIOS folder (a BIOS of their own always wins over the open one).
void App::OpenBiosChoice(BiosWait wait, const std::string& path)
{
	m_bios_ask = true;
	m_bios_sel = 0;
	m_bios_wait = wait;
	m_bios_wait_path = path;
	m_held = 0;
	std::printf("[frontend] BIOS question: %s%s%s (the open BIOS %s)\n",
		wait == BiosWait::Pick ? "a pick" : wait == BiosWait::SystemMenu ? "the PS2 system menu" : "a start from outside the shelf",
		path.empty() ? "" : ": ", path.c_str(), m_cfg.open_bios_bundled ? "offered" : "not in this build");
	std::fflush(stdout);
}

bool App::OfferOutsideLaunch(const std::string& path)
{
	if (!m_bios_missing && (!m_cfg.bios_present || m_cfg.bios_present()))
		return true;
	if (m_bios_missing && m_cfg.bios_present && m_cfg.bios_present()) // a BIOS came meanwhile
	{
		m_bios_missing = false;
		m_bios_problem.clear();
		m_bios_open_in_use = m_cfg.bios_open_in_use && m_cfg.bios_open_in_use();
		return true;
	}
	m_bios_missing = true;
	if (path != m_bios_outside_asked && !m_bios_ask && !m_launching)
	{
		m_bios_outside_asked = path;
		m_bios_refused = m_time;
		Sound(Sfx::Edge, 0.0f);
		OpenBiosChoice(BiosWait::Outside, path);
	}
	return false;
}

std::string App::TakeApprovedLaunch()
{
	std::string r;
	r.swap(m_approved_launch);
	return r;
}

bool App::UseOpenBios()
{
	std::string what, error;
	if (m_cfg.options.gs_ini.empty() ||
		!settings::EditSettingsFile(m_cfg.options.gs_ini, "", {{settings::Change::Set, "PS5SX2/OpenBios", "true"}}, what, error))
	{
		std::printf("[frontend] BIOS question: the open BIOS couldn't be chosen (%s)\n", error.c_str());
		std::fflush(stdout);
		return false;
	}
	if (m_cfg.options.log)
		m_cfg.options.log("shelf: gs.ini: the BIOS question: the open-source BIOS: " + (what.empty() ? std::string("no change") : what));
	if (!m_cfg.bios_present || !m_cfg.bios_present())
		return false;
	m_bios_missing = false;
	m_bios_problem.clear();
	m_bios_open_in_use = m_cfg.bios_open_in_use && m_cfg.bios_open_in_use();
	return true;
}

void App::RefreshBios()
{
	if (!m_cfg.bios_present)
		return;
	const bool ok = m_cfg.bios_present();
	m_bios_missing = !ok;
	m_bios_problem = ok ? std::string() : (m_cfg.bios_problem ? m_cfg.bios_problem() : std::string());
	m_bios_open_in_use = ok && m_cfg.bios_open_in_use && m_cfg.bios_open_in_use();
}

void App::UpdateBiosChoice(const Input& in)
{
	const int cards = m_cfg.open_bios_bundled ? 2 : 1;
	if (cards == 2 && ((in.left && !m_prev.left) || (in.right && !m_prev.right)))
	{
		m_bios_sel = 1 - m_bios_sel;
		Sound(Sfx::Move, in.left ? -0.2f : 0.2f);
		return;
	}
	if (in.circle && !m_prev.circle)
	{
		std::printf("[frontend] BIOS question: closed\n");
		std::fflush(stdout);
		m_bios_ask = false;
		m_bios_wait = BiosWait::None;
		Sound(Sfx::Edge, 0.0f);
		return;
	}
	if (!(in.cross && !m_prev.cross))
		return;
	const bool open = cards == 2 && m_bios_sel == 0;
	if (!open)
	{
		std::printf("[frontend] BIOS question: your own BIOS (in %s)\n", m_cfg.bios_dir.c_str());
		std::fflush(stdout);
		char line[320];
		std::snprintf(line, sizeof(line), Tr(Str::BiosAddOwnLine1), m_cfg.bios_dir.c_str());
		m_refused_text = std::string(line) + ", " + Tr(Str::BiosAddOwnLine2);
		m_refused_time = m_time;
		m_bios_refused = m_time;
		m_bios_ask = false;
		m_bios_wait = BiosWait::None;
		Sound(Sfx::Move, 0.0f);
		return;
	}
	if (!UseOpenBios())
	{
		m_refused_text = Tr(Str::BiosOpenFailed);
		m_refused_time = m_time;
		m_bios_ask = false;
		m_bios_wait = BiosWait::None;
		Sound(Sfx::Edge, 0.0f);
		return;
	}
	std::printf("[frontend] BIOS question: the open-source BIOS (OpenPS2BIOS, experimental): remembered; %s starts\n",
		m_bios_wait == BiosWait::Outside ? m_bios_wait_path.c_str() : m_bios_wait == BiosWait::SystemMenu ? "the PS2 system menu" : "the pick");
	std::fflush(stdout);
	m_bios_ask = false;
	const BiosWait wait = m_bios_wait;
	m_bios_wait = BiosWait::None;
	if (wait == BiosWait::Outside)
	{
		m_approved_launch = m_bios_wait_path;
		m_done = true;
		Sound(Sfx::Launch, 0.0f);
		return;
	}
	if (wait == BiosWait::None)
	{
		Sound(Sfx::Move, 0.0f);
		return;
	}
	if (m_cfg.game_achievements.cancel)
		m_cfg.game_achievements.cancel();
	m_system_menu = wait == BiosWait::SystemMenu;
	m_launching = true;
	m_launch_time = m_time;
	Sound(Sfx::Launch, 0.0f);
}

void App::BuildBiosChoice(std::vector<UiVertex>& ui, float W, float H, float k, uint32_t accent)
{
	const float e = Smoothstep(0.0f, 1.0f, m_bios_anim);
	const size_t begin = ui.size();
	const uint32_t hi = Rgba(1, 1, 1), mid = Rgba(0.76f, 0.74f, 0.86f), lo = Rgba(0.56f, 0.54f, 0.68f);
	const uint32_t ink = Rgba(0.07f, 0.06f, 0.16f), inkmid = Rgba(0.30f, 0.28f, 0.44f);
	Fonts::AddRoundedRect(ui, 0, 0, W, H, 0.0f, Rgba(0.01f, 0.01f, 0.03f, 0.76f));

	// The panel, as the disc question's (BuildDiscAsk): a shadow, a rim, the body, an amber line along its top (the BIOS line's colour).
	const float pw = 1560 * k, ph = 700 * k, px = (W - pw) * 0.5f, py = (H - ph) * 0.5f - 20 * k;
	const float pad = 84 * k, cx = px + pad, inner = pw - 2 * pad;
	Fonts::AddRoundedRect(ui, px - 8 * k, py + 14 * k, pw + 16 * k, ph + 16 * k, 52 * k, Rgba(0, 0, 0, 0.38f));
	Fonts::AddRoundedRect(ui, px - 2 * k, py - 2 * k, pw + 4 * k, ph + 4 * k, 46 * k, Rgba(1, 1, 1, 0.14f));
	Fonts::AddRoundedRect(ui, px, py, pw, ph, 44 * k, Rgba(0.045f, 0.050f, 0.105f, 0.985f));
	Fonts::AddRoundedRect(ui, px + 120 * k, py, pw - 240 * k, 6 * k, 3 * k, Rgba(0.95f, 0.62f, 0.22f));

	m_fonts->AddText(ui, Fit(*m_fonts, Tr(Str::BiosChoiceTitle), 60 * k, inner).c_str(), cx, py + 120 * k, 60 * k, hi, 0.55f);
	m_fonts->AddText(ui, Fit(*m_fonts, Tr(Str::BiosChoiceIntro), 30 * k, inner).c_str(), cx, py + 184 * k, 30 * k, mid, 0.2f);
	Fonts::AddRoundedRect(ui, cx, py + 222 * k, inner, 2 * k, 0, Rgba(1, 1, 1, 0.10f));

	const bool two = m_cfg.open_bios_bundled;
	const float gap = 36 * k, cw = two ? (inner - gap) * 0.5f : inner, ch = 240 * k, cy = py + 270 * k;
	auto card = [&](float x0, const std::string& title, const std::string& line1, const std::string& line2, bool focused) {
		if (focused)
		{
			Fonts::AddRoundedRect(ui, x0 - 8 * k, cy - 8 * k, cw + 16 * k, ch + 16 * k, 46 * k, Rgba(1, 1, 1, 0.16f));
			Fonts::AddRoundedRect(ui, x0 - 4 * k, cy - 4 * k, cw + 8 * k, ch + 8 * k, 42 * k, accent);
			Fonts::AddRoundedRect(ui, x0, cy, cw, ch, 38 * k, Rgba(1, 1, 1, 0.96f));
		}
		else
			Fonts::AddRoundedRect(ui, x0, cy, cw, ch, 38 * k, Rgba(1, 1, 1, 0.07f));
		const uint32_t t = focused ? ink : hi, c = focused ? inkmid : mid;
		const float tw = cw - 48 * k;
		m_fonts->AddText(ui, Fit(*m_fonts, title, 44 * k, tw).c_str(), x0 + cw * 0.5f, cy + 88 * k, 44 * k, t, 0.55f, Fonts::Center);
		m_fonts->AddText(ui, Fit(*m_fonts, line1, 29 * k, tw).c_str(), x0 + cw * 0.5f, cy + 148 * k, 29 * k, c, 0.15f, Fonts::Center);
		m_fonts->AddText(ui, Fit(*m_fonts, line2, 29 * k, tw).c_str(), x0 + cw * 0.5f, cy + 190 * k, 29 * k, c, 0.15f, Fonts::Center);
	};
	char own[320];
	std::snprintf(own, sizeof(own), Tr(Str::BiosAddOwnLine1), m_cfg.bios_dir.c_str());
	if (two)
	{
		card(cx, Tr(Str::BiosUseOpen), Tr(Str::BiosUseOpenLine1), Tr(Str::BiosUseOpenLine2), m_bios_sel == 0);
		card(cx + cw + gap, Tr(Str::BiosAddOwn), own, Tr(Str::BiosAddOwnLine2), m_bios_sel == 1);
	}
	else
		card(cx, Tr(Str::BiosAddOwn), own, Tr(Str::BiosAddOwnLine2), true);

	float hx = cx;
	auto hint = [&](const char* key, const char* what) {
		const float kpx = 24 * k, kw = m_fonts->Measure(key, kpx) + 36 * k, kh = 46 * k, hy = py + ph - 100 * k;
		Fonts::AddRoundedRect(ui, hx, hy, kw, kh, kh * 0.5f, Rgba(1, 1, 1, 0.14f));
		m_fonts->AddText(ui, key, hx + kw * 0.5f, hy + kh * 0.5f + 9 * k, kpx, hi, 0.45f, Fonts::Center);
		hx += kw + 14 * k;
		hx += m_fonts->AddText(ui, what, hx, hy + kh * 0.5f + 9 * k, kpx, lo, 0.20f) + 40 * k;
	};
	if (two)
		hint("Left / Right", Tr(Str::BiosHintChoose));
	hint("Cross", Tr(Str::BiosHintConfirm));
	hint("Circle", Tr(Str::HintBack));
	FadeRange(ui, begin, ui.size(), e);
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
	// 2.12: a request for a tile that the helper has not taken within moments is not left for a helper that is loaded later (the page's too: one registry).
	if (!m_cfg.tiles_dir.empty())
		gametile::WithdrawOwn(m_cfg.tiles_dir, gametile::kShutdownWithdrawS, gametile::WallSeconds());
	if (m_cfg.game_achievements.cancel)
		m_cfg.game_achievements.cancel();
	ClearAchievementBadges();
	m_account.Close();
	FreeDiscArt(); // 2.11 (review)
	for (Shelved& h : m_shelved) // vk-285-137
	m_slots.push_back(h.slot);
	m_shelved.clear();
	for (Slot& s : m_slots)
	{
		m_renderer->FreeTextureSet(s.set);
		m_renderer->DestroyTexture(s.cover);
		m_renderer->DestroyTexture(s.placeholder);
		m_renderer->DestroyTexture(s.spine);
		m_renderer->DestroyTexture(s.back_ph); // 2.11
		m_renderer->DestroyTexture(s.back_real);
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
	m_last_dir = dir > 0 ? 1 : -1; // 2.11: the backs ahead are fetched on this side first
	m_selected = next;
	m_moved = true; // fix180
	m_flip_latched = false; // 2.11: a case turned over goes back when the shelf moves
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
		if ((e.game.hidden && !m_show_hidden) || (!m_search.applied.empty() && e.index >= 0 && !SearchMatches(e.game))) // 2.11: and the search's
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
	m_renderer->DestroyTexture(s.back_ph); // 2.11
	m_renderer->DestroyTexture(s.back_real);
	s = Slot();
	m_covers->ForgetBack(index); // 2.11
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

// R3 (build 175). What the flip adds to the pose of the case d places from the scroll position (Pose): all of the flip for the picked case, less for the
// ones beside it, none from a place away. The same number draws the case and decides whether its back may go, so the two cannot differ.
float App::FlipTurn(float d) const
{
	const float e = Smoothstep(0.0f, 1.0f, std::min(std::fabs(d), 1.0f));
	return (1.0f - e) * m_flip * m_flip_dir * Radians(180.0f);
}

// The case at this position is drawn turned over by the flip (so its back may be in view): not the picked one only, everything on the shelf.
bool App::BackOnShow(int pos) const
{
	return std::fabs(FlipTurn(static_cast<float>(pos) - m_scroll)) > Radians(kBackShownDeg);
}

void App::ReleaseBack(Slot& s, int index)
{
	m_covers->ForgetBack(index);
	m_renderer->DestroyTexture(s.back_real);
	s.back_real = nullptr;
	s.back_wanted = false;
	s.back_net = false;
	s.back_asked = -1;
	s.back_shown = s.back_left = -1;
	s.dirty = true; // its set takes the painted back
}

// The backs the selection has left but the flip still shows: let go once their case is no longer turned over on screen (and kBackGrace more), or at once
// when the case is gone from the shelf, or picked again (its back is then the picked case's).
void App::PollBackHolds(int sel_index)
{
	for (size_t h = 0; h < m_back_held.size();)
	{
		const int index = m_back_held[h];
		const int pos = PositionOf(index);
		bool gone = pos < 0 || index == sel_index;
		if (!gone)
		{
			Slot& s = m_slots[static_cast<size_t>(pos)];
			if (!s.back_real) // let go already (DropCovers, or the next selection change)
				gone = true;
			else if (BackOnShow(pos))
				s.back_shown = m_time;
			else if (m_time - s.back_shown > kBackGrace)
			{
				std::printf("[frontend] back cover of %s let go %d ms after the selection left it: its case is turned back to the front\n",
					m_games[static_cast<size_t>(pos)].title.c_str(), static_cast<int>((m_time - s.back_left) * 1000.0)); // 2.11: for the console log
				std::fflush(stdout);
				ReleaseBack(s, index);
				gone = true;
			}
		}
		if (gone)
			m_back_held.erase(m_back_held.begin() + static_cast<long>(h));
		else
			h++;
	}
}

int App::BackRealOf(int game) const
{
	const int pos = PositionOf(game);
	return pos < 0 ? -1 : m_slots[static_cast<size_t>(pos)].back_real ? 1 : 0;
}

int App::SetOf(int game) const
{
	const int pos = PositionOf(game);
	return pos < 0 ? -1 : m_slots[static_cast<size_t>(pos)].set != VK_NULL_HANDLE ? 1 : 0;
}

int App::TurnOf(int game) const
{
	const int pos = PositionOf(game);
	return pos < 0 ? 0 : static_cast<int>(std::lround(std::fabs(FlipTurn(static_cast<float>(pos) - m_scroll)) / Radians(1.0f)));
}

int App::BackRealCount() const
{
	int n = 0;
	for (const Slot& s : m_slots)
		n += s.back_real ? 1 : 0;
	for (const Shelved& h : m_shelved)
		n += h.slot.back_real ? 1 : 0;
	return n;
}

void App::PollCovers()
{
	if (!m_covers)
		return;
	KeepCoversNear();
	// 2.11: the picked game's back cover is looked for once it has been picked for a moment (the flip is then instant); the others' are let go.
	// Asked for in two steps (see kBackLocalDwell): the files after 0.20 s, the network after 0.35 s or at once when the player starts to turn the case, so
	// a game passed over costs nothing and one landed on has its back before the flip is over. The cases beside it get theirs fetched ahead.
	if (!m_games.empty() && m_index[static_cast<size_t>(m_selected)] >= 0)
	{
		const int sel_index = m_index[static_cast<size_t>(m_selected)];
		if (m_back_for != sel_index)
		{
			for (size_t pos = 0; pos < m_slots.size(); pos++)
			{
				Slot& other = m_slots[pos];
				if ((other.back_wanted || other.back_real) && m_index[pos] != sel_index)
				{
					// R3 (build 175): a case still turned over on screen keeps its back until the flip has brought it round (PollBackHolds); a back
					// only asked for (no texture to keep) and one not turned over are let go at once, as before.
					if (other.back_real && BackOnShow(static_cast<int>(pos)))
					{
						other.back_shown = m_time;
						if (std::find(m_back_held.begin(), m_back_held.end(), m_index[pos]) == m_back_held.end())
						{
							other.back_left = m_time;
							m_back_held.push_back(m_index[pos]);
						}
					}
					else
						ReleaseBack(other, m_index[pos]);
				}
			}
			m_back_for = sel_index;
		}
		PollBackHolds(sel_index);
		Slot& sel = m_slots[static_cast<size_t>(m_selected)];
		const double dwell = m_time - m_select_time;
		if (sel.wanted && !sel.back_real)
		{
			const bool net = m_flip_wanted || dwell > kBackNetDwell;
			if (!sel.back_wanted && (net || dwell > kBackLocalDwell))
			{
				sel.back_wanted = true;
				sel.back_net = net;
				sel.back_asked = m_time;
				m_covers->WantBack(sel_index, net);
				std::printf("[frontend] back cover asked for %s (%s) %d ms after it was picked\n", m_games[static_cast<size_t>(m_selected)].title.c_str(),
					net ? "files and download" : "files only", static_cast<int>(dwell * 1000.0)); // 2.11: for the console log (and the host tests)
				std::fflush(stdout);
			}
			else if (sel.back_wanted && net && !sel.back_net)
			{
				sel.back_net = true;
				m_covers->WantBack(sel_index, true);
				std::printf("[frontend] back cover asked for %s (download too) %d ms after it was picked\n",
					m_games[static_cast<size_t>(m_selected)].title.c_str(), static_cast<int>(dwell * 1000.0));
				std::fflush(stdout);
			}
		}
		if (dwell > kBackRingDwell && m_prefetch_for != sel_index)
		{
			// Nearest first, the side the selection is moving towards first: those are the backs the player flips to next.
			m_prefetch_for = sel_index;
			std::vector<int> ring;
			const int n = static_cast<int>(m_games.size());
			for (int d = 1; d <= kBackRing; d++)
				for (const int side : {m_last_dir, -m_last_dir})
				{
					const int pos = m_selected + side * d;
					if (pos >= 0 && pos < n && m_index[static_cast<size_t>(pos)] >= 0) // (not the disc entries)
						ring.push_back(m_index[static_cast<size_t>(pos)]);
				}
			m_covers->PrefetchBacks(ring);
			m_ring_count = static_cast<int>(ring.size());
			m_ring_first = ring.empty() ? -1 : ring.front();
		}
	}
	else
		PollBackHolds(-3); // a disc entry is picked: nothing is the picked game, the flip still brings the held cases round
	CoverImage img;
	int budget = 4; // textures a frame
	while (budget-- > 0 && m_covers->Poll(img))
	{
	if (img.kind == CoverImage::Disc) // 2.11: belongs to the dump entry, not to a game of the list
	{
	TakeDiscArt(img);
	continue;
	}
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
		if (img.kind == CoverImage::RealBack && img.game != m_back_for) // 2.11: arrived after the selection moved on: not kept (the file is in the cache)
			continue;
		Slot& s = *slot;
		Texture* t = m_renderer->CreateTexture(static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height),
			VK_FORMAT_R8G8B8A8_UNORM, img.rgba.data());
		if (!t)
			continue;
		Texture** dst = img.kind == CoverImage::Spine ? &s.spine : img.kind == CoverImage::Placeholder ? &s.placeholder :
		                img.kind == CoverImage::Back ? &s.back_ph : img.kind == CoverImage::RealBack ? &s.back_real : &s.cover; // 2.11: the back
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
			if (img.kind == CoverImage::RealBack)
			{
				std::printf("[frontend] back cover for %s (%s, %dx%d) %d ms after it was asked for\n", game->title.c_str(), img.source, img.width, img.height,
					s.back_asked >= 0 ? static_cast<int>((m_time - s.back_asked) * 1000.0) : -1); // 2.11: for the console log
				std::fflush(stdout);
			}
		s.dirty = true;
	}
	for (Slot& s : m_slots)
		if (s.dirty && s.spine && s.placeholder)
		{
			m_renderer->FreeTextureSet(s.set);
			s.set = m_renderer->AllocTextureSet(s.cover ? s.cover : s.placeholder, s.placeholder, s.spine,
				s.back_real ? s.back_real : s.back_ph ? s.back_ph : s.placeholder); // 2.11: and the back
			s.dirty = !s.set; // 2026-10-10: the pool full for now (sets freed are given back a frame later): try again
		}
}

// 2.11: the disc art arrived (CoverService::RequestDisc): a texture of its own, drawn over the dump entry's box (Build) in place of the painted disc.
void App::TakeDiscArt(const CoverImage& img)
{
	if ((!m_dump_on && !m_drive_on) || img.width < 16 || img.height < 16 || img.serial != m_disc_art_serial)
	return;
	Texture* t = m_renderer->CreateTexture(static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height), VK_FORMAT_R8G8B8A8_UNORM, img.rgba.data());
	if (!t)
		return;
	const VkDescriptorSet set = m_renderer->AllocTextureSet(t, t, t);
	if (!set) // the descriptor pool is full for now: the painted disc stays
	{
		m_renderer->DestroyTexture(t);
		return;
	}
	const std::string keep = m_disc_art_serial; // FreeDiscArt forgets which disc the art is for
	FreeDiscArt();
	m_disc_art_serial = keep;
	m_dump_disc = t;
	m_dump_disc_set = set;
	m_dump_disc_aspect = static_cast<float>(img.width) / static_cast<float>(img.height);
	std::printf("[frontend] disc dump: the disc's own art (%s, %dx%d)\n", img.source, img.width, img.height);
	std::fflush(stdout);
}

void App::FreeDiscArt()
{
	if (m_dump_disc_set)
		m_renderer->FreeTextureSet(m_dump_disc_set);
	m_renderer->DestroyTexture(m_dump_disc);
	m_dump_disc_set = VK_NULL_HANDLE;
	m_dump_disc = nullptr;
	m_disc_art_serial.clear();
	}

	void App::EnsureDiscArt(const std::string& serial)
	{
	if (serial.empty() || serial == m_disc_art_serial)
	return;
	FreeDiscArt();
	m_disc_art_serial = serial;
	if (m_covers)
	m_covers->RequestDisc(serial); // the disc's own art (LaunchBox or covers/<serial>_disc.png)
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
	if (m_drive_on)
	RemoveDriveEntry(); // the disc is being copied: the copy's entry stands for it
	const std::string disp = title.empty() ? std::string("PS2 disc") : title;
	GameInfo g;
	g.path = "@disc-dump"; // a sentinel path: this entry is never launched
	g.serial = serial;
	g.stem = DiscStem(serial); // gamedb (2026-10-30): the copy's name, so its sheet is the copy's and the disc's
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
		s.set = m_renderer->AllocTextureSet(s.placeholder, s.placeholder, s.spine, s.placeholder);
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
	EnsureDiscArt(serial); // 2.11: the disc's own art (LaunchBox or covers/<serial>_disc.png) replaces the painted disc when it arrives
	Sound(Sfx::Move, 0.0f);
	std::printf("[frontend] disc dump: %s (%s) %d%% on the shelf\n", disp.c_str(), serial.c_str(), pct);
	std::fflush(stdout);
}

// live-22 (AI-assisted): drop the dump entry (the copy ended). The game either launches at once (the dumper's launch
// request) or appears on the shelf at the next open, as it did before.
void App::RemoveDumpEntry()
{
	const int pos = DumpPos();
	m_dump_on = false; // (the disc art is let go by PollDriveDisc once no entry wants it)
	m_dump_mbps = 0.0; m_dump_eta = 0; m_dump_speed.clear(); // 179
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
	double mbps = 0.0; // 179
	int eta = 0;       // 179 (seconds)
	char speed[32] = {0}; // 179 ("8x"/"3.2x"/"-")
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
				char l3[160] = {0};
				if (std::fgets(l3, sizeof(l3), f)) // 179: optional "rate=<MB/s> eta=<s> speed=<tag>"
					std::sscanf(l3, "rate=%lf eta=%d speed=%31s", &mbps, &eta, speed);
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
		{
		if (!ShelfBusy()) // 2.11 (review): the entry selects itself; not under a launch, a sheet, the account panel, a picker or the search
		InsertDumpEntry(serial, title, pct);
		}
		else
		m_dump_pct = pct;
		// 179: the live speed/time-left/drive-speed shown on the dump entry (Build)
		m_dump_mbps = mbps;
		m_dump_eta = eta;
		m_dump_speed = (speed[0] && std::strcmp(speed, "-") != 0) ? speed : std::string();
	}
	else if (m_dump_on)
		RemoveDumpEntry();
}

// gamedb (2026-10-30): the settings file stem of a disc's entries (AppConfig::disc_settings_stem), "" when unknown.
std::string App::DiscStem(const std::string& serial) const
{
	return m_cfg.disc_settings_stem && !serial.empty() ? m_cfg.disc_settings_stem(serial) : std::string();
}

int App::DrivePos() const
{
for (size_t i = 0; i < m_index.size(); i++)
if (m_index[i] == kDriveIndex)
return static_cast<int>(i);
return -1;
}

void App::InsertDriveEntry(const std::string& serial, const std::string& title)
{
GameInfo g;
g.path = "@disc-drive"; // a sentinel path: this entry is never launched
g.serial = serial;
g.stem = DiscStem(serial); // gamedb (2026-10-30): its sheet writes the file a start from the drive reads
g.title = title.empty() ? std::string("PS2 disc") : title;
Slot s;
CoverImage ph, sp;
GameInfo front = g;
front.title.clear(); // the box front holds the disc, not the title (as the copy's entry)
CoverService::PaintPlaceholder(*m_fonts, front, ph);
CoverService::PaintSpine(*m_fonts, g, sp);
s.placeholder = m_renderer->CreateTexture(static_cast<uint32_t>(ph.width), static_cast<uint32_t>(ph.height), VK_FORMAT_R8G8B8A8_UNORM, ph.rgba.data());
s.spine = m_renderer->CreateTexture(static_cast<uint32_t>(sp.width), static_cast<uint32_t>(sp.height), VK_FORMAT_R8G8B8A8_UNORM, sp.rgba.data());
if (s.placeholder && s.spine)
{
s.set = m_renderer->AllocTextureSet(s.placeholder, s.placeholder, s.spine, s.placeholder);
s.dirty = !s.set;
}
else
s.dirty = true;
const bool had_games = !m_games.empty();
m_games.insert(m_games.begin(), std::move(g));
m_slots.insert(m_slots.begin(), std::move(s));
m_index.insert(m_index.begin(), kDriveIndex);
// The shelf stays where it was: what was picked stays picked (the new entry is one to the left of it).
if (had_games)
{
m_selected++;
m_scroll += 1.0f;
}
m_drive_on = true;
m_drive_serial = serial;
m_drive_title = m_games[0].title;
EnsureDiscArt(serial);
std::printf("[frontend] disc in the drive: %s (%s) is the first entry on the shelf\n", m_drive_title.c_str(), serial.c_str());
// fix180: the game played last was this disc from the drive, and the shelf has not been moved since it opened: the disc's entry is picked
if (!m_moved && !m_cfg.preselect_disc_serial.empty() && serial == m_cfg.preselect_disc_serial)
{
m_selected = 0;
m_scroll = 0.0f;
m_scroll_vel = 0;
m_select_time = m_time;
std::printf("[frontend] disc in the drive: it was played last, so it is picked\n");
}
std::fflush(stdout);
}

void App::RemoveDriveEntry()
{
const int pos = DrivePos();
m_drive_on = false; // (the disc art stays for a moment: the copy's entry takes it over)
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
std::printf("[frontend] disc in the drive: entry removed\n");
std::fflush(stdout);
}

// The watcher tells the shelf which PS2 disc is in the drive and not copied (AppConfig::drive_disc_poll): it is listed first. Not while the disc
// is being copied (that entry stands for it) or the shelf is closing for a game.
void App::PollDriveDisc()
{
// The disc art is let go two seconds after the last entry that wanted it (the drive's entry becomes the copy's and back without asking again).
if (!m_dump_on && !m_drive_on && !m_disc_art_serial.empty())
{
if (m_art_idle < 0)
m_art_idle = m_time;
else if (m_time - m_art_idle > 2.0)
{
FreeDiscArt();
m_art_idle = -1;
}
}
else
m_art_idle = -1;
if (!m_cfg.drive_disc_poll)
return;
std::string title, serial;
const bool present = m_cfg.drive_disc_poll(title, serial) && !m_dump_on && !m_launching;
if (present && m_drive_on && serial != m_drive_serial)
RemoveDriveEntry(); // another disc
if (present && !m_drive_on && !ShelfBusy())
InsertDriveEntry(serial, title);
else if (!present && m_drive_on)
RemoveDriveEntry();
}

void App::Update(double dt, const Input& in)
{
	m_time += dt;
	UpdateQuitTick(); // 180: PS5SX2 closes a moment after the installer started
	m_account.Poll(m_cfg.achievements);
	PollGameAchievements();
	const float fdt = static_cast<float>(std::min(dt, 0.1));
	m_dump_spin += fdt; // live-22: the dump disc's rotation, advanced whether or not a dump is on
	PollDiscAsk();
	m_ask_anim += ((m_ask_on ? 1.0f : 0.0f) - m_ask_anim) * (1.0f - std::exp(-fdt * 16.0f));
	m_bios_anim += ((m_bios_ask ? 1.0f : 0.0f) - m_bios_anim) * (1.0f - std::exp(-fdt * 16.0f)); // bioscheck

	// 2.11: the search: what the settings page's box says, the PS5's keyboard while it is up, and the fades.
	if (m_cfg.search_remote)
	{
		std::string from_page;
		if (m_cfg.search_remote(from_page) && from_page != m_search.text)
		{
			if (m_search.typing && !m_search.system)
				CloseSearch(); // the page typed: its text wins over a half-typed one on the panel
			SetSearch(from_page);
		}
	}
	if (m_search.system)
	{
		std::string typed;
		const int r = m_cfg.text_entry.poll ? m_cfg.text_entry.poll(typed) : -1;
		if (r != 0)
		{
			m_search.system = false;
			m_search.typing = false;
			if (r > 0)
				SetSearch(typed);
		}
	}
	m_search.kb_anim += ((m_search.typing && !m_search.system ? 1.0f : 0.0f) - m_search.kb_anim) * (1.0f - std::exp(-fdt * 16.0f));
	m_search.pill_anim += ((m_search.typing || !m_search.applied.empty() ? 1.0f : 0.0f) - m_search.pill_anim) * (1.0f - std::exp(-fdt * 12.0f));

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
	else if (m_bios_ask) // bioscheck: the BIOS question
	{
		UpdateBiosChoice(in);
		m_prev = in;
	}
	else if (m_ask_on)
	{
		UpdateDiscAsk(in);
		m_prev = in;
	}
	else if (m_search.typing)
	{
		UpdateSearch(in, dt); // 2.11: the search keyboard has the pad
		m_prev = in;
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
				else if (m_index[static_cast<size_t>(m_selected)] == kDriveIndex) // 2.11: the disc in the drive: the question about it again
				{
				if (m_cfg.drive_disc_reask)
				m_cfg.drive_disc_reask();
				m_refused_text = "Asking about the disc...";
				m_refused_time = m_time;
				Sound(Sfx::Move, 0.0f);
				std::printf("[frontend] the disc in the drive was picked: asking about it again\n");
				std::fflush(stdout);
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
				else if (BiosReady(BiosWait::Pick)) // vk-285-134; bioscheck: the BIOS question when there is none
				{
					if (m_cfg.game_achievements.cancel)
						m_cfg.game_achievements.cancel();
					m_launching = true;
					m_launch_time = m_time;
					Sound(Sfx::Launch, 0.0f);
				}
			}
			// 2.11: R2 searches; Circle puts the whole shelf back while a search narrows it (and the QR view isn't up).
			else if (!m_qr_big && in.r2 && !m_prev.r2 && m_sheet_anim < 0.05f && !m_launching)
			{
				OpenSearch();
				Sound(Sfx::Move, 0.3f);
			}
			else if (!m_qr_big && in.circle && !m_prev.circle && !m_search.applied.empty())
			{
				SetSearch(std::string());
				Sound(Sfx::Move, -0.3f);
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
			// 2.11: a disc entry had no sheet of its own: the all-games one; gamedb (2026-10-30): it has, when it knows its settings file
			OpenSheet(m_games.empty() || m_games[static_cast<size_t>(m_selected)].stem.empty());
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

	// 2.11: the picked case turns over with the right stick (more of it the further it is pushed, either way), and stays over with its
	// click (R3) until the stick, the click or a move says otherwise. A soft spring with hardly any overshoot, and a cap on how fast it turns, so it is a thing
	// with weight that a flick of the thumb cannot spin (swordpdf: "small stick movements make the rotation go fast. make it harder to rotate").
	{
		const bool free_shelf = !m_games.empty() && m_index[static_cast<size_t>(m_selected)] >= 0 && !m_launching && !m_sheet_open && m_sheet_anim < 0.05f && !m_account.open &&
		                        !m_search.typing && !m_ask_on && !m_picker.open && !m_qr_big;
		float target = 0.0f;
		m_flip_wanted = false;
		if (free_shelf)
		{
			if (in.r3 && !m_r3_prev)
				m_flip_latched = !m_flip_latched;
			const float ax = std::fabs(in.rx);
			const bool pushed = ax > kFlipDead; // past the dead zone: the player means to turn it
			const float stick = pushed ? std::pow(std::min(1.0f, (ax - kFlipDead) / (kFlipFull - kFlipDead)), kFlipCurve) : 0.0f;
			if (pushed)
			{
				m_flip_latched = false; // the stick takes over from the click
				const float side = in.rx < 0.0f ? -1.0f : 1.0f;
				if (side != m_flip_dir)
				{
					// Pushed the other way while it is turned: the angle (m_flip * m_flip_dir) and its speed stay as they are, only the side they are counted
					// from changes -- the case never jumps across.
					m_flip = -m_flip;
					m_flip_vel = -m_flip_vel;
					m_flip_dir = side;
				}
			}
			target = m_flip_latched ? 1.0f : stick;
			m_flip_wanted = pushed || m_flip_latched; // (PollCovers: the back is wanted now, network included)
		}
		else
			m_flip_latched = false;
		m_r3_prev = in.r3;
		// The pose repeats every two flips (a case turned right round is where it began). After a swing to the other side the target may be much nearer the
		// other way round: from the back, a push to the other side leaves it where it is, and from well over it goes on to the back instead of all the way round
		// through the front. A way of more than a flip and a half is not taken (so the usual ways -- to the front, and to the back from the front -- never change).
		float want = target;
		if (want - m_flip > 1.5f)
			want -= 2.0f;
		else if (want - m_flip < -1.5f)
			want += 2.0f;
		const float fk = kFlipSpring, fc = 2.0f * std::sqrt(fk) * kFlipDamping;
		for (int i = 0; i < 4; i++)
		{
			const float h = fdt / 4;
			m_flip_vel += (fk * (want - m_flip) - fc * m_flip_vel) * h;
			m_flip_vel = std::max(-kFlipMaxSpeed, std::min(kFlipMaxSpeed, m_flip_vel));
			m_flip += m_flip_vel * h;
		}
		if (m_flip > 1.04f) // (the same pose, counted from the other side of the turn; below zero only while it swings through the front)
			m_flip -= 2.0f;
		else if (m_flip < -1.04f)
			m_flip += 2.0f;
		if (target == 0.0f && std::fabs(m_flip) < 0.002f && std::fabs(m_flip_vel) < 0.01f)
			m_flip = m_flip_vel = 0.0f;
	}

	PollDiscDump(); // live-22: the disc being copied shows as a spinning-disc entry at the front
	PollDriveDisc(); // 2.11: the disc in the drive that is not copied is the first entry
	PollTiles(false); // 2.12: the Home screen tile row's state (nothing is looked at unless the row is shown or an ask is pending)
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
	model = Mat4::Translate(x, bob, z) * Mat4::RotateY(turn + FlipTurn(d)); // 2.11: turned over with the right stick
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
			if (m_dump_disc_set)
			{
			AddDiscArt(f.ui, m_achievement_images, m_dump_disc_set, m_dump_disc_aspect, f.view_proj, b.model, m_dump_spin * 2.2f, a, W, H); // 2.11
			AddDiscSpinner(f.ui, f.view_proj, b.model, m_dump_spin, a, W, H, false); // the loading arc round it
			}
			else
			AddDiscSpinner(f.ui, f.view_proj, b.model, m_dump_spin, a, W, H);
			const Vec4 wp = b.model * Vec4(0.0f, kDiscTextY, kBoxHalfD + 0.01f, 1.0f);
			const Vec4 cp = f.view_proj * wp;
			const float sx = ((cp.x / cp.w) * 0.5f + 0.5f) * W, sy = ((cp.y / cp.w) * 0.5f + 0.5f) * H;
			char pctbuf[8];
			std::snprintf(pctbuf, sizeof(pctbuf), "%d%%", m_dump_pct);
			const float ppx = (54.0f + 60.0f * b.selected) * k;
			m_fonts->AddText(f.ui, pctbuf, sx, sy, ppx, Rgba(1.0f, 1.0f, 1.0f, std::min(1.0f, a * 1.4f)), 0.6f, Fonts::Center);
			}
			else if (m_index[static_cast<size_t>(i)] == kDriveIndex)
			{
			// 2.11: the disc in the drive, not copied: its own art on the box, turning slowly, and what picking it does.
			const float a = std::max(0.0f, 1.0f - std::fabs(d) * 1.6f) * b.brightness;
			if (m_dump_disc_set)
			AddDiscArt(f.ui, m_achievement_images, m_dump_disc_set, m_dump_disc_aspect, f.view_proj, b.model, m_dump_spin * 0.45f, a, W, H);
			else
			AddDiscSpinner(f.ui, f.view_proj, b.model, m_dump_spin * 0.18f, a, W, H, true, false);
			const Vec4 cp = f.view_proj * (b.model * Vec4(0.0f, kDiscTextY, kBoxHalfD + 0.01f, 1.0f));
			const float sx = ((cp.x / cp.w) * 0.5f + 0.5f) * W, sy = ((cp.y / cp.w) * 0.5f + 0.5f) * H;
			const float ppx = (30.0f + 26.0f * b.selected) * k;
			m_fonts->AddText(f.ui, "Disc in the drive", sx, sy, ppx, Rgba(1.0f, 1.0f, 1.0f, std::min(1.0f, a * 1.4f)), 0.6f, Fonts::Center);
			if (b.selected > 0.5f)
			m_fonts->AddText(f.ui, "Play it or copy it", sx, sy + 46.0f * k, 26.0f * k, Rgba(0.82f, 0.80f, 0.92f, std::min(1.0f, a * 1.2f)), 0.3f, Fonts::Center);
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
		f.halo.color[3] = settle * (0.50f + 0.08f * std::sin(t * 2.2f)) * (1.0f - std::min(1.0f, std::fabs(m_flip) * 2.0f)); // 2.11: gone as it turns over
	}

	// Fade out when launching. 186 (swordpdf, 2026-11-02, on opening PS5SX2 from the home screen: "it can be faster"): no fade in at start any more. The
	// system's launch screen (sce_sys/pic1.dds) is up until the first frame, and after "back to the menu" the screen is black: either way the 0.35 s fade
	// from black only put a dark moment between them and the shelf.
	f.fade = 1.0f;
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
	BuildGameDbLine(ui, W, k); // gamedb (2026-10-30)

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
	int scan_done = 0, scan_total = 0;
	if (n > 0 && m_cfg.scan_progress && m_cfg.scan_progress(scan_done, scan_total))
	{
		// chdscan: a disc drive's entry came before the games: the scan's line goes above the shelf
		char line[160];
		std::snprintf(line, sizeof(line), Tr(Str::ShelfScanning), scan_done, scan_total);
		m_fonts->AddText(ui, line, W * 0.5f, H * 0.12f, 44.0f * k, dim, 0.2f, Fonts::Center);
	}
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
		if (m_index[static_cast<size_t>(m_selected)] == kDumpIndex) // live-22 / 179: the disc being copied, with the live speed and the time left
		{
			char cp[48];
			std::snprintf(cp, sizeof(cp), Tr(Str::CopyDumpPct), m_dump_pct);
			add(cp);
			if (m_dump_mbps > 0.05)
			{
				char r[32];
				std::snprintf(r, sizeof(r), "%.1f MB/s", m_dump_mbps);
				add(r);
			}
			if (m_dump_eta > 0)
			{
				char e[48];
				if (m_dump_eta < 60)
					std::snprintf(e, sizeof(e), "%s", Tr(Str::CopyEtaLessMin));
				else
					std::snprintf(e, sizeof(e), Tr(Str::CopyEtaMin), (m_dump_eta + 59) / 60);
				add(e);
			}
			if (!m_dump_speed.empty())
				add(m_dump_speed); // 179: the drive speed taken (8x / 3.2x)
		}
			else if (m_index[static_cast<size_t>(m_selected)] == kDriveIndex) // 2.11 (review): no size
			add("In the drive");
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
		{
			int scan_done = 0, scan_total = 0;
			if (m_cfg.scan_progress && m_cfg.scan_progress(scan_done, scan_total))
			{
				// chdscan: the games come when the images not seen before have been read
				char line[160];
				std::snprintf(line, sizeof(line), Tr(Str::ShelfScanning), scan_done, scan_total);
				m_fonts->AddText(ui, line, W * 0.5f, H * 0.5f, 64.0f * k, white, 0.4f, Fonts::Center);
			}
			else if (m_search.applied.empty())
			{
				// 2026-10-16: with games folders set, the empty shelf names the first of them, not /data/PCSX2/games (a language file
				// without that path in its line is shown as it is)
				std::string none = Tr(Str::NoGames);
				static const char kDefaultPath[] = "/data/PCSX2/games";
				const size_t at = m_cfg.games_folder_note.empty() ? std::string::npos : none.find(kDefaultPath);
				if (at != std::string::npos)
				{
					none.replace(at, sizeof(kDefaultPath) - 1, m_cfg.games_folder_note);
					none = Fit(*m_fonts, none, 64.0f * k, W * 0.9f); // a long path is cut, not drawn off the screen
				}
				m_fonts->AddText(ui, none.c_str(), W * 0.5f, H * 0.5f, 64.0f * k, white, 0.4f, Fonts::Center);
			}
			else
			{
				// 2.11: a search with no match
				m_fonts->AddText(ui, "No game matches", W * 0.5f, H * 0.47f, 64.0f * k, white, 0.4f, Fonts::Center);
				m_fonts->AddText(ui, Fit(*m_fonts, "\xE2\x80\x9C" + m_search.text + "\xE2\x80\x9D", 48.0f * k, W * 0.7f).c_str(), W * 0.5f,
					H * 0.53f, 48.0f * k, dim, 0.2f, Fonts::Center);
			}
		}
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
	else if (m_search.typing && !m_search.system)
	{
		// 2.11: the search keyboard's buttons
		hint(icon::DpadLeftRight, nullptr, "Move");
		hint(icon::Cross, nullptr, "Type");
		hint(icon::Square, nullptr, "Delete");
		hint(icon::Triangle, nullptr, "Clear");
		hint("#L1", "#R1", "Keys");
		hint(icon::Circle, nullptr, "Done");
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
		hint(icon::Cross, nullptr, n > 0 && m_index[static_cast<size_t>(m_selected)] == kDriveIndex ? "Play or copy the disc" : Tr(Str::HintPlay)); // vk-285-110: hints in the PS5's language
		// vk-285-69: the shelf opens with one game too (its QR code leads to the settings page and the
		// logs); browsing needs two.
		if (n > 1)
		{
			hint(icon::DpadLeftRight, nullptr, Tr(Str::HintBrowse));
			hint("#L1", "#R1", Tr(Str::HintJump));
		}
		hint(icon::Square, nullptr, Tr(Str::HintSettings)); // vk-285-114
		if (!m_qr_big)
		{
			hint("#R2", nullptr, "Search"); // 2.11
			if (!m_search.applied.empty())
				hint(icon::Circle, nullptr, "Show all");
		}
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
	if (m_ask_anim > 0.001f)
		BuildDiscAsk(ui, W, H, k, accent);
	if (m_bios_anim > 0.001f)
		BuildBiosChoice(ui, W, H, k, accent); // bioscheck
	BuildSearch(ui, W, H, k, accent); // 2.11: the search line and its keyboard, over everything but the disc question
	f.ui_images = m_achievement_images; // 2.11: the texture changes among the UI vertices (badges, the disc being copied)
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
	// gamedb (2026-10-30): an entry with no settings file name (a disc whose name isn't known) has no sheet of its own: L1 from the all-games
	// sheet opened one that wrote settings/.ini, which no start reads (#bugs: a disc game ignored its own settings).
	if (g && g->stem.empty())
		g = nullptr;
	m_sheet_global = g == nullptr;
	if (m_sheet_global && m_cfg.game_achievements.cancel)
		m_cfg.game_achievements.cancel();
	m_sheet.SetTexturePackRow(static_cast<bool>(m_cfg.texture_packs)); // 2026-10-05
	m_sheet.SetOnlinePatchRow(static_cast<bool>(m_cfg.online_patches)); // 2026-10-08
	m_sheet.SetSystemMenuRow(m_cfg.system_menu);                        // 2026-10-08
	m_sheet.SetUpdateRows(static_cast<bool>(m_cfg.updates) && m_cfg.own_tile_game.empty()); // 180: not in a game tile's copy (it updates the main app)
	m_sheet.SetReloadTileRow(static_cast<bool>(m_cfg.reload_tile_on) && static_cast<bool>(m_cfg.set_reload_tile) && m_cfg.own_tile_game.empty()); // b185
	m_sheet.SetFolderRows(!m_cfg.folder_places.empty());                // vk-285-135
	m_tile_row = HomeTileEligible(g);                                   // 2.12: the Home screen tile row (a game with a plain image file; the flag turns it on)
	m_sheet.SetHomeTileRow(m_tile_row);
	m_sheet.SetHideRow(!g || m_index[static_cast<size_t>(m_selected)] >= 0); // gamedb (2026-10-30): not on a disc entry's sheet
	m_picker.open = false;
	OptionsPaths paths = m_cfg.options;
	if (!g || IsElfName(g->file.c_str())) // 2026-10-08: an ELF's Disc image row lists the shelf's disc images; vk-285-139: the Cheat disc row
	{
		for (size_t pos = 0; pos < m_games.size(); pos++) // 2.11 (review): not the disc entries (no file)
		if (m_index[pos] >= 0 && !IsElfName(m_games[pos].file.c_str()))
		paths.disc_images.emplace_back(m_games[pos].file, m_games[pos].title);
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
	// 2.12: the Home screen tile row is for this game from now on; the helper's state is looked at once, now, so the row shows the truth from its first frame.
	m_tile_view = gametile::View();
	m_tile_view_known = false;
	if (m_tile_row)
	{
		m_tile_path = gametile::NormalizePath(g->path);
		m_tile_title = g->title;
		m_tile_serial = g->serial;
		PollTiles(true);
	}
	else
		m_tile_path.clear();
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
	{
		RefreshBadges();
		if (m_sheet_global)
			RefreshBios(); // bioscheck: PS5SX2/OpenBios may have changed
	}
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
		row.kind == OptionsSheet::Kind::TexturesFolder || row.kind == OptionsSheet::Kind::NfsShares || // 2026-10-16: and the textures folder
		row.kind == OptionsSheet::Kind::SmbShares)
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
				if (BiosReady(BiosWait::SystemMenu)) // vk-285-134: the PS2's own menu is the BIOS (bioscheck: the question when there is none)
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

	// 180: the PS5SX2 group: System update (Cross looks; Cross twice installs a newer release) and Session logs (Cross, Left or Right switch it).
	if (row.kind == OptionsSheet::Kind::SystemUpdate || row.kind == OptionsSheet::Kind::LogUpload)
	{
		const bool horizontal = (in.left && !m_prev.left) || (in.right && !m_prev.right);
		if (pressed(in.cross, m_prev.cross) || (horizontal && row.kind == OptionsSheet::Kind::LogUpload))
			UpdateRowPress(row, now);
		return;
	}

	// b185: "Reload PS2 Disc tile": Cross, Left or Right switch it.
	if (row.kind == OptionsSheet::Kind::ReloadTile)
	{
		const bool horizontal = (in.left && !m_prev.left) || (in.right && !m_prev.right);
		if (pressed(in.cross, m_prev.cross) || horizontal)
			ReloadTileToggle(now);
		return;
	}

	// 2.12: "Home screen tile": Cross, Left and Right put the game on the PS5 home screen (the helper makes the tile), Cross or Triangle take it off again.
	if (row.kind == OptionsSheet::Kind::HomeTile)
	{
		const bool horizontal = (in.left && !m_prev.left) || (in.right && !m_prev.right);
		if (pressed(in.cross, m_prev.cross))
			HomeTileToggle(0);
		else if (horizontal)
			HomeTileToggle(1);
		else if (pressed(in.triangle, m_prev.triangle))
			HomeTileToggle(2);
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

// ---- 180: the sheet's PS5SX2 group: System update and Session logs (AI-assisted; see fe_update.h) -------------------
// The rows' words come from fe_i18n (English until a table has them).

namespace
{
std::string Say(Str id, const std::string& arg)
{
	char buf[1024];
	std::snprintf(buf, sizeof(buf), Tr(id), arg.c_str());
	return buf;
}

std::string ReleaseName(const ReleaseInfo& r)
{
	return r.title.empty() || r.title == r.tag ? r.tag : r.title + " (" + r.tag + ")";
}
} // namespace

void App::UpdateRowPress(const OptionsSheet::Row& row, double now)
{
	if (!m_cfg.updates)
		return;
	auto status = [&](const std::string& text) {
		m_sheet_status = text;
		m_sheet_status_time = now;
	};
	if (row.kind == OptionsSheet::Kind::LogUpload)
	{
		const bool on = !m_cfg.updates.log_upload();
		const bool ok = m_cfg.updates.set_log_upload(on);
		status(ok ? std::string(Tr(on ? Str::LogsOn : Str::LogsOff)) : std::string("Couldn't change /data/PS5SX2-Installer/no-log-upload"));
		std::printf("[options] session logs: %s%s\n", on ? "on" : "off", ok ? "" : " (failed)");
		std::fflush(stdout);
		Sound(ok ? Sfx::Move : Sfx::Edge, 0.3f);
		return;
	}
	using S = UpdateStatus::State;
	const UpdateStatus s = m_cfg.updates.status();
	switch (s.state)
	{
		case S::Available:
		case S::Interrupted:
		case S::StartFailed: // the installer didn't start: Cross twice tries again
			if (m_time > m_update_armed_until)
			{
				m_update_armed_until = m_time + 4.0;
				status(Tr(Str::UpdatePressAgain));
				Sound(Sfx::Move, 0.3f);
				return;
			}
			m_update_armed_until = -1;
			std::printf("[options] system update: install asked (%s, latest %s)\n", UpdateStateName(s.state), s.latest.tag.c_str());
			std::fflush(stdout);
			if (m_cfg.updates.install())
			{
				// installer-dl: no installer in the app: it is fetched first, and the row says so until it starts
				status(Tr(m_cfg.updates.status().state == S::DownloadingInstaller ? Str::UpdateDownloadingInstaller : Str::UpdateStarted));
				Sound(Sfx::Launch, 0.0f);
			}
			else
			{
				status(Say(Str::UpdateStartFailed, m_cfg.updates.status().error));
				Sound(Sfx::Edge, 0.3f);
			}
			return;
		case S::Checking:
		case S::Starting:
		case S::Started:
		case S::DownloadingInstaller:
			Sound(Sfx::Edge, 0.3f);
			return;
		default:
			break;
	}
	m_update_armed_until = -1;
	const bool started = m_cfg.updates.check(false);
	std::printf("[options] system update: look %s\n", started ? "asked" : "already running");
	std::fflush(stdout);
	Sound(started ? Sfx::Move : Sfx::Edge, 0.3f); // the row's value says "Checking..." and then the answer
}

// b185: the Reload PS2 Disc tile switched: the helper's flag file made (Off) or deleted (On); the helper does the rest within seconds.
void App::ReloadTileToggle(double now)
{
	if (!m_cfg.reload_tile_on || !m_cfg.set_reload_tile)
		return;
	const bool on = !m_cfg.reload_tile_on();
	const bool ok = m_cfg.set_reload_tile(on);
	m_sheet_status = ok ? std::string(Tr(on ? Str::ReloadTileOn : Str::ReloadTileOff)) : std::string("Couldn't change /data/PCSX2/flags/disctile_noreload");
	m_sheet_status_time = now;
	std::printf("[options] reload tile: %s%s\n", on ? "on" : "off (flag disctile_noreload)", ok ? "" : " (failed)");
	std::fflush(stdout);
	Sound(ok ? Sfx::Move : Sfx::Edge, 0.3f);
}

std::string App::UpdateRowValue(const OptionsSheet::Row& row) const
{
	if (!m_cfg.updates)
		return {};
	if (row.kind == OptionsSheet::Kind::LogUpload)
		return Tr(m_cfg.updates.log_upload() ? Str::LogsOn : Str::LogsOff);
	using S = UpdateStatus::State;
	const UpdateStatus s = m_cfg.updates.status();
	const bool armed = m_time <= m_update_armed_until;
	switch (s.state)
	{
		case S::Idle: return Tr(Str::UpdateCheck);
		case S::Checking: return Tr(Str::UpdateChecking);
		case S::Current: return Say(Str::UpdateCurrent, s.latest.title.empty() ? s.latest.tag : s.latest.title);
		case S::Newer: return Say(Str::UpdateNewer, s.latest.title.empty() ? s.latest.tag : s.latest.title);
		case S::Available: return armed ? std::string(Tr(Str::UpdatePressAgain)) : Say(Str::UpdateAvailable, ReleaseName(s.latest));
		case S::Failed: return Say(Str::UpdateFailed, s.error);
		case S::Interrupted: return armed ? std::string(Tr(Str::UpdatePressAgain)) : std::string(Tr(Str::UpdateInterrupted));
		case S::Starting: return Tr(Str::UpdateStarting);
		case S::Started: return Tr(Str::UpdateStarted);
		case S::StartFailed: return armed ? std::string(Tr(Str::UpdatePressAgain)) : Say(Str::UpdateStartFailed, s.error);
		case S::DownloadingInstaller: return Tr(Str::UpdateDownloadingInstaller);
	}
	return {};
}

std::string App::UpdateRowHelp(const OptionsSheet::Row& row) const
{
	if (!m_cfg.updates)
		return {};
	if (row.kind == OptionsSheet::Kind::LogUpload)
		return Tr(Str::LogsHelp);
	using S = UpdateStatus::State;
	const UpdateStatus s = m_cfg.updates.status();
	switch (s.state)
	{
		case S::Available:
		case S::StartFailed:
			return s.latest.tag.empty() ? std::string(Tr(Str::UpdateHelpInterrupted)) : Say(Str::UpdateHelpAvailable, ReleaseName(s.latest));
		case S::Interrupted: return Tr(Str::UpdateHelpInterrupted);
		case S::Starting:
		case S::Started:
		case S::DownloadingInstaller: return Tr(Str::UpdateHelpStarted);
		default: return Tr(Str::UpdateHelp);
	}
}

void App::UpdateQuitTick()
{
	if (!m_cfg.updates || m_update_quit_done)
		return;
	if (m_update_quit_at < 0)
	{
		if (m_cfg.updates.status().state == UpdateStatus::State::Started)
			m_update_quit_at = m_time + 3.0; // the notification is read; the installer waits for this process anyway
		return;
	}
	if (m_time >= m_update_quit_at)
	{
		m_update_quit_done = true;
		std::printf("[frontend] system update: the installer runs, PS5SX2 closes now\n");
		std::fflush(stdout);
		if (m_cfg.updates.quit)
			m_cfg.updates.quit();
	}
}

// ---- 2.12: the sheet's "Home screen tile" row (AI-assisted; see fe_gametile.h) ---------------------------------------
// swordpdf: "add an option to add a game from the shelf to the xmb (square - and then option on) it should utilize the existing mounting scheme we have, we
// should also be able to delete it, again, from the shelf". The row asks the PS5SX2 Disc Helper (one small file per press, in tiles_dir) to make a copy of this
// app under a title ID of its own that starts this game from the home screen, and shows what the helper says about it. English like the rest of the sheet.
// Nothing is read or written unless AppConfig::tiles_dir is set (on by default; the flag file flags/gametile_off clears it), and then only with the row shown or an ask pending.

bool App::ReloadTileFocused() const
{
	if (!m_sheet_open)
		return false;
	const auto& rows = m_sheet.rows();
	return m_sheet_row >= 0 && m_sheet_row < static_cast<int>(rows.size()) && rows[static_cast<size_t>(m_sheet_row)].kind == OptionsSheet::Kind::ReloadTile;
}

int App::TileFocused() const
{
	if (!m_sheet_open || !m_tile_row)
		return 0;
	const auto& rows = m_sheet.rows();
	return m_sheet_row >= 0 && m_sheet_row < static_cast<int>(rows.size()) && rows[static_cast<size_t>(m_sheet_row)].kind == OptionsSheet::Kind::HomeTile ? 1 : 0;
}

// A game of the shelf's own list (not the copy in progress or the disc in the drive: no file of the list), a plain image the helper can use.
bool App::HomeTileEligible(const GameInfo* g) const
{
	return !m_cfg.tiles_dir.empty() && g && !m_games.empty() && g == &m_games[static_cast<size_t>(m_selected)] &&
	       m_index[static_cast<size_t>(m_selected)] >= 0 && gametile::RowEligible(g->path, g->file, g->damaged);
}

// The menu thread's file access stalls while a texture pack is unpacked (the project measured frames of 0.95 s): nothing is looked at then.
bool App::DiskBusy() const
{
	if (!m_cfg.texture_packs || !m_cfg.texture_packs.activity)
		return false;
	using TS = TexturePackStatus::State;
	const TexturePackActivity a = m_cfg.texture_packs.activity();
	return a.active && (a.state == TS::Unpacking || a.state == TS::Installing || a.state == TS::Removing);
}

void App::HomeTileSay(const std::string& text)
{
	m_sheet_status = text;
	m_sheet_status_time = m_time;
}

// alive.txt and state.txt, looked at every game with an ask in the books (the sheet may have moved on to another game, or closed) and the sheet's game.
void App::PollTiles(bool force)
{
	if (m_cfg.tiles_dir.empty())
		return;
	const bool shown = m_sheet_open && m_tile_row;
	const bool pending = m_tile.AnyPending();
	if (!gametile::PollDue(shown || pending, pending, m_tile_view.busy, false, force, m_time, m_tile_poll))
		return;
	if (!force && DiskBusy())
		return; // due, but a texture pack is being unpacked: the menu thread's file access stalls then; looked at again as soon as that is over
	m_tile_poll = m_time;
	m_tile_polls++;
	const std::string& dir = m_cfg.tiles_dir;
	const bool alive = gametile::AliveFresh(dir, static_cast<long long>(std::time(nullptr)));
	m_tile_state.Refresh(dir, force, pending);
	std::vector<std::string> paths = m_tile.Paths();
	if (shown && std::find(paths.begin(), paths.end(), m_tile_path) == paths.end())
		paths.push_back(m_tile_path);
	for (const std::string& path : paths)
	{
		const std::string file = m_tile.FileOf(path);
		const bool present = !file.empty() && gametile::RequestPresent(dir, file);
		gametile::View v = m_tile.Evaluate(path, m_tile_state.entries, alive, present, m_time);
		if (v.withdraw)
		{
			// Not taken within 15 s: taken back, so a helper loaded later does not act on it. ENOENT: the helper took it just now.
			const gametile::WithdrawResult r = gametile::WithdrawFile(dir, file);
			m_tile.WithdrawDone(path, r, m_time);
			std::printf("[tiles] the request %s was not taken within %.0f s: %s\n", file.c_str(), gametile::kTakeS,
				r == gametile::WithdrawResult::Withdrawn ? "taken back" : r == gametile::WithdrawResult::AlreadyTaken ? "taken by the helper just now" : "could not be taken back");
			std::fflush(stdout);
			if (r == gametile::WithdrawResult::AlreadyTaken)
				v = m_tile.Evaluate(path, m_tile_state.entries, alive, false, m_time);
		}
		if (!shown || path != m_tile_path)
			continue;
		const gametile::Phase before = m_tile_view.phase;
		if (!m_tile_view_known || v.phase != before)
		{
			const std::string id = v.tile.empty() ? std::string("no tile id") : v.tile;
			std::printf("[tiles] %s: %s (%s%s%s)\n", m_tile_title.c_str(), gametile::PhaseName(v.phase), id.c_str(), v.reason.empty() ? "" : ", failed: ",
				v.reason.c_str());
			std::fflush(stdout);
			if (m_cfg.options.log)
				m_cfg.options.log("shelf: home screen tile: " + m_tile_title + ": " + gametile::PhaseName(v.phase) + " (" + id +
				                  (v.reason.empty() ? std::string() : ", failed: " + v.reason) + ")");
			if (m_tile_view_known && v.phase == gametile::Phase::On && before != gametile::Phase::On)
			{
				HomeTileSay("Tile ready: " + m_tile_title + " is on the home screen"); // never a system popup: no popups for tile mechanics
				Sound(Sfx::Move, 0.3f);
			}
		}
		m_tile_view = v;
		m_tile_view_known = true;
	}
}

// how: 0 Cross, 1 Left or Right, 2 Triangle. Cross adds or removes (what the row can do now); Left and Right only add (from Off); Triangle only removes.
void App::HomeTileToggle(int how)
{
	if (m_cfg.tiles_dir.empty() || !m_tile_row || m_tile_path.empty() || m_games.empty())
		return;
	const gametile::View& v = m_tile_view;
	if (v.phase == gametile::Phase::NoHelper)
	{
		// No file is written for a helper that is not there: nobody would take it, and a helper loaded later would act on a request of long ago.
		HomeTileSay("Load the PS5SX2 Disc Helper 171g first");
		Sound(Sfx::Edge, 0.3f);
		return;
	}
	if (v.busy)
	{
		HomeTileSay("Still working: wait for the helper");
		Sound(Sfx::Edge, 0.3f);
		return;
	}
	const bool add = how == 0 ? v.can == gametile::Can::Add : how == 1 && v.can == gametile::Can::Add && v.phase == gametile::Phase::Off;
	const bool remove = (how == 0 || how == 2) && v.can == gametile::Can::Remove;
	if (!add && !remove)
	{
		Sound(Sfx::Edge, 0.3f);
		return;
	}
	if (remove && !m_cfg.own_tile_game.empty() && gametile::NormalizePath(m_cfg.own_tile_game) == m_tile_path)
	{
		// Taking off the tile this very process runs from would have the helper close it under the player.
		HomeTileSay("This is the tile this game was started from: take it off from the main app");
		Sound(Sfx::Edge, 0.3f);
		return;
	}
	const GameInfo& g = m_games[static_cast<size_t>(m_selected)];
	const std::string cover = add && m_cfg.tile_cover ? m_cfg.tile_cover(g) : std::string();
	const std::string text = add ? gametile::FormatAdd(m_tile_path, m_tile_serial, m_tile_title, cover) : gametile::FormatRemove(m_tile_path);
	std::string file, err;
	if (text.empty() || !gametile::QueueRequest(m_cfg.tiles_dir, text, &file, &err))
	{
		if (text.empty())
			err = "not a path it can use";
		HomeTileSay("Couldn't ask the helper: " + err);
		std::printf("[tiles] %s: couldn't ask the helper: %s\n", m_tile_title.c_str(), err.c_str());
		std::fflush(stdout);
		Sound(Sfx::Edge, 0.3f);
		return;
	}
	m_tile.Ask(m_tile_path, add, file, m_time, gametile::FindByPath(m_tile_state.entries, m_tile_path));
	m_tile_view = m_tile.Evaluate(m_tile_path, m_tile_state.entries, true, true, m_time);
	m_tile_view_known = true;
	m_tile_poll = m_time; // the next look in a second
	HomeTileSay(std::string(add ? "Asked the helper to add " : "Asked the helper to take off ") + m_tile_title);
	std::printf("[tiles] %s: asked the helper to %s (%s)%s%s\n", m_tile_title.c_str(), add ? "add it" : "take it off", file.c_str(), cover.empty() ? "" : ", cover ",
		cover.c_str());
	std::fflush(stdout);
	if (m_cfg.options.log)
		m_cfg.options.log("shelf: home screen tile: " + m_tile_title + ": asked the helper to " + (add ? "add it" : "take it off") + " (" + m_tile_path + ")");
	Sound(Sfx::Move, 0.3f);
}

std::string App::HomeTileValue() const
{
	return m_cfg.tiles_dir.empty() ? std::string() : gametile::ValueText(m_tile_view);
}

std::string App::HomeTileHelp() const
{
	const bool own = !m_cfg.own_tile_game.empty() && gametile::NormalizePath(m_cfg.own_tile_game) == m_tile_path;
	return gametile::HelpText(m_tile_view, m_tile_title, own);
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
			status(ok ? "Asked for " + p.label + " (" + (p.OnDrive() ? std::string("Google Drive") : FormatBytes(p.bytes)) + ")" : "Couldn't start the download");
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
			return (n > 1 ? p.label : std::string("Download")) + "  \xC2\xB7  " + (p.OnDrive() ? std::string("Google Drive") : FormatBytes(p.bytes));
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
			std::string h = p.OnDrive()
			                    ? "Cross downloads " + p.maker + "'s pack (" + p.name +
			                      (p.hint_bytes > 0 ? ", about " + FormatBytes(p.hint_bytes) + "; the exact size shows when it starts" : std::string("; its size shows when it starts")) +
			                      ") from the community's Google Drive "
			                      "folder, puts it in " + folder + " and turns Texture replacements on for this game." + carry_on
			                    : "Cross downloads " + p.label + " (" + FormatBytes(p.bytes) +
			                      (p.files > 0 ? ", " + std::to_string(p.files) + " files" : std::string()) +
			                      ") from archive.org's PCSX2 HD Texture Packs, puts it in " + folder +
			                      " and turns Texture replacements on for this game." + carry_on;
			if (n > 1)
				h += " Left and right pick another version.";
			return h;
		}
		case State::Queued: return "Waiting for the pack before it." + carry_on + " Triangle twice cancels it.";
		case State::Checking: return "Checking what was downloaded before." + carry_on + " Triangle twice cancels it.";
		case State::Downloading:
			return std::string(s.job_pack >= 0 && s.job_pack < n && s.packs[static_cast<size_t>(s.job_pack)].OnDrive() ? "Downloading from Google Drive." :
			                                                                                                            "Downloading from archive.org.") +
			       carry_on + " Triangle twice cancels it.";
		case State::Verifying:
			return s.job_pack >= 0 && s.job_pack < n && s.packs[static_cast<size_t>(s.job_pack)].OnDrive() ? std::string("Checking the download.") :
			                                                                                              std::string("Checking the download against archive.org's checksum.");
		case State::Unpacking: return "Unpacking into " + folder + ". Triangle twice cancels it.";
		case State::Installing: return "Moving the pack into " + folder + ".";
		case State::Installed:
			if (s.ours)
			{
				std::string name = s.installed.empty() ? std::string("The pack") : s.installed;
				if (name.size() > 4 && (name.compare(name.size() - 4, 4, ".rar") == 0 || name.compare(name.size() - 4, 4, ".zip") == 0))
					name.resize(name.size() - 4);
				else if (name.size() > 3 && name.compare(name.size() - 3, 3, ".7z") == 0) // b176
					name.resize(name.size() - 3);
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
	if (!m_bios_missing && m_bios_open_in_use) // bioscheck: the open BIOS is named while games start with it
	{
		char note[160];
		std::snprintf(note, sizeof(note), Tr(Str::BiosInUse), "OpenPS2BIOS (experimental)");
		m_fonts->AddText(ui, note, W * 0.5f, 250.0f * k, 28.0f * k, Rgba(0.95f, 0.78f, 0.52f, 0.85f), 0.2f, Fonts::Center);
		return;
	}
	if (!m_bios_missing)
		return;
	const float since = static_cast<float>(m_time - m_bios_refused);
	const float flash = since >= 0.0f && since < 1.6f ? 1.0f - since / 1.6f : 0.0f;
	const float w = 2000.0f * k, x = W * 0.5f - w * 0.5f, y = 222.0f * k, h = m_bios_problem.empty() && !m_cfg.open_bios_bundled ? 96.0f * k : 150.0f * k;
	Fonts::AddRoundedRect(ui, x, y, w, h, 24.0f * k, Rgba(0.42f + 0.25f * flash, 0.20f + 0.08f * flash, 0.04f, 0.88f));
	char head[320];
	std::snprintf(head, sizeof(head), Tr(Str::ShelfNoBios), m_cfg.bios_dir.c_str());
	const float hpx = 40.0f * k, dpx = 32.0f * k;
	m_fonts->AddText(ui, Fit(*m_fonts, head, hpx, w - 60.0f * k).c_str(), W * 0.5f, y + 60.0f * k, hpx, Rgba(1, 1, 1), 0.4f, Fonts::Center);
	// bioscheck: with the open BIOS in the build, the line says a pick asks what to do (rather than what was found instead)
	const std::string second = m_cfg.open_bios_bundled ? std::string(Tr(Str::ShelfNoBiosChoice)) : m_bios_problem;
	if (!second.empty())
		m_fonts->AddText(ui, Fit(*m_fonts, second, dpx, w - 60.0f * k).c_str(), W * 0.5f, y + 116.0f * k, dpx,
			Rgba(1.0f, 0.86f, 0.62f), 0.2f, Fonts::Center);
}

// gamedb (2026-10-30, AI-assisted): resources/GameIndex.yaml is missing or unusable: one line under where the BIOS line goes (below it when
// that is up), so the player knows the folder is incomplete and that the installer repairs it. Games still get their fixes (the eboot's copy).
void App::BuildGameDbLine(std::vector<UiVertex>& ui, float W, float k)
{
	m_gamedb_note_shown = !m_cfg.gamedb_missing.empty();
	if (!m_gamedb_note_shown)
		return;
	char note[400];
	std::snprintf(note, sizeof(note), Tr(Str::ShelfNoGameDb), m_cfg.gamedb_missing.c_str());
	const float y = m_bios_missing ? 200.0f : m_bios_open_in_use ? 296.0f : 250.0f; // with the BIOS box up, just above it (under it is the case)
	const float px = 28.0f * k;
	m_fonts->AddText(ui, Fit(*m_fonts, note, px, 2000.0f * k).c_str(), W * 0.5f, y * k, px, Rgba(0.95f, 0.78f, 0.52f, 0.85f), 0.2f, Fonts::Center);
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
		const bool tile_row = r.kind == OptionsSheet::Kind::HomeTile; // 2.12: the helper's word for the game's tile (Off, Adding..., On, ...)
		if (tile_row)
			value = HomeTileValue();
		const bool update_row = r.kind == OptionsSheet::Kind::SystemUpdate; // 180: no arrows (Cross looks and installs)
		if (update_row || r.kind == OptionsSheet::Kind::LogUpload)
			value = UpdateRowValue(r);
		if (r.kind == OptionsSheet::Kind::ReloadTile && m_cfg.reload_tile_on) // b185
			value = Tr(m_cfg.reload_tile_on() ? Str::ReloadTileOn : Str::ReloadTileOff);
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
		                        r.kind == OptionsSheet::Kind::TexturesFolder || r.kind == OptionsSheet::Kind::NfsShares ||
		                        r.kind == OptionsSheet::Kind::SmbShares;
		if (folder_row && focused)
			value += "  \xE2\x80\xBA";
		const bool arrows = focused && !action && pick_arrows && !folder_row && !tile_row && !update_row;

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
			if (tile_row) // 2.12: On in the shelf's own colour; a failure, a request nobody answered or a helper that is not there in the warning colour
			{
				using TP = gametile::Phase;
				const TP p = m_tile_view.phase;
				vc = p == TP::On ? own : (p == TP::Failed || p == TP::NoAnswer || p == TP::Stuck || p == TP::NoHelper) ? Rgba(1.0f, 0.62f, 0.60f) : mid;
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
		if (focus_row.kind == OptionsSheet::Kind::HomeTile) // 2.12
			help = HomeTileHelp();
		if (focus_row.kind == OptionsSheet::Kind::SystemUpdate || focus_row.kind == OptionsSheet::Kind::LogUpload) // 180
			help = UpdateRowHelp(focus_row);
		if (focus_row.kind == OptionsSheet::Kind::ReloadTile) // b185
			help = Tr(Str::ReloadTileHelp);
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

// Review 2026-10-16: what the picker says when the settings writer refused the value (the file can't hold a '#', it starts a comment
// there, nor more than 200 characters): the plain reason, else the sheet's own "Couldn't save: ...".
std::string NotSavedBecause(const std::string& value, const std::string& sheet_status)
{
	if (value.find('#') != std::string::npos)
		return "Not saved: a '#' in a folder's name can't be kept in the settings";
	if (value.size() > 200)
		return "Not saved: the settings keep at most 200 characters here";
	return sheet_status;
}
} // namespace

namespace
{
bool IsShareList(OptionsSheet::Kind k)
{
	return k == OptionsSheet::Kind::NfsShares || k == OptionsSheet::Kind::SmbShares;
}

const char* ShareKey(OptionsSheet::Kind k)
{
	return k == OptionsSheet::Kind::SmbShares ? kSmbSharesKey : kNfsSharesKey;
}

const char* ShareScheme(OptionsSheet::Kind k)
{
	return k == OptionsSheet::Kind::SmbShares ? "smb://" : "nfs://";
}

void Wipe(std::string& s)
{
	volatile char* c = s.data();
	for (size_t i = 0; i < s.size(); i++)
		c[i] = 0;
	s.clear();
}
} // namespace

void App::OpenPicker(OptionsSheet::Kind kind)
{
	m_picker = FolderPicker();
	m_picker.open = true;
	m_picker.kind = kind;
	PickerList("");
	std::printf("[options] %s\n", kind == OptionsSheet::Kind::NfsShares ? "NFS share list" :
	                              kind == OptionsSheet::Kind::SmbShares ? "SMB share list" :
	                              kind == OptionsSheet::Kind::BiosFolder ? "folder picker for the BIOS folder" :
	                              kind == OptionsSheet::Kind::TexturesFolder ? "folder picker for the textures folder" : "folder picker for game folders");
	std::fflush(stdout);
}

void App::PickerList(const std::string& dir, const std::string& focus)
{
	FolderPicker& p = m_picker;
	p.dir = dir;
	p.items.clear();
	using T = PickerItem::Type;
	if (IsShareList(p.kind))
	{
		const bool smb = p.kind == OptionsSheet::Kind::SmbShares;
		p.items.push_back({T::Add, "Add a share", "", ShareScheme(p.kind)});
		for (const std::string& share : SplitFolderList(m_sheet.OwnValue(ShareKey(p.kind))))
			p.items.push_back({T::Share, share, share,
				smb && m_cfg.smb_has_password && m_cfg.smb_has_password(share) ? "Password set" : ""});
	}
	else if (dir.empty())
	{
		if (p.kind == OptionsSheet::Kind::GameFolders)
			for (const std::string& f : SplitFolderList(m_sheet.OwnValue(kGameFoldersKey)))
				p.items.push_back({T::Listed, f, f, "Added"});
		// 2.11 (swordpdf: "for picking paths you cant really browse to your folder"): a folder that is none of the places below (under
		// /data, say) is typed instead; the places also reach /data and the root now.
		p.items.push_back({T::TypePath, "Type a folder path", "", "/data/..."});
		for (const auto& [label, path] : m_cfg.folder_places)
			if (IsFolder(path))
				p.items.push_back({T::Place, label, path, path});
		for (const char* const extra : {"/data", "/mnt", "/"})
		{
			bool listed = false;
			for (const PickerItem& it : p.items)
				listed = listed || (it.type == T::Place && it.path == extra);
			if (!listed && IsFolder(extra))
				p.items.push_back({T::Place, std::string(extra) == "/" ? "Everything (the root)" : std::string("All of ") + extra, extra, extra});
		}
	}
	else
	{
		std::vector<std::string> folders;
		int images = 0;
		ListFolder(dir, folders, images);
		std::string value;
		if (p.kind == OptionsSheet::Kind::GameFolders)
		{
			// The games the shelf would list from it: its own, and those one folder down (as main-boot.cpp lists folders);
			// 2026-10-16: not those, with Scan subfolders off.
			int below = 0;
			const bool one_down = m_sheet.OwnValue(kScanSubfoldersKey) != "false";
			for (size_t i = 0; one_down && i < folders.size() && i < 64; i++)
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

void App::AskSharePassword(const std::string& url)
{
	FolderPicker& p = m_picker;
	p.password_for = url;
	if (m_cfg.text_entry.open && m_cfg.text_entry.open("Password for " + url, "", true, 255))
	{
		p.typing = true;
		return;
	}
	p.osk = true;
	p.osk_text.clear();
	p.osk_page = 0;
	p.osk_row = 1;
	p.osk_col = 0;
}

// vk-285-139: what a keyboard (the PS5's or the panel's) gave for a share's address: r > 0 typed, else cancelled.
void App::FinishShareText(int r, std::string text)
{
	FolderPicker& p = m_picker;
	auto status = [&](const std::string& t) {
		m_sheet_status = t;
		m_sheet_status_time = m_time;
	};
	if (!p.password_for.empty())
	{
		const std::string url = p.password_for;
		p.password_for.clear();
		Wipe(p.osk_text);
		std::string error;
		if (r > 0 && m_cfg.smb_set_password && m_cfg.smb_set_password(url, text, error))
			status(text.empty() ? "Password removed" : "Password saved: restart PS5SX2 to connect with it");
		else
			status(r > 0 ? "Password not saved: " + error : std::string("Password unchanged"));
		Wipe(text);
		PickerList("", url);
		Sound(r > 0 && error.empty() ? Sfx::Move : Sfx::Edge, 0.3f);
		return;
	}
	const char* const key = ShareKey(p.kind);
	const char* const scheme = ShareScheme(p.kind);
	const std::string kind_name = p.kind == OptionsSheet::Kind::SmbShares ? "SMB" : "NFS";
	while (!text.empty() && (text.back() == ' ' || text.back() == ';'))
		text.pop_back();
	while (!text.empty() && text.front() == ' ')
		text.erase(0, 1);
	// 2.11: a folder's path typed: go there when it is a folder (the listing then offers "Use this folder").
	if (p.path_entry)
	{
		p.path_entry = false;
		while (text.size() > 1 && text.back() == '/')
			text.pop_back();
		if (r > 0 && !text.empty() && text[0] == '/' && IsFolder(text))
		{
			PickerList(text);
			Sound(Sfx::Move, 0.3f);
		}
		else
		{
			status(r > 0 && !text.empty() ? "Not a folder on this console: " + text : std::string("Nothing typed"));
			Sound(Sfx::Edge, 0.3f);
		}
		return;
	}
	// vk-285-138 (AI-assisted; swordpdf: "i cant edit my nfs path"): Cross on a listed share opens the keyboard with it, and
	// what comes back takes its place.
	const std::string editing = p.editing;
	p.editing.clear();
	if (r > 0 && text.size() > 6 && StartsWithNoCase(text, scheme) && text.find(';') == std::string::npos)
	{
		std::vector<std::string> list = SplitFolderList(m_sheet.OwnValue(key));
		const auto old = std::find(list.begin(), list.end(), editing);
		if (!editing.empty() && old != list.end())
		{
			*old = text;
			list.erase(std::unique(list.begin(), list.end()), list.end());
		}
		else if (std::find(list.begin(), list.end(), text) == list.end())
			list.push_back(text);
		const bool changed = text != editing;
		if (changed && m_sheet.SetOwn(key, JoinFolderList(list), kind_name + (editing.empty() ? " share added: " : " share changed: ") + text))
			status((editing.empty() ? "Added " : "Changed to ") + text + ": restart PS5SX2 to mount it");
		else if (!changed)
			status("Unchanged");
		PickerList("", text);
		Sound(Sfx::Move, 0.3f);
		// A new SMB share with a user name: its password next.
		if (changed && p.kind == OptionsSheet::Kind::SmbShares && text.find('@') != std::string::npos && m_cfg.smb_set_password &&
			!(m_cfg.smb_has_password && m_cfg.smb_has_password(text)))
			AskSharePassword(text);
	}
	else
	{
		status(r > 0 && !text.empty() && text != scheme ? std::string("Not an ") + scheme + " address: " + (editing.empty() ? "nothing added" : "the share stays as it was")
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
			bool place = p.dir == "/"; // 2026-10-16: the root is the top: Circle there goes back to the places (it stayed on the root)
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
				// 2026-10-16: the root itself is no folder to read from (the app skips a '/' entry): "Everything" opens it, a folder in it is used
				if (item.path.size() <= 1)
				{
					status("Pick a folder inside it");
					Sound(Sfx::Edge, 0.3f);
					break;
				}
				// Review 2026-10-16: the status says it was saved only when it was (a path with a '#' in it, or over 200 characters, is refused by
				// the settings writer: then the reason is what the player reads). Picking the same folder again is no failure.
				if (p.kind == K::BiosFolder)
				{
					const bool saved = m_sheet.SetOwn(kBiosFolderKey, item.path, "BIOS folder picked") || m_sheet.OwnValue(kBiosFolderKey) == item.path;
					// review 2026-10-16 (lens 3): the path last: the status is one line (30 px, about 93 characters) and a long path cut the
					// "(restart PS5SX2)" off the end
					status(saved ? "BIOS folder, the only place the BIOS is looked for (restart PS5SX2): " + item.path
					             : NotSavedBecause(item.path, m_sheet.status()));
				}
				else if (p.kind == K::TexturesFolder)
				{
					const bool saved = m_sheet.SetOwn(kTexturesDirKey, item.path, "Textures folder picked") || m_sheet.OwnValue(kTexturesDirKey) == item.path;
					status(saved ? "Textures folder: " + item.path + " (restart PS5SX2)" : NotSavedBecause(item.path, m_sheet.status()));
				}
				else
				{
					std::vector<std::string> list = SplitFolderList(m_sheet.OwnValue(kGameFoldersKey));
					if (std::find(list.begin(), list.end(), item.path) != list.end())
						status(item.path + " is listed already");
					else
					{
						const bool first = list.empty();
						list.push_back(item.path);
						// 2026-10-16: the folders added are the only ones games are listed from
						const std::string joined = JoinFolderList(list);
						if (m_sheet.SetOwn(kGameFoldersKey, joined, "game folder added"))
							status(first ? "Games now come from the folders you added only (restart PS5SX2): " + item.path
							             : "Added " + item.path + ": restart PS5SX2 to list its games"); // (the path last when it is the long text: one line)
						else
							status(NotSavedBecause(joined, m_sheet.status())); // review 2026-10-16: the list is over 200 characters, or a name has a '#'
					}
				}
				std::printf("[options] %s picked: %s\n", p.kind == K::BiosFolder ? "BIOS folder" : p.kind == K::TexturesFolder ? "textures folder" : "game folder",
					item.path.c_str());
				std::fflush(stdout);
				p.open = false;
				Sound(Sfx::Move, 0.3f);
				break;
			case T::TypePath: // 2.11: a folder's path on the keyboard
				p.path_entry = true;
				p.editing.clear();
				if (m_cfg.text_entry.open && m_cfg.text_entry.open("Folder path, for example /data/roms", "/data/", false, 255))
					p.typing = true;
				else
				{
					p.osk = true;
					p.osk_text = "/data/";
					p.osk_page = 0;
					p.osk_row = 1;
					p.osk_col = 0;
					Sound(Sfx::Move, 0.3f);
				}
				break;
			case T::Share: // vk-285-138: change it on the keyboard
			case T::Add:
				p.editing = item.type == T::Share ? item.path : std::string();
				if (m_cfg.text_entry.open &&
					m_cfg.text_entry.open(p.kind == K::SmbShares ? "SMB share: smb://[user@]server/share" : "NFS share: nfs://server/shared folder",
						item.type == T::Share ? item.path : item.value, false, 255))
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
	if (pressed(in.square, m_prev.square) && item.type == T::Share && p.kind == K::SmbShares)
	{
		if (!m_cfg.smb_set_password)
			status("Passwords can't be set here");
		else if (item.path.find('@') == std::string::npos)
			status("A guest share: put user@ before the server for a password");
		else
			AskSharePassword(item.path);
		Sound(Sfx::Move, 0.3f);
		return;
	}
	if (pressed(in.triangle, m_prev.triangle) && (item.type == T::Listed || item.type == T::Share))
	{
		const char* const key = item.type == T::Share ? ShareKey(p.kind) : kGameFoldersKey;
		std::vector<std::string> list = SplitFolderList(m_sheet.OwnValue(key));
		list.erase(std::remove(list.begin(), list.end(), item.path), list.end());
		const std::string what = item.type != T::Share ? "game folder" : p.kind == K::SmbShares ? "SMB share" : "NFS share";
		if (m_sheet.SetOwn(key, JoinFolderList(list), what + " removed: " + item.path))
			status("Removed " + item.path + (item.type == T::Listed && list.empty() ? ": back to the usual places (restart PS5SX2)" : " (restart PS5SX2)"));
		std::string error;
		if (item.type == T::Share && p.kind == K::SmbShares && m_cfg.smb_has_password && m_cfg.smb_has_password(item.path) &&
			m_cfg.smb_set_password)
			m_cfg.smb_set_password(item.path, "", error); // its password goes with it
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
	const char* what = p.kind == K::NfsShares ? "NFS SHARES" : p.kind == K::SmbShares ? "SMB SHARES" : p.kind == K::BiosFolder ? "THE BIOS FOLDER" :
	                   p.kind == K::TexturesFolder ? "THE TEXTURES FOLDER" : "A GAME FOLDER";
	const std::string head = IsShareList(p.kind) ? std::string(what) : std::string("PICK ") + what;
	m_fonts->AddText(ui, head.c_str(), cx, list_top + 40 * k, 26 * k, lo, 0.45f);
	if (!p.dir.empty())
		m_fonts->AddText(ui, Fit(*m_fonts, p.dir, 30 * k, inner).c_str(), cx, list_top + 84 * k, 30 * k, mid, 0.3f);
	else if (!IsShareList(p.kind))
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
		const bool action = it.type == T::Use || it.type == T::Add || it.type == T::TypePath;
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
	const bool smb = p.kind == K::SmbShares;
	if (!p.password_for.empty())
		help = "Type the password for " + p.password_for + (p.osk ? " (Cross types the key, Square deletes, Options is done, Circle cancels)" : "") +
		       ". Empty removes it. It is kept on the console only.";
	else if (p.osk && p.path_entry)
		help = "Type the folder's path, like /data/roms: Cross types the key, Square deletes, Options is done, Circle cancels.";
	else if (p.typing && p.path_entry)
		help = "Type the folder's path on the PS5's keyboard, like /data/roms. The picker goes to that folder.";
	else if (p.osk)
		help = smb ? "Type the share's address, smb://[user@]<server>/<share>: Cross types the key, Square deletes, Options is done, Circle cancels."
		           : "Type the share's address, nfs://<server>/<shared folder>: Cross types the key, Square deletes, Options is done, Circle "
		             "cancels. Add ?version=4 for an NFS v4 server.";
	else if (p.typing)
		help = smb ? "Type the share's address on the PS5's keyboard: smb://[user@]<server>/<share>, a folder inside it works too. "
		             "No password here: it is asked for next."
		           : "Type the share's address on the PS5's keyboard: nfs://<server>/<shared folder>, as the server shares it (a folder "
		             "inside it works too). Add ?version=4 for an NFS v4 server.";
	else if (it && it->type == T::TypePath)
		help = "Cross: type a folder's path (like /data/roms) when it is not under the places below. Circle: back to the sheet.";
	else if (it && it->type == T::Use)
		// 2026-10-16: the folder picked is the only one (the games, the BIOS), not one more place
		help = p.kind == K::BiosFolder ? "Cross: look for the BIOS only here (and in the folders in it). Circle: back."
		     : p.kind == K::TexturesFolder ? "Cross: read texture packs from here and install HD packs here. Circle: back."
		                                   : "Cross: list the games in this folder (and in the folders in it) only. Circle: back.";
	else if (it && (it->type == T::Listed || it->type == T::Share))
		help = it->type == T::Share ? std::string("Cross: change it on the PS5's keyboard. ") + (smb ? "Square: its password. " : "") +
		                                  "Triangle: remove it from the shares. Circle: back to the sheet."
		                            : std::string("Triangle: remove it from the game folders. Circle: back to the sheet.");
	else if (it && it->type == T::Add)
		help = smb ? "Cross: type a share's address on the PS5's keyboard (smb://[user@]<server>/<share>). Circle: back to the sheet."
		           : "Cross: type a share's address on the PS5's keyboard (nfs://<server>/<shared folder>). Circle: back to the sheet.";
	else
		help = std::string("Cross: open the folder. ") + (p.dir.empty() ? "Circle: back to the sheet." : "Circle: up a folder.");
	if (!p.typing && !IsShareList(p.kind))
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

// 2.04: a PS2 disc was put in the drive and is not copied: the disc watcher (orbis-shims/ProsperoDiscDump.cpp) asks here whether to
// play it straight from the disc or copy it first. Left/Right choose, Cross confirms, Circle leaves the disc alone for now.
void App::PollDiscAsk()
{
	if (!m_cfg.disc_ask_poll)
		return;
	std::string title, serial;
	const bool pending = m_cfg.disc_ask_poll(title, serial);
	if (pending && !m_ask_on && !m_launching && !m_bios_ask)
	{
		m_ask_on = true;
		m_ask_title = title;
		m_ask_serial = serial;
		m_ask_sel = 0;
		m_held = 0;
		Sound(Sfx::Move, 0.0f);
		std::printf("[frontend] disc ask: %s (%s)\n", title.c_str(), serial.c_str());
		std::fflush(stdout);
	}
	else if (!pending && m_ask_on)
		m_ask_on = false; // the disc was taken out, or the question was answered elsewhere
}

void App::UpdateDiscAsk(const Input& in)
{
	const bool left = in.left && !m_prev.left, right = in.right && !m_prev.right;
	int answer = 0;
	if (left || right)
	{
		m_ask_sel = 1 - m_ask_sel;
		Sound(Sfx::Move, left ? -0.2f : 0.2f);
	}
	else if (in.cross && !m_prev.cross)
		answer = m_ask_sel == 0 ? 2 : 1;
	else if (in.circle && !m_prev.circle)
		answer = 3;
	if (answer != 0)
	{
		std::printf("[frontend] disc ask: answered %s\n", answer == 2 ? "play from the disc" : answer == 1 ? "copy it" : "not now");
		std::fflush(stdout);
		if (m_cfg.disc_ask_answer)
			m_cfg.disc_ask_answer(answer);
		m_ask_on = false;
		Sound(answer == 3 ? Sfx::Edge : Sfx::Launch, 0.0f);
	}
}

void App::BuildDiscAsk(std::vector<UiVertex>& ui, float W, float H, float k, uint32_t accent)
{
	const float e = Smoothstep(0.0f, 1.0f, m_ask_anim);
	const size_t begin = ui.size();
	const uint32_t hi = Rgba(1, 1, 1), mid = Rgba(0.76f, 0.74f, 0.86f), lo = Rgba(0.56f, 0.54f, 0.68f);
	const uint32_t ink = Rgba(0.07f, 0.06f, 0.16f), inkmid = Rgba(0.30f, 0.28f, 0.44f);
	// The shelf behind, dimmed so that the panel is what the eye goes to.
	Fonts::AddRoundedRect(ui, 0, 0, W, H, 0.0f, Rgba(0.01f, 0.01f, 0.03f, 0.76f));

	const float pw = 1560 * k, ph = 740 * k, px = (W - pw) * 0.5f, py = (H - ph) * 0.5f - 20 * k;
	const float pad = 84 * k, cx = px + pad, inner = pw - 2 * pad;
	// The panel: a soft shadow, a faint rim, the dark body, and a thin accent line along its top.
	Fonts::AddRoundedRect(ui, px - 8 * k, py + 14 * k, pw + 16 * k, ph + 16 * k, 52 * k, Rgba(0, 0, 0, 0.38f));
	Fonts::AddRoundedRect(ui, px - 2 * k, py - 2 * k, pw + 4 * k, ph + 4 * k, 46 * k, Rgba(1, 1, 1, 0.14f));
	Fonts::AddRoundedRect(ui, px, py, pw, ph, 44 * k, Rgba(0.045f, 0.050f, 0.105f, 0.985f));
	Fonts::AddRoundedRect(ui, px + 120 * k, py, pw - 240 * k, 6 * k, 3 * k, accent);

	// The disc: a bright rim, a darker face with a ring in the accent colour, and the hole.
	const float dr = 62 * k, dcx = cx + dr, dcy = py + 148 * k;
	Fonts::AddRoundedRect(ui, dcx - dr - 6 * k, dcy - dr - 6 * k, 2 * (dr + 6 * k), 2 * (dr + 6 * k), dr + 6 * k, Rgba(1, 1, 1, 0.10f));
	Fonts::AddRoundedRect(ui, dcx - dr, dcy - dr, 2 * dr, 2 * dr, dr, Rgba(0.80f, 0.80f, 0.92f, 0.95f));
	Fonts::AddRoundedRect(ui, dcx - dr * 0.80f, dcy - dr * 0.80f, 2 * dr * 0.80f, 2 * dr * 0.80f, dr * 0.80f, Rgba(0.10f, 0.10f, 0.22f, 1.0f));
	Fonts::AddRoundedRect(ui, dcx - dr * 0.50f, dcy - dr * 0.50f, 2 * dr * 0.50f, 2 * dr * 0.50f, dr * 0.50f, accent);
	Fonts::AddRoundedRect(ui, dcx - dr * 0.44f, dcy - dr * 0.44f, 2 * dr * 0.44f, 2 * dr * 0.44f, dr * 0.44f, Rgba(0.10f, 0.10f, 0.22f, 1.0f));
	Fonts::AddRoundedRect(ui, dcx - dr * 0.17f, dcy - dr * 0.17f, 2 * dr * 0.17f, 2 * dr * 0.17f, dr * 0.17f, Rgba(0.80f, 0.80f, 0.92f, 0.95f));

	// What is in the drive: a small label, the game's name, its serial.
	const float tx = cx + 2 * dr + 56 * k, tw = inner - 2 * dr - 56 * k;
	m_fonts->AddText(ui, "PS2 DISC IN THE DRIVE", tx, py + 96 * k, 26 * k, mid, 0.40f);
	m_fonts->AddText(ui, Fit(*m_fonts, m_ask_title, 64 * k, tw).c_str(), tx, py + 168 * k, 64 * k, hi, 0.55f);
	if (!m_ask_serial.empty())
		m_fonts->AddText(ui, m_ask_serial.c_str(), tx, py + 214 * k, 28 * k, lo, 0.20f);

	Fonts::AddRoundedRect(ui, cx, py + 262 * k, inner, 2 * k, 0, Rgba(1, 1, 1, 0.10f));
	m_fonts->AddText(ui, "How do you want to play it?", cx, py + 322 * k, 34 * k, mid, 0.25f);

	// Two cards, side by side: the focused one is white with dark text and a glow in the accent colour.
	const float gap = 36 * k, cw = (inner - gap) * 0.5f, ch = 220 * k, cy = py + 360 * k;
	auto card = [&](float x0, const char* title, const char* line1, const char* line2, bool focused) {
		if (focused)
		{
			Fonts::AddRoundedRect(ui, x0 - 8 * k, cy - 8 * k, cw + 16 * k, ch + 16 * k, 46 * k, Rgba(1, 1, 1, 0.16f));
			Fonts::AddRoundedRect(ui, x0 - 4 * k, cy - 4 * k, cw + 8 * k, ch + 8 * k, 42 * k, accent);
			Fonts::AddRoundedRect(ui, x0, cy, cw, ch, 38 * k, Rgba(1, 1, 1, 0.96f));
		}
		else
			Fonts::AddRoundedRect(ui, x0, cy, cw, ch, 38 * k, Rgba(1, 1, 1, 0.07f));
		const uint32_t t = focused ? ink : hi, c = focused ? inkmid : mid;
		m_fonts->AddText(ui, title, x0 + cw * 0.5f, cy + 84 * k, 46 * k, t, 0.55f, Fonts::Center);
		m_fonts->AddText(ui, line1, x0 + cw * 0.5f, cy + 140 * k, 29 * k, c, 0.15f, Fonts::Center);
		m_fonts->AddText(ui, line2, x0 + cw * 0.5f, cy + 182 * k, 29 * k, c, 0.15f, Fonts::Center);
	};
	card(cx, "Play from disc", "Starts right now.", "The disc stays in the drive.", m_ask_sel == 0);
	card(cx + cw + gap, "Copy to storage", "A few minutes, then it plays.", "After that the disc can come out.", m_ask_sel == 1);

	// The buttons, in small chips along the bottom.
	float hx = cx;
	auto hint = [&](const char* key, const char* what) {
		const float kpx = 24 * k, kw = m_fonts->Measure(key, kpx) + 36 * k, kh = 46 * k, hy = py + ph - 100 * k;
		Fonts::AddRoundedRect(ui, hx, hy, kw, kh, kh * 0.5f, Rgba(1, 1, 1, 0.14f));
		m_fonts->AddText(ui, key, hx + kw * 0.5f, hy + kh * 0.5f + 9 * k, kpx, hi, 0.45f, Fonts::Center);
		hx += kw + 14 * k;
		hx += m_fonts->AddText(ui, what, hx, hy + kh * 0.5f + 9 * k, kpx, lo, 0.20f) + 40 * k;
	};
	hint("Left / Right", "choose");
	hint("Cross", "confirm");
	hint("Circle", "not now");
	FadeRange(ui, begin, ui.size(), e);
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
	std::string shown = p.password_for.empty() ? p.osk_text : std::string(p.osk_text.size(), '*');
	const float px = 44 * k, room = w - 120 * k;
	while (shown.size() > 1 && m_fonts->Measure(shown.c_str(), px) > room)
		shown.erase(0, 1);
	const bool caret = std::fmod(m_time, 1.0) < 0.6;
	m_fonts->AddText(ui, (shown + (caret ? "|" : " ")).c_str(), x + 60 * k, fy + fh * 0.5f + px * 0.36f, px, Rgba(1, 1, 1), 0.3f);
	BuildPanelKeyboard(ui, x, ky, w, k,
		!p.password_for.empty() ? "Password" : p.kind == OptionsSheet::Kind::SmbShares ? "SMB share" : "NFS share", p.osk_page,
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

// ---- 2.11: the shelf's search (AI-assisted; swordpdf: "add a search section to the shelf (make it sleek)") --------------------------
// R2 on the shelf opens a keyboard and the shelf narrows to the games that match as each key goes in (ApplyHidden leaves the others
// waiting in m_shelved, covers and all, so clearing the search is instant). The PS5's own keyboard is used when the app can open it
// (AppConfig::text_entry: it returns the whole text when it closes); otherwise the panel's, in a compact form at the bottom of the screen
// with the shelf live above it. The settings page's search box does the same from a phone (AppConfig::search_remote).

namespace
{
std::string SearchLower(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (const char c : s)
		out += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
	return out;
}

// Spaces collapsed, none at either end, at most 64 bytes (not cut inside a UTF-8 character).
std::string SearchClean(const std::string& s)
{
	std::string out;
	for (const char c : s)
	{
		if (static_cast<unsigned char>(c) < 0x20)
			continue;
		if (c == ' ' && (out.empty() || out.back() == ' '))
			continue;
		out += c;
	}
	while (!out.empty() && out.back() == ' ')
		out.pop_back();
	if (out.size() > 64)
	{
		out.resize(64);
		while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
			out.pop_back();
		if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0xC0)
			out.pop_back();
	}
	return out;
}
} // namespace

bool App::SearchMatches(const GameInfo& g) const
{
	const std::string& q = m_search.applied;
	if (q.empty())
		return true;
	std::string plain;
	for (const char c : g.serial)
		if (c != '-')
			plain += c;
	const std::string hay = SearchLower(g.title + " " + g.serial + " " + plain + " " + g.region + " " + g.extra + " " + g.stem);
	for (size_t at = 0; at < q.size();)
	{
		size_t end = q.find(' ', at);
		if (end == std::string::npos)
			end = q.size();
		if (end > at && hay.find(q.substr(at, end - at)) == std::string::npos)
			return false; // every word of the search must be somewhere in the game's names
		at = end + 1;
	}
	return true;
}

void App::SetSearch(const std::string& text)
{
	const std::string clean = SearchClean(text);
	const bool changed_text = clean != m_search.text;
	m_search.text = clean;
	const std::string applied = SearchLower(clean);
	if (applied != m_search.applied)
	{
		// The game picked stays picked while it matches; else the next one on the shelf. With no match the shelf is empty, and
		// clearing the search brings back the game that was picked last.
		const int keep = m_games.empty() ? m_search_return : m_index[static_cast<size_t>(m_selected)];
		if (!m_games.empty() && m_index[static_cast<size_t>(m_selected)] >= 0)
			m_search_return = m_index[static_cast<size_t>(m_selected)];
		m_search.applied = applied;
		ApplyHidden(keep);
		m_flip_latched = false;
		m_select_time = m_time;
		std::printf("[frontend] search \"%s\": %zu of %zu games\n", clean.c_str(), m_games.size(), m_games.size() + m_shelved.size());
		std::fflush(stdout);
	}
	if (changed_text && m_cfg.search_report)
		m_cfg.search_report(clean);
}

void App::OpenSearch()
{
	if (m_search.typing)
		return;
	m_search.typing = true;
	m_search.system = false;
	m_search.page = 0; // the letters, on "q"
	m_search.row = 1;
	m_search.col = 0;
	m_search.held_x = m_search.held_y = 0;
	if (m_cfg.text_entry.open && m_cfg.text_entry.open("Search games", m_search.text, false, 64))
		m_search.system = true; // the PS5's keyboard: it has the pad, and the result comes when it closes
	std::printf("[frontend] search: %s keyboard\n", m_search.system ? "the PS5's" : "the panel's");
	std::fflush(stdout);
}

void App::CloseSearch()
{
	m_search.typing = false;
	m_search.system = false;
}

void App::UpdateSearch(const Input& in, double dt)
{
	using Panel = AchievementAccountPanel;
	Search& s = m_search;
	if (s.system)
		return; // the PS5's keyboard is up: Update polls it
	auto pressed = [&](bool now, bool before) { return now && !before; };
	// The D-pad, repeating while it is held.
	const int dx = in.left ? -1 : in.right ? 1 : 0, dy = in.up ? -1 : in.down ? 1 : 0;
	const bool fresh = (in.left && !m_prev.left) || (in.right && !m_prev.right) || (in.up && !m_prev.up) || (in.down && !m_prev.down);
	bool step = false;
	if ((dx || dy) && fresh)
	{
		step = true;
		s.held_x = dx;
		s.held_y = dy;
		s.held_for = 0;
		s.next_repeat = 0.34;
	}
	else if ((dx || dy) && dx == s.held_x && dy == s.held_y)
	{
		s.held_for += dt;
		while (s.held_for >= s.next_repeat)
		{
			step = true;
			s.next_repeat += 0.075;
		}
	}
	else
		s.held_x = s.held_y = 0;
	if (step)
	{
		bool moved = false;
		if (dy != 0)
		{
			const int to = std::clamp(s.row + dy, 0, Panel::FnRow);
			if (to != s.row)
			{
				const float x = Panel::KeyCentre(s.page, s.row, s.col);
				int best = 0;
				for (int c = 1; c < Panel::RowKeys(s.page, to); c++)
					if (std::abs(Panel::KeyCentre(s.page, to, c) - x) < std::abs(Panel::KeyCentre(s.page, to, best) - x))
						best = c;
				s.row = to;
				s.col = best;
				moved = true;
			}
		}
		if (dx != 0)
		{
			const int to = std::clamp(s.col + dx, 0, Panel::RowKeys(s.page, s.row) - 1);
			moved = moved || to != s.col;
			s.col = to;
		}
		Sound(moved ? Sfx::Move : Sfx::Edge, 0.3f);
	}
	auto erase = [&] {
		std::string t = s.text;
		if (!t.empty())
		{
			t.pop_back();
			while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80)
				t.pop_back();
			if (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0xC0)
				t.pop_back();
		}
		SetSearch(t);
	};
	if (pressed(in.square, m_prev.square))
	{
		erase();
		Sound(Sfx::Move, 0.3f);
	}
	if (pressed(in.triangle, m_prev.triangle))
	{
		SetSearch(std::string());
		Sound(Sfx::Move, -0.3f);
	}
	if (pressed(in.l1, m_prev.l1))
		s.page = (s.page + Panel::Pages - 1) % Panel::Pages;
	if (pressed(in.r1, m_prev.r1))
		s.page = (s.page + 1) % Panel::Pages;
	if (pressed(in.l1, m_prev.l1) || pressed(in.r1, m_prev.r1))
	{
		s.col = std::min(s.col, Panel::RowKeys(s.page, s.row) - 1);
		Sound(Sfx::Move, 0.3f);
	}
	if (pressed(in.circle, m_prev.circle) || pressed(in.options, m_prev.options))
	{
		CloseSearch();
		Sound(Sfx::Move, -0.3f);
		return;
	}
	if (pressed(in.cross, m_prev.cross))
	{
		Sound(Sfx::Move, 0.3f);
		if (s.row < Panel::FnRow)
		{
			std::string t = s.text;
			t += Panel::PageRows[s.page][s.row][s.col];
			SetSearch(t);
		}
		else
			switch (s.col)
			{
				case Panel::FnShift: s.page = s.page == 0 ? 1 : 0; break;
				case Panel::FnSymbols: s.page = s.page == 2 ? 0 : 2; break;
				case Panel::FnSpace: SetSearch(s.text + " "); break;
				case Panel::FnDelete: erase(); break;
				case Panel::FnDone: CloseSearch(); return;
			}
		s.col = std::min(s.col, Panel::RowKeys(s.page, s.row) - 1);
	}
}

// The search line: a glass pill at the top centre (a magnifier, what was typed with a caret, and how many games match), and under it,
// while typing on the panel's keyboard, a compact keyboard sliding up from the bottom.
void App::BuildSearch(std::vector<UiVertex>& ui, float W, float H, float k, uint32_t accent)
{
	using Panel = AchievementAccountPanel;
	const Search& s = m_search;
	const float pe = Smoothstep(0.0f, 1.0f, s.pill_anim), ke = Smoothstep(0.0f, 1.0f, s.kb_anim);
	if (pe < 0.01f && ke < 0.01f)
		return;
	const uint32_t hi = Rgba(1, 1, 1), mid = Rgba(0.74f, 0.72f, 0.84f), lo = Rgba(0.52f, 0.50f, 0.64f);
	const float sheet_e = Smoothstep(0.0f, 1.0f, m_sheet_anim);
	if (pe >= 0.01f && sheet_e < 0.5f && m_account_anim < 0.5f)
	{
		const size_t begin = ui.size();
		const bool typing = s.typing;
		const float px = 46.0f * k, cpx = 32.0f * k, pill_h = 96.0f * k, pad = 38.0f * k;
		int n = 0; // 2.11 (review): the games, not the disc entries
		for (const int idx : m_index)
		n += idx >= 0 ? 1 : 0;
		char count[48];
		if (s.applied.empty())
			std::snprintf(count, sizeof(count), "%d games", n);
		else if (n == 0)
			std::snprintf(count, sizeof(count), "no match");
		else
			std::snprintf(count, sizeof(count), n == 1 ? "%d game" : "%d games", n);
		const float count_w = m_fonts->Measure(count, cpx);
		const float icon_w = 40.0f * k, gap = 26.0f * k;
		const char* const hint_text = "Type to search";
		const float max_text = W * 0.30f;
		std::string shown = s.text;
		while (shown.size() > 1 && m_fonts->Measure(shown.c_str(), px) > max_text)
		{
			shown.erase(0, 1);
			while (!shown.empty() && (static_cast<unsigned char>(shown.front()) & 0xC0) == 0x80)
				shown.erase(0, 1);
		}
		const float text_w = std::max(shown.empty() ? m_fonts->Measure(hint_text, px) : m_fonts->Measure(shown.c_str(), px), 360.0f * k);
		const float w = pad + icon_w + gap + text_w + gap * 1.5f + count_w + pad;
		const float x = std::floor((W - w) * 0.5f), y = 108.0f * k;
		// Shadow, a hairline border (the accent while typing) and the glass.
		Fonts::AddRoundedRect(ui, x - 6 * k, y + 10 * k, w + 12 * k, pill_h + 12 * k, pill_h * 0.5f + 6 * k, Rgba(0, 0, 0, 0.30f));
		Fonts::AddRoundedRect(ui, x - 2 * k, y - 2 * k, w + 4 * k, pill_h + 4 * k, pill_h * 0.5f + 2 * k,
			typing ? Rgba(Mix(m_glow[0], 1.0f, 0.5f), Mix(m_glow[1], 1.0f, 0.5f), Mix(m_glow[2], 1.0f, 0.5f), 0.85f) : Rgba(1, 1, 1, 0.18f));
		const uint32_t glass = Rgba(0.045f, 0.050f, 0.105f, 0.95f);
		Fonts::AddRoundedRect(ui, x, y, w, pill_h, pill_h * 0.5f, glass);
		// The magnifier: a ring and a short handle.
		const float ix = x + pad, iy = y + pill_h * 0.5f - 21.0f * k, d = 30.0f * k;
		const uint32_t ink = typing || !s.text.empty() ? hi : mid;
		Fonts::AddRoundedRect(ui, ix, iy, d, d, d * 0.5f, ink);
		Fonts::AddRoundedRect(ui, ix + 4.5f * k, iy + 4.5f * k, d - 9.0f * k, d - 9.0f * k, (d - 9.0f * k) * 0.5f, glass);
		for (int i = 0; i < 4; i++)
			Fonts::AddRoundedRect(ui, ix + d * 0.78f + i * 4.6f * k, iy + d * 0.78f + i * 4.6f * k, 7.0f * k, 7.0f * k, 3.0f * k, ink);
		// What was typed (or the hint), and the caret while the keyboard is up.
		const float tx = x + pad + icon_w + gap, ty = y + pill_h * 0.5f + px * 0.36f;
		if (shown.empty())
		{
			m_fonts->AddText(ui, hint_text, tx, ty, px, lo, 0.3f);
			if (typing && std::fmod(m_time, 1.0) < 0.6)
				Fonts::AddRoundedRect(ui, tx - 2 * k, y + 24 * k, 4 * k, pill_h - 48 * k, 2 * k, accent);
		}
		else
		{
			const float adv = m_fonts->AddText(ui, shown.c_str(), tx, ty, px, hi, 0.3f);
			if (typing && std::fmod(m_time, 1.0) < 0.6)
				Fonts::AddRoundedRect(ui, tx + adv + 4 * k, y + 24 * k, 4 * k, pill_h - 48 * k, 2 * k, accent);
		}
		m_fonts->AddText(ui, count, x + w - pad, y + pill_h * 0.5f + cpx * 0.36f, cpx, n == 0 && !s.applied.empty() ? Rgba(1.0f, 0.78f, 0.55f) : lo,
			0.3f, Fonts::Right);
		FadeRange(ui, begin, ui.size(), pe * (1.0f - sheet_e));
	}
	if (ke >= 0.01f)
	{
		const float ks = k * 0.70f;
		const float key_w = 152 * ks, gap = 16 * ks, pad = 48 * ks, key_h = 108 * ks;
		const float w = Panel::Columns * key_w + (Panel::Columns - 1) * gap + 2 * pad;
		const float h = 5 * key_h + 4 * gap + 2 * pad + 64 * ks;
		const float x = std::floor((W - w) * 0.5f);
		const float rest = H - h - 176.0f * k;
		const float y = rest + (1.0f - ke) * (h + 240.0f * k);
		const size_t begin = ui.size();
		BuildPanelKeyboard(ui, x, y, w, ks, "Search", s.page, s.row, s.col);
		FadeRange(ui, begin, ui.size(), Clamp(ke * 1.5f, 0.0f, 1.0f));
	}
}
} // namespace fe
