// PS5 port frontend, PC test (chdscan, AI-assisted; a PS5 Pro tester: "the black screen while loading the shelf only happens with CHD
// games - ISO is much faster"). Built by test-chdscan.sh with the real libchdr and CHD images made by make_chd_fixtures.py:
//   - a CHD's serial reads the map only as far as the first hunks (fe_games.cpp chdv5::MapPrefix): the bytes read against what libchdr's
//     own chd_open reads, and against an ISO's;
//   - a read far into the image (the boot ELF, ReadAchievementExecutable) still reads the right bytes, DVD and CD;
//   - the serial cache keeps the images without a serial too (no-serial.txt): a second start opens no image at all, a damaged CHD still
//     says what is wrong with it, an image changed since is read again, and a file of another reader's is not used.
//   ps5/frontend/host/test-chdscan.sh
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fe_games.h"
#include "libchdr/chd.h"
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

static std::string g_images;
static int g_opens = 0; // opens of files in g_images
extern "C" int open(const char* path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT)
	{
		va_list ap;
		va_start(ap, flags);
		mode = static_cast<mode_t>(va_arg(ap, int));
		va_end(ap);
	}
	if (!g_images.empty() && std::strncmp(path, g_images.c_str(), g_images.size()) == 0)
		g_opens++;
	return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags, mode));
}

static int fails = 0;
static void Check(bool ok, const std::string& what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
	fails += !ok;
}

static long long ReadBytes() // this process's bytes read so far (/proc/self/io rchar)
{
	std::ifstream f("/proc/self/io");
	std::string k;
	long long v = 0;
	while (f >> k >> v)
		if (k == "rchar:")
			return v;
	return -1;
}

struct Cost
{
	long long bytes;
	double ms;
};

template <typename F>
static Cost Measure(F&& f)
{
	const long long b0 = ReadBytes();
	const auto t0 = std::chrono::steady_clock::now();
	f();
	const auto t1 = std::chrono::steady_clock::now();
	return {ReadBytes() - b0, std::chrono::duration<double, std::milli>(t1 - t0).count()};
}

// What libchdr's own open costs (2.15 opened every CHD this way): chd_open, the hunk of sector 16.
static Cost LibchdrOpen(const std::string& path)
{
	return Measure([&] {
		chd_file* chd = nullptr;
		if (chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &chd) != CHDERR_NONE)
			return;
		std::vector<uint8_t> hunk(chd_get_header(chd)->hunkbytes);
		chd_read(chd, static_cast<uint32_t>(16ull * chd_get_header(chd)->unitbytes / chd_get_header(chd)->hunkbytes), hunk.data());
		chd_close(chd);
	});
}

static std::string Serial(const std::string& path)
{
	fe::SetSerialCacheFile(std::string()); // no cache: the image itself
	return fe::ReadSerial(path);
}

// One "start" as fe_ps5.cpp's ShelfScan makes it: the folder's games, each through the cache files (KnownSerial), else the image.
struct Start
{
	int known = 0, read = 0;
	std::string seen; // "file=serial;" for each game, "file=-(what)" for one without
};
static Start RunStart(const std::string& cache)
{
	fe::SetSerialCacheFile(std::string());
	fe::SetSerialCacheFile(cache);
	Start s;
	for (fe::GameInfo& g : fe::ScanGames({g_images.substr(0, g_images.size() - 1)}, false))
	{
		std::string what;
		if (fe::KnownSerial(g, g.serial, &what))
			s.known++;
		else
		{
			g.serial = fe::ReadSerial(g, &what);
			s.read++;
		}
		s.seen += g.file + "=" + (g.serial.empty() ? "-(" + what.substr(0, 24) + ")" : g.serial) + ";";
	}
	return s;
}

static bool ElfPattern(const std::vector<uint8_t>& b)
{
	if (b.size() != 6000)
		return false;
	for (size_t i = 0; i < b.size(); i++)
		if (b[i] != static_cast<uint8_t>((i * 7 + 3) & 0xff))
			return false;
	return true;
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "chdscan_test <fixtures dir> <work dir>\n");
		return 2;
	}
	const std::string fx = argv[1], work = argv[2];

	// 1. The bytes a serial costs.
	struct Image
	{
		const char* file;
		const char* serial;
	};
	const Image images[] = {{"dvd2k.chd", "SLUS-20370"}, {"dvd4k.chd", "SLUS-20374"}, {"cd.chd", "SLUS-20371"}, {"dvd.iso", "SLUS-20370"},
		{"far2k.chd", "SLUS-20372"}, {"farcd.chd", "SLUS-20373"}};
	long long dvd_now = 0, dvd_before = 0, iso_now = 0;
	std::printf("image        serial       now: bytes / ms      libchdr's own open: bytes / ms\n");
	for (const Image& im : images)
	{
		const std::string path = fx + "/" + im.file;
		std::string serial;
		const Cost now = Measure([&] { serial = Serial(path); });
		const bool chd = std::strstr(im.file, ".chd") != nullptr;
		const Cost before = chd ? LibchdrOpen(path) : Cost{0, 0};
		std::printf("%-12s %-11s %9lld / %6.2f     %9lld / %6.2f\n", im.file, serial.c_str(), now.bytes, now.ms, before.bytes, before.ms);
		Check(serial == im.serial, std::string(im.file) + ": serial " + im.serial);
		if (std::strcmp(im.file, "dvd2k.chd") == 0)
		{
			dvd_now = now.bytes;
			dvd_before = before.bytes;
		}
		if (std::strcmp(im.file, "dvd.iso") == 0)
			iso_now = now.bytes;
		// A DVD's map is megabytes (an eighth at most is read now); a CD's ~110 KB (less than half of it).
		const bool dvd = std::strstr(im.file, "cd.chd") == nullptr;
		if (chd)
			Check(now.bytes * (dvd ? 8 : 2) < before.bytes,
				std::string(im.file) + (dvd ? ": under an eighth" : ": under half") + " of what libchdr's open reads");
	}
	Check(dvd_now > 0 && dvd_now < 600 * 1024, "the 4.2 GB DVD CHD (2048-byte hunks): its serial for under 600 KB read (" + std::to_string(dvd_now) +
												   "; libchdr's open: " + std::to_string(dvd_before) + "; the ISO: " + std::to_string(iso_now) + ")");

	// 2. Reads far into the image: the boot ELF (the RetroAchievements hash, online patches' CRC) is the right bytes.
	for (const char* far : {"far2k.chd", "farcd.chd"})
	{
		std::string name;
		std::vector<uint8_t> bytes;
		const bool ok = fe::ReadAchievementExecutable(fx + "/" + far, name, bytes);
		Check(ok && ElfPattern(bytes), std::string(far) + ": the boot ELF 6000 bytes into the far end of the image read right (" + name + ")");
	}
	{
		std::string name;
		std::vector<uint8_t> bytes;
		Check(!fe::ReadAchievementExecutable(fx + "/dvd2k.chd", name, bytes), "dvd2k.chd: no boot ELF file on it, none read");
	}

	// 3. The cache files: a folder with a PS2 DVD CHD, a CD CHD, an ISO, a CHD without an ISO 9660 volume (another system's disc) and a
	// damaged CHD.
	g_images = work + "/images/";
	std::system(("rm -rf '" + work + "' && mkdir -p '" + g_images + "'").c_str());
	for (const char* f : {"dvd4k.chd", "cd.chd", "dvd.iso", "novolume.chd"})
		std::system(("cp --sparse=always '" + fx + "/" + f + "' '" + g_images + f + "'").c_str());
	{
		std::ofstream bad(g_images + "damaged.chd", std::ios::binary); // a v5 header with no hunk size: libchdr's "invalid data"
		std::string h(124, '\0');
		std::memcpy(&h[0], "MComprHD", 8);
		h[11] = 124;
		h[15] = 5;
		bad.write(h.data(), static_cast<std::streamsize>(h.size()));
		bad << std::string(4096, 'x');
	}
	const std::string cache = work + "/cache/chd-serials.txt";
	g_opens = 0;
	const Start first = RunStart(cache);
	std::printf("first start: %s (%d opens)\n", first.seen.c_str(), g_opens);
	Check(first.read == 5 && first.known == 0, "first start: all 5 images read");
	Check(first.seen.find("novolume.chd=-(v5, 2448-byte units") != std::string::npos, "the CHD without a volume: no serial, described");
	Check(first.seen.find("damaged.chd=-(libchdr can't open it") != std::string::npos, "the damaged CHD: no serial, libchdr's reason");
	g_opens = 0;
	const Start second = RunStart(cache);
	std::printf("second start: %s (%d opens)\n", second.seen.c_str(), g_opens);
	Check(second.known == 5 && second.read == 0 && g_opens == 0, "second start: all 5 from the cache files, no image opened");
	Check(second.seen == first.seen, "second start: the same serials and the same descriptions");
	{
		std::ifstream f(work + "/cache/no-serial.txt");
		std::string head;
		std::getline(f, head);
		Check(head == "# PS5SX2: images without a PS2 serial (reader 1)", "no-serial.txt begins with its reader line");
	}
	// An image changed since (its mtime): read again.
	{
		struct timeval tv[2] = {{1700000000, 0}, {1700000000, 0}};
		utimes((g_images + "novolume.chd").c_str(), tv);
	}
	const Start third = RunStart(cache);
	Check(third.read == 1 && third.known == 4, "an image whose mtime changed is read again, the rest are not");
	// Another reader's file: not used (read again), and begun afresh with this reader's line.
	{
		std::ifstream in(work + "/cache/no-serial.txt");
		std::stringstream ss;
		ss << in.rdbuf();
		std::string text = ss.str();
		text.replace(0, text.find('\n'), "# PS5SX2: images without a PS2 serial (reader 0)");
		std::ofstream(work + "/cache/no-serial.txt", std::ios::binary) << text;
	}
	const Start fourth = RunStart(cache);
	Check(fourth.read == 2 && fourth.known == 3, "another reader's no-serial.txt is not used: the 2 images without a serial are read again");
	const Start fifth = RunStart(cache);
	Check(fifth.read == 0 && fifth.known == 5, "... and the file is begun afresh: the next start reads none");
	// An image gone: no line kept for it (it may come back on a drive plugged in later).
	{
		fe::SetSerialCacheFile(std::string());
		fe::SetSerialCacheFile(cache);
		fe::GameInfo g;
		g.path = g_images + "gone.chd";
		g.bytes = 1;
		g.mtime = 1;
		std::string what;
		fe::ReadSerial(g, &what);
		std::string serial;
		Check(!fe::KnownSerial(g, serial), "a CHD that couldn't be opened (not there) is not kept as having no serial");
	}

	std::printf("%s\n", fails ? "SOME FAILED" : "all passed");
	return fails ? 1 : 0;
}
