/* PS5SX2 Installer: every file and folder it uses, with plat_root() in front. */
#pragma once

#define PATH_LEN 1024

typedef struct {
  char data[PATH_LEN];  /* /data */
  char app[PATH_LEN];   /* /data/homebrew/PPSA99203 */
  char pcsx2[PATH_LEN]; /* /data/PCSX2 */
  char work[PATH_LEN];  /* /data/PS5SX2-Installer: everything of ours */

  char log[PATH_LEN];          /* installer.log */
  char install_lock[PATH_LEN]; /* install.lock (one install at a time) */
  char logger_lock[PATH_LEN];  /* logger.lock (one resident logger) */
  char journal[PATH_LEN];      /* journal.txt: an install in progress */
  char manifest[PATH_LEN];     /* manifest.txt: what we installed, with SHA-256 */
  char installed[PATH_LEN];    /* installed.txt: the last install */
  char download[PATH_LEN];     /* download/ */
  char staging[PATH_LEN];      /* staging/ */
  char backup[PATH_LEN];       /* backup/<tag>/: the files an install replaced */
  char rollback[PATH_LEN];     /* rollback/: new files taken out again by an undo */
  char notes[PATH_LEN];        /* release-notes/<tag>/ */
  char outbox[PATH_LEN];       /* outbox/: reports waiting to be sent */
  char console_id[PATH_LEN];   /* console-id.txt */
  char logger_state[PATH_LEN]; /* logger-state.txt */

  /* Switches (files the user creates) */
  char no_install[PATH_LEN];    /* no-install: only the logger */
  char no_log_upload[PATH_LEN]; /* no-log-upload: no logger */
  char send_shelf_logs[PATH_LEN]; /* send-shelf-logs: also send sessions that only showed the shelf (1.3) */
  char reinstall[PATH_LEN];     /* reinstall: install even if up to date (used once) */
  char tester_name[PATH_LEN];   /* tester-name.txt */
  char upload_url[PATH_LEN];    /* upload-url.txt */
} paths_t;

extern paths_t g_p;

int paths_init(void);
