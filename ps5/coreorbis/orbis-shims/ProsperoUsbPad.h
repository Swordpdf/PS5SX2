// PS5SX2 (vk-285-140, AI-assisted): USB controllers the PS5 doesn't take as its own controllers: a PS3-mode USB guitar (a
// modded guitar set to act as a PS3 controller, a PS3 Guitar Hero or Rock Band guitar), PS3 USB pads, a DualShock 3 on a
// cable and Xbox 360 (XInput) pads and guitars. See ProsperoUsbPad.cpp.
#pragma once

#include "OrbisUsbPadDecode.h"

// Starts the thread that looks for such a device and reads it (once; later calls do nothing). The flag file nousbpad
// keeps it off.
void OrbisUsbPadStart();

// Waits up to `ms` for the thread's first look at the USB devices. True when a guitar was found then (main-boot puts
// PCSX2's Guitar controller on PS2 port 1 for it).
bool OrbisUsbPadWaitGuitar(int ms);

// What the device holds now, for the pad thread. False with nothing connected.
struct OrbisUsbPadOut
{
	bool guitar = false;
	orbis_usbpad::PadOut pad;       // as a controller (a pad, or a guitar shown as a DualShock 2)
	orbis_usbpad::GuitarOut strings; // as a guitar (guitar == true)
};
bool OrbisUsbPadState(OrbisUsbPadOut& out);
