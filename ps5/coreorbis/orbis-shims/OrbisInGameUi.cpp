// PS5 port (2.31, AI-assisted): see OrbisInGameUi.h.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisInGameUi.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace orbis_ui
{
namespace
{
std::mutex s_mutex;
std::vector<Vertex> s_verts;
std::vector<Range> s_ranges;
bool s_have_frame = false;
std::atomic<bool> s_active{false};
std::atomic<uint32_t> s_session{0};

std::vector<uint8_t> s_atlas;
int s_atlas_w = 0, s_atlas_h = 0;
std::atomic<uint32_t> s_atlas_version{0};

struct Shot
{
	int w = 0, h = 0;
	std::vector<uint32_t> rgba;
	uint32_t serial = 0;
};
Shot s_shots[kPictures];

std::atomic<float> s_present[6] = {};
std::atomic<float> s_hold{0.0f};
std::atomic<HoldBuilder> s_hold_builder{nullptr};
} // namespace

void Begin()
{
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		s_verts.clear();
		s_ranges.clear();
		s_have_frame = false;
	}
	s_session.fetch_add(1, std::memory_order_acq_rel);
	s_active.store(true, std::memory_order_release);
}

void End()
{
	s_active.store(false, std::memory_order_release);
	std::lock_guard<std::mutex> lock(s_mutex);
	s_verts.clear();
	s_ranges.clear();
	s_have_frame = false;
}

bool Active()
{
	return s_active.load(std::memory_order_acquire);
}

uint32_t Session()
{
	return s_session.load(std::memory_order_acquire);
}

void Publish(const Vertex* v, size_t count, const Range* r, size_t ranges)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	s_verts.assign(v, v + count);
	s_ranges.assign(r, r + ranges);
	s_have_frame = true;
}

bool Latest(std::vector<Vertex>& v, std::vector<Range>& r)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	if (!s_have_frame)
		return false;
	v = s_verts;
	r = s_ranges;
	return true;
}

void SetAtlas(const uint8_t* pixels, int w, int h, uint32_t version)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	s_atlas.assign(pixels, pixels + static_cast<size_t>(w) * h);
	s_atlas_w = w;
	s_atlas_h = h;
	s_atlas_version.store(version ? version : 1, std::memory_order_release);
}

uint32_t AtlasVersion()
{
	return s_atlas_version.load(std::memory_order_acquire);
}

bool CopyAtlas(std::vector<uint8_t>& out, int& w, int& h, uint32_t& version)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	if (s_atlas.empty())
		return false;
	out = s_atlas;
	w = s_atlas_w;
	h = s_atlas_h;
	version = s_atlas_version.load(std::memory_order_relaxed);
	return true;
}

void SetSlotShot(int index, int w, int h, const std::vector<uint32_t>& rgba)
{
	if (index < 0 || index >= kPictures)
		return;
	std::lock_guard<std::mutex> lock(s_mutex);
	Shot& s = s_shots[index];
	const bool ok = w > 0 && h > 0 && rgba.size() >= static_cast<size_t>(w) * h;
	s.w = ok ? w : 0;
	s.h = ok ? h : 0;
	s.rgba = ok ? rgba : std::vector<uint32_t>();
	s.serial++;
}

uint32_t SlotSerial(int index)
{
	if (index < 0 || index >= kPictures)
		return 0;
	std::lock_guard<std::mutex> lock(s_mutex);
	return s_shots[index].serial;
}

bool CopySlotShot(int index, int& w, int& h, std::vector<uint32_t>& rgba, uint32_t& serial)
{
	if (index < 0 || index >= kPictures)
		return false;
	std::lock_guard<std::mutex> lock(s_mutex);
	const Shot& s = s_shots[index];
	serial = s.serial;
	if (s.rgba.empty())
		return false;
	w = s.w;
	h = s.h;
	rgba = s.rgba;
	return true;
}

void SetPresent(float x0, float y0, float x1, float y1, float w, float h)
{
	const float v[6] = {x0, y0, x1, y1, w, h};
	for (int i = 0; i < 6; i++)
		s_present[i].store(v[i], std::memory_order_relaxed);
}

void GetPresent(float rect[4], float& w, float& h)
{
	for (int i = 0; i < 4; i++)
		rect[i] = s_present[i].load(std::memory_order_relaxed);
	w = s_present[4].load(std::memory_order_relaxed);
	h = s_present[5].load(std::memory_order_relaxed);
}

void SetHold(float progress)
{
	s_hold.store(progress, std::memory_order_relaxed);
}

float Hold()
{
	return s_hold.load(std::memory_order_relaxed);
}

void SetHoldBuilder(HoldBuilder b)
{
	s_hold_builder.store(b, std::memory_order_release);
}

HoldBuilder GetHoldBuilder()
{
	return s_hold_builder.load(std::memory_order_acquire);
}
} // namespace orbis_ui
