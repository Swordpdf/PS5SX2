// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// PS5 port (vk-285-104): the GS thread's log lines, kept in memory and written out by the ticker thread
// (main-boot.cpp) once a second, so the GS thread never writes to /data or waits for stdout's lock. A write
// there costs ~0.7 ms and sometimes 30+ ms: vk-285-101/102's GS thread stalled ~34 ms every few seconds in a
// merge or present printf/fflush (the 85-88% seconds in Shadow of the Colossus's heavy view). vk-285-102/103
// tried to move the file writes to a logger thread behind a pipe (apps may not call pipe()) and then a loopback
// connection (no line arrived); this leaves stdout as it is and only defers what the GS-side files print.
//
// A file opts in after its includes:
//   #include "OrbisDeferredLog.h"
//   #define printf OrbisDeferredPrintf
//   #define fflush OrbisDeferredFlush
// OrbisDeferredFlush(stdout) does nothing (the ticker flushes); other streams are flushed as before. The crash
// printer writes the buffer out first (orbis_log_drain). At most 1 MB waits; beyond that lines are dropped and
// counted. Needs proper testing.
//
// vk-285-107: vk-285-106's [vsslow] lines found the remaining ~33 ms stalls (one every ~6 s at the spot): the GS
// thread asleep in a direct write -- GSRendererHW's [gsout] printf+fflush every 200 frames, the profiler's
// once-a-second line, and the Vulkan driver's own stderr lines (from the GS thread, its recorder or its queue
// worker, whose waits the GS thread then shares). So there is a second buffer for stderr: OrbisDeferredFprintf
// takes either stream, and the driver hands its lines over through ps5vk_log_hook (below).

#include <cstddef>
#include <cstdio>

int OrbisDeferredPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// stdout or stderr: into that stream's buffer; any other stream: written as before.
int OrbisDeferredFprintf(FILE* stream, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int OrbisDeferredFlush(FILE* stream);
extern "C" void orbis_log_drain(); // both buffers out, and both streams flushed (the ticker; the crash printer)
// vk-285-107: the Vulkan driver's lines for stdout (stream 1) and stderr (2) (ps5vk_private.h's log routing calls
// it; weak there).
extern "C" void ps5vk_log_hook(int stream, const char* text, size_t len);
// vk-285-107: a line for the settings log (the frontend's orbis_event_log opens, appends and closes the file),
// written by the ticker thread with the next drain. For lines from the emulation threads (the GS thread's
// once-a-minute perf line).
void OrbisDeferredEvent(const char* line);
