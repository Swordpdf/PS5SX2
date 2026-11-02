// PS5 port frontend: LaunchBox art in the cover service (2.11, AI-assisted): the index (fe_launchbox.h), the order of the sources, the front /
// back / disc downloads, the replacement of a cached cover, and the disc art; and the back covers' protocol (who asks and when, who answers, the
// neighbours' backs fetched ahead, the order of the queue, the pauses that end, the painted card and its note). A fake network serves generated pictures.
//
//   launchbox_test <fixture dir> <work dir>      (test-launchbox.sh makes the pictures)
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_covers.h"
#include "fe_launchbox.h"
#include "fe_text.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace fe;

static int g_failures = 0;
#define CHECK(cond)                                                       \
	do                                                                    \
	{                                                                     \
		if (!(cond))                                                      \
		{                                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++;                                                 \
		}                                                                 \
	} while (0)

static std::string g_fix, g_work;

static bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

static bool Exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

static void WriteText(const std::string& path, const std::string& text)
{
	std::ofstream(path, std::ios::binary) << text;
}

// The fake network: url -> fixture file (or a plain status); every request is logged.
struct Net
{
	std::mutex mutex;
	std::map<std::string, std::string> files;   // url -> file under g_fix served with 200
	std::map<std::string, int> status;          // url -> status served with an empty body
	std::map<std::string, int> once;            // url -> status served with an empty body the first time only (then the file or status above)
	std::vector<std::string> log;
	std::atomic<bool> down{false};
	// 2.11: a request takes this long (it is logged when it starts, and the delay is outside the lock, so Count() and Total() never wait for it), and
	// how many were in flight at once at most (the cover service has one download thread: it must stay 1).
	std::atomic<int> delay_ms{0}, inflight{0}, peak_inflight{0};

	int Get(const std::string& url, std::vector<uint8_t>& out)
	{
		const int now_in = ++inflight;
		for (int seen = peak_inflight.load(); now_in > seen && !peak_inflight.compare_exchange_weak(seen, now_in);)
		{
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			log.push_back(url);
		}
		if (delay_ms > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms.load()));
		int answer;
		{
			std::lock_guard<std::mutex> lock(mutex);
			answer = Answer(url, out);
		}
		--inflight;
		return answer;
	}
	int Answer(const std::string& url, std::vector<uint8_t>& out)
	{
		out.clear();
		if (down)
			return -1;
		if (const auto o = once.find(url); o != once.end())
		{
			const int first = o->second;
			once.erase(o);
			return first;
		}
		const auto s = status.find(url);
		if (s != status.end())
			return s->second;
		const auto f = files.find(url);
		if (f == files.end())
			return 404;
		return ReadFileBytes(g_fix + "/" + f->second, out) ? 200 : 500;
	}
	std::vector<std::string> Snapshot()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return log;
	}
	size_t Count(const std::string& needle)
	{
		std::lock_guard<std::mutex> lock(mutex);
		size_t n = 0;
		for (const std::string& u : log)
			n += u.find(needle) != std::string::npos ? 1 : 0;
		return n;
	}
	size_t Total()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return log.size();
	}
	std::string First()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return log.empty() ? std::string() : log.front();
	}
};

static GameInfo MakeGame(const std::string& serial, const std::string& title)
{
	GameInfo g;
	g.path = "/games/" + serial + ".iso";
	g.stem = serial;
	g.title = title;
	g.serial = serial;
	g.region = "USA";
	return g;
}

// Polls the service until `done` holds or the time is out; every image goes through `take`.
template <class Take, class Done>
static bool Wait(CoverService& cs, double seconds, Take take, Done done)
{
	const auto t0 = std::chrono::steady_clock::now();
	while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds)
	{
		CoverImage img;
		while (cs.Poll(img))
			take(img);
		if (done())
			return true;
		usleep(20000);
	}
	return done();
}

static void Units()
{
	LaunchboxIndex idx;
	const std::string text =
		"# comment\n"
		"SLUS-21005 f=aaaaaaaa-1111-2222-3333-444444444444.jpg* b=bbbbbbbb-1111-2222-3333-444444444444.jpg d=dddddddd-1111-2222-3333-444444444444.png* s=ssssssss-1111-2222-3333-444444444444.jpg* m=1\r\n"
		"\n"
		"SLES-54006 f=r2_cccccccc-1111-2222-3333-444444444444.png m=2\n"
		"SLPM-66001 b=bbbbbbbb-5555-2222-3333-444444444444.jpg*\n"
		"SLUS-99999 f=../../etc/passwd m=1\n"      // a file name that is no file name: the whole line is refused
		"SLUS-99998 f=has space.jpg m=1\n"          // (the space splits it: "space.jpg" is no field, f= has no value)
		"bad lower-case f=aaaaaaaa.jpg\n"            // not a serial
		"SLUS-99997\n"                              // no art
		"SLUS-99996 z=whatever m=1\n"                // an unknown field only
		"SLES-54006 f=r2_dddddddd-1111-2222-3333-444444444444.png* m=1\n"; // the later line replaces the earlier one
	const size_t added = idx.Add(text);
	CHECK(added == 4);
	CHECK(idx.Size() == 3);
	const LaunchboxArt* kh = idx.Find("SLUS-21005");
	CHECK(kh && kh->front == "aaaaaaaa-1111-2222-3333-444444444444.jpg" && kh->front_exact);
	CHECK(kh && kh->back == "bbbbbbbb-1111-2222-3333-444444444444.jpg" && !kh->back_exact);
	CHECK(kh && kh->disc == "dddddddd-1111-2222-3333-444444444444.png" && kh->disc_exact && kh->tier == 1);
	CHECK(kh && LaunchboxIndex::CanUpgrade(*kh));
	CHECK(kh && kh->spine == "ssssssss-1111-2222-3333-444444444444.jpg" && kh->spine_exact);
	const LaunchboxArt* cars = idx.Find("SLES-54006");
	CHECK(cars && cars->front.compare(0, 3, "r2_") == 0 && cars->front_exact && cars->front.find("dddddddd") != std::string::npos);
	const LaunchboxArt* ja = idx.Find("SLPM-66001");
	CHECK(ja && ja->front.empty() && ja->back_exact && !LaunchboxIndex::CanUpgrade(*ja));
	CHECK(!idx.Find("SLUS-99999") && !idx.Find("SLUS-99998") && !idx.Find("SLUS-99997") && !idx.Find("SLUS-99996"));
	CHECK(!idx.Find("SLUS-00000"));
	CHECK(LaunchboxIndex::Url("abc.jpg") == "https://images.launchbox-app.com/abc.jpg");
	CHECK(LaunchboxIndex::Url("r2_abc.png") == "https://gamesdb-images.launchbox.gg/r2_abc.png");
	LaunchboxArt tier2;
	tier2.tier = 2;
	tier2.front = "x.jpg";
	tier2.front_exact = true;
	CHECK(!LaunchboxIndex::CanUpgrade(tier2)); // only a title matched as it stands may replace a cover
	tier2.tier = 1;
	tier2.front_exact = false;
	CHECK(!LaunchboxIndex::CanUpgrade(tier2)); // and only the serial's own region

	// The order of the sources: the serial's own region first, the configured source, other regions last.
	auto shared = std::make_shared<LaunchboxIndex>();
	shared->Add("SLUS-00001 f=front-exact.jpg* b=back-exact.jpg* d=disc.png* s=spine-1.jpg* m=1\n"
				"SLUS-00002 f=front-other.jpg b=back-other.jpg d=disc2.png m=1\n");
	CoverConfig cfg;
	cfg.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
	cfg.url_template = "https://example.test/covers/${serial}.jpg";
	cfg.back_url_template = "https://example.test/backs/${serial}.jpg";
	cfg.launchbox = shared;
	auto urls = CoverService::FrontUrls(cfg, "SLUS-00001");
	CHECK(urls.size() == 2 && urls[0] == "https://images.launchbox-app.com/front-exact.jpg" && urls[1] == "https://example.test/covers/SLUS-00001.jpg");
	urls = CoverService::FrontUrls(cfg, "SLUS-00002");
	CHECK(urls.size() == 2 && urls[0] == "https://example.test/covers/SLUS-00002.jpg" && urls[1] == "https://images.launchbox-app.com/front-other.jpg");
	urls = CoverService::FrontUrls(cfg, "SLUS-00003");
	CHECK(urls.size() == 1 && urls[0] == "https://example.test/covers/SLUS-00003.jpg");
	urls = CoverService::BackUrls(cfg, "SLUS-00001");
	CHECK(urls.size() == 2 && urls[0] == "https://images.launchbox-app.com/back-exact.jpg" && urls[1] == "https://example.test/backs/SLUS-00001.jpg");
	urls = CoverService::BackUrls(cfg, "SLUS-00002");
	CHECK(urls.size() == 2 && urls[0] == "https://example.test/backs/SLUS-00002.jpg" && urls[1] == "https://images.launchbox-app.com/back-other.jpg");
	urls = CoverService::SpineUrls(cfg, "SLUS-00001");
	CHECK(urls.size() == 1 && urls[0] == "https://images.launchbox-app.com/spine-1.jpg");
	CHECK(CoverService::SpineUrls(cfg, "SLUS-00002").empty());
	urls = CoverService::DiscUrls(cfg, "SLUS-00002");
	CHECK(urls.size() == 1 && urls[0] == "https://images.launchbox-app.com/disc2.png");
	CHECK(CoverService::DiscUrls(cfg, "SLUS-00003").empty());
	cfg.url_template.clear();
	cfg.back_url_template.clear();
	CHECK(CoverService::FrontUrls(cfg, "SLUS-00003").empty());
	CHECK(CoverService::FrontUrls(cfg, "SLUS-00002").size() == 1);
	cfg.launchbox.reset();
	CHECK(CoverService::FrontUrls(cfg, "SLUS-00001").empty() && CoverService::BackUrls(cfg, "SLUS-00001").empty());
	std::printf("units: done\n");
}

// ---- 2.11 (swordpdf: "back covers are not loading instantly"): what asks for a back, who answers it and when, the neighbours' backs fetched ahead, the
// order of the downloads, the pauses -----------------------------------------------------------------------------------------------------------------

static const std::string kLbHost = "https://images.launchbox-app.com/";

static std::string BackSerial(int group, int i)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "SLUS-%d%04d", group, i);
	return buf;
}

static void CopyFixture(const std::string& fixture, const std::string& to)
{
	std::vector<uint8_t> bytes;
	if (ReadFileBytes(g_fix + "/" + fixture, bytes))
		std::ofstream(to, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

static double Since(std::chrono::steady_clock::time_point t0)
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

static const auto kNever = [] { return false; };

// A. A back asked for while the painting thread is busy with a long list is answered between the stages of the game it is on: quickly, and without that
// game (or any other) losing its cover or spine.
static void BackInline(Fonts& fonts)
{
	std::printf("-- back: answered inside the painting pass\n");
	const std::string dir = g_work + "/back-a", cache = dir + "/cache";
	CHECK(system(("rm -rf '" + dir + "' && mkdir -p '" + cache + "' '" + dir + "/covers'").c_str()) == 0);
	const int n = 120, first_back = 50, backs = 10; // (long enough a list that the painting is still going when the backs are asked for)
	std::vector<GameInfo> games;
	for (int i = 0; i < n; i++)
	{
		games.push_back(MakeGame(BackSerial(6, i), "Game " + std::to_string(i)));
		const std::string base = cache + "/" + games.back().serial;
		CopyFixture("front_a.jpg", base + ".jpg");
		CopyFixture("spine.jpg", base + "_spine.jpg");
		if (i >= first_back && i < first_back + backs)
			CopyFixture("back.jpg", base + "_back.jpg");
	}
	CoverConfig cfg;
	cfg.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
	cfg.manual_dir = dir + "/covers";
	cfg.cache_dir = cache;
	cfg.allow_download = false;
	CoverService cs;
	cs.Start(games, &fonts, cfg, nullptr);
	for (int i = 0; i < n; i++)
		cs.Want(i);
	cs.SetSelected(first_back + backs / 2);
	std::vector<int> covers(n), spines(n), holds(n);
	std::map<int, std::chrono::steady_clock::time_point> asked;
	std::map<int, double> back_ms;
	auto take = [&](CoverImage& img) {
		if (img.game < 0 || img.game >= n)
			return;
		if (img.kind == CoverImage::Cover)
			covers[static_cast<size_t>(img.game)]++;
		else if (img.kind == CoverImage::Spine)
			spines[static_cast<size_t>(img.game)]++;
		else if (img.kind == CoverImage::Placeholder)
			holds[static_cast<size_t>(img.game)]++;
		else if (img.kind == CoverImage::RealBack && !back_ms.count(img.game) && asked.count(img.game))
			back_ms[img.game] = Since(asked[img.game]);
	};
	auto frame = [&] { // the shelf's loop: four images every 16 ms
		CoverImage img;
		for (int k = 0; k < 4 && cs.Poll(img); k++)
			take(img);
		usleep(16000);
	};
	for (int b = 0; b < backs; b++) // one back asked for every frame while the list is being painted
	{
		asked[first_back + b] = std::chrono::steady_clock::now();
		cs.WantBack(first_back + b);
		frame();
	}
	const auto all_done = [&] {
		if (back_ms.size() < static_cast<size_t>(backs))
			return false;
		for (int i = 0; i < n; i++)
			if (!covers[static_cast<size_t>(i)] || !spines[static_cast<size_t>(i)] || !holds[static_cast<size_t>(i)])
				return false;
		return true;
	};
	const auto t0 = std::chrono::steady_clock::now();
	while (!all_done() && Since(t0) < 30.0)
		frame();
	CHECK(back_ms.size() == static_cast<size_t>(backs));
	double slowest = 0;
	for (const auto& kv : back_ms)
		slowest = std::max(slowest, kv.second);
	std::printf("the %d backs came at most %.0f ms after they were asked for\n", backs, slowest * 1000.0);
	CHECK(slowest < 1.0);
	for (int i = 0; i < n; i++) // no game lost its pass to the back (a continue out of it would have left one without cover or spine)
		CHECK(covers[static_cast<size_t>(i)] >= 1 && spines[static_cast<size_t>(i)] >= 1 && holds[static_cast<size_t>(i)] >= 1);
	CHECK(cs.Stop(5000));
}

// B, C, D. WantBack(i) looks on disk only; the network is used when the shelf says so (WantBack(i, true)), once however the calls come, and a game the shelf
// let go of is not fetched on a permission that arrives for it later.
static void BackGate(Fonts& fonts)
{
	std::printf("-- back: the permission to download\n");
	const std::string dir = g_work + "/back-b", cache = dir + "/cache";
	CHECK(system(("rm -rf '" + dir + "' && mkdir -p '" + cache + "' '" + dir + "/covers'").c_str()) == 0);
	std::vector<GameInfo> games;
	auto idx = std::make_shared<LaunchboxIndex>();
	Net net;
	for (int i = 0; i < 6; i++)
	{
		games.push_back(MakeGame(BackSerial(7, i), "Game " + std::to_string(i)));
		idx->Add(games.back().serial + " b=lb-b" + std::to_string(i) + "-back.jpg* m=1\n");
		net.files[kLbHost + "lb-b" + std::to_string(i) + "-back.jpg"] = "back.jpg";
	}
	CoverConfig cfg;
	cfg.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
	cfg.manual_dir = dir + "/covers";
	cfg.cache_dir = cache;
	cfg.allow_download = true;
	cfg.launchbox = idx;
	CoverService cs;
	cs.Start(games, &fonts, cfg, [&net](const std::string& url, std::vector<uint8_t>& out) { return net.Get(url, out); });
	for (int i = 0; i < 6; i++)
		cs.Want(i);
	std::map<int, std::string> real;
	std::map<int, int> real_count;
	auto take = [&](CoverImage& img) {
		if (img.kind == CoverImage::RealBack)
		{
			real[img.game] = img.source;
			real_count[img.game]++;
		}
	};
	// B. Looking on disk finds nothing and asks nobody; the permission makes the one request.
	cs.SetSelected(0);
	cs.WantBack(0);
	CHECK(!Wait(cs, 0.6, take, [&] { return real.count(0) > 0; }));
	CHECK(net.Total() == 0);
	cs.WantBack(0, true);
	CHECK(Wait(cs, 10.0, take, [&] { return real.count(0) > 0; }));
	CHECK(net.Total() == 1 && net.Count("lb-b0-back.jpg") == 1 && real[0] == "launchbox");
	CHECK(Wait(cs, 5.0, take, [&] { return Exists(cache + "/" + games[0].serial + "_back.jpg"); })); // (the file is written after the picture is handed over)
	cs.WantBack(0, true); // asking again for what has been asked costs nothing
	cs.WantBack(0);
	Wait(cs, 0.4, take, kNever);
	CHECK(net.Total() == 1 && real_count[0] == 1);
	// C. Both calls before the lookup has run: the lookup sees the permission, one request.
	cs.SetSelected(1);
	cs.WantBack(1);
	cs.WantBack(1, true);
	CHECK(Wait(cs, 10.0, take, [&] { return real.count(1) > 0; }));
	Wait(cs, 0.3, take, kNever);
	CHECK(net.Count("lb-b1-back.jpg") == 1 && real_count[1] == 1);
	// ... and the permission alone is enough.
	cs.SetSelected(2);
	cs.WantBack(2, true);
	CHECK(Wait(cs, 10.0, take, [&] { return real.count(2) > 0; }));
	CHECK(net.Count("lb-b2-back.jpg") == 1);
	// D. Looked up and missed, waiting for the permission; the shelf lets the game go: nothing is fetched for it, and a later ask starts afresh.
	cs.SetSelected(3);
	cs.WantBack(3);
	Wait(cs, 0.5, take, kNever);
	CHECK(net.Count("lb-b3-back.jpg") == 0);
	cs.ForgetBack(3);
	Wait(cs, 0.5, take, kNever);
	CHECK(net.Count("lb-b3-back.jpg") == 0 && !real.count(3));
	cs.WantBack(3, true);
	CHECK(Wait(cs, 10.0, take, [&] { return real.count(3) > 0; }));
	CHECK(net.Count("lb-b3-back.jpg") == 1);
	// A permission that comes while a game is no longer wanted is ignored.
	cs.Forget(4);
	cs.WantBack(4, true);
	Wait(cs, 0.4, take, kNever);
	CHECK(net.Count("lb-b4-back.jpg") == 0 && !real.count(4));
	CHECK(net.peak_inflight <= 1); // one request at a time, whatever was asked
	CHECK(cs.Stop(5000));
}

// E. The backs of the neighbours are fetched ahead: after the picked game's back, nearest first, before the spines that are queued; into the cache and
// nowhere else; and the flip then finds the file there without a request.
static void BackRing(Fonts& fonts)
{
	std::printf("-- back: the neighbours' backs ahead of the flip\n");
	const std::string dir = g_work + "/back-e", cache = dir + "/cache";
	CHECK(system(("rm -rf '" + dir + "' && mkdir -p '" + cache + "' '" + dir + "/covers'").c_str()) == 0);
	const int n = 20;
	std::vector<GameInfo> games;
	auto idx = std::make_shared<LaunchboxIndex>();
	Net net;
	net.delay_ms = 120; // a request takes a while, as it does on the console: the order of the queue shows
	for (int i = 0; i < n; i++)
	{
		games.push_back(MakeGame(BackSerial(8, i), "Game " + std::to_string(i)));
		const std::string id = std::to_string(i), base = cache + "/" + games.back().serial;
		idx->Add(games.back().serial + " f=lb-e" + id + "-front.jpg* b=lb-e" + id + "-back.jpg* s=lb-e" + id + "-spine.jpg* m=1\n");
		net.files[kLbHost + "lb-e" + id + "-back.jpg"] = "back.jpg";
		net.files[kLbHost + "lb-e" + id + "-spine.jpg"] = "spine.jpg";
		CopyFixture("front_a.jpg", base + ".jpg"); // a cover from LaunchBox already: nothing to download or to improve
		WriteText(base + ".lb", "1\n");
	}
	CoverConfig cfg;
	cfg.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
	cfg.manual_dir = dir + "/covers";
	cfg.cache_dir = cache;
	cfg.allow_download = true;
	cfg.launchbox = idx;
	CoverService cs;
	cs.Start(games, &fonts, cfg, [&net](const std::string& url, std::vector<uint8_t>& out) { return net.Get(url, out); });
	for (int i = 0; i < n; i++)
		cs.Want(i);
	cs.SetSelected(10);
	std::map<int, std::string> real;
	auto take = [&](CoverImage& img) {
		if (img.kind == CoverImage::RealBack)
			real[img.game] = img.source;
	};
	CHECK(Wait(cs, 10.0, take, [&] { return net.Total() >= 2; })); // the spines are on their way (one per painted game)
	const size_t mark = net.Total();
	cs.WantBack(10, true);
	const std::vector<int> ring = {11, 9, 12, 8, 13, 7};
	cs.PrefetchBacks(ring);
	CHECK(Wait(cs, 30.0, take, [&] { return Exists(cache + "/" + games[7].serial + "_back.jpg") && real.count(10); }));
	const std::vector<std::string> log = net.Snapshot();
	size_t first_back = std::string::npos;
	for (size_t k = 0; k < log.size() && first_back == std::string::npos; k++)
		if (log[k].find("-back.jpg") != std::string::npos)
			first_back = k;
	CHECK(first_back != std::string::npos && first_back <= mark + 1); // (a spine already on its way may come first)
	const std::vector<int> want_order = {10, 11, 9, 12, 8, 13, 7};
	for (size_t k = 0; k < want_order.size() && first_back != std::string::npos; k++)
		CHECK(first_back + k < log.size() && log[first_back + k] == kLbHost + "lb-e" + std::to_string(want_order[k]) + "-back.jpg");
	CHECK(first_back != std::string::npos && log.size() > first_back + want_order.size() && log[first_back + want_order.size()].find("spine") != std::string::npos);
	for (int i = 0; i < n; i++) // exactly those backs are in the cache
	{
		const bool wanted = std::find(want_order.begin(), want_order.end(), i) != want_order.end();
		CHECK(Exists(cache + "/" + games[static_cast<size_t>(i)].serial + "_back.jpg") == wanted);
	}
	CHECK(real.size() == 1 && real.count(10) && real[10] == "launchbox"); // no image was made for the neighbours' backs
	// The flip to a neighbour: the file is found on disk, no request.
	const size_t asked_before = net.Count("lb-e11-back.jpg");
	cs.SetSelected(11);
	cs.WantBack(11, true);
	CHECK(Wait(cs, 10.0, take, [&] { return real.count(11) > 0; }));
	CHECK(real[11] == "cache" && net.Count("lb-e11-back.jpg") == asked_before && asked_before == 1);
	// The ring is replaced, not added to: an empty list ends it.
	cs.PrefetchBacks({3, 4});
	cs.PrefetchBacks({});
	const size_t total = net.Count("-back.jpg");
	Wait(cs, 0.6, take, kNever);
	CHECK(net.Count("-back.jpg") == total);
	CHECK(net.peak_inflight <= 1);
	CHECK(cs.Stop(5000));
}

// F. The ring keeps to what is known: a 404 or a picture the shelf can't use is remembered (once), nothing is fetched for a back that is on disk or marked,
// and the flag file's setting (back_prefetch off) turns the whole thing off.
static void BackNegatives(Fonts& fonts)
{
	std::printf("-- back: the neighbours' backs keep to what is known\n");
	const std::string dir = g_work + "/back-f", cache = dir + "/cache";
	CHECK(system(("rm -rf '" + dir + "' && mkdir -p '" + cache + "' '" + dir + "/covers'").c_str()) == 0);
	std::vector<GameInfo> games;
	auto idx = std::make_shared<LaunchboxIndex>();
	Net net;
	for (int i = 0; i < 4; i++)
	{
		games.push_back(MakeGame(BackSerial(9, i), "Game " + std::to_string(i)));
		idx->Add(games.back().serial + " b=lb-f" + std::to_string(i) + "-back.jpg* m=1\n");
	}
	net.files[kLbHost + "lb-f1-back.jpg"] = "huge.jpg"; // 5000 pixels wide: refused
	net.files[kLbHost + "lb-f2-back.jpg"] = "back.jpg";
	net.files[kLbHost + "lb-f3-back.jpg"] = "back.jpg";
	// game 0: a 404 (nothing served). Game 3's back is on disk already (the player's, in covers/).
	CopyFixture("back.jpg", dir + "/covers/" + games[3].serial + "_back.jpg");
	CoverConfig cfg;
	cfg.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
	cfg.manual_dir = dir + "/covers";
	cfg.cache_dir = cache;
	cfg.allow_download = true;
	cfg.launchbox = idx;
	CoverService cs;
	cs.Start(games, &fonts, cfg, [&net](const std::string& url, std::vector<uint8_t>& out) { return net.Get(url, out); });
	for (int i = 0; i < 4; i++)
		cs.Want(i);
	std::map<int, bool> back_none; // the painted back's last note flag
	std::map<int, int> real;
	auto take = [&](CoverImage& img) {
		if (img.kind == CoverImage::Back)
			back_none[img.game] = img.none;
		if (img.kind == CoverImage::RealBack)
			real[img.game]++;
	};
	CHECK(Wait(cs, 5.0, take, [&] { return back_none.size() == 4; }));
	CHECK(!back_none[0] && !back_none[1] && !back_none[2] && !back_none[3]); // a back may come for each: the card says nothing yet
	cs.PrefetchBacks({0, 1, 2, 3});
	CHECK(Wait(cs, 10.0, take, [&] { return Exists(cache + "/" + games[2].serial + "_back.jpg") && Exists(cache + "/" + games[0].serial + "_back.missing") &&
										  Exists(cache + "/" + games[1].serial + "_back.missing"); }));
	CHECK(!Exists(cache + "/" + games[1].serial + "_back.jpg")); // the 5000-pixel picture was refused, not kept
	CHECK(!Exists(cache + "/" + games[2].serial + "_back.missing") && !Exists(cache + "/" + games[3].serial + "_back.jpg"));
	CHECK(net.Count("lb-f0-back.jpg") == 1 && net.Count("lb-f1-back.jpg") == 1 && net.Count("lb-f2-back.jpg") == 1 && net.Count("lb-f3-back.jpg") == 0);
	cs.PrefetchBacks({0, 1, 2, 3}); // again: the marks and the files answer
	Wait(cs, 0.6, take, kNever);
	CHECK(net.Total() == 3);
	// The picked game's own ask finds the marks too: no request, and the card with the note (that no back can come) replaces the neutral one.
	cs.WantBack(0, true);
	cs.WantBack(1, true);
	CHECK(Wait(cs, 5.0, take, [&] { return back_none[0] && back_none[1]; }));
	CHECK(net.Total() == 3 && !real.count(0) && !real.count(1));
	cs.WantBack(2, true); // game 2's back is in the cache now: a picture, no request
	CHECK(Wait(cs, 5.0, take, [&] { return real.count(2) > 0; }));
	CHECK(net.Total() == 3);
	CHECK(cs.Stop(5000));
	// back_prefetch off: PrefetchBacks does nothing.
	{
		const std::string cache2 = dir + "/cache2";
		CHECK(system(("mkdir -p '" + cache2 + "'").c_str()) == 0);
		Net quiet;
		quiet.files[kLbHost + "lb-f2-back.jpg"] = "back.jpg";
		CoverConfig off = cfg;
		off.cache_dir = cache2;
		off.back_prefetch = false;
		CoverService s2;
		s2.Start(games, &fonts, off, [&quiet](const std::string& url, std::vector<uint8_t>& out) { return quiet.Get(url, out); });
		for (int i = 0; i < 4; i++)
			s2.Want(i);
		s2.PrefetchBacks({2});
		Wait(s2, 0.8, [](CoverImage&) {}, kNever);
		CHECK(quiet.Total() == 0 && !Exists(cache2 + "/" + games[2].serial + "_back.jpg"));
		s2.WantBack(2, true); // the picked game's own back still comes
		CHECK(Wait(s2, 5.0, [](CoverImage&) {}, [&] { return Exists(cache2 + "/" + games[2].serial + "_back.jpg"); }));
		CHECK(s2.Stop(5000));
	}
}

// G. A failure pauses the downloads for a while, not for the session: a rate limit on one of the neighbours' backs stops LaunchBox for the back-off time
// and the queue goes on by itself afterwards; the configured source failing pauses everything and the download thread lives on to serve the back asked for.
static void BackPauses(Fonts& fonts)
{
	std::printf("-- back: a pause ends\n");
	const std::string dir = g_work + "/back-g", kOther = "https://example.test/backs/";
	{
		const std::string cache = dir + "/cache1";
		CHECK(system(("rm -rf '" + dir + "' && mkdir -p '" + cache + "' '" + dir + "/covers'").c_str()) == 0);
		std::vector<GameInfo> games;
		auto idx = std::make_shared<LaunchboxIndex>();
		Net net;
		for (int i = 0; i < 4; i++)
		{
			games.push_back(MakeGame(BackSerial(5, i), "Game " + std::to_string(i)));
			idx->Add(games.back().serial + " b=lb-g" + std::to_string(i) + "-back.jpg* m=1\n");
			net.files[kLbHost + "lb-g" + std::to_string(i) + "-back.jpg"] = "back.jpg";
		}
		net.status[kLbHost + "lb-g0-back.jpg"] = 429; // slow down
		CoverConfig cfg;
		cfg.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
		cfg.manual_dir = dir + "/covers";
		cfg.cache_dir = cache;
		cfg.allow_download = true;
		cfg.launchbox = idx;
		cfg.net_retry_ms = 300;
		CoverService cs;
		cs.Start(games, &fonts, cfg, [&net](const std::string& url, std::vector<uint8_t>& out) { return net.Get(url, out); });
		for (int i = 0; i < 4; i++)
			cs.Want(i);
		const auto none = [](CoverImage&) {};
		Wait(cs, 0.3, none, kNever); // (the painting is done: no spine or front is asked of LaunchBox in this setup)
		CHECK(net.Total() == 0);
		cs.PrefetchBacks({0, 1, 2});
		CHECK(Wait(cs, 5.0, none, [&] { return net.Count("lb-g0-back.jpg") == 1; }));
		const auto t429 = std::chrono::steady_clock::now();
		Wait(cs, 0.12, none, kNever);
		CHECK(net.Total() == 1); // LaunchBox is paused: the neighbours wait in the queue
		CHECK(Wait(cs, 10.0, none, [&] { return Exists(cache + "/" + games[2].serial + "_back.jpg") && Exists(cache + "/" + games[1].serial + "_back.jpg"); }));
		CHECK(Since(t429) >= 0.2); // and went on when it was over, by themselves
		CHECK(net.Total() == 3 && !Exists(cache + "/" + games[0].serial + "_back.jpg") && !Exists(cache + "/" + games[0].serial + "_back.missing")); // (a rate limit is no 404)
		CHECK(cs.Stop(5000));
	}
	{
		const std::string cache = dir + "/cache2";
		CHECK(system(("mkdir -p '" + cache + "'").c_str()) == 0);
		std::vector<GameInfo> games = {MakeGame(BackSerial(5, 10), "No LaunchBox"), MakeGame(BackSerial(5, 11), "With LaunchBox")};
		auto idx = std::make_shared<LaunchboxIndex>();
		idx->Add(games[1].serial + " b=lb-g11-back.jpg* m=1\n");
		Net net;
		net.status[kOther + games[0].serial + ".jpg"] = -1; // the configured source: no answer
		net.files[kLbHost + "lb-g11-back.jpg"] = "back.jpg";
		CoverConfig cfg;
		cfg.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
		cfg.manual_dir = dir + "/covers";
		cfg.cache_dir = cache;
		cfg.allow_download = true;
		cfg.launchbox = idx;
		cfg.back_url_template = kOther + "${serial}.jpg";
		cfg.net_retry_ms = 300;
		CoverService cs;
		cs.Start(games, &fonts, cfg, [&net](const std::string& url, std::vector<uint8_t>& out) { return net.Get(url, out); });
		cs.Want(0);
		cs.Want(1);
		int got = 0;
		const auto take = [&](CoverImage& img) { got += img.kind == CoverImage::RealBack && img.game == 1 ? 1 : 0; };
		cs.SetSelected(0);
		cs.WantBack(0, true);
		CHECK(Wait(cs, 5.0, take, [&] { return net.Count(games[0].serial + ".jpg") == 1; }));
		cs.SetSelected(1);
		cs.WantBack(1, true); // asked while everything is paused
		Wait(cs, 0.12, take, kNever);
		CHECK(got == 0);
		CHECK(Wait(cs, 10.0, take, [&] { return got == 1; })); // the thread lived on, and served it when the pause was over
		CHECK(Wait(cs, 10.0, take, [&] { return net.Count(games[0].serial + ".jpg") >= 2; })); // game 0's request began the pause: it is tried again after it
		CHECK(cs.Stop(5000));
	}
	{
		// The picked game's back whose request was the one that began a pause (a rate limit) is not dropped: it is fetched when the pause is over.
		const std::string cache = dir + "/cache3";
		CHECK(system(("mkdir -p '" + cache + "'").c_str()) == 0);
		std::vector<GameInfo> games = {MakeGame(BackSerial(5, 20), "Rate limited")};
		auto idx = std::make_shared<LaunchboxIndex>();
		idx->Add(games[0].serial + " b=lb-g20-back.jpg* m=1\n");
		Net net;
		net.once[kLbHost + "lb-g20-back.jpg"] = 429; // a rate limit, the first time only
		net.files[kLbHost + "lb-g20-back.jpg"] = "back.jpg";
		CoverConfig cfg;
		cfg.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
		cfg.manual_dir = dir + "/covers";
		cfg.cache_dir = cache;
		cfg.allow_download = true;
		cfg.launchbox = idx;
		cfg.net_retry_ms = 300;
		CoverService cs;
		cs.Start(games, &fonts, cfg, [&net](const std::string& url, std::vector<uint8_t>& out) { return net.Get(url, out); });
		cs.Want(0);
		int got = 0;
		const auto take = [&](CoverImage& img) { got += img.kind == CoverImage::RealBack && img.game == 0 ? 1 : 0; };
		cs.SetSelected(0);
		cs.WantBack(0, true);
		CHECK(Wait(cs, 5.0, take, [&] { return net.Count("lb-g20-back.jpg") == 1; }));
		const auto t429 = std::chrono::steady_clock::now();
		Wait(cs, 0.1, take, kNever);
		CHECK(got == 0 && net.Count("lb-g20-back.jpg") == 1); // still paused
		CHECK(Wait(cs, 10.0, take, [&] { return got == 1; }));
		CHECK(Since(t429) >= 0.2 && net.Count("lb-g20-back.jpg") == 2);
		CHECK(cs.Stop(5000));
	}
	{
		// (review) One request without an answer (a connection that failed, no pause yet: LaunchBox is left out after three) does not lose the picked game's
		// back: it is asked for again at once. And for ever failing it stops with the pause (three requests), not spinning.
		const std::string cache = dir + "/cache4";
		CHECK(system(("mkdir -p '" + cache + "'").c_str()) == 0);
		std::vector<GameInfo> games = {MakeGame(BackSerial(5, 30), "Hiccup"), MakeGame(BackSerial(5, 31), "Down")};
		auto idx = std::make_shared<LaunchboxIndex>();
		idx->Add(games[0].serial + " b=lb-g30-back.jpg* m=1\n" + games[1].serial + " b=lb-g31-back.jpg* m=1\n");
		Net net;
		net.once[kLbHost + "lb-g30-back.jpg"] = -1; // the first request gets no answer
		net.files[kLbHost + "lb-g30-back.jpg"] = "back.jpg";
		net.status[kLbHost + "lb-g31-back.jpg"] = -1; // always
		CoverConfig cfg;
		cfg.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
		cfg.manual_dir = dir + "/covers";
		cfg.cache_dir = cache;
		cfg.allow_download = true;
		cfg.launchbox = idx;
		cfg.net_retry_ms = 60000;
		CoverService cs;
		cs.Start(games, &fonts, cfg, [&net](const std::string& url, std::vector<uint8_t>& out) { return net.Get(url, out); });
		cs.Want(0);
		cs.Want(1);
		int got = 0;
		const auto take = [&](CoverImage& img) { got += img.kind == CoverImage::RealBack && img.game == 0 ? 1 : 0; };
		cs.SetSelected(0);
		cs.WantBack(0, true);
		CHECK(Wait(cs, 5.0, take, [&] { return got == 1; }));
		CHECK(net.Count("lb-g30-back.jpg") == 2);
		cs.ForgetBack(0);
		cs.SetSelected(1);
		cs.WantBack(1, true);
		Wait(cs, 1.0, take, kNever);
		CHECK(net.Count("lb-g31-back.jpg") == 3); // three requests, then LaunchBox is paused (a minute here): no more
		CHECK(cs.Stop(5000));
	}
}

// G2. (review) A game whose LaunchBox address was not asked because LaunchBox is paused is not marked as having no cover when the other source says 404:
// LaunchBox was never consulted, and the pause ends.
static void PauseNoMark(Fonts& fonts)
{
	std::printf("-- cover: a source that was paused is not blamed\n");
	const std::string dir = g_work + "/pause-nomark", cache = dir + "/cache", kOther = "https://example.test/covers/";
	CHECK(system(("rm -rf '" + dir + "' && mkdir -p '" + cache + "' '" + dir + "/covers'").c_str()) == 0);
	std::vector<GameInfo> games = {MakeGame("SLUS-31000", "Rate limited"), MakeGame("SLUS-31001", "Paused")};
	auto idx = std::make_shared<LaunchboxIndex>();
	idx->Add("SLUS-31000 f=lb-n0.jpg* m=1\nSLUS-31001 f=lb-n1.jpg* m=1\n");
	Net net;
	net.status[kLbHost + "lb-n0.jpg"] = 429;            // the first game's LaunchBox answer pauses LaunchBox (for a minute here)
	net.files[kOther + games[0].serial + ".jpg"] = "front_b.jpg";
	// the second game: LaunchBox is not asked (paused), the configured source has nothing (404)
	CoverConfig cfg;
	cfg.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
	cfg.manual_dir = dir + "/covers";
	cfg.cache_dir = cache;
	cfg.url_template = kOther + "${serial}.jpg";
	cfg.allow_download = true;
	cfg.launchbox = idx;
	cfg.net_retry_ms = 60000;
	CoverService cs;
	cs.Start(games, &fonts, cfg, [&net](const std::string& url, std::vector<uint8_t>& out) { return net.Get(url, out); });
	cs.SetSelected(0);
	cs.Want(0);
	cs.Want(1);
	CHECK(Wait(cs, 10.0, [](CoverImage&) {}, [&] { return net.Count(games[1].serial + ".jpg") >= 1 && Exists(cache + "/" + games[0].serial + ".jpg"); }));
	Wait(cs, 0.4, [](CoverImage&) {}, kNever);
	CHECK(net.Count("lb-n1.jpg") == 0); // LaunchBox was not asked for the second game
	CHECK(!Exists(cache + "/" + games[1].serial + ".missing"));
	CHECK(cs.Stop(5000));
}

// H. The painted back says "No back cover" only where none can come; a card painted without the note is replaced by one with it when it turns out that
// none exists, once.
static void BackNote(Fonts& fonts)
{
	std::printf("-- back: the painted card and its note\n");
	const std::string dir = g_work + "/back-h", cache = dir + "/cache";
	CHECK(system(("rm -rf '" + dir + "' && mkdir -p '" + cache + "' '" + dir + "/covers'").c_str()) == 0);
	std::vector<GameInfo> games;
	for (int i = 0; i < 4; i++)
		games.push_back(MakeGame(BackSerial(4, i), "Game " + std::to_string(i)));
	auto idx = std::make_shared<LaunchboxIndex>();
	idx->Add(games[1].serial + " b=lb-h1-back.jpg* m=1\n" + games[2].serial + " b=lb-h2-back.jpg* m=1\n" + games[3].serial + " b=lb-h3-back.jpg* m=1\n");
	Net net;
	net.files[kLbHost + "lb-h1-back.jpg"] = "back.jpg";
	// game 0: no source at all. Game 1: LaunchBox has its back. Game 2: marked as having none (a 404 last week). Game 3: LaunchBox lists one that is gone.
	WriteText(cache + "/" + games[2].serial + "_back.missing", "404 lb\n");
	CoverConfig cfg;
	cfg.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
	cfg.manual_dir = dir + "/covers";
	cfg.cache_dir = cache;
	cfg.allow_download = true;
	cfg.launchbox = idx;
	CoverService cs;
	cs.Start(games, &fonts, cfg, [&net](const std::string& url, std::vector<uint8_t>& out) { return net.Get(url, out); });
	for (int i = 0; i < 4; i++)
		cs.Want(i);
	std::map<int, std::vector<bool>> cards; // the painted backs that came, per game, with their note flag
	std::map<int, int> real;
	const auto take = [&](CoverImage& img) {
		if (img.kind == CoverImage::Back)
			cards[img.game].push_back(img.none);
		if (img.kind == CoverImage::RealBack)
			real[img.game]++;
	};
	CHECK(Wait(cs, 5.0, take, [&] { return cards.size() == 4; }));
	CHECK(cards[0] == std::vector<bool>({true}) && cards[1] == std::vector<bool>({false}) && cards[2] == std::vector<bool>({true}) &&
		  cards[3] == std::vector<bool>({false}));
	// Asking where the note is out already makes no second card.
	cs.WantBack(0, true);
	cs.WantBack(2, true);
	Wait(cs, 0.6, take, kNever);
	CHECK(cards[0].size() == 1 && cards[2].size() == 1 && net.Total() == 0);
	// Game 3: the download finds it gone, the game is marked, and the card with the note comes.
	cs.SetSelected(3);
	cs.WantBack(3, true);
	CHECK(Wait(cs, 10.0, take, [&] { return cards[3].size() == 2; }));
	CHECK(cards[3] == std::vector<bool>({false, true}) && Exists(cache + "/" + games[3].serial + "_back.missing") && net.Count("lb-h3-back.jpg") == 1 && !real.count(3));
	// Game 1: the picture comes and the card is not replaced.
	cs.SetSelected(1);
	cs.WantBack(1, true);
	CHECK(Wait(cs, 10.0, take, [&] { return real.count(1) > 0; }));
	CHECK(cards[1].size() == 1);
	// The card is painted again for a game the shelf dropped and wanted again, from what is known by then.
	cs.Forget(3);
	cs.Want(3);
	CHECK(Wait(cs, 5.0, take, [&] { return cards[3].size() == 3; }));
	CHECK(cards[3].back() == true);
	CHECK(cs.Stop(5000));
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf("usage: launchbox_test <fixture dir> <work dir>\n");
		return 2;
	}
	g_fix = argv[1];
	g_work = argv[2];
	Units();

	std::vector<uint8_t> text_font, icon_font;
	const std::string res = std::string(getenv("FE_RES") ? getenv("FE_RES") : "") ;
	if (!ReadFileBytes(res + "/fonts/Roboto-Regular.ttf", text_font) || !ReadFileBytes(res + "/fonts/promptfont.otf", icon_font))
	{
		std::printf("FAIL: fonts not found under FE_RES=%s\n", res.c_str());
		return 2;
	}
	Fonts fonts;
	CHECK(fonts.Init(text_font.data(), text_font.size(), icon_font.data(), icon_font.size()));

	const std::string covers_dir = g_work + "/covers", cache_dir = g_work + "/cache";
	system(("mkdir -p '" + covers_dir + "' '" + cache_dir + "'").c_str());

	// Five games: 0 has nothing yet and LaunchBox's own-region scan; 1 has LaunchBox only from another region; 2 is gone from everything;
	// 3 has a cached (other source) cover and a sure LaunchBox match; 4 has a cached cover and a title match that is not sure.
	std::vector<GameInfo> games = {MakeGame("SLUS-00001", "One"), MakeGame("SLUS-00002", "Two"), MakeGame("SLUS-00003", "Three"),
		MakeGame("SLUS-00004", "Four"), MakeGame("SLUS-00005", "Five")};
	auto idx = std::make_shared<LaunchboxIndex>();
	idx->Add("SLUS-00001 f=lb-1-front.jpg* b=lb-1-back.jpg* d=lb-1-disc.png* s=lb-1-spine.jpg* m=1\n"
			 "SLUS-00002 f=lb-2-front.jpg b=lb-2-back.jpg m=1\n"
			 "SLUS-00003 f=lb-3-front.jpg* m=1\n"
			 "SLUS-00004 f=lb-4-front.jpg* b=lb-4-back.jpg m=1\n"
			 "SLUS-00005 f=lb-5-front.jpg* m=2\n");
	const std::string kLb = "https://images.launchbox-app.com/", kOther = "https://example.test/covers/";
	Net net;
	net.files[kLb + "lb-1-front.jpg"] = "front_a.jpg";
	net.files[kLb + "lb-1-back.jpg"] = "back.jpg";
	net.files[kLb + "lb-1-disc.png"] = "disc.png";
	net.files[kLb + "lb-1-spine.jpg"] = "spine.jpg";
	net.files[kOther + "SLUS-00002.jpg"] = "front_b.jpg";
	net.files[kLb + "lb-2-front.jpg"] = "front_a.jpg";
	net.files[kLb + "lb-2-back.jpg"] = "back.jpg";
	net.files[kLb + "lb-4-front.jpg"] = "front_a.jpg";
	net.files[kLb + "lb-4-back.jpg"] = "huge.jpg"; // too big a picture: not used
	net.files[kLb + "lb-5-front.jpg"] = "front_a.jpg";
	// game 2: nothing anywhere (404 from every source)
	// games 3 and 4 have a cached cover from before
	std::vector<uint8_t> old_cover;
	CHECK(ReadFileBytes(g_fix + "/front_b.jpg", old_cover));
	for (const char* serial : {"SLUS-00004", "SLUS-00005"})
	{
		std::ofstream(cache_dir + "/" + serial + ".jpg", std::ios::binary).write(reinterpret_cast<const char*>(old_cover.data()), static_cast<std::streamsize>(old_cover.size()));
	}

	CoverConfig cfg;
	cfg.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
	cfg.manual_dir = covers_dir;
	cfg.cache_dir = cache_dir;
	cfg.url_template = kOther + "${serial}.jpg";
	cfg.allow_download = true;
	cfg.launchbox = idx;
	CoverService cs;
	cs.Start(games, &fonts, cfg, [&net](const std::string& url, std::vector<uint8_t>& out) { return net.Get(url, out); });
	for (int i = 0; i < 5; i++)
		cs.Want(i);

	std::map<int, std::string> cover_source;
	int covers_seen[5] = {};
	bool back_seen = false;
	std::string back_source;
	bool spine_seen = false; // a spine beside the painted one the shelf always gets first
	std::string spine_source;
	int spines = 0;
	auto take = [&](CoverImage& img) {
		if (img.kind == CoverImage::Cover && img.game >= 0 && img.game < 5)
		{
			cover_source[img.game] = img.source;
			covers_seen[img.game]++;
		}
		if (img.kind == CoverImage::Spine && img.game == 0)
		{
		spines++;
		if (std::string(img.source) == "launchbox")
		{
		spine_seen = true;
		spine_source = img.source;
		}
		}
		if (img.kind == CoverImage::RealBack)
		{
			back_seen = true;
			back_source = img.source;
		}
	};
	// 1. Game 0 (nothing cached) takes LaunchBox's own-region scan, game 1 (LaunchBox only from another region) the configured source,
	//    game 2 finds nothing; 3 (sure match) is replaced by LaunchBox's, 4 (title match not sure) is left alone.
	CHECK(Wait(cs, 10.0, take, [&] { return cover_source.count(0) && cover_source.count(1) && covers_seen[3] >= 2 && covers_seen[4] >= 1; }));
	CHECK(cover_source[0] == "launchbox" || cover_source[0] == "cache"); // (the painting thread may find the saved file before the download thread hands the image over)
	CHECK(net.First() == kLb + "lb-1-front.jpg");
	CHECK(Exists(cache_dir + "/SLUS-00001.jpg") && Exists(cache_dir + "/SLUS-00001.lb"));
	CHECK(cover_source[1] == "download" || cover_source[1] == "cache");
	CHECK(Exists(cache_dir + "/SLUS-00002.jpg") && !Exists(cache_dir + "/SLUS-00002.lb"));
	CHECK(net.Count("lb-2-front.jpg") == 0); // the other region's scan was not needed
	CHECK(Wait(cs, 5.0, take, [&] { return Exists(cache_dir + "/SLUS-00003.missing"); }));
	CHECK(net.Count("lb-3-front.jpg") == 1 && net.Count("SLUS-00003.jpg") == 1); // both sources asked, both 404
	CHECK(Wait(cs, 5.0, take, [&] { return Exists(cache_dir + "/SLUS-00004.lb"); }));
	CHECK(Exists(cache_dir + "/SLUS-00004.jpg.old") && net.Count("lb-4-front.jpg") == 1);
	std::vector<uint8_t> now_cover, front_a;
	CHECK(ReadFileBytes(cache_dir + "/SLUS-00004.jpg", now_cover) && ReadFileBytes(g_fix + "/front_a.jpg", front_a) && now_cover == front_a);
	CHECK(cover_source[3] == "launchbox");
	CHECK(net.Count("lb-5-front.jpg") == 0 && !Exists(cache_dir + "/SLUS-00005.lb") && !Exists(cache_dir + "/SLUS-00005.jpg.old")); // not sure: left alone

	// 2. The back covers: the picked game's LaunchBox back; a picture too big (game 3's back is the 5000-pixel one) is refused, nothing then.
	// 1b. The case's side: LaunchBox's spine for game 0, kept in the cache; the others have none (no request).
	CHECK(Wait(cs, 10.0, take, [&] { return spine_seen; }));
	CHECK(spine_source == "launchbox" && Exists(cache_dir + "/SLUS-00001_spine.jpg") && net.Count("lb-1-spine.jpg") == 1);
	CHECK(net.Count("spine") == 1);
	cs.SetSelected(0);
	cs.WantBack(0, true); // (true: the network may be used; WantBack(0) alone looks on disk only)
	CHECK(Wait(cs, 10.0, take, [&] { return back_seen; }));
	CHECK(back_source == "launchbox");
	CHECK(Wait(cs, 5.0, take, [&] { return Exists(cache_dir + "/SLUS-00001_back.jpg"); })); // (2.11: written after the picture is handed over, so the shelf gets it first)
	back_seen = false;
	cs.SetSelected(3);
	cs.WantBack(3, true);
	CHECK(Wait(cs, 10.0, take, [&] { return net.Count("lb-4-back.jpg") >= 1; }));
	CHECK(Wait(cs, 2.0, take, [&] { return false; }) == false && !back_seen && !Exists(cache_dir + "/SLUS-00004_back.jpg"));

	// 3. The disc art for a disc being copied: LaunchBox's PNG, the round label's transparent surround kept; cached for the next copy.
	bool disc_seen = false;
	CoverImage disc;
	auto take_disc = [&](CoverImage& img) {
		if (img.kind == CoverImage::Disc)
		{
			disc_seen = true;
			disc = img;
		}
	};
	cs.RequestDisc("SLUS-00001");
	CHECK(Wait(cs, 10.0, take_disc, [&] { return disc_seen; }));
	CHECK(disc.game == -1 && std::string(disc.source) == "launchbox" && disc.width > 0 && disc.height > 0 && disc.width <= 512 && disc.height <= 512);
	size_t clear = 0, solid = 0;
	for (size_t i = 3; i < disc.rgba.size(); i += 4)
	{
		clear += disc.rgba[i] == 0 ? 1 : 0;
		solid += disc.rgba[i] == 255 ? 1 : 0;
	}
	CHECK(clear > 0 && solid > 0 && solid > clear / 4); // a round label: corners clear, the middle solid
	CHECK(disc.rgba[3] == 0); // the corner pixel
	CHECK(Exists(cache_dir + "/SLUS-00001_disc.png"));
	const size_t before = net.Count("lb-1-disc.png");
	disc_seen = false;
	cs.RequestDisc("SLUS-00001");
	CHECK(Wait(cs, 10.0, take_disc, [&] { return disc_seen; }));
	CHECK(std::string(disc.source) == "cache" && net.Count("lb-1-disc.png") == before); // the second copy needs no download
	// the player's own disc art wins
	system(("cp '" + g_fix + "/disc.png' '" + covers_dir + "/SLUS-00001_disc.png'").c_str());
	disc_seen = false;
	cs.RequestDisc("SLUS-00001");
	CHECK(Wait(cs, 10.0, take_disc, [&] { return disc_seen; }));
	CHECK(std::string(disc.source) == "manual");
	// a game LaunchBox has no disc for: nothing comes, and nothing is asked
	disc_seen = false;
	const size_t total_before = net.Total();
	cs.RequestDisc("SLUS-00003");
	CHECK(!Wait(cs, 1.5, take_disc, [&] { return disc_seen; }) && net.Total() == total_before);

	// 4. Another run with the cache as it is now: nothing is asked again (the .lb mark, the .missing mark, the cached covers).
	CHECK(cs.Stop(5000));
	const size_t asked = net.Total();
	CoverService again;
	again.Start(games, &fonts, cfg, [&net](const std::string& url, std::vector<uint8_t>& out) { return net.Get(url, out); });
	for (int i = 0; i < 5; i++)
		again.Want(i);
	int seen2 = 0;
	Wait(again, 2.0, [&](CoverImage& img) { seen2 += img.kind == CoverImage::Cover ? 1 : 0; }, [&] { return false; });
	CHECK(seen2 >= 4); // the covers on disk are found again
	CHECK(net.Total() == asked);
	CHECK(again.Stop(5000));

	// 5. The network down: one failed request, and then none (downloads are paused for the back-off window: 30 s, doubling; far longer than this test).
	{
		const std::string cache2 = g_work + "/cache2";
		system(("rm -rf '" + cache2 + "' && mkdir -p '" + cache2 + "'").c_str());
		Net down;
		down.down = true;
		CoverConfig c2 = cfg;
		c2.cache_dir = cache2;
		CoverService off;
		off.Start(games, &fonts, c2, [&down](const std::string& url, std::vector<uint8_t>& out) { return down.Get(url, out); });
		for (int i = 0; i < 5; i++)
			off.Want(i);
		Wait(off, 1.5, [](CoverImage&) {}, [] { return false; });
		CHECK(down.Total() == 2); // one game's sources (LaunchBox, then the configured one), then none
		CHECK(!Exists(cache2 + "/SLUS-00001.missing"));
		CHECK(off.Stop(5000));
	}

	// 6. The prefetch before the jailbreak takes the same sources.
	{
		const std::string cache3 = g_work + "/cache3";
		system(("rm -rf '" + cache3 + "' && mkdir -p '" + cache3 + "'").c_str());
		CoverConfig c3 = cfg;
		c3.cache_dir = cache3;
		Net pre;
		pre.files = net.files;
		const int saved = CoverService::Prefetch(games, {0, 1, 2}, c3, [&pre](const std::string& url, std::vector<uint8_t>& out) { return pre.Get(url, out); }, 20.0);
		CHECK(saved == 2);
		CHECK(Exists(cache3 + "/SLUS-00001.jpg") && Exists(cache3 + "/SLUS-00001.lb"));
		CHECK(Exists(cache3 + "/SLUS-00002.jpg") && !Exists(cache3 + "/SLUS-00002.lb"));
		CHECK(Exists(cache3 + "/SLUS-00003.missing"));
		CHECK(pre.First() == kLb + "lb-1-front.jpg");
	}

	// 7. (code review) One source failing is not the network down: LaunchBox's host failing three times running leaves LaunchBox out for the back-off window (2 minutes), the
	// configured source still delivers; an answer that is not "gone" (a 503) is not remembered, a 403 and a picture the shelf can't use are; a "404" mark
	// from 2.10 (no "lb" in it) doesn't keep LaunchBox from being asked.
	{
	const std::string cache4 = g_work + "/cache4";
	system(("rm -rf '" + cache4 + "' && mkdir -p '" + cache4 + "'").c_str());
	std::vector<GameInfo> g4;
	auto idx4 = std::make_shared<LaunchboxIndex>();
	Net n4;
	for (int i = 1; i <= 5; i++)
	{
	const std::string serial = "SLUS-1000" + std::to_string(i);
	g4.push_back(MakeGame(serial, "Game " + std::to_string(i)));
	idx4->Add(serial + " f=lb-" + std::to_string(i) + "-front.jpg* m=1\n");
	n4.status[kLb + "lb-" + std::to_string(i) + "-front.jpg"] = -1; // the host times out
	n4.files[kOther + serial + ".jpg"] = "front_b.jpg";
	}
	CoverConfig c4;
	c4.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
	c4.manual_dir = covers_dir;
	c4.cache_dir = cache4;
	c4.url_template = kOther + "${serial}.jpg";
	c4.allow_download = true;
	c4.launchbox = idx4;
	CoverService s4;
	s4.Start(g4, &fonts, c4, [&n4](const std::string& url, std::vector<uint8_t>& out) { return n4.Get(url, out); });
	for (int i = 0; i < 5; i++)
	s4.Want(i);
	// review 2026-10-16 (lens 3): five DIFFERENT games must have their cover (a cover may arrive twice: from the file and from the download, so counting
	// images let one game's second arrival stand in for the last game's, and the files were checked while that game's download was still on its way;
	// found when the test was built with the address and undefined behaviour sanitizers, whose slowness changes the order)
	std::set<int> got4;
	Wait(s4, 10.0, [&](CoverImage& img) { if (img.kind == CoverImage::Cover) got4.insert(img.game); }, [&] { return got4.size() >= 5; });
	CHECK(got4.size() >= 5);                 // every cover came from the configured source
	CHECK(n4.Count("launchbox") == 3);        // LaunchBox was tried three times, then left out
	CHECK(n4.Count("example.test") == 5);
	for (int i = 1; i <= 5; i++)
	CHECK(!Exists(cache4 + "/SLUS-1000" + std::to_string(i) + ".missing") && Exists(cache4 + "/SLUS-1000" + std::to_string(i) + ".jpg"));
	CHECK(s4.Stop(5000));
	}
	{
	const std::string cache5 = g_work + "/cache5";
	system(("rm -rf '" + cache5 + "' && mkdir -p '" + cache5 + "'").c_str());
	std::vector<GameInfo> g5 = {MakeGame("SLUS-20001", "A"), MakeGame("SLUS-20002", "B"), MakeGame("SLUS-20003", "C"), MakeGame("SLUS-20004", "D")};
	auto idx5 = std::make_shared<LaunchboxIndex>();
	idx5->Add("SLUS-20001 f=lb-a.jpg* m=1\nSLUS-20002 f=lb-b.jpg* m=1\nSLUS-20003 f=lb-c.jpg* m=1\nSLUS-20004 f=lb-d.jpg* m=1\n");
	Net n5;
	n5.status[kLb + "lb-a.jpg"] = 503;                  // a server error: say nothing about the file
	n5.status[kLb + "lb-b.jpg"] = 403;                  // refused: gone
	n5.files[kLb + "lb-c.jpg"] = "huge.jpg";            // a picture the shelf can't use
	n5.files[kLb + "lb-d.jpg"] = "front_a.jpg";
	CoverConfig c5;
	c5.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
	c5.manual_dir = covers_dir;
	c5.cache_dir = cache5;
	c5.allow_download = true;
	c5.launchbox = idx5;
	// a 404 mark from 2.10 for D (no "lb" in it): LaunchBox is still asked; one from now for C is respected
	WriteText(cache5 + "/SLUS-20004.missing", "404\n");
	WriteText(cache5 + "/SLUS-20003.missing", "404 lb\n");
	const std::vector<int> need = CoverService::MissingCovers(g5, c5);
	CHECK(need == std::vector<int>({0, 1, 3})); // C is remembered, D's old mark doesn't count
	const int saved = CoverService::Prefetch(g5, need, c5, [&n5](const std::string& url, std::vector<uint8_t>& out) { return n5.Get(url, out); }, 20.0);
	CHECK(saved == 1 && Exists(cache5 + "/SLUS-20004.jpg") && Exists(cache5 + "/SLUS-20004.lb"));
	CHECK(!Exists(cache5 + "/SLUS-20001.missing"));  // the 503 is not remembered
	CHECK(Exists(cache5 + "/SLUS-20002.missing"));   // the 403 is
	}
	// (cover upgrade) a replaced cover keeps the old one, and a failed write leaves the cover as it was
	{
	const std::string cache6 = g_work + "/cache6";
	system(("rm -rf '" + cache6 + "' && mkdir -p '" + cache6 + "'").c_str());
	std::vector<uint8_t> old_bytes;
	CHECK(ReadFileBytes(g_fix + "/front_b.jpg", old_bytes));
	std::ofstream(cache6 + "/SLUS-30001.jpg", std::ios::binary).write(reinterpret_cast<const char*>(old_bytes.data()), static_cast<std::streamsize>(old_bytes.size()));
	std::vector<GameInfo> g6 = {MakeGame("SLUS-30001", "Up")};
	auto idx6 = std::make_shared<LaunchboxIndex>();
	idx6->Add("SLUS-30001 f=lb-up.jpg* m=1\n");
	Net n6;
	n6.files[kLb + "lb-up.jpg"] = "front_a.jpg";
	CoverConfig c6;
	c6.launchbox_front_first = true; // 186: 2.11's LaunchBox-first order (flag lbfront)
	c6.manual_dir = covers_dir;
	c6.cache_dir = cache6;
	c6.allow_download = true;
	c6.launchbox = idx6;
	CoverService s6;
	s6.Start(g6, &fonts, c6, [&n6](const std::string& url, std::vector<uint8_t>& out) { return n6.Get(url, out); });
	s6.Want(0);
	CHECK(Wait(s6, 10.0, [](CoverImage&) {}, [&] { return Exists(cache6 + "/SLUS-30001.lb"); }));
	std::vector<uint8_t> now_bytes, old_kept, new_ref;
	CHECK(ReadFileBytes(cache6 + "/SLUS-30001.jpg", now_bytes) && ReadFileBytes(g_fix + "/front_a.jpg", new_ref) && now_bytes == new_ref);
	CHECK(ReadFileBytes(cache6 + "/SLUS-30001.jpg.old", old_kept) && old_kept == old_bytes);
	CHECK(!Exists(cache6 + "/SLUS-30001.jpg.lbnew"));
	CHECK(s6.Stop(5000));
	}
	// 7. (186; the owner: "switch back to the old covers (only for the front cover)") launchbox_front_first false, the default: the configured source's
	//    front first, LaunchBox's only where it has none; a cached LaunchBox front goes back once: to the file it replaced, else to the configured source's,
	//    LaunchBox's kept as .jpg.lbfront; a source with none leaves LaunchBox's and is not asked again.
	{
	const std::string cache7 = g_work + "/cache7";
	system(("rm -rf '" + cache7 + "' && mkdir -p '" + cache7 + "'").c_str());
	std::vector<uint8_t> lb_bytes, old_bytes;
	CHECK(ReadFileBytes(g_fix + "/front_a.jpg", lb_bytes) && ReadFileBytes(g_fix + "/front_b.jpg", old_bytes));
	auto put = [&](const std::string& name, const std::vector<uint8_t>& b) {
		std::ofstream(cache7 + "/" + name, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
	};
	put("SLUS-40001.jpg", lb_bytes); // upgraded by 2.11: LaunchBox's, the old one kept
	put("SLUS-40001.jpg.old", old_bytes);
	WriteText(cache7 + "/SLUS-40001.lb", "1\n");
	put("SLUS-40002.jpg", lb_bytes); // LaunchBox's from the start
	WriteText(cache7 + "/SLUS-40002.lb", "1\n");
	put("SLUS-40003.jpg", lb_bytes); // LaunchBox's, and the configured source has none
	WriteText(cache7 + "/SLUS-40003.lb", "1\n");
	std::vector<GameInfo> g7 = {MakeGame("SLUS-40001", "Old"), MakeGame("SLUS-40002", "Back"), MakeGame("SLUS-40003", "Stays"), MakeGame("SLUS-40004", "New")};
	auto idx7 = std::make_shared<LaunchboxIndex>();
	idx7->Add("SLUS-40001 f=lb-o1.jpg* m=1\nSLUS-40002 f=lb-o2.jpg* m=1\nSLUS-40003 f=lb-o3.jpg* m=1\nSLUS-40004 f=lb-o4.jpg* m=1\n");
	Net n7;
	n7.files[kOther + "SLUS-40002.jpg"] = "front_b.jpg";
	n7.files[kOther + "SLUS-40004.jpg"] = "front_b.jpg";
	n7.files[kLb + "lb-o4.jpg"] = "front_a.jpg";
	CoverConfig c7; // launchbox_front_first stays false: the default
	c7.manual_dir = covers_dir;
	c7.cache_dir = cache7;
	c7.url_template = kOther + "${serial}.jpg";
	c7.allow_download = true;
	c7.launchbox = idx7;
	CHECK(!c7.launchbox_front_first);
	CoverService s7;
	s7.Start(g7, &fonts, c7, [&n7](const std::string& url, std::vector<uint8_t>& out) { return n7.Get(url, out); });
	for (int i = 0; i < 4; i++)
		s7.Want(i);
	CHECK(Wait(s7, 10.0, [](CoverImage&) {}, [&] {
		return !Exists(cache7 + "/SLUS-40001.lb") && !Exists(cache7 + "/SLUS-40002.lb") && Exists(cache7 + "/SLUS-40003_x.missing") && Exists(cache7 + "/SLUS-40004.jpg");
	}));
	std::vector<uint8_t> b1, b1k, b2, b2k, b3, b4;
	CHECK(ReadFileBytes(cache7 + "/SLUS-40001.jpg", b1) && b1 == old_bytes); // the one it replaced is back
	CHECK(ReadFileBytes(cache7 + "/SLUS-40001.jpg.lbfront", b1k) && b1k == lb_bytes && !Exists(cache7 + "/SLUS-40001.jpg.old"));
	CHECK(n7.Count("SLUS-40001.jpg") == 0); // no download for it
	CHECK(ReadFileBytes(cache7 + "/SLUS-40002.jpg", b2) && b2 == old_bytes); // the configured source's
	CHECK(ReadFileBytes(cache7 + "/SLUS-40002.jpg.lbfront", b2k) && b2k == lb_bytes && !Exists(cache7 + "/SLUS-40002.jpg.xnew"));
	CHECK(ReadFileBytes(cache7 + "/SLUS-40003.jpg", b3) && b3 == lb_bytes && Exists(cache7 + "/SLUS-40003.lb")); // none there: LaunchBox's stays
	CHECK(ReadFileBytes(cache7 + "/SLUS-40004.jpg", b4) && b4 == old_bytes && !Exists(cache7 + "/SLUS-40004.lb")); // a new game: the configured source's
	CHECK(n7.Count("lb-o4.jpg") == 0 && n7.Count("lb-o1.jpg") == 0 && n7.Count("lb-o2.jpg") == 0); // LaunchBox's fronts not asked for
	const int asked3 = n7.Count("SLUS-40003.jpg");
	CHECK(s7.Stop(5000));
	CoverService s7b; // the next start: the source that had none is not asked again
	s7b.Start(g7, &fonts, c7, [&n7](const std::string& url, std::vector<uint8_t>& out) { return n7.Get(url, out); });
	s7b.Want(2);
	Wait(s7b, 1.5, [](CoverImage&) {}, [] { return false; });
	CHECK(n7.Count("SLUS-40003.jpg") == asked3 && asked3 == 1);
	CHECK(s7b.Stop(5000));
	}
	// 8. (2.11) The back covers: who asks, who answers, the neighbours ahead of the flip, the order of the queue, the pauses, the card and its note.
	BackInline(fonts);
	BackGate(fonts);
	BackRing(fonts);
	BackNegatives(fonts);
	BackPauses(fonts);
	PauseNoMark(fonts);
	BackNote(fonts);
	std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
	return g_failures ? 1 : 0;
}
