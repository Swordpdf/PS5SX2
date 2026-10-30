#!/bin/bash
# Host test of the relaunch PAYLOAD (relaunch.c) with stand-ins for the console's calls (relaunch_stub.c): builds it for the PC, writes a request over
# its marker the way the app does, runs it next to a child process that plays the closing app, and checks the files it writes and the launches it
# makes (the stub logs each sceLncUtilLaunchApp). 2.11 (AI-assisted). Cases run side by side; the whole test takes about 20 s.
#   CC=clang-18 ps5/coreorbis/relaunch/test-relaunch-payload.sh
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
${CC:-clang} -std=gnu11 -O1 -g -Wall -Wextra -Werror -DRELAUNCH_HOST_TEST -o "$work/relaunch" "$here/relaunch.c" "$here/relaunch_stub.c"
python3 -I - "$work" <<'PY'
import os, subprocess, sys, time, threading
from concurrent.futures import ThreadPoolExecutor

work = sys.argv[1]
exe = os.path.join(work, "relaunch")
blob = open(exe, "rb").read()
marker = b"PS5SX2-RELAUNCH-REQUEST-V1"
ROOM = 1024
at = -1
i = 0
while True:
    i = blob.find(marker, i)
    if i < 0:
        break
    if blob[i + len(marker):i + ROOM] == b"\0" * (ROOM - len(marker)):
        at = i
        break
    i += 1
assert at >= 0, "g_request not found in the payload"

failures = []
lock = threading.Lock()


def check(case, cond, what, extra=""):
    if not cond:
        with lock:
            failures.append("%s: %s %s" % (case, what, extra))


class Run:
    def __init__(self, name, app_seconds=1.5, wait="PPSA99230", launch="PPSA99203", settle_ms=400, limit_s=20, nosig_ms=1200,
                 env=None, appfile=True, bigfile=None, raw=None, stdout=None, reaped=False, pid=None, state_dir=None, forget_at="app", extra=""):
        self.name = name
        self.dir = os.path.join(work, name)
        os.makedirs(self.dir, exist_ok=True)
        self.state = os.path.join(state_dir or self.dir, "relaunch-100")
        self.log = os.path.join(self.dir, "stub.log")
        self.appfile = os.path.join(self.dir, "app.exists")
        self.bigfile = os.path.join(self.dir, "bigapp.txt")
        if appfile:
            open(self.appfile, "w").write("1\n")
        if bigfile is not None:
            open(self.bigfile, "w").write(bigfile + "\n")
        if reaped:
            a = subprocess.Popen(["sleep", "0.1"])
            a.wait()
            self.app = a
        else:
            self.app = subprocess.Popen(["sleep", str(app_seconds)])
        self.app_start = time.time()
        self.app_seconds = app_seconds
        self.settle = settle_ms / 1000.0
        self.nosig = nosig_ms / 1000.0
        req = raw
        if req is None:
            req = ("PS5SX2-RELAUNCH-REQUEST-V1\npid=%d\nwait=%s\nlaunch=%s\nstate=%s\nsettle_ms=%d\nlimit_s=%d\nnosig_ms=%d\n%s"
                   % (self.app.pid if pid is None else pid, wait, launch, self.state, settle_ms, limit_s, nosig_ms, extra)).encode("utf-8")
        patched = bytearray(blob)
        patched[at:at + len(req)] = req
        patched[at + len(req)] = 0
        self.elf = os.path.join(self.dir, "payload.elf")
        open(self.elf, "wb").write(patched)
        os.chmod(self.elf, 0o755)
        e = dict(os.environ)
        e["RELAUNCH_STUB_LOG"] = self.log
        e["RELAUNCH_STUB_OLD"] = wait
        e["RELAUNCH_STUB_LAUNCH"] = launch
        e["RELAUNCH_STUB_APPFILE"] = self.appfile
        if bigfile is not None:
            e["RELAUNCH_STUB_BIGFILE"] = self.bigfile
        e.update(env or {})
        self.t0 = time.time()
        self.p = subprocess.Popen([self.elf], env=e, stdout=stdout, stderr=stdout)
        # the shell forgets the closing title as the process ends (a test that wants it otherwise says forget_at=None and does it itself)
        if appfile and forget_at == "app":
            def forget(path=self.appfile, when=self.app_start + app_seconds):
                time.sleep(max(0.0, when - time.time()))
                if os.path.exists(path):
                    os.unlink(path)
            threading.Thread(target=forget, daemon=True).start()

    def sleep_until(self, seconds_after_start):
        d = self.t0 + seconds_after_start - time.time()
        if d > 0:
            time.sleep(d)

    def finish(self, timeout=40):
        try:
            self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
            check(self.name, False, "payload did not end in %d s" % timeout)
        self.t_end = time.time()
        try:
            self.app.kill()
        except Exception:
            pass
        try:
            self.app.wait(timeout=2)
        except Exception:
            pass
        return self.p.returncode

    def launches(self):
        out = []
        if os.path.exists(self.log):
            for line in open(self.log):
                if line.startswith("launch "):
                    f = line.split()
                    out.append({"title": f[1], "user": int(f[3]), "size": int(f[5]), "at": int(f[7]) / 1000.0})
        return out

    def read(self, suffix):
        p = self.state + suffix
        return open(p, "r").read() if os.path.exists(p) else None

    def status(self):
        d = self.read(".done")
        return d.split("\n", 1)[0][len("status "):] if d else None

    def relaunch_log(self):
        p = os.path.join(os.path.dirname(self.state), "relaunch.log")
        return open(p).read() if os.path.exists(p) else ""

    def app_end(self):
        return self.app_start + self.app_seconds


def timing_after(run, base, extra, case, what, slack=1.8):
    ls = run.launches()
    if not ls:
        check(case, False, what + ": no launch")
        return
    lo = base + extra - 0.08
    check(case, ls[0]["at"] >= lo, what + ": launched too early", "(%.2f s before the earliest allowed moment)" % (lo - ls[0]["at"]))
    check(case, ls[0]["at"] <= base + extra + slack, what + ": launched late", "(%.2f s after it)" % (ls[0]["at"] - (base + extra)))


def case_happy():
    r = Run("happy", app_seconds=1.5)
    r.sleep_until(0.5)
    check("happy", r.read(".started") is not None, ".started appears at once")
    check("happy", r.read(".done") is None and not r.launches(), "nothing done while the app still runs")
    rc = r.finish()
    check("happy", rc == 0, "exit code", str(rc))
    ls = r.launches()
    check("happy", len(ls) == 1, "exactly one launch", str(ls))
    if ls:
        check("happy", ls[0]["title"] == "PPSA99203", "the launched title", ls[0]["title"])
        check("happy", ls[0]["user"] == 123, "the foreground user", str(ls[0]["user"]))
        check("happy", ls[0]["size"] == 32, "lnc_app_param_t size is the helper's 32", str(ls[0]["size"]))
    timing_after(r, r.app_end(), r.settle, "happy", "one launch after the app plus the settle")
    check("happy", r.status() == "launched", "done says launched", str(r.read(".done")))
    log = r.relaunch_log()
    check("happy", "the app is gone" in log and "settled" in log and "request: pid" in log, "relaunch.log has the steps", log[-300:])
    check("happy", r.read(".tmp") is None, "no .tmp left")


def case_title_lingers():
    # the process is gone at 1.0 s but the shell still knows the title until 2.6 s: nothing before that
    r = Run("lingers", app_seconds=1.0, forget_at=None)
    r.sleep_until(2.6)
    os.unlink(r.appfile)
    r.finish()
    timing_after(r, r.t0 + 2.6, r.settle, "lingers", "launch waits for the shell's word, then the settle")
    check("lingers", len(r.launches()) == 1, "one launch")


def case_title_lingers_forever():
    # the shell goes on listing the title for ever although the process is gone at 1.0 s: after cap_ms (2 s here, 8 s in a real request) it launches anyway
    r = Run("lingers_forever", app_seconds=1.0, forget_at=None, extra="cap_ms=2000\n")
    r.finish()
    timing_after(r, r.app_end(), 2.0 + r.settle, "lingers_forever", "launch after the cap, not before")
    check("lingers_forever", "still lists the closing app" in r.relaunch_log(), "the log says why", r.relaunch_log()[-300:])


def case_title_never_known():
    # the shell never knew the title (no file): the process decides
    r = Run("unknown_title", app_seconds=2.0, appfile=False)
    r.finish()
    timing_after(r, r.app_end(), r.settle, "unknown_title", "the process alone decides")


def case_cancel_before():
    r = Run("cancel_before", app_seconds=6.0, forget_at=None)
    r.sleep_until(0.6)
    open(r.state + ".cancel", "w").write("1\n")
    r.finish()
    check("cancel_before", r.t_end - r.t0 < 2.0, "ends soon after the cancel", "%.2f s" % (r.t_end - r.t0))
    check("cancel_before", not r.launches(), "no launch")
    check("cancel_before", r.status() == "cancelled", "done says cancelled", str(r.read(".done")))


def case_cancel_settle():
    r = Run("cancel_settle", app_seconds=0.8, settle_ms=2500)
    r.sleep_until(1.5)  # in the settle (0.9 .. 3.4)
    open(r.state + ".cancel", "w").write("1\n")
    r.finish()
    check("cancel_settle", not r.launches(), "no launch after a cancel during the settle")
    check("cancel_settle", r.status() == "cancelled", "done says cancelled", str(r.read(".done")))
    check("cancel_settle", r.t_end - r.t0 < 2.4, "ends at the cancel, not at the end of the settle", "%.2f s" % (r.t_end - r.t0))


def case_cancel_before_start():
    # the cancel note is already there when the payload starts (the app gave up before it ran): the payload launches nothing
    d = os.path.join(work, "cancel_first")
    os.makedirs(d)
    open(os.path.join(d, "relaunch-100.cancel"), "w").write("1\n")
    r = Run("cancel_first", app_seconds=0.3, state_dir=d)
    r.finish()
    check("cancel_first", not r.launches(), "no launch")
    check("cancel_first", r.status() == "cancelled", "done says cancelled", str(r.read(".done")))


def case_timeout():
    r = Run("timeout", app_seconds=60, limit_s=5, forget_at=None)
    rc = r.finish()
    check("timeout", rc == 0, "exit code")
    check("timeout", not r.launches(), "never launches on top of a live app")
    check("timeout", r.status() == "timeout", "done says timeout", str(r.read(".done")))
    check("timeout", 4.5 <= r.t_end - r.t0 <= 8.0, "ends after the limit (5 s)", "%.2f s" % (r.t_end - r.t0))


def case_rc_sequence():
    r = Run("rc_seq", app_seconds=0.5, env={"RELAUNCH_STUB_RC": "-3,-3,0"})
    r.finish()
    check("rc_seq", len(r.launches()) == 3, "three attempts", str(r.launches()))
    check("rc_seq", r.status() == "launched", "done says launched", str(r.read(".done")))
    ls = r.launches()
    if len(ls) == 3:
        check("rc_seq", 0.7 <= ls[1]["at"] - ls[0]["at"] <= 1.6 and 0.7 <= ls[2]["at"] - ls[1]["at"] <= 1.6, "attempts about 800 ms apart")


def case_already_running():
    r = Run("already", app_seconds=0.5, env={"RELAUNCH_STUB_RC": "0x8094000c"})
    r.finish()
    check("already", len(r.launches()) == 1, "0x8094000c is a success: one attempt", str(r.launches()))
    check("already", r.status() == "launched", "done says launched", str(r.read(".done")))


def case_all_fail():
    r = Run("allfail", app_seconds=0.3, env={"RELAUNCH_STUB_RC": "-3"})
    r.finish()
    check("allfail", len(r.launches()) == 10, "two rounds of five", str(len(r.launches())))
    check("allfail", r.status() == "failed", "done says failed", str(r.read(".done")))


def case_quiet():
    r = Run("quiet", app_seconds=0.5, env={"RELAUNCH_STUB_QUIET": "1"})
    r.finish()
    check("quiet", not r.launches(), "no launch while the console shuts down")
    check("quiet", r.status() == "shutdown", "done says shutdown", str(r.read(".done")))


def case_evf_unavailable():
    r = Run("evf", app_seconds=0.5, env={"RELAUNCH_STUB_QUIET": "1", "RELAUNCH_STUB_EVF": "fail"})
    r.finish()
    check("evf", len(r.launches()) == 1, "fail-open: the flags can't be read, so it launches", str(r.launches()))


def case_reaped():
    r = Run("reaped", reaped=True, appfile=False)
    r.finish()
    check("reaped", len(r.launches()) == 1, "a pid that does not exist is gone", str(r.launches()))
    check("reaped", r.t_end - r.t0 < 3.5, "without waiting for the limit", "%.2f s" % (r.t_end - r.t0))


def case_empty_wait_no_signal():
    r = Run("nosignal", app_seconds=1.0, wait="", settle_ms=200, nosig_ms=1500)
    r.finish()
    timing_after(r, r.app_end(), 1.5, "nosignal", "an unknown title and no big app: the fixed settle")
    check("nosignal", "nothing else tells" in r.relaunch_log(), "the log says why the settle is long", r.relaunch_log()[-300:])


def case_empty_wait_big_app():
    r = Run("bigapp", app_seconds=1.0, wait="", settle_ms=200, bigfile="0x8017", appfile=False)
    r.sleep_until(2.4)
    os.unlink(r.bigfile)  # the shell has no big app any more
    r.finish()
    timing_after(r, r.t0 + 2.4, 0.2, "bigapp", "no title: the big app's id decides, then the short settle")


def case_getappid_null():
    r = Run("noappid", app_seconds=1.0, env={"RELAUNCH_STUB_NULL": "sceLncUtilGetAppId"}, settle_ms=200, nosig_ms=1500)
    r.finish()
    timing_after(r, r.app_end(), 1.5, "noappid", "no sceLncUtilGetAppId and no big app: process gone + fixed settle")
    check("noappid", r.status() == "launched-unchecked", "cannot verify the start", str(r.read(".done")))


def case_getappid_null_bigapp():
    r = Run("noappid_big", app_seconds=1.0, env={"RELAUNCH_STUB_NULL": "sceLncUtilGetAppId"}, bigfile="0x8017", settle_ms=200)
    r.sleep_until(2.2)
    os.unlink(r.bigfile)
    r.finish()
    timing_after(r, r.t0 + 2.2, 0.2, "noappid_big", "no sceLncUtilGetAppId: the big app's id decides")


def case_launch_null():
    r = Run("nolaunch", app_seconds=0.5, env={"RELAUNCH_STUB_NULL": "sceLncUtilLaunchApp"})
    rc = r.finish()
    check("nolaunch", rc == 0, "exit code")
    check("nolaunch", not r.launches(), "nothing to launch with")
    check("nolaunch", r.read(".started") is None, "no handshake from a payload that cannot launch (the app then keeps its own way out)")
    check("nolaunch", r.status() == "nolaunch", "done says nolaunch", str(r.read(".done")))
    check("nolaunch", "sceLncUtilLaunchApp was not found" in (r.read(".done") or ""), "and says why")


def case_nolibs():
    r = Run("nolibs", app_seconds=0.5, env={"RELAUNCH_STUB_NOLIBS": "1"})
    r.finish()
    check("nolibs", not r.launches() and r.status() == "nolaunch", "no library, no launch", str(r.read(".done")))
    check("nolibs", r.read(".started") is None, "and no handshake")


def case_no_request():
    d = os.path.join(work, "norequest")
    os.makedirs(d)
    path = os.path.join(d, "empty.elf")
    open(path, "wb").write(blob)  # the marker and zeros: no request
    os.chmod(path, 0o755)
    r = subprocess.run([path], timeout=30, cwd=d)
    check("no_request", r.returncode == 1, "no request: exits 1", str(r.returncode))
    check("no_request", os.listdir(d) == ["empty.elf"], "writes nothing", str(os.listdir(d)))


def case_bad_requests():
    for tag, req in (
        ("garbage", b"PS5SX2-RELAUNCH-REQUEST-V1\n\xff\xfe junk\n"),
        ("no pid", b"PS5SX2-RELAUNCH-REQUEST-V1\nwait=A\nlaunch=PPSA99203\nstate=/tmp/x/relaunch-1\n"),
        ("pid 0", b"PS5SX2-RELAUNCH-REQUEST-V1\npid=0\nwait=A\nlaunch=PPSA99203\nstate=/tmp/x/relaunch-1\n"),
        ("no launch", b"PS5SX2-RELAUNCH-REQUEST-V1\npid=5\nwait=A\nstate=/tmp/x/relaunch-1\n"),
        ("no state", b"PS5SX2-RELAUNCH-REQUEST-V1\npid=5\nwait=A\nlaunch=PPSA99203\n"),
        ("relative state", b"PS5SX2-RELAUNCH-REQUEST-V1\npid=5\nwait=A\nlaunch=PPSA99203\nstate=relaunch-1\n"),
        ("no newline after the marker", b"PS5SX2-RELAUNCH-REQUEST-V1 pid=5\n"),
    ):
        d = os.path.join(work, "bad_" + tag.replace(" ", "_"))
        os.makedirs(d)
        patched = bytearray(blob)
        patched[at:at + len(req)] = req
        patched[at + len(req)] = 0
        path = os.path.join(d, "bad.elf")
        open(path, "wb").write(patched)
        os.chmod(path, 0o755)
        r = subprocess.run([path], timeout=30, cwd=d)
        check("bad_requests", r.returncode == 1, tag + ": exits 1", str(r.returncode))
        check("bad_requests", os.listdir(d) == ["bad.elf"], tag + ": writes nothing", str(os.listdir(d)))


def case_closed_stdout():
    rfd, wfd = os.pipe()
    os.close(rfd)  # nobody reads the loader's connection any more
    r = Run("sigpipe", app_seconds=0.5, stdout=wfd)
    os.close(wfd)
    rc = r.finish()
    check("sigpipe", rc == 0, "exit code with stdout and stderr closed", str(rc))
    check("sigpipe", len(r.launches()) == 1 and r.status() == "launched", "still launches", str(r.read(".done")))


def case_verify_second_round():
    # the title is only up after the SECOND launch: the first round's check (8 s) fails, a second launch follows
    r = Run("verify", app_seconds=0.3, env={"RELAUNCH_STUB_UPAFTER": "2"})
    r.finish()
    ls = r.launches()
    check("verify", len(ls) == 2, "a second round", str(ls))
    if len(ls) == 2:
        check("verify", 7.5 <= ls[1]["at"] - ls[0]["at"] <= 11.0, "after the 8 s check", "%.2f" % (ls[1]["at"] - ls[0]["at"]))
    check("verify", r.status() == "launched", "done says launched", str(r.read(".done")))


def case_initial_user():
    r = Run("user", app_seconds=0.3, env={"RELAUNCH_STUB_FGFAIL": "1"})
    r.finish()
    ls = r.launches()
    check("user", len(ls) == 1 and ls[0]["user"] == 456, "no foreground user: the initial user", str(ls))


def case_pid_unknown_with_signal():
    # the process can't be looked at: the shell's word (the title gone at 1.2 s) decides
    r = Run("pidunknown", app_seconds=30, env={"RELAUNCH_STUB_PIDSTATE": "unknown"}, forget_at=None)
    r.sleep_until(1.2)
    os.unlink(r.appfile)
    r.finish()
    timing_after(r, r.t0 + 1.2, r.settle, "pidunknown", "the title alone decides when the process can't be seen")


def case_pid_unknown_no_signal():
    r = Run("pidunknown2", app_seconds=30, limit_s=5, forget_at=None, env={"RELAUNCH_STUB_PIDSTATE": "unknown", "RELAUNCH_STUB_NULL": "sceLncUtilGetAppId"})
    r.finish()
    check("pidunknown2", not r.launches(), "nothing at all to go by: no launch")
    check("pidunknown2", r.status() == "timeout", "done says timeout", str(r.read(".done")))


def case_log_trimmed():
    d = os.path.join(work, "logtrim")
    os.makedirs(d)
    open(os.path.join(d, "relaunch.log"), "w").write("x" * 70000)
    r = Run("logtrim", app_seconds=0.3, state_dir=d)
    r.finish()
    size = os.path.getsize(os.path.join(d, "relaunch.log"))
    check("logtrim", size < 20000, "an over-long relaunch.log starts again", str(size))


# ---- 176: the launch is anchored to the PROCESS exit (min_exit_ms), the pause between refused launches is the request's (retry_ms) ----
def issued_ms(run):
    """the 'issued (N ms after the process exit)' numbers of the relaunch.log, in order"""
    import re
    return [int(m) for m in re.findall(r"issued \((\d+) ms after the process exit\)", run.relaunch_log())]


def case_fast_teardown_anchor():
    # a shell that forgets the title the moment the process ends (forget_at="app"): the short settle alone would launch 0.2 s after the exit, which is
    # inside ShadowMount+'s first second; with min_exit_ms 1600 the launch waits for 1.6 s after the exit. Fails with a payload that ignores the field.
    r = Run("fast_anchor", app_seconds=1.0, settle_ms=200, extra="min_exit_ms=1600\n")
    r.finish()
    timing_after(r, r.app_end(), 1.6, "fast_anchor", "not before 1.6 s after the process exit", slack=0.6)
    check("fast_anchor", len(r.launches()) == 1 and r.status() == "launched", "one launch, done says launched", str(r.read(".done")))
    log = r.relaunch_log()
    check("fast_anchor", "ms after the process exit: " in log and "ms more" in log, "the log says it waited for the anchor", log[-400:])
    nums = issued_ms(r)
    check("fast_anchor", len(nums) == 1 and 1550 <= nums[0] <= 2300, "the log has the launch's ms after the process exit", str(nums))
    check("fast_anchor", "the process %d is gone" % r.app.pid in log, "the log says when the process was seen gone", log[-400:])


def case_slow_teardown_no_extra_wait():
    # the shell forgets the title 2.0 s after the process ended (a slow console): the settle runs from there and the anchor (1.6 s after the exit) has
    # long passed, so it adds nothing: launch at forget + settle
    r = Run("slow_anchor", app_seconds=1.0, settle_ms=700, forget_at=None, extra="min_exit_ms=1600\n")
    r.sleep_until(3.0)
    os.unlink(r.appfile)
    r.finish()
    timing_after(r, r.t0 + 3.0, 0.7, "slow_anchor", "settle after the shell's word, the anchor is long over", slack=0.6)
    log = r.relaunch_log()
    check("slow_anchor", "ms more" not in log, "the anchor did not wait", log[-400:])
    nums = issued_ms(r)
    check("slow_anchor", len(nums) == 1 and 2600 <= nums[0] <= 3300, "the launch was 2.7 s after the process exit", str(nums))


def case_no_min_exit_field():
    # a request without min_exit_ms (or with a field that is no number, or negative) has no such rule: the old timing, launch at exit + settle
    for tag, extra in (("absent", ""), ("garbage", "min_exit_ms=abc\n"), ("negative", "min_exit_ms=-5\n")):
        r = Run("no_min_exit_" + tag, app_seconds=1.0, settle_ms=300, extra=extra)
        r.finish()
        timing_after(r, r.app_end(), 0.3, "no_min_exit_" + tag, "old timing: exit + settle", slack=0.8)
        check("no_min_exit_" + tag, "ms more" not in r.relaunch_log(), "no anchor wait")


def case_retry_ms():
    # three attempts (rc -3, -3, 0) 200 ms apart, not 800 ms (the old pause): the whole run is over before the old pause would have been used twice
    r = Run("retry_200", app_seconds=0.3, settle_ms=100, env={"RELAUNCH_STUB_RC": "-3,-3,0"}, extra="retry_ms=200\n")
    r.finish()
    ls = r.launches()
    check("retry_200", len(ls) == 3, "three attempts", str(ls))
    if len(ls) == 3:
        for a, b in ((0, 1), (1, 2)):
            gap = ls[b]["at"] - ls[a]["at"]
            check("retry_200", 0.15 <= gap <= 0.65, "attempts about 200 ms apart", "%.2f s" % gap)
    check("retry_200", r.status() == "launched", "done says launched", str(r.read(".done")))
    check("retry_200", "retry every 200 ms" in r.relaunch_log(), "the request line says so", r.relaunch_log()[:400])


def case_retry_ms_clamped():
    # below 100 ms it is 100, above 2000 ms it is 2000 (the payload does not trust the request blindly)
    small = Run("retry_small", app_seconds=0.3, settle_ms=100, env={"RELAUNCH_STUB_RC": "-3,0"}, extra="retry_ms=5\n")
    big = Run("retry_big", app_seconds=0.3, settle_ms=100, env={"RELAUNCH_STUB_RC": "-3,0"}, extra="retry_ms=99999\n")
    small.finish()
    big.finish()
    a = small.launches()
    b = big.launches()
    check("retry_small", len(a) == 2 and 0.09 <= a[1]["at"] - a[0]["at"] <= 0.6, "5 ms became 100 ms", str(a))
    check("retry_big", len(b) == 2 and 1.9 <= b[1]["at"] - b[0]["at"] <= 2.7, "99999 ms became 2000 ms", str(b))


def case_retry_window():
    # 176 review: a shorter pause must not shorten the time a refusing shell is tried for. 175 tried 5 times at 800 ms (about 4 s a round, 8 s in two rounds);
    # at the shipped 400 ms the same 4 s a round is 10 tries, 20 in two rounds, the last one about 7.6 s after the first. With 5 tries a round (the first 176
    # build) it gave up after 2 s a round: 10 launches, the last 3.6 s after the first.
    r = Run("retry_window", app_seconds=0.3, settle_ms=100, env={"RELAUNCH_STUB_RC": "-3"}, extra="retry_ms=400\n")
    r.finish()
    ls = r.launches()
    check("retry_window", len(ls) == 20, "two rounds of ten at a 400 ms pause", str(len(ls)))
    if len(ls) >= 2:
        span = ls[-1]["at"] - ls[0]["at"]
        check("retry_window", 7.0 <= span <= 8.8, "the last try is about 7.6 s after the first (the 175 window was 8 s)", "%.2f s" % span)
    check("retry_window", r.status() == "failed", "done says failed", str(r.read(".done")))
    # (the 800 ms pause of 175 keeps its five tries a round: the allfail case above)
    check("retry_window", "retry every 400 ms" in r.relaunch_log(), "the request line says so", r.relaunch_log()[:400])


def case_retry_window_fast():
    # 100 ms is the smallest pause: 40 tries a round, 80 in two rounds, still about 4 s a round
    r = Run("retry_window_fast", app_seconds=0.3, settle_ms=100, env={"RELAUNCH_STUB_RC": "-3"}, extra="retry_ms=100\n")
    r.finish()
    ls = r.launches()
    check("retry_window_fast", len(ls) == 80, "two rounds of forty at a 100 ms pause", str(len(ls)))
    if len(ls) >= 2:
        span = ls[-1]["at"] - ls[0]["at"]
        check("retry_window_fast", 7.0 <= span <= 8.8, "the last try is about 7.9 s after the first", "%.2f s" % span)


def case_cancel_in_anchor_wait():
    # the app takes the request back while the payload waits for the anchor: nothing is launched, and the wait ends at the cancel
    r = Run("cancel_anchor", app_seconds=0.5, settle_ms=100, extra="min_exit_ms=3000\n")
    r.sleep_until(1.5)  # the process went at 0.5, the anchor holds until 3.5
    check("cancel_anchor", "ms more" in r.relaunch_log() and not r.launches(), "it is waiting for the anchor", r.relaunch_log()[-300:])
    open(r.state + ".cancel", "w").write("1\n")
    r.finish()
    check("cancel_anchor", not r.launches(), "no launch after a cancel during the anchor wait")
    check("cancel_anchor", r.status() == "cancelled", "done says cancelled", str(r.read(".done")))
    check("cancel_anchor", r.t_end - r.t0 < 2.4, "ends at the cancel, not at the end of the anchor", "%.2f s" % (r.t_end - r.t0))


def case_anchor_needs_the_process():
    # the process can't be looked at (only the shell's word counts): it was never seen gone, so there is nothing to anchor to and a large min_exit_ms
    # does not hold the launch back
    r = Run("anchor_unseen", app_seconds=30, settle_ms=300, env={"RELAUNCH_STUB_PIDSTATE": "unknown"}, forget_at=None, extra="min_exit_ms=5000\n")
    r.sleep_until(1.2)
    os.unlink(r.appfile)
    r.finish()
    timing_after(r, r.t0 + 1.2, 0.3, "anchor_unseen", "the title alone decides, no anchor", slack=0.8)
    check("anchor_unseen", "ms more" not in r.relaunch_log(), "no anchor wait")


# ---- fix180: GUARD mode (relaunch_guard.h). The main app leaves by its own exec; on 180 that exec twice brought nothing back (the player stayed on the home
# screen). The payload launches PPSA99203 only when no new process says it is up ("<state>.alive") guard_ms after the old one went. ----
GUARD = "guard_ms=2000\nhard_ms=5000\n"


def case_guard_came_back():
    # the exec works: the old process goes at 0.5 s and the new one says so at 1.5 s: nothing is launched, the payload ends at once
    r = Run("guard_back", app_seconds=0.5, wait="PPSA99203", settle_ms=100, extra=GUARD)
    r.sleep_until(1.5)
    check("guard_back", not r.launches(), "nothing launched while the guard waits", r.relaunch_log()[-300:])
    open(r.state + ".alive", "w").write("1\n")
    r.finish()
    check("guard_back", not r.launches(), "no launch: the exec brought the app back")
    check("guard_back", r.status() == "came-back", "done says came-back", str(r.read(".done")))
    check("guard_back", r.t_end - r.t0 < 2.3, "ends as soon as the new process says so", "%.2f s" % (r.t_end - r.t0))
    check("guard_back", "guard mode" in r.relaunch_log() and "guard: the process" in r.relaunch_log(), "the log has the guard's steps", r.relaunch_log()[-400:])


def case_guard_alive_before_gone():
    # the new process said so before the old one was seen gone (a fast exec): nothing launched
    d = os.path.join(work, "guard_early")
    os.makedirs(d)
    open(os.path.join(d, "relaunch-100.alive"), "w").write("1\n")
    r = Run("guard_early", app_seconds=3.0, wait="PPSA99203", settle_ms=100, state_dir=d, extra=GUARD)
    r.finish()
    check("guard_early", not r.launches() and r.status() == "came-back", "came back, nothing launched", str(r.read(".done")))
    check("guard_early", r.t_end - r.t0 < 1.5, "without waiting for the old process", "%.2f s" % (r.t_end - r.t0))


def case_guard_not_back():
    # the 180 case: the exec is accepted, the old process goes at 0.5 s and nothing comes: the launch follows guard_ms (2 s) + the settle after the exit
    r = Run("guard_not_back", app_seconds=0.5, wait="PPSA99203", settle_ms=100, extra=GUARD)
    r.finish()
    ls = r.launches()
    check("guard_not_back", len(ls) == 1 and ls[0]["title"] == "PPSA99203", "PPSA99203 launched once", str(ls))
    timing_after(r, r.app_end(), 2.0 + 0.1, "guard_not_back", "after the guard and the settle, not before", slack=1.2)
    check("guard_not_back", r.status() == "launched", "done says launched", str(r.read(".done")))
    check("guard_not_back", "no new process said it is up" in r.relaunch_log(), "the log says why it launched", r.relaunch_log()[-400:])


def case_guard_slow_new_app():
    # another app process is there (the new one, starting slowly) but says nothing: the payload waits up to hard_ms (5 s), then launches (on the console
    # the launch then answers "already running")
    r = Run("guard_slow", app_seconds=0.5, wait="PPSA99203", settle_ms=100, extra=GUARD, env={"RELAUNCH_STUB_OTHERAPP": "1"})
    r.finish()
    timing_after(r, r.app_end(), 5.0 + 0.1, "guard_slow", "after hard_ms, not after guard_ms", slack=1.2)
    check("guard_slow", "another app process is there" in r.relaunch_log(), "the log says it waits for it", r.relaunch_log()[-400:])


def case_guard_slow_then_back():
    # another app process is there and says it is up at 3 s (after guard_ms, before hard_ms): nothing launched
    r = Run("guard_slow_back", app_seconds=0.5, wait="PPSA99203", settle_ms=100, extra=GUARD, env={"RELAUNCH_STUB_OTHERAPP": "1"})
    r.sleep_until(3.0)
    open(r.state + ".alive", "w").write("1\n")
    r.finish()
    check("guard_slow_back", not r.launches() and r.status() == "came-back", "came back late, nothing launched", str(r.read(".done")))


def case_guard_cancelled():
    # the exec was refused and the app went back to the game: it cancels; the game ends normally later: nothing is launched
    r = Run("guard_cancel", app_seconds=3.0, wait="PPSA99203", settle_ms=100, extra=GUARD)
    r.sleep_until(0.8)
    open(r.state + ".cancel", "w").write("1\n")
    r.finish()
    check("guard_cancel", not r.launches() and r.status() == "cancelled", "a cancelled guard launches nothing", str(r.read(".done")))


def case_guard_clamped():
    # a guard of 5 ms is 1 s, not 0 (no guard would launch at once on top of an exec that works)
    r = Run("guard_clamp", app_seconds=0.3, wait="PPSA99203", settle_ms=100, extra="guard_ms=5\n")
    r.finish()
    timing_after(r, r.app_end(), 1.0 + 0.1, "guard_clamp", "5 ms became 1 s", slack=1.2)


cases = [v for k, v in sorted(globals().items()) if k.startswith("case_") and callable(v)]
t0 = time.time()
with ThreadPoolExecutor(max_workers=len(cases)) as pool:
    futures = [pool.submit(c) for c in cases]
    for c, f in zip(cases, futures):
        try:
            f.result()
        except Exception as e:  # a case that blew up is a failure, not a crash of the test
            failures.append("%s: raised %r" % (c.__name__, e))

for f in failures:
    print("FAIL:", f)
print("relaunch payload test: %s (%d cases, %.0f s)" % ("all passed" if not failures else "%d FAILED" % len(failures), len(cases), time.time() - t0))
sys.exit(1 if failures else 0)
PY
