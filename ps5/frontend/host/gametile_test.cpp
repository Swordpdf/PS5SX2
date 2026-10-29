// PS5 port frontend, PC test (2.12, AI-assisted; swordpdf: "add an option to add a game from the shelf to the xmb"): fe_gametile.h, the one header that holds every
// rule of the Home screen tile row: the request files the shelf writes (one per press), alive.txt and state.txt as the helper writes them, the row's state machine
// (Tracker) in virtual time, the polling policy, and the tile's own start (marker, launch check, own folder). The formats are plans/gametiles_contract.md's sections 4 to 9.
//   test-gametile.sh builds this with fe_i18n.cpp (the two notification texts) and runs it.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../fe_gametile.h"
#include "../fe_i18n.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace fe::gametile;

namespace
{
int fails = 0, checks = 0;

void Check(bool ok, const std::string& what, const std::string& detail = std::string())
{
	checks++;
	if (!ok)
	{
		fails++;
		std::printf("FAIL: %s%s%s\n", what.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
	}
}

template <class A, class B>
void Eq(const A& got, const B& want, const std::string& what)
{
	std::ostringstream g, w;
	g << got;
	w << want;
	Check(g.str() == w.str(), what, "got [" + g.str() + "], wanted [" + w.str() + "]");
}

std::string Show(const std::string& s)
{
	std::string out;
	for (const unsigned char c : s)
	{
		if (c == '\t')
			out += "<TAB>";
		else if (c == '\n')
			out += "<LF>\n";
		else if (c == '\r')
			out += "<CR>";
		else if (c < 0x20 || c == 0x7F)
		{
			char b[8];
			std::snprintf(b, sizeof(b), "<%02X>", c);
			out += b;
		}
		else
			out += static_cast<char>(c);
	}
	return out;
}

std::string Read(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}

void Write(const std::string& path, const std::string& text)
{
	std::ofstream f(path, std::ios::binary);
	f << text;
}

bool Exists(const std::string& p)
{
	struct stat st;
	return stat(p.c_str(), &st) == 0;
}

void MakeDir(const std::string& p)
{
	mkdir(p.c_str(), 0777);
}

void SetMtime(const std::string& path, long long t)
{
	const struct timeval tv[2] = {{static_cast<time_t>(t), 0}, {static_cast<time_t>(t), 0}};
	utimes(path.c_str(), tv);
}

std::vector<std::string> List(const std::string& dir)
{
	std::vector<std::string> out;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (const dirent* e = readdir(d))
			if (e->d_name[0] != '.')
				out.push_back(e->d_name);
		closedir(d);
	}
	std::sort(out.begin(), out.end());
	return out;
}

// A stand-in for fe::IsDiscImageName (fe_games.cpp, which this test does not link): the extensions the shelf lists.
bool ImageName(const char* name)
{
	const std::string n = name;
	for (const char* ext : {".iso", ".chd", ".cso", ".zso", ".bin", ".img"})
		if (HasExtension(n, ext))
			return n[0] != '.';
	return false;
}

const std::string kGame = "/data/PCSX2/games/God of War (USA).iso";
const std::string kGame2 = "/mnt/usb0/PS2/Okami.iso";

std::string Line(const std::string& id, const std::string& path, const std::string& serial, const std::string& title, const std::string& status)
{
	return id + "\t" + path + "\t" + serial + "\t" + title + "\t" + status + "\n";
}

// ---- text ---------------------------------------------------------------------------------------------------------------------------------------------------------

void TestText()
{
	Eq(NormalizePath("/data//PCSX2///games//a.iso/"), "/data/PCSX2/games/a.iso", "NormalizePath collapses // and strips the trailing /");
	Eq(NormalizePath("/"), "/", "NormalizePath keeps a lone /");
	Eq(NormalizePath("/data/"), "/data", "NormalizePath: /data/ -> /data");
	Eq(NormalizePath(""), "", "NormalizePath of nothing");

	// PathOk: the helper's gt_path_ok after its gt_norm_path.
	struct Row
	{
		std::string path;
		bool ok;
		const char* what;
	};
	const std::string a510 = "/data/" + std::string(504, 'a'), a511 = "/data/" + std::string(505, 'a'), a512 = "/data/" + std::string(506, 'a');
	const std::vector<Row> rows = {
		{kGame, true, "a game in the games folder"},
		{kGame2, true, "a USB game"},
		{"/data/x", true, "one byte after /data/"},
		{"/mnt/x", true, "one byte after /mnt/"},
		{"/mnt/ext0/PS2/a.chd", true, "extended storage"},
		{"/data/", false, "/data/ alone"},
		{"/data", false, "/data without the slash"},
		{"/data//", false, "/data// is /data"},
		{"/mnt/", false, "/mnt/ alone"},
		{"/mnt", false, "/mnt"},
		{"/dev/cd0", false, "a disc drive has its own tile"},
		{"/dev/cd0/x", false, "under /dev"},
		{"/nfs/host/share/game.iso", false, "an NFS share (virtual to this process)"},
		{"/app0/eboot.bin", false, "not /data or /mnt"},
		{"/datax/y", false, "/datax is not /data/"},
		{"/mntx/y", false, "/mntx is not /mnt/"},
		{"relative/x.iso", false, "relative"},
		{"data/x.iso", false, "no leading slash"},
		{"", false, "empty"},
		{a510, true, "510 bytes"},
		{a511, true, "511 bytes"},
		{a512, false, "512 bytes"},
		{"/data/a/../b.iso", false, ".. in the middle"},
		{"/data/a/..", false, ".. at the end"},
		{"/data/../etc/x", false, ".. right after /data"},
		{"/data/..hidden/x.iso", true, "a name that starts with .."},
		{"/data/a..b/x.iso", true, ".. inside a name"},
		{"/data/.../x.iso", true, "three dots"},
		{"/data/a/.../x.iso", true, "three dots after a folder"},
		{"/data/a\tb.iso", false, "a TAB"},
		{"/data/a\nb.iso", false, "a line break"},
		{"/data/a\rb.iso", false, "a CR"},
		{std::string("/data/a") + '\x01' + "b.iso", false, "a control byte"},
		{std::string("/data/a") + '\x7F' + "b.iso", false, "DEL"},
		{"/data/Pok\xC3\xA9mon.iso", true, "UTF-8 in the name"},
		{"/data/PCSX2/games//a.iso/", true, "double and trailing slashes are normalized away"},
		{"/data/PCSX2/games/God of War (USA) [v1.0].iso", true, "spaces, parentheses and brackets"},
		{"/data/a//../b.iso", false, ".. after a double slash"},
		{"//data/x", true, "a leading // is collapsed first"},
		{"/data//PCSX2///x.iso", true, "inner // collapsed"},
	};
	for (const Row& r : rows)
	{
		std::string why;
		const bool ok = PathOk(r.path, &why);
		Check(ok == r.ok, std::string("PathOk: ") + r.what, "[" + Show(r.path.substr(0, 60)) + "] -> " + (ok ? "ok" : why));
		if (!r.ok)
			Check(!why.empty(), std::string("PathOk gives a reason: ") + r.what);
	}

	// CleanSerial: [A-Za-z0-9._-]{1,15} or empty.
	Eq(CleanSerial("SCUS-97399"), "SCUS-97399", "serial ok");
	Eq(CleanSerial("SLES_501.23"), "SLES_501.23", "serial with _ and .");
	Eq(CleanSerial(std::string(15, 'A')), std::string(15, 'A'), "a serial of 15");
	Eq(CleanSerial(std::string(16, 'A')), "", "a serial of 16 is not sent");
	Eq(CleanSerial("SCUS+97399"), "", "a serial with +");
	Eq(CleanSerial("SCUS 97399"), "", "a serial with a space");
	Eq(CleanSerial("SCUS\t97399"), "", "a serial with a TAB");
	Eq(CleanSerial("SC\xC3\xA9S"), "", "a serial with UTF-8");
	Eq(CleanSerial(""), "", "no serial");

	// CleanTitle.
	Eq(CleanTitle("  God of War  "), "God of War", "title trimmed");
	Eq(CleanTitle("God\tof\nWar\r"), "God of War", "TAB, LF and CR become spaces (trimmed at the end)");
	Eq(CleanTitle("A\x01\x02" "B"), "A  B", "control bytes become spaces");
	Eq(CleanTitle("\t\n "), "", "only blanks: empty");
	Eq(CleanTitle("Pok\xC3\xA9mon"), "Pok\xC3\xA9mon", "UTF-8 kept");
	Eq(CleanTitle(std::string(120, 'x')), std::string(120, 'x'), "120 bytes stay");
	Eq(CleanTitle(std::string(121, 'x')), std::string(120, 'x'), "121 bytes are cut at 120");
	auto valid_utf8 = [](const std::string& s) {
		for (size_t i = 0; i < s.size();)
		{
			const unsigned char c = static_cast<unsigned char>(s[i]);
			const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
			if (!n || i + n > s.size())
				return false;
			for (size_t k = 1; k < n; k++)
				if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
					return false;
			i += n;
		}
		return true;
	};
	const std::string two = "\xC3\xA9", three = "\xE2\x82\xAC", four = "\xF0\x9F\x98\x80"; // e-acute, euro, grinning face
	for (const std::string& ch : {two, three, four})
		for (size_t pad = 110; pad <= 121; pad++)
		{
			const std::string t = CleanTitle(std::string(pad, 'a') + ch + ch + ch + "tail");
			Check(t.size() <= 120 && valid_utf8(t), "a title of " + std::to_string(pad) + " a's and " + std::to_string(ch.size()) + "-byte characters is cut on a boundary", Show(t));
		}
	Eq(CleanTitle(std::string(119, 'a') + two + "z").size(), 119, "a 2-byte character that would cross 120 goes whole");
	Eq(CleanTitle(std::string(118, 'a') + three + "z").size(), 118, "a 3-byte character that would cross 120 goes whole");
	Eq(CleanTitle(std::string(117, 'a') + four + "z").size(), 117, "a 4-byte character that would cross 120 goes whole");
	Eq(CleanTitle(std::string(118, 'a') + two + "z").size(), 120, "a 2-byte character that ends at 120 stays");
	Eq(CleanTitle(std::string(119, 'a') + " b").size(), 119, "a trailing space left by the cut goes too");

	// TitleFor: never empty.
	Eq(TitleFor("God of War", kGame), "God of War", "TitleFor: the shelf's title");
	Eq(TitleFor("", kGame), "God of War (USA)", "TitleFor: no title: the file's name without its extension");
	Eq(TitleFor("\t \n", kGame), "God of War (USA)", "TitleFor: a blank title is none");
	Eq(TitleFor("", "/data/x/\t.iso"), "Game", "TitleFor: nothing usable: a word");

	// Covers.
	Check(CoverPathOk("/data/PCSX2/cache/covers/SCUS-97399.jpg"), "CoverPathOk: a cache cover");
	Check(!CoverPathOk(""), "CoverPathOk: empty");
	Check(!CoverPathOk("covers/a.jpg"), "CoverPathOk: relative");
	Check(!CoverPathOk("/data/a/../b.jpg"), "CoverPathOk: ..");
	Check(!CoverPathOk("/data/a/b..c.jpg"), "CoverPathOk: .. anywhere (the helper is stricter than for games)");
	Check(CoverPathOk("/" + std::string(298, 'a')), "CoverPathOk: 299 bytes");
	Check(!CoverPathOk("/" + std::string(299, 'a')), "CoverPathOk: 300 bytes");
	Check(!CoverPathOk("/data/a\tb.jpg"), "CoverPathOk: a TAB");
	Check(!CoverPathOk("/data/a\nb.jpg"), "CoverPathOk: a line break");
	Check(RowEligible(kGame, "God of War (USA).iso", ""), "RowEligible: a plain image");
	Check(!RowEligible(kGame, "God of War (USA).iso", "invalid data"), "RowEligible: a damaged image");
	Check(!RowEligible("/data/x/Game.elf", "Game.elf", ""), "RowEligible: an ELF");
	Check(!RowEligible("/data/x/Game.ELF", "Game.ELF", ""), "RowEligible: an ELF in capitals");
	Check(!RowEligible("/dev/cd0", "cd0", ""), "RowEligible: a disc drive");
	Check(!RowEligible("/nfs/h/s/Game.iso", "Game.iso", ""), "RowEligible: an NFS share");

	// Tile ids.
	for (int n = 99251; n <= 99260; n++)
		Check(TileIdInRange("PPSA" + std::to_string(n)), "TileIdInRange PPSA" + std::to_string(n));
	for (const char* bad : {"PPSA99250", "PPSA99261", "PPSA99230", "PPSA99231", "PPSA99203", "ppsa99251", "PPSA9925", "PPSA992510", "PPSA9925x", "", "99251"})
		Check(!TileIdInRange(bad), std::string("TileIdInRange refuses [") + bad + "]");
	Eq(TileIdOf(0), "PPSA99251", "TileIdOf(0)");
	Eq(TileIdOf(9), "PPSA99260", "TileIdOf(9)");
}

// ---- the request files --------------------------------------------------------------------------------------------------------------------------------------

void TestFormat()
{
	Eq(FormatAdd(kGame, "SCUS-97399", "God of War"), "v1\nadd\t" + kGame + "\tSCUS-97399\tGod of War\n", "FormatAdd: exact bytes (4 fields)");
	Eq(FormatAdd(kGame2, "", "Okami"), "v1\nadd\t" + kGame2 + "\t\tOkami\n", "FormatAdd: no serial is two TABs");
	Eq(FormatAdd(kGame, "SCUS-97399", "God of War", "/data/PCSX2/cache/covers/SCUS-97399.jpg"),
		"v1\nadd\t" + kGame + "\tSCUS-97399\tGod of War\t/data/PCSX2/cache/covers/SCUS-97399.jpg\n", "FormatAdd: the cover is the fifth field");
	Eq(FormatAdd(kGame, "SCUS-97399", "God of War", "/data/a/../b.jpg"), "v1\nadd\t" + kGame + "\tSCUS-97399\tGod of War\n", "FormatAdd: an unfit cover is left out");
	Eq(FormatAdd("/data/PCSX2//games/God of War (USA).iso/", "SCUS-97399", "God of War"), "v1\nadd\t" + kGame + "\tSCUS-97399\tGod of War\n", "FormatAdd: the path is sent normalized");
	Eq(FormatAdd(kGame, "", "Tab\there\nand\rthere"), "v1\nadd\t" + kGame + "\t\tTab here and there\n", "FormatAdd: a title with TAB, LF, CR is one line");
	Eq(FormatAdd(kGame, "SCUS+1", ""), "v1\nadd\t" + kGame + "\t\tGod of War (USA)\n", "FormatAdd: an unfit serial is dropped, a missing title comes from the file");
	Eq(FormatAdd("/dev/cd0", "SLUS-1", "Disc"), "", "FormatAdd: a path the helper refuses gives nothing");
	Eq(FormatAdd("/data/a\tb.iso", "", "x"), "", "FormatAdd: a TAB in the path gives nothing");
	Eq(FormatRemove(kGame), "v1\nremove\t" + kGame + "\n", "FormatRemove: by path");
	Eq(FormatRemove("PPSA99251"), "v1\nremove\tPPSA99251\n", "FormatRemove: by tile id");
	Eq(FormatRemove("PPSA99203"), "", "FormatRemove: PPSA99203 is no tile id and no path");
	Eq(FormatRemove("/etc/passwd"), "", "FormatRemove: a path the helper refuses");
	// A request is small: the longest possible line stays far under the helper's 8192 bytes.
	const std::string longest = FormatAdd("/data/" + std::string(505, 'p'), std::string(15, 'S'), std::string(400, 't'), "/" + std::string(298, 'c'));
	Check(!longest.empty() && longest.size() < 1200, "the longest request is under 1200 bytes", std::to_string(longest.size()));
}

void TestQueue(const std::string& root)
{
	const std::string dir = root + "/q";
	MakeDir(dir);
	// The names: 13 digits of milliseconds, a 3-digit counter; the first call of the process is counter 000.
	std::string a, b, c, err;
	const std::string text = FormatAdd(kGame, "SCUS-97399", "God of War");
	Check(QueueRequest(dir, text, &a, &err, 1760611234567LL), "QueueRequest: the first press", err);
	Check(QueueRequest(dir, text, &b, &err, 1760611234567LL), "QueueRequest: the second in the same millisecond", err);
	Check(QueueRequest(dir, text, &c, &err, 1760611234567LL), "QueueRequest: the third", err);
	Eq(a, "request-1760611234567-000.txt", "the first file's name");
	Eq(b, "request-1760611234567-001.txt", "the second");
	Eq(c, "request-1760611234567-002.txt", "the third");
	Check(a < b && b < c, "names sort in the order of the presses");
	Eq(Read(dir + "/" + a), text, "the file holds the request, byte for byte");
	Eq(List(dir).size(), 3, "three files and no .tmp left");
	struct stat st = {};
	stat((dir + "/" + a).c_str(), &st);
	Check((st.st_mode & 0666) == 0666, "mode 0666", std::to_string(st.st_mode & 0777));
	Eq(RequestName(1760611234567LL, 999), "request-1760611234567-999.txt", "counter 999");
	Eq(RequestName(1760611234567LL, 1000), "request-1760611234567-000.txt", "the counter wraps at 1000");
	Eq(RequestName(5, 7), "request-0000000000005-007.txt", "13 digits, 3 digits");
	// Another file name than the helper's patterns is never made: the helper takes request-*.txt only.
	for (const std::string& n : List(dir))
		Check(n.compare(0, 8, "request-") == 0 && n.compare(n.size() - 4, 4, ".txt") == 0, "every file is request-*.txt: " + n);

	// Presence, taking back, and the race (ENOENT = the helper took it just now).
	Check(RequestPresent(dir, a), "RequestPresent: there");
	Check(WithdrawFile(dir, a) == WithdrawResult::Withdrawn, "WithdrawFile: taken back");
	Check(!RequestPresent(dir, a), "RequestPresent: gone");
	Check(WithdrawFile(dir, a) == WithdrawResult::AlreadyTaken, "WithdrawFile: a second time is ENOENT: it was taken just now");
	Check(WithdrawFile(root + "/no-such-dir", a) == WithdrawResult::AlreadyTaken, "WithdrawFile: no folder is ENOENT too");
	// No folder: no file, the reason in words, nothing created (the shelf never makes the helper's folder).
	err.clear();
	Check(!QueueRequest(root + "/no-such-dir", text, &a, &err), "QueueRequest: no folder fails");
	Check(!err.empty() && !Exists(root + "/no-such-dir"), "QueueRequest: the error says why, and makes no folder", err);
	Write(root + "/afile", "x");
	err.clear();
	Check(!QueueRequest(root + "/afile", text, &a, &err) && !err.empty(), "QueueRequest: a folder that is a file fails", err);

	// App::Shutdown: the files this process wrote and older than 3 s are taken back, younger ones stay.
	const std::string d2 = root + "/shutdown";
	MakeDir(d2);
	const double t0 = WallSeconds();
	std::string f1, f2;
	QueueRequest(d2, text, &f1, &err);
	QueueRequest(d2, text, &f2, &err);
	Eq(WithdrawOwn(d2, kShutdownWithdrawS, t0 + 1.0), 0, "WithdrawOwn: files 1 s old stay");
	Eq(List(d2).size(), 2, "...both are still there");
	Eq(WithdrawOwn(d2, kShutdownWithdrawS, WallSeconds() + 4.0), 2, "WithdrawOwn: files 4 s old go");
	Eq(List(d2).size(), 0, "...and are gone");
	Eq(WithdrawOwn(d2, kShutdownWithdrawS, WallSeconds() + 4.0), 0, "WithdrawOwn: again: nothing");
	// One the helper took meanwhile: not an error; another folder's files are not touched.
	QueueRequest(d2, text, &f1, &err);
	QueueRequest(dir, text, &f2, &err);
	std::remove((d2 + "/" + f1).c_str());
	Eq(WithdrawOwn(d2, 0.0, WallSeconds() + 10.0), 0, "WithdrawOwn: a file the helper took is no error");
	Check(Exists(dir + "/" + f2), "WithdrawOwn: another folder's file stays");
	Eq(WithdrawOwn(dir, 0.0, WallSeconds() + 10.0), 3, "WithdrawOwn: ...and goes when its folder is asked (with the two of the first presses that are still there)");
	Eq(List(dir).size(), 0, "WithdrawOwn: that folder is empty now");

	// 8 threads x 50 presses: every file whole, none lost, no .tmp left, every name different.
	const std::string d3 = root + "/threads";
	MakeDir(d3);
	std::vector<std::thread> pool;
	std::atomic<int> failed{0};
	std::vector<std::vector<std::string>> names(8);
	for (int t = 0; t < 8; t++)
		pool.emplace_back([&, t] {
			for (int i = 0; i < 50; i++)
			{
				std::string name, e;
				const std::string body = "v1\nadd\t/data/g" + std::to_string(t) + "/" + std::to_string(i) + ".iso\t\tT" + std::to_string(t) + "-" + std::to_string(i) + "\n";
				if (!QueueRequest(d3, body, &name, &e))
					failed++;
				else
					names[static_cast<size_t>(t)].push_back(name + "|" + body);
			}
		});
	for (std::thread& th : pool)
		th.join();
	Eq(failed.load(), 0, "threads: every press was written");
	std::set<std::string> seen;
	int torn = 0, total = 0;
	for (const auto& v : names)
		for (const std::string& nb : v)
		{
			const size_t bar = nb.find('|');
			const std::string name = nb.substr(0, bar), body = nb.substr(bar + 1);
			total++;
			seen.insert(name);
			if (Read(d3 + "/" + name) != body)
				torn++;
		}
	Eq(total, 400, "threads: 400 presses");
	Eq(seen.size(), 400, "threads: 400 different names");
	Eq(torn, 0, "threads: no torn or mixed file");
	Eq(List(d3).size(), 400, "threads: 400 files and no .tmp");
}

// ---- alive.txt and state.txt -------------------------------------------------------------------------------------------------------------------------------

void TestHelperFiles(const std::string& root)
{
	Check(AliveLineOk("1760611234 1234 gametile 1\n"), "alive line: the helper's own");
	Check(AliveLineOk("1760611234 1234 gametile 1"), "alive line: no line break");
	Check(AliveLineOk("1760611234 1234 gametile 1\r\n"), "alive line: CRLF");
	Check(AliveLineOk("1 2 gametile 1 more\n"), "alive line: more fields are ignored");
	Check(!AliveLineOk("1760611234 1234 gametile 2\n"), "alive line: version 2");
	Check(!AliveLineOk("1760611234 1234 discdisc 1\n"), "alive line: another word");
	Check(!AliveLineOk("1760611234 gametile 1\n"), "alive line: three fields");
	Check(!AliveLineOk("12ab 1234 gametile 1\n"), "alive line: a time that is not digits");
	Check(!AliveLineOk("1760611234 pid gametile 1\n"), "alive line: a pid that is not digits");
	Check(!AliveLineOk(""), "alive line: empty");
	Check(!AliveLineOk("\n1760611234 1234 gametile 1\n"), "alive line: it must be the first line");

	const std::string dir = root + "/alive";
	MakeDir(dir);
	const std::string f = dir + "/alive.txt";
	Check(!AliveFresh(dir, 1000), "AliveFresh: no file");
	Write(f, "1760611234 1234 gametile 1\n");
	SetMtime(f, 1000000);
	Check(AliveFresh(dir, 1000000), "AliveFresh: 0 s old");
	Check(AliveFresh(dir, 1000020), "AliveFresh: 20 s old");
	Check(AliveFresh(dir, 1000059), "AliveFresh: 59 s old");
	Check(AliveFresh(dir, 1000060), "AliveFresh: 60 s old (still there)");
	Check(!AliveFresh(dir, 1000061), "AliveFresh: 61 s old: the helper is gone");
	Check(AliveFresh(dir, 999000), "AliveFresh: a time in the future counts as 0 s");
	Write(f, "1760611234 1234 gametile 2\n");
	SetMtime(f, 1000000);
	Check(!AliveFresh(dir, 1000000), "AliveFresh: version 2");
	Write(f, "1760611234 1234 other 1\n");
	SetMtime(f, 1000000);
	Check(!AliveFresh(dir, 1000000), "AliveFresh: another word");
	Write(f, "");
	SetMtime(f, 1000000);
	Check(!AliveFresh(dir, 1000000), "AliveFresh: empty file");
	std::remove(f.c_str());
	MakeDir(f); // a folder of that name
	Check(!AliveFresh(dir, 1000000), "AliveFresh: not a file");
	rmdir(f.c_str());

	// ParseState: the helper's own lines (contract 4.2).
	const std::string text = Line("PPSA99251", kGame, "SCUS-97399", "God of War", "ok") + Line("PPSA99252", kGame2, "", "Okami", "installing") +
	                         Line("PPSA99253", "/data/PCSX2/games/Okami.iso", "SLUS-21309", "Okami", "removing") +
	                         Line("-", "/mnt/usb1/PS2/Gone.iso", "", "Gone", "failed:nogame");
	std::vector<Entry> e = ParseState(text);
	Eq(e.size(), 4, "ParseState: four lines");
	if (e.size() == 4)
	{
		Eq(e[0].tile, "PPSA99251", "line 1: tile");
		Eq(e[0].path, kGame, "line 1: path");
		Eq(e[0].serial, "SCUS-97399", "line 1: serial");
		Eq(e[0].title, "God of War", "line 1: title");
		Check(e[0].kind == Kind::Ok && e[0].status == "ok", "line 1: ok");
		Check(e[1].kind == Kind::Installing && e[1].serial.empty(), "line 2: installing, no serial");
		Check(e[2].kind == Kind::Removing, "line 3: removing");
		Check(e[3].kind == Kind::Failed && e[3].tile == "-" && e[3].reason == "nogame" && e[3].status == "failed:nogame", "line 4: a failed request has id - and a reason");
	}
	Eq(ParseState("").size(), 0, "ParseState: an empty file (helper up, no tiles)");
	Eq(ParseState("\n\n\n").size(), 0, "ParseState: blank lines");
	Eq(ParseState("v1\n" + Line("PPSA99251", kGame, "S", "T", "ok")).size(), 1, "ParseState: a v1 line is tolerated");
	Eq(ParseState("# comment\n" + Line("PPSA99251", kGame, "S", "T", "ok")).size(), 1, "ParseState: a comment is skipped");
	Eq(ParseState(Line("PPSA99251", kGame, "S", "T", "ok") + "\r\n").size(), 1, "ParseState: CRLF");
	std::string crlf = "PPSA99251\t" + kGame + "\tS\tT\tok\r\nPPSA99252\t" + kGame2 + "\t\tT2\tinstalling\r\n";
	e = ParseState(crlf);
	Check(e.size() == 2 && e[0].status == "ok" && e[1].kind == Kind::Installing, "ParseState: CRLF lines give clean status words");
	Eq(ParseState("PPSA99251\t" + kGame + "\tS\tT\n").size(), 0, "ParseState: a short line (4 fields) is ignored");
	Eq(ParseState("PPSA99251\t" + kGame + "\n").size(), 0, "ParseState: a shorter line");
	Eq(ParseState(Line("PPSA99251", kGame, "S", "T", "maybe")).size(), 0, "ParseState: an unknown status word is ignored (not taken for busy)");
	Eq(ParseState(Line("PPSA99251", kGame, "S", "T", "OK")).size(), 0, "ParseState: status words are exact");
	e = ParseState("PPSA99251\t" + kGame + "\tS\tT\tok\textra\tcolumns\n");
	Check(e.size() == 1 && e[0].kind == Kind::Ok && e[0].status == "ok", "ParseState: extra columns are ignored");
	e = ParseState(Line("-", kGame, "", "T", "failed:a:b"));
	Check(e.size() == 1 && e[0].reason == "a:b", "ParseState: a colon inside the reason stays");
	e = ParseState(Line("-", kGame, "", "T", "failed"));
	Check(e.size() == 1 && e[0].kind == Kind::Failed && e[0].reason.empty(), "ParseState: failed with no reason");
	e = ParseState(Line("-", kGame, "", "T", "failed:"));
	Check(e.size() == 1 && e[0].kind == Kind::Failed && e[0].reason.empty(), "ParseState: failed: with no reason");
	e = ParseState(Line("PPSA99251", "/data/PCSX2/games//God of War (USA).iso/", "S", "T", "ok"));
	Check(e.size() == 1 && e[0].path == kGame, "ParseState: the path is normalized");
	e = ParseState(Line("PPSA99251", kGame, "SCUS-97399", "Pok\xC3\xA9mon \xE2\x82\xAC", "ok"));
	Check(e.size() == 1 && e[0].title == "Pok\xC3\xA9mon \xE2\x82\xAC", "ParseState: a UTF-8 title is kept");

	// FindByPath: exact (after the normalizing), ok > installing > removing > failed.
	const std::vector<Entry> pri = {ParseState(Line("-", kGame, "", "T", "failed:nogame"))[0], ParseState(Line("PPSA99253", kGame, "", "T", "removing"))[0],
		ParseState(Line("PPSA99252", kGame, "", "T", "installing"))[0], ParseState(Line("PPSA99251", kGame, "", "T", "ok"))[0]};
	std::vector<int> order = {0, 1, 2, 3};
	int perms = 0, wrong = 0;
	do
	{
		std::vector<Entry> v;
		for (int i : order)
			v.push_back(pri[static_cast<size_t>(i)]);
		const Entry* best = FindByPath(v, kGame);
		perms++;
		wrong += !best || best->kind != Kind::Ok;
		v.erase(std::remove_if(v.begin(), v.end(), [](const Entry& x) { return x.kind == Kind::Ok; }), v.end());
		best = FindByPath(v, kGame);
		wrong += !best || best->kind != Kind::Installing;
		v.erase(std::remove_if(v.begin(), v.end(), [](const Entry& x) { return x.kind == Kind::Installing; }), v.end());
		best = FindByPath(v, kGame);
		wrong += !best || best->kind != Kind::Removing;
	} while (std::next_permutation(order.begin(), order.end()));
	Eq(perms, 24, "FindByPath: 24 orders");
	Eq(wrong, 0, "FindByPath: ok beats installing beats removing beats failed in every order");
	Check(!FindByPath(pri, kGame2), "FindByPath: another game: none");
	Check(!FindByPath(pri, "/data/PCSX2/games/god of war (usa).iso"), "FindByPath: case matters");
	Check(!FindByPath(pri, kGame + "x"), "FindByPath: exact");

	// StateReader: a missing file is "no tiles"; a change is read; a rewrite of the same size in the same second is seen (nanosecond signature); force and always_read read.
	const std::string sd = root + "/state";
	MakeDir(sd);
	StateReader r;
	Check(!r.Refresh(sd, false, false) && !r.exists && r.entries.empty(), "StateReader: no file: nothing exists");
	Write(sd + "/state.txt", Line("PPSA99251", kGame, "S", "AAAA", "ok"));
	Check(r.Refresh(sd, false, false) && r.exists && r.entries.size() == 1 && r.entries[0].title == "AAAA", "StateReader: reads a new file");
	Check(!r.Refresh(sd, false, false), "StateReader: unchanged: not read again");
	Check(r.Refresh(sd, false, true) && r.entries.size() == 1, "StateReader: always_read reads it again");
	Check(r.Refresh(sd, true, false), "StateReader: force reads it again");
	usleep(30000);
	Write(sd + "/state.txt", Line("PPSA99251", kGame, "S", "BBBB", "ok")); // the same size, a moment later
	Check(r.Refresh(sd, false, false) && r.entries.size() == 1 && r.entries[0].title == "BBBB", "StateReader: a same-size rewrite is seen");
	usleep(30000);
	Write(sd + "/state.txt", "");
	Check(r.Refresh(sd, false, false) && r.exists && r.entries.empty(), "StateReader: an empty file is read: helper up, no tiles");
	std::remove((sd + "/state.txt").c_str());
	Check(r.Refresh(sd, false, false) && !r.exists && r.entries.empty(), "StateReader: the file removed: entries cleared");
	// A huge file is read only up to 64 KB (the lines inside that are still understood).
	std::string big;
	for (int i = 0; i < 2000; i++)
		big += Line("PPSA99251", "/data/PCSX2/games/g" + std::to_string(i) + ".iso", "S", "Title number " + std::to_string(i), "ok");
	Write(sd + "/state.txt", big);
	Check(r.Refresh(sd, true, false) && r.entries.size() > 100 && r.entries.size() < 2000, "StateReader: reads at most 64 KB", std::to_string(r.entries.size()));
	// A folder or a pipe where state.txt goes is no state. (Opening a pipe for reading waits for a writer for ever: the shelf's menu thread would hang. The cross test of
	// 2026-10-16 found it; the alarm turns a hang into a failed run.)
	std::remove((sd + "/state.txt").c_str());
	MakeDir(sd + "/state.txt");
	Check(r.Refresh(sd, true, false) && !r.exists && r.entries.empty(), "StateReader: a folder named state.txt: no state");
	rmdir((sd + "/state.txt").c_str());
	if (mkfifo((sd + "/state.txt").c_str(), 0666) == 0)
	{
		alarm(20);
		const bool looked = r.Refresh(sd, true, false);
		alarm(0);
		Check(!looked || (!r.exists && r.entries.empty()), "StateReader: a pipe named state.txt: no state, and no hang");
		Check(!r.exists && r.entries.empty(), "StateReader: ...nothing is left of the old entries");
		std::remove((sd + "/state.txt").c_str());
	}
}

// ---- the row's state machine --------------------------------------------------------------------------------------------------------------------------------

struct Sim
{
	Tracker t;
	std::string path = kGame;
	std::vector<Entry> entries;
	bool alive = true, file = false; // the helper is there; the own request file is in the folder
	double now = 100;
	void State(const std::string& text) { entries = ParseState(text); }
	void Press(bool add)
	{
		const Entry* cur = FindByPath(entries, path);
		t.Ask(path, add, "request-1-000.txt", now, cur);
		file = true;
	}
	View Look()
	{
		View v = t.Evaluate(path, entries, alive, file, now);
		if (v.withdraw)
		{
			file = false;
			t.WithdrawDone(path, WithdrawResult::Withdrawn, now);
		}
		return v;
	}
	View At(double dt)
	{
		now += dt;
		return Look();
	}
	std::string Installing() const { return Line("PPSA99251", kGame, "SCUS-97399", "God of War", "installing"); }
	std::string Ok() const { return Line("PPSA99251", kGame, "SCUS-97399", "God of War", "ok"); }
	std::string Removing() const { return Line("PPSA99251", kGame, "SCUS-97399", "God of War", "removing"); }
};

void TestTracker()
{
	// No ask: the row follows the state.
	{
		Sim s;
		View v = s.Look();
		Check(v.phase == Phase::Off && v.can == Can::Add && !v.busy, "no ask, no line: Off, can add");
		s.State(s.Installing());
		v = s.Look();
		Check(v.phase == Phase::Installing && v.can == Can::None && v.busy && v.tile == "PPSA99251", "no ask, a line installing: Installing, busy", PhaseName(v.phase));
		s.State(s.Ok());
		v = s.Look();
		Check(v.phase == Phase::On && v.can == Can::Remove && !v.busy, "no ask, a line ok: On, can remove");
		s.State(s.Removing());
		v = s.Look();
		Check(v.phase == Phase::Removing && v.can == Can::None && v.busy, "no ask, a line removing: Removing, busy");
		s.State(Line("-", kGame, "SCUS-97399", "God of War", "failed:nospace"));
		v = s.Look();
		Check(v.phase == Phase::Failed && v.can == Can::Add && v.reason == "nospace", "no ask, a failed line: Failed, can add (a retry)");
		s.State(Line("PPSA99252", kGame2, "", "Okami", "ok"));
		v = s.Look();
		Check(v.phase == Phase::Off, "another game's line is not this game's");
	}
	// ADD: Asking -> Waiting -> Installing (for 600 s: never Stuck) -> On, and the ask is dropped.
	{
		Sim s;
		s.State("");
		s.Press(true);
		View v = s.Look();
		Check(v.phase == Phase::Asking && v.busy && v.can == Can::None && !v.ask_remove && s.t.AnyPending(), "add: Asking while the file is in the folder");
		Eq(ValueText(v), "Adding...", "add: the value while asking");
		v = s.At(14.0);
		Check(v.phase == Phase::Asking, "add: still Asking at 14 s");
		s.file = false; // the helper takes it
		v = s.At(0.5);
		Check(v.phase == Phase::Waiting && v.busy && !v.behind_removal && s.t.AnyPending(), "add: the file is gone: Waiting", PhaseName(v.phase));
		Eq(ValueText(v), "Adding...", "add: the value while waiting");
		s.State(s.Installing());
		v = s.At(1.0);
		Check(v.phase == Phase::Installing && v.busy && !s.t.AnyPending() && s.t.Has(s.path), "add: a line installing: Installing; not pending any more, still tracked");
		Eq(ValueText(v), "Installing...", "add: the value while installing");
		int stuck = 0, wrong = 0;
		for (int i = 0; i < 60; i++) // 600 virtual seconds
		{
			v = s.At(10.0);
			stuck += v.phase == Phase::Stuck;
			wrong += v.phase != Phase::Installing;
		}
		Eq(stuck, 0, "add: installing for 600 s while the helper is there: never Stuck");
		Eq(wrong, 0, "add: ... and always Installing");
		s.State(s.Ok());
		v = s.At(1.0);
		Check(v.phase == Phase::On && v.can == Can::Remove && v.tile == "PPSA99251", "add: ok: On");
		Check(!s.t.Has(s.path) && s.t.Paths().empty() && !s.t.AnyPending(), "add: a finished ask is dropped");
	}
	// ADD, nobody takes the file: Asking until 15 s, then No answer and the file is taken back, once.
	{
		Sim s;
		s.Press(true);
		View v = s.At(15.0);
		Check(v.phase == Phase::Asking && !v.withdraw, "no answer: at exactly 15.0 s still Asking");
		v = s.At(0.02);
		Check(v.phase == Phase::NoAnswer && v.can == Can::Add && !v.busy, "no answer: after 15 s: No answer, can retry", PhaseName(v.phase));
		Check(!s.file, "no answer: the file was taken back (Sim::Look unlinked it)");
		Check(!s.t.AnyPending(), "no answer: not pending (polling stops)");
		Check(s.t.Has(s.path), "no answer: still in the books for a late answer");
		View v2 = s.At(1.0);
		Check(v2.phase == Phase::NoAnswer && !v2.withdraw, "no answer: the next look still says so, and does not ask to take back again");
		Eq(ValueText(v2), "No answer", "no answer: the value");
		// a late answer resolves it
		s.State(s.Installing());
		v2 = s.At(1.0);
		Check(v2.phase == Phase::Installing, "no answer: a late line still resolves it");
		// withdraw is asked once even when the caller does not report the result
		Sim s2;
		s2.Press(true);
		s2.now += 16;
		View w1 = s2.t.Evaluate(s2.path, s2.entries, true, true, s2.now);
		View w2 = s2.t.Evaluate(s2.path, s2.entries, true, true, s2.now + 1);
		Check(w1.withdraw && !w2.withdraw, "withdraw is asked exactly once");
		s2.t.WithdrawDone(s2.path, WithdrawResult::Failed, s2.now);
		View w3 = s2.t.Evaluate(s2.path, s2.entries, true, true, s2.now + 2);
		Check(w3.withdraw, "withdraw is asked again after a failed unlink");
		// the ENOENT race: it was taken just now
		Sim s3;
		s3.Press(true);
		s3.now += 16;
		View r1 = s3.t.Evaluate(s3.path, s3.entries, true, true, s3.now);
		Check(r1.phase == Phase::NoAnswer && r1.withdraw, "race: asked to take back");
		s3.t.WithdrawDone(s3.path, WithdrawResult::AlreadyTaken, s3.now);
		View r2 = s3.t.Evaluate(s3.path, s3.entries, true, false, s3.now + 0.1);
		Check(r2.phase == Phase::Waiting, "race: ENOENT counts as taken: Waiting", PhaseName(r2.phase));
		// a new ask resets the timers
		Sim s4;
		s4.Press(true);
		s4.At(20);
		s4.Press(true);
		View n1 = s4.Look();
		Check(n1.phase == Phase::Asking, "a new ask starts over: Asking");
		n1 = s4.At(14.9);
		Check(n1.phase == Phase::Asking, "a new ask: 14.9 s later still Asking");
	}
	// ADD, taken but no line ever: Waiting, then Stuck after 90 s from the take.
	{
		Sim s;
		s.Press(true);
		s.file = false;
		View v = s.At(1.0); // the take is seen
		Check(v.phase == Phase::Waiting, "stuck: taken: Waiting");
		v = s.At(89.0);
		Check(v.phase == Phase::Waiting, "stuck: 89 s after the take: still Waiting", PhaseName(v.phase));
		v = s.At(1.5);
		Check(v.phase == Phase::Stuck && v.can == Can::Add && !v.busy && !v.ask_remove, "stuck: after 90 s: Stuck, can retry", PhaseName(v.phase));
		Check(!s.t.AnyPending(), "stuck: not pending");
		Eq(ValueText(v), "Stuck", "stuck: the value");
		s.State(s.Ok());
		v = s.At(1.0);
		Check(v.phase == Phase::On, "stuck: a late ok line resolves it");
	}
	// FAILED, and the retry: the helper's old failure is not the new answer.
	{
		Sim s;
		s.State(Line("-", kGame, "SCUS-97399", "God of War", "failed:nogame"));
		View v = s.Look();
		Check(v.phase == Phase::Failed && v.reason == "nogame", "retry: Failed(nogame)");
		s.Press(true); // the retry; the failed line is present
		v = s.Look();
		Check(v.phase == Phase::Asking, "retry: Asking, the old failure is ignored while the file is there", PhaseName(v.phase));
		s.file = false;
		v = s.At(1.0);
		Check(v.phase == Phase::Waiting, "retry: taken: the old failure (same text) is ignored for the grace time: Waiting", PhaseName(v.phase));
		v = s.At(3.0);
		Check(v.phase == Phase::Waiting, "retry: 3 s after the take: still the grace", PhaseName(v.phase));
		s.State(Line("-", kGame, "SCUS-97399", "God of War", "failed:nogame")); // the helper forgot the old line and failed again: the same text
		v = s.At(2.5);
		Check(v.phase == Phase::Failed && v.reason == "nogame" && !s.t.Has(s.path), "retry: after the grace the same failure is the answer", PhaseName(v.phase));
		// a different failure is the answer at once
		Sim s2;
		s2.State(Line("-", kGame, "", "God of War", "failed:nogame"));
		s2.Press(true);
		s2.file = false;
		s2.At(0.5);
		s2.State(Line("-", kGame, "", "God of War", "failed:nospace"));
		v = s2.At(0.5);
		Check(v.phase == Phase::Failed && v.reason == "nospace", "retry: a different reason is the answer at once");
		// the old line goes and the tile comes
		Sim s3;
		s3.State(Line("-", kGame, "", "God of War", "failed:limit"));
		s3.Press(true);
		s3.file = false;
		s3.State(s3.Installing());
		v = s3.At(0.5);
		Check(v.phase == Phase::Installing, "retry: an installing line is the answer");
		// a first failure (no prior one) is the answer at once
		Sim s4;
		s4.Press(true);
		s4.file = false;
		s4.State(Line("-", kGame, "", "God of War", "failed:busy"));
		v = s4.At(0.5);
		Check(v.phase == Phase::Failed && v.reason == "busy" && v.can == Can::Add, "a first failure is the answer at once");
	}
	// REMOVE.
	{
		Sim s;
		s.State(s.Ok());
		s.Press(false);
		View v = s.Look();
		Check(v.phase == Phase::Asking && v.ask_remove && v.busy, "remove: Asking");
		Eq(ValueText(v), "Removing...", "remove: the value while asking says Removing...");
		s.file = false;
		v = s.At(0.5);
		Check(v.phase == Phase::Removing && v.busy && s.t.AnyPending(), "remove: taken, the line still ok: Removing (pending)", PhaseName(v.phase));
		s.State(s.Removing());
		v = s.At(1.0);
		Check(v.phase == Phase::Removing && !s.t.AnyPending(), "remove: a line removing: Removing, not pending");
		int wrong = 0;
		for (int i = 0; i < 60; i++) // 600 s: the removal waits for PS5SX2 to close
			wrong += s.At(10.0).phase != Phase::Removing;
		Eq(wrong, 0, "remove: removing for 600 s while the helper is there: always Removing");
		s.State("");
		v = s.At(1.0);
		Check(v.phase == Phase::Off && v.can == Can::Add && !s.t.Has(s.path), "remove: the line is gone: Off, the ask is dropped");
	}
	{
		Sim s; // the removal does not start: Stuck after 90 s
		s.State(s.Ok());
		s.Press(false);
		s.file = false;
		s.At(1.0);
		View v = s.At(88.0);
		Check(v.phase == Phase::Removing, "remove stuck: 88 s after the take: still Removing", PhaseName(v.phase));
		v = s.At(3.0);
		Check(v.phase == Phase::Stuck && v.can == Can::Remove && v.ask_remove, "remove stuck: after 90 s: Stuck, can try again", PhaseName(v.phase));
		Eq(HelpText(v, "God of War"), "The tile is still on the home screen. If the game runs from it, close it first, then try again.", "remove stuck: the help");
	}
	{
		Sim s; // the helper does not take the remove
		s.State(s.Ok());
		s.Press(false);
		View v = s.At(16.0);
		Check(v.phase == Phase::NoAnswer && v.can == Can::Remove, "remove: not taken: No answer, can try again (remove)", PhaseName(v.phase));
		Check(!s.file, "remove: the file was taken back");
	}
	{
		Sim s; // remove answered by a failure
		s.State(s.Ok());
		s.Press(false);
		s.file = false;
		s.At(0.5);
		s.State(Line("-", kGame, "", "God of War", "failed:damaged"));
		View v = s.At(0.5);
		Check(v.phase == Phase::Failed && v.reason == "damaged", "remove: a failed line is the answer");
	}
	// ADD while the game's tile is being removed: Waiting (behind the removal), not pending, text; then built.
	{
		Sim s;
		s.State(s.Removing());
		s.Press(true);
		s.file = false;
		View v = s.At(1.0);
		Check(v.phase == Phase::Waiting && v.behind_removal && v.busy && !s.t.AnyPending(), "add behind a removal: Waiting, behind the removal, not pending", PhaseName(v.phase));
		Eq(HelpText(v, "God of War"), "The helper has the request. A tile that was removed a moment ago is finished first.", "add behind a removal: the help");
		int wrong = 0;
		for (int i = 0; i < 60; i++)
			wrong += s.At(10.0).phase != Phase::Waiting;
		Eq(wrong, 0, "add behind a removal: Waiting for 600 s (no time-out while the line is there)");
		s.State("");
		v = s.At(1.0);
		Check(v.phase == Phase::Waiting && s.t.AnyPending(), "add behind a removal: the line left: Waiting for the build (pending)");
		s.State(s.Installing());
		Check(s.At(1.0).phase == Phase::Installing, "add behind a removal: then Installing");
		s.State(s.Ok());
		Check(s.At(1.0).phase == Phase::On, "add behind a removal: then On");
	}
	// Helper absent: nothing is believed, nothing is offered; the own file still has its time limit.
	{
		Sim s;
		s.alive = false;
		View v = s.Look();
		Check(v.phase == Phase::NoHelper && v.can == Can::None && !v.busy, "no helper: phase 9, nothing to do");
		Eq(ValueText(v), "No helper", "no helper: the value");
		Check(HelpText(v, "x").find("171k") != std::string::npos && HelpText(v, "x").find("by hand") != std::string::npos, "no helper: the help names the helper revision and the manual-load fallback");
		s.State(s.Ok());
		v = s.Look();
		Check(v.phase == Phase::NoHelper, "no helper: state.txt is not believed");
		s.alive = true;
		v = s.Look();
		Check(v.phase == Phase::On, "the helper is back: the state is believed again");
		// an ask whose helper vanishes: phase 9, and the file is still taken back at 15 s
		Sim s2;
		s2.Press(true);
		s2.alive = false;
		v = s2.At(3.0);
		Check(v.phase == Phase::NoHelper && !v.withdraw, "no helper with an ask: phase 9 (the ask kept)");
		Check(s2.t.AnyPending(), "no helper with an ask: the own file is in the folder: still pending (its 15 s limit needs the one-second looks)");
		v = s2.At(13.0);
		Check(v.phase == Phase::NoHelper && !s2.file, "no helper with an ask: the file is taken back after 15 s");
		Check(!s2.t.AnyPending(), "no helper with an ask: taken back: not pending (reviewer: it used to poll once a second until the helper came back)");
		s2.alive = true;
		v = s2.At(1.0);
		Check(v.phase == Phase::NoAnswer, "the helper returns: the ask is No answer", PhaseName(v.phase));
		Check(!s2.t.AnyPending(), "the helper returns: No answer is not pending");
		// the helper took the file, then went away: the ask waits quietly (no polling for it), and is believed again when the helper is back
		Sim s5;
		s5.Press(true);
		s5.file = false; // taken
		v = s5.At(0.5);
		Check(v.phase == Phase::Waiting && s5.t.AnyPending(), "taken: Waiting, pending");
		s5.alive = false;
		v = s5.At(1.0);
		Check(v.phase == Phase::NoHelper && !s5.t.AnyPending(), "taken, then the helper goes away: phase 9, not pending");
		s5.alive = true;
		s5.State(s5.Installing());
		v = s5.At(1.0);
		Check(v.phase == Phase::Installing && s5.t.Has(s5.path), "the helper is back: the ask is still followed (Installing)", PhaseName(v.phase));
		s5.State(s5.Ok());
		v = s5.At(1.0);
		Check(v.phase == Phase::On && !s5.t.Has(s5.path), "the helper is back: ...and ends On");
	}
	// Many games: asks are tracked per path.
	{
		Tracker t;
		const std::string a = kGame, b = kGame2;
		t.Ask(a, true, "request-1-000.txt", 10, nullptr);
		t.Ask(b, true, "request-1-001.txt", 11, nullptr);
		Eq(t.Paths().size(), 2, "multi: two asks");
		Eq(t.FileOf(a), "request-1-000.txt", "multi: A's file");
		Eq(t.FileOf(b), "request-1-001.txt", "multi: B's file");
		Eq(t.PendingCount(), 2, "multi: both pending");
		const std::vector<Entry> none;
		View va = t.Evaluate(a, none, true, true, 12.0), vb = t.Evaluate(b, none, true, true, 12.0);
		Check(va.phase == Phase::Asking && vb.phase == Phase::Asking, "multi: both Asking (the file of one is not the file of the other)");
		// A is taken, B's file is still there
		va = t.Evaluate(a, none, true, false, 13.0);
		vb = t.Evaluate(b, none, true, true, 13.0);
		Check(va.phase == Phase::Waiting && vb.phase == Phase::Asking, "multi: A taken (Waiting) while B is still Asking");
		// A resolves while only B is looked at afterwards
		const std::vector<Entry> okA = ParseState(Line("PPSA99251", a, "S", "T", "ok"));
		va = t.Evaluate(a, okA, true, false, 14.0);
		Check(va.phase == Phase::On && !t.Has(a) && t.Has(b), "multi: A resolved and dropped, B kept");
		Eq(t.PendingCount(), 1, "multi: one pending");
		vb = t.Evaluate(b, okA, true, true, 25.0);
		Check(vb.phase == Phase::Asking && !vb.withdraw, "multi: B at 14 s of its own age: still Asking");
		vb = t.Evaluate(b, okA, true, true, 27.0);
		Check(vb.phase == Phase::NoAnswer && vb.withdraw, "multi: B is past 15 s: No answer, take back");
		Eq(t.PendingCount(), 0, "multi: nothing pending: polling would stop");
		Check(t.Has(b), "multi: B is still in the books");
		t.Forget(b);
		Check(!t.Has(b) && t.Paths().empty(), "Forget");
	}
	// The clock going back never gives a negative age.
	{
		Sim s;
		s.Press(true);
		View v = s.At(-50.0);
		Check(v.phase == Phase::Asking, "clock back: Asking, no underflow into NoAnswer");
		v = s.At(14.0);
		Check(v.phase == Phase::Asking, "clock back: the age counts from the new time");
		v = s.At(1.5);
		Check(v.phase == Phase::NoAnswer, "clock back: 15.5 s after the new time it is No answer (not waiting for the old time to come round)", PhaseName(v.phase));
	}
	// The shown words, all phases (contract 8).
	{
		View v;
		auto text = [&](Phase p, bool rem = false, bool behind = false, const std::string& reason = "") {
			v = View();
			v.phase = p;
			v.ask_remove = rem;
			v.behind_removal = behind;
			v.reason = reason;
			return HelpText(v, "God of War");
		};
		Eq(text(Phase::Off), "Puts God of War on the PS5 home screen as its own tile: press it there and this game starts, no shelf. The tile appears about 30 seconds after you close PS5SX2.", "help: Off");
		Eq(text(Phase::Asking), "Asked the helper.", "help: Asking");
		Eq(text(Phase::Waiting), "The helper has the request.", "help: Waiting");
		Eq(text(Phase::Installing), "The tile is built. It appears on the home screen about 30 seconds after you close PS5SX2: the console adds it only when no game is running, and PS5SX2 counts as one, also in the background.", "help: Installing");
		Eq(text(Phase::On), "On the home screen. Cross takes it off again (it goes away about 30 seconds after you close PS5SX2). If the game runs from the tile, close it first.", "help: On");
		Eq(text(Phase::Removing), "The tile is hidden now and is taken away about 30 seconds after you close PS5SX2.", "help: Removing");
		Eq(text(Phase::NoAnswer), "The helper did not take the request within 15 seconds. Load the newest PS5SX2 Disc Helper (171g), then press Cross to try again.", "help: No answer");
		Eq(text(Phase::Stuck), "The helper took the request but the tile did not show up in time. Press Cross to try again; disctile.log (flag kp_log) says why.", "help: Stuck (add)");
		Eq(text(Phase::NoHelper), "The helper that makes game tiles is not running yet. The app loads it at start; if it did not, load the PS5SX2 Disc Helper (171k) by hand.", "help: No helper");
		Eq(text(Phase::Failed, false, false, "nogame"), "Failed: The helper cannot find the game file (is its drive connected?).", "help: Failed nogame");
		Eq(text(Phase::Failed, false, false, "badpath"), "Failed: The helper cannot use this path (only files under /data or on a drive under /mnt).", "help: Failed badpath");
		Eq(text(Phase::Failed, false, false, "limit"), "Failed: All 10 game tiles are in use: take one off first.", "help: Failed limit");
		Eq(text(Phase::Failed, false, false, "nospace"), "Failed: Not enough free space on /data (about 200 MB are needed).", "help: Failed nospace");
		Eq(text(Phase::Failed, false, false, "build"), "Failed: Building the tile failed (the helper's log has the details).", "help: Failed build");
		Eq(text(Phase::Failed, false, false, "disabled"), "Failed: Game tiles are switched off in the helper (flag gametile_off).", "help: Failed disabled");
		Eq(text(Phase::Failed, false, false, "busy"), "Failed: The helper's request queue is full: try again in a minute.", "help: Failed busy");
		Eq(text(Phase::Failed, false, false, "damaged"), "Failed: The tile's folder was damaged and has been taken down: add it again.", "help: Failed damaged");
		Eq(text(Phase::Failed, false, false, "weird"), "Failed (weird)", "help: an unknown reason");
		View on;
		on.phase = Phase::On;
		Eq(HelpText(on, "God of War", true), "On the home screen. This is the tile this game was started from: take it off from the main app.", "help: On, the tile this process runs from");
		Eq(ValueText(View()), "Off", "value: Off");
		for (const std::pair<Phase, const char*>& p : std::vector<std::pair<Phase, const char*>>{{Phase::Installing, "Installing..."}, {Phase::On, "On"}, {Phase::Failed, "Failed"},
				 {Phase::Removing, "Removing..."}, {Phase::NoAnswer, "No answer"}, {Phase::Stuck, "Stuck"}, {Phase::NoHelper, "No helper"}})
		{
			View x;
			x.phase = p.first;
			Eq(ValueText(x), p.second, std::string("value: ") + p.second);
		}
		Eq(static_cast<int>(Phase::Off), 0, "enum: Off = 0");
		Eq(static_cast<int>(Phase::Stuck), 8, "enum: Stuck = 8");
		Eq(static_cast<int>(Phase::NoHelper), 9, "enum: No helper = 9 (appended)");
		Eq(CanName(Can::Add), "add", "CanName add");
		Eq(CanName(Can::Remove), "remove", "CanName remove");
		Eq(CanName(Can::None), "none", "CanName none");
	}
	// The failed words have a sentence each, an unknown one none.
	for (const char* w : {"nogame", "badpath", "limit", "nospace", "build", "disabled", "busy", "damaged"})
		Check(!ReasonText(w).empty(), std::string("ReasonText: ") + w);
	Check(ReasonText("other").empty(), "ReasonText: an unknown word has none");
}

void TestPoll()
{
	// PollDue(wanted, pending, view_busy, disk_busy, force, now, last)
	Check(!PollDue(false, false, false, false, false, 100, 0), "poll: not wanted: no");
	Check(!PollDue(false, true, true, false, true, 100, 0), "poll: not wanted wins over force");
	Check(PollDue(true, false, false, false, true, 100, 99.9), "poll: forced: yes");
	Check(PollDue(true, false, false, true, true, 100, 99.9), "poll: forced even while a texture pack unpacks");
	Check(!PollDue(true, true, true, true, false, 100, 0), "poll: a texture pack unpacks: no");
	Check(!PollDue(true, true, false, false, false, 100.9, 100), "poll: pending, 0.9 s: no");
	Check(PollDue(true, true, false, false, false, 101.0, 100), "poll: pending, 1.0 s: yes");
	Check(!PollDue(true, false, true, false, false, 100.9, 100), "poll: the shown row busy, 0.9 s: no");
	Check(PollDue(true, false, true, false, false, 101.0, 100), "poll: the shown row busy, 1.0 s: yes");
	Check(!PollDue(true, false, false, false, false, 102.9, 100), "poll: idle, 2.9 s: no");
	Check(PollDue(true, false, false, false, false, 103.0, 100), "poll: idle, 3.0 s: yes");
	Check(PollDue(true, false, false, false, false, 50, 100), "poll: the clock went back: yes");
}

// ---- the tile's own start ----------------------------------------------------------------------------------------------------------------------------------

void TestMarker(const std::string& root)
{
	Marker m;
	// What the helper's gt_marker_write writes.
	const std::string good = kGame + "\nserial=SCUS-97399\ntitle=God of War\ntile=PPSA99251\nrev=1\n";
	Check(ParseMarker(good, m), "marker: the helper's own");
	Eq(m.path, kGame, "marker: path");
	Eq(m.tile_id, "PPSA99251", "marker: tile id");
	Eq(m.serial, "SCUS-97399", "marker: serial");
	Eq(m.title, "God of War", "marker: title");
	Check(ParseMarker("/data/PCSX2/games/Pok\xC3\xA9mon (Europe) [!].iso\ntile=PPSA99260\n", m) && m.path == "/data/PCSX2/games/Pok\xC3\xA9mon (Europe) [!].iso" && m.tile_id == "PPSA99260",
		"marker: UTF-8, spaces, parentheses, brackets in line 1");
	Check(ParseMarker("/mnt/usb0/a.iso", m) && m.path == "/mnt/usb0/a.iso" && m.tile_id.empty(), "marker: the path only, no line break");
	Check(ParseMarker("/mnt/usb0/a.iso\r\ntile=PPSA99252\r\n", m) && m.path == "/mnt/usb0/a.iso" && m.tile_id == "PPSA99252", "marker: CRLF");
	Check(ParseMarker("/mnt/usb0/a.iso  \n", m) && m.path == "/mnt/usb0/a.iso  ", "marker: trailing spaces are kept (only CR and LF are trimmed)");
	Check(ParseMarker(std::string("/data/") + std::string(1400, 'a') + "\ntile=PPSA99251\n", m) && m.tile_id == "PPSA99251", "marker: a long first line that ends in a line break");
	Check(!ParseMarker("/data/" + std::string(1593, 'a'), m), "marker: a first line that fills the 1599-byte buffer with no line break is invalid");
	Check(ParseMarker("/data/" + std::string(1500, 'a'), m), "marker: 1506 bytes with no line break are fine");
	Check(!ParseMarker("", m), "marker: empty");
	Check(!ParseMarker("\n/data/x.iso\n", m), "marker: an empty first line");
	Check(ParseMarker("/data/x.iso\ntile=PPSA9\n", m) && m.tile_id.empty(), "marker: a tile id of 5 characters is ignored");
	Check(ParseMarker("/data/x.iso\ntile=PPSA99251PPSA9\n", m) && m.tile_id.empty(), "marker: a tile id of 14 characters is ignored");
	Check(ParseMarker("/data/x.iso\ntile=PPSA 9925\n", m) && m.tile_id.empty(), "marker: a tile id with a space is ignored");
	Check(ParseMarker("/data/x.iso\nunknown=1\nrev=7\n", m) && m.tile_id.empty() && m.path == "/data/x.iso", "marker: unknown keys are ignored");

	// CheckLaunchPath with real files.
	const std::string d = root + "/launch";
	MakeDir(d);
	Write(d + "/Game.iso", "x");
	Write(d + "/Game.chd", "x");
	Write(d + "/Game.bin", "x");
	Write(d + "/Empty.iso", "");
	Write(d + "/Game.elf", "x");
	Write(d + "/notes.txt", "x");
	Write(d + "/.hidden.iso", "x");
	MakeDir(d + "/Folder.iso");
	symlink((d + "/Game.iso").c_str(), (d + "/Link.iso").c_str());
	symlink((d + "/nowhere.iso").c_str(), (d + "/Dangling.iso").c_str());
	std::string why;
	Check(CheckLaunchPath(d + "/Game.iso", &why, ImageName), "launch: an .iso");
	Check(CheckLaunchPath(d + "/Game.chd", &why, ImageName), "launch: a .chd");
	Check(CheckLaunchPath(d + "/Game.bin", &why, ImageName), "launch: a .bin");
	Check(CheckLaunchPath(d + "/Link.iso", &why, ImageName), "launch: a symlink to a file");
	Check(!CheckLaunchPath(d + "/Missing.iso", &why, ImageName) && why == "missing", "launch: missing", why);
	Check(!CheckLaunchPath(d + "/Dangling.iso", &why, ImageName) && why == "missing", "launch: a dangling link is missing", why);
	Check(!CheckLaunchPath(d + "/Folder.iso", &why, ImageName) && why == "invalid", "launch: a folder", why);
	Check(!CheckLaunchPath(d + "/Empty.iso", &why, ImageName) && why == "invalid", "launch: an empty file", why);
	Check(!CheckLaunchPath(d + "/Game.elf", &why, ImageName) && why == "invalid", "launch: an .elf", why);
	Check(!CheckLaunchPath(d + "/notes.txt", &why, ImageName) && why == "invalid", "launch: a .txt", why);
	Check(!CheckLaunchPath(d + "/.hidden.iso", &why, ImageName) && why == "invalid", "launch: a hidden file", why);
	Check(!CheckLaunchPath(d + "/../launch/Game.iso", &why, ImageName) && why == "invalid", "launch: ..", why);
	Check(!CheckLaunchPath("Game.iso", &why, ImageName) && why == "invalid", "launch: relative", why);
	Check(!CheckLaunchPath("", &why, ImageName) && why == "invalid", "launch: empty", why);
	Check(!CheckLaunchPath("(unreadable)", &why, ImageName) && why == "invalid", "launch: the placeholder for an unreadable marker", why);
	Check(!CheckLaunchPath("/dev/cd0", &why, ImageName) && why == "invalid", "launch: /dev/cd0", why);
	Check(!CheckLaunchPath("/dev/disc.iso", &why, ImageName) && why == "invalid", "launch: under /dev (not missing: no tile starts a device)", why);
	Check(!CheckLaunchPath(d + "/a\tb.iso", &why, ImageName) && why == "invalid", "launch: a TAB", why);
	Check(CheckLaunchPath(d + "/Game.iso", nullptr, ImageName), "launch: no reason wanted");
	Check(CheckLaunchPath(d + "/notes.txt", &why, nullptr), "launch: no name check given: only the rest");

	// FindOwnTileDir.
	const std::string hb = root + "/homebrew";
	MakeDir(hb);
	auto tile = [&](const std::string& id, const std::string& game, const std::string& tile_id) {
		MakeDir(hb + "/" + id);
		Write(hb + "/" + id + "/game-tile.txt", game + "\nserial=S\ntitle=T\ntile=" + tile_id + "\nrev=1\n");
	};
	tile("PPSA99251", kGame, "PPSA99251");
	tile("PPSA99252", kGame2, "PPSA99252");
	MakeDir(hb + "/PPSA99203");
	Write(hb + "/PPSA99203/game-tile.txt", kGame + "\n"); // the main app is never a candidate
	MakeDir(hb + "/PPSA99231");
	Write(hb + "/PPSA99231/disc-tile.txt", "/dev/cd0\n"); // a disc tile either
	MakeDir(hb + "/OtherApp");
	Write(hb + "/OtherApp/game-tile.txt", kGame + "\n");
	struct stat seen = {}, none0 = {};
	stat((hb + "/PPSA99251/game-tile.txt").c_str(), &seen);
	Marker mk;
	mk.path = kGame;
	mk.tile_id = "PPSA99251";
	Eq(FindOwnTileDir(hb, mk, seen), hb + "/PPSA99251", "own dir: by id and the same file");
	{
		Marker other = mk; // the path as this process read it differs from the folder's first line: only the file's identity (inode, size, time) says it is the one
		other.path = "/data/PCSX2/games/Another Name.iso";
		Eq(FindOwnTileDir(hb, other, seen), hb + "/PPSA99251", "own dir: the same file by inode, size and time, whatever the first line says");
		Eq(FindOwnTileDir(hb, other, none0), "", "own dir: ... and with nothing seen and another first line: none");
	}
	mk.tile_id = "";
	Eq(FindOwnTileDir(hb, mk, seen), hb + "/PPSA99251", "own dir: no id: the ten ids are searched, the same file (inode, size, time)");
	struct stat none = {};
	Eq(FindOwnTileDir(hb, mk, none), hb + "/PPSA99251", "own dir: no stat seen: the first line");
	mk.tile_id = "PPSA99203";
	Eq(FindOwnTileDir(hb, mk, none), hb + "/PPSA99251", "own dir: an id outside the range: the ten ids are searched (the main app is not one)");
	mk.tile_id = "PPSA99252";
	Eq(FindOwnTileDir(hb, mk, none), "", "own dir: the id's folder has another game's marker: not it, and nothing else is tried");
	mk.tile_id = "PPSA99255";
	Eq(FindOwnTileDir(hb, mk, none), "", "own dir: the id's folder is not there");
	// the same game in two tiles and nothing else to tell them apart: never guessed
	tile("PPSA99253", kGame, "PPSA99253");
	mk.tile_id = "";
	Eq(FindOwnTileDir(hb, mk, none), "", "own dir: two folders with the same first line: not guessed");
	// ... and the file this process saw does not help: both count, so none is taken (the leave plan then only closes: never the wrong tile)
	Eq(FindOwnTileDir(hb, mk, seen), "", "own dir: two folders count (the first line and the inode of one): not guessed either");
	mk.tile_id = "PPSA99251";
	Eq(FindOwnTileDir(hb, mk, seen), hb + "/PPSA99251", "own dir: the id names the one to look at: it is the one");
	mk.tile_id = "";
	Eq(FindOwnTileDir(root + "/no-homebrew", mk, none), "", "own dir: no folder at all");
	mk.path = "";
	Eq(FindOwnTileDir(hb, mk, none), "", "own dir: no path and no stat: nothing to go by");
}

void TestI18n()
{
	// The two notification texts: one %s in the first, none in the second, in every language table.
	fe::SetLanguage(0, "");
	const std::string en_missing = fe::Tr(fe::Str::NotifyTileGameMissing), en_invalid = fe::Tr(fe::Str::NotifyTileGameInvalid);
	Eq(en_missing, "PS5SX2: the game of this home screen tile was not found (%s). Is its drive connected? The shelf opens instead.", "i18n: the English missing-game text");
	Eq(en_invalid, "PS5SX2: this home screen tile doesn't name a game PS5SX2 can start. The shelf opens instead.", "i18n: the English invalid-marker text");
	for (const int language : {1, 2, 3, 20, 4, 5, 6, 7, 17, 0})
	{
		fe::SetLanguage(language, "");
		const std::string code = fe::LanguageCode();
		const std::string miss = fe::Tr(fe::Str::NotifyTileGameMissing), inv = fe::Tr(fe::Str::NotifyTileGameInvalid);
		size_t count = 0, percents = 0;
		for (size_t at = miss.find("%s"); at != std::string::npos; at = miss.find("%s", at + 2))
			count++;
		for (const char c : miss)
			percents += c == '%';
		Check(count == 1 && percents == 1, "i18n " + code + ": exactly one %s (and no other %) in the missing-game text", miss);
		Check(inv.find('%') == std::string::npos, "i18n " + code + ": no conversion in the invalid-marker text", inv);
		Check(fe::SameFormat(miss.c_str(), en_missing.c_str()) && fe::SameFormat(inv.c_str(), en_invalid.c_str()), "i18n " + code + ": the same conversions as English");
		Check(!miss.empty() && !inv.empty() && miss.find("PS5SX2") != std::string::npos, "i18n " + code + ": not empty");
		Check(std::string(fe::Key(fe::Str::NotifyTileGameMissing)) == "notify.tile_game_missing" && std::string(fe::Key(fe::Str::NotifyTileGameInvalid)) == "notify.tile_game_invalid",
			"i18n " + code + ": the override keys");
	}
	fe::SetLanguage(0, "");
}
} // namespace

int main(int argc, char** argv)
{
	const std::string root = argc > 1 ? argv[1] : "/tmp/gametile-test";
	std::system(("rm -rf '" + root + "' && mkdir -p '" + root + "'").c_str());
	TestQueue(root); // first: it checks the counter of a process that has not written a request yet
	TestText();
	TestFormat();
	TestHelperFiles(root);
	TestTracker();
	TestPoll();
	TestMarker(root);
	TestI18n();
	std::printf("%d checks, %d failed\n", checks, fails);
	std::printf(fails ? "FAILED\n" : "all passed\n");
	return fails ? 1 : 0;
}
