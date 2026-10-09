// PS5SX2 (vk-285-150/151, AI-assisted): the PS5's disc drive read faster than 2x.
//
// swordpdf: "for next round we should try 8x dump speed". vk-285-149 on his console: a PS2 DVD copied at a flat 2.0x with
// the drive busy 100% of the time, and FreeBSD's CDRIOCREADSPEED on /dev/cd0 came back EINVAL both as "max" and as
// 11080 kB/s (it sends SET CD SPEED, a CD command). He pointed at the 11.40 system modules: SceShellCore carries FreeBSD's
// libcam ("CAMGETPASSTHRU ioctl failed", "/dev/pass%d", "system=CAM subsystem=SCSI_CD") and its ioctl numbers are this
// SDK's: CAMIOCOMMAND 0xC4E01902 (9 places) and CAMGETPASSTHRU 0xC4E01903, so the kernel's union ccb is the 1248 bytes
// we build. (/dev/icc_bddrive in libkernel is the drive's power, tray and chucking, not its speed.)
//
// vk-285-150 on the console (13.60): CAMGETPASSTHRU gives pass0, which opens; INQUIRY "SONY | PS-SYSTEM 503R | 2305";
// GET PERFORMANCE: LBA 0..1403615 at 4432..11080 kB/s, 3.2x..8.0x DVD (a CAV drive able to do 8x); but SET CD SPEED and
// SET STREAMING both "illegal request, asc 20": invalid command operation code, not supported by Sony's firmware. And
// the copy still ran at 2.0x, under even the 3.2x the drive gives the inner edge: so the drive's spin isn't what holds it.
//
// vk-285-151: the reads themselves. /dev/cd0 cuts every read into commands of the system's largest I/O, one after
// another; a drive that waits between them (its read-ahead not covering the gap) delivers a steady fraction of its speed.
// So before each copy, 16 MiB each, timed: pread on /dev/cd0 (what the copy did), READ(10) through pass0 of 64 KiB to
// 1 MiB (a size the kernel refuses is skipped), READ(12) with the Streaming bit, and two lanes (two pass descriptors,
// two threads, two commands in flight). Each method's first read is checked against pread's bytes; the copy then uses
// the fastest that matches, if it beats pread by 15%. Needs proper testing on a console.
//
// vk-285-151 on the console: all 2.0x (pread 2.72 MB/s, READ(10) 64 KiB 2.75, READ(12) Streaming 2.76, two lanes 1.10;
// over 64 KiB E2BIG). A payload (PS5SX2DiscTest.elf) read at 2.00x too. swordpdf: "can you look at the libs again":
// SceShellCore (11.40) has _AutoMounter::OpticalDisc::setSpeed(int), "exec_sie_set_read_speed", "drive is idle and
// drive speed is set to %#x during disc key update", "drive speed is restored to %#x". setSpeed (0x376790) opens
// "cd0" with libcam and sends a Sony vendor command: CDB DB <rotation & 3> <speed hi> <speed lo>, 12 bytes, no data,
// 10 s, flags CAM_DIR_NONE | CAM_DEV_QFRZDIS | CAM_PASS_ERR_RECOVER, simple tag; the speed from a table of
// rotation/speed pairs where 0/0x20 (2.0) is the default and 1/0x80 is 8.0 (OrbisDiscScsi.h).
//
// vk-285-152: so before each copy: time pread as the drive is, then each of Sony's own faster settings (1/0x80 8.0,
// 1/0x100, 0/0xFFFF max, 1/0x60 6.0), 16 MiB each after a 4 MiB warm-up; copy at the fastest if it beats the drive as it
// was by 15%; put the drive back to 2.0 (0/0x20) after the copy. Only values from Sony's table are sent. The 151 pass
// read timing only with flag disc_passbench.
//
// Every command's CAM status and sense data is logged when it fails. Nothing is written to the disc or the drive's
// firmware. All through libkernel's ioctl (no system call from our own code). Flag nodiscspeed: nothing is sent.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProsperoDiscSpeed.h"
#include "OrbisDiscScsi.h"
#include "ProsperoNotify.h"
#include "OrbisPaths.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

static_assert(sizeof(union ccb) == 0x4E0, "union ccb must be the 1248 bytes SceShellCore's CAMIOCOMMAND (0xC4E01902) carries");
static_assert(CAMIOCOMMAND == 0xC4E01902u && CAMGETPASSTHRU == 0xC4E01903u, "CAM ioctl numbers differ from SceShellCore's");

namespace
{
	void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
	void Log(const char* fmt, ...)
	{
		char line[1024];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(line, sizeof(line), fmt, ap);
		va_end(ap);
		printf("[discspeed] %s\n", line);
		fflush(stdout);
	}

	std::string Hex(const uint8_t* p, size_t n)
	{
		std::string s;
		char b[4];
		for (size_t i = 0; i < n; i++)
		{
			snprintf(b, sizeof(b), "%02x", p[i]);
			s += b;
			if (i + 1 < n && (i & 3) == 3)
				s += ' ';
		}
		return s;
	}

	// The pass device for the drive (e.g. "/dev/pass0"), or empty.
	std::string PassDevice(int cd, bool quiet = false)
	{
		union ccb ccb;
		memset(&ccb, 0, sizeof(ccb));
		ccb.ccb_h.func_code = XPT_GDEVLIST;
		if (ioctl(cd, CAMGETPASSTHRU, &ccb) != 0)
		{
			Log("CAMGETPASSTHRU on the drive: errno %d", errno);
			return {};
		}
		char name[DEV_IDLEN + 1] = {};
		memcpy(name, ccb.cgdl.periph_name, DEV_IDLEN);
		if (!quiet)
			Log("CAMGETPASSTHRU: status %d, %s%u (path %u, target %u, lun %u)", static_cast<int>(ccb.cgdl.status), name,
				ccb.cgdl.unit_number, ccb.ccb_h.path_id, ccb.ccb_h.target_id, static_cast<unsigned>(ccb.ccb_h.target_lun));
		if (ccb.cgdl.status == CAM_GDEVLIST_ERROR || !name[0])
			return {};
		return std::string("/dev/") + name + std::to_string(ccb.cgdl.unit_number);
	}

	// What a command came to: 0 done; else the ioctl's errno (> 0), or -1 for a CAM/SCSI error.
	struct Result
	{
		int code = 0;
		std::string why;
	};

	// One command through the pass device. `quiet`: log nothing when it works (reads).
	Result Command(int pass, const std::vector<uint8_t>& cdb, uint32_t dir, void* data, uint32_t len, const char* what, bool quiet = false)
	{
		union ccb ccb;
		memset(&ccb, 0, sizeof(ccb));
		cam_fill_csio(&ccb.csio, /*retries*/ 1, nullptr, dir | CAM_DEV_QFRZDIS, MSG_SIMPLE_Q_TAG, static_cast<u_int8_t*>(data), len,
			SSD_FULL_SIZE, static_cast<u_int8_t>(cdb.size()), /*timeout ms*/ 15000);
		memcpy(ccb.csio.cdb_io.cdb_bytes, cdb.data(), cdb.size());
		const auto t0 = std::chrono::steady_clock::now();
		const int rc = ioctl(pass, CAMIOCOMMAND, &ccb);
		const int err = rc ? errno : 0;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		Result r;
		if (rc != 0)
		{
			r.code = err > 0 ? err : 1;
			r.why = "CAMIOCOMMAND errno " + std::to_string(err);
			if (!quiet)
				Log("%s: %s", what, r.why.c_str());
			return r;
		}
		const uint32_t status = ccb.ccb_h.status & CAM_STATUS_MASK;
		if (status == CAM_REQ_CMP)
		{
			if (!quiet)
				Log("%s: ok (%.0f ms)", what, ms);
			return r;
		}
		std::string sense = "no sense data";
		if (status == CAM_SCSI_STATUS_ERROR && (ccb.ccb_h.status & CAM_AUTOSNS_VALID))
		{
			const size_t n = SSD_FULL_SIZE - ccb.csio.sense_resid;
			sense = orbis_mmc::SenseText(orbis_mmc::ParseSense(reinterpret_cast<const uint8_t*>(&ccb.csio.sense_data), n));
		}
		char b[200];
		snprintf(b, sizeof(b), "CAM status %#x, SCSI status %#x, %s (%.0f ms)", ccb.ccb_h.status, ccb.csio.scsi_status, sense.c_str(), ms);
		r.code = -1;
		r.why = b;
		static std::atomic<int> s_failures{0};
		if (!quiet || s_failures.fetch_add(1) < 20)
			Log("%s: %s", what, b);
		return r;
	}

	uint32_t Perf(int pass, const char* what)
	{
		std::vector<uint8_t> buf(orbis_mmc::GetPerformanceLen(), 0);
		uint32_t best = 0;
		if (Command(pass, orbis_mmc::GetPerformance(), CAM_DIR_IN, buf.data(), static_cast<uint32_t>(buf.size()), what).code)
			return 0;
		for (const auto& p : orbis_mmc::ParsePerformance(buf.data(), buf.size()))
		{
			Log("  LBA %u..%u: %u..%u kB/s (%.1fx..%.1fx DVD)", p.start_lba, p.end_lba, p.start_kbs, p.end_kbs, p.start_kbs / 1385.0,
				p.end_kbs / 1385.0);
			if (p.end_kbs > best)
				best = p.end_kbs;
			if (p.start_kbs > best)
				best = p.start_kbs;
		}
		return best;
	}

	// One lane: [off, off+len) in commands of m.cmd_bytes through `pass`. 0 or the first failure's code.
	int PassReadLane(const OrbisDiscReadMethod& m, int pass, uint64_t off, uint8_t* buf, size_t len)
	{
		while (len)
		{
			const size_t n = len < m.cmd_bytes ? len : m.cmd_bytes;
			const uint32_t lba = static_cast<uint32_t>(off / 2048), count = static_cast<uint32_t>(n / 2048);
			const std::vector<uint8_t> cdb = m.read12 ? orbis_mmc::Read12(lba, count, m.streaming) : orbis_mmc::Read10(lba, static_cast<uint16_t>(count));
			const Result r = Command(pass, cdb, CAM_DIR_IN, buf, static_cast<uint32_t>(n), "READ", true);
			if (r.code)
				return r.code;
			off += n;
			buf += n;
			len -= n;
		}
		return 0;
	}

	// The method on [off, off+len) without the pread fallback: 0, or the first failure's code.
	int PassRead(const OrbisDiscReadMethod& m, uint64_t off, void* buf, size_t len)
	{
		uint8_t* p = static_cast<uint8_t*>(buf);
		if (m.lanes < 2 || m.pass[1] < 0 || len < 2 * static_cast<size_t>(m.cmd_bytes))
			return PassReadLane(m, m.pass[0], off, p, len);
		// two commands in flight: the second lane takes the second half (cut on a command boundary)
		const size_t half = (len / 2 + m.cmd_bytes - 1) / m.cmd_bytes * m.cmd_bytes;
		int second = 0;
		std::thread t([&] { second = PassReadLane(m, m.pass[1], off + half, p + half, len - half); });
		const int first = PassReadLane(m, m.pass[0], off, p, half);
		t.join();
		return first ? first : second;
	}

	struct Bench
	{
		OrbisDiscReadMethod m;
		double mbs = 0;
		bool ok = false;
		std::string why;
	};
} // namespace

ssize_t OrbisDiscRead(const OrbisDiscReadMethod& m, uint64_t off, void* buf, size_t len)
{
	if (m.cmd_bytes == 0 || m.pass[0] < 0 || (off % 2048) || (len % 2048))
		return pread(m.cd, buf, len, static_cast<off_t>(off));
	if (PassRead(m, off, buf, len) == 0)
		return static_cast<ssize_t>(len);
	return pread(m.cd, buf, len, static_cast<off_t>(off)); // what the pass read couldn't do, the plain way
}

void OrbisDiscSpeedRestore(const char* cd_path, OrbisDiscReadMethod& m)
{
	if (m.set_speed == 0)
		return;
	const int cd = m.cd >= 0 ? m.cd : open(cd_path, O_RDONLY);
	if (cd < 0)
		return;
	const std::string pass_path = PassDevice(cd, true);
	if (m.cd < 0)
		close(cd);
	const int pass = pass_path.empty() ? -1 : open(pass_path.c_str(), O_RDWR);
	if (pass < 0)
	{
		Log("speed back to 2.0: no pass device (errno %d)", errno);
		return;
	}
	Command(pass, orbis_mmc::SieSetReadSpeed(0, 0x20), CAM_DIR_NONE | CAM_PASS_ERR_RECOVER, nullptr, 0, "Sony speed command: rotation 0, speed 2.0 (default, after the copy)");
	close(pass);
	m.set_speed = 0;
}

void OrbisDiscReadMethodClose(OrbisDiscReadMethod& m)
{
	for (int& p : m.pass)
	{
		if (p >= 0)
			close(p);
		p = -1;
	}
	m.cmd_bytes = 0;
	m.lanes = 1;
	m.name = "pread on /dev/cd0";
}

void OrbisDiscSpeedUp(const char* cd_path, uint64_t bytes, OrbisDiscReadMethod* out)
{
	if (OrbisFlag("nodiscspeed"))
	{
		Log("off (flag nodiscspeed)");
		return;
	}
	const int cd = out && out->cd >= 0 ? out->cd : open(cd_path, O_RDONLY);
	if (cd < 0)
	{
		Log("%s: open errno %d", cd_path, errno);
		return;
	}
	const std::string pass_path = PassDevice(cd);
	if (!(out && out->cd >= 0))
		close(cd);
	if (pass_path.empty())
		return;
	struct stat st;
	const int sr = stat(pass_path.c_str(), &st);
	Log("%s: %s", pass_path.c_str(), sr == 0 ? "there" : ("stat errno " + std::to_string(errno)).c_str());
	auto open_pass = [&]() {
		int fd = open(pass_path.c_str(), O_RDWR);
		if (fd < 0)
			fd = open(pass_path.c_str(), O_RDONLY);
		return fd;
	};
	const int pass = open_pass();
	if (pass < 0)
	{
		Log("%s: open errno %d", pass_path.c_str(), errno);
		return;
	}

	static bool s_described = false; // the drive's answers don't change: once per app run
	if (!s_described)
	{
		s_described = true;
		uint8_t inq[36] = {};
		if (!Command(pass, orbis_mmc::Inquiry(), CAM_DIR_IN, inq, sizeof(inq), "INQUIRY").code)
			Log("  drive: %s", orbis_mmc::InquiryText(inq, sizeof(inq)).c_str());
		uint8_t page[128] = {};
		if (!Command(pass, orbis_mmc::ModeSenseCaps(sizeof(page)), CAM_DIR_IN, page, sizeof(page), "MODE SENSE 2A").code)
		{
			const orbis_mmc::Caps c = orbis_mmc::ParseCaps(page, sizeof(page));
			Log("  capabilities page: %s, max read %u kB/s, current read %u kB/s; %s", c.ok ? "found" : "not found", c.max_read_kbs,
				c.cur_read_kbs, Hex(page, 32).c_str());
		}
		Perf(pass, "GET PERFORMANCE");
		// vk-285-150: SET CD SPEED and SET STREAMING both "invalid command operation code" (asc 20) on PS-SYSTEM 503R
		// 2305; asked once more per run in case another drive takes it
		Command(pass, orbis_mmc::SetCdSpeed(), CAM_DIR_NONE, nullptr, 0, "SET CD SPEED max");
		const uint32_t sectors = static_cast<uint32_t>(bytes / 2048);
		std::vector<uint8_t> desc = orbis_mmc::StreamingDescriptor(0, sectors ? sectors - 1 : 0, 22160);
		Command(pass, orbis_mmc::SetStreaming(), CAM_DIR_OUT, desc.data(), static_cast<uint32_t>(desc.size()), "SET STREAMING 22160 kB/s (16x)");
	}

	if (!out || out->cd < 0)
	{
		close(pass);
		return;
	}

	// vk-285-152: Sony's speed settings, timed with pread on cd0 (16 MiB each after a 4 MiB warm-up, fresh parts of the disc)
	{
		std::vector<uint8_t> sbuf(4u << 20);
		auto time_at = [&](uint64_t from, const char* what) {
			from &= ~static_cast<uint64_t>(2047);
			constexpr uint64_t kTimed = 16ull << 20;
			if (from + (4ull << 20) + kTimed > bytes)
			{
				Log("%s: the disc is too small to time", what);
				return 0.0;
			}
			pread(cd, sbuf.data(), sbuf.size(), static_cast<off_t>(from)); // warm-up: the drive settling at its new speed
			const auto t0 = std::chrono::steady_clock::now();
			uint64_t got = 0;
			for (uint64_t off = from + sbuf.size(); off < from + sbuf.size() + kTimed; off += sbuf.size())
			{
				if (pread(cd, sbuf.data(), sbuf.size(), static_cast<off_t>(off)) != static_cast<ssize_t>(sbuf.size()))
				{
					Log("%s: pread errno %d", what, errno);
					break;
				}
				got += sbuf.size();
			}
			const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			const double mbs = sec > 0 ? got / sec / 1e6 : 0;
			Log("%s: %.2f MB/s = %.2fx DVD (%llu MiB in %.2f s)", what, mbs, mbs * 1e6 / 1385000.0, static_cast<unsigned long long>(got >> 20), sec);
			return got == kTimed ? mbs : 0.0;
		};
		struct Setting
		{
			uint8_t rot;
			uint16_t speed;
		};
		static const Setting kTry[] = {{1, 0x0080}, {1, 0x0100}, {0, 0xFFFF}, {1, 0x0060}};
		constexpr uint64_t kStep = 24ull << 20, kFirst = 64ull << 20;
		const double base = time_at(kFirst, "speed as the drive is");
		double best = base;
		int best_i = -1;
		for (int i = 0; i < static_cast<int>(sizeof(kTry) / sizeof(kTry[0])); i++)
		{
			char what[96];
			snprintf(what, sizeof(what), "Sony speed command: rotation %u, speed %s (DB %02x %02x %02x)", kTry[i].rot,
				orbis_mmc::SieSpeedText(kTry[i].speed).c_str(), kTry[i].rot, kTry[i].speed >> 8, kTry[i].speed & 0xFF);
			if (Command(pass, orbis_mmc::SieSetReadSpeed(kTry[i].rot, kTry[i].speed), CAM_DIR_NONE | CAM_PASS_ERR_RECOVER, nullptr, 0, what).code)
				continue;
			const double mbs = time_at(kFirst + (i + 1) * kStep, what);
			if (mbs > best)
			{
				best = mbs;
				best_i = i;
			}
		}
		if (best_i >= 0 && best > base * 1.15)
		{
			const Setting& k = kTry[best_i];
			char what[96];
			snprintf(what, sizeof(what), "Sony speed command for the copy: rotation %u, speed %s", k.rot, orbis_mmc::SieSpeedText(k.speed).c_str());
			if (!Command(pass, orbis_mmc::SieSetReadSpeed(k.rot, k.speed), CAM_DIR_NONE | CAM_PASS_ERR_RECOVER, nullptr, 0, what).code)
			{
				out->set_rotation = k.rot;
				out->set_speed = k.speed;
				out->name = "pread on /dev/cd0 at " + orbis_mmc::SieSpeedText(k.speed) + " (rotation " + std::to_string(k.rot) + ")";
				Log("the copy reads at %.2f MB/s = %.2fx (the drive as it was: %.2f MB/s)", best, best * 1e6 / 1385000.0, base);
			}
		}
		else
		{
			Log("no faster speed setting (best %.2f MB/s, the drive as it was %.2f MB/s): the drive back to 2.0", best, base);
			Command(pass, orbis_mmc::SieSetReadSpeed(0, 0x20), CAM_DIR_NONE | CAM_PASS_ERR_RECOVER, nullptr, 0, "Sony speed command: rotation 0, speed 2.0 (default)");
		}
	}
	if (!OrbisFlag("disc_passbench"))
	{
		close(pass);
		return;
	}

	// vk-285-151: the read methods, 16 MiB each, in fresh parts of the disc (no method reads what another left in the
	// drive's cache)
	constexpr uint64_t kSpan = 16ull << 20, kBase = 64ull << 20;
	std::vector<uint8_t> buf(4u << 20), ref(64u << 10);
	const int pass2 = open_pass();
	std::vector<Bench> runs;
	auto add = [&](uint32_t cmd, bool r12, bool stream, int lanes, const char* name) {
		Bench b;
		b.m.cd = cd;
		b.m.pass[0] = cmd ? pass : -1;
		b.m.pass[1] = lanes > 1 ? pass2 : -1;
		b.m.cmd_bytes = cmd;
		b.m.read12 = r12;
		b.m.streaming = stream;
		b.m.lanes = lanes;
		b.m.name = name;
		runs.push_back(b);
	};
	add(0, false, false, 1, "pread on /dev/cd0, 4 MiB");
	add(64u << 10, false, false, 1, "READ(10) 64 KiB");
	add(128u << 10, false, false, 1, "READ(10) 128 KiB");
	add(256u << 10, false, false, 1, "READ(10) 256 KiB");
	add(1u << 20, false, false, 1, "READ(10) 1 MiB");
	size_t best_single = 0; // index of the fastest single-lane READ(10), for the variants
	auto measure = [&](size_t i) {
		Bench& b = runs[i];
		const uint64_t from = (kBase + i * (kSpan + (8ull << 20))) & ~static_cast<uint64_t>(2047);
		if (from + kSpan > bytes)
		{
			b.why = "the disc is too small for this run";
			Log("method %zu (%s): skipped, %s", i, b.m.name.c_str(), b.why.c_str());
			return;
		}
		if (b.m.cmd_bytes)
		{
			// the method's first 64 KiB against pread's
			std::vector<uint8_t> got(ref.size());
			const int rc = PassRead(b.m, from, got.data(), got.size());
			const bool same = rc == 0 && pread(cd, ref.data(), ref.size(), static_cast<off_t>(from)) == static_cast<ssize_t>(ref.size()) &&
							  memcmp(got.data(), ref.data(), ref.size()) == 0;
			if (!same)
			{
				b.why = rc ? "its read failed (code " + std::to_string(rc) + ")" : "its bytes differ from pread's";
				Log("method %zu (%s): not used, %s", i, b.m.name.c_str(), b.why.c_str());
				return;
			}
		}
		const auto t0 = std::chrono::steady_clock::now();
		uint64_t done = 0;
		int fail = 0;
		for (uint64_t off = from; off < from + kSpan; off += buf.size())
		{
			if (b.m.cmd_bytes)
				fail = PassRead(b.m, off, buf.data(), buf.size());
			else
				fail = pread(cd, buf.data(), buf.size(), static_cast<off_t>(off)) == static_cast<ssize_t>(buf.size()) ? 0 : (errno ? errno : 1);
			if (fail)
				break;
			done += buf.size();
		}
		const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		b.mbs = s > 0 ? done / s / 1e6 : 0;
		b.ok = !fail && done == kSpan;
		Log("method %zu (%s): %s%.2f MB/s = %.2fx DVD (%llu MiB in %.2f s)", i, b.m.name.c_str(),
			b.ok ? "" : ("failed (code " + std::to_string(fail) + ") after ").c_str(), b.mbs, b.mbs * 1e6 / 1385000.0,
			static_cast<unsigned long long>(done >> 20), s);
		if (b.ok && b.m.cmd_bytes && !b.m.read12 && b.m.lanes == 1 &&
			(runs[best_single].m.cmd_bytes == 0 || !runs[best_single].ok || b.mbs > runs[best_single].mbs))
			best_single = i;
	};
	const size_t singles = runs.size();
	for (size_t i = 0; i < singles; i++)
		measure(i);
	{
		// the variants on the fastest READ(10) size
		const uint32_t cmd = runs[best_single].m.cmd_bytes ? runs[best_single].m.cmd_bytes : (128u << 10);
		char n1[64], n2[64], n3[64];
		snprintf(n1, sizeof(n1), "READ(12) Streaming %u KiB", cmd >> 10);
		snprintf(n2, sizeof(n2), "READ(10) %u KiB, 2 lanes", cmd >> 10);
		snprintf(n3, sizeof(n3), "READ(12) Streaming %u KiB, 2 lanes", cmd >> 10);
		add(cmd, true, true, 1, n1);
		if (pass2 >= 0)
		{
			add(cmd, false, false, 2, n2);
			add(cmd, true, true, 2, n3);
		}
	}
	for (size_t i = singles; i < runs.size(); i++)
		measure(i);

	size_t pick = 0;
	for (size_t i = 1; i < runs.size(); i++)
		if (runs[i].ok && runs[i].mbs > runs[pick].mbs)
			pick = i;
	const double base = runs[0].ok ? runs[0].mbs : 0.0;
	if (pick != 0 && runs[pick].mbs > base * 1.15)
	{
		*out = runs[pick].m;
		out->pass[0] = pass;
		out->pass[1] = runs[pick].m.lanes > 1 ? pass2 : -1;
		if (out->pass[1] < 0 && pass2 >= 0)
			close(pass2);
		Log("the copy reads with %s: %.2f MB/s (%.2fx) against /dev/cd0's %.2f MB/s", out->name.c_str(), runs[pick].mbs,
			runs[pick].mbs * 1e6 / 1385000.0, base);
		return;
	}
	Log("the copy reads with pread on /dev/cd0 (no pass method was 15%% faster: best %s at %.2f MB/s, /dev/cd0 %.2f MB/s)",
		runs[pick].m.name.c_str(), runs[pick].mbs, base);
	close(pass);
	if (pass2 >= 0)
		close(pass2);
}

void OrbisDiscReadTest(const OrbisDiscReadMethod& m, uint64_t bytes)
{
	constexpr size_t kChunk = 4u << 20;
	constexpr uint64_t kSpan = 256ull << 20;
	std::vector<uint8_t> buf(kChunk);
	auto run = [&](uint64_t from, const char* where) {
		from &= ~static_cast<uint64_t>(2047);
		const auto t0 = std::chrono::steady_clock::now();
		uint64_t got = 0;
		for (uint64_t off = from; off < from + kSpan && off + kChunk <= bytes; off += kChunk)
		{
			if (OrbisDiscRead(m, off, buf.data(), kChunk) != static_cast<ssize_t>(kChunk))
				break;
			got += kChunk;
		}
		const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		const double mbs = s > 0 ? got / s / 1e6 : 0;
		Log("read test (%s), %s: %llu MB in %.1f s = %.2f MB/s (%.2fx DVD)", m.name.c_str(), where,
			static_cast<unsigned long long>(got >> 20), s, mbs, mbs * 1e6 / 1385000.0);
		return mbs;
	};
	const double a = run(0, "start of the disc");
	const double b = bytes > kSpan ? run(bytes - kSpan, "end of the disc") : 0.0;
	char note[220];
	snprintf(note, sizeof(note), "Disc read test (%s): start %.1f MB/s (%.1fx), end %.1f MB/s (%.1fx)", m.name.c_str(), a, a / 1.385, b,
		b / 1.385);
	OrbisNotifyPlain(note);
}
