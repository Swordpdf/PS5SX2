#!/usr/bin/env python3
"""Host tests for vk-285-113's log handling (no console needed).

The code under test lives inside main-boot.cpp (the boot log's hold and release) and frontend/fe_ps5.cpp (the settings
log's buffer and the note about the previous run). Both are cut out of the sources by their marker comments and built into
small programs, so the test follows the code:

  python3 ps5/coreorbis/tests/logs/test_logs.py

1. boot.log when /data isn't visible at the start: what the app prints is held, then written at the top of boot.log.
2. the settings log: lines written before /data or logs/ are there come out in order once they are; the first write of
   a run notes a previous run that ended without a closing line.
"""
import os, subprocess, sys, tempfile, shutil, textwrap

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
MAIN = open(os.path.join(ROOT, "ps5/coreorbis/main-boot.cpp")).read()
FE = open(os.path.join(ROOT, "ps5/frontend/fe_ps5.cpp")).read()

failures = 0


def check(name, ok, detail=""):
    global failures
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else "  " + detail))
    if not ok:
        failures += 1


def cut(src, start, end):
    a = src.index(start)
    b = src.index(end, a)
    return src[a:b]


def build(work, name, code):
    cpp = os.path.join(work, name + ".cpp")
    exe = os.path.join(work, name)
    open(cpp, "w").write(code)
    r = subprocess.run(["g++", "-std=c++17", "-O1", "-g", cpp, "-o", exe, "-lpthread"], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr)
        sys.exit("build failed: " + name)
    return exe


BOOT_HARNESS = r'''
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <mutex>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
static std::string g_root;
extern "C" { char g_orbis_pf_log[160]; }
static std::string OrbisLogPath(const char* name)
{
  struct stat st;
  const std::string logs = g_root + "/logs";
  if (stat(logs.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return logs + "/" + name;
  return g_root + "/" + name;
}
@BODY@
int main(int argc, char** argv)
{
  g_root = std::string(argv[1]) + "/PCSX2";
  const std::string mode = argv[2];
  signal(SIGPIPE, SIG_IGN);
  if (mode == "visible") { mkdir(g_root.c_str(), 0777); mkdir((g_root + "/logs").c_str(), 0777); }
  const bool first = orbis_boot_log_open(true);
  if (!first) orbis_boot_log_hold();
  setvbuf(stdout, nullptr, _IOFBF, 1 << 20);
  fprintf(stderr, "[boot] stderr-ok\n");
  printf("[boot] main\n");
  for (int i = 0; i < 3000; i++) printf("[boot] early line %d\n", i);
  fflush(stdout);
  orbis_boot_log_release("too early"); // /data isn't there yet (nothing happens); with mode "visible" nothing is held
  printf("[boot] between\n");
  fflush(stdout);
  if (mode != "visible") { mkdir(g_root.c_str(), 0777); mkdir((g_root + "/logs").c_str(), 0777); }
  orbis_boot_log_release("after the jailbreak");
  orbis_boot_log_release("again"); // a second call does nothing
  printf("[boot] after\n");
  fprintf(stderr, "[boot] stderr after\n");
  fflush(stdout);
  return 0;
}
'''

EVENT_HARNESS = r'''
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
namespace fe { long long (*g_utc_to_local)(long long) = nullptr; }
static long long SettingsLogLocalTime(long long utc) { return utc; }
@BODY@
static void put(const std::string& path, const char* text) { FILE* f = fopen(path.c_str(), "w"); fputs(text, f); fclose(f); }
int main(int argc, char** argv)
{
  const std::string root = std::string(argv[1]) + "/PCSX2";
  const std::string mode = argv[2];
  if (mode == "late")
  {
    orbis_event_log_init(root + "/settings.log");
    orbis_event_log("A app start");
    orbis_event_log("B console");
    mkdir(root.c_str(), 0777);
    mkdir((root + "/logs").c_str(), 0777);
    orbis_event_log("C after logs");
    return 0;
  }
  mkdir(root.c_str(), 0777);
  mkdir((root + "/logs").c_str(), 0777);
  const std::string log = root + "/logs/settings.log";
  if (mode == "killed") put(log, "2026-09-29 23:16:53  game start: X.iso\n2026-09-29 23:17:53  perf, last 60 s: 30.0 fps\n");
  if (mode == "menu") put(log, "2026-09-29 23:16:53  game start: X.iso\n2026-09-29 23:19:53  back to the menu\n");
  if (mode == "hang") put(log, "2026-09-29 23:16:53  GPU hang (VK_ERROR_DEVICE_LOST in vkQueueSubmit); the app closed itself. The dump is logs/vkhang.txt\n");
  if (mode == "signal") put(log, "2026-09-29 23:16:53  the system ended the app (signal 15)\n");
  if (mode == "crash") put(log, "2026-09-29 23:16:53  crash: signal 4 at eboot+0x1234 (fault address 0x0)\n");
  if (mode == "exit") put(log, "2026-09-29 23:16:53  app exit (the process is ending)\n");
  if (mode == "startonly") put(log, "2026-09-29 23:16:53  app start: vk-285-112 (pid 9)\n");
  if (mode == "garbage") put(log, "not a stamped line\n");
  if (mode == "empty") put(log, "");
  orbis_event_log_init(log);
  orbis_event_log("A app start");
  orbis_event_log("B second");
  return 0;
}
'''


def run(exe, work, mode):
    d = os.path.join(work, "run-" + mode)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    r = subprocess.run([exe, d, mode], capture_output=True, text=True, timeout=30)
    return d, r


def slurp(path):
    try:
        return open(path).read()
    except OSError:
        return None


def main():
    work = tempfile.mkdtemp(prefix="logs-test-")
    try:
        body = cut(MAIN, "// vk-285-51: the last sessions' logs are kept", "// vk-285-113: signals.")
        body = body.replace('stat("/data/PCSX2", &st)', "stat(g_root.c_str(), &st)")
        boot = build(work, "boot", BOOT_HARNESS.replace("@BODY@", body))

        # 1. /data isn't there at the start
        d, r = run(boot, work, "held")
        log = slurp(d + "/PCSX2/logs/boot.log") or ""
        lines = log.splitlines()
        check("held: the run ends normally", r.returncode == 0, r.stderr)
        check("held: boot.log is made after the release", bool(log))
        check("held: the note about the held lines comes first", bool(lines) and lines[0].startswith("[boot] the next ") and "after the jailbreak" in lines[0], lines[:1])
        check("held: stderr's line was kept", "[boot] stderr-ok" in lines)
        check("held: all 3000 early lines, in order", [l for l in lines if l.startswith("[boot] early line ")] == ["[boot] early line %d" % i for i in range(3000)])
        i_end = next((i for i, l in enumerate(lines) if l.startswith("[boot] end of the held lines")), -1)
        i_between = lines.index("[boot] between") if "[boot] between" in lines else -1
        check("held: the line printed before the release is among the held ones", 0 < i_between < i_end)
        check("held: what is printed after goes on in the same file, stderr too", lines[i_end + 1:] == ["[boot] after", "[boot] stderr after"], lines[i_end + 1:])
        check("held: pf.log was made", os.path.exists(d + "/PCSX2/logs/pf.log"))
        check("held: nothing in the top folder", not os.path.exists(d + "/PCSX2/boot.log"))

        # /data there from the start: as before, nothing held
        d, r = run(boot, work, "visible")
        log = slurp(d + "/PCSX2/logs/boot.log") or ""
        check("visible: no held-lines note", "the next " not in log and "end of the held lines" not in log)
        check("visible: early lines and the rest are in boot.log", "[boot] early line 2999" in log and "[boot] stderr after" in log)

        # 2. the settings log
        body = cut(FE, "// vk-285-113: the settings log on a console that shows /data only after the jailbreak.", "bool orbis_web_start(const OrbisFrontendPaths& paths")
        ev = build(work, "event", EVENT_HARNESS.replace("@BODY@", body))

        d, r = run(ev, work, "late")
        text = slurp(d + "/PCSX2/logs/settings.log") or ""
        got = [l[21:] for l in text.splitlines()]
        check("late: the two lines that waited come out first, in order, then the new one", got == ["A app start", "B console", "C after logs"], got)
        check("late: the file is in logs/", os.path.exists(d + "/PCSX2/logs/settings.log") and not os.path.exists(d + "/PCSX2/settings.log"))

        for mode, expect_note in [("killed", True), ("startonly", True), ("menu", False), ("hang", False), ("signal", False),
                                  ("crash", False), ("exit", False), ("garbage", False), ("empty", False)]:
            d, r = run(ev, work, mode)
            text = slurp(d + "/PCSX2/logs/settings.log") or ""
            has = "previous run: its log ends without a closing line" in text
            check("previous run (%s): note %s" % (mode, "written" if expect_note else "not written"), has == expect_note, text)
            got = [l[21:] for l in text.splitlines() if "previous run" not in l]
            check("previous run (%s): this run's lines follow" % mode, got[-2:] == ["A app start", "B second"], got)
            if has:
                lines = text.splitlines()
                i_note = next(i for i, l in enumerate(lines) if "previous run" in l)
                check("previous run (%s): the note comes before this run's first line" % mode, lines[i_note + 1].endswith("A app start"))
        d, r = run(ev, work, "killed")
        text = slurp(d + "/PCSX2/logs/settings.log")
        check("killed: the note names the last entry", '(last entry 2026-09-29 23:17:53: "perf, last 60 s: 30.0 fps")' in text, text)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\n%s" % ("all passed" if not failures else "%d failed" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
