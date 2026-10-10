// PS5 port frontend, PC test (2.02, AI-assisted; users: the black screen before the shelf got much longer in 2.01): every
// image's serial goes through the serial cache file, so a second start opens no image at all -- 2.01 read every ISO/BIN
// at every start (2,287 games on an NFS share: 104 s before the shelf), and opened each raw .bin twice.
//   ps5/frontend/host/test-serialcache.sh
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fe_games.h"
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

static std::string g_dir;
static int g_opens = 0; // opens of files under g_dir/images (the "NFS share")
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
	if (!g_dir.empty() && std::strncmp(path, (g_dir + "/images/").c_str(), g_dir.size() + 8) == 0)
		g_opens++;
	return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags, mode));
}
static int fails = 0;
static void Check(bool ok, const std::string& w) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", w.c_str()); fails += !ok; }
static std::vector<uint8_t> Iso(const std::string& boot_elf, size_t sectors) // as rawbin_test.cpp
{
	std::vector<uint8_t> img(sectors * 2048, 0);
	auto le32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) img[at + i] = static_cast<uint8_t>(v >> (8 * i)); };
	uint8_t* pvd = &img[16 * 2048];
	pvd[0] = 1; std::memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
	const size_t root = 16 * 2048 + 156;
	img[root] = 34; le32(root + 2, 18); le32(root + 10, 2048); img[root + 25] = 2; img[root + 32] = 1;
	uint8_t* term = &img[17 * 2048]; term[0] = 255; std::memcpy(term + 1, "CD001", 5);
	const std::string cnf = "BOOT2 = cdrom0:\\" + boot_elf + ";1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
	const std::string name = "SYSTEM.CNF;1";
	const size_t rec = 18 * 2048;
	img[rec] = static_cast<uint8_t>(33 + name.size() + ((33 + name.size()) & 1));
	le32(rec + 2, 19); le32(rec + 10, static_cast<uint32_t>(cnf.size()));
	img[rec + 32] = static_cast<uint8_t>(name.size());
	std::memcpy(&img[rec + 33], name.data(), name.size());
	std::memcpy(&img[19 * 2048], cnf.data(), cnf.size());
	return img;
}
static void WriteRaw(const std::string& path, const std::vector<uint8_t>& iso, size_t block, size_t offset)
{
	std::vector<uint8_t> out(iso.size() / 2048 * block, 0xAB);
	for (size_t s = 0; s < iso.size() / 2048; s++)
		std::memcpy(&out[s * block + offset], &iso[s * 2048], 2048);
	std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}
// One "start": the shelf's scan and serial reads (fe_ps5.cpp's orbis_frontend_run), with the cache file read afresh.
static std::string Start(const std::string& cache)
{
	fe::SetSerialCacheFile(std::string());
	fe::SetSerialCacheFile(cache);
	std::string seen;
	for (auto& g : fe::ScanGames({g_dir + "/images"}))
		seen += g.file + "=" + fe::ReadSerial(g) + "; ";
	return seen;
}
int main(int argc, char** argv)
{
	g_dir = argc > 1 ? argv[1] : "/tmp/serialcache-test";
	std::system(("rm -rf '" + g_dir + "' && mkdir -p '" + g_dir + "/images/Game A' '" + g_dir + "/images/Game B'").c_str());
	const size_t big = (17u << 20) / 2048;
	WriteRaw(g_dir + "/images/Game A/Game A (USA).iso", Iso("SLUS_203.70", 64), 2048, 0);
	WriteRaw(g_dir + "/images/Game B/Game B (Europe).bin", Iso("SLES_123.45", big), 2352, 24);
	const std::string cache = g_dir + "/cache/chd-serials.txt";

	g_opens = 0;
	std::string first = Start(cache);
	std::printf("first start: %s(%d image opens)\n", first.c_str(), g_opens);
	Check(first.find("Game A (USA).iso=SLUS-20370") != std::string::npos && first.find("Game B (Europe).bin=SLES-12345") != std::string::npos,
		"the first start reads both serials");
	Check(g_opens == 3, "the first start opens the .iso once and the .bin twice (its data-disc check, its serial)");

	g_opens = 0;
	std::string second = Start(cache);
	std::printf("second start: %s(%d image opens)\n", second.c_str(), g_opens);
	Check(second == first, "the second start lists the same serials");
	Check(g_opens == 0, "the second start opens no image (the serials and the .bin's data-disc check come from the cache)");

	// A file changed (another mtime): read again.
	struct timeval tv[2] = {{1000000000, 0}, {1000000000, 0}};
	utimes((g_dir + "/images/Game A/Game A (USA).iso").c_str(), tv);
	g_opens = 0;
	Start(cache);
	Check(g_opens == 1, "an image with another mtime is read again (only that one)");

	Check(fe::ReadSerial(g_dir + "/images/Game A/Game A (USA).iso") == "SLUS-20370", "ReadSerial(path) still works");
	std::system(("rm -rf '" + g_dir + "'").c_str());
	std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
