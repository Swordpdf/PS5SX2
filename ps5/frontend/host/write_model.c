// PS5 port frontend, host tests (2026-10-29, AI-assisted): the console's write limits for a TITLE, played on a PC by LD_PRELOAD.
//
// Measured on the console (PS5SX3's notes, helper/README.md "The write service"): a title's writes go at full speed for a burst of ~1.3 GiB and
// then at ~2 MiB/s (the allowance refills only while the title does not write), and each write() costs ~3.3 ms whatever its size. Another process
// is not held back. This library makes write(), pwrite() and pwrite64() on regular files of the process it is loaded into follow that model (the
// sleeps are added after the real write), counts the calls and the bytes, and prints them at exit. The texture pack writer payload is started
// without LD_PRELOAD (texpacks_test.cpp HostLaunch), as a process of its own is on the console.
//   WM_BURST_MB  the full-speed burst in MiB (default 1331 = 1.3 GiB)     WM_RATE_MBS  MiB/s after it (default 2)
//   WM_CALL_US   the cost of each call in microseconds (default 3300)     WM_REPORT    a file the counts are appended to (default stderr)
// Build: clang -shared -fPIC -O1 -o write_model.so write_model.c -ldl
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static ssize_t (*real_write)(int, const void*, size_t);
static ssize_t (*real_pwrite)(int, const void*, size_t, off_t);
static ssize_t (*real_pwrite64)(int, const void*, size_t, off_t);
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static double g_burst, g_rate, g_call_us;
static double g_spent;       // bytes past the burst so far
static double g_free_at;     // when the storage has written them (monotonic seconds)
static uint64_t g_calls, g_bytes;
static double g_slept;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

__attribute__((constructor)) static void init(void)
{
	real_write = (ssize_t(*)(int, const void*, size_t))dlsym(RTLD_NEXT, "write");
	real_pwrite = (ssize_t(*)(int, const void*, size_t, off_t))dlsym(RTLD_NEXT, "pwrite");
	real_pwrite64 = (ssize_t(*)(int, const void*, size_t, off_t))dlsym(RTLD_NEXT, "pwrite64");
	const char* b = getenv("WM_BURST_MB");
	const char* r = getenv("WM_RATE_MBS");
	const char* c = getenv("WM_CALL_US");
	g_burst = (b ? atof(b) : 1331.0) * 1048576.0;
	g_rate = (r ? atof(r) : 2.0) * 1048576.0;
	g_call_us = c ? atof(c) : 3300.0;
}

__attribute__((destructor)) static void report(void)
{
	FILE* f = getenv("WM_REPORT") ? fopen(getenv("WM_REPORT"), "a") : stderr;
	if (!f)
		return;
	fprintf(f, "write model: pid %d: %llu write calls to files, %.1f MiB, %.1f s added by the model\n", (int)getpid(), (unsigned long long)g_calls,
		g_bytes / 1048576.0, g_slept);
	if (f != stderr)
		fclose(f);
}

static void model(int fd, ssize_t n)
{
	if (n <= 0)
		return;
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
		return;
	double wait;
	pthread_mutex_lock(&g_lock);
	g_calls++;
	g_bytes += (uint64_t)n;
	const double t = now();
	if (g_burst > 0)
	{
		const double take = (double)n < g_burst ? (double)n : g_burst;
		g_burst -= take;
		n -= (ssize_t)take;
	}
	if (n > 0)
	{
		// the storage takes the bytes past the burst at g_rate, one after another (whichever thread wrote them)
		if (g_free_at < t)
			g_free_at = t;
		g_free_at += (double)n / g_rate;
		g_spent += (double)n;
	}
	wait = (g_free_at > t ? g_free_at - t : 0) + g_call_us / 1e6;
	g_slept += wait;
	pthread_mutex_unlock(&g_lock);
	struct timespec ts = {(time_t)wait, (long)((wait - (time_t)wait) * 1e9)};
	nanosleep(&ts, NULL);
}

ssize_t write(int fd, const void* buf, size_t n)
{
	const ssize_t r = real_write(fd, buf, n);
	model(fd, r);
	return r;
}

ssize_t pwrite(int fd, const void* buf, size_t n, off_t off)
{
	const ssize_t r = real_pwrite(fd, buf, n, off);
	model(fd, r);
	return r;
}

ssize_t pwrite64(int fd, const void* buf, size_t n, off_t off)
{
	const ssize_t r = real_pwrite64(fd, buf, n, off);
	model(fd, r);
	return r;
}
