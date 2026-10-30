// PS5SX2 (2.11, AI-assisted), PC test: orbis-shims/OrbisLeavePlan.h, the choice of how "back to the menu" leaves the process.
// All 128 combinations of the seven inputs: the flags win in their order, the relay is used when it answered, and without it a tile never execs another
// title's eboot (the 2.10 home-screen case). Then the rule for what follows a LoadExec: refused -> the next way, accepted (rc 0) -> keep waiting, never a
// second exec and never a cancelled payload.
// 176: the main app's own gate (flags menufast / menurelaunch, kMainAppExecByDefault): with neither flag every answer is the 175 one, menufast only changes
// the main app, WantRelay says exactly when the payload is worth starting. And the payload's timing (SettleMs for the flag menusettle) and the note for
// the next start (ParseShelfMark). fix180: the note's title (TargetOf) and which start takes it (NoteIsFor).
//   ps5/coreorbis/tests/leaveplan/test-leaveplan.sh
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../orbis-shims/OrbisLeavePlan.h"

#include <cstdio>
#include <string>

using orbis_leave::AfterExec;
using orbis_leave::How;
using orbis_leave::In;

static int fails = 0;
static int checks = 0;
static void Check(bool ok, const std::string& what)
{
	checks++;
	if (!ok)
	{
		fails++;
		std::printf("FAIL  %s\n", what.c_str());
	}
}

static const char* Name(How h)
{
	switch (h)
	{
		case How::Relaunch: return "Relaunch";
		case How::ExecOwnTile: return "ExecOwnTile";
		case How::ExecMain: return "ExecMain";
		case How::ExitOnly: return "ExitOnly";
	}
	return "?";
}

static In FromBits(int bits)
{
	In in = {};
	in.flag_menuexec = bits & 1;
	in.flag_menuclose = bits & 2;
	in.relay_started = bits & 4;
	in.is_tile = bits & 8;
	in.tile_dir_found = bits & 16;
	in.flag_menufast = bits & 32;
	in.flag_menurelaunch = bits & 64;
	return in;
}

// the old rule of main-boot.cpp (2.11 before this change), which the flag menuexec must reproduce
static How OldRule(const In& in)
{
	return in.is_tile && in.tile_dir_found ? How::ExecOwnTile : How::ExecMain;
}

// 175's Decide(), copied: with no menufast and no menurelaunch (and the default off) the new one must answer the same for every input
static How Decide175(const In& in)
{
	if (in.flag_menuclose)
		return How::ExitOnly;
	if (in.flag_menuexec)
		return OldRule(in);
	if (in.relay_started)
		return How::Relaunch;
	if (in.is_tile)
		return in.tile_dir_found ? How::ExecOwnTile : How::ExitOnly;
	return How::ExecMain;
}

static void CheckSettle(const char* text, int want)
{
	const int got = orbis_leave::SettleMs(text);
	char label[160];
	std::snprintf(label, sizeof(label), "SettleMs(%s) = %d, wanted %d", text ? ("\"" + std::string(text) + "\"").c_str() : "no flag", got, want);
	Check(got == want, label);
}

int main()
{
	static_assert(orbis_leave::Decide(In{}) == How::ExecMain, "the main app without a relay and without flags execs itself (as before)");
	static_assert(orbis_leave::AfterExecReturn(0) == AfterExec::KeepWaiting, "constexpr");
	// The 176 default. Flipping it is a decision of the owner, not a tidy-up: change this line with it (everything below follows the constant).
	// The owner chose the instant exec for the main app (2026-10-16, in chat); fix181: the relay, after the owner's "always the shelf" and 181's logs (the exec
	// never came back in time).
	static_assert(!orbis_leave::kMainAppExecByDefault, "the main app leaves by the relay unless menufast is there (fix181)");
	const bool by_default = orbis_leave::kMainAppExecByDefault;
	static_assert(orbis_leave::kSettleDefaultMs == 1000 && orbis_leave::kSettleMaxMs == 5000, "the settle's default and limit");
	static_assert(orbis_leave::kMinExitMs == 1600 && orbis_leave::kRetryMs == 400, "the payload's earliest launch after the exit, and the pause between refused launches");
	static_assert(orbis_leave::SettleMs(nullptr) == 1000 && orbis_leave::SettleMs("250") == 250, "constexpr");

	for (int bits = 0; bits < 128; bits++)
	{
		const In in = FromBits(bits);
		const How got = orbis_leave::Decide(in);
		char label[260];
		std::snprintf(label, sizeof(label), "menuexec=%d menuclose=%d relay=%d tile=%d dir=%d menufast=%d menurelaunch=%d -> %s", in.flag_menuexec,
			in.flag_menuclose, in.relay_started, in.is_tile, in.tile_dir_found, in.flag_menufast, in.flag_menurelaunch, Name(got));

		// the main app leaves by its own exec (menufast or the default, and not menurelaunch); a tile never does
		const bool main_exec = !in.is_tile && !in.flag_menurelaunch && (in.flag_menufast || by_default);
		How want;
		if (in.flag_menuclose)
			want = How::ExitOnly; // the close only, whatever else is set
		else if (main_exec)
			want = How::ExecMain; // 176: the old exec of this app's eboot, no payload
		else if (in.flag_menuexec)
			want = OldRule(in); // the old behaviour exactly, a relay that is up is not used
		else if (in.relay_started)
			want = How::Relaunch;
		else if (in.is_tile && in.tile_dir_found)
			want = How::ExecOwnTile;
		else if (in.is_tile)
			want = How::ExitOnly; // a tile whose folder is gone: never PPSA99203's eboot from inside the tile
		else
			want = How::ExecMain;
		Check(got == want, std::string(label) + " (wanted " + Name(want) + ")");
		Check(orbis_leave::MainAppExec(in) == main_exec, std::string("MainAppExec: ") + label);

		// the invariants, on their own
		if (in.flag_menuclose)
			Check(got == How::ExitOnly, std::string("menuclose always closes: ") + label);
		if (!in.flag_menuclose && in.flag_menuexec && !main_exec)
			Check(got == OldRule(in), std::string("menuexec is the old rule: ") + label);
		if (!in.relay_started)
			Check(got != How::Relaunch, std::string("no relay, no Relaunch: ") + label);
		if (got == How::Relaunch)
			Check(in.relay_started && !in.flag_menuexec && !in.flag_menuclose && !main_exec, std::string("Relaunch only with a relay and no flag: ") + label);
		if (in.is_tile && !in.tile_dir_found && !in.flag_menuexec)
			Check(got != How::ExecMain && got != How::ExecOwnTile, std::string("a tile without its folder execs nothing by itself: ") + label);

		// 176: with neither new flag (and the default off) every answer is 175's; a tile ignores both
		if (!by_default && !in.flag_menufast && !in.flag_menurelaunch)
			Check(got == Decide175(in), std::string("without menufast and menurelaunch the answer is 175's: ") + label);
		if (in.is_tile)
		{
			In without = in;
			without.flag_menufast = false;
			without.flag_menurelaunch = false;
			Check(got == orbis_leave::Decide(without), std::string("a disc tile ignores menufast and menurelaunch: ") + label);
			Check(got == Decide175(in), std::string("a disc tile decides as in 175: ") + label);
		}
		if (!in.is_tile && !in.flag_menuclose && !in.flag_menurelaunch && (in.flag_menufast || by_default))
			Check(got == How::ExecMain, std::string("the main app with menufast (or by default) execs its own eboot: ") + label);
		if (!in.is_tile && !in.flag_menuclose && in.flag_menufast && in.flag_menurelaunch)
			Check(got == Decide175(in), std::string("menurelaunch cancels menufast: ") + label);

		// WantRelay: exactly the inputs for which a payload that answered would be used; relay_started itself is not read
		In answered = in;
		answered.relay_started = true;
		Check(orbis_leave::WantRelay(in) == (orbis_leave::Decide(answered) == How::Relaunch), std::string("WantRelay is when Decide gives Relaunch: ") + label);
		In flipped = in;
		flipped.relay_started = !in.relay_started;
		Check(orbis_leave::WantRelay(in) == orbis_leave::WantRelay(flipped), std::string("WantRelay does not read relay_started: ") + label);
		if (in.flag_menuexec || in.flag_menuclose || main_exec)
			Check(!orbis_leave::WantRelay(in), std::string("no payload for the old ways out: ") + label);

		// what follows a refused close: the old chain, never the relay, and a second close is not tried
		const How next = orbis_leave::FallBackTo(in);
		Check(next != How::Relaunch, std::string("the fall back is never Relaunch: ") + label);
		if (got == How::Relaunch)
		{
			In without = in;
			without.relay_started = false;
			Check(next == orbis_leave::Decide(without), std::string("the fall back of a Relaunch is the no-relay decision: ") + label);
			Check(next == (in.is_tile ? (in.tile_dir_found ? How::ExecOwnTile : How::ExitOnly) : How::ExecMain), std::string("the fall back chain: ") + label);
		}
		if (got == How::ExitOnly)
			Check(next == How::ExitOnly, std::string("an ExitOnly has nothing after it: ") + label);
	}

	// the scenarios the owner asked for, by name
	{
		In main_app = {};
		main_app.relay_started = true;
		Check(orbis_leave::Decide(main_app) == (by_default ? How::ExecMain : How::Relaunch) && orbis_leave::WantRelay(main_app) == !by_default,
			"the main app, no flag: the default way (the payload while the default is off)");
		main_app.flag_menufast = true;
		Check(orbis_leave::Decide(main_app) == How::ExecMain && !orbis_leave::WantRelay(main_app), "the main app with menufast: the exec, and no payload is started");
		main_app.flag_menurelaunch = true;
		Check(orbis_leave::Decide(main_app) == How::Relaunch && orbis_leave::WantRelay(main_app), "the main app with menufast and menurelaunch: the payload");
		In tile = {};
		tile.is_tile = true;
		tile.tile_dir_found = true;
		tile.flag_menufast = true;
		tile.relay_started = true;
		Check(orbis_leave::Decide(tile) == How::Relaunch && orbis_leave::WantRelay(tile), "a disc tile with menufast: still the payload");
		tile.relay_started = false;
		Check(orbis_leave::Decide(tile) == How::ExecOwnTile, "a disc tile with menufast and no payload: its own eboot, as in 175");
		tile.tile_dir_found = false;
		Check(orbis_leave::Decide(tile) == How::ExitOnly, "a disc tile with menufast, no payload and no folder: the close");
		In closing = {};
		closing.flag_menufast = true;
		closing.flag_menuclose = true;
		Check(orbis_leave::Decide(closing) == How::ExitOnly && !orbis_leave::WantRelay(closing), "menuclose beats menufast");
		In both = {};
		both.flag_menuexec = true;
		both.flag_menurelaunch = true;
		Check(orbis_leave::Decide(both) == How::ExecMain && !orbis_leave::WantRelay(both), "menuexec beats menurelaunch (the old way, exactly)");
	}

	// AfterExecReturn: refused (any nonzero) -> the next way; accepted (0) -> keep waiting
	Check(orbis_leave::AfterExecReturn(0) == AfterExec::KeepWaiting, "rc 0: keep waiting (the kill is under way)");
	Check(orbis_leave::AfterExecReturn(-1) == AfterExec::FallBack, "rc -1: fall back");
	Check(orbis_leave::AfterExecReturn(1) == AfterExec::FallBack, "rc 1: fall back");
	Check(orbis_leave::AfterExecReturn(static_cast<int>(0x80020016u)) == AfterExec::FallBack, "rc 0x80020016: fall back");
	Check(orbis_leave::AfterExecReturn(static_cast<int>(0x80aa001au)) == AfterExec::FallBack, "rc 0x80aa001a: fall back");

	// SettleMs: the flag menusettle's content in ms, 0..5000; no flag, an empty file or a non-number gives 1000
	CheckSettle(nullptr, 1000);
	CheckSettle("", 1000);
	CheckSettle("   \n", 1000);
	CheckSettle("abc", 1000);
	CheckSettle("12abc", 1000);
	CheckSettle("1e3", 1000);
	CheckSettle("0x10", 1000);
	CheckSettle("+5", 1000);
	CheckSettle("-", 1000);
	CheckSettle("-5", 0);
	CheckSettle("-99999", 0);
	CheckSettle("0", 0);
	CheckSettle("250", 250);
	CheckSettle("250\n", 250);
	CheckSettle("250\r\n", 250);
	CheckSettle("  1000  ", 1000);
	CheckSettle("300 ms", 300);
	CheckSettle("1000", 1000);
	CheckSettle("1500", 1500);
	CheckSettle("5000", 5000);
	CheckSettle("5001", 5000);
	CheckSettle("99999", 5000);
	CheckSettle("99999999999999999999999999", 5000);
	CheckSettle("007", 7);

	// the note for the next start: "<time> <wall ms at the exit> <wall ms of the press>"; a 175 file has the time only
	{
		orbis_leave::ShelfMark mark;
		Check(orbis_leave::ParseShelfMark("1760000000\n", mark) && mark.sec == 1760000000 && mark.mark_ms == 0 && mark.press_ms == 0, "a 175 note: the time only");
		Check(orbis_leave::ParseShelfMark("1760000000 1760000000123 1760000000050\n", mark) && mark.sec == 1760000000 && mark.mark_ms == 1760000000123LL &&
		          mark.press_ms == 1760000000050LL,
			"a 176 note: three numbers");
		Check(orbis_leave::ParseShelfMark("1760000000 1760000000123\n", mark) && mark.mark_ms == 1760000000123LL && mark.press_ms == 0, "two numbers: no press time");
		Check(orbis_leave::ParseShelfMark("12 abc", mark) && mark.sec == 12 && mark.mark_ms == 0 && mark.press_ms == 0, "a second field that is no number is left out");
		Check(orbis_leave::ParseShelfMark("12 34 xyz", mark) && mark.sec == 12 && mark.mark_ms == 34 && mark.press_ms == 0, "a third field that is no number is left out");
		Check(!orbis_leave::ParseShelfMark("", mark) && mark.sec == 0, "an empty file is no note");
		Check(!orbis_leave::ParseShelfMark("abc", mark), "garbage is no note");
		Check(!orbis_leave::ParseShelfMark(nullptr, mark), "no text is no note");
		char written[96];
		std::snprintf(written, sizeof(written), "%lld %lld %lld %s\n", 1760000123LL, 1760000123456LL, 1760000120001LL, "PPSA99238"); // main-boot.cpp's format string
		Check(orbis_leave::ParseShelfMark(written, mark) && mark.sec == 1760000123LL && mark.mark_ms == 1760000123456LL && mark.press_ms == 1760000120001LL &&
		          std::string(mark.target) == "PPSA99238",
			"what the app writes is read back");
	}

	// fix180: the note's title (the fourth field) and who takes it. Tester on 180: a disc tile pressed 43 s after "back to the menu" in the main app (whose
	// own exec did not bring it back) took the main app's note and opened the shelf on the disc's cover; closed and pressed again, it started the disc.
	{
		using orbis_leave::NoteIsFor;
		orbis_leave::ShelfMark mark;
		Check(orbis_leave::ParseShelfMark("1 2 3 PPSA99203\n", mark) && std::string(mark.target) == "PPSA99203", "the title is read");
		Check(orbis_leave::ParseShelfMark("1 2 3\n", mark) && mark.target[0] == '\0', "a 176..180 note: no title");
		Check(orbis_leave::ParseShelfMark("1 2 3 PPSA99203 more\n", mark) && std::string(mark.target) == "PPSA99203", "what follows the title is left out");
		Check(orbis_leave::ParseShelfMark("1 2 3 PPSA/9203", mark) && mark.target[0] == '\0' && mark.press_ms == 3, "a title with other characters is left out");
		Check(orbis_leave::ParseShelfMark("1 2 3 ABCDEFGHIJKLMNOPQRSTUVWXYZ", mark) && mark.target[0] == '\0', "a title too long is left out");
		Check(orbis_leave::ParseShelfMark("1 2 x PPSA99203", mark) && mark.target[0] == '\0' && mark.press_ms == 0, "no title without the press time");
		Check(orbis_leave::ParseShelfMark("1 2 3 PPSA99251_0\r\n", mark) && std::string(mark.target) == "PPSA99251_0", "'_' and a CR line end");

		orbis_leave::ParseShelfMark("1 2 3 PPSA99203", mark);
		Check(NoteIsFor(mark, "PPSA99203", false), "the main app's note: the main app takes it");
		Check(!NoteIsFor(mark, "PPSA99238", true), "the main app's note: a disc tile does not (the tester's case)");
		Check(!NoteIsFor(mark, "PPSA99251", true), "the main app's note: a game tile does not");
		Check(!NoteIsFor(mark, "", true), "the main app's note: a tile whose folder was not found does not");
		orbis_leave::ParseShelfMark("1 2 3 PPSA99238", mark);
		Check(NoteIsFor(mark, "PPSA99238", true), "a tile's own exec: that tile takes it");
		Check(!NoteIsFor(mark, "PPSA99237", true), "a tile's own exec: another tile does not");
		Check(!NoteIsFor(mark, "PPSA99203", false), "a tile's own exec: the main app does not");
		Check(!NoteIsFor(mark, "", true), "a tile's own exec: a tile that does not know its folder does not");
		orbis_leave::ParseShelfMark("1 2 3", mark);
		Check(NoteIsFor(mark, "PPSA99203", false), "a note without a title (2.11..180): the main app takes it, as before");
		Check(!NoteIsFor(mark, "PPSA99238", true), "a note without a title: a tile press starts its game");
		Check(!NoteIsFor(mark, nullptr, true), "no own title: not taken");

		using orbis_leave::TargetOf;
		Check(std::string(TargetOf(How::Relaunch, "PPSA99238")) == "PPSA99203", "the relay starts the main app");
		Check(std::string(TargetOf(How::ExecMain, "")) == "PPSA99203", "the main app's exec starts the main app");
		Check(std::string(TargetOf(How::ExecOwnTile, "PPSA99238")) == "PPSA99238", "a tile's own exec starts that tile");
		Check(std::string(TargetOf(How::ExecOwnTile, "")) == "PPSA99203", "a tile's exec without its folder name: the main app's title");
		Check(std::string(TargetOf(How::ExitOnly, "PPSA99238")) == "PPSA99203", "the plain close: a note as before (the main app's)");
		// every way out, from every start: the note it writes is taken by the title that way starts, and by no tile it does not start
		for (int bits = 0; bits < 128; bits++)
		{
			const In in = FromBits(bits);
			const How how = orbis_leave::Decide(in);
			const char* own_tile = in.is_tile && in.tile_dir_found ? "PPSA99231" : "";
			char line[64];
			std::snprintf(line, sizeof(line), "1 2 3 %s\n", TargetOf(how, own_tile));
			orbis_leave::ShelfMark m;
			orbis_leave::ParseShelfMark(line, m);
			const bool main_takes = NoteIsFor(m, "PPSA99203", false);
			const bool own_takes = NoteIsFor(m, "PPSA99231", true);
			const bool other_takes = NoteIsFor(m, "PPSA99252", true);
			Check(!other_takes, std::string("bits ") + std::to_string(bits) + ": a tile the leave does not start never takes the note");
			Check(how == How::ExecOwnTile ? (own_takes && !main_takes) : (main_takes && !own_takes),
				std::string("bits ") + std::to_string(bits) + " (" + Name(how) + "): only the title it starts takes the note");
		}
	}

	// fix180 (owner: L1 + R1 + Touch must always end on the shelf): every way out by an exec gets the guard payload; the relay and the plain close do not
	for (int bits = 0; bits < 128; bits++)
	{
		const In in = FromBits(bits);
		const std::string tag = "bits " + std::to_string(bits);
		Check(orbis_leave::WantGuard(in) == (!in.flag_menuclose && !orbis_leave::WantRelay(in)), tag + ": WantGuard is 'no relay wanted and no menuclose'");
		In no_relay = in;
		no_relay.relay_started = false;
		const How how = orbis_leave::Decide(no_relay);
		if (orbis_leave::WantGuard(in))
			Check(how == How::ExecMain || how == How::ExecOwnTile, tag + ": a guarded leave is an exec (" + Name(how) + ")");
		if (how == How::ExecMain && !in.flag_menuclose && !orbis_leave::WantRelay(in))
			Check(orbis_leave::WantGuard(in), tag + ": the main app's own exec is guarded");
		if (in.flag_menuclose)
			Check(!orbis_leave::WantGuard(in), tag + ": menuclose (the home screen asked for): no guard");
	}
	{
		In main_default = {};
		Check(orbis_leave::WantRelay(main_default) && !orbis_leave::WantGuard(main_default), "fix181: the main app with no flags wants the relay");
		main_default.relay_started = true;
		Check(orbis_leave::Decide(main_default) == How::Relaunch, "fix181: and leaves by it when it is up");
		In fast = {};
		fast.flag_menufast = true;
		Check(orbis_leave::WantGuard(fast) && orbis_leave::Decide(fast) == How::ExecMain, "menufast: the exec, guarded");
		Check(orbis_leave::kGuardMs >= 3000 && orbis_leave::kGuardMs <= 15000 && orbis_leave::kGuardHardMs > orbis_leave::kGuardMs,
			"guard 3..15 s (179: back on the shelf 3.5 s after the press), hard limit above it");
	}

	std::printf("leaveplan test: %d checks, %s\n", checks, fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
