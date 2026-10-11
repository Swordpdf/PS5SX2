// Exercises the real downloader (ProsperoHTTPDownloader.cpp) over fake HTTPS connections (AI-assisted).
// pr9n: the transport after the PR #9 review: answers of any status with their bodies, network failures retryable
// (HTTP_STATUS_TIMEOUT), refusals not (HTTP_STATUS_ERROR), at most 4 requests at once, callbacks only on the polling
// thread, and nothing ever waits for a worker on the polling thread: a request whose abort doesn't get through (a DNS
// lookup that never returns) neither stalls the poll that times it out nor the downloader's destruction.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5/coreorbis/orbis-shims/ProsperoHTTPDownloader.h"
#include "common/Console.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace Threading
{
	void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
} // namespace Threading
namespace Log
{
	void Writef(LOGLEVEL, ConsoleColors, const char*, ...) {}
	void Writev(LOGLEVEL, ConsoleColors, const char*, va_list) {}
} // namespace Log
void pxOnAssertFail(const char*, int, const char*, const char*) { std::abort(); }

namespace
{
	std::mutex g_mutex;
	std::condition_variable g_wake;
	int g_running = 0, g_max_running = 0, g_made = 0, g_alive = 0, g_posts = 0;
	std::atomic<int> g_stuck_done{0};

	double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

	class FakeConnection final : public OrbisAchievementsHttp::Connection
	{
	public:
		FakeConnection()
		{
			std::lock_guard lock(g_mutex);
			g_made++;
			g_alive++;
		}
		~FakeConnection() override
		{
			std::lock_guard lock(g_mutex);
			g_alive--;
		}
		int Send(const std::string& method, const std::string& url, const std::string& body, const std::string& content_type,
			std::vector<u8>& response, int timeout_ms) override
		{
			{
				std::lock_guard lock(g_mutex);
				g_running++;
				g_max_running = std::max(g_max_running, g_running);
			}
			struct Leave
			{
				~Leave()
				{
					std::lock_guard lock(g_mutex);
					g_running--;
				}
			} leave;
			assert(timeout_ms >= 1000);
			const auto answer = [&](int status, const std::string& text) {
				response.assign(text.begin(), text.end());
				return status;
			};
			if (url.rfind("https://", 0) != 0)
				return OrbisAchievementsHttp::kRefused;
			if (method == "POST")
			{
				assert(content_type == "application/x-www-form-urlencoded" && body == "u=test&p=synthetic");
				std::lock_guard lock(g_mutex);
				g_posts++;
			}
			else
				assert(method == "GET" && body.empty());
			if (url.ends_with("/ok"))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(20)); // so requests overlap
				return answer(200, "success");
			}
			if (url.ends_with("/error"))
				return answer(401, "{\"Success\":false,\"Error\":\"invalid account\"}");
			if (url.ends_with("/blip"))
				return OrbisAchievementsHttp::kNoAnswer;
			if (url.ends_with("/untrusted"))
				return OrbisAchievementsHttp::kRefused;
			if (url.ends_with("/slow")) // ends when aborted
			{
				std::unique_lock lock(g_mutex);
				g_wake.wait_for(lock, std::chrono::seconds(5), [&] { return m_aborted; });
				return m_aborted ? OrbisAchievementsHttp::kAborted : answer(200, "late");
			}
			if (url.ends_with("/stuck")) // ignores Abort, like a DNS lookup that never returns: 1.5 s
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1500));
				g_stuck_done++;
				return OrbisAchievementsHttp::kNoAnswer;
			}
			return answer(404, "");
		}
		void Abort() override
		{
			std::lock_guard lock(g_mutex);
			m_aborted = true;
			g_wake.notify_all();
		}

	private:
		bool m_aborted = false;
	};

	std::unique_ptr<HTTPDownloader> Make(int shutdown_wait_ms = 1500)
	{
		return OrbisAchievementsHttp::Create([] { return std::make_unique<FakeConnection>(); }, shutdown_wait_ms);
	}
} // namespace

int main()
{
	const std::thread::id owner = std::this_thread::get_id();
	{
		auto downloader = Make();
		int callbacks = 0;
		downloader->CreatePostRequest("https://example.invalid/ok", "u=test&p=synthetic",
			[&](s32 status, const std::string&, HTTPDownloader::Request::Data data) {
				assert(std::this_thread::get_id() == owner);
				assert(status == 200 && std::string(data.begin(), data.end()) == "success");
				callbacks++;
			});
		// An API error comes with its body (rcheevos reads the message from it).
		downloader->CreateRequest("https://example.invalid/error", [&](s32 status, const std::string&, HTTPDownloader::Request::Data data) {
			assert(status == 401 && std::string(data.begin(), data.end()).find("invalid account") != std::string::npos);
			callbacks++;
		});
		downloader->CreatePostRequest("http://example.invalid/blocked", "u=test&p=synthetic",
			[&](s32 status, const std::string&, HTTPDownloader::Request::Data data) {
				assert(status == HTTPDownloader::HTTP_STATUS_ERROR && data.empty());
				callbacks++;
			});
		// Review item 5: no HTTP status at all is retryable (TIMEOUT); a refused certificate isn't (ERROR).
		downloader->CreatePostRequest("https://example.invalid/blip", "u=test&p=synthetic",
			[&](s32 status, const std::string&, HTTPDownloader::Request::Data data) {
				assert(status == HTTPDownloader::HTTP_STATUS_TIMEOUT && data.empty());
				callbacks++;
			});
		downloader->CreateRequest("https://example.invalid/untrusted", [&](s32 status, const std::string&, HTTPDownloader::Request::Data) {
			assert(status == HTTPDownloader::HTTP_STATUS_ERROR);
			callbacks++;
		});
		downloader->WaitForAllRequests();
		{
			std::lock_guard lock(g_mutex);
			assert(callbacks == 5 && g_posts == 2);
		}

		// Review item 7: never more than 4 at once, and the connections are kept for the next requests.
		int queued_callbacks = 0;
		for (int i = 0; i < 12; i++)
			downloader->CreateRequest("https://example.invalid/ok", [&](s32 status, const std::string&, HTTPDownloader::Request::Data) {
				assert(status == 200 && std::this_thread::get_id() == owner);
				queued_callbacks++;
			});
		downloader->WaitForAllRequests();
		assert(queued_callbacks == 12);
		{
			std::lock_guard lock(g_mutex);
			assert(g_max_running <= 4 && g_max_running >= 2);
			assert(g_made <= 4);
		}

		// The downloader's own limit closes a request: one callback, TIMEOUT, and the worker is aborted.
		downloader->SetTimeout(0.05f);
		downloader->CreateRequest("https://example.invalid/slow", [&](s32 status, const std::string&, HTTPDownloader::Request::Data) {
			assert(status == HTTPDownloader::HTTP_STATUS_TIMEOUT);
			callbacks++;
		});
		downloader->WaitForAllRequests();
		assert(callbacks == 6);

		// Review item 2: a request whose abort doesn't get through. The poll that times it out returns at once.
		downloader->CreateRequest("https://example.invalid/stuck", [&](s32 status, const std::string&, HTTPDownloader::Request::Data) {
			assert(status == HTTPDownloader::HTTP_STATUS_TIMEOUT);
			callbacks++;
		});
		const double t0 = Now();
		double longest_poll = 0;
		while (downloader->HasAnyRequests())
		{
			const double p0 = Now();
			downloader->PollRequests();
			longest_poll = std::max(longest_poll, Now() - p0);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		assert(callbacks == 7);
		assert(Now() - t0 < 0.5 && longest_poll < 0.1);
		std::printf("transport: a stuck request was given up after %.0f ms; the longest poll took %.1f ms\n", (Now() - t0) * 1000,
			longest_poll * 1000);
	}
	// The destructor's wait is bounded: one stuck worker is left behind after 100 ms, and no callback runs.
	{
		auto downloader = Make(100);
		downloader->CreateRequest("https://example.invalid/stuck", [](s32, const std::string&, HTTPDownloader::Request::Data) { assert(false); });
		std::this_thread::sleep_for(std::chrono::milliseconds(50)); // the worker is in Send
		const double t0 = Now();
		downloader.reset();
		const double took = Now() - t0;
		std::printf("transport: destroyed with a stuck request in %.0f ms\n", took * 1000);
		assert(took < 0.5);
	}
	// The workers left behind finish on their own and free their connections.
	const auto settled = [] {
		std::lock_guard lock(g_mutex);
		return g_stuck_done == 2 && g_alive == 0 && g_running == 0;
	};
	for (int i = 0; i < 300 && !settled(); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	assert(settled());
	std::printf("transport: PASS\n");
	return 0;
}
