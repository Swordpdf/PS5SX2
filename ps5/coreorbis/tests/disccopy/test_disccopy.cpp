// PS5 port, PC test (vk-285-147, AI-assisted): orbis-shims/OrbisDiscCopy.h, the disc copy's read-ahead loop. A fake
// drive that reads only whole 2048-byte sectors (as the PS5's /dev/cd0), sometimes slowly, with bad sectors; checks the
// copy is byte for byte (bad sectors as zeros), in order, that reads run ahead of slow writes, and that it stops on too
// many bad sectors or a failed write.
//   ps5/coreorbis/tests/disccopy/test-disccopy.sh
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../orbis-shims/OrbisDiscCopy.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <set>
static int fails = 0;
static void Check(bool ok, const std::string& w) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", w.c_str()); fails += !ok; }
static uint8_t Byte(uint64_t i) { return static_cast<uint8_t>((i * 2654435761u) >> 11 ^ (i >> 13)); }
struct Drive
{
	uint64_t bytes = 0;
	std::set<uint64_t> bad{};  // sectors that never read
	std::set<uint64_t> flaky{}; // sectors that fail once, then read
	int delay_us = 0;        // per read
	std::atomic<int> reads{0};
	ssize_t Read(uint64_t off, void* buf, size_t len)
	{
		reads++;
		if (off % 2048 || len % 2048 || off + len > bytes) { errno = EINVAL; return -1; }
		if (delay_us) std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
		for (uint64_t s = off / 2048; s < (off + len) / 2048; s++)
		{
			if (bad.count(s)) { errno = EIO; return -1; }
			if (flaky.count(s)) { flaky.erase(s); errno = EIO; return -1; }
		}
		uint8_t* p = static_cast<uint8_t*>(buf);
		for (size_t i = 0; i < len; i++) p[i] = Byte(off + i);
		return static_cast<ssize_t>(len);
	}
};
int main()
{
	using namespace orbis_disc;
	{
		Drive d; d.bytes = (37u << 20) + 2048 * 5; // not a whole number of chunks
		std::vector<uint8_t> out;
		std::vector<uint64_t> prog;
		const auto r = Copy(d.bytes, 4 << 20, 4, 3, 4096, [&](uint64_t o, void* b, size_t l) { return d.Read(o, b, l); },
			[&](const void* b, size_t l) { out.insert(out.end(), (const uint8_t*)b, (const uint8_t*)b + l); return true; },
			[&](uint64_t done, uint64_t) { prog.push_back(done); }, [&](uint64_t) {});
		bool same = out.size() == d.bytes;
		for (uint64_t i = 0; same && i < out.size(); i++) same = out[i] == Byte(i);
		Check(!r.failed && r.done == d.bytes && r.bad_sectors == 0, "a clean disc copies in full");
		Check(same, "byte for byte, in order (a last chunk shorter than 4 MiB too)");
		Check(!prog.empty() && prog.back() == d.bytes && std::is_sorted(prog.begin(), prog.end()), "progress goes up to the whole size");
	}
	{
		Drive d; d.bytes = 16u << 20;
		d.bad = {5, 6, 3000};
		d.flaky = {4000};
		std::vector<uint8_t> out;
		std::vector<uint64_t> said;
		const auto r = Copy(d.bytes, 1 << 20, 3, 3, 4096, [&](uint64_t o, void* b, size_t l) { return d.Read(o, b, l); },
			[&](const void* b, size_t l) { out.insert(out.end(), (const uint8_t*)b, (const uint8_t*)b + l); return true; },
			[](uint64_t, uint64_t) {}, [&](uint64_t s) { said.push_back(s); });
		bool ok = out.size() == d.bytes;
		for (uint64_t i = 0; ok && i < out.size(); i++)
		{
			const uint64_t s = i / 2048;
			ok = out[i] == ((s == 5 || s == 6 || s == 3000) ? 0 : Byte(i));
		}
		Check(!r.failed && r.bad_sectors == 3, "three bad sectors: copied, three counted");
		Check(ok, "bad sectors are zeros, the rest (a sector that failed once too) is the disc's");
		Check(said == std::vector<uint64_t>({5, 6, 3000}), "each bad sector is reported once, by number");
	}
	{
		Drive d; d.bytes = 8u << 20;
		for (uint64_t s = 100; s < 200; s++) d.bad.insert(s);
		size_t written = 0;
		const auto r = Copy(d.bytes, 1 << 20, 4, 1, 50, [&](uint64_t o, void* b, size_t l) { return d.Read(o, b, l); },
			[&](const void*, size_t l) { written += l; return true; }, [](uint64_t, uint64_t) {}, [](uint64_t) {});
		Check(r.failed && r.why.find("unreadable") != std::string::npos, "too many bad sectors stops the copy");
		Check(written < d.bytes, "(and it didn't write the whole size)");
	}
	{
		Drive d; d.bytes = 32u << 20;
		int n = 0;
		const auto r = Copy(d.bytes, 1 << 20, 4, 3, 4096, [&](uint64_t o, void* b, size_t l) { return d.Read(o, b, l); },
			[&](const void*, size_t) { return ++n < 5; }, [](uint64_t, uint64_t) {}, [](uint64_t) {});
		Check(r.failed && r.done == (4u << 20) && r.why.find("write failed") != std::string::npos, "a failed write stops it (4 MiB written)");
	}
	{
		// reads 20 ms, writes 20 ms, 16 chunks: in turn 640 ms; overlapped about 340.
		Drive d; d.bytes = 16u << 20;
		d.delay_us = 20000;
		auto t0 = std::chrono::steady_clock::now();
		const auto r = Copy(d.bytes, 1 << 20, 4, 3, 4096, [&](uint64_t o, void* b, size_t l) { return d.Read(o, b, l); },
			[&](const void*, size_t) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); return true; },
			[](uint64_t, uint64_t) {}, [](uint64_t) {});
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		std::printf("      overlapped: %.0f ms (in turn it would be 640)\n", ms);
		Check(!r.failed && ms < 520, "reads run while writes do (well under the 640 ms of one after the other)");
	}
	std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
