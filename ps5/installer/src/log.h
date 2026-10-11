/* PS5SX2 Installer: its own log (/data/PS5SX2-Installer/installer.log) and the notifications. */
#pragma once

void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* A notification on the PS5, also written to the log. */
void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
