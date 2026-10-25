// PS5 port, PC test (vk-285-147, AI-assisted): orbis-shims/OrbisDiscCopy.h, the disc copy's read-ahead loop. A fake
// drive that reads only whole 2048-byte sectors (as the PS5's /dev/cd0), sometimes slowly, with bad sectors; checks the
// copy is byte for byte (bad sectors as zeros), in order, that reads run ahead of slow writes, and that it stops on too
// many bad sectors or a failed write.
//   ps5/coreorbis/tests/disccopy/test-disccopy.sh
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../orbis-shims/OrbisDiscCopy.h"
#include "../../orbis-shims/OrbisDiscResume.h"
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
	// 2.11: a copy cut short by the app's restart is taken up again where its state says.
	{
	Drive d; d.bytes = (40u << 20) + 2048 * 7;
	d.bad = {3, 20000}; // one before the cut, one after
	std::vector<uint8_t> whole;
	const auto full = Copy(d.bytes, 4 << 20, 4, 1, 4096, [&](uint64_t o, void* b, size_t l) { return d.Read(o, b, l); },
	[&](const void* b, size_t l) { whole.insert(whole.end(), (const uint8_t*)b, (const uint8_t*)b + l); return true; },
	[](uint64_t, uint64_t) {}, [](uint64_t) {});
	Check(!full.failed && full.bad_sectors == 2, "(reference copy with two bad sectors)");
	// cut: the first 16 MiB were written (sector 3 as zeros: one bad counted), then the process died
	const uint64_t cut = 16u << 20;
	std::vector<uint8_t> out(whole.begin(), whole.begin() + cut);
	std::vector<uint64_t> prog;
	uint64_t bad_seen = 0;
	const auto rest = CopyFrom(cut, 1, d.bytes, 4 << 20, 4, 1, 4096, [&](uint64_t o, void* b, size_t l) { return d.Read(o, b, l); },
	[&](const void* b, size_t l) { out.insert(out.end(), (const uint8_t*)b, (const uint8_t*)b + l); return true; },
	[&](uint64_t done, uint64_t bad) { prog.push_back(done); bad_seen = bad; }, [](uint64_t) {});
	Check(!rest.failed && rest.done == d.bytes && rest.bad_sectors == 2, "a copy taken up at 16 MiB finishes, counting the earlier bad sector");
	Check(out == whole, "and the file is byte for byte the whole copy");
	Check(!prog.empty() && prog.front() > cut && prog.back() == d.bytes && bad_seen == 2, "progress counts from the cut up to the whole size");
	const auto none = CopyFrom(d.bytes, 0, d.bytes, 4 << 20, 4, 1, 4096, [&](uint64_t o, void* b, size_t l) { return d.Read(o, b, l); },
	[](const void*, size_t) { return true; }, [](uint64_t, uint64_t) {}, [](uint64_t) {});
	Check(!none.failed && none.done == d.bytes, "taken up at the end there is nothing left to do");
	const auto odd = CopyFrom(cut + 100, 0, d.bytes, 4 << 20, 4, 1, 4096, [&](uint64_t o, void* b, size_t l) { return d.Read(o, b, l); },
	[](const void*, size_t) { return true; }, [](uint64_t, uint64_t) {}, [](uint64_t) {});
	Check(!odd.failed && odd.done == d.bytes, "an offset that is not a whole sector is taken down to one");
	}
	// 2.16: the disc taken out with the eject button in the middle of the copy (every read fails from then on): the copy stops at once as
	// "the disc was taken out", without reading the rest sector by sector, and what was written stays a whole number of chunks of real data.
	{
	Drive d; d.bytes = 64u << 20;
	std::atomic<bool> out_now{false};
	std::atomic<int> after{0};
	std::vector<uint8_t> out;
	int asked = 0;
	const auto r = CopyFromStop(0, 0, d.bytes, 4 << 20, 4, 3, 4096,
	[&](uint64_t o, void* b, size_t l) -> ssize_t {
		if (o >= (20u << 20)) out_now = true; // the tray opens when the copy reaches 20 MiB
		if (out_now) { after++; errno = ENXIO; return -1; }
		return d.Read(o, b, l);
	},
	[&](const void* b, size_t l) { out.insert(out.end(), (const uint8_t*)b, (const uint8_t*)b + l); return true; },
	[](uint64_t, uint64_t) {}, [](uint64_t) {}, [&] { asked++; return out_now.load(); });
	bool same = true;
	for (uint64_t i = 0; same && i < out.size(); i++) same = out[i] == Byte(i);
	Check(r.failed && r.disc_gone && r.why == "the disc was taken out", "an eject mid-copy stops it as \"the disc was taken out\"");
	Check(r.done == (20u << 20) && out.size() == r.done && same && r.bad_sectors == 0, "what was written before is whole and nothing was zero-filled");
	Check(asked == 1 && after <= 3, "asked once, after the chunk's tries, not sector by sector (" + std::to_string(after.load()) + " failed reads)");
	// a bad sector with the disc still in: gone() says no and the old sector-by-sector path runs
	Drive d2; d2.bytes = 8u << 20; d2.bad = {7};
	int asked2 = 0;
	const auto r2 = CopyFromStop(0, 0, d2.bytes, 4 << 20, 4, 1, 4096, [&](uint64_t o, void* b, size_t l) { return d2.Read(o, b, l); },
	[](const void*, size_t) { return true; }, [](uint64_t, uint64_t) {}, [](uint64_t) {}, [&] { asked2++; return false; });
	Check(!r2.failed && !r2.disc_gone && r2.bad_sectors == 1 && asked2 == 1, "a bad sector with the disc in: asked, still in, zeros as before");
	}
	{
	ResumeState st; st.serial = "SLUS-20312"; st.total = 4000ull << 20; st.done = 1500ull << 20; st.bad = 2;
	ResumeState back;
	Check(ParseResumeState(FormatResumeState(st), back) && back.serial == st.serial && back.total == st.total && back.done == st.done && back.bad == 2,
	"the state file reads back what was written");
	Check(!ParseResumeState("", back) && !ParseResumeState("SLUS-20312 4000", back) && !ParseResumeState("garbage", back), "a damaged state file is refused");
	const uint64_t at = ResumeOffset(st, "SLUS-20312", st.total, st.done + 5);
	Check(at == (1500ull << 20) - (16ull << 20) && at % 2048 == 0, "carries on 16 MiB before where the state says");
	Check(ResumeOffset(st, "SLES-50001", st.total, st.done) == 0, "another disc: start over");
	Check(ResumeOffset(st, "SLUS-20312", st.total + 2048, st.done) == 0, "the same serial at another size: start over");
	Check(ResumeOffset(st, "SLUS-20312", st.total, st.done - 1) == 0, "a .part shorter than the state says (a power cut): start over");
	ResumeState early = st; early.done = 8ull << 20;
	Check(ResumeOffset(early, "SLUS-20312", st.total, early.done) == 0, "hardly started: not worth carrying on");
	ResumeState over = st; over.done = st.total + 1;
	Check(ResumeOffset(over, "SLUS-20312", st.total, over.done) == 0, "a state past the disc's size is no state");
	}
	std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
