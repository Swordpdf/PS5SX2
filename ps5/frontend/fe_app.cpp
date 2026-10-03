// PS5 port frontend: the cover-flow shelf (see fe_app.h).
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_app.h"
#include "fe_i18n.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
	m_scroll = static_cast<float>(m_selected);
	m_atlas = m_renderer->CreateTexture(static_cast<uint32_t>(fonts->AtlasWidth()), static_cast<uint32_t>(fonts->AtlasHeight()),
		VK_FORMAT_R8_UNORM, fonts->AtlasPixels().data());
	if (!m_atlas)
		return false;
	m_renderer->SetAtlas(m_atlas);
	if (m_covers)
		m_covers->SetSelected(m_selected);
	return true;
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
	if (m_covers)
		m_covers->SetSelected(m_selected);
	return true;
}

void App::Sound(Sfx sfx, float pan)
{
	if (m_cfg.sound)
		m_cfg.sound->Play(sfx, pan);
}

void App::PollCovers()
{
	if (!m_covers)
		return;
	CoverImage img;
	int budget = 4; // textures a frame
	while (budget-- > 0 && m_covers->Poll(img))
	{
		if (img.game < 0 || img.game >= static_cast<int>(m_slots.size()))
			continue;
		Slot& s = m_slots[static_cast<size_t>(img.game)];
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
			std::printf("[frontend] cover for %s (%s, %dx%d)\n", m_games[static_cast<size_t>(img.game)].title.c_str(),
				img.source, img.width, img.height);
		}
		s.dirty = true;
	}
	for (Slot& s : m_slots)
		if (s.dirty && s.spine && s.placeholder)
		{
			m_renderer->FreeTextureSet(s.set);
			s.set = m_renderer->AllocTextureSet(s.cover ? s.cover : s.placeholder, s.placeholder, s.spine);
			s.dirty = false;
		}
}

void App::Update(double dt, const Input& in)
{
	m_time += dt;
	m_account.Poll(m_cfg.achievements);
	PollGameAchievements();
	const float fdt = static_cast<float>(std::min(dt, 0.1));

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
	else if (m_sheet_open)
	{
		UpdateSheet(dt, in);
		m_prev = in;
	}
	else if (m_released && !m_qr_big && m_sheet_anim < 0.05f && m_cfg.achievements.state &&
	         in.l1 && in.square && !(m_prev.l1 && m_prev.square))
	{
		// Handle the account chord before shelf navigation or settings (AI-assisted).
		m_account.Open();
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
				Sound(Step(-5) ? Sfx::JumpLeft : Sfx::Edge, -0.25f);
			if (in.r1 && !m_prev.r1)
				Sound(Step(5) ? Sfx::JumpRight : Sfx::Edge, 0.25f);
			if (((in.cross && !m_prev.cross) || (in.options && !m_prev.options)) && !m_games.empty() && !m_qr_big)
			{
				if (m_cfg.game_achievements.cancel)
					m_cfg.game_achievements.cancel();
				m_launching = true;
				m_launch_time = m_time;
				Sound(Sfx::Launch, 0.0f);
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
	if (m_launching && ad < 0.5f)
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
	const Vec3 shift(1.45f * slide, 0.0f, 0.0f);
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
		label += m_account.account.saved ? m_account.account.username : "Sign in";
		m_fonts->AddText(ui, label.c_str(), margin, 290.0f * k, 34.0f * k, dim);
	}

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
		add(SizeText(g.bytes));
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
	FadeRange(ui, title_begin, ui.size(), 1.0f - sheet_e); // vk-285-114: the sheet shows the title itself

	// vk-285-114: the options sheet, over a dimmed shelf.
	if (m_sheet_anim > 0.001f)
	{
		Fonts::AddRoundedRect(ui, 0, 0, W, H, 0.0f, Rgba(0.01f, 0.01f, 0.03f, 0.42f * sheet_e));
		BuildSheet(ui, W, H, k, accent);
		f.ui_images = m_achievement_images;
	}

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
	if (m_sheet_open || m_sheet_anim > 0.5f)
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
	if (sheet_e < 0.5f)
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
	if (m_account.open)
		BuildAccount(ui, W, H, k);
}

void App::UpdateAccount(const Input& in)
{
	auto pressed = [](bool now, bool before) { return now && !before; };
	m_account.Move(pressed(in.right, m_prev.right) - pressed(in.left, m_prev.left),
		pressed(in.down, m_prev.down) - pressed(in.up, m_prev.up));
	if (pressed(in.circle, m_prev.circle))
		m_account.Back();
	else if (pressed(in.triangle, m_prev.triangle))
		m_account.Erase();
	else if (pressed(in.square, m_prev.square))
		m_account.TogglePasswordVisibility();
	else if (pressed(in.cross, m_prev.cross))
		m_account.Accept(m_cfg.achievements);
}

void App::BuildAccount(std::vector<UiVertex>& ui, float W, float H, float k)
{
	Fonts::AddRoundedRect(ui, 0, 0, W, H, 0, Rgba(0, 0, 0, 0.85f));
	const float left = W * 0.5f - 740 * k;
	const uint32_t white = Rgba(1, 1, 1), dim = Rgba(0.75f, 0.75f, 0.85f);
	m_fonts->AddText(ui, "RetroAchievements", left, 380 * k, 64 * k, white);
	const char* state = m_account.account.busy          ? "Signing in..." :
		                m_account.account.authenticated ? "Signed in" :
		                m_account.account.saved         ? "Account saved - reconnects when a game starts" :
		                                                  "Sign in";
	m_fonts->AddText(ui, state, left, 455 * k, 36 * k, dim);
	auto field_text = [](const std::string& value) {
		return value.size() > 48 ? "..." + value.substr(value.size() - 48) : value;
	};
	const std::string labels[] = {"Username: " + field_text(m_account.username),
		"Password: " + field_text(m_account.show_password ? m_account.password : std::string(m_account.password.size(), '*')),
		"Sign in", "Sign out"};
	for (int i = 0; i < 4; i++)
	{
		const float y = (520 + 100 * i) * k;
		Fonts::AddRoundedRect(ui, left, y, 1480 * k, 82 * k, 12 * k,
			Rgba(0.35f, 0.25f, 0.6f, i == m_account.row ? 0.95f : 0.35f));
		const std::string label = labels[i].size() > 64 ? labels[i].substr(0, 61) + "..." : labels[i];
		m_fonts->AddText(ui, label.c_str(), left + 28 * k, y + 56 * k, 38 * k, white);
	}
	if (!m_account.account.message.empty())
		m_fonts->AddText(ui, m_account.account.message.c_str(), left, 970 * k, 28 * k, dim);
	if (m_account.editing)
	{
		for (int i = 0; i < AchievementAccountPanel::KeyCount; i++)
		{
			const float x = left + (i % AchievementAccountPanel::Columns) * 123 * k;
			const float y = (1020 + (i / AchievementAccountPanel::Columns) * 82) * k;
			Fonts::AddRoundedRect(ui, x, y, 115 * k, 74 * k, 10 * k,
				Rgba(0.4f, 0.3f, 0.65f, i == m_account.key ? 1.0f : 0.3f));
			const char text[] = {AchievementAccountPanel::Keys[i], '\0'};
			m_fonts->AddText(ui, text[0] == ' ' ? "SP" : text, x + 57 * k, y + 52 * k, 42 * k, white, 0, Fonts::Center);
		}
	}
	m_fonts->AddText(ui, m_account.editing ? "D-pad: choose key   X: type   Triangle: erase   Circle: done" : "D-pad: choose field   X: select   Circle: back", left, 1770 * k, 34 * k, dim);
	m_fonts->AddText(ui, m_account.show_password ? "Square: hide password" : "Square: show password", left, 1840 * k, 34 * k, dim);
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
	constexpr float kSheetW = 1260.0f, kSheetTop = 206.0f, kSheetBottomGap = 222.0f;
	constexpr float kSheetListTop = 308.0f; // from the sheet's top (vk-285-117: the top is one line shorter)
	constexpr float kSheetHelpH = 352.0f; // the help box at the bottom
	constexpr float kRowH = 94.0f, kHeaderH = 76.0f;
} // namespace

void App::OpenSheet(bool global)
{
	const GameInfo* g = (!global && !m_games.empty()) ? &m_games[static_cast<size_t>(m_selected)] : nullptr;
	m_sheet_global = g == nullptr;
	if (m_sheet_global && m_cfg.game_achievements.cancel)
		m_cfg.game_achievements.cancel();
	m_sheet.Open(m_cfg.options, g);
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
	if (!m_cfg.refresh_game || m_games.empty())
		return;
	if (m_sheet_global)
		for (GameInfo& g : m_games)
			m_cfg.refresh_game(g);
	else
		m_cfg.refresh_game(m_games[static_cast<size_t>(m_selected)]);
}

void App::CloseSheet()
{
	if (!m_sheet_open)
		return;
	m_sheet_open = false;
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
	if (pressed(in.l2, m_prev.l2) || pressed(in.r2, m_prev.r2))
	{
		const int direction = pressed(in.r2, m_prev.r2) ? 1 : -1;
		const int tab = (m_sheet.tab() + direction + kTabCount) % kTabCount;
		if (tab != m_sheet.tab())
		{
			SheetTab(tab);
			Sound(tab == kTabControls ? Sfx::JumpRight : Sfx::JumpLeft, tab == kTabControls ? 0.3f : -0.1f);
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
void App::BuildGameAchievements(std::vector<UiVertex>& ui, float x, float y, float width, float height, float k)
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
	m_fonts->AddText(ui, Fit(*m_fonts, m_game_achievements.title, 34 * k, width).c_str(), x, y + 38 * k, 34 * k, white);
	m_fonts->AddText(ui, summary.c_str(), x, y + 85 * k, 29 * k, green);
	constexpr float row_height = 142;
	const int visible = std::max(1, static_cast<int>((height / k - 190) / row_height));
	const int count = static_cast<int>(m_game_achievements.entries.size());
	const int first = std::clamp(m_achievement_row - visible / 2, 0, std::max(0, count - visible));
	for (int i = first; i < std::min(count, first + visible); ++i)
	{
		const auto& entry = m_game_achievements.entries[static_cast<size_t>(i)];
		const float top = y + (115 + (i - first) * row_height) * k;
		Fonts::AddRoundedRect(ui, x - 12 * k, top, width + 24 * k, 130 * k, 18 * k,
			i == m_achievement_row ? Rgba(1, 1, 1, 0.12f) : Rgba(1, 1, 1, 0.035f));
		const auto& badge = m_achievement_badges[static_cast<size_t>(i)];
		if (badge.set)
		{
			const uint32_t first_vertex = static_cast<uint32_t>(ui.size());
			const float bx = x + 10 * k, by = top + 17 * k, size = 96 * k;
			const UiVertex a{bx, by, 0, 0, white, 0, 0, 0, 2}, b{bx + size, by, 1, 0, white, 0, 0, 0, 2};
			const UiVertex c{bx + size, by + size, 1, 1, white, 0, 0, 0, 2}, d{bx, by + size, 0, 1, white, 0, 0, 0, 2};
			ui.insert(ui.end(), {a, b, c, a, c, d});
			m_achievement_images.push_back({first_vertex, 6, badge.set});
		}
		else
		{
			Fonts::AddRoundedRect(ui, x + 10 * k, top + 17 * k, 96 * k, 96 * k, 12 * k, Rgba(1, 1, 1, 0.1f));
			m_fonts->AddText(ui, "RA", x + 58 * k, top + 80 * k, 28 * k, muted, 0.2f, Fonts::Center);
		}
		const float tx = x + 125 * k, room = width - 135 * k;
		const std::string status = (entry.unlocked ? Tr(Str::AchievementStatusUnlocked) : Tr(Str::AchievementStatusLocked)) + std::string("  |  ") +
			                       std::to_string(entry.points) + " " + Tr(Str::AchievementPoints);
		const float status_width = m_fonts->Measure(status.c_str(), 25 * k);
		m_fonts->AddText(ui, Fit(*m_fonts, entry.title, 31 * k, room - status_width - 25 * k).c_str(), tx, top + 40 * k, 31 * k, white);
		m_fonts->AddText(ui, status.c_str(), x + width, top + 40 * k, 25 * k, entry.unlocked ? green : muted, 0.2f, Fonts::Right);
		float baseline = top + 78 * k;
		for (const auto& line : Wrap(*m_fonts, entry.description, 26 * k, room, 2))
		{
			m_fonts->AddText(ui, line.c_str(), tx, baseline, 26 * k, muted);
			baseline += 32 * k;
		}
	}
	const std::string footer = std::to_string(count ? m_achievement_row + 1 : 0) + " / " + std::to_string(count) +
		                       "   |   " + (m_game_achievements.busy ? Tr(Str::AchievementImages) : Tr(Str::AchievementTriangleRefresh));
	m_fonts->AddText(ui, footer.c_str(), x, y + height - 10 * k, 26 * k, muted);
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
	// vk-285-117 (AI-assisted): a calmer top (Spyros: the title touched what was above and below it): the title alone on the
	// first line with room around it; under it this game / all games (L1 / R1) on the left and the Settings / Controls tabs
	// (L2 / R2) on the right, the shown tab white and underlined in the cover's colour. The file's name is gone (the
	// settings page shows it). A long translation shrinks the line (to three quarters) rather than overlapping.
	{
		float px = 52 * k;
		const std::string& t = m_sheet.title();
		while (px > 40 * k && m_fonts->Measure(t.c_str(), px) > inner)
			px -= 2 * k;
		m_fonts->AddText(ui, Fit(*m_fonts, t, px, inner).c_str(), cx, y + 112 * k, px, hi, 0.55f);
	}
	{
		const float py = y + 184 * k, ph = 60 * k; // 56 px between the title's descenders and the pills
		const bool controls = m_sheet.tab() == kTabControls;
		const bool achievements = m_sheet.tab() == kTabAchievements;
		const char* const this_label = Tr(Str::SheetThisGame);
		const char* const all_label = Tr(Str::SheetAllGames);
		const char* const settings_label = Tr(Str::HintSettings);
		const char* const controls_label = Tr(Str::SheetControls);
		float f = 1.0f, pills_w = 0, tabs_w = 0;
		auto widths = [&]() {
			const float ppx = 30 * k * f, tpx = 34 * k * f, kpx = 24 * k * f;
			pills_w = m_fonts->Measure(this_label, ppx) + m_fonts->Measure(all_label, ppx) + 2 * 50 * k * f + 14 * k * f;
			tabs_w = m_fonts->Measure("L2", kpx) + m_fonts->Measure("R2", kpx) + 2 * 20 * k * f + 2 * 22 * k * f + 40 * k * f +
				     m_fonts->Measure(settings_label, tpx) + m_fonts->Measure(controls_label, tpx) +
				     m_fonts->Measure(Tr(Str::Achievements), tpx) + 28 * k * f;
		};
		widths();
		while (f > 0.76f && pills_w + tabs_w + 40 * k > inner)
		{
			f -= 0.05f;
			widths();
		}
		const float ppx = 30 * k * f, pad = 50 * k * f, tpx = 34 * k * f, kpx = 24 * k * f;
		float px = cx;
		auto pill = [&](const char* label, bool on, bool enabled) {
			const float w = m_fonts->Measure(label, ppx) + pad;
			if (on)
				Fonts::AddRoundedRect(ui, px, py, w, ph, ph * 0.5f, Rgba(1, 1, 1, 0.92f));
			else
				Fonts::AddRoundedRect(ui, px, py, w, ph, ph * 0.5f, Rgba(1, 1, 1, enabled ? 0.10f : 0.04f));
			m_fonts->AddText(ui, label, px + w * 0.5f, py + ph * 0.5f + 11 * k * f, ppx, on ? Rgba(0.07f, 0.06f, 0.16f) : (enabled ? mid : lo),
				0.5f, Fonts::Center);
			px += w + 14 * k * f;
		};
		pill(this_label, !m_sheet_global, !m_games.empty());
		pill(all_label, m_sheet_global, true);
		const float base = py + ph * 0.5f + 12 * k * f;
		float tx = x + sw - 64 * k - tabs_w;
		auto key_pill = [&](const char* name) {
			const float kw = m_fonts->Measure(name, kpx) + 20 * k * f, kh = 38 * k * f;
			Fonts::AddRoundedRect(ui, tx, py + (ph - kh) * 0.5f, kw, kh, 10 * k * f, Rgba(1, 1, 1, 0.82f));
			m_fonts->AddText(ui, name, tx + kw * 0.5f, py + ph * 0.5f + 9 * k * f, kpx, Rgba(0.08f, 0.08f, 0.14f), 0.6f, Fonts::Center);
			tx += kw;
		};
		auto tab = [&](const char* label, bool on) {
			const float w = m_fonts->Measure(label, tpx);
			m_fonts->AddText(ui, label, tx, base, tpx, on ? hi : lo, on ? 0.5f : 0.3f);
			if (on)
				Fonts::AddRoundedRect(ui, tx, base + 12 * k * f, w, 5 * k * f, 2.5f * k * f, accent);
			tx += w;
		};
		key_pill("L2");
		tx += 22 * k * f;
		tab(settings_label, !controls && !achievements);
		tx += 40 * k * f;
		tab(controls_label, controls);
		tx += 28 * k * f;
		tab(Tr(Str::Achievements), achievements);
		tx += 22 * k * f;
		key_pill("R2");
	}
	Fonts::AddRoundedRect(ui, cx, y + 280 * k, inner, 2 * k, 0, Rgba(1, 1, 1, 0.12f));

	if (m_sheet.tab() == kTabAchievements)
	{
		BuildGameAchievements(ui, cx, y + 310 * k, inner, sh - 370 * k, k);
		FadeRange(ui, begin, ui.size(), e);
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
		const float lpx = 38 * k, vpx = 36 * k;
		const bool action = r.kind == OptionsSheet::Kind::Recommended || r.kind == OptionsSheet::Kind::ResetAll;

		std::string value = m_sheet.Value(r);
		const bool armed = m_sheet.Armed(r, m_time);
		if (armed)
			value = "Press again";
		else if (action && r.kind == OptionsSheet::Kind::ResetAll)
			value.clear();
		if (r.kind == OptionsSheet::Kind::NewCard && focused)
			value += "  \xC2\xB7  Create";

		const float value_room = bw * 0.46f;
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
			const float vx = bx + bw - 44 * k - (focused && !action ? 30 * k : 0.0f);
			float vw = m_fonts->AddText(ui, shown.c_str(), vx, mid_y, vpx, vc, 0.5f, Fonts::Right);
			if (vsym && vglyph != icon::Blank)
				vw += 14 * k + m_fonts->AddText(ui, vglyph.c_str(), vx - vw - 14 * k, mid_y + kSymbolDrop * vpx, vpx * kSymbolScale, vc, 0.5f,
								   Fonts::Right);
			if (focused && !action)
			{
				// The arrows either side of a value that left and right change.
				m_fonts->AddText(ui, "\xE2\x80\xB9", vx - vw - 26 * k, mid_y + 1 * k, 44 * k, hi, 0.4f, Fonts::Center);
				m_fonts->AddText(ui, "\xE2\x80\xBA", vx + 26 * k, mid_y + 1 * k, 44 * k, hi, 0.4f, Fonts::Center);
			}
			// A dot beside a value this file sets itself (not what it follows).
			if (from == OptionsSheet::From::Own && !action && !focused)
				Fonts::AddRoundedRect(ui, vx - vw - 30 * k, mid_y - 18 * k, 12 * k, 12 * k, 6 * k, own);
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
		const std::string help = m_sheet.Help(rows[static_cast<size_t>(m_sheet_row)]);
		float hy = list_bottom + 110 * k;
		for (const std::string& line : Wrap(*m_fonts, help, 31 * k, inner, 4))
		{
			m_fonts->AddText(ui, line.c_str(), cx, hy, 31 * k, mid, 0.1f);
			hy += 44 * k;
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
} // namespace fe
