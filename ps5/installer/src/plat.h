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

/* 1.4: whether PS5SX2 (PPSA99203) is the console's running app (the shell's "big app", running or suspended): 1 yes,
 * 0 no, -1 when the system can't be asked. The host tests say it with $PS5SX2_TEST_BIGAPP (a title ID). */
int plat_app_running(void);

/* 1.4: starts PS5SX2 and brings it to the front, as its tile does (sceLncUtilLaunchApp for the foreground user; never
 * fork or LoadExec). 0 when the system took the request; `why` gets the system's answer either way. On the host: a
 * line in $PS5SX2_ROOT/launched.txt. */
int plat_launch_app(char *why, size_t why_size);

/* fix181: 1 while the console is shutting down, going to rest mode or suspending (SceSystemStateMgrInfo 100/300/500, or SceSystemStateMgrStatus bit
 * 0x200000, read as ShadowMount+, the helper's kp.c and the relaunch payload read them), 0 otherwise or when it can't be read. On the host: the file
 * named by $PS5SX2_TEST_SHUTDOWN_FILE exists. */
int plat_shutting_down(void);

/* Host tests only: extra CA file (PEM) and URL overrides; NULL on the PS5. */
const char *plat_test_env(const char *name);

/* Ignore SIGPIPE etc. */
void plat_init(void);
