// PS5 port (killzone-online, 2026-10-30, AI-assisted): the ticker's stall line (main-boot.cpp, once a second).
//
// Killzone online (#bugs 2026-10-30) froze ~34 s mid-match: the minute's perf line came 94 s after the one before and nothing said where
// the emulation was. Fed once a second with the EE's cycle count (still while the VM runs = stopped), the EE's and IOP's pc and what DEV9's
// TCP send is doing (OrbisDev9Send.h): a boot.log line after 2 s still and every 5 s after, and one settings.log line once it moves again
// (the session reports carry settings.log). Plain C++ for the host test (ps5/coreorbis/tests/dev9send).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

struct OrbisStallSample
{
	bool running = false;        // the VM runs (not paused, not in a menu)
	uint64_t ee_cycle = 0;
	uint32_t ee_pc = 0, iop_pc = 0;
	long long dev9_wait_ms = -1; // a DEV9 TCP send waiting now for that long; -1: none
	int dev9_port = 0;
	unsigned dev9_kept = 0;          // sends whose rest was kept for later, so far
	long long dev9_wait_max_ms = 0;  // the longest DEV9 send wait since the last sample
};

class OrbisStallWatch
{
public:
	// One second's sample. `boot_line`: a line for boot.log now (else empty); `event_line`: the stall that just ended, for settings.log.
	void Second(const OrbisStallSample& s, std::string* boot_line, std::string* event_line)
	{
		boot_line->clear();
		event_line->clear();
		const bool still = s.running && m_have && s.ee_cycle == m_cycle;
		if (still)
		{
			if (m_still == 0)
			{
				m_ee_pc = s.ee_pc;
				m_iop_pc = s.iop_pc;
				m_dev9_port = 0;
				m_dev9_wait = -1;
			}
			m_still++;
			if (s.dev9_wait_ms >= 0 && s.dev9_wait_ms > m_dev9_wait)
			{
				m_dev9_wait = s.dev9_wait_ms;
				m_dev9_port = s.dev9_port;
			}
			if (m_still == 2 || (m_still > 2 && m_still % 5 == 0))
				*boot_line = "[stall] the EE has not moved for " + std::to_string(m_still) + " s (" + Where(s) + ")";
		}
		else
		{
			if (m_still >= 2)
				*event_line = "stall: the emulation stopped for about " + std::to_string(m_still) + " s (" + Where(s) + ")";
			m_still = 0;
		}
		m_have = true;
		m_cycle = s.ee_cycle;
	}

private:
	std::string Where(const OrbisStallSample& s) const
	{
		char buf[200];
		std::snprintf(buf, sizeof(buf), "EE pc %08x, IOP pc %08x", m_ee_pc, m_iop_pc);
		std::string out = buf;
		if (m_dev9_wait >= 0)
			out += "; DEV9: a TCP send to port " + std::to_string(m_dev9_port) + " waiting " + std::to_string(m_dev9_wait) + " ms";
		else
			out += "; no DEV9 send waiting";
		if (s.dev9_kept)
			out += "; DEV9 sends kept for later so far: " + std::to_string(s.dev9_kept);
		return out;
	}

	bool m_have = false;
	uint64_t m_cycle = 0;
	int m_still = 0;
	uint32_t m_ee_pc = 0, m_iop_pc = 0;
	long long m_dev9_wait = -1;
	int m_dev9_port = 0;
};
