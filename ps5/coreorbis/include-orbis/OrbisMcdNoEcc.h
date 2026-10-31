// PS5 port (b185, AI-assisted; the owner: make .mc2 memory cards - MemCard PRO2 / SD2PSX, no ECC - safe on PS5): what PCSX2's file card
// (SIO/Memcard/MemoryCardFile.cpp) needs for its .mc2 and .bin cards, kept apart so a PC test can run it (ps5/coreorbis/tests/mcdnoecc).
//
// A .mc2 / .bin card is the card's pages without their ECC bytes (512 per page). PCSX2 works on a RAW copy beside it, "<card>x" (528 per
// page: the page, 3 ECC bytes for each 128-byte quarter, 4 zero bytes), made when the card opens and turned back into the card when it closes.
// On the PS5 the close often never comes (the app closed from the home screen kills it), so the session's saves stayed in the .mc2x and the
// next start overwrote it from the old card. Now the card is written back while the game runs (MemoryCardFile.cpp), and a leftover .mc2x
// newer than the card is put back into it at the next open. Every write of the card is durable: a .tmp beside it, fsync'd, renamed over it,
// the folder fsync'd - a power cut leaves the old card or the new one, never half of one.
//
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace mcd_noecc
{
constexpr size_t kPage = 512;    // a page in the .mc2
constexpr size_t kRawPage = 528; // the same page in the .mc2x: + 12 ECC bytes + 4 zero bytes

// `name` ends with `ext` (".mc2"), any case ("Card.MC2" too).
inline bool EndsWithNoCase(std::string_view name, std::string_view ext)
{
	if (name.size() < ext.size())
		return false;
	for (size_t i = 0; i < ext.size(); i++)
		if (std::tolower(static_cast<unsigned char>(name[name.size() - ext.size() + i])) != std::tolower(static_cast<unsigned char>(ext[i])))
			return false;
	return true;
}

// A card without ECC bytes: ".mc2" or ".bin", any case.
inline bool IsNoEccName(std::string_view name)
{
	return EndsWithNoCase(name, ".mc2") || EndsWithNoCase(name, ".bin");
}

// A file PCSX2 offers as a card: .ps2, .mcr, .mcd, .bin or .mc2, any case.
inline bool IsCardFileName(std::string_view name)
{
	for (const char* ext : {".ps2", ".mcr", ".mcd", ".bin", ".mc2"})
		if (EndsWithNoCase(name, ext))
			return true;
	return false;
}

// ECC code ported from mymc (https://sourceforge.net/p/mymc-opl/code/ci/master/tree/ps2mc_ecc.py, public domain): the 3 ECC bytes of 128 bytes.
inline uint32_t CalculateECC(const uint8_t* buf)
{
	static constexpr uint8_t parity_table[256] = {0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1,
		0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0,
		1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1,
		0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0,
		1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1,
		0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0,
		1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0,
		1, 0, 1, 1, 0};

	static constexpr uint8_t column_parity_mask[256] = {0, 7, 22, 17, 37, 34, 51, 52, 52, 51, 34, 37, 17, 22,
		7, 0, 67, 68, 85, 82, 102, 97, 112, 119, 119, 112, 97, 102, 82, 85, 68, 67, 82, 85, 68, 67, 119, 112,
		97, 102, 102, 97, 112, 119, 67, 68, 85, 82, 17, 22, 7, 0, 52, 51, 34, 37, 37, 34, 51, 52, 0, 7, 22, 17,
		97, 102, 119, 112, 68, 67, 82, 85, 85, 82, 67, 68, 112, 119, 102, 97, 34, 37, 52, 51, 7, 0, 17, 22,
		22, 17, 0, 7, 51, 52, 37, 34, 51, 52, 37, 34, 22, 17, 0, 7, 7, 0, 17, 22, 34, 37, 52, 51, 112, 119, 102,
		97, 85, 82, 67, 68, 68, 67, 82, 85, 97, 102, 119, 112, 112, 119, 102, 97, 85, 82, 67, 68, 68, 67, 82,
		85, 97, 102, 119, 112, 51, 52, 37, 34, 22, 17, 0, 7, 7, 0, 17, 22, 34, 37, 52, 51, 34, 37, 52, 51, 7, 0,
		17, 22, 22, 17, 0, 7, 51, 52, 37, 34, 97, 102, 119, 112, 68, 67, 82, 85, 85, 82, 67, 68, 112, 119, 102,
		97, 17, 22, 7, 0, 52, 51, 34, 37, 37, 34, 51, 52, 0, 7, 22, 17, 82, 85, 68, 67, 119, 112, 97, 102, 102,
		97, 112, 119, 67, 68, 85, 82, 67, 68, 85, 82, 102, 97, 112, 119, 119, 112, 97, 102, 82, 85, 68, 67,
		0, 7, 22, 17, 37, 34, 51, 52, 52, 51, 34, 37, 17, 22, 7, 0};

	uint8_t column_parity = 0x77;
	uint8_t line_parity_0 = 0x7F;
	uint8_t line_parity_1 = 0x7F;

	for (int i = 0; i < 128; i++)
	{
		const uint8_t b = buf[i];
		column_parity ^= column_parity_mask[b];
		if (parity_table[b])
		{
			line_parity_0 ^= static_cast<uint8_t>(~i);
			line_parity_1 ^= static_cast<uint8_t>(i);
		}
	}

	return column_parity | (static_cast<uint32_t>(line_parity_0) << 8) | (static_cast<uint32_t>(line_parity_1) << 16);
}

// The .mc2 of a RAW image (the whole 528-byte pages; a cut-off last page is left out, as PCSX2's ConvertRAWtoNoECC did).
inline std::vector<uint8_t> NoEccFromRaw(const uint8_t* raw, size_t raw_size)
{
	const size_t pages = raw_size / kRawPage;
	std::vector<uint8_t> out(pages * kPage);
	for (size_t p = 0; p < pages; p++)
		std::memcpy(&out[p * kPage], raw + p * kRawPage, kPage);
	return out;
}

// The RAW image of a .mc2 (the whole 512-byte pages, each with its ECC bytes and 4 zero bytes, as PCSX2's ConvertNoECCtoRAW writes it).
inline std::vector<uint8_t> RawFromNoEcc(const uint8_t* card, size_t card_size)
{
	const size_t pages = card_size / kPage;
	std::vector<uint8_t> out(pages * kRawPage, 0);
	for (size_t p = 0; p < pages; p++)
	{
		uint8_t* o = &out[p * kRawPage];
		std::memcpy(o, card + p * kPage, kPage);
		for (int j = 0; j < 4; j++)
		{
			const uint32_t ecc = CalculateECC(card + p * kPage + j * 128);
			o[kPage + j * 3 + 0] = static_cast<uint8_t>(ecc);
			o[kPage + j * 3 + 1] = static_cast<uint8_t>(ecc >> 8);
			o[kPage + j * 3 + 2] = static_cast<uint8_t>(ecc >> 16);
		}
	}
	return out;
}

// A card's size without ECC bytes that PCSX2 makes or MemCard PRO2 / SD2PSX use: 8, 16, 32 or 64 MiB.
inline bool IsCardSize(long long card_bytes)
{
	return card_bytes == (8ll << 20) || card_bytes == (16ll << 20) || card_bytes == (32ll << 20) || card_bytes == (64ll << 20);
}

// What to do with a "<card>x" found when the card opens (a session that never closed left it).
enum class Leftover
{
	Recover,  // a RAW image of this card, written after the card was: its saves go back into the card
	NotNewer, // not newer than the card (the card was written back after it, or it was never written to): not used
	BadSize,  // not a RAW image of a card this size: not used
};

// card_size <= 0: there is no card (any card size goes then). Times are the files' modification times (seconds).
inline Leftover DecideLeftover(long long card_size, long long card_mtime, long long raw_size, long long raw_mtime)
{
	if (raw_size <= 0 || raw_size % static_cast<long long>(kRawPage) != 0)
		return Leftover::BadSize;
	const long long as_card = raw_size / static_cast<long long>(kRawPage) * static_cast<long long>(kPage);
	if (card_size > 0 ? as_card != card_size : !IsCardSize(as_card))
		return Leftover::BadSize;
	if (card_size > 0 && raw_mtime <= card_mtime)
		return Leftover::NotNewer;
	return Leftover::Recover;
}

// The folder a path is in ("." for none).
inline std::string FolderOf(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? std::string(".") : slash == 0 ? std::string("/") : path.substr(0, slash);
}

inline bool WriteAll(int fd, const uint8_t* data, size_t n)
{
	while (n > 0)
	{
		const ssize_t w = write(fd, data, n);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return false;
		data += w;
		n -= static_cast<size_t>(w);
	}
	return true;
}

inline void SyncFolder(const std::string& folder)
{
#ifdef O_DIRECTORY
	const int fd = open(folder.c_str(), O_RDONLY | O_DIRECTORY);
#else
	const int fd = open(folder.c_str(), O_RDONLY);
#endif
	if (fd >= 0)
	{
		fsync(fd);
		close(fd);
	}
}

// `path` becomes `data`, durably: "<path>.tmp" written and fsync'd, then (backup non-empty and `path` there) `path` renamed to `backup`,
// the .tmp renamed to `path`, the folder fsync'd. False (and `error` says why) leaves `path` as it was.
inline bool WriteDurable(const std::string& path, const std::vector<uint8_t>& data, const std::string& backup, std::string* error)
{
	const std::string tmp = path + ".tmp";
	const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
	{
		if (error)
			*error = "can't make " + tmp + " (errno " + std::to_string(errno) + ")";
		return false;
	}
	const bool written = WriteAll(fd, data.data(), data.size());
	const int write_errno = errno;
	const bool synced = written && fsync(fd) == 0;
	const int sync_errno = errno;
	close(fd);
	if (!written || !synced)
	{
		unlink(tmp.c_str());
		if (error)
			*error = std::string(!written ? "writing " : "fsync of ") + tmp + " failed (errno " + std::to_string(!written ? write_errno : sync_errno) + ")";
		return false;
	}
	struct stat st = {};
	if (!backup.empty() && stat(path.c_str(), &st) == 0 && rename(path.c_str(), backup.c_str()) != 0)
	{
		const int e = errno;
		unlink(tmp.c_str());
		if (error)
			*error = "can't keep " + path + " as " + backup + " (errno " + std::to_string(e) + ")";
		return false;
	}
	if (rename(tmp.c_str(), path.c_str()) != 0)
	{
		const int e = errno;
		unlink(tmp.c_str());
		if (error)
			*error = "can't rename " + tmp + " over " + path + " (errno " + std::to_string(e) + ")";
		return false;
	}
	SyncFolder(FolderOf(path));
	return true;
}

// A whole file into memory. False when it can't be read.
inline bool ReadAll(const std::string& path, std::vector<uint8_t>& out)
{
	out.clear();
	std::FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	uint8_t buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		out.insert(out.end(), buf, buf + n);
	const bool ok = !std::ferror(f);
	std::fclose(f);
	return ok;
}
} // namespace mcd_noecc
