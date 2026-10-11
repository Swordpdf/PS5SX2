// PS5 port frontend, PC test (vk-285-134, AI-assisted): fe_bios.cpp: a BIOS taken out of a .zip into the BIOS folder, and
// the shelf's sentence for what build 130's testers had instead (an empty folder, no folder, a PS1 BIOS, a ROM1, a 4 MB file
// that isn't a BIOS, a .zip without one, a game, folders of nothing). A "BIOS" here is a 4 MB file with RESET and ROMVER in
// it, which is what the test's is_bios looks for (PCSX2's IsBIOS reads the ROMDIR on the console).
//   ps5/frontend/host/test-bios.sh
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fe_bios.h"

#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

static int s_fail = 0, s_checks = 0;
#define CHECK(c)                                                         \
	do                                                                   \
	{                                                                    \
		s_checks++;                                                      \
		if (!(c))                                                        \
		{                                                                \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
			s_fail++;                                                    \
		}                                                                \
	} while (0)

static std::string s_root;

static std::string Dir(const std::string& rel)
{
	const std::string path = s_root + "/" + rel;
	std::string cur;
	for (size_t i = 0; i <= path.size(); i++)
		if (i == path.size() || path[i] == '/')
		{
			cur = path.substr(0, i);
			if (!cur.empty())
				mkdir(cur.c_str(), 0777);
		}
	return path;
}

static std::vector<char> Bytes(size_t size, bool bios)
{
	std::vector<char> b(size, 0x5a);
	if (bios && size > 64)
	{
		std::memcpy(b.data() + 16, "RESET", 5);
		std::memcpy(b.data() + 40, "ROMVER", 6);
	}
	return b;
}

static void File(const std::string& path, size_t size, bool bios = false)
{
	const std::vector<char> b = Bytes(size, bios);
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
	{
		std::printf("can't write %s\n", path.c_str());
		std::exit(2);
	}
	if (!b.empty())
		std::fwrite(b.data(), 1, b.size(), f);
	std::fclose(f);
}

// A zip of stored (uncompressed) entries, written by hand: the vendored libarchive only reads.
static void Put16(std::vector<unsigned char>& v, unsigned x) { v.push_back(x & 0xff); v.push_back((x >> 8) & 0xff); }
static void Put32(std::vector<unsigned char>& v, unsigned long x) { Put16(v, x & 0xffff); Put16(v, (x >> 16) & 0xffff); }
static void Zip(const std::string& path, const std::vector<std::pair<std::string, std::vector<char>>>& files)
{
	std::vector<unsigned char> out, central;
	for (const auto& [name, data] : files)
	{
		const unsigned long crc = crc32(0L, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size()));
		const unsigned long offset = out.size();
		Put32(out, 0x04034b50); Put16(out, 10); Put16(out, 0); Put16(out, 0); Put16(out, 0); Put16(out, 0x21);
		Put32(out, crc); Put32(out, data.size()); Put32(out, data.size()); Put16(out, name.size()); Put16(out, 0);
		out.insert(out.end(), name.begin(), name.end());
		out.insert(out.end(), data.begin(), data.end());
		Put32(central, 0x02014b50); Put16(central, 20); Put16(central, 10); Put16(central, 0); Put16(central, 0); Put16(central, 0);
		Put16(central, 0x21); Put32(central, crc); Put32(central, data.size()); Put32(central, data.size()); Put16(central, name.size());
		Put16(central, 0); Put16(central, 0); Put16(central, 0); Put16(central, 0); Put32(central, 0); Put32(central, offset);
		central.insert(central.end(), name.begin(), name.end());
	}
	const unsigned long cd_offset = out.size();
	out.insert(out.end(), central.begin(), central.end());
	Put32(out, 0x06054b50); Put16(out, 0); Put16(out, 0); Put16(out, files.size()); Put16(out, files.size());
	Put32(out, central.size()); Put32(out, cd_offset); Put16(out, 0);
	FILE* f = std::fopen(path.c_str(), "wb");
	std::fwrite(out.data(), 1, out.size(), f);
	std::fclose(f);
}

static bool IsBios(const std::string& path)
{
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0 || st.st_size < static_cast<off_t>(fe::kBiosMinSize))
		return false;
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char head[64] = {};
	const size_t n = std::fread(head, 1, sizeof(head), f);
	std::fclose(f);
	return n == sizeof(head) && std::memcmp(head + 16, "RESET", 5) == 0 && std::memcmp(head + 40, "ROMVER", 6) == 0;
}

static bool Has(const std::string& s, const char* part)
{
	const bool ok = s.find(part) != std::string::npos;
	if (!ok)
		std::printf("  (got \"%s\", wanted \"%s\")\n", s.c_str(), part);
	return ok;
}

static std::string Describe(const std::string& bios_rel, const std::string& top_rel)
{
	return fe::DescribeBiosProblem(s_root + "/" + bios_rel, s_root + "/" + top_rel, {s_root + "/" + top_rel}, IsBios);
}

int main(int argc, char** argv)
{
	s_root = argc > 1 ? argv[1] : "/tmp/fe-bios-test";
	const size_t MB4 = 4u * 1024u * 1024u, KB512 = 512u * 1024u;

	// The empty folder (700 of the 961 failed starts) and no folder at all (bios_dir is then /data/PCSX2 itself).
	Dir("a/PCSX2/bios");
	CHECK(Has(Describe("a/PCSX2/bios", "a/PCSX2"), "bios is empty."));
	Dir("b/PCSX2/games");
	CHECK(Has(Describe("b/PCSX2", "b/PCSX2"), "There is no bios folder: make "));
	CHECK(Has(Describe("b/PCSX2", "b/PCSX2"), "b/PCSX2/bios and copy your BIOS file into it."));

	// A PS1 BIOS (scph5501.bin, 512 KB) and a ROM1 without its main file.
	File(Dir("c/PCSX2/bios") + "/scph5501.bin", KB512);
	CHECK(Has(Describe("c/PCSX2/bios", "c/PCSX2"), "scph5501.bin (512 KB) is a PS1 BIOS"));
	File(Dir("d/PCSX2/bios") + "/SCPH-70012_BIOS_V12_USA_200.ROM1", KB512);
	File(s_root + "/d/PCSX2/bios/SCPH-70012_BIOS_V12_USA_200.NVM", 1024);
	CHECK(Has(Describe("d/PCSX2/bios", "d/PCSX2"), "Only parts of a PS2 BIOS are there"));

	// A 4 MB file that isn't one; a game in the BIOS folder; an empty file; folders of nothing.
	File(Dir("e/PCSX2/bios") + "/SCPH-90001_BIOS_V18_USA_230.bin", MB4, false);
	CHECK(Has(Describe("e/PCSX2/bios", "e/PCSX2"), "SCPH-90001_BIOS_V18_USA_230.bin is the size of a PS2 BIOS but isn't one"));
	File(Dir("f/PCSX2/bios") + "/Burnout 3 Takedown.iso", 60u * 1024u * 1024u);
	CHECK(Has(Describe("f/PCSX2/bios", "f/PCSX2"), "Burnout 3 Takedown.iso is a game, not a BIOS: games go in "));
	File(Dir("g/PCSX2/bios") + "/scph10000-jp.bin", 0);
	CHECK(Has(Describe("g/PCSX2/bios", "g/PCSX2"), "scph10000-jp.bin is empty (0 bytes)"));
	Dir("h/PCSX2/bios/ps2 bios usa/inner");
	CHECK(Has(Describe("h/PCSX2/bios", "h/PCSX2"), "have no BIOS file in them."));

	// A .zip with a BIOS in a folder inside it, in the BIOS folder's own folder: taken out into the BIOS folder.
	Dir("i/PCSX2/bios/pack");
	Zip(s_root + "/i/PCSX2/bios/pack/PS2_BIOS (1).zip",
		{{"readme.txt", std::vector<char>(100, 'r')}, {"PS2 BIOS/SCPH-70012.bin", Bytes(MB4, true)}, {"scph5501.bin", Bytes(KB512, false)}});
	std::set<std::string> tried;
	std::string from;
	std::string taken = fe::ExtractBiosFromArchives({s_root + "/i/PCSX2/bios"}, s_root + "/i/PCSX2/bios", IsBios, &tried, &from);
	CHECK(taken == s_root + "/i/PCSX2/bios/SCPH-70012.bin");
	CHECK(Has(from, "PS2_BIOS (1).zip"));
	CHECK(IsBios(taken));
	struct stat st = {};
	CHECK(stat((taken + ".part").c_str(), &st) != 0);
	// Tried archives aren't opened again; a second run with a fresh list finds the name taken and picks another.
	CHECK(fe::ExtractBiosFromArchives({s_root + "/i/PCSX2/bios"}, s_root + "/i/PCSX2/bios", IsBios, &tried, &from).empty());
	std::set<std::string> fresh;
	taken = fe::ExtractBiosFromArchives({s_root + "/i/PCSX2/bios"}, s_root + "/i/PCSX2/bios", IsBios, &fresh, &from);
	CHECK(taken == s_root + "/i/PCSX2/bios/SCPH-70012 (from the archive).bin");

	// A .zip without one: nothing kept (no leftover files), and the sentence names it.
	Dir("j/PCSX2/bios");
	Zip(s_root + "/j/PCSX2/bios/bios-pack.zip", {{"SCPH-90001.bin", Bytes(MB4, false)}, {"scph1001.bin", Bytes(KB512, false)}});
	std::set<std::string> tried_j;
	CHECK(fe::ExtractBiosFromArchives({s_root + "/j/PCSX2/bios"}, s_root + "/j/PCSX2/bios", IsBios, &tried_j, &from).empty());
	CHECK(stat((s_root + "/j/PCSX2/bios/SCPH-90001.bin").c_str(), &st) != 0);
	CHECK(Has(Describe("j/PCSX2/bios", "j/PCSX2"), "No PS2 BIOS could be taken out of bios-pack.zip"));

	// A BIOS-named file outside the BIOS folder counts; a drive's other 4 MB files don't.
	Dir("k/PCSX2/bios");
	Dir("k/usb0");
	File(s_root + "/k/usb0/ProsperoEden-v1.000.030.ffpfsc", MB4, false);
	CHECK(Has(fe::DescribeBiosProblem(s_root + "/k/PCSX2/bios", s_root + "/k/PCSX2", {s_root + "/k/usb0"}, IsBios), "bios is empty."));
	File(s_root + "/k/usb0/scph39001.NVM", 1024);
	CHECK(Has(fe::DescribeBiosProblem(s_root + "/k/PCSX2/bios", s_root + "/k/PCSX2", {s_root + "/k/usb0"}, IsBios), "Only parts of a PS2 BIOS"));

	std::printf("%s: %d of %d checks passed (the BIOS before the shelf)\n", s_fail ? "FAIL" : "PASS", s_checks - s_fail, s_checks);
	return s_fail ? 1 : 0;
}
