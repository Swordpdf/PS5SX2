// PS5SX2 (vk-285-179, AI-assisted): PC test of the helper auto-load decision (OrbisHelperAutoload.h).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "OrbisHelperAutoload.h"

#include <cstdio>
#include <string>

using namespace orbis_helper;

static int s_failed = 0;
static void Check(bool ok, const char* what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		s_failed++;
}

static Status Running(const char* rev, bool alive = true, bool busy = false)
{
	Status s;
	s.alive = alive;
	s.pid = 4321;
	s.rev = rev ? rev : "";
	s.busy = busy;
	return s;
}

int main()
{
	// RevCompare
	Check(RevCompare("171j", "171j") == 0, "RevCompare: same revision is 0");
	Check(RevCompare("171i", "171j") < 0, "RevCompare: 171i is older than 171j");
	Check(RevCompare("171k", "171j") > 0, "RevCompare: 171k is newer than 171j");
	Check(RevCompare("180", "171j") > 0, "RevCompare: 180 is newer than 171j");
	Check(RevCompare("171", "171j") < 0, "RevCompare: 171 (no suffix) is older than 171j");
	Check(RevCompare("", "171j") == INT_MIN, "RevCompare: empty is unknown");
	Check(RevCompare("junk", "171j") == INT_MIN, "RevCompare: garbled is unknown");

	std::string why;
	// no flag, no helper running, ELF present -> load
	Check(Decide(true, false, true, Running(nullptr, false), "171j", why) == Decision::Load, "no helper running -> load");
	// flag set -> skip
	Check(Decide(true, true, true, Running("171j"), "171j", why) == Decision::Skip, "flag helper_noautoload -> skip");
	// no ELF -> skip
	Check(Decide(true, false, false, Running(nullptr, false), "171j", why) == Decision::Skip, "no ELF to send -> skip");
	// same revision running -> leave
	Check(Decide(true, false, true, Running("171j"), "171j", why) == Decision::LeaveSame, "same revision runs -> leave");
	// newer running -> leave
	Check(Decide(true, false, true, Running("171k"), "171j", why) == Decision::LeaveNewer, "newer helper runs -> leave");
	Check(Decide(true, false, true, Running("200"), "171j", why) == Decision::LeaveNewer, "much newer helper runs -> leave");
	// older running, idle -> load
	Check(Decide(true, false, true, Running("171i"), "171j", why) == Decision::Load, "older helper, idle -> load (replace)");
	// older running, busy copying -> leave (do not interrupt the copy)
	Check(Decide(true, false, true, Running("171i", true, true), "171j", why) == Decision::LeaveBusy, "older helper, copying -> leave");
	// unknown revision running, idle -> load
	Check(Decide(true, false, true, Running("", true), "171j", why) == Decision::Load, "unknown revision, idle -> load");

	std::printf(s_failed ? "FAILED: %d\n" : "ALL PASSED: %d failed\n", s_failed);
	return s_failed ? 1 : 0;
}
