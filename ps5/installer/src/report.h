/* PS5SX2 Installer: the report of one PS5SX2 session, like the settings page's "Download logs". */
#pragma once

#include "util.h"

#include <sys/types.h>
#include <time.h>

typedef struct {
  pid_t pid;
  time_t started;      /* pid.txt's time */
  int watched;         /* 1 = the logger saw it end; 0 = found afterwards */
  const char *suffix;  /* "" = the logs of the last session; ".1" = the one before */
  /* filled in by report_build: */
  char label[160];     /* "Test build 1 · vk-285-112" */
  char build[80];      /* "vk-285-112" */
  char game[256];      /* "Black (USA).iso" */
  char end[48];        /* ok, crash, gpu-hang, no-start, no-game (+note); no-logs: none of its logs were found */
  char end_line[400];
  int logs_found;      /* 1 = its settings.log lines or its boot log were found */
  int problem;         /* 1 = its boot log or a hang dump shows something wrong the settings.log lines may lack */
  unsigned shelf_skipped; /* in: shelf-only sessions not sent since the last report (the header says so) */
} session_info;

/* Builds the report (addresses and the settings page's token removed); 0 on success, -1 (nothing to send) if
 * that couldn't be done. */
int report_build(session_info *s, sbuf *out);

/* 1 = the session only showed the shelf and ended normally: its logs were found, no game was started, nothing
 * went wrong (no crash, GPU hang, game that didn't start, empty game list) and no tester note. 1.3 doesn't send
 * these (the send-shelf-logs switch does). */
int report_quiet_shelf(const session_info *s);

/* Masks IPv4/IPv6/MAC addresses and the given secret in place (secret may be NULL); -1 if it couldn't. */
int report_redact(sbuf *text, const char *secret);
