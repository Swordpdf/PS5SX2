// PS5SX2 (2.11, AI-assisted): the relaunch payload for "back to the menu" (L1 + R1 + Touch). A tiny program the console's ELF loader runs in a process of
// its own: it waits until this app has been closed by the system and then starts the main app again, which opens the shelf. See relaunch/relaunch.c,
// ProsperoRelaunch.cpp and orbis-shims/OrbisLeavePlan.h (how the app leaves).
#pragma once

#include "OrbisLeavePlan.h"

#include <string>

// True when this build embeds the payload.
bool OrbisRelaunchBuilt();

// 176: the payload's timing, written into its request (relaunch/relaunch.c): the wait after the shell has forgotten the closing title, the earliest launch
// after the closing process' exit, the pause between refused launches. The defaults are orbis_leave's (the flag menusettle sets settle_ms).
struct OrbisRelaunchTiming
{
	int settle_ms = orbis_leave::kSettleDefaultMs;
	int min_exit_ms = orbis_leave::kMinExitMs;
	int retry_ms = orbis_leave::kRetryMs;
	// fix180: GUARD mode (relaunch/relaunch_guard.h) when > 0: the app leaves by its own exec; the payload launches `launch_title` only when no new process
	// says it is up (OrbisRelaunchConfirmCameBack) guard_ms after the old one went (hard_ms while another app process is there).
	int guard_ms = 0;
	int hard_ms = 0;
};

// Sends the payload to the ELF loader (127.0.0.1:9021) and waits (at most about 3 s) until it says it is running ("<state>.started"). `closing_title` is
// the title the system will close (the disc tile's ID, "PPSA99203" for the main app, "" when not known: the payload then goes by the running big app),
// `launch_title` the title to start afterwards, `pid` this process. True when the payload is up: the caller may then leave through the plain close
// (LoadExec("exit")). False (`why` says what): nothing is left running that would start anything, the caller keeps the old way out.
bool OrbisRelaunchStart(const char* closing_title, const char* launch_title, int pid, std::string& why, const OrbisRelaunchTiming& timing = OrbisRelaunchTiming());

// Takes the request back (a note the payload checks every 100 ms until it launches): used when the close was refused and the app stays up. Harmless
// without a request, and more than once.
void OrbisRelaunchCancel();

// fix180: called once at start, as soon as /data can be written (main-boot.cpp, after the jailbreak): a guard left by the process this one replaces
// (logs/menu-guard.txt, at most 2 minutes old) is told that the app came back ("<state>.alive"), so it launches nothing. Returns a line for boot.log
// ("" when there was no guard).
std::string OrbisRelaunchConfirmCameBack();

// fix180: the last request's lines of logs/relaunch.log (the payload's steps of the previous "back to the menu"), for boot.log: a session report then shows
// which way out worked. "" when there is none.
std::string OrbisRelaunchLastLog(size_t max_lines = 24);
