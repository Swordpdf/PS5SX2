// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// vk-285-12 (PS5 port): 16:9 widescreen for Ratchet & Clank PAL (SCES-50916).
//
// A runtime port of ElHecht's widescreen patch (pcsx2_patches, SCES-50916_76F724A3.pnach).
// That pnach targets another revision of the disc (ELF CRC 76F724A3); ours is 6A8F18B9.
// The game loads its camera and vendor code with each level, at addresses that move
// by different amounts between the two revisions, so the per-level addresses of the pnach
// cannot be reused. Instead this finds the same code by its instruction patterns in the
// loaded level code and applies the pnach's hooks there:
//
//   inject  0x0ffef4..0x0fff78  the pnach's FOV function (free RAM below the ELF)
//   H       FOV hook: c46000b0 46010002 (lwc1 f0,0xb0(v1); mul.s f0,f0,f1), with the
//           halfword 0xcaff at H-0x40 (the pnach's level check) -> j 0x0ffef4; nop,
//           and 0x0fff7c = j H+4
//   D       H-0x158: lui at,0x3f21; ori at,at,0x47ae (the gameplay FOV constant), which
//           the vendor hooks rewrite to 0x47af (4:3 marker) while the vendor is open
//   VO      vendor open: 24120001 [ae220054 ae200058] -> j 0x0fff84; nop
//   VC      vendor close: [24050001 8c46001c] 24030008 -> j 0x0fffb0; nop
//   M       menu FOV: 3c013f21 [342147ae] 44810000 xxxxxxxx e44000b0 -> 342147af
//
// Runs on the EE thread at every vsync (Host::PumpMessagesOnCPUThread), writes through
// memWrite32 like PCSX2's own patch engine (recompiled blocks on a written page are
// dropped by the page protection), and only touches a site whose pattern matches exactly
// once. The display switches to 16:9 while the FOV hook is in place and back to 4:3
// when no level code is loaded (logos, loading screens). live.ini widescreen=0 undoes it.

#include "Config.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "R5900.h"
#include "VMManager.h"

#include <atomic>
#include <cstdio>
#include <cstring>

std::atomic<int> g_orbis_widescreen{1}; // live.ini widescreen= (written on the GS thread)
std::atomic<int> g_orbis_ws_active{0}; // 1 while the 16:9 FOV hook is in place

namespace
{
	constexpr u32 CRC_THIS_DISC = 0x6A8F18B9u;
	constexpr u32 CRC_PNACH_DISC = 0x76F724A3u;
	// vk-285-63: Ratchet & Clank USA (SCUS-97199). pcsx2_patches' SCUS-97199_CE4933D0.pnach
	// (PsxFan107) hooks the same FOV instructions (c46000b0 46010002) and uses the same 4:3 marker
	// (342147af), but it is disabled there ("appears to break rendering of textures in some
	// areas"). This disc gets the PAL port's hooks, found by the same patterns; its level code may
	// load elsewhere, so it is searched over a wider range, and D is looked for near H when it is
	// not at H-0x158.
	constexpr u32 CRC_USA_DISC = 0xCE4933D0u;
	constexpr u32 USA_LO = 0x00100000u, USA_HI = 0x00380000u;

	bool IsRatchetDisc(u32 crc) { return crc == CRC_THIS_DISC || crc == CRC_PNACH_DISC || crc == CRC_USA_DISC; }

	constexpr u32 INJ = 0x000ffef4u; // FOV function
	constexpr u32 INJ_RET = 0x000fff7cu; // j H+4; nop
	constexpr u32 VO_INJ = 0x000fff84u; // vendor open: 9 words
	constexpr u32 VC_INJ = 0x000fffb0u; // vendor close: 9 words
	constexpr u32 INJ_END = 0x000fffd4u;

	constexpr u32 H_LO = 0x001a0000u, H_HI = 0x00260000u; // level camera code
	constexpr u32 O_LO = 0x00220000u, O_HI = 0x00300000u; // level vendor/menu code

	constexpr u32 H_ORIG0 = 0xc46000b0u, H_ORIG1 = 0x46010002u;
	constexpr u32 VO_PREV = 0x24120001u, VO_ORIG0 = 0xae220054u, VO_ORIG1 = 0xae200058u;
	constexpr u32 VC_ORIG0 = 0x24050001u, VC_ORIG1 = 0x8c46001cu, VC_NEXT = 0x24030008u;
	constexpr u32 FOV_LUI = 0x3c013f21u, FOV_ORI = 0x342147aeu, FOV_ORI_43 = 0x342147afu;
	constexpr u32 M_NEXT = 0x44810000u, M_NEXT3 = 0xe44000b0u;

	constexpr u32 J(u32 target) { return 0x08000000u | ((target >> 2) & 0x03ffffffu); }

	// 0x0ffef4..0x0fff78, verbatim from the pnach.
	constexpr u32 s_inject[34] = {
		0x3c013f40, 0x34210001, 0x4481f000, // f30 = 0.75 (4:3 -> 16:9)
		0x3c013f21, 0x342147af, 0x8c7900b0, 0x14390007, 0x00000000, // FOV marked 4:3 (menu, vendor, hud)
		0x4481f800, 0xe47f00b0, 0x461e0843, 0xc46000b0, 0x46010003, 0xe46000b4,
		0x3c013f21, 0x342147ae, 0x8c7900b0, 0x14390004, 0x00000000, // gameplay FOV
		0x4481f800, 0x461effc3, 0xe47f00b0,
		0x3c013ed4, 0x34210674, 0x8c7900b0, 0x14390004, 0x00000000, // cut-scene FOV
		0x4481f800, 0x461effc3, 0xe47f00b0,
		0x461e0842, 0xc46000b0, 0x46010002, 0xe46000b4, // the hooked instructions
	};
	static_assert(INJ + 4 * (sizeof(s_inject) / sizeof(s_inject[0])) == INJ_RET);

	struct Sites
	{
		u32 H, VO, VC, M;
	};
	Sites s_sites{};
	u32 s_crc = 0;
	u32 s_d = 0; // the gameplay FOV constant the vendor hooks rewrite (H-0x158 on the PAL discs)
	u32 s_h_lo = H_LO, s_h_hi = H_HI, s_o_lo = O_LO, s_o_hi = O_HI; // search ranges for this disc
	bool s_broken = false; // the inject area holds something else: never patch this boot
	bool s_wide = false;
	int s_lost = 0; // scans since the FOV hook was last in place
	int s_vendor_backoff = 0;
	u32 s_logged_h = ~0u;

	u32 R(u32 addr)
	{
		u32 v;
		std::memcpy(&v, eeMem->Main + addr, sizeof(v));
		return v;
	}

	void W(u32 addr, u32 value)
	{
		if (R(addr) != value)
			memWrite32(addr, value);
	}

	u32 Hi(u32 addr) { return ((addr + 0x8000u) >> 16) & 0xffffu; }
	u32 Lo(u32 addr) { return addr & 0xffffu; }

	void VendorWords(u32 ori, u32 d, u32 orig0, u32 orig1, u32 ret, u32 out[9])
	{
		out[0] = 0x3c013421u; // lui at,0x3421
		out[1] = ori; // ori at,at,0x47af / 0x47ae  -> at = that instruction word
		out[2] = 0x4481f000u; // mtc1 at,f30
		out[3] = 0x3c010000u | Hi(d); // lui at,hi(D)
		out[4] = 0xe43e0000u | Lo(d); // swc1 f30,lo(D)(at): rewrite the FOV constant at D
		out[5] = orig0;
		out[6] = orig1;
		out[7] = J(ret);
		out[8] = 0;
	}

	bool InjectIntact()
	{
		for (u32 i = 0; i < sizeof(s_inject) / sizeof(s_inject[0]); i++)
		{
			if (R(INJ + i * 4) != s_inject[i])
				return false;
		}
		return true;
	}

	// The area is free if every word is zero or one this code writes there.
	bool InjectAreaFree()
	{
		for (u32 a = INJ; a < INJ_END; a += 4)
		{
			const u32 v = R(a);
			if (v == 0)
				continue;
			if (a < INJ_RET)
			{
				if (v != s_inject[(a - INJ) / 4])
					return false;
				continue;
			}
			const u32 op = v >> 26;
			if (op == 2 || (v & 0xffff0000u) == 0x3c010000u || (v & 0xffff0000u) == 0xe43e0000u ||
				v == 0x3c013421u || v == 0x342147afu || v == 0x342147aeu || v == 0x4481f000u || v == VO_ORIG0 ||
				v == VO_ORIG1 || v == VC_ORIG0 || v == VC_ORIG1)
				continue;
			return false;
		}
		return true;
	}

	bool HMatch(u32 a, bool patched)
	{
		if ((R(a - 0x40) & 0xffffu) != 0xcaffu)
			return false;
		return patched ? (R(a) == J(INJ) && R(a + 4) == 0) : (R(a) == H_ORIG0 && R(a + 4) == H_ORIG1);
	}

	bool VOMatch(u32 a, bool patched)
	{
		if (R(a - 4) != VO_PREV)
			return false;
		return patched ? (R(a) == J(VO_INJ) && R(a + 4) == 0) : (R(a) == VO_ORIG0 && R(a + 4) == VO_ORIG1);
	}

	bool VCMatch(u32 a, bool patched)
	{
		if (R(a + 8) != VC_NEXT)
			return false;
		return patched ? (R(a) == J(VC_INJ) && R(a + 4) == 0) : (R(a) == VC_ORIG0 && R(a + 4) == VC_ORIG1);
	}

	bool MContext(u32 a)
	{
		return R(a - 4) == FOV_LUI && R(a + 4) == M_NEXT && R(a + 12) == M_NEXT3;
	}

	bool DValid(u32 d)
	{
		return R(d - 4) == FOV_LUI && (R(d) == FOV_ORI || R(d) == FOV_ORI_43);
	}

	// Returns the address if exactly one site in [lo, hi) matches (patched or not), else 0.
	template <typename F>
	u32 FindUnique(u32 lo, u32 hi, F&& match, int* count)
	{
		u32 found = 0;
		int n = 0;
		for (u32 a = lo; a < hi; a += 4)
		{
			if (match(a))
			{
				found = a;
				if (++n > 1)
					break;
			}
		}
		*count = n;
		return (n == 1) ? found : 0;
	}

	bool PcNear(u32 a, u32 len)
	{
		const u32 pc = cpuRegs.pc & 0x1fffffffu;
		return pc >= a && pc < a + len;
	}

	void SetWide(bool on)
	{
		const float want = on ? (16.0f / 9.0f) : 0.0f;
		if (EmuConfig.CurrentCustomAspectRatio != want)
			EmuConfig.CurrentCustomAspectRatio = want;
		if (s_wide != on)
		{
			s_wide = on;
			g_orbis_ws_active.store(on ? 1 : 0, std::memory_order_release);
			printf("[ws] display %s (aspect setting %d)\n", on ? "16:9" : "4:3", static_cast<int>(EmuConfig.CurrentAspectRatio));
			fflush(stdout);
		}
	}

	// Put back the original instructions wherever our hooks are still in place.
	void Unpatch()
	{
		const Sites s = s_sites;
		if (s.H && HMatch(s.H, true) && !PcNear(s.H, 8))
		{
			W(s.H, H_ORIG0);
			W(s.H + 4, H_ORIG1);
		}
		if (s.VO && VOMatch(s.VO, true) && !PcNear(s.VO, 8))
		{
			W(s.VO, VO_ORIG0);
			W(s.VO + 4, VO_ORIG1);
		}
		if (s.VC && VCMatch(s.VC, true) && !PcNear(s.VC, 8))
		{
			W(s.VC, VC_ORIG0);
			W(s.VC + 4, VC_ORIG1);
		}
		if (s.M && MContext(s.M) && R(s.M) == FOV_ORI_43)
			W(s.M, FOV_ORI);
		if (s.H || s.VO || s.VC || s.M)
		{
			printf("[ws] hooks removed (H %06x VO %06x VC %06x M %06x)\n", s.H, s.VO, s.VC, s.M);
			fflush(stdout);
		}
		s_sites = {};
	}

	bool BlockIs(u32 base, const u32 (&words)[9])
	{
		for (u32 i = 0; i < 9; i++)
		{
			if (R(base + i * 4) != words[i])
				return false;
		}
		return true;
	}

	// vk-285-63: D at H-0x158 (both PAL discs), or the one lui/ori pair of the gameplay FOV within
	// 0x800 bytes of H.
	u32 FindD(u32 h)
	{
		if (DValid(h - 0x158))
			return h - 0x158;
		int n = 0;
		const u32 d = FindUnique(h > 0x800u ? h - 0x800u : 4u, h + 0x800u, [](u32 a) { return DValid(a); }, &n);
		return d;
	}

	void PatchVendorSites()
	{
		const u32 d = s_d;
		if (!d || !DValid(d))
			return;

		u32 vo_words[9] = {}, vc_words[9] = {};
		if (s_sites.VO)
			VendorWords(FOV_ORI_43, d, VO_ORIG0, VO_ORIG1, s_sites.VO + 4, vo_words);
		if (s_sites.VC)
			VendorWords(FOV_ORI, d, VC_ORIG0, VC_ORIG1, s_sites.VC + 4, vc_words);
		const bool vo_ok = s_sites.VO && VOMatch(s_sites.VO, true) && BlockIs(VO_INJ, vo_words);
		const bool vc_ok = s_sites.VC && VCMatch(s_sites.VC, true) && BlockIs(VC_INJ, vc_words);
		const bool m_ok = s_sites.M && MContext(s_sites.M) && R(s_sites.M) == FOV_ORI_43;
		if (vo_ok && vc_ok && m_ok)
			return;
		if (s_vendor_backoff > 0)
		{
			s_vendor_backoff--;
			return;
		}

		int nvo = 1, nvc = 1, nm = 1;
		const u32 vo = vo_ok ? s_sites.VO :
							   FindUnique(s_o_lo + 4, s_o_hi, [](u32 a) { return VOMatch(a, false) || VOMatch(a, true); }, &nvo);
		const u32 vc = vc_ok ? s_sites.VC :
							   FindUnique(s_o_lo, s_o_hi, [](u32 a) { return VCMatch(a, false) || VCMatch(a, true); }, &nvc);
		const u32 m = m_ok ? s_sites.M :
							 FindUnique(s_o_lo + 4, s_o_hi, [d](u32 a) {
								 return a != d && MContext(a) && (R(a) == FOV_ORI || R(a) == FOV_ORI_43);
							 }, &nm);

		// Inject words first, then the jump into them.
		if (vo && !vo_ok && !PcNear(VO_INJ, 36) && !PcNear(vo, 8))
		{
			VendorWords(FOV_ORI_43, d, VO_ORIG0, VO_ORIG1, vo + 4, vo_words);
			for (u32 i = 0; i < 9; i++)
				W(VO_INJ + i * 4, vo_words[i]);
			W(vo, J(VO_INJ));
			W(vo + 4, 0);
			s_sites.VO = vo;
		}
		if (vc && !vc_ok && !PcNear(VC_INJ, 36) && !PcNear(vc, 8))
		{
			VendorWords(FOV_ORI, d, VC_ORIG0, VC_ORIG1, vc + 4, vc_words);
			for (u32 i = 0; i < 9; i++)
				W(VC_INJ + i * 4, vc_words[i]);
			W(vc, J(VC_INJ));
			W(vc + 4, 0);
			s_sites.VC = vc;
		}
		if (m && !m_ok)
		{
			W(m, FOV_ORI_43);
			s_sites.M = m;
		}

		static u32 s_last_log[7] = {};
		const u32 cur[7] = {d, vo, static_cast<u32>(nvo), vc, static_cast<u32>(nvc), m, static_cast<u32>(nm)};
		if (std::memcmp(cur, s_last_log, sizeof(cur)) != 0)
		{
			std::memcpy(s_last_log, cur, sizeof(cur));
			printf("[ws] vendor/menu hooks: D %06x VO %06x (%d) VC %06x (%d) M %06x (%d)\n", d, vo, nvo, vc, nvc, m, nm);
			fflush(stdout);
		}
		if (!vo || !vc || !m)
			s_vendor_backoff = 8; // not loaded yet or not unique: look again in ~2 s
	}
} // namespace

void OrbisWidescreenTick()
{
	const u32 crc = VMManager::GetCurrentCRC();
	if (crc != s_crc)
	{
		s_crc = crc;
		s_sites = {};
		s_broken = false;
		s_lost = 0;
		s_vendor_backoff = 0;
		s_logged_h = ~0u;
		if (s_wide)
			SetWide(false);
		s_d = 0;
		const bool usa = crc == CRC_USA_DISC;
		s_h_lo = usa ? USA_LO : H_LO;
		s_h_hi = usa ? USA_HI : H_HI;
		s_o_lo = usa ? USA_LO : O_LO;
		s_o_hi = usa ? USA_HI : O_HI;
		if (IsRatchetDisc(crc))
		{
			printf("[ws] Ratchet & Clank %s (CRC %08X): 16:9 patch %s\n", usa ? "USA" : "PAL", crc,
				g_orbis_widescreen.load(std::memory_order_relaxed) ? "on" : "off (live.ini)");
			fflush(stdout);
		}
	}
	if (!IsRatchetDisc(crc))
		return;
	if (!eeMem)
		return;

	if (g_orbis_widescreen.load(std::memory_order_relaxed) == 0)
	{
		if (s_sites.H || s_sites.VO || s_sites.VC || s_sites.M)
			Unpatch();
		if (s_wide)
			SetWide(false);
		return;
	}
	if (s_broken)
		return;

	// Every vsync: the hooks jump into the inject area, so it must still hold our code.
	if (s_sites.H && !InjectIntact())
	{
		if (InjectAreaFree() && !PcNear(INJ, INJ_END - INJ))
		{
			for (u32 i = 0; i < sizeof(s_inject) / sizeof(s_inject[0]); i++)
				W(INJ + i * 4, s_inject[i]);
			printf("[ws] inject function rewritten\n");
		}
		else
		{
			Unpatch();
			s_broken = true;
			SetWide(false);
			printf("[ws] inject area 0x%06x taken by the game: widescreen off\n", INJ);
		}
		fflush(stdout);
		return;
	}

	static unsigned s_tick = 0;
	if ((s_tick++ % 15) != 0)
	{
		if (s_wide && EmuConfig.CurrentCustomAspectRatio != 16.0f / 9.0f)
			EmuConfig.CurrentCustomAspectRatio = 16.0f / 9.0f; // a settings reload clears it
		return;
	}
	if (PcNear(INJ, INJ_END - INJ))
		return;

	const bool h_ok = s_sites.H && HMatch(s_sites.H, true) && R(INJ_RET) == J(s_sites.H + 4) && R(INJ_RET + 4) == 0;
	if (!h_ok)
	{
		int n = 0;
		const u32 h = FindUnique(s_h_lo + 0x40, s_h_hi, [](u32 a) { return HMatch(a, false) || HMatch(a, true); }, &n);
		if (h && !PcNear(h, 8))
		{
			if (!InjectIntact())
			{
				if (!InjectAreaFree())
				{
					s_broken = true;
					printf("[ws] inject area 0x%06x is in use by this revision: widescreen off\n", INJ);
					fflush(stdout);
					return;
				}
				for (u32 i = 0; i < sizeof(s_inject) / sizeof(s_inject[0]); i++)
					W(INJ + i * 4, s_inject[i]);
			}
			W(INJ_RET, J(h + 4));
			W(INJ_RET + 4, 0);
			W(h, J(INJ));
			W(h + 4, 0);
			if (s_sites.H != h)
			{
				s_sites.VO = s_sites.VC = s_sites.M = 0; // another level: find its vendor code again
				s_vendor_backoff = 0;
			}
			s_sites.H = h;
			s_d = FindD(h);
			printf("[ws] FOV hook at %06x (D %06x %s)\n", h, s_d, s_d == 0 ? "not found" : s_d == h - 0x158 ? "ok" : "ok, moved");
			fflush(stdout);
			s_logged_h = h;
		}
		else
		{
			if (n != 0 && s_logged_h != 0)
			{
				printf("[ws] FOV hook pattern matched %d times: not patching\n", n);
				fflush(stdout);
				s_logged_h = 0;
			}
			s_sites.H = 0;
		}
	}

	if (s_sites.H)
	{
		s_lost = 0;
		SetWide(true);
		PatchVendorSites();
	}
	else if (s_wide && ++s_lost >= 8)
	{
		SetWide(false); // no level code for ~2.5 s (logos, loading)
	}
}
