// Orbis replacement for common/Linux/LnxMisc.cpp (which needs dbus/X11/sysinfo).
// Uses only SDK libc: sysconf, clock_gettime, usleep.

#include "common/HostSys.h"
#include "common/Threading.h"

#include <ctime>
#include <fcntl.h>
#include <string>
#include <unistd.h>

struct if_nameindex;
extern "C" struct if_nameindex* if_nameindex(void)
{
  return 0;
}
extern "C" void if_freenameindex(struct if_nameindex* ptr)
{
  (void)ptr;
}

// SDK libc provides F_LOCK/F_TLOCK/F_ULOCK/F_TEST constants but no lockf().
extern "C" int lockf(int fd, int function, off_t size)
{
  struct flock fl = {};
  fl.l_whence = SEEK_CUR;
  fl.l_start = 0;
  fl.l_len = size;
  fl.l_pid = 0;
  switch (function)
  {
    case F_LOCK:
      fl.l_type = F_WRLCK;
      return fcntl(fd, F_SETLKW, &fl);
    case F_TLOCK:
      fl.l_type = F_WRLCK;
      return fcntl(fd, F_SETLK, &fl);
    case F_ULOCK:
      fl.l_type = F_UNLCK;
      return fcntl(fd, F_SETLK, &fl);
    case F_TEST:
      fl.l_type = F_WRLCK;
      if (fcntl(fd, F_GETLK, &fl) != 0)
        return -1;
      return (fl.l_type == F_UNLCK) ? 0 : -1;
    default:
      return -1;
  }
}

u64 GetPhysicalMemory()
{
#ifdef _SC_PHYS_PAGES
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long pagesz = sysconf(_SC_PAGESIZE);
  if (pages > 0 && pagesz > 0)
    return static_cast<u64>(pages) * static_cast<u64>(pagesz);
#endif
  return 0;
}

u64 GetAvailablePhysicalMemory()
{
  // Payload sandbox: poc2 showed ~2.6GB visible, 2GB malloc OK.
  // Fixed conservative figure until direct-memory query is wired.
  return 6ULL * 1024ULL * 1024ULL * 1024ULL;
}

u64 GetTickFrequency()
{
  return 1000000000; // clock_gettime resolution: nanoseconds
}

u64 GetCPUTicks()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<u64>(ts.tv_sec) * 1000000000ULL + static_cast<u64>(ts.tv_nsec);
}

std::string GetOSVersionString()
{
  return "Orbis";
}

bool Common::InhibitScreensaver(bool inhibit)
{
  (void)inhibit;
  return true;
}

bool Common::PlaySoundAsync(const char* path)
{
  (void)path;
  return false;
}

void Common::SetMousePosition(int x, int y)
{
  (void)x;
  (void)y;
}

bool Common::AttachMousePositionCb(std::function<void(int, int)> cb)
{
  (void)cb;
  return false;
}

void Common::DetachMousePositionCb()
{
}

void Threading::Sleep(int ms)
{
  usleep(1000 * ms);
}

void Threading::SleepUntil(u64 ticks)
{
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(ticks / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ticks % 1000000000ULL);
  clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
}
