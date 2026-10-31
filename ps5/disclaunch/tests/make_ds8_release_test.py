#!/usr/bin/env python3
# Helper 171l (build 184), AI-assisted: the host test of DiscSpeed8's shell release AS IT SHIPS. Cuts shell_release() out of disc_speed8.c and runs it
# against a made-up SceShellCore (traced / stopped / a SIGSTOP of PT_ATTACH still pending, PT_DETACH that fails, a SIGCONT that does not land) to check
# that the shell is never left traced or stopped when it can be helped, and that a shell that stays stopped is reported. Also checks that the thread
# decides the shell patch from the firmware (11.40 only) and the flag disc_speed8_anyfw before patch_shell() is first called.
# Usage: make_ds8_release_test.py <disc_speed8.c> <out.c>
import sys

src = open(sys.argv[1], encoding="utf-8").read()
a = src.index("static int shell_release(pid_t pid, const char *why) {")
b = src.index("static int remote_attach(pid_t pid) {", a)
release = src[a:b]

# static facts of the shipped thread (the order of the firmware gate and the first patch)
th = src[src.index("void *ds8_thread(void *arg) {"):]
gate = th.index("g_patch_on = is_1140 || anyfw;")
first = th.index("    patch_shell();\n")
assert gate < first, "the firmware gate must come before the first patch_shell()"
assert "FW_1140" in th[:gate] and "FLAG_ANYFW" in th[:gate]
ps = src[src.index("static void patch_shell(void) {"):]
ps = ps[:ps.index("\n}\n")]
assert ps.index("if (!g_patch_on)") < ps.index("find_shellcore()"), "patch_shell must return before looking for the shell when the patch is off"
assert "if (g_patch_on && !kp_quiet()) {" in th, "the loop must not look for the shell when the patch is off"
sess = src[src.index("static int patch_session(pid_t pid, uintptr_t base) {"):]
sess = sess[:sess.index("\n}\n")]
assert sess.count("remote_detach(pid)") == 1 and sess.rstrip().endswith("return result;"), "patch_session must detach once on every path"
assert "break;" in sess and "return" not in sess[sess.index("do {"):sess.index("} while (0);")], "no return inside the attached section"

head = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#define PT_DETACH 11
#define SIGCONT 19

/* the made-up shell */
static int s_traced, s_stopped, s_pending_stop, s_gone, s_detach_fails, s_cont_ignored, s_detaches, s_conts, s_steps;
static char s_last[512];

static void stepf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void stepf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last, sizeof(s_last), fmt, ap);
    va_end(ap);
    s_steps++;
    if (getenv("DS_VERBOSE")) printf("  step: %s\n", s_last);
}
static int privileged_ptrace(int request, pid_t pid, void *address, int data) {
    (void)pid; (void)address; (void)data;
    if (request != PT_DETACH) return -1;
    s_detaches++;
    if (s_detach_fails > 0) { s_detach_fails--; errno = 16; return -1; }
    if (!s_traced) { errno = 16; return -1; }
    s_traced = 0;
    s_stopped = 0;
    if (s_pending_stop) { s_pending_stop = 0; s_stopped = 1; } /* the SIGSTOP of PT_ATTACH lands after the detach: stopped for good */
    return 0;
}
static int kill(pid_t pid, int sig) {
    (void)pid;
    if (sig != SIGCONT) return -1;
    s_conts++;
    if (s_cont_ignored) return 0;
    if (s_traced) { s_pending_stop = 0; return 0; } /* a traced stop is the tracer's: SIGCONT does not end it */
    s_pending_stop = 0;
    s_stopped = 0;
    return 0;
}
static int proc_state(pid_t pid, int *stopped, int *traced) {
    (void)pid;
    if (s_gone) return 0;
    *stopped = s_stopped;
    *traced = s_traced;
    return 1;
}
static void ds8_sleep_ms(long ms) { (void)ms; }
'''

tail = r'''
static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s (line %d)\n", what, __LINE__); fails++; } else if (getenv("DS_VERBOSE")) printf("ok: %s\n", what); } while (0)
static void shell(int traced, int stopped, int pending) {
    s_traced = traced; s_stopped = stopped; s_pending_stop = pending; s_gone = 0; s_detach_fails = 0; s_cont_ignored = 0;
    s_detaches = 0; s_conts = 0; s_steps = 0; s_last[0] = 0;
}
int main(void) {
    shell(1, 1, 0);
    CHECK(shell_release(42, "t") == 1 && !s_traced && !s_stopped, "normal: detached, running");
    CHECK(s_steps == 0, "normal: nothing logged");
    CHECK(s_conts == 1, "normal: one SIGCONT after the detach");

    shell(1, 0, 1); /* the SIGSTOP of PT_ATTACH had not landed when we detach */
    CHECK(shell_release(42, "t") == 1 && !s_traced && !s_stopped, "pending SIGSTOP at the detach: the shell runs");

    shell(1, 1, 0);
    s_detach_fails = 1;
    CHECK(shell_release(42, "t") == 1 && !s_traced && !s_stopped, "first PT_DETACH fails: retried, the shell runs");
    CHECK(s_detaches == 2, "PT_DETACH sent twice");
    CHECK(strstr(s_last, "runs untraced") != NULL, "the recovery is logged");

    shell(1, 1, 0);
    s_detach_fails = 100;
    CHECK(shell_release(42, "t") == 0, "PT_DETACH never works: reported as a failure");
    CHECK(strstr(s_last, "STILL") != NULL, "and logged as STILL traced");
    CHECK(s_detaches == 11, "PT_DETACH tried 1 + 10 times");

    shell(1, 0, 1);
    s_cont_ignored = 1;
    CHECK(shell_release(42, "t") == 0 && s_stopped, "a stop that SIGCONT does not end: reported");

    shell(1, 1, 0);
    s_gone = 1;
    CHECK(shell_release(42, "t") == 1, "the shell gone after a good detach: fine");

    CHECK(shell_release(0, "t") == 1, "no pid: nothing to do");

    printf("ds8 shell release test: %s (%d failure(s))\n", fails ? "FAILED" : "all passed", fails);
    return fails ? 1 : 0;
}
'''

open(sys.argv[2], "w", encoding="utf-8").write(head + release + tail)
