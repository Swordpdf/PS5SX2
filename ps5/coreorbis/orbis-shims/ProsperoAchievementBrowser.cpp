// Read-only shelf client. Never evaluates memory or submits unlocks (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProsperoAchievements.h"
#include "ps5/frontend/fe_i18n.h"
#include "common/HTTPDownloader.h"
#include "common/MD5Digest.h"
#include "pcsx2/Host.h"
#include "common/SettingsInterface.h"
#include "ps5/frontend/fe_games.h"
#include "rc_client.h"
#include "rc_api_user.h"
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>

namespace
{
	struct Browser
	{
		std::mutex mutex;
		fe::GameAchievementsState state;
		std::atomic<bool> cancel{false};
		pthread_t worker{};
		bool started = false;
		std::string cache_directory;
	};
	Browser& GetBrowser()
	{
		static Browser* browser = new Browser();
		return *browser;
	}
	void Publish(const fe::GameAchievementsState& state)
	{
		auto& browser = GetBrowser();
		std::lock_guard lock(browser.mutex);
		const auto revision = browser.state.revision + 1;
		browser.state = state;
		browser.state.revision = revision;
	}
	void ServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* userdata, rc_client_t* client)
	{
		auto* http = static_cast<HTTPDownloader*>(rc_client_get_userdata(client));
		auto finished = [callback, userdata](int status, const std::string&, HTTPDownloader::Request::Data data) {
			const size_t length = data.size();
			data.push_back(0);
			rc_api_server_response_t response{};
			response.http_status_code = status;
			response.body = reinterpret_cast<const char*>(data.data());
			response.body_length = length;
			callback(&response, userdata);
		};
		if (request->post_data)
			http->CreatePostRequest(request->url, request->post_data, std::move(finished));
		else
			http->CreateRequest(request->url, std::move(finished));
	}
	void Completed(int result, const char*, rc_client_t*, void* userdata) { *static_cast<int*>(userdata) = result; }
	void* Load(void*)
	{
		auto& browser = GetBrowser();
		fe::GameAchievementsState state;
		{
			std::lock_guard lock(browser.mutex);
			state = browser.state;
		}
		auto work = [&] {
			std::string username, token;
			{
				auto lock = Host::GetSecretsSettingsLock();
				auto* secrets = Host::Internal::GetSecretsSettingsLayer();
				if (secrets)
				{
					username = secrets->GetStringValue("Achievements", "Username", "");
					token = secrets->GetStringValue("Achievements", "Token", "");
				}
			}
			if (username.empty() || token.empty())
			{
				state.message = fe::Tr(fe::Str::AchievementShelfSignIn);
				return;
			}
			std::string name;
			std::vector<uint8_t> executable;
			if (!fe::ReadAchievementExecutable(state.path, name, executable))
			{
				state.message = fe::Tr(fe::Str::AchievementBootFailed);
				return;
			}
			if (browser.cancel)
				return;
			MD5Digest digest;
			digest.Update(name.data(), static_cast<uint32_t>(name.size()));
			digest.Update(executable.data(), static_cast<uint32_t>(executable.size()));
			executable.clear();
			executable.shrink_to_fit();
			uint8_t md5[16];
			digest.Final(md5);
			char hash[33];
			for (int i = 0; i < 16; ++i)
				std::snprintf(hash + 2 * i, 3, "%02x", md5[i]);
			auto http = HTTPDownloader::Create(Host::GetHTTPUserAgent());
			if (!http)
			{
				state.message = fe::Tr(fe::Str::AchievementSecureFailed);
				return;
			}
			http->SetTimeout(10);
			http->SetMaxActiveRequests(4);
			auto deleter = [](rc_client_t* client) { rc_client_destroy(client); };
			std::unique_ptr<rc_client_t, decltype(deleter)> client(rc_client_create([](uint32_t, uint8_t* buffer, uint32_t size, rc_client_t*) -> uint32_t {
				// Supply only validation bytes. This client never runs do_frame.
				std::memset(buffer, 0, size);
				return size;
			},
																	   ServerCall),
				deleter);
			if (!client)
			{
				state.message = fe::Tr(fe::Str::AchievementCreateFailed);
				return;
			}
			rc_client_set_userdata(client.get(), http.get());
			rc_client_set_hardcore_enabled(client.get(), 0);
			rc_client_set_spectator_mode_enabled(client.get(), 1);
			int result = -1;
			rc_client_begin_login_with_token(client.get(), username.c_str(), token.c_str(), Completed, &result);
			http->WaitForAllRequests();
			std::fill(token.begin(), token.end(), '\0');
			if (result != RC_OK)
			{
				state.message = fe::Tr(fe::Str::AchievementConnectFailed);
				return;
			}
			if (browser.cancel)
				return;
			result = -1;
			rc_client_begin_load_game(client.get(), hash, Completed, &result);
			http->WaitForAllRequests();
			if (result != RC_OK)
			{
				state.message = fe::Tr(fe::Str::AchievementSetFailed);
				return;
			}
			if (browser.cancel)
				return;
			if (const auto* game = rc_client_get_game_info(client.get()))
				state.title = game->title ? game->title : fe::Tr(fe::Str::Achievements);
			auto list_deleter = [](rc_client_achievement_list_t* list) { rc_client_destroy_achievement_list(list); };
			std::unique_ptr<rc_client_achievement_list_t, decltype(list_deleter)> list(
				rc_client_create_achievement_list(client.get(), RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
					RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE),
				list_deleter);
			if (!list)
			{
				state.message = fe::Tr(fe::Str::AchievementListFailed);
				return;
			}
			// Spectator clients skip startsession and therefore don't receive unlocks.
			// Query the read-only API explicitly; otherwise every entry would look locked.
			const auto* game = rc_client_get_game_info(client.get());
			const auto* user = rc_client_get_user_info(client.get());
			if (!game || !user)
			{
				state.message = fe::Tr(fe::Str::AchievementAccountMissing);
				return;
			}
			std::unordered_set<uint32_t> unlocked;
			bool unlocks_ok = true;
			for (uint32_t hardcore = 0; hardcore < 2; ++hardcore)
			{
				rc_api_fetch_user_unlocks_request_t parameters{};
				parameters.username = user->username;
				parameters.api_token = user->token;
				parameters.game_id = game->id;
				parameters.hardcore = hardcore;
				rc_api_request_t request{};
				if (rc_api_init_fetch_user_unlocks_request(&request, &parameters) != RC_OK)
				{
					rc_api_destroy_request(&request);
					unlocks_ok = false;
					break;
				}
				auto finished = [&](int status, const std::string&, HTTPDownloader::Request::Data data) {
					const size_t length = data.size();
					data.push_back(0);
					rc_api_server_response_t server{};
					server.http_status_code = status;
					server.body_length = length;
					server.body = reinterpret_cast<const char*>(data.data());
					rc_api_fetch_user_unlocks_response_t response{};
					if (status != 200 || rc_api_process_fetch_user_unlocks_server_response(&response, &server) != RC_OK ||
						!response.response.succeeded)
						unlocks_ok = false;
					else
						for (uint32_t i = 0; i < response.num_achievement_ids; ++i)
							unlocked.insert(response.achievement_ids[i]);
					rc_api_destroy_fetch_user_unlocks_response(&response);
				};
				if (request.post_data)
					http->CreatePostRequest(request.url, request.post_data, std::move(finished));
				else
					http->CreateRequest(request.url, std::move(finished));
				rc_api_destroy_request(&request);
			}
			http->WaitForAllRequests();
			if (!unlocks_ok)
			{
				state.message = fe::Tr(fe::Str::AchievementUnlockFailed);
				return;
			}
			if (browser.cancel)
				return;
			// Browse the main set only. Bonus sets may have separate legacy game IDs
			// and require their own unlock queries before they can be shown correctly.
			auto subsets_deleter = [](rc_client_subset_list_t* subsets) { rc_client_destroy_subset_list(subsets); };
			std::unique_ptr<rc_client_subset_list_t, decltype(subsets_deleter)> subsets(
				rc_client_create_subset_list(client.get()), subsets_deleter);
			if (!subsets || !subsets->num_subsets)
			{
				state.message = fe::Tr(fe::Str::AchievementMainFailed);
				return;
			}
			const uint32_t main_subset = subsets->subsets[0]->id;
			for (uint32_t bucket = 0; bucket < list->num_buckets; ++bucket)
			{
				if (list->buckets[bucket].subset_id && list->buckets[bucket].subset_id != main_subset)
					continue;
				for (uint32_t item = 0; item < list->buckets[bucket].num_achievements; ++item)
				{
					const auto* achievement = list->buckets[bucket].achievements[item];
					fe::GameAchievement entry;
					entry.id = achievement->id;
					entry.points = achievement->points;
					entry.unlocked = unlocked.contains(entry.id);
					entry.title = achievement->title ? achievement->title : "";
					entry.description = achievement->description ? achievement->description : "";
					char url[1024]{};
					rc_client_achievement_get_image_url(achievement,
						entry.unlocked ? RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED : RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE,
						url, sizeof(url));
					entry.image_url = url;
					state.entries.push_back(std::move(entry));
				}
			}
			std::stable_sort(state.entries.begin(), state.entries.end(), [](const auto& a, const auto& b) {
				return a.unlocked && !b.unlocked;
			});
			std::printf("[achievements-browser] game %u: %zu achievements, %zu account unlock IDs\n",
				game->id, state.entries.size(), unlocked.size());
			std::fflush(stdout);
			state.message = state.entries.empty() ? fe::Tr(fe::Str::AchievementEmpty) : fe::Tr(fe::Str::AchievementImages);
			Publish(state);
			const std::string cache = browser.cache_directory;
			mkdir(cache.c_str(), 0700);
			size_t image_bytes = 0;
			for (size_t i = 0; i < state.entries.size() && !browser.cancel && image_bytes < 16 * 1024 * 1024; ++i)
			{
				const auto& entry = state.entries[i];
				const std::string path = cache + "/" + std::to_string(entry.id) + (entry.unlocked ? ".png" : "-locked.png");
				std::vector<uint8_t> image;
				std::ifstream input(path, std::ios::binary | std::ios::ate);
				if (input && input.tellg() > 0 && input.tellg() <= 256 * 1024)
				{
					image.resize(static_cast<size_t>(input.tellg()));
					input.seekg(0);
					if (!input.read(reinterpret_cast<char*>(image.data()), image.size()))
						image.clear();
				}
				if (image.empty() && entry.image_url.starts_with("https://"))
				{
					http->CreateRequest(entry.image_url, [&](int status, const std::string&, HTTPDownloader::Request::Data data) {
						if (status == 200 && data.size() <= 256 * 1024)
							image = std::move(data);
					});
					http->WaitForAllRequests();
					if (!image.empty())
					{
						const std::string temporary = path + ".tmp";
						std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
						output.write(reinterpret_cast<const char*>(image.data()), image.size());
						output.close();
						if (output)
							std::rename(temporary.c_str(), path.c_str());
						else
							std::remove(temporary.c_str());
					}
				}
				image_bytes += image.size();
				if (!image.empty())
					state.entries[i].image = std::make_shared<const std::vector<uint8_t>>(std::move(image));
				Publish(state);
			}
			state.message = "";
		};
		work();
		state.busy = false;
		if (browser.cancel && state.entries.empty())
			state.message = fe::Tr(fe::Str::AchievementCancelled);
		Publish(state);
		return nullptr;
	}
} // namespace
fe::GameAchievementsService OrbisAchievementsBrowserService(const std::string& cache_directory)
{
	{
		auto& b = GetBrowser();
		std::lock_guard lock(b.mutex);
		if (!b.state.busy)
			b.cache_directory = cache_directory;
	}
	return {[] { auto& b = GetBrowser(); std::lock_guard lock(b.mutex); return b.state; },
		[](const std::string& path) {
			auto& b = GetBrowser();
			{
				std::lock_guard lock(b.mutex);
				if (b.state.busy)
					return false;
			}
			OrbisAchievementsWaitForBrowser();
			b.cancel = false;
			fe::GameAchievementsState state;
			state.path = path;
			state.busy = true;
			state.message = fe::Tr(fe::Str::AchievementLoading);
			Publish(state);
			pthread_attr_t attr;
			pthread_attr_init(&attr);
			pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
			b.started = pthread_create(&b.worker, &attr, Load, nullptr) == 0;
			pthread_attr_destroy(&attr);
			if (!b.started)
			{
				state.busy = false;
				state.message = fe::Tr(fe::Str::AchievementStartFailed);
				Publish(state);
			}
			return b.started;
		},
		[] { GetBrowser().cancel = true; }};
}
void OrbisAchievementsWaitForBrowser()
{
	auto& b = GetBrowser();
	if (b.started)
	{
		pthread_join(b.worker, nullptr);
		b.started = false;
	}
}
