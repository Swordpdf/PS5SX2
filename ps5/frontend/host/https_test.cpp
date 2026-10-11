// PS5 port frontend: tests for fe_https (2026-10-05, AI-assisted).
//
//   https_test local <servers.json>   the servers of https_testserver.py (made by test-https.sh)
//   https_test real                   archive.org, through $https_proxy's CONNECT when it is set (the PC's preview VM)
//   https_test units                  URL parsing, redirects' addresses, the calendar conversion
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_https.h"

#include "mbedtls/build_info.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using fe::HttpsClient;

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

// ---- the PC's platform: getaddrinfo, or a CONNECT tunnel through an HTTP proxy ----
static std::string g_proxy_host, g_proxy_auth;
static int g_proxy_port = 0;
static std::string g_map_name, g_map_to; // "other.test" connects to 127.0.0.1 in the tests

static std::string Base64(const std::string& in)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	size_t i = 0;
	for (; i + 2 < in.size(); i += 3)
	{
		const unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
		out += t[v >> 18];
		out += t[(v >> 12) & 63];
		out += t[(v >> 6) & 63];
		out += t[v & 63];
	}
	if (i + 1 == in.size())
	{
		const unsigned v = (unsigned char)in[i] << 16;
		out += t[v >> 18];
		out += t[(v >> 12) & 63];
		out += "==";
	}
	else if (i + 2 == in.size())
	{
		const unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
		out += t[v >> 18];
		out += t[(v >> 12) & 63];
		out += t[(v >> 6) & 63];
		out += '=';
	}
	return out;
}

static std::string Unescape(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] == '%' && i + 2 < s.size())
		{
			out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
			i += 2;
		}
		else
			out += s[i];
	}
	return out;
}

// http://user:pass@host:port
static void UseProxy(const char* url)
{
	std::string u = url;
	if (u.compare(0, 7, "http://") == 0)
		u = u.substr(7);
	while (!u.empty() && u.back() == '/')
		u.pop_back();
	const size_t at = u.rfind('@');
	if (at != std::string::npos)
	{
		const std::string cred = u.substr(0, at);
		const size_t colon = cred.find(':');
		g_proxy_auth = Base64(Unescape(cred.substr(0, colon)) + ":" + (colon == std::string::npos ? "" : Unescape(cred.substr(colon + 1))));
		u = u.substr(at + 1);
	}
	const size_t colon = u.rfind(':');
	g_proxy_host = u.substr(0, colon);
	g_proxy_port = colon == std::string::npos ? 80 : std::atoi(u.c_str() + colon + 1);
	if (g_proxy_host == "localhost")
		g_proxy_host = "127.0.0.1";
}

static int TcpConnect(const std::string& host, int port, int io_ms, std::string& error)
{
	addrinfo hints{}, *res = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
	{
		error = "can't find " + host;
		return -1;
	}
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(fd, res->ai_addr, res->ai_addrlen) != 0)
	{
		freeaddrinfo(res);
		close(fd);
		error = "no connection to " + host;
		return -1;
	}
	freeaddrinfo(res);
	timeval tv{io_ms / 1000, (io_ms % 1000) * 1000};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	return fd;
}

static int HostConnect(const std::string& host, uint16_t port, int, int io_ms, std::string& error)
{
	const std::string target = host == g_map_name ? g_map_to : host;
	if (g_proxy_port == 0)
		return TcpConnect(target, port, io_ms, error);
	const int fd = TcpConnect(g_proxy_host, g_proxy_port, io_ms, error);
	if (fd < 0)
		return -1;
	std::string req = "CONNECT " + target + ":" + std::to_string(port) + " HTTP/1.1\r\nHost: " + target + ":" + std::to_string(port) + "\r\n";
	if (!g_proxy_auth.empty())
		req += "Proxy-Authorization: Basic " + g_proxy_auth + "\r\n";
	req += "\r\n";
	send(fd, req.data(), req.size(), MSG_NOSIGNAL);
	std::string answer;
	char c;
	while (answer.find("\r\n\r\n") == std::string::npos && recv(fd, &c, 1, 0) == 1)
		answer += c;
	if (answer.compare(0, 12, "HTTP/1.1 200") != 0 && answer.compare(0, 12, "HTTP/1.0 200") != 0)
	{
		error = "the proxy refused " + target + ": " + answer.substr(0, answer.find('\r'));
		close(fd);
		return -1;
	}
	return fd;
}

static fe::HttpsPlatform HostPlatform()
{
	fe::HttpsPlatform p;
	p.connect = HostConnect;
	p.random = [](void* buf, size_t len) {
		unsigned char* b = static_cast<unsigned char*>(buf);
		while (len > 0)
		{
			const ssize_t n = getrandom(b, len, 0);
			if (n <= 0)
				return false;
			b += n;
			len -= static_cast<size_t>(n);
		}
		return true;
	};
	p.log = [](const std::string& line) { std::printf("  log: %s\n", line.c_str()); };
	return p;
}

// ---- helpers ----
static std::string Sha256(const std::string& data)
{
	unsigned char out[32];
	mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), reinterpret_cast<const unsigned char*>(data.data()), data.size(), out);
	char hex[65];
	for (int i = 0; i < 32; i++)
		std::snprintf(hex + 2 * i, 3, "%02x", out[i]);
	return hex;
}

struct Fetch
{
	int status = 0;
	std::string body, error;
};

static Fetch Get(HttpsClient& c, const std::string& url, bool ranged = false, uint64_t offset = 0, uint64_t length = 0, size_t stop_after = 0,
	int total_ms = 0)
{
	Fetch f;
	f.status = c.Get(url, ranged, offset, length, [&](const void* p, size_t n) {
		f.body.append(static_cast<const char*>(p), n);
		return stop_after == 0 || f.body.size() < stop_after;
	}, total_ms, &f.error);
	std::printf("  GET %s%s -> %d, %zu bytes%s%s\n", url.c_str(), ranged ? (" [" + std::to_string(offset) + "+" + std::to_string(length) + "]").c_str() : "",
		f.status, f.body.size(), f.error.empty() ? "" : ": ", f.error.c_str());
	return f;
}

// A tiny JSON field reader for the servers' line (the test controls its format).
static std::string JsonString(const std::string& json, const std::string& key)
{
	const size_t k = json.find("\"" + key + "\": \"");
	if (k == std::string::npos)
		return {};
	std::string out;
	for (size_t i = k + key.size() + 5; i < json.size() && json[i] != '"'; i++)
	{
		if (json[i] == '\\' && i + 1 < json.size())
		{
			i++;
			out += json[i] == 'n' ? '\n' : json[i];
		}
		else
			out += json[i];
	}
	return out;
}

static int JsonPort(const std::string& json, const std::string& key)
{
	const size_t k = json.find("\"" + key + "\": ");
	return k == std::string::npos ? 0 : std::atoi(json.c_str() + k + key.size() + 4);
}

// ---- the tests ----
static void Units()
{
	fe::HttpsUrl u;
	CHECK(fe::ParseHttpsUrl("https://archive.org/metadata/pcsx2-hd-texture-packs", u));
	CHECK(u.host == "archive.org" && u.port == 443 && u.path == "/metadata/pcsx2-hd-texture-packs");
	CHECK(fe::ParseHttpsUrl("HTTPS://Archive.ORG", u) && u.host == "archive.org" && u.path == "/");
	CHECK(fe::ParseHttpsUrl("https://localhost:8443?x=1#frag", u) && u.port == 8443 && u.path == "/?x=1");
	CHECK(fe::ParseHttpsUrl("https://h/a%20b?c=d#e", u) && u.path == "/a%20b?c=d");
	CHECK(!fe::ParseHttpsUrl("http://archive.org/", u));
	CHECK(!fe::ParseHttpsUrl("https://user@archive.org/", u));
	CHECK(!fe::ParseHttpsUrl("https://archive.org:0/", u));
	CHECK(!fe::ParseHttpsUrl("https://archive.org:65536/", u));
	CHECK(!fe::ParseHttpsUrl("https://archive.org/a b", u));
	CHECK(!fe::ParseHttpsUrl("https://archive.org/a\r\nX: y", u));
	CHECK(!fe::ParseHttpsUrl("https://[::1]/", u));
	CHECK(!fe::ParseHttpsUrl("https:///path", u));
	CHECK(!fe::ParseHttpsUrl("https://exa_mple.com/", u));

	fe::HttpsUrl base;
	fe::ParseHttpsUrl("https://archive.org/download/item/file.rar?x=1", base);
	CHECK(fe::ResolveHttpsLocation(base, "https://dn721905.ca.archive.org/0/items/x") == "https://dn721905.ca.archive.org/0/items/x");
	CHECK(fe::ResolveHttpsLocation(base, " //ia800105.us.archive.org/6/items/x ") == "https://ia800105.us.archive.org/6/items/x");
	CHECK(fe::ResolveHttpsLocation(base, "/details/item") == "https://archive.org/details/item");
	CHECK(fe::ResolveHttpsLocation(base, "other.rar") == "https://archive.org/download/item/other.rar");
	CHECK(fe::ResolveHttpsLocation(base, "http://archive.org/x").empty());
	CHECK(fe::ResolveHttpsLocation(base, "ftp://archive.org/x").empty());
	CHECK(fe::ResolveHttpsLocation(base, "").empty());
	fe::HttpsUrl port_base;
	fe::ParseHttpsUrl("https://localhost:8443/a/b", port_base);
	CHECK(fe::ResolveHttpsLocation(port_base, "/c") == "https://localhost:8443/c");

	// The calendar conversion against the C library's, over a wide spread of times.
	std::mt19937_64 rng(7);
	int bad = 0;
	for (int i = 0; i < 200000; i++)
	{
		const int64_t t = i < 1000 ? (int64_t)i * 86399 - 500 * 86400 : (int64_t)(rng() % (int64_t)8000000000LL) - 2000000000LL;
		const time_t tt = static_cast<time_t>(t);
		struct tm ours, libc;
		mbedtls_time_t mt = t;
		if (!mbedtls_platform_gmtime_r(&mt, &ours) || !gmtime_r(&tt, &libc))
		{
			bad++;
			continue;
		}
		if (ours.tm_year != libc.tm_year || ours.tm_mon != libc.tm_mon || ours.tm_mday != libc.tm_mday || ours.tm_hour != libc.tm_hour ||
			ours.tm_min != libc.tm_min || ours.tm_sec != libc.tm_sec || ours.tm_wday != libc.tm_wday || ours.tm_yday != libc.tm_yday)
		{
			if (bad++ < 3)
				std::printf("  gmtime differs at %lld\n", (long long)t);
		}
	}
	CHECK(bad == 0);
	std::printf("units: done\n");
}

static void Local(const char* json_path)
{
	std::ifstream in(json_path);
	std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const std::string root = JsonString(json, "root"), ecroot = JsonString(json, "ecroot"), blob_sha = JsonString(json, "blob_sha256");
	const int main_port = JsonPort(json, "main"), ec_port = JsonPort(json, "ec"), expired_port = JsonPort(json, "expired"),
			  other_port = JsonPort(json, "other");
	CHECK(!root.empty() && !ecroot.empty() && main_port > 0 && ec_port > 0);
	const std::string base = "https://localhost:" + std::to_string(main_port);

	// 1. Mozilla's roots only: the test servers' chains must be refused.
	{
		HttpsClient c(HostPlatform());
		std::string err;
		CHECK(c.Init(err));
		const Fetch f = Get(c, base + "/file");
		CHECK(f.status == HttpsClient::kConnect && f.body.empty() && f.error.find("isn't trusted") != std::string::npos);
	}

	HttpsClient c(HostPlatform());
	std::string err;
	c.SkipBundledRoots();
	CHECK(c.Init(err));
	CHECK(c.AddRoots(root.c_str(), err));
	CHECK(c.AddRoots(ecroot.c_str(), err));

	// 2. archive.org's shape: leaf, intermediate, the root cross-signed by an untrusted SHA-1 root, that root. Trusted.
	Fetch f = Get(c, base + "/file");
	CHECK(f.status == 200 && f.body.size() == 5u * 1024 * 1024 && Sha256(f.body) == blob_sha);
	const std::string blob = f.body;

	// 3. Ranges, chunked, no length, a 100 first.
	f = Get(c, base + "/file", true, 1234567, 3 * 1024 * 1024);
	CHECK(f.status == 206 && f.body == blob.substr(1234567, 3 * 1024 * 1024));
	f = Get(c, base + "/file", true, blob.size() - 10, 10);
	CHECK(f.status == 206 && f.body == blob.substr(blob.size() - 10));
	f = Get(c, base + "/chunked");
	CHECK(f.status == 200 && f.body == blob);
	f = Get(c, base + "/chunked", true, 100, 200000);
	CHECK(f.status == 206 && f.body == blob.substr(100, 200000));
	f = Get(c, base + "/nolength");
	CHECK(f.status == 200 && f.body == blob);
	f = Get(c, base + "/continue");
	CHECK(f.status == 200 && f.body == blob);

	// 4. Redirects: to another server (keeping the range), relative, root-relative; never to http://; not forever.
	f = Get(c, base + "/redirect", true, 4096, 65536);
	CHECK(f.status == 206 && f.body == blob.substr(4096, 65536));
	f = Get(c, base + "/redirect-rel");
	CHECK(f.status == 200 && f.body == blob);
	f = Get(c, base + "/redirect-slash");
	CHECK(f.status == 200 && f.body == blob);
	f = Get(c, base + "/redirect-http");
	CHECK(f.status == HttpsClient::kConnect && f.body.empty());
	f = Get(c, base + "/loop");
	CHECK(f.status == HttpsClient::kRead && f.error == "too many redirects");

	// 5. Answers that aren't the file.
	f = Get(c, base + "/norange", true, 1000, 500);
	CHECK(f.status == HttpsClient::kIgnoredRange && f.body.empty());
	f = Get(c, base + "/norange", true, 0, 500, 500);
	CHECK(f.status == 200 && f.body.compare(0, 500, blob, 0, 500) == 0);
	f = Get(c, base + "/missing");
	CHECK(f.status == 404 && f.body.empty());
	f = Get(c, base + "/wrongrange", true, 500, 10);
	CHECK(f.status == HttpsClient::kRead && f.body.empty());
	f = Get(c, base + "/short");
	CHECK(f.status == HttpsClient::kRead && f.body.size() == 400);

	// 6. The sink stops it early; the total time limit.
	f = Get(c, base + "/file", false, 0, 0, 100000);
	CHECK(f.status == 200 && f.body.size() >= 100000 && f.body.size() < blob.size());
	{
		const auto t0 = std::chrono::steady_clock::now();
		f = Get(c, base + "/slow", false, 0, 0, 0, 1500);
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		std::printf("  the total limit ended it after %lld ms\n", (long long)ms);
		CHECK(f.status == HttpsClient::kRead && f.body.size() == 1000 && ms < 4000);
	}

	// 7. ECDSA (P-384 root, P-256 leaf).
	f = Get(c, "https://localhost:" + std::to_string(ec_port) + "/file", true, 0, 1000);
	CHECK(f.status == 206 && f.body == blob.substr(0, 1000));

	// 8. Refused: an expired leaf; a leaf for another name (asked as localhost); asked by an IP the leaf doesn't name.
	f = Get(c, "https://localhost:" + std::to_string(expired_port) + "/file");
	CHECK(f.status == HttpsClient::kConnect && f.error.find("expired") != std::string::npos);
	f = Get(c, "https://localhost:" + std::to_string(other_port) + "/file");
	CHECK(f.status == HttpsClient::kConnect && f.error.find("isn't trusted") != std::string::npos);
	f = Get(c, "https://127.0.0.1:" + std::to_string(main_port) + "/file");
	CHECK(f.status == HttpsClient::kConnect && f.error.find("isn't trusted") != std::string::npos);
	// ... and the name it does carry works (other.test, connected to 127.0.0.1).
	g_map_name = "other.test";
	g_map_to = "127.0.0.1";
	f = Get(c, "https://other.test:" + std::to_string(other_port) + "/file", true, 0, 10);
	CHECK(f.status == 206 && f.body == blob.substr(0, 10));

	// 9. Abort from another thread, during a stalled answer; the next request works.
	{
		std::atomic<bool> done{false};
		Fetch slow;
		const auto t0 = std::chrono::steady_clock::now();
		std::thread t([&] {
			slow = Get(c, base + "/slow");
			done = true;
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(700));
		c.Abort();
		t.join();
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		std::printf("  aborted after %lld ms\n", (long long)ms);
		CHECK(slow.status == HttpsClient::kAborted && ms < 3000);
		f = Get(c, base + "/file", true, 0, 100);
		CHECK(f.status == 206 && f.body == blob.substr(0, 100));
	}

	// 10. pr9n: Request, for an API (RetroAchievements): a POST's form with its type and length and our User-Agent; the
	// body of any answer (an error's JSON too); redirects returned, not followed; refusals as kUntrusted.
	{
		const auto request = [&](const std::string& method, const std::string& url, const std::string& body) {
			Fetch r;
			r.status = c.Request(method, url, body, body.empty() ? "" : "application/x-www-form-urlencoded",
				[&](const void* p, size_t n) {
					r.body.append(static_cast<const char*>(p), n);
					return true;
				},
				5000, &r.error);
			std::printf("  %s %s -> %d, %zu bytes%s%s\n", method.c_str(), url.c_str(), r.status, r.body.size(), r.error.empty() ? "" : ": ",
				r.error.c_str());
			return r;
		};
		c.user_agent = "PS5SX2/test (PS5 11.40) rcheevos/12.4.0";
		f = request("POST", base + "/echo", "r=login2&u=someone&p=secret%20word");
		CHECK(f.status == 201 && f.body.find("length=34\n") != std::string::npos &&
			  f.body.find("type=application/x-www-form-urlencoded\n") != std::string::npos &&
			  f.body.find("agent=PS5SX2/test (PS5 11.40) rcheevos/12.4.0\n") != std::string::npos &&
			  f.body.find("body=r=login2&u=someone&p=secret%20word") != std::string::npos);
		f = request("POST", base + "/api-error", "r=login2&u=someone&p=wrong");
		CHECK(f.status == 401 && f.body.find("Invalid user/password") != std::string::npos);
		f = request("GET", base + "/missing", "");
		CHECK(f.status == 404 && f.body == "not here\n");
		f = request("GET", base + "/redirect", "");
		CHECK(f.status == 302);
		f = request("GET", base + "/file", "");
		CHECK(f.status == 200 && f.body == blob);
		f = request("POST", "https://localhost:" + std::to_string(other_port) + "/echo", "a=1");
		CHECK(f.status == HttpsClient::kUntrusted && f.body.empty());
		f = request("POST", "http://localhost:" + std::to_string(main_port) + "/echo", "a=1");
		CHECK(f.status == HttpsClient::kUntrusted);
		f = request("GET", "https://localhost:1/nothing-listens", "");
		CHECK(f.status == HttpsClient::kConnect);
		// Get still says kConnect for an untrusted certificate (the texture packs' code reads it that way).
		f = Get(c, "https://localhost:" + std::to_string(other_port) + "/file");
		CHECK(f.status == HttpsClient::kConnect);
		c.user_agent = "PS5SX2/1.0";
	}

	// 11. Two threads at once: one at a time, both right.
	{
		Fetch a, b;
		std::thread t1([&] { a = Get(c, base + "/file", true, 0, 1000000); });
		std::thread t2([&] { b = Get(c, base + "/chunked", true, 1000000, 1000000); });
		t1.join();
		t2.join();
		CHECK(a.status == 206 && a.body == blob.substr(0, 1000000));
		CHECK(b.status == 206 && b.body == blob.substr(1000000, 1000000));
	}
	std::printf("local: done\n");
}

static void Real()
{
	if (const char* p = std::getenv("https_proxy"))
		UseProxy(p);
	else if (const char* q = std::getenv("HTTPS_PROXY"))
		UseProxy(q);
	HttpsClient c(HostPlatform());
	std::string err;
	CHECK(c.Init(err));
	// archive.org's list of the packs.
	Fetch f = Get(c, "https://archive.org/metadata/pcsx2-hd-texture-packs", false, 0, 0, 0, 60000);
	CHECK(f.status == 200 && f.body.find("\"files\"") != std::string::npos);
	const size_t rars = [&] {
		size_t n = 0, p = 0;
		while ((p = f.body.find(".rar\"", p)) != std::string::npos)
			n++, p++;
		return n;
	}();
	std::printf("  %zu .rar names in the list\n", rars);
	CHECK(rars > 300);
	// A data node directly, and a whole pack through the download redirect, against archive.org's MD5.
	f = Get(c, "https://ia800105.us.archive.org/6/items/pcsx2-hd-texture-packs/pcsx2-hd-texture-packs_meta.xml", true, 0, 100);
	CHECK(f.status == 206 && f.body.size() == 100);
	static const char kPack[] = "Spider-Man%202%20%28USA%29%20%5BSLUS-20776%5D%20HD%20Remaster.rar";
	auto t0 = std::chrono::steady_clock::now();
	f = Get(c, std::string("https://archive.org/download/pcsx2-hd-texture-packs/") + kPack, true, 0, 12996665, 0, 900000);
	if (f.status == HttpsClient::kConnect && f.error.find("the proxy refused") != std::string::npos)
	{
		// A PC whose proxy only lets some of archive.org's servers through: the same file from the data node it allows.
		std::printf("  (the proxy here doesn't allow the server archive.org redirects to; the data node instead)\n");
		t0 = std::chrono::steady_clock::now();
		f = Get(c, std::string("https://ia800105.us.archive.org/6/items/pcsx2-hd-texture-packs/") + kPack, true, 0, 12996665, 0, 900000);
	}
	const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::printf("  %.1f s, %.2f MB/s, SHA-256 %s\n", s, f.body.size() / s / 1048576.0, Sha256(f.body).c_str());
	// The size on archive.org's list and RAR 5's mark (the texture pack manager checks archive.org's MD5 itself).
	CHECK(f.status == 206 && f.body.size() == 12996665 && f.body.compare(0, 8, std::string("Rar!\x1a\x07\x01\x00", 8)) == 0);
	std::printf("real: done\n");
}

int main(int argc, char** argv)
{
	const std::string mode = argc > 1 ? argv[1] : "";
	if (mode == "units")
		Units();
	else if (mode == "local" && argc > 2)
		Local(argv[2]);
	else if (mode == "real")
		Real();
	else
	{
		std::printf("usage: https_test units | local <servers.json> | real\n");
		return 2;
	}
	std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
	return g_failures ? 1 : 0;
}
