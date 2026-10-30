// PS5 port, PC test (killzone-online, 2026-10-30, AI-assisted): DEV9's TCP send never waits long on the emulation's thread
// (include-orbis/OrbisDev9Send.h) and the ticker's stall line (include-orbis/OrbisStall.h). The send runs on a real socket pair whose other
// end stops reading, as the game server did in the Killzone report (#bugs 2026-10-30: a ~34 s freeze, then "TCP: Send error: 32").
//   ps5/coreorbis/tests/dev9send/test-dev9send.sh
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisDev9Send.h"
#include "OrbisStall.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

static int g_fails = 0;

static void Check(bool ok, const std::string& what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
	if (!ok)
		g_fails++;
}

static long long Ms(std::chrono::steady_clock::time_point a)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - a).count();
}

struct Pair
{
	int a = -1, b = -1;
	Pair()
	{
		int sv[2];
		socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
		a = sv[0];
		b = sv[1];
		fcntl(a, F_SETFL, fcntl(a, F_GETFL) | O_NONBLOCK);
		int small = 16384;
		setsockopt(a, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
		setsockopt(b, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
	}
	~Pair()
	{
		if (a >= 0)
			close(a);
		if (b >= 0)
			close(b);
	}
	OrbisDev9SendResult Send(std::vector<uint8_t>& pending, const std::vector<uint8_t>& data, int budget_ms)
	{
		return OrbisDev9Send(
			[this](const uint8_t* p, int n) {
				const ssize_t r = send(a, p, static_cast<size_t>(n), MSG_NOSIGNAL);
				return r < 0 ? -errno : static_cast<int>(r);
			},
			[this](std::chrono::microseconds w) {
				pollfd pfd{a, POLLOUT, 0};
				poll(&pfd, 1, static_cast<int>((w.count() + 999) / 1000));
			},
			pending, data.empty() ? nullptr : data.data(), static_cast<int>(data.size()), std::chrono::milliseconds(budget_ms), 11001);
	}
	// Everything readable now.
	void Drain(std::vector<uint8_t>& got)
	{
		uint8_t buf[65536];
		while (true)
		{
			const ssize_t r = recv(b, buf, sizeof(buf), MSG_DONTWAIT);
			if (r <= 0)
				break;
			got.insert(got.end(), buf, buf + r);
		}
	}
};

static std::vector<uint8_t> Bytes(size_t n, uint32_t seed)
{
	std::vector<uint8_t> v(n);
	for (size_t i = 0; i < n; i++)
	{
		seed = seed * 1103515245u + 12345u;
		v[i] = static_cast<uint8_t>(seed >> 16);
	}
	return v;
}

int main()
{
	signal(SIGPIPE, SIG_IGN);
	{
		// The server reads nothing: a 1 MB send returns after the budget, keeping the rest; nothing is lost or reordered later.
		Pair p;
		std::vector<uint8_t> pending, got;
		const std::vector<uint8_t> a = Bytes(1 << 20, 1), b = Bytes(300000, 2), c = Bytes(1000, 3);
		auto t = std::chrono::steady_clock::now();
		OrbisDev9SendResult r = p.Send(pending, a, 20);
		const long long took = Ms(t);
		Check(r.error == 0 && !r.overflow && r.kept_now > 0 && pending.size() == static_cast<size_t>(r.kept_now),
			"server full: the send keeps " + std::to_string(r.kept_now) + " of " + std::to_string(a.size()) + " bytes");
		Check(took < 200, "and returns after " + std::to_string(took) + " ms (budget 20 ms; the old loop waited until the socket failed)");
		Check(g_orbis_dev9_send_since_us.load() == 0 && g_orbis_dev9_wait_max_us.load() >= 15000, "the wait is noted for the stall line, and ends");
		r = p.Send(pending, b, 20);
		Check(r.error == 0 && r.kept_now == static_cast<int>(b.size()), "a send while data waits keeps all of it (after the earlier)");
		// The receive thread's passes (no wait) while the server reads again.
		t = std::chrono::steady_clock::now();
		while (!pending.empty() && Ms(t) < 5000)
		{
			p.Drain(got);
			r = p.Send(pending, {}, 0);
			if (r.error)
				break;
		}
		r = p.Send(pending, c, 20);
		p.Drain(got);
		for (int i = 0; i < 50 && got.size() < a.size() + b.size() + c.size(); i++)
		{
			usleep(1000);
			p.Drain(got);
		}
		std::vector<uint8_t> want = a;
		want.insert(want.end(), b.begin(), b.end());
		want.insert(want.end(), c.begin(), c.end());
		Check(pending.empty() && got == want, "all " + std::to_string(want.size()) + " bytes arrive, in order (got " + std::to_string(got.size()) + ")");
		Check(g_orbis_dev9_kept.load() == 2, "two sends had their rest kept (" + std::to_string(g_orbis_dev9_kept.load()) + ")");
	}
	{
		// The server closed: an error, as before (the session closes with RST).
		Pair p;
		close(p.b);
		p.b = -1;
		std::vector<uint8_t> pending;
		const OrbisDev9SendResult r = p.Send(pending, Bytes(5000, 4), 20);
		Check(r.error == EPIPE && r.kept_now == 0 && pending.empty(), "server gone: EPIPE (" + std::to_string(r.error) + "), nothing kept");
	}
	{
		// A server that takes nothing for long: over 4 MB kept is an overflow (the session closes).
		Pair p;
		std::vector<uint8_t> pending;
		OrbisDev9SendResult r;
		for (int i = 0; i < 6 && !r.overflow; i++)
			r = p.Send(pending, Bytes(1 << 20, 10 + i), 1);
		Check(r.overflow && r.error == 0 && pending.size() > kOrbisDev9MaxPending, "over 4 MB kept: overflow");
	}
	{
		// A plain send with room: all of it, nothing kept, no wait.
		Pair p;
		std::vector<uint8_t> pending, got;
		const std::vector<uint8_t> d = Bytes(1460, 5);
		const OrbisDev9SendResult r = p.Send(pending, d, 20);
		p.Drain(got);
		Check(r.error == 0 && r.kept_now == 0 && pending.empty() && got == d, "a segment with room: sent whole");
	}
	{
		// The stall line.
		OrbisStallWatch w;
		std::string boot, ev;
		OrbisStallSample s;
		s.running = true;
		s.ee_pc = 0x2b34a0;
		s.iop_pc = 0x51c0;
		std::vector<std::string> boots;
		bool event_early = false;
		for (int i = 0; i < 40; i++)
		{
			s.ee_cycle = i < 5 ? 1000u * i : 5000; // moving, then still for 34 s
			s.dev9_wait_ms = i >= 6 ? (i - 5) * 1000 : -1;
			s.dev9_port = 10077;
			w.Second(s, &boot, &ev);
			if (!boot.empty())
				boots.push_back(boot);
			event_early = event_early || !ev.empty();
		}
		Check(!event_early, "no settings.log line while it lasts");
		Check(boots.size() == 7 && boots[0] == "[stall] the EE has not moved for 2 s (EE pc 002b34a0, IOP pc 000051c0; DEV9: a TCP send to port 10077 waiting 2000 ms)",
			"boot.log after 2 s, then every 5 s: " + (boots.empty() ? std::string("none") : boots[0]) + " (" + std::to_string(boots.size()) + " lines)");
		s.ee_cycle = 6000;
		s.dev9_wait_ms = -1;
		s.dev9_kept = 0;
		w.Second(s, &boot, &ev);
		Check(ev == "stall: the emulation stopped for about 34 s (EE pc 002b34a0, IOP pc 000051c0; DEV9: a TCP send to port 10077 waiting 34000 ms)",
			"settings.log when it moves again: " + ev);
		// Paused (a menu): no stall.
		OrbisStallWatch w2;
		s.running = false;
		for (int i = 0; i < 10; i++)
			w2.Second(s, &boot, &ev);
		Check(boot.empty() && ev.empty(), "paused: nothing");
		// One still second: nothing.
		OrbisStallWatch w3;
		s.running = true;
		s.ee_cycle = 1;
		w3.Second(s, &boot, &ev);
		w3.Second(s, &boot, &ev);
		s.ee_cycle = 2;
		w3.Second(s, &boot, &ev);
		Check(boot.empty() && ev.empty(), "one still second: nothing");
	}
	std::printf(g_fails ? "%d FAILED\n" : "all passed\n", g_fails);
	return g_fails ? 1 : 0;
}
