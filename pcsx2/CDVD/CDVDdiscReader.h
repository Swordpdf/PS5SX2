// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#include "CDVDcommon.h"
#include "common/Pcsx2Defs.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Error;

extern int curDiskType;
extern int curTrayStatus;

struct toc_entry
{
	u32 lba;
	u8 track;
	u8 adr : 4;
	u8 control : 4;
};

class IOCtlSrc
{
	IOCtlSrc(const IOCtlSrc&) = delete;
	IOCtlSrc& operator=(const IOCtlSrc&) = delete;

	std::string m_filename;

#if defined(_WIN32)
	HANDLE m_device = INVALID_HANDLE_VALUE;
	mutable std::mutex m_lock;
#else
	int m_device = -1;
#endif
#ifdef __PROSPERO__
	// PS5SX2 (ps5/coreorbis/orbis-shims/OrbisIOCtlSrc.cpp): the drive's CAM pass-through device (raw CD sectors, the
	// sub-channel, TEST UNIT READY), the lock that keeps Reopen from closing the descriptors under a read on another
	// thread (the CDVD thread, its keep-alive thread, the CPU thread's sub-channel reads), and DiscReady's last answer.
	int m_pass = -1;
	mutable std::mutex m_lock;
	s64 m_ready_checked_ms = -1;
	bool m_ready = false;
	bool m_said_unreadable = false;
	mutable bool m_raw_off = false; // READ CD didn't work on this disc: its sectors are made from their 2048 bytes
#endif

	s32 m_media_type = 0;
	u32 m_sectors = 0;
	u32 m_layer_break = 0;
	std::vector<toc_entry> m_toc;

	bool ReadDVDInfo();
	bool ReadCDInfo();

public:
	IOCtlSrc(std::string filename);
	~IOCtlSrc();

	bool Reopen(Error* error);

	u32 GetSectorCount() const;
	const std::vector<toc_entry>& ReadTOC() const;
	bool ReadSectors2048(u32 sector, u32 count, u8* buffer) const;
	bool ReadSectors2352(u32 sector, u32 count, u8* buffer) const;
	bool ReadTrackSubQ(cdvdSubQ* subq) const;
	u32 GetLayerBreakAddress() const;
	s32 GetMediaType() const;
	void SetSpindleSpeed(bool restore_defaults) const;
	bool DiscReady();
};

extern std::unique_ptr<IOCtlSrc> src;

std::vector<std::string> GetOpticalDriveList();
void GetValidDrive(std::string& drive);

extern bool disc_has_changed;
extern bool weAreInNewDiskCB;

extern void (*newDiscCB)();

void cdvdStartThread();
void cdvdStopThread();
void cdvdRequestSector(u32 sector, s32 mode);
u8* cdvdGetSector(u32 sector, s32 mode);
s32 cdvdDirectReadSector(u32 sector, s32 mode, u8* buffer);
void cdvdRefreshData();
void cdvdParseTOC();
