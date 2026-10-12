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
// vk-285-152: so before each copy: time pread as the drive is, then each of Sony's own faster settings (152: 1/0x80 8.0,
// 1/0x100, 0/0xFFFF max, 1/0x60 6.0; 154: rotation 1 0x80, then rotation 0 0x32, after the payload tests), 16 MiB each
// after a 4 MiB warm-up; copy at the fastest if it beats the drive as it
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
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <poll.h>
#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <type_traits>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" int sceKernelOpen(const char* path, int flags, int mode);
extern "C" int sceKernelGetdents(int fd, char* buf, int nbytes);
extern "C" int sceKernelClose(int fd);

int OrbisOpenDev(const char* path, int flags, int* kernel_err)
{
	if (kernel_err)
		*kernel_err = 0;
	const int fd = open(path, flags);
	if (fd >= 0)
		return fd;
	const int k = sceKernelOpen(path, flags, 0);
	if (k >= 0)
		return k;
	if (kernel_err)
		*kernel_err = k;
	return -1;
}

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
		// vk-285-153: PS5SX2DiscTest v2/v3 on swordpdf's drive (PS-SYSTEM 503R 2305, a PS2 DVD, profile 0x10): rotation 1
		// (Sony's BD entries) and rotation 2/3 refused (asc 0x24); rotation 0 took 0x20, 0x32 and 0xFFFF and refused 0x50
		// and 0x80; 0x32 read at 3.20x (4.43 MB/s) at both ends of the disc, against 2.0x, and 0xFFFF changed nothing.
		// vk-285-154: test payloads v4-v7 on that drive: rotation 0 takes only 0x20, 0x32 and 0xFFFF; every other value and
		// rotation, every other CDB bit, and Sony's changeable mode pages 31/32 change nothing (page 2F's fixed "04 32" looks
		// like the firmware's DVD ceiling). So: Sony's 8.0 (rotation 1) first for a drive that takes it for a DVD (another
		// console's; a refusal costs one command), then 3.2.
		static const Setting kTry[] = {{1, 0x0080}, {0, 0x0032}};
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

namespace
{
	int OpenPass(int cd)
	{
		const std::string path = PassDevice(cd, true);
		if (path.empty())
			return -1;
		const int pass = open(path.c_str(), O_RDWR);
		if (pass < 0)
			Log("pass device %s: open errno %d", path.c_str(), errno);
		return pass;
	}
}

// `pass` stays open. `media_fd` is asked for DIOCGMEDIASIZE (the cd node, or the same pass fd).
static bool LayerFromPass(int pass, int media_fd, OrbisDiscLayer* out)
{
	uint8_t buf[32] = {};
	const auto read_layer = [&](uint8_t layer, orbis_mmc::DvdPhysical& parsed) {
		std::memset(buf, 0, sizeof(buf));
		char what[64];
		std::snprintf(what, sizeof(what), "READ DVD STRUCTURE layer %u", layer);
		if (Command(pass, orbis_mmc::ReadDvdStructure(layer, sizeof(buf)), CAM_DIR_IN, buf, sizeof(buf), what).code)
			return false;
		parsed = orbis_mmc::ParseDvdPhysical(buf, sizeof(buf));
		return parsed.ok;
	};
	orbis_mmc::DvdPhysical layer0, layer1;
	if (!read_layer(0, layer0))
	{
		out->why = "READ DVD STRUCTURE failed";
		Log("DVD structure: %s", out->why.c_str());
		return false;
	}
	const orbis_mmc::DvdPhysical* second = nullptr;
	if (layer0.layers != 0 && layer0.track_path == 0)
	{
		if (!read_layer(1, layer1))
		{
			out->why = "READ DVD STRUCTURE layer 1 failed";
			Log("DVD structure: %s", out->why.c_str());
			return false;
		}
		second = &layer1;
	}
	const orbis_mmc::DvdLayout layout = orbis_mmc::DvdLayoutFrom(layer0, second);
	if (!layout.ok)
	{
		out->why = "DVD structure is not a PS2 DVD layout";
		Log("DVD structure: %s (layers %u, track path %u, start %u, end %u, end layer 0 %u)", out->why.c_str(), layer0.layers,
			layer0.track_path, layer0.start, layer0.end, layer0.end_l0);
		return false;
	}
	out->ok = true;
	out->media_type = layout.media_type;
	out->sectors = layout.sectors;
	out->layer_break = layout.layer_break;
	uint64_t from_media = 0;
	off_t media = 0;
	if (media_fd >= 0 && ioctl(media_fd, DIOCGMEDIASIZE, &media) == 0 && media > 0)
		from_media = static_cast<uint64_t>(media) / 2048;
	else if (media_fd >= 0)
		from_media = OrbisPassCapacity(media_fd);
	if (from_media > out->sectors && from_media <= 0xFFFFFFFFull)
	{
		Log("DVD structure %u sectors, media size %llu sectors: using the media size", out->sectors,
			static_cast<unsigned long long>(from_media));
		out->sectors = static_cast<uint32_t>(from_media);
	}
	static const char* const kinds[] = {"single layer", "parallel dual layer", "opposite dual layer"};
	Log("DVD structure: %s, %u sectors, layer break %u", kinds[out->media_type], out->sectors, out->layer_break);
	return true;
}

std::string OrbisDiscPassPath(int cd)
{
	return PassDevice(cd, true);
}

bool OrbisDiscReadLayer(int cd, OrbisDiscLayer* out)
{
	OrbisDiscLayer empty;
	*out = empty;
	const int pass = OpenPass(cd);
	if (pass < 0)
	{
		out->why = "no pass device";
		Log("DVD structure: %s", out->why.c_str());
		return false;
	}
	const bool ok = LayerFromPass(pass, cd, out);
	close(pass);
	return ok;
}

bool OrbisDiscSetPlaySpeed(int cd, bool restore)
{
	if (OrbisFlag("nodiscspeed"))
	{
		Log("play speed left as it is (flag nodiscspeed)");
		return true;
	}
	const int pass = OpenPass(cd);
	if (pass < 0)
	{
		Log("play speed: no pass device");
		return false;
	}
	const uint16_t speed = restore ? 0x20 : 0x32;
	char what[96];
	std::snprintf(what, sizeof(what), "Sony speed command: rotation 0, speed %s (%s)", orbis_mmc::SieSpeedText(speed).c_str(),
		restore ? "back to 2.0 after playing" : "3.2x to play from the disc");
	const bool ok = !Command(pass, orbis_mmc::SieSetReadSpeed(0, speed), CAM_DIR_NONE | CAM_PASS_ERR_RECOVER, nullptr, 0, what).code;
	close(pass);
	return ok;
}

namespace
{
	uint32_t Be32(const uint8_t* p)
	{
		return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
	}

	std::vector<uint8_t> ReadCdCdb(uint32_t lba, uint32_t count, uint8_t flags)
	{
		std::vector<uint8_t> c(12, 0);
		c[0] = 0xBE;
		c[2] = static_cast<uint8_t>(lba >> 24);
		c[3] = static_cast<uint8_t>(lba >> 16);
		c[4] = static_cast<uint8_t>(lba >> 8);
		c[5] = static_cast<uint8_t>(lba);
		c[6] = static_cast<uint8_t>(count >> 16);
		c[7] = static_cast<uint8_t>(count >> 8);
		c[8] = static_cast<uint8_t>(count);
		c[9] = flags; // 0x10 user data (2048), 0xF8 raw sector (2352)
		return c;
	}
}

bool OrbisPassDescribe(int pass, OrbisPassInfo* out)
{
	OrbisPassInfo empty;
	*out = empty;
	uint8_t inq[36] = {};
	std::vector<uint8_t> c(6, 0);
	c[0] = 0x12;
	c[4] = sizeof(inq);
	if (Command(pass, c, CAM_DIR_IN, inq, sizeof(inq), "INQUIRY", true).code)
		return false;
	out->optical = (inq[0] & 0x1f) == 5;
	char text[64];
	std::snprintf(text, sizeof(text), "%.8s | %.16s | %.4s", inq + 8, inq + 16, inq + 32);
	out->text = text;
	out->sony = out->text.find("PS-SYSTEM") != std::string::npos;
	return true;
}

void OrbisPassReleaseQueue(int pass)
{
	union ccb ccb;
	std::memset(&ccb, 0, sizeof(ccb));
	ccb.ccb_h.func_code = XPT_REL_SIMQ;
	ccb.ccb_h.flags = CAM_DEV_QFREEZE;
	if (ioctl(pass, CAMIOCOMMAND, &ccb) != 0)
		return;
	const int n = static_cast<int>(ccb.crs.qfrozen_cnt);
	if (n <= 0)
		return;
	Log("usb: device queue frozen count %d", n);
	for (int i = 0; i < n && i < 8; i++)
	{
		std::memset(&ccb, 0, sizeof(ccb));
		ccb.ccb_h.func_code = XPT_REL_SIMQ;
		ioctl(pass, CAMIOCOMMAND, &ccb);
	}
}

bool OrbisPassReady(int pass)
{
	const std::vector<uint8_t> c(6, 0);
	return !Command(pass, c, CAM_DIR_NONE, nullptr, 0, "TEST UNIT READY", true).code;
}

uint32_t OrbisPassCapacity(int pass)
{
	uint8_t r[8] = {};
	std::vector<uint8_t> c(10, 0);
	c[0] = 0x25;
	if (Command(pass, c, CAM_DIR_IN, r, sizeof(r), "READ CAPACITY", true).code)
		return 0;
	if (Be32(r + 4) != 2048)
		return 0;
	const uint32_t last = Be32(r);
	return last == 0xFFFFFFFFu ? 0 : last + 1;
}

bool OrbisPassRead2048(int pass, uint32_t lba, uint32_t count, void* buf)
{
	auto* p = static_cast<uint8_t*>(buf);
	while (count)
	{
		const uint32_t n = count > 32 ? 32 : count;
		const uint32_t bytes = n * 2048;
		if (Command(pass, orbis_mmc::Read10(lba, static_cast<uint16_t>(n)), CAM_DIR_IN, p, bytes, "READ(10)", true).code &&
			Command(pass, orbis_mmc::Read12(lba, n, false), CAM_DIR_IN, p, bytes, "READ(12)", true).code)
			return false;
		p += bytes;
		lba += n;
		count -= n;
	}
	return true;
}

bool OrbisPassReadCdUser(int pass, uint32_t lba, uint32_t count, void* buf)
{
	auto* p = static_cast<uint8_t*>(buf);
	while (count)
	{
		const uint32_t n = count > 16 ? 16 : count;
		if (Command(pass, ReadCdCdb(lba, n, 0x10), CAM_DIR_IN, p, n * 2048, "READ CD user", true).code)
			return false;
		p += n * 2048;
		lba += n;
		count -= n;
	}
	return true;
}

bool OrbisPassRead2352(int pass, uint32_t lba, uint32_t count, void* buf)
{
	auto* p = static_cast<uint8_t*>(buf);
	while (count)
	{
		const uint32_t n = count > 16 ? 16 : count;
		if (Command(pass, ReadCdCdb(lba, n, 0xF8), CAM_DIR_IN, p, n * 2352, "READ CD raw", true).code)
			return false;
		p += n * 2352;
		lba += n;
		count -= n;
	}
	return true;
}

bool OrbisPassReadCdToc(int pass, OrbisCdToc* out)
{
	OrbisCdToc empty;
	*out = empty;
	uint8_t d[2048] = {};
	std::vector<uint8_t> c(10, 0);
	c[0] = 0x43;
	c[7] = sizeof(d) >> 8;
	c[8] = sizeof(d) & 0xff;
	if (Command(pass, c, CAM_DIR_IN, d, sizeof(d), "READ TOC", true).code)
		return false;
	unsigned total = (static_cast<unsigned>(d[0]) << 8 | d[1]) + 2;
	if (total > sizeof(d))
		total = sizeof(d);
	bool leadout = false;
	for (unsigned i = 4; i + 8 <= total && out->count < 99; i += 8)
	{
		const uint8_t num = d[i + 2];
		const uint32_t lba = Be32(d + i + 4);
		if (num == 0xAA)
		{
			out->leadout = lba;
			leadout = true;
			break;
		}
		if (num == 0)
			continue;
		OrbisCdTrack& t = out->tracks[out->count++];
		t.number = num;
		t.lba = lba;
		// READ TOC byte 1 is ADR in the high nibble and CONTROL in the low nibble (0x14 = ADR 1, data track).
		// They were swapped, so the data track was reported as audio and the BIOS went to its menu.
		t.adr = static_cast<uint8_t>(d[i + 1] >> 4);
		t.control = static_cast<uint8_t>(d[i + 1] & 0x0f);
	}
	out->ok = leadout && out->count > 0;
	if (!out->ok)
		Log("READ TOC: no lead-out or no track");
	return out->ok;
}

bool OrbisPassSetSpeed(int pass, uint16_t read_kbs)
{
	if (OrbisFlag("nodiscspeed"))
	{
		Log("usb play speed left as it is (flag nodiscspeed)");
		return true;
	}
	char what[80];
	std::snprintf(what, sizeof(what), "SET CD SPEED %s", read_kbs == 0xFFFF ? "default" : "for play");
	return !Command(pass, orbis_mmc::SetCdSpeed(read_kbs), CAM_DIR_NONE | CAM_PASS_ERR_RECOVER, nullptr, 0, what).code;
}

bool OrbisDiscReadLayerPass(int pass, OrbisDiscLayer* out)
{
	OrbisDiscLayer empty;
	*out = empty;
	return LayerFromPass(pass, pass, out);
}

// libSceUsbd bulk-only transport. The sandbox /dev listing has no pass node; the USB stack still enumerates the drive.
extern "C" int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags, void* opt, int* res);
extern "C" int sceKernelDlsym(int handle, const char* symbol, void** addrp);
extern "C" int sceKernelGetModuleList(int32_t* handles, size_t max, size_t* count);

namespace
{

struct UsbMs
{
	bool loaded = false;
	bool open = false;
	bool bulk_proven = false;
	void* handle = nullptr;
	uint8_t ep_in = 0, ep_out = 0;
	int iface = -1;
	bool detached = false;
	uint32_t tag = 1;
	std::string text;
	int (*Init)() = nullptr;
	long (*GetDeviceList)(void*** list) = nullptr;
	void (*FreeDeviceList)(void** list, int unref) = nullptr;
	int (*GetDeviceDescriptor)(void* dev, void* desc) = nullptr;
	int (*Open)(void* dev, void** handle) = nullptr;
	void (*Close)(void* handle) = nullptr;
	int (*KernelDriverActive)(void* handle, int iface) = nullptr;
	int (*DetachKernelDriver)(void* handle, int iface) = nullptr;
	int (*AttachKernelDriver)(void* handle, int iface) = nullptr;
	int (*ClaimInterface)(void* handle, int iface) = nullptr;
	int (*ReleaseInterface)(void* handle, int iface) = nullptr;
	int (*SetConfiguration)(void* handle, int config) = nullptr;
	int (*ControlTransfer)(void* handle, uint8_t type, uint8_t req, uint16_t value, uint16_t index, unsigned char* data, uint16_t len,
		unsigned int timeout) = nullptr;
	int (*BulkTransfer)(void* handle, unsigned char ep, unsigned char* data, int len, int* done, unsigned int timeout) = nullptr;
	void* (*AllocTransfer)(int iso_packets) = nullptr;
	void (*FreeTransfer)(void* transfer) = nullptr;
	int (*SubmitTransfer)(void* transfer) = nullptr;
	int (*HandleEventsTimeout)(void* timeval) = nullptr;
	std::string fail;
} g_usb;

// libusb 1.0's transfer, 64-bit. sceUsbdBulkTransfer is a wrapper around this; some builds do not export the wrapper.
struct UsbTransfer
{
	void* dev_handle;
	uint8_t flags;
	uint8_t endpoint;
	uint8_t type;
	uint8_t pad0;
	uint32_t timeout;
	int status;
	int length;
	int actual_length;
	uint32_t pad1;
	void (*callback)(UsbTransfer*);
	void* user_data;
	unsigned char* buffer;
	int num_iso_packets;
};
static_assert(offsetof(UsbTransfer, callback) == 32, "UsbTransfer callback offset");
static_assert(offsetof(UsbTransfer, buffer) == 48, "UsbTransfer buffer offset");

extern "C" void OrbisUsbSyncCb(UsbTransfer* transfer)
{
	if (transfer && transfer->user_data)
		*static_cast<int*>(transfer->user_data) = 1;
}

bool UsbCanSubmit()
{
	return g_usb.AllocTransfer && g_usb.FreeTransfer && g_usb.SubmitTransfer && g_usb.HandleEventsTimeout;
}
std::mutex g_usb_mu;

template <typename F>
void UsbSym(int module, const char* name, F& fn)
{
	void* a = nullptr;
	if (sceKernelDlsym(module, name, &a) == 0 && a)
		fn = reinterpret_cast<F>(a);
}

bool UsbMarkerBlocked()
{
	const std::string path = OrbisLogPath("usbdisc-marker.txt");
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char line[32] = {};
	const size_t n = fread(line, 1, sizeof(line) - 1, f);
	fclose(f);
	line[n] = 0;
	return strncmp(line, "started", 7) == 0;
}

void UsbMarker(const char* text)
{
	if (FILE* f = fopen(OrbisLogPath("usbdisc-marker.txt").c_str(), "wb"))
	{
		fputs(text, f);
		fputc('\n', f);
		fclose(f);
	}
}

template <typename F>
F UsbExport(const char* name)
{
	int32_t handles[256];
	size_t count = 0;
	if (sceKernelGetModuleList(handles, 256, &count) != 0)
		return nullptr;
	for (size_t i = 0; i < count && i < 256; i++)
	{
		void* address = nullptr;
		if (sceKernelDlsym(handles[i], name, &address) == 0 && address)
			return reinterpret_cast<F>(address);
	}
	return nullptr;
}

// Firmware 12 refuses sceKernelLoadStartModule for a system library (0x80020063). libkernel's sysmodule loader is the
// path the keyboard uses. One try: a call that does not return is not repeated.
int UsbLoadForSysmodule(const char* path)
{
	using LoadStart = int (*)(const char*, size_t, const void*, uint32_t, void*, int*);
	const std::string marker = OrbisLogPath("usbdisc-sysmodule.txt");
	if (FILE* f = fopen(marker.c_str(), "rb"))
	{
		char line[32] = {};
		const size_t n = fread(line, 1, sizeof(line) - 1, f);
		fclose(f);
		line[n] = 0;
		if (strncmp(line, "started", 7) == 0)
		{
			g_usb.fail = "USB drive: sysmodule load did not return";
			return -1;
		}
	}
	const LoadStart fn = UsbExport<LoadStart>("sceKernelLoadStartModuleForSysmodule");
	if (!fn)
	{
		Log("libSceUsbd: sceKernelLoadStartModuleForSysmodule is not in libkernel");
		return -1;
	}
	if (FILE* f = fopen(marker.c_str(), "wb"))
	{
		fputs("started\n", f);
		fclose(f);
	}
	int res = 0;
	const int handle = fn(path, 0, nullptr, 0, nullptr, &res);
	if (FILE* f = fopen(marker.c_str(), "wb"))
	{
		std::fprintf(f, "done %#x\n", static_cast<unsigned>(handle));
		fclose(f);
	}
	Log("libSceUsbd: ForSysmodule %s: %#x (start %d)", path, static_cast<unsigned>(handle), res);
	return handle;
}

bool UsbResolve(int module)
{
	const auto take = [&](const char* name, auto& fn) {
		if (module >= 0)
			UsbSym(module, name, fn);
		if (!fn)
			fn = UsbExport<std::remove_reference_t<decltype(fn)>>(name);
	};
	take("sceUsbdInit", g_usb.Init);
	take("sceUsbdGetDeviceList", g_usb.GetDeviceList);
	take("sceUsbdFreeDeviceList", g_usb.FreeDeviceList);
	take("sceUsbdGetDeviceDescriptor", g_usb.GetDeviceDescriptor);
	take("sceUsbdOpen", g_usb.Open);
	take("sceUsbdClose", g_usb.Close);
	take("sceUsbdKernelDriverActive", g_usb.KernelDriverActive);
	take("sceUsbdDetachKernelDriver", g_usb.DetachKernelDriver);
	take("sceUsbdAttachKernelDriver", g_usb.AttachKernelDriver);
	take("sceUsbdClaimInterface", g_usb.ClaimInterface);
	take("sceUsbdReleaseInterface", g_usb.ReleaseInterface);
	take("sceUsbdSetConfiguration", g_usb.SetConfiguration);
	take("sceUsbdControlTransfer", g_usb.ControlTransfer);
	take("sceUsbdBulkTransfer", g_usb.BulkTransfer);
	take("sceUsbdAllocTransfer", g_usb.AllocTransfer);
	take("sceUsbdFreeTransfer", g_usb.FreeTransfer);
	take("sceUsbdSubmitTransfer", g_usb.SubmitTransfer);
	take("sceUsbdHandleEventsTimeout", g_usb.HandleEventsTimeout);
	if (!g_usb.Init || !g_usb.GetDeviceList || !g_usb.Open || !g_usb.Close || !g_usb.ControlTransfer || !g_usb.ClaimInterface)
	{
		if (module >= 0 || g_usb.Init)
			g_usb.fail = "USB drive: libSceUsbd is missing a call";
		return false;
	}
	if (!g_usb.BulkTransfer && !UsbCanSubmit())
	{
		g_usb.fail = "USB drive: no bulk transfer";
		Log("libSceUsbd: bulk missing, submit %d", g_usb.SubmitTransfer ? 1 : 0);
		return false;
	}
	const int rc = g_usb.Init();
	Log("libSceUsbd: sceUsbdInit %#x", static_cast<unsigned>(rc));
	if (rc < 0)
	{
		char msg[80];
		std::snprintf(msg, sizeof(msg), "USB drive: sceUsbdInit %x", static_cast<unsigned>(rc));
		g_usb.fail = msg;
		return false;
	}
	return true;
}

bool UsbLoad()
{
	if (g_usb.loaded)
		return g_usb.fail.empty() && (g_usb.BulkTransfer != nullptr || UsbCanSubmit());
	g_usb.loaded = true;
	if (OrbisFlag("nousbd"))
	{
		g_usb.fail = "USB drive: libSceUsbd skipped";
		Log("libSceUsbd: skipped (flag nousbd)");
		return false;
	}
	int module = -1;
	unsigned last = 0;
	bool sdk_refused = false;
	std::string first;
	static const char* const dirs[] = {"/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/",
		"/system_ex/priv_ex/lib/"};
	for (const char* dir : dirs)
	{
		const std::string path = std::string(dir) + "libSceUsbd.sprx";
		if (first.empty())
			first = path;
		int res = 0;
		const int handle = sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, &res);
		Log("libSceUsbd: %s: load %#x (start %d)", path.c_str(), static_cast<unsigned>(handle), res);
		last = static_cast<unsigned>(handle);
		if (handle >= 0)
		{
			module = handle;
			break;
		}
		if (last == 0x80020063u)
			sdk_refused = true;
	}
	if (module < 0 && (sdk_refused || last != 0))
		module = UsbLoadForSysmodule(first.c_str());
	if (module < 0 && !g_usb.fail.empty())
		return false;
	if (module < 0)
	{
		using LoadModule = int (*)(uint16_t);
		if (const LoadModule load = UsbExport<LoadModule>("sceSysmoduleLoadModule"))
		{
			const int rc = load(0xB7); // libSceUsbd
			Log("libSceUsbd: sceSysmoduleLoadModule %#x", static_cast<unsigned>(rc));
			last = static_cast<unsigned>(rc);
		}
	}
	if (module >= 0)
	{
		if (!UsbResolve(module))
			return false;
	}
	else if (!UsbResolve(-1))
	{
		if (g_usb.fail.empty())
		{
			char msg[80];
			std::snprintf(msg, sizeof(msg), "USB drive: libSceUsbd load %x", last);
			g_usb.fail = msg;
		}
		return false;
	}
	return g_usb.fail.empty() && (g_usb.BulkTransfer != nullptr || UsbCanSubmit());
}

bool UsbXferSubmit(uint8_t ep, void* data, int len, unsigned timeout)
{
	UsbTransfer* transfer = static_cast<UsbTransfer*>(g_usb.AllocTransfer(0));
	if (!transfer)
		return false;
	transfer->dev_handle = g_usb.handle;
	transfer->endpoint = ep;
	transfer->type = 2; // bulk
	transfer->timeout = timeout;
	transfer->length = len;
	transfer->buffer = static_cast<unsigned char*>(data);
	transfer->user_data = nullptr;
	transfer->callback = OrbisUsbSyncCb;
	int completed = 0;
	transfer->user_data = &completed;
	const int rc = g_usb.SubmitTransfer(transfer);
	if (rc < 0)
	{
		g_usb.FreeTransfer(transfer);
		return false;
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout + 1000);
	while (!completed)
	{
		struct Tv
		{
			long sec;
			long usec;
		} tv{0, 200000};
		if (g_usb.HandleEventsTimeout(&tv) < 0 || std::chrono::steady_clock::now() > deadline)
			break;
	}
	const bool ok = completed && transfer->status == 0 && transfer->actual_length == len;
	g_usb.FreeTransfer(transfer);
	return ok;
}

int g_ugen_fd = -1;
int g_ugen_out_fd = -1;
int g_ugen_in_fd = -1;
int g_ugen_iface = -1;
bool g_ugen_claimed = false;
uint32_t g_fs_max = 2048;
usb_fs_endpoint g_ugen_ep[2];
bool g_ugen = false;
bool g_ugen_rw = false;
bool g_fs_complete = false;

bool UgenRw(uint8_t ep, void* data, int len, unsigned timeout)
{
	const int fd = ep == g_usb.ep_out ? g_ugen_out_fd : g_ugen_in_fd;
	if (fd < 0 || len <= 0)
		return false;
	pollfd p = {fd, static_cast<short>(ep == g_usb.ep_out ? POLLOUT : POLLIN), 0};
	if (poll(&p, 1, timeout > 60000u ? 60000 : static_cast<int>(timeout)) <= 0)
		return false;
	uint8_t* bytes = static_cast<uint8_t*>(data);
	int left = len;
	while (left > 0)
	{
		const int n = ep == g_usb.ep_out ? static_cast<int>(write(fd, bytes, static_cast<size_t>(left))) :
										   static_cast<int>(read(fd, bytes, static_cast<size_t>(left)));
		if (n <= 0)
			return false;
		bytes += n;
		left -= n;
	}
	return true;
}

bool g_fs_zlp = false;
bool g_pipe_dead = false;
bool g_fs_busy = false;
bool g_fs_need_csw = false;
// Set when a transfer on that endpoint ended in error. The controller halts the endpoint; the next transfer asks the
// kernel to clear the halt first (USB_FS_FLAG_CLEAR_STALL). Needs proper testing.
bool g_fs_halted[2] = {false, false};
uint8_t g_fs_busy_ep = 0;
std::chrono::steady_clock::time_point g_fs_busy_since;

// The kernel keeps the previous IN buffer. A status packet was written over the sector (USBS at the start of a CD
// sync). One IN buffer and one OUT buffer stay in place for the whole session. The caller gets a copy. Needs proper testing.
uint8_t g_bounce_in[37632];
uint8_t g_bounce_out[4096];
void* g_bounce_in_ptr = g_bounce_in;
void* g_bounce_out_ptr = g_bounce_out;
uint32_t g_bounce_in_len = 0;
uint32_t g_bounce_out_len = 0;

void UsbFsDrain(uint8_t ep_index)
{
	usb_fs_stop stop = {};
	stop.ep_index = ep_index;
	ioctl(g_ugen_fd, USB_FS_STOP, &stop);
	for (int i = 0; i < 6; i++)
	{
		usb_fs_complete done = {};
		done.ep_index = ep_index;
		if (ioctl(g_ugen_fd, USB_FS_COMPLETE, &done) == 0 || errno != EBUSY)
			return;
		pollfd p = {g_ugen_fd, POLLIN | POLLOUT, 0};
		poll(&p, 1, 50);
	}
}

bool UgenFs(uint8_t ep_index, void* data, int len, unsigned timeout, bool allow_short)
{
	if (g_pipe_dead || len < 0)
		return false;
	const bool inbound = ep_index == g_usb.ep_in;
	uint8_t* bounce = inbound ? g_bounce_in : g_bounce_out;
	const int cap = inbound ? static_cast<int>(sizeof(g_bounce_in)) : static_cast<int>(sizeof(g_bounce_out));
	if (len > cap)
	{
		Log("USB_FS ep %u ask %d larger than the bounce buffer", ep_index, len);
		return false;
	}
	if (inbound && len > 0)
		std::memset(bounce, 0, static_cast<size_t>(len));
	if (!inbound && data && len > 0)
		std::memcpy(bounce, data, static_cast<size_t>(len));
	if (inbound)
		g_bounce_in_len = static_cast<uint32_t>(len);
	else
		g_bounce_out_len = static_cast<uint32_t>(len);
	usb_fs_endpoint& ep = g_ugen_ep[ep_index];
	ep.ppBuffer = inbound ? &g_bounce_in_ptr : &g_bounce_out_ptr;
	ep.pLength = inbound ? &g_bounce_in_len : &g_bounce_out_len;
	ep.nFrames = 1;
	ep.aFrames = 0;
	ep.flags = allow_short ? USB_FS_FLAG_SINGLE_SHORT_OK : 0;
	if (g_fs_halted[ep_index & 1])
	{
		ep.flags |= USB_FS_FLAG_CLEAR_STALL;
		g_fs_halted[ep_index & 1] = false;
		static int said = 0;
		if (said++ < 6)
			Log("USB_FS ep %u: clearing the halt before this transfer", ep_index);
	}
	ep.timeout = timeout > 65535u ? 65535u : static_cast<uint16_t>(timeout);
	ep.status = 0;
	g_fs_complete = false;
	g_fs_zlp = false;
	usb_fs_start st = {};
	st.ep_index = ep_index;
	if (ioctl(g_ugen_fd, USB_FS_START, &st) != 0)
	{
		const int start_errno = errno;
		if (start_errno == EBUSY)
		{
			UsbFsDrain(ep_index);
			if (ioctl(g_ugen_fd, USB_FS_START, &st) == 0)
				goto started;
		}
		if (start_errno == ENXIO)
			g_pipe_dead = true;
		static int logged = 0;
		if (logged++ < 4)
			Log("USB_FS start ep %u ask %d errno %d", ep_index, len, start_errno);
		return false;
	}
started:
	uint32_t& live_len = inbound ? g_bounce_in_len : g_bounce_out_len;
	if (live_len != static_cast<uint32_t>(len))
		Log("USB_FS start ep %u ask %d now %u", ep_index, len, live_len);
	auto publish = [&](uint32_t got) {
		if (!inbound || !data || got == 0)
			return;
		const uint32_t n = got < static_cast<uint32_t>(len) ? got : static_cast<uint32_t>(len);
		std::memcpy(data, bounce, n);
	};
	// A CD seek can take several seconds. Leaving after 1.6 s kept this transfer running, and the next command
	// read its status packet as the sector. Needs proper testing.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout < 1 ? 1 : timeout);
	// The bounce is zeroed before START, so bytes in it after COMPLETE came from this transfer.
	const auto bounce_has_data = [&]() {
		const unsigned n = len < 16 ? static_cast<unsigned>(len) : 16u;
		for (unsigned i = 0; i < n; i++)
			if (bounce[i])
				return true;
		return false;
	};
	const auto hex_front = [&](char* hex, size_t hex_n, uint32_t got) {
		std::memset(hex, 0, hex_n);
		const unsigned n = got < 16 ? got : 16;
		for (unsigned i = 0; i < n && i * 3 + 4 <= hex_n; i++)
			std::snprintf(hex + i * 3, 4, "%02x ", bounce[i]);
	};
	int empty_completions = 0;
	for (;;)
	{
		usb_fs_complete done = {};
		done.ep_index = ep_index;
		if (ioctl(g_ugen_fd, USB_FS_COMPLETE, &done) == 0)
		{
			// USB_FS_COMPLETE is _IOR: the kernel reports which endpoint finished. An entry for the other
			// endpoint is stale (nothing else is in flight); keep waiting for ours. Needs proper testing.
			if (done.ep_index != ep_index)
			{
				static int other = 0;
				if (other++ < 6)
					Log("USB_FS waiting on ep %u, got a completion for ep %u", ep_index, done.ep_index);
				continue;
			}
			const uint32_t got = live_len;
			if (ep.status == 0 && got == 0)
			{
				// 13 bytes asked and a zero length reported, but the status packet is in the buffer: the length
				// report is wrong, not the packet.
				if (inbound && len == 13 && bounce[0] == 0x55 && bounce[1] == 0x53 && bounce[2] == 0x42 && bounce[3] == 0x53)
				{
					static int said = 0;
					if (said++ < 4)
						Log("USB_FS ep %u length 0 but the status packet is in the buffer (aFrames %u)", ep_index, ep.aFrames);
					publish(13);
					return true;
				}
				// No frame completed: the entry was not for a finished transfer. Give the real completion a moment.
				if (ep.aFrames == 0 && empty_completions++ < 1)
				{
					static int said = 0;
					if (said++ < 6)
						Log("USB_FS ep %u empty completion (asked %d, aFrames 0); waiting again", ep_index, len);
					const auto again = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
					bool finished = false;
					while (std::chrono::steady_clock::now() < again)
					{
						pollfd p = {g_ugen_fd, POLLIN | POLLOUT, 0};
						poll(&p, 1, 50);
						usb_fs_complete more = {};
						if (ioctl(g_ugen_fd, USB_FS_COMPLETE, &more) == 0)
						{
							if (more.ep_index != ep_index)
								continue;
							finished = true;
							break;
						}
						if (errno != EBUSY)
							break;
					}
					if (!finished)
					{
						g_fs_zlp = true;
						return false;
					}
					const uint32_t got2 = live_len;
					if (got2 > 0 && ep.status == 0)
					{
						publish(got2);
						if (got2 == static_cast<uint32_t>(len))
							return true;
						g_fs_complete = true;
						return false;
					}
					g_fs_zlp = true;
					return false;
				}
				// A status read of 0 bytes is a zero-length packet in front of the real 13-byte packet. Do not stop the
				// pipe; the caller reads again. Stopping it is what made the next 2048-byte read busy. Needs proper testing.
				g_fs_zlp = true;
				static int zlp_logged = 0;
				if (zlp_logged++ < 4)
					Log("USB_FS ep %u zero-length packet (asked %d, aFrames %u)", ep_index, len, ep.aFrames);
				return false;
			}
			// Status 0 with a length other than asked: a short reply; the kernel's count is off (513 of 804 for a
			// 20-byte TOC) but the front of the buffer is right. Status 21 (short) and 20 (timeout) do not copy
			// data on this kernel as far as seen; the length cell is then just what was asked. Only trust those
			// when bytes are in the buffer. Needs proper testing.
			const bool has_data = inbound ? bounce_has_data() : true;
			if (ep.status != 0)
				g_fs_halted[ep_index & 1] = true;
			bool delivered = false;
			if (ep.status == 0 && got > 0)
				delivered = true;
			else if ((ep.status == 21 || ep.status == 20) && got > 0 && has_data)
				delivered = true;
			if (delivered)
				publish(got);
			if (delivered && got == static_cast<uint32_t>(len) && ep.status == 0)
				return true;
			if (delivered)
				g_fs_complete = true;
			static int logged = 0;
			if (logged++ < 12)
			{
				char hex[80];
				hex_front(hex, sizeof(hex), got > 0 ? got : 16);
				Log("USB_FS ep %u status %d got %u/%d aFrames %u [%s]", ep_index, ep.status, got, len, ep.aFrames, hex);
			}
			return false;
		}
		if (errno == ENXIO)
		{
			g_pipe_dead = true;
			static int dead = 0;
			if (dead++ < 2)
				Log("USB_FS ep %u complete errno 6; pipe closed", ep_index);
			return false;
		}
		if (errno != EBUSY)
		{
			static int logged = 0;
			if (logged++ < 8)
				Log("USB_FS ep %u complete errno %d", ep_index, errno);
			return false;
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			g_fs_busy = true;
			g_fs_busy_ep = ep_index;
			g_fs_busy_since = std::chrono::steady_clock::now();
			static int waited = 0;
			if (waited++ < 4)
				Log("USB_FS ep %u still running after %u ms", ep_index, timeout);
			return false;
		}
		pollfd p = {g_ugen_fd, POLLIN | POLLOUT, 0};
		poll(&p, 1, 200);
	}
}

void UgenClose(bool claimed, int iface)
{
	if (g_ugen_fd < 0)
		return;
	for (uint8_t i = 0; i < 2; i++)
	{
		usb_fs_close c = {};
		c.ep_index = i;
		ioctl(g_ugen_fd, USB_FS_CLOSE, &c);
	}
	usb_fs_uninit u = {};
	ioctl(g_ugen_fd, USB_FS_UNINIT, &u);
	if (claimed)
		ioctl(g_ugen_fd, USB_RELEASE_INTERFACE, &iface);
	if (g_ugen_out_fd >= 0)
		close(g_ugen_out_fd);
	if (g_ugen_in_fd >= 0)
		close(g_ugen_in_fd);
	g_ugen_out_fd = g_ugen_in_fd = -1;
	close(g_ugen_fd);
	g_ugen_fd = -1;
	g_ugen = false;
	g_ugen_rw = false;
	g_ugen_claimed = false;
	g_ugen_iface = -1;
	g_fs_max = 2048;
	g_usb.open = false;
	g_usb.handle = nullptr;
}

bool UsbBot(const uint8_t* cdb, int cdb_len, bool data_in, void* buf, uint32_t len, unsigned timeout, bool may_reset = true);

bool UgenFind(std::string* what, std::string* why)
{
	static std::string last_why;
	static std::chrono::steady_clock::time_point next_try{};
	if (std::chrono::steady_clock::now() < next_try)
	{
		*why = last_why.empty() ? "USB drive: no optical device on ugen" : last_why;
		return false;
	}
	const int dev = sceKernelOpen("/dev", O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
	if (dev < 0)
	{
		*why = "USB drive: can't list /dev";
		return false;
	}
	alignas(0x4000) static char buffer[0x10000];
	std::vector<std::string> names;
	for (;;)
	{
		const int n = sceKernelGetdents(dev, buffer, static_cast<int>(sizeof(buffer)));
		if (n <= 0)
			break;
		for (size_t off = 0; off + 8 <= static_cast<size_t>(n);)
		{
			char* p = buffer + off;
			const size_t reclen = static_cast<unsigned char>(p[4]) | (static_cast<size_t>(static_cast<unsigned char>(p[5])) << 8);
			const size_t namlen = static_cast<unsigned char>(p[7]);
			if (reclen < 12 || off + reclen > static_cast<size_t>(n) || 8 + namlen >= reclen)
				break;
			names.emplace_back(p + 8, namlen);
			off += reclen;
		}
	}
	sceKernelClose(dev);
	int looked = 0;
	for (const std::string& name : names)
	{
		if (name.rfind("ugen", 0) != 0 || std::count(name.begin(), name.end(), '.') != 1)
			continue;
		const std::string path = "/dev/" + name;
		const int fd = OrbisOpenDev(path.c_str(), O_RDWR);
		if (fd < 0)
		{
			Log("%s: open errno %d", path.c_str(), errno);
			continue;
		}
		looked++;
		usb_device_descriptor dd = {};
		if (ioctl(fd, USB_GET_DEVICE_DESC, &dd) != 0)
		{
			Log("%s: USB_GET_DEVICE_DESC errno %d", path.c_str(), errno);
			close(fd);
			continue;
		}
		if (dd.bDeviceClass == 9)
		{
			close(fd);
			continue;
		}
		usb_gen_descriptor full = {};
		uint8_t cfg[1024] = {};
		full.ugd_data = cfg;
		full.ugd_maxlen = sizeof(cfg);
		if (ioctl(fd, USB_GET_FULL_DESC, &full) != 0 || full.ugd_actlen < 9)
		{
			Log("%s: USB_GET_FULL_DESC errno %d", path.c_str(), errno);
			close(fd);
			continue;
		}
		int iface = -1, ep_in = 0, ep_out = 0, config_value = cfg[5];
		for (int at = 0; at + 2 <= full.ugd_actlen;)
		{
			const int dlen = cfg[at];
			const int dtype = cfg[at + 1];
			if (dlen < 2 || at + dlen > full.ugd_actlen)
				break;
			if (dtype == 4 && dlen >= 9)
			{
				iface = -1;
				ep_in = ep_out = 0;
				if (cfg[at + 5] == 0x08 && cfg[at + 7] == 0x50)
					iface = cfg[at + 2];
			}
			else if (dtype == 5 && iface >= 0 && dlen >= 7 && (cfg[at + 3] & 0x03) == 2)
			{
				if (cfg[at + 2] & 0x80)
					ep_in = cfg[at + 2];
				else
					ep_out = cfg[at + 2];
			}
			at += dlen;
		}
		if (iface < 0 || !ep_in || !ep_out)
		{
			Log("%s: class %#x, no mass-storage bulk interface", path.c_str(), dd.bDeviceClass);
			close(fd);
			continue;
		}
		// 11.60: USB_SET_CONFIG(1) returns success and leaves the device at 255, so only /dev/usb/2.2.0 exists.
		// Read the endpoint nodes first. A real SET_CONFIGURATION is sent only if those nodes are missing.
		// Needs proper testing.
		const uint8_t out_no = static_cast<uint8_t>(ep_out);
		const uint8_t in_no = static_cast<uint8_t>(ep_in);
		unsigned bus = 0, addr = 0;
		std::sscanf(name.c_str(), "ugen%u.%u", &bus, &addr);
		bool opened = false;
		auto read_config = [&]() {
			int value = -1;
			return ioctl(fd, USB_GET_CONFIG, &value) == 0 ? value : -1;
		};
		auto log_usb = [&](const char* tag) {
			static int full_lists = 0;
			const bool everything = full_lists++ == 0;
			const int usb = sceKernelOpen("/dev/usb", O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
			if (usb < 0)
			{
				Log("/dev/usb (%s): list errno %d", tag, errno);
				return;
			}
			alignas(0x4000) static char usb_buf[0x4000];
			std::string line;
			char prefix[16];
			std::snprintf(prefix, sizeof(prefix), "%u.%u.", bus, addr);
			const int got = sceKernelGetdents(usb, usb_buf, static_cast<int>(sizeof(usb_buf)));
			for (size_t off = 0; got > 0 && off + 8 <= static_cast<size_t>(got) && line.size() < 360;)
			{
				char* p = usb_buf + off;
				const size_t reclen = static_cast<unsigned char>(p[4]) | (static_cast<size_t>(static_cast<unsigned char>(p[5])) << 8);
				const size_t namlen = static_cast<unsigned char>(p[7]);
				if (reclen < 12 || off + reclen > static_cast<size_t>(got) || 8 + namlen >= reclen)
					break;
				const std::string entry(p + 8, namlen);
				if (everything || entry.rfind(prefix, 0) == 0)
				{
					if (!line.empty())
						line += ' ';
					line += entry;
				}
				off += reclen;
			}
			sceKernelClose(usb);
			Log("/dev/usb (%s): %s", tag, line.empty() ? "(none)" : line.c_str());
		};
		auto try_nodes = [&]() {
			if (opened)
				return;
			static int node_logs = 0;
			const bool verbose = node_logs++ < 2;
			int out_fd = -1, in_fd = -1;
			for (unsigned ep = 0; ep < 8 && (out_fd < 0 || in_fd < 0); ep++)
			{
				char node[64];
				std::snprintf(node, sizeof(node), "/dev/usb/%u.%u.%u", bus, addr, ep);
				// Bulk IN is read-only and bulk OUT is write-only. O_RDWR is errno 13 on 11.60.
				int kernel_err = 0;
				int efd = OrbisOpenDev(node, O_RDONLY, &kernel_err);
				int rd_errno = efd < 0 ? errno : 0;
				int rd_kernel = kernel_err;
				const char* how = "read";
				if (efd < 0)
				{
					kernel_err = 0;
					efd = OrbisOpenDev(node, O_WRONLY, &kernel_err);
					how = "write";
				}
				if (efd < 0)
				{
					if (verbose)
						Log("%s: read errno %d k %#x, write errno %d k %#x", node, rd_errno, rd_kernel, errno, kernel_err);
					continue;
				}
				Log("%s: opened %s", node, how);
				usb_endpoint_descriptor ed = {};
				const int rx = ioctl(efd, USB_GET_RX_ENDPOINT_DESC, &ed);
				const int tx = rx == 0 ? 0 : ioctl(efd, USB_GET_TX_ENDPOINT_DESC, &ed);
				if (verbose || (ed.bmAttributes & UE_XFERTYPE) == UE_BULK)
					Log("%s: addr %#x attr %#x rx %d tx %d", node, ed.bEndpointAddress, ed.bmAttributes, rx, tx);
				const bool bulk = (rx == 0 || tx == 0) && (ed.bmAttributes & UE_XFERTYPE) == UE_BULK && ed.bEndpointAddress != 0;
				if (bulk && (ed.bEndpointAddress & UE_DIR_IN) && in_fd < 0)
					in_fd = efd;
				else if (bulk && !(ed.bEndpointAddress & UE_DIR_IN) && out_fd < 0)
					out_fd = efd;
				else
					close(efd);
			}
			if (out_fd < 0 || in_fd < 0)
			{
				if (out_fd >= 0)
					close(out_fd);
				if (in_fd >= 0)
					close(in_fd);
				return;
			}
			const int ms = 5000;
			ioctl(out_fd, USB_SET_TX_TIMEOUT, &ms);
			ioctl(in_fd, USB_SET_RX_TIMEOUT, &ms);
			int short_ok = 1;
			ioctl(in_fd, USB_SET_RX_SHORT_XFER, &short_ok);
			g_ugen_fd = fd;
			g_ugen_out_fd = out_fd;
			g_ugen_in_fd = in_fd;
			g_ugen_rw = true;
			g_ugen = true;
			g_pipe_dead = false;
			g_fs_busy = false;
			g_fs_need_csw = false;
			g_fs_halted[0] = g_fs_halted[1] = false;
			g_usb.open = true;
			g_usb.ep_out = out_no;
			g_usb.ep_in = in_no;
			opened = true;
			Log("%s: bulk read/write out %#x in %#x", path.c_str(), out_no, in_no);
		};
		auto config_request = [&](int value) {
			usb_ctl_request req = {};
			req.ucr_request.bmRequestType = 0x00;
			req.ucr_request.bRequest = 0x09;
			req.ucr_request.wValue[0] = static_cast<uint8_t>(value);
			req.ucr_request.wValue[1] = static_cast<uint8_t>(value >> 8);
			const int rc = ioctl(fd, USB_DO_REQUEST, &req);
			Log("%s: SET_CONFIGURATION %d errno %d, config now %d", path.c_str(), value, rc == 0 ? 0 : errno, read_config());
		};
		uint8_t out_index = 0, in_index = 1;
		auto try_fs = [&]() {
			if (opened)
				return;
			usb_alt_interface alt = {};
			alt.uai_interface_index = static_cast<uint8_t>(iface);
			alt.uai_alt_index = 0;
			const int alt_rc = ioctl(fd, USB_SET_ALTINTERFACE, &alt);
			const int alt_errno = alt_rc == 0 ? 0 : errno;
			int iface_arg = iface;
			const int claim_rc = ioctl(fd, USB_CLAIM_INTERFACE, &iface_arg);
			Log("%s: alt errno %d, claim errno %d", path.c_str(), alt_errno, claim_rc == 0 ? 0 : errno);
			struct Attempt
			{
				uint8_t index_max;
				uint8_t out_index;
				uint8_t in_index;
				uint8_t out_no;
				uint8_t in_no;
				uint32_t bytes;
			};
			// 37632 is 16 raw CD sectors, which is what the disc thread reads at once. 2048 stays as the fallback.
			const Attempt attempts[] = {
				{16, 0, 1, out_no, in_no, 37632},
				{16, 0, 1, out_no, in_no, 4096},
				{16, 0, 1, out_no, in_no, 2048},
				{16, 0, 1, out_no, in_no, 512},
				{16, static_cast<uint8_t>(out_no & 15), static_cast<uint8_t>(in_no & 15), out_no, in_no, 2048},
				{16, 0, 1, static_cast<uint8_t>(out_no & 15), static_cast<uint8_t>(in_no & 15), 2048},
			};
			for (const Attempt& a : attempts)
			{
				std::memset(g_ugen_ep, 0, sizeof(g_ugen_ep));
				usb_fs_init init = {};
				init.pEndpoints = g_ugen_ep;
				init.ep_index_max = a.index_max;
				g_ugen_fd = fd;
				if (ioctl(fd, USB_FS_INIT, &init) != 0)
				{
					Log("%s: USB_FS_INIT errno %d", path.c_str(), errno);
					continue;
				}
				const auto open_ep = [&](uint8_t index, uint8_t ep_no) {
					usb_fs_open op = {};
					op.max_bufsize = a.bytes;
					op.max_frames = 1;
					op.ep_index = index;
					op.ep_no = ep_no;
					if (ioctl(fd, USB_FS_OPEN, &op) == 0)
						return true;
					Log("%s: USB_FS_OPEN index %u no %#x bytes %u errno %d", path.c_str(), index, ep_no, a.bytes, errno);
					return false;
				};
				if (open_ep(a.out_index, a.out_no) && open_ep(a.in_index, a.in_no))
				{
					opened = true;
					out_index = a.out_index;
					in_index = a.in_index;
					g_fs_max = a.bytes;
					g_ugen_iface = iface;
					g_ugen_claimed = claim_rc == 0;
					Log("%s: USB_FS buffer %u", path.c_str(), a.bytes);
					break;
				}
				for (uint8_t i = 0; i < 2; i++)
				{
					usb_fs_close c = {};
					c.ep_index = i;
					ioctl(fd, USB_FS_CLOSE, &c);
				}
				usb_fs_uninit u = {};
				ioctl(fd, USB_FS_UNINIT, &u);
				g_ugen_fd = -1;
			}
		};
		int iface_arg = iface;
		const int active = ioctl(fd, USB_IFACE_DRIVER_ACTIVE, &iface_arg);
		Log("%s: interface %d bulk out %#x in %#x, driver active ioctl %d, config %d", path.c_str(), iface, ep_out, ep_in, active,
			read_config());
		{
			// A bus-powered slim drive draws its most while spinning up. A drive that resets under load re-enumerates
			// (the pipe dies with errno 6 and cd1 shows up). The power figure says how much it asks for.
			usb_device_info di = {};
			if (ioctl(fd, USB_GET_DEVICEINFO, &di) == 0)
				Log("%s: speed %u, asks for %u mA, bMaxPower %u mA, power mode %u", path.c_str(), di.udi_speed, di.udi_power,
					full.ugd_actlen >= 9 ? cfg[8] * 2u : 0u, di.udi_power_mode);
			else
				Log("%s: USB_GET_DEVICEINFO errno %d; bMaxPower %u mA, bcdUSB %#x", path.c_str(), errno,
					full.ugd_actlen >= 9 ? cfg[8] * 2u : 0u, (dd.bcdUSB[1] << 8) | dd.bcdUSB[0]);
		}
		log_usb("before");
		try_nodes();
		if (!opened)
			try_fs();
		if (!opened && active == 0)
		{
			iface_arg = iface;
			Log("%s: detach errno %d, config now %d", path.c_str(),
				ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface_arg) == 0 ? 0 : errno, read_config());
			try_nodes();
			if (!opened)
				try_fs();
		}
		if (!opened)
		{
			config_request(config_value);
			log_usb("after request");
			try_nodes();
		}
		if (!opened && read_config() != config_value)
		{
			int index = 0;
			Log("%s: set config index 0 errno %d, config now %d", path.c_str(),
				ioctl(fd, USB_SET_CONFIG, &index) == 0 ? 0 : errno, read_config());
			log_usb("after index 0");
			try_nodes();
		}
		if (!opened)
			try_fs();
		if (!opened)
		{
			g_ugen_fd = fd;
			UgenClose(false, iface);
			continue;
		}
		g_ugen_fd = fd;
		g_ugen = true;
		g_pipe_dead = false;
		g_fs_busy = false;
		g_fs_need_csw = false;
		g_fs_halted[0] = g_fs_halted[1] = false;
		g_usb.open = true;
		if (!g_ugen_rw)
		{
			g_usb.ep_out = out_index;
			g_usb.ep_in = in_index;
		}
		uint8_t inq[36] = {};
		const uint8_t cdb[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
		const bool inquiry = UsbBot(cdb, 6, true, inq, sizeof(inq), 5000);
		const bool optical = inquiry && (inq[0] & 0x1f) == 5;
		char text[64] = {};
		if (inquiry)
			std::snprintf(text, sizeof(text), "%.8s | %.16s | %.4s", inq + 8, inq + 16, inq + 32);
		if (!optical || std::string(text).find("PS-SYSTEM") != std::string::npos)
		{
			Log("%s: not a USB optical drive (%s)", path.c_str(), inquiry ? text : "INQUIRY failed");
			UgenClose(true, iface);
			continue;
		}
		g_usb.text = text;
		*what = g_usb.text;
		Log("USB optical via %s: %s", path.c_str(), text);
		return true;
	}
	if (looked == 0)
		*why = "USB drive: no ugen device";
	else
		*why = "USB drive: no optical device on ugen";
	last_why = *why;
	next_try = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	return false;
}

bool UsbXfer(uint8_t ep, void* data, int len, unsigned timeout)
{
	if (g_ugen_rw)
		return UgenRw(ep, data, len, timeout);
	if (g_ugen)
		return UgenFs(ep, data, len, timeout, len % 512 != 0);
	if (g_usb.BulkTransfer)
	{
		int done = 0;
		const int rc = g_usb.BulkTransfer(g_usb.handle, ep, static_cast<unsigned char*>(data), len, &done, timeout);
		if (rc < 0)
			return false;
		if (done == 0 && rc > 0)
			done = rc;
		return done == len;
	}
	return UsbXferSubmit(ep, data, len, timeout);
}

void UsbGiveBack(void* handle, int iface, bool claimed, bool detached)
{
	if (claimed && g_usb.ReleaseInterface)
		g_usb.ReleaseInterface(handle, iface);
	if (detached && g_usb.AttachKernelDriver)
		g_usb.AttachKernelDriver(handle, iface);
	g_usb.Close(handle);
}

// Caller holds g_usb_mu. The handle is already claimed.
void UsbReset()
{
	// Stopping the pipes left the next read busy (errno 16) and the tray locked when the class reset was sent.
	static int said = 0;
	if (said++ < 4)
		Log("USB pipe left as it is");
}

// A transfer left running past its wait. Reap its completion when it comes; after 5 s stop it so the next command
// can go out. The drive is then waiting for a command packet, not for us. Needs proper testing.
bool ReapBusy()
{
	if (!g_fs_busy)
		return true;
	usb_fs_complete done = {};
	done.ep_index = g_fs_busy_ep;
	if (ioctl(g_ugen_fd, USB_FS_COMPLETE, &done) == 0 || errno != EBUSY)
	{
		static int said = 0;
		if (said++ < 4)
			Log("USB_FS ep %u: the late transfer finished (ep %u, status %d, %u bytes)", g_fs_busy_ep, done.ep_index,
				g_ugen_ep[g_fs_busy_ep & 1].status, g_fs_busy_ep == g_usb.ep_in ? g_bounce_in_len : g_bounce_out_len);
		g_fs_busy = false;
		return true;
	}
	if (std::chrono::steady_clock::now() - g_fs_busy_since < std::chrono::seconds(5))
		return false;
	static int stopped = 0;
	if (stopped++ < 4)
		Log("USB_FS ep %u: stopping the late transfer", g_fs_busy_ep);
	UsbFsDrain(g_fs_busy_ep);
	g_fs_busy = false;
	return true;
}

bool UsbBot(const uint8_t* cdb, int cdb_len, bool data_in, void* buf, uint32_t len, unsigned timeout, bool may_reset)
{
	if (g_pipe_dead || !g_usb.open || cdb_len < 1 || cdb_len > 16)
		return false;
	if (!ReapBusy())
		return false;
	const bool first = !g_usb.bulk_proven;
	if (first)
	{
		if (UsbMarkerBlocked())
			return false;
		UsbMarker("started");
	}
	const auto finish = [&](bool ok) {
		if (first)
			UsbMarker("done");
		if (ok)
			g_usb.bulk_proven = true;
		return ok;
	};
	uint8_t cbw[31] = {};
	cbw[0] = 0x55;
	cbw[1] = 0x53;
	cbw[2] = 0x42;
	cbw[3] = 0x43;
	const uint32_t tag = g_usb.tag++;
	cbw[4] = static_cast<uint8_t>(tag);
	cbw[5] = static_cast<uint8_t>(tag >> 8);
	cbw[6] = static_cast<uint8_t>(tag >> 16);
	cbw[7] = static_cast<uint8_t>(tag >> 24);
	cbw[8] = static_cast<uint8_t>(len);
	cbw[9] = static_cast<uint8_t>(len >> 8);
	cbw[10] = static_cast<uint8_t>(len >> 16);
	cbw[11] = static_cast<uint8_t>(len >> 24);
	cbw[12] = data_in ? 0x80 : 0x00;
	cbw[14] = static_cast<uint8_t>(cdb_len);
	std::memcpy(cbw + 15, cdb, static_cast<size_t>(cdb_len));
	auto hex16 = [](const uint8_t* p, unsigned n, char* out, size_t out_n) {
		unsigned i = 0;
		for (; i < n && i < 16 && i * 3 + 4 < out_n; i++)
			std::snprintf(out + i * 3, 4, "%02x ", p[i]);
		if (i * 3 < out_n)
			out[i * 3] = 0;
	};
	if (!UsbXfer(g_usb.ep_out, cbw, 31, timeout))
	{
		Log("USB BOT %02x command packet failed", cdb[0]);
		if (may_reset)
			UsbReset();
		return finish(false);
	}
	// While the drive is busy this bridge answers an IN with a zero-length packet or no handshake (status 20).
	// Read again a few times. An unbounded retry loop with a halt clear each time killed the pipe (errno 6), so
	// this stays at three tries. Needs proper testing.
	const auto read_in = [&](void* dst, uint32_t n, const char* what) -> bool {
		unsigned pause = 20;
		for (int attempt = 0; attempt < 3; attempt++)
		{
			if (g_pipe_dead)
				return false;
			const unsigned slice = attempt == 0 ? (timeout > 15000u ? 15000u : timeout) : 3000u;
			if (UsbXfer(g_usb.ep_in, dst, static_cast<int>(n), slice))
				return true;
			if (g_fs_busy || g_fs_complete)
				return false;
			static int logged = 0;
			if (logged++ < 24)
				Log("USB BOT %02x %s try %d: %s; waiting %u ms", cdb[0], what, attempt + 1,
					g_fs_zlp ? "zero-length packet" : "no reply", pause);
			std::this_thread::sleep_for(std::chrono::milliseconds(pause));
			pause *= 4;
		}
		return false;
	};
	bool full = !len;
	if (len && data_in)
	{
		full = read_in(buf, len, "data");
		// 13 bytes beginning USBS in the data phase: the status of the command before. The data follows it.
		if (!full && g_fs_complete && g_bounce_in_len == 13 && buf)
		{
			const auto* bytes = static_cast<const uint8_t*>(buf);
			if (bytes[0] == 0x55 && bytes[1] == 0x53 && bytes[2] == 0x42 && bytes[3] == 0x53)
			{
				static int said = 0;
				if (said++ < 8)
					Log("USB BOT %02x the previous status came first (tag %02x%02x); reading the data again", cdb[0], bytes[5], bytes[4]);
				full = read_in(buf, len, "data");
			}
		}
	}
	else if (len)
		full = UsbXfer(g_usb.ep_out, buf, static_cast<int>(len), timeout);
	// A sector is whole or it is nothing. 1328 of 2352 bytes came back at the outer edge of a CD with status 0;
	// keeping that put zeros into the game's files. Replies of variable length (TOC, sense) may be short.
	const bool sector = data_in && len >= 2048;
	const bool data_ok = full || (g_fs_complete && !sector);
	if (g_fs_busy)
		return finish(false);
	if (sector && !full && g_fs_complete)
	{
		static int said = 0;
		if (said++ < 12)
			Log("USB BOT %02x short sector: %u of %u bytes; retried", cdb[0], g_bounce_in_len, len);
	}
	if (len && !data_ok)
	{
		char hex[80] = {};
		hex16(static_cast<const uint8_t*>(buf), 16, hex, sizeof(hex));
		static int logged = 0;
		if (logged++ < 24)
			Log("USB BOT %02x data failed [%s]", cdb[0], hex);
	}
	uint8_t csw[13] = {};
	bool sig = false;
	bool xfer = false;
	for (int attempt = 0; attempt < 3; attempt++)
	{
		std::memset(csw, 0, sizeof(csw));
		xfer = read_in(csw, 13, "status");
		sig = csw[0] == 0x55 && csw[1] == 0x53 && csw[2] == 0x42 && csw[3] == 0x53;
		// This bridge does not always echo the tag (status 0 came back with tag 7f for another command). Rejecting
		// it and waiting for "ours" stalled the pipe, so a USBS packet right after our data is taken as ours.
		if (xfer && sig)
		{
			if (csw[4] != cbw[4] || csw[5] != cbw[5] || csw[6] != cbw[6] || csw[7] != cbw[7])
			{
				static int said = 0;
				if (said++ < 8)
					Log("USB BOT %02x status tag %02x%02x, sent %02x%02x; taken", cdb[0], csw[5], csw[4], cbw[5], cbw[4]);
			}
			break;
		}
		char hex[80] = {};
		hex16(csw, 13, hex, sizeof(hex));
		static int logged = 0;
		if (logged++ < 24)
			Log("USB BOT %02x status try %d %s [%s]", cdb[0], attempt + 1, !xfer ? "did not come" : "not USBS", hex);
		if (!xfer)
			break;
	}
	if (!data_ok || !xfer || !sig)
	{
		// The status never came but the data phase was whole. The build that got Crazy Taxi running kept the
		// sector here; only whole sectors are kept. Needs proper testing.
		if (!sig && full && data_in && len >= 512 && buf && !g_fs_busy && !g_pipe_dead)
		{
			const auto* bytes = static_cast<const uint8_t*>(buf);
			bool any = false;
			for (unsigned i = 0; i < 16; i++)
				any = any || bytes[i] != 0;
			const bool csw_like = bytes[0] == 0x55 && bytes[1] == 0x53 && bytes[2] == 0x42 && bytes[3] == 0x53;
			if (any && !csw_like)
			{
				static int kept = 0;
				if (kept++ < 8)
					Log("USB BOT %02x kept %u whole bytes; the status packet never came", cdb[0], len);
				return finish(true);
			}
		}
		if (!sig)
		{
			static int missing = 0;
			if (missing++ < 24)
				Log("USB BOT %02x status missing", cdb[0]);
		}
		if (may_reset && !sig)
			UsbReset();
		return finish(false);
	}
	const bool ok = csw[12] == 0;
	if (!ok && cdb[0] != 0x03)
	{
		uint8_t sense[18] = {};
		const uint8_t scdb[6] = {0x03, 0, 0, 0, sizeof(sense), 0};
		const bool got = UsbBot(scdb, 6, true, sense, sizeof(sense), 2000, false);
		Log("USB SCSI %02x csw %d sense %s %02x/%02x/%02x", cdb[0], csw[12], got ? "yes" : "no", sense[2] & 0x0f, sense[12], sense[13]);
	}
	else if (len >= 512 && buf)
	{
		static int logged = 0;
		if (logged++ < 6)
		{
			char hex[80] = {};
			hex16(static_cast<const uint8_t*>(buf), 16, hex, sizeof(hex));
			Log("USB BOT %02x data [%s] status %u", cdb[0], hex, csw[12]);
		}
	}
	return finish(ok);
}

bool UsbFindUnlocked(std::string* what, std::string* why)
{
	if (g_usb.open)
	{
		*what = g_usb.text;
		return true;
	}
	if (UgenFind(what, why))
		return true;
	if (*why == "USB drive: no optical device on ugen")
		return false;
	if (UsbMarkerBlocked())
	{
		*why = "USB drive: last USB scan did not return";
		return false;
	}
	if (!UsbLoad())
	{
		*why = g_usb.fail.empty() ? "USB drive: libSceUsbd not available" : g_usb.fail;
		return false;
	}
	void** list = nullptr;
	const long count = g_usb.GetDeviceList(&list);
	Log("libSceUsbd: %ld devices for the optical scan", count);
	if (count <= 0 || !list)
	{
		*why = "USB drive: no USB device";
		return false;
	}
	bool found = false;
	for (long i = 0; i < count && !found; i++)
	{
		uint8_t dd[32] = {};
		if (g_usb.GetDeviceDescriptor(list[i], dd) < 0 || dd[4] == 9)
			continue;
		void* h = nullptr;
		if (g_usb.Open(list[i], &h) < 0 || !h)
			continue;
		unsigned char cfg[1024] = {};
		const int cfg_len = g_usb.ControlTransfer(h, 0x80, 0x06, 0x0200, 0, cfg, sizeof(cfg), 500);
		int iface = -1, ep_in = 0, ep_out = 0;
		for (int at = 0; cfg_len > 0 && at + 2 <= cfg_len;)
		{
			const int dlen = cfg[at];
			const int dtype = cfg[at + 1];
			if (dlen < 2 || at + dlen > cfg_len)
				break;
			if (dtype == 4 && dlen >= 9)
			{
				iface = -1;
				ep_in = ep_out = 0;
				if (cfg[at + 5] == 0x08 && cfg[at + 7] == 0x50)
					iface = cfg[at + 2];
			}
			else if (dtype == 5 && iface >= 0 && dlen >= 7 && (cfg[at + 3] & 0x03) == 2)
			{
				if (cfg[at + 2] & 0x80)
					ep_in = cfg[at + 2];
				else
					ep_out = cfg[at + 2];
			}
			at += dlen;
		}
		if (iface < 0 || !ep_in || !ep_out)
		{
			g_usb.Close(h);
			continue;
		}
		const int config_value = cfg_len >= 6 && cfg[1] == 2 ? cfg[5] : 1;
		if (g_usb.SetConfiguration)
			g_usb.SetConfiguration(h, config_value);
		bool detached = false;
		if (g_usb.KernelDriverActive && g_usb.DetachKernelDriver && g_usb.KernelDriverActive(h, iface) == 1)
			detached = g_usb.DetachKernelDriver(h, iface) == 0;
		if (g_usb.ClaimInterface(h, iface) < 0)
		{
			Log("libSceUsbd: claim interface %d failed", iface);
			UsbGiveBack(h, iface, false, detached);
			continue;
		}
		g_usb.handle = h;
		g_usb.ep_in = static_cast<uint8_t>(ep_in);
		g_usb.ep_out = static_cast<uint8_t>(ep_out);
		g_usb.iface = iface;
		g_usb.detached = detached;
		g_pipe_dead = false;
		g_fs_busy = false;
		g_fs_need_csw = false;
		g_usb.open = true;
		uint8_t inq[36] = {};
		const uint8_t cdb[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
		const bool inquiry = UsbBot(cdb, 6, true, inq, sizeof(inq), 5000);
		const bool optical = inquiry && (inq[0] & 0x1f) == 5;
		char text[64] = {};
		if (inquiry)
			std::snprintf(text, sizeof(text), "%.8s | %.16s | %.4s", inq + 8, inq + 16, inq + 32);
		const bool sony = std::string(text).find("PS-SYSTEM") != std::string::npos;
		if (!optical || sony)
		{
			Log("libSceUsbd: interface %d is not a USB optical drive (%s)", iface, inquiry ? text : "INQUIRY failed");
			g_usb.open = false;
			g_usb.handle = nullptr;
			UsbGiveBack(h, iface, true, detached);
			continue;
		}
		g_usb.text = text;
		*what = g_usb.text;
		found = true;
		Log("USB optical via libSceUsbd: %s", text);
	}
	g_usb.FreeDeviceList(list, 1);
	if (!found)
		*why = "USB drive: no optical device on the USB bus";
	return found;
}

bool UsbReadyUnlocked()
{
	const uint8_t cdb[6] = {};
	return g_usb.open && UsbBot(cdb, 6, false, nullptr, 0, 5000);
}

uint32_t UsbCapacityUnlocked()
{
	uint8_t r[8] = {};
	const uint8_t cdb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	if (!UsbBot(cdb, 10, true, r, sizeof(r), 5000))
		return 0;
	const uint32_t block = (static_cast<uint32_t>(r[4]) << 24) | (static_cast<uint32_t>(r[5]) << 16) | (static_cast<uint32_t>(r[6]) << 8) | r[7];
	if (block != 2048)
		return 0;
	const uint32_t last = (static_cast<uint32_t>(r[0]) << 24) | (static_cast<uint32_t>(r[1]) << 16) | (static_cast<uint32_t>(r[2]) << 8) | r[3];
	return last == 0xFFFFFFFFu ? 0 : last + 1;
}
} // namespace

bool OrbisUsbDiscFind(std::string* what, std::string* why)
{
	std::lock_guard<std::mutex> lock(g_usb_mu);
	return UsbFindUnlocked(what, why);
}

bool OrbisUsbDiscReady()
{
	std::lock_guard<std::mutex> lock(g_usb_mu);
	return UsbReadyUnlocked();
}

bool OrbisUsbDiscSession()
{
	return g_usb.open;
}

uint32_t OrbisUsbDiscCapacity()
{
	std::lock_guard<std::mutex> lock(g_usb_mu);
	return UsbCapacityUnlocked();
}

uint32_t UsbFit(uint32_t count, uint32_t sector_bytes, uint32_t cap)
{
	uint32_t n = count > cap ? cap : count;
	if (g_fs_max < sector_bytes)
		return 1;
	const uint32_t fit = g_fs_max / sector_bytes;
	if (n > fit)
		n = fit;
	return n == 0 ? 1 : n;
}

bool OrbisUsbDiscRead2048(uint32_t lba, uint32_t count, void* buf)
{
	std::lock_guard<std::mutex> lock(g_usb_mu);
	// The BD drive starts spinning on the first read and answers "not ready" until it is up. Needs proper testing.
	static bool spun = false;
	if (!spun)
	{
		for (int i = 0; i < 40 && g_usb.open; i++)
		{
			const uint8_t tur[6] = {};
			if (UsbBot(tur, 6, false, nullptr, 0, 1000))
			{
				Log("USB disc ready");
				spun = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		if (!spun)
			Log("USB disc not ready after spin-up");
		spun = true;
	}
	auto* p = static_cast<uint8_t*>(buf);
	while (count)
	{
		const uint32_t n = UsbFit(count, 2048, 1);
		const uint32_t bytes = n * 2048;
		const auto sector = [&]() {
			return UsbBot(orbis_mmc::Read10(lba, static_cast<uint16_t>(n)).data(), 10, true, p, bytes, 15000) ||
				   UsbBot(orbis_mmc::Read12(lba, n, false).data(), 12, true, p, bytes, 15000);
		};
		if (!sector() && !sector())
			return false;
		p += bytes;
		lba += n;
		count -= n;
	}
	return true;
}

bool OrbisUsbDiscReadCdUser(uint32_t lba, uint32_t count, void* buf)
{
	std::lock_guard<std::mutex> lock(g_usb_mu);
	auto* p = static_cast<uint8_t*>(buf);
	while (count)
	{
		const uint32_t n = UsbFit(count, 2048, 1);
		if (!UsbBot(ReadCdCdb(lba, n, 0x10).data(), 12, true, p, n * 2048, 15000, false))
			return false;
		p += n * 2048;
		lba += n;
		count -= n;
	}
	return true;
}

bool OrbisUsbDiscRead2352(uint32_t lba, uint32_t count, void* buf)
{
	std::lock_guard<std::mutex> lock(g_usb_mu);
	auto* p = static_cast<uint8_t*>(buf);
	static const uint8_t sync[12] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
	while (count)
	{
		const uint32_t n = UsbFit(count, 2352, 1);
		// A raw sector starts with the 12-byte sync. Anything else is not the sector (a status packet, a stale
		// buffer); read it once more before giving up. Needs proper testing.
		bool ok = false;
		for (int attempt = 0; attempt < 3 && !ok && !g_pipe_dead; attempt++)
		{
			if (!UsbBot(ReadCdCdb(lba, n, 0xF8).data(), 12, true, p, n * 2352, 15000, false))
				continue;
			ok = std::memcmp(p, sync, sizeof(sync)) == 0;
			if (!ok)
			{
				static int said = 0;
				if (said++ < 6)
					Log("USB READ CD %u: no sync at the start [%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x]",
						lba, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
			}
		}
		if (!ok)
			return false;
		p += n * 2352;
		lba += n;
		count -= n;
	}
	return true;
}

bool OrbisUsbDiscReadToc(OrbisCdToc* out)
{
	std::lock_guard<std::mutex> lock(g_usb_mu);
	OrbisCdToc empty;
	*out = empty;
	// 804 is the longest formatted TOC and is not a multiple of 512, so a short reply is kept. A 2048-byte
	// request was completed as a short transfer and then the drive was reset, which locks the tray.
	uint8_t d[804] = {};
	uint8_t cdb[10] = {};
	cdb[0] = 0x43;
	cdb[7] = sizeof(d) >> 8;
	cdb[8] = sizeof(d) & 0xff;
	const bool transferred = UsbBot(cdb, 10, true, d, sizeof(d), 10000, false);
	const unsigned announced = (static_cast<unsigned>(d[0]) << 8 | d[1]) + 2;
	if (!transferred && announced < 10)
	{
		Log("READ TOC failed");
		return false;
	}
	unsigned total = (static_cast<unsigned>(d[0]) << 8 | d[1]) + 2;
	if (total > sizeof(d))
		total = sizeof(d);
	bool leadout = false;
	for (unsigned i = 4; i + 8 <= total && out->count < 99; i += 8)
	{
		const uint8_t num = d[i + 2];
		const uint32_t lba = (static_cast<uint32_t>(d[i + 4]) << 24) | (static_cast<uint32_t>(d[i + 5]) << 16) |
			(static_cast<uint32_t>(d[i + 6]) << 8) | d[i + 7];
		if (num == 0xAA)
		{
			out->leadout = lba;
			leadout = true;
			break;
		}
		if (num == 0)
			continue;
		OrbisCdTrack& t = out->tracks[out->count++];
		t.number = num;
		t.lba = lba;
		// READ TOC byte 1 is ADR in the high nibble and CONTROL in the low nibble (0x14 = ADR 1, data track).
		// They were swapped, so the data track was reported as audio and the BIOS went to its menu.
		t.adr = static_cast<uint8_t>(d[i + 1] >> 4);
		t.control = static_cast<uint8_t>(d[i + 1] & 0x0f);
	}
	out->ok = leadout && out->count > 0;
	if (!out->ok)
		Log("READ TOC: no lead-out (%02x %02x %02x %02x)", d[0], d[1], d[2], d[3]);
	return out->ok;
}

void OrbisUsbDiscRelease()
{
	std::lock_guard<std::mutex> lock(g_usb_mu);
	if (!g_usb.open && g_ugen_fd < 0)
		return;
	if (g_usb.open)
	{
		const uint8_t allow[6] = {0x1E, 0, 0, 0, 0, 0};
		UsbBot(allow, 6, false, nullptr, 0, 2000, false);
	}
	if (g_ugen_fd >= 0)
		UgenClose(g_ugen_claimed, g_ugen_iface);
	else if (g_usb.handle)
		UsbGiveBack(g_usb.handle, g_usb.iface, true, g_usb.detached);
	g_usb.open = false;
	Log("USB drive released; the tray can open");
}

bool OrbisUsbDiscSetSpeed(uint16_t read_kbs)
{
	if (OrbisFlag("nodiscspeed"))
		return true;
	std::lock_guard<std::mutex> lock(g_usb_mu);
	const std::vector<uint8_t> cdb = orbis_mmc::SetCdSpeed(read_kbs);
	const bool ok = UsbBot(cdb.data(), static_cast<int>(cdb.size()), false, nullptr, 0, 10000);
	Log("USB SET CD SPEED %u kB/s: %s", read_kbs, ok ? "accepted" : "refused");
	return ok;
}

bool OrbisUsbDiscReadLayer(OrbisDiscLayer* out)
{
	std::lock_guard<std::mutex> lock(g_usb_mu);
	OrbisDiscLayer empty;
	*out = empty;
	uint8_t buf[32] = {};
	const auto read_layer = [&](uint8_t layer, orbis_mmc::DvdPhysical& parsed) {
		std::memset(buf, 0, sizeof(buf));
		const std::vector<uint8_t> cdb = orbis_mmc::ReadDvdStructure(layer, sizeof(buf));
		if (!UsbBot(cdb.data(), static_cast<int>(cdb.size()), true, buf, sizeof(buf), 10000, false))
			return false;
		parsed = orbis_mmc::ParseDvdPhysical(buf, sizeof(buf));
		return parsed.ok;
	};
	orbis_mmc::DvdPhysical layer0, layer1;
	if (!read_layer(0, layer0))
	{
		out->why = "READ DVD STRUCTURE failed";
		return false;
	}
	const orbis_mmc::DvdPhysical* second = nullptr;
	if (layer0.layers != 0 && layer0.track_path == 0)
	{
		if (!read_layer(1, layer1))
		{
			out->why = "READ DVD STRUCTURE layer 1 failed";
			return false;
		}
		second = &layer1;
	}
	const orbis_mmc::DvdLayout layout = orbis_mmc::DvdLayoutFrom(layer0, second);
	if (!layout.ok)
	{
		out->why = "DVD structure is not a PS2 DVD layout";
		return false;
	}
	out->ok = true;
	out->media_type = layout.media_type;
	out->sectors = layout.sectors;
	out->layer_break = layout.layer_break;
	const uint64_t from_media = UsbCapacityUnlocked();
	if (from_media > out->sectors)
		out->sectors = static_cast<uint32_t>(from_media);
	return true;
}
