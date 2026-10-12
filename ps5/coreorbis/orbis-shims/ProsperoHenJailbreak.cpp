// PS5SX2 cooperative privilege elevation via the Lapy owned-root daemon.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// live-15 (AI-assisted; swordpdf): replaces the etaHEN/OnionHEN download0
// marker protocol with the Lapy cooperative elevation protocol. The resident
// answerer is the Lapy owned-root daemon (blackbearreloaded/PS5-Lapy-JB-Daemon
// PR #50). Nothing watches the old /download0/etahen_jailbreak name any more;
// that write is gone (the old broker that produced it is removed below).
// [2.02: not so -- etaHEN, OnionHEN and the PS5SX2 Helper still watch it; the
// request is back as etahen_request(), see the 2.02 note below.]
//
// Protocol (orbis_lapy_jailbreak, called once from main-boot.cpp BEFORE any
// thread other than the main thread exists -- the daemon rejects a target with
// more than one thread and consumes the marker either way, so a multi-threaded
// request is silently lost):
//   1. seteuid(geteuid()) and require 0: the process gets a private ucred, which
//      the daemon needs. If it fails, do not write the request (the caller takes
//      the no-jailbreak fallback).
//   2. Write a complete JSON body to /download0/elevate_proc: {"PID":<pid>}\n
//      (unlink, O_WRONLY|O_CREAT|O_EXCL, one full write, close -- the daemon must
//      only ever see a complete body).
//   3. The daemon unlinks the marker when it takes it. Poll access(...) ==
//      ENOENT every 50 ms for up to 10 s. The daemon ptrace-stops this process
//      during the transaction; the pause is normal, not an error.
//   4. On acknowledgement, verify /data with a real round trip (create O_EXCL,
//      write, close, reopen, read back, compare, unlink). Only a round trip that
//      matches counts as elevated.
//   5. Always write /download0/lapy_owned_result "DATA_OK=%d OPEN_ERRNO=%d\n"
//      after the verification, success or failure.
//   6. A marker written before the daemon started is stale (the daemon ignores
//      markers older than its own start time), so an acknowledgement timeout is
//      not a failure: rewrite the marker and wait again, up to 4 attempts
//      (~40 s). After the last attempt, the caller takes the no-jailbreak
//      fallback unchanged. [2.02: 2 x 10 s when Lapy worked at the last start,
//      else one attempt of 10 s (nothing known) or 2.5 s (a probe).]
//   7. At most one request per process: after elevation the process's root is
//      the system root and a second request would be rejected and consumed, so
//      the whole request+wait sequence runs exactly once.
//
// orbis_probe_jit() and orbis_log_hen_config() are unchanged (main-boot still
// calls them). Each step is logged through orbis_boot_early_log so a session
// log shows the full sequence even on a console whose boot log is held until
// after elevation (main-boot defers the boot-log pump thread to keep this
// request single-threaded).
//
// 2.02 (AI-assisted; users: "the black screen before the shelf is extra long"
// since 2.01): live-15 made the Lapy daemon the ONLY elevation, so every console
// without it (etaHEN, OnionHEN, the PS5SX2 Helper: every 2.00 log has their
// /download0/etahen_jailbreak taken in 1-2 polls, ~30 ms) sat 4 x 10 s on a
// black screen and then ran unelevated. orbis_elevate() below tries both
// protocols, one after the other on the main thread (neither starts a thread,
// so the Lapy request stays single-threaded), the one that worked at the last
// start first with its long wait, the other with a short probe. Nothing is
// requested when the process is already root. main-boot.cpp keeps the method
// that worked in cache/elevation.txt and logs how long elevation took.
#include "ProsperoHenJailbreak.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// main-boot.cpp: prints the line (into boot.log on a normal boot) and, while the
// boot log is deferred for single-threaded elevation, also keeps it in memory to
// flush once the log is live. No thread. Pass a line with no trailing newline.
extern "C" void orbis_boot_early_log(const char* line);

namespace {

constexpr char kRequestPath[] = "/download0/elevate_proc";
constexpr char kResultPath[] = "/download0/lapy_owned_result";
constexpr int kPollUs = 50 * 1000;      // 50 ms
// 2.02: the attempts and the wait per attempt come from orbis_elevate (were 4 x 10 s).

// 2.02: the etaHEN/OnionHEN/PS5SX2 Helper request (the 2.00 broker's files).
constexpr char kEtaHenRequestPath[] = "/download0/etahen_jailbreak";
constexpr char kEtaHenStagedPath[] = "/download0/etahen_jailbreak.tmp";
constexpr int kEtaHenPollUs = 16667;    // the 2.00 broker's cadence

double mono_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
}

void lapy_log(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    orbis_boot_early_log(buf);
}

// Step 2: write the complete request marker. Returns true on a fully written,
// closed marker; false (and the error in *err) otherwise.
bool write_request(int pid, int* err) {
    unlink(kRequestPath);
    const int fd = open(kRequestPath, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        *err = errno;
        return false;
    }
    char body[32];
    const int n = std::snprintf(body, sizeof(body), "{\"PID\":%d}\n", pid);
    if (n <= 0 || n >= static_cast<int>(sizeof(body))) {
        *err = EINVAL;
        close(fd);
        unlink(kRequestPath);
        return false;
    }
    size_t written = 0;
    while (written < static_cast<size_t>(n)) {
        const ssize_t w = write(fd, body + written, static_cast<size_t>(n) - written);
        if (w <= 0) {
            *err = errno;
            close(fd);
            unlink(kRequestPath);
            return false;
        }
        written += static_cast<size_t>(w);
    }
    if (close(fd) != 0) {
        *err = errno;
        unlink(kRequestPath);
        return false;
    }
    *err = 0;
    return true;
}

// Step 3: wait for the daemon to take (unlink) the marker. True on acknowledgement.
// 2.02: for up to wait_ms (was a fixed 10 s).
bool wait_for_ack(int wait_ms) {
    const int polls = wait_ms / (kPollUs / 1000) + 1;
    for (int i = 0; i < polls; ++i) {
        if (access(kRequestPath, F_OK) == -1 && errno == ENOENT)
            return true;
        usleep(kPollUs);
    }
    return false;
}

// Step 4: a real /data round trip. data_ok is 1 only when the bytes read back
// match. open_errno is the create open()'s errno (0 on success).
int verify_data(int* open_errno) {
    const char* path = "/data/.ps5sx2_lapy_rw";
    static const char marker[] = "lapy-owned";
    *open_errno = 0;
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        *open_errno = errno;
        return 0;
    }
    bool ok = write(fd, marker, sizeof(marker)) == static_cast<ssize_t>(sizeof(marker));
    if (close(fd) != 0)
        ok = false;
    if (ok) {
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            ok = false;
        } else {
            char back[sizeof(marker)] = {0};
            ok = read(fd, back, sizeof(back)) == static_cast<ssize_t>(sizeof(marker)) &&
                 std::memcmp(back, marker, sizeof(marker)) == 0;
            close(fd);
        }
    }
    unlink(path);
    return ok ? 1 : 0;
}

// Step 5: the result file, always written after verification.
void write_result(int data_ok, int open_errno) {
    const int fd = open(kResultPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        lapy_log("[lapy] result: cannot write %s (errno %d)", kResultPath, errno);
        return;
    }
    char line[64];
    const int n = std::snprintf(line, sizeof(line), "DATA_OK=%d OPEN_ERRNO=%d\n", data_ok, open_errno);
    if (n > 0)
        (void)!write(fd, line, static_cast<size_t>(n));
    close(fd);
}

// 2.02: the etaHEN-style request, as the 2.00 broker sent it (staged file, 0666,
// fsync, rename into place). True when the process is root afterwards. Taken =
// access() fails with ENOENT only (an EPERM before elevation is not "taken": the
// 2.00 broker read it as taken and then waited its whole 7.5 s grace).
bool etahen_request(int consume_ms, int grace_ms) {
    const int pid = static_cast<int>(getpid());
    char body[32];
    const int n = std::snprintf(body, sizeof(body), "{\"PID\":%d}\n", pid);
    if (n <= 0 || n >= static_cast<int>(sizeof(body)))
        return false;
    unlink(kEtaHenRequestPath);
    unlink(kEtaHenStagedPath);
    const int fd = open(kEtaHenStagedPath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) {
        lapy_log("[elevate] etaHEN request: cannot create %s (errno %d)", kEtaHenStagedPath, errno);
        return false;
    }
    bool ok = fchmod(fd, 0666) == 0;
    size_t written = 0;
    while (ok && written < static_cast<size_t>(n)) {
        const ssize_t w = write(fd, body + written, static_cast<size_t>(n) - written);
        if (w <= 0)
            ok = false;
        else
            written += static_cast<size_t>(w);
    }
    if (ok && fsync(fd) != 0)
        ok = false;
    if (close(fd) != 0)
        ok = false;
    if (!ok || rename(kEtaHenStagedPath, kEtaHenRequestPath) != 0) {
        lapy_log("[elevate] etaHEN request: write/publish failed (errno %d)", errno);
        unlink(kEtaHenStagedPath);
        return false;
    }
    const double t0 = mono_ms();
    double taken_at = -1.0;
    for (;;) {
        usleep(kEtaHenPollUs);
        const double now = mono_ms();
        if (geteuid() == 0) {
            lapy_log("[elevate] etaHEN request: root %.0f ms after the request (%s)", now - t0,
                     taken_at >= 0 ? "taken, then root" : "root before the file was seen gone");
            return true;
        }
        if (taken_at < 0) {
            if (access(kEtaHenRequestPath, F_OK) == -1 && errno == ENOENT) {
                taken_at = now;
                lapy_log("[elevate] etaHEN request: taken after %.0f ms; waiting up to %d ms for root", now - t0, grace_ms);
            } else if (now - t0 >= consume_ms) {
                unlink(kEtaHenRequestPath);
                lapy_log("[elevate] etaHEN request: not taken within %d ms (no etaHEN/OnionHEN/Helper answering)", consume_ms);
                return false;
            }
        } else if (now - taken_at >= grace_ms) {
            lapy_log("[elevate] etaHEN request: taken, but still uid %d %d ms later", static_cast<int>(geteuid()), grace_ms);
            return false;
        }
    }
}

// The Lapy request (live-15's steps 1-6), with the attempts and the wait per
// attempt given. True only when the /data round trip after acknowledgement
// succeeds.
bool lapy_request(int attempts, int wait_ms) {
    const int pid = static_cast<int>(getpid());

    // Step 1: clone the credential. seteuid(geteuid()) gives a private ucred;
    // the daemon requires it. On failure, do not write a request.
    const int euid = static_cast<int>(geteuid());
    if (seteuid(euid) != 0) {
        lapy_log("[lapy] seteuid(%d) failed (errno %d): not requesting elevation", euid, errno);
        return false;
    }
    lapy_log("[lapy] credential clone ok (euid %d, pid %d); requesting via %s (%d x %d ms)", euid, pid, kRequestPath,
             attempts, wait_ms);

    const double t0 = mono_ms();
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        int err = 0;
        if (!write_request(pid, &err)) {
            lapy_log("[lapy] request write failed (errno %d)", err);
            return false; // a marker we cannot write will not write on a retry either
        }
        lapy_log("[lapy] request written (attempt %d of %d): {\"PID\":%d}", attempt, attempts, pid);

        if (wait_for_ack(wait_ms)) {
            // Acknowledged: the daemon took the marker. Do not request again even
            // if /data turns out unusable (step 7) -- a second marker would be
            // consumed and lost.
            const double acked = mono_ms() - t0;
            int open_errno = 0;
            const int data_ok = verify_data(&open_errno);
            write_result(data_ok, open_errno);
            lapy_log("[lapy] acknowledged on attempt %d after %.0f ms; data_rw ok=%d (open errno %d); result written to %s",
                     attempt, acked, data_ok, open_errno, kResultPath);
            if (data_ok != 1)
                lapy_log("[lapy] acknowledged but /data round trip failed: not treating as elevated");
            return data_ok == 1;
        }

        unlink(kRequestPath); // drop the stale marker before the next attempt (or for good)
        lapy_log("[lapy] no acknowledgement within %d ms on attempt %d%s", wait_ms, attempt,
                 attempt < attempts ? " (marker may predate the daemon; rewriting)" : "");
    }
    return false;
}

} // namespace

// 2.02: see the top of this file. hint is what worked at the last start
// ("lapy", "etahen", "ports", "none"; "" when not known, e.g. /data not
// readable yet). try_ports is main-boot's 9028/9069 CMD-port jailbreak. Returns
// "already", "lapy", "etahen", "ports", or "none". Runs once per process.
const char* orbis_elevate(const char* hint, bool (*try_ports)()) {
    static const char* s_method = nullptr;
    if (s_method) {
        lapy_log("[elevate] elevation already requested this process: not requesting again");
        return s_method;
    }
    const std::string h = hint ? hint : "";
    if (geteuid() == 0) {
        lapy_log("[elevate] already root (euid 0): nothing requested");
        return s_method = "already";
    }
    const bool known_lapy = h == "lapy", known_etahen = h == "etahen";
    // The method that worked last time gets its long wait; the other a short probe. With nothing known (a first start, or
    // a console whose /data shows only after elevation), etaHEN's probe first (every 2.00 log: taken in ~30 ms), then one
    // full Lapy attempt. "ports"/"none" last time: short probes of both.
    const int eta_consume_ms = known_etahen ? 5000 : 1500;
    const int lapy_attempts = known_lapy ? 2 : 1;
    const int lapy_wait_ms = (known_lapy || h.empty()) ? 10000 : 2500;
    lapy_log("[elevate] last start: %s; order: %s", h.empty() ? "not known" : h.c_str(),
             known_lapy ? "Lapy, etaHEN, ports" : h == "ports" ? "ports, etaHEN, Lapy" : "etaHEN, Lapy, ports");

    if (h == "ports" && try_ports && try_ports())
        return s_method = "ports";
    if (known_lapy) {
        if (lapy_request(lapy_attempts, lapy_wait_ms))
            return s_method = "lapy";
        if (geteuid() == 0 || etahen_request(eta_consume_ms, 5000))
            return s_method = "etahen";
    } else {
        if (etahen_request(eta_consume_ms, 5000))
            return s_method = "etahen";
        if (geteuid() == 0) // the etaHEN request was taken just after its window
            return s_method = "etahen";
        if (lapy_request(lapy_attempts, lapy_wait_ms))
            return s_method = "lapy";
    }
    if (h != "ports" && try_ports && try_ports())
        return s_method = "ports";
    return s_method = "none";
}

// Privilege probe: root is the only thing that makes JIT shared memory work
// on this firmware (unprivileged create fails). One page, left mapped.
extern "C" int sceKernelJitCreateSharedMemory(int flags, unsigned long long size, int protection, int* destinationHandle);
extern "C" int sceKernelJitMapSharedMemory(int handle, int protection, void** destination);

bool orbis_probe_jit()
{
    int handle = -1;
    const int rc = sceKernelJitCreateSharedMemory(0, 0x4000, 7, &handle);
    printf("[jitprobe] create rc=%d handle=%d\n", rc, handle);
    fflush(stdout);
    if (rc != 0)
        return false;
    void* addr = nullptr;
    const int rm = sceKernelJitMapSharedMemory(handle, 7, &addr);
    printf("[jitprobe] map rc=%d addr=%p\n", rm, addr);
    fflush(stdout);
    return rm == 0 && addr != nullptr;
}

// Best-effort self-check: can this process even see a HEN config, and does it
// list our TitleID? Sandbox may deny the open; every outcome is logged. Kept
// from the etaHEN era as a diagnostic; harmless on a Lapy console (the files are
// absent, which it reports).
void orbis_log_hen_config()
{
    static const char* const paths[] = {
        "/data/etaHEN/config.ini",
        "/data/OnionHEN/config.ini",
    };
    for (const char* path : paths) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            printf("[hen] config %s: unreadable errno=%d\n", path, errno);
            fflush(stdout);
            continue;
        }
        char buf[2048];
        ssize_t total = 0;
        while (total < (ssize_t)sizeof(buf) - 1) {
            const ssize_t r = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total);
            if (r <= 0)
                break;
            total += r;
        }
        close(fd);
        buf[total < 0 ? 0 : total] = 0;
        bool has_id = false, has_enabled = false;
        for (ssize_t i = 0; i + 9 <= total; ++i) {
            if (!__builtin_memcmp(buf + i, "PPSA99203", 9))
                has_id = true;
            if (!__builtin_memcmp(buf + i, "enabled", 7))
                has_enabled = true;
        }
        printf("[hen] config %s: bytes=%d has_id=%d has_enabled=%d\n", path, (int)total,
            (int)has_id, (int)has_enabled);
        fflush(stdout);
    }
}
