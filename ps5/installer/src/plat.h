/* PS5SX2 Installer: what differs between the PS5 and the host test build. */
#pragma once

#include <stddef.h>
#include <sys/types.h>

/* Put in front of every logical path ("/data/..."): "" on the PS5, $PS5SX2_ROOT on the host. */
const char *plat_root(void);

/* A PS5 system notification (debug style, text only), like the helper's "PS5SX2 Helper Loaded!".
 * On the host: a line on stderr and in $PS5SX2_ROOT/notifications.txt. */
void plat_notify_text(const char *text);

/* The kernel's random numbers; 0 on success. No weak fallback. */
int plat_random(void *buf, size_t len);

/* When the system started (seconds since 1970), 0 if unknown. */
#include <time.h>
time_t plat_boot_time(void);

/* 1 if a process with this pid exists, 0 if not, -1 if we can't tell. */
int plat_pid_alive(pid_t pid);

/* Host tests only: extra CA file (PEM) and URL overrides; NULL on the PS5. */
const char *plat_test_env(const char *name);

/* Ignore SIGPIPE etc. */
void plat_init(void);
