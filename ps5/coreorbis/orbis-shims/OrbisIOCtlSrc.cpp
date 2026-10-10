// PS5 port: PCSX2's IOCtlSrc (CDVD/CDVDdiscReader.h) on the PS5: physical discs in a disc drive.
//
// IOCtlSrc on a FreeBSD cd(4) device, a USB DVD or BD drive (OrbisDiscDrive.h). A DVD's sectors are pread from /dev/cdN;
// its layers come from DVDIOCREADSTRUCTURE, as Linux's IOCtlSrc reads them with DVD_READ_STRUCT. A CD's table of contents
// comes from CDIOREADTOCENTRYS, and its raw sectors and sub-channel from SCSI commands through the drive's pass-through
// device. DVDs were tried on the console (a TSSTcorp SN-208FB on USB); CDs still need proper testing.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

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

#include <cerrno>
#include <cstdio>
#include <cstring>

// (disc globals live in CDVDdiscReader.cpp already.)

namespace
{
// The pass-through device of the one IOCtlSrc there is (CDVDdiscReader.cpp's `src`); the class's members are PCSX2's.
int s_pass = -1;

void ClosePass()
{
  if (s_pass >= 0)
    close(s_pass);
  s_pass = -1;
}

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
} // namespace

IOCtlSrc::IOCtlSrc(std::string filename)
  : m_filename(std::move(filename))
{
}

IOCtlSrc::~IOCtlSrc()
{
  ClosePass();
  if (m_device != -1)
    close(m_device);
}

bool IOCtlSrc::Reopen(Error* error)
{
  ClosePass();
  if (m_device != -1)
    close(m_device);
  m_sectors = 0;
  m_layer_break = 0;
  m_media_type = 0;
  m_toc.clear();

  m_device = open(m_filename.c_str(), O_RDONLY);
  if (m_device == -1)
  {
    const int e = errno;
    printf("[disc] %s doesn't open: errno %d (%s)\n", m_filename.c_str(), e, strerror(e));
    fflush(stdout);
    if (e == ENXIO || e == EIO)
      Error::SetString(error, "No disc in the drive");
    else
      Error::SetErrno(error, e);
    return false;
  }
  s_pass = OrbisDiscDrive::OpenPassThrough(m_device);

  // DVD first, as on Linux: cd(4) answers the table of contents for both.
  const bool dvd = ReadDVDInfo();
  const bool known = dvd || ReadCDInfo();
  printf("[disc] %s: %s, %s, %u sectors, layer break %u, %zu track(s); pass-through %s%s\n", m_filename.c_str(),
    s_pass >= 0 ? OrbisDiscDrive::Describe(s_pass).c_str() : "(no pass-through)",
    !known ? "no disc read" : m_media_type < 0 ? "CD" : m_media_type == 0 ? "DVD, one layer" : m_media_type == 1 ? "DVD, two layers (PTP)" :
                                                                                                "DVD, two layers (OTP)",
    m_sectors, m_layer_break, m_toc.size(), s_pass >= 0 ? "open" : "not found",
    s_pass < 0 && m_media_type < 0 ? " (CD sectors made from their data; XA streams won't play)" : "");
  fflush(stdout);
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
  // The drive's own size, when it disagrees (a burned disc's physical format can describe the blank disc).
  off_t bytes = 0;
  if (ioctl(m_device, DIOCGMEDIASIZE, &bytes) == 0 && bytes > 0 && static_cast<u64>(bytes) / 2048 < m_sectors)
  {
    printf("[disc] the DVD's physical format says %u sectors, the drive %llu: using the drive's\n", m_sectors,
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
  const ssize_t bytes_to_read = 2048 * static_cast<ssize_t>(count);
  const ssize_t bytes_read = pread(m_device, buffer, static_cast<size_t>(bytes_to_read), sector * 2048ULL);
  if (bytes_read == bytes_to_read)
    return true;
  if (bytes_read == -1)
    printf("[disc] read sectors %u-%u failed: %s\n", sector, sector + count - 1, strerror(errno));
  else
    printf("[disc] read sectors %u-%u: %zd bytes read, %zd bytes expected\n", sector, sector + count - 1, bytes_read, bytes_to_read);
  return false;
}

bool IOCtlSrc::ReadSectors2352(u32 sector, u32 count, u8* buffer) const
{
  if (s_pass >= 0 && OrbisDiscDrive::ReadRaw(s_pass, sector, count, buffer))
    return true;
  // No pass-through, or READ CD refused: the sectors made from their 2048 bytes of data.
  u8 data[2048];
  for (u32 n = 0; n < count; n++)
  {
    if (!ReadSectors2048(sector + n, 1, data))
      return false;
    MakeRaw(sector + n, data, buffer + n * 2352);
  }
  return true;
}

bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ* subq) const
{
  // READ SUB-CHANNEL, the current position's Q data (MSF): ADR and control, track, index.
  u8 data[16] = {};
  const u8 cdb[10] = {0x42, 0x02, 0x40, 0x01, 0, 0, 0, 0, sizeof(data), 0};
  if (s_pass < 0 || !OrbisDiscDrive::Command(s_pass, cdb, sizeof(cdb), data, sizeof(data), 5000))
    return false;
  subq->adr = data[5] >> 4;
  subq->trackNum = data[6];
  subq->trackIndex = data[7];
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
  if (m_device == -1)
    return false;
  bool ready;
  if (s_pass >= 0)
  {
    OrbisDiscDrive::Sense sense;
    ready = OrbisDiscDrive::Ready(s_pass, &sense);
    if (!ready && sense.key == 6) // UNIT ATTENTION: a disc went in (or the drive reset); the next one says how it is now
      ready = OrbisDiscDrive::Ready(s_pass, &sense);
    if (!ready && sense.key == 6)
      ready = OrbisDiscDrive::Ready(s_pass, &sense);
  }
  else
  {
    off_t bytes = 0;
    ready = ioctl(m_device, DIOCGMEDIASIZE, &bytes) == 0 && bytes > 0;
  }
  if (ready)
  {
    if (!m_sectors)
      Reopen(nullptr);
  }
  else
  {
    m_sectors = 0;
    m_layer_break = 0;
    m_media_type = 0;
  }
  return !!m_sectors;
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
