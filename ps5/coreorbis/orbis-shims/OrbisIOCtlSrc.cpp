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
#include "OrbisDiscEject.h"
#include "OrbisDiscReadAhead.h"
#include "OrbisDiscPread64.h"
#include "OrbisDiscReady.h"

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

// The switch files (OrbisPaths.h). Weak, so the console test of this file (tests/discdrive), which does not link orbis_paths.cpp, still builds: every
// switch is off there.
bool OrbisFlag(const char* name) __attribute__((weak));

namespace
{
bool Flag(const char* name)
{
  return OrbisFlag && OrbisFlag(name);
}

// 2.16 (OrbisDiscEject.h): the tray lock cd(4) sets at open is taken off, so the drive's eject button works while a game plays from it; again every
// kAllowEveryMs from DiscReady (the driver can set it again when it looks at the media afresh). Switch file disc_lockeject: left locked (as before 2.16).
std::atomic<long long> s_allow_ms{0};
void KeepEjectFree(int fd, const std::string& name, long long now_ms, bool log)
{
  if (fd < 0 || Flag("disc_lockeject"))
  {
    if (log && fd >= 0)
    {
      std::printf("[disc] %s: the tray stays locked while the game plays (switch file disc_lockeject)\n", name.c_str());
      std::fflush(stdout);
    }
    return;
  }
  s_allow_ms.store(now_ms);
  const int e = orbis_disc::AllowEject(fd);
  static std::atomic<int> s_fail_said{0};
  if (log || (e && s_fail_said.fetch_add(1) < 3))
  {
    if (e)
      std::printf("[disc] %s: the tray lock could not be taken off (CDIOCALLOW errno %d %s): the eject button may not work while the game plays\n",
        name.c_str(), e, std::strerror(e));
    else
      std::printf("[disc] %s: tray lock taken off (CDIOCALLOW): the drive's eject button works while the game plays\n", name.c_str());
    std::fflush(stdout);
  }
}

s64 NowMs(); // forward decl: defined lower, used by MeasureCd0 below
// Sony's own read speed command (OrbisDiscScsi.h: SieSetReadSpeed): rotation 0 0x20 is the drive's 2.0x, 0x32 its 3.2x (4.4 MB/s on a
// DVD); rotation 1 0x80 is its 8.0 level - DiscSpeed8 (the owner's method) found the PS5 drive can take it through the pass device on some
// firmware, about twice the 3.2x it was limited to. Returns whether the drive took the command.
bool SetSonySpeed(int pass, uint16_t speed, uint8_t rotation = 0)
{
  orbis_mmc::Sense sense;
  return OrbisDiscDrive::Command(pass, orbis_mmc::SieSetReadSpeed(rotation, speed), nullptr, 0, 10000, &sense);
}

// A short timed read straight off /dev/cd0 in 64 KiB preads (DiscSpeed8's fast path), for the measured-speed line in the log after a speed
// command. Returns MB/s, or 0 when the disc is too small to time or a read failed. Reads 8 MiB after a 1 MiB warm-up, 4 MiB in from the
// inner edge so the drive is at its set class.
double MeasureCd0(int fd, u32 total_sectors)
{
  if (fd < 0)
    return 0.0;
  static u8 buf[64 * 1024];
  const u32 step = 32;                       // 32 sectors = 64 KiB per pread
  const u32 start = (4u << 20) / 2048;       // 4 MiB in
  const u32 warm = (1u << 20) / 2048;        // 1 MiB warm-up
  const u32 span = (8u << 20) / 2048;        // time 8 MiB
  if (total_sectors == 0 || start + warm + span > total_sectors)
    return 0.0;
  for (u32 s = start; s < start + warm; s += step)
    if (pread(fd, buf, sizeof(buf), static_cast<off_t>(s) * 2048) != static_cast<ssize_t>(sizeof(buf)))
      return 0.0;
  const s64 t0 = NowMs();
  u64 got = 0;
  for (u32 s = start + warm; s < start + warm + span; s += step)
  {
    if (pread(fd, buf, sizeof(buf), static_cast<off_t>(s) * 2048) != static_cast<ssize_t>(sizeof(buf)))
      return 0.0;
    got += sizeof(buf);
  }
  const double sec = (NowMs() - t0) / 1000.0;
  return sec > 0 ? static_cast<double>(got) / sec / 1e6 : 0.0;
}

// The read-ahead's counters as one line of boot.log when the drive is closed. (While a game plays, the CDVD thread's line every 10 s has them:
// CDVDdiscThread.cpp, IOCtlSrc::DriveStats.)
void LogReadAhead(const std::string& name, const orbis_disc::ReadAhead& ra)
{
  const orbis_disc::ReadAhead::Stats& s = ra.GetStats();
  std::printf("[disc] %s read-ahead %s: %llu reads asked, %llu served from memory, %llu window reads, %llu plain reads, %llu failed windows, "
              "%llu MiB from the drive\n",
    name.c_str(), ra.Enabled() ? "on" : "off (flag nodiscreadahead)", static_cast<unsigned long long>(s.calls),
    static_cast<unsigned long long>(s.hits), static_cast<unsigned long long>(s.window_reads), static_cast<unsigned long long>(s.direct_reads),
    static_cast<unsigned long long>(s.window_failures), static_cast<unsigned long long>(s.sectors_from_drive / 512));
  std::fflush(stdout);
}

s64 NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 2.11: the last time a read really reached the drive (Pread2048 and READ CD succeeded; NOT a read the windows answered from memory). DiscReady takes
// it as proof that the disc is there (no TEST UNIT READY then) and the keep-alive thread skips its read after one: one atomic for both
// (OrbisDiscReady.h). DiscReady asks the drive at most every ReadyPolicy::kEveryMs; the CDVD thread calls it on every loop.
orbis_disc::LastRead s_last_read;

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
  // DiscSpeed8: the PS5's cd(4) driver serves several small preads far faster than one big one (64 KiB measured 15-24 MB/s against ~5 for
  // 1 MiB on the same disc), so a read over 64 KiB is cut into 64 KiB preads (OrbisDiscPread64.h). The switch file disc_nopread64 restores
  // the single big pread.
  static const bool s_one_pread = Flag("disc_nopread64");
  const ssize_t got = orbis_disc::Pread64(buffer, static_cast<size_t>(want), static_cast<uint64_t>(sector) * 2048ULL, s_one_pread,
    [fd](void* b, size_t n, uint64_t o) { return pread(fd, b, n, static_cast<off_t>(o)); });
  if (got == want)
  {
    s_last_read.Note(NowMs());
    return true;
  }
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

s64 OrbisDiscLastReadMs()
{
  return s_last_read.Get();
}

IOCtlSrc::IOCtlSrc(std::string filename)
  : m_filename(std::move(filename))
{
  // 2.11: read-ahead windows (OrbisDiscReadAhead.h); the switch file nodiscreadahead turns them off.
  m_ahead = std::make_unique<orbis_disc::ReadAhead>();
  m_ahead->SetEnabled(!Flag("nodiscreadahead"));
  m_ready_policy = std::make_unique<orbis_disc::ReadyPolicy>();
}

IOCtlSrc::~IOCtlSrc()
{
  std::lock_guard<std::mutex> lock(m_lock);
  if (m_pass >= 0 && m_fast)
    SetSonySpeed(m_pass, 0x20); // the drive back to its 2.0x
  if (m_ahead && m_ahead->GetStats().calls)
    LogReadAhead(m_filename, *m_ahead);
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
  m_fast = false; // (a new disc: the drive is at its own speed again)
  m_ahead->Reset();
  m_ready_policy->Reset(); // (and the settle window of the old speed command)
  s_last_read.Clear();     // (a read of the old disc proves nothing about this one)
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
    KeepEjectFree(m_device, m_filename, NowMs(), false); // 2.16: a disc that can't be read must still come out with the eject button
    Error::SetString(error, "The disc in the drive can't be read");
    return false;
  }
  m_said_unreadable = false;
  const std::string drive = m_pass >= 0 ? OrbisDiscDrive::Describe(m_pass) : std::string();
  // The PS5's own drive reads a DVD at 2.0x (2.78 MB/s) unless asked; it takes 3.2x (4.4 MB/s, +59%) and nothing faster (disc drive test logs,
  // 2026-10-16, three consoles). 2.11: on by default for the PS5's own drive (SONY PS-SYSTEM) and DVDs; the switch file nodisc32 asks for 2.0x
  // instead (the old switch file disc32 does nothing now). A USB drive, a CD and a drive that refuses get no speed command.
  if (m_pass >= 0 && m_media_type >= 0 && drive.find("PS-SYSTEM") != std::string::npos)
  {
    // The drive keeps the speed it was given for as long as its disc stays in, and the app that gave it 3.2x may have been closed by the PS button
    // (nothing runs then to put it back): the switch file nodisc32 sends the 2.0x command, else a run "without 3.2x" would read at 3.2x all the same.
    // DiscSpeed8 (the owner's method): try Sony's 8.0 level (rotation 1, DB 01 00 80) first - the PS5 drive can take it through the pass device
    // on some firmware, about twice the 3.2x - and fall back to 3.2x (DB 00 00 32) if it refuses. The switch file disc_no8x skips the 8x try (3.2x
    // straight away, the 2.15 behaviour).
    const bool want_fast = !Flag("nodisc32");
    const char* speed_txt = "2.0x (switch file nodisc32)";
    bool taken = false;
    if (want_fast)
    {
      if (!Flag("disc_no8x") && SetSonySpeed(m_pass, 0x80, 1)) { taken = true; speed_txt = "8.0x (DB 01 00 80)"; }
      else if (SetSonySpeed(m_pass, 0x32, 0)) { taken = true; speed_txt = "3.2x (DB 00 00 32)"; }
      else speed_txt = "fast (refused)";
    }
    else
      taken = SetSonySpeed(m_pass, 0x20, 0);
    m_fast = taken && want_fast; // (a faster-than-default speed is put back to 2.0x by the destructor)
    // The speed change can be reported as UNIT ATTENTION by the next command: taken here, like the one at the start of this function, so
    // that DiscReady's next UNIT ATTENTION means a real disc change. The drive needs ~0.7 s to change speed (the first read after the
    // command took 497-710 ms): nothing is asked of it for 3 s after it (ReadyPolicy::kSettleMs).
    int unit_attentions = 0;
    orbis_mmc::Sense after;
    bool after_ready = false;
    if (taken)
    {
      for (int i = 0; i < 3 && !(after_ready = OrbisDiscDrive::Ready(m_pass, &after)) && after.valid && after.key == 6; i++)
        unit_attentions++;
      m_ready_policy->StartSettle(NowMs());
    }
    // Measure and log the real MB/s at the set speed (DiscSpeed8 wants 8x vs 3.2x shown); the switch file disc_nomeasure skips it.
    const double mbs = (m_fast && !Flag("disc_nomeasure")) ? MeasureCd0(m_device, m_sectors) : 0.0;
    char mbs_txt[48] = {0};
    if (mbs > 0)
      std::snprintf(mbs_txt, sizeof(mbs_txt), "; measured %.2f MB/s (%.2fx DVD)", mbs, mbs * 1e6 / 1385000.0);
    std::printf("[disc] %s: drive speed %s %s%s%s%s%s\n", m_filename.c_str(), speed_txt, taken ? "taken" : "refused",
      unit_attentions ? " (a UNIT ATTENTION followed it, taken)" : "", taken && !after_ready ? "; TEST UNIT READY right after it: " : "",
      taken && !after_ready ? orbis_mmc::SenseText(after).c_str() : "", mbs_txt);
    std::fflush(stdout);
  }
  std::printf("[disc] %s: %s, %s, %u sectors, layer break %u, %zu track(s); pass-through %s%s\n", m_filename.c_str(),
    drive.empty() ? "(drive not named)" : drive.c_str(),
    m_media_type < 0 ? "CD" : m_media_type == 0 ? "DVD, one layer" : m_media_type == 1 ? "DVD, two layers (PTP)" : "DVD, two layers (OTP)",
    m_sectors, m_layer_break, m_toc.size(), m_pass >= 0 ? pass_name.c_str() : "not found",
    m_pass < 0 && m_media_type < 0 ? " (CD sectors made from their data; XA streams won't play)" : "");
  std::fflush(stdout);
  // The drive just answered the disc's layers or table of contents: it is ready (DiscReady, which may not ask for a while after the speed
  // command, hands this on until it does).
  m_ready = true;
  KeepEjectFree(m_device, m_filename, NowMs(), true); // 2.16: after the layers / table of contents were read (the driver locks when it takes the media)
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
  (void)restore_defaults; // (the drive speed is set in Reopen and put back in the destructor)
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
  if (m_device == -1)
    return false;
  // Through the read-ahead windows: a seek costs the drive 150-250 ms, so a miss reads 128-512 KiB, not the 32 KiB PCSX2 asked for.
  return m_ahead->Read(sector, count, buffer, m_sectors, [this](u32 s, u32 c, u8* b) { return Pread2048(m_device, s, c, b); });
}

bool IOCtlSrc::ReadSectors2352(u32 sector, u32 count, u8* buffer) const
{
  std::lock_guard<std::mutex> lock(m_lock);
  if (m_device == -1)
    return false;
  if (m_pass >= 0 && !m_raw_off &&
      OrbisDiscDrive::ReadRaw(m_pass, sector, count, buffer, AllData(m_toc, m_sectors, sector, sector + count - 1)))
  {
    s_last_read.Note(NowMs()); // (a READ CD that reached the drive and came back whole)
    return true;
  }
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
  // 2.11: a DVD has no Q sub-channel (DISCreadSubQ fills in track 1, index 1 itself when this says no). Asking would send the drive a command, and
  // wait for m_lock behind the CDVD thread's window reads, in the middle of a game: the CPU thread called this from the CDVD status command.
  if (m_media_type >= 0)
    return false;
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
    // 2.16: the tray lock off again every few seconds (no command when it is off already)
    if (m_device != -1 && now - s_allow_ms.load() >= orbis_disc::kAllowEveryMs)
      KeepEjectFree(m_device, m_filename, now, false);
    // 2.11 (OrbisDiscReady.h): the drive is asked at most every 500 ms; not at all for 3 s after the speed command, nor within 2 s of a read that
    // really reached the drive (that read proves a known disc is there and spinning); and a doubtful answer must come twice in a row.
    const s64 proof = m_sectors != 0 && m_ready ? s_last_read.Get() : -1;
    if (!m_ready_policy->ShouldAsk(now, proof))
      return m_ready && m_sectors != 0;
    // m_device == -1: it didn't open last time (no disc in it then): Reopen below tries again.
    if (m_device != -1)
    {
      bool ready = false;
      orbis_mmc::Sense sense;
      if (m_pass >= 0)
      {
        ready = OrbisDiscDrive::Ready(m_pass, &sense);
      }
      else
      {
        off_t bytes = 0;
        ready = ioctl(m_device, DIOCGMEDIASIZE, &bytes) == 0 && bytes > 0;
        sense = {true, 2, 0x3A, 0}; // (no medium)
      }
      const orbis_disc::ReadyPolicy::Verdict verdict = m_ready_policy->Answer(ready, sense.valid, sense.key, sense.asc);
      if (verdict.out)
      {
        // PCSX2 sees the tray open now; the next answer (500 ms on) reads the disc that's in then.
        if (m_sectors)
        {
          std::printf("[disc] %s: %s (%s)\n", m_filename.c_str(), verdict.changed ? "the drive reports a disc change" : "the disc is out (or not ready)",
            sense.valid ? orbis_mmc::SenseText(sense).c_str() : "the command did not get through");
          std::fflush(stdout);
        }
        m_ready = false;
        m_sectors = 0;
        m_layer_break = 0;
        m_media_type = 0;
        m_fast = false;
        m_ahead->Reset();
        s_last_read.Clear();
        return false;
      }
      if (verdict.doubt)
      {
        // Not ready, but not "no disc" nor a disc change: once is not an eject. The next question is 250 ms on.
        static std::atomic<int> s_doubts{0};
        if (s_doubts.fetch_add(1) < 20)
        {
          if (verdict.own_ua) // (the first answer after our speed command: the drive reported the speed change late, not a new disc)
            std::printf("[disc] %s: the drive reported a UNIT ATTENTION (%s) at the first question after the speed command: taken for the speed change, "
                        "asking again in %d ms\n", m_filename.c_str(), orbis_mmc::SenseText(sense).c_str(), static_cast<int>(orbis_disc::ReadyPolicy::kRetryMs));
          else
            std::printf("[disc] %s: the drive answered not ready (%s): asking again in %d ms before taking the disc for out\n", m_filename.c_str(),
              sense.valid ? orbis_mmc::SenseText(sense).c_str() : "the command did not get through", static_cast<int>(orbis_disc::ReadyPolicy::kRetryMs));
          std::fflush(stdout);
        }
        return m_ready && m_sectors != 0;
      }
      m_ready = true;
      if (m_sectors)
        return true;
      // a disc is in that hasn't been read yet: Reopen below
    }
  }
  // Outside the lock: Reopen takes it.
  const bool ok = Reopen(nullptr);
  std::lock_guard<std::mutex> lock(m_lock);
  m_ready = ok;
  m_ready_policy->Reopened(NowMs());
  return ok && m_sectors != 0;
}

// 2.11: the drive's part of the CDVD thread's line every 10 s (CDVDdiscThread.cpp). try_lock: the line is not worth waiting for a read in flight.
size_t IOCtlSrc::DriveStats(char* out, size_t size) const
{
  if (!out || size == 0)
    return 0;
  orbis_disc::ReadAhead::Stats now_stats;
  bool fast = false;
  bool enabled = false;
  {
    std::unique_lock<std::mutex> lock(m_lock, std::try_to_lock);
    if (!lock.owns_lock())
      return 0;
    now_stats = m_ahead->GetStats();
    enabled = m_ahead->Enabled();
    fast = m_fast;
  }
  // (the counters count from the start of the drive object: the line shows what changed since the last one)
  static orbis_disc::ReadAhead::Stats before;
  if (now_stats.calls < before.calls)
    before = orbis_disc::ReadAhead::Stats();
  const unsigned long long windows = now_stats.window_reads - before.window_reads;
  const unsigned long long failed = now_stats.window_failures - before.window_failures;
  const unsigned long long hits = now_stats.hits - before.hits;
  const unsigned long long direct = now_stats.direct_reads - before.direct_reads;
  const unsigned long long mib = (now_stats.sectors_from_drive - before.sectors_from_drive) / 512;
  before = now_stats;
  const s64 last = s_last_read.Get();
  char seen[40];
  if (last < 0)
    std::snprintf(seen, sizeof(seen), "never");
  else
    std::snprintf(seen, sizeof(seen), "%lld ms ago", static_cast<long long>(NowMs() - last));
  const int n = std::snprintf(out, size, "read-ahead %s: windows %llu%s, hits %llu, plain reads %llu, %llu MiB from the drive | speed %s | drive read last: %s",
    enabled ? "on" : "off", windows, failed ? (" (" + std::to_string(failed) + " failed)").c_str() : "", hits, direct, mib,
    fast ? "3.2x" : "the drive's own (2.0x on the PS5's)", seen);
  return n < 0 ? 0 : (static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1);
}

// 2.11: for CDVDdiscThread.cpp (weak there: absent in the PC tests of the thread).
size_t OrbisDiscDriveStats(char* out, size_t size)
{
  return src ? src->DriveStats(out, size) : 0;
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
