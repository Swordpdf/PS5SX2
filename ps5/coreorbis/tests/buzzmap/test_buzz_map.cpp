// PS5SX2 (AI-assisted): a check of the Buzz! buzzers' buttons (orbis-shims/OrbisBuzzMap.h) on a PC.
//   g++ -std=c++17 -Wall -I../../orbis-shims -o /tmp/test_buzz_map test_buzz_map.cpp && /tmp/test_buzz_map
#include "OrbisBuzzMap.h"

#include <cstdio>
#include <map>
#include <string>

using namespace orbis_buzzmap;

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

// Settings as a file would hold them.
static Config From(std::map<std::string, std::string> kv)
{
	return FromSettings([&](const char* key, std::string& v) {
		const auto it = kv.find(key);
		if (it == kv.end())
			return false;
		v = it->second;
		return true;
	});
}

static uint8_t Bit(Color c)
{
	return static_cast<uint8_t>(1u << c);
}

int main()
{
	// ScePad's bits (OrbisPadMap.h SourceAt).
	const uint32_t cross = 0x4000u, circle = 0x2000u, square = 0x8000u, triangle = 0x1000u, r2 = 0x200u, l1 = 0x400u, up = 0x10u;

	// Nothing set: R2 red, Triangle blue, Circle orange, Cross green, Square yellow.
	{
		const Config c = From({});
		CHECK(c == Config());
		CHECK(Apply(c, 0, 0, 0) == 0);
		CHECK(Apply(c, r2, 0, 0) == Bit(RED));
		CHECK(Apply(c, triangle, 0, 0) == Bit(BLUE));
		CHECK(Apply(c, circle, 0, 0) == Bit(ORANGE));
		CHECK(Apply(c, cross, 0, 0) == Bit(GREEN));
		CHECK(Apply(c, square, 0, 0) == Bit(YELLOW));
		CHECK(Apply(c, cross | square, 0, 0) == (Bit(GREEN) | Bit(YELLOW)));
		CHECK(Apply(c, l1 | up, 0, 0) == 0); // not mapped
		CHECK(Describe(c) == "red R2, blue Triangle, orange Circle, green Cross, yellow Square");
	}

	// R2 (and L2) press from the trigger alone, a little past the top.
	{
		const Config c = From({{"BuzzBlue", "L2"}});
		CHECK(Apply(c, 0, 0, 63) == 0);
		CHECK(Apply(c, 0, 0, 64) == Bit(RED));
		CHECK(Apply(c, 0, 200, 0) == Bit(BLUE));
	}

	// Any case, spaces around it; a value that isn't a button keeps the default.
	{
		const Config c = From({{"BuzzRed", "  cross "}, {"BuzzGreen", "Banana"}, {"BuzzYellow", "UP"}});
		CHECK(c.src[RED] == orbis_padmap::S_CROSS);
		CHECK(c.src[GREEN] == orbis_padmap::S_CROSS); // the default, kept
		CHECK(c.src[YELLOW] == orbis_padmap::S_UP);
		// Two colors on one button: both press.
		CHECK(Apply(c, cross, 0, 0) == (Bit(RED) | Bit(GREEN)));
		CHECK(Apply(c, up, 0, 0) == Bit(YELLOW));
	}

	// None never presses; the touchpad's sides can't be picked (they need the finger's place).
	{
		const Config c = From({{"BuzzRed", "None"}, {"BuzzBlue", "TouchLeft"}, {"BuzzOrange", "Touchpad"}});
		CHECK(c.src[RED] == kNone);
		CHECK(c.src[BLUE] == orbis_padmap::S_TRIANGLE); // TouchLeft refused, the default kept
		CHECK(c.src[ORANGE] == orbis_padmap::S_TOUCHPAD);
		CHECK(Apply(c, r2, 0, 255) == 0);
		CHECK(Apply(c, 0x00100000u, 0, 0) == Bit(ORANGE)); // the touchpad's click
		CHECK(Apply(c, 0x40000000u, 0, 0) == Bit(ORANGE)); // the keyboard's Backspace, Select as for the remapping
		CHECK(c != Config());
	}

	if (s_failures == 0)
		printf("test_buzz_map: all checks passed\n");
	return s_failures == 0 ? 0 : 1;
}
