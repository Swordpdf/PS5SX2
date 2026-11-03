// PS5SX2 (vk-285-179, AI-assisted): the app loads the PS5SX2 Disc Helper itself at start, the way PS5SX3 does - it sends
// the helper ELF shipped in its own title folder to the console's ELF loader (127.0.0.1:9021). So the user no longer has
// to load the helper by hand.
//
// The decision (Decide / RevCompare) is kept pure here so the PC test (tests/helperload) can drive it; the socket send and
// the status/ELF reading live in OrbisHelperAutoload.cpp. The helper writes /data/PCSX2/logs/ps5sx2-helper.txt every
// second (disctile.c write_helper_status): "pid=<n>\ntime=<unix>\nrev=<PAYLOAD_REV>\n".
// 187: one helper runs at a time, and the running one stays: the helper's host takes a lock (/data/PS5SXHelper.lock,
// helper-host/main*.c) and a newly sent helper EXITS at once when another copy holds it ("another copy is resident"; this
// comment used to say the new one takes over). So a newer shipped helper replaces an older running one only after a console
// restart: the app then sends nothing and tells the person once (OrbisHelperAutoload.cpp).
//
// Flag file flags/helper_noautoload: the app sends nothing (the user loads the helper by hand, as before).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// 180: sends a whole ELF to the console's ELF loader on 127.0.0.1:9021 and closes the sending side (OrbisHelperAutoload.cpp; the
// helper's start and the PS5SX2 Installer's, OrbisInstallerStart.cpp). True when the loader took all of it; `detail` says what
// happened, for boot.log; `no_loader` (may be null) is set when nothing answered on the port.
bool OrbisLoaderSend(const std::vector<unsigned char>& data, std::string& detail, bool* no_loader);

namespace orbis_helper
{
// The revision the app ships (the helper source's PAYLOAD_REV). Bump together with the helper.
// 187: only what the app falls back to: the revision is read from the shipped ELF itself (RevFromElf). It said 171k while
// 171o shipped, so a running 171l..171n counted as newer and 171o was never sent. Makefile.vk stops the build when this is
// not ps5/dischelper/disctile.c's PAYLOAD_REV.
inline constexpr const char* kShippedRev = "171p"; // 171p: build 187 (no automatic unmount by the no-mount helper)

// 187: the PAYLOAD_REV of a helper ELF, from the line its disc payload logs at start ("disc payload <rev>", a string literal
// in disctile.c's payload_progress call, kept by the strip). "" when the ELF has no such text.
inline std::string RevFromElf(const unsigned char* data, size_t size)
{
	static const char kTag[] = "disc payload ";
	const size_t tl = sizeof(kTag) - 1;
	for (size_t i = 0; data && i + tl < size; i++)
	{
		if (data[i] != 'd' || std::memcmp(data + i, kTag, tl) != 0)
			continue;
		size_t j = i + tl;
		std::string rev;
		while (j < size && rev.size() < 16 && ((data[j] >= '0' && data[j] <= '9') || (data[j] >= 'a' && data[j] <= 'z')))
			rev.push_back(static_cast<char>(data[j++]));
		if (!rev.empty() && rev[0] >= '0' && rev[0] <= '9' && j < size && data[j] == '\0')
			return rev;
	}
	return {};
}

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
	LeaveBusy,   // an older helper runs but is mid-copy: do not replace it now
	LeaveOlder   // 187: an older (or unknown) helper runs: one sent now would exit at once, the shipped one loads after a restart
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
	case Decision::LeaveOlder: return "leave (an older one runs: the shipped one after a console restart)";
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
	// 187: until 186 this sent the shipped helper ("replacing it"), which then exited at once on the running one's lock (and next
	// to a disc payload without the host, e.g. PS5SX2-Disc.elf, two payloads would have acted on every disc)
	why = "an older/unknown helper runs: a helper sent now would exit at once (one helper at a time, the running one holds the lock): "
		  "the shipped one is loaded at the first start after a console restart";
	return Decision::LeaveOlder;
}
} // namespace orbis_helper
