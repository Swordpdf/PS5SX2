// PS5 port frontend: the shelf's options sheet (see fe_options.h).
//
// Copyright (C) 2026 Spyros
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
const std::vector<OptionGroup>& OptionGroups()
{
	static const std::vector<OptionGroup> groups = {
		{"Display",
			{
				Seg("upscale_multiplier", "Resolution", "1", "%", {{"1", "1x"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {"5", "5x"}, {"6", "6x"}},
					"Internal resolution. 6x is about 4K."),
				Seg("AspectRatio", "Aspect ratio", "Auto 4:3/3:2", "Aspect %",
					{{"Auto 4:3/3:2", "Auto"}, {"4:3", "4:3"}, {"16:9", "16:9"}, {"Stretch", "Stretch"}}),
				Toggle("EmuCore/EnableWideScreenPatches", "Widescreen patches", "false", "Widescreen %",
					"Uses the game's 16:9 patch when there is one."),
				Seg("TVShader", "Display filter", "0", "%", {{"6", "FSR"}, {"7", "FSR soft"}, {"0", "Classic"}, {"5", "CRT"}}),
			}},
		// vk-285-115: PCSX2's crop (EmuCore/GS/CropLeft..CropBottom, PS2 pixels), as on the page.
		{"Crop",
			{
				Seg("CropLeft", "Crop left", "0", "Crop left %", CropChoices(), kCropHint),
				Seg("CropTop", "Crop top", "0", "Crop top %", CropChoices(), kCropHint),
				Seg("CropRight", "Crop right", "0", "Crop right %", CropChoices(), kCropHint),
				Seg("CropBottom", "Crop bottom", "0", "Crop bottom %", CropChoices(), kCropHint),
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
			}},
		{"On screen",
			{
				Seg("PS5SX2/Overlay", "Info box", "2", "Info box %", {{"0", "Off"}, {"1", "FPS"}, {"2", "FPS + load"}},
					"The box in the top right corner. Load is how busy the EE, GS and VU threads are."),
				Toggle("PS5SX2/FpsGraph", "FPS graph", "false", "FPS graph %",
					"A blue graph of the last minute's frame rate in the top right corner. It shows with the info box off too."),
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
	}
	else
	{
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
	for (const OptionGroup& g : OptionGroups())
	{
		if (g.tab != m_tab)
			continue;
		add(Kind::Header, g.title);
		for (const OptionDef& d : g.items)
			add(Kind::Option, d.label).def = &d;
	}
	if (settings && !m_paths.memcards_dir.empty())
	{
		add(Kind::Header, "Memory cards");
		add(Kind::Card, "Slot 1").slot = 1;
		add(Kind::Card, "Slot 2").slot = 2;
		add(Kind::NewCard, "New card");
	}
	if (settings && !m_patches.empty())
	{
		add(Kind::Header, "Patches");
		for (const PatchGroup& p : m_patches)
			add(Kind::Patch, p.name).patch_desc = p.description.empty() ? p.file : p.description + " (" + p.file + ")";
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
			const Effective e = Get(r.def->key, r.def->def);
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
		case Kind::Recommended:
		{
			const IniState rec = ReadState(m_preset);
			return SameState(m_own, rec) ? "In use" : "Use";
		}
		case Kind::ResetAll:
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
		case Kind::Recommended:
			return SameState(m_own, ReadState(m_preset)) ? From::Own : From::Default;
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
			const Effective e = Get(d.key, d.def);
			const int n = static_cast<int>(d.choices.size());
			int i = ChoiceIndex(d, e.value);
			i = i < 0 ? 0 : ((i + dir) % n + n) % n;
			return Save({{Change::Set, d.key, d.choices[static_cast<size_t>(i)].value}});
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
		default:
			return false;
	}
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
		default:
			return false;
	}
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
