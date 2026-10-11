/* PS5SX2 Installer: a payload ELF, separate from the PS5SX2 Helper.
 *
 * Every time it is sent to the console:
 *   1. it installs the latest release from GitHub if it is newer than the one installed (install.c);
 *   2. if no copy of it is running yet, it stays running as the logger: when PS5SX2 closes after a game or a
 *      problem, that session's logs are sent (logger.c). A copy sent later in the same boot installs and then
 *      exits, because the first copy already holds logger.lock.
 * The no-log build (INSTALLER_NO_LOGGER, make ps5 NOLOG=1) only does 1: it has no logger in it.
 * Switches (empty files in /data/PS5SX2-Installer): no-install, no-log-upload, send-shelf-logs, reinstall (used
 * once). */
#include "config.h"
#include "fsx.h"
#include "install.h"
#include "log.h"
#ifndef INSTALLER_NO_LOGGER
#include "logger.h"
#endif
#include "paths.h"
#include "plat.h"
#include "tls.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

  const int tls_ok = tls_global_init() == 0;
  if (!tls_ok)
    notify("PS5SX2 Installer: %s", err_get());
  else
    install_run();

#ifdef INSTALLER_NO_LOGGER
  log_line("---- done (the no-log build: no logs are kept or sent)");
#else
  if (plat_test_env("PS5SX2_TEST_NO_LOGGER")) {
    log_line("---- done (host test: no logger)");
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
