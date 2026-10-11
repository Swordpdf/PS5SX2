// Host test helper for orbis-shims/OrbisDDS.h: decodes a DDS file and writes every level as raw RGBA8 to
// <out-prefix>.<level>.rgba, printing "level width height" lines. Used by test_dds.py.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbis-shims/OrbisDDS.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: dds_dump <file.dds> <out-prefix> [base-only]\n");
		return 2;
	}
	std::FILE* f = std::fopen(argv[1], "rb");
	if (!f)
	{
		std::fprintf(stderr, "cannot open %s\n", argv[1]);
		return 2;
	}
	std::vector<uint8_t> data;
	uint8_t buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		data.insert(data.end(), buf, buf + n);
	std::fclose(f);
	std::vector<OrbisDDS::Image> levels;
	const char* why = "";
	if (!OrbisDDS::Decode(data.data(), data.size(), argc > 3, levels, why))
	{
		std::printf("FAIL %s\n", why);
		return 1;
	}
	for (size_t i = 0; i < levels.size(); i++)
	{
		const std::string name = std::string(argv[2]) + "." + std::to_string(i) + ".rgba";
		std::FILE* o = std::fopen(name.c_str(), "wb");
		if (!o)
			return 2;
		std::fwrite(levels[i].rgba.data(), 1, levels[i].rgba.size(), o);
		std::fclose(o);
		std::printf("%zu %u %u\n", i, levels[i].width, levels[i].height);
	}
	return 0;
}
