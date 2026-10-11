#include <unistd.h>
#include "OrbisPaths.h" // vk-285-33
/*
 * ps5-native-app-boilerplate - CPU VideoOut demonstration implementation.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Provides the bounded drawing surface and PS5 presentation loop used by the
 * editable starter application.
 */

#include "demo_renderer.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string_view>
#include <pthread.h>
#include <utility>

extern "C"
{
    std::size_t sceKernelGetDirectMemorySize();
    int sceKernelAllocateDirectMemory(std::int64_t search_start, std::int64_t search_end,
                                      std::size_t length, std::size_t alignment, int memory_type,
                                      std::int64_t *physical_address);
    int sceKernelMapDirectMemory(void **address, std::size_t length, int protection, int flags,
                                 std::int64_t physical_address, std::size_t alignment);
    int sceKernelSendNotificationRequest(std::uint32_t device, void *request, std::size_t size,
                                         int blocking);
    int sceKernelUsleep(std::uint32_t microseconds);
    int sceSystemServiceHideSplashScreen();
    int open(const char *path, int flags, ...);
    long read(int descriptor, void *buffer, std::size_t size);
    int close(int descriptor);
    int sceVideoOutOpen(std::int32_t user_id, std::int32_t bus_type, std::int32_t index,
                        const void *param);
    int sceVideoOutSetFlipRate(std::int32_t handle, std::int32_t rate);
    int sceVideoOutSubmitFlip(std::int32_t handle, std::int32_t buffer_index,
                              std::uint32_t flip_mode, std::int64_t flip_argument);
    int sceVideoOutWaitVblank(std::int32_t handle);
    int sceVideoOutClose(std::int32_t handle);
    int sceVideoOutUnregisterBuffers(std::int32_t handle, std::int32_t set_index);
    int sceKernelMunmap(void *address, std::size_t length);
    int sceKernelReleaseDirectMemory(std::int64_t start, std::size_t length);
    bool ps5ObserveOwnedAllocation(const void *address) noexcept;
    extern volatile unsigned long long g_orbis_data_base;
    extern volatile unsigned long long g_orbis_data_size;
    extern volatile unsigned long long g_orbis_code_base;
    extern volatile unsigned long long g_orbis_code_size;
}

namespace ps5::demo
{
namespace
{
constexpr unsigned frame_width = 1920;
constexpr unsigned frame_height = 1080;
constexpr std::size_t frame_bytes = 0x1000000;
constexpr std::size_t memory_bytes = frame_bytes * 2;
constexpr std::size_t memory_alignment = 0x200000;
constexpr int memory_type_wc_garlic = 3;
constexpr int map_protection = 0x33;
constexpr std::uint64_t pixel_format_rgba8_srgb = UINT64_C(0x8000000022000000);

struct VideoBuffer
{
    void *data;
    void *metadata;
    void *reserved0;
    void *reserved1;
};

struct VideoAttribute
{
    std::uint8_t reserved[80];
};

extern "C" void sceVideoOutSetBufferAttribute2(VideoAttribute *attribute,
                                               std::uint64_t pixel_format,
                                               std::uint32_t tiling_mode, std::uint32_t width,
                                               std::uint32_t height, std::uint64_t option,
                                               std::uint32_t dcc_control,
                                               std::uint64_t dcc_clear_color);
extern "C" int sceVideoOutRegisterBuffers2(std::int32_t handle, std::int32_t set_index,
                                           std::int32_t buffer_index_start, VideoBuffer *buffers,
                                           std::int32_t buffer_count, VideoAttribute *attribute,
                                           std::int32_t category, void *option);

struct NotificationRequest
{
    std::uint8_t reserved[45];
    char message[3075];
};

struct Glyph
{
    char character;
    std::array<std::uint8_t, 7> rows;
};

constexpr std::array<Glyph, 61> glyphs{{
    {' ', {0, 0, 0, 0, 0, 0, 0}},        {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},      {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {30, 1, 1, 14, 1, 1, 30}},     {'4', {2, 6, 10, 18, 31, 2, 2}},
    {'5', {31, 16, 16, 30, 1, 1, 30}},   {'6', {14, 16, 16, 30, 17, 17, 14}},
    {'7', {31, 1, 2, 4, 8, 8, 8}},       {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 1, 14}},   {'A', {14, 17, 17, 31, 17, 17, 17}},
    {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'F', {31, 16, 16, 30, 16, 16, 16}}, {'G', {14, 17, 16, 23, 17, 17, 14}},
    {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {31, 4, 4, 4, 4, 4, 31}},
    {'J', {7, 2, 2, 2, 18, 18, 12}},     {'K', {17, 18, 20, 24, 20, 18, 17}},
    {'L', {16, 16, 16, 16, 16, 16, 31}}, {'M', {17, 27, 21, 21, 17, 17, 17}},
    {'N', {17, 25, 21, 19, 17, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}},
    {'R', {30, 17, 17, 30, 20, 18, 17}}, {'S', {15, 16, 16, 14, 1, 1, 30}},
    {'T', {31, 4, 4, 4, 4, 4, 4}},       {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}},  {'W', {17, 17, 17, 21, 21, 21, 10}},
    {'X', {17, 17, 10, 4, 10, 17, 17}},  {'Y', {17, 17, 10, 4, 4, 4, 4}},
    {'Z', {31, 1, 2, 4, 8, 16, 31}},
    // vk-285-12: OSD resolution line ("512x512 > 3840x2160")
    {'x', {0, 0, 17, 10, 4, 10, 17}},    {'>', {16, 8, 4, 2, 4, 8, 16}},
    {':', {0, 12, 12, 0, 12, 12, 0}},    {'.', {0, 0, 0, 0, 0, 12, 12}},
    {'-', {0, 0, 0, 14, 0, 0, 0}},       {'/', {1, 1, 2, 4, 8, 16, 16}},
    {'%', {24, 25, 2, 4, 8, 19, 3}},
    // vk-285-30: the game selector's titles ("Lord of the Rings, The - ...", "Ratchet & Clank")
    {'(', {2, 4, 8, 8, 8, 4, 2}},        {')', {8, 4, 2, 2, 2, 4, 8}},
    {',', {0, 0, 0, 0, 12, 4, 8}},       {'&', {12, 18, 20, 8, 21, 18, 13}},
    {'\'', {4, 4, 8, 0, 0, 0, 0}},       {'!', {4, 4, 4, 4, 4, 0, 4}},
    {'[', {14, 8, 8, 8, 8, 8, 14}},      {']', {14, 2, 2, 2, 2, 2, 14}},
    {'+', {0, 4, 4, 31, 4, 4, 0}},       {'_', {0, 0, 0, 0, 0, 0, 31}},
    {'?', {14, 17, 1, 2, 4, 0, 4}},      {'=', {0, 0, 31, 0, 31, 0, 0}},
    {';', {0, 12, 12, 0, 12, 4, 8}},     {'#', {10, 10, 31, 10, 31, 10, 10}},
    {'"', {10, 10, 0, 0, 0, 0, 0}},      {'*', {0, 4, 21, 14, 21, 4, 0}},
    {'<', {1, 2, 4, 8, 4, 2, 1}},
}};

class File final
{
  public:
    explicit File(int descriptor = -1) noexcept : descriptor_{descriptor}
    {
    }

    ~File()
    {
        reset();
    }

    File(const File &) = delete;
    File &operator=(const File &) = delete;

    File(File &&other) noexcept : descriptor_{std::exchange(other.descriptor_, -1)}
    {
    }

    File &operator=(File &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return descriptor_ >= 0;
    }

    [[nodiscard]] int get() const noexcept
    {
        return descriptor_;
    }

  private:
    void reset() noexcept
    {
        if (descriptor_ >= 0)
        {
            (void)close(descriptor_);
            descriptor_ = -1;
        }
    }

    int descriptor_;
};

class LifetimeProbe final
{
  public:
    explicit LifetimeProbe(bool &destroyed) noexcept : destroyed_{&destroyed}
    {
    }

    ~LifetimeProbe()
    {
        *destroyed_ = true;
    }

    LifetimeProbe(const LifetimeProbe &) = delete;
    LifetimeProbe &operator=(const LifetimeProbe &) = delete;

  private:
    bool *destroyed_;
};

NotificationRequest notification{};

void copy_message(std::span<char> destination, std::string_view source) noexcept
{
    if (destination.empty())
        return;

    const std::size_t count =
        source.size() < destination.size() - 1 ? source.size() : destination.size() - 1;
    for (std::size_t index = 0; index < count; ++index)
        destination[index] = source[index];
    destination[count] = '\0';
}

void notify(std::string_view message) noexcept
{
    copy_message(std::span{notification.message}, message);
    (void)sceKernelSendNotificationRequest(0, &notification, sizeof(notification), 0);
}

[[nodiscard]] bool verify_unique_ownership() noexcept
{
    bool destroyed = false;
    {
        std::unique_ptr<LifetimeProbe> probe{new (std::nothrow_t{}) LifetimeProbe{destroyed}};
        if (!ps5ObserveOwnedAllocation(probe.get()))
            return false;
    }
    return destroyed;
}

[[noreturn]] void halt(std::string_view message) noexcept
{
    notify(message);
    for (;;)
        (void)sceKernelUsleep(1000000);
}

[[nodiscard]] constexpr std::span<const std::uint8_t, 7> glyph_rows(char character) noexcept
{
    for (const auto &glyph : glyphs)
    {
        if (glyph.character == character)
            return glyph.rows;
    }
    return glyphs.front().rows;
}

[[nodiscard]] constexpr std::size_t tiled_byte_offset(unsigned x, unsigned y) noexcept
{
    const std::uint32_t offset = ((y << 4) & 0x70U) ^ ((y << 5) & 0xf00U) ^ ((y << 9) & 0x1000U) ^
                                 ((y << 8) & 0x4000U) ^ ((x << 2) & 0xcU) ^ ((x << 5) & 0x380U) ^
                                 ((x << 4) & 0x400U) ^ ((x << 6) & 0x800U) ^ ((x << 9) & 0xa000U);
    const std::uint32_t blocks_per_row = (frame_width + 127U) >> 7;
    const std::uint32_t block_index = (y >> 7) * blocks_per_row + (x >> 7);

    return (static_cast<std::size_t>(block_index) << 16) + offset;
}

void put_pixel_unchecked(std::uint32_t *pixels, unsigned x, unsigned y, Color color) noexcept
{
    auto *bytes = reinterpret_cast<std::uint8_t *>(pixels);
    *reinterpret_cast<std::uint32_t *>(bytes + tiled_byte_offset(x, y)) =
        static_cast<std::uint32_t>(color);
}

void fill_rect(std::uint32_t *pixels, unsigned x, unsigned y, unsigned width, unsigned height,
               Color color) noexcept
{
    if (x >= frame_width || y >= frame_height)
        return;

    const unsigned right = width > frame_width - x ? frame_width : x + width;
    const unsigned bottom = height > frame_height - y ? frame_height : y + height;
    for (unsigned row = y; row < bottom; ++row)
    {
        for (unsigned column = x; column < right; ++column)
            put_pixel_unchecked(pixels, column, row, color);
    }
}

void fill_circle(std::uint32_t *pixels, unsigned center_x, unsigned center_y, unsigned radius,
                 Color color) noexcept
{
    const int signed_radius = static_cast<int>(radius);
    for (int y = -signed_radius; y <= signed_radius; ++y)
    {
        for (int x = -signed_radius; x <= signed_radius; ++x)
        {
            if (x * x + y * y <= signed_radius * signed_radius)
            {
                const int pixel_x = static_cast<int>(center_x) + x;
                const int pixel_y = static_cast<int>(center_y) + y;
                if (pixel_x >= 0 && pixel_y >= 0)
                {
                    const auto bounded_x = static_cast<unsigned>(pixel_x);
                    const auto bounded_y = static_cast<unsigned>(pixel_y);
                    if (bounded_x < frame_width && bounded_y < frame_height)
                        put_pixel_unchecked(pixels, bounded_x, bounded_y, color);
                }
            }
        }
    }
}

void fill_triangle(std::uint32_t *pixels, unsigned center_x, unsigned top, unsigned half_width,
                   unsigned height, Color color) noexcept
{
    if (height == 0 || center_x >= frame_width)
        return;

    for (unsigned row = 0; row < height; ++row)
    {
        const unsigned half = row * half_width / height;
        const unsigned left = half > center_x ? 0 : center_x - half;
        const unsigned right = half >= frame_width - center_x ? frame_width : center_x + half + 1;
        fill_rect(pixels, left, top + row, right - left, 1, color);
    }
}

void draw_text(std::uint32_t *pixels, unsigned x, unsigned y, std::string_view value,
               unsigned scale, Color color) noexcept
{
    for (const char raw : value)
    {
        char character = raw;
        if (character >= 'a' && character <= 'z')
            character = static_cast<char>(character - 'a' + 'A');
        const bool known = (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9');
        if (known)
        {
            const auto rows = glyph_rows(character);
            for (unsigned row = 0; row < rows.size(); ++row)
            {
                for (unsigned column = 0; column < 5; ++column)
                {
                    if ((rows[row] & (1U << (4 - column))) != 0)
                        fill_rect(pixels, x + column * scale, y + row * scale, scale, scale, color);
                }
            }
        }
        x += (known ? 6 : 2) * scale;
        if (x >= frame_width)
            return;
    }
}

// vk-285-30: a glyph by its exact character (nullptr when the font has none).
[[nodiscard]] const Glyph *find_glyph(char character) noexcept
{
    for (const auto &glyph : glyphs)
    {
        if (glyph.character == character)
            return &glyph;
    }
    return nullptr;
}

// vk-285-30: label()'s characters: letters upper-cased, a missing glyph drawn as '?'.
[[nodiscard]] char label_char(char raw) noexcept
{
    const char upper = (raw >= 'a' && raw <= 'z') ? static_cast<char>(raw - 'a' + 'A') : raw;
    return find_glyph(upper) ? upper : '?';
}

// vk-285-30: the tiled frame's bytes that hold the visible 1920x1080 (whole 128x128 blocks).
constexpr std::size_t used_frame_bytes =
    static_cast<std::size_t>((frame_width + 127U) >> 7) * ((frame_height + 127U) >> 7) * 0x10000U;
static_assert(used_frame_bytes <= frame_bytes);

void flush_range(void *address, std::size_t length) noexcept
{
    auto *at = static_cast<std::uint8_t *>(address);
    const auto *end = at + length;

    for (; at < end; at += 64)
        __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}
} // namespace

void Canvas::clear(Color color) noexcept
{
    fill_rect(pixels_, 0, 0, frame_width, frame_height, color);
}

void Canvas::rectangle(unsigned x, unsigned y, unsigned width, unsigned height,
                       Color color) noexcept
{
    fill_rect(pixels_, x, y, width, height, color);
}

void Canvas::circle(unsigned center_x, unsigned center_y, unsigned radius, Color color) noexcept
{
    fill_circle(pixels_, center_x, center_y, radius, color);
}

void Canvas::triangle(unsigned center_x, unsigned top, unsigned half_width, unsigned height,
                      Color color) noexcept
{
    fill_triangle(pixels_, center_x, top, half_width, height, color);
}

void Canvas::text(unsigned x, unsigned y, std::string_view value, unsigned scale,
                  Color color) noexcept
{
    draw_text(pixels_, x, y, value, scale, color);
}

// vk-285-30: every pixel of a tiled frame lies in one of its whole 64 KiB blocks, so one colour
// is a linear fill of those blocks.
void Canvas::fill_screen(Color color) noexcept
{
    const std::uint32_t value = static_cast<std::uint32_t>(color);
    std::uint32_t *at = pixels_;
    for (std::size_t i = 0; i < used_frame_bytes / 4; ++i)
        at[i] = value;
}

// vk-285-30: 6*scale per glyph, 4*scale per space.
unsigned label_width(std::string_view value, unsigned scale) noexcept
{
    unsigned width = 0;
    for (const char raw : value)
        width += (raw == ' ' ? 4U : 6U) * scale;
    return width > scale ? width - scale : width; // no gap after the last glyph
}

void Canvas::label(unsigned x, unsigned y, std::string_view value, unsigned scale,
                   Color color) noexcept
{
    for (const char raw : value)
    {
        if (raw == ' ')
        {
            x += 4U * scale;
            continue;
        }
        const Glyph *glyph = find_glyph(label_char(raw));
        if (glyph != nullptr)
        {
            for (unsigned row = 0; row < glyph->rows.size(); ++row)
            {
                for (unsigned column = 0; column < 5; ++column)
                {
                    if ((glyph->rows[row] & (1U << (4 - column))) != 0)
                        fill_rect(pixels_, x + column * scale, y + row * scale, scale, scale, color);
                }
            }
        }
        x += 6U * scale;
        if (x >= frame_width)
            return;
    }
}

static inline std::uint32_t orbis_lerp_px(std::uint32_t a, std::uint32_t b, std::uint32_t f) noexcept
{
    // f in [0,256]: a*(256-f) + b*f, two channels at a time.
    const std::uint32_t rb = ((((a & 0x00FF00FFu) * (256u - f)) + ((b & 0x00FF00FFu) * f)) >> 8) & 0x00FF00FFu;
    const std::uint32_t g = ((((a >> 8) & 0x00FF00FFu) * (256u - f)) + (((b >> 8) & 0x00FF00FFu) * f)) & 0xFF00FF00u;
    return rb | g;
}

void Canvas::blit(const std::uint32_t *src, unsigned src_w, unsigned src_h,
                  unsigned dst_x, unsigned dst_y, unsigned dst_w, unsigned dst_h) noexcept
{
    // eerec-245: table-driven tiled blit with bilinear scaling (flag /data/PCSX2/nearest = old point sampling).
    if (!src || src_w == 0 || src_h == 0 || dst_x >= frame_width || dst_y >= frame_height)
        return;
    if (dst_w > frame_width - dst_x) dst_w = frame_width - dst_x;
    if (dst_h > frame_height - dst_y) dst_h = frame_height - dst_y;
    static const bool s_nearest = OrbisFlag("nearest");
    static std::uint32_t xlow[frame_width], xblk[frame_width], xs0[frame_width], xs1[frame_width], xf[frame_width];
    static std::uint32_t hrow[2][frame_width];
    static unsigned cached_sw = 0, cached_dw = 0, cached_dx = ~0u;
    if (cached_sw != src_w || cached_dw != dst_w || cached_dx != dst_x)
    {
        for (unsigned dx = 0; dx < dst_w; dx++)
        {
            const unsigned x = dst_x + dx;
            xlow[dx] = ((x << 2) & 0xcU) ^ ((x << 5) & 0x380U) ^ ((x << 4) & 0x400U) ^ ((x << 6) & 0x800U) ^ ((x << 9) & 0xa000U);
            xblk[dx] = (x >> 7) << 16;
            if (s_nearest)
            {
                unsigned sx = (dx * src_w) / dst_w;
                xs0[dx] = xs1[dx] = sx < src_w ? sx : src_w - 1;
                xf[dx] = 0;
            }
            else
            {
                // centre-aligned sample position in 8.8 fixed point
                long long pos = ((2LL * dx + 1) * src_w * 256) / (2LL * dst_w) - 128;
                if (pos < 0) pos = 0;
                unsigned s0 = (unsigned)(pos >> 8);
                if (s0 >= src_w - 1) { s0 = src_w - 1; xs0[dx] = xs1[dx] = s0; xf[dx] = 0; }
                else { xs0[dx] = s0; xs1[dx] = s0 + 1; xf[dx] = (unsigned)(pos & 255); }
            }
        }
        cached_sw = src_w; cached_dw = dst_w; cached_dx = dst_x;
    }
    const std::uint32_t blocks_per_row = (frame_width + 127U) >> 7;
    auto *bytes = reinterpret_cast<std::uint8_t *>(pixels_);
    int cached_row[2] = {-1, -1};
    auto hscale = [&](unsigned sy, int slot) {
        if (cached_row[slot] == (int)sy) return;
        if (cached_row[slot ^ 1] == (int)sy) { std::memcpy(hrow[slot], hrow[slot ^ 1], dst_w * 4); cached_row[slot] = (int)sy; return; }
        const std::uint32_t *srow = src + static_cast<std::size_t>(sy) * frame_width;
        std::uint32_t *out = hrow[slot];
        for (unsigned dx = 0; dx < dst_w; dx++)
            out[dx] = xf[dx] ? orbis_lerp_px(srow[xs0[dx]], srow[xs1[dx]], xf[dx]) : srow[xs0[dx]];
        cached_row[slot] = (int)sy;
    };
    for (unsigned dy = 0; dy < dst_h; dy++)
    {
        unsigned sy0, sy1, fy;
        if (s_nearest)
        {
            sy0 = sy1 = (dy * src_h) / dst_h; fy = 0;
            if (sy0 >= src_h) continue;
        }
        else
        {
            long long pos = ((2LL * dy + 1) * src_h * 256) / (2LL * dst_h) - 128;
            if (pos < 0) pos = 0;
            sy0 = (unsigned)(pos >> 8);
            if (sy0 >= src_h - 1) { sy0 = sy1 = src_h - 1; fy = 0; }
            else { sy1 = sy0 + 1; fy = (unsigned)(pos & 255); }
        }
        hscale(sy0, 0);
        if (fy) hscale(sy1, 1);
        const unsigned y = dst_y + dy;
        const std::uint32_t ylow = ((y << 4) & 0x70U) ^ ((y << 5) & 0xf00U) ^ ((y << 9) & 0x1000U) ^ ((y << 8) & 0x4000U);
        std::uint8_t *rowbase = bytes + (static_cast<std::size_t>((y >> 7) * blocks_per_row) << 16);
        const std::uint32_t *r0 = hrow[0], *r1 = hrow[1];
        if (fy)
            for (unsigned dx = 0; dx < dst_w; dx++)
                *reinterpret_cast<std::uint32_t *>(rowbase + xblk[dx] + (ylow ^ xlow[dx])) = orbis_lerp_px(r0[dx], r1[dx], fy) | 0xFF000000u;
        else
            for (unsigned dx = 0; dx < dst_w; dx++)
                *reinterpret_cast<std::uint32_t *>(rowbase + xblk[dx] + (ylow ^ xlow[dx])) = r0[dx] | 0xFF000000u;
    }
}

// eerec-278: demo font into a linear RGBA buffer (GL presenter FPS / mode box).
extern "C" void orbis_text_rgba(std::uint32_t *buf, unsigned w, unsigned h, unsigned x, unsigned y,
                                const char *s, unsigned scale, std::uint32_t color)
{
    for (; s && *s; ++s)
    {
        // vk-285-12: exact glyph first (small 'x', punctuation), else the upper-case letter.
        char c = *s;
        const auto has = [](char ch) {
            for (const auto &glyph : glyphs)
                if (glyph.character == ch && ch != ' ')
                    return true;
            return false;
        };
        if (!has(c) && c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
        const bool known = has(c);
        if (known)
        {
            const auto rows = glyph_rows(c);
            for (unsigned row = 0; row < 7; ++row)
                for (unsigned col = 0; col < 5; ++col)
                    if ((rows[row] & (1U << (4 - col))) != 0)
                        for (unsigned yy = 0; yy < scale; ++yy)
                            for (unsigned xx = 0; xx < scale; ++xx)
                            {
                                const unsigned px = x + col * scale + xx, py = y + row * scale + yy;
                                if (px < w && py < h)
                                    buf[py * w + px] = color;
                            }
        }
        x += (known ? 6 : 2) * scale;
    }
}

// Shared display buffer written by GSDeviceOrbis::PresentRect (MTGS thread),
// drawn by the overlay (render thread).
namespace
{
    std::uint32_t g_display[frame_width * frame_height];
    std::mutex g_display_mutex;
    std::atomic<bool> g_display_valid{false};
    std::atomic<unsigned> g_display_w{0};
    std::atomic<unsigned> g_display_h{0};
}

static std::atomic<unsigned long long> g_orbis_presents{0};
extern "C" unsigned long long orbis_present_count(void) { return g_orbis_presents.load(std::memory_order_relaxed); }
extern "C" void orbis_present_frame(const unsigned char* pixels, int w, int h, int pitch)
{
    g_orbis_presents.fetch_add(1, std::memory_order_relaxed);
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        std::uint32_t sample = 0;
        if (pixels && w > 0 && h > 0)
            std::memcpy(&sample, pixels, 4);
        printf("[overlay] first present: %dx%d pitch=%d sample=%08x\n", w, h, pitch, sample);
        fflush(stdout);
    }
    if (!pixels || w <= 0 || h <= 0 || w > frame_width || h > frame_height)
        return;
    std::lock_guard<std::mutex> lock(g_display_mutex);
    static unsigned long long present_count = 0;
    // eerec-257: frame.bmp dump (~35 ms hitch under the display lock) only with /data/PCSX2/framedump.
    static const bool s_framedump = OrbisFlag("framedump");
    if ((present_count++ % 500) == 0 && s_framedump)
    {
        std::uint32_t samples[4] = {0, 0, 0, 0};
        const std::uint32_t* p32 = reinterpret_cast<const std::uint32_t*>(pixels);
        samples[0] = p32[0];
        samples[1] = p32[static_cast<size_t>(h / 2) * (pitch / 4) + static_cast<size_t>(w / 2)];
        samples[2] = p32[static_cast<size_t>(h - 1) * (pitch / 4)];
        samples[3] = p32[static_cast<size_t>(w - 1)];
        printf("[overlay] present#%llu: %dx%d s0=%08x s1=%08x s2=%08x s3=%08x\n",
            present_count, w, h, samples[0], samples[1], samples[2], samples[3]);
        fflush(stdout);
        // Orbis: dump the frame for FTP inspection (stride/format diagnosis).
        FILE* f = fopen(OrbisLogPath("frame.bmp").c_str(), "wb");
        if (f)
        {
            unsigned char hdr[54] = {0};
            unsigned rowbytes = (unsigned)w * 4;
            unsigned imgsize = rowbytes * (unsigned)h;
            hdr[0] = 'B'; hdr[1] = 'M';
            *(unsigned*)(hdr + 2) = 54 + imgsize;
            *(unsigned*)(hdr + 10) = 54;
            *(unsigned*)(hdr + 14) = 40;
            *(int*)(hdr + 18) = w;
            *(int*)(hdr + 22) = h;
            *(unsigned short*)(hdr + 26) = 1;
            *(unsigned short*)(hdr + 28) = 32;
            *(unsigned*)(hdr + 34) = imgsize;
            fwrite(hdr, 1, 54, f);
            for (int yy = h - 1; yy >= 0; yy--)
                fwrite(pixels + (size_t)yy * pitch, 1, rowbytes, f);
            fclose(f);
            printf("[overlay] frame.bmp dumped %dx%d\n", w, h);
            fflush(stdout);
        }
    }
    const unsigned dst_w = static_cast<unsigned>(w);
    const unsigned dst_h = static_cast<unsigned>(h);
    for (unsigned y = 0; y < dst_h; y++)
    {
        const std::uint32_t* src_row = reinterpret_cast<const std::uint32_t*>(pixels + static_cast<size_t>(y) * pitch);
        std::memcpy(&g_display[y * frame_width], src_row, static_cast<size_t>(dst_w) * sizeof(std::uint32_t));
    }
    g_display_w.store(dst_w);
    g_display_h.store(dst_h);
    g_display_valid.store(true);
}

extern "C" bool orbis_get_display(std::uint32_t** out, unsigned* w, unsigned* h)
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        printf("[overlay] get_display: valid=%d\n", (int)g_display_valid.load());
        fflush(stdout);
    }
    if (!g_display_valid.load())
        return false;
    *out = g_display;
    *w = g_display_w.load();
    *h = g_display_h.load();
    return true;
}

extern "C" void orbis_display_lock()
{
    g_display_mutex.lock();
}

extern "C" void orbis_display_unlock()
{
    g_display_mutex.unlock();
}

void read_asset_text(const char *path, std::span<char> destination,
                     std::string_view fallback) noexcept
{
    copy_message(destination, fallback);
    File descriptor{open(path, 0)};
    if (!descriptor.valid() || destination.empty())
        return;

    const long count = read(descriptor.get(), destination.data(), destination.size() - 1);
    if (count <= 0)
        return;

    std::size_t length = static_cast<std::size_t>(count);
    while (length > 0 && (destination[length - 1] == '\r' || destination[length - 1] == '\n'))
        --length;
    destination[length] = '\0';
}

[[noreturn]] void run(DrawScene draw, std::string_view ready_message) noexcept
{
    // Orbis: the SDK ownership-observation API isn't wired up in this build and
    // would halt the overlay thread; skip the probe-style ownership check.
    if (draw == nullptr)
        halt("Hello World: scene callback missing");

    printf("[overlay] thread tid=%llu\n", (unsigned long long)pthread_self());
    fflush(stdout);

    (void)sceSystemServiceHideSplashScreen();
    // Orbis: OVERLAY bus (1) so ps5-opengl's GPU runtime can own the MAIN bus
    // (0) for its own VideoOut handle. Two handles with the same (0xff,0,0)
    // tuple cannot coexist; different buses can.
    const int video = sceVideoOutOpen(0xff, 1, 0, nullptr);
    if (video < 0)
        halt("Hello World: sceVideoOutOpen(OVERLAY) failed");

    const std::size_t pool_size = sceKernelGetDirectMemorySize();
    if (pool_size < memory_bytes)
        halt("Hello World: insufficient direct memory");

    std::int64_t physical_address = 0;
    int result =
        sceKernelAllocateDirectMemory(0, static_cast<std::int64_t>(pool_size), memory_bytes,
                                      memory_alignment, memory_type_wc_garlic, &physical_address);
    if (result < 0)
        halt("Hello World: direct-memory allocation failed");

    void *mapped = reinterpret_cast<void *>(0x720000000ULL);
    result = sceKernelMapDirectMemory(&mapped, memory_bytes, map_protection, 0, physical_address,
                                      memory_alignment);
    if (result < 0)
        halt("Hello World: direct-memory mapping failed");

    const auto video_begin = reinterpret_cast<std::uintptr_t>(mapped);
    const auto video_end = video_begin + memory_bytes;
    const auto overlaps = [video_begin, video_end](volatile unsigned long long base,
                                                    volatile unsigned long long size) {
        const auto begin = static_cast<std::uintptr_t>(base);
        const auto end = begin + static_cast<std::size_t>(size);
        return begin < video_end && video_begin < end;
    };
    printf("[overlay] VideoOut map=%p len=%zu data=%llx+%llx code=%llx+%llx\n",
           mapped, memory_bytes, g_orbis_data_base, g_orbis_data_size,
           g_orbis_code_base, g_orbis_code_size);
    fflush(stdout);
    if (overlaps(g_orbis_data_base, g_orbis_data_size) ||
        overlaps(g_orbis_code_base, g_orbis_code_size))
        halt("Hello World: VideoOut mapping overlaps PCSX2 memory");

    Canvas first{static_cast<std::uint32_t *>(mapped)};
    auto *second_frame = static_cast<std::uint8_t *>(mapped) + frame_bytes;
    Canvas second{reinterpret_cast<std::uint32_t *>(second_frame)};
    draw(first);
    draw(second);
    flush_range(mapped, memory_bytes);

    std::array<VideoBuffer, 2> buffers{{
        {mapped, nullptr, nullptr, nullptr},
        {second_frame, nullptr, nullptr, nullptr},
    }};
    VideoAttribute attribute{};
    (void)sceVideoOutSetFlipRate(video, 0);
    sceVideoOutSetBufferAttribute2(&attribute, pixel_format_rgba8_srgb, 0, frame_width,
                                   frame_height, 0, 0, 0);

    result = sceVideoOutRegisterBuffers2(video, 0, 0, buffers.data(),
                                         static_cast<std::int32_t>(buffers.size()), &attribute, 0,
                                         nullptr);
    if (result < 0)
        halt("Hello World: buffer registration failed");
    if (sceVideoOutSubmitFlip(video, 0, 1, 1) < 0)
        halt("Hello World: initial flip failed");

    (void)sceVideoOutWaitVblank(video);
    notify(ready_message);

    // Redraw each frame so live status + game frame update on screen.
    unsigned long frame_no = 0;
    // eerec-255: only redraw a scanout buffer when the emulator presented a new frame since that
    // buffer was last drawn (or every 32 vblanks for the FPS text), and flush only that buffer
    // (was: blit + 16 MB cache flush every vblank for both buffers).
    unsigned long long drawn_at[2] = {~0ULL, ~0ULL};
    unsigned vb = 0;
    auto refresh = [&](Canvas &c, int i, void *base) {
        const unsigned long long p = orbis_present_count();
        if (p != drawn_at[i] || (vb & 31) == 0)
        {
            draw(c);
            flush_range(base, frame_bytes);
            drawn_at[i] = p;
        }
        vb++;
    };
    for (;;)
    {
        refresh(first, 0, mapped);
        if (sceVideoOutSubmitFlip(video, 0, 1, 1) < 0)
            break;
        (void)sceVideoOutWaitVblank(video);

        refresh(second, 1, second_frame);
        if (sceVideoOutSubmitFlip(video, 1, 1, 1) < 0)
            break;
        (void)sceVideoOutWaitVblank(video);
    }
}

// ---- vk-285-30: the game selector's screen ----

bool MenuDisplay::open() noexcept
{
    (void)sceSystemServiceHideSplashScreen();
    // The OVERLAY bus, as the boot overlay: the Vulkan device takes the MAIN bus later.
    video_ = sceVideoOutOpen(0xff, 1, 0, nullptr);
    if (video_ < 0)
    {
        printf("[menu] sceVideoOutOpen(OVERLAY) failed: %#x\n", static_cast<unsigned>(video_));
        video_ = -1;
        return false;
    }
    const std::size_t pool_size = sceKernelGetDirectMemorySize();
    std::int64_t physical = 0;
    int result = pool_size < memory_bytes ? -1 :
        sceKernelAllocateDirectMemory(0, static_cast<std::int64_t>(pool_size), memory_bytes, memory_alignment,
                                      memory_type_wc_garlic, &physical);
    if (result < 0)
    {
        printf("[menu] direct memory: pool %zu, allocate %#x\n", pool_size, static_cast<unsigned>(result));
        close();
        return false;
    }
    physical_ = physical;
    allocated_ = true;
    void *mapped = reinterpret_cast<void *>(0x720000000ULL);
    result = sceKernelMapDirectMemory(&mapped, memory_bytes, map_protection, 0, physical, memory_alignment);
    if (result < 0)
    {
        printf("[menu] map direct memory failed: %#x\n", static_cast<unsigned>(result));
        close();
        return false;
    }
    mapped_ = mapped;
    // Both buffers start transparent black, so nothing shows before the first present.
    for (int i = 0; i < 2; ++i)
    {
        Canvas canvas{reinterpret_cast<std::uint32_t *>(static_cast<std::uint8_t *>(mapped_) + i * frame_bytes)};
        canvas.fill_screen(static_cast<Color>(0));
    }
    flush_range(mapped_, memory_bytes);
    std::array<VideoBuffer, 2> buffers{{
        {mapped_, nullptr, nullptr, nullptr},
        {static_cast<std::uint8_t *>(mapped_) + frame_bytes, nullptr, nullptr, nullptr},
    }};
    VideoAttribute attribute{};
    (void)sceVideoOutSetFlipRate(video_, 0);
    sceVideoOutSetBufferAttribute2(&attribute, pixel_format_rgba8_srgb, 0, frame_width, frame_height, 0, 0, 0);
    result = sceVideoOutRegisterBuffers2(video_, 0, 0, buffers.data(), static_cast<std::int32_t>(buffers.size()),
                                         &attribute, 0, nullptr);
    if (result < 0)
    {
        printf("[menu] register buffers failed: %#x\n", static_cast<unsigned>(result));
        close();
        return false;
    }
    registered_ = true;
    back_ = 0;
    printf("[menu] display open: video %d, buffers at %p\n", video_, mapped_);
    return true;
}

void MenuDisplay::present(MenuScene draw, const void *user) noexcept
{
    if (!registered_ || draw == nullptr)
        return;
    auto *base = static_cast<std::uint8_t *>(mapped_) + static_cast<std::size_t>(back_) * frame_bytes;
    Canvas canvas{reinterpret_cast<std::uint32_t *>(base)};
    draw(canvas, user);
    flush_range(base, used_frame_bytes);
    (void)sceVideoOutSubmitFlip(video_, back_, 1, 1);
    (void)sceVideoOutWaitVblank(video_);
    back_ ^= 1;
}

void MenuDisplay::close() noexcept
{
    int unregister = 0, closed = 0, unmapped = 0, released = 0;
    if (registered_)
    {
        // A transparent frame first: should the bus outlive close(), it can't cover the game.
        for (int i = 0; i < 2; ++i)
        {
            auto *base = static_cast<std::uint8_t *>(mapped_) + static_cast<std::size_t>(back_) * frame_bytes;
            Canvas canvas{reinterpret_cast<std::uint32_t *>(base)};
            canvas.fill_screen(static_cast<Color>(0));
            flush_range(base, used_frame_bytes);
            (void)sceVideoOutSubmitFlip(video_, back_, 1, 1);
            (void)sceVideoOutWaitVblank(video_);
            back_ ^= 1;
        }
        (void)sceVideoOutWaitVblank(video_);
        unregister = sceVideoOutUnregisterBuffers(video_, 0);
        registered_ = false;
    }
    if (video_ >= 0)
    {
        closed = sceVideoOutClose(video_);
        video_ = -1;
    }
    if (mapped_ != nullptr)
    {
        unmapped = sceKernelMunmap(mapped_, memory_bytes);
        mapped_ = nullptr;
    }
    if (allocated_)
    {
        released = sceKernelReleaseDirectMemory(physical_, memory_bytes);
        allocated_ = false;
    }
    printf("[menu] display closed: unregister %#x close %#x unmap %#x release %#x\n",
           static_cast<unsigned>(unregister), static_cast<unsigned>(closed), static_cast<unsigned>(unmapped),
           static_cast<unsigned>(released));
}
} // namespace ps5::demo
