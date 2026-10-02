// PS5 port (vk-285-33): where things live in the PCSX2 data folder.
//
// The data folder defaults to /data/PCSX2 and can be changed by the user to any directory the
// PS5 can read (an external HDD, USB drive, etc.). The active root is read once at startup from
// /data/ps5sx2_root.txt; without that file the default is used. OrbisRoot() returns whichever
// is active. OrbisSetRoot() writes a new choice for the next launch.
//
//   games/       disc images (the game selector also still looks in the top folder)
//   bios/        the BIOS and its .mec/.nvm
//   memcards/    Mcd001.ps2, Mcd002.ps2
//   savestates/  *.p2s
//   settings/    per-game settings (<disc image name>.ini)
//   patches/     *.pnach
//   textures/    replacement textures
//   resources/   GameIndex.yaml, shaders/
//   cache/       PCSX2's and the Vulkan driver's shader caches
//   flags/       the on/off switch files (vk_async, swjit, ...)
//   logs/        boot.log, emulog.txt, stderr.log and diagnostic output
//   gs.ini, live.ini, lastgame.txt, playtime.dat, pid.txt stay in the top folder.
//
// Each folder is used when it exists; without it the top folder is, as in the old flat layout, so
// an older console setup keeps working and a new build can go on before the files move.
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

// The active PCSX2 data root (/data/PCSX2 by default, or the path from /data/ps5sx2_root.txt).
// Read once at startup; the result is stable for the lifetime of the process.
const std::string& OrbisRoot();

// Saves `dir` as the new data root in /data/ps5sx2_root.txt for the next launch.
// Validates the path (must start with '/', no '..', max 480 chars). Returns true on success.
// Does NOT update the in-process OrbisRoot() — a restart is required to apply the change.
bool OrbisSetRoot(const std::string& dir);

// "<OrbisRoot()>/<sub>" when that folder exists, else OrbisRoot().
std::string OrbisDir(const char* sub);
// A switch file: "<root>/flags/<name>" when it's there, else "<root>/<name>" (the old place).
std::string OrbisFlagPath(const char* name);
// Whether that switch is on (the file exists in either place). vk-285-105: from a snapshot once
// OrbisFlagsRefresh has run (main-boot's ticker, once a second), before that from the files.
bool OrbisFlag(const char* name);
// vk-285-105: the snapshot's refresh (the ticker thread), and a small file's contents from the same
// refresh (the first call for a path reads it directly and registers it). False when the file isn't there.
void OrbisFlagsRefresh();
bool OrbisCachedRead(const char* path, std::string& out);
// Where a log or diagnostic file goes: logs/<name>, or the top folder without logs/.
std::string OrbisLogPath(const char* name);
// pf.log's path for the page-fault handler (no allocation there); main-boot sets it at start.
extern "C" char g_orbis_pf_log[160];
