// PS5SX2 (2.11, AI-assisted): the relaunch payload, started through the console's ELF loader just before the app closes for "back to the menu".
//
// swordpdf / testers: "L1 + R1 + Touch sometimes crashes the console, it should always return you to the shelf". The app used to ask the shell to
// kill it and start its own eboot again (sceSystemServiceLoadExec(<eboot>)), the least certain operation it has. Now it closes the way the Reload
// tile and every game-less start close (LoadExec("exit"), known to be harmless here) and this payload, which lives in a process of its own, starts the
// main app again afterwards, the way its tile on the home screen does (sceLncUtilLaunchApp). The "open the shelf" note the app leaves (logs/
// open-shelf.txt, logs/relaunch.txt) makes that start open the shelf with the last game picked. ProsperoRelaunch.cpp embeds this file, writes the
// request over g_request's marker and sends the whole file to the loader on 127.0.0.1:9021.
//
// Request (the app writes it over the marker, NUL-terminated):
//   PS5SX2-RELAUNCH-REQUEST-V1\n pid=<the app's pid>\n wait=<the closing title, "" if unknown>\n launch=<title to start>\n state=<path prefix>\n
//   settle_ms=<ms after the app is gone>\n limit_s=<seconds to wait for it>\n [nosig_ms=<settle when nothing tells that the app is gone>\n]
//   [cap_ms=<how long the shell may go on listing the title after its process is gone, default 8000>\n]  (nosig_ms and cap_ms are for the PC test)
//   [min_exit_ms=<never launch earlier than this long after the app's process was seen gone, default 0 = no such rule>\n]
//   [retry_ms=<pause between two launch attempts, default 800, 100..2000>\n]  (176: the app always sends settle_ms, min_exit_ms and retry_ms)
//   [guard_ms=<GUARD mode: the app leaves by its own exec; launch only if no new process says "<state>.alive" this long after the old one went>\n
//    hard_ms=<with another app process there, wait this long at most>\n]  (fix180; relaunch_guard.h)
// Files: "<state>.started" is written as soon as the payload's libraries are loaded and sceLncUtilLaunchApp is known (the app waits for it before it commits
// to leaving; a payload that cannot launch gives no handshake, only a .done), "<state>.done" last ("status <word>\n..."),
// and the app can create "<state>.cancel" at any moment before the launch to stop it. Every step is appended to relaunch.log beside the state file
// (stdout and stderr belong to the loader's connection, which the app closes when it dies) and, in the console build, to the kernel log.
//
// What it does: waits (100 ms steps, at most limit_s) until the app's process is gone AND the shell no longer knows its title; settles settle_ms
// (the shell's own clean-up of the killed app takes about 2.5 s from the kill) and, with min_exit_ms, waits on until that long after the process went
// (ShadowMount+ takes an app that starts within a second of its exit for a handoff of the same app); does NOT launch when the console is going to rest
// mode or shutting down (fail-open: when the state can't be read it launches); then launches (a round is 5 tries at the 800 ms pause, more at a shorter
// pause: about 4 s either way) and checks the app really came up. It never launches while
// the old app's process is still there (a shell that goes on listing the title 8 s after its process went does not keep it from starting), and a
// cancelled request launches nothing. The SDK's library calls are looked up by name (dlsym), like the disc helper
// does (console-proven there); a missing one degrades to "process gone + a fixed settle" instead of failing.
//
// Built with the ps5-payload-dev SDK by Makefile.vk and embedded with .incbin. The PC test (test-relaunch-payload.sh) builds it with
// -DRELAUNCH_HOST_TEST and the stand-ins of relaunch_stub.c.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "relaunch_guard.h" // fix180: the guard mode's decision

#ifndef RELAUNCH_HOST_TEST
#include <dlfcn.h>
#include <ps5/klog.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#endif

// The app finds the marker and writes the request over it; the array lies in .data with its whole size in the file. volatile: the compiler must read
// what the app wrote, not the marker it sees here.
#define RELAUNCH_MARKER "PS5SX2-RELAUNCH-REQUEST-V1"
__attribute__((used)) volatile char g_request[1024] = RELAUNCH_MARKER;

// lnc_app_param_t as the disc helper (daemon/disc_launch.c, console-proven) lays it out.
typedef struct
{
	uint32_t size;
	int user_id;
	uint32_t app_opt;
	uint64_t crash_report;
	uint64_t check_flag;
} lnc_app_param_t;
typedef int (*LaunchApp_t)(const char*, const char**, lnc_app_param_t*);
typedef int (*GetAppId_t)(const char*);
typedef uint32_t (*GetBigApp_t)(void);
typedef int (*GetTitle_t)(uint32_t, char*);
typedef int (*UsrInit_t)(void*);
typedef int (*UsrUser_t)(int*);

typedef struct
{
	LaunchApp_t launch;
	GetAppId_t app_id;     // not used by the disc helper: may be missing
	GetBigApp_t big_app;
	GetTitle_t title_of;
	UsrInit_t user_init;
	UsrUser_t fg_user;
	UsrUser_t initial_user;
} Lnc;

static Lnc g_lnc;
static char g_state[480];
static char g_log_path[520];
static long long g_t0_ms;
static long long g_gone_at; // when the app's process was first seen gone (0: never seen)

// ---------------------------------------------------------------------------------------------------------------------------- platform layer
// (the console's version here; the PC test's is relaunch_stub.c)
#ifndef RELAUNCH_HOST_TEST
typedef intptr_t evf_t;
int sceKernelOpenEventFlag(evf_t* ef, const char* name);
int sceKernelPollEventFlag(evf_t ef, uint64_t bit_pattern, unsigned int wait_mode, uint64_t* result_pattern);
int sceKernelCloseEventFlag(evf_t ef);

// The libraries are opened and the symbols looked up by name, like the disc helper does it (daemon/disc_launch.c load_lnc): no hard NEEDED entry.
static void* rl_lib(const char* name)
{
	return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

static void* rl_sym(void* lib, const char* name)
{
	return lib ? dlsym(lib, name) : NULL;
}

// 1 gone (no such process, or a zombie), 0 still there, -1 can't tell. The PS5's kinfo_proc is bigger than the SDK's: read into a larger buffer
// (the disc helper's proc_is_app does the same).
static int rl_pid_state(int pid)
{
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
	size_t len = 0;
	if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0)
		return (errno == ESRCH || errno == ENOENT) ? 1 : -1;
	if (len == 0)
		return 1;
	char* buf = (char*)calloc(1, len + 4096);
	if (!buf)
		return -1;
	len += 4096;
	int state = -1;
	if (sysctl(mib, 4, buf, &len, NULL, 0) == 0)
	{
		if (len == 0)
			state = 1; // nothing came back: no such process
		else if (len < offsetof(struct kinfo_proc, ki_stat) + sizeof(((struct kinfo_proc*)0)->ki_stat))
			state = -1; // not even a header: can't tell
		else
		{
			const struct kinfo_proc* kp = (const struct kinfo_proc*)buf;
			state = (kp->ki_pid != pid || kp->ki_stat == 5 /* SZOMB */) ? 1 : 0;
		}
	}
	else if (errno == ESRCH || errno == ENOENT)
		state = 1;
	free(buf);
	return state;
}

// fix180: 1 when an app process (eboot.bin) other than `old_pid` runs, 0 when none, -1 when the process table can't be believed. The PS5's kinfo_proc is
// bigger than the SDK's: the records are stepped by their own ki_structsize (the disc helper's app_processes does the same, console-proven).
static int rl_other_app(int old_pid)
{
	int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC};
	size_t len = 0;
	if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0 || len == 0)
		return -1;
	len += len / 4 + 8192;
	char* buf = (char*)malloc(len);
	if (!buf)
		return -1;
	if (sysctl(mib, 3, buf, &len, NULL, 0) != 0)
	{
		free(buf);
		return -1;
	}
	int found = 0, total = 0, saw_self = 0;
	size_t off = 0;
	while (off + sizeof(int) <= len)
	{
		const struct kinfo_proc* kp = (const struct kinfo_proc*)(buf + off);
		const size_t sz = kp->ki_structsize > 0 ? (size_t)kp->ki_structsize : 0;
		if (sz < offsetof(struct kinfo_proc, ki_comm) + sizeof(kp->ki_comm) || off + sz > len)
			break;
		total++;
		if (kp->ki_pid == getpid())
			saw_self = 1;
		if (kp->ki_pid != old_pid && kp->ki_stat != 5 /* SZOMB */ && strcmp(kp->ki_comm, "eboot.bin") == 0)
			found = 1;
		off += sz;
	}
	free(buf);
	if (!saw_self || total < 10)
		return -1;
	return found;
}

// 1 when the console is shutting down, going to rest mode or suspending (SceSystemStateMgrInfo: 100, 300, 500; SceSystemStateMgrStatus bit 0x200000,
// the way the helper's kp.c reads them). Fail-open: a flag that can't be opened or polled counts as "not shutting down".
static int rl_shutting_down(void)
{
	int quiet = 0;
	evf_t info = -1, status = -1;
	if (sceKernelOpenEventFlag(&info, "SceSystemStateMgrInfo") >= 0)
	{
		uint64_t pat = 0;
		if (sceKernelPollEventFlag(info, UINT64_MAX, 2u /* OR */, &pat) == 0)
		{
			const unsigned st = (unsigned)(pat & 0xffffu);
			quiet = st == 100u || st == 300u || st == 500u;
		}
		sceKernelCloseEventFlag(info);
	}
	if (sceKernelOpenEventFlag(&status, "SceSystemStateMgrStatus") >= 0)
	{
		uint64_t pat = 0;
		if (sceKernelPollEventFlag(status, UINT64_MAX, 2u /* OR */, &pat) == 0 && (pat & 0x0000000000200000ULL))
			quiet = 1;
		sceKernelCloseEventFlag(status);
	}
	return quiet;
}
#else
void* rl_lib(const char* name);
void* rl_sym(void* lib, const char* name);
int rl_pid_state(int pid);
int rl_shutting_down(void);
int rl_other_app(int old_pid);
#endif

static void rl_load(Lnc* l)
{
	void* h = rl_lib("libSceSystemService.sprx");
	l->launch = (LaunchApp_t)rl_sym(h, "sceLncUtilLaunchApp");
	l->app_id = (GetAppId_t)rl_sym(h, "sceLncUtilGetAppId");
	l->big_app = (GetBigApp_t)rl_sym(h, "sceLncUtilGetAppIdOfRunningBigApp");
	l->title_of = (GetTitle_t)rl_sym(h, "sceLncUtilGetAppTitleId");
	void* u = rl_lib("libSceUserService.sprx");
	l->user_init = (UsrInit_t)rl_sym(u, "sceUserServiceInitialize");
	l->fg_user = (UsrUser_t)rl_sym(u, "sceUserServiceGetForegroundUser");
	l->initial_user = (UsrUser_t)rl_sym(u, "sceUserServiceGetInitialUser");
}

// ---------------------------------------------------------------------------------------------------------------------------- helpers
static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// One line to relaunch.log (opened and closed for each, so a crash leaves what happened) and to the kernel log.
static void note(const char* fmt, ...)
{
	char line[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (g_log_path[0])
	{
		FILE* f = fopen(g_log_path, "a");
		if (f)
		{
			fprintf(f, "%ld +%lld ms %s\n", (long)time(NULL), now_ms() - g_t0_ms, line);
			fclose(f);
		}
	}
#ifndef RELAUNCH_HOST_TEST
	klog_printf("[relaunch] %s\n", line);
#endif
}

// "key=value" lines of the request: the value of `key` up to the end of its line (copied into out), or "".
static void field(const char* req, const char* key, char* out, size_t room)
{
	out[0] = '\0';
	char needle[32];
	snprintf(needle, sizeof(needle), "\n%s=", key);
	const char* at = strstr(req, needle);
	if (!at)
		return;
	at += strlen(needle);
	size_t n = 0;
	while (at[n] && at[n] != '\n' && n + 1 < room)
	{
		out[n] = at[n];
		n++;
	}
	out[n] = '\0';
}

static void write_file(const char* path, const char* text)
{
	char tmp[540];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* f = fopen(tmp, "w");
	if (!f)
		return;
	fputs(text, f);
	fclose(f);
	if (rename(tmp, path) != 0)
		unlink(tmp);
}

static int exists_with(const char* suffix)
{
	char p[540];
	struct stat st;
	snprintf(p, sizeof(p), "%s%s", g_state, suffix);
	return stat(p, &st) == 0;
}

static void done(const char* status, const char* detail)
{
	char p[540], text[300];
	snprintf(p, sizeof(p), "%s.done", g_state);
	snprintf(text, sizeof(text), "status %s\n%s\n", status, detail ? detail : "");
	write_file(p, text);
	note("done: %s%s%s", status, detail && detail[0] ? ": " : "", detail ? detail : "");
}

// The end: _exit() stops every thread before anything is unloaded (as notify_relay.c and ime_relay.c end).
__attribute__((noreturn)) static void finish(const char* status, const char* detail)
{
	done(status, detail);
	_exit(0);
}

static int cancelled(void)
{
	return exists_with(".cancel");
}

// Sleeps `ms` in steps of 100 ms; false when the request was cancelled meanwhile.
static int nap(int ms)
{
	while (ms > 0)
	{
		if (cancelled())
			return 0;
		const int step = ms < 100 ? ms : 100;
		usleep((useconds_t)step * 1000);
		ms -= step;
	}
	return !cancelled();
}

__attribute__((noreturn)) static void cancel_exit(void)
{
	finish("cancelled", "the app took the request back");
}

typedef struct
{
	int pid;
	char wait[40];    // the closing title, "" if unknown
	char launch[40];  // the title to start
	int settle_ms;
	int limit_s;
	int nosig_ms;
	int cap_ms;       // how long after its process went the shell may go on listing the closing app before it is taken as gone anyway
	int min_exit_ms;  // not before this long after the process was seen gone (0: no such rule)
	int retry_ms;     // pause between two launch attempts
	int guard_ms;     // fix180: guard mode (the app leaves by its own exec): 0 = off
	int hard_ms;      // fix180: guard mode, with another app process there
} Request;

// " (N ms after the process exit)" for a note, "" when the process was never seen gone.
static const char* since_exit(char* buf, size_t room)
{
	buf[0] = '\0';
	if (g_gone_at)
		snprintf(buf, room, " (%lld ms after the process exit)", now_ms() - g_gone_at);
	return buf;
}

// 1 gone, 0 still there, from what the shell says: the closing title's app id, or, with no title, the id of the running big app taken at the start.
static int id_gone(const Request* r, uint32_t big_start, int use_title, int use_big)
{
	if (use_title)
		return g_lnc.app_id(r->wait) <= 0;
	if (use_big)
		return g_lnc.big_app() != big_start;
	return 1; // nothing to ask: the process being gone is all there is
}

// How many launches one round makes: 5 at the 800 ms pause of 175 (a round tried for about 4 s), and as many more as a shorter pause needs to keep that 4 s. A
// pause of 400 ms with 5 launches would give up after 2 s, half the time the shell had in 175 to become ready for a launch.
static int attempts_per_round(int retry_ms)
{
	const int n = (4000 + retry_ms - 1) / retry_ms;
	return n < 5 ? 5 : n;
}

// One round of launches retry_ms apart (attempts_per_round of them). 1 on success (rc >= 0, or 0x8094000c: the shell says it is running already), 0 when none took.
static int launch_round(const Request* r, int round, int* attempts_total, unsigned* last_rc)
{
	int user = -1;
	if (g_lnc.fg_user)
	{
		const int rc = g_lnc.fg_user(&user);
		if (rc != 0)
			user = -1;
	}
	if (user < 0 && g_lnc.initial_user)
	{
		const int rc = g_lnc.initial_user(&user);
		if (rc != 0)
			user = -1;
	}
	lnc_app_param_t prm;
	const int attempts = attempts_per_round(r->retry_ms);
	for (int attempt = 1; attempt <= attempts; attempt++)
	{
		if (cancelled())
			cancel_exit();
		memset(&prm, 0, sizeof(prm));
		prm.size = sizeof(prm);
		prm.user_id = user;
		char since[64];
		since_exit(since, sizeof(since)); // taken before the call: the moment it is issued
		const int rc = g_lnc.launch(r->launch, NULL, &prm);
		(*attempts_total)++;
		*last_rc = (unsigned)rc;
		note("round %d, attempt %d: sceLncUtilLaunchApp(%s, user %d) = 0x%08x, issued%s", round, attempt, r->launch, user, (unsigned)rc, since);
		if (rc >= 0 || (unsigned)rc == 0x8094000cu)
			return 1;
		if (!nap(r->retry_ms))
			cancel_exit();
	}
	return 0;
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	// stdout and stderr go to the loader's connection, which the app closes when it dies: nothing is written there.
	const int nul = open("/dev/null", O_WRONLY);
	if (nul >= 0)
	{
		dup2(nul, 1);
		dup2(nul, 2);
	}
	const char* req = (const char*)g_request;
	if (strncmp(req, RELAUNCH_MARKER "\n", sizeof(RELAUNCH_MARKER)) != 0)
		return 1; // sent without a request: nothing to do
	Request r;
	memset(&r, 0, sizeof(r));
	char pid_text[24], settle_text[24], limit_text[24], nosig_text[24], cap_text[24], min_exit_text[24], retry_text[24], guard_text[24], hard_text[24];
	field(req, "pid", pid_text, sizeof(pid_text));
	field(req, "wait", r.wait, sizeof(r.wait));
	field(req, "launch", r.launch, sizeof(r.launch));
	field(req, "state", g_state, sizeof(g_state));
	field(req, "settle_ms", settle_text, sizeof(settle_text));
	field(req, "limit_s", limit_text, sizeof(limit_text));
	field(req, "nosig_ms", nosig_text, sizeof(nosig_text));
	field(req, "cap_ms", cap_text, sizeof(cap_text));
	field(req, "min_exit_ms", min_exit_text, sizeof(min_exit_text));
	field(req, "retry_ms", retry_text, sizeof(retry_text));
	field(req, "guard_ms", guard_text, sizeof(guard_text));
	field(req, "hard_ms", hard_text, sizeof(hard_text));
	r.guard_ms = rg_clamp_guard(guard_text[0] ? atoi(guard_text) : 0);
	r.hard_ms = r.guard_ms ? rg_clamp_hard(hard_text[0] ? atoi(hard_text) : 0, r.guard_ms) : 0;
	r.min_exit_ms = min_exit_text[0] ? atoi(min_exit_text) : 0;
	if (r.min_exit_ms < 0)
		r.min_exit_ms = 0;
	if (r.min_exit_ms > 10000)
		r.min_exit_ms = 10000;
	r.retry_ms = retry_text[0] ? atoi(retry_text) : 800;
	if (r.retry_ms < 100)
		r.retry_ms = 100;
	if (r.retry_ms > 2000)
		r.retry_ms = 2000;
	r.cap_ms = cap_text[0] ? atoi(cap_text) : 8000;
	if (r.cap_ms < 1000)
		r.cap_ms = 1000;
	if (r.cap_ms > 30000)
		r.cap_ms = 30000;
	r.pid = atoi(pid_text);
	r.settle_ms = settle_text[0] ? atoi(settle_text) : 1500;
	r.limit_s = limit_text[0] ? atoi(limit_text) : 45;
	r.nosig_ms = nosig_text[0] ? atoi(nosig_text) : 3500;
	if (r.settle_ms < 0)
		r.settle_ms = 0;
	if (r.settle_ms > 10000)
		r.settle_ms = 10000;
	if (r.limit_s < 5)
		r.limit_s = 5;
	if (r.limit_s > 120)
		r.limit_s = 120;
	if (r.nosig_ms < 0)
		r.nosig_ms = 0;
	if (r.nosig_ms > 10000)
		r.nosig_ms = 10000;
	if (r.pid <= 1 || !r.launch[0] || g_state[0] != '/')
		return 1; // a request without its fields: nothing is written (there is nowhere known to write it)
	g_t0_ms = now_ms();
	{
		// the log lies beside the state files; one that has grown past 64 KiB starts again
		snprintf(g_log_path, sizeof(g_log_path), "%s", g_state);
		char* slash = strrchr(g_log_path, '/');
		if (slash)
			snprintf(slash, sizeof(g_log_path) - (size_t)(slash - g_log_path), "/relaunch.log");
		struct stat st;
		if (stat(g_log_path, &st) == 0 && st.st_size > 65536)
			unlink(g_log_path);
	}
	note("request: pid %d, closing title '%s', launch %s, settle %d ms, not before %d ms after the process exit, retry every %d ms, limit %d s", r.pid, r.wait,
		r.launch, r.settle_ms, r.min_exit_ms, r.retry_ms, r.limit_s);
	if (r.guard_ms)
		note("guard mode: the app leaves by its own exec; %s is launched only if no new process says it is up %d ms after the old one went (%d ms while another "
			 "app process is there)", r.launch, r.guard_ms, r.hard_ms);

	rl_load(&g_lnc);
	note("symbols: launch %p, app_id %p, big_app %p, title_of %p, user_init %p, fg_user %p, initial_user %p", (void*)g_lnc.launch, (void*)g_lnc.app_id,
		(void*)g_lnc.big_app, (void*)g_lnc.title_of, (void*)g_lnc.user_init, (void*)g_lnc.fg_user, (void*)g_lnc.initial_user);
	// No launch call, no handshake: the app then does not commit to the plain close (it would end at the home screen with nothing to start it) and keeps
	// its own way out. "<state>.done" says why, for an app that looks.
	if (!g_lnc.launch)
		finish("nolaunch", "sceLncUtilLaunchApp was not found: this payload cannot start anything, no handshake was given");
	{
		char p[540];
		snprintf(p, sizeof(p), "%s.started", g_state);
		write_file(p, "1\n"); // the handshake: the payload is alive and can launch; the app waits for this before it commits to leaving
	}
	if (g_lnc.user_init)
	{
		const int rc = g_lnc.user_init(NULL);
		note("sceUserServiceInitialize = 0x%08x", (unsigned)rc);
	}

	// What tells that the shell is done with the closing app: its title's app id, else the running big app's id taken now (while it still runs).
	uint32_t big_start = 0;
	if (g_lnc.big_app)
	{
		big_start = g_lnc.big_app();
		if (big_start == 0 || big_start == 0xffffffffu)
			big_start = 0;
		char t[32] = {0};
		if (big_start && g_lnc.title_of && g_lnc.title_of(big_start, t) == 0)
			note("the running big app: id 0x%x, title %s", (unsigned)big_start, t);
		else
			note("the running big app: id 0x%x", (unsigned)big_start);
	}
	const int use_title = r.wait[0] && g_lnc.app_id;
	const int use_big = !use_title && big_start != 0;
	const int have_signal = use_title || use_big;
	note("waiting for %s", use_title ? "the title's app id to go" : use_big ? "the big app's id to go" : "the process alone (nothing else tells; a longer settle follows)");

	const long long deadline = now_ms() + (long long)r.limit_s * 1000;
	// fix180, guard mode: the old process goes by its own exec; a new one that comes up says so ("<state>.alive"); only when none does, the launch below.
	if (r.guard_ms)
	{
		int said = 0;
		for (;;)
		{
			if (cancelled())
				cancel_exit();
			if (exists_with(".alive"))
				finish("came-back", "the app's own exec brought it back: nothing launched");
			const int ps = rl_pid_state(r.pid);
			if (ps == 1)
			{
				g_gone_at = now_ms();
				note("guard: the process %d is gone", r.pid);
				break;
			}
			if (ps < 0 && !said)
			{
				said = 1;
				note("guard: the process %d can't be looked at: counted from now", r.pid);
				g_gone_at = now_ms();
				break;
			}
			if (now_ms() >= deadline)
				finish("timeout", "guard: the app was still there; nothing started");
			usleep(100000);
		}
		int said_other = 0;
		for (;;)
		{
			if (cancelled())
				cancel_exit();
			const int alive = exists_with(".alive");
			const int other = alive ? 0 : rl_other_app(r.pid);
			const long long since = now_ms() - g_gone_at;
			const int step = rg_step(alive, other, since, r.guard_ms, r.hard_ms);
			if (step == RG_CAME_BACK)
			{
				char detail[96];
				snprintf(detail, sizeof(detail), "the app's own exec brought it back %lld ms after the old process went: nothing launched", since);
				finish("came-back", detail);
			}
			if (step == RG_LAUNCH)
			{
				note("guard: no new process said it is up %lld ms after the old one went (another app process: %s): %s is launched", since,
					other == 1 ? "yes" : other == 0 ? "no" : "can't tell", r.launch);
				break;
			}
			if (other == 1 && !said_other)
			{
				said_other = 1;
				note("guard: another app process is there %lld ms after the old one went: waiting for it to say it is up", since);
			}
			usleep(100000);
		}
	}

	// Phase 1: until the app is gone (the cancel note first in every round).
	int said_unknown = 0;
	for (;;)
	{
		if (cancelled())
			cancel_exit();
		const int ps = rl_pid_state(r.pid);
		if (ps < 0 && !said_unknown)
		{
			said_unknown = 1;
			note("the process %d can't be looked at: only the shell's word counts", r.pid);
		}
		if (ps == 1 && !g_gone_at)
		{
			g_gone_at = now_ms();
			note("the process %d is gone", r.pid);
		}
		if ((ps == 1 || (ps < 0 && have_signal)) && id_gone(&r, big_start, use_title, use_big))
			break;
		if (ps == 1 && now_ms() - g_gone_at >= r.cap_ms)
		{
			// The process is gone, so the app is not alive; a shell that goes on listing its title for this long must not keep the shelf away.
			note("the shell still lists the closing app %d ms after its process went: going on", r.cap_ms);
			break;
		}
		if (now_ms() >= deadline)
			finish("timeout", "the app was still there; nothing started");
		usleep(100000);
	}
	note("the app is gone");

	// Phase 2: the shell's clean-up of the killed app takes about 2.5 s; still polling the cancel note.
	const int settle = have_signal ? r.settle_ms : (r.nosig_ms > r.settle_ms ? r.nosig_ms : r.settle_ms);
	if (!nap(settle))
		cancel_exit();
	note("settled %d ms", settle);
	// Not before min_exit_ms after the process went (when it was seen going): the settle runs from the shell forgetting the title, which on a console that
	// forgets it at once is too early for ShadowMount+ (it finalizes a stopped app about a second after its exit).
	if (r.min_exit_ms > 0 && g_gone_at)
	{
		const long long left = g_gone_at + r.min_exit_ms - now_ms();
		if (left > 0)
		{
			note("not before %d ms after the process exit: %lld ms more", r.min_exit_ms, left);
			if (!nap((int)left))
				cancel_exit();
		}
	}

	// Phase 3: launch (twice at most), unless the console is on its way down.
	int attempts = 0;
	unsigned last_rc = 0xffffffffu;
	int ok = 0;
	for (int round = 1; round <= 2; round++)
	{
		if (cancelled())
			cancel_exit();
		if (rl_shutting_down())
			finish("shutdown", "the console is shutting down or going to rest mode: nothing started");
		if (!launch_round(&r, round, &attempts, &last_rc))
		{
			ok = 0;
			continue; // none of the round took: once more
		}
		ok = 1;
		if (!g_lnc.app_id)
			break; // can't tell whether it came up: the shell accepted the launch
		// up to 8 s for the title to show up in the shell
		int up = 0;
		for (int i = 0; i < 80 && !up; i++)
		{
			if (g_lnc.app_id(r.launch) > 0)
				up = 1;
			else if (!nap(100))
				cancel_exit();
		}
		char since[64];
		note("round %d: %s is %s%s", round, r.launch, up ? "running" : "not running after 8 s", since_exit(since, sizeof(since)));
		if (up)
			break;
		ok = 0;
	}
	char detail[160];
	snprintf(detail, sizeof(detail), "sceLncUtilLaunchApp(%s) rc 0x%08x after %d attempt(s)", r.launch, last_rc, attempts);
	if (ok)
		finish(g_lnc.app_id ? "launched" : "launched-unchecked", detail);
	finish("failed", detail);
}
