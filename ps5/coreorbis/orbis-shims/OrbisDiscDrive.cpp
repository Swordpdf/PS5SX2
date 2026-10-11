// PS5SX2: disc drives for PS2 discs played from a drive (from PR #34 by Heyde Moura; AI-assisted port). See
// OrbisDiscDrive.h.
//
// Port notes: the CDBs and the sense parsing are OrbisDiscScsi.h's (the PC-tested ones the disc copy uses); a command is
// filled the way ProsperoDiscSpeed.cpp fills its own (cam_fill_csio, no queue freeze on an error, since nothing here
// would release it). A command counts as done only when all its bytes came (the review of PR #34: on the PS5's USB path
// a drive answered CAM_REQ_CMP with 0 bytes, and PCSX2 was then handed whatever the buffer held). All through libkernel's
// ioctl, read-only.
//
// Copyright (C) 2026 Heyde Moura
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisDiscDrive.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/disk.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace OrbisDiscDrive
{
namespace
{
// At most this many failed commands are logged (a disc that won't read would otherwise fill the log).
std::atomic<int> s_logged_failures{0};
constexpr int kMaxLoggedFailures = 20;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	std::printf("[disc] %s\n", line);
	std::fflush(stdout);
}
} // namespace

std::vector<std::string> List()
{
	std::vector<std::string> drives;
	for (int i = 0; i < 8; i++)
	{
		char path[16];
		std::snprintf(path, sizeof(path), "/dev/cd%d", i);
		struct stat st;
		if (stat(path, &st) == 0 && S_ISCHR(st.st_mode))
			drives.push_back(path);
	}
	return drives;
}

int OpenPassThrough(int cd_fd, std::string* name)
{
	if (cd_fd < 0)
		return -1;
	union ccb c;
	std::memset(&c, 0, sizeof(c));
	c.ccb_h.func_code = XPT_GDEVLIST;
	if (ioctl(cd_fd, CAMGETPASSTHRU, &c) != 0 || c.cgdl.status == CAM_GDEVLIST_ERROR || !c.cgdl.periph_name[0])
		return -1;
	char path[48];
	std::snprintf(path, sizeof(path), "/dev/%.16s%u", c.cgdl.periph_name, c.cgdl.unit_number);
	if (name)
		*name = path;
	int fd = open(path, O_RDWR);
	if (fd < 0)
		fd = open(path, O_RDONLY);
	return fd;
}

bool Command(int pass_fd, const std::vector<uint8_t>& cdb, void* buf, uint32_t len, uint32_t timeout_ms, orbis_mmc::Sense* sense)
{
	if (sense)
		*sense = orbis_mmc::Sense();
	if (pass_fd < 0 || cdb.empty() || cdb.size() > IOCDBLEN)
		return false;
	union ccb c;
	std::memset(&c, 0, sizeof(c));
	cam_fill_csio(&c.csio, /*retries*/ 1, nullptr, (len ? CAM_DIR_IN : CAM_DIR_NONE) | CAM_DEV_QFRZDIS, MSG_SIMPLE_Q_TAG,
		static_cast<u_int8_t*>(buf), len, SSD_FULL_SIZE, static_cast<u_int8_t>(cdb.size()), timeout_ms);
	std::memcpy(c.csio.cdb_io.cdb_bytes, cdb.data(), cdb.size());
	if (ioctl(pass_fd, CAMIOCOMMAND, &c) != 0)
	{
		const int e = errno;
		if (s_logged_failures.fetch_add(1) < kMaxLoggedFailures)
			Log("command %02x: CAMIOCOMMAND errno %d", cdb[0], e);
		return false;
	}
	const uint32_t status = c.ccb_h.status & CAM_STATUS_MASK;
	if (status == CAM_REQ_CMP)
	{
		// Done, but did the bytes come? resid is what the transfer fell short by.
		if (len == 0 || c.csio.resid == 0)
			return true;
		if (s_logged_failures.fetch_add(1) < kMaxLoggedFailures)
			Log("command %02x: complete, but %u of %u bytes came: not taken", cdb[0],
				len - (c.csio.resid < len ? c.csio.resid : len), len);
		return false;
	}
	orbis_mmc::Sense s;
	if (status == CAM_SCSI_STATUS_ERROR && (c.ccb_h.status & CAM_AUTOSNS_VALID))
	{
		const size_t n = SSD_FULL_SIZE - c.csio.sense_resid;
		s = orbis_mmc::ParseSense(reinterpret_cast<const uint8_t*>(&c.csio.sense_data), n);
	}
	if (sense)
		*sense = s;
	// TEST UNIT READY's "no disc" and "becoming ready" are answers, not failures worth a line.
	if (cdb[0] != 0x00 && s_logged_failures.fetch_add(1) < kMaxLoggedFailures)
		Log("command %02x: CAM status %#x, SCSI status %#x, %s", cdb[0], c.ccb_h.status, c.csio.scsi_status,
			orbis_mmc::SenseText(s).c_str());
	return false;
}

bool Ready(int pass_fd, orbis_mmc::Sense* sense)
{
	return Command(pass_fd, orbis_mmc::TestUnitReady(), nullptr, 0, 5000, sense);
}

bool ReadRaw(int pass_fd, uint32_t lba, uint32_t count, uint8_t* buf, bool data)
{
	// 16 sectors a command keeps each transfer under 40 KB, well inside the pass-through's limit (64 KiB on the PS5's
	// drive, ProsperoDiscSpeed.cpp's E2BIG above it).
	while (count)
	{
		const uint32_t n = count < 16 ? count : 16;
		std::memset(buf, 0, n * 2352);
		if (!Command(pass_fd, orbis_mmc::ReadCd(lba, n), buf, n * 2352, 20000))
			return false;
		for (uint32_t i = 0; data && i < n; i++)
			if (!orbis_mmc::CdSyncOk(buf + i * 2352))
			{
				if (s_logged_failures.fetch_add(1) < kMaxLoggedFailures)
					Log("READ CD of sector %u: no sync pattern in what came back: not taken", lba + i);
				return false;
			}
		lba += n;
		count -= n;
		buf += n * 2352;
	}
	return true;
}

std::string Describe(int pass_fd)
{
	uint8_t inq[36] = {};
	if (!Command(pass_fd, orbis_mmc::Inquiry(), inq, sizeof(inq), 5000) || !inq[8])
		return {};
	return orbis_mmc::InquiryText(inq, sizeof(inq));
}

bool WaitReady(const std::string& device, uint32_t timeout_ms, std::string* why)
{
	const int fd = open(device.c_str(), O_RDONLY);
	if (fd < 0)
	{
		if (why)
			*why = errno == ENXIO || errno == EIO ? "no disc in the drive" : std::string("doesn't open: ") + std::strerror(errno);
		return false;
	}
	const int pass = OpenPassThrough(fd);
	bool ready = pass < 0; // nothing to ask: let the disc's own reads tell
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	orbis_mmc::Sense sense;
	while (!ready)
	{
		ready = Ready(pass, &sense);
		// becoming ready (2/04) or a disc change being reported (6): ask again shortly; anything else is the answer
		const bool wait = !ready && sense.valid && (sense.key == 6 || (sense.key == 2 && sense.asc == 0x04));
		if (!wait || std::chrono::steady_clock::now() >= until)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
	if (!ready && why)
		*why = sense.valid && sense.key == 2 && sense.asc == 0x3A ? "no disc in the drive" : "not ready: " + orbis_mmc::SenseText(sense);
	if (pass >= 0)
		close(pass);
	close(fd);
	return ready;
}
} // namespace OrbisDiscDrive
