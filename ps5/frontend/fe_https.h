// PS5 port frontend: our own HTTPS GET client (2026-10-05, AI-assisted; needs proper testing on the console).
//
// After the jailbreak the console's libSceSsl can't reach its certificate store (sceSslGetCaList answers 0x80020002), so
// it trusts no site unless it is handed roots, and with roots it still can't follow a cross-signed chain: archive.org
// sends GoDaddy's Root G2 cross-signed by the old Class 2 root, and its download servers fail the same way. This client
// runs TLS itself with mbedTLS (ps5/third_party/mbedtls), checks every server's chain against Mozilla's roots (bundled,
// fe_https_roots.inc) and its name, and speaks enough HTTP/1.1 for downloads: redirects, ranges, chunked answers. The
// texture pack downloads use it (fe_ps5.cpp); the system's parts (DNS, the socket, random numbers) come from the
// platform, so the PC preview and the tests run it too.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fe
{
	// What the client needs from the system.
	struct HttpsPlatform
	{
		// A connected TCP socket to host:port (with the receive and send timeouts set), or -1 with `error` set.
		std::function<int(const std::string& host, uint16_t port, int timeout_ms, int io_timeout_ms, std::string& error)> connect;
		// Fills `buf` with the system's random numbers; false when it has none.
		std::function<bool(void* buf, size_t len)> random;
		// A line for the log (no newline).
		std::function<void(const std::string&)> log;
	};

	struct HttpsUrl
	{
		std::string host; // lower case
		uint16_t port = 443;
		std::string path; // from the first '/', with the query; "/" when the URL has none
	};

	// https://host[:port][/path]: false for anything else (no http://, no user info, no fragment kept).
	bool ParseHttpsUrl(const std::string& url, HttpsUrl& out);
	// A redirect's Location against the URL it came from: absolute, "//host/path" or "/path" (and "path" relative to the
	// base's folder). Empty when it isn't an https:// address.
	std::string ResolveHttpsLocation(const HttpsUrl& base, const std::string& location);

	class HttpsClient
	{
	public:
		// The results below 0.
		static constexpr int kConnect = -1; // no connection, or the TLS handshake failed (a certificate the roots don't vouch for)
		static constexpr int kRead = -2; // the transfer broke, timed out or the answer made no sense
		static constexpr int kIgnoredRange = -3; // a range after the start was answered with the whole file (nothing delivered)
		static constexpr int kAborted = -4; // Abort()
		static constexpr int kUntrusted = -5; // Request only: the certificate isn't one the roots vouch for (Get says kConnect)

		explicit HttpsClient(HttpsPlatform platform);
		~HttpsClient();
		HttpsClient(const HttpsClient&) = delete;
		HttpsClient& operator=(const HttpsClient&) = delete;

		// Reads the bundled roots and seeds the random generator; once (later calls answer the first one's result).
		bool Init(std::string& error);
		// More trusted roots (PEM), for the tests. Before the first Get.
		bool AddRoots(const char* pem, std::string& error);
		// Leaves out the bundled roots (the tests' "a server Mozilla's roots don't vouch for"). Before Init.
		void SkipBundledRoots() { m_skip_bundled = true; }

		// One GET of an https:// URL: the whole answer (ranged false) or bytes [offset, offset + length). Follows up to 5
		// redirects (https only), keeping the range. The body of a 2xx answer goes to `sink`; a false from it ends the
		// transfer early (the status is still returned). A range from byte 0 answered with the whole file (200) is
		// delivered from its start; one from later is not delivered at all (kIgnoredRange). Returns the final HTTP status,
		// or one of the codes above. One request at a time (others wait). `total_timeout_ms` caps the whole request (0:
		// none).
		int Get(const std::string& url, bool ranged, uint64_t offset, uint64_t length,
			const std::function<bool(const void*, size_t)>& sink, int total_timeout_ms = 0, std::string* error = nullptr);

		// pr9n (2026-10-05, AI-assisted): one request of any method for an API (RetroAchievements, ProsperoHTTPDownloader.cpp).
		// `body` goes out with `content_type` when it isn't empty (or the method is POST). The answer's body goes to `sink`
		// whatever the status (an API's errors come as JSON); redirects aren't followed. Returns the HTTP status, or a code
		// above: kConnect (no connection, DNS, TLS), kRead, kAborted, kUntrusted.
		int Request(const std::string& method, const std::string& url, const std::string& body, const std::string& content_type,
			const std::function<bool(const void*, size_t)>& sink, int total_timeout_ms = 0, std::string* error = nullptr);

		// From another thread: fails the request in flight with kAborted. The next request starts normally.
		void Abort();

		// The User-Agent header (Get's and Request's).
		std::string user_agent = "PS5SX2/1.0";

		// The time limits for each connect and each wait for data (milliseconds).
		int connect_timeout_ms = 10000;
		int io_timeout_ms = 30000;

	private:
		struct Impl;
		std::unique_ptr<Impl> m;
		bool m_skip_bundled = false;
	};

	// The console's parts (fe_https_ps5.cpp, PS5 builds only): DNS with sceNetResolver on the libnet pool `net_pool`, an
	// IPv4 TCP socket, and the kernel's random numbers (kern.arandom; /dev/urandom or RDRAND when that's refused).
	HttpsPlatform MakeConsoleHttpsPlatform(int net_pool, std::function<void(const std::string&)> log);
} // namespace fe
