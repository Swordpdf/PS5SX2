// PS5SX2 (2.11, AI-assisted): how "back to the menu" (L1 + R1 + Touch) leaves the process, kept apart from the PS5 calls so a PC test can run every branch.
//
// swordpdf / testers: "L1 + R1 + Touch sometimes crashes the console, it should always return you to the shelf". The cause is not proven (no log has a
// crash with a "[menu]" line in it), but the old way out is the least certain operation the app has: sceSystemServiceLoadExec(<own eboot>) asks the
// shell to kill this process and spawn a new one of the same title while the helper and ShadowMount+ react to both, and since 2.11 a disc tile runs
// the shelf in the tile's identity and re-executes the TILE's eboot. The way out now is the one that is known to be harmless here, the plain close
// (LoadExec("exit"), as the Reload tile and every game-less start use), plus a tiny payload (relaunch/relaunch.c, started through the console's ELF
// loader before the close) that waits until the shell is done with this process and starts the main app again, the way its tile does. The old
// in-process exec stays as the fall back for a console with no loader, and the flags menuexec (the old way, exactly) and menuclose (the close only)
// choose a way by hand.
//
// 176 (AI-assisted; swordpdf: "L1 + R1 + Touch briefly goes to the XMB, and then the shelf, it should be the shelf instantly"): nothing that works from
// outside the process can be instant. The system closes the app, shows the home screen, and only then can a payload start the app again (2-3 s of shell
// launch lead time on top of the close). Only the old exec is instant, because the shell answers it with a spawn into the same title. So the MAIN app
// (not a disc tile: the shelf lives in the main title, a tile cannot reach it by an exec) has a gate of its own, MainAppExec(): flag menufast = the old
// exec, flag menurelaunch = the payload, and kMainAppExecByDefault below for neither. It is TRUE: the owner chose the instant exec for the main app
// (2026-10-16, in chat, knowing that the exec is what "sometimes crashed the console" in the reports; the cause was never proven). A disc tile never execs
// into the main app, it keeps the payload. If testers' leave.txt shows crashes after "back to the menu", set this back to false (one line plus the pin in
// tests/leaveplan) or tell them to put the flag file menurelaunch into /data/PCSX2/flags/.
// The payload's timing is chosen here too (SettleMs, kMinExitMs, kRetryMs); relaunch/relaunch.c gets it through ProsperoRelaunch.cpp.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace orbis_leave
{
// 176: the one place that says whether the main app leaves by the old exec (instant, no home screen) when neither flag is there. true = the old exec (the
// owner's choice); false = the relaunch payload (a flash of the home screen, the 175 way). The flags menufast (exec) and menurelaunch (payload) decide by
// hand whatever this says.
// fix181 (owner, 2026-10-30: "L1 + R1 + Touchpad must ALWAYS return to the PS5SX2 shelf"): FALSE now. On vk-285-180/181 the exec never came back in time:
// 181's reports: of the main app's exits whose next start is in a report (3, one console, the guard up), the old process went 0.34 s after the exec and
// NO new process came within 7 s in any (one after 6.6 s, the guard then waited; two were started by the guard at 8.0-8.1 s, the shelf 9.8 s after the
// press; without
// the guard, 180, nothing came at all). The relay from a game tile on the same build: PPSA99203 running 2.3 s after the exit, the shelf 3.5 s after the
// press, every time. So the main app leaves by the relay too (a moment of the home screen, then the shelf), and the exec is the fall back when the
// payload can't be sent (and the flag menufast, which keeps the guard).
constexpr bool kMainAppExecByDefault = false;

// 176: the payload's timing. The settle is how long it waits after the shell has forgotten the closing title (175 had a fixed 1500 ms; the flag menusettle
// sets it, 0..kSettleMaxMs). kMinExitMs: it never launches earlier than this long after the closing PROCESS exited, whatever the settle is: ShadowMount+
// takes an app that starts inside the first second after its exit for a handoff of the same app (a branch no log has ever run), and on a console that
// forgets the title the moment the process ends, a short settle alone would land there (PC probe: 0.80 s after the exit with a 700 ms settle, 1.70 s with
// this). kRetryMs is the pause between two launch attempts that were refused (175 had 800 ms).
constexpr int kSettleDefaultMs = 1000;
constexpr int kSettleMaxMs = 5000;
constexpr int kMinExitMs = 1600;
constexpr int kRetryMs = 400;

constexpr bool IsBlank(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// One whole number off the front of `p` (blanks, an optional '-', digits, up to the next blank or the end); `p` moves past it. False for anything else.
// The value stops growing at about 10^12, so a long run of digits cannot overflow.
constexpr bool TakeNumber(const char*& p, long long& out)
{
	while (IsBlank(*p))
		p++;
	bool negative = false;
	if (*p == '-')
	{
		negative = true;
		p++;
	}
	if (*p < '0' || *p > '9')
		return false;
	long long v = 0;
	while (*p >= '0' && *p <= '9')
	{
		if (v < 1000000000000LL)
			v = v * 10 + (*p - '0');
		p++;
	}
	if (*p != '\0' && !IsBlank(*p))
		return false;
	out = negative ? -v : v;
	return true;
}

// The flag menusettle's content (nullptr = there is no such flag): the first number in it is the settle in ms, clamped to 0..kSettleMaxMs. No flag, an
// empty file or anything that is not a number gives the default.
constexpr int SettleMs(const char* text)
{
	if (!text)
		return kSettleDefaultMs;
	long long v = 0;
	if (!TakeNumber(text, v))
		return kSettleDefaultMs;
	return v < 0 ? 0 : v > kSettleMaxMs ? kSettleMaxMs : static_cast<int>(v);
}

// logs/open-shelf.txt, the note for the next start: "<time(NULL)> <wall clock ms when the exit was issued> <wall clock ms of the press> <title>". The 175
// file had the first number only; the other two are 0 then. The new process prints how long ago the other two were (the whole gap from the press to its
// main(), in boot.log and settings.log).
// fix180 (tester on 180: "after the first time, sometimes the tile just opens PS5SX2 to the game's cover, but doesn't boot the game; I need to close
// everything and reopen"): the fourth field is the title the leave starts (TargetOf). A disc tile's console log: "back to the menu" in the main app at
// 23:37:18 (its own exec), no new process; the disc tile pressed 43 s later took the note (fresh for 3 minutes) and opened the shelf; closed and pressed
// again 11 s later, it started the disc. Any start within 3 minutes took a note that was left for another title. Now a start takes only the note
// addressed to it (NoteIsFor): a tile press after a leave of the main app, or of another tile, starts its game.
constexpr const char* kMainTitle = "PPSA99203";
constexpr int kTitleMax = 16;

struct ShelfMark
{
	long long sec = 0;
	long long mark_ms = 0;  // when the exit was issued
	long long press_ms = 0; // when "back to the menu" was pressed (the first note of the leave, for a start that failed)
	char target[kTitleMax] = {}; // fix180: the title the leave starts ("" = a note of 2.11 to 180, which had none)
};

constexpr bool IsTitleChar(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

constexpr bool ParseShelfMark(const char* text, ShelfMark& out)
{
	out = ShelfMark{};
	if (!text || !TakeNumber(text, out.sec))
		return false;
	if (!TakeNumber(text, out.mark_ms))
		return true;
	if (!TakeNumber(text, out.press_ms))
	{
		out.press_ms = 0;
		return true;
	}
	// fix180: the title, when there is one (letters, digits, '_' and '-', shorter than kTitleMax); anything else leaves it out
	while (IsBlank(*text))
		text++;
	int n = 0;
	while (IsTitleChar(text[n]) && n < kTitleMax - 1)
		n++;
	if (n > 0 && (text[n] == '\0' || IsBlank(text[n])))
		for (int i = 0; i < n; i++)
			out.target[i] = text[i];
	return true;
}

constexpr bool SameText(const char* a, const char* b)
{
	if (!a || !b)
		return false;
	while (*a && *a == *b)
	{
		a++;
		b++;
	}
	return *a == *b;
}

// fix180: whether the note is for this start. `own` = this process's title: kMainTitle for the main app, the tile's folder name (PPSA99231.., PPSA99251..)
// for a disc or game tile, "" for a tile whose folder was not found. A note without a title (2.11 to 180) is the main app's only: a tile press starts its
// game, whatever such a note says.
constexpr bool NoteIsFor(const ShelfMark& mark, const char* own, bool is_tile)
{
	if (mark.target[0] == '\0')
		return !is_tile;
	return own && own[0] != '\0' && SameText(mark.target, own);
}

enum class How
{
	Relaunch,    // the relay payload is up: leave through LoadExec("exit"), the payload starts the main app once this process is gone
	ExecOwnTile, // the old way for a disc tile: LoadExec of the tile's own eboot (then, if that did not take, of the main app's)
	ExecMain,    // the old way for the main app: LoadExec of this app's eboot
	ExitOnly,    // LoadExec("exit"): back to the home screen, nothing starts (the tile and the app's own tile still work)
};

struct In
{
	bool flag_menuexec;    // flag file menuexec: the old behaviour, exactly
	bool flag_menuclose;   // flag file menuclose: close to the home screen only
	bool relay_started;    // the relaunch payload answered its handshake (ProsperoRelaunch.cpp)
	bool is_tile;          // this process was started from a disc tile (its /app0/disc-tile.txt names a drive) or a game tile (2.12: /app0/game-tile.txt)
	bool tile_dir_found;   // and its folder under /data/homebrew was found again after the jailbreak
	bool flag_menufast;    // 176, flag file menufast: the main app leaves by the old exec (instant); a disc tile ignores it
	bool flag_menurelaunch; // 176, flag file menurelaunch: the main app leaves by the relaunch payload, whatever kMainAppExecByDefault says
};

// 176: the main app (never a disc tile) leaves by the old exec of its own eboot instead of the relaunch payload: menufast, or the default when
// menurelaunch is not there. menuexec and menuclose are decided before this in Decide() and need not be asked here.
constexpr bool MainAppExec(const In& in)
{
	return !in.is_tile && !in.flag_menurelaunch && (in.flag_menufast || kMainAppExecByDefault);
}

// 176: whether the relaunch payload is worth starting at all (it takes up to 3 s before the leave): not for the old ways out. Exactly the inputs for which
// Decide() answers Relaunch once the payload has answered (relay_started is not read).
constexpr bool WantRelay(const In& in)
{
	return !in.flag_menuclose && !in.flag_menuexec && !MainAppExec(in);
}

// fix180 (owner: "L1 + R1 + Touchpad during a game must ALWAYS return to the PS5SX2 shelf, never to the XMB"): a way out by an exec (the main app's own,
// menufast or the default; the flag menuexec) is no longer alone. On vk-285-180 the main app's exec was accepted and nothing came back, twice on one
// console. So the relaunch payload is started in GUARD mode before it (relaunch/relaunch_guard.h): it launches PPSA99203 only if no new process says it
// is up kGuardMs after the old one went. Not with menuclose (the player asked for the home screen) and not when the relay itself is the way.
constexpr int kGuardMs = 7000;
constexpr int kGuardHardMs = 20000;
constexpr bool WantGuard(const In& in)
{
	return !in.flag_menuclose && !WantRelay(in);
}

// In this order: menuclose, the main app's own exec (menufast / the default), menuexec (the old rule), the relay, and without a relay the tile's own eboot
// or the main app's. A tile whose folder is gone does NOT exec PPSA99203's eboot from inside the tile (a cross-title exec, what ended at the home screen in
// 2.10): it closes.
constexpr How Decide(const In& in)
{
	if (in.flag_menuclose)
		return How::ExitOnly;
	if (MainAppExec(in))
		return How::ExecMain;
	if (in.flag_menuexec)
		return in.is_tile && in.tile_dir_found ? How::ExecOwnTile : How::ExecMain;
	if (in.relay_started)
		return How::Relaunch;
	if (in.is_tile)
		return in.tile_dir_found ? How::ExecOwnTile : How::ExitOnly;
	return How::ExecMain;
}

enum class AfterExec
{
	FallBack,    // LoadExec refused at once: nothing is under way, take the next way
	KeepWaiting, // LoadExec accepted (rc 0): the shell has started the kill, this process merely outlived the wait; never cancel the payload, never exec twice
};

// fix180: the title a way out starts, for the note (open-shelf.txt): the relay and the main app's eboot start PPSA99203, a tile's own eboot that tile
// (`own_tile`, its folder name). The plain close starts nothing: its note is the main app's, as every note was before (a later start of the main app opens
// the shelf and the bootdisc flag does not start the disc again; a tile press starts its game).
constexpr const char* TargetOf(How how, const char* own_tile)
{
	return how == How::ExecOwnTile && own_tile && own_tile[0] != '\0' ? own_tile : kMainTitle;
}

// What to do once sceSystemServiceLoadExec(...) and the wait after it are over and this process still lives. rc is LoadExec's return value.
constexpr AfterExec AfterExecReturn(int rc)
{
	return rc != 0 ? AfterExec::FallBack : AfterExec::KeepWaiting;
}

// The way that follows when the close of a Relaunch (LoadExec("exit")) was refused: the payload is cancelled and the old chain runs without it. ExitOnly
// means nothing is left to try (the close itself was the last way, or the person asked for it): back to the game. Never Relaunch.
constexpr How FallBackTo(const In& in)
{
	In next = in;
	next.relay_started = false;
	return Decide(next);
}
} // namespace orbis_leave
