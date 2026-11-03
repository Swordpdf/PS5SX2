// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVDdiscReader.h"
#include "CDVD/CDVD.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <queue>
#include <thread>

const u32 sectors_per_read = 16;

static_assert(sectors_per_read > 1 && !(sectors_per_read & (sectors_per_read - 1)),
			  "sectors_per_read must by a power of 2");

struct SectorInfo
{
	u32 lsn;
	// Sectors are read in blocks, not individually
	u8 data[2352 * sectors_per_read];
};

u32 g_last_sector_block_lsn;

static std::thread s_thread;

static std::mutex s_notify_lock;
static std::condition_variable s_notify_cv;
static std::mutex s_request_lock;
static std::queue<u32> s_request_queue;
static std::mutex s_cache_lock;

static std::atomic<bool> cdvd_is_open;

// ---------------------------------------------------------------------------------------------------------------------------------------
// PS5SX2 (2.11, AI-assisted; plan "disclag"): a PS2 game played from a PS5 drive or a USB DVD drive must not freeze the emulator on every seek.
//
// Before: on a cache miss cdvdGetSector read the 16-sector block itself, on the emulation's CPU thread (the thread that also runs the IOP, so the
// whole emulator stood still for the read: no frames, no audio mixing), while this thread read for the same head; two threads, one head, a miss
// could cost two seeks (215-265 ms each on the PS5's drive). Now the CPU thread waits for this thread's read of the block (same wait, no head
// fight); only when this thread does not deliver in 3 s, or is not running, or the disc went, does it read the block itself as it always did.
// The switch file discblock gives the old behaviour back. Also: this thread's idle wait looks at the queue first (a request pushed while the last
// prefetch read was running used to wait the whole 250 ms), and one line of counters every 10 s goes to boot.log.
//
// Optional, both off: the switch file discpipe makes the prefetch a continuous pipeline that follows the emulation instead of 16 blocks after every
// miss; the switch file discasync lets cdvdReadInterrupt go on with the emulation while a block is awaited (cdvdTryGetSector, CDVD.cpp). The switch
// files are read once, in cdvdStartThread, through the weak OrbisFlag: the PC tests and tools that link this file without the app's paths still build
// (every switch is off there). tests/cdvdthread checks all of this against the real file, on a scripted drive and on a model of the PS5's.
#if defined(__GNUC__) || defined(__clang__)
bool OrbisFlag(const char* name) __attribute__((weak));
// OrbisIOCtlSrc.cpp: the drive layer's part of the 10 s line (read-ahead windows, speed, last drive read). Absent in the PC tests.
size_t OrbisDiscDriveStats(char* out, size_t size) __attribute__((weak));
static bool cdvdSwitch(const char* name)
{
	return OrbisFlag && OrbisFlag(name);
}
static size_t cdvdDriveStats(char* out, size_t size)
{
	return OrbisDiscDriveStats ? OrbisDiscDriveStats(out, size) : 0;
}
#else
static bool cdvdSwitch(const char*)
{
	return false;
}
static size_t cdvdDriveStats(char*, size_t)
{
	return 0;
}
#endif

static constexpr u32 kNoBlock = 0xFFFFFFFFu;
static std::atomic<u32> s_pending{0};            // requests in the queue (the queue's size, kept under s_request_lock, read without it)
static std::atomic<u32> s_inflight{kNoBlock};    // the block this thread has taken from the queue (or picked to prefetch) and is reading
static std::atomic<u32> s_failed_lsn{kNoBlock};  // the block this thread gave up on last ...
static std::atomic<u32> s_fail_seq{0};           // ... and how many it gave up on so far (a failure counts for a waiter only if it came after it began)
static std::atomic<bool> s_disc_gone{false};     // disc_has_changed, written here, for the CPU thread to read
static std::atomic<bool> s_quiesce{false};       // cdvdQuiesce: start no new drive read
static std::atomic<bool> s_cpu_wait{true};       // the CPU thread waits for this thread's read (switch file discblock: it reads itself)
static std::atomic<u32> s_wait_cap_ms{3000};     // ... for at most this long, then it reads the block itself
static std::atomic<bool> s_pipeline{false};      // switch file discpipe: the continuous prefetch pipeline
static std::atomic<bool> s_async{false};         // switch file discasync: DISCgetBuffer may answer "not yet" (-2), see cdvdTryGetSector
static std::atomic<u32> s_emu_block{0};          // pipeline: the block the emulation asked for last
static std::atomic<u32> s_frontier{0};           // pipeline: the next block the prefetch would read
static std::atomic<bool> s_stream{false};        // pipeline: a block was read, so the prefetch has a place to go on from
static constexpr u32 kPipelineBlocks = 24;       // pipeline: 768 KiB ahead of the emulation (was: 16 blocks after each miss)

// Counters of the line every 10 s (taken and cleared by the logger).
static std::atomic<unsigned long long> s_st_asked{0};      // sectors the CPU thread asked cdvdGetSector for (once per sector: DISCgetBuffer)
static std::atomic<unsigned long long> s_st_missed{0};     // ... blocks that were not in the cache (the first sector of a block that is not there yet)
static std::atomic<unsigned long long> s_st_wait_us{0};    // time the CPU thread spent on the misses
static std::atomic<unsigned long long> s_st_wait_max_us{0};
static std::atomic<unsigned long long> s_st_over33{0};     // misses that took over 33 ms (a frame at 30 fps)
static std::atomic<unsigned long long> s_st_fallbacks{0};  // misses where the CPU thread gave up waiting and read the block itself
static std::atomic<unsigned long long> s_st_blocks{0};     // blocks this thread read from the drive layer
static std::atomic<unsigned long long> s_st_read_us{0};    // time it spent on them
static std::chrono::steady_clock::time_point s_stats_at;

// Orbis: reduced to 8 (256 entries, ~9.6MB) to fit bigapp BSS budget
#define CACHE_SIZE 8

static constexpr u32 CacheSize = 1U << CACHE_SIZE;
static SectorInfo Cache[CacheSize];

static u32 cdvdSectorHash(u32 lsn)
{
	u32 t = 0;

	int i = 32;
	u32 m = CacheSize - 1;

	while (i >= 0)
	{
		t ^= lsn & m;
		lsn >>= CACHE_SIZE;
		i -= CACHE_SIZE;
	}

	return t & m;
}

static void cdvdCacheUpdate(u32 lsn, u8* data)
{
	std::lock_guard<std::mutex> guard(s_cache_lock);
	u32 entry = cdvdSectorHash(lsn);

	memcpy(Cache[entry].data, data, 2352 * sectors_per_read);
	Cache[entry].lsn = lsn;
}

static bool cdvdCacheCheck(u32 lsn)
{
	std::lock_guard<std::mutex> guard(s_cache_lock);
	u32 entry = cdvdSectorHash(lsn);

	return Cache[entry].lsn == lsn;
}

static bool cdvdCacheFetch(u32 lsn, u8* data)
{
	std::lock_guard<std::mutex> guard(s_cache_lock);
	u32 entry = cdvdSectorHash(lsn);

	if (Cache[entry].lsn == lsn)
	{
		memcpy(data, Cache[entry].data, 2352 * sectors_per_read);
		return true;
	}
	//printf("NOT IN CACHE\n");
	return false;
}

static void cdvdCacheReset()
{
	std::lock_guard<std::mutex> guard(s_cache_lock);
	for (u32 i = 0; i < CacheSize; i++)
	{
		Cache[i].lsn = std::numeric_limits<u32>::max();
	}
}

static bool cdvdReadBlockOfSectors(u32 sector, u8* data)
{
	u32 count = std::min(sectors_per_read, src->GetSectorCount() - sector);
	const s32 media = src->GetMediaType();

	// TODO: Is it really necessary to retry if it fails? I'm not sure the
	// second time is really going to be any better.
	for (int tries = 0; tries < 2; ++tries)
	{
		if (media >= 0)
		{
			if (src->ReadSectors2048(sector, count, data))
				return true;
		}
		else
		{
			if (src->ReadSectors2352(sector, count, data))
				return true;
		}
	}
	return false;
}

// PS5SX2: this thread's read of a block: the block is "in flight" while it is being read (a waiting CPU thread does not ask for it again), and the
// time goes into the 10 s line.
static bool cdvdThreadRead(u32 lsn, u8* data)
{
	const auto t0 = std::chrono::steady_clock::now();
	const bool ok = cdvdReadBlockOfSectors(lsn, data);
	s_st_blocks.fetch_add(1, std::memory_order_relaxed);
	s_st_read_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(),
		std::memory_order_relaxed);
	return ok;
}

// PS5SX2: the line every 10 s in boot.log (and a last one when the thread ends), only when the disc was touched in the time: what the CPU thread
// asked and waited for, what this thread read, the drive layer's part. With the [perf] lines it tells a slow second that waited for the drive from one
// that did not. Printed from this thread, never under the drive's lock.
static void cdvdLogStats(bool last)
{
	const auto now = std::chrono::steady_clock::now();
	if (!last && now < s_stats_at)
		return;
	s_stats_at = now + std::chrono::seconds(10);
	const unsigned long long asked = s_st_asked.exchange(0), missed = s_st_missed.exchange(0), wait_us = s_st_wait_us.exchange(0),
							 wait_max_us = s_st_wait_max_us.exchange(0), over33 = s_st_over33.exchange(0), fallbacks = s_st_fallbacks.exchange(0),
							 blocks = s_st_blocks.exchange(0), read_us = s_st_read_us.exchange(0);
	if (asked == 0 && blocks == 0)
		return;
	char drive[320] = {};
	cdvdDriveStats(drive, sizeof(drive));
	// 187: the speed those reads came at (16 sectors a block: 2048 bytes a DVD's, 2352 a CD's; a block from the read-ahead's memory counts too, so
	// under 1 ms of reads there is nothing to time). It stands in for the drive-speed measurement at every disc open, a 9 MiB read that is opt-in
	// now (OrbisIOCtlSrc.cpp, switch file disc_measure).
	char speed[48] = "under 1 ms of reads";
	if (read_us >= 1000)
		std::snprintf(speed, sizeof(speed), "%.2f MB/s while reading",
			static_cast<double>(blocks) * sectors_per_read * (src->GetMediaType() >= 0 ? 2048.0 : 2352.0) / static_cast<double>(read_us));
	// (the CPU thread asks cdvdGetSector once per sector, 16 to a block: "asked" counts sectors, "missed" the blocks that were not in the cache yet)
	printf("[disc] %s: cpu thread asked %llu sectors, missed %llu blocks, waited %llu ms (max %llu ms, over 33 ms: %llu%s) | cdvd thread read %llu blocks in %llu ms | "
		   "%s | %s | cpu-wait %s%s\n",
		last ? "last" : "10 s", asked, missed, wait_us / 1000, wait_max_us / 1000, over33, fallbacks ? (", own reads: " + std::to_string(fallbacks)).c_str() : "",
		blocks, read_us / 1000, speed, drive[0] ? drive : "(no drive layer figures)", s_cpu_wait.load() ? "on" : "off (switch file discblock)",
		s_pipeline.load() ? " | pipeline on" : "");
	fflush(stdout);
}

// PS5SX2: the CPU thread waits for the CDVD thread to deliver a block (instead of reading the drive itself). True: `buffer` has the block. False: give up
// (this thread is not running, the disc went or is being left, this thread failed to read the block while we waited, or 3 s passed) and the caller reads
// the block itself as it always did. A request is made again when the block is neither in the queue nor being read: the first time at once if the queue
// is empty (the request was lost, or its block was evicted from the cache), then every 50 ms (a disc change clears the queue).
static bool cdvdWaitForBlock(u32 block, u8* buffer)
{
	using clock = std::chrono::steady_clock;
	const auto t0 = clock::now();
	const auto cap = std::chrono::milliseconds(s_wait_cap_ms.load());
	const u32 fail_seq = s_fail_seq.load();
	auto last_request = t0;
	bool first = true;

	for (;;)
	{
		if (cdvdCacheFetch(block, buffer))
			return true;
		if (!cdvd_is_open || s_disc_gone.load() || s_quiesce.load())
			return false;
		if (s_fail_seq.load() != fail_seq && s_failed_lsn.load() == block)
			return false;
		const auto now = clock::now();
		if (now - t0 > cap)
			return false;
		if (s_inflight.load() != block &&
			((first && s_pending.load() == 0) || now - last_request > std::chrono::milliseconds(50)))
		{
			cdvdRequestSector(block, 0);
			last_request = now;
		}
		first = false;
		std::this_thread::sleep_for(std::chrono::microseconds(500));
	}
}

// PS5SX2 (pipeline): the next block the prefetch would read, when there is room: a block was read before (the pipeline has a place to go on from),
// and the next block is less than kPipelineBlocks ahead of the emulation's last request and inside the disc. A frontier the emulation has passed
// (or one left behind by a jump into the cache) is moved up to the emulation's block, else the thread would read from the old frontier all the way
// to emu + 24 blocks.
static bool cdvdPipelineNext(u32* block)
{
	if (!s_stream.load())
		return false;
	const u32 total = src->GetSectorCount();
	const u32 emu = s_emu_block.load();
	u32 frontier = s_frontier.load();
	if (frontier < emu)
		frontier = emu;
	if (frontier >= total || static_cast<unsigned long long>(frontier) >= static_cast<unsigned long long>(emu) + kPipelineBlocks * sectors_per_read)
		return false;
	if (block)
		*block = frontier;
	return true;
}

static void cdvdCallNewDiscCB()
{
	weAreInNewDiskCB = true;
	newDiscCB();
	weAreInNewDiskCB = false;
}

static bool cdvdUpdateDiscStatus()
{
	bool ready = src->DiscReady();

	if (!ready)
	{
		if (!disc_has_changed)
		{
			disc_has_changed = true;
			s_disc_gone.store(true); // PS5SX2: a CPU thread that waits for a block gives up
			curDiskType = CDVD_TYPE_NODISC;
			curTrayStatus = CDVD_TRAY_OPEN;
			cdvdCallNewDiscCB();
		}
	}
	else
	{
		if (disc_has_changed)
		{
			curDiskType = CDVD_TYPE_NODISC;
			curTrayStatus = CDVD_TRAY_CLOSE;

			disc_has_changed = false;
			s_disc_gone.store(false);
			cdvdRefreshData();

			{
				std::lock_guard<std::mutex> request_guard(s_request_lock);
				s_request_queue = decltype(s_request_queue)();
				s_pending.store(0);
			}
			s_stream.store(false);

			cdvdCallNewDiscCB();
		}
	}
	return !ready;
}

static void cdvdThread()
{
	u8 buffer[2352 * sectors_per_read];
	u32 prefetches_left = 0;

	printf(" * CDVD: IO thread started...\n");
	std::unique_lock<std::mutex> guard(s_notify_lock);

	while (cdvd_is_open)
	{
		cdvdLogStats(false); // PS5SX2: the line every 10 s

		// PS5SX2: quiesced (cdvdQuiesce, the app is about to re-execute itself): no new drive read until cdvdResume, and no question to the drive either
		// (DiscReady sends TEST UNIT READY, and after a disc change a speed command): this comes before cdvdUpdateDiscStatus.
		if (s_quiesce.load())
		{
			s_notify_cv.wait_for(guard, std::chrono::milliseconds(10), [] { return !cdvd_is_open || !s_quiesce.load(); });
			continue;
		}

		if (cdvdUpdateDiscStatus())
		{
			// Need to sleep some to avoid an aggressive spin that sucks the cpu dry.
			s_notify_cv.wait_for(guard, std::chrono::milliseconds(10));
			prefetches_left = 0;
			s_stream.store(false);
			continue;
		}

		// PS5SX2: the wait for work looks at the queue first (a request pushed while the last prefetch read was running used to wait the whole
		// 250 ms: nobody was waiting when it was notified), and is shorter. The pipeline waits further down, when it has nothing to read either.
		if (prefetches_left == 0 && !s_pipeline.load())
			s_notify_cv.wait_for(guard, std::chrono::milliseconds(50), [] { return !cdvd_is_open || s_pending.load() != 0 || s_quiesce.load(); });

		// check again to make sure we're not done here...
		if (!cdvd_is_open)
			break;
		if (s_quiesce.load()) // (quiesced during the wait: the request stays in the queue for cdvdResume)
			continue;

		// Read request
		bool handling_request = false;
		u32 request_lsn = 0;

		{
			std::lock_guard<std::mutex> request_guard(s_request_lock);
			if (!s_request_queue.empty())
			{
				request_lsn = s_request_queue.front();
				s_request_queue.pop();
				s_inflight.store(request_lsn); // (before s_pending: a waiter that sees an empty queue sees the block in flight)
				s_pending.store(static_cast<u32>(s_request_queue.size()));
				handling_request = true;
			}
		}

		if (!handling_request)
		{
			if (s_pipeline.load())
			{
				// PS5SX2 (pipeline): the prefetch goes on while it is less than kPipelineBlocks ahead of where the emulation reads; else wait for a
				// request or for the emulation to move on (a block it found in the cache makes room without a request: the timeout looks again).
				if (!cdvdPipelineNext(&request_lsn))
				{
					s_notify_cv.wait_for(guard, std::chrono::milliseconds(25),
						[] { return !cdvd_is_open || s_pending.load() != 0 || s_quiesce.load() || cdvdPipelineNext(nullptr); });
					continue;
				}
				s_inflight.store(request_lsn);
			}
			else
			{
				if (prefetches_left == 0)
					continue;

				--prefetches_left;

				u32 next_prefetch_lsn = g_last_sector_block_lsn + sectors_per_read;
				request_lsn = next_prefetch_lsn;
				s_inflight.store(request_lsn);
			}
		}

		// Handle request
		if (!cdvdCacheCheck(request_lsn))
		{
			if (cdvdThreadRead(request_lsn, buffer))
			{
				cdvdCacheUpdate(request_lsn, buffer);
			}
			else
			{
				// If the read fails, further reads are likely to fail too.
				prefetches_left = 0;
				s_stream.store(false);
				s_failed_lsn.store(request_lsn); // PS5SX2: a CPU thread that waits for this block is told
				s_fail_seq.fetch_add(1);
				s_inflight.store(kNoBlock);
				continue;
			}
		}
		s_inflight.store(kNoBlock);

		g_last_sector_block_lsn = request_lsn;

		if (s_pipeline.load())
		{
			// PS5SX2 (pipeline): the next block to read is the one after this, whether this was a request or a prefetch.
			s_frontier.store(request_lsn + sectors_per_read);
			s_stream.store(true);
			continue;
		}

		if (!handling_request)
			continue;

		// Prefetch
		u32 next_prefetch_lsn = g_last_sector_block_lsn + sectors_per_read;
		if (next_prefetch_lsn >= src->GetSectorCount())
		{
			prefetches_left = 0;
		}
		else
		{
			const u32 max_prefetches = 16;
			u32 remaining = src->GetSectorCount() - next_prefetch_lsn;
			prefetches_left = std::min((remaining + sectors_per_read - 1) / sectors_per_read, max_prefetches);
		}
	}
	cdvdLogStats(true); // PS5SX2
	printf(" * CDVD: IO thread finished.\n");
}

void cdvdStartThread()
{
	if (cdvd_is_open == false)
	{
		// PS5SX2: the switch files are read once here (see the comment at the top), the state of the last run is forgotten.
		s_cpu_wait.store(!cdvdSwitch("discblock"));
		s_pipeline.store(cdvdSwitch("discpipe"));
		s_async.store(cdvdSwitch("discasync"));
		{
			std::lock_guard<std::mutex> guard(s_request_lock);
			s_request_queue = decltype(s_request_queue)();
			s_pending.store(0);
		}
		s_inflight.store(kNoBlock);
		s_failed_lsn.store(kNoBlock);
		s_disc_gone.store(disc_has_changed);
		s_quiesce.store(false);
		s_stream.store(false);
		s_frontier.store(0);
		s_emu_block.store(0);
		s_st_asked.store(0);
		s_st_missed.store(0);
		s_st_wait_us.store(0);
		s_st_wait_max_us.store(0);
		s_st_over33.store(0);
		s_st_fallbacks.store(0);
		s_st_blocks.store(0);
		s_st_read_us.store(0);
		s_stats_at = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		char drive[320] = {};
		cdvdDriveStats(drive, sizeof(drive)); // (the drive layer's figures: read-ahead on or off, the drive's speed)
		printf("[disc] read path: cpu-wait=%s (cap %u ms)%s%s%s%s\n", s_cpu_wait.load() ? "on" : "off (switch file discblock: the CPU thread reads the drive itself)",
			s_wait_cap_ms.load(), s_pipeline.load() ? ", prefetch pipeline on (switch file discpipe)" : "",
			s_async.load() ? ", asynchronous reads on (switch file discasync: the emulation goes on while a block is awaited)" : "", drive[0] ? " | " : "", drive);
		fflush(stdout);
		cdvd_is_open = true;
		s_thread = std::thread(cdvdThread);
	}

	cdvdCacheReset();
}

void cdvdSetWaitCapMs(u32 ms)
{
	s_wait_cap_ms.store(ms);
}

// PS5SX2: the CDVD thread starts no new drive read (the one in flight finishes); waits at most max_ms for that. Never takes the drive's lock and sends no
// command. A read the thread has just decided to start when this is called may still go out: this is a best effort before a re-exec, not a fence.
void cdvdQuiesce(int max_ms)
{
	if (!cdvd_is_open.load())
		return;
	s_quiesce.store(true);
	s_notify_cv.notify_one();
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
	while (s_inflight.load() != kNoBlock && std::chrono::steady_clock::now() < until)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void cdvdResume()
{
	s_quiesce.store(false);
	s_notify_cv.notify_one();
}

void cdvdStopThread()
{
	cdvd_is_open = false;
	s_notify_cv.notify_one();
	if (s_thread.joinable())
		s_thread.join();
}

void cdvdRequestSector(u32 sector, s32 mode)
{
	if (sector >= src->GetSectorCount())
		return;

	// Align to cache block
	sector &= ~(sectors_per_read - 1);

	s_emu_block.store(sector, std::memory_order_relaxed); // PS5SX2 (pipeline): where the emulation reads

	if (cdvdCacheCheck(sector))
	{
		// PS5SX2 (pipeline): the emulation moved on without a request (a block it found in the cache), so the prefetch may have room again
		if (s_pipeline.load(std::memory_order_relaxed) &&
			s_frontier.load(std::memory_order_relaxed) < sector + kPipelineBlocks * sectors_per_read / 2)
			s_notify_cv.notify_one();
		return;
	}

	{
		std::lock_guard<std::mutex> guard(s_request_lock);
		s_request_queue.push(sector);
		s_pending.store(static_cast<u32>(s_request_queue.size()));
	}

	s_notify_cv.notify_one();
}

// The sector's place in its block (2048 bytes for a DVD; a CD's depends on its mode).
static u8* cdvdSectorPointer(u8* buffer, u32 sector, u32 sector_block, s32 mode)
{
	if (src->GetMediaType() >= 0)
	{
		u32 offset = 2048 * (sector - sector_block);
		return buffer + offset;
	}

	u32 offset = 2352 * (sector - sector_block);
	u8* data = buffer + offset;

	switch (mode)
	{
		case CDVD_MODE_2048:
			// Data location depends on CD mode
			return (data[15] & 3) == 2 ? data + 24 : data + 16;
		case CDVD_MODE_2328:
			return data + 24;
		case CDVD_MODE_2340:
			return data + 12;
	}
	return data;
}

u8* cdvdGetSector(u32 sector, s32 mode)
{
	static u8 buffer[2352 * sectors_per_read];

	// Align to cache block
	u32 sector_block = sector & ~(sectors_per_read - 1);

	s_st_asked.fetch_add(1, std::memory_order_relaxed);
	if (!cdvdCacheFetch(sector_block, buffer))
	{
		// PS5SX2: the block is not here yet. This is the emulation's CPU thread, the one that also runs the IOP: it waits for the CDVD thread's read of
		// the block (the same wait it used to spend in its own read, without a second reader fighting for the drive's head). It reads the block itself
		// only when the CDVD thread does not deliver (cdvdWaitForBlock), or when the switch file discblock says so.
		const auto t0 = std::chrono::steady_clock::now();
		bool got = false;
		if (s_cpu_wait.load())
			got = cdvdWaitForBlock(sector_block, buffer);
		if (!got)
		{
			if (s_cpu_wait.load())
				s_st_fallbacks.fetch_add(1, std::memory_order_relaxed);
			if (cdvdReadBlockOfSectors(sector_block, buffer))
				cdvdCacheUpdate(sector_block, buffer);
		}
		const unsigned long long us =
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
		s_st_missed.fetch_add(1, std::memory_order_relaxed);
		s_st_wait_us.fetch_add(us, std::memory_order_relaxed);
		if (us > 33000)
			s_st_over33.fetch_add(1, std::memory_order_relaxed);
		unsigned long long max_us = s_st_wait_max_us.load(std::memory_order_relaxed);
		while (us > max_us && !s_st_wait_max_us.compare_exchange_weak(max_us, us, std::memory_order_relaxed))
		{
		}
	}

	return cdvdSectorPointer(buffer, sector, sector_block, mode);
}

// PS5SX2 (switch file discasync, off): cdvdGetSector without the wait, for cdvdReadInterrupt when the disc source runs asynchronously (CDVD.cpp, the
// PS5SX2 hunk; DISCgetBuffer only calls this then). 0: *out is the sector. -2: the block is not here yet (the CDVD thread has it, or is asked for it
// here): the caller comes back a little later and the emulation goes on meanwhile. After 3 s without it (the cap), when the CDVD thread failed on it,
// or the disc went, the block is read here, as cdvdGetSector would, and 0 comes back.
s32 cdvdTryGetSector(u32 sector, s32 mode, u8** out)
{
	using clock = std::chrono::steady_clock;
	static u8 buffer[2352 * sectors_per_read];
	static u32 s_block = kNoBlock; // the block the last -2 was for
	static clock::time_point s_since, s_asked;
	static u32 s_seq = 0;

	const u32 sector_block = sector & ~(sectors_per_read - 1);
	if (s_block != sector_block)
		s_st_asked.fetch_add(1, std::memory_order_relaxed);
	if (!cdvdCacheFetch(sector_block, buffer))
	{
		const auto now = clock::now();
		const bool first = s_block != sector_block;
		if (first)
		{
			s_block = sector_block;
			s_since = s_asked = now;
			s_seq = s_fail_seq.load();
			s_st_missed.fetch_add(1, std::memory_order_relaxed);
		}
		const bool failed = s_fail_seq.load() != s_seq && s_failed_lsn.load() == sector_block;
		const bool give_up = !cdvd_is_open || s_disc_gone.load() || s_quiesce.load() || failed || now - s_since > std::chrono::milliseconds(s_wait_cap_ms.load());
		if (!give_up)
		{
			if (s_inflight.load() != sector_block && ((first && s_pending.load() == 0) || now - s_asked > std::chrono::milliseconds(50)))
			{
				cdvdRequestSector(sector_block, mode);
				s_asked = now;
			}
			return -2;
		}
		s_st_fallbacks.fetch_add(1, std::memory_order_relaxed);
		if (cdvdReadBlockOfSectors(sector_block, buffer))
			cdvdCacheUpdate(sector_block, buffer);
	}
	if (s_block == sector_block)
	{
		// the wait that began with the first -2 ends: it was the emulation's time in the bargain, not a freeze, but the line counts it
		const unsigned long long us = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - s_since).count();
		s_st_wait_us.fetch_add(us, std::memory_order_relaxed);
		if (us > 33000)
			s_st_over33.fetch_add(1, std::memory_order_relaxed);
		unsigned long long max_us = s_st_wait_max_us.load(std::memory_order_relaxed);
		while (us > max_us && !s_st_wait_max_us.compare_exchange_weak(max_us, us, std::memory_order_relaxed))
		{
		}
		s_block = kNoBlock;
	}
	*out = cdvdSectorPointer(buffer, sector, sector_block, mode);
	return 0;
}

bool cdvdAsyncOn()
{
	return s_async.load();
}

s32 cdvdDirectReadSector(u32 sector, s32 mode, u8* buffer)
{
	static u8 data[2352 * sectors_per_read];

	if (src == nullptr)
		return -1;

	if (sector >= src->GetSectorCount())
		return -1;

	// Align to cache block
	u32 sector_block = sector & ~(sectors_per_read - 1);

	if (!cdvdCacheFetch(sector_block, data))
	{
		if (cdvdReadBlockOfSectors(sector_block, data))
			cdvdCacheUpdate(sector_block, data);
	}

	if (src->GetMediaType() >= 0)
	{
		u32 offset = 2048 * (sector - sector_block);
		memcpy(buffer, data + offset, 2048);
		return 0;
	}

	u32 offset = 2352 * (sector - sector_block);
	u8* bfr = data + offset;

	switch (mode)
	{
		case CDVD_MODE_2048:
			// Data location depends on CD mode
			std::memcpy(buffer, (bfr[15] & 3) == 2 ? bfr + 24 : bfr + 16, 2048);
			return 0;
		case CDVD_MODE_2328:
			memcpy(buffer, bfr + 24, 2328);
			return 0;
		case CDVD_MODE_2340:
			memcpy(buffer, bfr + 12, 2340);
			return 0;
		default:
			memcpy(buffer, bfr, 2352);
			return 0;
	}
}

static s32 cdvdGetMediaType()
{
	return src->GetMediaType();
}

void cdvdRefreshData()
{
	const char* diskTypeName = "Unknown";

	//read TOC from device
	cdvdParseTOC();

	if ((etrack == 0) || (strack > etrack))
	{
		curDiskType = CDVD_TYPE_NODISC;
	}
	else
	{
		s32 mt = cdvdGetMediaType();

		if (mt < 0)
			curDiskType = CDVD_TYPE_DETCTCD;
		else if (mt == 0)
			curDiskType = CDVD_TYPE_DETCTDVDS;
		else
			curDiskType = CDVD_TYPE_DETCTDVDD;
	}

	curTrayStatus = CDVD_TRAY_CLOSE;

	switch (curDiskType)
	{
		case CDVD_TYPE_DETCTDVDD:
			diskTypeName = "Double-Layer DVD";
			break;
		case CDVD_TYPE_DETCTDVDS:
			diskTypeName = "Single-Layer DVD";
			break;
		case CDVD_TYPE_DETCTCD:
			diskTypeName = "CD-ROM";
			break;
		case CDVD_TYPE_NODISC:
			diskTypeName = "No Disc";
			break;
	}

	printf(" * CDVD: Disk Type: %s\n", diskTypeName);

	cdvdCacheReset();
}
