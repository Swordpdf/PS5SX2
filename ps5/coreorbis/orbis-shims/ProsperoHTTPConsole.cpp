// PS5SX2: RetroAchievements' HTTPS on the console (pr9n, 2026-10-05, AI-assisted; needs proper testing).
//
// PCSX2's HTTPDownloader::Create for the PS5: ProsperoHTTPDownloader.cpp over fe::HttpsClient (ps5/frontend/fe_https.h),
// our own TLS with mbedTLS, which checks every server against Mozilla's roots and its name. PR #9 used the console's
// libSceHttp2 with one pinned root (GTS Root R4): after the jailbreak libSceSsl can't reach the system's store, and it
// can't follow a cross-signed chain even with the roots loaded (the texture pack work found that on archive.org and on a
// Let's Encrypt chain), so a change of RetroAchievements' certificate would have stopped sign-in, unlocks and badges.
// Roots for a chain Mozilla's list doesn't cover can be added without a release: /data/PCSX2/retroachievements-roots.pem
// (PEM, read when a downloader is made). Certificate checks are never turned off.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProsperoHTTPDownloader.h"

#include "ps5/frontend/fe_https.h"

#include <cstdio>
#include <memory>
#include <string>

extern "C" {
int sceNetInit();
int sceNetPoolCreate(const char*, int, int);
int sceNetPoolDestroy(int);
}

namespace
{
	constexpr const char* kExtraRoots = "/data/PCSX2/retroachievements-roots.pem";
	constexpr size_t kMaxAnswer = 16 * 1024 * 1024;

	void Log(const std::string& line)
	{
		std::printf("[achievements-http] %s\n", line.c_str());
		std::fflush(stdout);
	}

	// The libnet pool the resolver works in; kept while any connection uses it.
	struct Net
	{
		int pool = -1;
		~Net()
		{
			if (pool >= 0)
				sceNetPoolDestroy(pool);
		}
	};

	class ConsoleConnection final : public OrbisAchievementsHttp::Connection
	{
	public:
		ConsoleConnection(std::shared_ptr<Net> net, const std::string& user_agent, const std::string& extra_roots)
			: m_net(std::move(net))
			, m_client(fe::MakeConsoleHttpsPlatform(m_net->pool, Log))
		{
			m_client.user_agent = user_agent;
			m_client.connect_timeout_ms = 15000;
			m_client.io_timeout_ms = 30000;
			std::string error;
			if (!extra_roots.empty() && !m_client.AddRoots(extra_roots.c_str(), error))
				Log(std::string(kExtraRoots) + ": not all of it reads (" + error + ")");
			m_ready = m_client.Init(error);
			if (!m_ready)
				Log("HTTPS isn't ready: " + error);
		}

		bool Ready() const { return m_ready; }

		int Send(const std::string& method, const std::string& url, const std::string& body, const std::string& content_type,
			std::vector<u8>& response, int timeout_ms) override
		{
			response.clear();
			bool too_big = false;
			std::string error;
			const int r = m_client.Request(method, url, body, content_type,
				[&](const void* p, size_t n) {
					if (response.size() + n > kMaxAnswer)
					{
						too_big = true;
						return false;
					}
					const u8* bytes = static_cast<const u8*>(p);
					response.insert(response.end(), bytes, bytes + n);
					return true;
				},
				timeout_ms, &error);
			if (too_big)
			{
				Log("an answer over 16 MB: dropped");
				return OrbisAchievementsHttp::kRefused;
			}
			if (r >= 100)
				return r;
			if (r == fe::HttpsClient::kAborted)
				return OrbisAchievementsHttp::kAborted;
			if (r == fe::HttpsClient::kUntrusted)
				return OrbisAchievementsHttp::kRefused;
			return OrbisAchievementsHttp::kNoAnswer; // DNS, connect, TLS, a timeout or a broken transfer: retried
		}

		void Abort() override { m_client.Abort(); }

	private:
		std::shared_ptr<Net> m_net; // before m_client: its platform uses the pool
		fe::HttpsClient m_client;
		bool m_ready = false;
	};

	std::string ReadExtraRoots()
	{
		std::FILE* f = std::fopen(kExtraRoots, "rb");
		if (!f)
			return {};
		std::string pem;
		char buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && pem.size() < 256 * 1024)
			pem.append(buf, n);
		std::fclose(f);
		Log(std::string("extra roots from ") + kExtraRoots + " (" + std::to_string(pem.size()) + " bytes)");
		return pem;
	}
} // namespace

std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(std::string user_agent)
{
	sceNetInit(); // may be done already (the covers, the texture packs)
	auto net = std::make_shared<Net>();
	net->pool = sceNetPoolCreate("pcsx2-achievements", 128 * 1024, 0);
	if (net->pool < 0)
	{
		Log("no libnet pool: " + std::to_string(net->pool));
		return nullptr;
	}
	const std::string extra_roots = ReadExtraRoots();
	return OrbisAchievementsHttp::Create([net, user_agent, extra_roots]() -> std::unique_ptr<OrbisAchievementsHttp::Connection> {
		auto c = std::make_unique<ConsoleConnection>(net, user_agent, extra_roots);
		if (!c->Ready())
			return nullptr;
		return c;
	});
}
