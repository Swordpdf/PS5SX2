// PS5SX2 RetroAchievements transport (AI-assisted): PCSX2's HTTPDownloader over HTTPS connections. pr9n
// (2026-10-05), after the PR #9 review: the connections are fe::HttpsClient (our own TLS with Mozilla's roots,
// ProsperoHTTPConsole.cpp) on the console and a fake in tests/achievements/test_transport.cpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common/HTTPDownloader.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace OrbisAchievementsHttp
{
	// Connection::Send's results below 0.
	constexpr int kNoAnswer = -1; // no HTTP status came back (DNS, connect, a timeout, a dropped connection): retried
	constexpr int kRefused = -2; // never worth retrying: not an https:// address, a certificate the roots don't vouch for
	constexpr int kAborted = -3; // Abort()

	// One request at a time.
	class Connection
	{
	public:
		virtual ~Connection() = default;
		// The HTTP status (any body in `response`), or a code above. `timeout_ms` caps the whole request.
		virtual int Send(const std::string& method, const std::string& url, const std::string& body, const std::string& content_type,
			std::vector<u8>& response, int timeout_ms) = 0;
		// From another thread: the request in flight ends with kAborted (if the connection can tell; Send may still run on).
		virtual void Abort() = 0;
	};
	using ConnectionFactory = std::function<std::unique_ptr<Connection>()>;

	// The downloader over connections from `factory`, at most 4 requests at once. It never waits on the thread that polls
	// it (the EE thread, with the achievements lock held): a closed request is aborted and its worker finishes on its own.
	// The destructor waits at most `shutdown_wait_ms` for workers still running and leaves the rest behind.
	std::unique_ptr<HTTPDownloader> Create(ConnectionFactory factory, int shutdown_wait_ms = 1500);
} // namespace OrbisAchievementsHttp
