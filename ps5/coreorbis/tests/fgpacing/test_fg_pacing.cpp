// PS5SX2 (vk-285-133, AI-assisted): a check of frame generation's pacing (orbis-shims/OrbisFrameGenPacing.h) on a PC.
//   g++ -std=c++17 -Wall -Wextra -I../../orbis-shims -o /tmp/test_fg_pacing test_fg_pacing.cpp && /tmp/test_fg_pacing
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "OrbisFrameGenPacing.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

	printf("%s: %d of %d checks passed (frame generation's pacing)\n", s_failures ? "FAIL" : "PASS", s_checks - s_failures, s_checks);
	return s_failures ? 1 : 0;
}
