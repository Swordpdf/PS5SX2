// Host test (bioscheck, 2026-10-29, AI-assisted): the BIOS before every start (orbis-shims/OrbisOpenBios.h) and OpenPS2BIOS through
// PCSX2's own BIOS reading. test-openbios.sh builds it twice with LoadBiosVersion and the romdir struct cut out of pcsx2/ps2/BiosTools.cpp
// (bios_parse.inc): once as the PS5 builds it (__PROSPERO__, the port's naming) and once as upstream PCSX2 (OB_UPSTREAM).
//   test_openbios <OpenPS2BIOS-0.5.bin> <work folder>
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "OrbisOpenBios.h"

// What BiosTools.cpp's LoadBiosVersion needs of PCSX2.
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using s64 = int64_t;
static constexpr u32 _1mb = 1024 * 1024;
[[maybe_unused]] static constexpr u32 MAX_BIOS_SIZE = 8 * _1mb;
[[maybe_unused]] static constexpr u32 DIRENTRY_SIZE = 16;
namespace FileSystem
{
static s64 FSize64(std::FILE* fp)
{
	const long pos = std::ftell(fp);
	std::fseek(fp, 0, SEEK_END);
	const long n = std::ftell(fp);
	std::fseek(fp, pos, SEEK_SET);
	return n;
}
static s64 FTell64(std::FILE* fp) { return std::ftell(fp); }
static int FSeek64(std::FILE* fp, s64 off, int whence) { return std::fseek(fp, static_cast<long>(off), whence); }
} // namespace FileSystem
namespace StringUtil
{
static std::string StdStringFromFormat(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}
} // namespace StringUtil
static struct
{
	void WriteLn(const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		std::printf("      [pcsx2] ");
		std::vprintf(fmt, ap);
		std::printf("\n");
		va_end(ap);
	}
} Console;

#include "bios_parse.inc"

static int fails = 0;
static void Check(bool ok, const std::string& what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
	fails += !ok;
}

struct Parsed
{
	bool ok = false;
	u32 version = 0, region = 0;
	std::string description, zone, serial;
};
static Parsed Parse(const std::string& path)
{
	Parsed p;
	std::FILE* fp = std::fopen(path.c_str(), "rb");
	if (!fp)
		return p;
	p.ok = LoadBiosVersion(fp, p.version, p.description, p.region, p.zone, p.serial);
	std::fclose(fp);
	return p;
}

// A Sony-like image: RESET, ROMDIR, EXTINFO (with a serial at +0x10), ROMVER, and one module. 4 MB.
static std::vector<unsigned char> SonyLike()
{
	std::vector<unsigned char> rom(4 * _1mb, 0xff);
	auto entry = [&](size_t at, const char* name, uint32_t size) {
		std::memset(&rom[at], 0, 16);
		std::memcpy(&rom[at], name, std::strlen(name));
		std::memcpy(&rom[at + 12], &size, 4);
	};
	const uint32_t reset = 0x2000, romdir = 6 * 16, extinfo = 0x30, romver = 0x10, osdsys = 0x100;
	entry(reset + 0, "RESET", reset);
	entry(reset + 16, "ROMDIR", romdir);
	entry(reset + 32, "EXTINFO", extinfo);
	entry(reset + 48, "ROMVER", romver);
	entry(reset + 64, "OSDSYS", osdsys);
	std::memset(&rom[reset + 80], 0, 16);
	size_t at = reset + romdir;
	std::memset(&rom[at], 0, extinfo);
	std::memcpy(&rom[at + 0x10], "SCPH-70004", 10);
	at += extinfo;
	std::memcpy(&rom[at], "0220EC20060210", 14);
	return rom;
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: test_openbios <OpenPS2BIOS-0.5.bin> <work folder>\n");
		return 2;
	}
	const std::string open_bin = argv[1], work = argv[2];

	// PCSX2's own reading of the shipped image.
	const Parsed open = Parse(open_bin);
	Check(open.ok, "PCSX2's LoadBiosVersion takes OpenPS2BIOS-0.5.bin (RESET found, ROMVER read)");
	Check(open.region == 1 && open.zone == "USA", "its region is USA (ROMVER 0005AD...: 'A')");
	Check(open.version == 0x0005, "its version reads as 00.05");
	Check(open.serial.empty(), "no serial is made up from the code after its empty EXTINFO");
#ifdef OB_UPSTREAM
	Check(open.description.find("USA") == 0 && open.description.find("v00.05(28/10/2026)") != std::string::npos &&
	          open.description.find("Devel") != std::string::npos,
		"upstream names it by its ROMVER: " + open.description);
#else
	Check(open.description == "OpenPS2BIOS (experimental)", "the PS5 build names it \"OpenPS2BIOS (experimental)\": " + open.description);
#endif

	// A Sony-like image is read as before.
	const std::vector<unsigned char> sony = SonyLike();
	const std::string sony_path = work + "/sony-like.bin";
	{
		std::FILE* f = std::fopen(sony_path.c_str(), "wb");
		std::fwrite(sony.data(), 1, sony.size(), f);
		std::fclose(f);
	}
	const Parsed s = Parse(sony_path);
	Check(s.ok && s.zone == "Europe" && s.region == 2 && s.version == ((2u << 8) | 20u), "a Sony-like image: Europe v02.20: " + s.description);
	Check(s.serial == "SCPH-70004", "its EXTINFO serial is still read: " + s.serial);
	Check(s.description.find("OpenPS2BIOS") == std::string::npos, "and it isn't taken for the open BIOS");

#ifndef OB_UPSTREAM
	// The header: OpenPS2BIOS by its own text, a Sony-like image not.
	const OrbisBios::OpenBiosInfo info = OrbisBios::IdentifyOpenBiosFile(open_bin.c_str());
	Check(info.open && info.version == "0.5", "IdentifyOpenBios: OpenPS2BIOS 0.5 (\"" + info.version + "\")");
	Check(OrbisBios::OpenBiosLogName(info) == "OpenPS2BIOS (experimental) 0.5", "its log name: " + OrbisBios::OpenBiosLogName(info));
	Check(!OrbisBios::IdentifyOpenBios(sony.data(), sony.size()).open, "a Sony-like image is not OpenPS2BIOS");
	const unsigned char boot_screen_only[] = "xxOpenPS2BIOSxx";
	const OrbisBios::OpenBiosInfo screen = OrbisBios::IdentifyOpenBios(boot_screen_only, sizeof(boot_screen_only) - 1);
	Check(screen.open && screen.version.empty(), "the name alone (no EE line) is the open BIOS of no known version");

	// Which BIOS: the player's own always wins; the open one only when chosen and ready.
	using OrbisBios::Use;
	Check(OrbisBios::Choose(true, true, true) == Use::Own, "own + open chosen: own");
	Check(OrbisBios::Choose(true, false, false) == Use::Own, "own only: own");
	Check(OrbisBios::Choose(false, true, true) == Use::Open, "no own, open chosen and ready: open");
	Check(OrbisBios::Choose(false, true, false) == Use::None, "no own, open chosen but its image couldn't be written: none");
	Check(OrbisBios::Choose(false, false, true) == Use::None, "no own, open not chosen: none (the choice is offered)");

	// Every start path through the gate.
	using OrbisBios::StartFrom;
	const StartFrom all[] = {StartFrom::Shelf, StartFrom::ShelfOutside, StartFrom::GameTile, StartFrom::DiscTile, StartFrom::DiscLaunch,
		StartFrom::BootDisc, StartFrom::PlainList};
	for (StartFrom from : all)
	{
		const std::string name = OrbisBios::StartFromName(from);
		for (Use u : {Use::Own, Use::Open})
		{
			const OrbisBios::Verdict v = OrbisBios::Gate(from, u, true);
			Check(v.start && !v.to_shelf && !v.notify, name + " with " + OrbisBios::UseName(u) + ": starts");
		}
		const OrbisBios::Verdict none = OrbisBios::Gate(from, Use::None, true);
		const OrbisBios::Verdict none_noshelf = OrbisBios::Gate(from, Use::None, false);
		const bool on_shelf = from == StartFrom::Shelf || from == StartFrom::ShelfOutside;
		Check(!none.start && !none_noshelf.start, name + " with no BIOS: doesn't start");
		if (on_shelf)
			Check(none.to_shelf && !none.notify, name + " with no BIOS: stays on the shelf, which shows the choice (no notification)");
		else if (from == StartFrom::PlainList)
			Check(!none.to_shelf && none.notify, name + " with no BIOS: a notification, the app closes (no shelf in that mode)");
		else
			Check(none.to_shelf && none.notify && !none_noshelf.to_shelf && none_noshelf.notify,
				name + " with no BIOS: a notification and the shelf with the choice (the notification only when the shelf is off)");
	}

	// What the open BIOS can't start.
	using OrbisBios::Disc;
	Check(OrbisBios::OpenBiosCanStart(Disc::Ps2) && OrbisBios::OpenBiosCanStart(Disc::Other), "open BIOS: PS2 discs (and no disc) start");
	Check(!OrbisBios::OpenBiosCanStart(Disc::Ps1) && !OrbisBios::OpenBiosCanStart(Disc::DvdVideo) && !OrbisBios::OpenBiosCanStart(Disc::AudioCd),
		"open BIOS: PS1 discs, DVD video and audio CDs are refused");

	// The bundled image written to the cache folder, once.
	const std::string dest = work + "/cache/" + OrbisBios::kOpenBiosDir + "/" + OrbisBios::kOpenBiosFile;
	std::vector<unsigned char> image;
	{
		std::FILE* f = std::fopen(open_bin.c_str(), "rb");
		image.resize(4 * _1mb);
		image.resize(std::fread(image.data(), 1, image.size(), f));
		std::fclose(f);
	}
	std::system(("mkdir -p '" + work + "/cache'").c_str());
	Check(OrbisBios::WriteIfDifferent(dest, image.data(), image.size()), "the image is written to cache/openps2bios/ (the folder made)");
	Check(Parse(dest).description == "OpenPS2BIOS (experimental)", "the written copy reads the same");
	struct stat st1 = {}, st2 = {};
	stat(dest.c_str(), &st1);
	Check(OrbisBios::WriteIfDifferent(dest, image.data(), image.size()), "an identical copy is left alone");
	stat(dest.c_str(), &st2);
	Check(st1.st_ino == st2.st_ino, "(the same file: not written again)");
	{
		std::FILE* f = std::fopen(dest.c_str(), "r+b");
		std::fputc(0, f);
		std::fclose(f);
	}
	Check(OrbisBios::WriteIfDifferent(dest, image.data(), image.size()) && Parse(dest).ok, "a damaged copy is written again");
	Check(!OrbisBios::WriteIfDifferent("/proc/no-such-dir/x/OpenPS2BIOS.bin", image.data(), image.size()), "an unwritable place says so");
#endif

	std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
