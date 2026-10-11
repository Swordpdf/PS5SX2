// Orbis on-screen debug overlay API (used by main-boot.cpp).
#pragma once

namespace ps5::debug
{
    void set_stage(unsigned stage);
    void set_build(const char* tag);
    void set_line(int line, const char* fmt, ...);
    void notify(const char* message);
    void start_overlay();
    bool active();
} // namespace ps5::debug
