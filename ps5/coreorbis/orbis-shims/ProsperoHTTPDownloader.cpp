// PS5SX2 RetroAchievements transport (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/HTTPDownloader.h"
#include "RetroAchievementsCA.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <pthread.h>

struct ProsperoSslData
{
	const void* ptr;
	size_t size;
};

extern "C" {
int sceNetInit();
int sceNetCtlInit();
int sceNetCtlTerm();
int sceNetCtlGetState(int*);
int sceNetPoolCreate(const char*, int, int);
int sceNetPoolDestroy(int);
int sceSslInit(size_t);
int sceSslTerm(int);
int sceSslLoadCert(int, int, const ProsperoSslData* const*, const ProsperoSslData*, const ProsperoSslData*);
int sceSslUnloadCert(int);
int sceHttp2Init(int, int, size_t, int);
int sceHttp2Term(int);
int sceHttp2CreateTemplate(int, const char*, int, int);
int sceHttp2DeleteTemplate(int);
int sceHttp2CreateRequestWithURL(int, const char*, const char*, uint64_t);
int sceHttp2DeleteRequest(int);
int sceHttp2SendRequest(int, const void*, size_t);
int sceHttp2GetStatusCode(int, int*);
int sceHttp2ReadData(int, void*, size_t);
int sceHttp2AbortRequest(int);
int sceHttp2SetConnectTimeOut(int, uint32_t);
int sceHttp2SetSendTimeOut(int, uint32_t);
int sceHttp2SetRecvTimeOut(int, uint32_t);
int sceHttp2SetTimeOut(int, uint32_t);
int sceHttp2AddRequestHeader(int, const char*, const char*, int);
}

namespace
{
	class ProsperoHTTPDownloader final : public HTTPDownloader
	{
	public:
		~ProsperoHTTPDownloader() override
		{
			// Join before releasing contexts or callback data. No callbacks during destruction.
			for (Request* request : m_pending_http_requests)
				CloseRequest(request);
			m_pending_http_requests.clear();
			if (m_template >= 0)
				sceHttp2DeleteTemplate(m_template);
			if (m_context >= 0)
				sceHttp2Term(m_context);
			if (m_ca_loaded)
				sceSslUnloadCert(m_ssl);
			if (m_ssl >= 0)
				sceSslTerm(m_ssl);
			if (m_pool >= 0)
				sceNetPoolDestroy(m_pool);
			if (m_netctl)
				sceNetCtlTerm();
		}

		bool Init(const std::string& user_agent)
		{
			// Cover downloads may never initialize NetCtl when every cover is local.
			const int netctl = sceNetCtlInit();
			m_netctl = netctl == 0;
			int network_state[4] = {-1, 0, 0, 0};
			const int state_result = sceNetCtlGetState(network_state);
			std::printf("[achievements-http] netctl %#x, network state %d (%#x)\n",
				static_cast<unsigned>(netctl), network_state[0], static_cast<unsigned>(state_result));
			if (state_result == 0 && network_state[0] >= 0 && network_state[0] < 3)
				return false;
			sceNetInit(); // May already be initialized by the frontend.
			m_pool = sceNetPoolCreate("pcsx2-achievements", 128 * 1024, 0);
			m_ssl = m_pool >= 0 ? sceSslInit(512 * 1024) : -1;
			if (m_ssl < 0)
				return false;
			// The console rejected RA's chain, then rejected DER certificate loading.
			// Try the public root in PEM format; native format support needs testing.
			// Add its verified public root to this private context; keep hostname,
			// validity and signature verification enabled. Native ABI needs proper testing.
			const int ca_result = sceSslLoadCert(m_ssl, 1, m_ca_list, nullptr, nullptr);
			m_ca_loaded = ca_result == 0;
			std::printf("[achievements-http] GTS Root R4 load: %#x\n", static_cast<unsigned>(ca_result));
			std::fflush(stdout);
			if (!m_ca_loaded)
				return false;
			m_context = m_ssl >= 0 ? sceHttp2Init(m_pool, m_ssl, 512 * 1024, 4) : -1;
			m_template = m_context >= 0 ? sceHttp2CreateTemplate(m_context, user_agent.c_str(), 3, 1) : -1;
			std::printf("[achievements-http] pool %#x, ssl %#x, http2 %#x, template %#x\n",
				static_cast<unsigned>(m_pool), static_cast<unsigned>(m_ssl), static_cast<unsigned>(m_context),
				static_cast<unsigned>(m_template));
			std::fflush(stdout);
			// Use the system's TLS validation defaults. Never disable certificate checks.
			SetMaxActiveRequests(4);
			return m_template >= 0;
		}

	private:
		struct Job : Request
		{
			pthread_t thread{};
			bool started = false;
			bool stopping = false;
			int native_request = -1;
			uint32_t timeout_us = 0;
			std::mutex native_lock;
			std::atomic<bool> finished{false};
			Data result_data;
			int result_status = HTTP_STATUS_ERROR;
		};

		Request* InternalCreateRequest() override { return new Job(); }
		void InternalPollRequests() override
		{
			for (Request* request : m_pending_http_requests)
			{
				auto* job = static_cast<Job*>(request);
				if (job->state == Request::State::Started && job->finished.load(std::memory_order_acquire))
				{
					job->status_code = job->result_status;
					job->data = std::move(job->result_data);
					job->state = Request::State::Complete;
				}
			}
		}

		bool StartRequest(Request* request) override
		{
			auto* job = static_cast<Job*>(request);
			job->timeout_us = static_cast<uint32_t>(std::clamp(m_timeout, 1.0f, 60.0f) * 1000000.0f);
			job->state = Request::State::Started;
			pthread_attr_t attr;
			pthread_attr_init(&attr);
			pthread_attr_setstacksize(&attr, 512 * 1024);
			job->started = pthread_create(&job->thread, &attr, Worker, job) == 0;
			pthread_attr_destroy(&attr);
			if (!job->started)
			{
				job->status_code = HTTP_STATUS_ERROR;
				job->state = Request::State::Complete;
			}
			// Even startup failures go through PollRequests, with exactly one callback.
			return true;
		}

		void CloseRequest(Request* request) override
		{
			auto* job = static_cast<Job*>(request);
			{
				std::lock_guard lock(job->native_lock);
				job->stopping = true;
				if (job->native_request >= 0)
					sceHttp2AbortRequest(job->native_request);
			}
			if (job->started)
				pthread_join(job->thread, nullptr);
			delete job;
		}

		static void* Worker(void* userdata)
		{
			auto* job = static_cast<Job*>(userdata);
			auto* self = static_cast<ProsperoHTTPDownloader*>(job->parent);
			Request::Data data;
			int status = HTTP_STATUS_ERROR;
			int req = -1;
			// Do not send credentials over HTTP if a custom host is misconfigured.
			if (job->url.starts_with("https://"))
			{
				std::lock_guard lock(job->native_lock);
				if (!job->stopping)
				{
					const bool post = job->type == Request::Type::Post;
					req = sceHttp2CreateRequestWithURL(self->m_template, post ? "POST" : "GET", job->url.c_str(),
						post ? job->post_data.size() : 0);
					job->native_request = req;
				}
			}
			if (req >= 0)
			{
				const auto started_at = std::chrono::steady_clock::now();
				auto failure_status = [&] {
					return std::chrono::steady_clock::now() - started_at >= std::chrono::microseconds(job->timeout_us) ?
					           HTTP_STATUS_TIMEOUT :
					           HTTP_STATUS_ERROR;
				};
				const bool post = job->type == Request::Type::Post;
				// ABI and timeout behaviour need proper testing on the console SDK.
				// Keep the resolver's system defaults: its setter rejected the assumed
				// millisecond value on the console. Overall request deadlines still apply.
				const int connect = sceHttp2SetConnectTimeOut(req, job->timeout_us);
				const int send_timeout = sceHttp2SetSendTimeOut(req, job->timeout_us);
				const int receive_timeout = sceHttp2SetRecvTimeOut(req, job->timeout_us);
				const int total_timeout = sceHttp2SetTimeOut(req, job->timeout_us);
				const int header = post ? sceHttp2AddRequestHeader(req, "Content-Type", "application/x-www-form-urlencoded", 0) : 0;
				const bool configured = connect == 0 && send_timeout == 0 &&
				                        receive_timeout == 0 && total_timeout == 0 && header == 0;
				if (!configured)
					std::printf("[achievements-http] configuration: connect %#x send %#x recv %#x total %#x header %#x\n",
						static_cast<unsigned>(connect), static_cast<unsigned>(send_timeout),
						static_cast<unsigned>(receive_timeout), static_cast<unsigned>(total_timeout), static_cast<unsigned>(header));
				const int sent = configured ? sceHttp2SendRequest(req, post ? job->post_data.data() : nullptr,
												  post ? job->post_data.size() : 0) :
				                              -1;
				const int received = sent == 0 ? sceHttp2GetStatusCode(req, &status) : -1;
				std::printf("[achievements-http] %s: send %#x, status %d (%#x), elapsed %lld ms\n", post ? "POST" : "GET",
					static_cast<unsigned>(sent), status, static_cast<unsigned>(received),
					static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now() - started_at)
							.count()));
				std::fflush(stdout);
				if (sent == 0 && received == 0)
				{
					std::array<u8, 16384> buffer;
					for (;;)
					{
						const int size = sceHttp2ReadData(req, buffer.data(), buffer.size());
						if (size == 0)
							break;
						if (size < 0 || size > static_cast<int>(buffer.size()) || data.size() + size > 16 * 1024 * 1024)
						{
							status = size < 0 ? failure_status() : HTTP_STATUS_ERROR;
							data.clear();
							break;
						}
						data.insert(data.end(), buffer.begin(), buffer.begin() + size);
					}
				}
				else
				{
					// Log native result codes only, never a URL, POST payload or response body.
					std::printf("[achievements-http] configured %d, send %#x, status %#x\n", configured,
						static_cast<unsigned>(sent), static_cast<unsigned>(received));
					status = failure_status();
				}
				{
					std::lock_guard lock(job->native_lock);
					job->native_request = -1;
					sceHttp2DeleteRequest(req);
				}
			}
			else
			{
				std::printf("[achievements-http] request creation failed: %#x\n", static_cast<unsigned>(req));
				std::fflush(stdout);
			}
			// Publish privately; only InternalPollRequests touches the common Request payload.
			// WaitForAllRequests holds the pending lock while waiting, so workers never acquire it.
			job->result_status = status;
			job->result_data = std::move(data);
			job->finished.store(true, std::memory_order_release);
			return nullptr;
		}

		int m_pool = -1, m_ssl = -1, m_context = -1, m_template = -1;
		bool m_netctl = false;
		bool m_ca_loaded = false;
		const ProsperoSslData m_ca{OrbisAchievementsTrust::GoogleRootR4, sizeof(OrbisAchievementsTrust::GoogleRootR4) - 1};
		const ProsperoSslData* m_ca_list[1]{&m_ca};
	};
} // namespace

std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(std::string user_agent)
{
	auto downloader = std::make_unique<ProsperoHTTPDownloader>();
	if (!downloader->Init(user_agent))
		return nullptr;
	return downloader;
}
