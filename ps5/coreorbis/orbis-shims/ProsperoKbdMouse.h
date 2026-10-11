// PS5SX2 (vk-285-72, vk-285-113): the PS5's USB keyboard and mouse. See ProsperoKbdMouse.cpp.
#pragma once

#include <cstdint>

// Starts the thread that reads the console's keyboard and mouse libraries (once; later calls do nothing).
void OrbisKbdMouseStart();

// At each vsync on the CPU thread: hands what the thread queued to the PS2's USB keyboard and mouse, and looks at
// whether a game reads them.
void OrbisKbdMousePump();

// vk-285-113: the PS5SX2/KeyboardMouse, MouseAim, MouseSpeed and MouseButtons settings (gs.ini or the game's file, live).
//   mode:    0 Auto (the keyboard and mouse drive the PS2 controller until a game reads the PS2's USB keyboard or mouse),
//            1 Controller only, 2 USB devices only, 3 Off
//   aim:     0 the mouse's movement does nothing, 1 it moves the right stick, 2 the left stick
//   speed:   1 Slow, 2 Normal, 3 Fast, 4 Very fast
//   buttons: 0 left click is R1 and right click L1, 1 they are R2 and L2 (the wheel click is R3 in both)
void OrbisKbdMouseConfigure(int mode, int aim, int speed, int buttons);

// The controller the keyboard and mouse make, in the layout the pad thread reads from the DualSense: ScePad button bits,
// the two triggers 0..255 and the four sticks 0..255 with 128 in the middle.
struct OrbisKbdMousePad
{
	uint32_t buttons = 0;
	uint8_t l2 = 0, r2 = 0;
	uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
};

// False while the keyboard and mouse are not driving the controller (the mode, or a game reading the PS2's USB devices);
// then `out` is left alone. True: `out` is what is held now (nothing held: the idle values above).
bool OrbisKbdMousePadState(OrbisKbdMousePad& out);

// The keyboard shortcuts of the controller mode, one per press, taken once: 0 none, 1 F1 (save the state), 2 F3 (load
// it), 3 Esc held for a second (back to the menu). The same numbers as g_orbis_state_request.
int OrbisKbdMouseTakeHotkey();
