// Orbis CDVD: .gz images need PCSX2's indexed-zlib reader (GzippedFileReader), which isn't
// built, so that backend refuses. Raw .iso/.bin works fully via the real InputIsoFile
// (kept in build); .chd since vk-285-108 (the real ChdFileReader over libchdr, with zlib and
// zstd vendored in ps5/third_party); .cso and .zso since vk-285-113 (the real CsoFileReader over
// the vendored inflate and lz4).
//
// 2026-10-10: a PS2 DVD can be played from the drive (flag discplay, ProsperoDiscDump.cpp). PCSX2's disc thread
// (CDVDdiscThread.cpp) asks this IOCtlSrc for 16 sectors at a time. DVD sectors are pread on /dev/cd0, the same read
// the dumper uses. The layer break comes from the drive (ProsperoDiscSpeed.cpp), and the drive is set to Sony's 3.2x.
// A PS2 CD is not played from the console's own drive: /dev/cd0 does not give its 2352-byte sectors.
//
// 2026-10-11: a USB DVD drive is only a pass device here (the cd driver does not attach). That path is read with MMC
// (READ(10) for a PS2 DVD, READ CD for a PS2 CD). Needs proper testing on a console with a USB DVD drive.
#include "CDVD/GzippedFileReader.h"
#include "CDVD/CDVDdiscReader.h"
#include "common/Error.h"
#include "ProsperoDiscSpeed.h"

#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace
{
std::mutex s_disc_io;
bool s_layer_failed = false;

bool IsPassNode(const std::string& path)
{
  const size_t slash = path.rfind('/');
  const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  return name.compare(0, 4, "pass") == 0;
}

bool ReadAt(int fd, uint64_t offset, void* buf, size_t len)
{
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (len)
  {
    const ssize_t n = pread(fd, p, len, static_cast<off_t>(offset));
    if (n <= 0)
      return false;
    p += n;
    offset += static_cast<uint64_t>(n);
    len -= static_cast<size_t>(n);
  }
  return true;
}
} // namespace

IOCtlSrc::IOCtlSrc(std::string filename)
  : m_filename(std::move(filename))
{
}

IOCtlSrc::~IOCtlSrc()
{
  if (m_device >= 0 && !OrbisUsbDiscPath(m_filename))
  {
    SetSpindleSpeed(true);
    close(m_device);
    m_device = -1;
  }
}

bool IOCtlSrc::ReadDVDInfo()
{
  OrbisDiscLayer layer;
  const bool ok = OrbisUsbDiscPath(m_filename) ? OrbisUsbDiscReadLayer(&layer)
                  : IsPassNode(m_filename)    ? OrbisDiscReadLayerPass(m_device, &layer)
                                              : OrbisDiscReadLayer(m_device, &layer);
  if (!ok)
  {
    std::printf("[disc] %s\n", layer.why.c_str());
    std::fflush(stdout);
    return false;
  }
  m_media_type = layer.media_type;
  m_sectors = layer.sectors;
  m_layer_break = layer.layer_break;
  m_toc.clear();
  return m_sectors > 0;
}

bool IOCtlSrc::ReadCDInfo()
{
  // The console's own /dev/cd0 does not give a PS2 CD's 2352-byte sectors. A USB pass device does, via READ CD.
  if (!IsPassNode(m_filename) && !OrbisUsbDiscPath(m_filename))
    return false;
  OrbisCdToc toc;
  if (!(OrbisUsbDiscPath(m_filename) ? OrbisUsbDiscReadToc(&toc) : OrbisPassReadCdToc(m_device, &toc)))
    return false;
  m_toc.clear();
  for (uint32_t i = 0; i < toc.count; i++)
  {
    const OrbisCdTrack& t = toc.tracks[i];
    m_toc.push_back({t.lba, t.number, t.adr, t.control});
  }
  m_sectors = toc.leadout;
  m_layer_break = 0;
  m_media_type = -1;
  std::printf("[disc] USB CD: %u tracks, lead-out %u\n", toc.count, toc.leadout);
  std::fflush(stdout);
  return m_sectors > 0;
}

bool IOCtlSrc::Reopen(Error* error)
{
  std::lock_guard<std::mutex> lock(s_disc_io);
  if (m_device >= 0 && !OrbisUsbDiscPath(m_filename))
  {
    SetSpindleSpeed(true);
    close(m_device);
    m_device = -1;
  }
  m_sectors = 0;
  m_layer_break = 0;
  m_media_type = 0;
  const bool usb = OrbisUsbDiscPath(m_filename);
  const bool pass = IsPassNode(m_filename);
  if (usb)
  {
    std::string what, why;
    if (!OrbisUsbDiscFind(&what, &why))
    {
      Error::SetString(error, why.empty() ? "could not open the USB drive" : why.c_str());
      return false;
    }
    m_device = 1; // the session is the libSceUsbd handle, not a file descriptor
  }
  else
    m_device = OrbisOpenDev(m_filename.c_str(), pass ? O_RDWR : O_RDONLY);
  if (m_device < 0)
  {
    Error::SetString(error, "could not open the disc drive");
    std::printf("[disc] open %s: errno %d\n", m_filename.c_str(), errno);
    std::fflush(stdout);
    return false;
  }
  if (pass)
    OrbisPassReleaseQueue(m_device);
  // A PS2 CD answers READ DVD STRUCTURE with sense 05/30/02. The TOC is what makes it a CD.
  if (!ReadDVDInfo() && !((pass || usb) && ReadCDInfo()))
  {
    Error::SetString(error, (pass || usb) ? "the USB drive did not report a PS2 DVD or a CD"
                                          : "the drive did not report a PS2 DVD (a PS2 CD is not played from the console drive)");
    if (usb)
      OrbisUsbDiscRelease();
    else
      close(m_device);
    m_device = -1;
    return false;
  }
  SetSpindleSpeed(false);
  s_layer_failed = false;
  std::printf("[disc] playing %s: %u sectors, %s, layer break %u\n", m_filename.c_str(), m_sectors,
    m_media_type < 0 ? "CD" : m_media_type == 0 ? "single layer" : m_media_type == 1 ? "parallel dual layer" : "opposite dual layer", m_layer_break);
  std::fflush(stdout);
  return true;
}

void IOCtlSrc::SetSpindleSpeed(bool restore_defaults) const
{
  if (m_device < 0)
    return;
  if (OrbisUsbDiscPath(m_filename))
  {
    // 2x DVD / 4x CD. Raw CD sectors came back short (1328 of 2352) only at the outer edge, where a CAV drive
    // is fastest; the inner tracks read whole. Needs proper testing.
    const uint16_t kbs = restore_defaults ? 0xFFFF : (m_media_type >= 0 ? 2770 : 706);
    OrbisUsbDiscSetSpeed(kbs);
    return;
  }
  if (IsPassNode(m_filename))
  {
    // A PC drive takes SET CD SPEED. Sony's vendor command is only for the console's own drive.
    const uint16_t kbs = restore_defaults ? 0xFFFF : (m_media_type >= 0 ? 5540 : 1408);
    OrbisPassSetSpeed(m_device, kbs);
    return;
  }
  OrbisDiscSetPlaySpeed(m_device, restore_defaults);
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
  if (count == 0)
    return false;
  std::lock_guard<std::mutex> lock(s_disc_io);
  if (m_device < 0)
    return false;
  if (OrbisUsbDiscPath(m_filename))
    return OrbisUsbDiscRead2048(sector, count, buffer);
  if (IsPassNode(m_filename))
    return OrbisPassRead2048(m_device, sector, count, buffer);
  return ReadAt(m_device, static_cast<uint64_t>(sector) * 2048, buffer, static_cast<size_t>(count) * 2048);
}

bool IOCtlSrc::ReadSectors2352(u32 sector, u32 count, u8* buffer) const
{
  if ((!IsPassNode(m_filename) && !OrbisUsbDiscPath(m_filename)) || count == 0)
    return false;
  std::lock_guard<std::mutex> lock(s_disc_io);
  if (m_device < 0)
    return false;
  if (OrbisUsbDiscPath(m_filename))
    return OrbisUsbDiscRead2352(sector, count, buffer);
  return OrbisPassRead2352(m_device, sector, count, buffer);
}

bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ* subq) const
{
  (void)subq;
  return false;
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
    std::lock_guard<std::mutex> lock(s_disc_io);
    if (m_device < 0)
      return false;
    const bool pass = IsPassNode(m_filename);
    bool present = false;
    if (OrbisUsbDiscPath(m_filename))
      present = OrbisUsbDiscReady();
    else if (pass)
      present = OrbisPassReady(m_device);
    else
    {
      off_t media = 0;
      present = ioctl(m_device, DIOCGMEDIASIZE, &media) == 0 && media > 0;
    }
    if (!present)
    {
      m_sectors = 0;
      m_layer_break = 0;
      s_layer_failed = false;
      return false;
    }
    if (m_sectors != 0)
      return true;
    if (s_layer_failed)
      return false;
  }
  if (!Reopen(nullptr))
  {
    s_layer_failed = true;
    return false;
  }
  return m_sectors != 0;
}

void GetValidDrive(std::string& drive)
{
  if (drive.empty())
    drive = "/dev/cd0";
  if (OrbisUsbDiscPath(drive))
    return;
  struct stat st{};
  if (stat(drive.c_str(), &st) != 0)
    drive.clear();
}

static bool Unsupported(Error* error, const char* what)
{
  Error::SetString(error, what);
  return false;
}

GzippedFileReader::GzippedFileReader() = default;
GzippedFileReader::~GzippedFileReader() = default;
bool GzippedFileReader::Open2(std::string filename, Error* error)
{
  (void)filename;
  return Unsupported(error, "GZIP images not supported on Orbis (use .cso, .zso or .chd)");
}
ThreadedFileReader::Chunk GzippedFileReader::ChunkForOffset(u64 offset)
{
  (void)offset;
  return ThreadedFileReader::Chunk{-1, 0, 0};
}
int GzippedFileReader::ReadChunk(void* dst, s64 chunkID)
{
  (void)dst;
  (void)chunkID;
  return -1;
}
void GzippedFileReader::Close2()
{
}
u32 GzippedFileReader::GetBlockCount() const
{
  return 0;
}
