/* Mount /data into homebrew app sandboxes, the ShadowMountPlus way. */
#pragma once

#include <stddef.h>

/* Polls /mnt/sandbox and nullfs-mounts /user/data (the real /data) onto
 * <sandbox>/data for every PPSA* title slot, so homebrew apps see /data
 * before the Lapy elevation moves their root. Self-healing: a slot whose
 * /data is not the mount is remounted. Runs forever; meant for a thread. */
void *payload_mount_thread(void *arg);
