// PS5SX2 (vk-285-179, AI-assisted): the app loads the PS5SX2 Disc Helper itself at start, the way PS5SX3 does - it sends
// the helper ELF shipped in its own title folder to the console's ELF loader (127.0.0.1:9021). So the user no longer has
// to load the helper by hand.
//
// The decision (Decide / RevCompare) is kept pure here so the PC test (tests/helperload) can drive it; the socket send and
// the status/ELF reading live in OrbisHelperAutoload.cpp. The helper writes /data/PCSX2/logs/ps5sx2-helper.txt every
// second (disctile.c write_helper_status): "pid=<n>\ntime=<unix>\nrev=<PAYLOAD_REV>\n". The helper has its own
// one-instance rule (a newly sent helper kills an older one and takes over; disc_launch.c / disctile.c), so the app only
// has to decide whether to send at all.
//
// Flag file flags/helper_noautoload: the app sends nothing (the user loads the helper by hand, as before).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <climits>
#include <cstdlib>
#include <string>
#include <vector>

// 180: sends a whole ELF to the console's ELF loader on 127.0.0.1:9021 and closes the sending side (OrbisHelperAutoload.cpp; the
// helper's start and the PS5SX2 Installer's, OrbisInstallerStart.cpp). True when the loader took all of it; `detail` says what
// happened, for boot.log; `no_loader` (may be null) is set when nothing answered on the port.
bool OrbisLoaderSend(const std::vector<unsigned char>& data, std::string& detail, bool* no_loader);

namespace orbis_helper
{
// The revision the app ships (the helper source's PAYLOAD_REV). Bump together with the helper.
inline constexpr const char* kShippedRev = "171k"; // 171k: next to the PS5SX3 helper (ps5/disclaunch/disc_compat.h)

// Compare two revision strings like "171i" / "171j" / "180": the leading integer first, then the suffix lexically.
// < 0 running older, 0 same, > 0 running newer; INT_MIN when either is empty/garbled (treated as "older/unknown").
inline int RevCompare(const char* running, const char* shipped)
{
	if (!running || !*running || !shipped || !*shipped)
		return INT_MIN;
	char* re = nullptr;
	char* se = nullptr;
	const long rn = std::strtol(running, &re, 10);
	const long sn = std::strtol(shipped, &se, 10);
	if (re == running || se == shipped)
		return INT_MIN; // not a revision we understand
	if (rn != sn)
		return rn < sn ? -1 : 1;
	// same leading number: compare the suffix ("", "i", "j", ...) lexically
	const int c = std::string(re).compare(se);
	return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

struct Status
{
	bool alive = false; // a fresh heartbeat (within the freshness window)
	int pid = 0;
	long long age_s = 0; // how old the heartbeat is
	std::string rev;     // the running helper's PAYLOAD_REV, "" when unknown
	bool busy = false;   // a copy is in progress (don't replace it)
};

enum class Decision
{
	Skip,        // flag set, or no ELF to send
	Load,        // send the helper
	LeaveSame,   // the shipped revision already runs
	LeaveNewer,  // a newer helper runs (sent from a PC or a newer title): keep it
	LeaveBusy    // an older helper runs but is mid-copy: do not replace it now
};

inline const char* DecisionName(Decision d)
{
	switch (d)
	{
	case Decision::Skip: return "skip";
	case Decision::Load: return "load";
	case Decision::LeaveSame: return "leave (same revision)";
	case Decision::LeaveNewer: return "leave (newer runs)";
	case Decision::LeaveBusy: return "leave (a copy is running)";
	}
	return "?";
}

// `enabled`: the auto-load feature is on (false only in a build/test that disables it). `noautoload`: the flag file.
// `have_elf`: the app found a helper ELF to send. `st`: the running helper, if any. `shipped_rev`: what the app ships.
inline Decision Decide(bool enabled, bool noautoload, bool have_elf, const Status& st, const char* shipped_rev, std::string& why)
{
	if (!enabled || noautoload)
	{
		why = noautoload ? "flag helper_noautoload: the app loads no helper" : "auto-load off";
		return Decision::Skip;
	}
	if (!have_elf)
	{
		why = "no helper ELF shipped with the app (nothing to send)";
		return Decision::Skip;
	}
	if (!st.alive)
	{
		why = "no helper is running: sending the shipped one";
		return Decision::Load;
	}
	const int cmp = RevCompare(st.rev.c_str(), shipped_rev);
	if (cmp == 0)
	{
		why = "the shipped revision already runs";
		return Decision::LeaveSame;
	}
	if (cmp > 0)
	{
		why = "a newer helper runs: kept";
		return Decision::LeaveNewer;
	}
	// older or unknown
	if (st.busy)
	{
		why = "an older helper runs but a copy is in progress: not replaced now";
		return Decision::LeaveBusy;
	}
	why = "an older/unknown helper runs and is idle: replacing it";
	return Decision::Load;
}
} // namespace orbis_helper
