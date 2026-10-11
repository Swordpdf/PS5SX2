// PS5 port, PC test (vk-285-135, AI-assisted): games on NFS shares (orbis-shims/OrbisNfs.cpp) against two go-nfs servers:
// one that mounts what it's asked, one that refuses every path but /export (a FreeBSD server without -alldirs). Linked
// with OrbisNfs.wrap's --wrap list (and opendir/readdir/closedir) as the console's eboot is, so the C library's calls
// below go through OrbisNfs as PS5SX2's do: the addresses, the mount (a shorter path when refused), /nfs's own folders,
// listings, stat from what a listing said, FILE and descriptor reads compared with the files themselves (window edges,
// big reads, the end of a file, threads), the frontend's game scan and serials, writes refused, a server that isn't
// there, and other paths untouched.
//   ps5/coreorbis/tests/nfs/test-nfs.sh
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisNfs.h"
#include "fe_games.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

static int s_fails = 0, s_checks = 0;
static void Check(bool ok, const std::string& what)
{
	s_checks++;
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
	if (!ok)
		s_fails++;
}

static double Now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A 2048-byte-sector ISO 9660 image with SYSTEM.CNF naming `boot_elf` (as the frontend's rawbin test makes them), the
// sectors after the file system filled with a pattern that differs at every byte offset.
static std::vector<uint8_t> Iso(const std::string& boot_elf, size_t sectors, uint32_t seed)
{
	std::vector<uint8_t> img(sectors * 2048, 0);
	auto le32 = [&](size_t at, uint32_t v) {
		for (int i = 0; i < 4; i++)
			img[at + i] = static_cast<uint8_t>(v >> (8 * i));
	};
	uint8_t* pvd = &img[16 * 2048];
	pvd[0] = 1;
	std::memcpy(pvd + 1, "CD001", 5);
	pvd[6] = 1;
	const size_t root = 16 * 2048 + 156;
	img[root] = 34;
	le32(root + 2, 18);
	le32(root + 10, 2048);
	img[root + 25] = 2;
	img[root + 32] = 1;
	uint8_t* term = &img[17 * 2048];
	term[0] = 255;
	std::memcpy(term + 1, "CD001", 5);
	const std::string cnf = "BOOT2 = cdrom0:\\" + boot_elf + ";1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
	const std::string name = "SYSTEM.CNF;1";
	const size_t rec = 18 * 2048;
	img[rec] = static_cast<uint8_t>(33 + name.size() + ((33 + name.size()) & 1));
	le32(rec + 2, 19);
	le32(rec + 10, static_cast<uint32_t>(cnf.size()));
	img[rec + 32] = static_cast<uint8_t>(name.size());
	std::memcpy(&img[rec + 33], name.data(), name.size());
	std::memcpy(&img[19 * 2048], cnf.data(), cnf.size());
	uint32_t x = seed;
	for (size_t i = 20 * 2048; i < img.size(); i++)
	{
		x = x * 1664525u + 1013904223u;
		img[i] = static_cast<uint8_t>(x >> 24);
	}
	return img;
}

static void Put(const std::string& path, const std::vector<uint8_t>& data)
{
	// Written through the real calls: the test's own folder isn't a share.
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
	{
		std::printf("can't write %s\n", path.c_str());
		std::exit(2);
	}
	std::fwrite(data.data(), 1, data.size(), f);
	std::fclose(f);
}

static void MkDir(const std::string& path)
{
	mkdir(path.c_str(), 0777);
}

static std::vector<std::string> List(const std::string& dir)
{
	std::vector<std::string> names;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (const dirent* e = readdir(d))
		{
			if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) // a local folder lists them
				names.push_back(std::string(e->d_name) + (e->d_type == DT_DIR ? "/" : ""));
		}
		closedir(d);
	}
	std::sort(names.begin(), names.end());
	return names;
}

static std::string Joined(const std::vector<std::string>& v)
{
	std::string s;
	for (const std::string& x : v)
		s += (s.empty() ? "" : ", ") + x;
	return s;
}

// FILE reads at `off` of `n` bytes against the file's bytes.
static bool SameAt(FILE* f, const std::vector<uint8_t>& want, uint64_t off, size_t n)
{
	std::vector<uint8_t> got(n + 1, 0xEE);
	if (fseeko(f, static_cast<off_t>(off), SEEK_SET) != 0)
		return false;
	const size_t expect = off >= want.size() ? 0 : std::min<uint64_t>(n, want.size() - off);
	const size_t r = std::fread(got.data(), 1, n, f);
	if (r != expect)
	{
		std::printf("    fread at %llu of %zu: %zu, wanted %zu\n", static_cast<unsigned long long>(off), n, r, expect);
		return false;
	}
	return std::memcmp(got.data(), want.data() + off, r) == 0 && ftello(f) == static_cast<off_t>(off + r);
}

int main(int argc, char** argv)
{
	if (argc < 5)
	{
		std::printf("usage: test_nfs <served folder> <port> <strict folder> <strict port>\n");
		return 2;
	}
	const std::string root = argv[1], port = argv[2], sroot = argv[3], sport = argv[4];

	// ---- the files ----
	MkDir(root + "/PS2");
	MkDir(root + "/PS2/More");
	MkDir(sroot + "/PS2");
	const std::vector<uint8_t> game1 = Iso("SLUS_203.70", 12 * 1024 + 7, 1); // 24 MB and a bit: past a few windows
	const std::vector<uint8_t> game2 = Iso("SLES_123.45", 600, 2);
	const std::vector<uint8_t> game3 = Iso("SCUS_971.99", 900, 3);
	Put(root + "/PS2/Test Game (USA).iso", game1);
	Put(root + "/PS2/More/Other Game (Europe).iso", game2);
	Put(root + "/PS2/readme.txt", {'h', 'i', '\n'});
	Put(sroot + "/PS2/Strict Game (USA).iso", game3);

	// ---- addresses ----
	{
		OrbisNfs::Address a;
		std::string e;
		Check(OrbisNfs::ParseAddress("nfs://192.168.1.10/volume1/PS2", a, e) && a.host == "192.168.1.10" && a.path == "/volume1/PS2" &&
				  a.nfsport == 0 && a.version == 0 && OrbisNfs::MountPointOf(a) == "/nfs/192.168.1.10/volume1/PS2",
			"an address: host, path, mount point");
		Check(OrbisNfs::ParseAddress(" NFS://nas:2049//srv/./games/?version=4&uid=1000&gid=100 ", a, e) && a.host == "nas" &&
				  a.path == "/srv/games" && a.nfsport == 2049 && a.version == 4 && a.uid == 1000 && a.gid == 100,
			"port, version, ids, and the path tidied");
		Check(OrbisNfs::ParseAddress("nfs://pc/My%20Games", a, e) && a.path == "/My Games" && OrbisNfs::MountPointOf(a) == "/nfs/pc/My Games",
			"%20 in the path is a space");
		Check(OrbisNfs::ParseAddress("nfs://pc", a, e) && a.path == "/" && OrbisNfs::MountPointOf(a) == "/nfs/pc", "the server's root");
		Check(!OrbisNfs::ParseAddress("smb://pc/games", a, e), "not nfs:// (" + e + ")");
		Check(!OrbisNfs::ParseAddress("nfs:///games", a, e), "no server (" + e + ")");
		Check(!OrbisNfs::ParseAddress("nfs://pc:99999/x", a, e), "a bad port (" + e + ")");
		Check(!OrbisNfs::ParseAddress("nfs://pc/../x", a, e), "climbing out (" + e + ")");
		Check(!OrbisNfs::ParseAddress("nfs://pc/x?speed=fast", a, e), "an unknown option (" + e + ")");
	}

	// ---- not a share: everything works as before ----
	{
		struct stat st;
		Check(stat(root.c_str(), &st) == 0 && S_ISDIR(st.st_mode), "with no shares set, other paths are the system's");
		errno = 0;
		Check(stat("/nfs", &st) != 0, "and /nfs isn't there without shares");
	}

	// ---- the shares ----
	std::vector<std::string> problems;
	const std::string list = "nfs://127.0.0.1/?nfsport=" + port + "&mountport=" + port +
	                         "; nfs://127.0.0.2/export/PS2?nfsport=" + sport + "&mountport=" + sport + "\nnot an address";
	const std::vector<std::string> points = OrbisNfs::SetShares(list, &problems);
	Check(points.size() == 2 && points[0] == "/nfs/127.0.0.1" && points[1] == "/nfs/127.0.0.2/export/PS2" && problems.size() == 1,
		"two shares and one problem: " + Joined(points) + " / " + Joined(problems));
	const double t0 = Now();
	const auto states = OrbisNfs::MountAll(4000);
	for (const auto& s : states)
		std::printf("    %s -> %s: %s\n", s.url.c_str(), s.mount_point.c_str(), s.state.c_str());
	Check(states.size() == 2 && states[0].mounted && states[1].mounted, "both mount (" + std::to_string(Now() - t0) + " s)");
	Check(states.size() == 2 && states[1].state.find("mounted /export (NFS v3), folder /PS2") == 0,
		"the strict server: /export/PS2 refused, /export mounted, /PS2 the folder in it");

	// ---- /nfs's own folders ----
	Check(Joined(List("/nfs")) == "127.0.0.1/, 127.0.0.2/", "/nfs lists the servers: " + Joined(List("/nfs")));
	Check(Joined(List("/nfs/127.0.0.2")) == "export/", "a folder above a share lists the way to it");
	{
		struct stat st;
		Check(stat("/nfs/127.0.0.2/export", &st) == 0 && S_ISDIR(st.st_mode), "and is a folder to stat()");
	}

	// ---- listings, and stat() from them ----
	const std::string ps2 = "/nfs/127.0.0.1/PS2";
	Check(Joined(List(ps2)) == "More/, Test Game (USA).iso, readme.txt", "a share's folder: " + Joined(List(ps2)));
	Check(Joined(List("/nfs/127.0.0.2/export/PS2")) == "Strict Game (USA).iso", "the strict share's folder");
	{
		const auto before = OrbisNfs::GetStats();
		struct stat st;
		const bool a = stat((ps2 + "/Test Game (USA).iso").c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
		               st.st_size == static_cast<off_t>(game1.size());
		errno = 0;
		const bool b = stat((ps2 + "/Missing.iso").c_str(), &st) != 0 && errno == ENOENT;
		const auto after = OrbisNfs::GetStats();
		Check(a && b && after.lookups == before.lookups && after.cache_hits >= before.cache_hits + 2,
			"stat() after a listing asks nothing of the server (size, and a name that isn't there)");
		Check(stat((ps2 + "/More").c_str(), &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0222) == 0, "a folder, read-only");
		Check(access((ps2 + "/readme.txt").c_str(), R_OK) == 0 && access((ps2 + "/readme.txt").c_str(), W_OK) != 0 && errno == EROFS,
			"access(): readable, not writable");
	}

	// ---- FILE reads ----
	{
		FILE* f = std::fopen((ps2 + "/Test Game (USA).iso").c_str(), "rb");
		Check(f != nullptr, "fopen on a share");
		if (f)
		{
			Check(fseeko(f, 0, SEEK_END) == 0 && ftello(f) == static_cast<off_t>(game1.size()) && fseeko(f, 0, SEEK_SET) == 0,
				"the size by seeking to the end (FileSystem::FSize64)");
			bool ok = true;
			// Window edges: 64 KB, 128 KB... and a byte either side; a read bigger than the window; small reads.
			const uint64_t offs[] = {0, 1, 2047, 65535, 65536, 65537, 131071, 1048575, 1048576, 3 * 1048576 + 5, 16 * 2048, 20 * 1048576 + 3};
			const size_t sizes[] = {1, 2048, 65537, 4 * 1048576 + 17};
			for (const uint64_t o : offs)
				for (const size_t n : sizes)
					ok = SameAt(f, game1, o, n) && ok;
			Check(ok, "reads at window edges and of every size match the file");
			// Sequential 32 KB reads through 8 MB, as the CD reader does.
			ok = fseeko(f, 5 * 2048, SEEK_SET) == 0;
			std::vector<uint8_t> buf(32768);
			for (uint64_t at = 5 * 2048; ok && at < 5 * 2048 + (8u << 20); at += buf.size())
				ok = std::fread(buf.data(), 1, buf.size(), f) == buf.size() && std::memcmp(buf.data(), game1.data() + at, buf.size()) == 0;
			Check(ok, "8 MB read 32 KB at a time, in order");
			// The end: a short read, then nothing, with feof().
			ok = SameAt(f, game1, game1.size() - 100, 4096) && std::feof(f) && !std::ferror(f);
			std::vector<uint8_t> one(16);
			ok = ok && std::fread(one.data(), 1, 16, f) == 0 && std::feof(f);
			ok = ok && fseeko(f, 0, SEEK_SET) == 0 && !std::feof(f);
			Check(ok, "a read past the end is short, feof() says so, a seek clears it");
			Check(std::fgetc(f) == game1[0] && std::fgetc(f) == game1[1], "fgetc");
			char line[64];
			Check(fseeko(f, 19 * 2048, SEEK_SET) == 0 && std::fgets(line, sizeof(line), f) && std::string(line) == "BOOT2 = cdrom0:\\SLUS_203.70;1\r\n",
				"fgets reads SYSTEM.CNF's first line");
			Check(std::fwrite("x", 1, 1, f) == 0 && std::fputs("x", f) == EOF && std::fprintf(f, "%d", 1) < 0, "writing to it fails");
			struct stat st;
			Check(fstat(fileno(f), &st) == 0 && st.st_size == static_cast<off_t>(game1.size()) && close(fileno(f)) != 0,
				"fstat(fileno()) answers; close(fileno()) doesn't close the FILE");
			Check(std::fclose(f) == 0, "fclose");
		}
		errno = 0;
		Check(std::fopen((ps2 + "/Nope.iso").c_str(), "rb") == nullptr && errno == ENOENT, "fopen of a missing file: ENOENT");
		errno = 0;
		Check(std::fopen((ps2 + "/new.txt").c_str(), "wb") == nullptr && errno == EROFS, "fopen for writing: EROFS");
		errno = 0;
		Check(std::fopen((ps2 + "/More").c_str(), "rb") == nullptr && errno == EISDIR, "fopen of a folder: EISDIR");
	}

	// ---- descriptors ----
	{
		const int fd = open((ps2 + "/Test Game (USA).iso").c_str(), O_RDONLY);
		Check(fd >= 0, "open on a share");
		std::vector<uint8_t> buf(70000);
		Check(pread(fd, buf.data(), buf.size(), 12345) == static_cast<ssize_t>(buf.size()) &&
				  std::memcmp(buf.data(), game1.data() + 12345, buf.size()) == 0,
			"pread");
		Check(lseek(fd, 2048 * 16, SEEK_SET) == 2048 * 16 && read(fd, buf.data(), 2048) == 2048 && std::memcmp(buf.data(), game1.data() + 2048 * 16, 2048) == 0 &&
				  lseek(fd, 0, SEEK_CUR) == 2048 * 17,
			"lseek and read");
		struct stat st;
		Check(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == static_cast<off_t>(game1.size()), "fstat");
		Check(close(fd) == 0 && close(fd) != 0, "close (once)");
		errno = 0;
		Check(open((ps2 + "/x").c_str(), O_WRONLY | O_CREAT, 0644) < 0 && errno == EROFS, "open for writing: EROFS");
	}

	// ---- the frontend's scan and serials (fe_games.cpp: opendir/readdir/stat, open/pread) ----
	{
		const auto games = fe::ScanGames({ps2, "/nfs/127.0.0.2/export/PS2"});
		std::string seen;
		for (const auto& g : games)
			seen += g.file + "=" + fe::ReadSerial(g.path) + "; ";
		std::printf("    %s\n", seen.c_str());
		Check(seen.find("Test Game (USA).iso=SLUS-20370") != std::string::npos, "the shelf lists a share's game, with its serial");
		Check(seen.find("Other Game (Europe).iso=SLES-12345") != std::string::npos, "and one in a folder in it");
		Check(seen.find("Strict Game (USA).iso=SCUS-97199") != std::string::npos, "and the strict share's");
	}

	// ---- threads: four readers of one file ----
	{
		std::atomic<int> bad{0};
		std::vector<std::thread> threads;
		for (int t = 0; t < 4; t++)
		{
			threads.emplace_back([&, t]() {
				FILE* f = std::fopen((ps2 + "/Test Game (USA).iso").c_str(), "rb");
				if (!f)
				{
					bad++;
					return;
				}
				std::vector<uint8_t> b(50000);
				for (int i = 0; i < 40; i++)
				{
					const uint64_t off = (static_cast<uint64_t>(t) * 6000000 + static_cast<uint64_t>(i) * 131071) % (game1.size() - b.size());
					if (fseeko(f, static_cast<off_t>(off), SEEK_SET) != 0 || std::fread(b.data(), 1, b.size(), f) != b.size() ||
						std::memcmp(b.data(), game1.data() + off, b.size()) != 0)
						bad++;
				}
				std::fclose(f);
			});
		}
		for (std::thread& t : threads)
			t.join();
		Check(bad == 0, "four threads reading one file each get its bytes");
	}

	// ---- many files ----
	{
		std::vector<FILE*> open_files;
		for (int i = 0; i < 80; i++)
		{
			FILE* f = std::fopen((ps2 + "/readme.txt").c_str(), "rb");
			if (!f)
				break;
			open_files.push_back(f);
		}
		const bool full = open_files.size() == 64 && errno == EMFILE;
		for (FILE* f : open_files)
			std::fclose(f);
		FILE* again = std::fopen((ps2 + "/readme.txt").c_str(), "rb");
		Check(full && again, "64 files open at once, the 65th refused (EMFILE), and slots come back");
		if (again)
			std::fclose(again);
	}

	// ---- paths ----
	{
		char buf[PATH_MAX];
		Check(realpath((ps2 + "/More/../readme.txt").c_str(), buf) && std::string(buf) == ps2 + "/readme.txt", "realpath tidies a share's path");
		errno = 0;
		Check(!realpath((ps2 + "/More/../nope").c_str(), buf) && errno == ENOENT, "realpath of a missing file fails");
		struct stat st;
		Check(stat((ps2 + "//More/./").c_str(), &st) == 0 && S_ISDIR(st.st_mode), "doubled and dot path parts");
		errno = 0;
		Check(stat("/nfs/10.0.0.1", &st) != 0 && errno == ENOENT, "a server that isn't a share isn't there");
	}

	// ---- the real calls still work for everything else, while shares are set ----
	{
		const std::string local = root + "/PS2/readme.txt";
		FILE* f = std::fopen(local.c_str(), "rb");
		char c[4] = {};
		Check(f && std::fread(c, 1, 3, f) == 3 && std::string(c) == "hi\n" && std::fclose(f) == 0, "a local FILE");
		const int fd = open(local.c_str(), O_RDONLY);
		Check(fd >= 0 && fd < 0x3d000000 && read(fd, c, 3) == 3 && close(fd) == 0, "a local descriptor");
		struct stat st;
		Check(stat(local.c_str(), &st) == 0 && st.st_size == 3, "a local stat");
		Check(Joined(List(root + "/PS2")) == "More/, Test Game (USA).iso, readme.txt", "a local folder");
	}

	// ---- a server that isn't there ----
	{
		OrbisNfs::SetShares("nfs://127.0.0.1/?nfsport=9&mountport=9; nfs://127.0.0.1/?nfsport=" + port + "&mountport=" + port, nullptr);
		const double t1 = Now();
		const auto st2 = OrbisNfs::MountAll(2000);
		const double took = Now() - t1;
		for (const auto& s : st2)
			std::printf("    %s: %s\n", s.url.c_str(), s.state.c_str());
		// Two addresses with one mount point: the first wins.
		Check(st2.size() == 1 && !st2[0].mounted && st2[0].state.find("not mounted") == 0 && took < 8.0,
			"a port nobody answers: not mounted, said why, in " + std::to_string(took) + " s");
		const double t2 = Now();
		struct stat st;
		Check(stat("/nfs/127.0.0.1/PS2", &st) != 0 && Now() - t2 < 0.5, "and isn't tried again at once (no wait)");
		Check(st2.size() == 1 && st2[0].state.find("NFS v3 /:") != std::string::npos && st2[0].state.find("; then NFS v4 /:") != std::string::npos,
			"the state names v3's error and then v4's");
	}
	// An address nothing answers at all (no refusal: the packets go nowhere): given up within about the time asked.
	{
		OrbisNfs::SetShares("nfs://10.255.255.1/games", nullptr);
		const double t1 = Now();
		const auto st3 = OrbisNfs::MountAll(2000);
		const double took = Now() - t1;
		std::printf("    %s: %s (%.1f s)\n", st3.empty() ? "?" : st3[0].url.c_str(), st3.empty() ? "?" : st3[0].state.c_str(), took);
		Check(st3.size() == 1 && !st3[0].mounted && took < 12.0, "a server that never answers: given up in " + std::to_string(took) + " s");
	}

	const auto stats = OrbisNfs::GetStats();
	std::printf("    stats: %llu opens, %llu reads, %llu requests, %.1f MB, %llu from listings, %llu lookups\n",
		static_cast<unsigned long long>(stats.opens), static_cast<unsigned long long>(stats.reads), static_cast<unsigned long long>(stats.rpcs),
		static_cast<double>(stats.bytes) / 1048576.0, static_cast<unsigned long long>(stats.cache_hits),
		static_cast<unsigned long long>(stats.lookups));
	std::printf("%s: %d of %d checks passed (games on NFS shares)\n", s_fails ? "FAIL" : "PASS", s_checks - s_fails, s_checks);
	return s_fails ? 1 : 0;
}
