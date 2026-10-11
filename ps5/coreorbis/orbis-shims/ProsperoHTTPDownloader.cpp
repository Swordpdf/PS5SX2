// PS5SX2 RetroAchievements transport (AI-assisted). See ProsperoHTTPDownloader.h.
//
// pr9n (2026-10-05), after the PR #9 review (the first version ran libSceHttp2 with one pinned root):
//   - every request runs on a worker thread of its own; the worker and the request share the job's state, so a closed
//     request (timed out, cancelled, or the downloader going away) is aborted and simply left to its worker. Nothing
//     here joins a thread: CloseRequest runs on the EE thread inside FrameUpdate with the achievements lock held, and
//     an abort that can't interrupt a DNS lookup or a connect used to freeze the game there;
//   - a request that got no HTTP status at all is HTTP_STATUS_TIMEOUT, which ClientServerCall makes retryable, so an
//     unlock sent during a Wi-Fi blip is retried instead of dropped;
//   - at most 4 requests at once, the connections kept for the next ones;
//   - the destructor waits at most 1.5 s for the workers; the ones left keep what they use alive.
// Never logged: a URL, a POST body or an answer (they carry the password or the token). Needs proper testing.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProsperoHTTPDownloader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <pthread.h>

namespace OrbisAchievementsHttp
{
	namespace
	{
		constexpr u32 kMaxConnections = 4;
		constexpr size_t kWorkerStack = 512 * 1024;

		// What the downloader and its workers share; a worker may outlive the downloader.
		struct Core
		{
			ConnectionFactory factory;
			std::mutex mutex;
			std::condition_variable workers_done;
			std::vector<std::unique_ptr<Connection>> idle; // connections for the next requests
			int running = 0; // workers alive

			std::unique_ptr<Connection> Take()
			{
				{
					std::lock_guard lock(mutex);
					if (!idle.empty())
					{
						std::unique_ptr<Connection> c = std::move(idle.back());
						idle.pop_back();
						return c;
					}
				}
				return factory ? factory() : nullptr;
			}
			void Give(std::unique_ptr<Connection> c)
			{
				if (!c)
					return;
				std::lock_guard lock(mutex);
				if (idle.size() < kMaxConnections)
					idle.push_back(std::move(c));
			}
		};

		// One request's state, shared by the request and its worker.
		struct JobState
		{
			std::mutex mutex;
			Connection* active = nullptr; // while Send runs
			bool abandoned = false; // the request was closed
			std::atomic<bool> finished{false};
			s32 status = HTTPDownloader::HTTP_STATUS_ERROR; // the worker's, published by `finished`
			HTTPDownloader::Request::Data data;
		};

		struct WorkerArgs
		{
			std::shared_ptr<Core> core;
			std::shared_ptr<JobState> job;
			std::string method, url, body, content_type;
			int timeout_ms = 30000;
		};

		const char* Describe(int r)
		{
			return r == kNoAnswer ? "no answer (retried)" : r == kRefused ? "refused (not retried)" : r == kAborted ? "aborted" : "";
		}

		void* Worker(void* arg)
		{
			std::unique_ptr<WorkerArgs> a(static_cast<WorkerArgs*>(arg));
			JobState& job = *a->job;
			s32 status = HTTPDownloader::HTTP_STATUS_CANCELLED;
			HTTPDownloader::Request::Data data;
			std::unique_ptr<Connection> connection;
			bool run;
			{
				std::lock_guard lock(job.mutex);
				run = !job.abandoned;
			}
			if (run)
			{
				connection = a->core->Take();
				if (!connection)
				{
					status = HTTPDownloader::HTTP_STATUS_ERROR;
					std::printf("[achievements-http] %s: no HTTPS connection\n", a->method.c_str());
				}
				else
				{
					{
						std::lock_guard lock(job.mutex);
						run = !job.abandoned;
						if (run)
							job.active = connection.get();
					}
					if (run)
					{
						const auto t0 = std::chrono::steady_clock::now();
						const int r = connection->Send(a->method, a->url, a->body, a->content_type, data, a->timeout_ms);
						{
							std::lock_guard lock(job.mutex);
							job.active = nullptr;
						}
						if (r >= 100)
							status = r;
						else
						{
							status = r == kNoAnswer ? HTTPDownloader::HTTP_STATUS_TIMEOUT :
									 r == kAborted  ? HTTPDownloader::HTTP_STATUS_CANCELLED :
													  HTTPDownloader::HTTP_STATUS_ERROR;
							data.clear();
						}
						std::printf("[achievements-http] %s: %d%s%s (%lld ms)\n", a->method.c_str(), r, r >= 100 ? "" : " ",
							r >= 100 ? "" : Describe(r),
							static_cast<long long>(
								std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
						std::fflush(stdout);
					}
				}
			}
			job.status = status;
			job.data = std::move(data);
			job.finished.store(true, std::memory_order_release);
			a->core->Give(std::move(connection));
			{
				std::lock_guard lock(a->core->mutex);
				a->core->running--;
			}
			a->core->workers_done.notify_all();
			return nullptr;
		}

		class Downloader final : public HTTPDownloader
		{
		public:
			Downloader(ConnectionFactory factory, int shutdown_wait_ms)
				: m_core(std::make_shared<Core>())
				, m_shutdown_wait_ms(shutdown_wait_ms)
			{
				m_core->factory = std::move(factory);
				SetMaxActiveRequests(kMaxConnections);
			}

			~Downloader() override
			{
				// No callbacks now: every request still here is closed (aborted and left to its worker).
				for (Request* request : m_pending_http_requests)
					CloseRequest(request);
				m_pending_http_requests.clear();
				std::unique_lock lock(m_core->mutex);
				if (!m_core->workers_done.wait_for(lock, std::chrono::milliseconds(std::max(0, m_shutdown_wait_ms)),
						[&] { return m_core->running == 0; }))
				{
					std::printf("[achievements-http] %d request(s) still running after %d ms: left to finish on their own\n",
						m_core->running, m_shutdown_wait_ms);
					std::fflush(stdout);
				}
			}

		private:
			struct Job : Request
			{
				std::shared_ptr<JobState> shared = std::make_shared<JobState>();
			};

			Request* InternalCreateRequest() override { return new Job(); }

			void InternalPollRequests() override
			{
				for (Request* request : m_pending_http_requests)
				{
					auto* job = static_cast<Job*>(request);
					if (job->state == Request::State::Started && job->shared->finished.load(std::memory_order_acquire))
					{
						job->status_code = job->shared->status;
						job->data = std::move(job->shared->data);
						job->state = Request::State::Complete;
					}
				}
			}

			bool StartRequest(Request* request) override
			{
				auto* job = static_cast<Job*>(request);
				job->state = Request::State::Started;
				auto args = std::make_unique<WorkerArgs>();
				args->core = m_core;
				args->job = job->shared;
				const bool post = job->type == Request::Type::Post;
				args->method = post ? "POST" : "GET";
				args->url = job->url;
				if (post)
				{
					args->body = job->post_data;
					args->content_type = "application/x-www-form-urlencoded";
				}
				// Just inside the downloader's own limit, so the worker's answer is the one reported.
				args->timeout_ms = std::max(1000, static_cast<int>(std::clamp(m_timeout, 1.0f, 600.0f) * 1000.0f) - 250);
				{
					std::lock_guard lock(m_core->mutex);
					m_core->running++;
				}
				pthread_attr_t attr;
				pthread_attr_init(&attr);
				pthread_attr_setstacksize(&attr, kWorkerStack);
				pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
				pthread_t thread;
				const bool started = pthread_create(&thread, &attr, Worker, args.get()) == 0;
				pthread_attr_destroy(&attr);
				if (started)
					args.release(); // the worker's now
				else
				{
					{
						std::lock_guard lock(m_core->mutex);
						m_core->running--;
					}
					std::printf("[achievements-http] couldn't start a request thread\n");
					job->shared->status = HTTP_STATUS_ERROR;
					job->shared->finished.store(true, std::memory_order_release);
				}
				// Even a start that failed goes through PollRequests, with exactly one callback.
				return true;
			}

			void CloseRequest(Request* request) override
			{
				auto* job = static_cast<Job*>(request);
				{
					std::lock_guard lock(job->shared->mutex);
					job->shared->abandoned = true;
					if (job->shared->active)
						job->shared->active->Abort();
				}
				delete job; // the worker keeps the shared state; never joined here
			}

			std::shared_ptr<Core> m_core;
			int m_shutdown_wait_ms;
		};
	} // namespace

	std::unique_ptr<HTTPDownloader> Create(ConnectionFactory factory, int shutdown_wait_ms)
	{
		return std::make_unique<Downloader>(std::move(factory), shutdown_wait_ms);
	}
} // namespace OrbisAchievementsHttp
