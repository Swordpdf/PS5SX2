/* Elevate the PS5SX2 title so it can open /dev/cd and /dev/pass.
 *
 * The signed eboot asks the Lapy owned-root daemon by writing /download0/elevate_proc
 * and, if nobody answers, the etaHEN ports 9028 and 9069. This console has kstuff and
 * neither of those. Without the decid credential, open("/dev/cd1") returns EACCES and
 * the title falls back to driving the USB pipe itself, which races the kernel driver.
 *
 * This payload is the answerer. It stays up: it elevates whatever pid the marker names,
 * and also the pid written in /data/PCSX2/logs/boot.log, so a title that already gave up
 * waiting still gets the credential before its next disc scan. Send it to the ELF loader
 * (port 9021) once after each boot, before or while PS5SX2 is open. It does not exit.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#define AUTHID 0x4800000000010003ull
#define MARKER "/download0/elevate_proc"
#define BOOTLOG "/data/PCSX2/logs/boot.log"
#define OUTLOG "/data/PCSX2/logs/elevate.txt"

typedef struct { char unused[45]; char message[3075]; } notify_req_t;
int sceKernelSendNotificationRequest(int32_t device, void *req, size_t size, int32_t blocking);

static FILE *g_out;

static void say(const char *fmt, ...) {
	char buf[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	klog_printf("[elevate] %s\n", buf);
	if (g_out) {
		fprintf(g_out, "%s\n", buf);
		fflush(g_out);
	}
}

static void notify(const char *msg) {
	static notify_req_t req;
	memset(&req, 0, sizeof(req));
	snprintf(req.message, sizeof(req.message), "%s", msg);
	sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void elevate_self(void) {
	uint8_t all[16];
	memset(all, 0xff, sizeof(all));
	kernel_set_ucred_authid(getpid(), AUTHID);
	kernel_set_ucred_caps(getpid(), all);
}

/* The title is already uid 0, so it never asks Lapy for the step that sets its
 * root to the system root. Its /dev is still the sandbox, which has no cd0.
 * This process can open /dev/cd0; give the title that same root, jail and prison. */
static void share_view(pid_t pid) {
	const pid_t self = getpid();
	const intptr_t root = kernel_get_proc_rootdir(self);
	const intptr_t jail = kernel_get_proc_jaildir(self);
	intptr_t prison = kernel_get_ucred_prison(self);
	if (!prison)
		prison = KERNEL_ADDRESS_PRISON0;
	const intptr_t old_root = kernel_get_proc_rootdir(pid);
	const intptr_t old_jail = kernel_get_proc_jaildir(pid);
	const intptr_t old_prison = kernel_get_ucred_prison(pid);
	const int32_t rr = root ? kernel_set_proc_rootdir(pid, root) : -1;
	const int32_t rj = jail ? kernel_set_proc_jaildir(pid, jail) : -1;
	const int rp = prison ? kernel_set_ucred_prison(pid, prison) : -1;
	say("pid %d view: root %llx -> %llx (%d), jail %llx -> %llx (%d), prison %llx -> %llx (%d)",
		(int)pid,
		(unsigned long long)old_root, (unsigned long long)kernel_get_proc_rootdir(pid), rr,
		(unsigned long long)old_jail, (unsigned long long)kernel_get_proc_jaildir(pid), rj,
		(unsigned long long)old_prison, (unsigned long long)kernel_get_ucred_prison(pid), rp);
	if (root && kernel_get_proc_rootdir(pid) == root)
		notify("PS5SX2 disk gorunumu verildi");
}

/* uid 0 plus the decid authid and full caps, same grant the title asks the jailbreak
 * ports for. Then the filesystem view of this process, which can open /dev/cd0. */
static int elevate_pid(pid_t pid, const char *why) {
	if (pid <= 0 || !kernel_get_proc(pid)) {
		say("pid %d is not a process (%s)", (int)pid, why);
		return 0;
	}
	const uint64_t before = kernel_get_ucred_authid(pid);
	uint8_t all[16];
	memset(all, 0xff, sizeof(all));
	kernel_set_ucred_uid(pid, 0);
	kernel_set_ucred_ruid(pid, 0);
	kernel_set_ucred_svuid(pid, 0);
	kernel_set_ucred_rgid(pid, 0);
	kernel_set_ucred_svgid(pid, 0);
	kernel_set_ucred_authid(pid, AUTHID);
	kernel_set_ucred_caps(pid, all);
	const uint64_t after = kernel_get_ucred_authid(pid);
	say("pid %d (%s): authid %llx -> %llx", (int)pid, why,
		(unsigned long long)before, (unsigned long long)after);
	if (after == AUTHID)
		share_view(pid);
	return after == AUTHID;
}

static pid_t pid_from_marker(void) {
	const int fd = open(MARKER, O_RDONLY);
	if (fd < 0)
		return -1;
	char body[64];
	const ssize_t n = read(fd, body, sizeof(body) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	body[n] = 0;
	const char *p = strstr(body, "\"PID\"");
	if (!p)
		return -1;
	p = strchr(p, ':');
	if (!p)
		return -1;
	return (pid_t)atoi(p + 1);
}

/* Same open("/dev/cd0") the disc-auto payload uses. This is this process, not the
 * title: a successful open does not add the node to the title's /dev. */
static void probe_cd0(void) {
	static int last_err = -1;
	static long long last_size = -1;
	char nodes[240];
	size_t used = 0;
	nodes[0] = 0;
	DIR *dir = opendir("/dev");
	if (!dir) {
		say("/dev: opendir errno %d", errno);
	} else {
		struct dirent *ent;
		while ((ent = readdir(dir))) {
			if (strncmp(ent->d_name, "cd", 2) != 0 && strncmp(ent->d_name, "pass", 4) != 0)
				continue;
			const size_t n = strlen(ent->d_name);
			if (used + n + 2 >= sizeof(nodes))
				break;
			if (used)
				nodes[used++] = ' ';
			memcpy(nodes + used, ent->d_name, n);
			used += n;
			nodes[used] = 0;
		}
		closedir(dir);
	}

	const int fd = open("/dev/cd0", O_RDONLY);
	if (fd < 0) {
		if (errno != last_err || last_size != 0) {
			last_err = errno;
			last_size = 0;
			say("/dev/cd0: open errno %d; nodes: %s", errno, nodes[0] ? nodes : "none");
			char msg[80];
			snprintf(msg, sizeof(msg), "cd0 acilamadi errno %d", errno);
			notify(msg);
		}
		return;
	}
	off_t size = 0;
	const int rc = ioctl(fd, DIOCGMEDIASIZE, &size);
	const int ioerr = errno;
	close(fd);
	if (rc != 0)
		size = 0;
	if (last_err == 0 && last_size == (long long)size)
		return;
	last_err = 0;
	last_size = (long long)size;
	if (rc != 0)
		say("/dev/cd0: open ok, size ioctl errno %d; nodes: %s", ioerr, nodes[0] ? nodes : "none");
	else
		say("/dev/cd0: open ok, %lld bytes; nodes: %s", (long long)size, nodes[0] ? nodes : "none");
	char msg[96];
	if (size > 0)
		snprintf(msg, sizeof(msg), "cd0 acildi, %lld bayt", (long long)size);
	else
		snprintf(msg, sizeof(msg), "cd0 acildi, disk yok");
	notify(msg);
}

static pid_t pid_from_bootlog(void) {
	FILE *f = fopen(BOOTLOG, "r");
	if (!f)
		return -1;
	char line[256];
	pid_t pid = -1;
	while (fgets(line, sizeof(line), f)) {
		const char *p = strstr(line, "[boot] pid=");
		if (p)
			pid = (pid_t)atoi(p + 11);
	}
	fclose(f);
	return pid;
}

int main(void) {
	elevate_self();
	g_out = fopen(OUTLOG, "a");
	say("elevate payload up, pid %d", (int)getpid());
	notify("PS5SX2: yetki payload'u acik");
	probe_cd0();

	pid_t done = -1;
	unsigned tick = 0;
	for (;;) {
		const pid_t asked = pid_from_marker();
		if (asked > 0) {
			if (elevate_pid(asked, "elevate_proc")) {
				unlink(MARKER);
				if (asked != done) {
					notify("PS5SX2 yetkisi yukseldi");
					done = asked;
				}
			}
		}
		const pid_t booted = pid_from_bootlog();
		if (booted > 0 && booted != done && kernel_get_proc(booted)) {
			if (elevate_pid(booted, "boot.log")) {
				notify("PS5SX2 yetkisi yukseldi");
				done = booted;
			}
		}
		if (++tick >= 15) {
			tick = 0;
			probe_cd0();
		}
		usleep(200 * 1000);
	}
}
