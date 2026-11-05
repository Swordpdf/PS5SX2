// PS5 port frontend: the in-game menu (see fe_ingame.h).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_ingame.h"

#include "fe_i18n.h"
#include "fe_math.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace fe
{
namespace
{
// The shelf's sheet (fe_app.cpp), in 2160-line pixels: the panel is the sheet, at the same place and size.
constexpr float kMargin = 110.0f;
constexpr float kPanelW = 1520.0f, kPanelTop = 206.0f, kPanelBottomGap = 222.0f;
constexpr float kListTop = 308.0f, kHelpH = 352.0f;
constexpr float kRowH = 100.0f, kHeaderH = 84.0f;
constexpr float kActionRowH = 120.0f; // the Game tab's rows: fewer, so taller
constexpr float kAchievementRowH = 124.0f;
constexpr float kSymbolScale = 1.35f, kSymbolDrop = 0.12f;
constexpr double kOpenSeconds = 0.34, kCloseSeconds = 0.24;
constexpr int kSlotStrip = 5; // slots shown under the save and load rows

// Font Awesome Free solid (SIL OFL 1.1; the icons CC BY 4.0).
constexpr uint32_t kIconPlay = 0xF04B, kIconSave = 0xF0C7, kIconLoad = 0xF07C, kIconForward = 0xF04E, kIconDisc = 0xF51F,
				   kIconPhone = 0xF3CD, kIconFlag = 0xF024, kIconHouse = 0xF015, kIconPause = 0xF04C, kIconTrophy = 0xF091,
				   kIconLock = 0xF023;

const float kHouse[3] = {0.55f, 0.42f, 1.0f};

uint32_t Rgba(float r, float g, float b, float a = 1.0f)
{
	auto c = [](float v) { return static_cast<uint32_t>(Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
	return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

uint32_t Mixed(float t, float a = 1.0f)
{
	return Rgba(Mix(kHouse[0], 1.0f, t), Mix(kHouse[1], 1.0f, t), Mix(kHouse[2], 1.0f, t), a);
}

uint32_t Faded(uint32_t c, float f)
{
	const float a = static_cast<float>(c >> 24) / 255.0f * Clamp(f, 0.0f, 1.0f);
	return (c & 0x00FFFFFFu) | (static_cast<uint32_t>(a * 255.0f + 0.5f) << 24);
}

const uint32_t kHi = Rgba(1, 1, 1), kMid = Rgba(0.74f, 0.72f, 0.84f), kLo = Rgba(0.52f, 0.50f, 0.64f);
const uint32_t kDim = Rgba(0.72f, 0.69f, 0.82f), kInk = Rgba(0.07f, 0.06f, 0.16f), kWarn = Rgba(1.0f, 0.62f, 0.60f);
const uint32_t kAmber = Rgba(1.0f, 0.77f, 0.45f), kGold = Rgba(1.0f, 0.82f, 0.40f);

std::string Utf8(uint32_t cp)
{
	std::string s;
	if (cp < 0x80)
		s += static_cast<char>(cp);
	else if (cp < 0x800)
	{
		s += static_cast<char>(0xC0 | (cp >> 6));
		s += static_cast<char>(0x80 | (cp & 0x3F));
	}
	else if (cp < 0x10000)
	{
		s += static_cast<char>(0xE0 | (cp >> 12));
		s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		s += static_cast<char>(0x80 | (cp & 0x3F));
	}
	else
	{
		s += static_cast<char>(0xF0 | (cp >> 18));
		s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		s += static_cast<char>(0x80 | (cp & 0x3F));
	}
	return s;
}

std::string Format(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

std::string Fit(const Fonts& fonts, std::string text, float px, float width)
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
	for (const char c : text)
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
		lines.back() = Fit(fonts, lines.back() + " \xE2\x80\xA6", px, width);
	}
	return lines;
}

// vk-285-116: "<PromptFont glyph>  Cross" (fe_options.cpp's Controls rows) into its two parts.
bool SplitSymbol(const std::string& s, std::string& glyph, std::string& name)
{
	if (s.size() < 6 || static_cast<unsigned char>(s[0]) != 0xE2 || s.compare(3, 2, "  ") != 0)
		return false;
	glyph = s.substr(0, 3);
	name = s.substr(5);
	return true;
}

float EaseOut(float t)
{
	t = Clamp(t, 0.0f, 1.0f);
	const float u = 1.0f - t;
	return 1.0f - u * u * u;
}

// ---- the shapes (ui_mode) ----------------------------------------------------------------------------------------------------

// The frame's ranges: a new one whenever the texture changes.
void Use(InGameFrame& f, int image)
{
	if (!f.ranges.empty() && f.ranges.back().image == image)
		return;
	if (!f.ranges.empty() && f.ranges.back().first == f.ui.size())
	{
		f.ranges.back().image = image; // the last one is still empty
		return;
	}
	InGameRange r;
	r.first = static_cast<uint32_t>(f.ui.size());
	r.image = image;
	f.ranges.push_back(r);
}

void Seal(InGameFrame& f)
{
	for (size_t i = 0; i < f.ranges.size(); i++)
	{
		const uint32_t end = i + 1 < f.ranges.size() ? f.ranges[i + 1].first : static_cast<uint32_t>(f.ui.size());
		f.ranges[i].count = end - f.ranges[i].first;
	}
	f.ranges.erase(std::remove_if(f.ranges.begin(), f.ranges.end(), [](const InGameRange& r) { return r.count == 0; }), f.ranges.end());
}

// A quad: corners (x0, y0) .. (x1, y1), per-corner colours (top-left, top-right, bottom-right, bottom-left), u/v per corner.
void Quad(std::vector<UiVertex>& out, float x0, float y0, float x1, float y1, const uint32_t c[4], const float uv[8], float p0, float p1,
	float p2, float p3)
{
	const UiVertex a = {x0, y0, uv[0], uv[1], c[0], p0, p1, p2, p3};
	const UiVertex b = {x1, y0, uv[2], uv[3], c[1], p0, p1, p2, p3};
	const UiVertex cc = {x1, y1, uv[4], uv[5], c[2], p0, p1, p2, p3};
	const UiVertex d = {x0, y1, uv[6], uv[7], c[3], p0, p1, p2, p3};
	out.insert(out.end(), {a, b, cc, a, cc, d});
}

// A rounded rectangle whose colour runs from `top` to `bottom`.
void RectV(InGameFrame& f, float x, float y, float w, float h, float r, uint32_t top, uint32_t bottom)
{
	Use(f, static_cast<int>(InGameImage::Atlas));
	const float hw = w * 0.5f, hh = h * 0.5f;
	r = std::min(r, std::min(hw, hh));
	const uint32_t c[4] = {top, top, bottom, bottom};
	const float uv[8] = {-hw, -hh, hw, -hh, hw, hh, -hw, hh};
	Quad(f.ui, x, y, x + w, y + h, c, uv, hw, hh, r, ui_mode::Rect);
}

void Rect(InGameFrame& f, float x, float y, float w, float h, float r, uint32_t color)
{
	RectV(f, x, y, w, h, r, color, color);
}

// A rectangle whose colour runs from `left` to `right` (square corners).
void RectH(InGameFrame& f, float x, float y, float w, float h, uint32_t left, uint32_t right)
{
	Use(f, static_cast<int>(InGameImage::Atlas));
	const float hw = w * 0.5f, hh = h * 0.5f;
	const uint32_t c[4] = {left, right, right, left};
	const float uv[8] = {-hw, -hh, hw, -hh, hw, hh, -hw, hh};
	Quad(f.ui, x, y, x + w, y + h, c, uv, hw, hh, 0.0f, ui_mode::Rect);
}

// A glow or a shadow: the rounded rectangle (x, y, w, h) solid, fading to nothing over `soft` pixels around it.
void Soft(InGameFrame& f, float x, float y, float w, float h, float r, float soft, uint32_t color)
{
	Use(f, static_cast<int>(InGameImage::Atlas));
	soft = Clamp(soft, 1.0f, 4000.0f);
	const float hw = w * 0.5f, hh = h * 0.5f;
	r = std::min(r, std::min(hw, hh));
	const float ex = hw + soft, ey = hh + soft;
	const uint32_t c[4] = {color, color, color, color};
	const float uv[8] = {-ex, -ey, ex, -ey, ex, ey, -ex, ey};
	Quad(f.ui, x - soft, y - soft, x + w + soft, y + h + soft, c, uv, hw, hh, r, ui_mode::Soft + soft / 4096.0f);
}

// A rounded outline `t` pixels thick, inside the rectangle's edge.
void Outline(InGameFrame& f, float x, float y, float w, float h, float r, float t, uint32_t color)
{
	Use(f, static_cast<int>(InGameImage::Atlas));
	const float hw = w * 0.5f, hh = h * 0.5f;
	r = std::min(r, std::min(hw, hh));
	t = Clamp(t, 0.5f, 4000.0f);
	const uint32_t c[4] = {color, color, color, color};
	const float uv[8] = {-hw, -hh, hw, -hh, hw, hh, -hw, hh};
	Quad(f.ui, x, y, x + w, y + h, c, uv, hw, hh, r, ui_mode::Outline + t / 4096.0f);
}

// An image (`image`: InGameImage or a slot's) with rounded corners, tinted by `color` (its alpha fades it).
void Picture(InGameFrame& f, int image, float x, float y, float w, float h, float r, uint32_t color)
{
	Use(f, image);
	const float hw = w * 0.5f, hh = h * 0.5f;
	r = std::min(r, std::min(hw, hh));
	const uint32_t c[4] = {color, color, color, color};
	const float uv[8] = {0, 0, 1, 0, 1, 1, 0, 1};
	Quad(f.ui, x, y, x + w, y + h, c, uv, hw, hh, r, ui_mode::RoundImage);
}

float Text(InGameFrame& f, const Fonts& fonts, const char* s, float x, float base, float px, uint32_t color, float weight = 0.0f,
	Fonts::Align align = Fonts::Left)
{
	Use(f, static_cast<int>(InGameImage::Atlas));
	return fonts.AddText(f.ui, s, x, base, px, color, weight, align);
}

void FadeRange(std::vector<UiVertex>& ui, size_t begin, size_t end, float k)
{
	for (size_t i = begin; i < end; i++)
		ui[i].color = Faded(ui[i].color, k);
}

void Offset(std::vector<UiVertex>& ui, size_t begin, size_t end, float dx, float dy)
{
	for (size_t i = begin; i < end; i++)
	{
		ui[i].x += dx;
		ui[i].y += dy;
	}
}
} // namespace

// ---- setup -----------------------------------------------------------------------------------------------------------------------

std::vector<uint32_t> InGameMenu::IconCodepoints()
{
	return {kIconPlay, kIconSave, kIconLoad, kIconForward, kIconDisc, kIconPhone, kIconFlag, kIconHouse, kIconPause, kIconTrophy,
		kIconLock};
}

void InGameMenu::Configure(const Config& cfg)
{
	m_cfg = cfg;
}

void InGameMenu::SetSession(const InGameSession& s)
{
	m_s = s;
	m_disc_pick = 0;
	for (size_t i = 0; i < m_s.discs.size(); i++)
		if (m_s.discs[i] == m_s.disc)
			m_disc_pick = static_cast<int>(i);
	BuildActions();
	m_tab = Tab::Game;
	OpenSheet(!m_s.has_game);
	m_ach = GameAchievementsState();
	if (m_cfg.achievements)
		m_ach = m_cfg.achievements();
	m_ach_time = -10;
	m_ach_row = 0;
	m_ach_scroll = 0;
	m_ach_first = 0;
	m_ach_last = 9;
	for (BadgePic& b : m_badges)
		b = BadgePic(); // the host empties the pictures at each opening
	// The save slot: the first empty one, else the one saved longest ago. The load slot: the newest save.
	const int n = static_cast<int>(m_s.slots.size());
	m_save_slot = 1;
	m_load_slot = 1;
	int64_t newest = -1, oldest = INT64_MAX;
	int first_empty = 0, oldest_slot = 1;
	for (int i = 0; i < n; i++)
	{
		const InGameSlot& sl = m_s.slots[static_cast<size_t>(i)];
		if (!sl.used)
		{
			if (!first_empty)
				first_empty = i + 1;
			continue;
		}
		if (sl.mtime > newest)
		{
			newest = sl.mtime;
			m_load_slot = i + 1;
		}
		if (sl.mtime < oldest)
		{
			oldest = sl.mtime;
			oldest_slot = i + 1;
		}
	}
	m_save_slot = first_empty ? first_empty : oldest_slot;
}

void InGameMenu::BuildActions()
{
	m_actions.clear();
	m_actions.push_back({Action::Resume, Tr(Str::IngameResume)});
	m_actions.push_back({Action::Save, Tr(Str::IngameSaveState)});
	m_actions.push_back({Action::Load, Tr(Str::IngameLoadState)});
	m_actions.push_back({Action::Fast, Tr(Str::IngameFastForward)});
	if (m_s.discs.size() >= 2)
		m_actions.push_back({Action::Disc, Tr(Str::IngameChangeDisc)});
	m_actions.push_back({Action::Header, "PS5SX2"});
	m_actions.push_back({Action::Phone, Tr(Str::IngamePhone)});
	m_actions.push_back({Action::Report, Tr(Str::IngameReport)});
	m_actions.push_back({Action::Shelf, Tr(Str::IngameShelf)});
	m_action_row = 0;
}

void InGameMenu::OpenSheet(bool global)
{
	m_sheet_global = global || !m_s.has_game;
	m_sheet.SetGroupsLast({"Shelf", "Discs"});
	m_sheet.Open(m_cfg.paths, m_sheet_global ? nullptr : &m_s.game);
	m_sheet.SetTab(m_tab == Tab::Controls ? kTabControls : kTabSettings);
	m_sheet_open = true;
	m_sheet_row = 0;
	const auto& rows = m_sheet.rows();
	while (m_sheet_row < static_cast<int>(rows.size()) && !m_sheet.Selectable(rows[static_cast<size_t>(m_sheet_row)]))
		m_sheet_row++;
	m_sheet_scroll = m_sheet_scroll_target = 0;
	m_focus_y = -1;
}

void InGameMenu::Open(double now, uint32_t held)
{
	m_state = State::Opening;
	m_now = now;
	m_state_time = now;
	m_open = 0;
	m_ignore = held;
	m_prev = 0;
	m_tab = Tab::Game;
	m_tab_pos = 0;
	m_tab_time = -10;
	m_action_row = 0;
	m_armed_action = -1;
	m_status.clear();
	m_status_time = -10;
	m_busy = false;
	m_focus_y = -1;
	v_held = h_held = 0;
	{
		std::lock_guard<std::mutex> lock(m_req_mutex);
		m_requests.clear();
	}
}

void InGameMenu::Close()
{
	if (m_state == State::Closed || m_state == State::Closing)
		return;
	// From where the opening got to, if it is still opening.
	m_state = State::Closing;
	m_state_time = m_now - (1.0 - m_open) * kCloseSeconds;
}

bool InGameMenu::TakeRequest(InGameRequest& out)
{
	std::lock_guard<std::mutex> lock(m_req_mutex);
	if (m_requests.empty())
		return false;
	out = m_requests.front();
	m_requests.erase(m_requests.begin());
	return true;
}

void InGameMenu::Ask(const InGameRequest& r)
{
	std::lock_guard<std::mutex> lock(m_req_mutex);
	m_requests.push_back(r);
	m_busy = true;
}

void InGameMenu::Done(bool ok, const std::string& status, bool close)
{
	m_busy = false;
	if (!status.empty())
		SetStatus(status, ok);
	if (close)
		Close();
}

void InGameMenu::SlotSaved(int slot, int64_t utc)
{
	if (slot < 1 || slot > static_cast<int>(m_s.slots.size()))
		return;
	InGameSlot& s = m_s.slots[static_cast<size_t>(slot - 1)];
	s.used = true;
	s.mtime = utc;
	s.frame_shot = true;
	s.shot_w = s.shot_h = 0;
	m_load_slot = slot;
}

void InGameMenu::SlotShot(int slot, int w, int h)
{
	if (slot < 1 || slot > static_cast<int>(m_s.slots.size()))
		return;
	InGameSlot& s = m_s.slots[static_cast<size_t>(slot - 1)];
	if (s.frame_shot)
		return; // saved while the menu is open: the paused picture is the one
	s.shot_w = w;
	s.shot_h = h;
}

std::vector<InGameBadgeWant> InGameMenu::WantedBadges(double now) const
{
	std::vector<InGameBadgeWant> want;
	const int n = static_cast<int>(m_ach.entries.size());
	if (n == 0 || m_state == State::Closed || m_state == State::Closing)
		return want;
	const int lo = std::clamp(m_ach_first - 3, 0, n - 1), hi = std::clamp(m_ach_last + 3, lo, n - 1);
	const int focus = std::clamp(m_ach_row, lo, hi);
	auto consider = [&](int r) {
		const GameAchievement& a = m_ach.entries[static_cast<size_t>(r)];
		const int index = r % kInGameBadges;
		const BadgePic& b = m_badges[static_cast<size_t>(index)];
		if (b.asked && b.id == a.id && b.unlocked == a.unlocked && (b.loaded || b.never || now - b.tried < 1.0))
			return;
		for (const InGameBadgeWant& w : want)
			if (w.index == index)
				return;
		want.push_back({index, a.id, a.unlocked});
	};
	consider(focus);
	for (int d = 1; d <= hi - lo && want.size() < 4; d++)
	{
		if (focus + d <= hi)
			consider(focus + d);
		if (focus - d >= lo && want.size() < 4)
			consider(focus - d);
	}
	return want;
}

void InGameMenu::BadgeDone(const InGameBadgeWant& w, bool ok, bool never, double now)
{
	if (w.index < 0 || w.index >= kInGameBadges)
		return;
	BadgePic& b = m_badges[static_cast<size_t>(w.index)];
	b.asked = true;
	b.id = w.id;
	b.unlocked = w.unlocked;
	b.loaded = ok;
	b.never = !ok && never;
	b.tried = now;
}

void InGameMenu::SetStatus(const std::string& s, bool ok)
{
	m_status = s;
	m_status_ok = ok;
	m_status_time = m_now;
}

// ---- input -------------------------------------------------------------------------------------------------------------------

std::vector<InGameMenu::Tab> InGameMenu::Tabs() const
{
	std::vector<Tab> t = {Tab::Game, Tab::Settings, Tab::Controls};
	if (m_cfg.achievements)
		t.push_back(Tab::Achievements);
	return t;
}

int InGameMenu::TabIndex() const
{
	const std::vector<Tab> t = Tabs();
	for (size_t i = 0; i < t.size(); i++)
		if (t[i] == m_tab)
			return static_cast<int>(i);
	return 0;
}

void InGameMenu::SwitchTab(int dir)
{
	const std::vector<Tab> t = Tabs();
	const int i = std::clamp(TabIndex() + dir, 0, static_cast<int>(t.size()) - 1);
	if (t[static_cast<size_t>(i)] == m_tab)
		return;
	m_tab = t[static_cast<size_t>(i)];
	m_tab_time = m_now;
	m_focus_y = -1;
	m_status.clear();
	m_armed_action = -1;
	if (m_tab == Tab::Settings || m_tab == Tab::Controls)
	{
		m_sheet.SetTab(m_tab == Tab::Controls ? kTabControls : kTabSettings);
		m_sheet_row = 0;
		const auto& rows = m_sheet.rows();
		while (m_sheet_row < static_cast<int>(rows.size()) && !m_sheet.Selectable(rows[static_cast<size_t>(m_sheet_row)]))
			m_sheet_row++;
		m_sheet_scroll = m_sheet_scroll_target = 0;
	}
	if (m_tab == Tab::Achievements && m_cfg.achievements)
	{
		m_ach = m_cfg.achievements();
		m_ach_time = m_now;
	}
}

void InGameMenu::Update(double now, const InGamePad& pad)
{
	const double dt = std::max(0.0, now - m_now);
	m_now = now;
	if (m_state == State::Opening)
	{
		m_open = static_cast<float>(Clamp(static_cast<float>((now - m_state_time) / kOpenSeconds), 0.0f, 1.0f));
		if (m_open >= 1.0f)
			m_state = State::Open;
	}
	else if (m_state == State::Closing)
	{
		m_open = static_cast<float>(Clamp(1.0f - static_cast<float>((now - m_state_time) / kCloseSeconds), 0.0f, 1.0f));
		if (m_open <= 0.0f)
		{
			m_state = State::Closed;
			return;
		}
	}
	if (m_state == State::Closed)
		return;

	// The pills and the lists glide.
	const float a = 1.0f - std::exp(-static_cast<float>(dt) * 16.0f);
	m_tab_pos += (static_cast<float>(TabIndex()) - m_tab_pos) * a;
	if (m_focus_y >= 0 && m_focus_target >= 0)
	{
		m_focus_y += (m_focus_target - m_focus_y) * (1.0f - std::exp(-static_cast<float>(dt) * 22.0f));
		if (std::fabs(m_focus_target - m_focus_y) < 0.5f)
			m_focus_y = m_focus_target;
	}
	m_sheet_scroll += (m_sheet_scroll_target - m_sheet_scroll) * a;
	if (std::fabs(m_sheet_scroll_target - m_sheet_scroll) < 0.5f)
		m_sheet_scroll = m_sheet_scroll_target;
	if (m_tab == Tab::Achievements && m_cfg.achievements && now - m_ach_time > 2.0)
	{
		m_ach = m_cfg.achievements();
		m_ach_time = now;
	}

	if (m_state == State::Closing)
		return; // no buttons while it closes

	// The buttons that opened it count once they have been let go; the left stick works as the D-pad.
	m_ignore &= pad.held;
	uint32_t held = pad.held & ~m_ignore;
	if (pad.ly < 56)
		held |= ingame_pad::Up;
	else if (pad.ly > 200)
		held |= ingame_pad::Down;
	if (pad.lx < 56)
		held |= ingame_pad::Left;
	else if (pad.lx > 200)
		held |= ingame_pad::Right;
	const uint32_t pressed = ((pad.pressed & ~m_ignore) | (held & ~m_prev)) & held;
	m_prev = held;
	auto down = [&](uint32_t b) { return (pressed & b) != 0; };

	if (down(ingame_pad::Circle) || down(ingame_pad::Options))
	{
		Close();
		return;
	}
	if (down(ingame_pad::L2) || down(ingame_pad::R2))
	{
		SwitchTab(down(ingame_pad::R2) ? 1 : -1);
		return;
	}
	if ((m_tab == Tab::Settings || m_tab == Tab::Controls) && (down(ingame_pad::L1) || down(ingame_pad::R1)) && m_s.has_game)
	{
		const bool global = down(ingame_pad::R1);
		if (global != m_sheet_global)
		{
			OpenSheet(global);
			m_status.clear();
			m_tab_time = m_now; // the rows rise in, as for a tab
		}
		return;
	}

	// Up and down repeat while held (as the shelf's sheet: 0.34 s, then every 0.085 s); left and right too, slower.
	auto repeat = [&](int dir, bool fresh, int& held_dir, double& next, double first, double every) {
		if (dir == 0)
		{
			held_dir = 0;
			return 0;
		}
		if (fresh || dir != held_dir)
		{
			held_dir = dir;
			next = now + first;
			return dir;
		}
		if (now >= next)
		{
			next = std::max(next + every, now - every);
			return dir;
		}
		return 0;
	};
	const int vd = (held & ingame_pad::Up) ? -1 : (held & ingame_pad::Down) ? 1 : 0;
	const int hd = (held & ingame_pad::Left) ? -1 : (held & ingame_pad::Right) ? 1 : 0;
	const int v = repeat(vd, down(ingame_pad::Up | ingame_pad::Down), v_held, v_next, 0.34, 0.085);
	const int h = repeat(hd, down(ingame_pad::Left | ingame_pad::Right), h_held, h_next, 0.45, 0.12);

	InGamePad edge;
	edge.held = held;
	edge.pressed = pressed;
	if (m_tab == Tab::Game)
		UpdateGame(now, edge, v, h);
	else if (m_tab == Tab::Achievements)
		UpdateAchievements(now, v);
	else
		UpdateSheet(now, edge, v, h);
}

bool InGameMenu::MoveGame(int dir)
{
	int r = m_action_row;
	for (;;)
	{
		r += dir;
		if (r < 0 || r >= static_cast<int>(m_actions.size()))
			return false;
		if (m_actions[static_cast<size_t>(r)].action != Action::Header)
			break;
	}
	m_action_row = r;
	m_armed_action = -1;
	return true;
}

int InGameMenu::UsedSlotNear(int from, int dir) const
{
	const int n = static_cast<int>(m_s.slots.size());
	for (int s = from + dir; s >= 1 && s <= n; s += dir)
		if (m_s.slots[static_cast<size_t>(s - 1)].used)
			return s;
	return from;
}

void InGameMenu::UpdateGame(double now, const InGamePad& pad, int v, int h)
{
	if (v)
		MoveGame(v);
	if (m_actions.empty())
		return;
	const ActionRow& row = m_actions[static_cast<size_t>(m_action_row)];
	const int slots = static_cast<int>(m_s.slots.size());
	if (h)
	{
		switch (row.action)
		{
			case Action::Save:
				m_save_slot = std::clamp(m_save_slot + h, 1, std::max(1, slots));
				m_armed_action = -1;
				break;
			case Action::Load:
				m_load_slot = UsedSlotNear(m_load_slot, h);
				break;
			case Action::Disc:
				m_disc_pick = std::clamp(m_disc_pick + h, 0, std::max(0, static_cast<int>(m_s.discs.size()) - 1));
				break;
			case Action::Report:
				m_problem = (m_problem + h + 5) % 5;
				break;
			default:
				break;
		}
	}
	if (!(pad.pressed & ingame_pad::Cross) || m_busy)
		return;
	const bool armed = m_armed_action == m_action_row && now <= m_armed_until;
	auto arm = [&]() {
		m_armed_action = m_action_row;
		m_armed_until = now + 4.0;
	};
	InGameRequest r;
	switch (row.action)
	{
		case Action::Resume:
			Close();
			break;
		case Action::Save:
		{
			if (slots <= 0)
				break;
			const bool used = m_s.slots[static_cast<size_t>(m_save_slot - 1)].used;
			if (used && !armed)
			{
				arm();
				break;
			}
			m_armed_action = -1;
			r.kind = InGameRequest::Kind::SaveState;
			r.slot = m_save_slot;
			SetStatus(Format(Tr(Str::IngameSaving), m_save_slot), true);
			Ask(r);
			break;
		}
		case Action::Load:
		{
			if (m_s.hardcore || slots <= 0 || !m_s.slots[static_cast<size_t>(m_load_slot - 1)].used)
				break;
			r.kind = InGameRequest::Kind::LoadState;
			r.slot = m_load_slot;
			SetStatus(Format(Tr(Str::IngameLoading), m_load_slot), true);
			Ask(r);
			break;
		}
		case Action::Fast:
			r.kind = InGameRequest::Kind::FastForward;
			Ask(r);
			break;
		case Action::Disc:
			if (m_disc_pick >= 0 && m_disc_pick < static_cast<int>(m_s.discs.size()) && m_s.discs[static_cast<size_t>(m_disc_pick)] != m_s.disc)
			{
				r.kind = InGameRequest::Kind::ChangeDisc;
				r.text = m_s.discs[static_cast<size_t>(m_disc_pick)];
				Ask(r);
			}
			break;
		case Action::Report:
		{
			static const Str kinds[5] = {Str::IngameProblemGraphics, Str::IngameProblemSlow, Str::IngameProblemSound,
				Str::IngameProblemControls, Str::IngameProblemOther};
			r.kind = InGameRequest::Kind::Note;
			r.text = std::string("in-game menu: ") + Tr(kinds[m_problem]) + " (" + m_s.game.title +
			         (m_s.game.serial.empty() ? std::string() : ", " + m_s.game.serial) + ")";
			Ask(r);
			break;
		}
		case Action::Shelf:
			if (!armed)
			{
				arm();
				break;
			}
			m_armed_action = -1;
			r.kind = InGameRequest::Kind::BackToShelf;
			Ask(r);
			break;
		default:
			break;
	}
}

bool InGameMenu::MoveSheet(int dir)
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

void InGameMenu::UpdateSheet(double now, const InGamePad& pad, int v, int h)
{
	const auto& rows = m_sheet.rows();
	if (rows.empty())
		return;
	m_sheet_row = std::clamp(m_sheet_row, 0, static_cast<int>(rows.size()) - 1);
	if (v)
		MoveSheet(v);
	const OptionsSheet::Row row = rows[static_cast<size_t>(m_sheet_row)];
	const bool action = row.kind == OptionsSheet::Kind::Recommended || row.kind == OptionsSheet::Kind::ResetAll;
	auto after = [&](bool changed) {
		const bool restart = row.kind == OptionsSheet::Kind::Option && row.def && row.def->restart;
		if (changed)
		{
			const Str said = restart ? (m_sheet_global ? Str::IngameSavedAllNext : Str::IngameSavedThisNext) :
			                           (m_sheet_global ? Str::IngameSavedAll : Str::IngameSavedThis);
			SetStatus(Tr(said), true);
		}
		else if (!m_sheet.status().empty())
			SetStatus(m_sheet.status(), false);
		// A change can make the rows again (a cheat turned on, a card made): keep the focus on a row that is there.
		const auto& now_rows = m_sheet.rows();
		m_sheet_row = std::clamp(m_sheet_row, 0, std::max(0, static_cast<int>(now_rows.size()) - 1));
	};
	if (h && !action)
	{
		after(m_sheet.Step(row, h));
		return;
	}
	if (pad.pressed & ingame_pad::Cross)
	{
		after(action || row.kind == OptionsSheet::Kind::NewCard ? m_sheet.Activate(row, now) : m_sheet.Step(row, 1));
		return;
	}
	if (pad.pressed & ingame_pad::Triangle)
		after(m_sheet.Reset(row));
}

void InGameMenu::UpdateAchievements(double, int v)
{
	if (v)
		m_ach_row = std::clamp(m_ach_row + v, 0, std::max(0, static_cast<int>(m_ach.entries.size()) - 1));
}

// ---- texts ------------------------------------------------------------------------------------------------------------------

std::string InGameMenu::SlotValue(int slot) const
{
	std::string s = Format(Tr(Str::IngameSlot), slot) + "  \xC2\xB7  ";
	if (slot < 1 || slot > static_cast<int>(m_s.slots.size()) || !m_s.slots[static_cast<size_t>(slot - 1)].used)
		return s + Tr(Str::IngameEmpty);
	const InGameSlot& sl = m_s.slots[static_cast<size_t>(slot - 1)];
	if (sl.frame_shot)
		return s + Tr(Str::IngameJustNow);
	return s + (m_cfg.when ? m_cfg.when(sl.mtime) : std::to_string(sl.mtime));
}

std::string InGameMenu::ActionValue(const ActionRow& r) const
{
	const bool armed = m_armed_action >= 0 && m_actions[static_cast<size_t>(m_armed_action)].action == r.action && m_now <= m_armed_until;
	if (armed)
		return Tr(Str::IngamePressAgain);
	switch (r.action)
	{
		case Action::Save:
			return SlotValue(m_save_slot);
		case Action::Load:
		{
			if (m_s.hardcore)
				return Tr(Str::IngameHardcore);
			for (const InGameSlot& s : m_s.slots)
				if (s.used)
					return SlotValue(m_load_slot);
			return Tr(Str::IngameNoSaves);
		}
		case Action::Fast:
			return Tr(m_s.fast_forward ? Str::IngameOn : Str::IngameOff);
		case Action::Disc:
			return Format(Tr(Str::IngameDiscOf), m_disc_pick + 1, static_cast<int>(m_s.discs.size()));
		case Action::Phone:
			return m_s.web_address.empty() ? std::string(Tr(Str::IngameNoNetwork)) : m_s.web_address;
		case Action::Report:
		{
			static const Str kinds[5] = {Str::IngameProblemGraphics, Str::IngameProblemSlow, Str::IngameProblemSound,
				Str::IngameProblemControls, Str::IngameProblemOther};
			return Tr(kinds[m_problem]);
		}
		default:
			return {};
	}
}

std::string InGameMenu::ActionHelp(const ActionRow& r) const
{
	switch (r.action)
	{
		case Action::Resume: return Tr(Str::IngameHelpResume);
		case Action::Save: return Tr(Str::IngameHelpSave);
		case Action::Load: return Tr(m_s.hardcore ? Str::IngameHelpLoadHardcore : Str::IngameHelpLoad);
		case Action::Fast: return Tr(Str::IngameHelpFast);
		case Action::Disc: return Tr(Str::IngameHelpDisc);
		case Action::Phone: return Tr(m_s.web_address.empty() ? Str::IngameHelpPhoneNone : Str::IngameHelpPhone);
		case Action::Report: return Tr(Str::IngameHelpReport);
		case Action::Shelf: return Tr(Str::IngameHelpShelf);
		default: return {};
	}
}

std::string InGameMenu::PlayedText() const
{
	if (m_s.played_s < 0)
		return {};
	const int minutes = static_cast<int>(m_s.played_s / 60.0);
	const std::string t = minutes >= 60 ? Format("%d h %d min", minutes / 60, minutes % 60) : Format("%d min", std::max(1, minutes));
	return Format(Tr(Str::IngamePlayed), t.c_str());
}

// ---- drawing ------------------------------------------------------------------------------------------------------------------

void InGameMenu::Build(InGameFrame& out, float W, float H, const float frame[4])
{
	out.ui.clear();
	out.ranges.clear();
	if (m_state == State::Closed || !m_cfg.fonts)
		return;
	const float k = H / 2160.0f;
	const float e = EaseOut(m_open);

	// The background: the paused picture, small and blurred, darkened towards the shelf's night (over the present's black).
	Picture(out, static_cast<int>(InGameImage::Blur), 0, 0, W, H, 0, Rgba(0.40f, 0.37f, 0.52f, e));
	RectV(out, 0, 0, W, H * 0.40f, 0, Rgba(0.01f, 0.01f, 0.03f, 0.50f * e), Rgba(0.01f, 0.01f, 0.03f, 0.0f));
	RectV(out, 0, H * 0.55f, W, H * 0.45f, 0, Rgba(0.01f, 0.01f, 0.03f, 0.0f), Rgba(0.01f, 0.01f, 0.03f, 0.62f * e));
	RectH(out, 0, 0, W * 0.30f, H, Rgba(0.01f, 0.01f, 0.03f, 0.40f * e), Rgba(0.01f, 0.01f, 0.03f, 0.0f));
	RectH(out, W * 0.55f, 0, W * 0.45f, H, Rgba(0.01f, 0.01f, 0.03f, 0.0f), Rgba(0.01f, 0.01f, 0.03f, 0.45f * e));

	BuildLeft(out, W, H, k, frame, e);

	// The wordmark and the clock, as on the shelf.
	{
		const size_t begin = out.ui.size();
		const Fonts& f = *m_cfg.fonts;
		float x = kMargin * k;
		x += Text(out, f, "PS5", x, 150.0f * k, 60.0f * k, kHi, 0.8f);
		Text(out, f, "SX2", x + 2.0f * k, 150.0f * k, 60.0f * k, Mixed(0.3f), 0.8f);
		const std::string clock = m_cfg.clock ? m_cfg.clock() : std::string();
		if (!clock.empty())
			Text(out, f, clock.c_str(), W - kMargin * k, 150.0f * k, 58.0f * k, kHi, 0.2f, Fonts::Right);
		FadeRange(out.ui, begin, out.ui.size(), e);
	}

	BuildPanel(out, W, H, k, e);
	BuildHints(out, W, H, k, e);
	Seal(out);
}

void InGameMenu::BuildLeft(InGameFrame& out, float W, float H, float k, const float frame[4], float e)
{
	(void)H;
	const Fonts& f = *m_cfg.fonts;
	const float col_x0 = kMargin * k, col_x1 = W - (kMargin + kPanelW + kMargin) * k;
	const float fw = std::max(1.0f, frame[2] - frame[0]), fh = std::max(1.0f, frame[3] - frame[1]);
	const float aspect = fw / fh;
	// The card: the whole column for 16:9, no taller than 1120 (a 4:3 game's is narrower), its top on the panel's.
	float cw = col_x1 - col_x0, ch = cw / aspect;
	if (ch > 1120.0f * k)
	{
		ch = 1120.0f * k;
		cw = ch * aspect;
	}
	const float tx = col_x0, ty = kPanelTop * k;
	const float x0 = Mix(frame[0], tx, e), y0 = Mix(frame[1], ty, e);
	const float x1 = Mix(frame[2], tx + cw, e), y1 = Mix(frame[3], ty + ch, e);
	const float radius = 28.0f * k * e;

	Soft(out, x0 - 10 * k, y0 + 6 * k, (x1 - x0) + 20 * k, (y1 - y0) + 20 * k, 40 * k, 70 * k, Mixed(0.3f, 0.30f * e));
	Soft(out, x0, y0 + 26 * k, x1 - x0, y1 - y0, 30 * k, 30 * k, Rgba(0, 0, 0, 0.55f * e));
	Picture(out, static_cast<int>(InGameImage::Frame), x0, y0, x1 - x0, y1 - y0, radius, kHi);
	Outline(out, x0, y0, x1 - x0, y1 - y0, radius, 3 * k, Rgba(1, 1, 1, 0.22f * e));

	// The game, under the card.
	const float fade = Smoothstep(0.55f, 1.0f, e);
	if (fade <= 0.001f)
		return;
	const size_t begin = out.ui.size();
	const float bx = x0, base = y1 + 132 * k;
	const std::string title = Fit(f, m_s.game.title.empty() ? std::string("PlayStation 2") : m_s.game.title, 78 * k, cw);
	Text(out, f, title.c_str(), bx, base, 78 * k, kHi, 0.6f);
	std::string meta;
	auto add = [&](const std::string& s) {
		if (s.empty())
			return;
		if (!meta.empty())
			meta += "  \xC2\xB7  ";
		meta += s;
	};
	add(m_s.game.serial);
	add(m_s.game.region.empty() ? std::string() : Region(m_s.game.region));
	add(PlayedText());
	Text(out, f, Fit(f, meta, 38 * k, cw).c_str(), bx, base + 74 * k, 38 * k, kDim, 0.15f);
	float cx = bx;
	for (const std::string& chip : m_s.chips)
	{
		const float w = f.Measure(chip.c_str(), 30 * k) + 40 * k;
		Rect(out, cx, base + 112 * k, w, 54 * k, 14 * k, Rgba(1, 1, 1, 0.12f));
		Text(out, f, chip.c_str(), cx + w * 0.5f, base + 148 * k, 30 * k, kHi, 0.45f, Fonts::Center);
		cx += w + 16 * k;
	}
	// RetroAchievements: how far along.
	if (!m_ach.entries.empty())
	{
		int unlocked = 0, points = 0, total_points = 0;
		for (const GameAchievement& a : m_ach.entries)
		{
			total_points += static_cast<int>(a.points);
			if (a.unlocked)
			{
				unlocked++;
				points += static_cast<int>(a.points);
			}
		}
		const float py = base + 250 * k;
		float lx = bx;
		if (m_cfg.icon_font >= 0)
			lx += Text(out, f, Utf8(kIconTrophy).c_str(), bx, py, 34 * k, kGold) + 24 * k;
		const std::string line = Format(Tr(Str::IngameAchievementsCount), unlocked, static_cast<int>(m_ach.entries.size()), points, total_points);
		Text(out, f, Fit(f, line, 34 * k, cw - (lx - bx)).c_str(), lx, py, 34 * k, kMid, 0.15f);
		const float bw = std::min(760.0f * k, cw);
		Rect(out, bx, py + 30 * k, bw, 10 * k, 5 * k, Rgba(1, 1, 1, 0.10f));
		const float p = static_cast<float>(unlocked) / static_cast<float>(m_ach.entries.size());
		if (p > 0)
			Rect(out, bx, py + 30 * k, std::max(10 * k, bw * p), 10 * k, 5 * k, Mixed(0.3f));
	}
	FadeRange(out.ui, begin, out.ui.size(), fade);
}

void InGameMenu::BuildPanel(InGameFrame& out, float W, float H, float k, float e)
{
	const Fonts& f = *m_cfg.fonts;
	const float sw = kPanelW * k, top = kPanelTop * k, sh = H - top - kPanelBottomGap * k;
	const float margin = kMargin * k;
	const float x = W - margin - sw + (1.0f - e) * (sw + margin + 60.0f * k);
	const float y = top;
	const size_t begin = out.ui.size();

	// The panel: a soft shadow, a hairline edge, the body (a little of the blurred picture shows through).
	Soft(out, x - 6 * k, y + 24 * k, sw + 12 * k, sh + 12 * k, 48 * k, 46 * k, Rgba(0, 0, 0, 0.55f));
	Rect(out, x - 2 * k, y - 2 * k, sw + 4 * k, sh + 4 * k, 44 * k, Rgba(1, 1, 1, 0.14f));
	RectV(out, x, y, sw, sh, 42 * k, Rgba(0.060f, 0.062f, 0.128f, 0.93f), Rgba(0.036f, 0.040f, 0.088f, 0.95f));

	const float cx = x + 64 * k, inner = sw - 128 * k;
	// The title line: what the page is, and on the sheets this game / all games (L1 / R1).
	{
		std::string title;
		float tx = cx;
		switch (m_tab)
		{
			case Tab::Game:
				title = Tr(Str::IngamePaused);
				break;
			case Tab::Settings:
				title = Tr(m_sheet_global ? Str::IngameSettingsAll : Str::IngameSettingsThis);
				break;
			case Tab::Controls:
				title = Tr(m_sheet_global ? Str::IngameControlsAll : Str::IngameControlsThis);
				break;
			case Tab::Achievements:
				title = Tr(Str::Achievements);
				break;
		}
		float room = inner;
		if (m_tab == Tab::Game && m_cfg.icon_font >= 0)
		{
			tx += Text(out, f, Utf8(kIconPause).c_str(), cx, y + 112 * k, 40 * k, Mixed(0.3f)) + 26 * k;
			room -= tx - cx;
		}
		if (m_tab == Tab::Settings || m_tab == Tab::Controls)
		{
			const char* const this_label = Tr(Str::SheetThisGame);
			const char* const all_label = Tr(Str::SheetAllGames);
			const float ph = 60 * k, ppx = 30 * k, pad = 50 * k, pgap = 14 * k;
			const float this_w = f.Measure(this_label, ppx) + pad, all_w = f.Measure(all_label, ppx) + pad;
			const float pills_w = this_w + pgap + all_w;
			room -= pills_w + 48 * k;
			const float py = y + 66 * k;
			float px = cx + inner - pills_w;
			auto pill = [&](const char* label, float w, bool on, bool enabled) {
				Rect(out, px, py, w, ph, ph * 0.5f, on ? Rgba(1, 1, 1, 0.92f) : Rgba(1, 1, 1, enabled ? 0.10f : 0.04f));
				Text(out, f, label, px + w * 0.5f, py + ph * 0.5f + 11 * k, ppx, on ? kInk : (enabled ? kMid : kLo), 0.5f, Fonts::Center);
				px += w + pgap;
			};
			pill(this_label, this_w, !m_sheet_global, m_s.has_game);
			pill(all_label, all_w, m_sheet_global, true);
		}
		float px = 52 * k;
		while (px > 40 * k && f.Measure(title.c_str(), px) > room)
			px -= 2 * k;
		Text(out, f, Fit(f, title, px, room).c_str(), tx, y + 112 * k, px, kHi, 0.55f);
	}
	// The tab strip: [L2] Game | Settings | Controls | Achievements [R2], the shown one a white pill that glides.
	{
		const std::vector<Tab> tabs = Tabs();
		const float ty = y + 168 * k, th = 74 * k, kpx = 24 * k, tpx = 32 * k;
		Rect(out, cx, ty, inner, th, th * 0.5f, Rgba(1, 1, 1, 0.05f));
		const float kh = 40 * k;
		auto key_pill = [&](const char* name, float kx) {
			const float kw = f.Measure(name, kpx) + 20 * k;
			Rect(out, kx, ty + (th - kh) * 0.5f, kw, kh, 10 * k, Rgba(1, 1, 1, 0.82f));
			Text(out, f, name, kx + kw * 0.5f, ty + th * 0.5f + 9 * k, kpx, kInk, 0.6f, Fonts::Center);
			return kw;
		};
		const float l2 = key_pill("L2", cx + 16 * k);
		const float r2w = f.Measure("R2", kpx) + 20 * k;
		key_pill("R2", cx + inner - 16 * k - r2w);
		const float sx0 = cx + 16 * k + l2 + 14 * k, sx1 = cx + inner - 16 * k - r2w - 14 * k;
		const float seg = (sx1 - sx0) / static_cast<float>(tabs.size());
		Rect(out, sx0 + m_tab_pos * seg + 4 * k, ty + 7 * k, seg - 8 * k, th - 14 * k, (th - 14 * k) * 0.5f, Rgba(1, 1, 1, 0.92f));
		for (size_t t = 0; t < tabs.size(); t++)
		{
			const char* name = tabs[t] == Tab::Game ? Tr(Str::IngameTabGame) :
			                   tabs[t] == Tab::Settings ? Tr(Str::HintSettings) :
			                   tabs[t] == Tab::Controls ? Tr(Str::SheetControls) :
			                                              Tr(Str::Achievements);
			// The label's ink follows the pill as it passes over it.
			const float over = Clamp(1.0f - std::fabs(m_tab_pos - static_cast<float>(t)), 0.0f, 1.0f);
			const uint32_t ink = Rgba(Mix(0.74f, 0.07f, over), Mix(0.72f, 0.06f, over), Mix(0.84f, 0.16f, over));
			const float sx = sx0 + static_cast<float>(t) * seg;
			Text(out, f, Fit(f, name, tpx, seg - 36 * k).c_str(), sx + seg * 0.5f, ty + th * 0.5f + 11 * k, tpx, ink, Mix(0.3f, 0.5f, over),
				Fonts::Center);
		}
	}
	Rect(out, cx, y + 280 * k, inner, 2 * k, 0, Rgba(1, 1, 1, 0.12f));

	// The page: a short rise and fade when the tab changes.
	const size_t page_begin = out.ui.size();
	if (m_tab == Tab::Game)
		BuildGameRows(out, x, y, sw, sh, k);
	else if (m_tab == Tab::Achievements)
		BuildAchievementRows(out, x, y, sw, sh, k);
	else
		BuildSheetRows(out, x, y, sw, sh, k);
	const float since = static_cast<float>(m_now - m_tab_time);
	if (since < 0.22f)
	{
		const float p = EaseOut(since / 0.22f);
		Offset(out.ui, page_begin, out.ui.size(), 0, (1.0f - p) * 24 * k);
		FadeRange(out.ui, page_begin, out.ui.size(), p);
	}

	// The status line: what the last change or action did.
	if (!m_status.empty() && m_now - m_status_time < 3.2)
	{
		const float a = 1.0f - Smoothstep(2.4f, 3.2f, static_cast<float>(m_now - m_status_time));
		const uint32_t c = Faded(m_status_ok ? Mixed(0.45f) : kWarn, a);
		Text(out, f, Fit(f, m_status, 30 * k, inner).c_str(), cx, y + sh - 40 * k, 30 * k, c, 0.4f);
	}

	FadeRange(out.ui, begin, out.ui.size(), Clamp(e * 1.4f, 0.0f, 1.0f));
}

void InGameMenu::BuildGameRows(InGameFrame& out, float x, float y, float sw, float sh, float k)
{
	const Fonts& f = *m_cfg.fonts;
	const float cx = x + 64 * k, inner = sw - 128 * k;
	const float list_top = y + kListTop * k + 10 * k, list_bottom = y + sh - kHelpH * k;
	const uint32_t hi = kHi, mid = kMid, lo = kLo, accent = Mixed(0.3f);
	const float bx = x + 32 * k, bw = sw - 64 * k;
	float ry = list_top;
	float focus_target = -1;
	std::vector<float> ys(m_actions.size());
	for (size_t i = 0; i < m_actions.size(); i++)
	{
		ys[i] = ry;
		ry += (m_actions[i].action == Action::Header ? kHeaderH : kActionRowH) * k;
		if (static_cast<int>(i) == m_action_row)
			focus_target = ys[i];
	}
	const float bh = (kActionRowH - 10) * k;
	if (focus_target >= 0)
	{
		// The highlight glides to the focused row (Update moves it), as the shelf's selection does.
		m_focus_target = focus_target;
		if (m_focus_y < 0)
			m_focus_y = focus_target;
		const float fy = m_focus_y;
		Rect(out, bx, fy, bw, bh, 26 * k, Rgba(1, 1, 1, 0.11f));
		Rect(out, bx + 14 * k, fy + 24 * k, 6 * k, bh - 48 * k, 3 * k, accent);
	}
	for (size_t i = 0; i < m_actions.size(); i++)
	{
		const ActionRow& r = m_actions[i];
		const float y0 = ys[i];
		if (r.action == Action::Header)
		{
			Text(out, f, r.label.c_str(), cx, y0 + 54 * k, 26 * k, lo, 0.45f);
			continue;
		}
		const bool focused = static_cast<int>(i) == m_action_row;
		const float mid_y = y0 + bh * 0.5f + 13 * k;
		const bool disabled = (r.action == Action::Load && (m_s.hardcore || ActionValue(r) == Tr(Str::IngameNoSaves))) ||
		                      (r.action == Action::Phone && m_s.web_address.empty());
		float lx = bx + 48 * k;
		if (m_cfg.icon_font >= 0)
		{
			static const uint32_t icons[] = {0, kIconPlay, kIconSave, kIconLoad, kIconForward, kIconDisc, kIconPhone, kIconFlag, kIconHouse};
			const float icx = bx + 84 * k, icy = y0 + bh * 0.5f;
			const float rad = 31 * k;
			Rect(out, icx - rad, icy - rad, 2 * rad, 2 * rad, rad, focused ? Faded(accent, 0.95f) : Rgba(1, 1, 1, 0.07f));
			Text(out, f, Utf8(icons[static_cast<int>(r.action)]).c_str(), icx, icy + 11 * k, 28 * k, focused ? kInk : (disabled ? lo : mid), 0.0f,
				Fonts::Center);
			lx = bx + 140 * k;
		}
		Text(out, f, r.label.c_str(), lx, mid_y, 40 * k, disabled ? lo : hi, 0.25f);
		const std::string value = ActionValue(r);
		if (!value.empty())
		{
			const bool armed = value == Tr(Str::IngamePressAgain);
			bool arrows = focused && !armed;
			if (r.action == Action::Load)
			{
				const bool other_save = UsedSlotNear(m_load_slot, 1) != m_load_slot || UsedSlotNear(m_load_slot, -1) != m_load_slot;
				arrows = arrows && !disabled && other_save;
			}
			else if (r.action == Action::Disc)
				arrows = arrows && m_s.discs.size() > 1;
			else if (r.action != Action::Save && r.action != Action::Report)
				arrows = false;
			const float vpx = 38 * k;
			const float vx = bx + bw - 44 * k - (arrows ? 30 * k : 0.0f);
			const uint32_t vc = armed ? Mixed(0.45f) : (r.action == Action::Fast && m_s.fast_forward) ? Mixed(0.45f) : (disabled ? lo : mid);
			const std::string shown = Fit(f, value, vpx, bw * 0.52f);
			const float vw = Text(out, f, shown.c_str(), vx, mid_y, vpx, vc, 0.5f, Fonts::Right);
			if (arrows)
			{
				Text(out, f, "\xE2\x80\xB9", vx - vw - 26 * k, mid_y + 1 * k, 44 * k, hi, 0.4f, Fonts::Center);
				Text(out, f, "\xE2\x80\xBA", vx + 26 * k, mid_y + 1 * k, 44 * k, hi, 0.4f, Fonts::Center);
			}
		}
	}

	// The help box: what the focused row does; the slots under Save and Load, the code beside Settings on your phone.
	Rect(out, cx, list_bottom + 44 * k, inner, 2 * k, 0, Rgba(1, 1, 1, 0.12f));
	if (m_actions.empty())
		return;
	const ActionRow& focus = m_actions[static_cast<size_t>(m_action_row)];
	const std::string help = ActionHelp(focus);
	const bool strip = (focus.action == Action::Save || (focus.action == Action::Load && !m_s.hardcore)) && !m_s.slots.empty();
	const bool qr = focus.action == Action::Phone && m_s.qr_size > 0;
	const float qr_side = 250 * k;
	const float text_w = qr ? inner - qr_side - 48 * k : inner;
	float hy = list_bottom + 110 * k;
	for (const std::string& line : Wrap(f, help, 32 * k, text_w, strip ? 1 : 4))
	{
		Text(out, f, line.c_str(), cx, hy, 32 * k, mid, 0.1f);
		hy += 46 * k;
	}
	if (strip)
		BuildSlotStrip(out, cx, list_bottom + 110 * k + 26 * k, inner, k);
	if (qr)
		BuildQr(out, cx + inner - qr_side, list_bottom + 72 * k, qr_side, k);
}

void InGameMenu::BuildSlotStrip(InGameFrame& out, float x, float y, float w, float k)
{
	const Fonts& f = *m_cfg.fonts;
	const int n = static_cast<int>(m_s.slots.size());
	const ActionRow& focus = m_actions[static_cast<size_t>(m_action_row)];
	const int sel = focus.action == Action::Save ? m_save_slot : m_load_slot;
	const int shown = std::min(kSlotStrip, n);
	const int first = std::clamp(sel - shown / 2, 1, std::max(1, n - shown + 1));
	const float gap = 48 * k;
	const float tw = (w - gap * static_cast<float>(shown - 1)) / static_cast<float>(shown), th = tw * 9.0f / 16.0f;
	const uint32_t accent = Mixed(0.3f);
	for (int i = 0; i < shown; i++)
	{
		const int slot = first + i;
		const InGameSlot& s = m_s.slots[static_cast<size_t>(slot - 1)];
		const float sx = x + static_cast<float>(i) * (tw + gap);
		const bool on = slot == sel;
		if (s.used && s.frame_shot)
			Picture(out, static_cast<int>(InGameImage::Frame), sx, y, tw, th, 14 * k, kHi);
		else if (s.used && s.shot_w > 0 && s.shot_h > 0)
		{
			// Behind the screenshot, in case its texture isn't there yet: the tile.
			Rect(out, sx, y, tw, th, 14 * k, Rgba(1, 1, 1, 0.10f));
			Picture(out, static_cast<int>(InGameImage::Slot0) + slot - 1, sx, y, tw, th, 14 * k, kHi);
		}
		else
		{
			Rect(out, sx, y, tw, th, 14 * k, Rgba(1, 1, 1, s.used ? 0.10f : 0.05f));
			if (m_cfg.icon_font >= 0 && !s.used)
				Text(out, f, Utf8(kIconSave).c_str(), sx + tw * 0.5f, y + th * 0.5f + 14 * k, 38 * k, Faded(kLo, 0.55f), 0.0f, Fonts::Center);
		}
		if (on)
			Outline(out, sx - 6 * k, y - 6 * k, tw + 12 * k, th + 12 * k, 20 * k, 5 * k, accent);
		else
			Outline(out, sx, y, tw, th, 14 * k, 2 * k, Rgba(1, 1, 1, 0.14f));
		std::string label = std::to_string(slot) + "  \xC2\xB7  ";
		if (!s.used)
			label += Tr(Str::IngameEmpty);
		else if (s.frame_shot)
			label += Tr(Str::IngameJustNow);
		else
			label += m_cfg.when ? m_cfg.when(s.mtime) : std::to_string(s.mtime);
		Text(out, f, Fit(f, label, 26 * k, tw + gap - 8 * k).c_str(), sx + tw * 0.5f, y + th + 44 * k, 26 * k,
			on ? kHi : (s.used ? kMid : kLo), on ? 0.35f : 0.15f, Fonts::Center);
	}
}

void InGameMenu::BuildQr(InGameFrame& out, float x, float y, float side, float k)
{
	// Dark modules on a light tile, as cameras expect; runs of dark modules merged and grown a little (no seams), as the shelf's.
	const int quiet = 2;
	const int n = m_s.qr_size;
	const float mod = std::floor(side / static_cast<float>(n + 2 * quiet));
	const float s = mod * static_cast<float>(n + 2 * quiet);
	Rect(out, x, y, s, s, 14 * k, Rgba(0.86f, 0.85f, 0.90f));
	const uint32_t ink = Rgba(0.07f, 0.06f, 0.16f);
	const float grow = 0.4f;
	for (int my = 0; my < n; my++)
		for (int mx0 = 0; mx0 < n;)
		{
			if (!m_s.qr[static_cast<size_t>(my) * n + mx0])
			{
				mx0++;
				continue;
			}
			int mx1 = mx0;
			while (mx1 < n && m_s.qr[static_cast<size_t>(my) * n + mx1])
				mx1++;
			Rect(out, x + (mx0 + quiet) * mod - grow, y + (my + quiet) * mod - grow, (mx1 - mx0) * mod + 2 * grow, mod + 2 * grow, 0.0f, ink);
			mx0 = mx1;
		}
}

void InGameMenu::BuildSheetRows(InGameFrame& out, float x, float y, float sw, float sh, float k)
{
	const Fonts& f = *m_cfg.fonts;
	const float cx = x + 64 * k, inner = sw - 128 * k;
	const uint32_t hi = kHi, mid = kMid, lo = kLo, accent = Mixed(0.3f), own = Mixed(0.45f);
	const auto& rows = m_sheet.rows();
	const float list_top = y + kListTop * k, list_bottom = y + sh - kHelpH * k;
	const float view_h = (list_bottom - list_top) / k;
	std::vector<float> offs(rows.size());
	float acc = 0, focus_top = 0, focus_bottom = 0;
	for (size_t i = 0; i < rows.size(); i++)
	{
		offs[i] = acc;
		const float hgt = rows[i].kind == OptionsSheet::Kind::Header ? (rows[i].label.empty() ? 30.0f : kHeaderH) : kRowH;
		if (static_cast<int>(i) == m_sheet_row)
		{
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

	const float bx = x + 32 * k, bw = sw - 64 * k;
	if (m_sheet_row >= 0 && m_sheet_row < static_cast<int>(rows.size()))
	{
		const float rh = kRowH * k, bh = rh - 10 * k;
		const float target = list_top + (offs[static_cast<size_t>(m_sheet_row)] - m_sheet_scroll) * k;
		m_focus_target = target;
		if (m_focus_y < 0)
			m_focus_y = target;
		const float fy = m_focus_y;
		if (fy > list_top - 4 * k && fy + bh < list_bottom + 4 * k)
		{
			Rect(out, bx, fy, bw, bh, 26 * k, Rgba(1, 1, 1, 0.11f));
			Rect(out, bx + 14 * k, fy + 24 * k, 6 * k, bh - 48 * k, 3 * k, accent);
		}
	}
	for (size_t i = 0; i < rows.size(); i++)
	{
		const OptionsSheet::Row& r = rows[i];
		const float ry = list_top + (offs[i] - m_sheet_scroll) * k;
		const bool header = r.kind == OptionsSheet::Kind::Header;
		const float rh = (header ? (r.label.empty() ? 30.0f : kHeaderH) : kRowH) * k;
		if (ry < list_top - 2 * k || ry + rh > list_bottom + 2 * k)
			continue;
		if (header)
		{
			if (!r.label.empty())
			{
				std::string upper = r.label;
				for (char& c : upper)
					c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
				Text(out, f, upper.c_str(), cx, ry + 54 * k, 26 * k, lo, 0.45f);
			}
			continue;
		}
		const bool focused = static_cast<int>(i) == m_sheet_row;
		const float bh = rh - 10 * k;
		const float mid_y = ry + bh * 0.5f + 13 * k;
		const float lpx = 40 * k, vpx = 38 * k;
		const bool action = r.kind == OptionsSheet::Kind::Recommended || r.kind == OptionsSheet::Kind::ResetAll;
		std::string value = m_sheet.Value(r);
		const bool armed = m_sheet.Armed(r, m_now);
		if (armed)
			value = Tr(Str::IngamePressAgain);
		else if (action && r.kind == OptionsSheet::Kind::ResetAll)
			value.clear();
		if (r.kind == OptionsSheet::Kind::NewCard && focused)
			value += "  \xC2\xB7  Create";
		const bool arrows = focused && !action && r.kind != OptionsSheet::Kind::NewCard;
		const bool restart = r.kind == OptionsSheet::Kind::Option && r.def && r.def->restart;
		std::string lglyph, lname, vglyph, vname;
		const bool lsym = SplitSymbol(r.label, lglyph, lname), vsym = SplitSymbol(value, vglyph, vname);
		const float slot = lsym ? 66 * k : 0.0f;
		const float value_room = bw * 0.46f;
		const float tag_w = restart ? f.Measure(Tr(Str::IngameNextStart), 24 * k) + 28 * k + 24 * k : 0.0f;
		const std::string label = Fit(f, lsym ? lname : r.label, lpx,
			bw - 80 * k - slot - tag_w - (value.empty() ? 0.0f : std::min(value_room, f.Measure(value.c_str(), vpx)) + 90 * k));
		const uint32_t lc = action ? (focused ? hi : mid) : hi;
		if (lsym && lglyph != icon::Blank)
			Text(out, f, lglyph.c_str(), bx + 48 * k + slot * 0.42f, mid_y + kSymbolDrop * lpx, lpx * kSymbolScale, lc, 0.25f, Fonts::Center);
		Text(out, f, label.c_str(), bx + 48 * k + slot, mid_y, lpx, lc, 0.25f);
		if (value.empty())
			continue;
		const std::string shown = Fit(f, vsym ? vname : value, vpx, value_room - (vsym ? 64 * k : 0.0f));
		const OptionsSheet::From from = m_sheet.Source(r);
		uint32_t vc = from == OptionsSheet::From::Own ? own : mid;
		if (armed || (action && focused))
			vc = own;
		const float vx = bx + bw - 44 * k - (arrows ? 30 * k : 0.0f);
		float vw = Text(out, f, shown.c_str(), vx, mid_y, vpx, vc, 0.5f, Fonts::Right);
		if (vsym && vglyph != icon::Blank)
			vw += 14 * k + Text(out, f, vglyph.c_str(), vx - vw - 14 * k, mid_y + kSymbolDrop * vpx, vpx * kSymbolScale, vc, 0.5f, Fonts::Right);
		if (arrows)
		{
			Text(out, f, "\xE2\x80\xB9", vx - vw - 26 * k, mid_y + 1 * k, 44 * k, hi, 0.4f, Fonts::Center);
			Text(out, f, "\xE2\x80\xBA", vx + 26 * k, mid_y + 1 * k, 44 * k, hi, 0.4f, Fonts::Center);
		}
		float left_of_value = vx - vw - (arrows ? 60 * k : 30 * k);
		if (from == OptionsSheet::From::Own && !action && !focused)
		{
			Rect(out, vx - vw - 30 * k, mid_y - 18 * k, 12 * k, 12 * k, 6 * k, own);
			left_of_value = vx - vw - 52 * k;
		}
		if (restart)
		{
			// "next start": a row read when a game starts (a change shows the next time).
			const float tw = f.Measure(Tr(Str::IngameNextStart), 24 * k) + 28 * k;
			const float tx = left_of_value - tw;
			Outline(out, tx, mid_y - 34 * k, tw, 44 * k, 22 * k, 2 * k, Faded(kAmber, 0.85f));
			Text(out, f, Tr(Str::IngameNextStart), tx + tw * 0.5f, mid_y - 3 * k, 24 * k, kAmber, 0.35f, Fonts::Center);
		}
	}
	if (m_sheet_scroll > 4.0f)
		Text(out, f, "\xE2\x80\xA2 \xE2\x80\xA2 \xE2\x80\xA2", x + sw * 0.5f, list_top - 10 * k, 22 * k, lo, 0.3f, Fonts::Center);
	if (m_sheet_scroll + view_h < total - 4.0f)
		Text(out, f, "\xE2\x80\xA2 \xE2\x80\xA2 \xE2\x80\xA2", x + sw * 0.5f, list_bottom + 22 * k, 22 * k, lo, 0.3f, Fonts::Center);

	Rect(out, cx, list_bottom + 44 * k, inner, 2 * k, 0, Rgba(1, 1, 1, 0.12f));
	if (m_sheet_row >= 0 && m_sheet_row < static_cast<int>(rows.size()))
	{
		const OptionsSheet::Row& r = rows[static_cast<size_t>(m_sheet_row)];
		std::string help = m_sheet.Help(r);
		const bool restart = r.kind == OptionsSheet::Kind::Option && r.def && r.def->restart;
		if ((r.kind == OptionsSheet::Kind::Option && !restart) || r.kind == OptionsSheet::Kind::Patch || r.kind == OptionsSheet::Kind::Cheat)
			help += std::string(" ") + Tr(Str::IngameAppliesResume);
		float hy = list_bottom + 110 * k;
		for (const std::string& line : Wrap(f, help, 32 * k, inner, 4))
		{
			Text(out, f, line.c_str(), cx, hy, 32 * k, mid, 0.1f);
			hy += 46 * k;
		}
	}
}

void InGameMenu::BuildAchievementRows(InGameFrame& out, float x, float y, float sw, float sh, float k)
{
	const Fonts& f = *m_cfg.fonts;
	const float cx = x + 64 * k, inner = sw - 128 * k;
	const float list_top = y + kListTop * k, list_bottom = y + sh - 120 * k;
	const float bx = x + 32 * k, bw = sw - 64 * k;
	if (m_ach.entries.empty())
	{
		const char* msg = m_ach.busy ? Tr(Str::IngameAchievementsLoading) : Tr(Str::IngameAchievementsNone);
		float hy = list_top + 80 * k;
		for (const std::string& line : Wrap(f, m_ach.message.empty() ? msg : m_ach.message, 34 * k, inner, 5))
		{
			Text(out, f, line.c_str(), cx, hy, 34 * k, kMid, 0.15f);
			hy += 50 * k;
		}
		return;
	}
	const float view = list_bottom - list_top;
	const float rh = kAchievementRowH * k;
	const float want = static_cast<float>(m_ach_row) * rh;
	if (want - m_ach_scroll > view - rh * 1.5f)
		m_ach_scroll = want - view + rh * 1.5f;
	if (want - m_ach_scroll < rh * 0.5f)
		m_ach_scroll = std::max(0.0f, want - rh * 0.5f);
	int first_shown = -1, last_shown = -1;
	for (size_t i = 0; i < m_ach.entries.size(); i++)
	{
		const GameAchievement& a = m_ach.entries[i];
		const float ry = list_top + static_cast<float>(i) * rh - m_ach_scroll;
		if (ry < list_top - 2 * k || ry + rh > list_bottom + 2 * k)
			continue;
		if (first_shown < 0)
			first_shown = static_cast<int>(i);
		last_shown = static_cast<int>(i);
		const bool focused = static_cast<int>(i) == m_ach_row;
		const float bh = rh - 10 * k;
		if (focused)
		{
			Rect(out, bx, ry, bw, bh, 26 * k, Rgba(1, 1, 1, 0.11f));
			Rect(out, bx + 14 * k, ry + 24 * k, 6 * k, bh - 48 * k, 3 * k, Mixed(0.3f));
		}
		float lx = bx + 48 * k;
		// Build 190: the achievement's own badge (RetroAchievements' picture, its grey one while locked) once the host has it in
		// the row's picture; the trophy or lock icon until then (and when it can't be had).
		const int pic = static_cast<int>(i % static_cast<size_t>(kInGameBadges));
		const BadgePic& bp = m_badges[static_cast<size_t>(pic)];
		const bool art = bp.loaded && bp.id == a.id && bp.unlocked == a.unlocked;
		const float side = 92 * k, tx = bx + 50 * k, ty = ry + (bh - side) * 0.5f;
		if (art)
		{
			Rect(out, tx, ty, side, side, 16 * k, Rgba(1, 1, 1, 0.06f)); // behind it, until its texture is there
			Picture(out, static_cast<int>(InGameImage::Slot0) + kInGameSaveSlots + pic, tx, ty, side, side, 16 * k,
				a.unlocked ? kHi : Rgba(0.80f, 0.80f, 0.84f));
			if (a.unlocked)
				Outline(out, tx - 4 * k, ty - 4 * k, side + 8 * k, side + 8 * k, 20 * k, 3 * k, Faded(kGold, 0.85f));
			else
				Outline(out, tx, ty, side, side, 16 * k, 2 * k, Rgba(1, 1, 1, 0.12f));
			lx = tx + side + 32 * k;
		}
		else if (m_cfg.icon_font >= 0)
		{
			Rect(out, tx, ty, side, side, 16 * k, a.unlocked ? Faded(kGold, 0.22f) : Rgba(1, 1, 1, 0.06f));
			Text(out, f, Utf8(a.unlocked ? kIconTrophy : kIconLock).c_str(), tx + side * 0.5f, ty + side * 0.5f + 11 * k, 30 * k,
				a.unlocked ? kGold : kLo, 0.0f, Fonts::Center);
			lx = tx + side + 32 * k;
		}
		const std::string pts = std::to_string(a.points);
		const float pw = Text(out, f, pts.c_str(), bx + bw - 44 * k, ry + bh * 0.5f + 13 * k, 38 * k, a.unlocked ? Mixed(0.45f) : kMid, 0.5f,
			Fonts::Right);
		const float room = bx + bw - 44 * k - pw - 40 * k - lx;
		Text(out, f, Fit(f, a.title, 38 * k, room).c_str(), lx, ry + 52 * k, 38 * k, a.unlocked ? kHi : kMid, 0.25f);
		Text(out, f, Fit(f, a.description, 28 * k, room).c_str(), lx, ry + 94 * k, 28 * k, kLo, 0.1f);
	}
	if (first_shown >= 0)
	{
		m_ach_first = first_shown;
		m_ach_last = last_shown;
	}
	if (m_ach_scroll > 4.0f)
		Text(out, f, "\xE2\x80\xA2 \xE2\x80\xA2 \xE2\x80\xA2", x + sw * 0.5f, list_top - 10 * k, 22 * k, kLo, 0.3f, Fonts::Center);
	if (m_ach_scroll + view < static_cast<float>(m_ach.entries.size()) * rh - 4.0f)
		Text(out, f, "\xE2\x80\xA2 \xE2\x80\xA2 \xE2\x80\xA2", x + sw * 0.5f, list_bottom + 22 * k, 22 * k, kLo, 0.3f, Fonts::Center);
}

void InGameMenu::BuildHints(InGameFrame& out, float W, float H, float k, float e)
{
	(void)W;
	const Fonts& f = *m_cfg.fonts;
	const size_t begin = out.ui.size();
	const float hy = H - 88.0f * k, ipx = 64.0f * k, tpx = 40.0f * k;
	float hx = kMargin * k;
	auto key = [&](const char* name) {
		if (name[0] != '#')
		{
			hx += Text(out, f, name, hx, hy + 6.0f * k, ipx, kHi);
			return;
		}
		const float kpx = 30.0f * k, kw = f.Measure(name + 1, kpx) + 26.0f * k, kh = 44.0f * k;
		Rect(out, hx, hy - 36.0f * k, kw, kh, 12.0f * k, Rgba(1, 1, 1, 0.9f));
		Text(out, f, name + 1, hx + kw * 0.5f, hy - 4.0f * k, kpx, kInk, 0.6f, Fonts::Center);
		hx += kw;
	};
	auto hint = [&](const char* a, const char* b, const char* label) {
		key(a);
		if (b)
		{
			hx += 8.0f * k;
			key(b);
		}
		hx += 16.0f * k;
		hx += Text(out, f, label, hx, hy, tpx, kDim, 0.1f);
		hx += 56.0f * k;
	};
	hint(icon::DpadUpDown, nullptr, Tr(Str::HintMove));
	if (m_tab == Tab::Game && !m_actions.empty())
	{
		const Action a = m_actions[static_cast<size_t>(m_action_row)].action;
		if (a == Action::Save || a == Action::Load)
			hint(icon::DpadLeftRight, nullptr, Tr(Str::IngameHintSlot));
		else if (a == Action::Disc || a == Action::Report)
			hint(icon::DpadLeftRight, nullptr, Tr(Str::IngameHintChoose));
		const char* cross = a == Action::Save ? Tr(Str::IngameHintSave) : a == Action::Load ? Tr(Str::IngameHintLoad) : Tr(Str::HintSelect);
		hint(icon::Cross, nullptr, cross);
	}
	else if (m_tab == Tab::Settings || m_tab == Tab::Controls)
	{
		hint(icon::DpadLeftRight, nullptr, Tr(Str::HintChange));
		hint(icon::Triangle, nullptr, Tr(Str::HintReset));
	}
	hint(icon::Circle, nullptr, Tr(Str::IngameHintResume));
	if ((m_tab == Tab::Settings || m_tab == Tab::Controls) && m_s.has_game)
		hint("#L1", "#R1", Tr(Str::IngameHintScope));
	hint("#L2", "#R2", Tr(Str::IngameHintTabs));
	FadeRange(out.ui, begin, out.ui.size(), e);
}

void InGameMenu::BuildHold(const Fonts& fonts, InGameFrame& out, float W, float H, float progress)
{
	out.ui.clear();
	out.ranges.clear();
	if (progress <= 0.0f)
		return;
	const float k = H / 2160.0f;
	// Fades in over the first part of the hold (a quick press of the buttons in a game shows nothing).
	const float a = Smoothstep(0.0f, 0.25f, progress);
	const char* label = Tr(Str::IngameHoldMenu);
	const float px = 34 * k;
	const float pw = fonts.Measure(label, px) + 150 * k, ph = 76 * k;
	const float x = W - kMargin * k - pw, y = H - 150 * k - ph;
	Soft(out, x, y + 10 * k, pw, ph, ph * 0.5f, 24 * k, Rgba(0, 0, 0, 0.45f * a));
	Rect(out, x, y, pw, ph, ph * 0.5f, Rgba(0.045f, 0.050f, 0.105f, 0.88f * a));
	// The fill: the shelf's colour, from the left.
	const float fill = Clamp(progress, 0.0f, 1.0f);
	if (fill > 0.02f)
		Rect(out, x + 6 * k, y + 6 * k, std::max(ph - 12 * k, (pw - 12 * k) * fill), ph - 12 * k, (ph - 12 * k) * 0.5f, Mixed(0.3f, 0.55f * a));
	Outline(out, x, y, pw, ph, ph * 0.5f, 2 * k, Rgba(1, 1, 1, 0.20f * a));
	// Two bars: pause.
	const float bx = x + 44 * k, by = y + ph * 0.5f - 15 * k;
	Rect(out, bx, by, 9 * k, 30 * k, 3 * k, Rgba(1, 1, 1, a));
	Rect(out, bx + 16 * k, by, 9 * k, 30 * k, 3 * k, Rgba(1, 1, 1, a));
	Text(out, fonts, label, x + 96 * k, y + ph * 0.5f + 12 * k, px, Rgba(1, 1, 1, a), 0.45f);
	Seal(out);
}
} // namespace fe
