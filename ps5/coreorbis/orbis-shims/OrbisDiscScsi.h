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
// From PR #34 (Heyde Moura): TEST UNIT READY, READ CD and READ SUB-CHANNEL, for a PS2 disc played from a drive.
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

	// vk-285-151: READ(10) and READ(12) of `count` 2048-byte sectors from `lba`. READ(12)'s Streaming bit (byte 10,
	// bit 7) asks for the drive's streaming read (less error recovery, steady speed).
	inline std::vector<uint8_t> Read10(uint32_t lba, uint16_t count)
	{
		std::vector<uint8_t> c(10, 0);
		c[0] = 0x28;
		Be32(&c[2], lba);
		Be16(&c[7], count);
		return c;
	}
	inline std::vector<uint8_t> Read12(uint32_t lba, uint32_t count, bool streaming)
	{
		std::vector<uint8_t> c(12, 0);
		c[0] = 0xA8;
		Be32(&c[2], lba);
		Be32(&c[6], count);
		c[10] = streaming ? 0x80 : 0x00;
		return c;
	}

	// vk-285-152: Sony's own read-speed command, as SceShellCore's _AutoMounter::OpticalDisc::setSpeed(int) builds it
	// (11.40, at 0x376865): a 12-byte CDB DB <rotation & 3> <speed hi> <speed lo> 00..., no data, 10 s, simple tag. The
	// speed is two nibbles "x.y" in its log ("set speed [speed:%d.%d kbyte/sec][rotation:%d]": (speed >> 4) & 15 and
	// speed & 15). Its table (0x1d823a0) holds rotation/speed pairs 0/0x20 (2.0, the default), 0/0x32, 0/0x26,
	// 1/0x50, 1/0x60, 1/0x80 (8.0), 2/0x40, 2/0x50, 1/0x100, 0/0xFFFF.
	inline std::vector<uint8_t> SieSetReadSpeed(uint8_t rotation, uint16_t speed)
	{
		std::vector<uint8_t> c(12, 0);
		c[0] = 0xDB;
		c[1] = rotation & 3;
		Be16(&c[2], speed);
		return c;
	}
	inline std::string SieSpeedText(uint16_t speed)
	{
		if (speed == 0xFFFF)
			return "max";
		char b[24];
		if (speed > 0xFF)
			std::snprintf(b, sizeof(b), "%#x", speed);
		else
			std::snprintf(b, sizeof(b), "%u.%ux", (speed >> 4) & 15, speed & 15);
		return b;
	}

	// PR #34 (Heyde Moura; AI-assisted port): what PCSX2's disc reader needs from a drive that cd(4) doesn't give
	// (OrbisDiscDrive.cpp, OrbisIOCtlSrc.cpp). TEST UNIT READY: no data; GOOD when a disc is in and spun up.
	inline std::vector<uint8_t> TestUnitReady() { return {0x00, 0, 0, 0, 0, 0}; }

	// READ CD: `count` raw 2352-byte sectors from `lba`, any sector type, the whole frame (sync, header and sub-header,
	// user data, EDC/ECC: byte 9 = 0xF8), no sub-channel.
	inline std::vector<uint8_t> ReadCd(uint32_t lba, uint32_t count)
	{
		std::vector<uint8_t> c(12, 0);
		c[0] = 0xBE;
		Be32(&c[2], lba);
		c[6] = static_cast<uint8_t>(count >> 16);
		Be16(&c[7], count & 0xFFFF);
		c[9] = 0xF8;
		return c;
	}
	// A raw CD data sector starts with the sync pattern 00 FF*10 00; a buffer the drive didn't fill doesn't.
	inline bool CdSyncOk(const uint8_t* sector)
	{
		static const uint8_t sync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
		return std::memcmp(sector, sync, sizeof(sync)) == 0;
	}

	// READ SUB-CHANNEL, the current position's Q data in MSF (SubQ on, format 01), `len` bytes.
	inline std::vector<uint8_t> ReadSubChannelQ(uint16_t len = 16)
	{
		std::vector<uint8_t> c(10, 0);
		c[0] = 0x42;
		c[1] = 0x02; // MSF
		c[2] = 0x40; // SubQ
		c[3] = 0x01; // current position
		Be16(&c[7], len);
		return c;
	}
	struct SubQ
	{
		bool ok = false;
		uint8_t adr = 0, control = 0, track = 0, index = 0;
	};
	// The answer: a 4-byte header (its length field, bytes 2-3, counts what follows) then the current position block,
	// format code 01 at byte 4. Anything shorter or of another format isn't taken (an unfilled buffer is all zeros).
	inline SubQ ParseSubChannelQ(const uint8_t* d, size_t n)
	{
		SubQ q;
		if (n < 8 || Get16(d + 2) < 4 || d[4] != 0x01)
			return q;
		q.ok = true;
		q.adr = d[5] >> 4;
		q.control = d[5] & 0x0f;
		q.track = d[6];
		q.index = d[7];
		return q;
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
