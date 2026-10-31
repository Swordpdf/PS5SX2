// Host test for include-orbis/OrbisMcdNoEcc.h (b185, AI-assisted): .mc2 / .bin memory cards (no ECC: MemCard PRO2, SD2PSX) on the PS5 -
// the names in any case, the ECC conversion both ways, which leftover .mc2x a killed session left goes back into the card, and the durable
// write that never leaves half a card; plus the frontend's card list (fe_settings.cpp HasCardExtension, cut out by test-mcdnoecc.sh).
//   ps5/coreorbis/tests/mcdnoecc/test-mcdnoecc.sh
//
// SPDX-License-Identifier: GPL-3.0+

#include "OrbisMcdNoEcc.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <sys/stat.h>
#include <utime.h>
#include <vector>

#include "fe_cards_cut.h" // fe::HasCardExtension as fe_settings.cpp has it (test-mcdnoecc.sh cuts it out)

static int fails = 0;
static void Check(bool ok, const std::string& what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
	fails += !ok;
}

static void Put(const std::string& path, const std::vector<uint8_t>& data, long long mtime)
{
	std::FILE* f = std::fopen(path.c_str(), "wb");
	if (f)
	{
		std::fwrite(data.data(), 1, data.size(), f);
		std::fclose(f);
	}
	utimbuf t = {static_cast<time_t>(mtime), static_cast<time_t>(mtime)};
	utime(path.c_str(), &t);
}

static std::vector<uint8_t> Get(const std::string& path)
{
	std::vector<uint8_t> v;
	mcd_noecc::ReadAll(path, v);
	return v;
}

static bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}

static std::vector<uint8_t> RandomCard(size_t bytes, unsigned seed)
{
	std::mt19937 rng(seed);
	std::vector<uint8_t> v(bytes);
	for (uint8_t& b : v)
		b = static_cast<uint8_t>(rng());
	return v;
}

int main(int argc, char** argv)
{
	using namespace mcd_noecc;
	const std::string dir = argc > 1 ? argv[1] : "/tmp/mcdnoecc-test";
	std::system(("rm -rf '" + dir + "' && mkdir -p '" + dir + "'").c_str());

	// 1. names, any case; the file keeps its name
	Check(IsNoEccName("Card.mc2") && IsNoEccName("Card.MC2") && IsNoEccName("Save.Bin") && IsNoEccName("/mnt/usb0/PS2/MemCard PRO2.Mc2"),
		"core: .mc2 / .bin in any case are no-ECC cards");
	Check(!IsNoEccName("Card.ps2") && !IsNoEccName("Card.mc2x") && !IsNoEccName("Card.mc2.bak") && !IsNoEccName("mc2"),
		"core: .ps2, the RAW copy (.mc2x) and the backup (.mc2.bak) are not");
	Check(IsCardFileName("A.PS2") && IsCardFileName("b.MCR") && IsCardFileName("c.Mcd") && IsCardFileName("d.BIN") && IsCardFileName("e.MC2") &&
			  !IsCardFileName("f.mc2x") && !IsCardFileName("g.txt"),
		"core: the card list takes .ps2 .mcr .mcd .bin .mc2 in any case");
	Check(fe::HasCardExtension("Card.MC2") && fe::HasCardExtension("Save.Bin") && fe::HasCardExtension("Mcd001.PS2") &&
			  !fe::HasCardExtension("Card.mc2x") && !fe::HasCardExtension(".mc2") && !fe::HasCardExtension("notes.txt"),
		"frontend: HasCardExtension in any case too (not the RAW copy, not a bare extension)");

	// 2. ECC: known values (mymc's algorithm), and the RAW page layout PCSX2's ConvertNoECCtoRAW writes
	{
		uint8_t zero[128] = {}, one[128] = {};
		one[5] = 0x01;
		Check(CalculateECC(zero) == 0x7F7F77u, "ECC of 128 zero bytes is 77 7F 7F");
		Check(CalculateECC(one) == 0x7A8570u, "ECC with one bit set (byte 5) is 70 85 7A");
	}
	{
		const std::vector<uint8_t> card = RandomCard(8u << 20, 1);
		const std::vector<uint8_t> raw = RawFromNoEcc(card.data(), card.size());
		Check(raw.size() == 8650752u, "an 8 MiB card is 8,650,752 bytes RAW (16,384 pages of 528)");
		bool layout = true;
		for (size_t p = 0; p < 16384 && layout; p += 997)
		{
			const uint8_t* o = &raw[p * kRawPage];
			layout = std::memcmp(o, &card[p * kPage], kPage) == 0 && o[524] == 0 && o[525] == 0 && o[526] == 0 && o[527] == 0;
			for (int j = 0; j < 4 && layout; j++)
			{
				const uint32_t e = CalculateECC(&card[p * kPage + j * 128]);
				layout = o[512 + j * 3] == (e & 0xff) && o[513 + j * 3] == ((e >> 8) & 0xff) && o[514 + j * 3] == ((e >> 16) & 0xff);
			}
		}
		Check(layout, "a RAW page: the 512 bytes, 3 ECC bytes per 128-byte quarter, 4 zero bytes");
		Check(NoEccFromRaw(raw.data(), raw.size()) == card, "round trip: card -> RAW -> card is the same 8 MiB");
		const std::vector<uint8_t> big = RandomCard(64u << 20, 2);
		const std::vector<uint8_t> big_raw = RawFromNoEcc(big.data(), big.size());
		Check(big_raw.size() == 69206016u && NoEccFromRaw(big_raw.data(), big_raw.size()) == big, "round trip of a 64 MiB card");
		Check(NoEccFromRaw(raw.data(), raw.size() - 100).size() == (16383u * kPage), "a cut-off last RAW page is left out (as before)");
	}

	// 3. which leftover goes back into the card
	{
		const long long card8 = 8ll << 20, raw8 = 8650752;
		Check(DecideLeftover(card8, 1000, raw8, 1005) == Leftover::Recover, "a RAW image of this card, newer than it: put back");
		Check(DecideLeftover(card8, 1000, raw8, 1000) == Leftover::NotNewer, "the same second as the card (written back after it): not used");
		Check(DecideLeftover(card8, 1000, raw8, 990) == Leftover::NotNewer, "older than the card: not used");
		Check(DecideLeftover(card8, 1000, raw8 - 100, 1005) == Leftover::BadSize, "not whole RAW pages: not used");
		Check(DecideLeftover(card8, 1000, raw8 * 2, 1005) == Leftover::BadSize, "a RAW image of another card size: not used");
		Check(DecideLeftover(0, 0, raw8, 1005) == Leftover::Recover, "no card: a RAW image of an 8 MiB card goes back");
		Check(DecideLeftover(0, 0, 528 * 1000, 1005) == Leftover::BadSize, "no card: not a card size: not used");
		Check(DecideLeftover(card8, 1000, 0, 1005) == Leftover::BadSize, "an empty leftover: not used");
	}

	// 4. the durable write
	{
		const std::string p = dir + "/Card.mc2";
		std::string err;
		const std::vector<uint8_t> a = RandomCard(8u << 20, 3), b = RandomCard(8u << 20, 4);
		Check(WriteDurable(p, a, {}, &err) && Get(p) == a && !Exists(p + ".tmp"), "a new card written whole, no .tmp left");
		Check(WriteDurable(p, b, p + ".bak", &err) && Get(p) == b && Get(p + ".bak") == a && !Exists(p + ".tmp"),
			"written over: the new card in place, the old one kept as .bak");
		err.clear();
		Check(!WriteDurable(dir + "/no-such-folder/Card.mc2", a, {}, &err) && !err.empty(), "a folder that isn't there: false, and why (" + err + ")");
		Check(Get(p) == b, "... the card that was there is untouched");
	}

	// 5. the open's recovery, as MemoryCardFile.cpp RecoverNoEccLeftover does it with these pieces: a killed session's newer .mc2x goes back,
	// the old card kept; an older one is left alone
	{
		const std::string card = dir + "/MemCard PRO2.MC2", raw = card + "x";
		const std::vector<uint8_t> before = RandomCard(8u << 20, 5), saved = RandomCard(8u << 20, 6);
		Put(card, before, 2000);
		const std::vector<uint8_t> raw_image = RawFromNoEcc(saved.data(), saved.size());
		Put(raw, raw_image, 2100);
		struct stat cs = {}, rs = {};
		stat(card.c_str(), &cs);
		stat(raw.c_str(), &rs);
		const Leftover what = DecideLeftover(cs.st_size, cs.st_mtime, rs.st_size, rs.st_mtime);
		Check(what == Leftover::Recover, "a newer .mc2x of a killed session: put back");
		std::string err;
		const bool ok = what == Leftover::Recover && WriteDurable(card, NoEccFromRaw(raw_image.data(), raw_image.size()), card + ".bak", &err);
		Check(ok && Get(card) == saved && Get(card + ".bak") == before, "the card has the session's saves, the card as it was is the .bak");
		Put(raw, raw_image, 1500);
		stat(card.c_str(), &cs);
		stat(raw.c_str(), &rs);
		Check(DecideLeftover(cs.st_size, cs.st_mtime, rs.st_size, rs.st_mtime) == Leftover::NotNewer, "an .mc2x older than the card: left alone");
	}

	std::system(("rm -rf '" + dir + "'").c_str());
	std::printf("%s\n", fails ? "SOME FAILED" : "all passed");
	return fails ? 1 : 0;
}
