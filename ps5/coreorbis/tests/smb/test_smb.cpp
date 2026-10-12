// PC test of OrbisSmb against Samba: ps5/coreorbis/tests/smb/test-smb.sh
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisSmb.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <signal.h>
#include <sstream>
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

static std::string Joined(const std::vector<std::string>& v)
{
	std::string s;
	for (const std::string& x : v)
		s += (s.empty() ? "" : ", ") + x;
	return s;
}

static std::vector<std::string> List(const std::string& path)
{
	std::vector<std::string> out;
	void* d = OrbisSmb::OpenDir(path.c_str());
	if (!d)
		return {"(error " + std::to_string(errno) + ")"};
	dirent e;
	while (OrbisSmb::ReadDir(d, &e))
		out.push_back(std::string(e.d_name) + (e.d_type == DT_DIR ? "/" : ""));
	OrbisSmb::CloseDir(d);
	std::sort(out.begin(), out.end());
	return out;
}

static std::vector<uint8_t> Pattern(size_t n, uint32_t seed)
{
	std::vector<uint8_t> v(n);
	uint32_t x = seed;
	for (size_t i = 0; i < n; i++)
	{
		x = x * 1664525u + 1013904223u;
		v[i] = static_cast<uint8_t>((x >> 24) ^ i ^ (i >> 11));
	}
	return v;
}

static void Write(const std::string& path, const std::vector<uint8_t>& data)
{
	std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

static std::string ReadText(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static bool Same(int h, const std::vector<uint8_t>& file, uint64_t off, size_t n)
{
	std::vector<uint8_t> got(n);
	const ssize_t r = OrbisSmb::PRead(h, got.data(), n, off);
	const size_t want = off >= file.size() ? 0 : std::min<size_t>(n, file.size() - off);
	return r == static_cast<ssize_t>(want) && std::equal(got.begin(), got.begin() + want, file.begin() + off);
}

int main(int argc, char** argv)
{
	if (argc != 5)
	{
		std::fprintf(stderr, "usage: %s <public folder> <private folder> <port> <passwords' file>\n", argv[0]);
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);
	const std::string pub = argv[1], priv = argv[2], port = argv[3], creds = argv[4];

	const std::vector<uint8_t> game = Pattern(23 * 1024 * 1024 + 777, 1);
	const std::vector<uint8_t> small = Pattern(5000, 2);
	mkdir((pub + "/PS2").c_str(), 0755);
	mkdir((pub + "/PS2/More").c_str(), 0755);
	Write(pub + "/PS2/Test Game (USA).iso", game);
	Write(pub + "/PS2/readme.txt", small);
	Write(priv + "/PS2/Private Game.iso", small);
	Write(priv + "/elsewhere.bin", small);

	{
		OrbisSmb::Address a;
		std::string e;
		Check(OrbisSmb::ParseAddress("smb://NAS/PS2", a, e) && a.server == "nas" && a.share == "PS2" && a.path == "/" && a.user.empty() &&
				  a.port == 0 && OrbisSmb::MountPointOf(a) == "/smb/nas/PS2",
			"a guest share (the server in lower case)");
		Check(OrbisSmb::ParseAddress(" smb://WORKGROUP;user@nas:4450//Games/./PS2/?version=3 ", a, e) && a.domain == "WORKGROUP" &&
				  a.user == "user" && a.port == 4450 && a.share == "Games" && a.path == "/PS2" && a.version == 3 &&
				  OrbisSmb::MountPointOf(a) == "/smb/nas/Games/PS2",
			"a domain, a user, a port, a folder in the share and SMB 3");
		Check(OrbisSmb::ParseAddress("smb://pc/My%20Games/PS%32", a, e) && a.share == "My Games" && a.path == "/PS2", "%-escapes");
		Check(!OrbisSmb::ParseAddress("smb://user:secret@pc/PS2", a, e) && e.find("password") != std::string::npos,
			"a password in the address is refused (" + e + ")");
		Check(!OrbisSmb::ParseAddress("smb://pc", a, e) && !OrbisSmb::ParseAddress("smb://pc/", a, e), "no share (" + e + ")");
		Check(!OrbisSmb::ParseAddress("nfs://pc/PS2", a, e), "not smb:// (" + e + ")");
		Check(!OrbisSmb::ParseAddress("smb://pc/PS2/../../x", a, e), "climbing out (" + e + ")");
		Check(!OrbisSmb::ParseAddress("smb://pc/PS2?speed=fast", a, e), "an unknown option (" + e + ")");
		Check(!OrbisSmb::ParseAddress("smb://pc:0/PS2", a, e) && !OrbisSmb::ParseAddress("smb://[::1]/PS2", a, e), "a bad port, IPv6");
	}

	{
		std::string e;
		std::remove(creds.c_str());
		Check(OrbisSmb::StoreCredential(creds, "smb://smbtest@127.0.0.1:" + port + "/private", "wrong one", e), "a password stored");
		Check(OrbisSmb::StoreCredential(creds, "smb://smbtest@127.0.0.1:" + port + "/private/PS2", "secret pass", e),
			"stored again for the same share (a folder in it is the same share)");
		Check(OrbisSmb::StoreCredential(creds, "smb://smbtest@127.0.0.1:" + port, "server wide", e), "and one for the whole server");
		Check(OrbisSmb::StoreCredential(creds, "smb://someone@other/x", "", e), "an empty password takes out (nothing here)");
		struct stat st;
		Check(stat(creds.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "the file is 0600");
		const std::string text = ReadText(creds);
		Check(text.find("secret pass") != std::string::npos && text.find("wrong one") == std::string::npos &&
				  text.find("server wide") != std::string::npos && text.find("#") == 0,
			"one line a share, the old password replaced, a comment first");
		Check(!OrbisSmb::StoreCredential(creds, "smb://127.0.0.1/private", "x", e), "no password without a user (" + e + ")");
		Check(!OrbisSmb::StoreCredential(creds, "smb://u@pc/s", "two\nlines", e), "no line break in a password");
		OrbisSmb::SetCredentialsFile(creds);
		Check(OrbisSmb::HasCredential("smb://smbtest@127.0.0.1:" + port + "/private/PS2") &&
				  OrbisSmb::HasCredential("smb://smbtest@127.0.0.1:" + port + "/other") &&
				  !OrbisSmb::HasCredential("smb://127.0.0.1:" + port + "/public") && !OrbisSmb::HasCredential("smb://x@nas/y"),
			"HasCredential: the share's line, the server's line, none for a guest or another server");
	}

	std::vector<std::string> problems;
	const std::string list = "smb://127.0.0.1:" + port + "/public; smb://smbtest@127.0.0.1:" + port + "/private/PS2\n" +
	                         "smb://127.0.0.1:" + port + "/nosuch; smb://stranger@localhost:" + port + "/private; " +
	                         "smb://user:pw@nas/x; smb://127.0.0.1:" + port + "/public";
	const std::vector<std::string> points = OrbisSmb::SetShares(list, &problems);
	Check(points.size() == 4 && points[0] == "/smb/127.0.0.1/public" && points[1] == "/smb/127.0.0.1/private/PS2" && problems.size() == 1,
		"four shares (a repeat left out) and one problem: " + Joined(points) + " / " + Joined(problems));
	Check(problems.size() == 1 && problems[0].find("pw") == std::string::npos && problems[0].find("user:***@nas") != std::string::npos,
		"the problem names the address with its password masked");
	const double t0 = Now();
	const auto states = OrbisSmb::MountAll(4000);
	for (const auto& s : states)
		std::printf("    %s -> %s: %s\n", s.url.c_str(), s.mount_point.c_str(), s.state.c_str());
	Check(states.size() == 4 && states[0].mounted && states[1].mounted, "public and private connect (" + std::to_string(Now() - t0) + " s)");
	Check(states.size() == 4 && states[0].state.find("as Guest") != std::string::npos, "public: as Guest");
	Check(states.size() == 4 && states[1].state.find("with its password") != std::string::npos,
		"private: the share's own line wins over the server's");
	Check(states.size() == 4 && !states[2].mounted && states[2].state.find("; then anonymously") != std::string::npos,
		"a share that isn't there: refused, both ways tried");
	Check(states.size() == 4 && !states[3].mounted, "a user the share doesn't take: refused");
	for (const auto& s : states)
		Check(s.state.find("secret") == std::string::npos && s.state.find("server wide") == std::string::npos, "no password in a state");

	Check(OrbisSmb::IsPath("/smb") && OrbisSmb::IsPath("/smb/x") && !OrbisSmb::IsPath("/smbx") && !OrbisSmb::IsPath("/nfs/x"), "IsPath");
	Check(Joined(List("/smb")) == "127.0.0.1/, localhost/", "/smb lists the servers: " + Joined(List("/smb")));
	Check(Joined(List("/smb/127.0.0.1")) == "nosuch/, private/, public/", "a server's folder lists its shares");
	Check(Joined(List("/smb/127.0.0.1/private")) == "PS2/", "a folder above a share lists the way to it");
	{
		struct stat st;
		Check(OrbisSmb::Stat("/smb/127.0.0.1", &st, true) == 0 && S_ISDIR(st.st_mode), "and is a folder to stat()");
	}

	const std::string ps2 = "/smb/127.0.0.1/public/PS2";
	Check(Joined(List(ps2)) == "More/, Test Game (USA).iso, readme.txt", "a share's folder: " + Joined(List(ps2)));
	Check(Joined(List("/smb/127.0.0.1/private/PS2")) == "Private Game.iso", "the private share's folder (its own PS2, not more)");
	{
		const auto before = OrbisSmb::GetStats();
		struct stat st;
		const bool a = OrbisSmb::Stat((ps2 + "/Test Game (USA).iso").c_str(), &st, true) == 0 && S_ISREG(st.st_mode) &&
		               st.st_size == static_cast<off_t>(game.size()) && (st.st_mode & 0222) == 0;
		errno = 0;
		const bool b = OrbisSmb::Stat((ps2 + "/Missing.iso").c_str(), &st, true) != 0 && errno == ENOENT;
		const auto after = OrbisSmb::GetStats();
		Check(a && b && after.lookups == before.lookups && after.cache_hits >= before.cache_hits + 2,
			"stat() after a listing asks nothing of the server (size, read-only, and a name that isn't there)");
		errno = 0;
		Check(OrbisSmb::Stat("/smb/127.0.0.1/public/Nothing/here.iso", &st, true) != 0 && errno == ENOENT, "stat() of nothing: ENOENT");
		Check(OrbisSmb::Stat("/smb/127.0.0.1/public/PS2/More", &st, true) == 0 && S_ISDIR(st.st_mode), "stat() of a folder");
		errno = 0;
		Check(OrbisSmb::Stat("/smb/127.0.0.1/nosuch/x", &st, true) != 0 && errno == EIO, "a share that didn't connect: EIO");
	}

	{
		errno = 0;
		const int dir = OrbisSmb::Open((ps2 + "/More").c_str());
		Check(dir < 0 && errno == EISDIR, "a folder doesn't open: EISDIR (" + std::to_string(dir) + ", errno " + std::to_string(errno) + ")");
		errno = 0;
		Check(OrbisSmb::Open((ps2 + "/Missing.iso").c_str()) < 0 && errno == ENOENT, "a missing file: ENOENT");
		const int h = OrbisSmb::Open((ps2 + "/Test Game (USA).iso").c_str());
		Check(h >= 0, "open a game");
		struct stat st;
		Check(OrbisSmb::FStat(h, &st) == 0 && st.st_size == static_cast<off_t>(game.size()), "fstat() its size");
		const double t1 = Now();
		bool all = true;
		std::vector<uint8_t> sector(2048);
		for (uint64_t off = 0; off < game.size() && all; off += 2048)
		{
			const size_t want = std::min<size_t>(2048, game.size() - off);
			all = OrbisSmb::PRead(h, sector.data(), 2048, off) == static_cast<ssize_t>(want) &&
			      std::equal(sector.begin(), sector.begin() + want, game.begin() + off);
		}
		const double took = Now() - t1;
		Check(all, "every 2048-byte sector, in order, the same as the file (" + std::to_string(static_cast<int>(game.size() / 1048576.0 / took)) +
		               " MB/s on loopback)");
		Check(Same(h, game, 65536 - 7, 20) && Same(h, game, 2 * 1048576 - 3, 9) && Same(h, game, 12345678, 70000), "across window edges");
		Check(Same(h, game, 3, 5 * 1048576 + 11), "a read bigger than the window (straight to the caller, several READs at once)");
		Check(Same(h, game, game.size() - 100, 4096) && Same(h, game, game.size(), 10) && Same(h, game, game.size() + 5000, 10),
			"the end: short, then nothing");
		Check(Same(h, game, 17 * 1048576 + 1, 1) && Same(h, game, 0, 1) && Same(h, game, 9 * 1048576 + 4097, 2048), "back and forth");
		std::atomic<int> bad{0};
		std::vector<std::thread> threads;
		for (int t = 0; t < 4; t++)
			threads.emplace_back([&, t]() {
				for (int i = 0; i < 200; i++)
				{
					const uint64_t off = (static_cast<uint64_t>(t) * 7919 + static_cast<uint64_t>(i) * 104729 * 13) % game.size();
					if (!Same(h, game, off, 2048 + static_cast<size_t>(i % 3) * 30000))
						bad++;
				}
			});
		for (std::thread& t : threads)
			t.join();
		Check(bad == 0, "four threads on one file: " + std::to_string(bad.load()) + " bad reads");

		const char* kill = std::getenv("SMB_KILL_CLIENTS");
		if (kill && *kill)
		{
			const auto before = OrbisSmb::GetStats();
			Check(std::system(kill) == 0, "smbd's process for this client killed");
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			Check(Same(h, game, 20 * 1048576 + 5, 300000), "the read after it: the same bytes (connected again, the file reopened)");
			const auto after = OrbisSmb::GetStats();
			Check(after.reconnects == before.reconnects + 1, "one reconnection counted");
			Check(Same(h, game, 1, 2048) && Joined(List(ps2)) == "More/, Test Game (USA).iso, readme.txt", "and the share works on");
		}
		else
			std::printf("    (no SMB_KILL_CLIENTS: the reconnection isn't tested)\n");
		Check(OrbisSmb::Close(h) == 0 && OrbisSmb::Close(h) != 0 && errno == EBADF, "close, and once only");
		errno = 0;
		Check(OrbisSmb::PRead(h, sector.data(), 10, 0) < 0 && errno == EBADF, "a closed handle reads nothing");

		const int p = OrbisSmb::Open("/smb/127.0.0.1/private/PS2/Private Game.iso");
		Check(p >= 0 && Same(p, small, 0, 6000) && OrbisSmb::Close(p) == 0, "a file on the private share, with the password");
	}

	{
		const std::string iso = ps2 + "/Test Game (USA).iso";
		const off_t size = static_cast<off_t>(game.size());
		struct stat st;
		Check(stat(iso.c_str(), &st) == 0 && st.st_size == size && lstat(iso.c_str(), &st) == 0 && S_ISREG(st.st_mode), "stat(), lstat()");
		Check(access(iso.c_str(), R_OK) == 0 && access(iso.c_str(), W_OK) != 0 && errno == EROFS, "access(): readable, not writable");
		errno = 0;
		Check(std::fopen(iso.c_str(), "r+") == nullptr && errno == EROFS, "fopen() for writing: EROFS");
		errno = 0;
		Check(open(iso.c_str(), O_RDWR) < 0 && errno == EROFS, "open() for writing: EROFS");
		FILE* f = std::fopen(iso.c_str(), "rb");
		Check(f != nullptr, "fopen()");
		if (f)
		{
			std::vector<uint8_t> buf(100000);
			const off_t at = 7 * 1048576 + 3;
			Check(fseeko(f, at, SEEK_SET) == 0 && std::fread(buf.data(), 1, buf.size(), f) == buf.size() &&
					  std::equal(buf.begin(), buf.end(), game.begin() + at) && ftello(f) == at + 100000,
				"fseeko(), fread(), ftello()");
			Check(std::fseek(f, -10, SEEK_END) == 0 && std::fread(buf.data(), 1, 100, f) == 10 && std::feof(f) && !std::ferror(f) &&
					  std::equal(buf.begin(), buf.begin() + 10, game.end() - 10),
				"a short fread() at the end, feof()");
			std::rewind(f);
			Check(std::fgetc(f) == game[0] && !std::feof(f), "rewind(), fgetc()");
			struct stat fs;
			Check(fstat(fileno(f), &fs) == 0 && fs.st_size == size, "fstat(fileno())");
			Check(std::fclose(f) == 0, "fclose()");
		}
		const int fd = open(iso.c_str(), O_RDONLY);
		Check(fd >= 0, "open()");
		if (fd >= 0)
		{
			std::vector<uint8_t> buf(2048);
			const bool a = pread(fd, buf.data(), 2048, 5 * 1048576) == 2048 && std::equal(buf.begin(), buf.end(), game.begin() + 5 * 1048576);
			const bool b = lseek(fd, 1000, SEEK_SET) == 1000 && read(fd, buf.data(), 2048) == 2048 &&
			               std::equal(buf.begin(), buf.end(), game.begin() + 1000) && lseek(fd, 0, SEEK_CUR) == 3048;
			Check(a && b, "pread(), lseek(), read()");
			Check(fstat(fd, &st) == 0 && st.st_size == size, "fstat()");
			Check(close(fd) == 0 && close(fd) != 0, "close(), once");
		}
		std::vector<std::string> names;
		if (DIR* d = opendir(ps2.c_str()))
		{
			while (dirent* e = readdir(d))
				names.push_back(e->d_name);
			closedir(d);
		}
		std::sort(names.begin(), names.end());
		Check(Joined(names) == "More, Test Game (USA).iso, readme.txt", "opendir(), readdir(): " + Joined(names));
		char* rp = realpath((ps2 + "/More/../readme.txt").c_str(), nullptr);
		Check(rp && std::string(rp) == ps2 + "/readme.txt", std::string("realpath(): ") + (rp ? rp : "null"));
		std::free(rp);
		const std::string local = pub + "/local.txt";
		FILE* w = std::fopen(local.c_str(), "w");
		const bool wrote = w && std::fputs("local\n", w) >= 0 && std::fclose(w) == 0;
		char line[16] = {};
		FILE* r = std::fopen(local.c_str(), "r");
		const bool read_back = r && std::fgets(line, sizeof(line), r) && std::string(line) == "local\n" && std::fclose(r) == 0;
		Check(wrote && read_back, "a local file is still the C library's");
	}
	{
		OrbisSmb::SetShares("smb://127.0.0.1:9/x", nullptr);
		const double t1 = Now();
		const auto st = OrbisSmb::MountAll(4000);
		const double took = Now() - t1;
		std::printf("    %s: %s (%.2f s)\n", st.empty() ? "?" : st[0].url.c_str(), st.empty() ? "?" : st[0].state.c_str(), took);
		Check(st.size() == 1 && !st[0].mounted && took < 2.0 && st[0].state.find("anonymously") == std::string::npos,
			"a port nobody answers: not connected, said why, the other way not tried");
		const double t2 = Now();
		struct stat s;
		Check(OrbisSmb::Stat("/smb/127.0.0.1/x/y", &s, true) != 0 && Now() - t2 < 0.5, "and isn't tried again at once (no wait)");
	}
	{
		OrbisSmb::SetShares("smb://10.255.255.1/games", nullptr);
		const double t1 = Now();
		const auto st = OrbisSmb::MountAll(2000);
		const double took = Now() - t1;
		std::printf("    %s: %s (%.1f s)\n", st.empty() ? "?" : st[0].url.c_str(), st.empty() ? "?" : st[0].state.c_str(), took);
		Check(st.size() == 1 && !st[0].mounted && took < 5.0, "a server that never answers: given up in " + std::to_string(took) + " s");
	}

	const auto stats = OrbisSmb::GetStats();
	std::printf("    stats: %llu opens, %llu reads, %llu requests, %.1f MB, %llu from listings, %llu lookups, %llu reconnections\n",
		static_cast<unsigned long long>(stats.opens), static_cast<unsigned long long>(stats.reads),
		static_cast<unsigned long long>(stats.requests), static_cast<double>(stats.bytes) / 1048576.0,
		static_cast<unsigned long long>(stats.cache_hits), static_cast<unsigned long long>(stats.lookups),
		static_cast<unsigned long long>(stats.reconnects));
	std::printf("%s: %d of %d checks passed (games on SMB shares)\n", s_fails ? "FAIL" : "PASS", s_checks - s_fails, s_checks);
	return s_fails ? 1 : 0;
}
