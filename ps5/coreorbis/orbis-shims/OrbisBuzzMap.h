// PS5SX2 (AI-assisted): the Buzz! buzzers' buttons, as functions of what a controller holds: no console library, no emulator,
// so tests/buzzmap/test_buzz_map.cpp checks it on a PC. main-boot.cpp's orbis_buzz_apply uses it for each player's controller.
//
// The settings (the Controls tab's Buzz! buzzers group; gs.ini or the game's own file):
//   PS5SX2/Buzz = auto (a Buzz! game: the game database's title holds "buzz!"), 0 off, 1 on: PCSX2's Buzz device on the PS2's
//     USB port 1. Player 1's controller is buzzer 1; the other PS5 users logged in when the game starts are buzzers 2 to 4.
//   PS5SX2/BuzzRed, BuzzBlue, BuzzOrange, BuzzGreen, BuzzYellow = the controller button that presses that buzzer button:
//     Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Options Touchpad Up Down Left Right, or None. Unset: R2 (the big red
//     button), Triangle, Circle, Cross, Square.
#pragma once

#include "OrbisPadMap.h"

#include <cstdint>
#include <string>

namespace orbis_buzzmap
{
// A buzzer's buttons, in usb-buzz.h's order (CID_BUZZ_PLAYER1_RED + player * COUNT + color).
enum Color : uint8_t
{
	RED,
	BLUE,
	ORANGE,
	GREEN,
	YELLOW,
	COUNT,
};

// orbis_padmap::S_COUNT stands for None.
constexpr orbis_padmap::Source kNone = orbis_padmap::S_COUNT;

struct Config
{
	orbis_padmap::Source src[COUNT] = {orbis_padmap::S_R2, orbis_padmap::S_TRIANGLE, orbis_padmap::S_CIRCLE, orbis_padmap::S_CROSS,
		orbis_padmap::S_SQUARE};

	bool operator==(const Config& o) const
	{
		for (int i = 0; i < COUNT; i++)
			if (src[i] != o.src[i])
				return false;
		return true;
	}
	bool operator!=(const Config& o) const { return !(*this == o); }
};

inline const char* ColorKey(int c)
{
	static const char* const k[COUNT] = {"BuzzRed", "BuzzBlue", "BuzzOrange", "BuzzGreen", "BuzzYellow"};
	return (c >= 0 && c < COUNT) ? k[c] : "";
}

inline const char* ColorName(int c)
{
	static const char* const k[COUNT] = {"red", "blue", "orange", "green", "yellow"};
	return (c >= 0 && c < COUNT) ? k[c] : "";
}

// The setting's name for a button: its padmap key without "Button" ("ButtonCross" -> "Cross"). The touchpad's sides are
// left out (they need where the finger is, not just the buttons).
inline bool Pickable(int s)
{
	return s >= 0 && s < orbis_padmap::S_COUNT && s != orbis_padmap::S_TOUCHLEFT && s != orbis_padmap::S_TOUCHRIGHT;
}

inline const char* ButtonName(int s)
{
	return Pickable(s) ? orbis_padmap::SourceAt(s).key + 6 : "None";
}

// "Triangle" (any case, spaces around it allowed) -> S_TRIANGLE, "None" -> kNone. False for anything else.
inline bool ParseButton(std::string v, orbis_padmap::Source& out)
{
	while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
		v.pop_back();
	size_t start = 0;
	while (start < v.size() && (v[start] == ' ' || v[start] == '\t'))
		start++;
	v.erase(0, start);
	if (orbis_padmap::SameText(v, "None"))
	{
		out = kNone;
		return true;
	}
	for (int s = 0; s < orbis_padmap::S_COUNT; s++)
		if (Pickable(s) && orbis_padmap::SameText(v, ButtonName(s)))
		{
			out = static_cast<orbis_padmap::Source>(s);
			return true;
		}
	return false;
}

// The settings, read with `get(key, value)` (the key within [PS5SX2]; true when it is set). A value that isn't a button
// leaves that color at its default.
template <typename Get>
inline Config FromSettings(Get get)
{
	Config c;
	std::string v;
	for (int i = 0; i < COUNT; i++)
	{
		v.clear();
		orbis_padmap::Source s;
		if (get(ColorKey(i), v) && ParseButton(v, s))
			c.src[i] = s;
	}
	return c;
}

// The buzzer buttons held (bit `color`), from a controller's ScePad buttons and triggers. L2 and R2 also press from their
// trigger, a little past the top, as the guitar does.
inline uint8_t Apply(const Config& c, uint32_t buttons, uint8_t l2, uint8_t r2)
{
	uint8_t held = 0;
	for (int i = 0; i < COUNT; i++)
	{
		const int s = c.src[i];
		if (!Pickable(s))
			continue;
		bool on = (buttons & orbis_padmap::SourceAt(s).bits) != 0;
		if (s == orbis_padmap::S_L2)
			on = on || l2 >= 64;
		else if (s == orbis_padmap::S_R2)
			on = on || r2 >= 64;
		if (on)
			held |= static_cast<uint8_t>(1u << i);
	}
	return held;
}

// "red R2, blue Triangle, ..." for the log.
inline std::string Describe(const Config& c)
{
	std::string out;
	for (int i = 0; i < COUNT; i++)
		out += std::string(i ? ", " : "") + ColorName(i) + " " + ButtonName(c.src[i]);
	return out;
}
} // namespace orbis_buzzmap
