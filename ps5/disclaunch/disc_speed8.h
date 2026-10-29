/* disc_speed8.h (PS5SX2 helper 171j, 2026-10-26, AI-assisted): DiscSpeed8's SceShellCore disc-speed patch and the 60 s
 * speed pin, as a thread of the resident helper. See disc_speed8.c for the recipe. The owner chose to include the
 * SceShellCore ptrace patch for this disc-speed feature; a flag file /data/PCSX2/flags/disc_nospeed8 turns all of it off.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISC_SPEED8_H
#define DISC_SPEED8_H

/* The helper's disc-speed thread: patch the running SceShellCore once per instance so every setSpeed it applies is the
 * table's 8.0 entry (CDB db 01 00 80), re-patch when the shell restarts, and re-send the 8x CDB through the drive's pass
 * device every 60 s while a disc is in. Does nothing (and returns at once) when /data/PCSX2/flags/disc_nospeed8 exists.
 * Assumes the helper process already holds debugger rights (disc_launch.c elevates it); it tops them up if not.
 * Never returns on the normal path. Safe to start from a detached pthread. */
void *ds8_thread(void *arg);

/* Starts ds8_thread on a detached thread of its own. Call once, after the helper has elevated itself. */
void ds8_start(void);

#endif
