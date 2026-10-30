// PS5SX2 (fix180, AI-assisted): the relaunch payload's GUARD mode, the decision alone (C, no I/O), so that relaunch.c and the PC tests share it.
//
// Owner, 2026-10-30: "L1 + R1 + Touchpad during a game must ALWAYS return to the PS5SX2 shelf, never to the XMB". The main app leaves "back to the menu"
// by the exec of its own eboot (OrbisLeavePlan.h, kMainAppExecByDefault: instant, no home screen). On vk-285-180 that exec was accepted and then nothing
// came back, twice on one console (boot.log ends at "[menu] re-executing /data/homebrew/PPSA99203/eboot.bin"; the next start was the player's, 125 s and
// 43 s later; on 179 the same console was back on the shelf in 3 s). Why the new process does not come up is not known from the logs (it left none).
// So the exec is no longer alone: before it, the app starts the relaunch payload in guard mode (request field guard_ms). The payload waits for the old
// process to go; the new process, once it can write to /data, creates "<state>.alive" (ProsperoRelaunch.cpp OrbisRelaunchConfirmCameBack). Came back:
// the payload launches nothing. Not back within guard_ms of the old process going: the payload starts PPSA99203 itself (sceLncUtilLaunchApp, as for a
// tile's leave), so the player lands on the shelf after a moment of the home screen instead of staying there. While another app process is there (the new
// one, starting slowly) it waits up to hard_ms before it launches (a launch then only answers "already running").
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef RELAUNCH_GUARD_H
#define RELAUNCH_GUARD_H

enum
{
	RG_WAIT = 0,      // look again in 100 ms
	RG_CAME_BACK = 1, // the new process said it is up: nothing to launch
	RG_LAUNCH = 2,    // it did not come back: launch the app
};

#define RG_GUARD_DEFAULT_MS 7000  // 179's console: the shelf's first frame 3.5 s after the press, the new process at +3.2 s
#define RG_HARD_DEFAULT_MS 20000  // a new app process that is there but has not said so yet: waited for this long at most

// alive: "<state>.alive" exists. other: another app process (eboot.bin, not the old pid) is running: 1 yes, 0 no, -1 cannot tell. since_gone_ms: how long
// ago the old process was seen gone. guard_ms / hard_ms: the request's (hard_ms below guard_ms counts as guard_ms).
static inline int rg_step(int alive, int other, long long since_gone_ms, int guard_ms, int hard_ms)
{
	if (alive)
		return RG_CAME_BACK;
	if (since_gone_ms < guard_ms)
		return RG_WAIT;
	if (other == 1 && since_gone_ms < hard_ms)
		return RG_WAIT;
	return RG_LAUNCH;
}

// The request's values, bounded: guard 1..30 s (0 = no guard), hard guard..60 s.
static inline int rg_clamp_guard(int ms)
{
	return ms <= 0 ? 0 : ms < 1000 ? 1000 : ms > 30000 ? 30000 : ms;
}
static inline int rg_clamp_hard(int ms, int guard_ms)
{
	if (ms <= 0)
		ms = RG_HARD_DEFAULT_MS;
	if (ms < guard_ms)
		ms = guard_ms;
	return ms > 60000 ? 60000 : ms;
}

#endif
