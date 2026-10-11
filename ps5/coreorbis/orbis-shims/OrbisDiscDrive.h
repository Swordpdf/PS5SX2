// PS5SX2: PS2 discs played straight from a disc drive (from PR #34 by Heyde Moura; AI-assisted port). A PS5 without a
// disc drive of its own (the Digital Edition, a Slim or Pro without the add-on drive) still has USB: a USB DVD or BD drive
// that reads DVDs shows up as a FreeBSD cd(4) device (/dev/cd1 on the console it was tried on, a TSSTcorp SN-208FB,
// beside its CAM pass-through device /dev/pass0).
//
// The disc's 2048-byte sectors are read straight from /dev/cdN (pread, whole sectors only). What cd(4) doesn't offer, a
// CD's raw 2352-byte sectors and its sub-channel, goes to the drive as SCSI commands (OrbisDiscScsi.h) through its
// pass-through device (CAMIOCOMMAND), found with CAMGETPASSTHRU. Read-only: nothing here writes to a disc or a drive.
// OrbisIOCtlSrc.cpp is PCSX2's IOCtlSrc on top of this; main-boot.cpp boots a drive as CDVD_SourceType::Disc (flag
// bootdisc, or a drive path given as the game). Nothing here runs in the background: a drive is only touched when a game
// is asked to start from it.
//
// Copyright (C) 2026 Heyde Moura
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "OrbisDiscScsi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace OrbisDiscDrive
{
// A cd(4) device path, "/dev/cd" and a number ("/dev/cd1").
inline bool IsDrivePath(const std::string& path)
{
	if (path.size() < 8 || path.size() > 10 || path.compare(0, 7, "/dev/cd") != 0)
		return false;
	for (size_t i = 7; i < path.size(); i++)
		if (path[i] < '0' || path[i] > '9')
			return false;
	return true;
}

// A serial read off a disc (SYSTEM.CNF's BOOT2, "SLUS-21351") is fit for a file name: letters, digits, '-' and '_' only,
// 1 to 32 of them. Anything else (a damaged or odd disc) isn't used to name a settings file.
inline bool SafeSerial(const std::string& serial)
{
	if (serial.empty() || serial.size() > 32)
		return false;
	for (const char c : serial)
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
			return false;
	return true;
}

// The cd(4) devices there are now, "/dev/cd0" to "/dev/cd7", whether or not a disc is in them (a stat each, no open).
std::vector<std::string> List();

// The pass-through device's descriptor for an open cd(4) device (O_RDWR, else O_RDONLY), or -1 when there's none.
// `name` (when not null) gets its path, "/dev/pass0".
int OpenPassThrough(int cd_fd, std::string* name = nullptr);

// One SCSI command that reads `len` bytes into `buf` (none when `len` is 0). True only when the drive answered GOOD and
// all `len` bytes came (CAM_REQ_CMP with no residue): on the PS5's USB path a drive can answer "complete" with nothing
// transferred. Else false, and `sense` (when not null) says why (valid = false when the command didn't reach the drive or
// came back short).
bool Command(int pass_fd, const std::vector<uint8_t>& cdb, void* buf, uint32_t len, uint32_t timeout_ms, orbis_mmc::Sense* sense = nullptr);

// TEST UNIT READY: true when a disc is in and spun up; else `sense` as for Command (key 2 asc 3A: no disc; 2/04: becoming
// ready; 6: UNIT ATTENTION, the disc changed or the drive reset).
bool Ready(int pass_fd, orbis_mmc::Sense* sense = nullptr);

// READ CD: `count` raw 2352-byte sectors (sync, header, data, EDC/ECC) from `lba`, whatever the track type, 16 a command.
// `data`: the sectors are in data tracks, so each must start with the CD sync pattern (a buffer the drive didn't fill
// isn't taken for data); an audio track's sectors have none.
bool ReadRaw(int pass_fd, uint32_t lba, uint32_t count, uint8_t* buf, bool data);

// The drive's "vendor | model | firmware" (INQUIRY), for the log; empty when INQUIRY fails.
std::string Describe(int pass_fd);

// For the bootdisc flag only (an explicit request; never polled): waits up to `timeout_ms` while drive `device` says a
// disc is spinning up (or reports a disc change), so a disc put in just before the app opened still starts. True when
// the drive is ready (or has no pass-through device to ask). `why` (when not null) says why not.
bool WaitReady(const std::string& device, uint32_t timeout_ms, std::string* why = nullptr);
} // namespace OrbisDiscDrive
