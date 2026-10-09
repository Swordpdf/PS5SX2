// PS5SX2 (AI-assisted): launch arguments from a home screen forwarder, after ProsperoEden's
// (blackbearreloaded/ProsperoEden#13). --rom <file> starts that disc image instead of the shelf;
// --exit-after-game closes PS5SX2 when that game goes back to the menu (ps5/docs/forwarder.md).
// Header only, so tests/forward/test_forward.cpp checks it on a PC.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace orbis_forward
{
	struct Args
	{
		std::string rom; // --rom <file> or --rom=<file>; empty when not given
		bool exit_after_game = false; // --exit-after-game
	};

	// Unknown arguments are ignored.
	inline Args Parse(int argc, char** argv)
	{
		Args result;
		if (argc <= 0 || argv == nullptr)
			return result;
		constexpr std::string_view rom_flag = "--rom";
		constexpr std::string_view rom_prefix = "--rom=";
		for (int i = 0; i < argc; ++i)
		{
			if (argv[i] == nullptr)
				break;
			const std::string_view arg{argv[i]};
			if (arg == "--exit-after-game")
				result.exit_after_game = true;
			else if (arg == rom_flag)
			{
				if (i + 1 < argc && argv[i + 1] != nullptr)
					result.rom = argv[++i];
			}
			else if (arg.starts_with(rom_prefix))
				result.rom = std::string{arg.substr(rom_prefix.size())};
		}
		return result;
	}

	// Where to look for the image: an absolute path as it is; a relative one inside each of dirs,
	// in order (games/, then /data/PCSX2), never above them: nothing for a path with a ".." part.
	inline std::vector<std::string> Candidates(std::string_view rom, const std::vector<std::string>& dirs)
	{
		while (!rom.empty() && (rom.back() == ' ' || rom.back() == '\r' || rom.back() == '\n'))
			rom.remove_suffix(1);
		if (rom.empty())
			return {};
		if (rom.front() == '/')
			return {std::string{rom}};
		for (std::string_view rest = rom; !rest.empty();)
		{
			const auto slash = rest.find('/');
			if (rest.substr(0, slash) == "..")
				return {};
			if (slash == std::string_view::npos)
				break;
			rest.remove_prefix(slash + 1);
		}
		std::vector<std::string> out;
		for (const std::string& dir : dirs)
		{
			std::string path = dir;
			if (!path.empty() && path.back() != '/')
				path += '/';
			path += rom;
			bool seen = false;
			for (const std::string& p : out)
				seen |= p == path;
			if (!seen)
				out.push_back(std::move(path));
		}
		return out;
	}
} // namespace orbis_forward
