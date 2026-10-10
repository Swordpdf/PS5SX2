// PS5 port: PS2 discs in a disc drive. A PS5 without a Blu-ray drive (the Digital Edition, a Slim or Pro
// without the add-on drive) still has USB: a USB DVD or BD drive that reads DVDs shows up as a FreeBSD cd(4) device
// (/dev/cd1 on the console it was tried on, a TSSTcorp SN-208FB, beside its CAM pass-through device /dev/pass0).
//
// The disc's 2048-byte sectors are read straight from /dev/cdN (pread, whole sectors only). What cd(4) doesn't offer, a
// CD's raw 2352-byte sectors and its sub-channel, goes to the drive as SCSI commands through its pass-through device
// (CAMIOCOMMAND), found with CAMGETPASSTHRU. OrbisIOCtlSrc.cpp is PCSX2's IOCtlSrc on top of this; the shelf lists the
// drive (fe_ps5.cpp) and main-boot.cpp boots it as CDVD_SourceType::Disc.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace OrbisDiscDrive
{
// The cd(4) devices there are now, "/dev/cd0" to "/dev/cd7", whether or not a disc is in them.
std::vector<std::string> List();

// The pass-through device's descriptor for an open cd(4) device (O_RDWR), or -1 when there's none.
int OpenPassThrough(int cd_fd);

// What a drive said about a command that failed: the sense key (-1 when the command didn't reach the drive) and the
// additional sense code and qualifier (key 2 with code 0x3A: no disc; 2 with 0x04: becoming ready; 6: the disc changed).
struct Sense
{
	int key = -1;
	int asc = 0, ascq = 0;
};

// One SCSI command that reads `len` bytes into `buf` (none when `len` is 0). True when the drive answered GOOD; else
// `sense` (when not null) says why.
bool Command(int pass_fd, const uint8_t* cdb, int cdb_len, void* buf, uint32_t len, uint32_t timeout_ms, Sense* sense = nullptr);

// TEST UNIT READY: true when a disc is in and spun up; else `sense` as for Command.
bool Ready(int pass_fd, Sense* sense = nullptr);

// What's in drive `device` ("/dev/cd1") right now, without reading the disc. `changed` (when not null) is set when the
// drive reported a disc change since the last command anyone sent it.
enum class State
{
	NoDrive, // no such device (unplugged)
	NoDisc,  // empty, or the tray is open
	SpinningUp, // a disc is in and the drive is getting it ready
	Ready, // a disc is in and can be read
};
State Probe(const std::string& device, bool* changed = nullptr);

// READ CD: `count` raw 2352-byte sectors (sync, header, data, EDC/ECC) from `lba`, whatever the track type.
bool ReadRaw(int pass_fd, uint32_t lba, uint32_t count, uint8_t* buf);

// The drive's vendor and product ("TSSTcorp DVD+-RW SN-208FB"), for the log; empty when INQUIRY fails.
std::string Describe(int pass_fd);
} // namespace OrbisDiscDrive
