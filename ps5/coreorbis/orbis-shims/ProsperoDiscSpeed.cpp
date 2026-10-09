// PS5SX2 (vk-285-150, AI-assisted): the PS5's disc drive asked for more than 2x.
//
// swordpdf: "for next round we should try 8x dump speed". vk-285-149 on his console: a PS2 DVD copied at a flat 2.0x with
// the drive busy 100% of the time, and FreeBSD's CDRIOCREADSPEED on /dev/cd0 came back EINVAL both as "max" and as
// 11080 kB/s (it sends SET CD SPEED, a CD command). He pointed at the 11.40 system modules: SceShellCore carries FreeBSD's
// libcam ("CAMGETPASSTHRU ioctl failed", "/dev/pass%d", "system=CAM subsystem=SCSI_CD") and its ioctl numbers are this
// SDK's: CAMIOCOMMAND 0xC4E01902 (9 places) and CAMGETPASSTHRU 0xC4E01903, so the kernel's union ccb is the 1248 bytes
// we build. (/dev/icc_bddrive in libkernel is the drive's power, tray and chucking, not its speed.)
//
// So: ask /dev/cd0 for its pass device, open it, and send MMC commands (OrbisDiscScsi.h):
//   INQUIRY                the drive's vendor, model, firmware
//   MODE SENSE page 0x2A   its max and current read speed, as it reports them
//   GET PERFORMANCE        its nominal read speeds over the disc
//   SET CD SPEED 0xFFFF    once, to see the drive's own reason for the EINVAL (its sense code)
//   SET STREAMING          read at the fastest speed it offers (at least 16x asked; it picks the nearest it can do)
//   GET PERFORMANCE again
// Every command's CAM status and sense data is logged. Nothing is written to the disc or the drive's firmware; SET
// STREAMING lasts until the disc comes out. All through libkernel's ioctl (no system call from our own code). Flag
// nodiscspeed: nothing is sent. Needs proper testing on a console.
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

#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
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
	std::string PassDevice(int cd)
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
		Log("CAMGETPASSTHRU: status %d, %s%u (path %u, target %u, lun %u)", static_cast<int>(ccb.cgdl.status), name,
			ccb.cgdl.unit_number, ccb.ccb_h.path_id, ccb.ccb_h.target_id, static_cast<unsigned>(ccb.ccb_h.target_lun));
		if (ccb.cgdl.status == CAM_GDEVLIST_ERROR || !name[0])
			return {};
		return std::string("/dev/") + name + std::to_string(ccb.cgdl.unit_number);
	}

	// One MMC command through the pass device. True when the drive did it.
	bool Command(int pass, const std::vector<uint8_t>& cdb, uint32_t dir, void* data, uint32_t len, const char* what,
		uint32_t* resid = nullptr)
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
		if (rc != 0)
		{
			Log("%s: CAMIOCOMMAND errno %d", what, err);
			return false;
		}
		const uint32_t status = ccb.ccb_h.status & CAM_STATUS_MASK;
		if (resid)
			*resid = ccb.csio.resid;
		if (status == CAM_REQ_CMP)
		{
			Log("%s: ok (%.0f ms)", what, ms);
			return true;
		}
		std::string sense = "no sense data";
		if (status == CAM_SCSI_STATUS_ERROR && (ccb.ccb_h.status & CAM_AUTOSNS_VALID))
		{
			const size_t n = SSD_FULL_SIZE - ccb.csio.sense_resid;
			sense = orbis_mmc::SenseText(orbis_mmc::ParseSense(reinterpret_cast<const uint8_t*>(&ccb.csio.sense_data), n));
		}
		Log("%s: CAM status %#x, SCSI status %#x, %s (%.0f ms)", what, ccb.ccb_h.status, ccb.csio.scsi_status, sense.c_str(), ms);
		return false;
	}

	uint32_t Perf(int pass, const char* what)
	{
		std::vector<uint8_t> buf(orbis_mmc::GetPerformanceLen(), 0);
		uint32_t best = 0;
		if (!Command(pass, orbis_mmc::GetPerformance(), CAM_DIR_IN, buf.data(), static_cast<uint32_t>(buf.size()), what))
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
} // namespace

void OrbisDiscSpeedUp(const char* cd_path, uint64_t bytes)
{
	if (OrbisFlag("nodiscspeed"))
	{
		Log("off (flag nodiscspeed)");
		return;
	}
	const int cd = open(cd_path, O_RDONLY);
	if (cd < 0)
	{
		Log("%s: open errno %d", cd_path, errno);
		return;
	}
	const std::string pass_path = PassDevice(cd);
	close(cd);
	if (pass_path.empty())
		return;
	struct stat st;
	const int sr = stat(pass_path.c_str(), &st);
	Log("%s: %s", pass_path.c_str(), sr == 0 ? "there" : ("stat errno " + std::to_string(errno)).c_str());
	int pass = open(pass_path.c_str(), O_RDWR);
	if (pass < 0)
	{
		const int e = errno;
		pass = open(pass_path.c_str(), O_RDONLY);
		Log("%s: open read/write errno %d; read-only %s%d", pass_path.c_str(), e, pass < 0 ? "errno " : "fd ", pass < 0 ? errno : pass);
		if (pass < 0)
			return;
	}

	{
		uint8_t inq[36] = {};
		if (Command(pass, orbis_mmc::Inquiry(), CAM_DIR_IN, inq, sizeof(inq), "INQUIRY"))
			Log("  drive: %s", orbis_mmc::InquiryText(inq, sizeof(inq)).c_str());
	}
	uint32_t caps_max = 0;
	{
		uint8_t page[128] = {};
		if (Command(pass, orbis_mmc::ModeSenseCaps(sizeof(page)), CAM_DIR_IN, page, sizeof(page), "MODE SENSE 2A"))
		{
			const orbis_mmc::Caps c = orbis_mmc::ParseCaps(page, sizeof(page));
			Log("  capabilities page: %s, max read %u kB/s, current read %u kB/s; %s", c.ok ? "found" : "not found", c.max_read_kbs,
				c.cur_read_kbs, Hex(page, 32).c_str());
			caps_max = c.ok ? c.max_read_kbs : 0;
		}
	}
	const uint32_t perf_max = Perf(pass, "GET PERFORMANCE");
	Command(pass, orbis_mmc::SetCdSpeed(), CAM_DIR_NONE, nullptr, 0, "SET CD SPEED max (what CDRIOCREADSPEED sends)");

	// At least 16x DVD (22160 kB/s), more if the drive says it can: with Exact = 0 it settles on the nearest it does.
	uint32_t want = 22160;
	if (perf_max > want)
		want = perf_max;
	if (caps_max > want && caps_max < 0xFFFF)
		want = caps_max;
	const uint32_t sectors = static_cast<uint32_t>(bytes / 2048);
	std::vector<uint8_t> desc = orbis_mmc::StreamingDescriptor(0, sectors ? sectors - 1 : 0, want);
	char what[96];
	snprintf(what, sizeof(what), "SET STREAMING %u kB/s (%.1fx DVD) over LBA 0..%u", want, want / 1385.0, sectors ? sectors - 1 : 0);
	const bool streamed = Command(pass, orbis_mmc::SetStreaming(), CAM_DIR_OUT, desc.data(), static_cast<uint32_t>(desc.size()), what);
	if (streamed)
		Perf(pass, "GET PERFORMANCE after SET STREAMING");
	close(pass);
}

void OrbisDiscReadTest(const char* cd_path, uint64_t bytes)
{
	const int fd = open(cd_path, O_RDONLY);
	if (fd < 0)
		return;
	constexpr size_t kChunk = 4u << 20;
	constexpr uint64_t kSpan = 256ull << 20;
	std::vector<uint8_t> buf(kChunk);
	char note[200] = {};
	auto run = [&](uint64_t from, const char* where) {
		from &= ~static_cast<uint64_t>(2047);
		const auto t0 = std::chrono::steady_clock::now();
		uint64_t got = 0;
		for (uint64_t off = from; off < from + kSpan && off + kChunk <= bytes; off += kChunk)
		{
			if (pread(fd, buf.data(), kChunk, static_cast<off_t>(off)) != static_cast<ssize_t>(kChunk))
				break;
			got += kChunk;
		}
		const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		const double mbs = s > 0 ? got / s / 1e6 : 0;
		Log("read test, %s: %llu MB in %.1f s = %.1f MB/s (%.1fx DVD)", where, static_cast<unsigned long long>(got >> 20), s, mbs,
			mbs * 1e6 / 1385000.0);
		return mbs;
	};
	const double a = run(0, "start of the disc");
	const double b = bytes > kSpan ? run(bytes - kSpan, "end of the disc") : 0.0;
	close(fd);
	snprintf(note, sizeof(note), "Disc read test: start %.1f MB/s (%.1fx), end %.1f MB/s (%.1fx)", a, a / 1.385, b, b / 1.385);
	OrbisNotifyPlain(note);
}
