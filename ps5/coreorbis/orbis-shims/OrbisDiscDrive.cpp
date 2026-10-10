// PS5 port: disc drives. See OrbisDiscDrive.h.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisDiscDrive.h"

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_pass.h>

#include <fcntl.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace OrbisDiscDrive
{
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

int OpenPassThrough(int cd_fd)
{
	if (cd_fd < 0)
		return -1;
	union ccb c;
	std::memset(&c, 0, sizeof(c));
	c.ccb_h.func_code = XPT_GDEVLIST;
	if (ioctl(cd_fd, CAMGETPASSTHRU, &c) != 0 || (c.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP)
		return -1;
	char path[48];
	std::snprintf(path, sizeof(path), "/dev/%.16s%u", c.cgdl.periph_name, c.cgdl.unit_number);
	return open(path, O_RDWR);
}

bool Command(int pass_fd, const uint8_t* cdb, int cdb_len, void* buf, uint32_t len, uint32_t timeout_ms, Sense* sense)
{
	if (sense)
		*sense = Sense();
	if (pass_fd < 0 || cdb_len <= 0 || cdb_len > IOCDBLEN)
		return false;
	union ccb c;
	std::memset(&c, 0, sizeof(c));
	c.ccb_h.func_code = XPT_SCSI_IO;
	// No queue freeze on an error: nothing here would release it (camcontrol's cam_release_devq).
	c.ccb_h.flags = (len ? CAM_DIR_IN : CAM_DIR_NONE) | CAM_DEV_QFRZDIS;
	c.ccb_h.retry_count = 1;
	c.ccb_h.timeout = timeout_ms;
	c.csio.data_ptr = static_cast<u_int8_t*>(buf);
	c.csio.dxfer_len = len;
	c.csio.sense_len = SSD_FULL_SIZE;
	c.csio.cdb_len = static_cast<u_int8_t>(cdb_len);
	c.csio.tag_action = 0x20; // MSG_SIMPLE_Q_TAG
	std::memcpy(c.csio.cdb_io.cdb_bytes, cdb, static_cast<size_t>(cdb_len));
	if (ioctl(pass_fd, CAMIOCOMMAND, &c) != 0)
		return false;
	if ((c.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP)
		return true;
	if (sense && (c.ccb_h.status & CAM_AUTOSNS_VALID))
	{
		const uint8_t* s = reinterpret_cast<const uint8_t*>(&c.csio.sense_data);
		const uint8_t code = s[0] & 0x7f;
		if (code == 0x72 || code == 0x73) // descriptor format
		{
			sense->key = s[1] & 0x0f;
			sense->asc = s[2];
			sense->ascq = s[3];
		}
		else // fixed format
		{
			sense->key = s[2] & 0x0f;
			sense->asc = s[12];
			sense->ascq = s[13];
		}
	}
	return false;
}

bool Ready(int pass_fd, Sense* sense)
{
	const uint8_t cdb[6] = {0x00, 0, 0, 0, 0, 0}; // TEST UNIT READY
	return Command(pass_fd, cdb, sizeof(cdb), nullptr, 0, 5000, sense);
}

State Probe(const std::string& device, bool* changed)
{
	if (changed)
		*changed = false;
	struct stat st;
	if (stat(device.c_str(), &st) != 0)
		return State::NoDrive;
	const int fd = open(device.c_str(), O_RDONLY);
	if (fd < 0)
		return errno == ENOENT || errno == ENODEV ? State::NoDrive : State::NoDisc;
	State state = State::NoDisc;
	const int pass = OpenPassThrough(fd);
	if (pass >= 0)
	{
		// A disc change reports UNIT ATTENTION once; the next command says how the drive is now.
		Sense sense;
		bool ready = Ready(pass, &sense);
		for (int i = 0; i < 2 && !ready && sense.key == 6; i++)
		{
			if (changed)
				*changed = true;
			ready = Ready(pass, &sense);
		}
		if (ready)
			state = State::Ready;
		else if (sense.key == 2 && sense.asc == 0x3A)
			state = State::NoDisc;
		else if (sense.key == 2 || sense.key == 6)
			state = State::SpinningUp;
		close(pass);
	}
	else
	{
		off_t bytes = 0;
		state = ioctl(fd, DIOCGMEDIASIZE, &bytes) == 0 && bytes > 0 ? State::Ready : State::NoDisc;
	}
	close(fd);
	return state;
}

bool ReadRaw(int pass_fd, uint32_t lba, uint32_t count, uint8_t* buf)
{
	// READ CD (MMC): expected sector type any, the whole 2352 bytes (sync, header and sub-header, user data, EDC/ECC: 0xF8),
	// no sub-channel. 16 sectors a command keeps each transfer under 40 KB, well inside the pass-through's limit.
	while (count)
	{
		const uint32_t n = count < 16 ? count : 16;
		const uint8_t cdb[12] = {0xBE, 0, static_cast<uint8_t>(lba >> 24), static_cast<uint8_t>(lba >> 16), static_cast<uint8_t>(lba >> 8),
			static_cast<uint8_t>(lba), static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n), 0xF8, 0, 0};
		if (!Command(pass_fd, cdb, sizeof(cdb), buf, n * 2352, 20000))
			return false;
		lba += n;
		count -= n;
		buf += n * 2352;
	}
	return true;
}

std::string Describe(int pass_fd)
{
	uint8_t inq[36] = {};
	const uint8_t cdb[6] = {0x12, 0, 0, 0, sizeof(inq), 0}; // INQUIRY
	if (!Command(pass_fd, cdb, sizeof(cdb), inq, sizeof(inq), 5000))
		return {};
	auto field = [&](int at, int len) {
		std::string s(reinterpret_cast<const char*>(inq + at), static_cast<size_t>(len));
		while (!s.empty() && (s.back() == ' ' || s.back() == '\0'))
			s.pop_back();
		return s;
	};
	return field(8, 8) + " " + field(16, 16);
}
} // namespace OrbisDiscDrive
