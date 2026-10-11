// PS5SX2 (vk-285-141, AI-assisted): the controller's microphone (libSceAudioIn), for PCSX2's USB microphone and headset.
// See ProsperoMic.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace orbis_mic
{
	// Starts capturing (the first user); Release stops it after the last user. A console that gives no microphone gets
	// silence at the same pace, so a game still sees a working USB microphone.
	void Acquire();
	void Release();

	// The samples captured, mono, resampled to `rate` Hz: how many can be read now, and reading them (each one put in
	// `channels` channels). Returns the frames written.
	size_t Available(int rate);
	size_t Read(int rate, int16_t* out, size_t frames, unsigned channels);
	// Drops what was captured (a rate change, a new stream).
	void Reset();
} // namespace orbis_mic
