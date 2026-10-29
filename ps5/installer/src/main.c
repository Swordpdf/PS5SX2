/* PS5SX2 Installer: a payload ELF, separate from the PS5SX2 Helper.
 *
 * Sent by hand (the launch block empty), every time it is sent to the console:
 *   1. it installs the latest release from GitHub if it is newer than the one installed (install.c);
 *   2. if no copy of it is running yet, it stays running as the logger: when PS5SX2 closes after a game or a
 *      problem, that session's logs are sent (logger.c). A copy sent later in the same boot installs and then
 *      exits, because the first copy already holds logger.lock.
 * 1.4: PS5SX2 itself sends it, with the launch block filled in (launch.h):
 *   - at each start, as the logger only (INSTALLER_LAUNCH_LOGGER): step 2 alone, never an install;
 *   - from the shelf's System update row (INSTALLER_LAUNCH_RELAUNCH): it waits until PS5SX2 is gone, does step 1,
 *     starts PS5SX2 again when the install went through or there was nothing to install, then step 2.
 * The no-log build (INSTALLER_NO_LOGGER, make ps5 NOLOG=1) only does 1: it has no logger in it.
 * Switches (empty files in /data/PS5SX2-Installer): no-install, no-log-upload, send-shelf-logs, reinstall (used
 * once). */
#include "config.h"
#include "fsx.h"
#include "install.h"
#include "launch.h"
#include "log.h"
#ifndef INSTALLER_NO_LOGGER
#include "logger.h"
#endif
#include "paths.h"
#include "plat.h"
#include "tls.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Filled in by PS5SX2 in the ELF it sends (launch.h); all zeros but the marker in the file. In .data, never optimized
 * away, and read through volatile: the compiler can't know its bytes change before the program starts. */
__attribute__((used, aligned(16))) volatile installer_launch g_launch = {INSTALLER_LAUNCH_MARKER, 0, 0, 0, 0, "", ""};

static void printable(char *s, size_t size) {
  s[size - 1] = '\0';
  for (char *p = s; *p; p++)
    if ((unsigned char)*p < 0x20 || (unsigned char)*p >= 0x7f)
      *p = '?';
}

/* 1 when PS5SX2 filled the block in (a copy in *out), 0 when it is the file's own. */
static int read_launch(installer_launch *out) {
  memset(out, 0, sizeof(*out));
  const volatile unsigned char *src = (const volatile unsigned char *)&g_launch;
  unsigned char *dst = (unsigned char *)out;
  for (size_t i = 0; i < sizeof(*out); i++)
    dst[i] = src[i];
  if (out->magic != INSTALLER_LAUNCH_MAGIC)
    return 0;
  if (out->size != sizeof(installer_launch)) {
    log_line("launch block: size %u, this installer knows %zu: ignored", (unsigned)out->size, sizeof(installer_launch));
    return 0;
  }
  printable(out->app_tag, sizeof(out->app_tag));
  printable(out->expect_tag, sizeof(out->expect_tag));
  return 1;
}

/* PS5SX2 started the installer and closes itself: waits until its process is gone and the shell no longer has it
 * as the running app. 0 when it is gone, -1 when it is still there after the time allowed. */
static int wait_for_app(pid_t pid) {
  const char *t = plat_test_env("PS5SX2_TEST_EXIT_WAIT");
  const uint64_t limit_ms = (uint64_t)(t ? strtol(t, NULL, 10) : APP_EXIT_WAIT_SECONDS) * 1000u;
  const uint64_t start = mono_ms();
  int told = 0;
  for (;;) {
    const int alive = plat_pid_alive(pid);
    const int shell = plat_app_running();
    if (alive != 1 && shell != 1) {
      log_line("PS5SX2 (pid %d) has closed after %.1f s (process %s, shell %s)", (int)pid,
               (double)(mono_ms() - start) / 1000.0, alive == 0 ? "gone" : "unknown",
               shell == 0 ? "has another app or none" : "can't be asked");
      return 0;
    }
    const uint64_t waited = mono_ms() - start;
    if (waited >= limit_ms)
      return -1;
    if (!told && waited >= 3000) {
      told = 1;
      notify("PS5SX2 Installer: waiting for PS5SX2 to close...");
    }
    sleep_ms(500);
  }
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  plat_init();
  if (paths_init() != 0) {
    plat_notify_text("PS5SX2 Installer can't start: a path is too long");
    return 1;
  }
  if (fs_mkdirs(g_p.work, 0777) != 0) {
    char msg[600];
    snprintf(msg, sizeof(msg), "PS5SX2 Installer can't write to /data (%s)", err_get());
    plat_notify_text(msg);
    return 1;
  }
  /* Our folders must be real folders: a link would send our writes and clean-ups somewhere else. */
  {
    const char *const own[] = {g_p.work, g_p.download, g_p.staging, g_p.backup, g_p.rollback, g_p.notes, g_p.outbox};
    for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
      struct stat st;
      if (lstat(own[i], &st) == 0 && !S_ISDIR(st.st_mode)) {
        char msg[PATH_LEN + 160];
        snprintf(msg, sizeof(msg),
                 "PS5SX2 Installer: %s is a link or a file, not a folder. Remove it, then send the installer again.",
                 own[i]);
        plat_notify_text(msg);
        return 1;
      }
    }
  }
  log_line("---- %s %s%s started (pid %d)", INSTALLER_NAME, INSTALLER_VERSION, INSTALLER_VARIANT, (int)getpid());

  installer_launch launch;
  const int from_app = read_launch(&launch);
  const int logger_only = from_app && (launch.flags & INSTALLER_LAUNCH_LOGGER);
  const int relaunch = from_app && !logger_only && (launch.flags & INSTALLER_LAUNCH_RELAUNCH);
  if (from_app)
    log_line("started by PS5SX2 %s (pid %d) %s%s%s", launch.app_tag[0] ? launch.app_tag : "(no tag)",
             (int)launch.app_pid, logger_only ? "as its session logger" : "for an update",
             launch.expect_tag[0] ? "; the shelf showed " : "", launch.expect_tag);

#ifdef INSTALLER_NO_LOGGER
  if (logger_only) {
    /* PS5SX2 ships the logging build; a no-log build put in its place has nothing to do here. */
    log_line("---- done (the no-log build has no logger)");
    return 0;
  }
#else
  if (logger_only && fs_exists(g_p.no_log_upload)) {
    /* Quietly: PS5SX2 sends this copy at every start, and the person switched the upload off. */
    log_line("---- done (the logger is switched off: %s)", g_p.no_log_upload);
    return 0;
  }
#endif

  int rc = INSTALL_NOTHING;
  const int tls_ok = tls_global_init() == 0;
  if (!tls_ok) {
    notify("PS5SX2 Installer: %s", err_get());
    rc = INSTALL_FAILED;
  } else if (logger_only) {
    log_line("install: not this time (PS5SX2 sent this copy as its session logger)");
  } else if (from_app && launch.app_pid > 0 && wait_for_app((pid_t)launch.app_pid) != 0) {
    notify("PS5SX2 didn't close, so nothing was installed: close it, then try the update again");
    log_line("PS5SX2 (pid %d) still runs after the time allowed: no install", (int)launch.app_pid);
    rc = INSTALL_FAILED;
  } else {
    rc = install_run();
  }

  if (relaunch && (rc == INSTALL_DONE || rc == INSTALL_NOTHING)) {
    notify("Starting PS5SX2...");
    char why[200];
    const int lr = plat_launch_app(why, sizeof(why));
    log_line("start PS5SX2: %s%s", why, lr == 0 ? "" : " (failed)");
    if (lr != 0)
      notify("PS5SX2 Installer: PS5SX2 couldn't be started (%s): start it from the home screen", why);
  } else if (relaunch) {
    log_line("PS5SX2 is not started again: the update didn't go through (see above)");
  }

#ifdef INSTALLER_NO_LOGGER
  log_line("---- done (the no-log build: no logs are kept or sent)");
#else
  if (plat_test_env("PS5SX2_TEST_NO_LOGGER")) {
    log_line("---- done (host test: no logger)");
    if (tls_ok)
      tls_global_free();
    return 0;
  }
  int lfd = -1;
  const int lk = fs_lock(g_p.logger_lock, &lfd);
  if (lk == 0) {
    logger_run();
    fs_unlock(lfd);
  } else if (lk == 1) {
    log_line("logger: another copy is already running");
  } else {
    log_line("logger: %s", err_get());
  }
  log_line("---- done");
#endif
  if (tls_ok)
    tls_global_free();
  return 0;
}
