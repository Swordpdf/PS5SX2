// vk-285-139 (AI-assisted): fe::DiscSet / DiscNumber, a game's discs for changing discs in the game.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../fe_games.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

static int failed = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); failed++; } else std::printf("ok: %s\n", #c); } while (0)

static void touch(const std::string& p) { std::ofstream(p) << "x"; }

int main(int argc, char** argv)
{
	const std::string d = argv[1];
	std::string rest;
	CHECK(fe::DiscNumber("Xenosaga Episode II (USA) (Disc 2).iso", &rest) == 2 && rest == "xenosaga episode ii (usa)");
	CHECK(fe::DiscNumber("Metal Gear Solid 3 (Disc 1 of 2).chd", &rest) == 1 && rest == "metal gear solid 3");
	CHECK(fe::DiscNumber("Okami (USA).iso", &rest) == 0 && rest == "okami (usa)");
	CHECK(fe::DiscNumber("Game (Discovery Edition).iso", nullptr) == 0);
	mkdir((d + "/a").c_str(), 0755);
	for (const char* n : {"Xeno (USA) (Disc 2).chd", "Xeno (USA) (Disc 1).iso", "Xeno (Europe) (Disc 1).iso", "Okami (USA).iso",
			 "GameShark 2 (USA).iso"})
		touch(d + "/a/" + n);
	std::vector<std::string> s = fe::DiscSet(d + "/a/Xeno (USA) (Disc 2).chd");
	CHECK(s.size() == 2 && s[0] == d + "/a/Xeno (USA) (Disc 1).iso" && s[1] == d + "/a/Xeno (USA) (Disc 2).chd");
	s = fe::DiscSet(d + "/a/Okami (USA).iso");
	CHECK(s.size() == 1 && s[0] == d + "/a/Okami (USA).iso");
	// An .m3u that lists the image wins, in its order; its missing entries are left out.
	mkdir((d + "/b").c_str(), 0755);
	touch(d + "/b/one.iso");
	touch(d + "/b/two.iso");
	std::ofstream(d + "/b/Game.m3u") << "# discs\ntwo.iso\r\none.iso\nmissing.iso\n";
	s = fe::DiscSet(d + "/b/one.iso");
	CHECK(s.size() == 2 && s[0] == d + "/b/two.iso" && s[1] == d + "/b/one.iso");
	std::printf(failed ? "discs: FAILED\n" : "discs: all passed\n");
	return failed ? 1 : 0;
}
