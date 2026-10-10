// PS5SX2 (vk-285-116, AI-assisted): the controller remapping, as functions of what is held: no console library, no emulator,
// so tests/padmap/test_pad_map.cpp checks it on a PC. main-boot.cpp's orbis_pad_apply uses it for both PS2 ports.
//
// The settings (the Controls tab of the settings page and of the shelf's sheet; gs.ini or the game's own file):
//   PS5SX2/Button<Cross|Circle|Square|Triangle|L1|R1|L2|R2|L3|R3|Options|Touchpad|Up|Down|Left|Right> = the PS2 input that
//     controller button presses: Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Start Select Up Down Left Right Analog
//     (the DualShock 2's analog mode button) Pressure (PCSX2's pressure modifier: while held, buttons press at half
//     strength) or None. Unset: the same button (Options: Start, the touchpad's click: Select).
//   PS5SX2/SwapSticks = true: the left stick moves the PS2's right stick and the right stick its left one.
//   PS5SX2/InvertLeft, InvertRight = 0 no, 1 up-down, 2 left-right, 3 both: the PS2 stick's axes (after the swap).
//   PS5SX2/LeftStickDpad = 0 no, 1 the PS2's left stick also presses the D-pad, 2 it presses only the D-pad (the stick
//     itself stays in the middle).
//   PS5SX2/SaveButton1, SaveButton2, LoadButton1, LoadButton2 = the two buttons held together to save or load the state
//     (slot 1): L3R3 (both stick clicks) Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Options Touchpad (its click)
//     TouchLeft TouchRight (a finger on the touchpad's left or right third) Up Down Left Right, or None. Unset: L3R3 + Up
//     saves, L3R3 + Down loads. PS5SX2/StateHold = how long (seconds, 0 to 10) they're held first; 0, the default: at once.
// PS5SX2's own button combos (save states, the menu, the settings page) read the controller's real buttons before this.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace orbis_padmap
{
// What a controller button can press on the PS2 controller.
enum Target : uint8_t
{
	T_CROSS,
	T_CIRCLE,
	T_SQUARE,
	T_TRIANGLE,
	T_L1,
	T_R1,
	T_L2,
	T_R2,
	T_L3,
	T_R3,
	T_START,
	T_SELECT,
	T_UP,
	T_DOWN,
	T_LEFT,
	T_RIGHT,
	T_ANALOG,
	T_PRESSURE,
	T_NONE, // nothing
	T_COUNT = T_NONE, // the PS2 inputs (T_NONE is not one)
};

// The controller's buttons that can be remapped.
enum Source : uint8_t
{
	S_CROSS,
	S_CIRCLE,
	S_SQUARE,
	S_TRIANGLE,
	S_L1,
	S_R1,
	S_L2,
	S_R2,
	S_L3,
	S_R3,
	S_OPTIONS,
	S_TOUCHPAD,
	S_TOUCHLEFT,  // a finger on the touchpad's left third
	S_TOUCHRIGHT, // a finger on the touchpad's right third
	S_UP,
	S_DOWN,
	S_LEFT,
	S_RIGHT,
	S_COUNT,
};

struct SourceInfo
{
	const char* key;  // the setting, in [PS5SX2]
	const char* name; // for the log
	uint32_t bits;    // ScePad's button bits (L2 and R2 are read from their triggers)
	Target def;       // what it presses when the setting is unset
};

// ScePad's bits, as orbis_pad_apply always read them. The touchpad's click is 0x00100000; 0x40000000 is the keyboard's
// Select (Backspace, orbis-shims/ProsperoKbdMap.h PAD_SELECT). vk-285-122 (AI-assisted): ScePad's 0x1, the DualSense's
// Create button (the old Share, left of the touchpad), presses nothing -- it pressed Select with the touchpad's click
// until now (swordpdf: "unbind select from the share button").
inline const SourceInfo& SourceAt(int s)
{
	static const SourceInfo k[S_COUNT] = {
		{"ButtonCross", "Cross", 0x00004000u, T_CROSS},
		{"ButtonCircle", "Circle", 0x00002000u, T_CIRCLE},
		{"ButtonSquare", "Square", 0x00008000u, T_SQUARE},
		{"ButtonTriangle", "Triangle", 0x00001000u, T_TRIANGLE},
		{"ButtonL1", "L1", 0x00000400u, T_L1},
		{"ButtonR1", "R1", 0x00000800u, T_R1},
		{"ButtonL2", "L2", 0x00000100u, T_L2},
		{"ButtonR2", "R2", 0x00000200u, T_R2},
		{"ButtonL3", "L3", 0x00000002u, T_L3},
		{"ButtonR3", "R3", 0x00000004u, T_R3},
		{"ButtonOptions", "Options", 0x00000008u, T_START},
		{"ButtonTouchpad", "the touchpad's click", 0x40100000u, T_SELECT},
		{"ButtonTouchLeft", "the touchpad's left side", 0u, T_NONE},
		{"ButtonTouchRight", "the touchpad's right side", 0u, T_NONE},
		{"ButtonUp", "D-pad up", 0x00000010u, T_UP},
		{"ButtonDown", "D-pad down", 0x00000040u, T_DOWN},
		{"ButtonLeft", "D-pad left", 0x00000080u, T_LEFT},
		{"ButtonRight", "D-pad right", 0x00000020u, T_RIGHT},
	};
	return k[s];
}

// The setting's values, in Target order (T_NONE last).
inline const char* TargetName(int t)
{
	static const char* const k[T_NONE + 1] = {"Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3", "Start",
		"Select", "Up", "Down", "Left", "Right", "Analog", "Pressure", "None"};
	return (t >= 0 && t <= T_NONE) ? k[t] : "None";
}

inline bool SameText(const std::string& a, const char* b)
{
	size_t i = 0;
	for (; i < a.size() && b[i]; i++)
	{
		const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
		const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
		if (x != y)
			return false;
	}
	return i == a.size() && b[i] == '\0';
}

// "Circle" (any case, spaces around it allowed) -> T_CIRCLE. False for anything else.
inline bool ParseTarget(std::string v, Target& out)
{
	while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
		v.pop_back();
	size_t start = 0;
	while (start < v.size() && (v[start] == ' ' || v[start] == '\t'))
		start++;
	v.erase(0, start);
	for (int t = 0; t <= T_NONE; t++)
		if (SameText(v, TargetName(t)))
		{
			out = static_cast<Target>(t);
			return true;
		}
	return false;
}

// vk-285-117: what a save or load combo's button can be.
enum ComboButton : uint8_t
{
	CB_NONE,
	CB_L3R3, // both stick clicks
	CB_CROSS,
	CB_CIRCLE,
	CB_SQUARE,
	CB_TRIANGLE,
	CB_L1,
	CB_R1,
	CB_L2,
	CB_R2,
	CB_L3,
	CB_R3,
	CB_OPTIONS,
	CB_TOUCHPAD,    // its click
	CB_TOUCH_LEFT,  // a finger on its left third (eerec-284's zone)
	CB_TOUCH_RIGHT, // on its right third
	CB_UP,
	CB_DOWN,
	CB_LEFT,
	CB_RIGHT,
	CB_COUNT,
};

// The settings' values, in ComboButton order.
inline const char* ComboButtonName(int b)
{
	static const char* const k[CB_COUNT] = {"None", "L3R3", "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3",
		"Options", "Touchpad", "TouchLeft", "TouchRight", "Up", "Down", "Left", "Right"};
	return (b >= 0 && b < CB_COUNT) ? k[b] : "None";
}

// For the log.
inline const char* ComboButtonLabel(int b)
{
	static const char* const k[CB_COUNT] = {"nothing", "L3+R3", "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3",
		"R3", "Options", "the touchpad's click", "the touchpad's left side", "the touchpad's right side", "D-pad up", "D-pad down",
		"D-pad left", "D-pad right"};
	return (b >= 0 && b < CB_COUNT) ? k[b] : "nothing";
}

inline bool ParseComboButton(std::string v, ComboButton& out)
{
	while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
		v.pop_back();
	size_t start = 0;
	while (start < v.size() && (v[start] == ' ' || v[start] == '\t'))
		start++;
	v.erase(0, start);
	for (int b = 0; b < CB_COUNT; b++)
		if (SameText(v, ComboButtonName(b)))
		{
			out = static_cast<ComboButton>(b);
			return true;
		}
	return false;
}

// The ScePad bits a combo button is (0 for the touchpad's zones, which are a touch, not a button; L2 and R2 are also
// held by their triggers).
inline uint32_t ComboBits(ComboButton b)
{
	switch (b)
	{
		case CB_L3R3: return 0x00000006u;
		case CB_CROSS: return 0x00004000u;
		case CB_CIRCLE: return 0x00002000u;
		case CB_SQUARE: return 0x00008000u;
		case CB_TRIANGLE: return 0x00001000u;
		case CB_L1: return 0x00000400u;
		case CB_R1: return 0x00000800u;
		case CB_L2: return 0x00000100u;
		case CB_R2: return 0x00000200u;
		case CB_L3: return 0x00000002u;
		case CB_R3: return 0x00000004u;
		case CB_OPTIONS: return 0x00000008u;
		case CB_TOUCHPAD: return 0x40100000u; // the click, and the keyboard's Backspace (as the remapping's source)
		case CB_UP: return 0x00000010u;
		case CB_DOWN: return 0x00000040u;
		case CB_LEFT: return 0x00000080u;
		case CB_RIGHT: return 0x00000020u;
		default: return 0;
	}
}

struct Config
{
	Target target[S_COUNT];
	// vk-285-118: how hard each button presses what it presses, 0.05..1 (PS5SX2/Button<Name>Pressure; 1, a full press, by
	// default). A DualShock 2's face buttons, D-pad and shoulders read pressure, and some games act on a partial press: SOCOM
	// II crouches at about 0.20 on Triangle, Combined Assault at 0.30 (a full press goes prone). The triggers press at most
	// this hard. Targets that only read on or off (Start, Select, L3, R3, Analog, Light press) press the same at any strength.
	float pressure[S_COUNT];
	bool swap_sticks = false;
	uint8_t left_dpad = 0;    // 0 no, 1 the D-pad too, 2 the D-pad only
	// vk-285-118: each stick's dead zone, percent of the way out (0..50; PS5SX2/DeadzoneLeft, DeadzoneRight): inside it the
	// stick reads as centred, past it the rest of the way is stretched over the whole range (no jump at its edge). For
	// sticks that drift, or games that read a resting stick as a push. Of the controller's sticks, before Swap sticks.
	uint8_t deadzone_left = 0;
	uint8_t deadzone_right = 0;
	uint8_t invert_left = 0;  // bit 0 up-down, bit 1 left-right
	uint8_t invert_right = 0;
	// vk-285-117: the save and load state combos (two buttons each) and how long they're held first.
	ComboButton save[2] = {CB_L3R3, CB_UP};
	ComboButton load[2] = {CB_L3R3, CB_DOWN};
	int hold_ms = 0;
	// vk-285-118: the fast forward combo (PS5SX2/FastButton1/2, experimental, to skip videos): it turns fast forward on and
	// off, after the same hold time. Nothing on both (the default): no combo.
	ComboButton fast[2] = {CB_NONE, CB_NONE};
	// vk-285-139: the change disc combo (PS5SX2/DiscButton1/2): the next disc of the game's set ("(Disc 2)" beside it).
	ComboButton disc[2] = {CB_L3R3, CB_RIGHT};
	// the open-settings-page combo (PS5SX2/WebButton1/2): opens this game's settings page in the PS5's browser (2 s hold, fixed).
	ComboButton web[2] = {CB_L2, CB_DOWN};

	Config()
	{
		for (int s = 0; s < S_COUNT; s++)
		{
			target[s] = SourceAt(s).def;
			pressure[s] = 1.0f;
		}
	}

	bool operator==(const Config& o) const
	{
		for (int s = 0; s < S_COUNT; s++)
			if (target[s] != o.target[s] || pressure[s] != o.pressure[s])
				return false;
		return swap_sticks == o.swap_sticks && left_dpad == o.left_dpad && deadzone_left == o.deadzone_left &&
		       deadzone_right == o.deadzone_right && invert_left == o.invert_left && invert_right == o.invert_right &&
		       save[0] == o.save[0] && save[1] == o.save[1] && load[0] == o.load[0] && load[1] == o.load[1] && hold_ms == o.hold_ms &&
		       fast[0] == o.fast[0] && fast[1] == o.fast[1] && disc[0] == o.disc[0] && disc[1] == o.disc[1] &&
		       web[0] == o.web[0] && web[1] == o.web[1];
	}
	bool operator!=(const Config& o) const { return !(*this == o); }
	bool IsDefault() const { return *this == Config(); }
};

inline bool Truthy(const std::string& v)
{
	return SameText(v, "true") || v == "1" || SameText(v, "on") || SameText(v, "yes");
}

// A small number setting (0..max); anything else is 0.
inline uint8_t SmallInt(const std::string& v, int max)
{
	char* end = nullptr;
	const long n = std::strtol(v.c_str(), &end, 10);
	if (v.empty() || end == v.c_str() || n < 0 || n > max)
		return 0;
	return static_cast<uint8_t>(n);
}

// vk-285-118: a button's strength, "0.2" or "20%"; anything outside 0.05..1, or not a number, is a full press.
inline float ParsePressure(const std::string& v)
{
	char* end = nullptr;
	double p = std::strtod(v.c_str(), &end);
	if (v.empty() || end == v.c_str())
		return 1.0f;
	if (*end == '%')
		p /= 100.0;
	return (p >= 0.05 && p <= 1.0) ? static_cast<float>(p) : 1.0f;
}

// The settings, read with `get(key, value)` (the key within [PS5SX2]; true when it is set). An unknown value leaves that
// button or stick option as it is by default.
template <typename Get>
inline Config FromSettings(Get get)
{
	Config c;
	std::string v;
	for (int s = 0; s < S_COUNT; s++)
	{
		v.clear();
		Target t;
		if (get(SourceAt(s).key, v) && ParseTarget(v, t))
			c.target[s] = t;
		v.clear();
		if (get((std::string(SourceAt(s).key) + "Pressure").c_str(), v))
			c.pressure[s] = ParsePressure(v);
	}
	v.clear();
	if (get("SwapSticks", v))
		c.swap_sticks = Truthy(v);
	v.clear();
	if (get("LeftStickDpad", v))
		c.left_dpad = SmallInt(v, 2);
	v.clear();
	if (get("DeadzoneLeft", v))
		c.deadzone_left = SmallInt(v, 50);
	v.clear();
	if (get("DeadzoneRight", v))
		c.deadzone_right = SmallInt(v, 50);
	v.clear();
	if (get("InvertLeft", v))
		c.invert_left = SmallInt(v, 3);
	v.clear();
	if (get("InvertRight", v))
		c.invert_right = SmallInt(v, 3);
	// vk-285-117: the save and load combos; a value that isn't a button leaves that one at its default.
	static const char* const combo_keys[10] = {"SaveButton1", "SaveButton2", "LoadButton1", "LoadButton2", "FastButton1", "FastButton2",
		"DiscButton1", "DiscButton2", "WebButton1", "WebButton2"};
	ComboButton* const combo[10] = {&c.save[0], &c.save[1], &c.load[0], &c.load[1], &c.fast[0], &c.fast[1], &c.disc[0], &c.disc[1],
		&c.web[0], &c.web[1]};
	for (int i = 0; i < 10; i++)
	{
		v.clear();
		ComboButton b;
		if (get(combo_keys[i], v) && ParseComboButton(v, b))
			*combo[i] = b;
	}
	v.clear();
	if (get("StateHold", v))
	{
		char* end = nullptr;
		const double seconds = std::strtod(v.c_str(), &end);
		if (end != v.c_str() && seconds >= 0.0 && seconds <= 10.0)
			c.hold_ms = static_cast<int>(seconds * 1000.0 + 0.5);
	}
	return c;
}

// vk-285-117: the save and load combos. What the controller holds now, with the touchpad's zone (0 no finger there or the
// middle third, 1 the left third, 2 the right third).
struct ComboState
{
	uint32_t buttons = 0;
	uint8_t l2 = 0, r2 = 0;
	int touch = 0;
};

inline bool ComboHeld(ComboButton b, const ComboState& s)
{
	switch (b)
	{
		case CB_NONE: return false;
		case CB_L3R3: return (s.buttons & 0x6u) == 0x6u;
		case CB_L2: return (s.buttons & 0x100u) != 0 || s.l2 >= 200u; // the trigger past 200 of 255, as the other combos read it
		case CB_R2: return (s.buttons & 0x200u) != 0 || s.r2 >= 200u;
		case CB_TOUCH_LEFT: return s.touch == 1;
		case CB_TOUCH_RIGHT: return s.touch == 2;
		default: return (s.buttons & ComboBits(b)) != 0;
	}
}

// One combo (save, or load): it fires once its buttons have been held together for hold_ms, and again only after one of
// them was let go. Nothing, or the same button twice, makes it a one-button combo; nothing twice turns it off. When it
// fires at once (hold 0), the press that completed it is kept from the game until that button is let go (as L3+R3 +
// D-pad's D-pad press); with a hold, the game sees the buttons until the combo fires.
struct ComboWatch
{
	bool armed = true;
	bool timing = false;
	long long since_ms = 0;
	uint32_t held_back = 0;
	bool was_held[2] = {false, false};

	// `block` gets the bits to keep from the game now (0x100 / 0x200: also that trigger's pull). True when it fires.
	bool Update(const ComboButton (&b)[2], int hold_ms, const ComboState& s, long long now_ms, uint32_t& block)
	{
		ComboButton list[2] = {CB_NONE, CB_NONE};
		int n = 0;
		for (ComboButton x : b)
			if (x != CB_NONE && x < CB_COUNT && (n == 0 || list[0] != x))
				list[n++] = x;
		bool fired = false;
		if (n == 0)
		{
			armed = true;
			timing = false;
			was_held[0] = was_held[1] = false;
		}
		else
		{
			bool held[2] = {false, false};
			bool all = true;
			for (int i = 0; i < n; i++)
			{
				held[i] = ComboHeld(list[i], s);
				all = all && held[i];
			}
			if (!all)
			{
				armed = true;
				timing = false;
			}
			else if (armed)
			{
				if (!timing)
				{
					timing = true;
					since_ms = now_ms;
				}
				if (now_ms - since_ms >= hold_ms)
				{
					fired = true;
					armed = false;
					timing = false;
					if (hold_ms <= 0)
						for (int i = 0; i < n; i++)
							if (!was_held[i])
								held_back |= ComboBits(list[i]);
				}
			}
			for (int i = 0; i < 2; i++)
				was_held[i] = i < n && held[i];
		}
		// A button let go reaches the game again (a trigger: once it's fully let go).
		held_back &= s.buttons | (s.l2 != 0 ? 0x100u : 0u) | (s.r2 != 0 ? 0x200u : 0u);
		block |= held_back;
		return fired;
	}
};

// "L3+R3 + D-pad up", "the touchpad's click held 1.5 s", "nothing".
inline std::string DescribeCombo(const ComboButton (&b)[2], int hold_ms)
{
	std::string out;
	ComboButton seen = CB_NONE;
	for (ComboButton x : b)
	{
		if (x == CB_NONE || x >= CB_COUNT || x == seen)
			continue;
		out += (out.empty() ? "" : " + ") + std::string(ComboButtonLabel(x));
		seen = x;
	}
	if (out.empty())
		return "nothing";
	if (hold_ms > 0)
	{
		char buf[32];
		std::snprintf(buf, sizeof(buf), " held %g s", hold_ms / 1000.0);
		out += buf;
	}
	return out;
}

// What the controller holds now (OrbisPadData's fields; the keyboard and mouse already added in).
struct State
{
	uint32_t buttons = 0;
	uint8_t l2 = 0, r2 = 0;
	uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
	int touch = 0; // 0 no finger or middle third, 1 left third, 2 right third
};

// What the PS2 controller gets: each input's value (0 released .. 1 fully pressed; the face buttons, the D-pad and the
// shoulders are pressure sensitive on a DualShock 2) and the sticks (0..255, 128 the middle).
struct Out
{
	float value[T_COUNT] = {};
	uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
};

// A trigger on anything but a trigger presses once it is pulled past an eighth (then as hard as it is pulled), so a
// finger resting on it presses nothing.
constexpr uint8_t kTriggerPress = 32;
// How far a stick is pushed (from the middle, of 128) before it presses the D-pad (Config::left_dpad).
constexpr int kStickDpad = 64;

inline uint8_t Invert(uint8_t v)
{
	return static_cast<uint8_t>(std::min(255, 256 - static_cast<int>(v)));
}

// vk-285-118: a stick through a round dead zone of `percent` (Config::deadzone_left/right).
inline void DeadZone(uint8_t& x, uint8_t& y, int percent)
{
	if (percent <= 0)
		return;
	const float dx = (static_cast<float>(x) - 128.0f) / 127.0f, dy = (static_cast<float>(y) - 128.0f) / 127.0f;
	const float r = std::sqrt(dx * dx + dy * dy), dz = static_cast<float>(percent) / 100.0f;
	if (r <= dz)
	{
		x = y = 128;
		return;
	}
	const float k = std::min(1.0f, (r - dz) / (1.0f - dz)) / r;
	auto axis = [&](float d) {
		const int v = static_cast<int>(std::lround(128.0f + d * k * 127.0f));
		return static_cast<uint8_t>(std::max(0, std::min(255, v)));
	};
	x = axis(dx);
	y = axis(dy);
}

inline Out Apply(const Config& c, const State& s)
{
	Out o;
	for (int src = 0; src < S_COUNT; src++)
	{
		const Target t = c.target[src];
		if (t >= T_NONE)
			continue;
		float v;
		if (src == S_L2 || src == S_R2)
		{
			const uint8_t raw = src == S_L2 ? s.l2 : s.r2;
			const bool to_trigger = t == T_L2 || t == T_R2;
			v = (to_trigger || raw >= kTriggerPress) ? raw / 255.0f * c.pressure[src] : 0.0f;
		}
		else if (src == S_TOUCHLEFT)
			v = (s.touch == 1) ? c.pressure[src] : 0.0f;
		else if (src == S_TOUCHRIGHT)
			v = (s.touch == 2) ? c.pressure[src] : 0.0f;
		else
			v = (s.buttons & SourceAt(src).bits) ? c.pressure[src] : 0.0f;
		// PCSX2 reads the analog button and the pressure modifier as on or off (it acts when they change).
		if ((t == T_ANALOG || t == T_PRESSURE) && v > 0.0f)
			v = 1.0f;
		o.value[t] = std::max(o.value[t], v);
	}

	uint8_t lx = s.lx, ly = s.ly, rx = s.rx, ry = s.ry;
	DeadZone(lx, ly, c.deadzone_left);
	DeadZone(rx, ry, c.deadzone_right);
	if (c.swap_sticks)
	{
		std::swap(lx, rx);
		std::swap(ly, ry);
	}
	if (c.invert_left & 1)
		ly = Invert(ly);
	if (c.invert_left & 2)
		lx = Invert(lx);
	if (c.invert_right & 1)
		ry = Invert(ry);
	if (c.invert_right & 2)
		rx = Invert(rx);
	if (c.left_dpad != 0)
	{
		if (lx <= 128 - kStickDpad)
			o.value[T_LEFT] = 1.0f;
		if (lx >= 128 + kStickDpad)
			o.value[T_RIGHT] = 1.0f;
		if (ly <= 128 - kStickDpad)
			o.value[T_UP] = 1.0f;
		if (ly >= 128 + kStickDpad)
			o.value[T_DOWN] = 1.0f;
		if (c.left_dpad == 2)
			lx = ly = 128;
	}
	o.lx = lx;
	o.ly = ly;
	o.rx = rx;
	o.ry = ry;
	return o;
}

// For the log: "Cross presses Circle, Circle presses Cross; sticks swapped" or "as on the controller".
inline std::string Describe(const Config& c)
{
	std::string buttons, sticks;
	for (int s = 0; s < S_COUNT; s++)
	{
		const Target t = c.target[s];
		if (t == SourceAt(s).def && c.pressure[s] == 1.0f)
			continue;
		if (!buttons.empty())
			buttons += ", ";
		buttons += SourceAt(s).name;
		buttons += t == T_NONE ? std::string(" presses nothing") : std::string(" presses ") + TargetName(t);
		if (t != T_NONE && c.pressure[s] != 1.0f)
			buttons += " at " + std::to_string(static_cast<int>(c.pressure[s] * 100.0f + 0.5f)) + "%";
	}
	auto add = [&](const std::string& what) { sticks += (sticks.empty() ? "" : ", ") + what; };
	static const char* const axes[4] = {"", "up-down", "left-right", "up-down and left-right"};
	if (c.swap_sticks)
		add("sticks swapped");
	if (c.invert_left)
		add(std::string("left stick ") + axes[c.invert_left & 3] + " inverted");
	if (c.invert_right)
		add(std::string("right stick ") + axes[c.invert_right & 3] + " inverted");
	if (c.deadzone_left)
		add("left stick dead zone " + std::to_string(c.deadzone_left) + "%");
	if (c.deadzone_right)
		add("right stick dead zone " + std::to_string(c.deadzone_right) + "%");
	if (c.left_dpad == 1)
		add("left stick also the D-pad");
	if (c.left_dpad == 2)
		add("left stick as the D-pad only");
	// vk-285-117: the save and load combos, when they aren't the default.
	const Config def;
	std::string states;
	if (c.save[0] != def.save[0] || c.save[1] != def.save[1] || c.load[0] != def.load[0] || c.load[1] != def.load[1] ||
		c.hold_ms != def.hold_ms)
		states = "save state on " + DescribeCombo(c.save, c.hold_ms) + ", load on " + DescribeCombo(c.load, c.hold_ms);
	if (c.fast[0] != CB_NONE || c.fast[1] != CB_NONE)
		states += (states.empty() ? "" : ", ") + std::string("fast forward on ") + DescribeCombo(c.fast, c.hold_ms);
	if (c.web[0] != def.web[0] || c.web[1] != def.web[1])
		states += (states.empty() ? "" : ", ") + std::string("settings page on ") + DescribeCombo(c.web, 2000);
	std::string out = buttons;
	for (const std::string* part : {&sticks, &states})
		if (!part->empty())
			out += (out.empty() ? "" : "; ") + *part;
	return out.empty() ? "as on the controller" : out;
}
} // namespace orbis_padmap
