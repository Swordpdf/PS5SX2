// PS5SX2 (vk-285-141, AI-assisted): the controller's microphone as the PS2's USB microphone.
//
// Lifeline (Voice Action Adventure) won't start without a USB microphone; SingStar and others want one too. PCSX2 has the USB
// microphone and headset (pcsx2/USB/usb-mic), fed by an audio source that on a PC is cubeb; here ProsperoAudio.cpp's source
// reads this capture instead. The capture is libSceAudioIn's, the system library that gives a title the controller's
// microphone (or a headset plugged into it), loaded at run time like the keyboard's: looked up among the modules already
// loaded first, then loaded from /system. Its calls are taken to be the PS4's:
//   int sceAudioInOpen(int user, unsigned type, unsigned index, unsigned len, unsigned freq, unsigned param)
//   int sceAudioInInput(int handle, void* dest)      (blocks for one grain of len samples, returns len or an error)
//   int sceAudioInClose(int handle)
// with types 1 general, 0 voice chat, 5 voice recognition; freq 16000 (48000 tried too); param 0 = signed 16-bit mono.
// Each try is logged ([mic] lines), and so is the level every few seconds, so a log shows whether the voice gets through.
// When nothing opens, silence comes at the same pace: the game still sees a microphone that works.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProsperoMic.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>

extern "C" {
int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags, void* opt, int* res);
int sceKernelDlsym(int handle, const char* symbol, void** addrp);
int sceKernelGetModuleList(int32_t* handles, size_t max, size_t* count);
int sceUserServiceGetInitialUser(int32_t* user);
}

namespace
{
	using OpenFn = int (*)(int32_t user, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param);
	using InputFn = int (*)(int32_t handle, void* dest);
	using CloseFn = int (*)(int32_t handle);

	std::mutex s_lock;
	std::deque<int16_t> s_fifo; // mono, at s_rate; under s_lock
	double s_pos = 0.0;         // the next output sample's place in s_fifo; under s_lock
	int s_rate = 16000;         // the capture's rate; under s_lock
	int s_users = 0;            // under s_lock
	std::atomic<bool> s_run{false};
	std::thread s_thread;

	template <typename F>
	F Loaded(const char* name)
	{
		int32_t handles[256];
		size_t count = 0;
		if (sceKernelGetModuleList(handles, 256, &count) != 0)
			return nullptr;
		for (size_t i = 0; i < count && i < 256; i++)
		{
			void* a = nullptr;
			if (sceKernelDlsym(handles[i], name, &a) == 0 && a)
				return reinterpret_cast<F>(a);
		}
		return nullptr;
	}

	bool Resolve(OpenFn& open, InputFn& input, CloseFn& close)
	{
		open = Loaded<OpenFn>("sceAudioInOpen");
		input = Loaded<InputFn>("sceAudioInInput");
		close = Loaded<CloseFn>("sceAudioInClose");
		if (open && input)
		{
			printf("[mic] libSceAudioIn: already loaded\n");
			return true;
		}
		static const char* const dirs[] = {"/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/"};
		for (const char* dir : dirs)
		{
			const std::string path = std::string(dir) + "libSceAudioIn.sprx";
			struct stat st;
			if (stat(path.c_str(), &st) != 0)
				continue;
			int res = 0;
			const int m = sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, &res);
			printf("[mic] %s: load %#x (start result %d)\n", path.c_str(), static_cast<unsigned>(m), res);
			if (m < 0)
				continue;
			void* a = nullptr;
			if (sceKernelDlsym(m, "sceAudioInOpen", &a) == 0) open = reinterpret_cast<OpenFn>(a);
			if (sceKernelDlsym(m, "sceAudioInInput", &a) == 0) input = reinterpret_cast<InputFn>(a);
			if (sceKernelDlsym(m, "sceAudioInClose", &a) == 0) close = reinterpret_cast<CloseFn>(a);
			if (open && input)
				return true;
		}
		printf("[mic] libSceAudioIn: not available (open %p, input %p)\n", reinterpret_cast<void*>(open), reinterpret_cast<void*>(input));
		return false;
	}

	void Push(const int16_t* s, size_t n, int rate)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		if (rate != s_rate)
		{
			s_fifo.clear();
			s_pos = 0.0;
			s_rate = rate;
		}
		s_fifo.insert(s_fifo.end(), s, s + n);
		// At most half a second behind: a game that stops reading for a while doesn't get old speech later.
		const size_t cap = static_cast<size_t>(rate / 2);
		if (s_fifo.size() > cap)
		{
			s_fifo.erase(s_fifo.begin(), s_fifo.begin() + static_cast<long>(s_fifo.size() - cap));
			s_pos = 0.0;
		}
	}

	void Run()
	{
		OpenFn open = nullptr;
		InputFn input = nullptr;
		CloseFn close = nullptr;
		int handle = -1, rate = 16000;
		constexpr uint32_t kLen = 256;
		if (Resolve(open, input, close))
		{
			int32_t user = -1;
			sceUserServiceGetInitialUser(&user);
			struct Try
			{
				uint32_t type, freq;
			};
			static const Try tries[] = {{1, 16000}, {0, 16000}, {5, 16000}, {1, 48000}, {0, 48000}};
			for (const Try& t : tries)
			{
				const int h = open(user, t.type, 0, kLen, t.freq, 0);
				printf("[mic] sceAudioInOpen(user %d, type %u, %u Hz, %u samples, S16 mono) -> %#x\n", user, t.type, t.freq, kLen,
					static_cast<unsigned>(h));
				if (h >= 0)
				{
					handle = h;
					rate = static_cast<int>(t.freq);
					break;
				}
			}
		}
		if (handle < 0)
			printf("[mic] no microphone opened: the game gets silence\n");
		fflush(stdout);

		int16_t buf[kLen * 2];
		unsigned errors = 0, windows = 0;
		int peak = 0;
		double sum2 = 0.0;
		size_t count = 0;
		auto window = std::chrono::steady_clock::now();
		auto next_silence = std::chrono::steady_clock::now();
		while (s_run.load(std::memory_order_relaxed))
		{
			if (handle >= 0)
			{
				const int n = input(handle, buf);
				if (n < 0)
				{
					if (++errors <= 5)
						printf("[mic] sceAudioInInput -> %#x\n", static_cast<unsigned>(n));
					if (errors >= 50)
					{
						printf("[mic] the microphone keeps failing: silence from now on\n");
						if (close)
							close(handle);
						handle = -1;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}
				const size_t got = std::min<size_t>(n > 0 ? static_cast<size_t>(n) : kLen, kLen);
				Push(buf, got, rate);
				for (size_t i = 0; i < got; i++)
				{
					peak = std::max(peak, std::abs(static_cast<int>(buf[i])));
					sum2 += static_cast<double>(buf[i]) * buf[i];
				}
				count += got;
			}
			else
			{
				// Silence at the capture's pace: 256 samples at 16 kHz every 16 ms.
				std::memset(buf, 0, sizeof(buf));
				Push(buf, kLen, 16000);
				next_silence += std::chrono::microseconds(16000);
				std::this_thread::sleep_until(next_silence);
			}
			const auto now = std::chrono::steady_clock::now();
			if (handle >= 0 && now - window >= std::chrono::seconds(5) && windows < 60)
			{
				windows++;
				printf("[mic] last 5 s: peak %d, rms %.0f (%zu samples; 0 means nothing heard)\n", peak,
					count ? std::sqrt(sum2 / count) : 0.0, count);
				fflush(stdout);
				peak = 0;
				sum2 = 0.0;
				count = 0;
				window = now;
			}
		}
		if (handle >= 0 && close)
			close(handle);
	}
} // namespace

namespace orbis_mic
{
	void Acquire()
	{
		std::lock_guard<std::mutex> lock(s_lock);
		if (s_users++ > 0)
			return;
		if (s_thread.joinable())
			s_thread.join();
		s_run.store(true);
		s_thread = std::thread(Run);
		printf("[mic] capture started\n");
		fflush(stdout);
	}

	void Release()
	{
		std::thread t;
		{
			std::lock_guard<std::mutex> lock(s_lock);
			if (s_users == 0 || --s_users > 0)
				return;
			s_run.store(false);
			t = std::move(s_thread);
			s_fifo.clear();
			s_pos = 0.0;
		}
		if (t.joinable())
			t.join();
		printf("[mic] capture stopped\n");
		fflush(stdout);
	}

	size_t Available(int rate)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		if (rate <= 0 || s_fifo.size() < 2)
			return 0;
		const double left = static_cast<double>(s_fifo.size() - 1) - s_pos;
		return left > 0 ? static_cast<size_t>(left * rate / s_rate) : 0;
	}

	size_t Read(int rate, int16_t* out, size_t frames, unsigned channels)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		if (rate <= 0 || channels == 0)
			return 0;
		const double step = static_cast<double>(s_rate) / rate;
		size_t done = 0;
		while (done < frames && s_pos + 1.0 < static_cast<double>(s_fifo.size()))
		{
			const size_t i = static_cast<size_t>(s_pos);
			const double f = s_pos - static_cast<double>(i);
			const double v = s_fifo[i] * (1.0 - f) + s_fifo[i + 1] * f;
			const int16_t s = static_cast<int16_t>(std::clamp(std::lround(v), -32768L, 32767L));
			for (unsigned c = 0; c < channels; c++)
				out[done * channels + c] = s;
			done++;
			s_pos += step;
		}
		const size_t drop = std::min(static_cast<size_t>(s_pos), s_fifo.size());
		s_fifo.erase(s_fifo.begin(), s_fifo.begin() + static_cast<long>(drop));
		s_pos -= static_cast<double>(drop);
		return done;
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(s_lock);
		s_fifo.clear();
		s_pos = 0.0;
	}
} // namespace orbis_mic
