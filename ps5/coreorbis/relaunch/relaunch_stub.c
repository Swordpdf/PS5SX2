// PS5SX2 (2.11, AI-assisted): stand-ins for the console's calls so relaunch.c runs on a PC (test-relaunch-payload.sh, built with -DRELAUNCH_HOST_TEST).
// What the payload asks the console (libSceSystemService / libSceUserService by name, the process table, the shutdown event flags) is answered from
// files and environment variables the test sets:
//   RELAUNCH_STUB_LOG      the file the stub "launches" are appended to ("launch <title> user <n> size <n> at <ms>"), also where launches are counted
//   RELAUNCH_STUB_RC       return codes of sceLncUtilLaunchApp in turn ("-3,-3,0"; the last one repeats; hex like 0x8094000c works); default 0
//   RELAUNCH_STUB_OLD      the closing title; sceLncUtilGetAppId(it) answers 5 while the file RELAUNCH_STUB_APPFILE exists, else -1
//   RELAUNCH_STUB_LAUNCH   the title to start; sceLncUtilGetAppId(it) answers 5 once RELAUNCH_STUB_UPAFTER launches (default 1) are in the log
//   RELAUNCH_STUB_BIGFILE  the running big app's id as text; no file: 0xffffffff
//   RELAUNCH_STUB_NULL     names of symbols that are not found (comma separated, e.g. sceLncUtilGetAppId)
//   RELAUNCH_STUB_NOLIBS   the libraries do not open (every symbol is NULL)
//   RELAUNCH_STUB_QUIET    1: the console is shutting down; RELAUNCH_STUB_EVF=fail: the event flags can't be opened (fail-open: not shutting down)
//   RELAUNCH_STUB_PIDSTATE unknown: the process can't be looked at
//   RELAUNCH_STUB_FGFAIL   1: no foreground user (the initial user, 456, is used); else the foreground user is RELAUNCH_STUB_USER (default 123)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
	uint32_t size;
	int user_id;
	uint32_t app_opt;
	uint64_t crash_report;
	uint64_t check_flag;
} stub_param_t;

static int env_is(const char* name, const char* value)
{
	const char* v = getenv(name);
	return v && strcmp(v, value) == 0;
}

static int file_exists(const char* path)
{
	struct stat st;
	return path && stat(path, &st) == 0;
}

static long long wall_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int launches_logged(void)
{
	const char* path = getenv("RELAUNCH_STUB_LOG");
	if (!path)
		return 0;
	FILE* f = fopen(path, "r");
	if (!f)
		return 0;
	int n = 0;
	char line[256];
	while (fgets(line, sizeof(line), f))
		if (strncmp(line, "launch ", 7) == 0)
			n++;
	fclose(f);
	return n;
}

static int stub_launch(const char* title, const char** argv, stub_param_t* prm)
{
	(void)argv;
	static int call;
	const char* path = getenv("RELAUNCH_STUB_LOG");
	if (path)
	{
		FILE* f = fopen(path, "a");
		if (f)
		{
			fprintf(f, "launch %s user %d size %u at %lld\n", title, prm->user_id, prm->size, wall_ms());
			fclose(f);
		}
	}
	const char* seq = getenv("RELAUNCH_STUB_RC");
	if (!seq || !seq[0])
	{
		call++;
		return 0;
	}
	// the call-th number of the list (the last one repeats)
	int idx = call++;
	const char* p = seq;
	for (;;)
	{
		const char* comma = strchr(p, ',');
		if (idx == 0 || !comma)
			return (int)(unsigned)strtoll(p, NULL, 0); // "-3" and "0x8094000c" both
		p = comma + 1;
		idx--;
	}
}

static int stub_app_id(const char* title)
{
	const char* old = getenv("RELAUNCH_STUB_OLD");
	const char* launch = getenv("RELAUNCH_STUB_LAUNCH");
	if (old && strcmp(title, old) == 0 && file_exists(getenv("RELAUNCH_STUB_APPFILE")))
		return 5;
	if (launch && strcmp(title, launch) == 0)
	{
		const char* after = getenv("RELAUNCH_STUB_UPAFTER");
		if (launches_logged() >= (after ? atoi(after) : 1))
			return 5;
	}
	return -1;
}

static uint32_t stub_big_app(void)
{
	const char* path = getenv("RELAUNCH_STUB_BIGFILE");
	if (!path)
		return 0xffffffffu;
	FILE* f = fopen(path, "r");
	if (!f)
		return 0xffffffffu;
	char buf[32] = {0};
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = '\0';
	fclose(f);
	return buf[0] ? (uint32_t)strtoul(buf, NULL, 0) : 0xffffffffu;
}

static int stub_title_of(uint32_t id, char* out)
{
	(void)id;
	strcpy(out, "PPSA99999");
	return 0;
}

static int stub_user_init(void* p)
{
	(void)p;
	return 0;
}

static int stub_fg_user(int* user)
{
	if (env_is("RELAUNCH_STUB_FGFAIL", "1"))
		return -1;
	const char* u = getenv("RELAUNCH_STUB_USER");
	*user = u ? atoi(u) : 123;
	return 0;
}

static int stub_initial_user(int* user)
{
	*user = 456;
	return 0;
}

static int listed(const char* list, const char* name)
{
	if (!list)
		return 0;
	const size_t n = strlen(name);
	for (const char* p = list; (p = strstr(p, name)) != NULL; p += n)
		if ((p == list || p[-1] == ',') && (p[n] == '\0' || p[n] == ','))
			return 1;
	return 0;
}

void* rl_lib(const char* name)
{
	(void)name;
	return getenv("RELAUNCH_STUB_NOLIBS") ? NULL : (void*)1;
}

void* rl_sym(void* lib, const char* name)
{
	if (!lib || listed(getenv("RELAUNCH_STUB_NULL"), name))
		return NULL;
	if (!strcmp(name, "sceLncUtilLaunchApp"))
		return (void*)&stub_launch;
	if (!strcmp(name, "sceLncUtilGetAppId"))
		return (void*)&stub_app_id;
	if (!strcmp(name, "sceLncUtilGetAppIdOfRunningBigApp"))
		return (void*)&stub_big_app;
	if (!strcmp(name, "sceLncUtilGetAppTitleId"))
		return (void*)&stub_title_of;
	if (!strcmp(name, "sceUserServiceInitialize"))
		return (void*)&stub_user_init;
	if (!strcmp(name, "sceUserServiceGetForegroundUser"))
		return (void*)&stub_fg_user;
	if (!strcmp(name, "sceUserServiceGetInitialUser"))
		return (void*)&stub_initial_user;
	return NULL;
}

// 1 gone (no such process, or a zombie), 0 still there, -1 can't tell: /proc/<pid>/stat, "pid (comm) S ..."
int rl_pid_state(int pid)
{
	if (env_is("RELAUNCH_STUB_PIDSTATE", "unknown"))
		return -1;
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	FILE* f = fopen(path, "r");
	if (!f)
		return 1;
	char buf[512] = {0};
	const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	const char* close = strrchr(buf, ')');
	if (!close || close[1] != ' ')
		return -1;
	return close[2] == 'Z' || close[2] == 'X' ? 1 : 0;
}

// fix180: RELAUNCH_STUB_OTHERAPP = 1 (another app process runs), unknown, else none
int rl_other_app(int old_pid)
{
	(void)old_pid;
	if (env_is("RELAUNCH_STUB_OTHERAPP", "unknown"))
		return -1;
	return env_is("RELAUNCH_STUB_OTHERAPP", "1") ? 1 : 0;
}

int rl_shutting_down(void)
{
	if (env_is("RELAUNCH_STUB_EVF", "fail"))
		return 0; // the event flags can't be opened: fail-open
	return env_is("RELAUNCH_STUB_QUIET", "1");
}
