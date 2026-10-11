// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// PS5 port (vk-285-104, vk-285-107 for stderr): see OrbisDeferredLog.h. Needs proper testing.

#include "OrbisDeferredLog.h"

#include <atomic>
#include <cstdarg>
#include <mutex>
#include <string>
#include <vector>

extern "C" void orbis_event_log(const char* line) __attribute__((weak)); // the frontend (fe_ps5.cpp)

namespace
{
struct DeferredStream
{
	std::mutex mutex; // held only to append or to swap the buffer out, never during a write
	std::string buffer;
	unsigned long long dropped = 0; // lines dropped with the buffer full
};
DeferredStream s_out, s_err;
constexpr size_t kMaxBytes = 1u << 20;
std::mutex s_event_mutex;
std::vector<std::string> s_events; // settings-log lines waiting for the ticker
// Set by the first drain (the ticker's, once the game runs): until then the driver's lines are written as they come,
// so the boot, the shelf and a launch that fails keep them in order and none waits for a ticker that never starts.
std::atomic<bool> s_ticker_running{false};

void Append(DeferredStream& s, const char* text, size_t len)
{
	std::lock_guard<std::mutex> lock(s.mutex);
	if (s.buffer.size() + len > kMaxBytes)
	{
		s.dropped++;
		return;
	}
	if (s.buffer.capacity() == 0)
		s.buffer.reserve(64 * 1024);
	s.buffer.append(text, len);
}

int Format(DeferredStream& s, const char* fmt, va_list ap)
{
	char line[2048];
	const int n = vsnprintf(line, sizeof(line), fmt, ap);
	if (n <= 0)
		return n;
	const size_t len = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line) - 1;
	Append(s, line, len);
	return n;
}

// The buffer out to its stream. try_lock: the crash printer may run on a thread that faulted while appending.
void Drain(DeferredStream& s, FILE* stream, const char* name)
{
	std::string out;
	unsigned long long dropped = 0;
	{
		std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
		if (!lock.owns_lock())
			return;
		out.swap(s.buffer);
		dropped = s.dropped;
		s.dropped = 0;
	}
	if (!out.empty())
		std::fwrite(out.data(), 1, out.size(), stream);
	if (dropped)
		std::fprintf(stream, "[log] %llu %s lines dropped (the deferred buffer was full)\n", dropped, name);
	std::fflush(stream);
}
} // namespace

int OrbisDeferredPrintf(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	const int n = Format(s_out, fmt, ap);
	va_end(ap);
	return n;
}

int OrbisDeferredFprintf(FILE* stream, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n;
	if (stream == stdout)
		n = Format(s_out, fmt, ap);
	else if (stream == stderr)
		n = Format(s_err, fmt, ap);
	else
		n = std::vfprintf(stream, fmt, ap);
	va_end(ap);
	return n;
}

int OrbisDeferredFlush(FILE* stream)
{
	if (stream == stdout || stream == stderr)
		return 0; // the ticker thread writes the buffers and flushes the streams
	return std::fflush(stream);
}

extern "C" void ps5vk_log_hook(int stream, const char* text, size_t len)
{
	if (!text || !len)
		return;
	if (!s_ticker_running.load(std::memory_order_relaxed))
	{
		FILE* const f = stream == 1 ? stdout : stderr;
		std::fwrite(text, 1, len, f);
		return;
	}
	Append(stream == 1 ? s_out : s_err, text, len);
}

void OrbisDeferredEvent(const char* line)
{
	if (!line)
		return;
	std::lock_guard<std::mutex> lock(s_event_mutex);
	if (s_events.size() < 256)
		s_events.emplace_back(line);
}

extern "C" void orbis_log_drain()
{
	s_ticker_running.store(true, std::memory_order_relaxed);
	Drain(s_out, stdout, "stdout");
	Drain(s_err, stderr, "stderr");
	std::vector<std::string> events;
	{
		std::unique_lock<std::mutex> lock(s_event_mutex, std::try_to_lock);
		if (lock.owns_lock())
			events.swap(s_events);
	}
	if (orbis_event_log)
	{
		for (const std::string& e : events)
			orbis_event_log(e.c_str());
	}
}
