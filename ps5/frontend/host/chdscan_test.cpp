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
#include <atomic>
#include <cerrno>
#include <iterator>
#include <pthread.h>
#include <thread>
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
static std::atomic<int> g_opens{0}; // opens of files in g_images (chd2: the reads run on threads of their own now)
// chd2: a USB drive's read that doesn't come back. A file whose name has "hang" in it opens as any other, but its reads wait while
// g_block is set (the descriptors are kept in g_hang_fd), as a drive that never answers; clearing it lets them finish.
static std::atomic<bool> g_block{false};
static std::atomic<bool> g_hang_fd[4096];
static std::atomic<int> g_hang_opens{0};
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
	const int fd = static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags, mode));
	const char* slash = std::strrchr(path, '/');
	const bool hang = std::strstr(slash ? slash : path, "hang") != nullptr;
	if (fd >= 0 && fd < 4096)
		g_hang_fd[fd] = hang;
	if (hang)
		g_hang_opens++;
	return fd;
}
extern "C" ssize_t pread(int fd, void* buf, size_t n, off_t off)
{
	while (fd >= 0 && fd < 4096 && g_hang_fd[fd] && g_block)
		usleep(10000);
	return static_cast<ssize_t>(syscall(SYS_pread64, fd, buf, n, off));
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
	std::printf("first start: %s (%d opens)\n", first.seen.c_str(), g_opens.load());
	Check(first.read == 5 && first.known == 0, "first start: all 5 images read");
	Check(first.seen.find("novolume.chd=-(v5, 2448-byte units") != std::string::npos, "the CHD without a volume: no serial, described");
	Check(first.seen.find("damaged.chd=-(libchdr can't open it") != std::string::npos, "the damaged CHD: no serial, libchdr's reason");
	g_opens = 0;
	const Start second = RunStart(cache);
	std::printf("second start: %s (%d opens)\n", second.seen.c_str(), g_opens.load());
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

	// 4. chd2: a read that never ends doesn't stop the scan (the PS5 Pro tester's ~1,800 CHDs, 2.20: "counting up the games until it stuck
	// when it reached game 778"). A big folder (1,000 CHDs, a level of folders, odd names) with images that hang a read for ever: a drive
	// that doesn't answer (hang*.chd), libchdr going round a metadata chain that points at itself (metaloop.chd); a hunk that copies itself
	// (selfloop.chd), a 0-byte and a cut-short CHD, a CHD with a hunk size no CHD has.
	{
		// libchdr itself on metaloop.chd: asked for a CD's track it never comes back (a thread left behind; the fixture is real)
		struct Meta
		{
			std::string path;
			std::atomic<bool> done{false};
		};
		static Meta meta;
		meta.path = fx + "/metaloop.chd";
		pthread_t t;
		pthread_create(&t, nullptr, [](void*) -> void* {
			chd_file* chd = nullptr;
			if (chd_open(meta.path.c_str(), CHD_OPEN_READ, nullptr, &chd) == CHDERR_NONE)
			{
				char text[256];
				uint32_t len = 0;
				chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, 0, text, sizeof(text), &len, nullptr, nullptr);
			}
			meta.done = true;
			return nullptr;
		}, nullptr);
		pthread_detach(t);
		usleep(1500000);
		Check(!meta.done, "metaloop.chd: libchdr's own chd_get_metadata is still going after 1.5 s (the hang is real)");
	}
	fe::SetSerialReadLimit(1.0);
	const std::string big = work + "/big/";
	g_images = big;
	std::system(("rm -rf '" + big + "' && mkdir -p '" + big + "' '" + big + "Disc 2 [EU]' '" + big + "Ünïcode Spiele' '" + big + "a b (c)'").c_str());
	std::system(("cp --sparse=always '" + fx + "/cd.chd' '" + work + "/one.chd'").c_str());
	const char* const subs[] = {"", "Disc 2 [EU]/", "Ünïcode Spiele/", "a b (c)/"};
	for (int i = 0; i < 1000; i++)
	{
		char name[160];
		std::snprintf(name, sizeof(name), "%sGame %04d - Ñame's (Europe) (En,Fr,De) [!].chd", subs[i % 4], i);
		if (link((work + "/one.chd").c_str(), (big + name).c_str()) != 0)
			Check(false, std::string("link ") + name);
	}
	for (const char* f : {"metaloop.chd", "selfloop.chd"})
		std::system(("cp --sparse=always '" + fx + "/" + f + "' '" + big + "Game 0500 " + f + "'").c_str());
	{
		// a hunk size no CHD has (1 GB): libchdr would allocate by it
		std::system(("cp --sparse=always '" + fx + "/dvd4k.chd' '" + big + "Game 0500 hugehunk.chd'").c_str());
		std::fstream h(big + "Game 0500 hugehunk.chd", std::ios::in | std::ios::out | std::ios::binary);
		h.seekp(56);
		const char huge[4] = {0x40, 0, 0, 0};
		h.write(huge, 4);
	}
	std::system(("cp --sparse=always '" + fx + "/dvd4k.chd' '" + big + "Game 0600 hang.chd'").c_str());
	std::system(("touch '" + big + "Game 0700 zero.chd' && head -c 70000 '" + fx + "/dvd2k.chd' > '" + big + "Game 0701 cut.chd'").c_str());
	const std::string cache2 = work + "/cache2/chd-serials.txt";
	struct Scan
	{
		int listed = 0, known = 0, read = 0, serials = 0, gave_up = 0, damaged = 0;
		double seconds = 0;
		std::string notes;
	};
	auto scan = [&](const std::string& images) {
		fe::SetSerialCacheFile(std::string());
		fe::SetSerialCacheFile(cache2);
		Scan s;
		const auto t0 = std::chrono::steady_clock::now();
		for (fe::GameInfo& g : fe::ScanGames({images.substr(0, images.size() - 1)}, true))
		{
			s.listed++;
			std::string what;
			if (fe::KnownSerial(g, g.serial, &what))
				s.known++;
			else
			{
				g.serial = fe::ReadSerial(g, &what);
				s.read++;
				s.gave_up += what.rfind("not read", 0) == 0; // (in this run)
			}
			s.serials += !g.serial.empty();
			s.damaged += what.find("invalid data") != std::string::npos;
			if (g.serial.empty())
				s.notes += g.file + ": " + what.substr(0, 90) + "\n";
		}
		s.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		return s;
	};
	g_block = true;
	g_opens = 0;
	const Scan a = scan(big);
	std::printf("big folder, first start: %d listed, %d read, %d serials, %d given up on, %d damaged, %.1f s, %d opens\n%s", a.listed, a.read,
		a.serials, a.gave_up, a.damaged, a.seconds, g_opens.load(), a.notes.c_str());
	Check(a.listed == 1006, "all 1,006 images listed (1,000 copies in 4 folders, odd names; no count limit)");
	Check(a.serials == 1000, "the 1,000 good CHDs have their serials");
	Check(a.gave_up == 2 && a.seconds < 12.0, "the drive that never answers and libchdr's endless metadata loop: given up on after 1 s each, the scan goes on (" +
												  std::to_string(a.seconds) + " s)");
	Check(a.notes.find("selfloop.chd: libchdr can't open it: invalid data (its map has a hunk that is a copy") != std::string::npos,
		"selfloop.chd: damaged (a hunk that copies itself), not handed to libchdr");
	Check(a.notes.find("hugehunk.chd: libchdr can't open it: invalid data (its header says") != std::string::npos, "hugehunk.chd: damaged (its header)");
	Check(a.notes.find("zero.chd: libchdr can't open it") != std::string::npos && a.notes.find("cut.chd: ") != std::string::npos,
		"a 0-byte and a cut-short CHD: no serial, no hang");
	Check(fe::SerialReadsStuck() == 2, "2 reads given up on are still running");
	{
		std::ifstream f(work + "/cache2/bad-images.txt");
		std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		Check(all.rfind("# PS5SX2: images whose reading never finished", 0) == 0 && all.find("metaloop.chd\t") != std::string::npos &&
				  all.find("hang.chd\t") != std::string::npos && all.find("selfloop") == std::string::npos,
			"bad-images.txt names the 2 images given up on, and not the damaged ones (no-serial.txt has those)");
		Check(access((work + "/cache2/reading-image.txt").c_str(), F_OK) != 0, "reading-image.txt is gone once the reads were given up on");
	}
	// The drive answers again: the read given up on ends (it is no longer counted as stuck).
	g_block = false;
	usleep(300000);
	Check(fe::SerialReadsStuck() == 1, "the drive's read ended once it answered; libchdr's loop is still the one stuck");
	g_opens = 0;
	g_hang_opens = 0;
	const Scan b = scan(big);
	std::printf("big folder, second start: %d known, %d read, %.2f s, %d opens\n", b.known, b.read, b.seconds, g_opens.load());
	Check(b.known == 1006 && b.read == 0 && g_opens == 0, "second start: all from the cache files (the 2 given up on too), no image opened");
	// An app closed while it read an image (the tester's 20 minutes): reading-image.txt names it; the next start skips it from then on.
	{
		std::system(("cp --sparse=always '" + fx + "/dvd4k.chd' '" + big + "Game 0800 hang2.chd'").c_str());
		struct stat st = {};
		stat((big + "Game 0800 hang2.chd").c_str(), &st);
		std::ofstream(work + "/cache2/reading-image.txt") << big << "Game 0800 hang2.chd\t" << st.st_size << "\t" << st.st_mtime << "\n";
		g_hang_opens = 0;
		const Scan c = scan(big);
		Check(c.read == 0 && c.known == 1007 && g_hang_opens == 0, "the image a closed app was reading (reading-image.txt) is not read again");
		Check(access((work + "/cache2/reading-image.txt").c_str(), F_OK) != 0, "... and reading-image.txt is taken (gone)");
	}
	// A read still going after a second is named in reading-image.txt while it runs.
	{
		std::system(("cp --sparse=always '" + fx + "/dvd4k.chd' '" + big + "Game 0801 hang3.chd'").c_str());
		fe::SetSerialReadLimit(2.0);
		g_block = true;
		std::atomic<bool> seen{false}, stop{false};
		std::string reading_file = work + "/cache2/reading-image.txt";
		std::thread watch([&] {
			while (!stop)
			{
				std::ifstream f(reading_file);
				std::string l;
				if (std::getline(f, l) && l.find("hang3.chd\t") != std::string::npos)
					seen = true;
				usleep(20000);
			}
		});
		const Scan d = scan(big);
		stop = true;
		watch.join();
		Check(seen && d.read == 1 && d.gave_up == 1, "a read going on after 1 s is in reading-image.txt while it runs");
	}
	// The drive answers no more: after 3 reads given up on, no more are tried in this run (and the next start reads them).
	{
		for (const char* f : {"Game 0900 hang4.chd", "Game 0901 hang5.chd", "Game 0902 hang6.chd"})
			std::system(("cp --sparse=always '" + fx + "/dvd4k.chd' '" + big + f + "'").c_str());
		fe::SetSerialReadLimit(0.5);
		const Scan e = scan(big);
		std::printf("drive gone: %d read, %d given up on, %.1f s, %d stuck\n%s", e.read, e.gave_up, e.seconds, fe::SerialReadsStuck(), e.notes.c_str());
		Check(fe::SerialReadsStuck() == 3 && e.notes.find("hang5.chd: not read in this run: 3 reads of other images never ended") != std::string::npos &&
				  e.notes.find("hang6.chd: not read in this run") != std::string::npos,
			"3 reads stuck (libchdr's loop, hang3, hang4): the next images aren't tried in this run");
		std::ifstream f(work + "/cache2/bad-images.txt");
		std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		Check(all.find("hang4.chd") != std::string::npos && all.find("hang5.chd") == std::string::npos && all.find("hang6.chd") == std::string::npos,
			"... and they aren't in bad-images.txt: the next start reads them");
		g_block = false;
	}

	std::printf("%s\n", fails ? "SOME FAILED" : "all passed");
	return fails ? 1 : 0;
}
