/* PS5SX2 Installer: fixed names and limits. */
#pragma once

#define INSTALLER_NAME "PS5SX2 Installer"
#define INSTALLER_VERSION "1.3"
#define INSTALLER_UA "PS5SX2-Installer/" INSTALLER_VERSION

/* The no-log build (make ps5 NOLOG=1): installs and leaves, with no logger in it. */
#ifdef INSTALLER_NO_LOGGER
#define INSTALLER_VARIANT " (no log upload)"
#else
#define INSTALLER_VARIANT ""
#endif

/* Where builds are published: the official releases of Swordpdf/PS5SX2 (public since 2026-09-29). Version 1.0
 * installed the test builds from Swordpdf/PS5SX2TESTS. */
#define GH_REPO "Swordpdf/PS5SX2"
#define GH_API_URL "https://api.github.com/repos/" GH_REPO "/releases/latest"
#define GH_DOWNLOAD_PREFIX "https://github.com/" GH_REPO "/releases/download/"

/* Log relay (a Cloudflare Worker that forwards to a private Discord channel; see worker/).
 * Empty = not deployed yet: reports wait in the outbox. /data/PS5SX2-Installer/upload-url.txt overrides it. */
#ifndef RELAY_URL_DEFAULT
#define RELAY_URL_DEFAULT ""
#endif

#define TITLE_ID "PPSA99203"

/* Logical paths; plat_root() is put in front of them (empty on the PS5, a test folder on the host). */
#define P_DATA "/data"
#define P_APP "/data/homebrew/" TITLE_ID
#define P_PCSX2 "/data/PCSX2"
#define P_WORK "/data/PS5SX2-Installer"

/* Limits for what we accept from GitHub. */
#define MAX_ZIP_BYTES (512ull << 20)
#define MAX_UNZIPPED_BYTES (1024ull << 20)
#define MAX_ZIP_ENTRIES 20000
#define MAX_ENTRY_NAME 512
#define MIN_FREE_MARGIN (64ull << 20)

/* Logs */
#define REPORT_MAX_BYTES (8u << 20) /* a Discord upload takes about 8-10 MB */
#define OUTBOX_MAX_FILES 20
#define OUTBOX_MAX_BYTES (60ull << 20)
#define LOGGER_POLL_SECONDS 5
