// PS5 port frontend: tests for the HD texture pack downloads (fe_texpacks.cpp), on a PC (2026-10-05, AI-assisted).
// test-texpacks.sh builds it with the vendored libarchive and makes the zip fixtures with Python. A fake platform serves
// the "archive.org" list and files from a folder; the manager's worker runs as on the console.
//
//   texpacks_test <fixtures dir> <work dir> [<a real pack .rar> <its serial>]
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_settings.h"
#include "fe_texpacks.h"

#include "OrbisTexturePak.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace fe;
using State = TexturePackStatus::State;

static int g_failures = 0;
#define CHECK(cond)                                                                       \
	do                                                                                    \
	{                                                                                     \
		if (!(cond))                                                                      \
		{                                                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
			g_failures++;                                                                 \
		}                                                                                 \
	} while (0)

static std::string ReadAll(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static bool Exists(const std::string& p)
{
	struct stat st;
	return stat(p.c_str(), &st) == 0;
}

static double Now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// pr9l: a pack file's entries, name -> bytes, read as GSTextureReplacements.cpp reads them (a later entry of one name wins).
static bool ReadPak(const std::string& path, std::map<std::string, std::string>& out, size_t* count = nullptr)
{
	out.clear();
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return false;
	std::vector<OrbisTexturePak::Entry> entries;
	std::string error;
	bool ok = OrbisTexturePak::ReadIndex(fd, entries, error);
	if (!ok)
		std::printf("  (%s: %s)\n", path.c_str(), error.c_str());
	for (const OrbisTexturePak::Entry& e : entries)
	{
		std::string data(static_cast<size_t>(e.size), '\0');
		if (e.size > 0 && !OrbisTexturePak::PreadAll(fd, data.data(), data.size(), e.offset))
			ok = false;
		out[e.name] = std::move(data);
	}
	if (count)
		*count = entries.size();
	close(fd);
	return ok;
}

// Folders set aside for deleting (.ps5sx2-old-*) still in `textures`.
static int SetAsideLeft(const std::string& textures)
{
	int n = 0;
	if (DIR* d = opendir(textures.c_str()))
	{
		while (dirent* e = readdir(d))
			n += std::strncmp(e->d_name, ".ps5sx2-old-", 12) == 0;
		closedir(d);
	}
	return n;
}

static bool WaitNoSetAside(const std::string& textures, double seconds)
{
	const double until = Now() + seconds;
	while (Now() < until)
	{
		if (SetAsideLeft(textures) == 0)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return false;
}

// pr9l: the pack file's format on its own (pcsx2/OrbisTexturePak.h): written by hand, read back, and refused when damaged.
static void TestPakFormat(const std::string& dir)
{
	using namespace OrbisTexturePak;
	CHECK(NameIsSane("a.dds") && NameIsSane("01/b.png") && NameIsSane("x/y/z.png"));
	CHECK(!NameIsSane("") && !NameIsSane("/a.png") && !NameIsSane("a/") && !NameIsSane("a//b") && !NameIsSane("../a") &&
		  !NameIsSane("a/./b") && !NameIsSane("a\\b") && !NameIsSane(std::string("a\x01b")) && !NameIsSane(std::string(1025, 'a')));
	const std::vector<std::pair<std::string, std::string>> files = {{"a.dds", "first"}, {"01/b.png", ""}, {"01/c.png", "third one"}};
	std::vector<uint8_t> body, index;
	uint64_t offset = kHeaderSize;
	for (const auto& [name, data] : files)
	{
		Entry e;
		e.offset = offset;
		e.size = data.size();
		e.name = name;
		AppendIndexEntry(index, e);
		body.insert(body.end(), data.begin(), data.end());
		offset += data.size();
	}
	const auto write = [&](const std::string& path, const std::vector<uint8_t>& header, const std::vector<uint8_t>& idx, size_t cut = 0) {
		std::vector<uint8_t> all = header;
		all.insert(all.end(), body.begin(), body.end());
		all.insert(all.end(), idx.begin(), idx.end());
		all.resize(all.size() - cut);
		std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(all.data()), static_cast<std::streamsize>(all.size()));
	};
	const auto read = [](const std::string& path, std::vector<Entry>& got, std::string& error) {
		const int fd = open(path.c_str(), O_RDONLY);
		const bool ok = fd >= 0 && ReadIndex(fd, got, error);
		if (fd >= 0)
			close(fd);
		return ok;
	};
	const std::vector<uint8_t> header = MakeHeader(3, offset, index.size());
	CHECK(header.size() == kHeaderSize);
	std::vector<Entry> got;
	std::string error;
	write(dir + "/good.pak", header, index);
	CHECK(read(dir + "/good.pak", got, error));
	CHECK(got.size() == 3 && got[0].name == "a.dds" && got[1].size == 0 && got[2].name == "01/c.png" && got[2].offset == kHeaderSize + 5);
	std::map<std::string, std::string> pak;
	CHECK(ReadPak(dir + "/good.pak", pak) && pak["01/c.png"] == "third one" && pak["a.dds"] == "first" && pak.count("01/b.png"));
	// Not finished (the header still zeros), cut short, a bad name, an entry past the data, another version.
	write(dir + "/zeros.pak", std::vector<uint8_t>(kHeaderSize, 0), index);
	CHECK(!read(dir + "/zeros.pak", got, error) && error.find("not a PS5SX2") != std::string::npos);
	write(dir + "/cut.pak", header, index, 3);
	CHECK(!read(dir + "/cut.pak", got, error));
	std::vector<uint8_t> bad_name = index;
	bad_name[18] = '/';
	write(dir + "/badname.pak", header, bad_name);
	CHECK(!read(dir + "/badname.pak", got, error) && error.find("bad name") != std::string::npos);
	std::vector<uint8_t> far = index;
	far[8] = 0xff; // a.dds's size
	write(dir + "/far.pak", header, far);
	CHECK(!read(dir + "/far.pak", got, error) && error.find("outside the data") != std::string::npos);
	std::vector<uint8_t> v2 = header;
	v2[8] = 2;
	write(dir + "/v2.pak", v2, index);
	CHECK(!read(dir + "/v2.pak", got, error) && error.find("newer") != std::string::npos);
	std::vector<uint8_t> more = MakeHeader(4, offset, index.size()); // one entry more than the index holds
	write(dir + "/count.pak", more, index);
	CHECK(!read(dir + "/count.pak", got, error));
	std::vector<uint8_t> extra = index;
	extra.push_back(0);
	write(dir + "/extra.pak", MakeHeader(3, offset, extra.size()), extra);
	CHECK(!read(dir + "/extra.pak", got, error) && error.find("left over") != std::string::npos);
}

// The "archive.org" of the tests: the list is `list`, a file is `dir`/<its name>, served in blocks, slowly if asked.
struct FakeArchive
{
	std::string dir, list;
	std::atomic<int> requests{0};
	std::atomic<uint64_t> first_offset{UINT64_MAX};
	std::atomic<bool> aborted{false};
	int block_delay_ms = 0;
	int fail_first = 0; // that many range requests answer 503 first

	TexturePackPlatform Platform(std::vector<std::string>* log, std::vector<std::string>* popups)
	{
		TexturePackPlatform p;
		p.get_text = [this](const std::string& url, std::string& body) {
			if (url != TexturePackMetadataUrl())
				return 404;
			body = list;
			return list.empty() ? -1 : 200;
		};
		p.get_range = [this](const std::string& url, uint64_t offset, uint64_t length, const std::function<bool(const void*, size_t)>& sink) {
			const std::string prefix = std::string("https://archive.org/download/") + kTexturePackItem + "/";
			if (url.compare(0, prefix.size(), prefix) != 0)
				return 404;
			uint64_t expected = UINT64_MAX;
			first_offset.compare_exchange_strong(expected, offset);
			if (requests.fetch_add(1) < fail_first)
				return 503;
			aborted = false;
			const std::string path = dir + "/" + PercentDecode(url.substr(prefix.size()));
			FILE* f = std::fopen(path.c_str(), "rb");
			if (!f)
				return 404;
			std::fseek(f, static_cast<long>(offset), SEEK_SET);
			std::vector<char> buf(64 * 1024);
			uint64_t left = length;
			while (left > 0)
			{
				const size_t n = std::fread(buf.data(), 1, static_cast<size_t>(std::min<uint64_t>(left, buf.size())), f);
				if (n == 0)
					break;
				if (block_delay_ms > 0)
					std::this_thread::sleep_for(std::chrono::milliseconds(block_delay_ms));
				if (aborted || !sink(buf.data(), n))
				{
					std::fclose(f);
					return -2;
				}
				left -= n;
			}
			std::fclose(f);
			return 206;
		};
		p.abort = [this] { aborted = true; };
		p.free_bytes = [](const std::string&) { return UINT64_MAX; };
		p.log = [log](const std::string& line) {
			std::printf("%s\n", line.c_str());
			if (log)
				log->push_back(line);
		};
		p.notify = [popups](const std::string& game, bool ok, const std::string& detail) {
			if (popups)
				popups->push_back(std::string(ok ? "ready: " : "failed: ") + game + " " + detail);
		};
		return p;
	}
};

static std::string Entry(const std::string& name, uint64_t size, const std::string& md5, const char* source = "original")
{
	return "{\"name\":\"" + name + "\",\"source\":\"" + source + "\",\"size\":\"" + std::to_string(size) + "\",\"md5\":\"" + md5 +
	       "\",\"format\":\"RAR\",\"filecount\":\"3\"}";
}

static bool WaitFor(TexturePackManager& m, const std::string& serial, State want, double seconds)
{
	const double until = Now() + seconds;
	while (Now() < until)
	{
		if (m.Status(serial).state == want)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	std::printf("  (waited for state %d, have %d: %s)\n", static_cast<int>(want), static_cast<int>(m.Status(serial).state),
		m.Status(serial).message.c_str());
	return false;
}

static void TestNames()
{
	CHECK(NormalSerial("slus-20776") == "SLUS-20776");
	CHECK(NormalSerial("SLUS-2077") == "");
	CHECK(NormalSerial("SL1S-20776") == "");
	CHECK(PercentEncode("God of War (USA) [SCUS-97399] HD Remaster.rar") ==
		  "God%20of%20War%20%28USA%29%20%5BSCUS-97399%5D%20HD%20Remaster.rar");
	CHECK(PercentDecode(PercentEncode("Beyond Good & Evil 'x' #1.rar")) == "Beyond Good & Evil 'x' #1.rar");
	CHECK(PercentDecode("100%") == "100%");
	CHECK(TexturePackMd5("", 0) == "d41d8cd98f00b204e9800998ecf8427e");
	CHECK(TexturePackMd5("abc", 3) == "900150983cd24fb0d6963f7d28e17f72");
	const std::string fox = "The quick brown fox jumps over the lazy dog";
	CHECK(TexturePackMd5(fox.data(), fox.size()) == "9e107d9d372bb6826bd81d3542a419d6");
	std::string big(1000003, 'x');
	CHECK(TexturePackMd5(big.data(), big.size()).size() == 32);
	CHECK(FormatBytes(1249000000) == "1.2 GB");
	CHECK(FormatBytes(356300000) == "356 MB");

	const std::string s = "SLUS-20776";
	CHECK(TexturePackTarget("Spider-Man 2 (USA) [SLUS-20776] HD Remaster/SLUS-20776/replacements/01/a.png", s) ==
		  "SLUS-20776/replacements/01/a.png");
	CHECK(TexturePackTarget("SLUS-20776/replacements/a.dds", s) == "SLUS-20776/replacements/a.dds");
	CHECK(TexturePackTarget("pack/slus-20776/replacements/a.dds", s) == "SLUS-20776/replacements/a.dds");
	CHECK(TexturePackTarget("pack/SLUS-20776/dumps/a.png", s) == "");
	CHECK(TexturePackTarget("pack/replacements/a.png", s) == "SLUS-20776/replacements/a.png");
	CHECK(TexturePackTarget("pack/SLUS-21362/replacements/b.png", "SLUS-21180") == "SLUS-21362/replacements/b.png");
	CHECK(TexturePackTarget("readme.txt", s) == "");
	CHECK(TexturePackTarget("pack/SLUS-20776", s) == "");
	CHECK(TexturePackTarget("../SLUS-20776/replacements/a.png", s) == "");
	CHECK(TexturePackTarget("pack/SLUS-20776/replacements/../../x", s) == "");
	CHECK(TexturePackTarget("/SLUS-20776/replacements/a.png", s) == "");
	CHECK(TexturePackTarget("pack/./SLUS-20776//replacements/a.png", s) == "SLUS-20776/replacements/a.png");
	CHECK(TexturePackTarget(std::string("pack/SLUS-20776/replacements/a\x01.png"), s) == "");
}

static void TestCatalog()
{
	const std::string md5(32, 'a');
	const std::string json = "{\"files\":[" + Entry("Cover.png", 100, md5) + "," +
	                         Entry("God of War (USA) [SCUS-97399] HD Remaster.rar", 1249000000, md5) + "," +
	                         Entry("God of War (USA) [SCUS-97399] HD Remaster Definitive Edition.rar", 4410000000ull, md5) + "," +
	                         Entry("Mortal Kombat Deadly Alliance (USA) [SLUS-21087] HD Remaster.rar", 10, md5) + "," +
	                         Entry("Mortal Kombat Shaolin Monks (USA) [SLUS-21087] HD Remaster.rar", 10, md5) + "," +
	                         Entry("Onimusha Dawn of Dreams (USA) (Disc 1) [SLUS-21180] & (Disc 2) [SLUS-21362] HD Remaster.rar", 13, md5) +
	                         "," + Entry("PlayStation 2 BIOS (USA) HD Remaster.rar", 65, md5) + "," +
	                         Entry("Forgotten Realms Demon Stone [SLUS-20804] HD Remaster.rar", 5, md5) + "," +
	                         Entry("Derived (USA) [SLUS-11111] HD Remaster.rar", 5, md5, "derivative") + "," +
	                         Entry("No Sum (USA) [SLUS-22222] HD Remaster.rar", 5, "") + "]}";
	std::vector<TexturePack> packs;
	std::string error;
	CHECK(ParseTexturePackCatalog(json, packs, error));
	CHECK(packs.size() == 6);
	if (packs.size() == 6)
	{
		CHECK(packs[0].serials.size() == 1 && packs[0].serials[0] == "SCUS-97399");
		CHECK(packs[0].label == "HD Remaster");
		CHECK(packs[1].label == "HD Remaster Definitive Edition");
		CHECK(packs[1].bytes == 4410000000ull);
		CHECK(packs[0].title == "God of War (USA) HD Remaster");
		CHECK(packs[4].serials.size() == 2 && packs[4].serials[1] == "SLUS-21362");
		CHECK(packs[5].serials.size() == 1 && packs[5].serials[0] == "SLUS-20804");
		CHECK(packs[0].files == 3);
	}
	CHECK(!ParseTexturePackCatalog("{\"error\":\"not found\"}", packs, error));
	CHECK(!ParseTexturePackCatalog("not json", packs, error));
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf("usage: texpacks_test <fixtures dir> <work dir> [<real .rar> <serial>]\n");
		return 2;
	}
	const std::string fixtures = argv[1], work = argv[2];
	TestNames();
	TestCatalog();
	mkdir(work.c_str(), 0777);
	mkdir((work + "/pakformat").c_str(), 0777);
	TestPakFormat(work + "/pakformat");

	// The fixtures (test-texpacks.sh): good.zip (Pack/SLUS-20776/replacements/a.dds, .../01/b.png, Pack/SLUS-20776/dumps/c.png,
	// readme.txt), a two-disc pack, and their MD5s in md5.txt.
	const std::string good = "Spider-Man 2 (USA) [SLUS-20776] HD Remaster.zip";
	const std::string disc = "Onimusha (USA) (Disc 1) [SLUS-21180] & (Disc 2) [SLUS-21362] HD Remaster.zip";
	const std::string many = "Many Files (USA) [SLUS-11111] HD Remaster.zip", clash = "Clash (USA) [SLUS-22222] HD Remaster.zip";
	std::string good_md5, disc_md5, many_md5, clash_md5;
	{
		std::istringstream md5s(ReadAll(fixtures + "/md5.txt"));
		md5s >> good_md5 >> disc_md5 >> many_md5 >> clash_md5;
	}
	const uint64_t many_size = ReadAll(fixtures + "/" + many).size(), clash_size = ReadAll(fixtures + "/" + clash).size();
	const uint64_t good_size = ReadAll(fixtures + "/" + good).size(), disc_size = ReadAll(fixtures + "/" + disc).size();
	CHECK(good_size > 0 && disc_size > 0 && good_md5.size() == 32);
	CHECK(TexturePackMd5(ReadAll(fixtures + "/" + good).data(), good_size) == good_md5);

	FakeArchive server;
	server.dir = fixtures;
	server.list = "{\"files\":[" + Entry(good, good_size, good_md5) + "," + Entry(disc, disc_size, disc_md5) + "," +
	              Entry("Broken (USA) [SLUS-99999] HD Remaster.zip", good_size, std::string(32, 'b')) + "," +
	              Entry(many, many_size, many_md5) + "," + Entry(clash, clash_size, clash_md5) + "]}";
	// The broken entry's file: a copy of good.zip under that name, whose MD5 doesn't match the list's.
	{
		std::ofstream(fixtures + "/Broken (USA) [SLUS-99999] HD Remaster.zip", std::ios::binary) << ReadAll(fixtures + "/" + good);
	}
	const std::string textures = work + "/textures", downloads = textures + "/.ps5sx2-downloads", cache = work + "/cache/texture-packs.json";
	const std::string settings_path = work + "/settings/Spider-Man 2.ini";
	mkdir((work + "/settings").c_str(), 0777);

	// 1. A pack comes in: the files under replacements/, not dumps/ or the readme; the marker; Texture replacements on.
	{
		std::vector<std::string> log, popups;
		server.fail_first = 1; // one 503 first: the download tries again
		TexturePackManager m(server.Platform(&log, &popups), textures, downloads, cache);
		CHECK(m.Status("SLUS-20776").state == State::Loading);
		m.Start();
		CHECK(WaitFor(m, "SLUS-20776", State::Available, 5));
		CHECK(m.Status("SLUS-12345").state == State::None);
		TexturePackStatus st = m.Status("slus-20776");
		CHECK(st.packs.size() == 1 && st.packs[0].name == good);
		CHECK(m.Begin("SLUS-20776", 0, settings_path, "# Spider-Man 2 (SLUS-20776)", "Spider-Man 2"));
		CHECK(!m.Begin("SLUS-20776", 0, settings_path, "", "Spider-Man 2")); // already on its way
		CHECK(WaitFor(m, "SLUS-20776", State::Installed, 20));
		st = m.Status("SLUS-20776");
		CHECK(st.ours && st.installed == good);
		// pr9l: the replacements in one file; the archive's own "replacements.pak" left out.
		std::map<std::string, std::string> pak;
		size_t entries = 0;
		CHECK(ReadPak(textures + "/SLUS-20776/replacements.pak", pak, &entries));
		CHECK(entries == 3 && pak.size() == 2); // 01/b.png twice: the later wins
		CHECK(pak["a.dds"].size() == 3u * 1024 * 1024);
		CHECK(pak["01/b.png"] == "the second texture, again");
		CHECK(!Exists(textures + "/SLUS-20776/replacements"));
		CHECK(!Exists(textures + "/SLUS-20776/dumps"));
		CHECK(!Exists(textures + "/readme.txt"));
		CHECK(ReadAll(textures + "/SLUS-20776/" + TexturePackManager::kMarker).find("name=" + good) != std::string::npos);
		CHECK(!Exists(textures + "/.ps5sx2-unpack"));
		CHECK(!Exists(downloads + "/SLUS-20776.job") && !Exists(downloads + "/" + good_md5 + ".zip"));
		const std::string ini = ReadAll(settings_path);
		CHECK(ini.find("LoadTextureReplacements=true") != std::string::npos);
		CHECK(ini.find("# Spider-Man 2 (SLUS-20776)") != std::string::npos);
		CHECK(popups.size() == 1 && popups[0].find("ready: Spider-Man 2") == 0);
		CHECK(Exists(cache));
		CHECK(!m.Activity().active);
		// 2. Triangle twice: removed.
		CHECK(m.Remove("SLUS-20776"));
		CHECK(WaitFor(m, "SLUS-20776", State::Available, 5));
		CHECK(!Exists(textures + "/SLUS-20776"));
		CHECK(WaitNoSetAside(textures, 5)); // pr9l: renamed aside, then deleted
		CHECK(m.Stop(2000));
	}
	// 3. A two-disc pack: both serials' folders.
	{
		TexturePackManager m(server.Platform(nullptr, nullptr), textures, downloads, cache);
		m.Start();
		CHECK(WaitFor(m, "SLUS-21362", State::Available, 5));
		CHECK(m.Begin("SLUS-21362", 0, "", "", "Onimusha"));
		CHECK(WaitFor(m, "SLUS-21362", State::Installed, 20));
		std::map<std::string, std::string> pak;
		CHECK(ReadPak(textures + "/SLUS-21180/replacements.pak", pak) && pak.size() == 1 && pak["d1.png"] == "disc one");
		CHECK(ReadPak(textures + "/SLUS-21362/replacements.pak", pak) && pak.size() == 1 && pak["d2.png"] == "disc two");
		CHECK(m.Stop(2000));
	}
	// 4. A download that doesn't match archive.org's MD5: failed, nothing kept, nothing installed.
	{
		std::vector<std::string> popups;
		TexturePackManager m(server.Platform(nullptr, &popups), textures, downloads, cache);
		m.Start();
		CHECK(WaitFor(m, "SLUS-99999", State::Available, 5));
		CHECK(m.Begin("SLUS-99999", 0, "", "", "Broken"));
		CHECK(WaitFor(m, "SLUS-99999", State::Failed, 20));
		CHECK(m.Status("SLUS-99999").message.find("checksum") != std::string::npos);
		CHECK(!Exists(downloads + "/" + std::string(32, 'b') + ".part"));
		CHECK(!Exists(textures + "/SLUS-99999"));
		CHECK(m.Cancel("SLUS-99999")); // Triangle twice: forgotten
		CHECK(m.Status("SLUS-99999").state == State::Available);
		CHECK(!Exists(downloads + "/SLUS-99999.job"));
		CHECK(m.Stop(2000));
	}
	// 5. Stopped half way (the shelf closed for a game), then picked up where it stopped by the next start.
	{
		server.block_delay_ms = 30;
		server.first_offset = UINT64_MAX;
		{
			TexturePackManager m(server.Platform(nullptr, nullptr), textures, downloads, cache);
			m.Start();
			CHECK(WaitFor(m, "SLUS-20776", State::Available, 5));
			CHECK(m.Begin("SLUS-20776", 0, settings_path, "", "Spider-Man 2"));
			const double until = Now() + 10;
			while (Now() < until && m.Status("SLUS-20776").done < good_size / 3)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			CHECK(m.Activity().active && m.Activity().title == "Spider-Man 2");
			CHECK(m.Stop(3000));
		}
		const std::string part = downloads + "/" + good_md5 + ".part";
		struct stat st{};
		CHECK(stat(part.c_str(), &st) == 0 && st.st_size > 0 && static_cast<uint64_t>(st.st_size) < good_size);
		CHECK(Exists(downloads + "/SLUS-20776.job"));
		const uint64_t kept = static_cast<uint64_t>(st.st_size);
		server.block_delay_ms = 0;
		server.first_offset = UINT64_MAX;
		TexturePackManager m(server.Platform(nullptr, nullptr), textures, downloads, cache);
		m.Start();
		CHECK(WaitFor(m, "SLUS-20776", State::Installed, 20));
		CHECK(server.first_offset == kept); // the next start asked for the rest only
		std::map<std::string, std::string> pak;
		CHECK(ReadPak(textures + "/SLUS-20776/replacements.pak", pak) && pak.count("a.dds"));
		CHECK(m.Remove("SLUS-20776"));
		CHECK(WaitFor(m, "SLUS-20776", State::Available, 5));
		CHECK(m.Stop(2000));
	}
	// 6. Cancelled while downloading: its files go.
	{
		server.block_delay_ms = 30;
		TexturePackManager m(server.Platform(nullptr, nullptr), textures, downloads, cache);
		m.Start();
		CHECK(WaitFor(m, "SLUS-20776", State::Available, 5));
		CHECK(m.Begin("SLUS-20776", 0, settings_path, "", "Spider-Man 2"));
		CHECK(WaitFor(m, "SLUS-20776", State::Downloading, 5));
		CHECK(m.Cancel("SLUS-20776"));
		CHECK(WaitFor(m, "SLUS-20776", State::Available, 10));
		CHECK(!Exists(downloads + "/" + good_md5 + ".part") && !Exists(downloads + "/SLUS-20776.job"));
		CHECK(m.Stop(2000));
		server.block_delay_ms = 0;
	}
	// 7. No network and no list kept: Unavailable; with the list kept, it's used.
	{
		FakeArchive offline;
		offline.dir = fixtures;
		const std::string other_cache = work + "/cache/none.json";
		TexturePackManager m(offline.Platform(nullptr, nullptr), textures, downloads, other_cache);
		m.Start();
		CHECK(WaitFor(m, "SLUS-20776", State::Unavailable, 5));
		CHECK(!m.Begin("SLUS-20776", 0, "", "", "x"));
		CHECK(m.Stop(2000));
		TexturePackManager cached(offline.Platform(nullptr, nullptr), textures, downloads, cache);
		cached.Start();
		CHECK(WaitFor(cached, "SLUS-20776", State::Available, 5));
		CHECK(cached.Stop(2000));
	}
	// 8. 3,004 files over 31 folders, four of them 5 MB: all in the one pack file, whole.
	{
		std::vector<std::string> log;
		TexturePackManager m(server.Platform(&log, nullptr), textures, downloads, cache);
		m.Start();
		CHECK(WaitFor(m, "SLUS-11111", State::Available, 5));
		const double t0 = Now();
		CHECK(m.Begin("SLUS-11111", 0, "", "", "Many"));
		CHECK(WaitFor(m, "SLUS-11111", State::Installed, 60));
		std::printf("  3,004 files installed in %.2f s\n", Now() - t0);
		std::map<std::string, std::string> pak;
		CHECK(ReadPak(textures + "/SLUS-11111/replacements.pak", pak) && pak.size() == 3004);
		size_t found = 0, right = 0;
		for (int i = 0; i < 3000; i++)
		{
			char rel[96];
			std::snprintf(rel, sizeof(rel), "%02d/t%04d.png", i % 30, i);
			const auto it = pak.find(rel);
			std::string want;
			for (int k = 0; k < 1 + i % 50; k++)
				want += "texture " + std::to_string(i) + " ";
			found += it != pak.end();
			right += it != pak.end() && it->second == want;
		}
		CHECK(found == 3000 && right == 3000);
		for (int i = 0; i < 4; i++)
			CHECK(pak["big/b" + std::to_string(i) + ".dds"].size() == 5u * 1024 * 1024);
		bool counted = false;
		for (const std::string& l : log)
			counted = counted || l.find("unpacked 3004 files") != std::string::npos;
		CHECK(counted);
		CHECK(m.Remove("SLUS-11111"));
		CHECK(WaitFor(m, "SLUS-11111", State::Available, 10));
		CHECK(!Exists(textures + "/SLUS-11111"));
		// 9. A file outside replacements/ where a folder already is: the write fails, the pack is reported failed and nothing
		// stays behind.
		CHECK(WaitFor(m, "SLUS-22222", State::Available, 5));
		CHECK(m.Begin("SLUS-22222", 0, "", "", "Clash"));
		CHECK(WaitFor(m, "SLUS-22222", State::Failed, 20));
		CHECK(m.Status("SLUS-22222").message.find("Couldn't write SLUS-22222/notes") != std::string::npos);
		CHECK(!Exists(textures + "/SLUS-22222") && !Exists(textures + "/.ps5sx2-unpack"));
		CHECK(m.Stop(2000));
	}
	// 10. pr9h's leftovers in the folder above (no textures folder then): moved into the textures folder at the start.
	{
		const std::string root = work + "/legacy", tex = root + "/textures";
		mkdir(root.c_str(), 0777);
		mkdir((root + "/.ps5sx2-downloads").c_str(), 0777);
		std::ofstream(root + "/.ps5sx2-downloads/SLUS-20776.job") << "name=x\n";
		mkdir((root + "/.ps5sx2-unpack").c_str(), 0777);
		mkdir((root + "/SLUS-20776").c_str(), 0777);
		std::ofstream(root + "/SLUS-20776/" + TexturePackManager::kMarker) << "name=" + good + "\n";
		mkdir((root + "/SLUS-21180").c_str(), 0777); // not ours (no marker): left alone
		mkdir((root + "/bios").c_str(), 0777);
		FakeArchive offline;
		offline.dir = fixtures;
		TexturePackManager m(offline.Platform(nullptr, nullptr), tex, tex + "/.ps5sx2-downloads", cache);
		m.Start();
		CHECK(WaitFor(m, "SLUS-20776", State::Installed, 5));
		CHECK(Exists(tex + "/SLUS-20776/" + TexturePackManager::kMarker) && !Exists(root + "/SLUS-20776"));
		CHECK(Exists(tex + "/.ps5sx2-unpack") || !Exists(root + "/.ps5sx2-unpack"));
		CHECK(!Exists(root + "/.ps5sx2-downloads"));
		CHECK(Exists(root + "/SLUS-21180") && Exists(root + "/bios") && !Exists(tex + "/SLUS-21180"));
		CHECK(m.Stop(2000));
	}
	// 12. pr9l: a pack this app installed before as a replacements/ folder (pr9k and earlier), and an unpack such a build left
	// half done in .ps5sx2-unpack: both set aside when the pack goes in again, and deleted once the worker is idle.
	{
		const std::string tex = work + "/old-install";
		mkdir(tex.c_str(), 0777);
		mkdir((tex + "/SLUS-20776").c_str(), 0777);
		mkdir((tex + "/SLUS-20776/replacements").c_str(), 0777);
		std::ofstream(tex + "/SLUS-20776/" + TexturePackManager::kMarker) << "name=" + good + "\n";
		std::ofstream(tex + "/SLUS-20776/replacements/old.dds") << "old";
		mkdir((tex + "/.ps5sx2-unpack").c_str(), 0777);
		mkdir((tex + "/.ps5sx2-unpack/SLUS-20776").c_str(), 0777);
		mkdir((tex + "/.ps5sx2-unpack/SLUS-20776/replacements").c_str(), 0777);
		for (int i = 0; i < 300; i++)
			std::ofstream(tex + "/.ps5sx2-unpack/SLUS-20776/replacements/f" + std::to_string(i) + ".dds") << i;
		std::vector<std::string> log;
		TexturePackManager m(server.Platform(&log, nullptr), tex, tex + "/.ps5sx2-downloads", work + "/cache/old-install.json");
		m.Start();
		CHECK(WaitFor(m, "SLUS-20776", State::Installed, 5)); // the old one, by its marker
		for (const double until = Now() + 5; Now() < until && m.Status("SLUS-20776").packs.empty();) // the list
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		CHECK(m.Begin("SLUS-20776", 0, "", "", "Spider-Man 2"));
		CHECK(m.Status("SLUS-20776").state != State::Installed); // on its way again
		CHECK(WaitFor(m, "SLUS-20776", State::Installed, 20));
		std::map<std::string, std::string> pak;
		CHECK(ReadPak(tex + "/SLUS-20776/replacements.pak", pak) && pak["01/b.png"] == "the second texture, again");
		CHECK(!Exists(tex + "/SLUS-20776/replacements"));
		CHECK(WaitNoSetAside(tex, 10));
		CHECK(!Exists(tex + "/.ps5sx2-unpack"));
		int set_aside = 0, deleted = 0;
		for (const std::string& l : log)
		{
			set_aside += l.find("] set aside ") != std::string::npos;
			deleted += l.find("] deleted ") != std::string::npos && l.find("files set aside") != std::string::npos;
		}
		CHECK(set_aside == 2 && deleted >= 1);
		CHECK(m.Stop(2000));
	}
	// 11. A real pack from archive.org (optional): unpacked as on the console.
	if (argc >= 5)
	{
		const std::string real = argv[3], serial = argv[4];
		const std::string data = ReadAll(real);
		const std::string name = real.substr(real.rfind('/') + 1);
		FakeArchive local;
		local.dir = real.substr(0, real.rfind('/'));
		local.list = "{\"files\":[" + Entry(name, data.size(), TexturePackMd5(data.data(), data.size())) + "]}";
		TexturePackManager m(local.Platform(nullptr, nullptr), work + "/real-textures", work + "/real-textures/.dl", work + "/cache/real.json");
		m.Start();
		CHECK(WaitFor(m, serial, State::Available, 5));
		CHECK(m.Begin(serial, 0, "", "", "real"));
		CHECK(WaitFor(m, serial, State::Installed, 120));
		std::map<std::string, std::string> pak;
		size_t entries = 0;
		CHECK(ReadPak(work + "/real-textures/" + serial + "/replacements.pak", pak, &entries) && entries > 0);
		std::printf("  %s: %zu files in the pack\n", serial.c_str(), entries);
		CHECK(m.Stop(2000));
	}

	if (g_failures)
	{
		std::printf("TEXPACKS TEST: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("TEXPACKS TEST PASS\n");
	return 0;
}
