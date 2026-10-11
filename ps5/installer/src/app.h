/* PS5SX2 Installer: facts about the installed PS5SX2 app. */
#pragma once

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

/* 1 = PS5SX2 is running (pid from /data/PCSX2/pid.txt), 0 = not, -1 = can't tell. */
int app_running(pid_t *pid_out, time_t *pid_mtime_out);

/* The build tag inside the installed eboot.bin ("[boot] build=<tag>"); 0 if found. */
int app_installed_tag(char *tag, size_t size);

/* <0, 0, >0 like strcmp for tags of the form <prefix>-<number> with the same prefix; 2 when not comparable. */
int tag_compare(const char *a, const char *b);
