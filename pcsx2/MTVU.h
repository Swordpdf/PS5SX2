// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "common/Threading.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "VUmicro.h"

#include <thread>

#define MTVU_LOG(...) do{} while(0)
//#define MTVU_LOG DevCon.WriteLn

// Notes:
// - This class should only be accessed from the EE thread...
// - buffer_size must be power of 2
// - ring-buffer has no complete pending packets when read_pos==write_pos
class VU_Thread final {
	static const s32 buffer_size = (_1mb * 16) / sizeof(s32);

	u32 buffer[buffer_size];
	// Note: keep atomic on separate cache line to avoid CPU conflict
	alignas(__cachelinesize) std::atomic<int> m_ato_read_pos; // Only modified by VU thread
	alignas(__cachelinesize) std::atomic<int> m_ato_write_pos;    // Only modified by EE thread
	alignas(__cachelinesize) int  m_read_pos; // temporary read pos (local to the VU thread)
	u32  m_orbis_rlaps = 0; // vk-285-93: MTVU_NULL_PACKETs the VU thread followed (VU thread only)
	// PS5 port (vk-285-10): each thread's position, and the semaphore both threads update on every
	// packet, get their own cache line. Sharing one, every EE write of m_write_pos and every VU-thread
	// write of m_read_pos or the semaphore state pulled the line across cores: the first load in
	// ReserveSpace stood out in the EE profile (vk-285-9). Needs proper testing.
	alignas(__cachelinesize) int  m_write_pos; // temporary write pos (local to the EE thread)
	// PS5 port (vk-285-84), EE-thread state on m_write_pos's line:
	// - m_cached_read_pos: the last m_ato_read_pos this thread loaded. Within one of this thread's laps
	//   the read position only moves forward, except once back to 0 when the VU thread follows the NULL
	//   packet into this lap (behind the write position: more room), and ReserveSpace reloads it at each
	//   wrap (vk-285-93: corrected; an earlier note here said positions never go back), so an old value
	//   can only under-state the free room: WaitOnSize checks it first and loads the shared line (a
	//   cross-core miss per packet in vk-285-83's SotC profile) only when it says there's no room.
	// - m_defer_kicks / m_kick_pending: inside a VIF1 transfer (Vif_Transfer.cpp) the packets' kicks
	//   wait for its end (FlushKick), so the VU thread gets one wake-up per transfer instead of one
	//   locked add per packet. Every EE wait on the VU or GS thread flushes first.
	int  m_cached_read_pos;
	bool m_defer_kicks;
	bool m_kick_pending;
	// PS5 port (vk-285-90), EE-thread state on the same line:
	// - m_orbis_batch: OrbisMTVUBatch as of the last vsync (OrbisVsyncRefresh), for BeginKickBatch, which
	//   loaded g_orbis_mtvu_batch per VIF1 transfer from a line the VU thread writes per program (its
	//   counters): ~1% of the EE thread in vk-285-89's SotC profile.
	// - m_orbis_ring_limit: where the ring wraps, in u32s (<= buffer_size): OrbisMTVURingKB, taken at a wrap
	//   (the VU thread follows the MTVU_NULL_PACKET, so a lap may be shorter than the buffer).
	bool m_orbis_batch;
	s32  m_orbis_ring_limit;
	// - the [eestat] counts (OrbisEEStats): unpacks handed over, and kicks.
	u64  m_orbis_unpacks = 0; // cumulative (not cleared by Reset)
	u64  m_orbis_kicks = 0;
	// - vk-285-93: wraps (MTVU_NULL_PACKETs written), and the wraps that waited for the VU thread to leave
	//   the lap's first packet (OrbisWaitLapStart). Cumulative.
	u64  m_orbis_wraps = 0;
	u64  m_orbis_lap_waits = 0;
	alignas(__cachelinesize) Threading::WorkSema semaEvent;
	std::atomic_bool m_shutdown_flag{false};

	Threading::Thread m_thread;

public:
	alignas(16)  vifStruct        vif;
	alignas(16)  VIFregisters     vifRegs;
	Threading::UserspaceSemaphore semaXGkick;
	alignas(__cachelinesize) std::atomic<unsigned int> vuCycles[4]; // Used for VU cycle stealing hack (own line: the VU thread writes it per program)
	u32 vuCycleIdx;  // Used for VU cycle stealing hack
	u32 vuFBRST;

	enum InterruptFlag {
		InterruptFlagFinish = 1 << 0,
		InterruptFlagSignal = 1 << 1,
		InterruptFlagLabel  = 1 << 2,
		InterruptFlagVUEBit = 1 << 3,
		InterruptFlagVUTBit = 1 << 4,
	};

	alignas(__cachelinesize) std::atomic<u32> mtvuInterrupts; // Used for GS Signal, Finish etc, plus VU End/T-Bit (own line: the EE polls it)
	std::atomic<u64> gsLabel; // Used for GS Label command
	std::atomic<u64> gsSignal; // Used for GS Signal command

	VU_Thread();
	~VU_Thread();

	__fi const Threading::ThreadHandle& GetThreadHandle() const { return m_thread; }

	/// Returns true if the VU thread has been started.
	__fi bool IsOpen() const { return m_thread.Joinable(); }

	/// Ensures the VU thread is started.
	void Open();

	/// Shuts down the VU thread if it is currently running.
	void Close();

	void Reset();

	// Get MTVU to start processing its packets if it isn't already
	void KickStart();

	// PS5 port (vk-285-84): a producer's kick after it wrote a packet (deferred inside a VIF1
	// transfer), the flush of a deferred one, and the VIF1 transfer's scope. EE thread only.
	void KickAfterWrite();
	void FlushKick();
	bool BeginKickBatch();
	void EndKickBatch(bool was_deferring);

	// PS5 port (vk-285-90): a data packet (unpack, memory or register write) only marks a kick pending; the
	// next program's ExecuteVU kicks for it, as does every wait on the VU thread (WaitVU, WaitOnSize) and on
	// the GS thread (FlushKick). The VU thread can't run anything that needs the data before a program.
	void OrbisDataKick() { m_kick_pending = true; }

	// PS5 port (vk-285-90): once per vsync on the EE thread (Counters.cpp): the live settings it caches.
	void OrbisVsyncRefresh();
	u64 OrbisUnpacks() const { return m_orbis_unpacks; }
	u64 OrbisKicks() const { return m_orbis_kicks; }
	u64 OrbisWraps() const { return m_orbis_wraps; }
	u64 OrbisLapWaits() const { return m_orbis_lap_waits; }

	// Used for assertions...
	bool IsDone();

	// Waits till MTVU is done processing
	void WaitVU();

	void Get_MTVUChanges();

	void ExecuteVU(u32 vu_addr, u32 vif_top, u32 vif_itop, u32 fbrst);

	void VifUnpack(vifStruct& _vif, VIFregisters& _vifRegs, const u8* data, u32 size);

	// Writes to VU's Micro Memory (size in bytes)
	void WriteMicroMem(u32 vu_micro_addr, const void* data, u32 size);

	// Writes to VU's Data Memory (size in bytes)
	void WriteDataMem(u32 vu_data_addr, const void* data, u32 size);

	void WriteVIRegs(REG_VI* viRegs);

	void WriteVFRegs(VECTOR* vfRegs);

	void WriteCol(vifStruct& _vif);

	void WriteRow(vifStruct& _vif);

private:
	void ExecuteRingBuffer();

	void WaitOnSize(s32 size);
	void ReserveSpace(s32 size);
	void OrbisWaitLapStart(s32 null_pos); // vk-285-93
	[[noreturn]] void OrbisBadCommand(u32 tag); // vk-285-93

	s32 GetReadPos();
	s32 GetWritePos();

	u32* GetWritePtr();

	void CommitWritePos();
	void CommitReadPos();

	u32 Read();
	void Read(void* dest, u32 size);
	void ReadRegs(VIFregisters* dest);

	void Write(u32 val);
	void Write(const void* src, u32 size);
	void WriteRegs(VIFregisters* src);

	u32 Get_vuCycles();
};

extern VU_Thread vu1Thread;
