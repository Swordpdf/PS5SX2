// PS5 port frontend: the shelf's options sheet (see fe_options.h).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_options.h"
#include "fe_text.h" // vk-285-116: the buttons' symbols (icon::)

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace fe
{
using namespace settings;

namespace
{
OptionDef Seg(const char* key, const char* label, const char* def, const char* short_fmt, std::vector<OptionChoice> choices,
	const char* hint = "", bool restart = false)
{
	OptionDef d;
	d.key = key;
	d.label = label;
	d.def = def;
	d.short_fmt = short_fmt;
	d.choices = std::move(choices);
	d.hint = hint;
	d.restart = restart;
	return d;
}

OptionDef Toggle(const char* key, const char* label, const char* def, const char* short_fmt, const char* hint = "", bool restart = false)
{
	OptionDef d = Seg(key, label, def, short_fmt, {{"false", "Off"}, {"true", "On"}}, hint, restart);
	d.toggle = true;
	return d;
}

// vk-285-115: the crop rows' values (PS2 pixels) and their line, as the page's CROP_PX and CROP_HINT.
std::vector<OptionChoice> CropChoices()
{
	return {{"0", "Off"}, {"2", "2 px"}, {"4", "4 px"}, {"6", "6 px"}, {"8", "8 px"}, {"10", "10 px"}, {"12", "12 px"}, {"16", "16 px"},
		{"24", "24 px"}, {"32", "32 px"}};
}

constexpr const char* kCropHint = "Cuts this many PS2 pixels off this side of the picture, for games with garbage at a border "
								  "(Shadow of the Colossus). The picture keeps its proportions.";

// 2026-10-08 (AI-assisted): the Hardware fixes rows (PCSX2's manual hardware fixes), as the page's Hardware fixes group.
OptionDef Fix(OptionDef d)
{
	d.manual_fix = true;
	return d;
}

// Texture offsets in thousandths of a texel (GSRendererHW::SetTCOffset: value / -1000), as the page's TC_OFFSETS.
std::vector<OptionChoice> TexOffsetChoices()
{
	return {{"0", "Off"}, {"100", "100"}, {"200", "200"}, {"250", "250"}, {"300", "300"}, {"400", "400"}, {"500", "500"}, {"525", "525"},
		{"550", "550"}, {"600", "600"}, {"700", "700"}, {"750", "750"}, {"800", "800"}, {"900", "900"}, {"1000", "1000"}};
}

constexpr const char* kTexOffsetHint = "Moves where textures are read from, in thousandths of a texel (500 is half a texel): for lines "
									   "or stray pixels at the edges of textures when upscaled. A tester's Kingdom Hearts fix: X 525, Y 0.";

std::vector<OptionDef> HardwareFixesGroup()
{
	return {
		Toggle(kManualFixesKey, "Manual hardware fixes", "false", "Manual fixes %",
			"PCSX2's fixes for upscaling glitches in this game. The rows below count while this is on; changing one turns it on, "
			"starting from PCSX2's own fixes for the game."),
		Fix(Seg("UserHacks_HalfPixelOffset", "Half-pixel offset", "0", "Half-pixel %",
			{{"0", "Off"}, {"1", "Normal (vertex)"}, {"2", "Special (texture)"}, {"3", "Special (aggressive)"}, {"4", "Align to native"},
				{"5", "Native + texture offset"}},
			"Moves the picture by half a pixel to line up effects (bloom, blur, shadows) that sit off when upscaled.")),
		Fix(Seg("UserHacks_native_scaling", "Native scaling", "0", "Native scaling %",
			{{"0", "Off"}, {"1", "Normal"}, {"2", "Aggressive"}, {"3", "Normal (keep upscale)"}, {"4", "Aggressive (keep upscale)"}},
			"Draws post-processing effects at the PS2's own size, then scales them up: for effects that break when upscaled (depth "
			"of field, glow). Keep upscale stays sharper.")),
		Fix(Seg("UserHacks_round_sprite_offset", "Round sprite", "0", "Round sprite %", {{"0", "Off"}, {"1", "Half"}, {"2", "Full"}},
			"Rounds 2D sprites' texture positions: for lines and blur in 2D pictures and text when upscaled.")),
		Fix(Toggle("UserHacks_align_sprite_X", "Align sprite", "false", "Align sprite %",
			"Lines up sprites that leave vertical lines when upscaled.")),
		Fix(Toggle("UserHacks_merge_pp_sprite", "Merge sprite", "false", "Merge sprite %",
			"Draws an effect made of many sprites as one: for lines through effects when upscaled.")),
		Fix(Seg("UserHacks_BilinearHack", "Bilinear upscale", "0", "Bilinear %", {{"0", "Auto"}, {"1", "Bilinear"}, {"2", "Nearest"}},
			"How textures the PS2 smooths are smoothed when upscaled. Auto: PCSX2 chooses.")),
		Fix(Seg("UserHacks_TCOffsetX", "Texture offset X", "0", "Texture offset X %", TexOffsetChoices(), kTexOffsetHint)),
		Fix(Seg("UserHacks_TCOffsetY", "Texture offset Y", "0", "Texture offset Y %", TexOffsetChoices(), kTexOffsetHint)),
		Fix(Seg("UserHacks_AutoFlushLevel", "Auto flush", "0", "Auto flush %", {{"0", "Off"}, {"1", "Sprites"}, {"2", "All"}},
			"Ends a draw when it reads the picture it is drawing on, as the PS2 would: for some missing or broken effects. Costs "
			"speed. Sprites: for sprites only.")),
		Fix(Seg("UserHacks_TextureInsideRt", "Texture inside target", "0", "Texture in target %", {{"0", "Off"}, {"1", "Inside"}, {"2", "Merge"}},
			"Lets a texture be read from inside a bigger picture the game drew earlier: for some missing or wrong effects. Merge also "
			"joins pictures that touch.")),
	};
}

// vk-285-116 (AI-assisted): the Controls tab. A button's symbol and its name ("<Cross>  Cross"), as the sheet shows them
// (icon::Blank keeps the symbol's room with nothing drawn, so the names line up).
std::string Sym(const char* glyph, const char* name)
{
	return std::string(glyph) + "  " + name;
}

// What a controller button can press (orbis-shims/OrbisPadMap.h's names), as the page's BUTTON_TARGETS: the face buttons
// and the D-pad with their symbols.
std::vector<OptionChoice> ButtonChoices()
{
	return {{"Cross", Sym(icon::Cross, "Cross")}, {"Circle", Sym(icon::Circle, "Circle")}, {"Square", Sym(icon::Square, "Square")},
		{"Triangle", Sym(icon::Triangle, "Triangle")}, {"L1", "L1"}, {"R1", "R1"}, {"L2", "L2"}, {"R2", "R2"}, {"L3", "L3"}, {"R3", "R3"},
		{"Start", "Start"}, {"Select", "Select"}, {"Up", Sym(icon::DpadUp, "D-pad up")}, {"Down", Sym(icon::DpadDown, "D-pad down")},
		{"Left", Sym(icon::DpadLeft, "D-pad left")}, {"Right", Sym(icon::DpadRight, "D-pad right")}, {"Analog", "Analog button"},
		{"Pressure", "Light press"}, {"None", "Nothing"}};
}

// The remapping rows, in the page's order: the setting, the button's symbol and name, what it presses by default.
std::vector<OptionDef> ButtonRows()
{
	static const struct
	{
		const char* key;
		const char* glyph;
		const char* name;
		const char* def;
	} rows[] = {
		{"PS5SX2/ButtonCross", icon::Cross, "Cross", "Cross"},
		{"PS5SX2/ButtonCircle", icon::Circle, "Circle", "Circle"},
		{"PS5SX2/ButtonSquare", icon::Square, "Square", "Square"},
		{"PS5SX2/ButtonTriangle", icon::Triangle, "Triangle", "Triangle"},
		{"PS5SX2/ButtonL1", icon::Blank, "L1", "L1"},
		{"PS5SX2/ButtonR1", icon::Blank, "R1", "R1"},
		{"PS5SX2/ButtonL2", icon::Blank, "L2", "L2"},
		{"PS5SX2/ButtonR2", icon::Blank, "R2", "R2"},
		{"PS5SX2/ButtonL3", icon::Blank, "L3", "L3"},
		{"PS5SX2/ButtonR3", icon::Blank, "R3", "R3"},
		{"PS5SX2/ButtonOptions", icon::Blank, "Options", "Start"},
		{"PS5SX2/ButtonTouchpad", icon::Blank, "Touchpad click", "Select"},
		{"PS5SX2/ButtonUp", icon::DpadUp, "D-pad up", "Up"},
		{"PS5SX2/ButtonDown", icon::DpadDown, "D-pad down", "Down"},
		{"PS5SX2/ButtonLeft", icon::DpadLeft, "D-pad left", "Left"},
		{"PS5SX2/ButtonRight", icon::DpadRight, "D-pad right", "Right"},
	};
	std::vector<OptionDef> out;
	for (const auto& r : rows)
	{
		OptionDef d;
		d.key = r.key;
		d.label = Sym(r.glyph, r.name);
		d.def = r.def;
		d.choices = ButtonChoices();
		d.hint = std::string("What the controller's ") + r.name +
		         " presses in games. Several can press the same one; Nothing turns it off. Light press: while it's held, buttons "
		         "press at half strength (Metal Gear Solid 2 and 3). PS5SX2's combos use the real buttons.";
		out.push_back(std::move(d));
	}
	return out;
}

// vk-285-118: how hard each button presses (OrbisPadMap.h Config::pressure, PS5SX2/Button<Name>Pressure), as the page's
// STRENGTHS and its Button strength group.
std::vector<OptionChoice> StrengthChoices()
{
	return {{"1", "Full"}, {"0.75", "75%"}, {"0.5", "50%"}, {"0.4", "40%"}, {"0.3", "30%"}, {"0.25", "25%"}, {"0.2", "20%"},
		{"0.15", "15%"}, {"0.1", "10%"}};
}

std::vector<OptionDef> StrengthGroup()
{
	static const struct
	{
		const char* key;
		const char* glyph;
		const char* name;
	} rows[] = {
		{"PS5SX2/ButtonCrossPressure", icon::Cross, "Cross"},
		{"PS5SX2/ButtonCirclePressure", icon::Circle, "Circle"},
		{"PS5SX2/ButtonSquarePressure", icon::Square, "Square"},
		{"PS5SX2/ButtonTrianglePressure", icon::Triangle, "Triangle"},
		{"PS5SX2/ButtonL1Pressure", icon::Blank, "L1"},
		{"PS5SX2/ButtonR1Pressure", icon::Blank, "R1"},
		{"PS5SX2/ButtonL2Pressure", icon::Blank, "L2"},
		{"PS5SX2/ButtonR2Pressure", icon::Blank, "R2"},
		{"PS5SX2/ButtonTouchpadPressure", icon::Blank, "Touchpad click"},
		{"PS5SX2/ButtonUpPressure", icon::DpadUp, "D-pad up"},
		{"PS5SX2/ButtonDownPressure", icon::DpadDown, "D-pad down"},
		{"PS5SX2/ButtonLeftPressure", icon::DpadLeft, "D-pad left"},
		{"PS5SX2/ButtonRightPressure", icon::DpadRight, "D-pad right"},
	};
	std::vector<OptionDef> out;
	for (const auto& r : rows)
		out.push_back(Seg(r.key, Sym(r.glyph, r.name).c_str(), "1", "", StrengthChoices(),
			"How hard this button presses what it's set to press. The PS2's face buttons, D-pad and shoulders read pressure, and some "
			"games act on a light press: SOCOM II crouches at 20% on Triangle, Combined Assault at 30% (a full press goes prone). Set "
			"the touchpad click to Triangle at 20% for a crouch button. L2 and R2 press at most this hard."));
	return out;
}

// vk-285-118: the sticks' dead zones (OrbisPadMap.h Config::deadzone_left/right), as the page's DEADZONES.
std::vector<OptionChoice> DeadzoneChoices()
{
	return {{"0", "Off"}, {"5", "5%"}, {"10", "10%"}, {"15", "15%"}, {"20", "20%"}, {"25", "25%"}, {"30", "30%"}};
}

constexpr const char* kDeadzoneHint = "Inside this much of the way out, the stick reads as centred; past it, the rest of the way still reaches full. For a stick that drifts, or a game that walks or turns by itself. Off is the stick as it is.";

std::vector<OptionChoice> InvertChoices()
{
	return {{"0", "Off"}, {"1", "Up-down"}, {"2", "Left-right"}, {"3", "Both"}};
}

// vk-285-117: what a save or load button can be (orbis-shims/OrbisPadMap.h ComboButtonName), as the page's COMBO_BUTTONS.
std::vector<OptionChoice> ComboChoices()
{
	return {{"L3R3", "L3 + R3"}, {"Cross", Sym(icon::Cross, "Cross")}, {"Circle", Sym(icon::Circle, "Circle")},
		{"Square", Sym(icon::Square, "Square")}, {"Triangle", Sym(icon::Triangle, "Triangle")}, {"L1", "L1"}, {"R1", "R1"}, {"L2", "L2"},
		{"R2", "R2"}, {"L3", "L3"}, {"R3", "R3"}, {"Options", "Options"}, {"Touchpad", "Touchpad click"}, {"TouchLeft", "Touchpad left"},
		{"TouchRight", "Touchpad right"}, {"Up", Sym(icon::DpadUp, "D-pad up")}, {"Down", Sym(icon::DpadDown, "D-pad down")},
		{"Left", Sym(icon::DpadLeft, "D-pad left")}, {"Right", Sym(icon::DpadRight, "D-pad right")}, {"None", "Nothing"}};
}

// The Buttons group: the save and load combos and their hold time first, then a row for each button.
std::vector<OptionDef> ButtonsGroup()
{
	constexpr const char* save_hint = "Hold both save buttons together to save the state (slot 1). L3 + R3: both sticks pressed in. Touchpad "
									  "left or right: a finger on that side. Nothing, or the same button twice, makes it one button. F1 on a "
									  "keyboard saves too.";
	constexpr const char* load_hint = "Hold both load buttons together to load the state (slot 1). L3 + R3: both sticks pressed in. Touchpad "
									  "left or right: a finger on that side. Nothing, or the same button twice, makes it one button. F3 on a "
									  "keyboard loads too.";
	constexpr const char* fast_hint = "Experimental, for skipping videos: hold both fast forward buttons together (for the hold time above) to run the game as fast as it goes, and again to go back to full speed. Nothing on both: no fast forward button. Sound may skip while it's on.";
	constexpr const char* disc_hint = "Hold both change disc buttons together to put in the game's next disc: images named \"(Disc 1)\", \"(Disc 2)\"... in the same folder, or listed in an .m3u file, are one game's discs. Any image (a GameShark disc's game, say) goes in from the settings page's Disc section while you play. Nothing on both: no change disc button.";
	std::vector<OptionDef> out = {
		Seg("PS5SX2/SaveButton1", "Save: button 1", "L3R3", "", ComboChoices(), save_hint),
		Seg("PS5SX2/SaveButton2", "Save: button 2", "Up", "", ComboChoices(), save_hint),
		Seg("PS5SX2/LoadButton1", "Load: button 1", "L3R3", "", ComboChoices(), load_hint),
		Seg("PS5SX2/LoadButton2", "Load: button 2", "Down", "", ComboChoices(), load_hint),
		Seg("PS5SX2/StateHold", "Hold time", "0", "", {{"0", "Instant"}, {"0.5", "0.5 s"}, {"1", "1 s"}, {"1.5", "1.5 s"}, {"2", "2 s"}, {"3", "3 s"}},
			"How long the save or load buttons are held before it happens. Instant: as soon as both are down, and that last press doesn't "
			"reach the game. A second or two stops saving or loading by accident."),
		// vk-285-118: fast forward (experimental), after the same hold time.
		Seg("PS5SX2/FastButton1", "Fast forward: button 1", "None", "", ComboChoices(), fast_hint),
		Seg("PS5SX2/FastButton2", "Fast forward: button 2", "None", "", ComboChoices(), fast_hint),
		Seg("PS5SX2/FastSpeed", "Fast forward speed", "0", "", {{"0", "Max"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}}, "How fast fast forward runs. Max: as fast as the console can. Some games' videos skip only at Max."),
		// vk-285-139: change disc, for games on more than one disc.
		Seg("PS5SX2/DiscButton1", "Change disc: button 1", "L3R3", "", ComboChoices(), disc_hint),
		Seg("PS5SX2/DiscButton2", "Change disc: button 2", "Right", "", ComboChoices(), disc_hint),
	};
	for (OptionDef& d : ButtonRows())
		out.push_back(std::move(d));
	return out;
}

bool Truthy(const std::string& v)
{
	return v == "true" || v == "1";
}

bool SameNumber(const std::string& a, const std::string& b)
{
	if (a.empty() || b.empty())
		return false;
	char* ea = nullptr;
	char* eb = nullptr;
	const double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
	return ea && *ea == '\0' && eb && *eb == '\0' && x == y;
}

const std::string* Find(const IniState& st, const std::string& key)
{
	for (const auto& p : st.kv)
		if (p.first == key)
			return &p.second;
	return nullptr;
}

const char* kSlotFile[3] = {"", "MemoryCards/Slot1_Filename", "MemoryCards/Slot2_Filename"};
const char* kSlotOn[3] = {"", "MemoryCards/Slot1_Enable", "MemoryCards/Slot2_Enable"};
const char* kSlotDefault[3] = {"", "Mcd001.ps2", "Mcd002.ps2"};

std::string CardSize(uint64_t bytes)
{
	if (bytes == kPs1CardBytes)
		return "PS1";
	for (uint64_t mb : {8, 16, 32, 64})
		if (bytes == mb * kCardMb || bytes == mb * 1048576ull)
			return std::to_string(mb) + " MB";
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / 1048576.0);
	return buf;
}
} // namespace

// The settings page's GROUPS (assets/web/index.html), in its order, with its labels, defaults and hints. The text field
// (PS5SX2/TexturesDir) stays on the page: the console has no keyboard for a path.
std::vector<std::string> SplitFolderList(const std::string& list)
{
	std::vector<std::string> items;
	size_t at = 0;
	while (at <= list.size())
	{
		size_t end = list.find_first_of(";|\n", at);
		if (end == std::string::npos)
			end = list.size();
		std::string item = list.substr(at, end - at);
		at = end + 1;
		while (!item.empty() && (item.back() == ' ' || item.back() == '\t' || item.back() == '\r' || (item.back() == '/' && item.size() > 1)))
			item.pop_back();
		while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
			item.erase(0, 1);
		if (!item.empty() && std::find(items.begin(), items.end(), item) == items.end())
			items.push_back(item);
	}
	return items;
}

std::string JoinFolderList(const std::vector<std::string>& items)
{
	std::string list;
	for (const std::string& i : items)
		list += (list.empty() ? "" : ";") + i;
	return list;
}

const std::vector<OptionGroup>& OptionGroups()
{
	static const std::vector<OptionGroup> groups = {
		// vk-285-137 (AI-assisted; swordpdf: "add an option to hide games from the shelf"): a game off the shelf (its own file
		// only), and the switch for all games that puts hidden games back on it, dimmed, so their sheets can turn it off again.
		// First, so neither is far down the sheet (the Folders rows come before them on the sheet for all games).
		{"Shelf",
			{
				Toggle("PS5SX2/HideGame", "Hide from the shelf", "false", "Hidden %",
					"Takes this game off the shelf when the sheet closes. Show hidden games, on the sheet for all games (R1), puts hidden games back on the shelf, dimmed, to turn this off again."),
			},
			kTabSettings, true},
		{"Shelf",
			{
				Toggle("PS5SX2/ShowHiddenGames", "Show hidden games", "false", "Hidden games %",
					"Puts the games hidden from the shelf back on it, dimmed, so you can open one's sheet and turn Hide from the shelf off."),
			},
			kTabSettings, false, true},
		// vk-285-139 (AI-assisted; swordpdf: a GameShark disc for all games, "it would have to be disc 1 at all times"): games
		// start from the cheat disc (picked on the sheet for all games), the game's own discs after it: the change disc combo
		// or the settings page puts the game's disc in when the cheat disc asks for it.
		{"Discs",
			{
				Toggle("PS5SX2/CheatDiscStart", "Start from the cheat disc", "false", "Cheat disc %",
					"Starts the game from the cheat disc (a GameShark, say: Cheat disc, on the sheet for all games), as disc 1, with the game's own discs after it. When it asks for the game's disc, hold the change disc buttons (Controls; L3 + R3 + D-pad right by default) or pick the disc on the settings page. Its codes stay on the memory card.", true),
			},
			kTabSettings},
		{"Display",
			{
				Seg("upscale_multiplier", "Resolution", "1", "%", {{"1", "1x"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {"5", "5x"}, {"6", "6x"},
										   {"8", "8x (PS5 Pro)"}},
					"Internal resolution. 6x is about 4K; 8x draws more than the screen shows (about 5K, made smaller to fit): sharpest, and the heaviest on the GPU."),
				Seg("AspectRatio", "Aspect ratio", "Auto 4:3/3:2", "Aspect %",
					{{"Auto 4:3/3:2", "Auto"}, {"4:3", "4:3"}, {"16:9", "16:9"}, {"Stretch", "Stretch"}}),
				Toggle("EmuCore/EnableWideScreenPatches", "Widescreen patches", "false", "Widescreen %",
					"Uses the game's 16:9 patch when there is one."),
				Seg("TVShader", "Display filter", "0", "%", {{"6", "FSR"}, {"7", "FSR soft"}, {"0", "Classic"}, {"5", "CRT"}}),
				// 2026-10-08 (AI-assisted): frame generation (GSDeviceVK.cpp), read when the game starts (main-boot.cpp).
				Toggle("PS5SX2/FrameGeneration", "Frame generation", "false", "Frame gen %",
					"Shows a frame made between two of the game's: a 60 fps game at 120 on a TV that takes 120 Hz (the TV switches to "
					"120 Hz for the game), a 30 fps game at 60. Only while the game holds its frame rate (an uneven one shows the game's own "
					"frames); the FPS box then counts what the TV gets. A little more input lag, and small smears at the edges of fast "
					"movement. Experimental. Takes effect when the game starts.",
					true),
			}},
		{"Graphics",
			{
				Seg("PS5SX2/Renderer", "Renderer", "Hardware", "%", {{"Hardware", "Hardware"}, {"Software", "Software"}}, "Software draws the game on the CPU, as a PS2 does it: slower, but right in games whose effects the hardware renderer gets wrong. Set it for one game in that game's settings. Takes effect when the game starts.", true), // vk-285-118
				Seg("filter", "Texture filtering", "2", "% filtering", {{"0", "Nearest"}, {"2", "PS2"}, {"1", "Bilinear"}, {"3", "Not sprites"}}),
				Seg("MaxAnisotropy", "Anisotropic filtering", "0", "AF %", {{"0", "Off"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}}),
				Seg("accurate_blending_unit", "Blending accuracy", "1", "Blending %", {{"0", "Min"}, {"1", "Basic"}, {"2", "Med"}, {"3", "High"}},
					"Higher fixes more effects and costs speed. Full and Max are left out for now: Max hung the GPU in Ratchet & Clank."),
				Toggle("hw_mipmap", "Mipmapping", "true", "Mipmaps %"),
				Toggle("LoadTextureReplacements", "Texture replacements", "false", "Replacements %",
					"Loads a texture pack's PNG and DDS files from <game serial>/replacements/ in a USB drive's PS5SX2/textures (or textures) "
					"folder, else in /data/PCSX2/textures. Another folder can be named on the settings page (Textures folder)."),
			}},
		{"Performance",
			{
				Seg("EmuCore/Speedhacks/EECycleRate", "EE cycle rate", "0", "EE %",
					{{"-3", "50%"}, {"-2", "60%"}, {"-1", "75%"}, {"0", "100%"}, {"1", "130%"}, {"2", "180%"}, {"3", "300%"}},
					"Above 100% can smooth out slowdown the PS2 had; some games dislike it."),
				Seg("EmuCore/Speedhacks/EECycleSkip", "EE cycle skip", "0", "Cycle skip %", {{"0", "Off"}, {"1", "Mild"}, {"2", "Moderate"}, {"3", "Max"}}),
				Toggle("EmuCore/Speedhacks/vuThread", "MTVU (VU1 on its own thread)", "false", "MTVU %", "", true),
				Seg("HWDownloadMode", "GPU readbacks", "0", "Readbacks %", {{"0", "Accurate"}, {"1", "Whole area"}, {"3", "Don't wait"}, {"4", "Skip"}},
					"Some games read the picture back from the GPU, which is slow on the PS5. Accurate is PCSX2's default. Whole area reads "
					"everything drawn since the last read in one go, so there are fewer stops. Don't wait doesn't stop the game for the GPU: "
					"quicker, but effects that depend on it can flicker. Skip ignores the reads: fastest, and those effects break. A game that "
					"slows down because of them (on firmware below 10 each read can wait a vblank; elsewhere some games read so often that "
					"the waits add up to a third of the time) switches to Don't wait by itself unless this is set."),
			}},
		{"Game",
			{
				Seg("PS5SX2/GameLanguage", "Game language", "-1", "Language %",
					{{"-1", "Auto (PS5)"}, {"0", "Japanese"}, {"1", "English"}, {"2", "French"}, {"3", "Spanish"}, {"4", "German"}, {"5", "Italian"},
						{"6", "Dutch"}, {"7", "Portuguese"}},
					"The language the PS2 tells games. PAL games with several languages start in it; most US games are English only. Auto "
					"follows the PS5's language.",
					true),
				// 2026-10-05 (AI-assisted): the PS2's network adapter, on for every game by default (main-boot.cpp).
				Toggle("DEV9/Eth/EthEnable", "Network adapter", "true", "Network %",
					"The PS2's network adapter, on the PS5's own connection, for online and LAN play (on revival servers such as PS "
					"Rewired; their DNS goes in the game's network settings). It costs nothing until a game uses it. Turn it off for a "
					"game that misbehaves with it. Takes effect when the game starts.",
					true),
				// 2026-10-08 (AI-assisted; testers: "Enable Host Filesystem"): PCSX2's host: device (EmuCore/HostFs).
				Toggle("EmuCore/HostFs", "Host filesystem", "false", "Host files %",
					"Lets homebrew read and write files in its own folder (host:), as PCSX2's Enable Host Filesystem: the ELF's folder, "
					"or the disc image's. Only for homebrew that asks for it.",
					true),
				// 2026-10-08 (AI-assisted; testers asked for the full boot): PCSX2's fast boot (EmuCore/EnableFastBoot).
				Toggle("EmuCore/EnableFastBoot", "Fast boot", "true", "Fast boot %",
					"Off: the PS2's own start-up first, the towers and the PlayStation 2 logo, as on a PS2 (a few games want it). On "
					"goes straight to the game.",
					true),
			}},
		{"On screen",
			{
				Seg("PS5SX2/Overlay", "Info box", "2", "Info box %", {{"0", "Off"}, {"1", "FPS"}, {"2", "FPS + load"}},
					"The box in the top right corner. Load is how busy the EE, GS and VU threads are."),
				Toggle("PS5SX2/FpsGraph", "FPS graph", "false", "FPS graph %",
					"A blue graph of the last minute's frame rate in the top right corner. It shows with the info box off too."),
				// 2026-10-08 (AI-assisted): GSRenderer.cpp OrbisDrawBezel; the folder is on the settings page (no keyboard here).
				Toggle("PS5SX2/Bezel", "Overlay picture", "false", "Overlay %",
					"A bezel or frame drawn over the screen around the game: overlays/<game serial>.png, else overlays/default.png, in "
					"/data/PCSX2 (or the folder named on the settings page): a 1920x1080 PNG with a see-through middle."),
				// 2026-10-08 (AI-assisted): GSRenderer.cpp OrbisDrawChallengeIcons.
				Toggle("PS5SX2/RAChallengeIcons", "Challenge icons", "true", "Challenge icons %",
					"RetroAchievements: the badge of each challenge going on (an achievement that unlocks if you keep it up, such as "
					"no damage taken) in the bottom right corner, while it lasts."),
			}},
		// 2026-10-08 (AI-assisted; a tester: "Kingdom Hearts needs HW hacks, texture offset X 525, Y 0"): PCSX2's manual hardware
		// fixes, on a game's sheet only. While they are on, PCSX2 leaves out the game database's own fixes for the game unless
		// the file sets the same value (GameDatabase.cpp applyGSHardwareFixes), so turning them on here writes those first
		// (settings::ManualFixSettings). They take effect in a running game (GSRendererHW::UpdateSettings). Needs proper
		// testing on the console.
		{"Hardware fixes", HardwareFixesGroup(), kTabSettings, true},
		// vk-285-115: PCSX2's crop (EmuCore/GS/CropLeft..CropBottom, PS2 pixels), as on the page. 2026-10-08: the last group of the
		// Settings tab, as testers asked (it is set once for a game and then left).
		{"Crop",
			{
				Seg("CropLeft", "Crop left", "0", "Crop left %", CropChoices(), kCropHint),
				Seg("CropTop", "Crop top", "0", "Crop top %", CropChoices(), kCropHint),
				Seg("CropRight", "Crop right", "0", "Crop right %", CropChoices(), kCropHint),
				Seg("CropBottom", "Crop bottom", "0", "Crop bottom %", CropChoices(), kCropHint),
			}},
		// vk-285-116 (AI-assisted): the Controls tab (R2 on the sheet, as the page's Controls tab). The remapping, (vk-285-117) the
		// save and load combos: main-boot.cpp orbis_ps5opts_from, orbis-shims/OrbisPadMap.h.
		{"Buttons", ButtonsGroup(), kTabControls},
		{"Button strength", StrengthGroup(), kTabControls}, // vk-285-118
		{"Sticks",
			{
				Toggle("PS5SX2/SwapSticks", "Swap sticks", "false", "", "The left stick moves the game's right stick, and the right stick its left one."),
				Seg("PS5SX2/InvertLeft", "Invert left stick", "0", "", InvertChoices(),
					"Turns the game's left stick up-down, left-right or both the other way round (the stick the game sees, after Swap sticks)."),
				Seg("PS5SX2/InvertRight", "Invert right stick", "0", "", InvertChoices(),
					"Turns the game's right stick up-down, left-right or both the other way round: the camera in most games (the stick the "
					"game sees, after Swap sticks)."),
				Seg("PS5SX2/LeftStickDpad", "Left stick as D-pad", "0", "", {{"0", "Off"}, {"1", "Also"}, {"2", "Only"}},
					"For games that only read the D-pad. Also: the left stick presses the D-pad as it moves. Only: it presses just the D-pad, "
					"and the game's left stick stays still."),
				Seg("PS5SX2/DeadzoneLeft", "Left stick dead zone", "0", "", DeadzoneChoices(), kDeadzoneHint),
				Seg("PS5SX2/DeadzoneRight", "Right stick dead zone", "0", "", DeadzoneChoices(), kDeadzoneHint),
			},
			kTabControls},
		{"Controller",
			{
				Toggle("PS5SX2/Rumble", "Rumble", "true", "Rumble %",
					"The game's vibration on the controller. In a game, hold L2 and D-pad down for 2 seconds to open the settings page in the "
					"PS5's own web browser; the game keeps running behind it."),
				Seg("PS5SX2/RumbleStrength", "Rumble strength", "1", "",
					{{"0.25", "25%"}, {"0.5", "50%"}, {"0.75", "75%"}, {"1", "100%"}, {"1.25", "125%"}, {"1.5", "150%"}, {"2", "200%"}},
					"How strongly the controller vibrates, as a share of what the game asks for. Above 100% lifts weak rumble; it never goes past the motors' full strength."),
				Seg("PS5SX2/Multitap", "Multitap", "0", "", {{"0", "Off"}, {"1", "Port 1"}, {"2", "Port 2"}}, "For 4-player games. Players 2 to 4 are the other PS5 users logged in when the game starts, each on their own controller. Port 1 suits most games; a few want the multitap in port 2. Off: player 2 on port 2, as before.", true),
				// vk-285-141 (AI-assisted): main-boot.cpp's microphone block (orbis-shims/ProsperoAudio.cpp captures it).
				Seg("PS5SX2/Microphone", "Microphone", "auto", "", {{"auto", "Auto"}, {"0", "Off"}, {"1", "Mic"}, {"2", "Headset"}},
					"The controller's microphone as a PS2 USB microphone, for games that need one (Lifeline - Voice Action Adventure). Auto turns it on for those games; Headset suits a game that asks for a USB headset. Takes effect when the game starts.", true),
			},
			kTabControls},
		{"Keyboard and mouse",
			{
				Seg("PS5SX2/KeyboardMouse", "Keyboard and mouse", "0", "Keyboard and mouse %",
					{{"0", "Auto"}, {"1", "Controller"}, {"2", "USB devices"}, {"3", "Off"}},
					"Controller: the keys and the mouse press the PS2 controller's buttons, as in PCSX2 on a PC (arrows are the D-pad, W A S D "
					"the left stick, Enter Start; F1 saves and F3 loads a state; hold Esc to go back to the menu). USB devices: the PS2 sees a "
					"USB keyboard and mouse, for games made for them. Auto: the controller, until a game reads the PS2's USB keyboard or mouse."),
				Seg("PS5SX2/MouseAim", "Mouse movement", "1", "Mouse movement %", {{"1", "Right stick"}, {"2", "Left stick"}, {"0", "Nothing"}},
					"Which stick the mouse moves in the controller mode. The right stick turns the camera in most games."),
				Seg("PS5SX2/MouseSpeed", "Mouse speed", "2", "Mouse speed %", {{"1", "Slow"}, {"2", "Normal"}, {"3", "Fast"}, {"4", "Very fast"}},
					"How far a mouse movement pushes the stick. Faster reaches a full stick with less movement."),
				Seg("PS5SX2/MouseButtons", "Mouse buttons", "0", "Mouse buttons %", {{"0", "R1 and L1"}, {"1", "R2 and L2"}},
					"Left click and right click. Most shooters fire with R1 or R2 and aim with L1 or L2. The wheel click is R3."),
			},
			kTabControls},
	};
	return groups;
}

void OptionsSheet::Open(const OptionsPaths& paths, const GameInfo* game)
{
	m_paths = paths;
	m_global = game == nullptr;
	m_armed_row = -1;
	m_status.clear();
	m_saved = 0;
	if (game)
	{
		m_title = game->title;
		m_serial = game->serial;
		m_id = game->serial;
		m_path = paths.settings_dir + "/" + game->stem + ".ini";
		m_file_label = "settings/" + game->stem + ".ini";
		m_header = "# " + game->title + (game->serial.empty() ? std::string() : " (" + game->serial + ")");
		m_game_fixes = ManualFixSettings(game->serial); // 2026-10-08
		m_elf = IsElfName(game->file.c_str());          // 2026-10-08
	}
	else
	{
		m_game_fixes.clear();
		m_elf = false;
		m_title = "All games";
		m_serial.clear();
		m_id = "@global";
		m_path = paths.gs_ini;
		m_file_label = "gs.ini";
		m_header = "# All games";
	}
	Reload();
}

void OptionsSheet::Reload()
{
	std::string text;
	ReadFile(m_path, text);
	m_own = ReadState(text);
	m_globals = IniState();
	if (!m_global)
	{
		std::string g;
		ReadFile(m_paths.gs_ini, g);
		m_globals = ReadState(g);
	}
	m_has_preset = PresetSection(m_paths.presets, m_id, m_preset);
	m_patches = m_global ? std::vector<PatchGroup>() : PatchGroups(m_paths.patches_dir, m_serial);
	m_cheats = (m_global || m_paths.cheats_dir.empty()) ? std::vector<PatchGroup>() : PatchGroups(m_paths.cheats_dir, m_serial); // 2026-10-08
	m_cards = ListCards(m_paths.memcards_dir);
	BuildRows();
}

void OptionsSheet::SetTab(int tab)
{
	tab = tab < 0 ? 0 : tab >= kTabCount ? kTabCount - 1 : tab;
	if (tab == m_tab)
		return;
	m_tab = tab;
	m_armed_row = -1;
	BuildRows();
}

// vk-285-116: the rows of the tab shown. The settings: Recommended, the option groups, the memory cards and the patches;
// the controls: their groups. Each ends with its own reset row.
void OptionsSheet::BuildRows()
{
	m_rows.clear();
	auto add = [&](Kind k, const std::string& label) -> Row& {
		Row r;
		r.kind = k;
		r.label = label;
		m_rows.push_back(r);
		return m_rows.back();
	};
	if (m_tab == kTabAchievements)
		return; // Read-only browser is rendered by App (AI-assisted).
	const bool settings = m_tab == kTabSettings;
	if (settings && (m_has_preset || !m_global))
		add(Kind::Recommended, m_global ? "Recommended for all games" : "Recommended settings");
	// vk-285-135 (AI-assisted; swordpdf: "i also want to pick a folder though the browser in the shelf"): where the games and the
	// BIOS are, on the sheet for all games (the app runs the picker and the share list). vk-285-135b: at the top (swordpdf
	// looked for them: at the bottom, under the memory cards, they were too far down).
	if (settings && m_global && m_folder_rows)
	{
		add(Kind::Header, "Folders");
		add(Kind::GameFolders, "Game folders");
		add(Kind::BiosFolder, "BIOS folder");
		add(Kind::NfsShares, "NFS shares");
	}
	// 2026-10-08 (AI-assisted; testers: "ELF properties: disc path"): an ELF's disc, first, as PCSX2's ELF properties have it.
	if (settings && m_elf)
	{
		add(Kind::Header, "ELF");
		add(Kind::ElfDisc, "Disc image");
	}
	for (const OptionGroup& g : OptionGroups())
	{
		if (g.tab != m_tab || (g.game_only && m_global) || (g.global_only && !m_global))
			continue;
		add(Kind::Header, g.title);
		// vk-285-139: the cheat disc itself, on the sheet for all games, before its switch.
		if (g.title == "Discs" && m_global)
			add(Kind::CheatDisc, "Cheat disc");
		for (const OptionDef& d : g.items)
			add(Kind::Option, d.label).def = &d;
		// 2026-10-05 (AI-assisted): the game's HD texture pack, under Texture replacements.
		if (g.title == "Graphics" && m_texture_pack_row && !m_global && !m_serial.empty())
			add(Kind::TexturePack, "HD texture pack");
	}
	if (settings && !m_paths.memcards_dir.empty())
	{
		add(Kind::Header, "Memory cards");
		add(Kind::Card, "Slot 1").slot = 1;
		add(Kind::Card, "Slot 2").slot = 2;
		add(Kind::NewCard, "New card");
	}
	// 2026-10-08 (AI-assisted; testers asked for the PS2's own boot and menu): the PS2's menu with no disc, from the sheet
	// for all games.
	if (settings && m_global && m_system_menu_row)
	{
		add(Kind::Header, "PS2");
		add(Kind::SystemMenu, "PS2 system menu");
	}
	// 2026-10-08: the "Get patches and cheats" row heads the game's patches (shown with none yet), then its cheats.
	const bool online = settings && m_online_patch_row && !m_global && !m_serial.empty();
	if (settings && (!m_patches.empty() || online))
	{
		add(Kind::Header, "Patches");
		if (online)
			add(Kind::OnlinePatches, "Get patches and cheats");
		for (const PatchGroup& p : m_patches)
			add(Kind::Patch, p.name).patch_desc = p.description.empty() ? p.file : p.description + " (" + p.file + ")";
	}
	if (settings && !m_cheats.empty())
	{
		add(Kind::Header, "Cheats");
		for (const PatchGroup& c : m_cheats)
			add(Kind::Cheat, c.name).patch_desc = c.description.empty() ? c.file : c.description + " (" + c.file + ")";
	}
	add(Kind::Header, "");
	if (settings)
		add(Kind::ResetAll, m_global ? "Reset to PCSX2's defaults" : "Follow the settings for all games");
	else
		add(Kind::ResetAll, m_global ? "Reset the controls" : "Follow the controls for all games");
}

OptionsSheet::Effective OptionsSheet::Get(const std::string& key, const std::string& def) const
{
	if (const std::string* v = Find(m_own, key))
		return {*v, From::Own};
	if (!m_global)
		if (const std::string* v = Find(m_globals, key))
			return {*v, From::Global};
	return {def, From::Default};
}

int OptionsSheet::ChoiceIndex(const OptionDef& d, const std::string& v) const
{
	for (size_t i = 0; i < d.choices.size(); i++)
		if (d.choices[i].value == v || SameNumber(d.choices[i].value, v) || (d.toggle && Truthy(v) == Truthy(d.choices[i].value)))
			return static_cast<int>(i);
	return -1;
}

std::string OptionsSheet::CardValue(int slot, bool* own_out) const
{
	const Effective fe = Get(kSlotFile[slot], kSlotDefault[slot]), oe = Get(kSlotOn[slot], "true");
	const bool own = fe.from == From::Own || oe.from == From::Own;
	if (own_out)
		*own_out = own;
	if (!own)
		return "";
	return Truthy(oe.value) ? fe.value : std::string("@none");
}

std::string OptionsSheet::Value(const Row& r) const
{
	switch (r.kind)
	{
		case Kind::Header:
			return {};
		case Kind::Option:
		{
			const Effective e = r.def->manual_fix ? FixValue(*r.def) : Get(r.def->key, r.def->def);
			const int i = ChoiceIndex(*r.def, e.value);
			return i >= 0 ? r.def->choices[static_cast<size_t>(i)].label : e.value;
		}
		case Kind::Card:
		{
			const std::string cur = CardValue(r.slot, nullptr);
			if (cur == "@none")
				return "No card";
			if (!cur.empty())
				return cur;
			// What it follows: gs.ini's card (a game's sheet) or the default.
			const Effective fe = Get(kSlotFile[r.slot], kSlotDefault[r.slot]), oe = Get(kSlotOn[r.slot], "true");
			return Truthy(oe.value) ? fe.value : std::string("No card");
		}
		case Kind::NewCard:
			return std::to_string(m_new_card_mb) + " MB";
		case Kind::Patch:
		{
			const bool on = std::find(m_own.enabled.begin(), m_own.enabled.end(), r.label) != m_own.enabled.end();
			return on ? "On" : "Off";
		}
		case Kind::Cheat:
			return CheatOn(r.label) ? "On" : "Off";
		case Kind::Recommended:
		{
			const IniState rec = ReadState(m_preset);
			return SameState(m_own, rec) ? "In use" : "Use";
		}
		case Kind::SystemMenu:
			return "Start";
		case Kind::GameFolders: // vk-285-135
		{
			const size_t n = SplitFolderList(OwnValue(kGameFoldersKey)).size();
			return n == 0 ? "Add a folder" : n == 1 ? "1 folder" : std::to_string(n) + " folders";
		}
		case Kind::BiosFolder:
		{
			const std::string v = OwnValue(kBiosFolderKey);
			return v.empty() ? "PCSX2/bios" : v;
		}
		case Kind::NfsShares:
		{
			const size_t n = SplitFolderList(OwnValue(kNfsSharesKey)).size();
			return n == 0 ? "None" : n == 1 ? "1 share" : std::to_string(n) + " shares";
		}
		case Kind::CheatDisc: // vk-285-139
		{
			const std::string* v = Find(m_own, kCheatDiscKey);
			if (!v || v->empty())
				return "None";
			for (const auto& [file, title] : m_paths.disc_images)
				if (file == *v)
					return title;
			return *v;
		}
		case Kind::ElfDisc:
		{
			const std::string* v = Find(m_own, kElfDiscKey);
			if (!v || v->empty())
				return "No disc";
			for (const auto& [file, title] : m_paths.disc_images)
				if (file == *v)
					return title;
			return *v; // not on the shelf now: its name as set
		}
		case Kind::ResetAll:
		case Kind::TexturePack: // the app's (fe_app.cpp, from fe_texpacks.h)
		case Kind::OnlinePatches: // the app's (fe_app.cpp, from fe_patchdl.h)
			return {};
	}
	return {};
}

OptionsSheet::From OptionsSheet::Source(const Row& r) const
{
	switch (r.kind)
	{
		case Kind::Option:
			return Get(r.def->key, r.def->def).from;
		case Kind::Card:
		{
			bool own = false;
			CardValue(r.slot, &own);
			if (own)
				return From::Own;
			return Get(kSlotFile[r.slot], "").from == From::Global || Get(kSlotOn[r.slot], "").from == From::Global ? From::Global : From::Default;
		}
		case Kind::Patch:
			return std::find(m_own.enabled.begin(), m_own.enabled.end(), r.label) != m_own.enabled.end() ? From::Own : From::Default;
		case Kind::Cheat:
			return CheatOn(r.label) ? From::Own : From::Default;
		case Kind::Recommended:
			return SameState(m_own, ReadState(m_preset)) ? From::Own : From::Default;
		case Kind::ElfDisc:
			return Find(m_own, kElfDiscKey) ? From::Own : From::Default;
		case Kind::CheatDisc:
			return Find(m_own, kCheatDiscKey) ? From::Own : From::Default;
		case Kind::GameFolders: // vk-285-135
			return OwnValue(kGameFoldersKey).empty() ? From::Default : From::Own;
		case Kind::BiosFolder:
			return OwnValue(kBiosFolderKey).empty() ? From::Default : From::Own;
		case Kind::NfsShares:
			return OwnValue(kNfsSharesKey).empty() ? From::Default : From::Own;
		default:
			return From::Default;
	}
}

std::string OptionsSheet::Help(const Row& r) const
{
	const std::string follows = m_global ? "PCSX2's default" : "the setting for all games";
	switch (r.kind)
	{
		case Kind::Option:
		{
			std::string h = r.def->hint;
			const Effective e = Get(r.def->key, r.def->def);
			std::string from;
			// 2026-10-08: the manual hardware fixes: whether they count now, and the game's own fixes.
			if (r.def->key == kManualFixesKey && !m_global)
			{
				std::string list;
				for (const auto& [key, value] : m_game_fixes)
				{
					std::string name = key, shown = value;
					for (const OptionGroup& g : OptionGroups())
						for (const OptionDef& d : g.items)
							if (d.manual_fix && d.key == key)
							{
								name = d.label;
								const int i = ChoiceIndex(d, value);
								if (i >= 0)
									shown = d.choices[static_cast<size_t>(i)].label;
							}
					if (name.compare(0, 10, "UserHacks_") == 0)
						name.erase(0, 10);
					list += (list.empty() ? "" : ", ") + name + " " + shown;
				}
				h += list.empty() ? " PCSX2 has no fixes of these for this game." : " This game's own fixes: " + list + ".";
			}
			else if (r.def->manual_fix)
			{
				const bool on = ManualFixesOn();
				if (!on && e.from == From::Own)
					from = "Set for this game; counts while Manual hardware fixes is on.";
				else if (!on)
					from = std::string(GameFix(r.def->key) ? "PCSX2's own fix for this game." : "PCSX2's default.") +
					       " Changing it turns Manual hardware fixes on.";
				else if (e.from == From::Own)
					from = "Set for this game.";
				else if (e.from == From::Global)
					from = "Follows the setting for all games.";
				else
					from = "PCSX2's default.";
				return h.empty() ? from : h + " " + from;
			}
			if (e.from == From::Own)
				from = m_global ? "Set for all games." : "Set for this game.";
			else if (e.from == From::Global)
				from = "Follows the setting for all games.";
			else if (r.def->key.compare(0, 7, "PS5SX2/") == 0) // vk-285-116: PS5SX2's own options have PS5SX2's defaults
				from = "The default.";
			else
				from = "PCSX2's default.";
			if (r.def->restart)
				from += " Used when a game starts.";
			return h.empty() ? from : h + " " + from;
		}
		case Kind::Card:
		{
			// The card's size, when it is one of memcards/ (8, 16, 32 or 64 MB, or a PS1 card).
			std::string size;
			const std::string shown = Value(r);
			for (const CardFile& c : m_cards)
				if (Lower(c.name) == Lower(shown))
				{
					const std::string sz = CardSize(c.bytes);
					const bool an = sz[0] == '8' || sz.rfind("11", 0) == 0 || sz.rfind("18", 0) == 0; // "an 8 MB card"
					size = " " + c.name + " is " + (an ? "an " : "a ") + sz + " card.";
				}
			return "Which memory card is in slot " + std::to_string(r.slot) + "." + size + " " +
			       (Source(r) == From::Own ? std::string("Set for ") + (m_global ? "all games." : "this game.") : "Follows " + follows + ".") +
			       " Cards are the files in memcards/.";
		}
		case Kind::NewCard:
			return "A blank card, named Card 1, Card 2 and so on. The game, or the PS2 browser, formats it the first time. Most games are "
			       "happiest with 8 MB; bigger cards run out much later. Pick it in a slot afterwards.";
		case Kind::Patch:
			return r.patch_desc.empty() ? "A patch from the patches folder." : r.patch_desc;
		case Kind::Cheat:
			return (r.patch_desc.empty() ? std::string("A cheat from the cheats folder.") : r.patch_desc) +
			       " Turning a cheat on turns on cheats for this game; cheats are left out in hardcore RetroAchievements mode.";
		case Kind::CheatDisc:
			return "A cheat disc (a GameShark, Action Replay or CodeBreaker image on the shelf), left and right to pick it. Games with Start from the cheat disc on start from it, as disc 1, with their own discs after it: hold the change disc buttons (Controls) when it asks for the game's disc. Triangle: none.";
		case Kind::ElfDisc:
			return "The disc image this ELF runs with, as PCSX2's ELF properties set it: a patched or translated game's executable "
			       "with its own disc, or homebrew that reads one. No disc: the ELF alone. Used when it starts.";
		case Kind::GameFolders: // vk-285-135
		{
			std::string h = "Cross: pick a folder on any drive or NFS share to list games from (and the folders in it); the ones added "
			                "are at the top there, Triangle removes one. Without them PS5SX2 looks in /data/PCSX2/games, /data/PCSX2 and "
			                "every USB, extended storage and M.2 drive. Read when PS5SX2 starts.";
			const std::vector<std::string> list = SplitFolderList(OwnValue(kGameFoldersKey));
			if (!list.empty())
				h += " Now: " + JoinFolderList(list) + ".";
			return h;
		}
		case Kind::BiosFolder:
			return "Cross: pick the folder where the BIOS is looked for first. Triangle: back to /data/PCSX2/bios (then /data/PCSX2 and "
			       "the drives' bios folders). Read when PS5SX2 starts.";
		case Kind::NfsShares:
		{
			std::string h = "Cross: the NFS shares, and Add a share with the PS5's keyboard: nfs://<server>/<shared folder> (NFS v3, else "
			                "v4; Windows: WinNFSd or haneWIN). Their games are listed like a drive's, from /nfs/<server>/. Read-only. "
			                "Mounted when PS5SX2 starts; boot.log says how it went.";
			const std::vector<std::string> list = SplitFolderList(OwnValue(kNfsSharesKey));
			if (!list.empty())
				h += " Now: " + JoinFolderList(list) + ".";
			return h;
		}
		case Kind::SystemMenu:
			return "Cross twice: the PS2's own menu with no disc in, as a PS2 with its tray empty: the memory card browser (copy and "
			       "delete saves), the clock, the language. The memory cards set here are in the slots. Back to the shelf as from a game.";
		case Kind::OnlinePatches:
			return "Cross: fetch this game's patches from PCSX2's patch list and Gabominated's 50/60 fps and widescreen patches, and its "
			       "cheats, from GitHub (by the game's serial and CRC). They show up here, each off until you turn it on. Your own files "
			       "are never replaced.";
		case Kind::Recommended:
		{
			std::string summary;
			const IniState rec = ReadState(m_preset);
			for (const OptionGroup& g : OptionGroups())
				for (const OptionDef& d : g.items)
					if (const std::string* v = Find(rec, d.key))
					{
						if (d.short_fmt.empty())
							continue;
						const int i = ChoiceIndex(d, *v);
						std::string label = i >= 0 ? d.choices[static_cast<size_t>(i)].label : *v;
						if (d.toggle)
							label = Truthy(*v) ? "on" : "off";
						std::string s = d.short_fmt;
						const size_t pct = s.find('%');
						if (pct != std::string::npos)
							s.replace(pct, 1, label);
						summary += (summary.empty() ? "" : " \xC2\xB7 ") + s;
					}
			for (const std::string& p : rec.enabled)
				summary += (summary.empty() ? "" : " \xC2\xB7 ") + p + " patch";
			if (!m_has_preset && !m_global)
				return "Nothing tuned for this game yet: using it makes the game follow the settings for all games.";
			return "What was tuned and tested on a PS5 Pro: " + (summary.empty() ? std::string("follows the settings for all games") : summary) + ".";
		}
		case Kind::ResetAll:
			if (m_tab == kTabControls) // vk-285-116
				return m_global ? "Every control on this tab goes back to its default: each button presses its own, the sticks as they are, "
				                  "L3+R3 + D-pad up or down to save or load."
				                : "This game's controls go back to following the controls for all games.";
			return m_global ? "Every option on this tab goes back to PCSX2's default. Lines of gs.ini this sheet doesn't show stay."
			                : "This game's options go back to following the settings for all games. Its patches, memory cards and controls "
			                  "stay.";
		default:
			return {};
	}
}

bool OptionsSheet::Save(const std::vector<Change>& changes, const std::string& note)
{
	std::string what, error;
	if (!EditSettingsFile(m_path, m_global ? m_header : m_header + ": written from the PS5SX2 shelf", changes, what, error))
	{
		m_status = "Couldn't save: " + error;
		std::printf("[options] %s: %s\n", m_file_label.c_str(), m_status.c_str());
		Reload();
		return false;
	}
	m_saved++;
	m_status = "Saved";
	std::printf("[options] %s: %s\n", m_file_label.c_str(), what.empty() ? "no change" : what.c_str());
	if (m_paths.log)
		m_paths.log("shelf: " + m_file_label + ": " + (note.empty() ? std::string() : note + ": ") + (what.empty() ? std::string("no change") : what));
	Reload();
	return true;
}

bool OptionsSheet::Step(const Row& r, int dir)
{
	m_armed_row = -1;
	switch (r.kind)
	{
		case Kind::Option:
		{
			const OptionDef& d = *r.def;
			const Effective e = d.manual_fix ? FixValue(d) : Get(d.key, d.def);
			const int n = static_cast<int>(d.choices.size());
			int i = ChoiceIndex(d, e.value);
			i = i < 0 ? 0 : ((i + dir) % n + n) % n;
			const std::string& v = d.choices[static_cast<size_t>(i)].value;
			std::vector<Change> ch = {{Change::Set, d.key, v}};
			// 2026-10-08: a fix changed, or manual fixes turned on, on a game's sheet: manual fixes on, from the game's own.
			if (!m_global && (d.manual_fix || (d.key == kManualFixesKey && Truthy(v))) && !ManualFixesOn())
			{
				const std::vector<Change> more = ManualFixesOnChanges(d.key);
				ch.insert(ch.end(), more.begin(), more.end());
			}
			return Save(ch);
		}
		case Kind::Card:
		{
			// "" follows, then the cards (and the file set now when it isn't in memcards/), then "@none", no card.
			std::vector<std::string> list = {""};
			for (const CardFile& c : m_cards)
				list.push_back(c.name);
			const std::string cur = CardValue(r.slot, nullptr);
			if (cur != "@none" && std::find(list.begin(), list.end(), cur) == list.end())
				list.push_back(cur);
			list.push_back("@none");
			const int n = static_cast<int>(list.size());
			const int at = static_cast<int>(std::find(list.begin(), list.end(), cur) - list.begin());
			const std::string& v = list[static_cast<size_t>(((at + dir) % n + n) % n)];
			const std::string file = kSlotFile[r.slot], on = kSlotOn[r.slot];
			if (v.empty())
				return Save({{Change::Unset, file, {}}, {Change::Unset, on, {}}});
			if (v == "@none")
				return Save({{Change::Unset, file, {}}, {Change::Set, on, "false"}});
			std::vector<Change> ch = {{Change::Set, file, v}};
			if (!Truthy(Get(on, "true").value))
				ch.push_back({Change::Set, on, "true"});
			return Save(ch);
		}
		case Kind::NewCard:
		{
			static const int sizes[] = {8, 16, 32, 64};
			int i = 0;
			while (i < 3 && sizes[i] != m_new_card_mb)
				i++;
			m_new_card_mb = sizes[((i + dir) % 4 + 4) % 4];
			m_status.clear();
			return true;
		}
		case Kind::Patch:
		{
			const bool on = std::find(m_own.enabled.begin(), m_own.enabled.end(), r.label) != m_own.enabled.end();
			return Save({{on ? Change::PatchOff : Change::PatchOn, r.label, {}}});
		}
		case Kind::Cheat:
			return SetCheat(r.label, !CheatOn(r.label));
		case Kind::ElfDisc:
		case Kind::CheatDisc: // vk-285-139: the same list, in gs.ini
		{
			const char* const key = r.kind == Kind::CheatDisc ? kCheatDiscKey : kElfDiscKey;
			// No disc, then the shelf's disc images in its order (and the one set, when it isn't on the shelf now).
			std::vector<std::string> list = {""};
			for (const auto& image : m_paths.disc_images)
				list.push_back(image.first);
			const std::string* v = Find(m_own, key);
			const std::string cur = v ? *v : std::string();
			if (!cur.empty() && std::find(list.begin(), list.end(), cur) == list.end())
				list.push_back(cur);
			const int n = static_cast<int>(list.size());
			const int at = static_cast<int>(std::find(list.begin(), list.end(), cur) - list.begin());
			const std::string& next = list[static_cast<size_t>(((at + dir) % n + n) % n)];
			if (next.empty())
				return v ? Save({{Change::Unset, key, {}}}) : false;
			return Save({{Change::Set, key, next}});
		}
		default:
			return false;
	}
}

// 2026-10-08: a cheat group on or off in this game's file, and PCSX2's cheats for the game with it: on while any of its
// groups is (EmuCore/EnableCheats; PCSX2 applies no cheat without it), unset when the last one goes.
bool OptionsSheet::CheatOn(const std::string& name) const
{
	return std::find(m_own.cheats.begin(), m_own.cheats.end(), name) != m_own.cheats.end();
}

bool OptionsSheet::SetCheat(const std::string& name, bool on)
{
	std::vector<Change> ch = {{on ? Change::CheatOn : Change::CheatOff, name, {}}};
	if (on)
	{
		if (!Truthy(Get("EmuCore/EnableCheats", "false").value) || Get("EmuCore/EnableCheats", "false").from != From::Own)
			ch.push_back({Change::Set, "EmuCore/EnableCheats", "true"});
	}
	else
	{
		const bool others = std::any_of(m_own.cheats.begin(), m_own.cheats.end(), [&](const std::string& c) { return c != name; });
		if (!others && Get("EmuCore/EnableCheats", "false").from == From::Own)
			ch.push_back({Change::Unset, "EmuCore/EnableCheats", {}});
	}
	return Save(ch);
}

// 2026-10-08 (AI-assisted): PCSX2's manual hardware fixes (EmuCore/GS/UserHacks), on in this file or in gs.ini.
bool OptionsSheet::ManualFixesOn() const
{
	return Truthy(Get(kManualFixesKey, "false").value);
}

const std::string* OptionsSheet::GameFix(const std::string& key) const
{
	for (const auto& kv : m_game_fixes)
		if (kv.first == key)
			return &kv.second;
	return nullptr;
}

// While manual fixes are off, PCSX2 uses the game's own fix (the game database's), not what the row follows: shown so.
OptionsSheet::Effective OptionsSheet::FixValue(const OptionDef& d) const
{
	Effective e = Get(d.key, d.def);
	if (e.from != From::Own && !ManualFixesOn())
		if (const std::string* v = GameFix(d.key))
			e = {*v, From::Default};
	return e;
}

// Manual fixes on, and the game's own fixes written as this file's (the ones it doesn't set already, and not `except_key`,
// which the caller sets), so PCSX2 keeps them (GameDatabase.cpp: a fix the settings give the same value isn't left out).
std::vector<Change> OptionsSheet::ManualFixesOnChanges(const std::string& except_key) const
{
	std::vector<Change> ch;
	if (except_key != kManualFixesKey)
		ch.push_back({Change::Set, kManualFixesKey, "true"});
	for (const auto& [key, value] : m_game_fixes)
		if (key != except_key && !Find(m_own, key))
			ch.push_back({Change::Set, key, value});
	return ch;
}

bool OptionsSheet::Reset(const Row& r)
{
	m_armed_row = -1;
	switch (r.kind)
	{
		case Kind::Option:
			if (Get(r.def->key, r.def->def).from != From::Own)
				return false;
			return Save({{Change::Unset, r.def->key, {}}});
		case Kind::Card:
		{
			bool own = false;
			CardValue(r.slot, &own);
			if (!own)
				return false;
			return Save({{Change::Unset, kSlotFile[r.slot], {}}, {Change::Unset, kSlotOn[r.slot], {}}});
		}
		case Kind::Patch:
			if (std::find(m_own.enabled.begin(), m_own.enabled.end(), r.label) == m_own.enabled.end())
				return false;
			return Save({{Change::PatchOff, r.label, {}}});
		case Kind::Cheat:
			if (!CheatOn(r.label))
				return false;
			return SetCheat(r.label, false);
		case Kind::ElfDisc:
			if (!Find(m_own, kElfDiscKey))
				return false;
			return Save({{Change::Unset, kElfDiscKey, {}}});
		case Kind::CheatDisc:
			if (!Find(m_own, kCheatDiscKey))
				return false;
			return Save({{Change::Unset, kCheatDiscKey, {}}});
		case Kind::BiosFolder: // vk-285-135
			if (OwnValue(kBiosFolderKey).empty())
				return false;
			return SetOwn(kBiosFolderKey, "", "BIOS folder back to /data/PCSX2/bios");
		case Kind::GameFolders:
		case Kind::NfsShares:
			m_status = "Cross: the list (Triangle there removes one)";
			return false;
		default:
			return false;
	}
}

std::string OptionsSheet::OwnValue(const std::string& key) const
{
	const std::string* v = Find(m_own, key);
	return v ? *v : std::string();
}

bool OptionsSheet::SetOwn(const std::string& key, const std::string& value, const std::string& note)
{
	if (OwnValue(key) == value)
		return false;
	return Save({{value.empty() ? Change::Unset : Change::Set, key, value}}, note);
}

bool OptionsSheet::Armed(const Row& r, double now) const
{
	return m_armed_row >= 0 && now < m_armed_until && &m_rows[static_cast<size_t>(m_armed_row)] == &r;
}

bool OptionsSheet::Activate(const Row& r, double now)
{
	const int index = static_cast<int>(&r - m_rows.data());
	switch (r.kind)
	{
		case Kind::NewCard:
		{
			std::string made, error;
			bool ok = false;
			for (int n = 1; n <= 99 && !ok; n++)
			{
				const std::string name = "Card " + std::to_string(n) + ".ps2";
				bool taken = false;
				for (const CardFile& c : m_cards)
					taken = taken || Lower(c.name) == Lower(name);
				if (taken)
					continue;
				ok = CreateCard(m_paths.memcards_dir, static_cast<uint64_t>(m_new_card_mb), name, made, error);
				if (!ok && error.find("already") == std::string::npos)
					break;
			}
			if (!ok)
			{
				m_status = "Couldn't make a card: " + error;
				return false;
			}
			m_status = "Made " + made + " (" + std::to_string(m_new_card_mb) + " MB): pick it in a slot";
			std::printf("[options] memory card %s made: %d MB\n", made.c_str(), m_new_card_mb);
			if (m_paths.log)
				m_paths.log("shelf: memory card made: " + made + " (" + std::to_string(m_new_card_mb) + " MB, blank)");
			Reload();
			return true;
		}
		case Kind::GameFolders: // vk-285-135: the app opens its picker or the share list (TakeFolderRequest)
		case Kind::BiosFolder:
		case Kind::NfsShares:
			m_folder_request = r.kind;
			return true;
		case Kind::SystemMenu: // 2026-10-08: the first press arms, a second within 4 s asks the app to start it
			if (!(m_armed_row == index && now < m_armed_until))
			{
				m_armed_row = index;
				m_armed_until = now + 4.0;
				m_status = "Press again to start the PS2 system menu";
				return false;
			}
			m_armed_row = -1;
			m_system_menu = true;
			m_status = "Starting the PS2 system menu";
			std::printf("[options] the PS2 system menu asked for\n");
			return true;
		case Kind::Recommended:
		case Kind::ResetAll:
		{
			if (r.kind == Kind::Recommended && Value(r) == "In use")
				return false;
			if (!(m_armed_row == index && now < m_armed_until))
			{
				m_armed_row = index;
				m_armed_until = now + 4.0;
				m_status = "Press again to confirm";
				return false;
			}
			m_armed_row = -1;
			if (r.kind == Kind::Recommended)
			{
				std::string what, error;
				if (!ApplyRecommended(m_path, m_paths.presets, m_id, m_header, what, error))
				{
					m_status = "Couldn't apply: " + error;
					return false;
				}
				m_saved++;
				m_status = "Recommended settings in use";
				std::printf("[options] %s: recommended settings\n", m_file_label.c_str());
				if (m_paths.log)
					m_paths.log("shelf: " + m_file_label + ": recommended settings" + (what.empty() ? std::string(", no change") : ": " + what) +
					            "; the old file is " + m_file_label + ".before-recommended");
				Reload();
				return true;
			}
			std::vector<Change> ch;
			for (const OptionGroup& g : OptionGroups())
				if (g.tab == m_tab) // vk-285-116: the tab shown
					for (const OptionDef& d : g.items)
						if (Find(m_own, d.key))
							ch.push_back({Change::Unset, d.key, {}});
			// 2026-10-08: and the game's own fixes that turning manual fixes on wrote without a row of their own.
			if (m_tab == kTabSettings && !m_global)
				for (const auto& kv : m_game_fixes)
					if (Find(m_own, kv.first) &&
						std::none_of(ch.begin(), ch.end(), [&](const Change& c) { return c.key == kv.first; }))
						ch.push_back({Change::Unset, kv.first, {}});
			if (ch.empty())
			{
				m_status = "Nothing to reset";
				return false;
			}
			if (m_tab == kTabControls)
				return Save(ch, m_global ? "controls reset to their defaults" : "controls follow all games again");
			return Save(ch, m_global ? "reset to PCSX2's defaults" : "follows all games again");
		}
		default:
			return Step(r, 1);
	}
}
} // namespace fe
