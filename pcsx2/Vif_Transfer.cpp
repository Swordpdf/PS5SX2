// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"
#include "MTVU.h" // vk-285-84: VU_Thread kick batching
#include "OrbisEEDiag.h" // vk-285-100

//------------------------------------------------------------------
// VifCode Transfer Interpreter (Vif0/Vif1)
//------------------------------------------------------------------

// Interprets packet
_vifT void vifTransferLoop(u32* &data) {
	vifStruct& vifX = GetVifX;

	u32& pSize = vifX.vifpacketsize;

	int ret = 0;

	vifXRegs.stat.VPS |= VPS_TRANSFERRING;
	vifXRegs.stat.ER1  = false;
	//VIF_LOG("Starting VIF%d loop, pSize = %x, stalled = %x", idx, pSize, vifX.vifstalled.enabled );
	while (pSize > 0 && !vifX.vifstalled.enabled) {

		if(!vifX.cmd) { // Get new VifCode

			if(!vifXRegs.err.MII)
			{
				if(vifX.irq && !CHECK_VIF1STALLHACK)
					break;

				vifX.irq      |= data[0] >> 31;
			}

			vifXRegs.code = data[0];
			vifX.cmd	  = data[0] >> 24;
			if (idx)
				ORBIS_EEDIAG(OrbisEEDiag::vif1_codes[vifX.cmd & 0x7f]++); // PS5 port (vk-285-100)


			VIF_LOG("New VifCMD %x tagsize %x irq %d", vifX.cmd, vifX.tag.size, vifX.irq);
			if (IsDevBuild && TraceLogging.EE.VIFcode.IsActive()) {
				// Pass 2 means "log it"
				vifCmdHandler[idx][vifX.cmd & 0x7f](2, data);
			}
		}

		// PS5 port (vk-285-96): a NOP (code 0, no IRQ bit) without the handler table's indirect call:
		// vifCode_Nop's pass 1, step for step. Shadow of the Colossus's VIF1 lists have enough of them
		// that the handler was ~1.7% of the EE thread at its heaviest view (vk-285-95's profile).
		if (vifX.cmd == 0 && vifX.pass == 0)
		{
			if (vifX.queued_program)
				vifExecQueue(idx);
			if (pSize > 1 && ((data[1] >> 24) & 0x7f) == 0x6 && (data[1] & 0x1)) // is mskpath3 next
			{
				vifX.vifstalled.enabled = VifStallEnable(vifXch);
				vifX.vifstalled.value = VIF_TIMING_BREAK;
			}
			data  += 1;
			pSize -= 1;
			continue;
		}

		// PS5 port (vk-285-102): the simple register codes too (pass 1 of vifCode_STMod, _STCycl, _Base, _Offset
		// and _ITop, step for step), without the handler table's indirect call. Shadow of the Colossus's heavy
		// view, per second: STMOD 1.54M, STCYCL 0.43M, BASE and OFFSET 0.18M each (of 11.6M VIF1 codes,
		// vk-285-101's [vifcodes]). IRQ-bit forms (cmd | 0x80) still take the table. flags/vifslow turns
		// these off, live. Needs proper testing.
		if (vifX.pass == 0 && vifX.cmd <= 0x05 && g_orbis_vif_fast.load(std::memory_order_relaxed))
		{
			bool done = true;
			switch (vifX.cmd)
			{
				case 0x05: // STMOD
					vifXRegs.mode = vifXRegs.code & 0x3;
					break;
				case 0x01: // STCYCL (whole-word store, as vk-285-99's vifCode_STCycl)
					*reinterpret_cast<u32*>(&vifXRegs.cycle) = vifXRegs.code & 0xffffu;
					break;
				case 0x03: // BASE (VIF1 only)
					if (!idx)
						done = false;
					else
						vif1Regs.base = vif1Regs.code & 0x3ff;
					break;
				case 0x02: // OFFSET (VIF1 only)
					if (!idx)
						done = false;
					else
					{
						vif1Regs.stat.DBF = false;
						vif1Regs.ofst = vif1Regs.code & 0x3ff;
						vif1Regs.tops = vif1Regs.base;
					}
					break;
				case 0x04: // ITOP
					vifXRegs.itops = vifXRegs.code & 0x3ff;
					break;
				default:
					done = false;
					break;
			}
			if (done)
			{
				vifX.cmd = 0;
				data += 1;
				pSize -= 1;
				continue;
			}
		}

		ret = vifCmdHandler[idx][vifX.cmd & 0x7f](vifX.pass, data);
		data   += ret;
		pSize  -= ret;
		if (vifX.vifstalled.enabled)
		{
			int current_STR = idx ? vif1ch.chcr.STR : vif0ch.chcr.STR;
			if (!current_STR)
				DevCon.Warning("Warning! VIF%d stalled during FIFO transfer!", idx);
		}
	}
}

_vifT static __fi bool vifTransfer(u32 *data, int size, bool TTE) {
	vifStruct& vifX = GetVifX;

	// PS5 port (vk-285-84): with EmuCore/Speedhacks/OrbisMTVUBatch on, the MTVU packets this transfer
	// writes (unpacks, MSCAL/MSCNT, STROW...) wake the VU thread once, at the end, instead of each doing
	// a locked add on the semaphore state (MTVU.h, m_defer_kicks). Needs proper testing.
	struct OrbisKickBatch
	{
		bool active = false, was = false;
		~OrbisKickBatch() { if (active) vu1Thread.EndKickBatch(was); }
	} orbis_batch;
	if (idx && THREAD_VU1)
	{
		orbis_batch.was = vu1Thread.BeginKickBatch();
		orbis_batch.active = true;
	}

	// irqoffset necessary to add up the right qws, or else will spin (spiderman)
	int transferred = vifX.irqoffset.enabled ? vifX.irqoffset.value : 0;

	vifX.vifpacketsize = size;
	vifTransferLoop<idx>(data);

	transferred += size - vifX.vifpacketsize;

	//Make this a minimum of 1 cycle so if it's the end of the packet it doesnt just fall through.
	//Metal Saga can do this, just to be safe :)
	if (!idx) g_vif0Cycles += std::max(1, (int)((transferred * BIAS) >> 2));
	else	  g_vif1Cycles += std::max(1, (int)((transferred * BIAS) >> 2));

	vifX.irqoffset.value = transferred % 4; // cannot lose the offset

	if (vifX.irq && vifX.cmd == 0) {
		VIF_LOG("Vif%d IRQ Triggering", idx);
		//Always needs to be set to return to the correct offset if there is data left.
		vifX.vifstalled.enabled = VifStallEnable(vifXch);
		vifX.vifstalled.value = VIF_IRQ_STALL;
	}

	if (!TTE) // *WARNING* - Tags CAN have interrupts! so lets just ignore the dma modifying stuffs (GT4)
	{
		transferred  = transferred >> 2;
		transferred = std::min((int)vifXch.qwc, transferred);
		vifXch.madr +=(transferred << 4);
		vifXch.qwc  -= transferred;

		hwDmacSrcTadrInc(vifXch);

		vifX.irqoffset.enabled = false;

		if(!vifXch.qwc)
			vifX.inprogress &= ~0x1;
		else if (vifX.irqoffset.value != 0)
			vifX.irqoffset.enabled = true;
	}
	else
	{
		if(vifX.irqoffset.value != 0){
			vifX.irqoffset.enabled = true;
		}else
			vifX.irqoffset.enabled = false;
	}

	vifExecQueue(idx);

	return !vifX.vifstalled.enabled;
}

// When TTE is set to 1, MADR and QWC are not updated as part of the transfer.
bool VIF0transfer(u32 *data, int size, bool TTE) {
	return vifTransfer<0>(data, size, TTE);
}
bool VIF1transfer(u32 *data, int size, bool TTE) {
	return vifTransfer<1>(data, size, TTE);
}
