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
// vk-285-148: the 146 copy (NFS Underground 2, 2741 MB in 1037 s) ran at 2.00x from the first 5% to the last: a drive
// left to itself speeds up toward the edge, so the drive is held at 2x (likely the speed Sony sets for films). 147's
// request for its top speed is what can change that. Logged now: ms per read and how much of the time a read was running
// ("drive busy"), so a held drive shows as busy near 100% at a flat speed; an 8x request in kB/s when "max" is refused; the
// /dev list in pieces (one log line stops at 1024 characters, so 146's list lost its end). File names: ": " becomes " - ".
//
// vk-285-149: a notification every 5% (it was every 25%) with the speed over the last 5% and the drive's busy share.
// RPCS3To5's PS3 disc dumper (a payload, not a title) says a title's file writes are throttled after about 1.3 GiB: "drive
// busy" well under 100% would show the copy waiting on writes rather than on the drive.
//
// vk-285-150: 149 on the console: CDRIOCREADSPEED EINVAL as max and as 8x, the copy at a flat 2.0x with the drive busy
// 100%: the drive's own pace. Now each copy first asks the drive through its pass device (ProsperoDiscSpeed.cpp: SET
// STREAMING, the DVD way, after what Sony's SceShellCore does with libcam). Flag disc_readtest: a disc already copied is
// read for a speed test (256 MB at its start and its end). Flag nodiscspeed: no drive commands.
//
// vk-285-151: 150 on the console: pass0 works and the drive (SONY PS-SYSTEM 503R, firmware 2305) reports 3.2x..8.0x, but
// takes neither SET CD SPEED nor SET STREAMING (invalid command), and the copy stays at 2.0x, below its own 3.2x: so each
// copy now first times ways of reading (pread on cd0, READ(10)/READ(12) through pass0, sizes, two lanes) on 16 MiB each
// and copies with the fastest (ProsperoDiscSpeed.cpp).
//
// vk-285-152: Sony's own speed command from SceShellCore (DB <rotation> <speed>; ProsperoDiscSpeed.cpp): each copy times
// the drive's faster settings from Sony's table, copies at the fastest, and puts the drive back to 2.0 afterwards.
// vk-285-153/154: for a PS2 DVD swordpdf's drive takes rotation 0 / 0x32 (3.2x, 4.43 MB/s) and nothing faster; 8.0
// (rotation 1) is tried first for drives that take it, then 3.2.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProsperoDiscDump.h"
#include "ProsperoDiscSpeed.h"
#include "OrbisDiscCopy.h"
#include "ProsperoNotify.h"

#include "OrbisPaths.h"
#include "../../frontend/fe_games.h"
#include "../../frontend/fe_ps5.h"

#include <sys/types.h>
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
		// vk-285-155 (AI-assisted): copy the media's real size when the drive gives it (on the PS5's drive DIOCGMEDIASIZE
		// is the exact disc size: NFSU2 read 2874605568, its ISO to the byte). The old code clamped down to the ISO 9660
		// volume size, but on a dual-layer PS2 DVD that volume can describe layer 0 only, so the copy stopped at the layer
		// break and the short .iso hung its game reading past it -- a black screen. The volume size is only a fallback now.
		out.bytes = (r1 == 0 && media > 0) ? static_cast<uint64_t>(media) : vol;
		if (log && vol && out.bytes != vol)
			Log("%s: media size %llu, ISO 9660 volume %llu: copying the media size", path.c_str(),
				static_cast<unsigned long long>(out.bytes), static_cast<unsigned long long>(vol));
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
			// vk-285-148: in pieces (one line stops at 1024 characters: the 146 log lost the end of the list)
			for (size_t at = 0; at < all.size();)
			{
				size_t end = std::min(all.size(), at + 900);
				if (end < all.size())
				{
					const size_t sp = all.rfind(' ', end);
					if (sp != std::string::npos && sp > at)
						end = sp;
				}
				Log("/dev:%s", all.substr(at, end - at).c_str());
				at = end;
			}
			std::string c;
			for (const std::string& p : out)
				c += " " + p;
			Log("candidate drive nodes:%s", c.empty() ? " none" : c.c_str());
		}
		return out;
	}

	std::string Clean(std::string s)
	{
		// vk-285-148: "Need for Speed: Underground 2" was "Need for Speed  Underground 2"; now "... - Underground 2"
		for (size_t at; (at = s.find(": ")) != std::string::npos;)
			s.replace(at, 2, " - ");
		for (char& ch : s)
			if (strchr("/\\:*?\"<>|", ch) || static_cast<unsigned char>(ch) < 0x20)
				ch = ' ';
		for (size_t at; (at = s.find("  ")) != std::string::npos;)
			s.erase(at, 1);
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

	// vk-285-150: flag disc_readtest: for a disc that's already copied, ask the drive for speed as a copy would and time
	// 256 MB at its start and its end (a drive that spins freely reads the end faster), once per disc.
	void ReadTestOnce(const Node& node, const std::string& serial)
	{
		if (!OrbisFlag("disc_readtest"))
			return;
		Log("%s: read test (flag disc_readtest)", serial.c_str());
		OrbisNotifyPlain("Disc read test: about 3 minutes at 2x");
		s_busy.store(true);
		const int fd = open(node.path.c_str(), O_RDONLY);
		if (fd < 0)
		{
			s_busy.store(false);
			return;
		}
		OrbisDiscReadMethod rm;
		rm.cd = fd;
		OrbisDiscSpeedUp(node.path.c_str(), node.bytes, &rm);
		OrbisDiscReadTest(rm, node.bytes);
		OrbisDiscSpeedRestore(node.path.c_str(), rm);
		OrbisDiscReadMethodClose(rm);
		close(fd);
		s_busy.store(false);
	}

	// Copies the disc unless games/ has it. The copy's path when it's there in full at the end, else empty. vk-285-155:
	// `clean` is set false when the copy had unreadable sectors (the caller then doesn't auto-start it).
	std::string Dump(const Node& node, const std::string& serial, bool& clean)
	{
		clean = true;
		fe::GameInfo gi;
		gi.serial = serial;
		gi.title = serial;
		fe::ApplyGameDbTitle(gi);
		const std::string title = gi.title.empty() || gi.title == serial ? std::string("PS2 disc") : gi.title;
		const std::string games = OrbisDir("games");
		mkdir(games.c_str(), 0777);
		const std::string name = Clean(title + " (" + serial + ")");
		const std::string iso = games + "/" + name + ".iso", part = iso + ".part";
		// vk-285-155: an ".incomplete" marker beside a copy that was made with unreadable sectors, so it stays off auto-start
		// across sessions (the caller's `clean` goes false), not only in the session that made it.
		auto has_holes = [](const std::string& image) { struct stat s; return stat((image + ".incomplete").c_str(), &s) == 0; };
		struct stat st;
		if (stat(iso.c_str(), &st) == 0 && static_cast<uint64_t>(st.st_size) == node.bytes)
		{
			clean = !has_holes(iso);
			static std::string s_said;
			if (s_said != iso)
			{
				s_said = iso;
				Log("%s is already copied (%s)%s", serial.c_str(), iso.c_str(), clean ? "" : " (with unreadable sectors: not auto-started)");
				ReadTestOnce(node, serial);
			}
			return iso;
		}
		const std::string existing = FindExisting(games, serial, node.bytes);
		if (!existing.empty())
		{
			clean = !has_holes(existing);
			static std::string s_said;
			if (s_said != existing)
			{
				s_said = existing;
				Log("%s is already in games/ as %s: not copied again%s", serial.c_str(), existing.c_str(),
					clean ? "" : " (with unreadable sectors: not auto-started)");
				ReadTestOnce(node, serial);
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
		// vk-285-150: the drive asked for its fastest read through its pass device (SET STREAMING; ProsperoDiscSpeed.cpp).
		// vk-285-147/148's CDRIOCREADSPEED came back EINVAL on the console: it sends SET CD SPEED, a CD command.
		// vk-285-151: and the fastest way to read it (pread on cd0, or READ commands through pass0; ProsperoDiscSpeed.cpp)
		OrbisDiscReadMethod rm;
		rm.cd = in;
		OrbisDiscSpeedUp(node.path.c_str(), node.bytes, &rm);
		Log("reading with %s", rm.name.c_str());
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
		// vk-285-148: how long the drive takes per read, and how much of the time a read was running ("drive busy"):
		// busy near 100% at a steady speed is the drive's own limit; well under it, the copy loop is waiting on something.
		std::atomic<uint64_t> read_us{0}, read_n{0};
		uint64_t mark_read_us = 0, mark_read_n = 0;
		Log("reading %zu MiB at a time, %d ahead", chunk >> 20, depth);
		const orbis_disc::CopyResult res = orbis_disc::Copy(
			node.bytes, chunk, depth, 3, 4096,
			[&](uint64_t off, void* buf, size_t len) {
				const auto a = std::chrono::steady_clock::now();
				const ssize_t r = OrbisDiscRead(rm, off, buf, len);
				read_us.fetch_add(static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - a).count()));
				read_n.fetch_add(1);
				return r;
			},
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
				const uint64_t rus = read_us.load(), rn = read_n.load();
				const double busy = span > 0 ? (rus - mark_read_us) / 1e6 / span * 100.0 : 0.0;
				const double per = rn > mark_read_n ? (rus - mark_read_us) / 1000.0 / (rn - mark_read_n) : 0.0;
				mark_read_us = rus;
				mark_read_n = rn;
				const double avg = rate(done);
				Log("%d%% (%llu of %llu MB, %.1f MB/s = %.1fx now, %.1fx average; %.0f ms per read, drive busy %.0f%%; %llu unreadable sectors)",
					pct, static_cast<unsigned long long>(done >> 20), static_cast<unsigned long long>(node.bytes >> 20), recent / 1e6,
					recent / orbis_disc::kDvd1x, avg / orbis_disc::kDvd1x, per, busy, static_cast<unsigned long long>(bad));
				// vk-285-149 (swordpdf: "need more frequent popups with percentages every 5% or so and also the momentary
				// read speed"): a notification every 5%, with the speed over the last 5% and how busy the drive was
				if (pct >= last_note + 5 && pct < 100)
				{
					last_note = pct - pct % 5;
					char msg[200];
					snprintf(msg, sizeof(msg), "Copying %s: %d%% | %.1f MB/s (%.1fx) | drive busy %.0f%%", title.c_str(), last_note,
						recent / 1e6, recent / orbis_disc::kDvd1x, busy);
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
		OrbisDiscSpeedRestore(node.path.c_str(), rm);
		OrbisDiscReadMethodClose(rm);
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
		clean = bad_sectors == 0; // vk-285-155: a copy with holes isn't auto-started
		// vk-285-155: remember the holes beside the file, so it stays off auto-start on later opens too.
		const std::string mark = iso + ".incomplete";
		if (clean)
			unlink(mark.c_str());
		else if (FILE* f = fopen(mark.c_str(), "w"))
		{
			fprintf(f, "%llu unreadable sectors\n", static_cast<unsigned long long>(bad_sectors));
			fclose(f);
		}
		return iso;
	}

	// vk-285-159 (AI-assisted, test build): when a PS2 disc goes in, the system shell (SceShellCore) takes the user to the
	// home screen with "disc not supported" -- it has no concept of a PS2 disc. The dump thread keeps the disc and copies it
	// in the background, but the user has been bounced out of PS5SX2, which kills the flow. There's no way to stop the shell's
	// dialog from our own app, so instead this re-executes our own eboot (the same sceSystemServiceLoadExec the menu uses) to
	// bring PS5SX2 back to the front. A marker file (disc-refg.txt) stops it looping: after the re-exec the disc is still in,
	// and we must not bounce a second time. Opt-in for this test: flag disc_refg. What the console must tell us: does the app
	// come back to the front at all, does the shell dialog still flash or need an OK press, and (from the log timestamps) did
	// the watcher keep running while the user was at the home screen or was the app suspended. Needs proper testing.
	extern "C" int sceSystemServiceLoadExec(const char* path, const char* argv[]);
	std::string ReForegroundMarkPath() { return OrbisLogPath("disc-refg.txt"); }
	void ClearReForegroundMark() { unlink(ReForegroundMarkPath().c_str()); }
	// Returns true when LoadExec took (the process is being replaced, so we won't really return then).
	bool TryReForeground(const std::string& serial)
	{
		if (!OrbisFlag("disc_refg"))
			return false;
		// Our own re-exec left this marker; on the fresh start the disc is still in, so don't bounce again.
		if (FILE* f = fopen(ReForegroundMarkPath().c_str(), "r"))
		{
			char s[64] = {};
			long long when = 0;
			const int got = fscanf(f, "%63s %lld", s, &when);
			fclose(f);
			if (got == 2 && time(nullptr) - static_cast<time_t>(when) < 1800)
			{
				Log("re-foreground: already bounced for %s at %lld, staying put (flag disc_refg)", s, static_cast<long long>(when));
				return false;
			}
		}
		const long long seen = static_cast<long long>(time(nullptr));
		// vk-285-159b: the console showed the bounce winning the race -- we re-foreground before the shell has finished taking
		// the user to the home screen, so the shell's takeover lands last and we're back at the XMB. So wait first, let the
		// shell put up its "not supported" dialog, then bounce so OUR app is the last thing on screen. Tunable without a
		// rebuild: the number of seconds is the content of the flags/disc_refg file (e.g. echo 6 > .../flags/disc_refg);
		// empty or unparsable means the default.
		int delay = 5; // vk-285-159d: 3 s cut it close on the console, so +2 s headroom (still tunable via the flag file)
		if (FILE* f = fopen(OrbisFlagPath("disc_refg").c_str(), "r"))
		{
			int v = 0;
			if (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 60)
				delay = v;
			fclose(f);
		}
		Log("re-foreground: PS2 disc %s seen at %lld; waiting %d s for the shell, then re-executing our eboot to come back to the front (flag disc_refg)",
			serial.c_str(), seen, delay);
		for (int i = 0; i < delay; i++)
			sleep(1);
		const long long now = static_cast<long long>(time(nullptr));
		if (FILE* f = fopen(ReForegroundMarkPath().c_str(), "w"))
		{
			fprintf(f, "%s %lld\n", serial.c_str(), now);
			fclose(f);
		}
		OrbisNotifyPlain("PS2 disc: bringing PS5SX2 back to the front");
		const char* path = "/data/homebrew/PPSA99203/eboot.bin";
		struct stat st{};
		if (stat(path, &st) != 0)
			path = "/app0/eboot.bin";
		fflush(stdout);
		const int rc = sceSystemServiceLoadExec(path, nullptr);
		Log("re-foreground: LoadExec(%s) returned 0x%08x%s at %lld", path, static_cast<unsigned>(rc),
			rc == 0 ? " (the app should be restarting)" : " (stayed; carrying on in the background)", static_cast<long long>(time(nullptr)));
		fflush(stdout);
		if (rc == 0)
			for (int i = 0; i < 100; i++)
				usleep(100000); // up to 10 s for the system to replace the process
		return rc == 0;
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

	// vk-285-155 (AI-assisted): the serial of the disc whose game was auto-started last, and when. If the app is opened
	// again with the same disc still in before kAutoStartGuardSeconds have passed, its game is not auto-started again: the
	// last launch most likely failed (a bad dump, a black screen), and a force-off then a full reboot and reopen would
	// otherwise loop. The window is wide enough to cover a cold power-cycle and re-jailbreak. The user can still pick the
	// game from the shelf; after that long it auto-starts again.
	constexpr int kAutoStartGuardSeconds = 900;
	std::string AutoStartMarkPath() { return OrbisLogPath("disc-autostart.txt"); }
	void WriteAutoStartMark(const std::string& serial)
	{
		if (FILE* f = fopen(AutoStartMarkPath().c_str(), "w"))
		{
			fprintf(f, "%s %lld\n", serial.c_str(), static_cast<long long>(time(nullptr)));
			fclose(f);
		}
	}
	// The serial auto-started in the last `within` seconds, or empty.
	std::string RecentAutoStart(int within)
	{
		FILE* f = fopen(AutoStartMarkPath().c_str(), "r");
		if (!f)
			return {};
		char serial[64] = {};
		long long when = 0;
		const int got = fscanf(f, "%63s %lld", serial, &when);
		fclose(f);
		if (got != 2 || time(nullptr) - static_cast<time_t>(when) >= within || time(nullptr) < static_cast<time_t>(when))
			return {};
		return serial;
	}

	void* Thread(void*)
	{
		bool first = true;
		std::vector<std::string> logged; // nodes already described
		// The disc whose game was started (or that was in the drive at a re-exec): not started again until it leaves.
		std::string handled;
		std::string failed;  // vk-285-155: a disc that didn't copy; not tried again until it leaves (no 3 s retry loop)
		std::string pending; // a copy whose game waits for the shelf
		bool refg_done = false; // vk-285-159: one re-foreground attempt per boot, whatever the outcome (flag disc_refg)
		const bool relaunch = TakeRelaunchMark();
		const std::string recent = RecentAutoStart(kAutoStartGuardSeconds); // vk-285-155: a disc auto-started recently (a likely failed launch)
		sleep(5); // vk-285-145: well after the app's start
		if (relaunch)
			Log("a re-exec into the shelf: a disc in the drive now doesn't start its game");
		if (!recent.empty())
			Log("%s was auto-started moments ago: if it's still in, its game isn't auto-started again (pick it from the shelf)", recent.c_str());
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
				// vk-285-155: at the first look, a re-exec or a just-auto-started disc counts as handled (don't auto-start).
				if (first && (relaunch || serial == recent))
					handled = serial;
				if (serial == failed)
					continue; // vk-285-155: already tried and it didn't copy; wait for the disc to be taken out
				// vk-285-159 (test): bring PS5SX2 back to the front after the shell sent the user to the home screen. Once per
				// boot, only for a genuinely fresh disc (not a menu re-exec, not one just auto-started or already handled).
				if (!refg_done && !relaunch && serial != recent && serial != handled)
				{
					refg_done = true;
					TryReForeground(serial); // under flag disc_refg this re-execs and doesn't return
				}
				bool clean = true;
				s_busy.store(true);
				const std::string copy = Dump(node, serial, clean);
				s_busy.store(false);
				if (copy.empty())
				{
					// vk-285-155: don't re-read the disc and rewrite gigabytes every 3 s; wait for it to leave.
					failed = serial;
					Log("%s: not copied; left until the disc is taken out and put back", serial.c_str());
					continue;
				}
				if (handled == serial)
					continue;
				handled = serial;
				if (!clean)
				{
					// vk-285-155: a copy with unreadable sectors may hang its game, so don't auto-start it; it's on the shelf.
					Log("%s: copied with unreadable sectors; not auto-started (pick it from the shelf to try it)", serial.c_str());
					continue;
				}
				pending = copy; // started when the shelf takes it (it may not be up yet)
				Log("%s: its game starts when the shelf is up (%s)", serial.c_str(), copy.c_str());
			}
			if (!pending.empty() && orbis_frontend_request_launch(pending))
			{
				Log("starting %s", pending.c_str());
				OrbisNotifyPlain("PS2 disc: starting the game");
				WriteAutoStartMark(handled); // vk-285-155: so a force-off then reopen doesn't loop this game
				pending.clear();
			}
			if (!any_ps2 && (!handled.empty() || !failed.empty()))
			{
				if (!handled.empty())
					Log("the disc is out: %s starts again when it's put back", handled.c_str());
				handled.clear();
				failed.clear(); // vk-285-155: a disc that failed is tried again once it's reinserted
				pending.clear();
				ClearReForegroundMark(); // vk-285-159: a re-inserted disc bounces us to the front again (flag disc_refg)
				refg_done = false;
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
