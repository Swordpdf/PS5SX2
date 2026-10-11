#include <chrono>
#include "OrbisPaths.h" // vk-285-33
#include <unistd.h>
// Orbis on-screen debug overlay: renders live boot status via demo_renderer.
#include "demo_renderer.hpp"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

extern "C" unsigned long long orbis_present_count(void);
extern "C" bool orbis_get_display(std::uint32_t** out, unsigned* w, unsigned* h);
extern "C" void orbis_display_lock();
extern "C" void orbis_display_unlock();

namespace ps5::debug
{
    struct State
    {
        std::atomic<unsigned> stage{0};
        char buildtag[64] = {};
        char line1[160] = {};
        char line2[160] = {};
        char line3[160] = {};
        char line4[160] = {};
        char line5[160] = {};
        char line6[160] = {};
        std::atomic<unsigned> updates{0};
        std::atomic<bool> active{false};
    };

    static State g_state;

    void set_stage(unsigned s)
    {
        g_state.stage.store(s);
    }

    void set_build(const char* tag)
    {
        std::snprintf(g_state.buildtag, sizeof(g_state.buildtag), "%s", tag ? tag : "?");
        g_state.updates.fetch_add(1);
    }

    void set_line(int idx, const char* fmt, ...)
    {
        if (idx < 1 || idx > 6)
            return;
        g_state.updates.fetch_add(1);
        char* target = idx == 1 ? g_state.line1 : idx == 2 ? g_state.line2 :
                       idx == 3 ? g_state.line3 : idx == 4 ? g_state.line4 :
                       idx == 5 ? g_state.line5 : g_state.line6;
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(target, sizeof(g_state.line1), fmt, args);
        va_end(args);
    }

    void notify(const char* msg)
    {
        set_line(6, "%s", msg);
    }

    namespace
    {
        std::atomic<unsigned> s_frames{0};

        void draw(ps5::demo::Canvas& canvas) noexcept
        {
            unsigned f = s_frames.fetch_add(1);
            // Show the emulated frame (if present), then the console overlay on top.
            std::uint32_t* disp = nullptr;
            unsigned dw = 0, dh = 0;
            // eerec-252: copy the emulated frame under the lock, blit (bilinear, slow-ish) outside it.
            // Holding g_display_mutex for the whole blit stalled PresentRect -> MTGS -> EE (stutters).
            static std::uint32_t s_copy[1920 * 1080];
            bool have = false;
            orbis_display_lock();
            if (orbis_get_display(&disp, &dw, &dh) && dw && dh && dw <= 1920 && dh <= 1080)
            {
                for (unsigned y = 0; y < dh; y++)
                    std::memcpy(&s_copy[y * 1920], &disp[y * 1920], dw * 4);
                have = true;
            }
            orbis_display_unlock();
            if (have)
            {
                // eerec-258: native-res sharpen (cross unsharp mask) before the bilinear upscale.
                // Strength from /data/PCSX2/sharpen (float, 0 = off), default 0.6.
                static const int s_k = []() {
                    float k = 0.6f;
                    if (FILE* f = std::fopen(OrbisFlagPath("sharpen").c_str(), "r")) { if (std::fscanf(f, "%f", &k) != 1) k = 0.6f; std::fclose(f); }
                    if (k < 0.0f) k = 0.0f;
                    if (k > 2.0f) k = 2.0f;
                    std::printf("[overlay] sharpen=%.2f\n", k);
                    return static_cast<int>(k * 256.0f);
                }();
                if (s_k > 0 && dw >= 3 && dh >= 3)
                {
                    static std::uint32_t s_sharp[1920 * 1080];
                    const int k = s_k;
                    for (unsigned y = 0; y < dh; y++)
                    {
                        const std::uint32_t* row = &s_copy[y * 1920];
                        const std::uint32_t* up = &s_copy[(y ? y - 1 : y) * 1920];
                        const std::uint32_t* dn = &s_copy[(y + 1 < dh ? y + 1 : y) * 1920];
                        std::uint32_t* out = &s_sharp[y * 1920];
                        for (unsigned x = 0; x < dw; x++)
                        {
                            const std::uint32_t c = row[x], l = row[x ? x - 1 : x], r = row[x + 1 < dw ? x + 1 : x];
                            const std::uint32_t u = up[x], d = dn[x];
                            std::uint32_t o = 0;
                            for (int sh = 0; sh < 24; sh += 8)
                            {
                                const int cc = (c >> sh) & 255;
                                const int nb = ((l >> sh) & 255) + ((r >> sh) & 255) + ((u >> sh) & 255) + ((d >> sh) & 255);
                                int v = cc + (((4 * cc - nb) * k) >> 10);
                                v = v < 0 ? 0 : (v > 255 ? 255 : v);
                                o |= static_cast<std::uint32_t>(v) << sh;
                            }
                            out[x] = o;
                        }
                    }
                    canvas.blit(s_sharp, dw, dh, 0, 0, 1920, 1080);
                }
                else
                    canvas.blit(s_copy, dw, dh, 0, 0, 1920, 1080);
            }
            // Orbis: FPS (emulated frames presented per second) in the top-right corner.
            {
                static unsigned long long last_presents = 0;
                static auto last_t = std::chrono::steady_clock::now();
                static unsigned fps_shown = 0;
                const auto now = std::chrono::steady_clock::now();
                const double dt = std::chrono::duration<double>(now - last_t).count();
                if (dt >= 1.0)
                {
                    const unsigned long long p = orbis_present_count();
                    fps_shown = static_cast<unsigned>((p - last_presents) / dt + 0.5);
                    last_presents = p;
                    last_t = now;
                }
                char fbuf[32];
                std::snprintf(fbuf, sizeof(fbuf), "FPS %u", fps_shown);
                canvas.rectangle(1680, 40, 220, 50, ps5::demo::Color::background);
                canvas.text(1695, 52, fbuf, 3, ps5::demo::Color::yellow);
            }
            // Orbis: debug panel only when /data/PCSX2/debugpanel exists (checked once).
            static const bool show_panel = OrbisFlag("debugpanel");
            if (!show_panel)
                return;
            // Terminal-style debug console in the TOP RIGHT (kernel debug box
            // occupies the top left). Dark panel, monospace prompt lines.
            canvas.rectangle(960, 20, 940, 620, ps5::demo::Color::background);
            canvas.rectangle(960, 20, 940, 50, ps5::demo::Color::panel);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "PCSX2 ORBIS %s", g_state.buildtag);
            canvas.text(975, 35, buf, 4, ps5::demo::Color::yellow);
            canvas.text(975, 90, "> /data/PCSX2/Ratchet & Clank.iso", 2, ps5::demo::Color::cyan);
            std::snprintf(buf, sizeof(buf), "STAGE %s", g_state.line1);
            canvas.text(975, 130, buf, 2, ps5::demo::Color::white);
            std::snprintf(buf, sizeof(buf), "STATUS %s", g_state.line2);
            canvas.text(975, 170, buf, 2, ps5::demo::Color::white);
            std::snprintf(buf, sizeof(buf), "INFO %s", g_state.line3);
            canvas.text(975, 210, buf, 2, ps5::demo::Color::white);
            std::snprintf(buf, sizeof(buf), "EE %s", g_state.line4);
            canvas.text(975, 250, buf, 2, ps5::demo::Color::white);
            std::snprintf(buf, sizeof(buf), "EESTATE %s", g_state.line5);
            canvas.text(975, 290, buf, 2, ps5::demo::Color::cyan);
            std::snprintf(buf, sizeof(buf), "IOP %s", g_state.line6);
            canvas.text(975, 330, buf, 2, ps5::demo::Color::magenta);
            std::snprintf(buf, sizeof(buf), "frames %u upd %u", f, g_state.updates.load());
            canvas.text(975, 370, buf, 2, ps5::demo::Color::white);
            canvas.text(975, 410, "> waiting for input...", 2, ps5::demo::Color::cyan);
        }
    } // namespace

    void start_overlay()
    {
        g_state.active.store(true);
        std::thread([]() {
            ps5::demo::run(&draw, "PCSX2 Orbis boot debug");
        }).detach();
    }

    bool active()
    {
        return g_state.active.load();
    }
} // namespace ps5::debug
