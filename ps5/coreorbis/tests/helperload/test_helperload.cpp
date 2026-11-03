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
	// 187: older running, idle -> leave (a helper sent now would exit at once on the running one's lock; the shipped one after a restart)
	Check(Decide(true, false, true, Running("171i"), "171j", why) == Decision::LeaveOlder, "older helper, idle -> leave (restart for the new one)");
	// older running, busy copying -> leave (do not interrupt the copy)
	Check(Decide(true, false, true, Running("171i", true, true), "171j", why) == Decision::LeaveBusy, "older helper, copying -> leave");
	// 187: unknown revision running, idle -> leave as well (never two payloads at once)
	Check(Decide(true, false, true, Running("", true), "171j", why) == Decision::LeaveOlder, "unknown revision, idle -> leave");
	// a heartbeat that is not fresh: nothing runs -> load
	Check(Decide(true, false, true, Running("171i", false), "171j", why) == Decision::Load, "older helper's stale heartbeat -> load");

	// 187: RevFromElf, the revision in the shipped ELF ("disc payload <rev>" and its NUL)
	auto rev = [](const char* text, size_t n) { return RevFromElf(reinterpret_cast<const unsigned char*>(text), n); };
	static const char kElf[] = "\x7f" "ELF....disc payload \0....disc payload 171p\0more";
	Check(rev(kElf, sizeof(kElf)) == "171p", "RevFromElf: finds 171p after a tag with no revision");
	static const char kText[] = "disc payload 171p is running";
	Check(rev(kText, sizeof(kText)).empty(), "RevFromElf: a revision not ended by its NUL is not one");
	static const char kWord[] = "disc payload abc";
	Check(rev(kWord, sizeof(kWord)).empty(), "RevFromElf: a revision must start with a digit");
	static const char kEnd[] = {'d', 'i', 's', 'c', ' ', 'p', 'a', 'y', 'l', 'o', 'a', 'd', ' ', '1', '7', '1'};
	Check(rev(kEnd, sizeof(kEnd)).empty(), "RevFromElf: cut off at the end of the data: nothing, and no read past it");
	Check(RevFromElf(nullptr, 0).empty(), "RevFromElf: no data");
	Check(RevCompare(rev(kElf, sizeof(kElf)).c_str(), kShippedRev) == 0, "kShippedRev is 171p (the helper this build ships)");

	std::printf(s_failed ? "FAILED: %d\n" : "ALL PASSED: %d failed\n", s_failed);
	return s_failed ? 1 : 0;
}
