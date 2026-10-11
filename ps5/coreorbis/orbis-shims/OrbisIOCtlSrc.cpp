// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright (C) 2026 Heyde Moura
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PS5SX2: PCSX2's IOCtlSrc (CDVD/CDVDdiscReader.h) on the PS5, for a PS2 disc played straight from a disc drive. Adapted
// from PCSX2's pcsx2/CDVD/Linux/IOCtlSrc.cpp for FreeBSD's cd(4) by Heyde Moura (PR #34), then ported into PS5SX2
// (AI-assisted). Replaces the IOCtlSrc stubs ProsperoCDVD.cpp had.
//
// A DVD's sectors are pread from /dev/cdN; its layers come from DVDIOCREADSTRUCTURE, as Linux's IOCtlSrc reads them with
// DVD_READ_STRUCT. A CD's table of contents comes from CDIOREADTOCENTRYS, and its raw sectors and sub-channel from SCSI
// commands through the drive's pass-through device (OrbisDiscDrive.h). DVDs were tried on a console in PR #34 (a TSSTcorp
// SN-208FB on USB); CDs need proper testing.
//
// Port changes from the review of PR #34: Reopen fails when neither a DVD's layers nor a CD's table of contents read (it
// said yes, and PCSX2's CDVD thread then asked DiscReady, and so Reopen, about 100 times a second); DiscReady asks the
// drive at most every 500 ms and keeps the answer in between (it sent TEST UNIT READY on each of the CDVD thread's
// loops); the pass-through descriptor is a member under a lock (it was a global shared by the CDVD thread, its
// keep-alive thread and the CPU thread); a command counts only when all its bytes came (OrbisDiscDrive::Command).

#include "CDVD/CDVDdiscReader.h"
#include "common/Error.h"

#include "OrbisDiscDrive.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/cdio.h>
#include <sys/disk.h>
#include <sys/dvdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

// (disc globals live in CDVDdiscReader.cpp already.)

namespace
{
// DiscReady asks the drive at most this often; the CDVD thread calls it on every loop (every 10 ms with no disc).
constexpr s64 kReadyEveryMs = 500;

s64 NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// At most this many failed reads are logged (a scratched disc would otherwise fill the log).
std::atomic<int> s_read_failures{0};

// A CD sector's MSF header field, in BCD, for LBA `lba` (150 sectors of pregap before LBA 0).
u8 Bcd(u32 v)
{
  return static_cast<u8>(((v / 10) << 4) | (v % 10));
}

// A raw mode 2 form 1 sector around 2048 bytes of user data, for a drive without SCSI pass-through: sync, header and
// sub-header, data, and no EDC/ECC (PCSX2 doesn't check them). Not enough for XA form 2 sectors (FMV and audio streams).
void MakeRaw(u32 lba, const u8* data, u8* out)
{
  std::memset(out, 0, 2352);
  std::memset(out + 1, 0xff, 10);
  const u32 a = lba + 150;
  out[12] = Bcd(a / 4500);
  out[13] = Bcd((a / 75) % 60);
  out[14] = Bcd(a % 75);
  out[15] = 2;
  out[18] = out[22] = 0x08; // sub-header: data, form 1
  std::memcpy(out + 24, data, 2048);
}

// Sectors [first, last] are all in data tracks (the table of contents' control field, bit 2), not audio.
bool AllData(const std::vector<toc_entry>& toc, u32 sectors, u32 first, u32 last)
{
  for (size_t i = 0; i < toc.size(); i++)
  {
    const u32 start = toc[i].lba;
    const u32 end = i + 1 < toc.size() ? toc[i + 1].lba : sectors; // one past the track's last sector
    if (!(toc[i].control & 0x04) && start <= last && first < end)
      return false;
  }
  return true;
}

// Whole 2048-byte sectors from the cd(4) device (it takes nothing else).
bool Pread2048(int fd, u32 sector, u32 count, u8* buffer)
{
  const ssize_t want = 2048 * static_cast<ssize_t>(count);
  const ssize_t got = pread(fd, buffer, static_cast<size_t>(want), sector * 2048ULL);
  if (got == want)
    return true;
  if (s_read_failures.fetch_add(1) < 50)
  {
    if (got == -1)
      std::printf("[disc] read sectors %u-%u failed: %s\n", sector, sector + count - 1, std::strerror(errno));
    else
      std::printf("[disc] read sectors %u-%u: %zd bytes read, %zd bytes expected\n", sector, sector + count - 1, got, want);
    std::fflush(stdout);
  }
  return false;
}
} // namespace

IOCtlSrc::IOCtlSrc(std::string filename)
  : m_filename(std::move(filename))
{
}

IOCtlSrc::~IOCtlSrc()
{
  std::lock_guard<std::mutex> lock(m_lock);
  if (m_pass >= 0)
    close(m_pass);
  if (m_device != -1)
    close(m_device);
}

bool IOCtlSrc::Reopen(Error* error)
{
  std::lock_guard<std::mutex> lock(m_lock);
  if (m_pass >= 0)
    close(m_pass);
  m_pass = -1;
  if (m_device != -1)
    close(m_device);
  m_device = -1;
  m_sectors = 0;
  m_layer_break = 0;
  m_media_type = 0;
  m_raw_off = false;
  m_toc.clear();

  m_device = open(m_filename.c_str(), O_RDONLY);
  if (m_device == -1)
  {
    const int e = errno;
    if (!m_said_unreadable)
    {
      std::printf("[disc] %s doesn't open: errno %d (%s)\n", m_filename.c_str(), e, std::strerror(e));
      std::fflush(stdout);
    }
    m_said_unreadable = true;
    if (e == ENXIO || e == EIO)
      Error::SetString(error, "No disc in the drive");
    else
      Error::SetErrno(error, e);
    return false;
  }
  std::string pass_name;
  m_pass = OrbisDiscDrive::OpenPassThrough(m_device, &pass_name);
  if (m_pass >= 0)
  {
    // A disc change (or the drive's reset) is reported once, as UNIT ATTENTION: taken here, so that DiscReady's next one
    // means a real change.
    orbis_mmc::Sense sense;
    for (int i = 0; i < 3 && !OrbisDiscDrive::Ready(m_pass, &sense) && sense.valid && sense.key == 6; i++)
    {
    }
  }

  // DVD first, as on Linux: cd(4) answers the table of contents for both.
  const bool known = ReadDVDInfo() || ReadCDInfo();
  if (!known)
  {
    // No disc, one still spinning up, or not a data disc. The device stays open: DiscReady asks the drive again (at most
    // every 500 ms) and reads the disc once it's ready.
    if (!m_said_unreadable)
    {
      std::printf("[disc] %s: neither a DVD's layers nor a CD's table of contents read (no disc, still spinning up, or "
                  "not a data disc); pass-through %s\n", m_filename.c_str(), m_pass >= 0 ? pass_name.c_str() : "not found");
      std::fflush(stdout);
    }
    m_said_unreadable = true;
    Error::SetString(error, "The disc in the drive can't be read");
    return false;
  }
  m_said_unreadable = false;
  const std::string drive = m_pass >= 0 ? OrbisDiscDrive::Describe(m_pass) : std::string();
  std::printf("[disc] %s: %s, %s, %u sectors, layer break %u, %zu track(s); pass-through %s%s\n", m_filename.c_str(),
    drive.empty() ? "(drive not named)" : drive.c_str(),
    m_media_type < 0 ? "CD" : m_media_type == 0 ? "DVD, one layer" : m_media_type == 1 ? "DVD, two layers (PTP)" : "DVD, two layers (OTP)",
    m_sectors, m_layer_break, m_toc.size(), m_pass >= 0 ? pass_name.c_str() : "not found",
    m_pass < 0 && m_media_type < 0 ? " (CD sectors made from their data; XA streams won't play)" : "");
  std::fflush(stdout);
  return true;
}

bool IOCtlSrc::ReadDVDInfo()
{
  struct dvd_struct ds;
  std::memset(&ds, 0, sizeof(ds));
  ds.format = DVD_STRUCT_PHYSICAL;
  ds.layer_num = 0;
  if (ioctl(m_device, DVDIOCREADSTRUCTURE, &ds) != 0)
    return false;
  struct dvd_layer l0;
  std::memcpy(&l0, ds.data, sizeof(l0));
  const u32 start_sector = l0.start_sector;
  const u32 end_sector = l0.end_sector;
  if (end_sector < start_sector || end_sector == 0)
    return false;
  if (l0.nlayers == 0)
  {
    // Single layer
    m_media_type = 0;
    m_layer_break = 0;
    m_sectors = end_sector - start_sector + 1;
  }
  else if (l0.track_path == 0)
  {
    // Dual layer, Parallel Track Path
    ds.layer_num = 1;
    if (ioctl(m_device, DVDIOCREADSTRUCTURE, &ds) != 0)
      return false;
    struct dvd_layer l1;
    std::memcpy(&l1, ds.data, sizeof(l1));
    m_media_type = 1;
    m_layer_break = end_sector - start_sector;
    m_sectors = end_sector - start_sector + 1 + l1.end_sector - l1.start_sector + 1;
  }
  else
  {
    // Dual layer, Opposite Track Path
    const u32 end_sector_layer0 = l0.end_sector_l0;
    m_media_type = 2;
    m_layer_break = end_sector_layer0 - start_sector;
    m_sectors = end_sector_layer0 - start_sector + 1 + end_sector - (~end_sector_layer0 & 0xFFFFFFU) + 1;
  }
  // The drive's own size, when it's smaller (a burned disc's physical format can describe the blank disc).
  off_t bytes = 0;
  if (ioctl(m_device, DIOCGMEDIASIZE, &bytes) == 0 && bytes > 0 && static_cast<u64>(bytes) / 2048 < m_sectors)
  {
    std::printf("[disc] the DVD's physical format says %u sectors, the drive %llu: using the drive's\n", m_sectors,
      static_cast<unsigned long long>(bytes / 2048));
    m_sectors = static_cast<u32>(bytes / 2048);
  }
  return true;
}

bool IOCtlSrc::ReadCDInfo()
{
  struct ioc_toc_header header;
  if (ioctl(m_device, CDIOREADTOCHEADER, &header) != 0 || header.ending_track < header.starting_track)
    return false;
  struct cd_toc_entry entries[100];
  std::memset(entries, 0, sizeof(entries));
  struct ioc_read_toc_entry req;
  std::memset(&req, 0, sizeof(req));
  req.address_format = CD_LBA_FORMAT;
  req.starting_track = header.starting_track;
  const int tracks = header.ending_track - header.starting_track + 1; // and the lead-out after them
  req.data_len = static_cast<u_short>(sizeof(entries[0]) * (tracks + 1 > 100 ? 100 : tracks + 1));
  req.data = entries;
  if (ioctl(m_device, CDIOREADTOCENTRYS, &req) != 0)
    return false;
  m_toc.clear();
  u32 leadout = 0;
  for (int i = 0; i < tracks + 1 && i < 100; i++)
  {
    const u32 lba = ntohl(static_cast<u32>(entries[i].addr.lba)); // network byte order (cdio.h)
    if (entries[i].track == 0xAA)
    {
      leadout = lba;
      break;
    }
    m_toc.push_back({lba, entries[i].track, static_cast<u8>(entries[i].addr_type), static_cast<u8>(entries[i].control)});
  }
  if (leadout == 0)
  {
    off_t bytes = 0;
    if (ioctl(m_device, DIOCGMEDIASIZE, &bytes) != 0 || bytes <= 0)
      return false;
    leadout = static_cast<u32>(bytes / 2048);
  }
  if (leadout == 0 || m_toc.empty())
    return false;
  m_sectors = leadout;
  m_media_type = -1;
  return true;
}

void IOCtlSrc::SetSpindleSpeed(bool restore_defaults) const
{
  (void)restore_defaults;
}

u32 IOCtlSrc::GetSectorCount() const
{
  return m_sectors;
}

const std::vector<toc_entry>& IOCtlSrc::ReadTOC() const
{
  return m_toc;
}

bool IOCtlSrc::ReadSectors2048(u32 sector, u32 count, u8* buffer) const
{
  std::lock_guard<std::mutex> lock(m_lock);
  return m_device != -1 && Pread2048(m_device, sector, count, buffer);
}

bool IOCtlSrc::ReadSectors2352(u32 sector, u32 count, u8* buffer) const
{
  std::lock_guard<std::mutex> lock(m_lock);
  if (m_device == -1)
    return false;
  if (m_pass >= 0 && !m_raw_off &&
      OrbisDiscDrive::ReadRaw(m_pass, sector, count, buffer, AllData(m_toc, m_sectors, sector, sector + count - 1)))
    return true;
  // No pass-through, or READ CD refused (or short): the sectors made from their 2048 bytes of data.
  u8 data[2048];
  for (u32 n = 0; n < count; n++)
  {
    if (!Pread2048(m_device, sector + n, 1, data))
      return false;
    MakeRaw(sector + n, data, buffer + n * 2352);
  }
  if (m_pass >= 0 && !m_raw_off)
  {
    // The data reads but READ CD doesn't: don't send it again for this disc (each try is a failed command).
    m_raw_off = true;
    std::printf("[disc] %s: READ CD doesn't work here: CD sectors made from their data from now on (XA streams won't play)\n",
      m_filename.c_str());
    std::fflush(stdout);
  }
  return true;
}

bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ* subq) const
{
  std::lock_guard<std::mutex> lock(m_lock);
  u8 data[16] = {};
  if (m_pass < 0 || !OrbisDiscDrive::Command(m_pass, orbis_mmc::ReadSubChannelQ(sizeof(data)), data, sizeof(data), 5000))
    return false;
  const orbis_mmc::SubQ q = orbis_mmc::ParseSubChannelQ(data, sizeof(data));
  if (!q.ok)
    return false;
  subq->adr = q.adr;
  subq->trackNum = q.track;
  subq->trackIndex = q.index;
  return true;
}

u32 IOCtlSrc::GetLayerBreakAddress() const
{
  return m_layer_break;
}

s32 IOCtlSrc::GetMediaType() const
{
  return m_media_type;
}

bool IOCtlSrc::DiscReady()
{
  {
    std::lock_guard<std::mutex> lock(m_lock);
    const s64 now = NowMs();
    if (m_ready_checked_ms >= 0 && now - m_ready_checked_ms < kReadyEveryMs)
      return m_ready && m_sectors != 0;
    m_ready_checked_ms = now;
    // m_device == -1: it didn't open last time (no disc in it then): Reopen below tries again.
    if (m_device != -1)
    {
      bool changed = false;
      if (m_pass >= 0)
      {
        orbis_mmc::Sense sense;
        m_ready = OrbisDiscDrive::Ready(m_pass, &sense);
        changed = !m_ready && sense.valid && sense.key == 6; // UNIT ATTENTION: the disc changed (or the drive reset)
      }
      else
      {
        off_t bytes = 0;
        m_ready = ioctl(m_device, DIOCGMEDIASIZE, &bytes) == 0 && bytes > 0;
      }
      if (changed || !m_ready)
      {
        // PCSX2 sees the tray open now; the next answer (500 ms on) reads the disc that's in then.
        if (m_sectors)
        {
          std::printf("[disc] %s: %s\n", m_filename.c_str(), changed ? "the drive reports a disc change" : "the disc is out (or not ready)");
          std::fflush(stdout);
        }
        m_ready = false;
        m_sectors = 0;
        m_layer_break = 0;
        m_media_type = 0;
        return false;
      }
      if (m_sectors)
        return true;
      // a disc is in that hasn't been read yet: Reopen below
    }
  }
  // Outside the lock: Reopen takes it.
  const bool ok = Reopen(nullptr);
  std::lock_guard<std::mutex> lock(m_lock);
  m_ready = ok;
  m_ready_checked_ms = NowMs();
  return ok && m_sectors != 0;
}

std::vector<std::string> GetOpticalDriveList()
{
  return OrbisDiscDrive::List();
}

void GetValidDrive(std::string& drive)
{
  if (!drive.empty())
    return;
  const std::vector<std::string> drives = OrbisDiscDrive::List();
  if (!drives.empty())
    drive = drives.front();
}
