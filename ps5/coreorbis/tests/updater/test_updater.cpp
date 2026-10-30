// PS5SX2 (vk-285-180, AI-assisted): PC test of the in-app updater's decisions (frontend/fe_update.h: GitHub's release answer, the
// build tags, the row's states with a fake platform; installer-dl: the installer fetched when the app has none) and of the installer's launch block as the app fills it
// (orbis-shims/OrbisInstallerLaunch.h), checked against the installer's own struct (ps5/installer/src/launch.h).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisInstallerLaunch.h"
#include "fe_update.h"

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#define _Static_assert static_assert
extern "C" {
#include "launch.h"
}
#undef _Static_assert

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, what)                                                       \
	do                                                                          \
	{                                                                           \
		if (cond)                                                               \
			g_pass++;                                                           \
		else                                                                    \
		{                                                                       \
			g_fail++;                                                           \
			std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, std::string(what).c_str()); \
		}                                                                       \
	} while (0)

static const std::string kPrefix = "https://github.com/Swordpdf/PS5SX2/releases/download/";
static const std::string kSha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static std::string Asset(const std::string& name, long long size, const std::string& digest, const std::string& url,
	const std::string& state = "uploaded")
{
	return "{\"url\":\"https://api.github.com/repos/Swordpdf/PS5SX2/releases/assets/1\",\"id\":1,\"name\":\"" + name +
		"\",\"label\":null,\"uploader\":{\"login\":\"swordpdf\",\"id\":5,\"site_admin\":false},\"content_type\":\"application/zip\",\"state\":\"" +
		state + "\",\"size\":" + std::to_string(size) + ",\"digest\":" + (digest.empty() ? std::string("null") : "\"" + digest + "\"") +
		",\"download_count\":12,\"browser_download_url\":\"" + url + "\"}";
}

static std::string Release(const std::string& tag, const std::string& assets, bool draft = false, const std::string& name = "")
{
	return "{\"url\":\"https://api.github.com/repos/Swordpdf/PS5SX2/releases/1\",\"tag_name\":\"" + tag + "\",\"name\":" +
		(name.empty() ? std::string("null") : "\"" + name + "\"") + ",\"draft\":" + (draft ? "true" : "false") +
		",\"prerelease\":false,\"published_at\":\"2026-10-14T20:01:00Z\",\"author\":{\"login\":\"swordpdf\"},"
		"\"body\":\"Notes with \\u00b7 unicode, \\\"quotes\\\", a tab\\t, \\ud83d\\ude00 and \\\\ escapes\",\"reactions\":{\"+1\":3,\"list\":[1,2.5,-3e2,null,true]},"
		"\"assets\":[" + assets + "]}";
}

static std::string Good(const std::string& tag, const std::string& file = "")
{
	const std::string name = file.empty() ? "PS5SX2-" + tag + ".zip" : file;
	return Release(tag, Asset(name, 41000000, "sha256:" + kSha, kPrefix + tag + "/" + name), false, "PS5SX2 2.20");
}

static void TestParse()
{
	fe::ReleaseInfo r;
	std::string e;
	CHECK(fe::ParseLatestRelease(Good("vk-285-181"), kPrefix, r, e), "a good release: " + e);
	CHECK(r.tag == "vk-285-181" && r.title == "PS5SX2 2.20" && r.asset_name == "PS5SX2-vk-285-181.zip" && r.size == 41000000 && r.sha256 == kSha,
		"its fields");
	CHECK(r.asset_url == kPrefix + "vk-285-181/PS5SX2-vk-285-181.zip" && r.published_at == "2026-10-14T20:01:00Z", "its address");

	// an upper-case digest is taken in lower case; a release without a name is called by its tag
	std::string up = kSha;
	for (char& c : up)
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	CHECK(fe::ParseLatestRelease(Release("vk-285-181", Asset("PS5SX2-x.zip", 5000, "sha256:" + up, kPrefix + "vk-285-181/PS5SX2-x.zip")), kPrefix, r, e) &&
			r.sha256 == kSha && r.title == "vk-285-181",
		"upper-case digest, no name: " + e);

	// another zip first (the PS5SX3 one, a no-log ELF zip), then ours: ours is picked
	const std::string two = Release("vk-285-181", Asset("Sources.zip", 5000, "sha256:" + kSha, kPrefix + "vk-285-181/Sources.zip") + "," +
		Asset("PS5SX2Installer.elf", 5000, "sha256:" + kSha, kPrefix + "vk-285-181/PS5SX2Installer.elf") + "," +
		Asset("PS5SX2-vk-285-181.zip", 9000, "sha256:" + kSha, kPrefix + "vk-285-181/PS5SX2-vk-285-181.zip"));
	CHECK(fe::ParseLatestRelease(two, kPrefix, r, e) && r.asset_name == "PS5SX2-vk-285-181.zip" && r.size == 9000, "ours among others: " + e);

	struct Bad
	{
		std::string json, why;
	};
	const std::string t = "vk-285-181";
	const Bad bad[] = {
		{Release(t, Asset("PS5SX2-a.zip", 9000, "sha256:" + kSha, kPrefix + t + "/PS5SX2-a.zip"), true), "draft"},
		{Release(t, ""), "no zip file"},
		{Release(t, Asset("Other.zip", 9000, "sha256:" + kSha, kPrefix + t + "/Other.zip")), "no PS5SX2 zip"},
		{Release(t, Asset("PS5SX2-a.zip", 9000, "sha256:" + kSha, kPrefix + t + "/PS5SX2-a.zip", "starter")), "zip not uploaded"},
		{Release(t, Asset("PS5SX2-a.zip", 9000, "", kPrefix + t + "/PS5SX2-a.zip")), "no SHA-256"},
		{Release(t, Asset("PS5SX2-a.zip", 9000, "sha256:" + kSha.substr(1), kPrefix + t + "/PS5SX2-a.zip")), "SHA-256 too short"},
		{Release(t, Asset("PS5SX2-a.zip", 9000, "md5:" + kSha, kPrefix + t + "/PS5SX2-a.zip")), "not SHA-256"},
		{Release(t, Asset("PS5SX2-a.zip", 9000, "sha256:" + kSha.substr(0, 63) + "g", kPrefix + t + "/PS5SX2-a.zip")), "not hex"},
		{Release(t, Asset("PS5SX2-a.zip", 100, "sha256:" + kSha, kPrefix + t + "/PS5SX2-a.zip")), "too small"},
		{Release(t, Asset("PS5SX2-a.zip", 600ll << 20, "sha256:" + kSha, kPrefix + t + "/PS5SX2-a.zip")), "too big"},
		{Release(t, Asset("PS5SX2-a.zip", 9000, "sha256:" + kSha, "https://evil.example/PS5SX2-a.zip")), "foreign address"},
		{Release(t, Asset("PS5SX2-a.zip", 9000, "sha256:" + kSha, kPrefix + "vk-285-180/PS5SX2-a.zip")), "another tag's address"},
		{Release(t, Asset("PS5SX2-a b.zip", 9000, "sha256:" + kSha, kPrefix + t + "/PS5SX2-a b.zip")), "a space in the name"},
		{Release("../x", Asset("PS5SX2-a.zip", 9000, "sha256:" + kSha, kPrefix + "../x/PS5SX2-a.zip")), "a bad tag"},
		{Release("", Asset("PS5SX2-a.zip", 9000, "sha256:" + kSha, kPrefix + "/PS5SX2-a.zip")), "an empty tag"},
		{Good(t) + " x", "data after the object"},
		{Good(t).substr(0, 200), "cut off"},
		{"[]", "not an object"},
		{"<html>rate limited</html>", "an HTML page"},
		{"", "empty"},
		{std::string(40, '[') + std::string(40, ']'), "nested too deep"},
		{"{\"tag_name\":\"vk-285-1\\x\"}", "a bad escape"},
	};
	for (const Bad& b : bad)
	{
		fe::ReleaseInfo r2;
		std::string e2;
		const bool ok = fe::ParseLatestRelease(b.json, kPrefix, r2, e2);
		CHECK(!ok && !e2.empty(), "refused: " + b.why + " (" + e2 + ")");
	}
	// a size with a fraction isn't an integer
	CHECK(!fe::ParseLatestRelease(Release(t, "{\"name\":\"PS5SX2-a.zip\",\"state\":\"uploaded\",\"size\":9000.5,\"digest\":\"sha256:" + kSha +
		"\",\"browser_download_url\":\"" + kPrefix + t + "/PS5SX2-a.zip\"}"), kPrefix, r, e), "a fractional size");
}

static void TestTags()
{
	CHECK(fe::CompareBuildTags("vk-285-180", "vk-285-180") == 0, "same");
	CHECK(fe::CompareBuildTags("vk-285-180", "vk-285-181") < 0, "older");
	CHECK(fe::CompareBuildTags("vk-285-181", "vk-285-171") > 0, "newer");
	CHECK(fe::CompareBuildTags("vk-285-99", "vk-285-180") < 0, "numbers, not text");
	CHECK(fe::CompareBuildTags("vk-286-1", "vk-285-999") > 0, "the first number first");
	CHECK(fe::CompareBuildTags("vk-285-180", "eerec-285-180") == 2, "another prefix");
	CHECK(fe::CompareBuildTags("vk-285-180", "vk-180") == 2, "another count of numbers");
	CHECK(fe::CompareBuildTags("vk-285-180-x", "vk-285-180") == 2, "words after numbers");
	CHECK(fe::CompareBuildTags("eerec-285-live", "vk-285-180") == 2, "a dev build's tag");
	CHECK(fe::CompareBuildTags("", "vk-285-180") == 2, "empty");
}

static void TestJudge()
{
	using S = fe::UpdateStatus::State;
	CHECK(fe::JudgeRelease("vk-285-180", -1, "", "no connection", kPrefix).state == S::Failed, "no answer");
	CHECK(fe::JudgeRelease("vk-285-180", -1, "", "no connection", kPrefix).error.find("no connection") != std::string::npos, "says why");
	CHECK(fe::JudgeRelease("vk-285-180", 404, "", "", kPrefix).error == "no release is published yet", "404");
	CHECK(fe::JudgeRelease("vk-285-180", 403, "{}", "", kPrefix).error.find("hourly limit") != std::string::npos, "403 rate limit");
	CHECK(fe::JudgeRelease("vk-285-180", 500, "", "", kPrefix).error == "GitHub answered HTTP 500", "500");
	CHECK(fe::JudgeRelease("vk-285-180", 200, "<html>", "", kPrefix).state == S::Failed, "unreadable");
	CHECK(fe::JudgeRelease("vk-285-180", 200, Good("vk-285-180"), "", kPrefix).state == S::Current, "current");
	CHECK(fe::JudgeRelease("vk-285-181", 200, Good("vk-285-180"), "", kPrefix).state == S::Newer, "newer");
	const fe::UpdateStatus a = fe::JudgeRelease("vk-285-177", 200, Good("vk-285-181"), "", kPrefix);
	CHECK(a.state == S::Available && a.latest.tag == "vk-285-181", "available");
	CHECK(fe::JudgeRelease("eerec-285-live", 200, Good("vk-285-181"), "", kPrefix).state == S::Available, "a tag it can't compare: offered");
}

// ---- the updater with a fake platform ----

struct Fake
{
	std::mutex m;
	std::set<std::string> files;
	int http = 200;
	std::string body;
	int start_rc = 0;
	std::vector<std::string> started;   // expect tags
	std::vector<std::string> notified;
	std::vector<std::string> logs;
	int gets = 0;
	int quits = 0;
};

static fe::UpdatePlatform MakePlatform(Fake& f)
{
	fe::UpdatePlatform p;
	p.get_text = [&f](const std::string& url, std::string& body, std::string& error) {
		std::lock_guard<std::mutex> l(f.m);
		f.gets++;
		if (url != fe::kUpdateApiUrl)
		{
			error = "wrong url " + url;
			return -1;
		}
		body = f.body;
		if (f.http < 0)
			error = "no connection";
		return f.http;
	};
	p.start_installer = [&f](const std::string& expect, std::string& detail) {
		std::lock_guard<std::mutex> l(f.m);
		f.started.push_back(expect);
		detail = "fake";
		return f.start_rc;
	};
	p.exists = [&f](const std::string& path) {
		std::lock_guard<std::mutex> l(f.m);
		return f.files.count(path) > 0;
	};
	p.set_file = [&f](const std::string& path, bool on) {
		std::lock_guard<std::mutex> l(f.m);
		if (on)
			f.files.insert(path);
		else
			f.files.erase(path);
		return true;
	};
	p.notify = [&f](const std::string& t) {
		std::lock_guard<std::mutex> l(f.m);
		f.notified.push_back(t);
	};
	p.log = [&f](const std::string& t) {
		std::lock_guard<std::mutex> l(f.m);
		f.logs.push_back(t);
	};
	p.quit = [&f] { f.quits++; };
	return p;
}

static fe::UpdateStatus WaitDone(fe::Updater& u)
{
	for (int i = 0; i < 500; i++)
	{
		const fe::UpdateStatus s = u.Status();
		if (s.state != fe::UpdateStatus::State::Checking)
			return s;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return u.Status();
}

static void TestUpdater()
{
	using S = fe::UpdateStatus::State;
	const std::string work = "/data/PS5SX2-Installer";
	{
		Fake f;
		f.body = Good("vk-285-181");
		fe::Updater u(MakePlatform(f), "vk-285-180", work);
		fe::UpdateService svc = u.Service();
		CHECK(svc && svc.this_tag == "vk-285-180", "the service");
		CHECK(u.Status().state == S::Idle, "idle at first");
		CHECK(!u.Install(), "nothing to install before a look");
		CHECK(f.started.empty(), "no installer yet");
		CHECK(svc.check(true), "the automatic look starts");
		fe::UpdateStatus s = WaitDone(u);
		CHECK(s.state == S::Available && s.automatic && s.latest.tag == "vk-285-181", "available");
		CHECK(f.notified.size() == 1 && f.notified[0] == "PS5SX2 2.20 (vk-285-181)", "the automatic look says so once");
		CHECK(u.Check(false), "a look by hand");
		s = WaitDone(u);
		CHECK(s.state == S::Available && !s.automatic && f.notified.size() == 1, "a look by hand: no popup");
		CHECK(f.gets == 2, "one request a look");
		CHECK(svc.install(), "install");
		CHECK(f.started.size() == 1 && f.started[0] == "vk-285-181", "the installer gets the release the shelf showed");
		CHECK(u.Status().state == S::Started, "started");
		CHECK(!u.Install() && f.started.size() == 1, "not twice");
		CHECK(!u.Check(false), "no look once the installer runs");
		svc.quit();
		CHECK(f.quits == 1, "quit is the platform's");
		CHECK(u.Stop(1000), "stops");
	}
	{
		Fake f;
		f.body = Good("vk-285-180");
		fe::Updater u(MakePlatform(f), "vk-285-180", work);
		u.Check(false);
		CHECK(WaitDone(u).state == S::Current, "current");
		CHECK(!u.Install() && f.started.empty(), "nothing to install when current");
		f.body = Good("vk-285-171");
		u.Check(true);
		CHECK(WaitDone(u).state == S::Newer && f.notified.empty(), "newer: no popup");
		f.http = -1;
		u.Check(false);
		const fe::UpdateStatus s = WaitDone(u);
		CHECK(s.state == S::Failed && s.error.find("no connection") != std::string::npos, "no network: " + s.error);
		CHECK(!u.Install(), "nothing to install after a failure");
		CHECK(u.Stop(1000), "stops");
	}
	{
		// the installer can't start: says why, and Cross may try again
		Fake f;
		f.body = Good("vk-285-181");
		f.start_rc = 3;
		fe::Updater u(MakePlatform(f), "vk-285-180", work);
		u.Check(false);
		WaitDone(u);
		CHECK(!u.Install(), "no loader: false");
		fe::UpdateStatus s = u.Status();
		CHECK(s.state == S::StartFailed && s.error == "no ELF loader answered on port 9021", "no loader: " + s.error);
		f.start_rc = 1;
		CHECK(!u.Install() && u.Status().error == "this PS5SX2 has no PS5SX2Installer.elf", "no installer");
		f.start_rc = 2;
		CHECK(!u.Install() && u.Status().error.find("not one this PS5SX2 can start") != std::string::npos, "bad installer");
		f.start_rc = 0;
		CHECK(u.Install() && u.Status().state == S::Started && f.started.size() == 4, "then it starts");
		u.Stop(1000);
	}
	{
		// a cut-off install: the row says so at once; the automatic look leaves it; Cross runs the installer for the latest
		Fake f;
		f.files.insert(work + "/journal.txt");
		f.body = Good("vk-285-181");
		fe::Updater u(MakePlatform(f), "vk-285-180", work);
		CHECK(u.Status().state == S::Interrupted, "interrupted");
		CHECK(!u.Check(true) && f.gets == 0, "the automatic look leaves it");
		CHECK(u.Install() && f.started.size() == 1 && f.started[0].empty(), "Cross: the installer, for the latest");
		u.Stop(1000);
	}
	{
		// a look in flight: no second one
		Fake f;
		f.body = Good("vk-285-181");
		std::mutex gate;
		gate.lock();
		fe::UpdatePlatform p = MakePlatform(f);
		auto inner = p.get_text;
		p.get_text = [&](const std::string& url, std::string& body, std::string& error) {
			std::lock_guard<std::mutex> l(gate);
			return inner(url, body, error);
		};
		fe::Updater u(p, "vk-285-180", work);
		CHECK(u.Check(false), "first look");
		CHECK(!u.Check(false), "no second look while one runs");
		CHECK(!u.Install(), "no install while looking");
		CHECK(!u.Stop(50), "Stop says it is still running");
		gate.unlock();
		CHECK(u.Stop(2000), "then it ends");
		CHECK(u.Status().state == S::Available, "and the result is there");
	}
	{
		// the session-log switch writes the installer's no-log-upload
		Fake f;
		fe::Updater u(MakePlatform(f), "vk-285-180", work);
		CHECK(u.LogUpload(), "on by default");
		CHECK(u.SetLogUpload(false) && f.files.count(work + "/no-log-upload") == 1 && !u.LogUpload(), "off: the switch file");
		CHECK(u.SetLogUpload(true) && f.files.count(work + "/no-log-upload") == 0 && u.LogUpload(), "on: removed");
	}
}


// ---- installer-dl: System update fetches PS5SX2Installer.elf when the app has none ----

static std::vector<uint8_t> FakeElf(size_t n)
{
	std::vector<uint8_t> b(n);
	for (size_t i = 0; i < n; i++)
		b[i] = static_cast<uint8_t>(i * 13 + 7);
	b[0] = 0x7f;
	b[1] = 'E';
	b[2] = 'L';
	b[3] = 'F';
	return b;
}

static std::string ReleaseWithInstaller(const std::string& tag, size_t elf_size, const std::string& elf_digest, const std::string& elf_url = "")
{
	const std::string zip = "PS5SX2-" + tag + ".zip";
	return Release(tag, Asset(zip, 41000000, "sha256:" + kSha, kPrefix + tag + "/" + zip) + "," +
		Asset("PS5SX2Installer.elf", static_cast<long long>(elf_size), elf_digest, elf_url.empty() ? kPrefix + tag + "/PS5SX2Installer.elf" : elf_url),
		false, "PS5SX2 2.21");
}

static void TestInstallerDownload()
{
	using S = fe::UpdateStatus::State;
	// SHA-256 (FIPS 180-4 examples; one block, two blocks, a million bytes)
	CHECK(fe::Sha256Hex(nullptr, 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 of nothing");
	CHECK(fe::Sha256Hex(reinterpret_cast<const uint8_t*>("abc"), 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
	const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	CHECK(fe::Sha256Hex(reinterpret_cast<const uint8_t*>(two.data()), two.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
		"sha256 of 56 bytes (two blocks)");
	const std::string million(1000000, 'a');
	CHECK(fe::Sha256Hex(reinterpret_cast<const uint8_t*>(million.data()), million.size()) ==
			  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
		"sha256 of a million a's");

	const std::vector<uint8_t> elf = FakeElf(300000);
	const std::string sha = fe::Sha256Hex(elf.data(), elf.size());
	const std::string tag = "vk-285-183";
	const std::string url = kPrefix + tag + "/PS5SX2Installer.elf";
	{
		fe::ReleaseInfo r;
		std::string e;
		CHECK(fe::ParseLatestRelease(ReleaseWithInstaller(tag, elf.size(), "sha256:" + sha), kPrefix, r, e) && r.installer_url == url &&
				  r.installer_size == elf.size() && r.installer_sha256 == sha && r.asset_name == "PS5SX2-" + tag + ".zip",
			"the release's installer asset: " + e);
		CHECK(fe::ParseLatestRelease(ReleaseWithInstaller(tag, elf.size(), ""), kPrefix, r, e) && r.installer_url == url && r.installer_sha256.empty(),
			"an installer without a digest: its size only");
		CHECK(fe::ParseLatestRelease(ReleaseWithInstaller(tag, elf.size(), "md5:" + sha), kPrefix, r, e) && r.installer_url.empty(),
			"an installer with a digest that isn't SHA-256: not used (the release still is)");
		CHECK(fe::ParseLatestRelease(ReleaseWithInstaller(tag, elf.size(), "sha256:" + sha, "https://evil.example/PS5SX2Installer.elf"), kPrefix, r, e) &&
				  r.installer_url.empty(),
			"an installer at another address: not used");
		CHECK(fe::ParseLatestRelease(ReleaseWithInstaller(tag, 100, "sha256:" + sha), kPrefix, r, e) && r.installer_url.empty(), "a 100-byte installer: not used");
		CHECK(fe::ParseLatestRelease(Good(tag), kPrefix, r, e) && r.installer_url.empty(), "a release without one");
	}

	struct Disk
	{
		std::mutex m;
		std::map<std::string, std::vector<uint8_t>> files;
		std::vector<std::string> renames;
		int downloads = 0;
		int http = 200;
		std::vector<uint8_t> served;
		std::mutex* gate = nullptr;
	};
	const std::string dest = "/data/homebrew/PPSA99203/PS5SX2Installer.elf";
	auto platform = [&](Fake& f, Disk& d, bool present) {
		fe::UpdatePlatform p = MakePlatform(f);
		p.installer_present = [present, &d, dest] {
			std::lock_guard<std::mutex> l(d.m);
			return present || d.files.count(dest) > 0;
		};
		p.get_file = [&d](const std::string& u, uint64_t max_bytes, std::vector<uint8_t>& out, std::string& error) {
			if (d.gate)
				std::lock_guard<std::mutex> g(*d.gate);
			std::lock_guard<std::mutex> l(d.m);
			d.downloads++;
			if (u.compare(0, 8, "https://") != 0 || max_bytes < d.served.size())
			{
				error = "bad request";
				return -1;
			}
			out = d.served;
			return d.http;
		};
		p.write_file = [&d](const std::string& path, const std::vector<uint8_t>& data, std::string&) {
			std::lock_guard<std::mutex> l(d.m);
			d.files[path] = data;
			return true;
		};
		p.rename_file = [&d](const std::string& from, const std::string& to) {
			std::lock_guard<std::mutex> l(d.m);
			auto it = d.files.find(from);
			if (it == d.files.end())
				return false;
			d.files[to] = it->second;
			d.files.erase(it);
			d.renames.push_back(from + " -> " + to);
			return true;
		};
		return p;
	};
	auto settle = [](fe::Updater& u) {
		for (int i = 0; i < 500; i++)
		{
			const fe::UpdateStatus s = u.Status();
			if (s.state != S::Checking && s.state != S::DownloadingInstaller && s.state != S::Starting)
				return s;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return u.Status();
	};
	auto logged = [](Fake& f, const std::string& what) {
		std::lock_guard<std::mutex> l(f.m);
		for (const std::string& line : f.logs)
			if (line.find(what) != std::string::npos)
				return true;
		return false;
	};
	{
		// no installer beside the eboot: fetched (size and SHA-256 checked), saved through a .part file, then started as usual
		Fake f;
		Disk d;
		d.served = elf;
		std::mutex gate;
		d.gate = &gate;
		f.body = ReleaseWithInstaller(tag, elf.size(), "sha256:" + sha);
		fe::Updater u(platform(f, d, false), "vk-285-182", "/data/PS5SX2-Installer");
		u.Check(false);
		CHECK(WaitDone(u).state == S::Available, "available");
		gate.lock();
		CHECK(u.Install(), "Cross twice with no installer: true (the download runs)");
		CHECK(u.Status().state == S::DownloadingInstaller, "the row: downloading the installer");
		CHECK(!u.Install() && !u.Check(false), "nothing else while it downloads");
		gate.unlock();
		const fe::UpdateStatus s = settle(u);
		CHECK(s.state == S::Started && f.started.size() == 1 && f.started[0] == tag, "then the installer starts for the release shown: " + s.error);
		CHECK(d.downloads == 1 && d.files.count(dest) == 1 && d.files[dest] == elf && d.files.count(dest + ".part") == 0, "saved in the app's folder");
		CHECK(d.renames.size() == 1 && d.renames[0] == dest + ".part -> " + dest, "through a .part file and a rename");
		CHECK(logged(f, "no PS5SX2Installer.elf in /app0") && logged(f, "installer: downloading " + url) && logged(f, "SHA-256 matches") &&
				  logged(f, "saved as " + dest),
			"each step in the log");
		u.Stop(1000);
	}
	{
		// with an installer: no download, as before
		Fake f;
		Disk d;
		d.served = elf;
		f.body = ReleaseWithInstaller(tag, elf.size(), "sha256:" + sha);
		fe::Updater u(platform(f, d, true), "vk-285-182", "/data/PS5SX2-Installer");
		u.Check(false);
		WaitDone(u);
		CHECK(u.Install() && u.Status().state == S::Started && d.downloads == 0 && f.started.size() == 1, "an installer there: started, nothing downloaded");
		u.Stop(1000);
	}
	struct Bad
	{
		std::string why, body;
		std::vector<uint8_t> served;
		int http;
		std::string error;
	};
	std::vector<uint8_t> flipped = elf;
	flipped[5000] ^= 1;
	std::vector<uint8_t> not_elf = elf;
	not_elf[0] = 'M';
	const Bad bad[] = {
		{"a SHA-256 that doesn't match", ReleaseWithInstaller(tag, elf.size(), "sha256:" + sha), flipped, 200, "doesn't match GitHub's SHA-256"},
		{"a short download", ReleaseWithInstaller(tag, elf.size(), "sha256:" + sha), std::vector<uint8_t>(elf.begin(), elf.begin() + 1000), 200,
			"is 1000 bytes, the release says 300000"},
		{"HTTP 404", ReleaseWithInstaller(tag, elf.size(), "sha256:" + sha), elf, 404, "HTTP 404 for the installer"},
		{"no network", ReleaseWithInstaller(tag, elf.size(), "sha256:" + sha), elf, -1, "couldn't download the installer"},
		{"not an ELF (no digest to catch it)", ReleaseWithInstaller(tag, elf.size(), ""), not_elf, 200, "not an ELF"},
		{"a release without an installer", Good(tag), elf, 200, "has no PS5SX2Installer.elf to download"},
	};
	for (const Bad& b : bad)
	{
		Fake f;
		Disk d;
		d.served = b.served;
		d.http = b.http;
		f.body = b.body;
		fe::Updater u(platform(f, d, false), "vk-285-182", "/data/PS5SX2-Installer");
		u.Check(false);
		WaitDone(u);
		CHECK(u.Install(), b.why + ": the download is tried");
		const fe::UpdateStatus s = settle(u);
		CHECK(s.state == S::StartFailed && s.error.find(b.error) != std::string::npos && f.started.empty() && d.files.count(dest) == 0,
			b.why + ": refused, nothing saved or started (" + s.error + ")");
		u.Stop(1000);
	}
	{
		// a cut-off install, no look yet: the release is looked at for its installer, then fetched and started (for the latest)
		Fake f;
		Disk d;
		d.served = elf;
		f.files.insert("/data/PS5SX2-Installer/journal.txt");
		f.body = ReleaseWithInstaller(tag, elf.size(), "sha256:" + sha);
		fe::Updater u(platform(f, d, false), "vk-285-182", "/data/PS5SX2-Installer");
		CHECK(u.Status().state == S::Interrupted, "interrupted");
		CHECK(u.Install(), "Cross: the installer is fetched");
		const fe::UpdateStatus s = settle(u);
		CHECK(s.state == S::Started && f.gets == 1 && d.downloads == 1 && f.started.size() == 1 && f.started[0].empty(),
			"looked, fetched, started for the latest: " + s.error);
		u.Stop(1000);
	}
}

// ---- the launch block ----

static std::vector<uint8_t> FakeInstaller(int blocks, bool filled = false)
{
	std::vector<uint8_t> elf = {0x7f, 'E', 'L', 'F', 2, 1, 1, 9};
	elf.resize(4096, 0x11);
	for (int b = 0; b < blocks; b++)
	{
		const size_t at = elf.size();
		elf.resize(at + 256, 0);
		std::memcpy(elf.data() + at, orbis_installer::kLaunchMarker, sizeof(orbis_installer::kLaunchMarker) - 1);
		if (filled)
			elf[at + 40] = 1;
		elf.resize(elf.size() + 512, 0x22);
	}
	// the marker in a string elsewhere (not followed by zeros) is no block
	const char text[] = "see PS5SX2-INSTALLER-LAUNCH-1 in launch.h";
	elf.insert(elf.end(), text, text + sizeof(text));
	elf.resize(elf.size() + 300, 0x33);
	return elf;
}

static void TestLaunchBlock()
{
	using namespace orbis_installer;
	CHECK(sizeof(installer_launch) == kLaunchSize, "the installer's struct is the app's size");
	CHECK(offsetof(installer_launch, magic) == kOffMagic && offsetof(installer_launch, size) == kOffSize &&
			offsetof(installer_launch, flags) == kOffFlags && offsetof(installer_launch, app_pid) == kOffPid &&
			offsetof(installer_launch, app_tag) == kOffAppTag && offsetof(installer_launch, expect_tag) == kOffExpectTag,
		"the same offsets");
	CHECK(std::strcmp(INSTALLER_LAUNCH_MARKER, kLaunchMarker) == 0 && INSTALLER_LAUNCH_MAGIC == kLaunchMagic &&
			INSTALLER_LAUNCH_RELAUNCH == kFlagRelaunch && INSTALLER_LAUNCH_LOGGER == kFlagLogger && INSTALLER_LAUNCH_TAG == kTagLen,
		"the same marker, magic and flags");

	std::vector<uint8_t> elf = FakeInstaller(1);
	const size_t at = FindLaunchBlock(elf);
	CHECK(at == 4096, "found");
	LaunchArgs a;
	a.flags = kFlagRelaunch;
	a.app_pid = 4321;
	a.app_tag = "vk-285-180";
	a.expect_tag = "vk-285-181";
	std::string e;
	const std::vector<uint8_t> before = elf;
	CHECK(FillLaunchBlock(elf, a, e), "filled: " + e);
	installer_launch got;
	std::memcpy(&got, elf.data() + at, sizeof(got));
	CHECK(std::strcmp(got.marker, INSTALLER_LAUNCH_MARKER) == 0 && got.magic == INSTALLER_LAUNCH_MAGIC && got.size == sizeof(installer_launch) &&
			got.flags == INSTALLER_LAUNCH_RELAUNCH && got.app_pid == 4321 && std::strcmp(got.app_tag, "vk-285-180") == 0 &&
			std::strcmp(got.expect_tag, "vk-285-181") == 0,
		"the installer reads what the app wrote");
	bool rest_same = true;
	for (size_t i = 0; i < elf.size(); i++)
		if ((i < at || i >= at + kLaunchSize) && elf[i] != before[i])
			rest_same = false;
	CHECK(rest_same && elf.size() == before.size(), "nothing else changed");
	CHECK(FindLaunchBlock(elf) == std::string_view::npos, "a filled block is not found again");

	a.flags = kFlagLogger;
	a.expect_tag.clear();
	elf = FakeInstaller(1);
	CHECK(FillLaunchBlock(elf, a, e), "logger: filled");
	std::memcpy(&got, elf.data() + 4096, sizeof(got));
	CHECK(got.flags == INSTALLER_LAUNCH_LOGGER && got.expect_tag[0] == 0, "logger flags, no expected tag");

	elf = FakeInstaller(0);
	CHECK(!FillLaunchBlock(elf, a, e) && e.find("no launch block") != std::string::npos, "an installer older than 1.4: " + e);
	elf = FakeInstaller(2);
	CHECK(!FillLaunchBlock(elf, a, e), "two blocks: refused");
	elf = FakeInstaller(1, true);
	CHECK(!FillLaunchBlock(elf, a, e), "an already filled block: refused");
	elf = FakeInstaller(1);
	elf[0] = 'X';
	CHECK(!FillLaunchBlock(elf, a, e) && e.find("isn't an ELF") != std::string::npos, "not an ELF");
	elf = FakeInstaller(1);
	a.app_tag = std::string(64, 'v');
	CHECK(!FillLaunchBlock(elf, a, e), "a tag too long");
	a.app_tag = "vk\n285";
	CHECK(!FillLaunchBlock(elf, a, e), "a tag with a control character");
	std::vector<uint8_t> tiny = {0x7f, 'E', 'L', 'F'};
	CHECK(!FillLaunchBlock(tiny, a, e), "a stub");
	// a block right at the end of the file
	elf = {0x7f, 'E', 'L', 'F'};
	elf.resize(64, 0);
	const size_t end_at = elf.size();
	elf.resize(end_at + kLaunchSize, 0);
	std::memcpy(elf.data() + end_at, kLaunchMarker, sizeof(kLaunchMarker) - 1);
	a.app_tag = "vk-285-180";
	CHECK(FindLaunchBlock(elf) == end_at && FillLaunchBlock(elf, a, e), "a block at the very end");
	elf.pop_back();
	CHECK(FindLaunchBlock(elf) == std::string_view::npos, "a block cut short is not one");
}

static void TestLoggerDecision()
{
	using namespace orbis_installer;
	CHECK(DecideLogger(false, false, true, false) == LoggerDecision::Send, "send");
	CHECK(DecideLogger(true, false, true, false) == LoggerDecision::SkipFlag, "the flag wins");
	CHECK(DecideLogger(false, true, true, false) == LoggerDecision::SkipOff, "no-log-upload: nothing sent");
	CHECK(DecideLogger(true, true, true, true) == LoggerDecision::SkipFlag, "flag first");
	CHECK(DecideLogger(false, false, false, false) == LoggerDecision::SkipNoElf, "no installer");
	CHECK(DecideLogger(false, false, true, true) == LoggerDecision::SkipRunning, "a logger runs");
	CHECK(std::string(LoggerDecisionName(LoggerDecision::SkipOff)).find("no-log-upload") != std::string::npos, "named");
}

int main()
{
	TestParse();
	TestTags();
	TestJudge();
	TestUpdater();
	TestInstallerDownload();
	TestLaunchBlock();
	TestLoggerDecision();
	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	if (g_fail == 0)
		std::printf("ALL PASSED\n");
	return g_fail ? 1 : 0;
}
