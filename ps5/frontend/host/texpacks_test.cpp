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
#include "fe_texwrite.h"
#include "fe_texwrite_proto.h"

#include "OrbisTexturePak.h"

#include "rapidjson/document.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utime.h>
#include <vector>
#include <zlib.h>

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

// b176: the log and popup vectors the tests hand to FakeArchive::Platform are appended to by the manager's worker (after the state a test waits for has changed:
// "installed" is logged, and the popup made, a moment after the pack shows as installed) and read by main. One mutex for both, and a copy to read from
// (ThreadSanitizer: the first runs of this test reported the unsynchronized reads).
static std::mutex g_sink_mutex;
static std::vector<std::string> Snapshot(std::vector<std::string>& v)
{
	std::lock_guard<std::mutex> l(g_sink_mutex);
	return v;
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
	// 2.11: Google Drive: a file's id -> a file of `dir`; what a probe answers (206 and its size, or another status); a type for it
	std::map<std::string, std::string> drive_files;
	int probe_status = 206;
	std::string probe_type = "application/octet-stream";
	std::atomic<int> probes{0}, drive_ranges{0};
	// 2026-10-29: Drive's "Quota exceeded" page, sent (200, ~2 KB) for any range of a Drive file but a one-byte one, when set
	std::string drive_page;
	// 2.11: the project's current Drive list (https://example.test/hd-pack-sources.json in the tests)
	std::string hosted;
	int hosted_status = 404;
	std::atomic<int> hosted_requests{0};

	TexturePackPlatform Platform(std::vector<std::string>* log, std::vector<std::string>* popups)
	{
		TexturePackPlatform p;
		p.get_text = [this](const std::string& url, std::string& body) {
		if (url == "https://example.test/hd-pack-sources.json")
		{
		hosted_requests++;
		body = hosted;
		return hosted_status;
		}
			if (url != TexturePackMetadataUrl())
				return 404;
			body = list;
			return list.empty() ? -1 : 200;
		};
		p.get_range = [this](const std::string& url, uint64_t offset, uint64_t length, const std::function<bool(const void*, size_t)>& sink) {
			const std::string prefix = std::string("https://archive.org/download/") + kTexturePackItem + "/";
			const std::string drive = "https://drive.usercontent.google.com/download?id=";
			std::string served; // the file's name in `dir`
			if (url.compare(0, prefix.size(), prefix) == 0)
				served = PercentDecode(url.substr(prefix.size()));
			else if (url.compare(0, drive.size(), drive) == 0 && url.find("&export=download&confirm=t") != std::string::npos)
			{
				const auto f = drive_files.find(url.substr(drive.size(), url.find('&') - drive.size()));
				if (f == drive_files.end())
					return 404;
				served = f->second;
				drive_ranges++;
				if (!drive_page.empty() && length > 1)
				{
					sink(drive_page.data(), drive_page.size());
					return 200;
				}
			}
			else
				return 404;
			uint64_t expected = UINT64_MAX;
			first_offset.compare_exchange_strong(expected, offset);
			if (requests.fetch_add(1) < fail_first)
				return 503;
			aborted = false;
			const std::string path = dir + "/" + served;
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
		p.probe = [this](const std::string& url, uint64_t& total, std::string& type) {
			probes++;
			const std::string drive = "https://drive.usercontent.google.com/download?id=";
			if (url.compare(0, drive.size(), drive) != 0)
				return 404;
			const auto f = drive_files.find(url.substr(drive.size(), url.find('&') - drive.size()));
			if (f == drive_files.end())
				return 404;
			if (probe_status != 206)
				return probe_status;
			total = ReadAll(dir + "/" + f->second).size();
			type = probe_type;
			return 206;
		};
		p.abort = [this] { aborted = true; };
		p.free_bytes = [](const std::string&) { return UINT64_MAX; };
		p.log = [log](const std::string& line) {
			std::printf("%s\n", line.c_str());
			if (log)
			{
				std::lock_guard<std::mutex> l(g_sink_mutex);
				log->push_back(line);
			}
		};
		p.notify = [popups](const std::string& game, bool ok, const std::string& detail) {
			if (popups)
			{
				std::lock_guard<std::mutex> l(g_sink_mutex);
				popups->push_back(std::string(ok ? "ready: " : "failed: ") + game + " " + detail);
			}
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

// 2.11: the Drive list (assets/hd-pack-sources.json's format): .rar and .zip kept, bad entries dropped. b176: and .7z (a reader exists now); another
// format is counted as unsupported; the optional "size" and "unpacked" are hints for the shelf's text and never the pack's size.
static void TestExtraCatalog()
{
	const std::string json =
		"{\"version\":1,\"packs\":["
		"{\"id\":\"1LhBh0YUnz3huHiOgE1on1yHjz4vhALEF\",\"name\":\"Black PS2 Upscale Textures [SLUS-21376].rar\",\"title\":\"Black\","
		"\"label\":\"CCKrizalid\",\"maker\":\"CCKrizalid\",\"serials\":[\"SLUS-21376\"]},"
		"{\"id\":\"1_LE98qWy65geHauxfW-QjlcIR6fyQF5w\",\"name\":\"007 - Everything or Nothing.7z\",\"title\":\"007\",\"label\":\"Bl4ckH4nd\","
		"\"maker\":\"Bl4ckH4nd\",\"serials\":[\"SLUS-20751\"],\"size\":78515029,\"unpacked\":\"279601824\"},"
		"{\"id\":\"1YMF3caBOex8Iub6MDwwjLNYaCYwM05lY\",\"name\":\"DMC2.zip\",\"serials\":[\"slus-20484\",\"SLUS-20627\",\"SLUS-20627\",\"junk\"]},"
		"{\"id\":\"1NoTitleSevenZipFile000\",\"name\":\"NoTitle.7Z\",\"serials\":[\"SLUS-20752\"]},"
		"{\"id\":\"1TarIsNotReadHere00000\",\"name\":\"Pack.tar.gz\",\"serials\":[\"SLUS-20753\"]},"
		"{\"id\":\"bad id/with slash\",\"name\":\"Bad.rar\",\"serials\":[\"SLUS-11111\"]},"
		"{\"id\":\"1abcdefghijklmnop\",\"name\":\"NoSerial.rar\",\"serials\":[]},"
		"{\"id\":\"1abcdefghijklmnoq\",\"name\":\"../escape.rar\",\"serials\":[\"SLUS-11112\"]}]}";
	std::vector<TexturePack> packs;
	std::string error;
	int unsupported = 0;
	CHECK(ParseExtraTexturePacks(json, packs, error, &unsupported));
	CHECK(packs.size() == 4 && unsupported == 1); // the .tar.gz is counted; the .7z are kept
	if (packs.size() == 4)
	{
		CHECK(packs[0].OnDrive() && packs[0].drive_id == "1LhBh0YUnz3huHiOgE1on1yHjz4vhALEF" && packs[0].label == "CCKrizalid" && packs[0].bytes == 0 &&
			packs[0].md5.empty() && packs[0].Key() == "g1LhBh0YUnz3huHiOgE1on1yHjz4vhALEF");
		CHECK(packs[1].name == "007 - Everything or Nothing.7z" && packs[1].serials.size() == 1 && packs[1].label == "Bl4ckH4nd" && packs[1].title == "007");
		CHECK(packs[1].hint_bytes == 78515029 && packs[1].hint_unpacked == 279601824 && packs[1].bytes == 0); // hints for the text only
		CHECK(packs[2].serials.size() == 2 && packs[2].serials[0] == "SLUS-20484" && packs[2].serials[1] == "SLUS-20627"); // normalized, once
		CHECK(packs[2].title == "DMC2" && packs[2].label == "DMC2"); // no title or label: the file's
		CHECK(packs[3].title == "NoTitle" && packs[3].label == "NoTitle" && packs[3].hint_bytes == 0); // a 7z's title loses ".7z", not a letter more
	}
	CHECK(!ParseExtraTexturePacks("not json", packs, error));
	CHECK(!ParseExtraTexturePacks("{\"version\":1}", packs, error));
	CHECK(TexturePackDriveUrl("1LhBh0YUnz3huHiOgE1") == "https://drive.usercontent.google.com/download?id=1LhBh0YUnz3huHiOgE1&export=download&confirm=t");
}

// ---- b176: the .7z packs through the manager (fe_sevenzip.c behind Unpack7z), made by make_7z_fixtures.py texpacks
namespace
{
struct Flags
{
	std::mutex m;
	std::set<std::string> on;
	bool Has(const std::string& n)
	{
		std::lock_guard<std::mutex> l(m);
		return on.count(n) > 0;
	}
	void Set(const std::string& n, bool v)
	{
		std::lock_guard<std::mutex> l(m);
		if (v)
			on.insert(n);
		else
			on.erase(n);
	}
};

struct SafeLog
{
	std::mutex m;
	std::vector<std::string> lines;
	void Add(const std::string& s)
	{
		std::lock_guard<std::mutex> l(m);
		lines.push_back(s);
	}
	bool Has(const std::string& needle)
	{
		std::lock_guard<std::mutex> l(m);
		for (const std::string& s : lines)
			if (s.find(needle) != std::string::npos)
				return true;
		return false;
	}
	std::string Find(const std::string& needle)
	{
		std::lock_guard<std::mutex> l(m);
		for (const std::string& s : lines)
			if (s.find(needle) != std::string::npos)
				return s;
		return {};
	}
	int Count(const std::string& needle)
	{
		std::lock_guard<std::mutex> l(m);
		int n = 0;
		for (const std::string& s : lines)
			if (s.find(needle) != std::string::npos)
				n++;
		return n;
	}
};

// Holds the worker where a log line says so (a line logged outside the manager's locks), so a test can look at the files, cancel or stop at that moment.
struct Pause
{
	std::mutex m;
	std::condition_variable cv;
	std::string needle;
	bool armed = false, hit = false, release = false;
	void Arm(const std::string& s)
	{
		std::lock_guard<std::mutex> l(m);
		needle = s;
		armed = true;
		hit = false;
		release = false;
	}
	void OnLog(const std::string& line)
	{
		std::unique_lock<std::mutex> l(m);
		if (!armed || line.find(needle) == std::string::npos)
			return;
		hit = true;
		cv.notify_all();
		cv.wait(l, [&] { return release; });
		armed = false;
	}
	bool WaitHit(double seconds)
	{
		std::unique_lock<std::mutex> l(m);
		return cv.wait_for(l, std::chrono::duration<double>(seconds), [&] { return hit; });
	}
	void Release()
	{
		std::lock_guard<std::mutex> l(m);
		release = true;
		cv.notify_all();
	}
};

std::string SzSource(const std::string& id, const std::string& name, const std::string& serials, const std::string& extra = std::string(),
	const std::string& maker = "Bl4ckH4nd")
{
	return "{\"id\":\"" + id + "\",\"name\":\"" + name + "\",\"title\":\"" + name.substr(0, name.rfind('.')) + "\",\"label\":\"" + maker + "\",\"maker\":\"" + maker +
	       "\",\"serials\":[" + serials + "]" + extra + "}";
}

// The place of a pack (by its file name) in the row's list for a game: -1 when it isn't there.
int PackIndex(TexturePackManager& m, const std::string& serial, const std::string& name)
{
	const TexturePackStatus st = m.Status(serial);
	for (size_t i = 0; i < st.packs.size(); i++)
		if (st.packs[i].name == name)
			return static_cast<int>(i);
	return -1;
}

// One manager over fresh folders, with the flags, the free space and memory, the log and the pause under the test's hand.
struct SzRig
{
	FakeArchive fx;
	Flags flags;
	Pause pause;
	SafeLog log;
	std::atomic<uint64_t> free_bytes{UINT64_MAX}, free_memory{UINT64_MAX};
	// holds the pack writer thread where it starts (a disk that takes its time): the unpacking thread then fills the three buffers and waits for a free one
	std::mutex hold_m;
	std::condition_variable hold_cv;
	bool hold_writer = false;
	void ReleaseWriter()
	{
		std::lock_guard<std::mutex> l(hold_m);
		hold_writer = false;
		hold_cv.notify_all();
	}
	std::string base, textures, downloads, cache;
	std::unique_ptr<TexturePackManager> m;
	std::function<bool(const std::string&, std::string&)> launch; // 2026-10-29: the writer process (fe_texwrite.h), when set

	SzRig(const std::string& fixtures, const std::string& root, const std::string& name)
	{
		fx.dir = fixtures;
		base = root + "/" + name;
		textures = base + "/textures";
		downloads = textures + "/.ps5sx2-downloads";
		cache = base + "/cache.json";
		mkdir(root.c_str(), 0777);
		mkdir(base.c_str(), 0777);
		mkdir(textures.c_str(), 0777);
	}
	TexturePackPlatform Platform()
	{
		TexturePackPlatform p = fx.Platform(nullptr, nullptr);
		p.log = [this](const std::string& line) {
			std::printf("%s\n", line.c_str());
			log.Add(line);
			pause.OnLog(line);
		};
		p.flag = [this](const char* n) { return flags.Has(n); };
		p.free_bytes = [this](const std::string&) { return free_bytes.load(); };
		p.free_memory = [this] { return free_memory.load(); };
		p.launch_writer = launch;
		p.thread_start = [this](const char* role) {
			if (std::string(role) == "pakwriter")
			{
				std::unique_lock<std::mutex> l(hold_m);
				hold_cv.wait(l, [&] { return !hold_writer; });
			}
		};
		return p;
	}
	void Open(const std::string& sources) { OpenJson("{\"version\":1,\"packs\":[" + sources + "]}"); }
	void OpenJson(const std::string& json)
	{
		m = std::make_unique<TexturePackManager>(Platform(), textures, downloads, cache);
		m->SetExtraSources(json, "");
		m->Start();
	}
	void Close()
	{
		ReleaseWriter(); // (a held pack writer would keep a stopping worker from ending)
		if (m)
			CHECK(m->Stop(10000));
		m.reset();
	}
	bool Exists(const std::string& rel) const { return ::Exists(textures + "/" + rel); }
	std::string Job(const std::string& serial) const { return downloads + "/" + serial + ".job"; }
};

// A pack of the manifest (<name under replacements/> <size> <md5>) is in the pack file, every entry whole.
void CheckAgainstManifest(const std::string& pak_path, const std::string& manifest_path, size_t expect_entries)
{
	std::map<std::string, std::string> pak;
	size_t entries = 0;
	CHECK(ReadPak(pak_path, pak, &entries) && entries == expect_entries);
	std::istringstream manifest(ReadAll(manifest_path));
	std::string name, md5;
	size_t size = 0, checked = 0;
	while (manifest >> name >> size >> md5)
	{
		const auto it = pak.find(name);
		CHECK(it != pak.end() && it->second.size() == size);
		if (it != pak.end() && it->second.size() == size && TexturePackMd5(it->second.data(), it->second.size()) == md5)
			checked++;
		else
			std::printf("  (entry %s differs)\n", name.c_str());
	}
	CHECK(checked == pak.size());
}


// ---- 2026-10-29: the texture pack writer process (fe_texwrite.h, ps5/coreorbis/tex-writer/tex_writer.c built for the PC as TEX_WRITER_BIN)

// The console's ELF loader, played by fork + exec: the payload with the request written over its marker, run as a process of its own (without
// LD_PRELOAD: the write model of texwrite_bench.sh is the title's alone). `pids` gets each one started; `mutate` may change the request first.
static std::function<bool(const std::string&, std::string&)> HostLaunch(const std::string& dir, std::vector<pid_t>* pids,
	std::function<std::string(const std::string&)> mutate = nullptr)
{
	return [dir, pids, mutate](const std::string& request_in, std::string& why) {
		const char* bin = std::getenv("TEX_WRITER_BIN");
		const std::string elf = bin ? ReadAll(bin) : std::string();
		const std::string request = mutate ? mutate(request_in) : request_in;
		const std::vector<uint8_t> patched = TexWriterPatched(reinterpret_cast<const uint8_t*>(elf.data()), elf.size(), request);
		if (patched.empty())
		{
			why = "no payload (TEX_WRITER_BIN) or no marker in it";
			return false;
		}
		static std::atomic<int> n{0};
		const std::string path = dir + "/tex_writer-" + std::to_string(n++) + ".elf";
		{
			std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(patched.data()), static_cast<std::streamsize>(patched.size()));
		}
		chmod(path.c_str(), 0755);
		const pid_t pid = fork();
		if (pid == 0)
		{
			unsetenv("LD_PRELOAD");
			execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
			_exit(127);
		}
		if (pid < 0)
		{
			why = "fork failed";
			return false;
		}
		if (pids)
			pids->push_back(pid);
		std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
		return true;
	};
}

static std::string Blob(size_t n, uint32_t seed)
{
	std::string out(n, '\0');
	uint32_t x = seed * 2654435761u + 1;
	for (size_t i = 0; i < n; i++)
	{
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		out[i] = static_cast<char>(x);
	}
	return out;
}

static void TestWriterProcess(const std::string& fixtures, const std::string& work)
{
	std::printf("---- the texture pack writer process\n");
	if (!std::getenv("TEX_WRITER_BIN"))
	{
		std::printf("  (TEX_WRITER_BIN not set: test-texpacks.sh builds it)\n");
		CHECK(false);
		return;
	}
	const std::string base = work + "/tw", root = base + "/textures";
	mkdir(base.c_str(), 0777);
	mkdir(root.c_str(), 0777);
	mkdir((root + "/sub").c_str(), 0777);
	SafeLog log;
	const auto logfn = [&log](const std::string& line) {
		std::printf("%s\n", line.c_str());
		log.Add(line);
	};
	std::vector<pid_t> pids;

	// The request over the marker: found, written, NUL-ended; one that doesn't fit or a file without the marker: nothing.
	{
		const std::string elf = ReadAll(std::getenv("TEX_WRITER_BIN"));
		const std::string req = TexWriterRequest(4242, std::string(32, 'a'), "/data/PCSX2/textures", "/data/PCSX2/logs/texwrite.log");
		const std::vector<uint8_t> out = TexWriterPatched(reinterpret_cast<const uint8_t*>(elf.data()), elf.size(), req);
		CHECK(out.size() == elf.size());
		const std::string patched(out.begin(), out.end());
		const size_t at = patched.find(req);
		CHECK(at != std::string::npos && patched[at + req.size()] == '\0' && elf.compare(0, at, patched, 0, at) == 0);
		CHECK(TexWriterPatched(reinterpret_cast<const uint8_t*>(elf.data()), elf.size(), req + std::string(TW_REQUEST_ROOM, 'x')).empty());
		CHECK(TexWriterPatched(reinterpret_cast<const uint8_t*>("no marker here"), 14, req).empty());
	}

	// 1. Files through the payload: whole, at offsets, a write bigger than one message split, mode 0666; paths it refuses; a file outside its root is the app's.
	{
		std::string why;
		std::unique_ptr<TexRemoteWriter> w = TexRemoteWriter::Start(root, base + "/tw.log", HostLaunch(base, &pids), logfn, 5000, why);
		CHECK(w && w->Usable());
		if (!w)
		{
			std::printf("  (no writer: %s)\n", why.c_str());
			return;
		}
		CHECK(w->Pid() == static_cast<uint32_t>(pids.back()));
		DIR* d = opendir(root.c_str());
		int marks = 0;
		while (dirent* e = d ? readdir(d) : nullptr)
			marks += std::string(e->d_name).rfind(".ps5sx2-texwrite-", 0) == 0;
		if (d)
			closedir(d);
		CHECK(marks == 0);
		const std::string a = Blob(3 << 20, 1), b = Blob((9 << 20) + 5, 2);
		{
			TexOutFile f;
			CHECK(f.Open(root + "/sub/a.bin", true, w.get(), false, logfn) && f.Remote());
			CHECK(f.PWrite(b.data(), b.size(), a.size())); // the later part first: the offsets are the payload's to keep
			CHECK(f.PWrite(a.data(), a.size(), 0));
			CHECK(f.Close());
			CHECK(f.RemoteBytes() == a.size() + b.size() && f.LocalBytes() == 0);
		}
		CHECK(ReadAll(root + "/sub/a.bin") == a + b);
		struct stat st = {};
		CHECK(stat((root + "/sub/a.bin").c_str(), &st) == 0 && (st.st_mode & 0777) == 0666);
		CHECK(w->BytesWritten() == a.size() + b.size());
		// kept as it is (a download picking up), then truncated (a new pack file)
		{
			TexOutFile f;
			CHECK(f.Open(root + "/sub/a.bin", false, w.get(), false, logfn) && f.Remote());
			CHECK(f.PWrite("XY", 2, 1) && f.Close());
			CHECK(ReadAll(root + "/sub/a.bin") == a.substr(0, 1) + "XY" + a.substr(3) + b);
			CHECK(f.Open(root + "/sub/a.bin", true, w.get(), false, logfn) && f.PWrite("new", 3, 0) && f.Close());
			CHECK(ReadAll(root + "/sub/a.bin") == "new");
		}
		uint32_t h = 0;
		int err = 0;
		CHECK(!w->Open(root + "/sub/../escape.bin", true, h, err) && err == EPERM && w->Usable());
		CHECK(!w->Open(root + "/./x.bin", true, h, err) && err == EPERM);
		CHECK(!w->Open(root + "//x.bin", true, h, err) && err == EPERM);
		CHECK(!w->Open(root + "/nofolder/x.bin", true, h, err) && err == ENOENT);
		CHECK(!w->Open(root + "/sub", true, h, err) && err != 0); // a folder is no file
		CHECK(!w->Open(base + "/outside.bin", true, h, err) && err == EPERM && !Exists(base + "/outside.bin"));
		// a symbolic link on the way, or as the file, is never followed out of the root
		CHECK(symlink(base.c_str(), (root + "/link").c_str()) == 0);
		CHECK(symlink((base + "/target.bin").c_str(), (root + "/filelink.bin").c_str()) == 0);
		CHECK(!w->Open(root + "/link/escape.bin", true, h, err) && err != 0 && !Exists(base + "/escape.bin"));
		CHECK(!w->Open(root + "/filelink.bin", true, h, err) && err != 0 && !Exists(base + "/target.bin"));
		CHECK(w->Usable());
		// TexOutFile: what the payload refuses or can't reach, the app writes
		{
			TexOutFile f;
			CHECK(f.Open(base + "/outside.bin", true, w.get(), false, logfn) && !f.Remote());
			CHECK(f.PWrite("own", 3, 0) && f.Close() && f.LocalBytes() == 3 && ReadAll(base + "/outside.bin") == "own");
		}
		// the disk's own refusal comes back as an error (not a fallback): a write to a file opened, then made read-only, is not the case here; an
		// unknown handle is the protocol's EBADF
		CHECK(!w->Write(77, 0, "x", 1, err) && err == EBADF && w->Usable());
		CHECK(!w->Close(77, err) && err == EBADF);

		// 2. The payload goes away in the middle (killed): the next piece and the rest are written by the app into the same file, nothing lost.
		{
			TexOutFile f;
			CHECK(f.Open(root + "/sub/b.bin", true, w.get(), false, logfn) && f.Remote());
			CHECK(f.PWrite(a.data(), 1 << 20, 0));
			kill(static_cast<pid_t>(w->Pid()), SIGKILL);
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			CHECK(f.PWrite(a.data() + (1 << 20), a.size() - (1 << 20), 1 << 20));
			CHECK(!f.Remote() && !w->Usable());
			CHECK(f.Close());
			CHECK(f.RemoteBytes() == (1u << 20) && f.LocalBytes() == a.size() - (1 << 20));
			CHECK(ReadAll(root + "/sub/b.bin") == a);
			TexOutFile g; // a new file after it: the app's from the start
			CHECK(g.Open(root + "/sub/c.bin", true, w.get(), false, logfn) && !g.Remote() && g.PWrite("c", 1, 0) && g.Close());
			CHECK(log.Has("the writer process stopped answering"));
		}
	}

	// 3. A connection with the wrong token first (another program on the loopback): closed; the payload's is taken.
	{
		std::string why;
		const auto launch = HostLaunch(base, &pids);
		std::unique_ptr<TexRemoteWriter> w = TexRemoteWriter::Start(root, "", [&](const std::string& req, std::string& y) {
			const int port = std::atoi(req.c_str() + req.find("port=") + 5);
			const int s = socket(AF_INET, SOCK_STREAM, 0);
			struct sockaddr_in addr = {};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(static_cast<uint16_t>(port));
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (connect(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0)
			{
				tw_hello bad = {};
				bad.magic = TW_HELLO_MAGIC;
				bad.version = TW_VERSION;
				bad.max_data = TW_MAX_DATA;
				send(s, &bad, sizeof(bad), 0);
			}
			std::thread([s] {
				std::this_thread::sleep_for(std::chrono::seconds(2));
				close(s);
			}).detach();
			return launch(req, y);
		}, logfn, 5000, why);
		CHECK(w && w->Usable());
		if (w)
		{
			TexOutFile f;
			CHECK(f.Open(root + "/sub/d.bin", true, w.get(), false, logfn) && f.Remote() && f.PWrite("d", 1, 0) && f.Close());
		}
	}

	// 4. A payload that never comes (the loader took it and nothing started): null within the wait, the reason said.
	{
		std::string why;
		const double t0 = Now();
		std::unique_ptr<TexRemoteWriter> w = TexRemoteWriter::Start(root, "", [](const std::string&, std::string&) { return true; }, logfn, 400, why);
		CHECK(!w && why.find("did not connect") != std::string::npos && Now() - t0 < 3);
		CHECK(!TexRemoteWriter::Start(root, "", [](const std::string&, std::string& y) { y = "no loader"; return false; }, logfn, 400, why) && why == "no loader");
		CHECK(!TexRemoteWriter::Start("relative", "", HostLaunch(base, &pids), logfn, 400, why));
	}

	// 5. A payload that sees another folder under the same name (a title's own view of /data): refused, its mark file deleted.
	{
		const std::string other = base + "/other";
		mkdir(other.c_str(), 0777);
		std::string why;
		std::unique_ptr<TexRemoteWriter> w = TexRemoteWriter::Start(root, "", HostLaunch(base, &pids, [&](const std::string& req) {
			std::string r = req;
			const size_t at = r.find("root=") + 5;
			r.replace(at, r.find('\n', at) - at, other);
			return r;
		}), logfn, 5000, why);
		CHECK(!w && why.find("sees another") != std::string::npos);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		DIR* d = opendir(other.c_str());
		int left = 0;
		while (dirent* e = d ? readdir(d) : nullptr)
			left += e->d_name[0] != '.' || std::string(e->d_name).rfind(".ps5sx2-texwrite-", 0) == 0;
		if (d)
			closedir(d);
		CHECK(left == 1); // (the payload's mark stays in the folder the app can't see as its own: harmless, and the case is an error)
	}

	// 6. Through the manager: a .7z pack from Drive (download and pack file) written by the payload, the same pack file as without it; the log says so.
	{
		const std::string sroot = work + "/sz";
		std::string without;
		for (int pass = 0; pass < 2; pass++)
		{
			SzRig r(fixtures, sroot, pass ? "writer-on" : "writer-off");
			if (pass)
				r.launch = HostLaunch(base, &pids);
			r.fx.drive_files["SZFILE0000000005"] = "sz-big-mt.7z";
			r.Open(SzSource("SZFILE0000000005", "Big MT.7z", "\"SLUS-77777\""));
			CHECK(WaitFor(*r.m, "SLUS-77777", State::Available, 5));
			CHECK(r.m->Begin("SLUS-77777", 0, "", "", "Big"));
			CHECK(WaitFor(*r.m, "SLUS-77777", State::Installed, 120));
			CheckAgainstManifest(r.textures + "/SLUS-77777/replacements.pak", fixtures + "/sz-big-mt.7z.manifest", 8);
			const std::string pak = ReadAll(r.textures + "/SLUS-77777/replacements.pak");
			if (pass)
			{
				CHECK(!without.empty() && pak == without);
				CHECK(r.log.Has("writer process ") && r.log.Has(" written by the writer process") && r.log.Has("the writer process wrote "));
				CHECK(!r.log.Has("by the app"));
			}
			else
			{
				without = pak;
				CHECK(!r.log.Has("writer process") && r.log.Has("written by the app"));
			}
			r.Close();
		}
		// the flag texpack_titlewrites: no payload started
		{
			SzRig r(fixtures, sroot, "writer-flag");
			const size_t before = pids.size();
			r.launch = HostLaunch(base, &pids);
			r.flags.Set("texpack_titlewrites", true);
			r.fx.drive_files["SZFILE0000000006"] = "sz-nonsolid.7z";
			r.Open(SzSource("SZFILE0000000006", "Non Solid.7z", "\"SLUS-33334\""));
			CHECK(WaitFor(*r.m, "SLUS-33334", State::Available, 5));
			CHECK(r.m->Begin("SLUS-33334", 0, "", "", "NS"));
			CHECK(WaitFor(*r.m, "SLUS-33334", State::Installed, 60));
			CHECK(pids.size() == before && !r.log.Has("writer process "));
			r.Close();
		}
		// a payload that never connects: the app writes the pack itself and says why, once (not again for the next pack within 10 minutes)
		{
			SzRig r(fixtures, sroot, "writer-absent");
			std::atomic<int> launches{0};
			r.launch = [&launches](const std::string&, std::string&) {
				launches++;
				return true;
			};
			r.fx.drive_files["SZFILE0000000006"] = "sz-nonsolid.7z";
			r.fx.drive_files["SZFILE0000000001"] = "sz-good.7z";
			r.Open(SzSource("SZFILE0000000006", "Non Solid.7z", "\"SLUS-33334\"") + "," + SzSource("SZFILE0000000001", "Spider Pack.7z", "\"SLUS-20776\""));
			CHECK(WaitFor(*r.m, "SLUS-33334", State::Available, 5));
			CHECK(r.m->Begin("SLUS-33334", 0, "", "", "NS"));
			CHECK(WaitFor(*r.m, "SLUS-33334", State::Installed, 60));
			CheckAgainstManifest(r.textures + "/SLUS-33334/replacements.pak", fixtures + "/sz-nonsolid.7z.manifest", 25);
			CHECK(r.log.Has("no writer process (the writer did not connect"));
			CHECK(r.m->Begin("SLUS-20776", 0, "", "", "Spider"));
			CHECK(WaitFor(*r.m, "SLUS-20776", State::Installed, 60));
			CHECK(launches == 1);
			r.Close();
		}
	}
	// 7. Google Drive's "Quota exceeded" page in place of the file (the 177 reports: "range at 0: status 200, 2040 of 33554432 bytes", then every later
	// range refused): the page is never written into the download, the pack fails at once with Drive's reason (no six tries), and a part an older build
	// left with the page at its start is started again.
	{
		const std::string sroot = work + "/sz";
		SzRig r(fixtures, sroot, "drive-quota");
		r.fx.drive_files["SZFILE0000000006"] = "sz-nonsolid.7z";
		r.fx.drive_page = "<!DOCTYPE html><html><head><title>Google Drive - Quota exceeded</title></head><body>Too many users have viewed or downloaded this file recently.</body></html>";
		r.Open(SzSource("SZFILE0000000006", "Non Solid.7z", "\"SLUS-33334\""));
		CHECK(WaitFor(*r.m, "SLUS-33334", State::Available, 5));
		CHECK(r.m->Begin("SLUS-33334", 0, "", "", "NS"));
		CHECK(WaitFor(*r.m, "SLUS-33334", State::Failed, 20));
		CHECK(r.m->Status("SLUS-33334").message.find("Quota exceeded") != std::string::npos);
		CHECK(r.log.Has("Google Drive answered 200 with a page (Google Drive - Quota exceeded), not the file"));
		CHECK(r.fx.drive_ranges == 1); // asked once, not six times
		const std::string part = r.downloads + "/gSZFILE0000000006.part";
		CHECK(!Exists(part) || ReadAll(part).empty());
		r.Close();
		// an older build's part: the page at its start
		r.fx.drive_page.clear();
		mkdir(r.downloads.c_str(), 0777);
		std::ofstream(part, std::ios::binary) << "<!DOCTYPE html><html><head><title>Google Drive - Quota exceeded</title>";
		r.Open(SzSource("SZFILE0000000006", "Non Solid.7z", "\"SLUS-33334\""));
		for (int i = 0; i < 250 && r.m->Status("SLUS-33334").state == State::Loading; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		r.m->Begin("SLUS-33334", 0, "", "", "NS"); // (or carried on by itself from the job file)
		CHECK(WaitFor(*r.m, "SLUS-33334", State::Installed, 60));
		CHECK(r.log.Has("begins with Google Drive's page, not the file"));
		CheckAgainstManifest(r.textures + "/SLUS-33334/replacements.pak", fixtures + "/sz-nonsolid.7z.manifest", 25);
		r.Close();
	}
	// the payloads end with their connections
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	int alive = 0;
	for (pid_t pid : pids)
		alive += kill(pid, 0) == 0;
	CHECK(alive == 0);
	if (alive)
		for (pid_t pid : pids)
			kill(pid, SIGKILL);
}

void TestSevenZipPacks(const std::string& fixtures, const std::string& work)
{
	const std::string root = work + "/sz";
	const auto size_of = [&](const std::string& f) { return ReadAll(fixtures + "/" + f).size(); };
	std::printf("---- .7z packs through the manager\n");

	// 1. A Bl4ckH4nd-style pack: the files under replacements/ (not dumps/, the readme or the archive's own replacements.pak), a pack file with the right entries,
	// the same pack file byte for byte as the zip reader makes of the same tree, the job file saying an unpack began, the download named <key>.7z, the shown
	// size a hint only (the file's own size is what is downloaded and logged).
	{
		SzRig r(fixtures, root, "good");
		r.fx.drive_files["SZFILE0000000001"] = "sz-good.7z";
		r.fx.drive_files["SZFILE0000000002"] = "sz-good.zip";
		r.Open(SzSource("SZFILE0000000001", "Spider Pack.7z", "\"SLUS-20776\"", ",\"size\":12345,\"unpacked\":999") + "," +
			   SzSource("SZFILE0000000002", "Spider Pack.zip", "\"SLUS-20776\""));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Available, 5));
		TexturePackStatus st = r.m->Status("SLUS-20776");
		// the .rar and .zip packs are listed before the .7z ones (what Cross takes without Left/Right is the first), whatever the makers' names
		CHECK(st.packs.size() == 2 && st.packs[0].OnDrive() && st.packs[0].name == "Spider Pack.zip" && st.packs[1].name == "Spider Pack.7z");
		CHECK(st.packs[1].bytes == 0 && st.packs[1].hint_bytes == 12345);
		const int seven = PackIndex(*r.m, "SLUS-20776", "Spider Pack.7z"), zip = PackIndex(*r.m, "SLUS-20776", "Spider Pack.zip");
		CHECK(seven == 1 && zip == 0);
		CHECK(r.log.Has("Drive packs in all: 2 offered (1 rar/zip, 1 7z), 0 left out"));
		r.pause.Arm("7z: needs about");
		CHECK(r.m->Begin("SLUS-20776", seven, "", "", "Spider-Man 2"));
		CHECK(r.pause.WaitHit(20));
		// the unpack is about to begin: the job file already says so, and the download has its own name
		CHECK(ReadAll(r.Job("SLUS-20776")).find("unpack_tries=1") != std::string::npos);
		CHECK(Exists(r.downloads + "/gSZFILE0000000001.7z") && !Exists(r.downloads + "/gSZFILE0000000001.rar"));
		r.pause.Release();
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Installed, 60));
		std::map<std::string, std::string> pak;
		size_t entries = 0;
		CHECK(ReadPak(r.textures + "/SLUS-20776/replacements.pak", pak, &entries));
		CHECK(entries == 4 && pak.size() == 3); // a.dds, 01/b.png twice (the later wins), empty.dds
		CHECK(pak["a.dds"].size() == 3u * 1024 * 1024);
		CHECK(pak["01/b.png"] == "the second texture, again" && pak.count("empty.dds") && pak["empty.dds"].empty());
		CHECK(!r.Exists("SLUS-20776/replacements") && !r.Exists("SLUS-20776/dumps") && !r.Exists("readme.txt") && !r.Exists(".ps5sx2-unpack"));
		CHECK(!r.Exists(".ps5sx2-downloads/gSZFILE0000000001.7z") && !Exists(r.Job("SLUS-20776")));
		const std::string marker = ReadAll(r.textures + "/SLUS-20776/" + TexturePackManager::kMarker);
		CHECK(marker.find("name=Spider Pack.7z") != std::string::npos && marker.find("Google Drive") != std::string::npos && marker.find("md5=") == std::string::npos);
		CHECK(r.log.Has("starting Spider Pack.7z for SLUS-20776 (" + FormatBytes(size_of("sz-good.7z")) + ", from Google Drive)"));
		CHECK(!r.log.Has("(12 KB")); // the hint is not the size
		CHECK(r.log.Has("7z unpacked Spider Pack.7z: archive " + std::to_string(size_of("sz-good.7z")) + " bytes, 4 files kept of "));
		CHECK(r.log.Has("flexible memory free") && r.log.Has("7z: needs about"));
		const std::string pak7 = ReadAll(r.textures + "/SLUS-20776/replacements.pak");
		CHECK(r.m->Remove("SLUS-20776"));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Available, 5));
		CHECK(WaitNoSetAside(r.textures, 5));
		// the zip reader on the same tree
		CHECK(r.m->Begin("SLUS-20776", zip, "", "", "Spider-Man 2"));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Installed, 60));
		CHECK(!pak7.empty() && ReadAll(r.textures + "/SLUS-20776/replacements.pak") == pak7); // the same pack file, byte for byte
		CHECK(r.m->Remove("SLUS-20776"));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Available, 5));
		r.Close();
	}

	// 2. A pack's own extra files beside its textures: kept whole when small, the 9 MB one left out (a loose file is held in memory), the cap logged.
	{
		SzRig r(fixtures, root, "loose");
		r.fx.drive_files["SZFILE0000000003"] = "sz-loose.7z";
		r.fx.drive_files["SZFILE0000000020"] = "zip-loose.zip";
		r.Open(SzSource("SZFILE0000000003", "Loose Pack.7z", "\"SLUS-20776\"") + "," + SzSource("SZFILE0000000020", "Loose Pack.zip", "\"SLUS-20776\""));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Available, 5));
		const auto check_loose = [&] {
			std::map<std::string, std::string> pak;
			CHECK(ReadPak(r.textures + "/SLUS-20776/replacements.pak", pak) && pak.size() == 1 && pak["x.png"] == "a texture");
			CHECK(ReadAll(r.textures + "/SLUS-20776/notes/n.txt") == "a note beside the replacements");
			CHECK(r.Exists("SLUS-20776/notes/small.bin") && ReadAll(r.textures + "/SLUS-20776/notes/small.bin").size() == 40000);
			CHECK(!r.Exists("SLUS-20776/notes/huge.bin")); // 9 MB: over the 8 MB a loose file may be
		};
		const int seven = PackIndex(*r.m, "SLUS-20776", "Loose Pack.7z"), zip = PackIndex(*r.m, "SLUS-20776", "Loose Pack.zip");
		CHECK(seven == 1 && zip == 0);
		CHECK(r.m->Begin("SLUS-20776", seven, "", "", "Loose"));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Installed, 60));
		check_loose();
		CHECK(r.log.Has("7z: wanted 3 files") && r.log.Has("1 loose over"));
		// the libarchive path (zip) has the same cap now: the same files from the same tree
		CHECK(r.m->Remove("SLUS-20776"));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Available, 5));
		CHECK(WaitNoSetAside(r.textures, 5));
		CHECK(r.m->Begin("SLUS-20776", zip, "", "", "Loose"));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Installed, 60));
		check_loose();
		CHECK(r.log.Has("1 other files left out for their size"));
		r.Close();
	}

	// 2b. A game with a .7z by a maker whose name sorts first and a .rar by a later one: the .rar is the first on the row (build 175's default stays the default); archive.org's
	// pack (the first source) stays before both.
	{
		SzRig r(fixtures, root, "order");
		r.Open(SzSource("SZFILE0000000023", "Aaa Game.7z", "\"SLUS-20779\"", "", "Aaa") + "," + SzSource("SZFILE0000000024", "Zzz Game.rar", "\"SLUS-20779\"", "", "Zzz") + "," +
			   SzSource("SZFILE0000000025", "Mmm Game.7z", "\"SLUS-20779\"", "", "Mmm") + "," + SzSource("SZFILE0000000026", "Bbb Game.zip", "\"SLUS-20779\"", "", "Bbb"));
		CHECK(WaitFor(*r.m, "SLUS-20779", State::Available, 5));
		const TexturePackStatus st = r.m->Status("SLUS-20779");
		CHECK(st.packs.size() == 4 && st.packs[0].name == "Bbb Game.zip" && st.packs[1].name == "Zzz Game.rar" && st.packs[2].name == "Aaa Game.7z" && st.packs[3].name == "Mmm Game.7z");
		r.Close();
	}

	// 3. Two discs in one pack: both serials' pack files.
	{
		SzRig r(fixtures, root, "discs");
		r.fx.drive_files["SZFILE0000000004"] = "sz-two-discs.7z";
		r.Open(SzSource("SZFILE0000000004", "Two Discs.7z", "\"SLUS-21180\",\"SLUS-21362\""));
		CHECK(WaitFor(*r.m, "SLUS-21362", State::Available, 5));
		CHECK(r.m->Begin("SLUS-21362", 0, "", "", "Two"));
		CHECK(WaitFor(*r.m, "SLUS-21362", State::Installed, 60));
		std::map<std::string, std::string> pak;
		CHECK(ReadPak(r.textures + "/SLUS-21180/replacements.pak", pak) && pak.size() == 1 && pak["d1.png"] == "disc one");
		CHECK(ReadPak(r.textures + "/SLUS-21362/replacements.pak", pak) && pak.size() == 1 && pak["d2.png"] == "disc two");
		CHECK(r.log.Has("for 2 pack file(s)"));
		r.Close();
	}

	// 4. Entries across the pack writer's 8 MiB buffers, as 7-Zip's multithreaded mode writes them (independent LZMA2 blocks in one stream); and a non-solid pack.
	{
		SzRig r(fixtures, root, "buffers");
		r.fx.drive_files["SZFILE0000000005"] = "sz-big-mt.7z";
		r.fx.drive_files["SZFILE0000000006"] = "sz-nonsolid.7z";
		r.Open(SzSource("SZFILE0000000005", "Big MT.7z", "\"SLUS-77777\"") + "," + SzSource("SZFILE0000000006", "Non Solid.7z", "\"SLUS-33334\""));
		CHECK(WaitFor(*r.m, "SLUS-77777", State::Available, 5));
		CHECK(r.m->Begin("SLUS-77777", 0, "", "", "Big"));
		CHECK(WaitFor(*r.m, "SLUS-77777", State::Installed, 120));
		CheckAgainstManifest(r.textures + "/SLUS-77777/replacements.pak", fixtures + "/sz-big-mt.7z.manifest", 8);
		CHECK(r.m->Begin("SLUS-33334", 0, "", "", "NS"));
		CHECK(WaitFor(*r.m, "SLUS-33334", State::Installed, 60));
		CheckAgainstManifest(r.textures + "/SLUS-33334/replacements.pak", fixtures + "/sz-nonsolid.7z.manifest", 25);
		r.Close();
	}

	// 5. Hostile names beside a good file: only the good file lands, nothing outside the textures folder.
	{
		SzRig r(fixtures, root, "hostile");
		r.fx.drive_files["SZFILE0000000007"] = "sz-hostile.7z";
		r.Open(SzSource("SZFILE0000000007", "Evil.7z", "\"SLUS-20776\""));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Available, 5));
		CHECK(r.m->Begin("SLUS-20776", 0, "", "", "Evil"));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Installed, 60));
		std::map<std::string, std::string> pak;
		size_t entries = 0;
		CHECK(ReadPak(r.textures + "/SLUS-20776/replacements.pak", pak, &entries) && entries == 1 && pak["ok.dds"] == "the only good one");
		CHECK(!Exists(r.base + "/escape.dds") && !Exists(r.base + "/escape2.dds") && !Exists(root + "/escape.dds") && !Exists(root + "/escape2.dds") &&
			  !Exists(r.textures + "/escape.dds") && !Exists(work + "/escape.dds") && !Exists("/abs"));
		CHECK(!r.Exists("SLUS-20776/replacements/link.dds") && !r.Exists("SLUS-20776/replacements/ctl\x01.dds"));
		r.Close();
	}

	// 6. A damaged pack: failed with the archive's words, the download (no checksum vouches for it) deleted, nothing installed, the unpack count back to zero.
	// A pack with nothing for the game: said so, download deleted.
	{
		SzRig r(fixtures, root, "damaged");
		r.fx.drive_files["SZFILE0000000008"] = "sz-damaged.7z";
		r.fx.drive_files["SZFILE0000000009"] = "sz-nothing.7z";
		r.Open(SzSource("SZFILE0000000008", "Damaged.7z", "\"SLUS-20776\"") + "," + SzSource("SZFILE0000000009", "Nothing.7z", "\"SLUS-20777\""));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Available, 5));
		CHECK(r.m->Begin("SLUS-20776", 0, "", "", "Damaged"));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Failed, 60));
		CHECK(r.m->Status("SLUS-20776").message.rfind("The pack is damaged", 0) == 0);
		CHECK(!r.Exists(".ps5sx2-downloads/gSZFILE0000000008.7z") && !r.Exists("SLUS-20776") && !r.Exists(".ps5sx2-unpack"));
		CHECK(!Exists(r.Job("SLUS-20776"))); // (review) damaged the same way next time: no job file to fetch the same gigabytes again at every start; Cross asks afresh
		CHECK(r.m->Cancel("SLUS-20776"));
		CHECK(r.m->Begin("SLUS-20777", 0, "", "", "Nothing"));
		CHECK(WaitFor(*r.m, "SLUS-20777", State::Failed, 60));
		CHECK(r.m->Status("SLUS-20777").message == "Nothing in it is a texture for SLUS-20777");
		CHECK(!r.Exists(".ps5sx2-downloads/gSZFILE0000000009.7z") && !r.Exists("SLUS-20777"));
		CHECK(r.m->Cancel("SLUS-20777"));
		// (review) the file decides this, so it is remembered like the other refusals: the next try costs the one-byte probe and no download
		CHECK(Exists(r.downloads + "/gSZFILE0000000009.SLUS-20777.refused"));
		const int ranges = r.fx.drive_ranges;
		CHECK(r.m->Begin("SLUS-20777", 0, "", "", "Nothing"));
		CHECK(WaitFor(*r.m, "SLUS-20777", State::Failed, 10));
		CHECK(r.m->Status("SLUS-20777").message == "Nothing in it is a texture for SLUS-20777" && r.fx.drive_ranges == ranges);
		CHECK(r.log.Has("was refused before (Nothing in it is a texture for SLUS-20777)"));
		CHECK(r.m->Cancel("SLUS-20777"));
		r.Close();
	}

	// 7. Packs this reader refuses: said before a byte is written, the download and the job deleted, the refusal kept so that the next try doesn't download it again.
	// The dictionary over 64 MB is lifted by the flag texpack_7z_bigdict.
	{
		SzRig r(fixtures, root, "refused");
		r.fx.drive_files["SZFILE0000000010"] = "sz-bigdict.7z";
		r.fx.drive_files["SZFILE0000000011"] = "sz-bcj.7z";
		r.fx.drive_files["SZFILE0000000012"] = "sz-aes.7z";
		r.fx.drive_files["SZFILE0000000013"] = "sz-nocrc.7z";
		r.fx.drive_files["SZFILE0000000022"] = "sz-wrongserial.7z";
		r.fx.drive_files["SZFILE0000000027"] = "sz-looseonly.7z";
		r.fx.drive_files["SZFILE0000000028"] = "sz-othertex.7z";
		r.Open(SzSource("SZFILE0000000010", "Big Dict.7z", "\"SLUS-33335\"") + "," + SzSource("SZFILE0000000011", "Filter.7z", "\"SLUS-30001\"") + "," +
			   SzSource("SZFILE0000000012", "Secret.7z", "\"SLUS-30002\"") + "," + SzSource("SZFILE0000000013", "No Sums.7z", "\"SLUS-30003\"") + "," +
			   SzSource("SZFILE0000000022", "Typo Pack.7z", "\"SLUS-21722\"") + "," + SzSource("SZFILE0000000027", "Loose Only.7z", "\"SLUS-30004\"") + "," +
			   SzSource("SZFILE0000000028", "Other Tex.7z", "\"SLUS-30005\""));
		CHECK(WaitFor(*r.m, "SLUS-33335", State::Available, 5));
		CHECK(r.m->Begin("SLUS-33335", 0, "", "", "Big Dict"));
		CHECK(WaitFor(*r.m, "SLUS-33335", State::Failed, 60));
		const std::string said = r.m->Status("SLUS-33335").message;
		std::printf("  refused: %s\n", said.c_str());
		CHECK(said.rfind("Too big for the console", 0) == 0 && said.find("70 MiB dictionary") != std::string::npos && said.find("limit is 64 MiB") != std::string::npos);
		CHECK(Exists(r.downloads + "/gSZFILE0000000010.SLUS-33335.refused") && !Exists(r.downloads + "/gSZFILE0000000010.7z") && !Exists(r.Job("SLUS-33335")));
		const int ranges = r.fx.drive_ranges, probes = r.fx.probes;
		CHECK(r.m->Begin("SLUS-33335", 0, "", "", "Big Dict")); // again: refused at once, with the one-byte probe (the refusal is for a file of that size) and no download
		CHECK(WaitFor(*r.m, "SLUS-33335", State::Failed, 10));
		CHECK(r.m->Status("SLUS-33335").message == said && r.fx.drive_ranges == ranges && r.fx.probes == probes + 1);
		CHECK(r.log.Has("was refused before"));
		// (review) a refusal is for the file it was made for: Drive keeps a file's id when its maker uploads a new version, so a marker made for another size is not believed
		// (the pack is downloaded and looked at again; here it is still the same bad pack, so it is refused again and the marker is made anew, with the right size)
		{
			const std::string marker = r.downloads + "/gSZFILE0000000010.SLUS-33335.refused";
			const std::string text = ReadAll(marker);
			CHECK(text.find("limits=1\nbytes=") == 0 && text.find("\nmessage=Too big for the console") != std::string::npos);
			std::ofstream(marker) << "limits=1\nbytes=12345\nmessage=" << said << "\n";
			const int ranges_old = r.fx.drive_ranges;
			CHECK(r.m->Begin("SLUS-33335", 0, "", "", "Big Dict"));
			CHECK(WaitFor(*r.m, "SLUS-33335", State::Failed, 60));
			CHECK(r.fx.drive_ranges > ranges_old && r.m->Status("SLUS-33335").message == said);
			CHECK(ReadAll(marker) == text);
		}
		r.flags.Set("texpack_7z_bigdict", true); // a 128 MB dictionary (70 MB of data) is allowed by the flag
		CHECK(r.m->Begin("SLUS-33335", 0, "", "", "Big Dict"));
		CHECK(WaitFor(*r.m, "SLUS-33335", State::Installed, 120));
		std::map<std::string, std::string> pak;
		CHECK(ReadPak(r.textures + "/SLUS-33335/replacements.pak", pak) && pak["z.dds"].size() == 70u << 20);
		r.flags.Set("texpack_7z_bigdict", false);
		const struct
		{
			const char* serial;
			const char* words;
		} refusals[] = {{"SLUS-30001", "coders"}, {"SLUS-30002", "password"}, {"SLUS-30003", "checksum"}};
		for (const auto& rf : refusals)
		{
			CHECK(WaitFor(*r.m, rf.serial, State::Available, 5));
			CHECK(r.m->Begin(rf.serial, 0, "", "", "x"));
			CHECK(WaitFor(*r.m, rf.serial, State::Failed, 60));
			const std::string msg = r.m->Status(rf.serial).message;
			std::printf("  refused: %s\n", msg.c_str());
			CHECK(msg.rfind("Can't use this pack: ", 0) == 0 && msg.find(rf.words) != std::string::npos);
			CHECK(!Exists(r.Job(rf.serial)));
		}
		// textures for another game than the list says: refused (nothing is installed under the other serial with this game's marker), and kept refused
		CHECK(WaitFor(*r.m, "SLUS-21722", State::Available, 5));
		CHECK(r.m->Begin("SLUS-21722", 0, "", "", "Typo"));
		CHECK(WaitFor(*r.m, "SLUS-21722", State::Failed, 60));
		const std::string wrong = r.m->Status("SLUS-21722").message;
		std::printf("  refused: %s\n", wrong.c_str());
		CHECK(wrong.rfind("Not for this game: the pack holds textures for SLUS-21752, not for SLUS-21722", 0) == 0);
		CHECK(!r.Exists("SLUS-21722") && !r.Exists("SLUS-21752") && !r.Exists(".ps5sx2-unpack") && !Exists(r.downloads + "/gSZFILE0000000022.7z"));
		CHECK(Exists(r.downloads + "/gSZFILE0000000022.SLUS-21722.refused") && !Exists(r.Job("SLUS-21722")));
		const int ranges_before = r.fx.drive_ranges;
		CHECK(r.m->Begin("SLUS-21722", 0, "", "", "Typo"));
		CHECK(WaitFor(*r.m, "SLUS-21722", State::Failed, 10));
		CHECK(r.m->Status("SLUS-21722").message == wrong && r.fx.drive_ranges == ranges_before);
		// (review) the game's own folder holds no texture (a readme and a note): nothing for the game to load, so not an "installed" pack with a marker and nothing in it
		CHECK(WaitFor(*r.m, "SLUS-30004", State::Available, 5));
		CHECK(r.m->Begin("SLUS-30004", 0, "", "", "Loose Only"));
		CHECK(WaitFor(*r.m, "SLUS-30004", State::Failed, 60));
		CHECK(r.m->Status("SLUS-30004").message == "Nothing in it is a texture for SLUS-30004");
		CHECK(!r.Exists("SLUS-30004") && !r.Exists(".ps5sx2-unpack") && Exists(r.downloads + "/gSZFILE0000000027.SLUS-30004.refused") && !Exists(r.Job("SLUS-30004")));
		// the textures are in the neighbour's folder, a readme in the game's own: refused as for the wrong game (the readme's folder is not the game's textures)
		CHECK(WaitFor(*r.m, "SLUS-30005", State::Available, 5));
		CHECK(r.m->Begin("SLUS-30005", 0, "", "", "Other Tex"));
		CHECK(WaitFor(*r.m, "SLUS-30005", State::Failed, 60));
		CHECK(r.m->Status("SLUS-30005").message.rfind("Not for this game: the pack holds textures for SLUS-30006, not for SLUS-30005", 0) == 0);
		CHECK(!r.Exists("SLUS-30005") && !r.Exists("SLUS-30006") && !r.Exists(".ps5sx2-unpack") && !Exists(r.Job("SLUS-30005")));
		r.Close();
	}

	// 8. Room and memory. The exact room is known once the header is read (what is wanted + 256 MB): the download is kept for the next try. The memory gate asks the
	// platform's free flexible memory (a 48 MB margin); the flag texpack_nomemgate lifts it.
	{
		SzRig r(fixtures, root, "gates");
		r.fx.drive_files["SZFILE0000000014"] = "sz-good.7z";
		r.Open(SzSource("SZFILE0000000014", "Gated.7z", "\"SLUS-20776\""));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Available, 5));
		r.pause.Arm("7z: wanted"); // logged right before the room is checked
		CHECK(r.m->Begin("SLUS-20776", 0, "", "", "Gated"));
		CHECK(r.pause.WaitHit(20));
		r.free_bytes = 100ull << 20; // less than the 256 MB and what is wanted
		r.pause.Release();
		CHECK(WaitFor(*r.m, "SLUS-20776", State::NeedSpace, 20));
		const std::string said = r.m->Status("SLUS-20776").message;
		std::printf("  %s\n", said.c_str());
		CHECK(said.rfind("Needs ", 0) == 0 && said.find("free to unpack (105 MB now)") != std::string::npos);
		CHECK(Exists(r.downloads + "/gSZFILE0000000014.7z") && !r.Exists(".ps5sx2-unpack") && !r.Exists("SLUS-20776"));
		const int ranges = r.fx.drive_ranges;
		r.free_bytes = UINT64_MAX;
		// the memory: below the header phase's floor (margin + 16 MB), then below what the unpack needs, then enough
		r.free_memory = 20ull << 20;
		CHECK(r.m->Begin("SLUS-20776", 0, "", "", "Gated"));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Failed, 20));
		CHECK(r.m->Status("SLUS-20776").message.rfind("Not enough memory to unpack this pack right now", 0) == 0);
		CHECK(Exists(r.downloads + "/gSZFILE0000000014.7z")); // kept: it is the moment that is wrong
		CHECK(Exists(r.Job("SLUS-20776")) && ReadAll(r.Job("SLUS-20776")).find("unpack_tries") == std::string::npos); // the job too (the next start tries again), its unpack count back at zero
		r.free_memory = 70ull << 20;
		CHECK(r.m->Begin("SLUS-20776", 0, "", "", "Gated"));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Failed, 20));
		const std::string low = r.m->Status("SLUS-20776").message;
		std::printf("  %s\n", low.c_str());
		CHECK(low.find("needs about") != std::string::npos && low.find("70 MiB now") != std::string::npos);
		r.flags.Set("texpack_nomemgate", true); // the flag: no gate, even at 20 MB
		r.free_memory = 20ull << 20;
		CHECK(r.m->Begin("SLUS-20776", 0, "", "", "Gated"));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Installed, 60));
		CHECK(r.log.Has("(not checked)"));
		CHECK(r.fx.drive_ranges == ranges); // every try used the download it had
		CHECK(r.m->Remove("SLUS-20776"));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Available, 5));
		r.flags.Set("texpack_nomemgate", false);
		r.free_memory = 400ull << 20; // plenty: goes through with the gate on
		CHECK(r.m->Begin("SLUS-20776", 0, "", "", "Gated"));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Installed, 60));
		r.Close();
	}

	// 9. Cancel (Triangle twice) and Stop (the shelf closes) while the unpack begins: staging gone, nothing installed; a Cancel removes the download and the job, a
	// Stop keeps both and the next start unpacks again from the download it has.
	{
		SzRig r(fixtures, root, "interrupt");
		r.fx.drive_files["SZFILE0000000015"] = "sz-good.7z";
		const std::string sources = SzSource("SZFILE0000000015", "Interrupted.7z", "\"SLUS-20776\"");
		r.Open(sources);
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Available, 5));
		r.pause.Arm("7z: needs about");
		CHECK(r.m->Begin("SLUS-20776", 0, "", "", "Cancel"));
		CHECK(r.pause.WaitHit(20));
		CHECK(r.m->Cancel("SLUS-20776"));
		r.pause.Release();
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Available, 20));
		CHECK(!r.Exists(".ps5sx2-unpack") && !r.Exists("SLUS-20776") && !Exists(r.downloads + "/gSZFILE0000000015.7z") && !Exists(r.Job("SLUS-20776")));
		CHECK(r.log.Has("cancelled Interrupted.7z"));
		// Stop
		r.pause.Arm("7z: needs about");
		CHECK(r.m->Begin("SLUS-20776", 0, "", "", "Stop"));
		CHECK(r.pause.WaitHit(20));
		bool stopped = false;
		std::thread stopper([&] { stopped = r.m->Stop(15000); });
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		r.pause.Release();
		stopper.join();
		CHECK(stopped);
		CHECK(!r.Exists(".ps5sx2-unpack") && !r.Exists("SLUS-20776"));
		CHECK(Exists(r.downloads + "/gSZFILE0000000015.7z") && Exists(r.Job("SLUS-20776")));
		CHECK(ReadAll(r.Job("SLUS-20776")).find("unpack_tries") == std::string::npos); // it ended (was stopped): the count is back to zero
		r.m.reset();
		const int ranges = r.fx.drive_ranges;
		r.Open(sources); // the next start carries on
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Installed, 60));
		CHECK(r.fx.drive_ranges == ranges);
		CHECK(r.m->Remove("SLUS-20776"));
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Available, 5));
		// A disk that takes its time (the pack writer thread is held): the unpacking thread fills the three 8 MiB buffers and waits for a free one. A cancel is heard in that
		// wait, not when the disk finally answers (the pack writer is still held when the log says so), and nothing is left behind.
		r.fx.drive_files["SZFILE0000000021"] = "sz-big-mt.7z";
		r.Close();
		r.hold_writer = true;
		r.Open(sources + "," + SzSource("SZFILE0000000021", "Slow Disk.7z", "\"SLUS-77777\""));
		CHECK(WaitFor(*r.m, "SLUS-77777", State::Available, 5));
		CHECK(r.m->Begin("SLUS-77777", 0, "", "", "Slow"));
		uint64_t last = 0;
		bool blocked = false;
		for (const double until = Now() + 60; Now() < until && !blocked;) // the progress stands still in Unpacking: it waits
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			const TexturePackStatus st = r.m->Status("SLUS-77777");
			blocked = st.state == State::Unpacking && st.done > 0 && st.done == last;
			last = st.done;
		}
		CHECK(blocked);
		CHECK(r.m->Cancel("SLUS-77777"));
		for (const double until = Now() + 10; Now() < until && !r.log.Has("while waiting for the disk");)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		CHECK(r.log.Has("7z: stopped while unpacking, while waiting for the disk")); // heard while the writer was still held
		r.ReleaseWriter();
		CHECK(WaitFor(*r.m, "SLUS-77777", State::Available, 20));
		CHECK(!r.Exists(".ps5sx2-unpack") && !r.Exists("SLUS-77777") && !Exists(r.Job("SLUS-77777")));
		r.Close();
	}

	// 10. The crash guard: a job file that says unpacks began and never ended (the app went down in them) is not carried on with by itself at the second one;
	// Cross asks again and it goes through (the first one still resumes).
	{
		SzRig r(fixtures, root, "crash");
		r.fx.drive_files["SZFILE0000000016"] = "sz-good.7z";
		mkdir(r.downloads.c_str(), 0777);
		const std::string archive = ReadAll(fixtures + "/sz-good.7z");
		const auto plant = [&](int tries) {
			std::ofstream(r.downloads + "/gSZFILE0000000016.7z", std::ios::binary) << archive;
			std::ofstream(r.Job("SLUS-20776")) << "serial=SLUS-20776\nname=Crashy.7z\nmd5=\nbytes=" << archive.size()
												<< "\ntitle=Crashy\nsettings=\nheader=\nhost=gdrive\ndrive=SZFILE0000000016\nunpack_tries=" << tries << "\n";
		};
		const std::string sources = SzSource("SZFILE0000000016", "Crashy.7z", "\"SLUS-20776\"");
		plant(2);
		r.Open(sources);
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Failed, 10));
		CHECK(r.m->Status("SLUS-20776").message.rfind("The last two tries at unpacking this pack were cut short", 0) == 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		CHECK(r.fx.probes == 0 && r.fx.drive_ranges == 0 && !r.Exists("SLUS-20776") && r.log.Has("not carrying on with Crashy.7z"));
		CHECK(r.m->Begin("SLUS-20776", 0, "", "", "Crashy")); // Cross: tries again, and this one ends normally
		CHECK(WaitFor(*r.m, "SLUS-20776", State::Installed, 60));
		r.Close();
		// one unfinished try is carried on with
		SzRig r1(fixtures, root, "crash1");
		r1.fx.drive_files["SZFILE0000000016"] = "sz-good.7z";
		mkdir(r1.downloads.c_str(), 0777);
		std::ofstream(r1.downloads + "/gSZFILE0000000016.7z", std::ios::binary) << archive;
		std::ofstream(r1.Job("SLUS-20776")) << "serial=SLUS-20776\nname=Crashy.7z\nmd5=\nbytes=" << archive.size()
											<< "\ntitle=Crashy\nsettings=\nheader=\nhost=gdrive\ndrive=SZFILE0000000016\nunpack_tries=1\n";
		r1.Open(sources);
		CHECK(WaitFor(*r1.m, "SLUS-20776", State::Installed, 60));
		CHECK(!Exists(r1.Job("SLUS-20776")));
		r1.Close();
	}

	// 11. The flag texpack_no7z gives build 175's behaviour: the 7z packs are not offered, counted and logged as left out; a 7z job resumed from before is failed.
	{
		SzRig r(fixtures, root, "no7z");
		r.fx.drive_files["SZFILE0000000017"] = "sz-good.7z";
		r.flags.Set("texpack_no7z", true);
		mkdir(r.downloads.c_str(), 0777);
		std::ofstream(r.Job("SLUS-20778")) << "serial=SLUS-20778\nname=Old Job.7z\nmd5=\nbytes=0\ntitle=Old\nsettings=\nheader=\nhost=gdrive\ndrive=SZFILE0000000018\n";
		r.Open(SzSource("SZFILE0000000017", "Switched Off.7z", "\"SLUS-20776\"") + "," +
			   "{\"id\":\"SZFILE0000000019\",\"name\":\"A Zip.zip\",\"title\":\"A Zip\",\"label\":\"X\",\"maker\":\"X\",\"serials\":[\"SLUS-20779\"]}");
		CHECK(WaitFor(*r.m, "SLUS-20779", State::Available, 5));
		CHECK(r.m->Status("SLUS-20776").packs.empty());
		CHECK(r.log.Has("1 7z left out (flag texpack_no7z)") && r.log.Has("Drive packs in all: 1 offered (1 rar/zip, 0 7z), 1 left out"));
		CHECK(WaitFor(*r.m, "SLUS-20778", State::Failed, 10));
		CHECK(r.m->Status("SLUS-20778").message.find("texpack_no7z") != std::string::npos);
		CHECK(r.fx.drive_ranges == 0 && r.fx.probes == 0);
		r.Close();
	}

	// 12. (review) Cancels and Stops at random moments, over and over, of the pack that crosses the pack writer's buffers (download, header, scan, decoding, a pack writer
	// held up by a slow disk, the final flush): every round ends with nothing half-installed (no staging folder, no pack file for a cancelled game), a pack that did get
	// installed is byte-exact, the job file of a stopped round has its unpack count back at zero and is carried on by the next start, and no file descriptor is left
	// open. Under the sanitizers: no leak, no race, no use of a writer that the unpack gave up on.
	{
		SzRig r(fixtures, root, "stress");
		r.fx.drive_files["SZFILE0000000030"] = "sz-big-mt.7z";
		const std::string sources = SzSource("SZFILE0000000030", "Stress.7z", "\"SLUS-77777\"");
		const std::string pak_path = r.textures + "/SLUS-77777/replacements.pak";
		const auto fds = [] {
			int n = 0;
			if (DIR* d = opendir("/proc/self/fd"))
			{
				while (readdir(d))
					n++;
				closedir(d);
			}
			return n;
		};
		const auto hold = [&r](bool on) {
			std::lock_guard<std::mutex> l(r.hold_m);
			r.hold_writer = on;
			r.hold_cv.notify_all();
		};
		const auto settle = [&r]() -> State {
			for (const double until = Now() + 180; Now() < until;)
			{
				const State s = r.m->Status("SLUS-77777").state;
				if (s == State::Available || s == State::Installed || s == State::Failed)
					return s;
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			return State::Loading;
		};
		r.Open(sources);
		CHECK(WaitFor(*r.m, "SLUS-77777", State::Available, 5));
		const int fds_before = fds();
		std::mt19937 rng(20261016);
		int cancelled = 0, stopped_rounds = 0, installed = 0;
		const int kRounds = 20;
		for (int round = 0; round < kRounds; round++)
		{
			const int how = round % 5; // 0, 1: cancel at a random moment; 2: the shelf closes at a random moment; 3: cancel while the pack writer is held; 4: let it finish
			const int ms = static_cast<int>(rng() % 1000);
			if (how == 3)
				hold(true);
			CHECK(r.m->Begin("SLUS-77777", 0, "", "", "Stress"));
			std::this_thread::sleep_for(std::chrono::milliseconds(how == 3 ? 500 + ms : how == 4 ? 0 : ms));
			if (how <= 1 || how == 3)
			{
				cancelled += r.m->Cancel("SLUS-77777") ? 1 : 0;
				hold(false);
			}
			else if (how == 2)
			{
				bool stopped = false;
				std::thread stopper([&] { stopped = r.m->Stop(30000); });
				stopper.join();
				CHECK(stopped);
				r.m.reset();
				stopped_rounds++;
				const bool job_left = Exists(r.Job("SLUS-77777"));
				if (job_left)
					CHECK(ReadAll(r.Job("SLUS-77777")).find("unpack_tries") == std::string::npos); // the count is back at zero
				CHECK(!r.Exists(".ps5sx2-unpack"));
				const int carried = r.log.Count("carrying on with Stress.7z");
				r.Open(sources);
				if (job_left)
					for (const double until = Now() + 10; Now() < until && r.log.Count("carrying on with Stress.7z") == carried;)
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			const State s = settle();
			CHECK(s != State::Failed && s != State::Loading);
			if (s == State::Installed)
			{
				installed++;
				CheckAgainstManifest(pak_path, fixtures + "/sz-big-mt.7z.manifest", 8);
				CHECK(!Exists(r.downloads + "/gSZFILE0000000030.7z") && !Exists(r.Job("SLUS-77777")) && !r.Exists(".ps5sx2-unpack"));
				CHECK(r.m->Remove("SLUS-77777"));
				CHECK(WaitFor(*r.m, "SLUS-77777", State::Available, 10));
				CHECK(WaitNoSetAside(r.textures, 10));
			}
			else
			{
				CHECK(!r.Exists(".ps5sx2-unpack") && !r.Exists("SLUS-77777") && !Exists(r.Job("SLUS-77777")));
			}
		}
		std::printf("  stress: %d rounds, %d cancelled, %d stopped and carried on, %d ended installed\n", kRounds, cancelled, stopped_rounds, installed);
		CHECK(cancelled > 0 && stopped_rounds > 0);
		r.Close();
		CHECK(fds() <= fds_before + 2); // (the worker's own files are closed again)
	}
}
} // namespace

// (review) One big pack through the manager, made outside the repository (it is gigabytes unpacked): TEXPACKS_BIG_7Z=<a.7z>, with <a>.json beside it ({"files": [[name, size, crc32 hex],
// ...]}, the corpus format of make_7z_fixtures.py). The manager installs it; the pack file's index is read back and every entry is checked by size and CRC32 (read in 16 MiB blocks:
// the pack is bigger than memory allows). Offsets past 4 GiB are where a 32-bit slip would show. Set TMPDIR to a disk with room for the unpacked size.
static void TestBigSevenZip(const std::string& work)
{
	const char* env = std::getenv("TEXPACKS_BIG_7Z");
	if (!env || !*env)
		return;
	const std::string archive = env;
	std::printf("---- a big .7z through the manager: %s\n", archive.c_str());
	const size_t slash = archive.rfind('/');
	const std::string dir = slash == std::string::npos ? "." : archive.substr(0, slash), base = slash == std::string::npos ? archive : archive.substr(slash + 1);
	rapidjson::Document doc;
	const std::string json = ReadAll((archive.size() > 3 ? archive.substr(0, archive.size() - 3) : archive) + ".json");
	doc.Parse(json.c_str(), json.size());
	CHECK(!doc.HasParseError() && doc.IsObject() && doc.HasMember("files") && doc["files"].IsArray() && doc["files"].Size() > 0);
	if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("files") || !doc["files"].IsArray() || doc["files"].Size() == 0)
		return;
	std::string serial;
	for (size_t at = 0; serial.empty() && at < std::string(doc["files"][0][0].GetString()).size();)
	{
		const std::string name = doc["files"][0][0].GetString();
		size_t end = name.find('/', at);
		if (end == std::string::npos)
			end = name.size();
		const std::string part = name.substr(at, end - at);
		if (part.size() == 10 && part[4] == '-')
			serial = part;
		at = end + 1;
	}
	CHECK(!serial.empty());
	SzRig r(dir, work + "/sz", "bigpack");
	r.fx.drive_files["SZFILE0000000040"] = base;
	r.Open(SzSource("SZFILE0000000040", "Big Pack.7z", "\"" + serial + "\""));
	CHECK(WaitFor(*r.m, serial, State::Available, 5));
	CHECK(r.m->Begin(serial, 0, "", "", "Big"));
	const double t0 = Now();
	CHECK(WaitFor(*r.m, serial, State::Installed, 3600));
	std::printf("  installed in %.0f s\n", Now() - t0);
	const int fd = open((r.textures + "/" + serial + "/replacements.pak").c_str(), O_RDONLY);
	CHECK(fd >= 0);
	if (fd >= 0)
	{
		std::vector<OrbisTexturePak::Entry> entries;
		std::string error;
		CHECK(OrbisTexturePak::ReadIndex(fd, entries, error));
		std::map<std::string, const OrbisTexturePak::Entry*> by_name;
		for (const OrbisTexturePak::Entry& e : entries)
			by_name[e.name] = &e;
		std::vector<uint8_t> buf(16u << 20);
		uint64_t last_offset = 0, total = 0;
		size_t checked = 0;
		for (const rapidjson::Value& f : doc["files"].GetArray())
		{
			std::string rel = f[0].GetString();
			const size_t at = rel.find("/replacements/");
			rel = at == std::string::npos ? rel : rel.substr(at + 14);
			const auto it = by_name.find(rel);
			CHECK(it != by_name.end() && it->second->size == f[1].GetUint64());
			if (it == by_name.end())
				continue;
			uLong crc = crc32(0, nullptr, 0);
			for (uint64_t done = 0; done < it->second->size;)
			{
				const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), it->second->size - done));
				if (!OrbisTexturePak::PreadAll(fd, buf.data(), n, it->second->offset + done))
				{
					CHECK(false);
					break;
				}
				crc = crc32(crc, buf.data(), static_cast<uInt>(n));
				done += n;
			}
			char hex[16];
			std::snprintf(hex, sizeof(hex), "%08x", static_cast<unsigned>(crc));
			CHECK(std::string(hex) == f[2].GetString());
			last_offset = std::max(last_offset, it->second->offset);
			total += it->second->size;
			checked++;
		}
		std::printf("  %zu of %u entries checked (%llu bytes); the last starts at %llu (past 4 GiB: %s)\n", checked, doc["files"].Size(), static_cast<unsigned long long>(total),
			static_cast<unsigned long long>(last_offset), last_offset > (4ull << 30) ? "yes" : "no");
		CHECK(entries.size() == doc["files"].Size() && checked == entries.size());
		close(fd);
	}
	r.Close();
}

// b176, 4g: the list the app ships (assets/hd-pack-sources.json; test-texpacks.sh names it in HD_PACK_SOURCES): nothing in it is dropped without being counted, and the
// numbers the boot.log gives are the file's own: all of it is offered by default (the .7z ones too), and with the flag texpack_no7z the .7z ones are left out and said so.
void TestShippedList(const std::string& fixtures, const std::string& work)
{
	const char* path = std::getenv("HD_PACK_SOURCES");
	if (!path || !*path)
	{
		std::printf("(HD_PACK_SOURCES is not set: the shipped list is not checked)\n");
		return;
	}
	std::printf("---- the shipped list %s\n", path);
	const std::string json = ReadAll(path);
	rapidjson::Document doc;
	doc.Parse(json.c_str(), json.size());
	CHECK(!doc.HasParseError() && doc.IsObject() && doc.HasMember("packs") && doc["packs"].IsArray());
	if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("packs") || !doc["packs"].IsArray())
		return;
	size_t total = 0, sevens = 0, others = 0;
	std::set<std::string> ids, serials;
	for (const rapidjson::Value& e : doc["packs"].GetArray())
	{
		total++;
		std::string name = e["name"].GetString();
		for (char& c : name)
			c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
		const bool seven = name.size() > 3 && name.compare(name.size() - 3, 3, ".7z") == 0;
		sevens += seven;
		others += !seven;
		CHECK(ids.insert(e["id"].GetString()).second); // a Drive file twice would be hidden by the merge
		CHECK(e["serials"].IsArray() && e["serials"].Size() >= 1);
		for (const rapidjson::Value& sv : e["serials"].GetArray())
			serials.insert(sv.GetString());
	}
	std::vector<TexturePack> packs;
	std::string error;
	int unsupported = 0;
	CHECK(ParseExtraTexturePacks(json, packs, error, &unsupported));
	CHECK(packs.size() == total && unsupported == 0); // every entry is a .rar, .zip or .7z with an id and a serial: none is dropped
	std::printf("  %zu packs (%zu .rar/.zip, %zu .7z), %zu games\n", total, others, sevens, serials.size());
	CHECK(sevens > 0 && others > 0);
	{
		SzRig r(fixtures, work + "/sz", "shipped-all");
		r.OpenJson(json);
		CHECK(r.log.Has("Drive packs in all: " + std::to_string(total) + " offered (" + std::to_string(others) + " rar/zip, " + std::to_string(sevens) + " 7z), 0 left out"));
		size_t offered_serials = 0, both = 0;
		for (const std::string& s : serials)
		{
			const TexturePackStatus st = r.m->Status(s);
			offered_serials += !st.packs.empty();
			// a game with a .rar or .zip and a .7z: the .rar or .zip is the first on the row (the default of build 175 stays the default)
			bool plain = false, seven = false;
			for (const TexturePack& pk : st.packs)
			{
				const bool is7z = pk.name.size() > 3 && pk.name.compare(pk.name.size() - 3, 3, ".7z") == 0;
				plain = plain || !is7z;
				seven = seven || is7z;
			}
			if (plain && seven)
			{
				both++;
				const std::string& first = st.packs[0].name;
				CHECK(!(first.size() > 3 && first.compare(first.size() - 3, 3, ".7z") == 0));
			}
		}
		std::printf("  %zu games have both a .rar/.zip and a .7z pack\n", both);
		CHECK(offered_serials == serials.size()); // every game of the list has its pack(s) on the row
		r.Close();
	}
	{
		SzRig r(fixtures, work + "/sz", "shipped-no7z");
		r.flags.Set("texpack_no7z", true);
		r.OpenJson(json);
		CHECK(r.log.Has("Drive packs in all: " + std::to_string(others) + " offered (" + std::to_string(others) + " rar/zip, 0 7z), " + std::to_string(sevens) + " left out"));
		CHECK(r.log.Has(std::to_string(sevens) + " 7z left out (flag texpack_no7z)"));
		size_t offered_serials = 0;
		for (const std::string& s : serials)
			offered_serials += !r.m->Status(s).packs.empty();
		std::printf("  with texpack_no7z: %zu games keep a pack\n", offered_serials);
		CHECK(offered_serials < serials.size() && offered_serials > 0);
		r.Close();
	}
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
	TestExtraCatalog();
	mkdir(work.c_str(), 0777);
	mkdir((work + "/pakformat").c_str(), 0777);
	TestPakFormat(work + "/pakformat");
	TestSevenZipPacks(fixtures, work); // b176
	TestWriterProcess(fixtures, work); // 2026-10-29
	TestShippedList(fixtures, work);
	TestBigSevenZip(work);
	if (const char* only = std::getenv("TEXPACKS_ONLY_7Z"); only && *only) // (review) a quicker run while working on the .7z path: TEXPACKS_ONLY_7Z=1 test-texpacks.sh
	{
		std::printf(g_failures ? "TEXPACKS TEST (7z cases only): %d failure(s)\n" : "TEXPACKS TEST PASS (7z cases only)\n", g_failures);
		return g_failures ? 1 : 0;
	}

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
		for (const double until = Now() + 3; Now() < until && Snapshot(popups).empty();) // (made a moment after the pack shows as installed)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		{
			const std::vector<std::string> shown = Snapshot(popups);
			CHECK(shown.size() == 1 && shown[0].find("ready: Spider-Man 2") == 0);
		}
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
	// 4b. 2.11: Google Drive. A Drive-only serial lists and installs (size from the probe, no checksum); the marker says Drive; a pack of
	// archive.org's serial lists both; when archive.org's list fails the Drive packs still list; a probe that says no fails it, and the
	// next try goes through; a broken download is unpacked into a failure and deleted, so the next try fetches it afresh.
	{
		FakeArchive drive;
		drive.dir = fixtures;
		drive.list = server.list;
		drive.drive_files["DRIVEFILE00000001"] = "drive-only.zip"; // SLUS-33333/replacements/x.png (test-texpacks.sh makes it)
		drive.drive_files["DRIVEFILE00000002"] = good; // another maker's pack of the same game
		{
			std::ofstream(fixtures + "/not-an-archive.rar", std::ios::binary) << std::string(200000, 'x');
			drive.drive_files["DRIVEFILE00000003"] = "not-an-archive.rar";
		}
		const std::string sources =
			"{\"version\":1,\"packs\":["
			"{\"id\":\"DRIVEFILE00000001\",\"name\":\"Drive Only PS2 Upscale Textures [SLUS-33333].zip\",\"title\":\"Drive Only\",\"label\":\"CCKrizalid\",\"maker\":\"CCKrizalid\",\"serials\":[\"SLUS-33333\"]},"
			"{\"id\":\"DRIVEFILE00000002\",\"name\":\"Spider-Man 2 [SLUS-20776].zip\",\"title\":\"Spider-Man 2\",\"label\":\"CCKrizalid\",\"maker\":\"CCKrizalid\",\"serials\":[\"SLUS-20776\"]},"
			"{\"id\":\"DRIVEFILE00000003\",\"name\":\"Garbage [SLUS-44444].rar\",\"title\":\"Garbage\",\"label\":\"Bl4ckH4nd\",\"maker\":\"Bl4ckH4nd\",\"serials\":[\"SLUS-44444\"]}]}";
		std::vector<std::string> log;
		TexturePackManager m(drive.Platform(&log, nullptr), textures, downloads, cache);
		m.SetExtraSources(sources, "");
		m.Start();
		CHECK(WaitFor(m, "SLUS-33333", State::Available, 5));
		TexturePackStatus st = m.Status("SLUS-33333");
		CHECK(st.packs.size() == 1 && st.packs[0].OnDrive() && st.packs[0].bytes == 0);
		st = m.Status("SLUS-20776"); // archive.org's first, then Drive's
		CHECK(st.state == State::Available && st.packs.size() == 2 && !st.packs[0].OnDrive() && st.packs[1].OnDrive());
		CHECK(m.Begin("SLUS-33333", 0, "", "", "Drive Only"));
		CHECK(WaitFor(m, "SLUS-33333", State::Installed, 20));
		CHECK(drive.probes == 1 && drive.drive_ranges >= 1);
		std::map<std::string, std::string> pak;
		CHECK(ReadPak(textures + "/SLUS-33333/replacements.pak", pak) && pak.size() == 1 && pak["x.png"] == "a drive texture");
		const std::string marker = ReadAll(textures + "/SLUS-33333/" + TexturePackManager::kMarker);
		CHECK(marker.find("Google Drive") != std::string::npos && marker.find("source=https://drive.google.com/file/d/DRIVEFILE00000001/view") != std::string::npos &&
			marker.find("md5=") == std::string::npos);
		CHECK(!Exists(downloads + "/gDRIVEFILE00000001.part") && !Exists(downloads + "/gDRIVEFILE00000001.zip") && !Exists(downloads + "/SLUS-33333.job"));
		CHECK(m.Remove("SLUS-33333"));
		CHECK(WaitFor(m, "SLUS-33333", State::Available, 5));
		// A probe Drive refuses (quota): Failed with its words; then it goes through.
		drive.probe_status = 429;
		CHECK(m.Begin("SLUS-20776", 1, "", "", "Spider-Man 2"));
		CHECK(WaitFor(m, "SLUS-20776", State::Failed, 10));
		CHECK(m.Status("SLUS-20776").message.find("Google Drive") != std::string::npos);
		drive.probe_status = 206;
		drive.probe_type = "text/html; charset=utf-8"; // a page where the file was asked
		CHECK(m.Begin("SLUS-20776", 1, "", "", "Spider-Man 2"));
		CHECK(WaitFor(m, "SLUS-20776", State::Failed, 10));
		drive.probe_type = "application/octet-stream";
		CHECK(m.Begin("SLUS-20776", 1, "", "", "Spider-Man 2"));
		CHECK(WaitFor(m, "SLUS-20776", State::Installed, 20));
		CHECK(m.Remove("SLUS-20776"));
		CHECK(WaitFor(m, "SLUS-20776", State::Available, 5));
		// A download that is no archive: the unpacking fails, the download is deleted, nothing is installed.
		CHECK(m.Begin("SLUS-44444", 0, "", "", "Garbage"));
		CHECK(WaitFor(m, "SLUS-44444", State::Failed, 20));
		CHECK(!Exists(downloads + "/gDRIVEFILE00000003.rar") && !Exists(textures + "/SLUS-44444"));
		CHECK(m.Cancel("SLUS-44444"));
		CHECK(m.Stop(2000));
		// archive.org's list fails (offline, nothing kept): Drive's packs still list and start.
		FakeArchive offline;
		offline.dir = fixtures;
		offline.drive_files = drive.drive_files;
		TexturePackManager o(offline.Platform(nullptr, nullptr), textures, downloads, work + "/cache/none-drive.json");
		o.SetExtraSources(sources, "");
		o.Start();
		CHECK(WaitFor(o, "SLUS-33333", State::Available, 5)); // not Unavailable
		CHECK(o.Status("SLUS-12345").state == State::Unavailable);
		CHECK(o.Begin("SLUS-33333", 0, "", "", "Drive Only"));
		CHECK(WaitFor(o, "SLUS-33333", State::Installed, 20));
		CHECK(o.Remove("SLUS-33333"));
		CHECK(WaitFor(o, "SLUS-33333", State::Available, 5));
		CHECK(o.Stop(2000));
	}
	// 4c. 2.11 (swordpdf: "the hd texture packs sources didn't get updated"): the project's current Drive list is fetched when the worker starts: its new packs list
	// (and start), it is kept for the next start (listed before the worker runs, offline), not fetched again within 6 hours, a list that is no list
	// is not used, and the flag nohdsources stops the fetch.
	{
	FakeArchive fx;
	fx.dir = fixtures;
	fx.list = server.list;
	fx.drive_files["DRIVEFILE00000011"] = "drive-only.zip";
	fx.drive_files["DRIVEFILE00000012"] = "drive-only.zip";
	const std::string old_list =
	"{\"version\":1,\"packs\":["
	"{\"id\":\"DRIVEFILE00000011\",\"name\":\"Drive Only PS2 Upscale Textures [SLUS-33333].zip\",\"title\":\"Drive Only\",\"label\":\"CCKrizalid\",\"maker\":\"CCKrizalid\",\"serials\":[\"SLUS-33333\"]}]}";
	fx.hosted =
	"{\"version\":1,\"packs\":["
	"{\"id\":\"DRIVEFILE00000011\",\"name\":\"Drive Only PS2 Upscale Textures [SLUS-33333].zip\",\"title\":\"Drive Only\",\"label\":\"CCKrizalid\",\"maker\":\"CCKrizalid\",\"serials\":[\"SLUS-33333\"]},"
	"{\"id\":\"DRIVEFILE00000012\",\"name\":\"Newer Game PS2 Upscale Textures [SLUS-55555].zip\",\"title\":\"Newer Game\",\"label\":\"CCKrizalid\",\"maker\":\"CCKrizalid\",\"serials\":[\"SLUS-55555\"]}]}";
	fx.hosted_status = 200;
	const std::string kept = work + "/cache/hd-pack-sources.fetched.json";
	const std::string url = "https://example.test/hd-pack-sources.json";
	std::remove(kept.c_str());
	{
	TexturePackManager m(fx.Platform(nullptr, nullptr), textures, downloads, cache);
	m.SetExtraSources(old_list, "", kept, url);
	CHECK(m.Status("SLUS-55555").packs.empty()); // the list built in doesn't have it
	m.Start();
	const double until = Now() + 8;
	while (Now() < until && m.Status("SLUS-55555").packs.empty())
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	CHECK(m.Status("SLUS-55555").packs.size() == 1 && m.Status("SLUS-55555").packs[0].OnDrive());
	CHECK(m.Status("SLUS-33333").packs.size() == 1); // the same Drive file twice stays once
	CHECK(fx.hosted_requests == 1 && Exists(kept));
	CHECK(ReadAll(kept) == fx.hosted);
	CHECK(m.Stop(2000));
	}
	// the next start: offline, the kept list is read before the worker runs; it is fresh, so nothing is fetched
	{
	fx.hosted_status = 500;
	fx.hosted_requests = 0;
	TexturePackManager m(fx.Platform(nullptr, nullptr), textures, downloads, cache);
	m.SetExtraSources(old_list, "", kept, url);
	CHECK(m.Status("SLUS-55555").packs.size() == 1);
	m.Start();
	CHECK(WaitFor(m, "SLUS-33333", State::Available, 5));
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	CHECK(fx.hosted_requests == 0);
	CHECK(m.Stop(2000));
	}
	// a copy older than 6 hours is fetched again; garbage is not used (the kept copy stays)
	{
	const std::string before = ReadAll(kept);
	utimbuf old_time;
	old_time.actime = old_time.modtime = std::time(nullptr) - 7 * 3600;
	CHECK(utime(kept.c_str(), &old_time) == 0);
	fx.hosted = "this is not a list";
	fx.hosted_status = 200;
	fx.hosted_requests = 0;
	TexturePackManager m(fx.Platform(nullptr, nullptr), textures, downloads, cache);
	m.SetExtraSources(old_list, "", kept, url);
	m.Start();
	const double until = Now() + 8;
	while (Now() < until && fx.hosted_requests == 0)
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	CHECK(fx.hosted_requests == 1);
	CHECK(ReadAll(kept) == before && m.Status("SLUS-55555").packs.size() == 1);
	CHECK(m.Stop(2000));
	}
	// the flag: no fetch at all
	{
	fx.hosted_requests = 0;
	utimbuf old_time;
	old_time.actime = old_time.modtime = std::time(nullptr) - 7 * 3600;
	CHECK(utime(kept.c_str(), &old_time) == 0);
	TexturePackPlatform platform = fx.Platform(nullptr, nullptr);
	platform.flag = [](const char* name) { return std::string(name) == "nohdsources"; };
	TexturePackManager m(platform, textures, downloads, cache);
	m.SetExtraSources(old_list, "", kept, url);
	m.Start();
	CHECK(WaitFor(m, "SLUS-33333", State::Available, 5));
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	CHECK(fx.hosted_requests == 0);
	CHECK(m.Stop(2000));
	}
		// a fetch that failed is not tried again for an hour (the console that can't reach the host asks once an hour, not at every start)
		{
			std::remove(kept.c_str());
			std::remove((kept + ".tried").c_str());
			fx.hosted_status = 500;
			fx.hosted_requests = 0;
			{
				TexturePackManager m(fx.Platform(nullptr, nullptr), textures, downloads, cache);
				m.SetExtraSources(old_list, "", kept, url);
				m.Start();
				const double until = Now() + 8;
				while (Now() < until && fx.hosted_requests == 0)
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
		CHECK(fx.hosted_requests == 1 && !Exists(kept) && Exists(kept + ".tried"));
				CHECK(m.Stop(2000));
			}
			{
				TexturePackManager m(fx.Platform(nullptr, nullptr), textures, downloads, cache);
				m.SetExtraSources(old_list, "", kept, url);
				m.Start();
				CHECK(WaitFor(m, "SLUS-33333", State::Available, 5));
				std::this_thread::sleep_for(std::chrono::milliseconds(300));
				CHECK(fx.hosted_requests == 1); // no second request
				CHECK(m.Stop(2000));
			}
			std::remove((kept + ".tried").c_str());
			fx.hosted_status = 200;
		}
}
	// 4d. 2.11: a pack whose entries run across the pack writer's 8 MiB buffers (exactly 8 MiB, one over, 20 MiB, one byte, none, three of 3 MiB): streamed in block by
	// block, every entry intact and in the pack, the folder installed.
	{
	FakeArchive fx;
	fx.dir = fixtures;
	fx.list = server.list;
	fx.drive_files["DRIVEFILE00000021"] = "big.zip";
	const std::string sources =
	"{\"version\":1,\"packs\":[{\"id\":\"DRIVEFILE00000021\",\"name\":\"Big PS2 Upscale Textures [SLUS-77777].zip\",\"title\":\"Big\",\"label\":\"CCKrizalid\",\"maker\":\"CCKrizalid\",\"serials\":[\"SLUS-77777\"]}]}";
	TexturePackManager m(fx.Platform(nullptr, nullptr), textures, downloads, cache);
	m.SetExtraSources(sources, "");
	m.Start();
	CHECK(WaitFor(m, "SLUS-77777", State::Available, 5));
	CHECK(m.Begin("SLUS-77777", 0, "", "", "Big"));
	CHECK(WaitFor(m, "SLUS-77777", State::Installed, 60));
	std::map<std::string, std::string> pak;
	size_t entries = 0;
	CHECK(ReadPak(textures + "/SLUS-77777/replacements.pak", pak, &entries) && entries == 8);
	std::istringstream manifest(ReadAll(fixtures + "/big.manifest"));
	std::string name, md5;
	size_t size = 0, checked = 0;
	while (manifest >> name >> size >> md5)
	{
	const auto it = pak.find(name);
	CHECK(it != pak.end() && it->second.size() == size);
	if (it != pak.end() && it->second.size() == size)
	{
	CHECK(TexturePackMd5(it->second.data(), it->second.size()) == md5);
	checked++;
	}
	}
	CHECK(checked == 8);
	CHECK(m.Remove("SLUS-77777"));
	CHECK(WaitFor(m, "SLUS-77777", State::Available, 5));
	CHECK(m.Stop(2000));
	}
	// 4e. (code review) A Drive download that fails to unpack because of a LOCAL fault (the textures folder can't be written) is kept for the next try;
	// only a damaged archive is deleted (4b).
	{
		FakeArchive fx;
		fx.dir = fixtures;
		fx.list = server.list;
		fx.drive_files["DRIVEFILE00000031"] = "drive-only.zip";
		const std::string sources =
			"{\"version\":1,\"packs\":[{\"id\":\"DRIVEFILE00000031\",\"name\":\"Locked PS2 Upscale Textures [SLUS-88888].zip\",\"title\":\"Locked\",\"label\":\"CCKrizalid\",\"maker\":\"CCKrizalid\",\"serials\":[\"SLUS-88888\"]}]}";
		const std::string locked = work + "/locked-textures";
		mkdir(locked.c_str(), 0755);
		TexturePackManager m(fx.Platform(nullptr, nullptr), locked, downloads, cache);
		m.SetExtraSources(sources, "");
		m.Start();
		CHECK(WaitFor(m, "SLUS-88888", State::Available, 5));
		chmod(locked.c_str(), 0555);
		CHECK(m.Begin("SLUS-88888", 0, "", "", "Locked"));
		CHECK(WaitFor(m, "SLUS-88888", State::Failed, 20));
		const int ranges = fx.drive_ranges;
		CHECK(Exists(downloads + "/gDRIVEFILE00000031.zip")); // kept
		chmod(locked.c_str(), 0755);
		CHECK(m.Begin("SLUS-88888", 0, "", "", "Locked"));
		CHECK(WaitFor(m, "SLUS-88888", State::Installed, 20));
		CHECK(fx.drive_ranges == ranges); // the second try used the file it had
		CHECK(m.Remove("SLUS-88888"));
		CHECK(WaitFor(m, "SLUS-88888", State::Available, 5));
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
		for (const std::string& l : Snapshot(log))
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
		for (const double until = Now() + 5; Now() < until;) // ("deleted N files set aside" is logged just after the last folder is gone)
		{
			set_aside = deleted = 0;
			for (const std::string& l : Snapshot(log))
			{
				set_aside += l.find("] set aside ") != std::string::npos;
				deleted += l.find("] deleted ") != std::string::npos && l.find("files set aside") != std::string::npos;
			}
			if (set_aside >= 2 && deleted >= 1)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
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
