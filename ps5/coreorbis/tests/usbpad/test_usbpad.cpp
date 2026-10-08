// PS5SX2 (vk-285-140, AI-assisted): a PC check of orbis-shims/OrbisUsbPadDecode.h with the descriptors and reports of a
// PS3 USB guitar (the common PS3 third-party layout), a DualShock 3, an Xbox 360 pad and guitar, and a HID boot keyboard.
//   c++ -std=c++17 -Wall -Wextra -I../../orbis-shims -o /tmp/test_usbpad test_usbpad.cpp && /tmp/test_usbpad
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "OrbisUsbPadDecode.h"

#include <cstdio>
#include <vector>

using namespace orbis_usbpad;

static int s_checks = 0, s_failures = 0;
#define CHECK(cond)                                                \
	do                                                             \
	{                                                              \
		s_checks++;                                                \
		if (!(cond))                                               \
		{                                                          \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			s_failures++;                                          \
		}                                                          \
	} while (0)

// The report descriptor PS3 third-party pads and the PS3 Guitar Hero guitar send (27-byte input report, no report ID).
static const uint8_t kPs3Desc[] = {
	0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,             // Generic Desktop, Game Pad, Application
	0x15, 0x00, 0x25, 0x01, 0x35, 0x00, 0x45, 0x01, // logical 0..1
	0x75, 0x01, 0x95, 0x0D, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0D, 0x81, 0x02, // 13 buttons
	0x95, 0x03, 0x81, 0x01,                         // 3 bits padding
	0x05, 0x01, 0x25, 0x07, 0x46, 0x3B, 0x01, 0x75, 0x04, 0x95, 0x01, 0x65, 0x14, 0x09, 0x39, 0x81, 0x42, // hat 0..7
	0x65, 0x00, 0x95, 0x01, 0x81, 0x01,             // 4 bits padding
	0x26, 0xFF, 0x00, 0x46, 0xFF, 0x00, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, // X Y Z Rz
	0x06, 0x00, 0xFF, 0x09, 0x20, 0x09, 0x21, 0x09, 0x22, 0x09, 0x23, 0x09, 0x24, 0x09, 0x25, 0x09, 0x26, 0x09, 0x27,
	0x09, 0x28, 0x09, 0x29, 0x09, 0x2A, 0x09, 0x2B, 0x95, 0x0C, 0x81, 0x02, // 12 pressure bytes
	0x0A, 0x21, 0x26, 0x95, 0x08, 0xB1, 0x02,       // feature
	0x0A, 0x21, 0x26, 0x91, 0x02,                   // output
	0x26, 0xFF, 0x03, 0x46, 0xFF, 0x03, 0x09, 0x2C, 0x09, 0x2D, 0x09, 0x2E, 0x09, 0x2F, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02, // motion
	0xC0,
};

// A boot keyboard's descriptor (kept for the keyboard code, never taken as a pad).
static const uint8_t kKbdDesc[] = {
	0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
	0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00,
	0x29, 0x65, 0x81, 0x00, 0xC0,
};

// A pad with report ID 3 and 16-bit signed sticks (another vendor's layout).
static const uint8_t kIdDesc[] = {
	0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x03,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
	0x09, 0x39, 0x15, 0x00, 0x25, 0x03, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x75, 0x04, 0x81, 0x01,
	0xC0,
};

static std::vector<uint8_t> Ps3Report(uint16_t buttons, uint8_t hat, uint8_t lx, uint8_t ly, uint8_t z, uint8_t rz)
{
	std::vector<uint8_t> r(27, 0);
	r[0] = buttons & 0xFF;
	r[1] = (buttons >> 8) & 0x1F;
	r[2] = hat;
	r[3] = lx;
	r[4] = ly;
	r[5] = z;
	r[6] = rz;
	return r;
}

int main()
{
	// ---- the PS3 layout through its descriptor
	{
		const HidLayout L = ParseHid(kPs3Desc, sizeof(kPs3Desc));
		CHECK(L.ok);
		CHECK(L.gamepad);
		CHECK(!L.keyboard);
		CHECK(!L.has_ids);
		int buttons = 0, axes = 0, hats = 0;
		for (const HidField& f : L.fields)
		{
			if (f.page == 9) buttons++;
			if (f.page == 1 && f.usage >= 0x30 && f.usage <= 0x35) axes++;
			if (f.page == 1 && f.usage == 0x39)
			{
				hats++;
				CHECK(f.bit == 16);
				CHECK(f.size == 4);
				CHECK(f.lmax == 7);
			}
			if (f.page == 1 && f.usage == 0x32)
				CHECK(f.bit == 40);
		}
		CHECK(buttons == 13);
		CHECK(axes == 4);
		CHECK(hats == 1);

		// Green (Cross) + Orange (L1) held, strumming down, whammy pushed from its rest at 0x7f.
		Common c;
		CHECK(DecodeHid(L, Ps3Report(B_CROSS | B_L1 | B_SELECT, 4, 0x80, 0x80, 0x7F, 0x80).data(), 27, c));
		CHECK(c.buttons == (B_CROSS | B_L1 | B_SELECT));
		CHECK(c.hat == 4);
		WhammyCal cal;
		GuitarOut g = ToGuitar(c, Format::Hid, cal);
		CHECK(g.green && g.orange && !g.red && !g.yellow && !g.blue);
		CHECK(g.strum_down && !g.strum_up);
		CHECK(g.select && !g.start);
		CHECK(g.whammy == 0.0f); // the first report is the rest
		CHECK(DecodeHid(L, Ps3Report(B_CIRCLE | B_TRIANGLE | B_SQUARE, 0, 0x80, 0x80, 0xFF, 0x80).data(), 27, c));
		g = ToGuitar(c, Format::Hid, cal);
		CHECK(g.red && g.yellow && g.blue && !g.green);
		CHECK(g.strum_up && !g.strum_down);
		CHECK(g.whammy > 0.99f);
		CHECK(DecodeHid(L, Ps3Report(0, 0x0F, 0x80, 0x80, 0xBF, 0x80).data(), 27, c));
		g = ToGuitar(c, Format::Hid, cal);
		CHECK(!g.strum_up && !g.strum_down);
		CHECK(g.whammy > 0.45f && g.whammy < 0.55f);

		// The fixed layout reads the same report the same way.
		Common f;
		CHECK(DecodePs3Fixed(Ps3Report(B_CROSS | B_L1 | B_SELECT, 4, 0x80, 0x80, 0x7F, 0x80).data(), 27, f));
		Common h;
		CHECK(DecodeHid(L, Ps3Report(B_CROSS | B_L1 | B_SELECT, 4, 0x80, 0x80, 0x7F, 0x80).data(), 27, h));
		CHECK(f.buttons == h.buttons && f.hat == h.hat && f.lx == h.lx && f.rx == h.rx && f.ry == h.ry);

		// As a pad: every PS3 button on its ScePad bit, the hat on the D-pad.
		CHECK(DecodeHid(L, Ps3Report(0x1FFF, 2, 0, 255, 10, 250).data(), 27, c));
		const PadOut p = ToPad(c);
		const uint32_t all = SP_SQUARE | SP_CROSS | SP_CIRCLE | SP_TRIANGLE | SP_L1 | SP_R1 | SP_L2 | SP_R2 | SP_SELECT |
		                     SP_OPTIONS | SP_L3 | SP_R3 | SP_RIGHT;
		CHECK(p.buttons == all);
		CHECK(p.l2 == 255 && p.r2 == 255);
		CHECK(p.lx == 0 && p.ly == 255 && p.rx == 10 && p.ry == 250);

		// A short report doesn't read past its end.
		CHECK(DecodeHid(L, Ps3Report(B_CROSS, 8, 1, 2, 3, 4).data(), 3, c));
		CHECK(c.buttons == B_CROSS);
		CHECK(c.lx == 128);
	}

	// ---- a keyboard is the keyboard code's
	{
		const HidLayout L = ParseHid(kKbdDesc, sizeof(kKbdDesc));
		CHECK(L.keyboard);
		CHECK(!L.gamepad);
	}

	// ---- report IDs, signed 16-bit sticks, a 4-way hat
	{
		const HidLayout L = ParseHid(kIdDesc, sizeof(kIdDesc));
		CHECK(L.ok && L.has_ids && L.gamepad);
		uint8_t r[7] = {0x03, 0x05, 0x00, 0x80, 0xFF, 0x7F, 0x02}; // id 3, buttons 1+3, X -32768, Y 32767, hat 2 (down)
		Common c;
		CHECK(DecodeHid(L, r, sizeof(r), c));
		CHECK(c.buttons == (B_SQUARE | B_CIRCLE));
		CHECK(c.lx == 0 && c.ly == 255);
		CHECK(c.hat == 4);
		uint8_t other[7] = {0x04, 0xFF, 0, 0, 0, 0, 0}; // another report: nothing of ours
		Common c2;
		CHECK(!DecodeHid(L, other, sizeof(other), c2));
	}

	// ---- DualShock 3
	{
		std::vector<uint8_t> r(49, 0);
		r[0] = 0x01;
		r[2] = 0x01 | 0x08 | 0x10; // Select, Start, Up
		r[3] = 0x40 | 0x04;        // Cross, L1
		r[4] = 0x01;               // PS
		r[6] = 0;
		r[7] = 255;
		r[8] = 128;
		r[9] = 128;
		r[19] = 200; // R2 pressure
		Common c;
		CHECK(DecodeDs3(r.data(), r.size(), c));
		CHECK(c.buttons == (B_SELECT | B_START | B_CROSS | B_L1 | B_PS));
		CHECK(c.hat == 0);
		CHECK(c.lx == 0 && c.ly == 255 && c.r2 == 200);
		r[0] = 0x02;
		CHECK(!DecodeDs3(r.data(), r.size(), c));
	}

	// ---- XInput pad and guitar
	{
		uint8_t r[20] = {0x00, 0x14};
		const uint16_t w = 0x1000 | 0x0100 | 0x0002 | 0x0010; // A, LB, D-pad down, Start
		r[2] = w & 0xFF;
		r[3] = w >> 8;
		r[5] = 255; // RT
		const int16_t lx = -32768, ly = 32767, rx = -32768, ry = 32767;
		memcpy(r + 6, &lx, 2);
		memcpy(r + 8, &ly, 2);
		memcpy(r + 10, &rx, 2);
		memcpy(r + 12, &ry, 2);
		Common c;
		CHECK(DecodeXInput(r, sizeof(r), c));
		CHECK((c.buttons & (B_CROSS | B_L1 | B_START | B_R2)) == (B_CROSS | B_L1 | B_START | B_R2));
		CHECK(c.hat == 4);
		CHECK(c.lx == 0 && c.ly == 0); // up on XInput is up here: ly 0
		CHECK(c.rx == 0);
		CHECK(c.tilt);
		WhammyCal cal; // XInput whammy rests at the bottom (-32768 -> 0)
		GuitarOut g = ToGuitar(c, Format::XInput, cal);
		CHECK(g.green && g.orange && g.strum_down && g.start && g.tilt);
		CHECK(g.whammy == 0.0f);
		const int16_t rx2 = 32767;
		memcpy(r + 10, &rx2, 2);
		CHECK(DecodeXInput(r, sizeof(r), c));
		g = ToGuitar(c, Format::XInput, cal);
		CHECK(g.whammy > 0.99f);
		const PadOut gp = GuitarAsPad(g);
		CHECK((gp.buttons & (SP_R2 | SP_SQUARE | SP_DOWN | SP_OPTIONS | SP_L2)) == (SP_R2 | SP_SQUARE | SP_DOWN | SP_OPTIONS | SP_L2));
		CHECK(gp.ly <= 1);
		uint8_t bad[20] = {0x01, 0x03};
		CHECK(!DecodeXInput(bad, sizeof(bad), c));
	}

	// ---- configuration descriptors: which interface and endpoint
	{
		// config, a boot keyboard interface (skipped), then a HID gamepad interface with an OUT and an IN endpoint
		const uint8_t cfg[] = {
			9, 2, 59, 0, 2, 1, 0, 0x80, 50,
			9, 4, 0, 0, 1, 3, 1, 1, 0,   // boot keyboard
			9, 0x21, 0x11, 1, 0, 1, 0x22, 45, 0,
			7, 5, 0x81, 3, 8, 0, 10,
			9, 4, 1, 0, 2, 3, 0, 0, 0,   // HID gamepad
			9, 0x21, 0x11, 1, 0, 1, 0x22, 148, 0,
			7, 5, 0x02, 3, 64, 0, 10,
			7, 5, 0x83, 3, 64, 0, 10,
		};
		const Target t = FindTarget(cfg, sizeof(cfg));
		CHECK(t.ok);
		CHECK(t.iface == 1);
		CHECK(t.ep_in == 0x83);
		CHECK(t.max_packet == 64);
		CHECK(t.report_desc_len == 148);
		CHECK(!t.xinput);

		const uint8_t xcfg[] = {
			9, 2, 25, 0, 1, 1, 0, 0x80, 250,
			9, 4, 0, 0, 2, 0xFF, 0x5D, 0x01, 0,
			7, 5, 0x81, 3, 32, 0, 4,
		};
		const Target x = FindTarget(xcfg, sizeof(xcfg));
		CHECK(x.ok && x.xinput && x.ep_in == 0x81 && x.max_packet == 32);

		const uint8_t storage[] = {9, 2, 32, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 2, 8, 6, 0x50, 0, 7, 5, 0x81, 2, 0, 2, 0, 7, 5, 0x02, 2, 0, 2, 0};
		CHECK(!FindTarget(storage, sizeof(storage)).ok);
		const uint8_t truncated[] = {9, 2, 59, 0, 2, 1, 0, 0x80, 50, 9, 4, 0, 0, 1, 3, 0, 0, 0, 7, 5};
		CHECK(!FindTarget(truncated, sizeof(truncated)).ok);
	}

	// ---- which devices
	{
		CHECK(SystemOwned(0x054C, 0x0CE6));  // DualSense
		CHECK(SystemOwned(0x054C, 0x09CC));  // DualShock 4
		CHECK(!SystemOwned(0x054C, 0x0268)); // DualShock 3
		CHECK(!SystemOwned(0x12BA, 0x0100));
		CHECK(LooksLikeGuitar(0x12BA, 0x0100, ""));
		CHECK(LooksLikeGuitar(0x1209, 0x2882, "Santroller Guitar"));
		CHECK(!LooksLikeGuitar(0x0E6F, 0x0214, "Afterglow PS3 Controller"));
	}

	// ---- robustness: random bytes as descriptors and reports
	{
		uint32_t seed = 12345;
		auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return static_cast<uint8_t>(seed >> 16); };
		for (int k = 0; k < 20000; k++)
		{
			uint8_t d[96], r[64];
			const size_t dn = rnd() % sizeof(d), rn = rnd() % sizeof(r);
			for (size_t i = 0; i < dn; i++) d[i] = rnd();
			for (size_t i = 0; i < rn; i++) r[i] = rnd();
			const HidLayout L = ParseHid(d, dn);
			Common c;
			DecodeHid(L, r, rn, c);
			FindTarget(d, dn);
			DecodeDs3(r, rn, c);
			DecodeXInput(r, rn, c);
			DecodePs3Fixed(r, rn, c);
		}
		CHECK(true);
	}

	if (s_failures)
	{
		printf("FAIL: %d of %d checks failed (USB pads)\n", s_failures, s_checks);
		return 1;
	}
	printf("PASS: %d of %d checks passed (USB pads)\n", s_checks, s_checks);
	return 0;
}
