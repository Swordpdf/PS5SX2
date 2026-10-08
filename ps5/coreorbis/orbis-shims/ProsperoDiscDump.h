// PS5SX2 (vk-285-144, AI-assisted): a PS2 DVD in the PS5's drive, copied to /data/PCSX2/games as an .iso. See
// ProsperoDiscDump.cpp.
#pragma once

// Starts the thread that watches the drive (once; the flag nodiscdump keeps it off).
void OrbisDiscDumpStart();

// A copy is running now.
bool OrbisDiscDumpBusy();
