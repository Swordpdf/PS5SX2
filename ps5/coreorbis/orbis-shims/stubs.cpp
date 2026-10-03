// Orbis stubs: assert/log/crash/window/http scaffolding.
// Replaces: Assertions.cpp (Windows-only headers), CrashHandler.cpp,
// StackWalker.cpp, WindowInfo.cpp (X11), HTTPDownloaderCurl.cpp (no curl).

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/HTTPDownloader.h"
#include "common/HostSys.h"
#include "common/WindowInfo.h"

void pxOnAssertFail(const char* file, int line, const char* func, const char* msg)
{
  printf("[assert] FAIL %s:%d in %s: %s\n", file, line, func, msg);
  fflush(stdout);
  abort();
}

namespace CrashHandler
{
bool Install()
{
  return true;
}
void SetWriteDirectory(std::string_view dump_directory)
{
  (void)dump_directory;
}
void WriteDumpForCaller()
{
}
} // namespace CrashHandler

std::optional<float> WindowInfo::QueryRefreshRateForWindow(const WindowInfo& wi)
{
  (void)wi;
  return std::nullopt;
}

#ifndef PS5SX2_ACHIEVEMENTS
std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(std::string user_agent)
{
  (void)user_agent;
  return nullptr;
}
#endif

// HostSys.cpp (excluded: needs cpuinfo lib) replacements.
const CPUInfo& GetCPUInfo()
{
  // PS5 Pro: 8C/16T Zen2. poc2 measured ncpu=16.
  static const CPUInfo info = {"AMD Zen2 (PS5 Pro)", 8, 0, 16, 1};
  return info;
}

u32 ShortSpin()
{
  return 0;
}

const u32 SPIN_TIME_NS = 0;

void AbortWithMessage(const char* msg)
{
  printf("[abort] %s\n", msg);
  fflush(stdout);
  abort();
}
