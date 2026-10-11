// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS.h"
#include "OrbisEEDiag.h" // vk-285-100
#include "Gif_Unit.h"
#include "MTGS.h"
#include "MTVU.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "VMManager.h"
#include "OrbisEEProf.h"

#include "common/FPControl.h"
#include "common/ScopedGuard.h"
#include "common/StringUtil.h"
#include "common/WrappedMemCopy.h"

#include <immintrin.h> // vk-285-124: _mm_pause (Gif_MTGS_WaitRead)
#include <list>
#include <mutex>
#include <thread>

// Uncomment this to enable profiling of the GS RingBufferCopy function.
//#define PCSX2_GSRING_SAMPLING_STATS

#if 0 //PCSX2_DEBUG
#define MTGS_LOG Console.WriteLn
#else
#define MTGS_LOG(...) \
	do                \
	{                 \
	} while (0)
#endif

namespace MTGS
{
	struct BufferedData
	{
		u128 m_Ring[RingBufferSize];
		u8 Regs[Ps2MemSize::GSregs];

		u128& operator[](uint idx)
		{
			pxAssert(idx < RingBufferSize);
			return m_Ring[idx];
		}
	};

	static void ThreadEntryPoint();
	static void MainLoop();

	static void GenericStall(uint size);

	static void PrepDataPacket(Command cmd, u32 size);
	static void PrepDataPacket(GIF_PATH pathidx, u32 size);
	static void SendDataPacket();

	static void SendSimplePacket(Command type, int data0, int data1, int data2);
	static void SendSimpleGSPacket(Command type, u32 offset, u32 size, GIF_PATH path);
	static void SendPointerPacket(Command type, u32 data0, void* data1);
	static void _FinishSimplePacket();
	static u8* GetDataPacketPtr();

	static void SetEvent();

	alignas(__cachelinesize) BufferedData RingBuffer;

	// note: when m_ReadPos == m_WritePos, the fifo is empty
	// Threading info: m_ReadPos is updated by the MTGS thread. m_WritePos is updated by the EE thread
	alignas(__cachelinesize) static std::atomic<unsigned int> s_ReadPos; // cur pos gs is reading from
	alignas(__cachelinesize) static std::atomic<unsigned int> s_WritePos; // cur pos ee thread is writing to

	// PS5 port (vk-285-90): the EE thread's own MTGS state on a cache line nothing else writes. In vk-285-89
	// GenericStall's cached read position (vk-285-84) sat on s_ReadPos's line, which this thread writes per
	// packet, so the cache missed about as often as the load it replaced; s_packet_size sat there too, and
	// s_CopyDataTally on the line of the flags the GS thread polls. An external, 64-byte object keeps its
	// layout (the compiler split and repacked the internal padded ones). The old names are references.
	struct alignas(__cachelinesize) OrbisMtgsEE
	{
		u32 packet_startpos, packet_size, packet_writepos;
		int copy_data_tally;
		uint cached_readpos;
	};
	OrbisMtgsEE g_orbis_mtgs_ee;
	static_assert(sizeof(OrbisMtgsEE) == __cachelinesize);

	// These vars maintain instance data for sending Data Packets.
	// Only one data packet can be constructed and uploaded at a time.
	static u32& s_packet_startpos = g_orbis_mtgs_ee.packet_startpos; // size of the packet (data only, ie. not including the 16 byte command!)
	static u32& s_packet_size = g_orbis_mtgs_ee.packet_size; // size of the packet (data only, ie. not including the 16 byte command!)
	static u32& s_packet_writepos = g_orbis_mtgs_ee.packet_writepos; // index of the data location in the ringbuffer.

	// PS5 port (vk-285-90): on a line of their own. The GS thread loads s_SignalRingEnable after every packet,
	// and it sat on s_WritePos's line, which the EE thread writes per packet.
	alignas(__cachelinesize) static std::atomic<bool> s_SignalRingEnable;
	static std::atomic<int> s_SignalRingPosition;

	static std::atomic<int> s_QueuedFrameCount;
	static std::atomic<bool> s_VsyncSignalListener;

	static std::mutex s_mtx_RingBufferBusy2; // Gets released on semaXGkick waiting...
	static Threading::WorkSema s_sem_event;
	static Threading::UserspaceSemaphore s_sem_OnRingReset;
	static Threading::UserspaceSemaphore s_sem_Vsync;

	// Used to delay the sending of events.  Performance is better if the ringbuffer
	// has more than one command in it when the thread is kicked.
	static int& s_CopyDataTally = g_orbis_mtgs_ee.copy_data_tally; // vk-285-90 (see OrbisMtgsEE)

#ifdef RINGBUF_DEBUG_STACK
	static std::mutex s_lock_Stack;
	static std::list<uint> ringposStack;
#endif

	static Threading::Thread s_thread;
	static std::atomic_bool s_open_flag{false};
	static std::atomic_bool s_shutdown_flag{false};
	static std::atomic_bool s_run_idle_flag{false};
	static Threading::UserspaceSemaphore s_open_or_close_done;
} // namespace MTGS

// =====================================================================================================
//  MTGS Threaded Class Implementation
// =====================================================================================================

const Threading::ThreadHandle& MTGS::GetThreadHandle()
{
	return s_thread;
}

bool MTGS::IsOpen()
{
	return s_open_flag.load(std::memory_order_acquire);
}

void MTGS::StartThread()
{
	if (s_thread.Joinable())
		return;

	pxAssertRel(!s_open_flag.load(), "GS thread should not be opened when starting");
	s_thread.SetStackSize(16 * 1024 * 1024);
	s_sem_event.Reset();
	s_shutdown_flag.store(false, std::memory_order_release);
	s_thread.Start(&MTGS::ThreadEntryPoint);
}

void MTGS::ShutdownThread()
{
	if (!s_thread.Joinable())
		return;

	// just go straight to shutdown, don't wait-for-open again
	s_shutdown_flag.store(true, std::memory_order_release);
	if (IsOpen())
		WaitForClose();

	// make sure the thread actually exits
	s_sem_event.NotifyOfWork();
	s_thread.Join();
}

void MTGS::ThreadEntryPoint()
{
	Threading::SetNameOfCurrentThread("GS");
	printf("[mtgs] thread tid=%llu\n", (unsigned long long)pthread_self());
	fflush(stdout);

	// GS can hit SMC write traps when executing InitAndReadFIFO
	// As racey as it sounds, it should be safe, since InitAndReadFIFO is requested and immediately waited for,
	// so the EE thread shouldn't be touching any of its codegen structures while it happens.
	PageFaultHandler::InstallSecondaryThread();

	// Explicitly set rounding mode to default (nearest, FTZ off).
	// Otherwise it appears to get inherited from the EE thread on Linux.
	FPControlRegister::SetCurrent(FPControlRegister::GetDefault());

	for (;;)
	{
		// wait until we're actually asked to initialize (and config has been loaded, etc)
		while (!s_open_flag.load(std::memory_order_acquire))
		{
			if (s_shutdown_flag.load(std::memory_order_acquire))
			{
				s_sem_event.Kill();
				return;
			}

			s_sem_event.WaitForWork();
		}

		// try initializing.. this could fail
		Console.WriteLn("[dbg] mtgs: GSopen begin renderer=%d", (int)EmuConfig.GS.Renderer);
		std::memcpy(RingBuffer.Regs, PS2MEM_GS, sizeof(PS2MEM_GS));
		const bool opened = GSopen(EmuConfig.GS, EmuConfig.GS.Renderer, RingBuffer.Regs,
			VMManager::GetEffectiveVSyncMode(), VMManager::ShouldAllowPresentThrottle());
		Console.WriteLn("[dbg] mtgs: GSopen end opened=%d", (int)opened);
		s_open_flag.store(opened, std::memory_order_release);

		// notify emu thread that we finished opening (or failed)
		s_open_or_close_done.Post();

		// are we open?
		if (!opened)
		{
			// wait until we're asked to try again...
			continue;
		}

		// we're ready to go
		MainLoop();

		// when we come back here, it's because we closed (or shutdown)
		// that means the emu thread should be blocked, waiting for us to be done
		pxAssertRel(!s_open_flag.load(std::memory_order_relaxed), "Open flag is clear on close");
		GSclose();
		s_open_or_close_done.Post();

		// we need to reset sem_event here, because MainLoop() kills it.
		s_sem_event.Reset();
	}
}

void MTGS::ResetGS(bool hardware_reset)
{
	// MTGS Reset process:
	//  * clear the ringbuffer.
	//  * Signal a reset.
	//  * clear the path and byRegs structs (used by GIFtagDummy)

	if (hardware_reset)
	{
		s_ReadPos = s_WritePos.load();
		s_QueuedFrameCount = 0;
		s_VsyncSignalListener = 0;
	}

	MTGS_LOG("MTGS: Sending Reset...");
	SendSimplePacket(Command::Reset, static_cast<int>(hardware_reset), 0, 0);

	if (hardware_reset)
		SetEvent();
}

int MTGS::GetCurrentVsyncQueueSize()
{
	return s_QueuedFrameCount.load(std::memory_order_acquire);
}

struct RingCmdPacket_Vsync
{
	u8 regset1[0x0f0];
	u32 csr;
	u32 imr;
	GSRegSIGBLID siglblid;

	// must be 16 byte aligned
	u32 registers_written;
	u32 pad[3];
};

unsigned long long g_orbis_ee_vsyncq_ticks; // eerec-281: EE blocked because the GS is VsyncQueueSize frames behind
std::atomic<int> g_orbis_ee_waiting{0}; // vk-285-8: > 0 while the EE thread is in an accounted wait (OrbisEEProf.h)
void MTGS::PostVsyncStart(bool registers_written)
{
	// Optimization note: Typically regset1 isn't needed.  The regs in that area are typically
	// changed infrequently, usually during video mode changes.  However, on modern systems the
	// 256-byte copy is only a few dozen cycles -- executed 60 times a second -- so probably
	// not worth the effort or overhead of trying to selectively avoid it.

	uint packsize = sizeof(RingCmdPacket_Vsync) / 16;
	PrepDataPacket(Command::VSync, packsize);
	MemCopy_WrappedDest((u128*)PS2MEM_GS, RingBuffer.m_Ring, s_packet_writepos, RingBufferSize, 0xf);

	u32* remainder = (u32*)GetDataPacketPtr();
	remainder[0] = GSCSRr;
	remainder[1] = GSIMR._u32;
	(GSRegSIGBLID&)remainder[2] = GSSIGLBLID;
	remainder[4] = static_cast<u32>(registers_written);
	s_packet_writepos = (s_packet_writepos + 2) & RingBufferMask;

	SendDataPacket();

	// Vsyncs should always start the GS thread, regardless of how little has actually be queued.
	if (s_CopyDataTally != 0)
		SetEvent();

	// If the MTGS is allowed to queue a lot of frames in advance, it creates input lag.
	// Use the Queued FrameCount to stall the EE if another vsync (or two) are already queued
	// in the ringbuffer.  The queue limit is disabled when both FrameLimiting and Vsync are
	// disabled, since the queue can have perverse effects on framerate benchmarking.

	// Edit: It's possible that MTGS is that much faster than GS that it creates so much lag,
	// a game becomes uncontrollable (software rendering for example).
	// For that reason it's better to have the limit always in place, at the cost of a few max FPS in benchmarks.
	// If those are needed back, it's better to increase the VsyncQueueSize via PCSX_vm.ini.
	// (The Xenosaga engine is known to run into this, due to it throwing bulks of data in one frame followed by 2 empty frames.)

	if ((s_QueuedFrameCount.fetch_add(1) < EmuConfig.GS.VsyncQueueSize) /*|| (!EmuConfig.GS.VsyncEnable && !EmuConfig.GS.FrameLimitEnable)*/)
		return;

	s_VsyncSignalListener.store(true, std::memory_order_release);
	//Console.WriteLn( Color_Blue, "(EEcore Sleep) Vsync\t\tringpos=0x%06x, writepos=0x%06x", m_ReadPos.load(), m_WritePos.load() );

	{
		vu1Thread.FlushKick(); // vk-285-84: before the EE waits on the GS (which may wait on VU1)
		const unsigned long long t0 = __builtin_ia32_rdtsc(); // eerec-281
		OrbisEEWaitScope orbis_wait; // vk-285-8
		s_sem_Vsync.Wait();
		g_orbis_ee_vsyncq_ticks += __builtin_ia32_rdtsc() - t0;
	}
}

void MTGS::InitAndReadFIFO(u8* mem, u32 qwc)
{
	if (EmuConfig.GS.HWDownloadMode >= GSHardwareDownloadMode::Unsynchronized && GSIsHardwareRenderer())
	{
		if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
			GSReadLocalMemoryUnsync(mem, qwc, vif1.BITBLTBUF._u64, vif1.TRXPOS._u64, vif1.TRXREG._u64);
		else
			std::memset(mem, 0, qwc * 16);

		return;
	}

	SendPointerPacket(Command::InitAndReadFIFO, qwc, mem);
	WaitGS(false, false, false);
}

union PacketTagType
{
	struct
	{
		u32 command;
		u32 data[3];
	};
	struct
	{
		u32 _command;
		u32 _data[1];
		uptr pointer;
	};
};

unsigned long long g_orbis_gs_idle_ticks, g_orbis_ee_waitgs_ticks, g_orbis_ee_stall_ticks, g_orbis_ee_waitgs_n, g_orbis_ee_stall_n;
void MTGS::MainLoop()
{
	// Threading info: run in MTGS thread
	// m_ReadPos is only update by the MTGS thread so it is safe to load it with a relaxed atomic

#ifdef RINGBUF_DEBUG_STACK
	PacketTagType prevCmd;
#endif

	std::unique_lock mtvu_lock(s_mtx_RingBufferBusy2);

	// PS5 port (vk-285-85): see Command::MTVUGSPacket. XGKICK posts claimed ahead, and PATH1 bytes read
	// but not yet given back to readAmount (the VU thread's room check); given back before this thread
	// waits on anything, so the VU thread never waits on bytes this thread already read.
	s32 orbis_xgkick_credits = 0;
	u32 orbis_path1_read = 0;
	const auto orbis_flush_path1_read = [&orbis_path1_read]() {
		if (orbis_path1_read)
		{
			gifUnit.gifPath[GIF_PATH_1].readAmount.fetch_sub(orbis_path1_read, std::memory_order_acq_rel);
			orbis_path1_read = 0;
		}
	};

	while (true)
	{
		orbis_flush_path1_read(); // vk-285-85: before an idle present or a wait for work
		if (s_run_idle_flag.load(std::memory_order_acquire) && VMManager::GetState() != VMState::Running && GSHasDisplayWindow())
		{
			if (!s_sem_event.CheckForWork())
			{
				GSPresentCurrentFrame();
				GSThrottlePresentation();
			}
		}
		else
		{
			mtvu_lock.unlock();
			{
				const unsigned long long t0 = __builtin_ia32_rdtsc();
				s_sem_event.WaitForWork();
				g_orbis_gs_idle_ticks += __builtin_ia32_rdtsc() - t0;
			}
			mtvu_lock.lock();
		}

		if (!s_open_flag.load(std::memory_order_acquire))
			break;

		// note: m_ReadPos is intentionally not volatile, because it should only
		// ever be modified by this thread.
		// PS5 port (vk-285-90): s_WritePos is loaded again only when this thread has caught up with the last
		// value it loaded, not after every packet: the EE thread writes it per packet (~1.2 million MTVU
		// packets a second in Shadow of the Colossus), so each load was a cross-core miss. Needs proper testing.
		unsigned int orbis_write_pos = s_WritePos.load(std::memory_order_acquire);
		for (;;)
		{
			if (s_ReadPos.load(std::memory_order_relaxed) == orbis_write_pos)
			{
				orbis_write_pos = s_WritePos.load(std::memory_order_acquire);
				if (s_ReadPos.load(std::memory_order_relaxed) == orbis_write_pos)
					break;
			}
			const unsigned int local_ReadPos = s_ReadPos.load(std::memory_order_relaxed);

			pxAssert(local_ReadPos < RingBufferSize);

			const PacketTagType& tag = (PacketTagType&)RingBuffer[local_ReadPos];
			u32 ringposinc = 1;

#ifdef RINGBUF_DEBUG_STACK
			// pop a ringpos off the stack.  It should match this one!

			s_lock_Stack.Lock();
			uptr stackpos = ringposStack.back();
			if (stackpos != local_ReadPos)
			{
				Console.Error("MTGS Ringbuffer Critical Failure ---> %x to %x (prevCmd: %x)\n", stackpos, local_ReadPos, prevCmd.command);
			}
			pxAssert(stackpos == local_ReadPos);
			prevCmd = tag;
			ringposStack.pop_back();
			s_lock_Stack.Release();
#endif

			switch (static_cast<Command>(tag.command))
			{
#if COPY_GS_PACKET_TO_MTGS == 1
				case Command::GIFPath1:
				{
					uint datapos = (local_ReadPos + 1) & RingBufferMask;
					const int qsize = tag.data[0];
					const u128* data = &RingBuffer[datapos];

					MTGS_LOG("(MTGS Packet Read) ringtype=P1, qwc=%u", qsize);

					uint endpos = datapos + qsize;
					if (endpos >= RingBufferSize)
					{
						uint firstcopylen = RingBufferSize - datapos;
						GSgifTransfer((u8*)data, firstcopylen);
						datapos = endpos & RingBufferMask;
						GSgifTransfer((u8*)RingBuffer.m_Ring, datapos);
					}
					else
					{
						GSgifTransfer((u8*)data, qsize);
					}

					ringposinc += qsize;
				}
				break;

				case Command::GIFPath2:
				{
					uint datapos = (local_ReadPos + 1) & RingBufferMask;
					const int qsize = tag.data[0];
					const u128* data = &RingBuffer[datapos];

					MTGS_LOG("(MTGS Packet Read) ringtype=P2, qwc=%u", qsize);

					uint endpos = datapos + qsize;
					if (endpos >= RingBufferSize)
					{
						uint firstcopylen = RingBufferSize - datapos;
						GSgifTransfer2((u32*)data, firstcopylen);
						datapos = endpos & RingBufferMask;
						GSgifTransfer2((u32*)RingBuffer.m_Ring, datapos);
					}
					else
					{
						GSgifTransfer2((u32*)data, qsize);
					}

					ringposinc += qsize;
				}
				break;

				case Command::GIFPath3:
				{
					uint datapos = (local_ReadPos + 1) & RingBufferMask;
					const int qsize = tag.data[0];
					const u128* data = &RingBuffer[datapos];

					MTGS_LOG("(MTGS Packet Read) ringtype=P3, qwc=%u", qsize);

					uint endpos = datapos + qsize;
					if (endpos >= RingBufferSize)
					{
						uint firstcopylen = RingBufferSize - datapos;
						GSgifTransfer3((u32*)data, firstcopylen);
						datapos = endpos & RingBufferMask;
						GSgifTransfer3((u32*)RingBuffer.m_Ring, datapos);
					}
					else
					{
						GSgifTransfer3((u32*)data, qsize);
					}

					ringposinc += qsize;
				}
				break;
#endif
				case Command::GSPacket:
				{
					Gif_Path& path = gifUnit.gifPath[tag.data[2]];
					u32 offset = tag.data[0];
					u32 size = tag.data[1];
					if (offset != ~0u)
						GSgifTransfer((u8*)&path.buffer[offset], size / 16);
					path.readAmount.fetch_sub(size, std::memory_order_acq_rel);
					break;
				}

				case Command::MTVUGSPacket:
				{
					MTVU_LOG("MTGS - Waiting on semaXGkick!");
					// PS5 port (vk-285-85): the per-packet handoff from the VU thread (~26,000 packets a
					// frame in Shadow of the Colossus; ~3.5% of the GS thread in vk-285-84's profile, in
					// atomics on lines the VU thread writes per program). Only this thread takes XGKICK
					// posts, so it claims every post there is at once and counts them down itself; a post
					// means the VU thread pushed its packet first, so the queue needs no empty check then.
					// PATH1's readAmount is given back in batches (orbis_flush_path1_read, before any
					// wait). Before sleeping on a post it spins ~20 us with the lock released.
					// Needs proper testing.
					if (orbis_xgkick_credits > 0)
						orbis_xgkick_credits--;
					else
					{
						s32 got = vu1Thread.semaXGkick.TryWaitAll();
						if (got == 0)
						{
							orbis_flush_path1_read();
							mtvu_lock.unlock();
							for (u32 waited = 0; waited < 20000 && got == 0;)
							{
								waited += ShortSpin();
								got = vu1Thread.semaXGkick.TryWaitAll();
							}
							if (got == 0)
							{
								// Wait for MTVU to complete vu1 program
								vu1Thread.semaXGkick.Wait();
								got = 1;
							}
							mtvu_lock.lock();
						}
						orbis_xgkick_credits = got - 1;
					}
					Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];
					GS_Packet gsPack = path.mtvu.gsPackQueue.front(); // vu1 program's xgkick packet(s)
					// PS5 port (vk-285-92): with another XGKICK already claimed, the next program's packet is
					// complete (the VU thread pushes before it posts): start pulling its first lines over from
					// the VU thread's cache while this one is parsed. Reading VU1's vertices as they arrived
					// was a string of cross-core misses in vk-285-90's GS profile (Transfer and the vertex
					// handlers). Needs proper testing.
					if (orbis_xgkick_credits > 0)
					{
						const GS_Packet& next = path.mtvu.gsPackQueue.peek(1);
						if (next.size)
						{
							const char* p = reinterpret_cast<const char*>(&path.buffer[next.offset]);
							const u32 lines = std::min<u32>((next.size + 63) / 64, 16);
							for (u32 i = 0; i < lines && i < 4; i++)
								_mm_prefetch(p + i * 64, _MM_HINT_T0);
							for (u32 i = 4; i < lines; i++)
								_mm_prefetch(p + i * 64, _MM_HINT_T1);
						}
					}
					if (gsPack.size)
						GSgifTransfer((u8*)&path.buffer[gsPack.offset], gsPack.size / 16);
					orbis_path1_read += gsPack.size + gsPack.readAmount;
					if (orbis_path1_read >= (64u << 10))
						orbis_flush_path1_read();
					path.PopGSPacketMTVU(); // Should be done last, for proper Gif_MTGS_Wait()
					break;
				}

				default:
				{
					switch (static_cast<Command>(tag.command))
					{
						case Command::VSync:
						{
							const int qsize = tag.data[0];
							ringposinc += qsize;

							MTGS_LOG("(MTGS Packet Read) ringtype=Vsync, field=%u, skip=%s", !!(((u32&)RingBuffer.Regs[0x1000]) & 0x2000) ? 0 : 1, tag.data[1] ? "true" : "false");

							// Mail in the important GS registers.
							// This seemingly obtuse system is needed in order to handle cases where the vsync data wraps
							// around the edge of the ringbuffer.  If not for that I'd just use a struct. >_<

							uint datapos = (local_ReadPos + 1) & RingBufferMask;
							MemCopy_WrappedSrc(RingBuffer.m_Ring, datapos, RingBufferSize, (u128*)RingBuffer.Regs, 0xf);

							u32* remainder = (u32*)&RingBuffer[datapos];
							((u32&)RingBuffer.Regs[0x1000]) = remainder[0];
							((u32&)RingBuffer.Regs[0x1010]) = remainder[1];
							((GSRegSIGBLID&)RingBuffer.Regs[0x1080]) = (GSRegSIGBLID&)remainder[2];

							// CSR & 0x2000; is the pageflip id.
							GSvsync((((u32&)RingBuffer.Regs[0x1000]) & 0x2000) ? 0 : 1, remainder[4] != 0);

							s_QueuedFrameCount.fetch_sub(1);
							if (s_VsyncSignalListener.exchange(false))
								s_sem_Vsync.Post();

							// Do not StateCheckInThread() here
							// Otherwise we could pause while there's still data in the queue
							// Which could make the MTVU thread wait forever for it to empty
						}
						break;

						case Command::AsyncCall:
							{
								AsyncCallType* const func = (AsyncCallType*)tag.pointer;
								(*func)();
								delete func;
							}
							break;

						case Command::Freeze:
						{
							MTGS::FreezeData* data = (MTGS::FreezeData*)tag.pointer;
							int mode = tag.data[0];
							data->retval = GSfreeze((FreezeAction)mode, (freezeData*)data->fdata);
						}
						break;

						case Command::Reset:
							MTGS_LOG("(MTGS Packet Read) ringtype=Reset");
							GSreset(tag.data[0] != 0);
							break;

						case Command::SoftReset:
						{
							int mask = tag.data[0];
							MTGS_LOG("(MTGS Packet Read) ringtype=SoftReset");
							GSgifSoftReset(mask);
						}
						break;

						case Command::InitAndReadFIFO:
							MTGS_LOG("(MTGS Packet Read) ringtype=Fifo2, size=%d", tag.data[0]);
							GSInitAndReadFIFO((u8*)tag.pointer, tag.data[0]);
							break;

#ifdef PCSX2_DEVBUILD
						default:
							Console.Error("GSThreadProc, bad packet (%x) at m_ReadPos: %x, m_WritePos: %x", tag.command, local_ReadPos, s_WritePos.load());
							pxFail("Bad packet encountered in the MTGS Ringbuffer.");
							s_ReadPos.store(s_WritePos.load(std::memory_order_acquire), std::memory_order_release);
							continue;
#else
							// Optimized performance in non-Dev builds.
							jNO_DEFAULT;
#endif
					}
				}
			}

			uint newringpos = (s_ReadPos.load(std::memory_order_relaxed) + ringposinc) & RingBufferMask;

			if (IsDevBuild && EmuConfig.GS.SynchronousMTGS) [[unlikely]]
			{
				pxAssert(s_WritePos == newringpos);
			}

			s_ReadPos.store(newringpos, std::memory_order_release);

			if (s_SignalRingEnable.load(std::memory_order_acquire))
			{
				// The EEcore has requested a signal after some amount of processed data.
				if (s_SignalRingPosition.fetch_sub(ringposinc) <= 0)
				{
					// Make sure to post the signal after the m_ReadPos has been updated...
					s_SignalRingEnable.store(false, std::memory_order_release);
					s_sem_OnRingReset.Post();
					continue;
				}
			}
		}

		// TODO: With the new race-free WorkSema do we still need these?

		// Safety valve in case standard signals fail for some reason -- this ensures the EEcore
		// won't sleep the eternity, even if SignalRingPosition didn't reach 0 for some reason.
		// Important: Need to unlock the MTGS busy signal PRIOR, so that EEcore SetEvent() calls
		// parallel to this handler aren't accidentally blocked.
		if (s_SignalRingEnable.exchange(false))
		{
			//Console.Warning( "(MTGS Thread) Dangling RingSignal on empty buffer!  signalpos=0x%06x", m_SignalRingPosition.exchange(0) ) );
			s_SignalRingPosition.store(0, std::memory_order_release);
			s_sem_OnRingReset.Post();
		}

		if (s_VsyncSignalListener.exchange(false))
			s_sem_Vsync.Post();

		//Console.Warning( "(MTGS Thread) Nothing to do!  ringpos=0x%06x", m_ReadPos );
	}

	// Unblock any threads in WaitGS in case MTGS gets cancelled while still processing work
	s_ReadPos.store(s_WritePos.load(std::memory_order_acquire), std::memory_order_relaxed);
	s_sem_event.Kill();
}

// Waits for the GS to empty out the entire ring buffer contents.
// If syncRegs, then writes pcsx2's gs regs to MTGS's internal copy
// If weakWait, then this function is allowed to exit after MTGS finished a path1 packet
// If isMTVU, then this implies this function is being called from the MTVU thread...
void MTGS::WaitGS(bool syncRegs, bool weakWait, bool isMTVU)
{
	pxAssertMsg(IsOpen(), "MTGS Warning!  WaitGS issued on a closed thread.");
	if (!IsOpen()) [[unlikely]]
		return;

	// vk-285-84: a VU1 kick deferred inside a VIF1 transfer goes out before the EE waits on the GS,
	// which may be waiting on that VU1 program's XGKICK. (From the MTVU thread, isMTVU, there's none.)
	if (!isMTVU)
		vu1Thread.FlushKick();

	Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];

	// Both m_ReadPos and m_WritePos can be relaxed as we only want to test if the queue is empty but
	// we don't want to access the content of the queue

	SetEvent();
	if (weakWait && isMTVU)
	{
		// On weakWait we will stop waiting on the MTGS thread if the
		// MTGS thread has processed a vu1 xgkick packet, or is pending on
		// its final vu1 xgkick packet (!curP1Packs)...
		// Note: m_WritePos doesn't seem to have proper atomic write
		// code, so reading it from the MTVU thread might be dangerous;
		// hence it has been avoided...
		u32 startP1Packs = path.GetPendingGSPackets();
		if (startP1Packs)
		{
			while (true)
			{
				// m_mtx_RingBufferBusy2.Wait();
				s_mtx_RingBufferBusy2.lock();
				s_mtx_RingBufferBusy2.unlock();
				if (path.GetPendingGSPackets() != startP1Packs)
					break;
			}
		}
	}
	else
	{
		const unsigned long long t0 = __builtin_ia32_rdtsc();
		OrbisEEWaitScope orbis_wait; // vk-285-8
		if (!s_sem_event.WaitForEmpty())
			pxFailRel("MTGS Thread Died");
		g_orbis_ee_waitgs_ticks += __builtin_ia32_rdtsc() - t0;
		++g_orbis_ee_waitgs_n;
	}

	pxAssert(!(weakWait && syncRegs) && "No synchronization for this!");

	if (syncRegs)
	{
		// Completely synchronize GS and MTGS register states.
		memcpy(RingBuffer.Regs, PS2MEM_GS, sizeof(RingBuffer.Regs));
	}
}

// Sets the gsEvent flag and releases a timeslice.
// For use in loops that wait on the GS thread to do certain things.
void MTGS::SetEvent()
{
	s_sem_event.NotifyOfWork();
	s_CopyDataTally = 0;
}

u8* MTGS::GetDataPacketPtr()
{
	return (u8*)&RingBuffer[s_packet_writepos & RingBufferMask];
}

// Closes the data packet send command, and initiates the gs thread (if needed).
void MTGS::SendDataPacket()
{
	// make sure a previous copy block has been started somewhere.
	pxAssert(s_packet_size != 0);

	uint actualSize = ((s_packet_writepos - s_packet_startpos) & RingBufferMask) - 1;
	pxAssert(actualSize <= s_packet_size);
	pxAssert(s_packet_writepos < RingBufferSize);

	PacketTagType& tag = (PacketTagType&)RingBuffer[s_packet_startpos];
	tag.data[0] = actualSize;

	s_WritePos.store(s_packet_writepos, std::memory_order_release);

	if (IsDevBuild && EmuConfig.GS.SynchronousMTGS) [[unlikely]]
	{
		WaitGS();
	}
	else
	{
		s_CopyDataTally += s_packet_size;
		if (s_CopyDataTally > 0x2000)
			SetEvent();
	}

	s_packet_size = 0;

	//m_PacketLocker.Release();
}

void MTGS::GenericStall(uint size)
{
	// Note on volatiles: m_WritePos is not modified by the GS thread, so there's no need
	// to use volatile reads here.  We do cache it though, since we know it never changes,
	// except for calls to RingbufferRestert() -- handled below.
	const uint writepos = s_WritePos.load(std::memory_order_relaxed);

	// Sanity checks! (within the confines of our ringbuffer please!)
	pxAssert(size < RingBufferSize);
	pxAssert(writepos < RingBufferSize);

	// generic gs wait/stall.
	// if the writepos is past the readpos then we're safe.
	// But if not then we need to make sure the readpos is outside the scope of
	// the block about to be written (writepos + size)

	// PS5 port (vk-285-84): first against the last read position this thread loaded. The GS thread only
	// moves s_ReadPos forward (ResetGS and the thread's exit jump it to s_WritePos, also forward), so an
	// old value can only under-state the room; the shared line (written by the GS thread per packet, a
	// cross-core miss per packet in vk-285-83's SotC profile, ~26,000 VU1 packets a frame) is loaded only
	// when it says there's no room. Needs proper testing.
	uint& s_orbis_cached_readpos = g_orbis_mtgs_ee.cached_readpos; // vk-285-90: its own line (OrbisMtgsEE)
	uint readpos = s_orbis_cached_readpos;
	uint freeroom;

	// PS5 port (vk-285-99): the ring's lines 1 KB past this packet's span, asked for with write intent (see
	// VU_Thread::ReserveSpace): each packet's stores otherwise wait on a read from memory in the store queue.
	// At most 64 lines for a big packet.
	if (g_orbis_pfw.load(std::memory_order_relaxed)) // vk-285-100: flags/nopfw turns these off, live
	{
		constexpr uint ahead = 64; // qwords
		const uint lines = std::min<uint>((size + 3) / 4, 64);
		for (uint i = 0; i < lines; i++)
			__builtin_prefetch(&RingBuffer[(writepos + ahead + i * 4) & RingBufferMask], 1, 3);
	}

	if (writepos < readpos)
		freeroom = readpos - writepos;
	else
		freeroom = RingBufferSize - (writepos - readpos);

	if (freeroom <= size)
	{
		readpos = s_ReadPos.load(std::memory_order_acquire);
		s_orbis_cached_readpos = readpos;
		if (writepos < readpos)
			freeroom = readpos - writepos;
		else
			freeroom = RingBufferSize - (writepos - readpos);
	}

	if (freeroom <= size)
	{
		vu1Thread.FlushKick(); // vk-285-84: the GS may be waiting on a VU1 program's XGKICK
		struct OrbisStallTimer { unsigned long long t0 = __builtin_ia32_rdtsc(); ~OrbisStallTimer() { g_orbis_ee_stall_ticks += __builtin_ia32_rdtsc() - t0; ++g_orbis_ee_stall_n; } } orbis_stall_timer;
		OrbisEEWaitScope orbis_wait; // vk-285-8
		// writepos will overlap readpos if we commit the data, so we need to wait until
		// readpos is out past the end of the future write pos, or until it wraps around
		// (in which case writepos will be >= readpos).

		// Ideally though we want to wait longer, because if we just toss in this packet
		// the next packet will likely stall up too.  So lets set a condition for the MTGS
		// thread to wake up the EE once there's a sizable chunk of the ringbuffer emptied.

		uint somedone = (RingBufferSize - freeroom) / 4;
		if (somedone < size + 1)
			somedone = size + 1;

		// FMV Optimization: FMVs typically send *very* little data to the GS, in some cases
		// every other frame is nothing more than a page swap.  Sleeping the EEcore is a
		// waste of time, and we get better results using a spinwait.

		if (somedone > 0x80)
		{
			pxAssertMsg(s_SignalRingEnable == 0, "MTGS Thread Synchronization Error");
			s_SignalRingPosition.store(somedone, std::memory_order_release);

			//Console.WriteLn( Color_Blue, "(EEcore Sleep) PrepDataPacker \tringpos=0x%06x, writepos=0x%06x, signalpos=0x%06x", readpos, writepos, m_SignalRingPosition );

			while (true)
			{
				s_SignalRingEnable.store(true, std::memory_order_release);
				SetEvent();
				s_sem_OnRingReset.Wait();
				readpos = s_ReadPos.load(std::memory_order_acquire);
				//Console.WriteLn( Color_Blue, "(EEcore Awake) Report!\tringpos=0x%06x", readpos );

				if (writepos < readpos)
					freeroom = readpos - writepos;
				else
					freeroom = RingBufferSize - (writepos - readpos);

				if (freeroom > size)
					break;
			}

			pxAssertMsg(s_SignalRingPosition <= 0, "MTGS Thread Synchronization Error");
		}
		else
		{
			//Console.WriteLn( Color_StrongGray, "(EEcore Spin) PrepDataPacket!" );
			SetEvent();
			while (true)
			{
				Threading::SpinWait();
				readpos = s_ReadPos.load(std::memory_order_acquire);

				if (writepos < readpos)
					freeroom = readpos - writepos;
				else
					freeroom = RingBufferSize - (writepos - readpos);

				if (freeroom > size)
					break;
			}
		}
		s_orbis_cached_readpos = readpos; // vk-285-84
	}
}

void MTGS::PrepDataPacket(Command cmd, u32 size)
{
	s_packet_size = size;
	++size; // takes into account our RingCommand QWC.
	GenericStall(size);

	// Command qword: Low word is the command, and the high word is the packet
	// length in SIMDs (128 bits).
	const unsigned int local_WritePos = s_WritePos.load(std::memory_order_relaxed);

	PacketTagType& tag = (PacketTagType&)RingBuffer[local_WritePos];
	tag.command = static_cast<u32>(cmd);
	tag.data[0] = s_packet_size;
	s_packet_startpos = local_WritePos;
	s_packet_writepos = (local_WritePos + 1) & RingBufferMask;
}

// Returns the amount of giftag data processed (in simd128 values).
// Return value is used by VU1's XGKICK instruction to wrap the data
// around VU memory instead of having buffer overflow...
// Parameters:
//  size - size of the packet data, in smd128's
void MTGS::PrepDataPacket(GIF_PATH pathidx, u32 size)
{
	//m_PacketLocker.Acquire();

	PrepDataPacket(static_cast<Command>(pathidx), size);
}

__fi void MTGS::_FinishSimplePacket()
{
	uint future_writepos = (s_WritePos.load(std::memory_order_relaxed) + 1) & RingBufferMask;
	pxAssert(future_writepos != s_ReadPos.load(std::memory_order_acquire));
	s_WritePos.store(future_writepos, std::memory_order_release);

	if (IsDevBuild && EmuConfig.GS.SynchronousMTGS) [[unlikely]]
		WaitGS();
	else
		++s_CopyDataTally;
}

void MTGS::SendSimplePacket(Command type, int data0, int data1, int data2)
{
	//ScopedLock locker( m_PacketLocker );

	GenericStall(1);
	PacketTagType& tag = (PacketTagType&)RingBuffer[s_WritePos.load(std::memory_order_relaxed)];

	tag.command = static_cast<u32>(type);
	tag.data[0] = data0;
	tag.data[1] = data1;
	tag.data[2] = data2;

	_FinishSimplePacket();
}

void MTGS::SendSimpleGSPacket(Command type, u32 offset, u32 size, GIF_PATH path)
{
	SendSimplePacket(type, (int)offset, (int)size, (int)path);

	if (!IsDevBuild || !EmuConfig.GS.SynchronousMTGS) [[likely]]
	{
		s_CopyDataTally += size / 16;
		if (s_CopyDataTally > 0x2000)
			SetEvent();
	}
}

void MTGS::SendPointerPacket(Command type, u32 data0, void* data1)
{
	//ScopedLock locker( m_PacketLocker );

	GenericStall(1);
	PacketTagType& tag = (PacketTagType&)RingBuffer[s_WritePos.load(std::memory_order_relaxed)];

	tag.command = static_cast<u32>(type);
	tag.data[0] = data0;
	tag.pointer = (uptr)data1;

	_FinishSimplePacket();
}

bool MTGS::WaitForOpen()
{
	if (IsOpen())
		return true;

	StartThread();

	// request open, and kick the thread.
	s_open_flag.store(true, std::memory_order_release);
	s_sem_event.NotifyOfWork();

	// wait for it to finish its stuff
	s_open_or_close_done.Wait();

	// did we succeed?
	const bool result = s_open_flag.load(std::memory_order_acquire);
	if (!result)
		Console.Error("GS failed to open.");

	return result;
}

void MTGS::WaitForClose()
{
	if (!IsOpen())
		return;

	// ask the thread to stop processing work, by clearing the open flag
	s_open_flag.store(false, std::memory_order_release);

	// and kick the thread if it's sleeping
	s_sem_event.NotifyOfWork();

	// and wait for it to finish up..
	s_open_or_close_done.Wait();
}

void MTGS::Freeze(FreezeAction mode, MTGS::FreezeData& data)
{
	pxAssertRel(IsOpen(), "GS thread is open");

	// synchronize regs before loading
	if (mode == FreezeAction::Load)
		WaitGS(true);

	SendPointerPacket(Command::Freeze, (int)mode, &data);
	WaitGS(false);
}

void MTGS::RunOnGSThread(AsyncCallType func)
{
	SendPointerPacket(Command::AsyncCall, 0, new AsyncCallType(std::move(func)));

	// wake the gs thread in case it's sleeping
	SetEvent();
}

void MTGS::GameChanged()
{
	pxAssertRel(IsOpen(), "MTGS is running");
	RunOnGSThread(GSGameChanged);
}

void MTGS::ApplySettings()
{
	pxAssertRel(IsOpen(), "MTGS is running");

	RunOnGSThread([opts = EmuConfig.GS]() {
		GSUpdateConfig(opts);
	});

	// We need to synchronize the thread when changing any settings when the download mode
	// is unsynchronized, because otherwise we might potentially read in the middle of
	// the GS renderer being reopened.
	if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
		WaitGS(false, false, false);
}

void MTGS::ResizeDisplayWindow(u32 width, u32 height, float scale)
{
	pxAssertRel(IsOpen(), "MTGS is running");
	RunOnGSThread([width, height, scale]() {
		GSResizeDisplayWindow(width, height, scale);

		// If we're paused, re-present the current frame at the new window size.
		if (VMManager::GetState() == VMState::Paused)
			GSPresentCurrentFrame();
	});
}

void MTGS::UpdateDisplayWindow()
{
	pxAssertRel(IsOpen(), "MTGS is running");
	RunOnGSThread([]() {
		GSUpdateDisplayWindow();

		// If we're paused, re-present the current frame at the new window size.
		if (VMManager::GetState() == VMState::Paused)
		{
			// Hackity hack, on some systems, presenting a single frame isn't enough to actually get it
			// displayed. Two seems to be good enough. Maybe something to do with direct scanout.
			GSPresentCurrentFrame();
			GSPresentCurrentFrame();
		}
	});
}

void MTGS::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	pxAssertRel(IsOpen(), "MTGS is running");

	RunOnGSThread([mode, allow_present_throttle]() { GSSetVSyncMode(mode, allow_present_throttle); });
}

void MTGS::UpdateVSyncMode()
{
	SetVSyncMode(VMManager::GetEffectiveVSyncMode(), VMManager::ShouldAllowPresentThrottle());
}

void MTGS::SetSoftwareRendering(bool software, GSInterlaceMode interlace, bool display_message /* = true */)
{
	pxAssertRel(IsOpen(), "MTGS is running");

	if (display_message)
	{
		Host::AddIconOSDMessage("SwitchRenderer", ICON_FA_WAND_MAGIC_SPARKLES, software ?
			TRANSLATE_STR("GS", "Switching to Software Renderer...") : TRANSLATE_STR("GS", "Switching to Hardware Renderer..."),
			Host::OSD_QUICK_DURATION);
	}

	RunOnGSThread([software, interlace]() {
		GSSetSoftwareRendering(software, interlace);
	});

	// See note in ApplySettings() for reasoning here.
	if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
		WaitGS(false, false, false);
}

void MTGS::ToggleSoftwareRendering()
{
	// reading from the GS thread.. but should be okay here
	SetSoftwareRendering(GSIsHardwareRenderer(), EmuConfig.GS.InterlaceMode);
}

bool MTGS::SaveMemorySnapshot(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
	u32* width, u32* height, std::vector<u32>* pixels)
{
	bool result = false;
	RunOnGSThread([window_width, window_height, apply_aspect, crop_borders, width, height, pixels, &result]() {
		result = GSSaveSnapshotToMemory(window_width, window_height, apply_aspect, crop_borders, width, height, pixels);
	});
	WaitGS(false, false, false);
	return result;
}

void MTGS::PresentCurrentFrame()
{
	if (s_run_idle_flag.load(std::memory_order_relaxed))
	{
		// If we're running idle, we're going to re-present anyway.
		return;
	}

	RunOnGSThread([]() {
		GSPresentCurrentFrame();
	});
}

void MTGS::SetRunIdle(bool enabled)
{
	// NOTE: Should only be called on the GS thread.
	s_run_idle_flag.store(enabled, std::memory_order_release);
}

// Used in MTVU mode... MTVU will later complete a real packet
void Gif_AddGSPacketMTVU(GS_Packet& gsPack, GIF_PATH path)
{
	MTGS::SendSimpleGSPacket(MTGS::Command::MTVUGSPacket, 0, 0, path);
}

void Gif_AddCompletedGSPacket(GS_Packet& gsPack, GIF_PATH path)
{
	//DevCon.WriteLn("Adding Completed Gif Packet [size=%x]", gsPack.size);
	if (COPY_GS_PACKET_TO_MTGS)
	{
		MTGS::PrepDataPacket(path, gsPack.size / 16);
		MemCopy_WrappedDest((u128*)&gifUnit.gifPath[path].buffer[gsPack.offset], MTGS::RingBuffer.m_Ring,
							MTGS::s_packet_writepos, MTGS::RingBufferSize, gsPack.size / 16);
		MTGS::SendDataPacket();
	}
	else
	{
		pxAssertMsg(!gsPack.readAmount, "Gif Unit - gsPack.readAmount only valid for MTVU path 1!");
		gifUnit.gifPath[path].readAmount.fetch_add(gsPack.size);
		MTGS::SendSimpleGSPacket(MTGS::Command::GSPacket, gsPack.offset, gsPack.size, path);
	}
}

void Gif_AddBlankGSPacket(u32 size, GIF_PATH path)
{
	//DevCon.WriteLn("Adding Blank Gif Packet [size=%x]", size);
	gifUnit.gifPath[path].readAmount.fetch_add(size);
	MTGS::SendSimpleGSPacket(MTGS::Command::GSPacket, ~0u, size, path);
}

void Gif_MTGS_Wait(bool isMTVU)
{
	MTGS::WaitGS(false, true, isMTVU);
}

// PS5 port (vk-285-124, AI-assisted): the EE thread's wait for room in a GIF path's buffer (Gif_Path::mtgsReadWait),
// until the GS thread has read the path down to `target` bytes still to read (it gives the bytes back packet by
// packet, MainLoop's GSPacket case), instead of WaitGS's drain of the whole ring. In GTA Liberty City Stories with
// Instant DMA the drains came ~23 times a second, ~11 ms each, and left the GS thread with an empty ring until the
// EE thread caught up (GS idle 16%, EE waiting 26%, fps unchanged at ~48). Spins with pause, then yields. Returns
// false (the caller waits the old way) when the ring is empty before the target is reached, which the accounting
// doesn't allow but which must not spin forever, or when flags/gifwaitfull asks for the old wait (live).
// Counted with WaitGS's waits ([threads] ee_waitgs_ms). Needs proper testing.
extern bool OrbisFlag(const char* name);
bool Gif_MTGS_WaitRead(std::atomic<int>& readAmount, s32 extra, s32 target)
{
	if (!MTGS::IsOpen() || OrbisFlag("gifwaitfull"))
		return false;
	// As WaitGS: a VU1 kick deferred inside a VIF1 transfer goes out first (the GS thread may be waiting for that
	// program's XGKICK before it reaches this path's packets).
	vu1Thread.FlushKick();
	MTGS::SetEvent();
	const unsigned long long t0 = __builtin_ia32_rdtsc();
	OrbisEEWaitScope orbis_wait;
	bool reached = true;
	for (u32 spin = 0; readAmount.load(std::memory_order_acquire) + extra > target; spin++)
	{
		if (MTGS::s_ReadPos.load(std::memory_order_acquire) == MTGS::s_WritePos.load(std::memory_order_relaxed) &&
			readAmount.load(std::memory_order_acquire) + extra > target)
		{
			reached = false;
			break;
		}
		if (spin < 4096)
			_mm_pause();
		else
			std::this_thread::yield();
	}
	g_orbis_ee_waitgs_ticks += __builtin_ia32_rdtsc() - t0;
	++g_orbis_ee_waitgs_n;
	return reached;
}
