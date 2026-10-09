// PS5SX2 (AI-assisted): a check of the forwarder's launch arguments (orbis-shims/OrbisForward.h) on a PC.
//   g++ -std=c++20 -Wall -I../../orbis-shims -o /tmp/test_forward test_forward.cpp && /tmp/test_forward
#include "OrbisForward.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace orbis_forward;

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

template <size_t N>
static Args ParseOf(const char* (&argv)[N])
{
	return Parse(static_cast<int>(N), const_cast<char**>(argv));
}

int main()
{
	{
		const char* argv[] = {"eboot.bin", "--rom", "Okami.iso", "--exit-after-game"};
		const Args a = ParseOf(argv);
		CHECK(a.rom == "Okami.iso");
		CHECK(a.exit_after_game);
	}
	{
		const char* argv[] = {"eboot.bin", "--rom=/mnt/usb0/PS2/Okami.chd"};
		const Args a = ParseOf(argv);
		CHECK(a.rom == "/mnt/usb0/PS2/Okami.chd");
		CHECK(!a.exit_after_game);
	}
	{
		const char* argv[] = {"eboot.bin", "--unknown", "--rom"}; // --rom with nothing after it
		const Args a = ParseOf(argv);
		CHECK(a.rom.empty());
	}
	CHECK(Parse(0, nullptr).rom.empty());

	const std::vector<std::string> dirs = {"/data/PCSX2/games", "/data/PCSX2"};
	CHECK((Candidates("Okami.iso", dirs) == std::vector<std::string>{"/data/PCSX2/games/Okami.iso", "/data/PCSX2/Okami.iso"}));
	CHECK((Candidates("RPG/Okami.iso\r\n", dirs) ==
		   std::vector<std::string>{"/data/PCSX2/games/RPG/Okami.iso", "/data/PCSX2/RPG/Okami.iso"}));
	// OrbisDir("games") is the top folder when games/ doesn't exist: the same path once.
	CHECK((Candidates("Okami.iso", {"/data/PCSX2", "/data/PCSX2"}) == std::vector<std::string>{"/data/PCSX2/Okami.iso"}));
	CHECK((Candidates("/mnt/usb0/Okami.iso", dirs) == std::vector<std::string>{"/mnt/usb0/Okami.iso"}));
	CHECK(Candidates("../Okami.iso", dirs).empty());
	CHECK(Candidates("RPG/../../Okami.iso", dirs).empty());
	CHECK(Candidates("RPG/..Okami.iso", dirs).size() == 2);
	CHECK(Candidates("", dirs).empty());

	printf(s_failures ? "%d failed\n" : "all passed\n", s_failures);
	return s_failures ? 1 : 0;
}
