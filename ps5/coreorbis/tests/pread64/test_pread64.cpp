// PS5SX2 (vk-285-179, AI-assisted): PC test of OrbisDiscPread64.h - the DiscSpeed8 64 KiB pread chunking used by the
// disc copy (ProsperoDiscSpeed.cpp) and the play read path (OrbisIOCtlSrc.cpp Pread2048).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisDiscPread64.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using orbis_disc::Pread64;
using orbis_disc::kPread64Bytes;

static int s_failed = 0;
static void Check(bool ok, const char* what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		s_failed++;
}

// A fake drive: byte b at absolute offset o is a function of o, so any copy can be checked. It records every pread it is
// asked for (size and offset), may cap a read at a short count once past a given offset, and may fail (return -1) once.
struct FakeDrive
{
	uint64_t size = 0;
	std::vector<std::pair<uint64_t, size_t>> calls; // (offset, len) in the order asked
	uint64_t short_at = ~0ull;                      // a read that starts at/after this returns only `short_len` bytes
	size_t short_len = 0;
	uint64_t fail_at = ~0ull;                        // a read that starts at/after this returns -1

	static uint8_t ByteAt(uint64_t o) { return static_cast<uint8_t>((o * 2654435761ull) >> 24); }

	ssize_t pread(void* buf, size_t n, uint64_t off)
	{
		calls.push_back({off, n});
		if (off >= fail_at)
			return -1;
		size_t give = n;
		if (off + n > size)
			give = off < size ? static_cast<size_t>(size - off) : 0;
		if (off >= short_at && give > short_len)
			give = short_len;
		uint8_t* p = static_cast<uint8_t*>(buf);
		for (size_t i = 0; i < give; i++)
			p[i] = ByteAt(off + i);
		return static_cast<ssize_t>(give);
	}
};

static bool BytesOk(const uint8_t* p, uint64_t off, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != FakeDrive::ByteAt(off + i))
			return false;
	return true;
}

int main()
{
	const uint64_t kDisc = 16ull << 20;

	// 1. A big read is cut into 64 KiB pieces, in order, with the right bytes.
	{
		FakeDrive d; d.size = kDisc;
		const uint64_t off = 1u << 20; const size_t len = 1u << 20; // 1 MiB
		std::vector<uint8_t> buf(len, 0);
		const ssize_t got = Pread64(buf.data(), len, off, false, [&](void* b, size_t n, uint64_t o) { return d.pread(b, n, o); });
		Check(got == (ssize_t)len, "big read: all bytes returned");
		Check(BytesOk(buf.data(), off, len), "big read: right bytes");
		bool pieces = d.calls.size() == len / kPread64Bytes;
		for (size_t i = 0; i < d.calls.size(); i++)
			pieces = pieces && d.calls[i].second == kPread64Bytes && d.calls[i].first == off + i * kPread64Bytes;
		Check(pieces, "big read: 64 KiB pieces, ascending offsets");
	}

	// 2. one_shot: one pread of the whole length.
	{
		FakeDrive d; d.size = kDisc;
		const size_t len = 1u << 20;
		std::vector<uint8_t> buf(len, 0);
		const ssize_t got = Pread64(buf.data(), len, 0, true, [&](void* b, size_t n, uint64_t o) { return d.pread(b, n, o); });
		Check(got == (ssize_t)len && d.calls.size() == 1 && d.calls[0].second == len, "one_shot (disc_nopread64): a single big pread");
		Check(BytesOk(buf.data(), 0, len), "one_shot: right bytes");
	}

	// 3. A read of 64 KiB or less is one pread either way.
	{
		FakeDrive d; d.size = kDisc;
		std::vector<uint8_t> buf(kPread64Bytes, 0);
		const ssize_t got = Pread64(buf.data(), kPread64Bytes, 4096, false, [&](void* b, size_t n, uint64_t o) { return d.pread(b, n, o); });
		Check(got == (ssize_t)kPread64Bytes && d.calls.size() == 1, "<= 64 KiB: one pread");
	}

	// 4. A non-multiple length: the last piece is the remainder.
	{
		FakeDrive d; d.size = kDisc;
		const size_t len = (uint64_t)(kPread64Bytes * 2 + 2048); // two full pieces + 2 KiB
		std::vector<uint8_t> buf(len, 0);
		const ssize_t got = Pread64(buf.data(), len, 0, false, [&](void* b, size_t n, uint64_t o) { return d.pread(b, n, o); });
		bool ok = got == (ssize_t)len && d.calls.size() == 3 && d.calls[2].second == 2048;
		Check(ok, "a remainder piece smaller than 64 KiB");
		Check(BytesOk(buf.data(), 0, len), "remainder: right bytes");
	}

	// 5. A short read (end of medium) stops the loop and returns the short total, like pread.
	{
		FakeDrive d; d.size = kDisc;
		const uint64_t off = 0; const size_t len = 1u << 20;
		d.short_at = kPread64Bytes * 2; d.short_len = 4096; // the third piece comes back short
		std::vector<uint8_t> buf(len, 0);
		const ssize_t got = Pread64(buf.data(), len, off, false, [&](void* b, size_t n, uint64_t o) { return d.pread(b, n, o); });
		Check(got == (ssize_t)(kPread64Bytes * 2 + 4096), "a short piece stops the read at the short total");
		Check(BytesOk(buf.data(), off, (size_t)got), "short read: the bytes that came are right");
	}

	// 6. The first piece failing returns the negative error; a later piece failing returns what was read so far.
	{
		FakeDrive a; a.size = kDisc; a.fail_at = 0;
		std::vector<uint8_t> buf(1u << 20, 0);
		const ssize_t g1 = Pread64(buf.data(), buf.size(), 0, false, [&](void* b, size_t n, uint64_t o) { return a.pread(b, n, o); });
		Check(g1 < 0, "first piece fails: a negative error is returned");

		FakeDrive b; b.size = kDisc; b.fail_at = kPread64Bytes * 2;
		const ssize_t g2 = Pread64(buf.data(), buf.size(), 0, false, [&](void* bb, size_t n, uint64_t o) { return b.pread(bb, n, o); });
		Check(g2 == (ssize_t)(kPread64Bytes * 2), "a later piece fails: the bytes read so far are returned");
	}

	std::printf(s_failed ? "FAILED: %d\n" : "ALL PASSED: %d failed\n", s_failed);
	return s_failed ? 1 : 0;
}
