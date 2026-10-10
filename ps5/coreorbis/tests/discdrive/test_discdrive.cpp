// PS5SX2: the disc drive code on the console, against the disc in a real drive (from PR #34 by Heyde Moura; AI-assisted
// port). A payload for the ELF loader (test-discdrive.sh builds and sends it): OrbisDiscDrive.cpp, PCSX2's IOCtlSrc from
// OrbisIOCtlSrc.cpp and fe_games.cpp's serial and executable reading on a device, checked against plain reads of the
// device; then PCSX2's own disc reader (CDVDdiscReader.cpp and its thread, CDVDapi_Disc) on top of them, as a game reads
// its disc. Needs a PS2 disc in the drive (the first /dev/cdN, or the one given as DISCDRIVE_DEVICE at build time).
//
// Copyright (C) 2026 Heyde Moura
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CDVD/CDVDdiscReader.h"
#include "Host.h"
#include "common/Console.h"
#include "common/Error.h"
#include "fe_games.h"
#include "OrbisDiscDrive.h"

#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

// PCSX2's Error, as far as OrbisIOCtlSrc.cpp uses it (always with no Error here).
void Error::SetString(Error*, std::string) {}
void Error::SetErrno(Error*, int) {}
void Error::SetStringView(Error*, std::string_view) {}
std::string_view Host::TranslateToStringView(std::string_view, std::string_view msg)
{
	return msg;
}
// What CDVDdiscReader.cpp needs from the rest of PCSX2: its log, and CDVDcommon.cpp's track list.
void Log::Writev(LOGLEVEL, ConsoleColors, const char* format, va_list ap)
{
	std::vprintf(format, ap);
}
u8 strack;
u8 etrack;
std::array<cdvdTrack, 100> tracks;
extern const CDVD_API CDVDapi_Disc;
static int s_new_discs = 0;

static int s_failed = 0;

static void Check(bool ok, const char* what, const std::string& detail = {})
{
	std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
	std::fflush(stdout);
	if (!ok)
		s_failed++;
}

static double Now()
{
	timeval tv;
	gettimeofday(&tv, nullptr);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

int main()
{
	const std::vector<std::string> drives = OrbisDiscDrive::List();
	std::string list;
	for (const std::string& d : drives)
		list += (list.empty() ? "" : " ") + d;
	Check(!drives.empty(), "a disc drive is there", list);
	if (drives.empty())
		return 1;
#ifdef DISCDRIVE_DEVICE
	const std::string dev = DISCDRIVE_DEVICE;
#else
	const std::string dev = drives.front();
#endif

	std::string why;
	Check(OrbisDiscDrive::WaitReady(dev, 15000, &why), "OrbisDiscDrive::WaitReady: a disc, ready", why);
	Check(!OrbisDiscDrive::WaitReady("/dev/cd9", 100), "OrbisDiscDrive::WaitReady: no such drive");

	// The device read plainly, to compare with.
	const int fd = open(dev.c_str(), O_RDONLY);
	Check(fd >= 0, "the device opens");
	std::vector<uint8_t> pvd(2048);
	Check(fd >= 0 && pread(fd, pvd.data(), 2048, 16 * 2048) == 2048 && std::memcmp(pvd.data() + 1, "CD001", 5) == 0,
		"sector 16 is an ISO 9660 volume descriptor");
	const uint32_t volume = pvd[80] | (pvd[81] << 8) | (pvd[82] << 16) | (static_cast<uint32_t>(pvd[83]) << 24);

	{
		const int pfd = OrbisDiscDrive::OpenPassThrough(fd);
		Check(true, "the drive", pfd >= 0 ? OrbisDiscDrive::Describe(pfd) : std::string("no pass-through device"));
		if (pfd >= 0)
			close(pfd);
	}

	IOCtlSrc src(dev);
	Check(src.Reopen(nullptr), "IOCtlSrc::Reopen");
	{
		IOCtlSrc none("/dev/cd9");
		Check(!none.Reopen(nullptr), "IOCtlSrc::Reopen fails for a drive that isn't there");
	}
	const s32 media = src.GetMediaType();
	const u32 sectors = src.GetSectorCount();
	char detail[160];
	std::snprintf(detail, sizeof(detail), "media %d, %u sectors, layer break %u, the volume %u sectors, %zu TOC entries", media,
		sectors, src.GetLayerBreakAddress(), volume, src.ReadTOC().size());
	Check(sectors > 0 && sectors >= volume, "the disc's size covers its volume", detail);
	Check(src.DiscReady(), "IOCtlSrc::DiscReady");
	{
		// The CDVD thread asks on every loop: the drive is asked at most every 500 ms, the answer kept in between.
		const double t = Now();
		bool all = true;
		for (int i = 0; i < 1000; i++)
			all &= src.DiscReady();
		char d[64];
		std::snprintf(d, sizeof(d), "1000 calls in %.1f ms", (Now() - t) * 1000);
		Check(all && Now() - t < 0.5, "IOCtlSrc::DiscReady is cheap when asked often", d);
	}

	std::vector<uint8_t> a(16 * 2048), b(16 * 2048);
	bool same = true;
	for (const u32 at : {0u, 16u, 1000u, sectors / 2, sectors - 16})
	{
		const bool r1 = src.ReadSectors2048(at, 16, a.data());
		const bool r2 = pread(fd, b.data(), b.size(), static_cast<off_t>(at) * 2048) == static_cast<ssize_t>(b.size());
		same &= r1 && r2 && a == b;
	}
	Check(same, "ReadSectors2048 reads what the device holds (start, middle, last sectors)");
	Check(!src.ReadSectors2048(sectors + 64, 1, a.data()), "a sector past the end doesn't read");

	std::vector<uint8_t> raw(4 * 2352);
	const bool raw_ok = src.ReadSectors2352(16, 4, raw.data());
	if (media < 0)
		Check(raw_ok && raw[15] != 0 && std::memcmp(raw.data() + (raw[15] == 2 ? 24 : 16) + 1, "CD001", 5) == 0,
			"ReadSectors2352 on a CD: sector 16's raw frame holds the volume descriptor");
	else
		Check(raw_ok && std::memcmp(raw.data() + 24, pvd.data(), 2048) == 0, "ReadSectors2352 on a DVD: made from the data");

	cdvdSubQ q = {};
	const bool subq = src.ReadTrackSubQ(&q);
	std::printf("INFO sub-channel: %s (adr %u, track %u, index %u)\n", subq ? "read" : "not read", q.adr, q.trackNum, q.trackIndex);

	// Reading speed, 16 sectors at a time as the CDVD thread does, here and there on the disc.
	double t0 = Now();
	uint64_t bytes = 0;
	for (u32 i = 0; i < 256; i++)
	{
		const u32 at = static_cast<u32>((static_cast<uint64_t>(sectors - 32) * ((i * 37) % 256)) / 256);
		if (!src.ReadSectors2048(at, 16, a.data()))
			break;
		bytes += a.size();
	}
	const double scattered = bytes / (Now() - t0) / 1e6;
	t0 = Now();
	uint64_t seq = 0;
	for (u32 i = 0; i < 512; i++)
	{
		if (!src.ReadSectors2048(20000 + i * 16, 16, a.data()))
			break;
		seq += a.size();
	}
	const double sequential = seq / (Now() - t0) / 1e6;
	std::snprintf(detail, sizeof(detail), "%.2f MB/s scattered, %.2f MB/s in a row (a PS2's DVD reads up to 5.3)", scattered, sequential);
	Check(bytes == 256 * a.size() && seq == 512 * a.size(), "reading speed", detail);

	const std::string serial = fe::ReadSerial(dev);
	Check(OrbisDiscDrive::SafeSerial(serial), "fe::ReadSerial on the device: a serial fit for a settings file's name",
		OrbisDiscDrive::SafeSerial(serial) ? serial : std::string("(none, or unfit)"));
	Check(OrbisDiscDrive::IsDrivePath(dev) && !OrbisDiscDrive::IsDrivePath("/data/PCSX2/games/x.iso"), "OrbisDiscDrive::IsDrivePath");
	std::string exe;
	std::vector<uint8_t> elf;
	const bool got = fe::ReadAchievementExecutable(dev, exe, elf);
	std::snprintf(detail, sizeof(detail), "%s, %zu bytes", exe.c_str(), elf.size());
	Check(got && elf.size() > 4 && std::memcmp(elf.data(), "\x7f" "ELF", 4) == 0, "the boot executable reads off the disc", detail);

	// PCSX2's disc reader, as VMManager opens it for CDVD_SourceType::Disc and the CDVD reads a game's sectors.
	CDVDapi_Disc.newDiskCB([] { s_new_discs++; });
	Check(CDVDapi_Disc.open(dev, nullptr), "CDVDapi_Disc.open");
	const s32 type = CDVDapi_Disc.getDiskType();
	std::snprintf(detail, sizeof(detail), "type 0x%02x (0x03: DVD, one layer; 0x04: two; 0x01: CD), tray %d", type,
		CDVDapi_Disc.getTrayStatus());
	Check(media < 0 ? type == CDVD_TYPE_DETCTCD : media == 0 ? type == CDVD_TYPE_DETCTDVDS : type == CDVD_TYPE_DETCTDVDD,
		"the disc's type", detail);
	cdvdTN tn = {};
	cdvdTD td = {};
	Check(CDVDapi_Disc.getTN(&tn) == 0 && CDVDapi_Disc.getTD(0, &td) == 0 && td.lsn == sectors, "the disc's tracks and size",
		std::to_string(tn.strack) + ".." + std::to_string(tn.etrack) + ", " + std::to_string(td.lsn) + " sectors");
	// Sector 16 and the boot executable's first sector the way the CDVD reads them: a request, then its buffer.
	std::vector<uint8_t> sec(2352);
	bool reads = true;
	for (const u32 at : {16u, 17u, 5000u, sectors / 3, sectors - 1})
	{
		reads &= CDVDapi_Disc.readTrack(at, CDVD_MODE_2048) == 0 && CDVDapi_Disc.getBuffer(sec.data()) == 0;
		reads &= pread(fd, b.data(), 2048, static_cast<off_t>(at) * 2048) == 2048 && std::memcmp(sec.data(), b.data(), 2048) == 0;
	}
	Check(reads, "readTrack and getBuffer give the disc's sectors (2048-byte mode)");
	Check(CDVDapi_Disc.readSector(sec.data(), 16, CDVD_MODE_2048) == 0 && std::memcmp(sec.data(), pvd.data(), 2048) == 0,
		"readSector (the direct read)");
	s32 dual = -1;
	u32 layer1 = 0;
	Check(CDVDapi_Disc.getDualInfo(&dual, &layer1) == 0 && dual == (media > 0 ? media : 0), "getDualInfo",
		"type " + std::to_string(dual) + ", layer 1 at " + std::to_string(layer1));
	// A game's loading: 2 MB in a row through the CDVD thread's prefetching.
	t0 = Now();
	bool streamed = true;
	for (u32 i = 0; i < 1024 && streamed; i++)
		streamed = CDVDapi_Disc.readTrack(60000 + i, CDVD_MODE_2048) == 0 && CDVDapi_Disc.getBuffer(sec.data()) == 0;
	std::snprintf(detail, sizeof(detail), "%.2f MB/s", 1024 * 2048 / (Now() - t0) / 1e6);
	Check(streamed, "2 MB read through the CDVD thread", detail);
	CDVDapi_Disc.close();
	Check(s_new_discs == 0, "no disc change seen while it was in");

	if (fd >= 0)
		close(fd);
	std::printf("%s: %d failed\n", s_failed ? "FAILED" : "ALL PASSED", s_failed);
	std::fflush(stdout);
	return s_failed ? 1 : 0;
}
