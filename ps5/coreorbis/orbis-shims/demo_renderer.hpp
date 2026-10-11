/*
 * ps5-native-app-boilerplate - Small CPU-rendered demonstration API.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Keeps PS5 VideoOut setup and the bitmap font out of the starter main file.
 */

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace ps5::demo
{
enum class Color : std::uint32_t
{
    background = UINT32_C(0xff190d0a),
    panel = UINT32_C(0xff301f17),
    white = UINT32_C(0xffffffff),
    cyan = UINT32_C(0xffffff00),
    magenta = UINT32_C(0xffff00ff),
    yellow = UINT32_C(0xff00ffff),
};

class Canvas;
using DrawScene = void (*)(Canvas &) noexcept;
[[noreturn]] void run(DrawScene draw, std::string_view ready_message) noexcept;

// vk-285-30: the game selector's screen, before the emulator owns the display. open() takes the
// VideoOut OVERLAY bus and two 1920x1080 buffers; present() draws the back buffer with `draw`,
// flips to it and waits for the vblank; close() shows a transparent frame, closes the bus and gives
// the memory back, so the Vulkan device can open the MAIN bus as before.
using MenuScene = void (*)(Canvas &, const void *user) noexcept;
class MenuDisplay final
{
  public:
    bool open() noexcept;
    void present(MenuScene draw, const void *user) noexcept;
    void close() noexcept;

  private:
    int video_ = -1;
    void *mapped_ = nullptr;
    std::int64_t physical_ = 0;
    bool allocated_ = false;
    bool registered_ = false;
    int back_ = 0;
};

// vk-285-30: the width label() gives `value` at `scale`.
unsigned label_width(std::string_view value, unsigned scale) noexcept;

class Canvas final
{
  public:
    void clear(Color color) noexcept;
    void rectangle(unsigned x, unsigned y, unsigned width, unsigned height, Color color) noexcept;
    void circle(unsigned center_x, unsigned center_y, unsigned radius, Color color) noexcept;
    void triangle(unsigned center_x, unsigned top, unsigned half_width, unsigned height,
                  Color color) noexcept;
    void text(unsigned x, unsigned y, std::string_view value, unsigned scale, Color color) noexcept;
    void blit(const std::uint32_t *src, unsigned src_w, unsigned src_h,
              unsigned dst_x, unsigned dst_y, unsigned dst_w, unsigned dst_h) noexcept;
    // vk-285-30: the whole frame in one colour (a linear fill of the tiled buffer), and text with
    // every glyph of the font (letters upper-cased, punctuation, '?' for the rest).
    void fill_screen(Color color) noexcept;
    void label(unsigned x, unsigned y, std::string_view value, unsigned scale, Color color) noexcept;

  private:
    explicit Canvas(std::uint32_t *pixels) noexcept : pixels_{pixels}
    {
    }

    std::uint32_t *pixels_;

    friend void run(DrawScene draw, std::string_view ready_message) noexcept;
    friend class MenuDisplay;
};

void read_asset_text(const char *path, std::span<char> destination,
                     std::string_view fallback) noexcept;
} // namespace ps5::demo
