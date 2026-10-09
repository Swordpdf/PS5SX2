// PS5 port, PC test (vk-285-150, AI-assisted): orbis-shims/OrbisDiscScsi.h, the MMC commands sent to the PS5's disc
// drive and the parsing of its answers, against the MMC-6 layouts.
//   ps5/coreorbis/tests/discscsi/test-discscsi.sh
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../orbis-shims/OrbisDiscScsi.h"
#include <cstdio>
using namespace orbis_mmc;
static int fails = 0;
static void Check(bool ok, const std::string& w) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", w.c_str()); fails += !ok; }
int main()
{
	Check(Inquiry() == std::vector<uint8_t>({0x12, 0, 0, 0, 36, 0}), "INQUIRY: 6-byte CDB, 36 bytes asked");
	{
		uint8_t d[36] = {0x05, 0x80, 0x05, 0x32, 31};
		std::memcpy(d + 8, "SONY    ", 8);
		std::memcpy(d + 16, "PS-SYSTEM   408R", 16);
		std::memcpy(d + 32, "1.00", 4);
		Check(InquiryText(d, 36) == "SONY | PS-SYSTEM   408R | 1.00", "INQUIRY text: vendor | model | firmware, trailing spaces gone");
	}
	{
		const auto c = ModeSenseCaps(128);
		Check(c.size() == 10 && c[0] == 0x5A && c[1] == 0x08 && c[2] == 0x2A && c[7] == 0 && c[8] == 128, "MODE SENSE(10): page 2A, DBD, 128 bytes");
		uint8_t d[128] = {};
		d[7] = 0; // no block descriptors
		uint8_t* p = d + 8;
		p[0] = 0x2A; p[1] = 0x28;
		Be16(p + 8, 11080); Be16(p + 14, 2770);
		const Caps k = ParseCaps(d, sizeof(d));
		Check(k.ok && k.max_read_kbs == 11080 && k.cur_read_kbs == 2770, "capabilities page: max and current read speed");
		d[7] = 8; // 8 bytes of block descriptor push the page along
		std::memmove(d + 16, d + 8, 64);
		const Caps k2 = ParseCaps(d, sizeof(d));
		Check(k2.ok && k2.max_read_kbs == 11080, "capabilities page after a block descriptor");
		uint8_t z[128] = {};
		z[8] = 0x05;
		Check(!ParseCaps(z, sizeof(z)).ok, "another page isn't taken for 2A");
	}
	{
		const auto c = GetPerformance(8);
		Check(c.size() == 12 && c[0] == 0xAC && c[1] == 0x10 && c[8] == 0 && c[9] == 8 && c[10] == 0, "GET PERFORMANCE: type 0, nominal read, 8 descriptors");
		std::vector<uint8_t> d(GetPerformanceLen(8), 0);
		Be32(&d[0], 4 + 32); // header rest + two descriptors
		Be32(&d[8], 0); Be32(&d[12], 4155); Be32(&d[16], 1403583); Be32(&d[20], 11080);
		Be32(&d[24], 0); Be32(&d[28], 2770); Be32(&d[32], 1403583); Be32(&d[36], 2770);
		const auto p = ParsePerformance(d.data(), d.size());
		Check(p.size() == 2 && p[0].start_kbs == 4155 && p[0].end_kbs == 11080 && p[0].end_lba == 1403583 && p[1].start_kbs == 2770,
			"GET PERFORMANCE: two descriptors read, the rest of the buffer ignored");
	}
	{
		const auto c = SetStreaming();
		Check(c.size() == 12 && c[0] == 0xB6 && c[8] == 0 && c[9] == 0 && c[10] == 28, "SET STREAMING: type 0, 28-byte list");
		const auto d = StreamingDescriptor(0, 1403583, 22160);
		Check(d.size() == 28 && d[0] == 0 && Get32(&d[4]) == 0 && Get32(&d[8]) == 1403583 && Get32(&d[12]) == 22160 &&
				Get32(&d[16]) == 1000 && Get32(&d[20]) == 22160 && Get32(&d[24]) == 1000,
			"performance descriptor: LBAs, 22160 kB per 1000 ms, Exact and RDD off");
	}
	{
		const auto c = SetCdSpeed();
		Check(c.size() == 12 && c[0] == 0xBB && c[2] == 0xFF && c[3] == 0xFF && c[4] == 0xFF && c[5] == 0xFF, "SET CD SPEED: max read and write");
	}
	{
		uint8_t f[18] = {0x70, 0, 0x05, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0x24, 0x00};
		Check(SenseText(ParseSense(f, sizeof(f))) == "illegal request (key 5, asc 24, ascq 00)", "fixed sense: illegal request, invalid field in CDB");
		uint8_t d[8] = {0x72, 0x02, 0x04, 0x01};
		const Sense s = ParseSense(d, sizeof(d));
		Check(s.valid && s.key == 2 && s.asc == 4 && s.ascq == 1, "descriptor sense: not ready, becoming ready");
		uint8_t n[4] = {0x00};
		Check(!ParseSense(n, sizeof(n)).valid && SenseText(ParseSense(n, 4)) == "no sense data", "no sense data");
	}
	{
		const auto r = Read10(0x00123456, 64);
		Check(r == std::vector<uint8_t>({0x28, 0, 0x00, 0x12, 0x34, 0x56, 0, 0, 64, 0}), "READ(10): LBA and 64 sectors");
		const auto s = Read12(1403000, 512, true);
		Check(s.size() == 12 && s[0] == 0xA8 && Get32(&s[2]) == 1403000 && Get32(&s[6]) == 512 && s[10] == 0x80 && s[11] == 0,
			"READ(12): LBA, 512 sectors, Streaming bit");
		Check(Read12(1, 1, false)[10] == 0, "READ(12) without Streaming");
	}
	{
		const auto c = SieSetReadSpeed(1, 0x0080);
		Check(c == std::vector<uint8_t>({0xDB, 0x01, 0x00, 0x80, 0, 0, 0, 0, 0, 0, 0, 0}), "Sony's speed command: DB 01 00 80 (8.0, rotation 1), 12 bytes");
		Check(SieSetReadSpeed(5, 0x20)[1] == 1, "rotation keeps its low 2 bits, as SceShellCore's 'and $0x3'");
		Check(SieSetReadSpeed(0, 0xFFFF)[2] == 0xFF && SieSetReadSpeed(0, 0xFFFF)[3] == 0xFF, "max: FF FF");
		Check(SieSpeedText(0x20) == "2.0x" && SieSpeedText(0x80) == "8.0x" && SieSpeedText(0x32) == "3.2x" && SieSpeedText(0xFFFF) == "max",
			"speed text as SceShellCore logs it");
	}
	std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
