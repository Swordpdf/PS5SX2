// PS5SX2 (vk-285-144, AI-assisted): a PS2 DVD in the PS5's drive, copied to /data/PCSX2/games as an .iso. See
// ProsperoDiscDump.cpp.
#pragma once

#include <string>

// Starts the thread that watches the drive (once; the flag nodiscdump keeps it off).
void OrbisDiscDumpStart();

// A copy is running now.
bool OrbisDiscDumpBusy();

// PR #34 port (AI-assisted): a game is about to play straight from drive `device` ("/dev/cd1", OrbisIOCtlSrc.cpp). From
// now on the watcher leaves that drive alone: it doesn't look at it, copy its disc or start its copy. False (and nothing
// changes) when that drive's disc is being copied right now. OrbisDiscDumpRelease gives the drive back to the watcher.
bool OrbisDiscDumpClaim(const std::string& device);
void OrbisDiscDumpRelease(const std::string& device);
