/* PS5SX2 Installer: what PS5SX2 tells the installer when it starts it (1.4).
 *
 * The installer ELF ships in PS5SX2's folder (/app0/PS5SX2Installer.elf). PS5SX2 reads it, finds the block below by
 * its marker (the marker, then zeros to the end of the block), fills it in, and streams the ELF to the console's ELF
 * loader (127.0.0.1:9021). Two reasons to start it:
 *   - at every start of PS5SX2, as the session logger (INSTALLER_LAUNCH_LOGGER): nothing is installed, the copy only
 *     stays running as the logger (or leaves at once when a logger already runs, or no-log-upload is there);
 *   - from the shelf's System update row (INSTALLER_LAUNCH_RELAUNCH): the installer waits until PS5SX2 (app_pid) is
 *     gone, installs the latest release, and starts PS5SX2 again when that went through (or there was nothing new).
 * Sent by hand (a PC's payload sender), the block is empty and the installer does what 1.3 did: it installs, then
 * stays as the logger.
 *
 * The same layout is in ps5/coreorbis/orbis-shims/OrbisInstallerLaunch.h: change both, and the marker's number,
 * together. (PS5SX3's installer has a block of its own with another marker: the two never mix.)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>

#define INSTALLER_LAUNCH_MARKER "PS5SX2-INSTALLER-LAUNCH-1"
#define INSTALLER_LAUNCH_MAGIC 0x32495850u /* "PXI2" */
#define INSTALLER_LAUNCH_RELAUNCH 1u       /* flags: start PS5SX2 again once the install is done (or there was none) */
#define INSTALLER_LAUNCH_LOGGER 2u         /* flags: only the session logger; never install */
#define INSTALLER_LAUNCH_TAG 64

typedef struct installer_launch {
  char marker[32];                   /* INSTALLER_LAUNCH_MARKER, then zeros */
  uint32_t magic;                    /* INSTALLER_LAUNCH_MAGIC once PS5SX2 filled the block; 0 in the file */
  uint32_t size;                     /* sizeof(installer_launch) as PS5SX2 knows it */
  uint32_t flags;                    /* INSTALLER_LAUNCH_* */
  int32_t app_pid;                   /* PS5SX2's process: the installer waits until it is gone (0: don't wait) */
  char app_tag[INSTALLER_LAUNCH_TAG];    /* the build PS5SX2 is ("vk-285-180"), for the log */
  char expect_tag[INSTALLER_LAUNCH_TAG]; /* the release the shelf showed ("vk-285-181"), for the log; "" for none */
} installer_launch;

_Static_assert(sizeof(installer_launch) == 176, "the launch block's layout is shared with PS5SX2");
