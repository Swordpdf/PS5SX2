// PS5SX2 (2.11, AI-assisted): PC test of OrbisDiscReadAhead.h against a fake disc. Counts what reaches the "drive": commands, and above all the
// jumps between them (a jump is a seek, 150-250 ms on a real drive).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisDiscReadAhead.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using orbis_disc::ReadAhead;

static int s_failed = 0;
static void Check(bool ok, const char* what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		s_failed++;
}

// The data of sector `lsn` is a function of the sector, so any copy can be checked.
static uint8_t Byte(uint32_t lsn, uint32_t i)
{
	return static_cast<uint8_t>((lsn * 2654435761u + i * 40503u) >> 13);
}
static bool Matches(uint32_t sector, uint32_t count, const uint8_t* p)
{
	for (uint32_t s = 0; s < count; s++)
		for (uint32_t i = 0; i < 2048; i += 97)
			if (p[s * 2048 + i] != Byte(sector + s, i))
				return false;
	return true;
}

struct Drive
{
	uint32_t total = 0;
	uint32_t bad_from = 0xFFFFFFFF, bad_to = 0; // sectors in [bad_from, bad_to) fail
	uint64_t commands = 0, sectors = 0, jumps = 0;
	uint32_t next = 0xFFFFFFFF;      // where a read that carries on would start
	uint32_t overrun = 0;            // reads past the end of the disc
	std::vector<uint32_t> starts;

	bool Read(uint32_t sector, uint32_t count, uint8_t* out)
	{
		commands++;
		starts.push_back(sector);
		if (total && sector + count > total)
			overrun++;
		if (next == 0xFFFFFFFF || sector + 1000 < next || sector > next + 1000)
			jumps++;
		next = sector + count;
		if (sector < bad_to && sector + count > bad_from)
			return false;
		sectors += count;
		for (uint32_t s = 0; s < count; s++)
			for (uint32_t i = 0; i < 2048; i++)
				out[s * 2048 + i] = Byte(sector + s, i);
		return true;
	}
};

static bool Get(ReadAhead& ra, Drive& d, uint32_t sector, uint32_t count, bool want_ok = true)
{
	std::vector<uint8_t> buf(static_cast<size_t>(count) * 2048, 0xEE);
	const bool ok = ra.Read(sector, count, buf.data(), d.total, [&](uint32_t s, uint32_t c, uint8_t* b) { return d.Read(s, c, b); });
	if (ok != want_ok)
		return false;
	return !ok || Matches(sector, count, buf.data());
}

int main()
{
	// 1. one pass over the disc in 16-sector blocks, as the CDVD thread reads it: right bytes, far fewer commands, no overread
	{
		ReadAhead ra;
		Drive d;
		d.total = 40000;
		bool ok = true;
		for (uint32_t s = 0; s + 16 <= 20000; s += 16)
			ok = Get(ra, d, s, 16) && ok;
		Check(ok, "sequential 16-sector blocks: every byte right");
		Check(d.commands < 100 && d.jumps <= 1, "sequential: ~80 commands instead of 1250 and one seek");
		Check(d.sectors <= 20000 + 512, "sequential: at most one window read past what was asked");
		Check(d.overrun == 0, "sequential: nothing read past the end");
		bool grew = d.starts.size() > 3 && d.starts[2] - d.starts[1] == 128 && d.starts[3] - d.starts[2] == 256;
		Check(grew, "sequential: the windows grow 64 -> 128 -> 256 sectors");
	}
	// 2. two places read in turn (a game streaming two files): after the first visits every command is 256 sectors, and the drive seeks about once
	//    for every 256 sectors read instead of for every 16
	{
		ReadAhead ra;
		Drive d;
		d.total = 600000;
		bool ok = true;
		uint32_t a = 1000, b = 500000;
		for (int i = 0; i < 400; i++)
		{
			ok = Get(ra, d, a, 16) && Get(ra, d, b, 16) && ok;
			a += 16;
			b += 16;
		}
		Check(ok, "two streams in turn: every byte right");
		std::printf("info  two streams: %llu commands, %llu seeks for 800 visits of 16 sectors\n", (unsigned long long)d.commands, (unsigned long long)d.jumps);
		Check(d.jumps < 60, "two streams in turn: under 60 seeks for 800 visits (800 without read-ahead)");
		Check(d.sectors <= 2 * 400 * 16 + 2 * 512, "two streams in turn: no more than the windows' worth read");
	}
	// 3. five places in turn: more than the 4 windows, still right
	{
		ReadAhead ra;
		Drive d;
		d.total = 900000;
		bool ok = true;
		uint32_t p[5] = {100, 200000, 400000, 600000, 800000};
		for (int i = 0; i < 300; i++)
			for (int k = 0; k < 5; k++)
			{
				ok = Get(ra, d, p[k], 16) && ok;
				p[k] += 16;
			}
		Check(ok, "five streams (more than the windows): every byte right");
	}
	// 4. random reads of any size and place: right bytes
	{
		ReadAhead ra;
		Drive d;
		d.total = 300000;
		bool ok = true;
		std::srand(7);
		for (int i = 0; i < 3000; i++)
		{
			const uint32_t count = 1 + static_cast<uint32_t>(std::rand() % 300);
			const uint32_t sector = static_cast<uint32_t>(std::rand()) % (d.total - count);
			ok = Get(ra, d, sector, count) && ok;
		}
		Check(ok, "random reads of 1 to 300 sectors: every byte right");
		Check(d.overrun == 0, "random reads: nothing read past the end");
	}
	// 5. the end of the disc: windows are cut, nothing is read past it
	{
		ReadAhead ra;
		Drive d;
		d.total = 10000;
		bool ok = Get(ra, d, 9984, 16) && Get(ra, d, 9968, 16) && Get(ra, d, 9000, 16);
		Check(ok, "end of the disc: right bytes");
		Check(d.overrun == 0, "end of the disc: no read past it");
		// a read that goes past the end of the disc is the drive's to refuse: passed on as it is
		Drive d2;
		d2.total = 10000;
		ReadAhead ra2;
		std::vector<uint8_t> buf(16 * 2048);
		ra2.Read(9995, 16, buf.data(), 10000, [&](uint32_t s, uint32_t c, uint8_t* b) { return d2.Read(s, c, b); });
		Check(d2.commands == 1 && d2.starts[0] == 9995, "a read past the end: handed to the drive as it is");
	}
	// 6. unknown disc size (total 0): works, windows not cut
	{
		ReadAhead ra;
		Drive d;
		d.total = 0;
		bool ok = true;
		for (uint32_t s = 0; s < 4000; s += 16)
			ok = Get(ra, d, s, 16) && ok;
		Check(ok && d.commands < 40, "disc size unknown: right bytes, few commands");
	}
	// 7. a bad place: one failed window, the good requests around it still come, the bad ones fail, then windows go on
	{
		ReadAhead ra;
		Drive d;
		d.total = 100000;
		d.bad_from = 5000;
		d.bad_to = 5100;
		bool ok = Get(ra, d, 4000, 16);
		ok = Get(ra, d, 4944, 16) && ok;                 // window 4944..5007 fails (touches 5000..5007); the exact 16 sectors are good
		Check(ok, "bad place: a request next to it still comes (exact read after the failed window)");
		Check(ra.GetStats().window_failures == 1, "bad place: one failed window");
		const uint64_t direct_before = ra.GetStats().direct_reads;
		bool ok2 = Get(ra, d, 4960, 16) && Get(ra, d, 4976, 16) && Get(ra, d, 4992, 8);
		bool bad = Get(ra, d, 5000, 16, false);
		Check(ok2 && bad, "bad place: sectors before it come, the bad ones fail");
		Check(ra.GetStats().direct_reads >= direct_before + 4, "bad place: after a failure the next requests are read as asked (no second failed window)");
		bool later = Get(ra, d, 6000, 16) && Get(ra, d, 6016, 16);
		Check(later && ra.GetStats().window_reads >= 2, "bad place: windows come back beyond it");
		// 187: and below it. The check had no lower bound: every sector below the end of the failed window was read as asked (a seek
		// each: 0.12 MB/s instead of 1.22) until the disc changed. This one fails on build 186's header.
		const uint64_t windows_before = ra.GetStats().window_reads, direct_before2 = ra.GetStats().direct_reads;
		bool below = Get(ra, d, 2000, 16) && Get(ra, d, 2016, 16);
		Check(below && ra.GetStats().window_reads == windows_before + 1 && ra.GetStats().direct_reads == direct_before2,
			"bad place: windows come back below it too (one window for two blocks)");
		// 187: a window that would run into the known-bad place stops where it starts (4940..5003 would touch the bad 5000..5003)
		bool edge = Get(ra, d, 4940, 4);
		Check(edge && ra.GetStats().window_failures == 1, "bad place: a window just below it stops at its start (no second failed window)");
		// 187: a failure next to the known-bad place widens it: what lies inside is read as asked, not by another window that fails
		bool next = Get(ra, d, 5008, 16, false); // window 5008..5071 fails: 4944..5071 is known bad now
		bool inside = Get(ra, d, 4968, 16);      // read as asked (a window 4968..5007 would fail on 5000..5007)
		Check(next && inside && ra.GetStats().window_failures == 2, "bad place: a failure next to it widens it (no third failed window)");
		// 187: Reset (a disc change) forgets the bad place
		ra.Reset();
		d.bad_from = 0xFFFFFFFF;
		d.bad_to = 0;
		const uint64_t windows_reset = ra.GetStats().window_reads;
		bool reset = Get(ra, d, 4960, 16);
		Check(reset && ra.GetStats().window_reads == windows_reset + 1, "bad place: forgotten after Reset (a window again)");
	}
	// 8. small reads always reach the drive (the keep-alive read must wake it), big ones too
	{
		ReadAhead ra;
		Drive d;
		d.total = 100000;
		bool ok = Get(ra, d, 1000, 16);
		const uint64_t c0 = d.commands;
		ok = Get(ra, d, 1005, 1) && ok;
		ok = Get(ra, d, 1005, 1) && ok;
		ok = Get(ra, d, 1005, 3) && ok;
		Check(ok && d.commands == c0 + 3, "reads of 1 to 3 sectors go to the drive every time");
		const uint64_t c1 = d.commands;
		ok = Get(ra, d, 2000, 300);
		Check(ok && d.commands == c1 + 1 && d.starts.back() == 2000, "a read of 256 sectors or more is one plain command");
	}
	// 9. Reset: the windows are empty
	{
		ReadAhead ra;
		Drive d;
		d.total = 100000;
		Get(ra, d, 1000, 16);
		const uint64_t c = d.commands;
		Get(ra, d, 1016, 16);
		Check(d.commands == c, "a block inside the window comes from memory");
		ra.Reset();
		Get(ra, d, 1016, 16);
		Check(d.commands == c + 1, "after Reset the same block goes to the drive");
	}
	// 10. switched off
	{
		ReadAhead ra;
		ra.SetEnabled(false);
		Drive d;
		d.total = 100000;
		for (uint32_t s = 0; s < 160; s += 16)
			Get(ra, d, s, 16);
		Check(d.commands == 10 && d.sectors == 160, "switched off: every request is its own command, nothing extra");
	}
	std::printf("%s\n", s_failed == 0 ? "ALL PASSED" : "SOME FAILED");
	return s_failed == 0 ? 0 : 1;
}
