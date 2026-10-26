// PS5SX2 (vk-285-179, AI-assisted): read a span off the PS5's disc in 64 KiB preads.
//
// DiscSpeed8 (the owner's method) found the PS5's cd(4) driver serves several small preads far faster than one big one:
// the same disc measured 15-24 MB/s with 64 KiB preads and only about 5 MB/s with 1 MiB ones. So the disc copy and the
// play read path cut a pread on /dev/cd0 larger than 64 KiB into 64 KiB preads. The copy's ring of 4 MiB chunks (its queue
// depth) and the play path's read-ahead windows are untouched - each is just filled with several 64 KiB reads. The switch
// file disc_nopread64 restores the single big pread (one_shot = true).
//
// A plain header so the PC test (tests/pread64) can drive it with a fake drive.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace orbis_disc
{
// 64 KiB, the proven fast pread size, both in bytes and in 2048-byte sectors.
constexpr std::size_t kPread64Bytes = 64u << 10;
constexpr std::uint32_t kPread64Sectors = kPread64Bytes / 2048; // 32

// Reads `len` bytes at `off` through `preadfn(buf, n, offset) -> ssize_t` (pread's contract), in pieces of at most 64 KiB
// (one piece when `one_shot`). Returns the total bytes read (a short total means a short read / end of medium, as pread
// does), or the first piece's negative error when nothing was read at all. 64 KiB is a multiple of 2048, so every piece
// keeps whatever sector alignment `off` and `len` had.
template <class PreadFn>
ssize_t Pread64(void* buf, std::size_t len, std::uint64_t off, bool one_shot, PreadFn&& preadfn)
{
	if (one_shot || len <= kPread64Bytes)
		return preadfn(buf, len, off);
	std::uint8_t* p = static_cast<std::uint8_t*>(buf);
	std::size_t done = 0;
	while (done < len)
	{
		const std::size_t n = std::min<std::size_t>(kPread64Bytes, len - done);
		const ssize_t g = preadfn(p + done, n, off + done);
		if (g < 0)
			return done ? static_cast<ssize_t>(done) : g;
		done += static_cast<std::size_t>(g);
		if (static_cast<std::size_t>(g) < n)
			break; // a short read: stop, like pread
	}
	return static_cast<ssize_t>(done);
}
} // namespace orbis_disc
