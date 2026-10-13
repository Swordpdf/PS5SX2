// Launch arguments sent by a PS5 Forwarder Format tile to PPSA99203.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstring>
#include <string>

struct OrbisForwarderArgs
{
	std::string rom;
	bool exit_after_game = false;
	bool invalid = false;
};

// sceSystemServiceLaunchApp passes the forwarder's array directly: argv[0]
// can be "--rom", while ordinary process starts may put the program there.
inline OrbisForwarderArgs OrbisParseForwarderArgs(int argc, const char* const argv[])
{
	OrbisForwarderArgs result;
	bool unknown_option = false;
	if (argc < 0 || argc > 65 || (argc > 0 && !argv))
	{
		result.invalid = true;
		return result;
	}

	for (int i = 0; i < argc; i++)
	{
		if (!argv[i])
		{
			result.invalid = true;
			return result;
		}
		if (std::strcmp(argv[i], "--rom") == 0)
		{
			if (!result.rom.empty() || ++i >= argc || !argv[i] || !argv[i][0] ||
				std::strcmp(argv[i], "--rom") == 0 || std::strcmp(argv[i], "--exit-after-game") == 0)
			{
				result.invalid = true;
				return result;
			}
			result.rom = argv[i];
		}
		else if (std::strcmp(argv[i], "--exit-after-game") == 0)
		{
			result.exit_after_game = true;
		}
		else if (std::strncmp(argv[i], "--", 2) == 0)
		{
			unknown_option = true;
		}
	}
	if ((result.exit_after_game && result.rom.empty()) || (unknown_option && !result.rom.empty()))
		result.invalid = true;
	return result;
}
