// PS5SX2 (vk-285-140, AI-assisted): USB controllers the PS5 itself doesn't take as controllers, decoded: a PS3-mode USB
// guitar (Guitar Hero / Rock Band, or a modded guitar set to act as a PS3 controller), a generic PS3 USB pad, a DualShock 3
// on a cable, and XInput (Xbox 360) pads and guitars. Header-only and free of the console's APIs, so tests/usbpad runs it on
// a PC. ProsperoUsbPad.cpp opens the devices and feeds the reports here.
//
// Every report becomes one common state in PS3 terms: 13 buttons numbered as PS3 USB pads number them (1 Square, 2 Cross,
// 3 Circle, 4 Triangle, 5 L1, 6 R1, 7 L2, 8 R2, 9 Select, 10 Start, 11 L3, 12 R3, 13 PS), a hat (0 up .. 7 up-left, 8 none)
// and four axes 0..255 (the right stick is a guitar's whammy). Then, by role:
//   pad:    the DualSense's ScePad button bits, triggers and sticks, so the pad thread merges it like the keyboard's controller;
//   guitar: frets, strum, whammy, star power, Start and tilt, for PCSX2's Guitar controller on PS2 port 1. PS3 guitars put
//           the frets on Cross (green), Circle (red), Square (yellow), Triangle (blue) and L1 (orange), strum on the hat's
//           up and down, the whammy on the right stick's X and star power on Select; XInput guitars use A, B, X, Y, LB, the
//           D-pad, the right stick's X (whammy) and Y (tilt).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace orbis_usbpad
{
	// ---- HID report descriptors: the input fields, where each sits in its report --------------------------------------------

	struct HidField
	{
		uint16_t page = 0, usage = 0;
		uint8_t report_id = 0;
		uint32_t bit = 0, size = 0; // offset after the report ID byte, and width, in bits
		int32_t lmin = 0, lmax = 0;
	};

	struct HidLayout
	{
		bool ok = false;
		bool has_ids = false;
		bool gamepad = false;  // a top collection of Generic Desktop Joystick (4) or Game Pad (5)
		bool keyboard = false; // ... Keyboard (6) or Mouse (2): those stay with the keyboard and mouse code
		std::vector<HidField> fields; // input fields with a usage (padding left out)
	};

	inline uint32_t HidUnsigned(const uint8_t* p, int n)
	{
		uint32_t v = 0;
		for (int i = 0; i < n; i++)
			v |= static_cast<uint32_t>(p[i]) << (8 * i);
		return v;
	}

	inline int32_t HidSigned(const uint8_t* p, int n)
	{
		const uint32_t v = HidUnsigned(p, n);
		if (n == 1) return static_cast<int8_t>(v);
		if (n == 2) return static_cast<int16_t>(v);
		return static_cast<int32_t>(v);
	}

	// The input items of a report descriptor (HID 1.11 section 6.2.2). Output and feature items only advance nothing:
	// they live in other reports. Push/Pop are followed; long items are skipped.
	inline HidLayout ParseHid(const uint8_t* d, size_t n)
	{
		HidLayout out;
		struct Globals
		{
			uint16_t page = 0;
			int32_t lmin = 0, lmax = 0;
			uint32_t size = 0, count = 0;
			uint8_t id = 0;
		} g;
		std::vector<Globals> stack;
		std::vector<uint32_t> usages; // local: page << 16 | usage when given with a page, else usage
		uint32_t umin = 0, umax = 0;
		bool have_range = false;
		uint32_t offset[256] = {};
		int depth = 0;
		size_t i = 0;
		while (i < n)
		{
			const uint8_t prefix = d[i];
			if (prefix == 0xFE) // long item
			{
				if (i + 2 >= n)
					break;
				i += 3 + d[i + 1];
				continue;
			}
			int len = prefix & 3;
			if (len == 3)
				len = 4;
			if (i + 1 + len > n)
				break;
			const uint8_t* data = d + i + 1;
			const uint8_t tag = prefix & 0xFC;
			const uint32_t u = HidUnsigned(data, len);
			switch (tag)
			{
				// main
				case 0x80: // Input
				{
					const bool constant = (u & 1) != 0;
					const bool variable = (u & 2) != 0;
					uint32_t& off = offset[g.id];
					// A report is at most 1 KiB here and a field at most 32 bits: a descriptor that says more is broken.
					if (g.size == 0 || g.size > 32 || g.count > 1024 || off + g.size * g.count > 8192)
					{
						usages.clear();
						have_range = false;
						break;
					}
					for (uint32_t k = 0; k < g.count; k++)
					{
						uint32_t usage = 0;
						if (!usages.empty())
							usage = usages[std::min<size_t>(k, usages.size() - 1)];
						else if (have_range)
							usage = std::min(umin + k, umax);
						if (!constant && variable && (usage != 0 || have_range))
						{
							HidField f;
							f.page = (usage >> 16) ? static_cast<uint16_t>(usage >> 16) : g.page;
							f.usage = static_cast<uint16_t>(usage & 0xFFFF);
							f.report_id = g.id;
							f.bit = off;
							f.size = g.size;
							f.lmin = g.lmin;
							f.lmax = g.lmax;
							out.fields.push_back(f);
						}
						off += g.size;
					}
					usages.clear();
					have_range = false;
					break;
				}
				case 0x90: // Output
				case 0xB0: // Feature
					usages.clear();
					have_range = false;
					break;
				case 0xA0: // Collection
					if (depth == 0 && u == 1 /* application */)
					{
						const uint32_t usage = !usages.empty() ? (usages[0] & 0xFFFF) : 0;
						if (g.page == 1 && (usage == 4 || usage == 5))
							out.gamepad = true;
						if (g.page == 1 && (usage == 2 || usage == 6))
							out.keyboard = true;
					}
					depth++;
					usages.clear();
					have_range = false;
					break;
				case 0xC0: // End Collection
					if (depth > 0)
						depth--;
					break;
				// global
				case 0x04: g.page = static_cast<uint16_t>(u); break;
				case 0x14: g.lmin = HidSigned(data, len); break;
				case 0x24: g.lmax = len ? HidSigned(data, len) : 0; break;
				case 0x74: g.size = u; break;
				case 0x84:
					g.id = static_cast<uint8_t>(u);
					out.has_ids = true;
					break;
				case 0x94: g.count = u; break;
				case 0xA4: stack.push_back(g); break;
				case 0xB4:
					if (!stack.empty())
					{
						g = stack.back();
						stack.pop_back();
					}
					break;
				// local
				case 0x08: usages.push_back(len == 4 ? u : (u & 0xFFFF)); break;
				case 0x18:
					umin = len == 4 ? (u & 0xFFFF) : u;
					have_range = true;
					break;
				case 0x28: umax = len == 4 ? (u & 0xFFFF) : u; break;
				default: break;
			}
			i += 1 + len;
		}
		// A logical maximum that reads negative as a signed byte (0x26 0xFF 0x00 is 255; 0x25 0xFF alone is -1): taken
		// as unsigned when the minimum is not negative, as most parsers do.
		for (HidField& f : out.fields)
			if (f.lmin >= 0 && f.lmax < f.lmin && f.size > 0 && f.size < 32)
				f.lmax = static_cast<int32_t>((1u << f.size) - 1);
		out.ok = !out.fields.empty();
		return out;
	}

	inline uint32_t ExtractBits(const uint8_t* r, size_t n, uint32_t bit, uint32_t size)
	{
		uint32_t v = 0;
		for (uint32_t k = 0; k < size && k < 32; k++)
		{
			const uint32_t b = bit + k;
			if (b / 8 >= n)
				break;
			v |= static_cast<uint32_t>((r[b / 8] >> (b % 8)) & 1u) << k;
		}
		return v;
	}

	// ---- The common state, in PS3 terms --------------------------------------------------------------------------------------

	enum Ps3Button : uint16_t
	{
		B_SQUARE = 1u << 0,
		B_CROSS = 1u << 1,
		B_CIRCLE = 1u << 2,
		B_TRIANGLE = 1u << 3,
		B_L1 = 1u << 4,
		B_R1 = 1u << 5,
		B_L2 = 1u << 6,
		B_R2 = 1u << 7,
		B_SELECT = 1u << 8,
		B_START = 1u << 9,
		B_L3 = 1u << 10,
		B_R3 = 1u << 11,
		B_PS = 1u << 12,
	};

	struct Common
	{
		uint16_t buttons = 0;
		uint8_t hat = 8; // 0 up, 1 up-right, 2 right ... 7 up-left, 8 none
		uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
		uint8_t l2 = 0, r2 = 0; // analog triggers where the device has them (else from the buttons)
		bool tilt = false;      // XInput guitars: the right stick's Y past half
	};

	enum class Format : uint8_t
	{
		None,
		Hid,      // a HID gamepad read through its report descriptor, PS3 button numbering
		Ps3Fixed, // a PS3 USB pad without a descriptor to read: buttons in bytes 0-1, hat in 2, sticks in 3-6
		Ds3,      // Sony's DualShock 3 (054c:0268), report 1
		XInput,   // Xbox 360 wired protocol (vendor class 0xff, subclass 0x5d)
	};

	inline const char* FormatName(Format f)
	{
		switch (f)
		{
			case Format::Hid: return "HID gamepad (PS3 numbering)";
			case Format::Ps3Fixed: return "PS3 USB pad (fixed layout)";
			case Format::Ds3: return "DualShock 3";
			case Format::XInput: return "XInput";
			default: return "none";
		}
	}

	inline uint8_t Scale8(int32_t v, int32_t lmin, int32_t lmax)
	{
		if (lmax <= lmin)
			return 128;
		const int64_t s = (static_cast<int64_t>(std::clamp(v, lmin, lmax)) - lmin) * 255 / (static_cast<int64_t>(lmax) - lmin);
		return static_cast<uint8_t>(s);
	}

	inline int32_t FieldValue(const HidField& f, const uint8_t* r, size_t n)
	{
		const uint32_t raw = ExtractBits(r, n, f.bit, f.size);
		if (f.lmin < 0 && f.size > 0 && f.size < 32 && (raw >> (f.size - 1)) & 1u)
			return static_cast<int32_t>(raw | ~((1u << f.size) - 1u));
		return static_cast<int32_t>(raw);
	}

	// A HID report through its layout. Report IDs: the first byte names the report; fields of other reports are skipped.
	inline bool DecodeHid(const HidLayout& L, const uint8_t* report, size_t n, Common& c)
	{
		if (!L.ok || n == 0)
			return false;
		uint8_t id = 0;
		const uint8_t* body = report;
		size_t len = n;
		if (L.has_ids)
		{
			id = report[0];
			body = report + 1;
			len = n - 1;
		}
		Common o;
		bool any = false, have_z = false, have_rz = false, have_rx = false, have_ry = false;
		uint8_t z = 128, rz = 128, rxv = 128, ryv = 128;
		for (const HidField& f : L.fields)
		{
			if (f.report_id != id)
				continue;
			if (f.bit + f.size > len * 8)
				continue;
			const int32_t v = FieldValue(f, body, len);
			if (f.page == 9 && f.usage >= 1 && f.usage <= 13)
			{
				if (v != 0)
					o.buttons |= static_cast<uint16_t>(1u << (f.usage - 1));
				any = true;
			}
			else if (f.page == 1)
			{
				switch (f.usage)
				{
					case 0x30: o.lx = Scale8(v, f.lmin, f.lmax); any = true; break;
					case 0x31: o.ly = Scale8(v, f.lmin, f.lmax); any = true; break;
					case 0x32: z = Scale8(v, f.lmin, f.lmax); have_z = true; break;
					case 0x35: rz = Scale8(v, f.lmin, f.lmax); have_rz = true; break;
					case 0x33: rxv = Scale8(v, f.lmin, f.lmax); have_rx = true; break;
					case 0x34: ryv = Scale8(v, f.lmin, f.lmax); have_ry = true; break;
					case 0x39:
					{
						const int32_t h = v - f.lmin;
						const int32_t steps = f.lmax - f.lmin + 1;
						o.hat = (v < f.lmin || v > f.lmax) ? 8 : steps == 8 ? static_cast<uint8_t>(h)
						                                     : steps == 4 ? static_cast<uint8_t>(h * 2) : 8;
						any = true;
						break;
					}
					default: break;
				}
			}
		}
		// PS3 pads put the right stick on Z and Rz; others on Rx and Ry.
		if (have_z || have_rz)
		{
			o.rx = have_z ? z : 128;
			o.ry = have_rz ? rz : 128;
		}
		else
		{
			o.rx = have_rx ? rxv : 128;
			o.ry = have_ry ? ryv : 128;
		}
		any = any || have_z || have_rz || have_rx || have_ry;
		if (!any)
			return false;
		o.l2 = (o.buttons & B_L2) ? 255 : 0;
		o.r2 = (o.buttons & B_R2) ? 255 : 0;
		c = o;
		return true;
	}

	// The common PS3 third-party layout (27 bytes, no report ID): 13 buttons, 3 padding bits, the hat's 4 bits and 4 more,
	// the four sticks, then pressures and the motion sensor. What a PS3 Guitar Hero guitar (12ba:0100) sends.
	inline bool DecodePs3Fixed(const uint8_t* r, size_t n, Common& c)
	{
		if (n < 7)
			return false;
		Common o;
		o.buttons = static_cast<uint16_t>((r[0] | (r[1] << 8)) & 0x1FFF);
		o.hat = (r[2] & 0x0F) <= 7 ? (r[2] & 0x0F) : 8;
		o.lx = r[3];
		o.ly = r[4];
		o.rx = r[5];
		o.ry = r[6];
		o.l2 = (o.buttons & B_L2) ? 255 : 0;
		o.r2 = (o.buttons & B_R2) ? 255 : 0;
		c = o;
		return true;
	}

	// DualShock 3 (USB report 1, 49 bytes): byte 2 Select, L3, R3, Start, Up, Right, Down, Left; byte 3 L2, R2, L1, R1,
	// Triangle, Circle, Cross, Square; byte 4 the PS button; sticks in 6-9; the triggers' pressure in 18 and 19.
	inline bool DecodeDs3(const uint8_t* r, size_t n, Common& c)
	{
		if (n < 20 || r[0] != 0x01)
			return false;
		Common o;
		const uint8_t a = r[2], b = r[3];
		if (a & 0x01) o.buttons |= B_SELECT;
		if (a & 0x02) o.buttons |= B_L3;
		if (a & 0x04) o.buttons |= B_R3;
		if (a & 0x08) o.buttons |= B_START;
		if (b & 0x01) o.buttons |= B_L2;
		if (b & 0x02) o.buttons |= B_R2;
		if (b & 0x04) o.buttons |= B_L1;
		if (b & 0x08) o.buttons |= B_R1;
		if (b & 0x10) o.buttons |= B_TRIANGLE;
		if (b & 0x20) o.buttons |= B_CIRCLE;
		if (b & 0x40) o.buttons |= B_CROSS;
		if (b & 0x80) o.buttons |= B_SQUARE;
		if (r[4] & 0x01) o.buttons |= B_PS;
		const bool up = a & 0x10, right = a & 0x20, down = a & 0x40, left = a & 0x80;
		o.hat = up ? (right ? 1 : left ? 7 : 0) : down ? (right ? 3 : left ? 5 : 4) : right ? 2 : left ? 6 : 8;
		o.lx = r[6];
		o.ly = r[7];
		o.rx = r[8];
		o.ry = r[9];
		o.l2 = std::max<uint8_t>(r[18], (o.buttons & B_L2) ? 255 : 0);
		o.r2 = std::max<uint8_t>(r[19], (o.buttons & B_R2) ? 255 : 0);
		c = o;
		return true;
	}

	inline uint8_t XStick(int16_t v, bool invert)
	{
		int32_t s = (static_cast<int32_t>(v) + 32768) >> 8; // 0..255
		if (invert)
			s = 255 - s;
		return static_cast<uint8_t>(std::clamp(s, 0, 255));
	}

	// Xbox 360 wired report: type 0, length 0x14, buttons (LE16) in 2-3, the triggers in 4 and 5, the sticks (s16) in 6-13.
	inline bool DecodeXInput(const uint8_t* r, size_t n, Common& c)
	{
		if (n < 14 || r[0] != 0x00 || r[1] < 0x14)
			return false;
		Common o;
		const uint16_t w = static_cast<uint16_t>(r[2] | (r[3] << 8));
		const bool up = w & 0x0001, down = w & 0x0002, left = w & 0x0004, right = w & 0x0008;
		o.hat = up ? (right ? 1 : left ? 7 : 0) : down ? (right ? 3 : left ? 5 : 4) : right ? 2 : left ? 6 : 8;
		if (w & 0x0010) o.buttons |= B_START;
		if (w & 0x0020) o.buttons |= B_SELECT;
		if (w & 0x0040) o.buttons |= B_L3;
		if (w & 0x0080) o.buttons |= B_R3;
		if (w & 0x0100) o.buttons |= B_L1;
		if (w & 0x0200) o.buttons |= B_R1;
		if (w & 0x0400) o.buttons |= B_PS;
		if (w & 0x1000) o.buttons |= B_CROSS;    // A
		if (w & 0x2000) o.buttons |= B_CIRCLE;   // B
		if (w & 0x4000) o.buttons |= B_SQUARE;   // X
		if (w & 0x8000) o.buttons |= B_TRIANGLE; // Y
		o.l2 = r[4];
		o.r2 = r[5];
		if (o.l2 >= 64) o.buttons |= B_L2;
		if (o.r2 >= 64) o.buttons |= B_R2;
		int16_t lx, ly, rx, ry;
		std::memcpy(&lx, r + 6, 2);
		std::memcpy(&ly, r + 8, 2);
		std::memcpy(&rx, r + 10, 2);
		std::memcpy(&ry, r + 12, 2);
		o.lx = XStick(lx, false);
		o.ly = XStick(ly, true); // XInput's Y is up-positive
		o.rx = XStick(rx, false);
		o.ry = XStick(ry, true);
		o.tilt = ry > 16384;
		c = o;
		return true;
	}

	// ---- Which devices, and as what ------------------------------------------------------------------------------------------

	// Sony's PS4 and PS5 controllers and accessories are the system's: ScePad already reads them, and taking their
	// interface away would take the controller from the whole console. The DualShock 3 (0268) is not a PS5 controller.
	inline bool SystemOwned(uint16_t vid, uint16_t pid)
	{
		if (vid != 0x054C)
			return false;
		return pid != 0x0268;
	}

	inline bool ContainsNoCase(const std::string& s, const char* what)
	{
		std::string a = s, b = what;
		for (char& ch : a) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		for (char& ch : b) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return a.find(b) != std::string::npos;
	}

	// A guitar by its IDs or name: RedOctane/Activision's PS3 Guitar Hero guitar (12ba:0100), Harmonix's PS3 Rock Band
	// guitar (12ba:0200), the Xbox 360 guitars (1430:4748 Guitar Hero, 1bad:0002 Rock Band), and anything whose product
	// string says "guitar" (a modded guitar usually keeps that). The flag files usbguitar and usbpad decide when it's wrong.
	inline bool LooksLikeGuitar(uint16_t vid, uint16_t pid, const std::string& product)
	{
		if (vid == 0x12BA && (pid == 0x0100 || pid == 0x0200))
			return true;
		if ((vid == 0x1430 && pid == 0x4748) || (vid == 0x1BAD && pid == 0x0002))
			return true;
		return ContainsNoCase(product, "guitar");
	}

	// ---- Out: the pad thread's two views ---------------------------------------------------------------------------------------

	// ScePad's bits, as the pad thread reads them from the DualSense (orbis-shims/OrbisPadMap.h): 0x40000000 is the
	// keyboard's Select, which ScePad never sets and the remapping takes as Select.
	constexpr uint32_t SP_L3 = 0x00000002u, SP_R3 = 0x00000004u, SP_OPTIONS = 0x00000008u, SP_UP = 0x00000010u,
	                   SP_RIGHT = 0x00000020u, SP_DOWN = 0x00000040u, SP_LEFT = 0x00000080u, SP_L2 = 0x00000100u,
	                   SP_R2 = 0x00000200u, SP_L1 = 0x00000400u, SP_R1 = 0x00000800u, SP_TRIANGLE = 0x00001000u,
	                   SP_CIRCLE = 0x00002000u, SP_CROSS = 0x00004000u, SP_SQUARE = 0x00008000u, SP_SELECT = 0x40000000u;

	struct PadOut
	{
		uint32_t buttons = 0;
		uint8_t l2 = 0, r2 = 0;
		uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
	};

	inline uint32_t HatBits(uint8_t hat)
	{
		switch (hat)
		{
			case 0: return SP_UP;
			case 1: return SP_UP | SP_RIGHT;
			case 2: return SP_RIGHT;
			case 3: return SP_DOWN | SP_RIGHT;
			case 4: return SP_DOWN;
			case 5: return SP_DOWN | SP_LEFT;
			case 6: return SP_LEFT;
			case 7: return SP_UP | SP_LEFT;
			default: return 0;
		}
	}

	inline PadOut ToPad(const Common& c)
	{
		PadOut p;
		const uint16_t b = c.buttons;
		if (b & B_SQUARE) p.buttons |= SP_SQUARE;
		if (b & B_CROSS) p.buttons |= SP_CROSS;
		if (b & B_CIRCLE) p.buttons |= SP_CIRCLE;
		if (b & B_TRIANGLE) p.buttons |= SP_TRIANGLE;
		if (b & B_L1) p.buttons |= SP_L1;
		if (b & B_R1) p.buttons |= SP_R1;
		if (b & B_L2) p.buttons |= SP_L2;
		if (b & B_R2) p.buttons |= SP_R2;
		if (b & B_SELECT) p.buttons |= SP_SELECT;
		if (b & B_START) p.buttons |= SP_OPTIONS;
		if (b & B_L3) p.buttons |= SP_L3;
		if (b & B_R3) p.buttons |= SP_R3;
		p.buttons |= HatBits(c.hat);
		p.l2 = c.l2;
		p.r2 = c.r2;
		p.lx = c.lx;
		p.ly = c.ly;
		p.rx = c.rx;
		p.ry = c.ry;
		return p;
	}

	struct GuitarOut
	{
		bool green = false, red = false, yellow = false, blue = false, orange = false;
		bool strum_up = false, strum_down = false, select = false, start = false, tilt = false;
		float whammy = 0.0f; // 0 at rest .. 1 pressed all the way
	};

	// The whammy's rest position is taken from the first report (PS3 guitars rest at 0x7f or 0x80, XInput ones at the
	// bottom), so the bar reads from where it sits, whichever way it travels.
	struct WhammyCal
	{
		bool have = false;
		uint8_t rest = 128;
	};

	inline GuitarOut ToGuitar(const Common& c, Format f, WhammyCal& cal)
	{
		GuitarOut g;
		const uint16_t b = c.buttons;
		g.green = b & B_CROSS;
		g.red = b & B_CIRCLE;
		// vk-285-143: yellow on Square / X and blue on Triangle / Y. The tester's guitars (vk-285-142, Guitar Hero II, a PS3-mode
		// and an Xbox 360 one): "Blue and yellow are swapped on both 360 and PS3" with Triangle/Y as yellow.
		g.yellow = b & B_SQUARE;
		g.blue = b & B_TRIANGLE;
		g.orange = b & B_L1;
		g.strum_up = c.hat == 0 || c.hat == 1 || c.hat == 7;
		g.strum_down = c.hat == 3 || c.hat == 4 || c.hat == 5;
		g.select = b & B_SELECT;
		g.start = b & B_START;
		g.tilt = f == Format::XInput ? c.tilt : (b & B_R1) != 0;
		if (!cal.have)
		{
			cal.have = true;
			cal.rest = c.rx;
		}
		const int span = std::max<int>(cal.rest, 255 - cal.rest);
		const int d = std::abs(static_cast<int>(c.rx) - cal.rest);
		g.whammy = span > 0 && d > 8 ? std::min(1.0f, static_cast<float>(d) / span) : 0.0f;
		return g;
	}

	// The guitar as a DualShock 2 would show it (what PCSX2's Guitar binds by default: green R2, red Circle, yellow
	// Triangle, blue Cross, orange Square, strum on the D-pad, whammy on the left stick's up, tilt L2): for a port that
	// isn't a Guitar (a guitar plugged in after the game started).
	inline PadOut GuitarAsPad(const GuitarOut& g)
	{
		PadOut p;
		if (g.green) { p.buttons |= SP_R2; p.r2 = 255; }
		if (g.red) p.buttons |= SP_CIRCLE;
		if (g.yellow) p.buttons |= SP_TRIANGLE;
		if (g.blue) p.buttons |= SP_CROSS;
		if (g.orange) p.buttons |= SP_SQUARE;
		if (g.strum_up) p.buttons |= SP_UP;
		if (g.strum_down) p.buttons |= SP_DOWN;
		if (g.select) p.buttons |= SP_SELECT;
		if (g.start) p.buttons |= SP_OPTIONS;
		if (g.tilt) { p.buttons |= SP_L2; p.l2 = 255; }
		p.ly = static_cast<uint8_t>(128 - std::clamp<long>(std::lround(g.whammy * 127.0f), 0, 127));
		return p;
	}

	// ---- USB descriptors: the interface to read --------------------------------------------------------------------------------

	struct Target
	{
		bool ok = false;
		uint8_t iface = 0, alt = 0;
		uint8_t ep_in = 0;   // the interrupt IN endpoint's address (0x81 ...)
		uint16_t max_packet = 64;
		uint8_t iface_class = 0, iface_subclass = 0, iface_protocol = 0;
		uint16_t report_desc_len = 0; // from the HID descriptor (0 for XInput)
		bool xinput = false;
	};

	// The first interface of a configuration descriptor (and the interfaces and endpoints after it) that is a HID gamepad
	// (class 3, not a boot keyboard or mouse) or an XInput controller (0xff/0x5d/0x01), with an interrupt IN endpoint.
	inline Target FindTarget(const uint8_t* d, size_t n)
	{
		Target best, cur;
		bool in_iface = false;
		size_t i = 0;
		while (i + 2 <= n)
		{
			const uint8_t len = d[i], type = d[i + 1];
			if (len < 2 || i + len > n)
				break;
			if (type == 4 && len >= 9) // interface
			{
				if (in_iface && cur.ok && !best.ok)
					best = cur;
				cur = Target();
				cur.iface = d[i + 2];
				cur.alt = d[i + 3];
				cur.iface_class = d[i + 5];
				cur.iface_subclass = d[i + 6];
				cur.iface_protocol = d[i + 7];
				const bool hid = cur.iface_class == 3 && !(cur.iface_subclass == 1 && (cur.iface_protocol == 1 || cur.iface_protocol == 2));
				cur.xinput = cur.iface_class == 0xFF && cur.iface_subclass == 0x5D && cur.iface_protocol == 0x01;
				in_iface = cur.alt == 0 && (hid || cur.xinput);
			}
			else if (type == 0x21 && len >= 9 && in_iface) // HID descriptor: the first class descriptor's length
			{
				if (d[i + 6] == 0x22)
					cur.report_desc_len = static_cast<uint16_t>(d[i + 7] | (d[i + 8] << 8));
			}
			else if (type == 5 && len >= 7 && in_iface) // endpoint
			{
				const uint8_t addr = d[i + 2], attr = d[i + 3];
				if ((addr & 0x80) && (attr & 3) == 3 && !cur.ok)
				{
					cur.ep_in = addr;
					cur.max_packet = static_cast<uint16_t>((d[i + 4] | (d[i + 5] << 8)) & 0x7FF);
					cur.ok = true;
				}
			}
			i += len;
		}
		if (in_iface && cur.ok && !best.ok)
			best = cur;
		return best;
	}
} // namespace orbis_usbpad
