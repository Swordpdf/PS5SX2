// Host check for the launch arguments passed by the PS5 Forwarder Format launcher.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "OrbisForwarderArgs.h"

#include <cassert>

int main()
{
	assert(OrbisParseForwarderArgs(0, nullptr).rom.empty());

	const char* direct[] = {"--rom", "/data/PCSX2/games/My Game.chd", "--exit-after-game"};
	const OrbisForwarderArgs from_tile = OrbisParseForwarderArgs(3, direct);
	assert(!from_tile.invalid);
	assert(from_tile.rom == "/data/PCSX2/games/My Game.chd");
	assert(from_tile.exit_after_game);

	const char* with_program[] = {"/app0/eboot.bin", "--rom", "My Game.iso"};
	const OrbisForwarderArgs from_program = OrbisParseForwarderArgs(3, with_program);
	assert(!from_program.invalid);
	assert(from_program.rom == "My Game.iso");
	assert(!from_program.exit_after_game);

	const char* missing[] = {"--rom"};
	assert(OrbisParseForwarderArgs(1, missing).invalid);
	const char* flag_as_path[] = {"--rom", "--exit-after-game"};
	assert(OrbisParseForwarderArgs(2, flag_as_path).invalid);
	const char* duplicate[] = {"--rom", "a.iso", "--rom", "b.iso"};
	assert(OrbisParseForwarderArgs(4, duplicate).invalid);
	const char* no_rom[] = {"--exit-after-game"};
	assert(OrbisParseForwarderArgs(1, no_rom).invalid);
	const char* unsupported[] = {"--rom", "game.iso", "--other"};
	assert(OrbisParseForwarderArgs(3, unsupported).invalid);
	const char* leading_dash_name[] = {"--rom", "-My Game.iso"};
	assert(OrbisParseForwarderArgs(2, leading_dash_name).rom == "-My Game.iso");
	const char* ordinary_launch[] = {"/app0/eboot.bin", "--system-option"};
	const OrbisForwarderArgs ordinary = OrbisParseForwarderArgs(2, ordinary_launch);
	assert(!ordinary.invalid && ordinary.rom.empty());
}
