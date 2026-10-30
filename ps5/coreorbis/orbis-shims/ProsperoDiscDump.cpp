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
#include "OrbisDiscEject.h"
#include "OrbisDiscPlaying.h"
#include "OrbisDiscExec.h" // 2.11 (dumpxmb): when the disc helper's exec signal is honoured
#include "OrbisDiscResume.h"
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
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
	std::atomic<bool> s_started{false};
	std::atomic<bool> s_busy{false};

	// PR #34 port (AI-assisted): the drive a game plays from (OrbisDiscDumpClaim) and the drive being copied now, under
	// one lock so that a claim and the start of a copy can't cross.
	std::mutex s_drive_mutex;
	std::string s_claimed, s_copying;
	bool Claimed(const std::string& path)
	{
		std::lock_guard<std::mutex> lock(s_drive_mutex);
		return path == s_claimed;
	}
	// 2.11 (disc lag round): logs/disc-playing.txt, "<unix time> <device> play\n", for the disc helper (the payload that runs beside the app and polls the
	// drives): it leaves the drive a game plays from alone while the stamp is fresh (a few seconds old at most), so its own commands to the drive (and a copy
	// of a USB disc) don't come in the middle of the game's reads. Written when a drive is claimed and every 2 s by the alive thread while it is, removed when
	// it is released (and any left by an earlier run when this one starts). Atomic: written to a temporary file and renamed over it, so the helper never reads
	// half a line. Called with s_drive_mutex held.
	void StampPlayingLocked()
	{
		const std::string path = OrbisLogPath("disc-playing.txt");
		if (s_claimed.empty())
		{
			unlink(path.c_str());
			return;
		}
		orbis_disc::WritePlayingStamp(path, static_cast<long long>(time(nullptr)), s_claimed); // (OrbisDiscPlaying.h: tmp + rename)
	}
	bool BeginCopy(const std::string& path)
	{
		std::lock_guard<std::mutex> lock(s_drive_mutex);
		if (path == s_claimed)
			return false;
		s_copying = path;
		return true;
	}
	void EndCopy()
	{
		std::lock_guard<std::mutex> lock(s_drive_mutex);
		s_copying.clear();
	}

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

	// 2.11 (dumpxmb): state for the disc helper's exec signal and for the timeline after a copy (see OrbisDiscExec.h).
	std::atomic<bool> s_game_starting{false}; // a game is being started (or runs) in this process
	std::atomic<bool> s_tile_process{false};  // main-boot: this process is a disc tile
	std::atomic<long long> s_game_starting_ms{0}, s_copy_end_ms{0}; // steady clock in ms; 0 = never
	long long NowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	void NoteGameStarting(const char* how)
	{
		if (!s_game_starting.exchange(true))
		{
			s_game_starting_ms.store(NowMs());
			Log("a game is being started (%s): the disc helper's disc-launch-exec.txt is ignored from now on (flag discexec: honoured as before)", how);
		}
	}
	std::string AgeOf(const char* name) // how old a file in logs/ is ("-" when it is not there)
	{
		struct stat st{};
		if (stat(OrbisLogPath(name).c_str(), &st) != 0)
			return "-";
		return std::to_string(static_cast<long long>(time(nullptr)) - static_cast<long long>(st.st_mtime)) + "s";
	}
	void LogSignalFiles(const std::string& what) // `what` is the line's lead, e.g. "at start" or "endcopy +3012 ms: media /dev/cd0=2874605568"
	{
		Log("%s: signal files: launch %s exec %s autostart %s refg %s relaunch %s; stamps: app %s helper %s; game starting %d, tile %d", what.c_str(),
			AgeOf("disc-launch.txt").c_str(), AgeOf("disc-launch-exec.txt").c_str(), AgeOf("disc-autostart.txt").c_str(), AgeOf("disc-refg.txt").c_str(),
			AgeOf("relaunch.txt").c_str(), AgeOf("ps5sx2-alive.txt").c_str(), AgeOf("disc-auto-alive.txt").c_str(), s_game_starting.load() ? 1 : 0,
			s_tile_process.load() ? 1 : 0);
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

	// vk-285-160l (AI-assisted): swordpdf: "after the disc is dumped, the drive is making a repetitive noise, like trying
	// to start to read". It stopped when PS5SX2 was closed with the disc-auto daemon still polling (open + DIOCGMEDIASIZE
	// every second): the noise was this watcher reading sector 16 and SYSTEM.CNF every 3 s, waking the drive each time it
	// tried to spin down. So a node's probe is kept and only redone when its media size changes (a different disc, or
	// none); the size query doesn't make the drive read. Needs proper testing on a console with a disc drive.
	struct ProbeCache
	{
		uint64_t media = 0; // DIOCGMEDIASIZE when probed
		bool iso = false;
		Node node;
		std::string serial;
	};

	// The media size without reading the disc; 0 when it doesn't open or holds nothing.
	uint64_t QuietMediaSize(const std::string& path)
	{
		const int fd = open(path.c_str(), O_RDONLY);
		if (fd < 0)
			return 0;
		off_t media = 0;
		const int r = ioctl(fd, DIOCGMEDIASIZE, &media);
		close(fd);
		return (r == 0 && media > 0) ? static_cast<uint64_t>(media) : 0;
	}

	// 2.11 (dumpxmb): for a minute after a copy ended, one line per pass of the watcher (every 1-3 s): what the app and the disc helper are doing, seen from
	// the app's side, WITHOUT asking the drive anything (the media sizes are the ones this pass already read). The next tester log then shows whether the
	// disc went away from the app after the copy (media 0, or another size), which signal files were there, and how old they were.
	void EndCopyTimeline(const std::map<std::string, ProbeCache>& probes)
	{
		const long long end = s_copy_end_ms.load();
		if (!end)
			return;
		const long long since = NowMs() - end;
		if (since > 60000)
			return;
		std::string lead = "endcopy +" + std::to_string(since) + " ms: media";
		for (const auto& kv : probes)
			lead += " " + kv.first + "=" + std::to_string(static_cast<unsigned long long>(kv.second.media));
		LogSignalFiles(lead);
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

	// 2026-10-16: the same in every folder games are listed from (OrbisGameSearchDirs: games/ as before; with games folders set, those and
	// the NFS shares, each flat), the first copy found.
	std::string FindExistingAnywhere(const std::string& serial, uint64_t bytes)
	{
		for (const std::string& dir : OrbisGameSearchDirs())
		{
			const std::string found = FindExisting(dir, serial, bytes);
			if (!found.empty())
				return found;
		}
		return {};
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

	// live-22 (AI-assisted): while a disc is copied, the shelf shows a spinning-disc entry with a live percentage
	// (fe_app.cpp PollDiscDump). This publishes the copy's serial, title and percent (0-100) to logs/disc-dump-progress.txt,
	// which the shelf polls once or twice a second. The write is atomic (a .tmp renamed over) and cheap (every ~1%, not every
	// sector); the file is removed when the copy ends (success or fail). Its mtime is refreshed on every write, so the shelf
	// can treat a file that's gone stale (a crash mid-copy) as finished. Format: "<serial> <pct>\n<title>\n" and, from 179, a
	// 3rd line "rate=<MB/s> eta=<seconds> speed=<tag>" so the shelf can show the live speed, the time left and which drive
	// speed was taken (8x / 3.2x). The first two lines are byte-for-byte as before (an older reader ignores the 3rd).
	std::string DumpProgressPath() { return OrbisLogPath("disc-dump-progress.txt"); }
	void PublishDumpProgress(const std::string& serial, const std::string& title, int pct, double mbps, int eta_sec, const std::string& speed)
	{
		const std::string path = DumpProgressPath();
		const std::string tmp = path + ".tmp";
		if (FILE* f = fopen(tmp.c_str(), "w"))
		{
			fprintf(f, "%s %d\n%s\nrate=%.1f eta=%d speed=%s\n", serial.c_str(), pct, title.c_str(), mbps, eta_sec, speed.empty() ? "-" : speed.c_str());
			fclose(f);
			if (rename(tmp.c_str(), path.c_str()) != 0)
				unlink(tmp.c_str());
		}
	}
	void ClearDumpProgress() { unlink(DumpProgressPath().c_str()); }

	// 2.04: the disc's name as the shelf and its copy show it.
	std::string DiscTitle(const std::string& serial)
	{
		fe::GameInfo gi;
		gi.serial = serial;
		gi.title = serial;
		fe::ApplyGameDbTitle(gi);
		return gi.title.empty() || gi.title == serial ? std::string("PS2 disc") : gi.title;
	}

	// 2.11 (swordpdf: "if you're copying a disc, start a game, and go back, it asks you again"): going back to the menu from a game starts the app afresh
	// (the console only lets it return to the shelf by re-executing itself). What the person chose for the disc in the drive is kept in
	// logs/disc-ask-kept.txt ("<serial> copy|play|later <time>", removed when the disc leaves), and a start that is such a re-exec with that disc still in
	// goes on from it: a copy is taken up again (OrbisDiscResume.h), "play" and "not now" are not asked again (the disc waits on the shelf).
	std::string KeptChoicePath() { return OrbisLogPath("disc-ask-kept.txt"); }
	void ClearKeptChoice() { unlink(KeptChoicePath().c_str()); }
	bool ReadKeptChoice(std::string& serial, std::string& word)
	{
		FILE* f = fopen(KeptChoicePath().c_str(), "r");
		if (!f)
			return false;
		char s[64] = {}, w[16] = {};
		long long when = 0;
		const int got = fscanf(f, "%63s %15s %lld", s, w, &when);
		fclose(f);
		const long long now = static_cast<long long>(time(nullptr));
		if (got != 3 || now < when || now - when > 6 * 3600)
			return false;
		serial = s;
		word = w;
		return true;
	}
	bool ReadSmallFile(const std::string& path, std::string& out)
	{
		FILE* f = fopen(path.c_str(), "r");
		if (!f)
			return false;
		char buf[256] = {};
		const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		out.assign(buf, n);
		return n > 0;
	}

	// 2.04: the shelf's answer for the disc daemon (disclaunch/disc_launch.c): "<serial> copy|play|later", removed when the disc leaves.
	void WriteAskChoice(const std::string& serial, const char* word)
	{
	if (FILE* f = fopen(OrbisLogPath("disc-ask-choice.txt").c_str(), "w"))
	{
	fprintf(f, "%s %s\n", serial.c_str(), word);
	fclose(f);
	}
	// 2.11: and for the next start of the app (see KeptChoice)
	if (FILE* f = fopen(KeptChoicePath().c_str(), "w"))
	{
	fprintf(f, "%s %s %lld\n", serial.c_str(), word, static_cast<long long>(time(nullptr)));
	fclose(f);
	}
	}

	// 2.04: games/ already holds this disc (the copy's own name at the disc's size, or an image of the same serial): no question is
	// asked, its game starts from the copy as before.
	bool AlreadyCopied(const Node& node, const std::string& serial)
	{
		const std::string games = OrbisGamesDestDir(); // 2026-10-16: the games folder set, else games/; "" with none that can be written to
		const std::string iso = games + "/" + Clean(DiscTitle(serial) + " (" + serial + ")") + ".iso";
		struct stat st;
		if (!games.empty() && stat(iso.c_str(), &st) == 0 && static_cast<uint64_t>(st.st_size) == node.bytes)
			return true;
		return !FindExistingAnywhere(serial, node.bytes).empty();
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
		const std::string games = OrbisGamesDestDir(); // 2026-10-16: the games folder set (the first that is no NFS share), else games/
		if (!games.empty())
			mkdir(games.c_str(), 0777);
		const std::string name = Clean(title + " (" + serial + ")");
		const std::string iso = games + "/" + name + ".iso", part = iso + ".part";
		// vk-285-155: an ".incomplete" marker beside a copy that was made with unreadable sectors, so it stays off auto-start
		// across sessions (the caller's `clean` goes false), not only in the session that made it.
		auto has_holes = [](const std::string& image) { struct stat s; return stat((image + ".incomplete").c_str(), &s) == 0; };
		struct stat st;
		if (!games.empty() && stat(iso.c_str(), &st) == 0 && static_cast<uint64_t>(st.st_size) == node.bytes)
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
		const std::string existing = FindExistingAnywhere(serial, node.bytes);
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
		if (games.empty())
		{
			// 2026-10-16: games folders are set (PS5SX2/GameFolders) and none of them can take a copy (a missing folder, or only NFS
			// shares, which are read-only): the disc is not copied, and plays from the drive.
			static std::string s_said_none;
			if (s_said_none != serial)
			{
				s_said_none = serial;
				Log("%s: not copied: none of the games folders set can be written to; the disc plays from the drive", serial.c_str());
				OrbisNotifyPlain("PS2 disc: set a games folder that can be written to");
			}
			return {};
		}
		// 2.11: a copy the app's restart cut short (OrbisDiscResume.h) is taken up again a little before where its state says.
		const std::string state_file = part + ".state";
		uint64_t resume_at = 0, resume_bad = 0, held = 0; // held: bytes the .part occupies already (a resumed copy needs only the rest of the room)
		{
			orbis_disc::ResumeState st;
			std::string text;
			struct stat ps;
			if (stat(part.c_str(), &ps) == 0 && ReadSmallFile(state_file, text) && orbis_disc::ParseResumeState(text, st))
			{
				resume_at = orbis_disc::ResumeOffset(st, serial, node.bytes, static_cast<uint64_t>(ps.st_size));
				resume_bad = resume_at ? st.bad : 0;
				held = resume_at ? std::min<uint64_t>(static_cast<uint64_t>(ps.st_size), node.bytes) : 0;
			}
		}
		// vk-285-145: libkernel's _fstatfs (fe_ps5), never libc's statfs. UINT64_MAX: not known, copied anyway.
		const uint64_t free_bytes = orbis_frontend_free_bytes(games);
		if (free_bytes != UINT64_MAX)
		{
			if (free_bytes + held < node.bytes + (256ull << 20)) // (2.11: a resumed copy already holds `held` of it)
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
		const int out = open(part.c_str(), O_WRONLY | O_CREAT | (resume_at ? 0 : O_TRUNC), 0666);
		if (resume_at && out >= 0 && lseek(out, static_cast<off_t>(resume_at), SEEK_SET) != static_cast<off_t>(resume_at)) // (no truncation: a second cut before the next state keeps the .part as long as the state says)
		{
			Log("%s: the unfinished copy could not be taken up (errno %d): starting over", serial.c_str(), errno);
			resume_at = resume_bad = 0;
			if (ftruncate(out, 0) != 0 || lseek(out, 0, SEEK_SET) != 0)
			{
				close(out);
				if (in >= 0) close(in);
				return {};
			}
		}
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
		if (resume_at)
			Log("%s: carrying on at %llu of %llu MB (a copy the app's restart cut short)", serial.c_str(), static_cast<unsigned long long>(resume_at >> 20),
				static_cast<unsigned long long>(node.bytes >> 20));
		PublishDumpProgress(serial, title, static_cast<int>(resume_at * 100 / node.bytes), 0.0, 0, std::string()); // live-22: the shelf's spinning-disc entry starts here
		// vk-285-150: the drive asked for its fastest read through its pass device (SET STREAMING; ProsperoDiscSpeed.cpp).
		// vk-285-147/148's CDRIOCREADSPEED came back EINVAL on the console: it sends SET CD SPEED, a CD command.
		// vk-285-151: and the fastest way to read it (pread on cd0, or READ commands through pass0; ProsperoDiscSpeed.cpp)
		OrbisDiscReadMethod rm;
		rm.cd = in;
		OrbisDiscSpeedUp(node.path.c_str(), node.bytes, &rm);
		Log("reading with %s", rm.name.c_str());
		// 179: the drive speed taken, short, for the shelf's copy line (8x / 3.2x; empty when the drive was left as it was)
		const std::string speedtag = rm.set_speed == 0x80 ? "8x" : (rm.set_speed == 0x32 ? "3.2x" : std::string());
		// 2.16 (OrbisDiscEject.h): the tray lock cd(4) set when `in` was opened is taken off, so the drive's eject button works during the copy (it did not:
		// tester 2026-10-26); again every few seconds from the reader. An eject then ends the copy as "the disc was taken out", and what was copied is kept
		// for the next time the same disc goes in (the copy carries on from there, OrbisDiscResume.h). Switch file disc_lockeject: locked as before.
		const bool lock_eject = OrbisFlag("disc_lockeject");
		std::atomic<long long> allow_ms{NowMs()};
		if (lock_eject)
			Log("the tray stays locked during the copy (switch file disc_lockeject)");
		else if (const int e = orbis_disc::AllowEject(in))
			Log("the tray lock could not be taken off (CDIOCALLOW errno %d): the eject button may not work during the copy", e);
		else
			Log("tray lock taken off (CDIOCALLOW): the drive's eject button works during the copy (the copy then stops and is carried on next time)");
		// After a read that failed: is this disc still in? A fresh look (open, size, sector 16), as the watcher looks; anything else than the same size
		// and a readable ISO volume counts as out.
		auto disc_gone = [&]() {
			Node now;
			const bool there = ProbeNode(node.path, now, false) && now.bytes == node.bytes;
			if (!there)
				Log("a read failed and %s holds no readable disc of this size now: the disc was taken out", node.path.c_str());
			return !there;
		};
		const size_t chunk = static_cast<size_t>(OrbisFlag("disc_1m") ? 1 : 4) << 20;
		const int depth = 4;
		int last_pct = -1, last_note = 0, last_file_pct = -1; // live-22: last percent published to the shelf
		const auto t0 = std::chrono::steady_clock::now();
		auto rate = [&](uint64_t done) {
			const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			return secs > 0 ? (done - resume_at) / secs : 0.0;
		};
		// the speed over the last 5% (a CAV drive speeds up toward the disc's edge)
		uint64_t mark_done = resume_at;
		auto mark_time = t0;
		// 179: a smoothed current rate (EWMA over ~1 s samples, so ~a few seconds of smoothing) and the time left, for the shelf
		uint64_t sm_done = resume_at;
		auto sm_time = t0;
		double sm_bps = 0.0;
		int sm_eta = 0;
		// vk-285-148: how long the drive takes per read, and how much of the time a read was running ("drive busy"):
		// busy near 100% at a steady speed is the drive's own limit; well under it, the copy loop is waiting on something.
		std::atomic<uint64_t> read_us{0}, read_n{0};
		std::mutex bad_mutex; // the unreadable sectors found in this run (the reader thread adds them, the progress callback counts them)
		std::vector<uint64_t> bad_list;
		uint64_t mark_read_us = 0, mark_read_n = 0;
		Log("reading %zu MiB at a time, %d ahead", chunk >> 20, depth);
		const orbis_disc::CopyResult res = orbis_disc::CopyFromStop(
		resume_at, resume_bad, node.bytes, chunk, depth, 3, 4096,
			[&](uint64_t off, void* buf, size_t len) {
				if (!lock_eject && NowMs() - allow_ms.load() >= orbis_disc::kAllowEveryMs) // 2.16: the tray lock off again (no command when it is off)
				{
					allow_ms.store(NowMs());
					orbis_disc::AllowEject(in);
				}
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
				// live-22: tell the shelf every ~1% (before the 5% log/notification gate below), so the spinning-disc
				// entry's percentage climbs smoothly.
				if (pct != last_file_pct)
				{
					last_file_pct = pct;
					{
						const auto now2 = std::chrono::steady_clock::now();
						const double dt = std::chrono::duration<double>(now2 - sm_time).count();
						if (dt >= 1.0)
						{
							const double inst = dt > 0 ? (done - sm_done) / dt : 0.0;
							sm_bps = sm_bps > 0 ? sm_bps * 0.5 + inst * 0.5 : inst;
							sm_done = done;
							sm_time = now2;
							sm_eta = (sm_bps > 1e5 && node.bytes > done) ? static_cast<int>((node.bytes - done) / sm_bps) : 0;
						}
					}
					PublishDumpProgress(serial, title, pct, sm_bps / 1e6, sm_eta, speedtag);
					// 2.11: how far the writing has come, for a restart to carry on from (OrbisDiscResume.h)
					{
						orbis_disc::ResumeState st;
						st.serial = serial;
						st.total = node.bytes;
						st.done = done;
						{
							const uint64_t resume_point = done > orbis_disc::kResumeRewind ? (done - orbis_disc::kResumeRewind) / 2048 : 0;
							std::lock_guard<std::mutex> lock(bad_mutex);
							st.bad = resume_bad;
							for (const uint64_t sector : bad_list)
								st.bad += sector < resume_point ? 1 : 0;
						}
						const std::string tmp = state_file + ".tmp";
						if (FILE* sf = fopen(tmp.c_str(), "w"))
						{
							const std::string text = orbis_disc::FormatResumeState(st);
							fwrite(text.data(), 1, text.size(), sf);
							fclose(sf);
							rename(tmp.c_str(), state_file.c_str());
						}
					}
				}
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
				{
					std::lock_guard<std::mutex> lock(bad_mutex);
					bad_list.push_back(sector);
				}
				static int s_said = 0;
				if (++s_said <= 20)
					Log("sector %llu unreadable (errno %d): zeros", static_cast<unsigned long long>(sector), errno);
			},
			disc_gone);
		const uint64_t done = res.done, bad_sectors = res.bad_sectors;
		const bool failed = res.failed;
		if (failed)
			Log("copy stopped: %s (errno %d)", res.why.c_str(), errno);
		OrbisDiscSpeedRestore(node.path.c_str(), rm);
		OrbisDiscReadMethodClose(rm);
		close(in);
		if (!failed && resume_at && ftruncate(out, static_cast<off_t>(node.bytes)) != 0) // a resumed .part may have been longer than what was written over it
			Log("%s: could not set the copy's length (errno %d)", serial.c_str(), errno);
		fsync(out);
		close(out);
		if (failed && res.disc_gone)
		{
			// 2.16: taken out with the eject button: the .part and its state stay, so the same disc carries on from about here when it goes in again.
			ClearDumpProgress();
			const int pct = static_cast<int>(done * 100 / node.bytes);
			Log("%s: the copy stops at %d%% (%llu of %llu MB): the disc was taken out; put it in again to carry on from there", serial.c_str(), pct,
				static_cast<unsigned long long>(done >> 20), static_cast<unsigned long long>(node.bytes >> 20));
			char msg[200];
			snprintf(msg, sizeof(msg), "PS2 disc %s taken out at %d%%: put it back in to carry on the copy", title.c_str(), pct);
			OrbisNotifyPlain(msg);
			sleep(2);
			return {};
		}
		if (failed)
		{
			ClearDumpProgress(); // live-22: drop the shelf's spinning-disc entry
			OrbisNotifyPlain("PS2 disc copy failed: see the log (Download logs)");
			unlink(part.c_str());
			unlink(state_file.c_str());
			sleep(30);
			return {};
		}
		ClearDumpProgress(); // live-22: the copy is done; the shelf drops the spinning-disc entry (the game launches next)
		rename(part.c_str(), iso.c_str());
		s_copy_end_ms.store(NowMs()); // 2.11 (dumpxmb): t = 0 of the "endcopy" lines and of the exec decision's "the copy ended N ms ago"
		unlink(state_file.c_str());
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		// 2.16: the speed of what this run read (a copy carried on from `resume_at` read only the rest; the line said 10.2 MB/s for a 4.4 MB/s copy)
		const uint64_t read_now = done > resume_at ? done - resume_at : 0;
		Log("done: %s, %llu MB in %.0f s (%llu MB read in this run: %.1f MB/s, %.1fx average), %llu unreadable sectors", iso.c_str(),
			static_cast<unsigned long long>(done >> 20), secs, static_cast<unsigned long long>(read_now >> 20), secs > 0 ? read_now / secs / 1e6 : 0.0,
			secs > 0 ? read_now / secs / orbis_disc::kDvd1x : 0.0, static_cast<unsigned long long>(bad_sectors));
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
	// vk-285-160e (AI-assisted): heartbeats shared with the disc-auto daemon (ps5/disclaunch/disc_launch.c).
	// Each side writes its own file with time(nullptr) every couple of seconds; the other side treats it as
	// alive when the stamp is fresh. Content, not mtime, so both use the same clock source.
	bool StampFresh(const std::string& path, int within)
	{
		FILE* f = fopen(path.c_str(), "r");
		if (!f)
			return false;
		long long t = 0;
		const int got = fscanf(f, "%lld", &t);
		fclose(f);
		const long long now = static_cast<long long>(time(nullptr));
		return got == 1 && now >= t && now - t <= within;
	}
	bool DaemonAlive() { return StampFresh(OrbisLogPath("disc-auto-alive.txt"), 10); }

	bool TryReForeground(const std::string& serial)
	{
		if (!OrbisFlag("disc_refg"))
			return false;
		// vk-285-160e: the disc-auto daemon does the bounce itself (after the shell settles, and only once it
		// knows whether to launch or dump), so don't bounce a second time from here.
		if (DaemonAlive())
		{
			Log("re-foreground: disc-auto daemon is running, leaving the bounce to it");
			return false;
		}
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
		std::map<std::string, ProbeCache> probes; // vk-285-160l: last probe per node, kept while its media size holds
		// The disc whose game was started (or that was in the drive at a re-exec): not started again until it leaves.
		std::string handled;
		std::string failed;  // vk-285-155: a disc that didn't copy; not tried again until it leaves (no 3 s retry loop)
		std::string pending; // a copy whose game waits for the shelf
		std::string pending_said; // 2.11 (dumpxmb): the copy whose "waits: no shelf is up" was logged (once)
		std::string asked;    // 2.04: the disc the shelf was asked about (play from the disc, or copy it)
		std::string unasked;  // 2.04: the disc that waits for a shelf (logged once)
		std::string at_start; // 2.04: the disc that was already in when this run looked first (it started PS5SX2, or PS5SX2 was opened with it in): not asked about, copied as before
		std::string declined; // 2.04: a disc the person said "not now" to (or whose question went away): left alone until it leaves
		bool refg_done = false; // vk-285-159: one re-foreground attempt per boot, whatever the outcome (flag disc_refg)
		long long last_media_change_ms = 0; // 2.11 (dumpxmb): when a media size other than 0 was last seen to change (the log's "N ms after the disc was seen")
		const bool relaunch = TakeRelaunchMark();
		// 2.11: a start that is a re-exec into the shelf (back to the menu) with the same disc still in carries on from what was chosen before; any other
		// start asks afresh.
		std::string kept_serial, kept_word;
		if (relaunch)
			ReadKeptChoice(kept_serial, kept_word);
		else
			ClearKeptChoice();
		std::string drive_shown; // 2.11: the disc the shelf was told is in the drive
		const std::string recent = RecentAutoStart(kAutoStartGuardSeconds); // vk-285-155: a disc auto-started recently (a likely failed launch)
		sleep(5); // vk-285-145: well after the app's start
		if (relaunch)
			Log("a re-exec into the shelf: a disc in the drive now doesn't start its game");
		if (!recent.empty())
			Log("%s was auto-started moments ago: if it's still in, its game isn't auto-started again (pick it from the shelf)", recent.c_str());
		for (;;)
		{
			// vk-285-160c (AI-assisted): DiscLaunch.elf wrote disc-launch-exec.txt to ask us to self-exec and
			// come to the foreground.  A self-exec from within our own process is what the PS5 shell needs to
			// bring the app to the front; an exec from an external ELF loader does not.  disc-launch.txt
			// (the ISO path) was already written by the ELF and will be picked up at the next boot.
			// 2.11 (dumpxmb): honoured only when no game is being started or runs in this process and this is not a disc tile (OrbisDiscExec.h); an
			// ignored signal takes disc-launch.txt with it. Flags: discexec (the old behaviour), nodiscexec (never).
			{
				const std::string exec_mark = OrbisLogPath("disc-launch-exec.txt");
				struct stat est{};
				if (stat(exec_mark.c_str(), &est) == 0)
				{
					unlink(exec_mark.c_str());
					orbis_disc::ExecIn ein = {};
					ein.is_tile = s_tile_process.load();
					ein.game_starting = s_game_starting.load();
					ein.flag_discexec = OrbisFlag("discexec");
					ein.flag_nodiscexec = OrbisFlag("nodiscexec");
					const orbis_disc::ExecVerdict verdict = orbis_disc::DecideExec(ein);
					const long long now_ms = NowMs(), copy_end = s_copy_end_ms.load(), starting = s_game_starting_ms.load();
					Log("disc-launch-exec.txt: seen (the copy ended %lld ms ago, a game is starting since %lld ms, tile %d, flags discexec %d nodiscexec %d): %s",
						copy_end ? now_ms - copy_end : -1LL, starting ? now_ms - starting : -1LL, ein.is_tile ? 1 : 0, ein.flag_discexec ? 1 : 0,
						ein.flag_nodiscexec ? 1 : 0, orbis_disc::ExecVerdictText(verdict));
					if (verdict != orbis_disc::ExecVerdict::Honor)
						orbis_disc::RemoveSignalFiles(OrbisLogPath("disc-launch.txt"), exec_mark);
					else
					{
						Log("disc-launch-exec.txt: self-exec to come to the foreground"); // (the old line, kept: the old logs are read for it)
						// vk-285-160e: a re-foreground mark too, so a disc_refg flag left on from 159 testing doesn't
						// bounce the fresh boot a second time for the same disc.
						if (FILE* rf = fopen(ReForegroundMarkPath().c_str(), "w"))
						{
							fprintf(rf, "disc-auto %lld\n", static_cast<long long>(time(nullptr)));
							fclose(rf);
						}
						const char* path = "/data/homebrew/PPSA99203/eboot.bin";
						struct stat st{};
						if (stat(path, &st) != 0)
							path = "/app0/eboot.bin";
						fflush(stdout);
						const int rc = sceSystemServiceLoadExec(path, nullptr);
						Log("disc-launch-exec.txt: LoadExec(%s) returned 0x%08x%s", path, static_cast<unsigned>(rc),
							rc == 0 ? " (accepted: the system is closing this process)" : " (refused; carrying on)");
						if (rc == 0)
							for (int i = 0; i < 100; i++)
								usleep(100000); // as TryReForeground: the kill is under way; start nothing in a dying process
					}
				}
			}

			const std::vector<std::string> nodes = Candidates(first);
			bool any_ps2 = false;
			bool waiting = false;
			std::string present_serial, present_title; // 2.11: the PS2 disc in the drive that is not copied (the shelf lists it)
			for (const std::string& path : nodes)
			{
				// PR #34 port: a game plays from this drive (OrbisDiscDumpClaim): not looked at, not copied. A PS2 disc
				// it held counts as still in, so its game isn't started again when the drive is given back.
				if (Claimed(path))
				{
					const auto it = probes.find(path);
					any_ps2 = any_ps2 || (it != probes.end() && it->second.iso && !it->second.serial.empty());
					continue;
				}
				const bool log = std::find(logged.begin(), logged.end(), path) == logged.end();
				// vk-285-160l: reuse the last probe while the media size is unchanged (no disc reads, so the drive can rest).
				const uint64_t media = QuietMediaSize(path);
				ProbeCache& pc = probes[path];
				if (media == 0 || media != pc.media || log)
				{
					// 2.11 (dumpxmb): the media size changing is the only sign the app gets that the disc went out or in (the drive is not asked anything more)
					if (media != pc.media)
					{
						if (!log)
							Log("%s: media size %llu -> %llu", path.c_str(), static_cast<unsigned long long>(pc.media), static_cast<unsigned long long>(media));
						if (media != 0)
							last_media_change_ms = NowMs();
					}
					pc = ProbeCache{};
					pc.iso = ProbeNode(path, pc.node, log);
					pc.media = media; // a non-PS2 disc is remembered too, so it isn't re-read every 3 s either
					if (pc.iso)
						pc.serial = fe::ReadSerial(path);
				}
				if (log)
					logged.push_back(path);
				if (!pc.iso)
					continue;
				Node node = pc.node;
				const std::string serial = pc.serial;
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
				if (present_serial.empty() && !AlreadyCopied(node, serial))
				{
					present_serial = serial;
					present_title = DiscTitle(serial);
				}
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
				// 2.04: a disc not copied yet: the shelf asks whether to play it from the disc or copy it first (flag nodiscask: copied
				// at once, as before). With no shelf up it is copied as before too.
				if (first && at_start.empty())
					at_start = serial;
				// 2.11: what was chosen before the app restarted (see ReadKeptChoice): a copy goes on without a question; "play" and "not now" are left alone.
				bool resumed_copy = false;
				if (serial == kept_serial)
				{
					const bool copy = kept_word == "copy";
					kept_serial.clear();
					if (copy)
					{
						resumed_copy = true;
						Log("%s: copy it first (chosen before the app restarted): carrying on without asking", serial.c_str());
					}
					else
					{
						declined = serial;
						Log("%s: %s before the app restarted: not asked again (pick the disc on the shelf to choose)", serial.c_str(),
							kept_word == "play" ? "played from the disc" : "\"not now\"");
					}
				}
				if (serial == declined)
				continue;
				// A disc is copied only when the person chooses it on the shelf. With no shelf up (the PS5 home screen with PS5SX2 in the
				// background, a game running, the app still starting) nothing is copied: the disc waits, and the question comes with the shelf.
				if (!resumed_copy && !OrbisFlag("nodiscask") && !AlreadyCopied(node, serial))
				{
					// fix180 (owner): a disc tile's process never asks "play from the disc or copy it" (the tile chose already: the dump by serial, else the
					// disc; main-boot.cpp, OrbisDiscTileStart.h). When the tile ended on the shelf after all, the disc is its entry there, picked by hand.
					if (s_tile_process.load()) // fix180
					{
						if (declined != serial)
							Log("%s: a disc tile's process: no question about the disc (it is on the shelf to pick)", serial.c_str());
						declined = serial;
						continue;
					}
					if (asked != serial)
					{
						if (orbis_frontend_ask_disc(DiscTitle(serial), serial))
						{
							asked = serial;
							waiting = true;
							Log("%s: asked on the shelf: play from the disc, or copy it first", serial.c_str());
							continue;
						}
						if (unasked != serial)
						{
							unasked = serial;
							Log("%s: no shelf is up: not copied; the question comes when the shelf is up", serial.c_str());
						}
						continue;
					}
					else
					{
						const int answer = orbis_frontend_disc_answer();
						if (answer == 0)
						{
							waiting = true;
							continue;
						}
						if (answer == 2)
						{
							if (OrbisDiscDumpClaim(path) && orbis_frontend_request_launch(path))
							{
								NoteGameStarting("play from the drive: the launch request was taken"); // 2.11 (dumpxmb)
								WriteAskChoice(serial, "play");
								Log("%s: playing straight from the drive %s", serial.c_str(), path.c_str());
								OrbisNotifyPlain("PS2 disc: starting the game from the disc");
								handled = serial;
							}
							else
							{
								WriteAskChoice(serial, "later");
								Log("%s: could not start from the drive %s; left alone", serial.c_str(), path.c_str());
								OrbisDiscDumpRelease(path);
								declined = serial;
							}
							continue;
						}
						if (answer == 3 || answer < 0)
						{
							WriteAskChoice(serial, "later");
							Log("%s: %s", serial.c_str(), answer == 3 ? "not now: the disc is left alone until it is taken out" : "the question went away: the disc is left alone");
							declined = serial;
							continue;
						}
						WriteAskChoice(serial, "copy");
						Log("%s: copy it first (chosen on the shelf)", serial.c_str());
					}
				}
				bool clean = true;
				if (!BeginCopy(path))
				continue; // PR #34 port: claimed for a game just now
				// 2.11 (review): the disc is being copied (or has been, or failed): the shelf's "disc in the drive" entry goes; the next look says again
				present_serial.clear();
				if (!drive_shown.empty())
				{
					drive_shown.clear();
					orbis_frontend_set_drive_disc(std::string(), std::string());
				}
				s_busy.store(true);
				const std::string copy = Dump(node, serial, clean);
				s_busy.store(false);
				EndCopy();
				if (copy.empty())
				{
				// vk-285-155: don't re-read the disc and rewrite gigabytes every 3 s; wait for it to leave.
				failed = serial;
				WriteAskChoice(serial, "later"); // 2.11 (review): a restart must not retry a copy that failed, without asking
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
			// 2.11 (swordpdf: "since the emulator knows what game is inside the drive, it would be cool to have the disc in the shelf as well"): the shelf lists
			// the PS2 disc in the drive that is not copied, first, and asks about it again when it is picked there.
			{
				const std::string shown = (OrbisFlag("nodiscask") || present_serial.empty()) ? std::string() : present_serial;
				if (shown != drive_shown)
				{
					drive_shown = shown;
					orbis_frontend_set_drive_disc(shown.empty() ? std::string() : present_title, shown);
				}
			}
			if (!pending.empty() && orbis_frontend_request_launch(pending))
			{
				NoteGameStarting("the launch request for the copy was taken"); // 2.11 (dumpxmb): from now on the disc helper's exec signal is ignored
				{
					const long long now_ms = NowMs(), end = s_copy_end_ms.load();
					Log("starting %s (%lld ms after the copy ended, %lld ms after the disc was seen)", pending.c_str(), end ? now_ms - end : -1LL,
						last_media_change_ms ? now_ms - last_media_change_ms : -1LL);
				}
				OrbisNotifyPlain("PS2 disc: starting the game");
				WriteAutoStartMark(handled); // vk-285-155: so a force-off then reopen doesn't loop this game
				pending.clear();
				pending_said.clear();
			}
			else if (!pending.empty() && pending_said != pending)
			{
				// 2.11 (dumpxmb): the copy is done and its game waits: no shelf is up to take the request (a game runs, or the shelf is not up yet)
				pending_said = pending;
				Log("%s waits: no shelf is up to take the launch request (a game runs, or the shelf is not up yet)", pending.c_str());
			}
			if (!any_ps2 && !asked.empty())
			{
				orbis_frontend_cancel_disc_ask(); // 2.04: the disc was taken out with the question open
				asked.clear();
				unlink(OrbisLogPath("disc-ask-choice.txt").c_str());
				ClearKeptChoice();
			}
			if (!any_ps2)
			{
			unasked.clear();
			declined.clear();
			at_start.clear();
			kept_serial.clear();
			ClearKeptChoice();
			}
			if (!any_ps2 && (!handled.empty() || !failed.empty()))
			{
				if (!handled.empty())
					Log("the disc is out: %s starts again when it's put back", handled.c_str());
				handled.clear();
				failed.clear(); // vk-285-155: a disc that failed is tried again once it's reinserted
				pending.clear();
				pending_said.clear();
				ClearReForegroundMark(); // vk-285-159: a re-inserted disc bounces us to the front again (flag disc_refg)
				refg_done = false;
				// vk-285-160d (AI-assisted): remove the autostart guard so a reinserted disc
				// auto-starts again and the user can browse the shelf without a stale guard.
				unlink(AutoStartMarkPath().c_str());
			}
			// a node that went away and came back (a disc swapped) is described again
			if (logged.size() > 32)
				logged.clear();
			first = false;
			EndCopyTimeline(probes); // 2.11 (dumpxmb): one line per pass for a minute after a copy ended
			// 2.04: a question waits on the shelf: look for the answer every second. 2.11: in tenths of a second, so the shelf's "ask about the disc again" is
			// heard at once.
			bool reask = false;
			for (int tenth = 0; tenth < (waiting ? 10 : 30) && !reask; tenth++)
			{
				reask = orbis_frontend_take_reask();
				if (!reask)
					usleep(100000);
			}
			if (reask)
			{
				Log("the shelf asks about the disc again");
				declined.clear();
				asked.clear();
				failed.clear();
				unasked.clear();
				handled.clear(); // 2.11 (review): a fresh choice: a copy then starts its game, as on a fresh insert
				pending.clear();
				pending_said.clear();
				kept_serial.clear();
				unlink(OrbisLogPath("disc-ask-choice.txt").c_str());
				ClearKeptChoice();
				orbis_frontend_cancel_disc_ask();
			}
		}
		return nullptr;
	}
} // namespace

// vk-285-160e (AI-assisted): "PS5SX2 is running" for the disc-auto daemon. A thread of its own (the disc
// watcher blocks for minutes inside a dump) that stamps logs/ps5sx2-alive.txt every 2 s. 159 showed the app's
// threads keep running while the shell has the user at the home screen, so a stale stamp means not running.
static void* AliveThread(void*)
{
	const std::string path = OrbisLogPath("ps5sx2-alive.txt");
	for (;;)
	{
		if (FILE* f = fopen(path.c_str(), "w"))
		{
			fprintf(f, "%lld\n", static_cast<long long>(time(nullptr)));
			fclose(f);
		}
		{
			// 2.11: a game plays from a drive: the stamp for the disc helper stays fresh (StampPlayingLocked)
			std::lock_guard<std::mutex> lock(s_drive_mutex);
			if (!s_claimed.empty())
				StampPlayingLocked();
		}
		sleep(2);
	}
	return nullptr;
}

void OrbisDiscDumpStart()
{
	if (s_started.exchange(true))
		return;
	{
		// 2.11: a stamp "a game plays from a drive" left by the run before this one (it ended by re-executing or being killed) is not true now
		std::lock_guard<std::mutex> lock(s_drive_mutex);
		if (s_claimed.empty())
			StampPlayingLocked();
	}
	{
		pthread_t at;
		pthread_attr_t aattr;
		pthread_attr_init(&aattr);
		pthread_attr_setstacksize(&aattr, 64 * 1024);
		pthread_attr_setdetachstate(&aattr, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&at, &aattr, AliveThread, nullptr) != 0)
			Log("alive thread: pthread_create failed");
		pthread_attr_destroy(&aattr);
	}
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

// 2.11 (dumpxmb): see ProsperoDiscDump.h and OrbisDiscExec.h
void OrbisDiscNoteTile(bool tile)
{
	s_tile_process.store(tile);
	if (tile)
		Log("this process is a disc tile: the disc helper's disc-launch-exec.txt is never honoured here");
}

void OrbisDiscNoteGameStarting(const char* how)
{
	NoteGameStarting(how);
}

void OrbisDiscLogSignalFiles(const char* when)
{
	LogSignalFiles(when);
}

std::string OrbisFindDiscCopy(const std::string& serial, const std::string& device)
{
	const uint64_t bytes = QuietMediaSize(device);
	const std::string games = OrbisGamesDestDir(); // 2026-10-16: the games folder set, else games/; "" with none that can be written to
	auto whole = [](const std::string& image) {
		struct stat s;
		return !image.empty() && stat((image + ".incomplete").c_str(), &s) != 0;
	};
	const std::string iso = games + "/" + Clean(DiscTitle(serial) + " (" + serial + ")") + ".iso";
	struct stat st;
	if (!games.empty() && stat(iso.c_str(), &st) == 0 && (bytes == 0 || static_cast<uint64_t>(st.st_size) == bytes) && whole(iso))
		return iso;
	if (bytes == 0)
		return {};
	const std::string other = FindExistingAnywhere(serial, bytes);
	return whole(other) ? other : std::string();
}

std::string OrbisDiscSettingsStem(const std::string& serial)
{
	return Clean(DiscTitle(serial) + " (" + serial + ")");
}

bool OrbisDiscDumpClaim(const std::string& device)
{
	std::lock_guard<std::mutex> lock(s_drive_mutex);
	if (device == s_copying)
	{
		Log("%s: its disc is being copied now: no game plays from it until the copy ends", device.c_str());
		return false;
	}
	if (s_claimed != device)
		Log("%s: a game plays from this drive: the disc watcher leaves it alone", device.c_str());
	s_claimed = device;
	StampPlayingLocked(); // 2.11: for the disc helper
	return true;
}

void OrbisDiscDumpRelease(const std::string& device)
{
	std::lock_guard<std::mutex> lock(s_drive_mutex);
	if (s_claimed != device)
		return;
	s_claimed.clear();
	StampPlayingLocked(); // 2.11: removes the disc helper's stamp
	Log("%s: no game plays from this drive now: the disc watcher may copy its disc", device.c_str());
}
