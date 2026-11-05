// PS5 port (2.31, AI-assisted): what the in-game menu (ps5/frontend/fe_ingame.h) hands the GS, and back.
//
// Three threads meet here. The VM thread runs the menu while the game is paused for it (main-boot.cpp orbis_ingame_menu_run):
// it publishes each frame's vertices (fe::UiVertex, fe::InGameRange: the same layouts as below), the font atlas and the save
// states' screenshots. The GS thread draws the newest frame at each present while the menu is up (GSRenderer::
// PresentCurrentFrame, with MTGS's idle presents while the VM is paused) and says where it draws the game's picture without
// the menu (the card's start). The pad thread says how far the menu's buttons are held in the game (the hold ring, drawn by
// the GS over the game). Copies under a mutex; no frame is shared while it is written.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace orbis_ui
{
// fe::UiVertex (ps5/frontend/fe_text.h): pixels, uv, RGBA8 (R low), the shape's four parameters. 36 bytes.
struct Vertex
{
	float x, y, u, v;
	uint32_t color;
	float p0, p1, p2, p3;
};
static_assert(sizeof(Vertex) == 36, "fe::UiVertex's layout");

// fe::InGameRange: vertices [first, first + count) drawn with texture `image`.
struct Range
{
	uint32_t first, count;
	int image;
};

// One run of the GS's UI draw (GSDeviceVK.cpp OrbisVkDrawUi): vertices [first, first + count) with texture `tex` (a GSTexture*),
// u, v 0..1 mapped to uv[0..1] * (u, v) + uv[2..3] (the paused picture's place in the GS's frame texture; 1, 1, 0, 0 otherwise).
struct DrawRun
{
	uint32_t first, count;
	void* tex;
	float uv[4];
};

// fe::InGameImage.
enum : int
{
	kAtlas = 0, // the font atlas (R8)
	kBlur = 1,  // the paused picture, small and blurred
	kFrame = 2, // the paused picture (u, v 0..1 over the game's picture)
	kSlot0 = 3, // + i: picture i: save state slot i + 1's screenshot (i < kSlots), then achievement badge i - kSlots (build 190)
};
constexpr int kSlots = 10;                   // the save states' screenshots: pictures 0 .. kSlots-1
constexpr int kBadges = 24;                  // the Achievements tab's badges: pictures kSlots .. kPictures-1 (fe::kInGameBadges)
constexpr int kPictures = kSlots + kBadges;

// ---- the menu: VM thread -> GS thread
// Begin: the menu is about to be drawn (the GS makes the blurred picture once per Begin). End: no menu (the game is drawn again).
void Begin();
void End();
bool Active();
uint32_t Session();
// The newest frame of the menu.
void Publish(const Vertex* v, size_t count, const Range* r, size_t ranges);
// GS: copies the newest frame; false when there is none.
bool Latest(std::vector<Vertex>& v, std::vector<Range>& r);

// ---- the font atlas (R8), set by the frontend's glue (fe_ps5.cpp); the GS uploads it again when the version changes.
void SetAtlas(const uint8_t* pixels, int w, int h, uint32_t version);
uint32_t AtlasVersion(); // 0: none yet
bool CopyAtlas(std::vector<uint8_t>& out, int& w, int& h, uint32_t& version);

// ---- the pictures, RGBA8 (R low), index 0..kPictures-1 (empty: none): the save states' screenshots (0..kSlots-1), then the
// achievements' badges (kSlots + the menu's badge index). Each change counts up a serial.
void SetSlotShot(int index, int w, int h, const std::vector<uint32_t>& rgba);
uint32_t SlotSerial(int index);
bool CopySlotShot(int index, int& w, int& h, std::vector<uint32_t>& rgba, uint32_t& serial);

// ---- GS thread -> VM thread: where the present draws the game's picture without the menu, and the window's size.
void SetPresent(float x0, float y0, float x1, float y1, float w, float h);
void GetPresent(float rect[4], float& w, float& h);

// ---- the hold ring: the pad thread says how far (0..1), the GS draws it over the game with the builder the frontend set.
void SetHold(float progress);
float Hold();
using HoldBuilder = void (*)(float progress, float w, float h, std::vector<Vertex>& v, std::vector<Range>& r);
void SetHoldBuilder(HoldBuilder b);
HoldBuilder GetHoldBuilder();
} // namespace orbis_ui
