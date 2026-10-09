// PS5SX2 (vk-285-150, AI-assisted): the MMC (optical drive) commands PS5SX2 sends to the PS5's disc drive, and what it
// reads back, kept apart from the PS5 calls so a PC test can check every byte (tests/discscsi).
//
// Why: vk-285-149 on swordpdf's console copied a PS2 DVD at a flat 2.0x with the drive busy 100% of the time, and
// FreeBSD's CDRIOCREADSPEED (which sends SET CD SPEED, a CD command) came back EINVAL. Sony's own shell (SceShellCore,
// 11.40) carries FreeBSD's libcam and sends drive commands through the drive's pass device, with the same CAMIOCOMMAND /
// CAMGETPASSTHRU numbers as this SDK (0xC4E01902 / 0xC4E01903). For a DVD, the MMC way to ask for a read speed is SET
// STREAMING (0xB6) with a performance descriptor; GET PERFORMANCE (0xAC) and the capabilities page (MODE SENSE 0x2A) say
// what the drive offers. Needs proper testing on a console.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace orbis_mmc
{
	inline void Be16(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }
	inline void Be32(uint8_t* p, uint32_t v)
	{
		p[0] = static_cast<uint8_t>(v >> 24); p[1] = static_cast<uint8_t>(v >> 16);
		p[2] = static_cast<uint8_t>(v >> 8); p[3] = static_cast<uint8_t>(v);
	}
	inline uint32_t Get16(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 8) | p[1]; }
	inline uint32_t Get32(const uint8_t* p)
	{
		return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
	}

	// INQUIRY, 36 bytes: the drive's vendor, model and firmware.
	inline std::vector<uint8_t> Inquiry() { return {0x12, 0, 0, 0, 36, 0}; }
	inline std::string InquiryText(const uint8_t* d, size_t n)
	{
		if (n < 36)
			return "short";
		auto field = [&](size_t at, size_t len) {
			std::string s(reinterpret_cast<const char*>(d + at), len);
			while (!s.empty() && (s.back() == ' ' || s.back() == '\0'))
				s.pop_back();
			for (char& c : s)
				if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e)
					c = '?';
			return s;
		};
		return field(8, 8) + " | " + field(16, 16) + " | " + field(32, 4);
	}

	// MODE SENSE(10) of the capabilities page (0x2A), block descriptors off.
	inline std::vector<uint8_t> ModeSenseCaps(uint16_t len = 128) { return {0x5A, 0x08, 0x2A, 0, 0, 0, 0, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len), 0}; }
	struct Caps
	{
		bool ok = false;
		uint32_t max_read_kbs = 0, cur_read_kbs = 0; // bytes 8-9 and 14-15 of the page (obsolete in newer MMC; drives often still fill them)
	};
	inline Caps ParseCaps(const uint8_t* d, size_t n)
	{
		Caps c;
		if (n < 8)
			return c;
		const size_t bd = Get16(d + 6); // block descriptor length
		const size_t page = 8 + bd;
		if (n < page + 16 || (d[page] & 0x3f) != 0x2A)
			return c;
		c.ok = true;
		c.max_read_kbs = Get16(d + page + 8);
		c.cur_read_kbs = Get16(d + page + 14);
		return c;
	}

	// GET PERFORMANCE, type 0 (performance), nominal read performance (tolerance 10b), up to `count` descriptors.
	inline std::vector<uint8_t> GetPerformance(uint16_t count = 8)
	{
		std::vector<uint8_t> c(12, 0);
		c[0] = 0xAC;
		c[1] = 0x10; // tolerance 10b, read, nominal
		Be16(&c[8], count);
		c[10] = 0x00; // type: performance
		return c;
	}
	inline uint32_t GetPerformanceLen(uint16_t count = 8) { return 8 + 16u * count; }
	struct Perf
	{
		uint32_t start_lba, start_kbs, end_lba, end_kbs;
	};
	inline std::vector<Perf> ParsePerformance(const uint8_t* d, size_t n)
	{
		std::vector<Perf> out;
		if (n < 8)
			return out;
		const size_t len = Get32(d) + 4; // the length field doesn't count itself
		const size_t end = len < n ? len : n;
		for (size_t at = 8; at + 16 <= end; at += 16)
			out.push_back({Get32(d + at), Get32(d + at + 4), Get32(d + at + 8), Get32(d + at + 12)});
		return out;
	}

	// SET STREAMING with one performance descriptor: read `kb_per_s` kB each second over LBAs [start, end]. Exact = 0, so
	// the drive picks the nearest speed it can do; RDD = 0 (don't restore defaults); write the same (a reader ignores it).
	inline std::vector<uint8_t> SetStreaming() { std::vector<uint8_t> c(12, 0); c[0] = 0xB6; c[8] = 0x00; Be16(&c[9], 28); return c; }
	inline std::vector<uint8_t> StreamingDescriptor(uint32_t start_lba, uint32_t end_lba, uint32_t kb_per_s)
	{
		std::vector<uint8_t> d(28, 0);
		d[0] = 0x00;
		Be32(&d[4], start_lba);
		Be32(&d[8], end_lba);
		Be32(&d[12], kb_per_s); // read size, kB
		Be32(&d[16], 1000);     // read time, ms
		Be32(&d[20], kb_per_s); // write size, kB
		Be32(&d[24], 1000);     // write time, ms
		return d;
	}

	// SET CD SPEED (what FreeBSD's CDRIOCREADSPEED sends): kB/s, 0xFFFF = as fast as it goes. Sent once to see the
	// drive's own answer (its sense code) to the EINVAL the ioctl gave.
	inline std::vector<uint8_t> SetCdSpeed(uint16_t read_kbs = 0xFFFF)
	{
		std::vector<uint8_t> c(12, 0);
		c[0] = 0xBB;
		Be16(&c[2], read_kbs);
		Be16(&c[4], 0xFFFF);
		return c;
	}

	// Sense data, fixed (0x70/0x71) or descriptor (0x72/0x73) format: "key/asc/ascq".
	struct Sense
	{
		bool valid = false;
		uint8_t key = 0, asc = 0, ascq = 0;
	};
	inline Sense ParseSense(const uint8_t* s, size_t n)
	{
		Sense r;
		if (n < 1)
			return r;
		const uint8_t code = s[0] & 0x7f;
		if ((code == 0x70 || code == 0x71) && n >= 14)
			r = {true, static_cast<uint8_t>(s[2] & 0x0f), s[12], s[13]};
		else if ((code == 0x72 || code == 0x73) && n >= 4)
			r = {true, static_cast<uint8_t>(s[1] & 0x0f), s[2], s[3]};
		return r;
	}
	inline std::string SenseText(const Sense& s)
	{
		if (!s.valid)
			return "no sense data";
		static const char* const keys[16] = {"no sense", "recovered", "not ready", "medium error", "hardware error",
			"illegal request", "unit attention", "data protect", "blank check", "vendor", "copy aborted", "aborted",
			"equal", "volume overflow", "miscompare", "reserved"};
		char b[96];
		std::snprintf(b, sizeof(b), "%s (key %X, asc %02X, ascq %02X)", keys[s.key], s.key, s.asc, s.ascq);
		return b;
	}
} // namespace orbis_mmc
