// PS5SX2 (vk-285-113): the controller a USB keyboard and mouse make, as functions of what is held: no console library, no
// emulator, so tests/kbm/test_kbm_map.cpp checks it on a PC. ProsperoKbdMouse.cpp uses it.
//
// The keys are PCSX2's own defaults (pcsx2/Input/InputManager.cpp, GetKeyboardGenericBindingMapping).
#pragma once

#include <algorithm>
#include <cstdint>

namespace orbis_kbm
{
// ScePad button bits, as main-boot.cpp's orbis_pad_apply reads them. vk-285-122: Select (Backspace) has a bit of its own,
// one ScePad never sets: ScePad's 0x1 is the DualSense's Create button (the old Share), which presses nothing now
// (orbis-shims/OrbisPadMap.h).
constexpr uint32_t PAD_SELECT = 0x40000000, PAD_L3 = 0x00000002, PAD_R3 = 0x00000004, PAD_START = 0x00000008;
constexpr uint32_t PAD_UP = 0x00000010, PAD_RIGHT = 0x00000020, PAD_DOWN = 0x00000040, PAD_LEFT = 0x00000080;
constexpr uint32_t PAD_L1 = 0x00000400, PAD_R1 = 0x00000800;
constexpr uint32_t PAD_TRIANGLE = 0x00001000, PAD_CIRCLE = 0x00002000, PAD_CROSS = 0x00004000, PAD_SQUARE = 0x00008000;

// HID usages (the keyboard page), for the keys PCSX2's default bindings use.
enum : uint16_t
{
	KEY_A = 0x04, KEY_D = 0x07, KEY_E = 0x08, KEY_F = 0x09, KEY_G = 0x0a, KEY_H = 0x0b, KEY_I = 0x0c, KEY_J = 0x0d,
	KEY_K = 0x0e, KEY_L = 0x0f, KEY_Q = 0x14, KEY_S = 0x16, KEY_T = 0x17, KEY_W = 0x1a,
	KEY_1 = 0x1e, KEY_2 = 0x1f, KEY_3 = 0x20, KEY_4 = 0x21,
	KEY_RETURN = 0x28, KEY_ESCAPE = 0x29, KEY_BACKSPACE = 0x2a,
	KEY_F1 = 0x3a, KEY_F3 = 0x3c,
	KEY_RIGHT = 0x4f, KEY_LEFT = 0x50, KEY_DOWN = 0x51, KEY_UP = 0x52, KEY_KEYPAD_ENTER = 0x58,
};

// The keys held now as the buttons they press, the triggers and the four stick directions.
struct PadKeys
{
	uint32_t buttons = 0;
	bool l2 = false, r2 = false;
	bool left_up = false, left_down = false, left_left = false, left_right = false;
	bool right_up = false, right_down = false, right_left = false, right_right = false;
};

inline PadKeys MapKeys(const uint16_t* keys, int count)
{
	PadKeys p;
	for (int i = 0; i < count; i++)
	{
		switch (keys[i])
		{
			case KEY_UP: p.buttons |= PAD_UP; break;
			case KEY_RIGHT: p.buttons |= PAD_RIGHT; break;
			case KEY_DOWN: p.buttons |= PAD_DOWN; break;
			case KEY_LEFT: p.buttons |= PAD_LEFT; break;
			case KEY_W: p.left_up = true; break;
			case KEY_D: p.left_right = true; break;
			case KEY_S: p.left_down = true; break;
			case KEY_A: p.left_left = true; break;
			case KEY_T: p.right_up = true; break;
			case KEY_H: p.right_right = true; break;
			case KEY_G: p.right_down = true; break;
			case KEY_F: p.right_left = true; break;
			case KEY_RETURN:
			case KEY_KEYPAD_ENTER: p.buttons |= PAD_START; break;
			case KEY_BACKSPACE: p.buttons |= PAD_SELECT; break;
			case KEY_I: p.buttons |= PAD_TRIANGLE; break;
			case KEY_L: p.buttons |= PAD_CIRCLE; break;
			case KEY_K: p.buttons |= PAD_CROSS; break;
			case KEY_J: p.buttons |= PAD_SQUARE; break;
			case KEY_Q: p.buttons |= PAD_L1; break;
			case KEY_E: p.buttons |= PAD_R1; break;
			case KEY_1: p.l2 = true; break;
			case KEY_3: p.r2 = true; break;
			case KEY_2: p.buttons |= PAD_L3; break;
			case KEY_4: p.buttons |= PAD_R3; break;
			default: break;
		}
	}
	return p;
}

// A stick's byte from the two keys of an axis: the one pressed, or the middle when neither or both are.
inline uint8_t KeyAxis(bool negative, bool positive)
{
	return negative == positive ? 128 : negative ? 0 : 255;
}

// The mouse's aim. A mouse reports how far it moved, a stick how far it is held: the movement of the last few polls
// (each poll's counts added to what is left of the ones before, four fifths of it) is the deflection, a full stick at
// a steady r counts a poll for r = 24, 12, 6 or 3 (the four speeds; 6000 to 750 counts a second). Any movement at all
// goes past the dead zone games keep round the middle, so a slow drift is a slow stick and not nothing.
constexpr float AIM_DECAY = 0.8f;
constexpr int AIM_DEAD_ZONE = 30;
inline uint8_t AimByte(float aim, float full_counts_per_poll)
{
	if (aim > -0.5f && aim < 0.5f)
		return 128;
	float deflection = aim / (full_counts_per_poll / (1.0f - AIM_DECAY));
	deflection = std::max(-1.0f, std::min(1.0f, deflection));
	const float magnitude = AIM_DEAD_ZONE + (deflection < 0 ? -deflection : deflection) * (127 - AIM_DEAD_ZONE);
	const int value = 128 + static_cast<int>(deflection < 0 ? -magnitude : magnitude);
	return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}
inline float SpeedCounts(int speed)
{
	switch (speed)
	{
		case 1: return 24.0f;
		case 3: return 6.0f;
		case 4: return 3.0f;
		default: return 12.0f;
	}
}
// Everything one poll of the thread publishes: the keys held and the mouse make one controller. `keys_to_pad` and
// `mouse_to_pad` say which of the two drive it (the mode, and a game reading the PS2's USB devices, decide that);
// `aim` is the mouse's movement (0 nothing, 1 the right stick, 2 the left), `speed` 1..4, `triggers` the mouse's buttons as
// R2 and L2 instead of R1 and L1. `mouse_buttons` are the mouse library's (bit 0 primary, 1 secondary, 2 the wheel's click).
struct PadInputs
{
	const uint16_t* keys = nullptr;
	int key_count = 0;
	uint32_t mouse_buttons = 0;
	bool keys_to_pad = false, mouse_to_pad = false;
	int aim = 1, speed = 2;
	bool triggers = false;
};

struct PadOut
{
	uint32_t buttons = 0;
	uint8_t l2 = 0, r2 = 0;
	uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
};

// The mouse's movement of the last polls, kept between them (see AimByte).
struct AimState
{
	float x = 0.0f, y = 0.0f;
};

// dx and dy: what the mouse moved since the last poll (counts).
inline PadOut ComposePad(const PadInputs& in, long dx, long dy, AimState& aim_state)
{
	PadOut out;
	if (in.keys_to_pad)
	{
		const PadKeys k = MapKeys(in.keys, in.key_count);
		out.buttons = k.buttons;
		out.l2 = k.l2 ? 255 : 0;
		out.r2 = k.r2 ? 255 : 0;
		out.lx = KeyAxis(k.left_left, k.left_right);
		out.ly = KeyAxis(k.left_up, k.left_down);
		out.rx = KeyAxis(k.right_left, k.right_right);
		out.ry = KeyAxis(k.right_up, k.right_down);
	}
	if (!in.mouse_to_pad)
	{
		aim_state.x = aim_state.y = 0.0f;
		return out;
	}
	aim_state.x = aim_state.x * AIM_DECAY + static_cast<float>(dx);
	aim_state.y = aim_state.y * AIM_DECAY + static_cast<float>(dy);
	if (in.aim == 1 || in.aim == 2)
	{
		const float full = SpeedCounts(in.speed);
		const uint8_t ax = AimByte(aim_state.x, full), ay = AimByte(aim_state.y, full);
		uint8_t& stick_x = in.aim == 1 ? out.rx : out.lx;
		uint8_t& stick_y = in.aim == 1 ? out.ry : out.ly;
		if (ax != 128) // the mouse's movement wins on its stick while it moves
			stick_x = ax;
		if (ay != 128)
			stick_y = ay;
	}
	if (in.mouse_buttons & 0x1) // primary: fire
	{
		if (in.triggers)
			out.r2 = 255;
		else
			out.buttons |= PAD_R1;
	}
	if (in.mouse_buttons & 0x2) // secondary: aim
	{
		if (in.triggers)
			out.l2 = 255;
		else
			out.buttons |= PAD_L1;
	}
	if (in.mouse_buttons & 0x4)
		out.buttons |= PAD_R3;
	return out;
}

// The keyboard's shortcuts (a keyboard has no touchpad): F1 and F3 when the key goes down, Esc after it has been held for
// a second. Update() is called every poll with the keys held and the time in milliseconds; it returns 0, or the shortcut
// that fired now: 1 F1 (save the state), 2 F3 (load it), 3 Esc (back to the menu).
struct Hotkeys
{
	bool f1 = false, f3 = false, esc = false, esc_fired = false;
	long long esc_since_ms = 0;

	static bool Has(const uint16_t* keys, int count, uint16_t key)
	{
		for (int i = 0; i < count; i++)
			if (keys[i] == key)
				return true;
		return false;
	}

	int Update(const uint16_t* keys, int count, bool active, long long now_ms)
	{
		int fired = 0;
		const bool now_f1 = active && Has(keys, count, KEY_F1);
		const bool now_f3 = active && Has(keys, count, KEY_F3);
		const bool now_esc = active && Has(keys, count, KEY_ESCAPE);
		if (now_f1 && !f1)
			fired = 1;
		if (now_f3 && !f3)
			fired = 2;
		if (!now_esc)
		{
			esc = false;
		}
		else if (!esc)
		{
			esc = true;
			esc_fired = false;
			esc_since_ms = now_ms;
		}
		else if (!esc_fired && now_ms - esc_since_ms >= 1000)
		{
			esc_fired = true;
			fired = 3;
		}
		f1 = now_f1;
		f3 = now_f3;
		return fired;
	}
};
} // namespace orbis_kbm
