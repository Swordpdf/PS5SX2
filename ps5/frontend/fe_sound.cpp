// PS5 port frontend: the shelf's sounds (see fe_sound.h).
//
// Every sound is built from one ingredient, a key tap: a short pitched "pok" whose pitch drops as
// it sounds (the body), a burst of band-passed noise (the key bottoming out) and a faint ring (the
// plastic). A step adds the softer, higher tap of the key coming back up. The clips get a top cut
// and a touch of room (a few early reflections, different on each side), so they sound close and
// soft rather than sharp. vk-285-48 made them softer at the user's ask: 5-6 dB less loud, a rounder
// onset, a smaller and lower click and a darker top (12 dB less above 2.5 kHz).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_sound.h"

#include <algorithm>
#include <cmath>

namespace fe
{
namespace
{
constexpr double kTwoPi = 6.283185307179586;
constexpr float kRate = static_cast<float>(SoundBank::kRate);
constexpr float kLevel = 0.34f; // every clip's peak scales by this (vk-285-47 was 1.0)

struct Rng
{
	uint32_t s;
	// -1..1
	float Next()
	{
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return static_cast<float>(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
	}
};

struct Biquad
{
	float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
	float Run(float x)
	{
		const float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return y;
	}
};

// The RBJ cookbook band-pass, 0 dB at the centre.
Biquad BandPass(float hz, float q)
{
	const float w = static_cast<float>(kTwoPi) * hz / kRate, alpha = std::sin(w) / (2.0f * q), a0 = 1.0f + alpha;
	Biquad f;
	f.b0 = alpha / a0;
	f.b2 = -alpha / a0;
	f.a1 = -2.0f * std::cos(w) / a0;
	f.a2 = (1.0f - alpha) / a0;
	return f;
}

void LowPass(std::vector<float>& x, float hz)
{
	const float a = 1.0f - std::exp(-static_cast<float>(kTwoPi) * hz / kRate);
	float y = 0;
	for (float& v : x)
	{
		y += a * (v - y);
		v = y;
	}
}

struct Tap
{
	float body_hz = 640;          // where the body's pitch settles
	float glide = 0.30f;          // it starts this much higher ...
	float glide_s = 0.0035f;      // ... and falls with this time constant
	float body_s = 0.020f;        // the body's decay time constant
	float body_attack_s = 0.0015f;
	float click_hz = 3200, click_q = 0.9f, click_s = 0.0018f, click = 0.12f;
	float click_attack_s = 0.0006f;
	float ring_hz = 2350, ring_s = 0.006f, ring = 0.03f;
	uint32_t seed = 1;
};

void Grow(std::vector<float>& out, size_t size)
{
	if (out.size() < size)
		out.resize(size, 0.0f);
}

// Adds `tap` at `at` seconds.
void AddTap(std::vector<float>& out, float at, const Tap& tap, float gain)
{
	const float longest = std::max(tap.body_s, std::max(tap.click_s, tap.ring_s));
	const size_t n = static_cast<size_t>(longest * 7.0f * kRate); // to about -60 dB
	const size_t start = static_cast<size_t>(at * kRate);
	Grow(out, start + n);
	Rng rng{tap.seed * 0x9e3779b9u + 0x7f4a7c15u};
	Biquad bp = BandPass(tap.click_hz, tap.click_q);
	double phase = 0, ring_phase = 0;
	for (size_t i = 0; i < n; i++)
	{
		const float t = static_cast<float>(i) / kRate;
		const float hz = tap.body_hz * (1.0f + tap.glide * std::exp(-t / tap.glide_s));
		phase += kTwoPi * hz / kRate;
		ring_phase += kTwoPi * tap.ring_hz / kRate;
		const float body = static_cast<float>(std::sin(phase)) * (1.0f - std::exp(-t / tap.body_attack_s)) * std::exp(-t / tap.body_s);
		const float noise = bp.Run(rng.Next()) * 3.0f;
		const float click = noise * (1.0f - std::exp(-t / tap.click_attack_s)) * std::exp(-t / tap.click_s);
		const float ring = static_cast<float>(std::sin(ring_phase)) * (1.0f - std::exp(-t / 0.0003f)) * std::exp(-t / tap.ring_s);
		out[start + i] += gain * (body + tap.click * click + tap.ring * ring);
	}
}

// A soft bell-like note: the fundamental, a quieter octave and a brief shimmer.
void AddChime(std::vector<float>& out, float at, float hz, float decay_s, float gain)
{
	const size_t n = static_cast<size_t>(decay_s * 5.5f * kRate);
	const size_t start = static_cast<size_t>(at * kRate);
	Grow(out, start + n);
	for (size_t i = 0; i < n; i++)
	{
		const float t = static_cast<float>(i) / kRate;
		const double w = kTwoPi * hz * t;
		const float env = (1.0f - std::exp(-t / 0.008f)) * std::exp(-t / decay_s);
		const float v = static_cast<float>(std::sin(w)) + 0.28f * std::exp(-t / 0.06f) * static_cast<float>(std::sin(2.0 * w)) +
						0.02f * std::exp(-t / 0.02f) * static_cast<float>(std::sin(5.1 * w));
		out[start + i] += gain * env * v;
	}
}

// Mono in, stereo out: the top cut, the early reflections, a short fade at the end, and the level
// set so the loudest sample is `peak`.
std::vector<float> Finish(std::vector<float> mono, float peak, float top_hz = 5000.0f)
{
	peak *= kLevel;
	LowPass(mono, top_hz);
	std::vector<float> dark = mono;
	LowPass(dark, 4000.0f);
	struct Echo
	{
		float ms, gain;
	};
	const Echo left[2] = {{3.1f, 0.16f}, {7.3f, 0.09f}}, right[2] = {{4.3f, 0.16f}, {8.9f, 0.09f}};
	const size_t frames = mono.size() + static_cast<size_t>(0.010f * kRate);
	std::vector<float> lr(frames * 2, 0.0f);
	auto echo = [&](const Echo* taps, size_t i) {
		float sum = 0;
		for (int k = 0; k < 2; k++)
		{
			const size_t d = static_cast<size_t>(taps[k].ms * kRate / 1000.0f);
			if (i >= d && i - d < dark.size())
				sum += taps[k].gain * dark[i - d];
		}
		return sum;
	};
	for (size_t i = 0; i < frames; i++)
	{
		const float dry = i < mono.size() ? mono[i] : 0.0f;
		lr[2 * i] = dry + echo(left, i);
		lr[2 * i + 1] = dry + echo(right, i);
	}
	float loudest = 0;
	for (float v : lr)
		loudest = std::max(loudest, std::fabs(v));
	const float k = loudest > 0 ? peak / loudest : 0.0f;
	const size_t fade = static_cast<size_t>(0.004f * kRate);
	for (size_t i = 0; i < frames; i++)
	{
		const float g = i + fade > frames ? k * static_cast<float>(frames - i) / static_cast<float>(fade) : k;
		lr[2 * i] *= g;
		lr[2 * i + 1] *= g;
	}
	return lr;
}

// The key coming back up after a step: higher, shorter and softer than going down.
Tap KeyUp(const Tap& down, uint32_t seed)
{
	Tap up = down;
	up.body_hz = down.body_hz * 1.55f;
	up.glide = 0.25f;
	up.body_s = 0.007f;
	up.click_hz = 3800.0f;
	up.ring = 0.0f;
	up.seed = seed;
	return up;
}
} // namespace

void SoundBank::Build()
{
	// Six keys a little apart in pitch, so a run of steps doesn't sound like a machine.
	const float pitch[6] = {1.00f, 1.07f, 0.94f, 1.12f, 0.97f, 1.04f};
	m_move.clear();
	m_repeat.clear();
	for (int i = 0; i < 6; i++)
	{
		Tap down;
		down.body_hz = 640.0f * pitch[i];
		down.ring_hz = 2350.0f * pitch[i];
		down.seed = 10 + static_cast<uint32_t>(i);
		std::vector<float> m;
		AddTap(m, 0.0f, down, 1.0f);
		AddTap(m, 0.072f + 0.004f * static_cast<float>(i), KeyUp(down, 20 + static_cast<uint32_t>(i)), 0.15f);
		m_move.push_back(Finish(std::move(m), 0.30f));

		Tap quick = down;
		quick.body_s = 0.013f;
		quick.seed = 30 + static_cast<uint32_t>(i);
		std::vector<float> q;
		AddTap(q, 0.0f, quick, 1.0f);
		m_repeat.push_back(Finish(std::move(q), 0.20f));
	}

	// L1/R1: three quick taps rising (on) or falling (back), then the key coming up.
	auto jump = [](bool rising) {
		const float hz[3] = {560.0f, 680.0f, 830.0f}, gain[3] = {0.65f, 0.8f, 1.0f};
		std::vector<float> m;
		Tap last;
		for (int i = 0; i < 3; i++)
		{
			Tap t;
			t.body_hz = rising ? hz[i] : hz[2 - i];
			t.ring_hz = t.body_hz * 3.6f;
			t.body_s = 0.012f;
			t.seed = 40 + static_cast<uint32_t>(i) + (rising ? 0u : 3u);
			AddTap(m, 0.03f * static_cast<float>(i), t, gain[i]);
			last = t;
		}
		AddTap(m, 0.15f, KeyUp(last, 50), 0.14f);
		return Finish(std::move(m), 0.31f);
	};
	m_jump_left = jump(false);
	m_jump_right = jump(true);

	// The end of the shelf: a muted, lower "tuk" and a small bounce.
	{
		Tap t;
		t.body_hz = 290.0f;
		t.glide = 0.2f;
		t.body_s = 0.026f;
		t.click = 0.12f;
		t.click_hz = 2200.0f;
		t.ring = 0.0f;
		t.seed = 60;
		Tap bounce = t;
		bounce.body_hz = 320.0f;
		bounce.body_s = 0.014f;
		bounce.seed = 61;
		std::vector<float> m;
		AddTap(m, 0.0f, t, 1.0f);
		AddTap(m, 0.065f, bounce, 0.3f);
		m_edge = Finish(std::move(m), 0.34f, 3500.0f);
	}

	// A game picked: a fuller "enter" key, then two soft chime notes going up (G5, D6).
	{
		Tap enter;
		enter.body_hz = 380.0f;
		enter.glide = 0.35f;
		enter.body_s = 0.032f;
		enter.click = 0.15f;
		enter.click_hz = 3300.0f;
		enter.ring_hz = 1800.0f;
		enter.ring = 0.07f;
		enter.seed = 70;
		Tap rattle;
		rattle.body_hz = 950.0f;
		rattle.glide = 0.2f;
		rattle.body_s = 0.004f;
		rattle.click = 0.5f;
		rattle.click_hz = 3500.0f;
		rattle.ring = 0.0f;
		rattle.seed = 71;
		std::vector<float> m;
		AddTap(m, 0.0f, enter, 1.0f);
		AddTap(m, 0.011f, rattle, 0.10f);
		AddChime(m, 0.075f, 783.99f, 0.11f, 0.40f);
		AddChime(m, 0.155f, 1174.66f, 0.12f, 0.34f);
		m_launch = Finish(std::move(m), 0.42f);
	}
}

const std::vector<float>* SoundBank::Pick(Sfx sfx)
{
	auto pick = [this](const std::vector<std::vector<float>>& set, int& last) -> const std::vector<float>* {
		if (set.empty())
			return nullptr;
		Rng r{m_rng};
		const int n = static_cast<int>(set.size());
		int i = std::min(n - 1, static_cast<int>((r.Next() * 0.5f + 0.5f) * static_cast<float>(n)));
		m_rng = r.s;
		if (i == last)
			i = (i + 1) % n;
		last = i;
		return &set[static_cast<size_t>(i)];
	};
	switch (sfx)
	{
		case Sfx::Move: return pick(m_move, m_last_move);
		case Sfx::MoveRepeat: return pick(m_repeat, m_last_repeat);
		case Sfx::JumpLeft: return &m_jump_left;
		case Sfx::JumpRight: return &m_jump_right;
		case Sfx::Edge: return &m_edge;
		case Sfx::Launch: return &m_launch;
	}
	return nullptr;
}

void Mixer::Play(Sfx sfx, float pan)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::vector<float>* clip = m_bank.Pick(sfx);
	if (!clip || clip->empty())
		return;
	pan = std::max(-1.0f, std::min(1.0f, pan));
	Voice v;
	v.clip = clip;
	v.pos = 0;
	v.gl = (pan > 0.0f ? 1.0f - pan : 1.0f) * m_volume;
	v.gr = (pan < 0.0f ? 1.0f + pan : 1.0f) * m_volume;
	if (m_voices.size() >= kMaxVoices)
		m_voices.erase(m_voices.begin());
	m_voices.push_back(v);
}

void Mixer::Mix(float* out, int frames)
{
	std::fill(out, out + frames * 2, 0.0f);
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (Voice& v : m_voices)
		{
			const std::vector<float>& c = *v.clip;
			const size_t n = std::min((c.size() - v.pos) / 2, static_cast<size_t>(frames));
			for (size_t i = 0; i < n; i++)
			{
				out[2 * i] += c[v.pos + 2 * i] * v.gl;
				out[2 * i + 1] += c[v.pos + 2 * i + 1] * v.gr;
			}
			v.pos += 2 * n;
		}
		m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(), [](const Voice& v) { return v.pos >= v.clip->size(); }),
			m_voices.end());
	}
	// Several voices at once can add up: bend anything past 0.8 smoothly towards 1.
	for (int i = 0; i < frames * 2; i++)
	{
		const float x = out[i], a = std::fabs(x);
		if (a > 0.8f)
			out[i] = std::copysign(0.8f + 0.2f * std::tanh((a - 0.8f) / 0.2f), x);
	}
}

bool Mixer::Idle()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_voices.empty();
}
} // namespace fe
