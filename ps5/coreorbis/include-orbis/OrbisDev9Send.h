// PS5 port (killzone-online, 2026-10-30, AI-assisted): DEV9's TCP send to a server, never waiting long on the emulation's thread.
//
// PCSX2's sockets adapter sends the PS2's TCP data from the emulation's thread and waited, with no limit, while the socket had no room
// (pcsx2/DEV9/Sessions/TCP_Session/TCP_Session_Out.cpp: a loop on EWOULDBLOCK). Killzone online (#bugs 2026-10-30, PS Rewired, a PS5 Pro)
// froze ~34 s mid-match: the minute's perf line came 94 s after the one before, then "TCP: Send error: 32" and the server dropped the
// game, while a real PS2 in the same match stayed. Here the wait has a budget (20 ms on that path); what is left is kept, in order, and
// sent by the receive thread (TCP_Session::Recv, no wait) and by the next sends. The PS2 is answered (ACK) as before. Plain C++ with the
// socket calls passed in, so the host test (ps5/coreorbis/tests/dev9send) runs it on a socket pair.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <vector>

// For the ticker's stall line (main-boot.cpp): a send waiting for room now (since when, steady clock in microseconds; 0: none) and its
// server port; how often data had to be kept, and the longest wait.
inline std::atomic<long long> g_orbis_dev9_send_since_us{0};
inline std::atomic<int> g_orbis_dev9_send_port{0};
inline std::atomic<unsigned> g_orbis_dev9_kept{0};
inline std::atomic<long long> g_orbis_dev9_wait_max_us{0};

// More kept than this for one connection: the server has taken nothing for long (the connection is closed with an error).
inline constexpr size_t kOrbisDev9MaxPending = 4u << 20;

struct OrbisDev9SendResult
{
	int error = 0;        // a socket error (errno), 0 when none
	bool overflow = false; // more than kOrbisDev9MaxPending kept
	int kept_now = 0;      // bytes of `data` kept for later
};

inline long long OrbisDev9NowUs()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Sends `pending` (what was kept before) and then `data` (`len` bytes, may be null), in that order, through send_fn(const uint8_t*, int)
// -> bytes sent or -errno. While the socket has no room (-EWOULDBLOCK / -EAGAIN) it waits through wait_fn(microseconds) until `budget` is
// spent (0: no wait); whatever is left of `data` is appended to `pending`.
template <class SendFn, class WaitFn>
OrbisDev9SendResult OrbisDev9Send(SendFn send_fn, WaitFn wait_fn, std::vector<uint8_t>& pending, const uint8_t* data, int len,
	std::chrono::microseconds budget, int port)
{
	OrbisDev9SendResult r;
	const long long start = OrbisDev9NowUs();
	const bool waits = budget.count() > 0;
	if (waits)
	{
		g_orbis_dev9_send_port.store(port, std::memory_order_relaxed);
		g_orbis_dev9_send_since_us.store(start, std::memory_order_release);
	}
	bool full = false;
	// One buffer: false when the socket stays full past the budget (or an error).
	auto push = [&](const uint8_t* p, int n, int* done) {
		*done = 0;
		while (*done < n)
		{
			const int ret = send_fn(p + *done, n - *done);
			if (ret > 0)
			{
				*done += ret;
				continue;
			}
			if (ret == -EWOULDBLOCK || ret == -EAGAIN || ret == 0)
			{
				const long long left = budget.count() - (OrbisDev9NowUs() - start);
				if (left <= 0)
				{
					full = true;
					return false;
				}
				wait_fn(std::chrono::microseconds(std::min<long long>(left, 2000)));
				continue;
			}
			r.error = -ret;
			return false;
		}
		return true;
	};
	int done = 0;
	if (!pending.empty())
	{
		const bool all = push(pending.data(), static_cast<int>(pending.size()), &done);
		pending.erase(pending.begin(), pending.begin() + done);
		if (!all)
		{
			if (data && len > 0 && r.error == 0)
			{
				pending.insert(pending.end(), data, data + len);
				r.kept_now = len;
			}
			data = nullptr;
		}
	}
	if (data && len > 0 && r.error == 0)
	{
		push(data, len, &done);
		if (r.error == 0 && done < len)
		{
			pending.insert(pending.end(), data + done, data + len);
			r.kept_now = len - done;
		}
	}
	if (r.kept_now > 0)
		g_orbis_dev9_kept.fetch_add(1, std::memory_order_relaxed);
	r.overflow = r.error == 0 && pending.size() > kOrbisDev9MaxPending;
	(void)full;
	if (waits)
	{
		g_orbis_dev9_send_since_us.store(0, std::memory_order_release);
		const long long took = OrbisDev9NowUs() - start;
		long long prev = g_orbis_dev9_wait_max_us.load(std::memory_order_relaxed);
		while (took > prev && !g_orbis_dev9_wait_max_us.compare_exchange_weak(prev, took, std::memory_order_relaxed))
		{
		}
	}
	return r;
}
