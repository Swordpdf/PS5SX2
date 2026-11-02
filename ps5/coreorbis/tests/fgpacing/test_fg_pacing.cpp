// PS5SX2 (vk-285-133, AI-assisted): a check of frame generation's pacing (orbis-shims/OrbisFrameGenPacing.h) on a PC.
// 2.11: and of the gate its status line names (orbis-shims/OrbisFrameGenGate.h), and that the pacing treats PAL 50 like NTSC 59.94.
//   g++ -std=c++17 -Wall -Wextra -I../../orbis-shims -o /tmp/test_fg_pacing test_fg_pacing.cpp && /tmp/test_fg_pacing
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "OrbisFrameGenGate.h" // 2.11: the "[fg] status" line
#include "OrbisFrameGenPacing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using orbis_fg::Decision;
using orbis_fg::Event;
using orbis_fg::Pacing;
using orbis_fg::State;

static int s_failures = 0;
static int s_checks = 0;
#define CHECK(cond)                                                    \
	do                                                                 \
	{                                                                  \
		s_checks++;                                                    \
		if (!(cond))                                                   \
		{                                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
			s_failures++;                                              \
		}                                                              \
	} while (0)

constexpr double kNtsc = 59.94, kPal = 50.0, kHz60 = 59.94, kHz120 = 119.88;

// A run of frames: each `vsyncs` apart on the PS2's clock, the emulation at `speed` percent (real time stretches to match),
// GSRenderer.cpp's presents modelled: the game's frame, the generated ones, and the repeats in between ("Skip duplicate
// frames" on, as by default: three skipped, the fourth presented).
struct Run
{
	Pacing p;
	double now = 100.0;
	double refreshes = 0.0; // the display's refreshes so far
	double presents = 0.0;  // the presents so far
	uint32_t frames = 0, generated = 0, last_real = 0;
	std::vector<Event> events;
	bool over_budget = false;

	Decision Frame(uint32_t vsyncs, double ps2, double hz, double speed = 100.0, bool nominal = true)
	{
		const double clock = ps2 > 0.0 ? ps2 : 59.94, display = hz > 0.0 ? hz : 59.94; // what the pacing assumes then
		now += vsyncs / clock * (100.0 / speed);
		const uint32_t repeats = vsyncs > 0 ? (vsyncs - 1) / 4 : 0;
		const uint32_t already = last_real + repeats;
		const Decision d = p.Frame(vsyncs, already, ps2, hz, now, speed, nominal);
		const uint32_t fit = static_cast<uint32_t>(vsyncs * display / clock + 0.03);
		if (d.presents > 0 && already + d.presents > fit)
			over_budget = true;
		refreshes += vsyncs * display / clock;
		presents += repeats + d.presents + 1;
		last_real = 1;
		frames++;
		generated += d.presents;
		if (const Event e = p.TakeEvent(); e != Event::None)
			events.push_back(e);
		return d;
	}
};

static bool Has(const std::vector<Event>& events, Event e)
{
	for (Event x : events)
		if (x == e)
			return true;
	return false;
}

// A steady game: every frame the same, after a few to settle.
static Decision Steady(uint32_t vsyncs, double ps2, double hz, uint32_t frames = 50)
{
	Run r;
	Decision d;
	for (uint32_t i = 0; i < frames; i++)
		d = r.Frame(vsyncs, ps2, hz);
	CHECK(!r.over_budget);
	CHECK(r.presents <= r.refreshes + 1.0);
	return d;
}

// 2.11: how long, in display refreshes, each picture the TV gets stays on screen (the model of the investigation's cadence.py). The
// emulator submits one flip per game frame, `vsyncs` PS2 vsyncs apart, and `generated` more flips back to back before it (the
// generated frames); the display shows a queued flip at the first vblank after it was submitted and never two in one vblank (a FIFO,
// flip rate 0); a picture stays until the next one is shown.
struct Cadence
{
	double pictures_per_second = 0.0;
	double stay_share[8] = {}; // percent of the pictures that stayed 1, 2, .. 7 refreshes (index 0 unused)
};

static Cadence MeasureCadence(double ps2_hz, double display_hz, uint32_t vsyncs, uint32_t generated, uint32_t frames = 20000)
{
	const double vblank = 1.0 / display_hz;
	std::vector<long> shown; // the vblank each flip is shown at
	long next_free = 0;
	for (uint32_t n = 0; n < frames; n++)
	{
		const double t = n * vsyncs / ps2_hz;
		const long first = static_cast<long>(std::ceil(t / vblank - 1e-9));
		for (uint32_t flip = 0; flip < 1 + generated; flip++)
		{
			const long k = first > next_free ? first : next_free;
			shown.push_back(k);
			next_free = k + 1;
		}
	}
	Cadence c;
	uint32_t counts[8] = {};
	for (size_t i = 1; i < shown.size(); i++)
	{
		const long d = shown[i] - shown[i - 1];
		counts[d < 7 ? d : 7]++;
	}
	const double total = static_cast<double>(shown.size() - 1);
	for (int i = 1; i < 8; i++)
		c.stay_share[i] = 100.0 * counts[i] / total;
	c.pictures_per_second = static_cast<double>(shown.size()) / static_cast<double>(shown.back() - shown.front()) * display_hz;
	return c;
}

static bool OnlyStays(const Cadence& c, int a, int b = 0)
{
	for (int i = 1; i < 8; i++)
		if (i != a && i != b && c.stay_share[i] > 0.0)
			return false;
	return true;
}

// ---- 2.11 (swordpdf on build 175: "framegen at 50fps doesn't seem to work"): the gate header, and that the pacing knows no region -------------
// OrbisFrameGenGate.h makes the "[fg] status" line, which names the gate that failed most in the last 10 s. A tester's log is read by its words.

using orbis_fg::Gate;
using orbis_fg::StatusInputs;
using orbis_fg::Window;

static bool Contains(const std::string& text, const char* part)
{
	return text.find(part) != std::string::npos;
}

// The window as OrbisFrameGenRecord fills it: `frames` new frames, each `vsyncs` PS2 vsyncs long, the pacing's state counted at each.
static StatusInputs GateRun(Pacing& p, double ps2, double display_hz, uint32_t vsyncs, uint32_t frames, bool nominal = true, double speed = 100.0,
	uint32_t driver_mhz = 0, uint32_t mode_mhz = 119880)
{
	StatusInputs in;
	double now = 100.0;
	for (uint32_t i = 0; i < frames; i++)
	{
		now += vsyncs / ps2;
		const Decision d = p.Frame(vsyncs, 1, ps2, display_hz, now, speed, nominal);
		in.w.Record(p.GetState());
		in.w.made += d.presents ? 1 : 0;
		in.w.presents += d.presents;
		in.w.counted++;
		in.w.vsync_sum += vsyncs;
	}
	in.w.seconds = frames * vsyncs / ps2;
	in.ps2_hz = ps2;
	in.refreshes = p.Refreshes();
	in.steady = p.SteadyFrames();
	in.warm_frames = p.WarmFrames();
	in.driver_mhz = driver_mhz;
	in.mode_mhz = mode_mhz;
	in.display = orbis_fg::PickDisplayHzSource(driver_mhz, 0, mode_mhz);
	in.speed = speed;
	in.launch = "main app";
	return in;
}

static void RunGateTests()
{
	using orbis_fg::EngagedWord;
	using orbis_fg::FirstGate;
	using orbis_fg::FormatStatus;

	// PAL 50 fps on a display that really is at 119.88 Hz (the driver says so): generating, 100 pictures a second.
	{
		Pacing p;
		const StatusInputs in = GateRun(p, kPal, kHz120, 1, 500, true, 100.0, 119880);
		CHECK(FirstGate(in) == Gate::None);
		CHECK(std::string(EngagedWord(in.w)) == "yes");
		const std::string line = FormatStatus(in);
		printf("%s\n", line.c_str());
		CHECK(Contains(line, "[fg] status: engaged=yes, first failing gate: none | launch: main app | display 119.88 Hz"));
		CHECK(Contains(line, "(the driver's measurement; the driver says 119.88 Hz, the listed mode 119.88 Hz)"));
		CHECK(Contains(line, "game 50.0 fps, 1.00 PS2 vsyncs a frame at 50.00 Hz = 2.40 refreshes a frame (1.97 needed)"));
		CHECK(Contains(line, "generated 49.3/s (1.0 presents each)"));
		CHECK(Contains(line, "pacing: generating 99%, warming-up 1%"));
		CHECK(Contains(line, "500 vsyncs in 10.0 s: 500 new frames, 0 not new, 0 blank, 0 duplicate-skipped, 0 present-throttled"));
	}
	// The same game when the driver fell back to 59.94 Hz although the 119.88 Hz mode was listed: no room, and the line says why.
	{
		Pacing p;
		const StatusInputs in = GateRun(p, kPal, kHz60, 1, 500, true, 100.0, 59940, 119880);
		CHECK(FirstGate(in) == Gate::NoRoom);
		CHECK(std::string(EngagedWord(in.w)) == "no");
		const std::string line = FormatStatus(in);
		printf("%s\n", line.c_str());
		CHECK(Contains(line, "engaged=no, first failing gate: no-room"));
		CHECK(Contains(line, "display 59.94 Hz (the driver's measurement; the driver says 59.94 Hz, the listed mode 119.88 Hz)"));
		CHECK(Contains(line, "1.20 refreshes a frame (1.97 needed)"));
		CHECK(Contains(line, "pacing: warming-up 1%, no-room 99%"));
		CHECK(Contains(line, "the display is at 60 Hz"));
	}
	// No driver reading and a listed 60 Hz mode: assumed from the mode, and the line says there was no reading.
	{
		Pacing p;
		const StatusInputs in = GateRun(p, kPal, kHz60, 1, 200, true, 100.0, 0, 60000);
		const std::string line = FormatStatus(in);
		CHECK(Contains(line, "the driver gives no rate, the listed mode 60.00 Hz"));
		CHECK(Contains(line, "(the display mode the driver lists;"));
		CHECK(FirstGate(in) == Gate::NoRoom);
		// ...and no mode listed at all
		StatusInputs none = in;
		none.mode_mhz = 0;
		CHECK(Contains(FormatStatus(none), "no mode listed"));
	}
	// PAL non-interlaced (49.76), PAL 25 fps on 119.88 Hz and on 59.94 Hz, NTSC 59.94 on 119.88: generating.
	{
		Pacing a, b, c, d;
		CHECK(FirstGate(GateRun(a, 49.76, kHz120, 1, 300, true, 100.0, 119880)) == Gate::None);
		CHECK(FirstGate(GateRun(b, kPal, kHz120, 2, 300, true, 100.0, 119880)) == Gate::None);
		CHECK(FirstGate(GateRun(c, kPal, kHz60, 2, 300, true, 100.0, 59940)) == Gate::None);
		CHECK(FirstGate(GateRun(d, kNtsc, kHz120, 1, 300, true, 100.0, 119880)) == Gate::None);
	}
	// Warming: the start.
	{
		Pacing p;
		const StatusInputs in = GateRun(p, kPal, kHz120, 1, 3, true, 100.0, 119880);
		CHECK(FirstGate(in) == Gate::Warming);
		CHECK(Contains(FormatStatus(in), "3 of 8 frames"));
	}
	// Unsteady: a PAL game that alternates frames of 1 and 2 vsyncs (33 fps).
	{
		Pacing p;
		StatusInputs in;
		double now = 100.0;
		for (int i = 0; i < 60; i++)
		{
			const uint32_t v = (i % 2 == 1) ? 2 : 1;
			now += v / kPal;
			p.Frame(v, 1, kPal, kHz120, now, 100.0, true);
			in.w.Record(p.GetState());
		}
		in.w.seconds = 1.0;
		in.steady = p.SteadyFrames();
		in.display = orbis_fg::PickDisplayHzSource(119880, 0, 119880);
		in.driver_mhz = 119880;
		CHECK(FirstGate(in) == Gate::Unsteady);
		CHECK(Contains(FormatStatus(in), "frames alike (7 to start, 6 to go on)"));
		CHECK(Contains(FormatStatus(in), "unsteady-frame-length 88%"));
	}
	// The limiter is not at normal speed.
	{
		Pacing p;
		CHECK(FirstGate(GateRun(p, kPal, kHz120, 1, 100, false, 100.0, 119880)) == Gate::NotNominal);
	}
	// The safety net's pause: the game at 50% while frames are generated (the pause outlasts the generating stretch).
	{
		Pacing p;
		StatusInputs in;
		double now = 100.0;
		for (int i = 0; i < 700; i++)
		{
			now += 1.0 / kPal;
			p.Frame(1, 1, kPal, kHz120, now, i < 50 ? 100.0 : 50.0, true);
			in.w.Record(p.GetState());
		}
		CHECK(FirstGate(in) == Gate::Paused);
		CHECK(std::string(EngagedWord(in.w)) == "part");
	}
	// Nothing reached the generator: the most common reason of the window.
	{
		StatusInputs in;
		in.w.throttled = 500;
		CHECK(FirstGate(in) == Gate::PresentThrottled);
		in.w.throttled = 10;
		in.w.blank = 480;
		CHECK(FirstGate(in) == Gate::BlankPicture);
		in.w.blank = 5;
		in.w.not_new = 485;
		CHECK(FirstGate(in) == Gate::NoNewFrames);
		in.w.dup_skipped = 600;
		CHECK(FirstGate(in) == Gate::DuplicateSkipped);
		CHECK(Contains(FormatStatus(in), "first failing gate: duplicate-skipped"));
		CHECK(Contains(FormatStatus(in), "the internal FPS count sees no new frame"));
		in.w = Window{};
		in.w.throttled = 400;
		CHECK(FirstGate(in) == Gate::PresentThrottled);
		CHECK(Contains(FormatStatus(in), "presenting is throttled"));
		in.w = Window{};
		CHECK(FirstGate(in) == Gate::NoNewFrames); // nothing counted at all: still named, never "none"
		CHECK(std::string(EngagedWord(in.w)) == "no");
		CHECK(Contains(FormatStatus(in), "engaged=no, first failing gate: no-new-frames"));
	}
	// Precedence: a failed engine or a missing swapchain come before the pacing, and the unserved frames are counted.
	{
		Pacing p;
		StatusInputs in = GateRun(p, kPal, kHz120, 1, 100, true, 100.0, 119880);
		in.engine_failed = true;
		in.w.unserved = 20;
		CHECK(FirstGate(in) == Gate::EngineFailed);
		CHECK(Contains(FormatStatus(in), "first failing gate: engine-failed"));
		CHECK(Contains(FormatStatus(in), "20 could not be taken by the generator"));
		in.engine_failed = false;
		in.swapchain = false;
		CHECK(FirstGate(in) == Gate::NoSwapchain);
		CHECK(Contains(FormatStatus(in), "no-swapchain"));
		// only unserved frames (what Record's early return counts): the engine's word decides, never "none"
		StatusInputs failed;
		failed.engine_failed = true;
		failed.w.unserved = 500;
		CHECK(FirstGate(failed) == Gate::EngineFailed);
		CHECK(failed.w.Vsyncs() == 500);
	}
	// "part": generating for part of the window, and the line names the other part.
	{
		StatusInputs in;
		in.w.recorded = 100;
		in.w.engaged = 70;
		in.w.by_state[static_cast<int>(State::Generating)] = 70;
		in.w.by_state[static_cast<int>(State::Unsteady)] = 30;
		in.w.seconds = 10.0;
		CHECK(std::string(EngagedWord(in.w)) == "part");
		CHECK(FirstGate(in) == Gate::Unsteady);
		const std::string line = FormatStatus(in);
		CHECK(Contains(line, "engaged=part, first failing gate: unsteady-frame-length"));
		CHECK(Contains(line, "pacing: generating 70%, unsteady-frame-length 30%"));
		in.w.engaged = 95;
		in.w.by_state[static_cast<int>(State::Generating)] = 95;
		in.w.by_state[static_cast<int>(State::Unsteady)] = 5;
		in.w.made = 95; // (and generated frames came out: see the "no-generated-frame" case below)
		CHECK(std::string(EngagedWord(in.w)) == "yes");
		CHECK(FirstGate(in) == Gate::None);
	}
	// The failing state the window had most often decides, not the pacing's state at the time of the print (which is what said
	// "generating" for a window that was mostly not).
	{
		StatusInputs in;
		in.w.recorded = 100;
		in.w.engaged = 40;
		in.w.by_state[static_cast<int>(State::Generating)] = 40;
		in.w.by_state[static_cast<int>(State::NoRoom)] = 40;
		in.w.by_state[static_cast<int>(State::Warming)] = 20;
		CHECK(FirstGate(in) == Gate::NoRoom);
		in.w.by_state[static_cast<int>(State::NoRoom)] = 20;
		in.w.by_state[static_cast<int>(State::Warming)] = 40;
		CHECK(FirstGate(in) == Gate::Warming);
	}
	// A game with a frame every second vsync: the repeats are skipped on purpose and count as duplicate skips, not as a failing gate.
	{
		StatusInputs in;
		in.w.dup_skipped = 250;
		in.w.recorded = 250;
		in.w.engaged = 250;
		in.w.by_state[static_cast<int>(State::Generating)] = 250;
		in.w.made = 250;
		CHECK(FirstGate(in) == Gate::None);
		CHECK(in.w.Vsyncs() == 500);
	}
	// A few new frames among a thousand blank pictures: the blank pictures are the reason, not the pacing's warm-up.
	{
		StatusInputs in;
		in.w.blank = 997;
		in.w.recorded = 3;
		in.w.by_state[static_cast<int>(State::Warming)] = 3;
		CHECK(FirstGate(in) == Gate::BlankPicture);
		in.w.blank = 2;
		in.w.not_new = 1;
		CHECK(FirstGate(in) == Gate::Warming);
	}
	// The launch path and the one-line bound.
	{
		Pacing p;
		StatusInputs in = GateRun(p, kPal, kHz60, 1, 500, true, 100.0, 59940);
		in.launch = "disc tile /dev/cd0";
		CHECK(Contains(FormatStatus(in), "launch: disc tile /dev/cd0 |"));
		in.launch.clear();
		CHECK(Contains(FormatStatus(in), "launch: unknown |"));
		in.videoout = "[videoout] switching to 119.88 Hz";
		in.gpu = "; GPU 3.10 ms a frame (1.00 the optical flow), most 4.00";
		const std::string line = FormatStatus(in);
		CHECK(line.find('\n') == std::string::npos);
		CHECK(line.size() < 900);
		CHECK(Contains(line, "| [videoout] switching to 119.88 Hz"));
		CHECK(Contains(line, "; GPU 3.10 ms a frame"));
		const std::string long_text(3000, 'x');
		in.videoout = long_text.c_str();
		const std::string cut = FormatStatus(in);
		CHECK(cut.size() == 1400 && cut.compare(cut.size() - 3, 3, "...") == 0 && cut.find('\n') == std::string::npos);
	}

	// Review of the gate (2.11): a pacing that generates for the whole window and puts nothing on the screen must not read "none".
	{
		Pacing p;
		StatusInputs in = GateRun(p, kPal, kHz120, 1, 500, true, 100.0, 119880);
		CHECK(FirstGate(in) == Gate::None && in.w.made > 0);
		in.w.made = 0;
		in.w.presents = 0;
		CHECK(std::string(EngagedWord(in.w)) == "yes");
		CHECK(FirstGate(in) == Gate::NothingShown);
		const std::string line = FormatStatus(in);
		CHECK(Contains(line, "engaged=yes, first failing gate: no-generated-frame"));
		CHECK(Contains(line, "generated 0.0/s (0.0 presents each)"));
		CHECK(Contains(line, "no generated frame was presented in the window"));
		// ...but a window that is mostly the start (a handful of frames) is not called a gate
		Pacing q;
		StatusInputs few = GateRun(q, kPal, kHz120, 1, 27, true, 100.0, 119880); // 8 warming frames, 19 generating: not mostly engaged
		few.w.made = 0;
		CHECK(FirstGate(few) == Gate::Warming);
		StatusInputs tiny;
		tiny.w.recorded = 10;
		tiny.w.engaged = 10;
		tiny.w.by_state[static_cast<int>(State::Generating)] = 10;
		CHECK(FirstGate(tiny) == Gate::None);
		tiny.w.recorded = tiny.w.engaged = tiny.w.by_state[static_cast<int>(State::Generating)] = orbis_fg::kNothingShownFrames;
		CHECK(FirstGate(tiny) == Gate::NothingShown);
		tiny.w.made = 1;
		CHECK(FirstGate(tiny) == Gate::None);
	}
	// New frames the generator could not take, with the engine well and a swapchain now (it was re-made during the window): named, not
	// "no new frames" or the pacing's state of the few frames that got through.
	{
		StatusInputs in;
		in.w.unserved = 500;
		CHECK(FirstGate(in) == Gate::NoSwapchain);
		in = StatusInputs();
		in.w.unserved = 400;
		in.w.recorded = 100;
		in.w.engaged = 0;
		in.w.by_state[static_cast<int>(State::Warming)] = 100;
		CHECK(FirstGate(in) == Gate::NoSwapchain);
		in.w.unserved = 50;
		CHECK(FirstGate(in) == Gate::Warming);
		CHECK(in.w.Vsyncs() == 150);
		// the count of the pacing's states has to follow the pacing: a State past the array is not counted (and does not write past it)
		in = StatusInputs();
		for (int extra = 0; extra < 4; extra++)
			in.w.Record(static_cast<State>(orbis_fg::kStates + extra));
		CHECK(in.w.recorded == 4);
		uint32_t counted = 0;
		for (int i = 0; i < orbis_fg::kStates; i++)
			counted += in.w.by_state[i];
		CHECK(counted == 0);
		CHECK(in.w.made == 0 && in.w.presents == 0 && in.w.counted == 0 && in.w.vsync_sum == 0.0); // (the fields after the array)
	}
	// One log line whatever the texts hold: a launch path or a summary with a line break in it.
	{
		Pacing p;
		StatusInputs in = GateRun(p, kPal, kHz60, 1, 100, true, 100.0, 59940);
		in.launch = "disc tile /dev/cd0\n[fg] forged";
		in.videoout = "[videoout] one\r\ntwo\ttab";
		in.gpu = "; GPU\n";
		const std::string line = FormatStatus(in);
		bool control = false;
		for (const char c : line)
			control = control || static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
		CHECK(!control);
		CHECK(Contains(line, "launch: disc tile /dev/cd0 [fg] forged |"));
		CHECK(Contains(line, "| [videoout] one  two tab"));
	}
	// The "not generating: ... no room" line (GSDeviceVK.cpp prints it; the words are the header's).
	{
		using orbis_fg::DisplayHzSource;
		using orbis_fg::FormatNoRoom;
		// PAL 50 fps on a display at 59.94 Hz, from the listed mode, the driver has no output control of its own to add
		orbis_fg::NoRoomText t = FormatNoRoom(1.0, kPal, 1.2, kHz60, DisplayHzSource::ListedMode, std::string());
		printf("%s\n", t.line.c_str());
		CHECK(t.needs_120hz);
		CHECK(Contains(t.line, "[fg] not generating: a new frame every 1.0 PS2 vsyncs (50.00 Hz) is 1.20 refreshes at 59.94 Hz, no room for another"));
		CHECK(Contains(t.line, "a PAL one at 50 fps (shown at 100), needs the 120 Hz output. The display rate is 59.94 Hz, from the display mode the driver lists;"));
		CHECK(Contains(t.line, "the [display] swapchain line says why it is not 120.)"));
		CHECK(t.line.find('\n') == std::string::npos && t.line.find(" )") == std::string::npos);
		// the longest source name is not cut by a buffer, and the app's own switch's summary is appended when there is one
		t = FormatNoRoom(1.0, kPal, 1.2, kHz60, DisplayHzSource::Assumed, "[videoout] not switched: the driver exports no output control");
		CHECK(Contains(t.line, "from assumed: the driver gave no rate and lists no 120 Hz mode; the [display] swapchain line says why it is not 120. "
		                       "[videoout] not switched: the driver exports no output control)"));
		// a 30 fps game on a display below 90 Hz that has no room (an uneven or odd rate): no 120 Hz label, the shorter words
		t = FormatNoRoom(2.0, kNtsc, 1.6, kHz60, DisplayHzSource::Driver, std::string());
		CHECK(!t.needs_120hz);
		CHECK(Contains(t.line, "(at 60 Hz only games at 30 fps or less get generated frames; 60 fps ones need the 120 Hz output)"));
		// on a 119.88 Hz display no room is not about the output: no extra words, no label
		t = FormatNoRoom(1.0, 100.0, 1.2, kHz120, DisplayHzSource::Driver, std::string());
		CHECK(!t.needs_120hz);
		CHECK(Contains(t.line, "refreshes at 119.88 Hz, no room for another") && t.line.find("(at 60 Hz") == std::string::npos);
		CHECK(t.line.compare(t.line.size() - 19, 19, "no room for another") == 0);
	}
}

// The engaged share (percent of the game's frames) of `frames` frames of a game whose frames are 1 vsync long, or 2 at random for the share
// `two` (the same seed gives the same lengths in the same order, whatever the PS2's rate).
static double EngagedShare(double ps2, double display, double two, unsigned seed, uint32_t frames)
{
	Pacing pace;
	std::mt19937 rng(seed);
	std::uniform_real_distribution<double> unit(0.0, 1.0);
	double now = 1000.0;
	uint32_t engaged = 0;
	for (uint32_t i = 0; i < frames; i++)
	{
		const uint32_t vsyncs = unit(rng) < two ? 2 : 1;
		now += vsyncs / ps2;
		engaged += pace.Frame(vsyncs, 1, ps2, display, now, 100.0, true).engaged ? 1 : 0;
	}
	return 100.0 * engaged / frames;
}

// The pacing knows no region: what it decides is the refreshes a frame has (PS2 vsyncs x display Hz / PS2 Hz), whether the PS2 runs at
// PAL's 50 Hz (49.76 non-interlaced) or NTSC's 59.94 (59.82, HD modes' 60.00). So PAL 50 -> 100 on a 119.88 Hz output is the design, and a
// PAL game that "does nothing" has a display that is not at 119.88 Hz, frames that are not steady, or a setting that did not reach the boot.
static void RunRegionTests()
{
	const double ps2_rates[] = {50.0, 49.76, 60.0, 59.94, 59.82};
	const double displays[] = {kHz60, kHz120};
	for (const double ps2 : ps2_rates)
	{
		for (const double display : displays)
		{
			for (uint32_t vsyncs = 1; vsyncs <= 3; vsyncs++)
			{
				Pacing p;
				Decision d;
				for (int i = 0; i < 60; i++)
					d = p.Frame(vsyncs, 1, ps2, display, 100.0 + i * vsyncs / ps2, 100.0, true);
				const double refreshes = vsyncs * display / ps2;
				const bool room = refreshes >= Pacing::kEngage;
				CHECK(std::fabs(p.Refreshes() - refreshes) < 1e-9);
				CHECK(d.engaged == room);
				const uint32_t fit = static_cast<uint32_t>(refreshes + 0.03);
				CHECK(d.presents == (room ? std::min(p.Repeats(), fit - 1) : 0u));
				CHECK(p.GetState() == (room ? State::Generating : State::NoRoom));
			}
		}
	}

	// Frames of random length, PAL 50 against NTSC 59.94 on 119.88 Hz (the same lengths in the same order): the same share generated.
	for (const double two : {0.0, 0.05, 0.1, 0.125, 0.2, 0.5, 1.0})
	{
		double pal = 0.0, ntsc = 0.0;
		const int seeds = 10;
		for (int seed = 1; seed <= seeds; seed++)
		{
			pal += EngagedShare(kPal, kHz120, two, seed, 3000);
			ntsc += EngagedShare(kNtsc, kHz120, two, seed, 3000);
		}
		pal /= seeds;
		ntsc /= seeds;
		printf("mix %.3f: PAL 50 %.2f%%, NTSC 59.94 %.2f%% engaged\n", two, pal, ntsc);
		CHECK(std::fabs(pal - ntsc) < 0.5);
		if (two == 0.0 || two == 1.0)
			CHECK(pal > 99.0);
		if (two == 0.05)
			CHECK(pal > 95.0);
		if (two == 0.5)
			CHECK(pal < 40.0);
	}
}

int main()
{
	// The cases of the header's comment.
	CHECK(Steady(1, kNtsc, kHz60).presents == 0);  // R&C at 60 Hz: what vk-285-131 slowed to half speed
	CHECK(!Steady(1, kNtsc, kHz60).engaged);
	CHECK(Steady(1, kNtsc, kHz120).presents == 1); // 60 fps on a 120 Hz display
	CHECK(Steady(2, kNtsc, kHz60).presents == 1);  // 30 fps at 60 Hz
	CHECK(Steady(2, kNtsc, kHz120).presents == 2); // 30 fps at 120 Hz: the generated frame twice
	CHECK(Steady(3, kNtsc, kHz120).presents == 3); // 20 fps at 120 Hz: three times
	CHECK(Steady(3, kNtsc, kHz60).presents == 1);  // 20 fps at 60 Hz: once, the game's frame up for two refreshes
	CHECK(Steady(4, kNtsc, kHz60).presents == 2);  // 15 fps at 60 Hz
	CHECK(Steady(1, kPal, kHz120).presents == 1);  // PAL 50 fps on 120 Hz: 2.4 refreshes a frame
	CHECK(Steady(2, kPal, kHz60).presents == 1);   // PAL 25 fps at 60 Hz
	CHECK(Steady(1, kPal, kHz60).presents == 0);   // PAL 50 fps at 60 Hz: 1.2 refreshes, no room
	CHECK(Steady(1, 49.76, kHz120).presents == 1); // 2.11: PAL non-interlaced (49.76) on 119.88: 2.41 refreshes
	CHECK(Steady(2, kPal, kHz120).presents == 2);  // 2.11: PAL 25 fps on 120 Hz: 4.8 refreshes, the generated frame twice
	CHECK(Steady(2, kPal, kHz60).engaged);         // 2.11: PAL 25 fps at 60 Hz: 2.4 refreshes
	CHECK(Steady(1, 60.0, kHz120).presents == 1);  // an HD mode's 60.00 Hz on 119.88: 1.998 refreshes
	CHECK(Steady(1, 59.83, kHz60).presents == 0);  // NTSC progressive (59.83) at 59.94: one refresh
	CHECK(Steady(1, 0.0, 0.0).presents == 0);      // rates unknown: 59.94 both
	CHECK(Steady(6, kNtsc, kHz60).presents == 3);  // 10 fps at 60 Hz: a repeat presented in the interval, still in budget

	// The start: 8 frames first, then the first generating frame starts the interpolator over, the next ones don't.
	{
		Run r;
		uint32_t first = 0;
		Decision d;
		for (uint32_t i = 1; i <= 20 && !first; i++)
		{
			d = r.Frame(1, kNtsc, kHz120);
			if (d.engaged)
				first = i;
		}
		CHECK(first == Pacing::kWindow);
		CHECK(d.reset);
		d = r.Frame(1, kNtsc, kHz120);
		CHECK(d.engaged && !d.reset && d.presents == 1);
	}

	// A gap (a load): nothing for 8 frames, then it starts over.
	{
		Run r;
		for (int i = 0; i < 20; i++)
			r.Frame(1, kNtsc, kHz120);
		Decision d = r.Frame(30, kNtsc, kHz120);
		CHECK(!d.engaged && d.presents == 0 && r.p.GetState() == State::Warming);
		for (uint32_t i = 1; i < Pacing::kWindow; i++)
		{
			d = r.Frame(1, kNtsc, kHz120);
			CHECK(!d.engaged);
		}
		d = r.Frame(1, kNtsc, kHz120);
		CHECK(d.engaged && d.reset && d.presents == 1);
	}

	// After a load, a first long interval doesn't switch it on for a 60 fps game at 60 Hz (the review's case).
	{
		Run r;
		for (int i = 0; i < 20; i++)
			r.Frame(1, kNtsc, kHz60);
		r.Frame(12, kNtsc, kHz60);
		r.Frame(8, kNtsc, kHz60);
		uint32_t engaged = 0;
		for (int i = 0; i < 40; i++)
			engaged += r.Frame(1, kNtsc, kHz60).engaged ? 1 : 0;
		CHECK(engaged == 0);
	}

	// 60 fps games that drop a frame now and then, at 60 Hz: never a generated frame (vk-285-131's trap).
	{
		Run r;
		for (int i = 0; i < 600; i++)
			r.Frame((i % 7) == 3 ? 2 : 1, kNtsc, kHz60);
		CHECK(r.generated == 0);
		CHECK(!r.over_budget);
	}

	// A 30 fps game at 60 Hz with a short frame now and then: it stays on, and that frame gets no generated present.
	{
		Run r;
		uint32_t short_with_presents = 0;
		for (int i = 0; i < 600; i++)
		{
			const uint32_t v = (i % 9) == 4 ? 1 : 2;
			const Decision d = r.Frame(v, kNtsc, kHz60);
			if (v == 1 && d.presents > 0)
				short_with_presents++;
		}
		CHECK(short_with_presents == 0);
		CHECK(r.generated > 480);
		CHECK(!r.over_budget);
		CHECK(r.presents <= r.refreshes + 1.0);
	}

	// Alternating 60 and 30 at 60 Hz: the median is 1.5 refreshes, no room.
	{
		Run r;
		for (int i = 0; i < 300; i++)
			r.Frame((i & 1) ? 2 : 1, kNtsc, kHz60);
		CHECK(r.generated == 0);
	}

	// Random frame patterns, both displays, both regions: never more presents than refreshes.
	{
		srand(1234);
		for (int run = 0; run < 400; run++)
		{
			Run r;
			const double ps2 = (run & 1) ? kPal : kNtsc;
			const double hz = (run & 2) ? kHz120 : kHz60;
			const int base = 1 + rand() % 3;
			for (int i = 0; i < 300; i++)
			{
				uint32_t v = static_cast<uint32_t>(base);
				const int roll = rand() % 10;
				if (roll == 0)
					v = 1;
				else if (roll == 1)
					v = static_cast<uint32_t>(base + 1);
				else if (roll == 2 && (rand() % 20) == 0)
					v = 12; // a load
				r.Frame(v, ps2, hz);
			}
			CHECK(!r.over_budget);
			CHECK(r.presents <= r.refreshes + 1.0);
		}
	}

	// Turbo, slow motion, the fast boot: nothing generated, and back when the limiter is.
	{
		Run r;
		for (int i = 0; i < 20; i++)
			r.Frame(1, kNtsc, kHz120);
		Decision d = r.Frame(1, kNtsc, kHz120, 300.0, false);
		CHECK(!d.engaged && d.presents == 0 && r.p.GetState() == State::NotNominal);
		d = r.Frame(1, kNtsc, kHz120);
		CHECK(d.engaged && d.reset && d.presents == 1 && r.p.GetState() == State::Generating);
	}

	// The safety net. Generated frames that cost speed: a 10 s pause, full speed in it, so the next pause is 20 s.
	{
		Run r;
		for (int i = 0; i < 120; i++) // two seconds at full speed
			r.Frame(1, kNtsc, kHz120);
		CHECK(!r.p.Paused());
		int slow_frames = 0;
		while (!r.p.Paused() && slow_frames < 1000)
		{
			r.Frame(1, kNtsc, kHz120, 80.0);
			slow_frames++;
		}
		CHECK(r.p.Paused() && r.p.GetState() == State::Paused);
		CHECK(Has(r.events, Event::Paused));
		CHECK(std::fabs(r.p.PauseSeconds() - 10.0) < 1e-9);
		CHECK(std::fabs(r.p.SlowSpeed() - 80.0) < 1e-9);
		CHECK(std::fabs(r.p.PauseFloor() - 95.0) < 1e-9);
		CHECK(slow_frames >= 48 * 2 && slow_frames <= 48 * 4); // two to four seconds at 80% (48 frames a second)
		const double pause_start = r.now;
		uint32_t generated_in_pause = 0;
		while (r.p.Paused() && r.now - pause_start < 30.0)
		{
			const Decision in_pause = r.Frame(1, kNtsc, kHz120);
			if (r.p.Paused())
				generated_in_pause += in_pause.presents;
		}
		CHECK(generated_in_pause == 0);
		CHECK(r.now - pause_start >= 10.0 && r.now - pause_start < 11.5);
		CHECK(Has(r.events, Event::ResumedCostly));
		CHECK(r.p.NoFgSpeed() > 99.0);
		Decision d = r.Frame(1, kNtsc, kHz120);
		CHECK(d.engaged && d.presents == 1);
		r.events.clear();
		while (!r.p.Paused() && slow_frames < 5000)
		{
			r.Frame(1, kNtsc, kHz120, 80.0);
			slow_frames++;
		}
		CHECK(std::fabs(r.p.PauseSeconds() - 20.0) < 1e-9);
		// Then five minutes without a pause: the next one is 10 s again.
		while (r.p.Paused())
			r.Frame(1, kNtsc, kHz120);
		const double calm_until = r.now + 310.0;
		while (r.now < calm_until)
			r.Frame(1, kNtsc, kHz120);
		while (!r.p.Paused() && r.frames < 200000)
			r.Frame(1, kNtsc, kHz120, 80.0);
		CHECK(std::fabs(r.p.PauseSeconds() - 10.0) < 1e-9);
	}

	// 186: another upscale (ForgetPauses): a pause grown to 40 s ends at once, frames are generated on the next frame, and the next
	// pause is 10 s again.
	{
		Run r;
		for (int i = 0; i < 120; i++)
			r.Frame(1, kNtsc, kHz120);
		for (int round = 0; round < 3; round++) // pauses of 10, 20 and 40 s, each with the game at full speed in it
		{
			while (!r.p.Paused() && r.frames < 100000)
				r.Frame(1, kNtsc, kHz120, 80.0);
			if (round < 2)
				while (r.p.Paused())
					r.Frame(1, kNtsc, kHz120);
		}
		CHECK(r.p.Paused() && std::fabs(r.p.PauseSeconds() - 40.0) < 1e-9);
		CHECK(r.p.ForgetPauses());
		CHECK(!r.p.Paused());
		Decision d = r.Frame(1, kNtsc, kHz120);
		CHECK(d.engaged && d.reset && d.presents == 1 && r.p.GetState() == State::Generating);
		CHECK(std::fabs(r.p.NextPause() - 10.0) < 1e-9);
		CHECK(!r.p.ForgetPauses()); // nothing left to forget
		while (!r.p.Paused() && r.frames < 200000)
			r.Frame(1, kNtsc, kHz120, 80.0);
		CHECK(std::fabs(r.p.PauseSeconds() - 10.0) < 1e-9);
	}

	// Better without them but not at full speed (80% with, 84% without): not blamed on them, the pause doesn't grow.
	{
		Run r;
		for (int i = 0; i < 120; i++)
			r.Frame(1, kNtsc, kHz120);
		while (!r.p.Paused() && r.frames < 2000)
			r.Frame(1, kNtsc, kHz120, 80.0);
		while (r.p.Paused() && r.frames < 4000)
			r.Frame(1, kNtsc, kHz120, 84.0);
		CHECK(Has(r.events, Event::ResumedSlowGame));
		CHECK(std::fabs(r.p.NextPause() - 10.0) < 1e-9);
	}

	// A game slow by itself: as slow in the pause, so frames are generated again and its speed (less 3) is the floor.
	{
		Run r;
		for (int i = 0; i < 120; i++)
			r.Frame(1, kNtsc, kHz120);
		while (!r.p.Paused() && r.frames < 2000)
			r.Frame(1, kNtsc, kHz120, 85.0);
		CHECK(r.p.Paused());
		while (r.p.Paused() && r.frames < 4000)
			r.Frame(1, kNtsc, kHz120, 86.0);
		CHECK(Has(r.events, Event::ResumedSlowGame));
		CHECK(std::fabs(r.p.Floor(r.now) - 83.0) < 1e-9);
		const double until = r.now + 30.0;
		while (r.now < until)
			r.Frame(1, kNtsc, kHz120, 85.0);
		CHECK(!r.p.Paused());
		while (!r.p.Paused() && r.frames < 20000)
			r.Frame(1, kNtsc, kHz120, 75.0);
		CHECK(r.p.Paused());
		CHECK(std::fabs(r.p.PauseSeconds() - 10.0) < 1e-9);
		CHECK(std::fabs(r.p.PauseFloor() - 83.0) < 1e-9);
	}

	// A slow game with nothing generated (60 fps at 60 Hz): no pause, nothing to blame.
	{
		Run r;
		for (int i = 0; i < 2000; i++)
			r.Frame(1, kNtsc, kHz60, 50.0);
		CHECK(!r.p.Paused());
		CHECK(r.events.empty());
	}

	// The pause's length tops out at 160 s.
	{
		Run r;
		for (int round = 0; round < 7; round++)
		{
			while (!r.p.Paused() && r.frames < 200000)
				r.Frame(1, kNtsc, kHz120, 70.0);
			while (r.p.Paused() && r.frames < 400000)
				r.Frame(1, kNtsc, kHz120, 100.0);
		}
		while (!r.p.Paused() && r.frames < 600000)
			r.Frame(1, kNtsc, kHz120, 70.0);
		CHECK(std::fabs(r.p.PauseSeconds() - 160.0) < 1e-9);
	}

	// vk-285-135: only a steady frame rate gets generated frames. Ratchet & Clank's stretch of frames 1 or 2 vsyncs long
	// (45-58 fps) at 120 Hz: none, the game's own frames.
	{
		Run r;
		const uint32_t pattern[] = {1, 1, 2, 1, 2, 1, 1, 2, 1, 2, 2, 1};
		uint32_t generated = 0;
		for (int i = 0; i < 600; i++)
		{
			const Decision d = r.Frame(pattern[i % 12], kNtsc, kHz120);
			generated += d.presents;
		}
		CHECK(generated == 0);
		CHECK(r.p.GetState() == State::Unsteady);
		CHECK(r.p.SteadyFrames() < Pacing::kSteadyStart);
	}
	// A steady 60 fps game with a slow frame now and then (1 in 8): still generated, the long frame capped by its own refreshes.
	{
		Run r;
		uint32_t generated = 0;
		for (int i = 0; i < 400; i++)
		{
			const Decision d = r.Frame(i % 8 == 7 ? 2 : 1, kNtsc, kHz120);
			if (i > 50)
				generated += d.presents;
		}
		CHECK(generated > 300);
		CHECK(r.p.GetState() == State::Generating);
		CHECK(!r.over_budget);
	}
	// Generating, then the frames turn uneven: 2 of 8 different keeps it on, 3 of 8 stops it; 7 of 8 even again starts it.
	{
		Run r;
		for (int i = 0; i < 60; i++)
			r.Frame(1, kNtsc, kHz120);
		CHECK(r.p.GetState() == State::Generating);
		r.Frame(2, kNtsc, kHz120);
		r.Frame(1, kNtsc, kHz120);
		r.Frame(2, kNtsc, kHz120);
		CHECK(r.p.SteadyFrames() == 6);
		CHECK(r.p.GetState() == State::Generating);
		r.Frame(2, kNtsc, kHz120);
		CHECK(r.p.SteadyFrames() == 5);
		CHECK(r.p.GetState() == State::Unsteady);
		// Back to even: the three 2s leave the window one by one; 6 of 8 isn't enough to start again, 7 is.
		int to_start = 0;
		while (r.p.GetState() != State::Generating && to_start < 20)
		{
			const uint32_t before = r.p.SteadyFrames();
			r.Frame(1, kNtsc, kHz120);
			to_start++;
			if (r.p.GetState() == State::Generating)
				CHECK(r.p.SteadyFrames() >= Pacing::kSteadyStart && before < Pacing::kSteadyStart + 1);
		}
		CHECK(r.p.GetState() == State::Generating);
		CHECK(r.p.SteadyFrames() == 7);
	}
	// A steady 30 fps game at 60 Hz still doubles; one alternating between 30 and 20 fps (2, 3 vsyncs) doesn't.
	{
		CHECK(Steady(2, kNtsc, kHz60).engaged);
		Run r;
		bool any = false;
		for (int i = 0; i < 400; i++)
			any = r.Frame(i % 2 ? 3 : 2, kNtsc, kHz60).presents > 0 || any;
		CHECK(!any);
	}

	// 2.11: PAL 50 -> 100, and what the TV shows for it. The pacing decides how many frames are generated (the cases above); the
	// cadence is what the display makes of the flips. A true 50 or 100 Hz output is not reachable from a title (the console's output
	// selectors 0..63 were asked: only 59.94 and 119.88 Hz work), so a PAL game's 50 fps cannot be even on a 60 or 120 Hz display;
	// frame generation at 119.88 Hz halves the step, and a VRR display would be even.
	{
		// PAL at 50 fps on 59.94 Hz without generated frames (no room: 1.2 refreshes): 50 pictures a second, one in five stays
		// for two refreshes, the others for one.
		const Decision pal_at_60 = Steady(1, kPal, kHz60);
		CHECK(pal_at_60.presents == 0 && !pal_at_60.engaged);
		const Cadence a = MeasureCadence(kPal, kHz60, 1, pal_at_60.presents);
		CHECK(std::fabs(a.pictures_per_second - 50.0) < 0.5);
		CHECK(OnlyStays(a, 1, 2));
		CHECK(a.stay_share[1] > 79.0 && a.stay_share[1] < 81.0 && a.stay_share[2] > 19.0 && a.stay_share[2] < 21.0);

		// The same game on a 119.88 Hz display with frame generation off: 2.4 refreshes a picture, 3 refreshes 40% of the time.
		const Cadence b = MeasureCadence(kPal, kHz120, 1, 0);
		CHECK(std::fabs(b.pictures_per_second - 50.0) < 0.5);
		CHECK(OnlyStays(b, 2, 3));
		CHECK(b.stay_share[2] > 59.0 && b.stay_share[2] < 61.0 && b.stay_share[3] > 39.0 && b.stay_share[3] < 41.0);

		// PAL 50 -> 100 on 119.88 Hz with frame generation: the pacing generates one frame before each of the game's, so the TV
		// gets 100 different pictures a second, each for 1 refresh (80%) or 2 (20%): half the step of the cases above.
		const Decision pal_at_120 = Steady(1, kPal, kHz120);
		CHECK(pal_at_120.engaged && pal_at_120.presents == 1);
		const Cadence c = MeasureCadence(kPal, kHz120, 1, pal_at_120.presents);
		CHECK(std::fabs(c.pictures_per_second - 100.0) < 0.5);
		CHECK(OnlyStays(c, 1, 2));
		CHECK(c.stay_share[1] >= 78.0 && c.stay_share[1] < 81.0 && c.stay_share[2] > 19.0 && c.stay_share[2] <= 22.0);

		// PAL 25 fps on 59.94 Hz (two vsyncs a frame, one generated): 50 pictures a second, 1 refresh 80%, 2 refreshes 20%.
		const Decision pal25 = Steady(2, kPal, kHz60);
		CHECK(pal25.presents == 1);
		const Cadence d = MeasureCadence(kPal, kHz60, 2, pal25.presents);
		CHECK(std::fabs(d.pictures_per_second - 50.0) < 0.5);
		CHECK(OnlyStays(d, 1, 2));
		CHECK(d.stay_share[1] >= 78.0 && d.stay_share[2] <= 22.0);

		// NTSC 59.94 -> 119.88: every picture for exactly one refresh.
		const Decision ntsc = Steady(1, kNtsc, kHz120);
		CHECK(ntsc.presents == 1);
		const Cadence e = MeasureCadence(kNtsc, kHz120, 1, ntsc.presents);
		CHECK(std::fabs(e.pictures_per_second - kHz120) < 0.5);
		CHECK(OnlyStays(e, 1) && e.stay_share[1] > 99.9);
	}

	RunGateTests();   // 2.11: OrbisFrameGenGate.h
	RunRegionTests(); // 2.11: PAL against NTSC

	printf("%s: %d of %d checks passed (frame generation's pacing and its status line)\n", s_failures ? "FAIL" : "PASS", s_checks - s_failures, s_checks);
	return s_failures ? 1 : 0;
}
