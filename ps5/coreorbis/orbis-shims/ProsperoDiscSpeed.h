// PS5SX2 (vk-285-150, AI-assisted): asking the PS5's disc drive to read faster than the 2x it holds a PS2 DVD at. See
// ProsperoDiscSpeed.cpp and OrbisDiscScsi.h.
#pragma once

#include <cstdint>

// Finds the drive's pass device through /dev/cd0 (CAMGETPASSTHRU), logs what the drive says about its speeds, and sends
// SET STREAMING for the fastest read it offers over the disc's `bytes`. Logs every step ([discspeed] lines). Flag
// nodiscspeed: nothing is sent.
void OrbisDiscSpeedUp(const char* cd_path, uint64_t bytes);

// Reads 256 MB at the start of the disc and 256 MB at its end without keeping them, and logs and shows the speed of
// each (flag disc_readtest, for a disc already copied).
void OrbisDiscReadTest(const char* cd_path, uint64_t bytes);
