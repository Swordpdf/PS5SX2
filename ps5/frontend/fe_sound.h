// PS5 port frontend: the shelf's sounds. Small key-press sounds, synthesized when the frontend
// starts (nothing is loaded from disk), and a voice mixer that the platform's audio thread pulls
// from. The shelf only reports what happened; whether anything plays is the platform's business.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace fe
{
enum class Sfx
{
	Move,       // one step along the shelf
	MoveRepeat, // a step while the direction is held (quieter, shorter)
	JumpLeft,   // L1: five back
	JumpRight,  // R1: five on
	Edge,       // a step past either end
	Launch,     // a game picked
};

class SoundSink
{
public:
	virtual ~SoundSink() = default;
	// pan: -1 left .. 1 right.
	virtual void Play(Sfx sfx, float pan) = 0;
};

// Interleaved stereo float clips at kRate.
class SoundBank
{
public:
	static constexpr int kRate = 48000;

	void Build();
	// A clip for `sfx`; the key sounds come in a few variants, never the same one twice running.
	const std::vector<float>* Pick(Sfx sfx);

private:
	std::vector<std::vector<float>> m_move, m_repeat;
	std::vector<float> m_jump_left, m_jump_right, m_edge, m_launch;
	uint32_t m_rng = 0x6d2b79f5u;
	int m_last_move = -1, m_last_repeat = -1;
};

class Mixer final : public SoundSink
{
public:
	explicit Mixer(float volume = 1.0f) : m_volume(volume) {}

	void Build() { m_bank.Build(); }
	void Play(Sfx sfx, float pan) override; // any thread
	// The audio thread: `frames` of interleaved stereo float into `out`.
	void Mix(float* out, int frames);
	bool Idle();

private:
	struct Voice
	{
		const std::vector<float>* clip;
		size_t pos;
		float gl, gr;
	};
	static constexpr size_t kMaxVoices = 12;

	std::mutex m_mutex;
	std::vector<Voice> m_voices;
	SoundBank m_bank;
	float m_volume;
};
} // namespace fe
