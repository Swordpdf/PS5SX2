// PS5SX2 (vk-285-113): a check of the keyboard and mouse controller mapping (orbis-shims/ProsperoKbdMap.h) on a PC.
//   g++ -std=c++17 -I../../orbis-shims -o /tmp/test_kbm_map test_kbm_map.cpp && /tmp/test_kbm_map
#include "ProsperoKbdMap.h"

#include <cstdio>
#include <cstdlib>

using namespace orbis_kbm;

static int s_failures = 0;
#define CHECK(cond)                                                                                  \
	do                                                                                               \
	{                                                                                                \
		if (!(cond))                                                                                 \
		{                                                                                            \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                   \
			s_failures++;                                                                            \
		}                                                                                            \
	} while (0)

static PadKeys Held(std::initializer_list<uint16_t> keys)
{
	uint16_t list[32] = {};
	int n = 0;
	for (uint16_t k : keys)
		list[n++] = k;
	return MapKeys(list, n);
}

int main()
{
	// Nothing held: nothing pressed, every stick in the middle.
	{
		const PadKeys p = Held({});
		CHECK(p.buttons == 0 && !p.l2 && !p.r2);
		CHECK(KeyAxis(p.left_left, p.left_right) == 128 && KeyAxis(p.left_up, p.left_down) == 128);
		CHECK(KeyAxis(p.right_left, p.right_right) == 128 && KeyAxis(p.right_up, p.right_down) == 128);
	}
	// PCSX2's defaults, key by key (HID usages: A=4 .. Z=0x1d, 1=0x1e, Return=0x28, Backspace=0x2a, arrows 0x4f-0x52).
	CHECK(Held({0x52}).buttons == PAD_UP);
	CHECK(Held({0x4f}).buttons == PAD_RIGHT);
	CHECK(Held({0x51}).buttons == PAD_DOWN);
	CHECK(Held({0x50}).buttons == PAD_LEFT);
	CHECK(Held({0x28}).buttons == PAD_START);
	CHECK(Held({0x58}).buttons == PAD_START); // the keypad's Enter too
	CHECK(Held({0x2a}).buttons == PAD_SELECT);
	CHECK(Held({0x0c}).buttons == PAD_TRIANGLE); // I
	CHECK(Held({0x0f}).buttons == PAD_CIRCLE);   // L
	CHECK(Held({0x0e}).buttons == PAD_CROSS);    // K
	CHECK(Held({0x0d}).buttons == PAD_SQUARE);   // J
	CHECK(Held({0x14}).buttons == PAD_L1);       // Q
	CHECK(Held({0x08}).buttons == PAD_R1);       // E
	CHECK(Held({0x1f}).buttons == PAD_L3);       // 2
	CHECK(Held({0x21}).buttons == PAD_R3);       // 4
	CHECK(Held({0x1e}).l2 && Held({0x1e}).buttons == 0);  // 1
	CHECK(Held({0x20}).r2 && Held({0x20}).buttons == 0);  // 3
	// The four button bits are the ones main-boot.cpp's orbis_pad_apply reads.
	CHECK(PAD_UP == 0x10 && PAD_RIGHT == 0x20 && PAD_DOWN == 0x40 && PAD_LEFT == 0x80);
	CHECK(PAD_TRIANGLE == 0x1000 && PAD_CIRCLE == 0x2000 && PAD_CROSS == 0x4000 && PAD_SQUARE == 0x8000);
	CHECK(PAD_L1 == 0x400 && PAD_R1 == 0x800 && PAD_L3 == 0x2 && PAD_R3 == 0x4 && PAD_START == 0x8);
	// vk-285-122: Select (Backspace) has its own bit, one ScePad never sets (its 0x1 is the DualSense's Create button).
	CHECK(PAD_SELECT == 0x40000000);

	// Sticks: W A S D on the left, T F G H on the right; both keys of an axis cancel.
	{
		PadKeys p = Held({0x1a}); // W
		CHECK(KeyAxis(p.left_up, p.left_down) == 0 && KeyAxis(p.left_left, p.left_right) == 128);
		p = Held({0x16}); // S
		CHECK(KeyAxis(p.left_up, p.left_down) == 255);
		p = Held({0x04}); // A
		CHECK(KeyAxis(p.left_left, p.left_right) == 0);
		p = Held({0x07}); // D
		CHECK(KeyAxis(p.left_left, p.left_right) == 255);
		p = Held({0x1a, 0x16}); // W and S together
		CHECK(KeyAxis(p.left_up, p.left_down) == 128);
		p = Held({0x17, 0x0b}); // T and H: right stick up and right
		CHECK(KeyAxis(p.right_up, p.right_down) == 0 && KeyAxis(p.right_left, p.right_right) == 255);
		p = Held({0x0a, 0x09}); // G and F: right stick down and left
		CHECK(KeyAxis(p.right_up, p.right_down) == 255 && KeyAxis(p.right_left, p.right_right) == 0);
	}
	// Several keys at once, and keys with no meaning (modifiers 0xe0.., F5, Tab).
	{
		const PadKeys p = Held({0x0e, 0x4f, 0xe1, 0x3e, 0x2b});
		CHECK(p.buttons == (PAD_CROSS | PAD_RIGHT));
	}

	// The mouse's aim: nothing while it is still, past the dead zone for any movement, a full stick at the speed's rate.
	CHECK(AimByte(0.0f, 12.0f) == 128);
	CHECK(AimByte(0.4f, 12.0f) == 128);
	CHECK(AimByte(-0.4f, 12.0f) == 128);
	{
		const int slow_drift = AimByte(1.0f, 12.0f) - 128; // a single count
		CHECK(slow_drift >= AIM_DEAD_ZONE && slow_drift < 40);
		const int left_drift = 128 - AimByte(-1.0f, 12.0f);
		CHECK(left_drift == slow_drift);
	}
	for (int speed = 1; speed <= 4; speed++)
	{
		const float full = SpeedCounts(speed);
		const float steady = full / (1.0f - AIM_DECAY); // what a steady `full` counts a poll settles to
		CHECK(AimByte(steady, full) == 255);
		CHECK(AimByte(-steady, full) == 1);
		CHECK(AimByte(steady * 3.0f, full) == 255);   // faster is still a full stick
		CHECK(AimByte(-steady * 3.0f, full) == 1);
		CHECK(AimByte(steady / 2.0f, full) > 128 + 60 && AimByte(steady / 2.0f, full) < 255);
	}
	CHECK(SpeedCounts(1) > SpeedCounts(2) && SpeedCounts(2) > SpeedCounts(3) && SpeedCounts(3) > SpeedCounts(4));
	CHECK(SpeedCounts(0) == SpeedCounts(2) && SpeedCounts(9) == SpeedCounts(2)); // out of range: Normal
	// Monotonic: a bigger movement never gives a smaller stick.
	{
		int last = 128;
		for (float a = 0.0f; a <= 80.0f; a += 0.5f)
		{
			const int now = AimByte(a, 12.0f);
			CHECK(now >= last);
			last = now;
		}
	}
	// A run: 5 polls of a steady 12 counts, then the mouse stops: the stick falls back to the middle within about 25 polls.
	{
		float aim = 0.0f;
		for (int i = 0; i < 30; i++)
			aim = aim * AIM_DECAY + 12.0f;
		CHECK(AimByte(aim, 12.0f) >= 250);
		int polls = 0;
		while (AimByte(aim, 12.0f) != 128 && polls < 100)
		{
			aim = aim * AIM_DECAY;
			polls++;
		}
		CHECK(polls > 5 && polls < 40);
	}

	// ComposePad: keys and mouse together.
	{
		AimState aim;
		const uint16_t keys[] = {0x0e /* K */, 0x1a /* W */};
		PadInputs in;
		in.keys = keys;
		in.key_count = 2;
		in.keys_to_pad = true;
		in.mouse_to_pad = true;
		PadOut out = ComposePad(in, 0, 0, aim);
		CHECK(out.buttons == PAD_CROSS && out.ly == 0 && out.lx == 128 && out.rx == 128 && out.ry == 128);
		// The mouse moves right and down: the right stick, the keys' left stick stays.
		for (int i = 0; i < 40; i++)
			out = ComposePad(in, 12, 12, aim);
		CHECK(out.rx >= 250 && out.ry >= 250 && out.ly == 0 && out.lx == 128); // a steady run gets to the end of the stick
		// Left stick aim: the mouse takes the left stick over from W while it moves.
		in.aim = 2;
		aim = AimState();
		for (int i = 0; i < 40; i++)
			out = ComposePad(in, -12, 0, aim);
		CHECK(out.lx <= 5 && out.ly == 0 /* W still up: no vertical mouse movement */ && out.rx == 128);
		// Aim off: the mouse only has its buttons.
		in.aim = 0;
		aim = AimState();
		out = ComposePad(in, 50, 50, aim);
		CHECK(out.rx == 128 && out.ry == 128 && out.lx == 128);
		// The mouse's buttons: shoulders by default, triggers when asked; the wheel's click is R3.
		in.aim = 1;
		in.key_count = 0;
		in.mouse_buttons = 0x1;
		out = ComposePad(in, 0, 0, aim);
		CHECK(out.buttons == PAD_R1 && out.r2 == 0);
		in.mouse_buttons = 0x2;
		out = ComposePad(in, 0, 0, aim);
		CHECK(out.buttons == PAD_L1 && out.l2 == 0);
		in.mouse_buttons = 0x7;
		out = ComposePad(in, 0, 0, aim);
		CHECK(out.buttons == (PAD_R1 | PAD_L1 | PAD_R3));
		in.triggers = true;
		out = ComposePad(in, 0, 0, aim);
		CHECK(out.buttons == PAD_R3 && out.r2 == 255 && out.l2 == 255);
		// Each of the two switched off on its own.
		in.mouse_to_pad = false;
		in.keys = keys;
		in.key_count = 2;
		out = ComposePad(in, 30, 30, aim);
		CHECK(out.buttons == PAD_CROSS && out.ly == 0 && out.r2 == 0 && out.rx == 128); // keys only
		in.mouse_to_pad = true;
		in.keys_to_pad = false;
		in.mouse_buttons = 0x1;
		out = ComposePad(in, 0, 0, aim);
		CHECK(out.buttons == 0 && out.r2 == 255 && out.ly == 128); // mouse only
		// Both off: idle, and the aim is forgotten.
		in.mouse_to_pad = false;
		out = ComposePad(in, 99, 99, aim);
		CHECK(out.buttons == 0 && out.r2 == 0 && out.rx == 128 && aim.x == 0.0f && aim.y == 0.0f);
	}
	// Hotkeys: F1 and F3 once per press, Esc after a second and once; nothing while the controller mode is off.
	{
		Hotkeys h;
		const uint16_t none[1] = {0};
		const uint16_t f1[] = {KEY_F1}, f3[] = {KEY_F3}, esc[] = {KEY_ESCAPE}, f1_f3[] = {KEY_F1, KEY_F3};
		long long t = 1000;
		CHECK(h.Update(none, 0, true, t) == 0);
		CHECK(h.Update(f1, 1, true, t += 4) == 1); // pressed
		CHECK(h.Update(f1, 1, true, t += 4) == 0); // held
		CHECK(h.Update(none, 0, true, t += 4) == 0);
		CHECK(h.Update(f1, 1, true, t += 4) == 1); // pressed again
		CHECK(h.Update(none, 0, true, t += 4) == 0);
		CHECK(h.Update(f3, 1, true, t += 4) == 2);
		CHECK(h.Update(f3, 1, true, t += 4) == 0);
		CHECK(h.Update(none, 0, true, t += 4) == 0);
		CHECK(h.Update(f1_f3, 2, true, t += 4) != 0); // both at once: one of them fires
		CHECK(h.Update(none, 0, true, t += 4) == 0);
		// Esc: 900 ms is not enough, 1000 ms fires once, holding on does not fire again, a new press starts again.
		CHECK(h.Update(esc, 1, true, t += 4) == 0);
		CHECK(h.Update(esc, 1, true, t += 900) == 0);
		CHECK(h.Update(esc, 1, true, t += 100) == 3);
		CHECK(h.Update(esc, 1, true, t += 500) == 0);
		CHECK(h.Update(none, 0, true, t += 4) == 0);
		CHECK(h.Update(esc, 1, true, t += 4) == 0);
		CHECK(h.Update(none, 0, true, t += 400) == 0); // let go before a second: nothing
		CHECK(h.Update(esc, 1, true, t += 4) == 0);
		CHECK(h.Update(esc, 1, true, t += 999) == 0);
		CHECK(h.Update(esc, 1, true, t += 2) == 3);
		// The controller mode off (the game reads the PS2's keyboard, or another mode): no shortcuts, and no memory of them.
		CHECK(h.Update(f1, 1, false, t += 4) == 0);
		CHECK(h.Update(esc, 1, false, t += 4) == 0);
		CHECK(h.Update(esc, 1, false, t += 2000) == 0);
		CHECK(h.Update(f1, 1, true, t += 4) == 1); // a key already held when the mode comes on counts as a press
	}

	if (s_failures == 0)
		printf("kbm map: PASS\n");
	return s_failures == 0 ? 0 : 1;
}
