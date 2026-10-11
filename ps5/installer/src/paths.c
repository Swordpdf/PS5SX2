/* PS5SX2 Installer: every file and folder it uses. */
#include "paths.h"

#include "config.h"
#include "plat.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

paths_t g_p;

static int root_join(char *dst, const char *logical) {
  const int n = snprintf(dst, PATH_LEN, "%s%s", plat_root(), logical);
  if (n < 0 || n >= PATH_LEN) {
    err_set("path too long: %s", logical);
    return -1;
  }
  return 0;
}

static int work_join(char *dst, const char *name) { return path_join(dst, PATH_LEN, g_p.work, name); }

int paths_init(void) {
  memset(&g_p, 0, sizeof(g_p));
  if (root_join(g_p.data, P_DATA) || root_join(g_p.app, P_APP) || root_join(g_p.pcsx2, P_PCSX2) ||
      root_join(g_p.work, P_WORK))
    return -1;
  if (work_join(g_p.log, "installer.log") || work_join(g_p.install_lock, "install.lock") ||
      work_join(g_p.logger_lock, "logger.lock") || work_join(g_p.journal, "journal.txt") ||
      work_join(g_p.manifest, "manifest.txt") || work_join(g_p.installed, "installed.txt") ||
      work_join(g_p.download, "download") || work_join(g_p.staging, "staging") ||
      work_join(g_p.backup, "backup") || work_join(g_p.rollback, "rollback") ||
      work_join(g_p.notes, "release-notes") || work_join(g_p.outbox, "outbox") ||
      work_join(g_p.console_id, "console-id.txt") || work_join(g_p.logger_state, "logger-state.txt") ||
      work_join(g_p.no_install, "no-install") || work_join(g_p.no_log_upload, "no-log-upload") ||
      work_join(g_p.send_shelf_logs, "send-shelf-logs") ||
      work_join(g_p.reinstall, "reinstall") || work_join(g_p.tester_name, "tester-name.txt") ||
      work_join(g_p.upload_url, "upload-url.txt"))
    return -1;
  return 0;
}
