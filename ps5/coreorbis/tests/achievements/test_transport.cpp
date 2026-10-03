// Exercises the real downloader against a simulated libSceHttp2 (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/HTTPDownloader.h"
#include "common/Console.h"
#include "ps5/coreorbis/orbis-shims/RetroAchievementsCA.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <thread>

namespace
{
	struct NativeRequest
	{
		std::string url, method, body, content_type;
		bool aborted = false;
		size_t offset = 0;
	};
	std::mutex native_mutex;
	std::condition_variable wake;
	std::map<int, NativeRequest> requests;
	int next_request = 10;
	int live_contexts = 0;
	int sent_posts = 0;
	int network_state = 3;
	int loaded_ca_contexts = 0;
	bool reject_ca = false;
	std::thread::id owner;
} // namespace

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

struct ProsperoSslData
{
	const void* ptr;
	size_t size;
};

extern "C" {
int sceNetInit() { return 0; }
int sceNetCtlInit()
{
	++live_contexts;
	return 0;
}
int sceNetCtlTerm()
{
	--live_contexts;
	return 0;
}
int sceNetCtlGetState(int* state)
{
	*state = network_state;
	return 0;
}
int sceNetPoolCreate(const char*, int, int) { return ++live_contexts; }
int sceNetPoolDestroy(int)
{
	--live_contexts;
	return 0;
}
int sceSslInit(size_t) { return ++live_contexts; }
int sceSslTerm(int)
{
	--live_contexts;
	return 0;
}
int sceSslLoadCert(int, int count, const ProsperoSslData* const* certificates,
	const ProsperoSslData* client_certificate, const ProsperoSslData* private_key)
{
	assert(count == 1 && !client_certificate && !private_key);
	assert(certificates[0]->size == sizeof(OrbisAchievementsTrust::GoogleRootR4) - 1);
	assert(std::memcmp(certificates[0]->ptr, OrbisAchievementsTrust::GoogleRootR4, certificates[0]->size) == 0);
	if (reject_ca)
		return -1;
	++loaded_ca_contexts;
	return 0;
}
int sceSslUnloadCert(int)
{
	--loaded_ca_contexts;
	return 0;
}
int sceHttp2Init(int, int, size_t, int) { return ++live_contexts; }
int sceHttp2Term(int)
{
	--live_contexts;
	return 0;
}
int sceHttp2CreateTemplate(int, const char* agent, int, int)
{
	assert(std::string(agent) == "test/1.0");
	return ++live_contexts;
}
int sceHttp2DeleteTemplate(int)
{
	--live_contexts;
	return 0;
}
int sceHttp2CreateRequestWithURL(int, const char* method, const char* url, uint64_t)
{
	std::lock_guard lock(native_mutex);
	const int id = next_request++;
	requests[id] = NativeRequest{url, method, "", ""};
	return id;
}
int sceHttp2DeleteRequest(int id)
{
	std::lock_guard lock(native_mutex);
	assert(requests.erase(id) == 1);
	return 0;
}
int sceHttp2SetResolveTimeOut(int, uint32_t)
{
	assert(false); // Use the console's default resolver settings; do not guess the ABI.
	return 0;
}
int sceHttp2SetConnectTimeOut(int, uint32_t) { return 0; }
int sceHttp2SetSendTimeOut(int, uint32_t) { return 0; }
int sceHttp2SetRecvTimeOut(int, uint32_t) { return 0; }
int sceHttp2SetTimeOut(int, uint32_t) { return 0; }
int sceHttp2AddRequestHeader(int id, const char* name, const char* value, int)
{
	std::lock_guard lock(native_mutex);
	assert(std::string(name) == "Content-Type");
	requests.at(id).content_type = value;
	return 0;
}
int sceHttp2AbortRequest(int id)
{
	std::lock_guard lock(native_mutex);
	requests.at(id).aborted = true;
	wake.notify_all();
	return 0;
}
int sceHttp2SendRequest(int id, const void* data, size_t size)
{
	std::unique_lock lock(native_mutex);
	auto& request = requests.at(id);
	if (request.method == "POST")
	{
		assert(request.content_type == "application/x-www-form-urlencoded");
		request.body.assign(static_cast<const char*>(data), size);
		assert(request.body == "u=test&p=synthetic");
		sent_posts++;
	}
	if (request.url.ends_with("slow"))
	{
		assert(wake.wait_for(lock, std::chrono::seconds(2), [&] { return request.aborted; }));
		return -1;
	}
	return 0;
}
int sceHttp2GetStatusCode(int id, int* status)
{
	std::lock_guard lock(native_mutex);
	*status = requests.at(id).url.ends_with("error") ? 401 : 200;
	return 0;
}
int sceHttp2ReadData(int id, void* data, size_t capacity)
{
	std::lock_guard lock(native_mutex);
	auto& request = requests.at(id);
	const std::string body = request.url.ends_with("error") ? "invalid account" : "success";
	if (request.offset == body.size())
		return 0;
	const size_t size = std::min(capacity, body.size() - request.offset);
	memcpy(data, body.data() + request.offset, size);
	request.offset += size;
	return static_cast<int>(size);
}
}

int main()
{
	owner = std::this_thread::get_id();
	{
		auto downloader = HTTPDownloader::Create("test/1.0");
		assert(downloader);
		int callbacks = 0;
		downloader->CreatePostRequest("https://example.invalid/ok", "u=test&p=synthetic",
			[&](s32 status, const std::string&, HTTPDownloader::Request::Data data) {
				assert(std::this_thread::get_id() == owner);
				assert(status == 200 && std::string(data.begin(), data.end()) == "success");
				callbacks++;
			});
		downloader->CreateRequest("https://example.invalid/error",
			[&](s32 status, const std::string&, HTTPDownloader::Request::Data data) {
				assert(status == 401 && std::string(data.begin(), data.end()) == "invalid account");
				callbacks++;
			});
		downloader->CreatePostRequest("http://example.invalid/blocked", "u=test&p=synthetic",
			[&](s32 status, const std::string&, HTTPDownloader::Request::Data) {
				assert(status == HTTPDownloader::HTTP_STATUS_ERROR);
				callbacks++;
			});
		downloader->WaitForAllRequests();
		assert(callbacks == 3 && sent_posts == 1);
		assert(requests.empty());
		int queued_callbacks = 0;
		for (int i = 0; i < 8; i++)
			downloader->CreateRequest("https://example.invalid/ok",
				[&](s32 status, const std::string&, HTTPDownloader::Request::Data) {
					assert(status == 200 && std::this_thread::get_id() == owner);
					queued_callbacks++;
				});
		downloader->WaitForAllRequests();
		assert(queued_callbacks == 8 && requests.empty());
		downloader->SetTimeout(0.02f);
		downloader->CreateRequest("https://example.invalid/slow",
			[&](s32 status, const std::string&, HTTPDownloader::Request::Data) {
				assert(status == HTTPDownloader::HTTP_STATUS_TIMEOUT);
				callbacks++;
			});
		downloader->WaitForAllRequests();
		assert(callbacks == 4 && requests.empty());
		downloader->CreateRequest("https://example.invalid/slow",
			[](s32, const std::string&, HTTPDownloader::Request::Data) { assert(false); });
		// Destruction aborts and joins without invoking a callback on a destroyed RA client.
	}
	assert(requests.empty() && live_contexts == 0 && loaded_ca_contexts == 0);
	reject_ca = true;
	assert(!HTTPDownloader::Create("test/1.0"));
	assert(live_contexts == 0 && loaded_ca_contexts == 0);
	reject_ca = false;
	network_state = 2;
	assert(!HTTPDownloader::Create("test/1.0"));
	assert(live_contexts == 0); // Failed initialization releases its NetCtl reference.
}
