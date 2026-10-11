// PS5SX2 (vk-285-147, AI-assisted): the disc copy's loop, kept apart from the PS5 calls so a PC test can run it
// (tests/disccopy).
//
// swordpdf: "for next round we should try 8x dump speed". A DVD drive at 8x gives about 11 MB/s (1x is 1.385 MB/s), at
// the disc's outer edge; a drive spinning at a constant rate (CAV) reads the inner edge at well under half that, so a
// whole disc averages less. vk-285-146 read 1 MiB, then wrote it, then read the next: the drive sat idle while the file
// was written and the file system sat idle while the drive read. Here one thread reads the disc ahead into a ring of
// buffers while the caller's thread writes them out in order, so the slower of the two sets the pace, not their sum.
// Bigger reads too (4 MiB by default). An unreadable range is read again sector by sector, and what stays unreadable is
// written as zeros and counted, as before. Needs proper testing on a console.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace orbis_disc
{
	// A DVD's 1x: 1,385,000 bytes per second.
	constexpr double kDvd1x = 1385000.0;

	struct CopyResult
	{
		uint64_t done = 0;        // bytes written
		uint64_t bad_sectors = 0; // written as zeros
		bool failed = false;
		std::string why;          // when failed
	};

	// Copies `bytes` (a multiple of 2048, as a disc is) from the start of the source.
	//   read(offset, buf, len) -> ssize_t, like pread (offset and len are whole 2048-byte sectors)
	//   write(buf, len) -> bool
	//   progress(done_bytes, bad_sectors), after each chunk is written
	//   bad(sector), for each sector that couldn't be read (the caller logs the first few)
	// `chunk`: bytes per read (a multiple of 2048); `depth`: how many chunks may be read ahead of the writing.
	template <class Read, class Write, class Progress, class Bad>
	CopyResult Copy(uint64_t bytes, size_t chunk, int depth, int tries, uint64_t max_bad, Read read, Write write,
		Progress progress, Bad bad)
	{
		CopyResult r;
		chunk = std::max<size_t>(2048, chunk / 2048 * 2048);
		depth = std::max(2, depth);
		tries = std::max(1, tries);
		struct Slot
		{
			std::vector<uint8_t> buf;
			size_t len = 0;
			bool full = false;
		};
		std::vector<Slot> slots(static_cast<size_t>(depth));
		for (Slot& s : slots)
			s.buf.resize(chunk);
		std::mutex m;
		std::condition_variable cv;
		bool stop = false, read_failed = false;
		std::string read_why;
		std::atomic<uint64_t> bad_total{0};

		std::thread reader([&] {
			uint64_t off = 0;
			for (size_t i = 0; off < bytes; i++)
			{
				Slot& s = slots[i % slots.size()];
				{
					std::unique_lock<std::mutex> lk(m);
					cv.wait(lk, [&] { return stop || !s.full; });
					if (stop)
						return;
				}
				const size_t want = static_cast<size_t>(std::min<uint64_t>(chunk, bytes - off));
				ssize_t got = -1;
				for (int t = 0; t < tries && got != static_cast<ssize_t>(want); t++)
					got = read(off, s.buf.data(), want);
				if (got != static_cast<ssize_t>(want))
				{
					for (size_t o = 0; o < want; o += 2048)
					{
						const size_t len = std::min<size_t>(2048, want - o);
						ssize_t g = -1;
						for (int t = 0; t < tries && g != static_cast<ssize_t>(len); t++)
							g = read(off + o, s.buf.data() + o, len);
						if (g != static_cast<ssize_t>(len))
						{
							std::memset(s.buf.data() + o, 0, len);
							bad((off + o) / 2048);
							bad_total.fetch_add(1);
						}
					}
					if (bad_total.load() > max_bad)
					{
						std::lock_guard<std::mutex> lk(m);
						read_failed = true;
						read_why = "more than " + std::to_string(max_bad) + " unreadable sectors";
						stop = true;
						cv.notify_all();
						return;
					}
				}
				{
					std::lock_guard<std::mutex> lk(m);
					s.len = want;
					s.full = true;
				}
				cv.notify_all();
				off += want;
			}
		});

		for (size_t i = 0; r.done < bytes; i++)
		{
			Slot& s = slots[i % slots.size()];
			{
				std::unique_lock<std::mutex> lk(m);
				cv.wait(lk, [&] { return s.full || read_failed; });
				if (!s.full) // the reader gave up
					break;
			}
			if (!write(s.buf.data(), s.len))
			{
				std::lock_guard<std::mutex> lk(m);
				r.failed = true;
				r.why = "write failed at " + std::to_string(r.done >> 20) + " MB";
				stop = true;
				cv.notify_all();
				break;
			}
			r.done += s.len;
			{
				std::lock_guard<std::mutex> lk(m);
				s.full = false;
			}
			cv.notify_all();
			progress(r.done, bad_total.load());
		}
		reader.join();
		r.bad_sectors = bad_total.load();
		if (read_failed && !r.failed)
		{
			r.failed = true;
			r.why = read_why;
		}
		return r;
	}
} // namespace orbis_disc
