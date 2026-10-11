// PS5 port frontend (vk-285-134, AI-assisted): the PS2 BIOS, before a game starts.
//
// Build 130's logs: 961 starts on 235 consoles (one console in eight) failed for want of a BIOS, some consoles trying the
// same game dozens of times in a row. What their BIOS folders held instead: nothing (700 of those starts), folders of
// folders, PS1 BIOS files (512 KB), only a BIOS's ROM1/NVM parts, a .zip, a game image, a 0-byte file. PCSX2 looks for the
// BIOS by itself (BiosTools.cpp; on the PS5 also in the BIOS folder's folders, /data/PCSX2 and the drives); these helpers
// take a BIOS out of an archive into the BIOS folder, and say in a sentence what was found instead, for the shelf.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace fe
{
// A PS2 BIOS file's size: 4 to 8 MB (PCSX2's MIN_BIOS_SIZE and MAX_BIOS_SIZE).
inline constexpr uint64_t kBiosMinSize = 4u * 1024u * 1024u;
inline constexpr uint64_t kBiosMaxSize = 8u * 1024u * 1024u;

// .zip, .rar and .7z files in `dirs` and in their folders (three levels down): each file of a BIOS's size inside is written
// to `dest_dir` (made when it isn't there) and kept when `is_bios(path)` says it is a PS2 BIOS, else removed again. Returns
// the first one kept ("" when none) and, in `from`, the archive it came from. `tried` (may be null) holds the archives
// already looked through, which are skipped.
std::string ExtractBiosFromArchives(const std::vector<std::string>& dirs, const std::string& dest_dir,
	const std::function<bool(const std::string&)>& is_bios, std::set<std::string>* tried, std::string* from);

// What `bios_dir` (three levels down) and `dirs` (their own files) hold instead of a PS2 BIOS, as one English sentence for
// the shelf's line under its translated headline. `top_dir` is the folder `bios_dir` is when no bios folder exists
// (/data/PCSX2), which the sentence then says.
std::string DescribeBiosProblem(const std::string& bios_dir, const std::string& top_dir, const std::vector<std::string>& dirs,
	const std::function<bool(const std::string&)>& is_bios);
} // namespace fe
