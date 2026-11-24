// PS5SX2 (2.11, AI-assisted): when the app honours the disc helper's "exec" signal (logs/disc-launch-exec.txt) and when it does not, kept apart from the
// PS5 calls so a PC test can run every case.
//
// Testers (via swordpdf): "after the game is dumped it wants to start, but the disc stops spinning, the PS5 recognizes there is a disc inside and
// aborts, and jumps back to the XMB instead". No log has the end of a copy, so the cause is not proven. What the code does at the end of a copy
// when the disc was put in while PS5SX2 was open is two starts: the watcher (ProsperoDiscDump.cpp) starts the game in this process at once, and about
// a second later the disc helper (the payload that runs beside the app) sees the finished copy too and asks the app, with logs/disc-launch-exec.txt,
// to re-execute its own eboot to come to the front. The watcher answered that with sceSystemServiceLoadExec about 3 s later: in the middle of the
// shelf's shut-down or the game's start, killing the game that was starting. The signal is still the only thing that starts a PS2 CD or a USB DVD
// the helper copied itself (the app never sees those discs), and it still serves the idle shelf, so it is not simply dropped: it is honoured unless a
// game is already being started or runs in this process (nothing to bring to the front: the helper's focus kick does that), or this process is a
// disc tile (a copy of the app under another title ID, where an exec of PPSA99203's eboot is the cross-title exec that ends at the home screen).
// Flag discexec: the old behaviour (every signal honoured, except in a tile). Flag nodiscexec: every signal ignored (for a test round).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <unistd.h>

namespace orbis_disc
{
enum class ExecVerdict
{
	Honor,              // start disc-launch.txt's game: from the shelf in this process when it is up (2026-11-24, ProsperoDiscDump.cpp), else by
	                    // re-executing the app's own eboot (the new process reads disc-launch.txt); the helper brings the app to the front
	IgnoreTile,         // this process is a disc tile
	IgnoreGameStarting, // a game is being started, or runs, in this process
	IgnoreFlag,         // flag nodiscexec
};

struct ExecIn
{
	bool is_tile;         // this process was started from a disc tile (main-boot: its marker was seen and names a drive)
	bool game_starting;   // a game is being started or runs in this process
	bool flag_discexec;   // flags/discexec: the old behaviour (every signal honoured, except in a tile)
	bool flag_nodiscexec; // flags/nodiscexec: every signal ignored
};

// In this order: a tile never execs (whatever the flags say), discexec (the old rule), nodiscexec, a game that is starting, else honour.
constexpr ExecVerdict DecideExec(const ExecIn& in)
{
	if (in.is_tile)
		return ExecVerdict::IgnoreTile;
	if (in.flag_discexec)
		return ExecVerdict::Honor;
	if (in.flag_nodiscexec)
		return ExecVerdict::IgnoreFlag;
	if (in.game_starting)
		return ExecVerdict::IgnoreGameStarting;
	return ExecVerdict::Honor;
}

constexpr const char* ExecVerdictText(ExecVerdict v)
{
	switch (v)
	{
		case ExecVerdict::Honor: return "honoured: the game starts (from the shelf in this process when it is up, else by a self-exec)";
		case ExecVerdict::IgnoreTile: return "ignored: this is a disc tile";
		case ExecVerdict::IgnoreGameStarting: return "ignored: a game is already starting here";
		case ExecVerdict::IgnoreFlag: return "ignored: flag nodiscexec";
	}
	return "?";
}

// disc-launch.txt older than `max_age` seconds is a leftover (an mtime of 0, or one in the future, counts as fresh: the clock is not trusted to condemn a file).
constexpr bool StaleLaunchFile(long long now, long long mtime, long long max_age = 600)
{
	return mtime > 0 && now >= mtime && now - mtime > max_age;
}

// An ignored signal takes the launch note with it: left behind, it would start the game at the next manual start of the app and skip the shelf.
inline void RemoveSignalFiles(const std::string& launch_txt, const std::string& exec_txt)
{
	unlink(exec_txt.c_str());
	unlink(launch_txt.c_str());
}
} // namespace orbis_disc
