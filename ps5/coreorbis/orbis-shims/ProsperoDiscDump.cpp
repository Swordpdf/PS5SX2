// PS5SX2 (vk-285-144, AI-assisted): a PS2 DVD in the PS5's drive, copied to /data/PCSX2/games as an .iso.
//
// swordpdf: "enable disc support, the disc needs to be mounted and dumped to data/pcsx2/games". The PS5's drive reads DVDs
// (it can't read CDs, so PS2 CD games are out), but the system has no use for a PS2 disc and doesn't mount it for an app.
// This thread looks for the drive's device node the kernel gives the jailbroken app, reads the disc's sectors itself and
// writes them to games/<title> (<serial>).iso, which the shelf then lists like any other image.
//
// Not tried on a console: which /dev node is the drive, and whether it gives a PS2 disc's sectors at all, is what the
// first log shows. So it logs a lot, once ([disc] lines in boot.log): every /dev name and
// for each candidate node (a /dev name that looks like an optical drive)
// whether it opens, its size and sector size, and what sector 16 holds (an ISO 9660 volume says "CD001").
//
// Then every 3 s while the app runs: a node whose sector 16 is an ISO 9660 volume with a PS2 SYSTEM.CNF (BOOT2) is
// copied, unless games/ already has that file at full size: 4 MiB reads (vk-285-147; 1 MiB before), each retried; a range that still won't read is
// re-read sector by sector and what stays unreadable is written as zeros and counted. The copy goes to a .part file,
// renamed to .iso at the end. Notifications at the start, every 25% and at the end. Nothing is ever written to the disc
// or a device. Flag nodiscdump: off.
//
// swordpdf: "after its dumped, whenever inserted should start that game with ps5sx2". A PS2 disc put in while the shelf is
// up starts its copy (once the copy is made), as if picked on the shelf; so does one already in the drive when the app is
// opened. Not when the app was re-executed into the shelf (back to the menu, a game that didn't start: main-boot writes
// logs/relaunch.txt), or the disc's game would start again at once. Taking the disc out and putting it back starts it
// again. While a game runs, a new disc is copied but nothing starts.
//
// vk-285-145: 144 crashed at boot on testers' consoles (SYSTEM_ILLEGAL_FUNCTION_CALL right after "PS5SX2: starting"). The list
// of mounted file systems came from libc's getmntinfo, which makes the getfsstat system call from the app's own code, and
// the free-space check from statfs, the same: the PS5 kills an app that makes a system call outside libkernel. Both are
// gone: no list of mounts (the /dev names are enough), free space through libkernel's _fstatfs like the texture packs.
// Also: a /dev name counts as a drive only as an exact name or the name plus a number (cd0, bd0, acd0...), so Sony's own
// nodes (bluetooth_hid, ...) are never opened, and the first look waits 5 s so the app's start never runs into it.
// Needs proper testing on a console with a disc drive.
//
// vk-285-146: the first console log (13.60, NFS Underground 2 in the drive): /dev/cd0 opens, DIOCGMEDIASIZE 2874605568
// (the size of the tester's own .iso of it), sector 16 reads. But "without a PS2 SYSTEM.CNF": fe::ReadSerial read
// SYSTEM.CNF at its own length and the drive reads only whole 2048-byte sectors. fe_games' reader now retries in whole
// sectors. And a disc that games/ already has an image of (same serial, any file name) isn't copied again; that image starts.
//
// vk-285-147 (swordpdf: "for next round we should try 8x dump speed"): the drive is asked for its top read speed
// (CDRIOCREADSPEED, logged whether it takes it), the disc is read 4 MiB at a time, up to 4 reads ahead of the writing
// (OrbisDiscCopy.h), and the log gives the speed as a DVD multiple ("now" over the last 5%, and the average). Flag disc_1m:
// 1 MiB reads, to compare.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProsperoDiscDump.h"
#include "OrbisDiscCopy.h"
#include "ProsperoNotify.h"

#include "OrbisPaths.h"
#include "../../frontend/fe_games.h"
#include "../../frontend/fe_ps5.h"

#include <sys/types.h>
#include <sys/cdrio.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdarg>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
	std::atomic<bool> s_started{false};
	std::atomic<bool> s_busy{false};

	void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
	void Log(const char* fmt, ...)
	{
		char line[1024];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(line, sizeof(line), fmt, ap);
		va_end(ap);
		printf("[disc] %s\n", line);
		fflush(stdout);
	}

	bool LooksOptical(const std::string& n)
	{
		// The name alone or the name plus a number: cd0 yes, bluetooth_hid / bdbus / cdev no.
		static const char* const names[] = {"cd", "bd", "disc", "odd", "dvd", "sbd", "optical", "acd", "scd", "bdrom"};
		for (const char* p : names)
		{
			const size_t len = strlen(p);
			if (n.compare(0, len, p) != 0)
				continue;
			bool digits = true;
			for (size_t i = len; i < n.size(); i++)
				digits = digits && n[i] >= '0' && n[i] <= '9';
			if (digits)
				return true;
		}
		return false;
	}

	uint32_t Le32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

	struct Node
	{
		std::string path;
		uint64_t bytes = 0; // the media's size (DIOCGMEDIASIZE), or the ISO volume's
	};

	// What a node holds: false when it doesn't open or sector 16 isn't an ISO 9660 volume. `log`: say what was found.
	bool ProbeNode(const std::string& path, Node& out, bool log)
	{
		const int fd = open(path.c_str(), O_RDONLY);
		if (fd < 0)
		{
			if (log)
				Log("%s: open: errno %d", path.c_str(), errno);
			return false;
		}
		off_t media = 0;
		u_int sector = 0;
		const int r1 = ioctl(fd, DIOCGMEDIASIZE, &media);
		const int e1 = r1 ? errno : 0;
		const int r2 = ioctl(fd, DIOCGSECTORSIZE, &sector);
		const int e2 = r2 ? errno : 0;
		uint8_t pvd[2048] = {};
		const ssize_t n = pread(fd, pvd, sizeof(pvd), 16 * 2048);
		const int e3 = n < 0 ? errno : 0;
		close(fd);
		const bool iso = n == 2048 && pvd[0] == 1 && memcmp(pvd + 1, "CD001", 5) == 0;
		const uint64_t vol = iso ? static_cast<uint64_t>(Le32(pvd + 80)) * 2048 : 0;
		if (log)
		{
			char label[33] = {};
			if (iso)
				memcpy(label, pvd + 40, 32);
			Log("%s: opens; media size %s %lld, sector size %s %u; sector 16: %s%zd bytes%s%s%s", path.c_str(), r1 ? "errno" : "",
				r1 ? static_cast<long long>(e1) : static_cast<long long>(media), r2 ? "errno" : "", r2 ? static_cast<unsigned>(e2) : sector,
				n < 0 ? "read errno " : "", n < 0 ? static_cast<ssize_t>(e3) : n, iso ? ", ISO 9660 volume \"" : "", iso ? label : "",
				iso ? "\"" : (n == 2048 ? ", not ISO 9660" : ""));
		}
		if (!iso)
			return false;
		out.path = path;
		out.bytes = (r1 == 0 && media > 0) ? static_cast<uint64_t>(media) : vol;
		if (vol && out.bytes > vol)
			out.bytes = vol; // the volume's own size: a drive may report the whole disc's capacity
		return out.bytes > 0;
	}

	std::vector<std::string> Candidates(bool log)
	{
		std::vector<std::string> out;
		std::string all;
		if (DIR* d = opendir("/dev"))
		{
			while (dirent* e = readdir(d))
			{
				const std::string n = e->d_name;
				if (n == "." || n == "..")
					continue;
				all += " " + n;
				if (LooksOptical(n))
					out.push_back("/dev/" + n);
			}
			closedir(d);
		}
		// vk-285-145: no getmntinfo here (a system call from the app's own code: the PS5 kills the app).
		if (log)
		{
			Log("/dev:%s", all.c_str());
			std::string c;
			for (const std::string& p : out)
				c += " " + p;
			Log("candidate drive nodes:%s", c.empty() ? " none" : c.c_str());
		}
		return out;
	}

	std::string Clean(std::string s)
	{
		for (char& ch : s)
			if (strchr("/\\:*?\"<>|", ch) || static_cast<unsigned char>(ch) < 0x20)
				ch = ' ';
		while (!s.empty() && (s.back() == ' ' || s.back() == '.'))
			s.pop_back();
		return s;
	}

	// vk-285-146: an image of this disc already in games/ under any name (the tester's NFS Underground 2 was there as
	// "SLUS_210.65.Need for Speed Underground 2.iso"): same serial, at least 90% of the disc's size (dumps differ in
	// padding). Only .iso files of about the right size are opened, so the scan stays cheap.
	std::string FindExisting(const std::string& games, const std::string& serial, uint64_t bytes)
	{
		DIR* d = opendir(games.c_str());
		if (!d)
			return {};
		std::string found;
		while (dirent* e = readdir(d))
		{
			const std::string n = e->d_name;
			if (n.size() < 5)
				continue;
			std::string ext = n.substr(n.size() - 4);
			for (char& c : ext)
				c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			if (ext != ".iso")
				continue;
			const std::string path = games + "/" + n;
			struct stat st;
			if (stat(path.c_str(), &st) != 0 || static_cast<uint64_t>(st.st_size) < bytes / 10 * 9 ||
				static_cast<uint64_t>(st.st_size) > bytes + (64ull << 20))
				continue;
			if (fe::ReadSerial(path) == serial)
			{
				found = path;
				break;
			}
		}
		closedir(d);
		return found;
	}

	// Copies the disc unless games/ has it. The copy's path when it's there in full at the end, else empty.
	std::string Dump(const Node& node, const std::string& serial)
	{
		fe::GameInfo gi;
		gi.serial = serial;
		gi.title = serial;
		fe::ApplyGameDbTitle(gi);
		const std::string title = gi.title.empty() || gi.title == serial ? std::string("PS2 disc") : gi.title;
		const std::string games = OrbisDir("games");
		mkdir(games.c_str(), 0777);
		const std::string name = Clean(title + " (" + serial + ")");
		const std::string iso = games + "/" + name + ".iso", part = iso + ".part";
		struct stat st;
		if (stat(iso.c_str(), &st) == 0 && static_cast<uint64_t>(st.st_size) == node.bytes)
		{
			static std::string s_said;
			if (s_said != iso)
			{
				s_said = iso;
				Log("%s is already copied (%s)", serial.c_str(), iso.c_str());
			}
			return iso;
		}
		const std::string existing = FindExisting(games, serial, node.bytes);
		if (!existing.empty())
		{
			static std::string s_said;
			if (s_said != existing)
			{
				s_said = existing;
				Log("%s is already in games/ as %s: not copied again", serial.c_str(), existing.c_str());
			}
			return existing;
		}
		// vk-285-145: libkernel's _fstatfs (fe_ps5), never libc's statfs. UINT64_MAX: not known, copied anyway.
		const uint64_t free_bytes = orbis_frontend_free_bytes(games);
		if (free_bytes != UINT64_MAX)
		{
			if (free_bytes < node.bytes + (256ull << 20))
			{
				Log("%s: %llu MB needed, %llu MB free: not copied", serial.c_str(), static_cast<unsigned long long>(node.bytes >> 20),
					static_cast<unsigned long long>(free_bytes >> 20));
				char msg[200];
				snprintf(msg, sizeof(msg), "PS2 disc %s: not enough space to copy it (%llu MB needed)", title.c_str(),
					static_cast<unsigned long long>(node.bytes >> 20));
				OrbisNotifyPlain(msg);
				sleep(60);
				return {};
			}
		}
		const int in = open(node.path.c_str(), O_RDONLY);
		const int out = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (in < 0 || out < 0)
		{
			Log("%s: open failed (drive %d, file %d, errno %d)", serial.c_str(), in, out, errno);
			if (in >= 0) close(in);
			if (out >= 0) close(out);
			return {};
		}
		Log("copying %s (%s, %llu MB) from %s to %s", serial.c_str(), title.c_str(), static_cast<unsigned long long>(node.bytes >> 20),
			node.path.c_str(), iso.c_str());
		{
			char msg[200];
			snprintf(msg, sizeof(msg), "Copying PS2 disc: %s (%llu MB)", title.c_str(), static_cast<unsigned long long>(node.bytes >> 20));
			OrbisNotifyPlain(msg);
		}
		// vk-285-147 (swordpdf: "try 8x dump speed"): ask the drive for its top read speed (FreeBSD cd(4)'s
		// CDRIOCREADSPEED, through libkernel's ioctl; CDR_MAX_SPEED is "as fast as it goes"). The PS5's drive may not
		// take it: the log says. Then reads run ahead of the writes (OrbisDiscCopy.h).
		{
			int speed = CDR_MAX_SPEED;
			const int rc = ioctl(in, CDRIOCREADSPEED, &speed);
			Log("read speed: CDRIOCREADSPEED max -> %s%d", rc ? "errno " : "rc ", rc ? errno : rc);
		}
		const size_t chunk = static_cast<size_t>(OrbisFlag("disc_1m") ? 1 : 4) << 20;
		const int depth = 4;
		int last_pct = -1, last_note = 0;
		const auto t0 = std::chrono::steady_clock::now();
		auto rate = [&](uint64_t done) {
			const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			return secs > 0 ? done / secs : 0.0;
		};
		// the speed over the last 5% (a CAV drive speeds up toward the disc's edge)
		uint64_t mark_done = 0;
		auto mark_time = t0;
		Log("reading %zu MiB at a time, %d ahead", chunk >> 20, depth);
		const orbis_disc::CopyResult res = orbis_disc::Copy(
			node.bytes, chunk, depth, 3, 4096,
			[&](uint64_t off, void* buf, size_t len) { return pread(in, buf, len, static_cast<off_t>(off)); },
			[&](const void* buf, size_t len) { return write(out, buf, len) == static_cast<ssize_t>(len); },
			[&](uint64_t done, uint64_t bad) {
				const int pct = static_cast<int>(done * 100 / node.bytes);
				if (pct / 5 == last_pct / 5)
					return;
				last_pct = pct;
				const auto now = std::chrono::steady_clock::now();
				const double span = std::chrono::duration<double>(now - mark_time).count();
				const double recent = span > 0 ? (done - mark_done) / span : 0.0;
				mark_done = done;
				mark_time = now;
				const double avg = rate(done);
				Log("%d%% (%llu of %llu MB, %.1f MB/s = %.1fx now, %.1fx average, %llu unreadable sectors)", pct,
					static_cast<unsigned long long>(done >> 20), static_cast<unsigned long long>(node.bytes >> 20), recent / 1e6,
					recent / orbis_disc::kDvd1x, avg / orbis_disc::kDvd1x, static_cast<unsigned long long>(bad));
				if (pct >= last_note + 25 && pct < 100)
				{
					last_note = pct - pct % 25;
					char msg[160];
					snprintf(msg, sizeof(msg), "Copying %s: %d%% (%.1fx)", title.c_str(), last_note, recent / orbis_disc::kDvd1x);
					OrbisNotifyPlain(msg);
				}
			},
			[&](uint64_t sector) {
				static int s_said = 0;
				if (++s_said <= 20)
					Log("sector %llu unreadable (errno %d): zeros", static_cast<unsigned long long>(sector), errno);
			});
		const uint64_t done = res.done, bad_sectors = res.bad_sectors;
		const bool failed = res.failed;
		if (failed)
			Log("copy stopped: %s (errno %d)", res.why.c_str(), errno);
		close(in);
		fsync(out);
		close(out);
		if (failed)
		{
			OrbisNotifyPlain("PS2 disc copy failed: see the log (Download logs)");
			unlink(part.c_str());
			sleep(30);
			return {};
		}
		rename(part.c_str(), iso.c_str());
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		Log("done: %s, %llu MB in %.0f s (%.1f MB/s, %.1fx average), %llu unreadable sectors", iso.c_str(),
			static_cast<unsigned long long>(done >> 20), secs, secs > 0 ? done / secs / 1e6 : 0.0,
			secs > 0 ? done / secs / orbis_disc::kDvd1x : 0.0, static_cast<unsigned long long>(bad_sectors));
		char msg[220];
		snprintf(msg, sizeof(msg), bad_sectors ? "Copied %s, but %llu sectors couldn't be read: the game may not work" :
		                                         "Copied %s", title.c_str(), static_cast<unsigned long long>(bad_sectors));
		OrbisNotifyPlain(msg);
		return iso;
	}

	// The start was a re-exec into the shelf (logs/relaunch.txt from the last minutes), not the user opening the app.
	bool TakeRelaunchMark()
	{
		const std::string path = OrbisLogPath("relaunch.txt");
		struct stat st;
		if (stat(path.c_str(), &st) != 0)
			return false;
		unlink(path.c_str());
		return time(nullptr) - st.st_mtime < 300;
	}

	void* Thread(void*)
	{
		bool first = true;
		std::vector<std::string> logged; // nodes already described
		// The disc whose game was started (or that was in the drive at a re-exec): not started again until it leaves.
		std::string handled;
		std::string pending; // a copy whose game waits for the shelf
		const bool relaunch = TakeRelaunchMark();
		sleep(5); // vk-285-145: well after the app's start
		if (relaunch)
			Log("a re-exec into the shelf: a disc in the drive now doesn't start its game");
		for (;;)
		{
			const std::vector<std::string> nodes = Candidates(first);
			bool any_ps2 = false;
			for (const std::string& path : nodes)
			{
				const bool log = std::find(logged.begin(), logged.end(), path) == logged.end();
				Node node;
				const bool iso = ProbeNode(path, node, log);
				if (log)
					logged.push_back(path);
				if (!iso)
					continue;
				const std::string serial = fe::ReadSerial(path);
				if (serial.empty())
				{
					static std::string s_said;
					if (s_said != path)
					{
						s_said = path;
						Log("%s: an ISO 9660 disc without a PS2 SYSTEM.CNF: not a PS2 game", path.c_str());
					}
					continue;
				}
				any_ps2 = true;
				if (first && relaunch)
					handled = serial;
				s_busy.store(true);
				const std::string copy = Dump(node, serial);
				s_busy.store(false);
				if (copy.empty() || handled == serial)
					continue;
				handled = serial;
				pending = copy; // started when the shelf takes it (it may not be up yet)
				Log("%s: its game starts when the shelf is up (%s)", serial.c_str(), copy.c_str());
			}
			if (!pending.empty() && orbis_frontend_request_launch(pending))
			{
				Log("starting %s", pending.c_str());
				OrbisNotifyPlain("PS2 disc: starting the game");
				pending.clear();
			}
			if (!any_ps2 && !handled.empty())
			{
				Log("the disc is out: %s starts again when it's put back", handled.c_str());
				handled.clear();
				pending.clear();
			}
			// a node that went away and came back (a disc swapped) is described again
			if (logged.size() > 32)
				logged.clear();
			first = false;
			sleep(3);
		}
		return nullptr;
	}
} // namespace

void OrbisDiscDumpStart()
{
	if (s_started.exchange(true))
		return;
	if (OrbisFlag("nodiscdump"))
	{
		Log("off (flag nodiscdump)");
		return;
	}
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 512 * 1024);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	const int rc = pthread_create(&t, &attr, Thread, nullptr);
	pthread_attr_destroy(&attr);
	if (rc != 0)
		Log("thread: pthread_create %d", rc);
}

bool OrbisDiscDumpBusy()
{
	return s_busy.load();
}
