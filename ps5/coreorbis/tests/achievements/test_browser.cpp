// Real rcheevos browser with synthetic discs and a fake HTTPS server (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5/coreorbis/orbis-shims/ProsperoAchievements.h"
#include "ps5/frontend/fe_games.h"
#include "common/HTTPDownloader.h"
#include "common/MemorySettingsInterface.h"
#include "common/Console.h"
#include "pcsx2/Host.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
namespace
{
	MemorySettingsInterface secrets;
	std::mutex secret_mutex;
	bool fail_unlocks = false, fail_transport = false;
	int sessions = 0, awards = 0, badge_requests = 0;
	std::string expected_hash;
	std::string GameResponse()
	{
		std::string json = R"({"Success":true,"GameId":1,"Title":"Synthetic game","ConsoleId":21,"ImageIconUrl":"https://media.retroachievements.org/Images/1.png","Sets":[{"AchievementSetId":17,"GameId":1,"Title":"Synthetic game","Type":"core","ImageIconUrl":"https://media.retroachievements.org/Images/1.png","Achievements":[)";
		for (int i = 1; i <= 3; ++i)
		{
			if (i > 1)
				json += ',';
			json += "{\"ID\":" + std::to_string(i) + R"(,"Title":"Objective","Description":"Synthetic condition","Flags":3,"Points":5,"MemAddr":"0xH000001=1","Author":"fixture","BadgeName":"00001","Created":1,"Modified":1})";
		}
		return json + R"(],"Leaderboards":[]},{"AchievementSetId":29,"GameId":2,"Title":"Bonus set","Type":"bonus","ImageIconUrl":"https://media.retroachievements.org/Images/2.png","Achievements":[{"ID":4,"Title":"Bonus objective","Description":"Separate set","Flags":3,"Points":10,"MemAddr":"0xH000001=1","Author":"fixture","BadgeName":"00002","Created":1,"Modified":1}],"Leaderboards":[]}]})";
	}
	class Downloader final : public HTTPDownloader
	{
		Request* InternalCreateRequest() override { return new Request(); }
		void InternalPollRequests() override {}
		void CloseRequest(Request* request) override { delete request; }
		bool StartRequest(Request* request) override
		{
			std::string json;
			const auto& body = request->post_data;
			if (body.find("r=login2") != std::string::npos)
				json = R"({"Success":true,"User":"fixture","Token":"fixture-token","Score":0,"SoftcoreScore":0,"Messages":0})";
			else if (body.find("r=achievementsets") != std::string::npos)
			{
				assert(body.find(expected_hash) != std::string::npos);
				json = GameResponse();
			}
			else if (body.find("r=unlocks") != std::string::npos)
				json = fail_unlocks                          ? R"({"Success":false,"Error":"fixture failure"})" :
				       body.find("h=1") != std::string::npos ? R"({"Success":true,"UserUnlocks":[2]})" :
				                                               R"({"Success":true,"UserUnlocks":[1]})";
			else if (body.find("r=startsession") != std::string::npos)
			{
				++sessions;
				assert(false);
			}
			else if (body.find("r=awardachievement") != std::string::npos)
			{
				++awards;
				assert(false);
			}
			else
			{
				assert(request->url.starts_with("https://media.retroachievements.org/"));
				++badge_requests;
				json = "fixture-image";
			}
			request->data.assign(json.begin(), json.end());
			request->status_code = 200;
			request->state = Request::State::Complete;
			return true;
		}
	};
	void Write32(std::vector<uint8_t>& data, size_t offset, uint32_t value)
	{
		for (int i = 0; i < 4; ++i)
			data[offset + i] = value >> (8 * i);
	}
	void Record(std::vector<uint8_t>& data, size_t offset, const std::string& name, uint32_t lba, uint32_t size, bool directory)
	{
		data[offset] = 33 + name.size() + (name.size() % 2 == 0);
		Write32(data, offset + 2, lba);
		Write32(data, offset + 10, size);
		data[offset + 25] = directory ? 2 : 0;
		data[offset + 32] = name.size();
		std::memcpy(data.data() + offset + 33, name.data(), name.size());
	}
	void Disc(const std::string& path)
	{
		std::vector<uint8_t> data(25 * 2048);
		data[16 * 2048] = 1;
		std::memcpy(data.data() + 16 * 2048 + 1, "CD001", 5);
		Record(data, 16 * 2048 + 156, std::string(1, '\0'), 20, 2048, true);
		const std::string cnf = "BOOT2 = cdrom0:\\DIR\\TEST.ELF;1\r\n";
		Record(data, 20 * 2048, "SYSTEM.CNF;1", 21, cnf.size(), false);
		Record(data, 20 * 2048 + 46, "DIR", 22, 2048, true);
		std::memcpy(data.data() + 21 * 2048, cnf.data(), cnf.size());
		Record(data, 22 * 2048, "TEST.ELF;1", 23, 4, false);
		data[23 * 2048] = 7;
		data[23 * 2048 + 1] = 8;
		data[23 * 2048 + 2] = 9;
		data[23 * 2048 + 3] = 10;
		std::ofstream output(path, std::ios::binary);
		output.write(reinterpret_cast<char*>(data.data()), data.size());
	}
} // namespace
namespace Host
{
	std::unique_lock<std::mutex> GetSecretsSettingsLock() { return std::unique_lock(secret_mutex); }
	std::string GetHTTPUserAgent() { return "PS5SX2-browser-test/1.0"; }
	namespace Internal
	{
		SettingsInterface* GetSecretsSettingsLayer() { return &secrets; }
	} // namespace Internal
} // namespace Host
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
std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(std::string)
{
	if (fail_transport)
		return nullptr;
	return std::make_unique<Downloader>();
}
int main(int argc, char** argv)
{
	assert(argc == 2);
	const std::string directory = argv[1], path = directory + "/synthetic.iso";
	Disc(path);
	std::string name;
	std::vector<uint8_t> bytes;
	assert(fe::ReadAchievementExecutable(path, name, bytes));
	assert(name == "TEST.ELF" && bytes == std::vector<uint8_t>({7, 8, 9, 10}));
	expected_hash = "3fc6bce7a551fcb1898533e56625296b";
	auto service = OrbisAchievementsBrowserService(directory + "/badges");
	assert(service.load(path));
	OrbisAchievementsWaitForBrowser();
	assert(!service.state().busy && service.state().entries.empty());
	secrets.SetStringValue("Achievements", "Username", "fixture");
	secrets.SetStringValue("Achievements", "Token", "fixture-token");
	assert(service.load(path));
	OrbisAchievementsWaitForBrowser();
	const auto state = service.state();
	assert(!state.busy && state.entries.size() == 3);
	assert(state.entries[0].id == 1 && state.entries[0].unlocked);
	assert(state.entries[1].id == 2 && state.entries[1].unlocked);
	assert(state.entries[2].id == 3 && !state.entries[2].unlocked);
	assert(state.entries[0].image && state.entries[2].image);
	assert(badge_requests == 3 && sessions == 0 && awards == 0);
	assert(secrets.GetStringValue("Achievements", "Token", "") == "fixture-token");
	assert(service.load(path));
	OrbisAchievementsWaitForBrowser();
	assert(badge_requests == 3); // Cache prevents repeat badge downloads.
	fail_unlocks = true;
	assert(service.load(path));
	OrbisAchievementsWaitForBrowser();
	assert(service.state().entries.empty() && !service.state().message.empty());
	fail_unlocks = false;
	fail_transport = true;
	assert(service.load(path));
	OrbisAchievementsWaitForBrowser();
	assert(service.state().entries.empty() && !service.state().busy);
	fail_transport = false;
	std::ofstream(path, std::ios::binary | std::ios::trunc).write("invalid", 7);
	assert(!fe::ReadAchievementExecutable(path, name, bytes));
	assert(service.load(path));
	OrbisAchievementsWaitForBrowser();
	assert(service.state().entries.empty() && !service.state().busy);
	assert(service.load(path));
	service.cancel();
	OrbisAchievementsWaitForBrowser();
	assert(!service.state().busy);
}
