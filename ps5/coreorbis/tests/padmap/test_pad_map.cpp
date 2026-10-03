// PS5SX2 (vk-285-116, AI-assisted): a check of the controller remapping (orbis-shims/OrbisPadMap.h) on a PC.
//   g++ -std=c++17 -Wall -I../../orbis-shims -o /tmp/test_pad_map test_pad_map.cpp && /tmp/test_pad_map
#include "OrbisPadMap.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

using namespace orbis_padmap;

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

static State Held(uint32_t buttons)
{
	State s;
	s.buttons = buttons;
	return s;
}

static bool Only(const Out& o, std::initializer_list<Target> on)
{
	for (int t = 0; t < T_COUNT; t++)
	{
		bool want = false;
		for (Target x : on)
			want = want || x == t;
		if ((o.value[t] > 0.0f) != want)
			return false;
	}
	return true;
}

int main()
{
	// Nothing set: every button presses its own, as orbis_pad_apply did before (touchpad click and Backspace are Select,
	// Options is Start), the triggers are analog, the sticks pass through.
	{
		const Config c = From({});
		CHECK(c.IsDefault());
		CHECK(Describe(c) == "as on the controller");
		CHECK(Only(Apply(c, Held(0)), {}));
		CHECK(Only(Apply(c, Held(0x4000)), {T_CROSS}));
		CHECK(Only(Apply(c, Held(0x2000)), {T_CIRCLE}));
		CHECK(Only(Apply(c, Held(0x8000)), {T_SQUARE}));
		CHECK(Only(Apply(c, Held(0x1000)), {T_TRIANGLE}));
		CHECK(Only(Apply(c, Held(0x0400)), {T_L1}));
		CHECK(Only(Apply(c, Held(0x0800)), {T_R1}));
		CHECK(Only(Apply(c, Held(0x0002)), {T_L3}));
		CHECK(Only(Apply(c, Held(0x0004)), {T_R3}));
		CHECK(Only(Apply(c, Held(0x0008)), {T_START}));
		CHECK(Only(Apply(c, Held(0x00100000)), {T_SELECT}));
		CHECK(Only(Apply(c, Held(0x40000000)), {T_SELECT})); // the keyboard's Backspace
		CHECK(Only(Apply(c, Held(0x00000001)), {}));        // vk-285-122: the Create (Share) button presses nothing
		CHECK(Only(Apply(c, Held(0x0010)), {T_UP}));
		CHECK(Only(Apply(c, Held(0x0040)), {T_DOWN}));
		CHECK(Only(Apply(c, Held(0x0080)), {T_LEFT}));
		CHECK(Only(Apply(c, Held(0x0020)), {T_RIGHT}));
		// The L2/R2 bits alone press nothing (the triggers' values do), as before.
		CHECK(Only(Apply(c, Held(0x0100 | 0x0200)), {}));
		State s;
		s.l2 = 100;
		s.r2 = 255;
		s.lx = 0;
		s.ly = 255;
		s.rx = 40;
		s.ry = 200;
		const Out o = Apply(c, s);
		CHECK(std::fabs(o.value[T_L2] - 100 / 255.0f) < 1e-6f && o.value[T_R2] == 1.0f);
		CHECK(o.lx == 0 && o.ly == 255 && o.rx == 40 && o.ry == 200);
		// A light pull of a trigger still reaches the PS2's trigger (it is analog there too).
		s.l2 = 5;
		CHECK(Apply(c, s).value[T_L2] > 0.0f);
		// Everything at once.
		CHECK(Only(Apply(c, Held(0x00104EFE | 0xF000)), {T_CROSS, T_CIRCLE, T_SQUARE, T_TRIANGLE, T_L1, T_R1, T_L3, T_R3, T_START,
														T_SELECT, T_UP, T_DOWN, T_LEFT, T_RIGHT}));
	}

	// Cross and Circle swapped (Japanese games), Square off, Triangle on Select; the file's spelling may vary.
	{
		const Config c = From({{"ButtonCross", "Circle"}, {"ButtonCircle", " cross "}, {"ButtonSquare", "None"}, {"ButtonTriangle", "SELECT"}});
		CHECK(!c.IsDefault());
		CHECK(Only(Apply(c, Held(0x4000)), {T_CIRCLE}));
		CHECK(Only(Apply(c, Held(0x2000)), {T_CROSS}));
		CHECK(Only(Apply(c, Held(0x8000)), {}));
		CHECK(Only(Apply(c, Held(0x1000)), {T_SELECT}));
		CHECK(Only(Apply(c, Held(0x1000 | 0x00100000)), {T_SELECT}));
		CHECK(Describe(c) == "Cross presses Circle, Circle presses Cross, Square presses nothing, Triangle presses Select");
	}

	// Unknown values and settings are left at their defaults.
	{
		const Config c = From({{"ButtonCross", "Jump"}, {"ButtonR1", ""}, {"LeftStickDpad", "7"}, {"InvertLeft", "x"}, {"SwapSticks", "maybe"}});
		CHECK(c.IsDefault());
	}

	// Two buttons on one: the stronger press counts, and releasing one keeps the other's.
	{
		const Config c = From({{"ButtonL1", "Cross"}});
		CHECK(Only(Apply(c, Held(0x0400)), {T_CROSS}));
		CHECK(Only(Apply(c, Held(0x0400 | 0x4000)), {T_CROSS}));
		CHECK(Only(Apply(c, Held(0x4000)), {T_CROSS}));
	}

	// A trigger on a face button: pressure follows the pull, past a light touch.
	{
		const Config c = From({{"ButtonR2", "Square"}, {"ButtonL2", "R2"}});
		State s;
		s.r2 = kTriggerPress - 1;
		CHECK(Apply(c, s).value[T_SQUARE] == 0.0f);
		s.r2 = 128;
		CHECK(std::fabs(Apply(c, s).value[T_SQUARE] - 128 / 255.0f) < 1e-6f);
		CHECK(Apply(c, s).value[T_R2] == 0.0f); // R2 no longer presses R2
		s.r2 = 0;
		s.l2 = 3; // L2 on R2: trigger to trigger, analog all the way
		CHECK(std::fabs(Apply(c, s).value[T_R2] - 3 / 255.0f) < 1e-6f && Apply(c, s).value[T_L2] == 0.0f);
	}

	// A button on a trigger is a full pull.
	{
		const Config c = From({{"ButtonL1", "L2"}, {"ButtonR1", "R2"}, {"ButtonL2", "L1"}, {"ButtonR2", "R1"}});
		CHECK(Only(Apply(c, Held(0x0400)), {T_L2}) && Apply(c, Held(0x0400)).value[T_L2] == 1.0f);
		State s;
		s.l2 = 255;
		s.r2 = 200;
		CHECK(Only(Apply(c, s), {T_L1, T_R1}));
	}

	// The analog button and the pressure modifier are on or off, even from a trigger.
	{
		const Config c = From({{"ButtonTouchpad", "Analog"}, {"ButtonL2", "Pressure"}});
		CHECK(Only(Apply(c, Held(0x00100000)), {T_ANALOG}) && Apply(c, Held(0x00100000)).value[T_ANALOG] == 1.0f);
		State s;
		s.l2 = 90;
		CHECK(Apply(c, s).value[T_PRESSURE] == 1.0f);
		s.l2 = 20;
		CHECK(Apply(c, s).value[T_PRESSURE] == 0.0f);
	}

	// The D-pad elsewhere and Options on Select.
	{
		const Config c = From({{"ButtonUp", "Triangle"}, {"ButtonDown", "Cross"}, {"ButtonOptions", "Select"}, {"ButtonTouchpad", "Start"}});
		CHECK(Only(Apply(c, Held(0x0010)), {T_TRIANGLE}));
		CHECK(Only(Apply(c, Held(0x0040)), {T_CROSS}));
		CHECK(Only(Apply(c, Held(0x0008)), {T_SELECT}));
		CHECK(Only(Apply(c, Held(0x00100000)), {T_START}));
		CHECK(Only(Apply(c, Held(0x40000000)), {T_START})); // the keyboard's Backspace follows the touchpad's click
		CHECK(Only(Apply(c, Held(0x00000001)), {}));        // vk-285-122: the Create (Share) button follows nothing
	}

	// Sticks: swapped, then inverted (the PS2's sticks), then the left one on the D-pad.
	{
		State s;
		s.lx = 10;
		s.ly = 20;
		s.rx = 30;
		s.ry = 250;
		Out o = Apply(From({{"SwapSticks", "true"}}), s);
		CHECK(o.lx == 30 && o.ly == 250 && o.rx == 10 && o.ry == 20);
		o = Apply(From({{"InvertRight", "1"}}), s);
		CHECK(o.rx == 30 && o.ry == 6 && o.lx == 10 && o.ly == 20);
		o = Apply(From({{"InvertRight", "3"}, {"InvertLeft", "2"}}), s);
		CHECK(o.rx == 226 && o.ry == 6 && o.lx == 246 && o.ly == 20);
		// The middle stays the middle; the ends map onto the ends.
		State mid;
		o = Apply(From({{"InvertLeft", "3"}, {"InvertRight", "3"}}), mid);
		CHECK(o.lx == 128 && o.ly == 128 && o.rx == 128 && o.ry == 128);
		CHECK(Invert(0) == 255 && Invert(255) == 1 && Invert(1) == 255);
		// Swap and invert: the inversion is the PS2's right stick, the controller's left.
		o = Apply(From({{"SwapSticks", "1"}, {"InvertRight", "1"}}), s);
		CHECK(o.rx == 10 && o.ry == 236 && o.lx == 30 && o.ly == 250);
		CHECK(Describe(From({{"SwapSticks", "1"}, {"InvertRight", "1"}})) == "sticks swapped, right stick up-down inverted");
	}
	{
		// The left stick on the D-pad too: up-left pressed, the stick still moves.
		State s;
		s.lx = 20;
		s.ly = 30;
		Out o = Apply(From({{"LeftStickDpad", "1"}}), s);
		CHECK(Only(o, {T_LEFT, T_UP}) && o.lx == 20 && o.ly == 30);
		// Only the D-pad: the PS2's left stick stays in the middle.
		o = Apply(From({{"LeftStickDpad", "2"}}), s);
		CHECK(Only(o, {T_LEFT, T_UP}) && o.lx == 128 && o.ly == 128);
		// A small push presses nothing.
		s.lx = 128 - kStickDpad + 1;
		s.ly = 128 + kStickDpad - 1;
		CHECK(Only(Apply(From({{"LeftStickDpad", "2"}}), s), {}));
		s.lx = 255;
		s.ly = 255;
		CHECK(Only(Apply(From({{"LeftStickDpad", "2"}}), s), {T_RIGHT, T_DOWN}));
		// With the sticks swapped it is the controller's right stick that drives the D-pad.
		State r;
		r.rx = 0;
		CHECK(Only(Apply(From({{"LeftStickDpad", "1"}, {"SwapSticks", "true"}}), r), {T_LEFT}));
		CHECK(Only(Apply(From({{"LeftStickDpad", "1"}}), r), {}));
		// The D-pad from the stick and from a button add up.
		CHECK(Only(Apply(From({{"LeftStickDpad", "1"}, {"ButtonCross", "Right"}}), [] {
			State x;
			x.buttons = 0x4000;
			x.lx = 0;
			return x;
		}()),
			{T_LEFT, T_RIGHT}));
	}

	// vk-285-117: the save and load combos.
	{
		// The default: L3+R3 + D-pad up saves, + down loads, at once; the D-pad press that completed it is kept from the game.
		const Config c = From({});
		CHECK(c.save[0] == CB_L3R3 && c.save[1] == CB_UP && c.load[0] == CB_L3R3 && c.load[1] == CB_DOWN && c.hold_ms == 0);
		ComboWatch save, load;
		ComboState s;
		uint32_t block = 0;
		s.buttons = 0x6; // L3+R3
		CHECK(!save.Update(c.save, c.hold_ms, s, 0, block) && !load.Update(c.load, c.hold_ms, s, 0, block) && block == 0);
		s.buttons = 0x6 | 0x10; // + up
		block = 0;
		CHECK(save.Update(c.save, c.hold_ms, s, 4, block) && !load.Update(c.load, c.hold_ms, s, 4, block));
		CHECK(block == 0x10); // the up press, not L3+R3 (held before)
		block = 0;
		CHECK(!save.Update(c.save, c.hold_ms, s, 8, block) && block == 0x10); // held: once, still kept back
		s.buttons = 0x6; // up let go
		block = 0;
		CHECK(!save.Update(c.save, c.hold_ms, s, 12, block) && block == 0);
		s.buttons = 0x6 | 0x40; // down: load
		block = 0;
		CHECK(!save.Update(c.save, c.hold_ms, s, 16, block) && load.Update(c.load, c.hold_ms, s, 16, block) && block == 0x40);
		// Up alone, or L3 alone with up, does nothing.
		ComboWatch w;
		s.buttons = 0x10;
		block = 0;
		CHECK(!w.Update(c.save, c.hold_ms, s, 0, block));
		s.buttons = 0x2 | 0x10;
		CHECK(!w.Update(c.save, c.hold_ms, s, 4, block) && block == 0);
		CHECK(DescribeCombo(c.save, 0) == "L3+R3 + D-pad up");
		CHECK(Describe(c) == "as on the controller");
	}
	{
		// Two buttons held 1.5 s: not before, once, the game sees them all along, and again after a release.
		const Config c = From({{"SaveButton1", "Touchpad"}, {"SaveButton2", "R1"}, {"LoadButton1", "touchpad"}, {"LoadButton2", "L1"},
			{"StateHold", "1.5"}});
		CHECK(c.save[0] == CB_TOUCHPAD && c.save[1] == CB_R1 && c.load[1] == CB_L1 && c.hold_ms == 1500);
		ComboWatch save;
		ComboState s;
		uint32_t block = 0;
		s.buttons = 0x00100000 | 0x800;
		CHECK(!save.Update(c.save, c.hold_ms, s, 1000, block));
		CHECK(!save.Update(c.save, c.hold_ms, s, 2499, block));
		CHECK(save.Update(c.save, c.hold_ms, s, 2500, block));
		CHECK(!save.Update(c.save, c.hold_ms, s, 9000, block));
		CHECK(block == 0);
		s.buttons = 0x800; // the click let go: armed again, and the time starts over
		CHECK(!save.Update(c.save, c.hold_ms, s, 9004, block));
		s.buttons = 0x00100000 | 0x800;
		CHECK(!save.Update(c.save, c.hold_ms, s, 9008, block) && !save.Update(c.save, c.hold_ms, s, 10500, block));
		CHECK(save.Update(c.save, c.hold_ms, s, 10508, block));
		// The keyboard's Backspace counts as the touchpad's click.
		ComboWatch k;
		s.buttons = 0x40000000 | 0x800;
		CHECK(!k.Update(c.save, c.hold_ms, s, 0, block) && k.Update(c.save, c.hold_ms, s, 1500, block));
		// vk-285-122: the Create (Share) button, ScePad's 0x1, does not.
		ComboWatch cr;
		s.buttons = 0x1 | 0x800;
		CHECK(!cr.Update(c.save, c.hold_ms, s, 0, block) && !cr.Update(c.save, c.hold_ms, s, 5000, block));
		CHECK(DescribeCombo(c.save, c.hold_ms) == "the touchpad's click + R1 held 1.5 s");
		CHECK(Describe(c) == "save state on the touchpad's click + R1 held 1.5 s, load on the touchpad's click + L1 held 1.5 s");
	}
	{
		// One button (the same twice, or Nothing for one): Options held 2 s. Nothing twice: off.
		ComboState s;
		uint32_t block = 0;
		for (const char* second : {"Options", "None"})
		{
			const Config c = From({{"SaveButton1", "Options"}, {"SaveButton2", second}, {"StateHold", "2"}});
			ComboWatch w;
			s.buttons = 0x8;
			CHECK(!w.Update(c.save, c.hold_ms, s, 0, block) && w.Update(c.save, c.hold_ms, s, 2000, block));
			CHECK(DescribeCombo(c.save, c.hold_ms) == "Options held 2 s");
		}
		const Config off = From({{"LoadButton1", "None"}, {"LoadButton2", "None"}});
		ComboWatch w;
		s.buttons = 0xFFFFFFFFu;
		CHECK(!w.Update(off.load, off.hold_ms, s, 0, block) && !w.Update(off.load, off.hold_ms, s, 99999, block));
		CHECK(DescribeCombo(off.load, 0) == "nothing");
	}
	{
		// The touchpad's sides (1.51's touch + Cross), and triggers by their pull; a trigger kept back stays so until let go.
		const Config c = From({{"SaveButton1", "TouchLeft"}, {"SaveButton2", "Cross"}, {"LoadButton1", "L2"}, {"LoadButton2", "R2"}});
		ComboWatch save, load;
		ComboState s;
		uint32_t block = 0;
		s.touch = 2;
		s.buttons = 0x4000;
		CHECK(!save.Update(c.save, 0, s, 0, block));
		s.buttons = 0;
		save.Update(c.save, 0, s, 4, block);
		s.touch = 1;
		CHECK(!save.Update(c.save, 0, s, 8, block));
		s.buttons = 0x4000;
		block = 0;
		CHECK(save.Update(c.save, 0, s, 12, block) && block == 0x4000); // the Cross press is kept from the game
		ComboState t;
		t.l2 = 255;
		block = 0;
		CHECK(!load.Update(c.load, 0, t, 0, block));
		t.r2 = 210;
		CHECK(load.Update(c.load, 0, t, 4, block) && (block & 0x200u));
		t.r2 = 100; // half let go: still kept back
		block = 0;
		load.Update(c.load, 0, t, 8, block);
		CHECK(block & 0x200u);
		t.r2 = 0;
		block = 0;
		load.Update(c.load, 0, t, 12, block);
		CHECK(!(block & 0x200u));
	}
	{
		// Bad values leave the defaults.
		const Config c = From({{"SaveButton1", "Jump"}, {"LoadButton2", ""}, {"StateHold", "-1"}});
		CHECK(c.save[0] == CB_L3R3 && c.load[1] == CB_DOWN && c.hold_ms == 0);
		CHECK(From({{"StateHold", "11"}}).hold_ms == 0 && From({{"StateHold", "x"}}).hold_ms == 0 && From({{"StateHold", "0.5"}}).hold_ms == 500);
		for (int b = 0; b < CB_COUNT; b++)
		{
			ComboButton back = CB_NONE;
			CHECK(ParseComboButton(ComboButtonName(b), back) && back == b);
		}
	}

	// Every target's name reads back as itself, and only those names do.
	for (int t = 0; t <= T_NONE; t++)
	{
		Target back = T_NONE;
		CHECK(ParseTarget(TargetName(t), back) && back == t);
	}
	{
		Target x;
		CHECK(!ParseTarget("Crosss", x) && !ParseTarget("Cros", x) && !ParseTarget("", x));
	}

	// vk-285-118: a button's strength (SOCOM II's crouch: the touchpad click as Triangle at 0.20).
	{
		const Config c = From({{"ButtonTouchpad", "Triangle"}, {"ButtonTouchpadPressure", "0.20"}, {"ButtonCrossPressure", "30%"},
			{"ButtonL2Pressure", "0.5"}, {"ButtonCirclePressure", "2"}, {"ButtonSquarePressure", "0"}, {"ButtonR1Pressure", "x"}});
		CHECK(std::fabs(c.pressure[S_TOUCHPAD] - 0.2f) < 1e-6f && std::fabs(c.pressure[S_CROSS] - 0.3f) < 1e-6f);
		CHECK(c.pressure[S_CIRCLE] == 1.0f && c.pressure[S_SQUARE] == 1.0f && c.pressure[S_R1] == 1.0f);
		CHECK(!c.IsDefault() && !From({{"ButtonCrossPressure", "0.5"}}).IsDefault() && From({{"ButtonCrossPressure", "1"}}).IsDefault());
		const Out touch = Apply(c, Held(0x00100000u));
		CHECK(Only(touch, {T_TRIANGLE}) && std::fabs(touch.value[T_TRIANGLE] - 0.2f) < 1e-6f);
		// the real Triangle still presses fully; the two together press as the harder one
		const Out both = Apply(c, Held(0x00100000u | 0x1000u));
		CHECK(both.value[T_TRIANGLE] == 1.0f);
		State trig;
		trig.l2 = 255;
		CHECK(std::fabs(Apply(c, trig).value[T_L2] - 0.5f) < 1e-6f);
		CHECK(Describe(c) == "Cross presses Cross at 30%, L2 presses L2 at 50%, the touchpad's click presses Triangle at 20%");
		// on/off targets press as on at any strength
		const Config m = From({{"ButtonL3", "Pressure"}, {"ButtonL3Pressure", "0.2"}});
		CHECK(Apply(m, Held(0x0002)).value[T_PRESSURE] == 1.0f);
	}

	// vk-285-118: dead zones (round, then stretched; the right stick's own; before Swap sticks).
	{
		const Config c = From({{"DeadzoneLeft", "20"}, {"DeadzoneRight", "60"}});
		CHECK(c.deadzone_left == 20 && c.deadzone_right == 0 && !c.IsDefault());
		State st;
		st.lx = 128 + 20; // about 16%: inside
		st.ly = 128 - 10;
		st.rx = 140;
		Out o = Apply(c, st);
		CHECK(o.lx == 128 && o.ly == 128 && o.rx == 140);
		st.lx = 255; st.ly = 128; // all the way: still all the way
		o = Apply(c, st);
		CHECK(o.lx == 255 && o.ly == 128);
		st.lx = 0;
		CHECK(Apply(c, st).lx <= 1);
		st.lx = 128 + 76; // 60% out: (0.6 - 0.2) / 0.8 = half way
		o = Apply(c, st);
		CHECK(o.lx >= 190 && o.lx <= 193);
		const Config sw = From({{"DeadzoneLeft", "30"}, {"SwapSticks", "1"}});
		State drift;
		drift.lx = 150;
		drift.rx = 150;
		o = Apply(sw, drift);
		CHECK(o.rx == 128 && o.lx == 150);
		CHECK(Describe(From({{"DeadzoneLeft", "10"}})) == "left stick dead zone 10%");
	}

	// vk-285-118: the fast forward combo: off by default; set, it fires like the others.
	{
		CHECK(Config().fast[0] == CB_NONE && Config().fast[1] == CB_NONE);
		const Config c = From({{"FastButton1", "L3R3"}, {"FastButton2", "Right"}});
		CHECK(c.fast[0] == CB_L3R3 && c.fast[1] == CB_RIGHT && !c.IsDefault());
		CHECK(Describe(c) == "fast forward on L3+R3 + D-pad right");
		ComboWatch w;
		ComboState st;
		st.buttons = 0x6u | 0x20u;
		uint32_t block = 0;
		CHECK(w.Update(c.fast, c.hold_ms, st, 0, block) && !w.Update(c.fast, c.hold_ms, st, 10, block));
	}

	if (s_failures == 0)
		printf("test_pad_map: all checks passed\n");
	return s_failures == 0 ? 0 : 1;
}
