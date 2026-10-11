// PS5 port (2026-10-05, AI-assisted): a texture pack's replacements in one file.
//
// On the PS5 each new file costs the app tens of milliseconds to hundreds of milliseconds of file-system work: Ratchet &
// Clank 2's pack (630 MB, some 15,000 DDS files) wrote its first 3,385 files in 11 s and then 5 to 10 a second, so its
// unpack took most of an hour. One big file writes at the disk's speed (a test payload: 256 MB in 1 s). So the texture
// pack manager (ps5/frontend/fe_texpacks.cpp) unpacks a pack's <serial>/replacements/ folder into
// <serial>/replacements.pak, and GSTextureReplacements.cpp reads the replacements out of it. Loose files in a
// replacements/ folder still work (packs copied over by hand) and win over the same name in the pack.
//
// The file, little-endian:
//   header, 64 bytes: "PS5SX2TP", u32 version (1), u32 entry count, u64 index offset, u64 index size, zeros;
//   the entries' bytes, back to back, from offset 64;
//   the index: per entry u64 offset, u64 size, u16 name length, the name (UTF-8, its path under replacements/ with '/').
// The header is written last, so a pack cut short has no magic and isn't read. Header only: the unpacker, the GS code
// and the PC tests (ps5/frontend/host) share it. Needs proper testing.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

namespace OrbisTexturePak
{
constexpr char kMagic[8] = {'P', 'S', '5', 'S', 'X', '2', 'T', 'P'};
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderSize = 64;
constexpr const char* kFileName = "replacements.pak";
constexpr uint64_t kMaxIndexBytes = 64ull << 20;
constexpr size_t kMaxNameBytes = 1024;

struct Entry
{
	uint64_t offset = 0;
	uint64_t size = 0;
	std::string name;
};

inline void PutU16(std::vector<uint8_t>& out, uint16_t v)
{
	for (int i = 0; i < 2; i++)
		out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void PutU32(std::vector<uint8_t>& out, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void PutU64(std::vector<uint8_t>& out, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline uint64_t GetLE(const uint8_t* p, int bytes)
{
	uint64_t v = 0;
	for (int i = bytes - 1; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

// A name the index may hold: a relative path without "." or ".." parts, empty parts, backslashes or control characters.
inline bool NameIsSane(const std::string& name)
{
	if (name.empty() || name.size() > kMaxNameBytes || name[0] == '/' || name.back() == '/')
		return false;
	size_t at = 0;
	while (at <= name.size())
	{
		size_t end = name.find('/', at);
		if (end == std::string::npos)
			end = name.size();
		const size_t len = end - at;
		if (len == 0 || (len == 1 && name[at] == '.') || (len == 2 && name[at] == '.' && name[at + 1] == '.'))
			return false;
		at = end + 1;
	}
	for (const char ch : name)
	{
		const unsigned char c = static_cast<unsigned char>(ch);
		if (c < 0x20 || c == 0x7f || c == '\\')
			return false;
	}
	return true;
}

inline std::vector<uint8_t> MakeHeader(uint32_t count, uint64_t index_offset, uint64_t index_size)
{
	std::vector<uint8_t> h(kMagic, kMagic + sizeof(kMagic));
	PutU32(h, kVersion);
	PutU32(h, count);
	PutU64(h, index_offset);
	PutU64(h, index_size);
	h.resize(kHeaderSize, 0);
	return h;
}

inline void AppendIndexEntry(std::vector<uint8_t>& index, const Entry& e)
{
	PutU64(index, e.offset);
	PutU64(index, e.size);
	PutU16(index, static_cast<uint16_t>(e.name.size()));
	index.insert(index.end(), e.name.begin(), e.name.end());
}

// The header and the index, checked against the file's size: every entry inside the data, every name sane.
inline bool ParseHeader(const uint8_t* h, uint64_t file_size, uint32_t& count, uint64_t& index_offset, uint64_t& index_size,
	std::string& error)
{
	if (file_size < kHeaderSize || std::memcmp(h, kMagic, sizeof(kMagic)) != 0)
	{
		error = "not a PS5SX2 texture pack (or not finished)";
		return false;
	}
	if (GetLE(h + 8, 4) != kVersion)
	{
		error = "a newer pack version (" + std::to_string(GetLE(h + 8, 4)) + ")";
		return false;
	}
	count = static_cast<uint32_t>(GetLE(h + 12, 4));
	index_offset = GetLE(h + 16, 8);
	index_size = GetLE(h + 24, 8);
	if (index_offset < kHeaderSize || index_size > kMaxIndexBytes || index_offset > file_size || index_size > file_size - index_offset)
	{
		error = "its index is outside the file";
		return false;
	}
	return true;
}

inline bool ParseIndex(const uint8_t* p, size_t n, uint32_t count, uint64_t index_offset, std::vector<Entry>& out, std::string& error)
{
	out.clear();
	out.reserve(count);
	size_t at = 0;
	for (uint32_t i = 0; i < count; i++)
	{
		if (n - at < 18)
		{
			error = "its index is cut short";
			return false;
		}
		Entry e;
		e.offset = GetLE(p + at, 8);
		e.size = GetLE(p + at + 8, 8);
		const size_t len = static_cast<size_t>(GetLE(p + at + 16, 2));
		at += 18;
		if (n - at < len)
		{
			error = "its index is cut short";
			return false;
		}
		e.name.assign(reinterpret_cast<const char*>(p + at), len);
		at += len;
		if (!NameIsSane(e.name))
		{
			error = "a bad name in its index";
			return false;
		}
		if (e.offset < kHeaderSize || e.offset > index_offset || e.size > index_offset - e.offset)
		{
			error = "an entry outside the data (" + e.name + ")";
			return false;
		}
		out.push_back(std::move(e));
	}
	if (at != n)
	{
		error = "its index has bytes left over";
		return false;
	}
	return true;
}

// pread until done (EINTR retried). False on an error or the end of the file.
inline bool PreadAll(int fd, void* dst, size_t size, uint64_t offset)
{
	uint8_t* p = static_cast<uint8_t*>(dst);
	while (size > 0)
	{
		const ssize_t r = pread(fd, p, size, static_cast<off_t>(offset));
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		p += r;
		size -= static_cast<size_t>(r);
		offset += static_cast<uint64_t>(r);
	}
	return true;
}

// The index of the open pack `fd`.
inline bool ReadIndex(int fd, std::vector<Entry>& out, std::string& error)
{
	const off_t end = lseek(fd, 0, SEEK_END);
	if (end < 0)
	{
		error = "can't find its size";
		return false;
	}
	uint8_t h[kHeaderSize];
	if (static_cast<uint64_t>(end) < kHeaderSize || !PreadAll(fd, h, kHeaderSize, 0))
	{
		error = "too short";
		return false;
	}
	uint32_t count = 0;
	uint64_t index_offset = 0, index_size = 0;
	if (!ParseHeader(h, static_cast<uint64_t>(end), count, index_offset, index_size, error))
		return false;
	std::vector<uint8_t> index(static_cast<size_t>(index_size));
	if (index_size > 0 && !PreadAll(fd, index.data(), index.size(), index_offset))
	{
		error = "can't read its index";
		return false;
	}
	return ParseIndex(index.data(), index.size(), count, index_offset, out, error);
}
} // namespace OrbisTexturePak
