// PS5SX2 (2.11, AI-assisted): read-ahead windows in front of a disc drive's sector reads.
//
// Why (tester logs of the disc drive test, 2026-10-16, three consoles on firmware 11.20, 13.20 and 13.60, two drive models): a DVD drive reads a
// disc in one go at 2.0x (2.78 MB/s) whatever the read size, but every seek costs it 150-250 ms. PCSX2's disc thread asks for 16 sectors (32 KiB)
// at a time. When a game reads two places of the disc in turn, or loads many small pieces, that is one seek for every 32 KiB: 0.12 MB/s measured.
// The same visits with 128 KiB per seek gave 0.44 MB/s and with 512 KiB 1.22 MB/s (a USB drive: 0.18 / 0.67 / 2.04). The 5S2CD fork reads 128 KiB
// for every miss for this reason (one window); this keeps a few windows, one per place that is being read, and lets a window that is being read
// on to grow (64 -> 128 -> 256 sectors = 128 -> 256 -> 512 KiB), so two or three streams each get their own run.
//
// Not touched: single-sector reads (the keep-alive thread reads one to keep the drive spinning: it has to reach the drive), reads of 256 sectors or
// more, and everything after a failed window read (the next 256 sectors are read exactly as asked, so a scratch costs one failed window, not two
// reads of every request).
//
// 187: "the next 256 sectors" are the failed window's sectors only. The check had no lower bound, so one failed window sent every sector below its
// end to plain 32 KiB reads (0.12 MB/s instead of 1.22) until the disc changed. Now the known-bad range is [m_bad_from, m_bad_until): a window
// stops where it begins (it would fail on it again), and a failure next to or over it widens it; a failure elsewhere replaces it.
//
// Used under IOCtlSrc's lock (OrbisIOCtlSrc.cpp); a plain header so the PC test (tests/readahead) can use it with a fake disc.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace orbis_disc
{
class ReadAhead
{
public:
	static constexpr uint32_t kSector = 2048;
	static constexpr uint32_t kMinWindow = 64;  // sectors: 128 KiB
	static constexpr uint32_t kMaxWindow = 256; // sectors: 512 KiB (the CDVD thread already reads up to 16 x 32 KiB ahead)
	static constexpr uint32_t kWindows = 4;     // places read at once
	static constexpr uint32_t kBypassBelow = 4; // sectors: a read this small goes to the drive as it is
	static constexpr uint32_t kSlack = 16;      // a read this close after a window's end carries that window on

	struct Stats
	{
		uint64_t calls = 0, hits = 0, window_reads = 0, direct_reads = 0, window_failures = 0, sectors_from_drive = 0;
	};

	void SetEnabled(bool on)
	{
		m_enabled = on;
		Reset();
	}
	bool Enabled() const { return m_enabled; }

	// The disc changed (or was reopened): nothing in the windows is its data.
	void Reset()
	{
		for (Window& w : m_w)
			w.count = 0;
		m_bad_from = 0; // 187
		m_bad_until = 0;
	}

	const Stats& GetStats() const { return m_stats; }

	// `count` sectors from `sector` into `out`, of a disc of `total` sectors (0 when not known). `read(sector, count, buffer)` reads from the drive
	// and returns whether all of it came.
	template <class ReadFn>
	bool Read(uint32_t sector, uint32_t count, uint8_t* out, uint32_t total, ReadFn&& read)
	{
		m_stats.calls++;
		if (count == 0)
			return true;
		if (!m_enabled || count < kBypassBelow || count >= kMaxWindow || (total && (sector >= total || count > total - sector)))
			return Direct(sector, count, out, read);
		while (count > 0)
		{
			Window* hit = Find(sector);
			if (!hit)
			{
				if (sector >= m_bad_from && sector < m_bad_until) // 187: inside the known-bad range only (see the top)
					return Direct(sector, count, out, read);
				hit = Fill(sector, count, total, read);
				if (!hit)
					return Direct(sector, count, out, read);
			}
			else
				m_stats.hits++;
			const uint32_t n = std::min(count, hit->start + hit->count - sector);
			std::memcpy(out, hit->buf.data() + static_cast<size_t>(sector - hit->start) * kSector, static_cast<size_t>(n) * kSector);
			hit->used = ++m_tick;
			sector += n;
			count -= n;
			out += static_cast<size_t>(n) * kSector;
		}
		return true;
	}

private:
	struct Window
	{
		uint32_t start = 0, count = 0; // count 0: empty
		uint64_t used = 0;
		std::vector<uint8_t> buf;
	};

	Window* Find(uint32_t sector)
	{
		for (Window& w : m_w)
			if (w.count && sector >= w.start && sector < w.start + w.count)
				return &w;
		return nullptr;
	}

	template <class ReadFn>
	bool Direct(uint32_t sector, uint32_t count, uint8_t* out, ReadFn& read)
	{
		m_stats.direct_reads++;
		m_stats.sectors_from_drive += count;
		return read(sector, count, out);
	}

	// A window at `sector`: the one that this read carries on (it grows), else the one used longest ago (kMinWindow). Null when the drive failed.
	template <class ReadFn>
	Window* Fill(uint32_t sector, uint32_t count, uint32_t total, ReadFn& read)
	{
		Window* slot = nullptr;
		uint32_t size = kMinWindow;
		for (Window& w : m_w)
			if (w.count && sector >= w.start + w.count && sector - (w.start + w.count) <= kSlack)
			{
				slot = &w;
				size = std::min(kMaxWindow, w.count * 2);
				break;
			}
		if (!slot)
		{
			slot = &m_w[0];
			for (Window& w : m_w)
				if (!w.count || w.used < slot->used)
					slot = &w;
		}
		if (total)
			size = std::min(size, total - sector);
		if (m_bad_until > m_bad_from && sector < m_bad_from) // 187: a window stops where the known-bad range begins
			size = std::min(size, m_bad_from - sector);
		if (size < count)
			return nullptr;
		if (slot->buf.size() < static_cast<size_t>(kMaxWindow) * kSector)
			slot->buf.resize(static_cast<size_t>(kMaxWindow) * kSector);
		slot->count = 0;
		if (!read(sector, size, slot->buf.data()))
		{
			m_stats.window_failures++;
			// 187: the failed window's sectors become the known-bad range; joined with the old one when they touch or overlap
			const uint32_t end = sector + size;
			if (m_bad_until > m_bad_from && sector <= m_bad_until && end >= m_bad_from)
			{
				m_bad_from = std::min(m_bad_from, sector);
				m_bad_until = std::max(m_bad_until, end);
			}
			else
			{
				m_bad_from = sector;
				m_bad_until = end;
			}
			return nullptr;
		}
		m_stats.window_reads++;
		m_stats.sectors_from_drive += size;
		slot->start = sector;
		slot->count = size;
		slot->used = ++m_tick;
		return slot;
	}

	Window m_w[kWindows];
	uint64_t m_tick = 0;
	uint32_t m_bad_from = 0, m_bad_until = 0; // 187: the known-bad range (empty when equal); m_bad_until alone before
	bool m_enabled = true;
	Stats m_stats;
};
} // namespace orbis_disc
