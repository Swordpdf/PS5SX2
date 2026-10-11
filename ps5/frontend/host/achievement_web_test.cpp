// In-game web API checks using a mutable runtime fixture (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../fe_web.h"
#include "../fe_i18n.h"
#include <fstream>
#include <unordered_set>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <string>
#include <vector>
#include <atomic>
using namespace fe;
std::string Request(uint16_t port, const std::string& token, const std::string& target, const char* method = "GET", const char* origin = "http://localhost")
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
	const std::string request = std::string(method) + " " + target + " HTTP/1.1\r\nHost: localhost\r\nOrigin: " + origin + "\r\nConnection: close\r\n\r\n";
	assert(send(fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));
	std::string response;
	char buffer[4096];
	ssize_t count;
	while ((count = recv(fd, buffer, sizeof(buffer), 0)) > 0)
		response.append(buffer, count);
	close(fd);
	return response;
}
int main(int argc, char** argv)
{
	assert(argc == 2);
	// Every supported language must have browser keys and preserve format contracts.
	// pr9n: the same conversions as the English text (the account panel's "Signed in as %s" is in this range since the
	// panel's redesign; the check used to require none at all).
	SetLanguage(0, "");
	std::vector<std::string> english;
	for (int i = static_cast<int>(Str::Achievements); i < static_cast<int>(Str::Count); ++i)
		english.emplace_back(Tr(static_cast<Str>(i)));
	for (int language : {1, 2, 3, 20, 4, 5, 6, 7, 17, 0})
	{
		SetLanguage(language, "");
		std::unordered_set<std::string> keys;
		for (int i = static_cast<int>(Str::Achievements); i < static_cast<int>(Str::Count); ++i)
		{
			const auto id = static_cast<Str>(i);
			assert(*Tr(id) && keys.insert(Key(id)).second);
			assert(SameFormat(Tr(id), english[static_cast<size_t>(i - static_cast<int>(Str::Achievements))].c_str()));
		}
	}
	SetLanguage(0, "");
	assert(std::string(LanguageCode()) == "en" && std::string(Tr(Str::Achievements)) == "Achievements");
	std::ofstream overrides(std::string(argv[1]) + "/pt-BR.txt");
	overrides << "achievements.refresh = Atualizar personalizado\n";
	overrides.close();
	SetLanguage(17, argv[1]);
	assert(std::string(Tr(Str::Achievements)) == "Conquistas");
	assert(std::string(Tr(Str::AchievementRefresh)) == "Atualizar personalizado");
	std::atomic<bool> unlocked{false};
	std::atomic<int> snapshot_calls{0}, image_calls{0};
	WebConfig cfg;
	cfg.token_path = std::string(argv[1]) + "/token";
	cfg.port = 30000 + getpid() % 20000;
	cfg.achievements = [&] {
		++snapshot_calls;
		GameAchievementsState state;
		state.game_id = 1;
		state.title = "Fixture \"title\"";
		GameAchievement entry;
		entry.id = 7;
		entry.points = 5;
		entry.unlocked = unlocked;
		entry.title = "<script>fixture</script>";
		entry.description = "A line\nwith quotes \" and UTF-8: café";
		entry.image_url = "private-field-must-not-be-serialized";
		state.entries.push_back(entry);
		return state;
	};
	cfg.achievement_badge = [&](uint32_t id) {
		++image_calls;
		if (id != 7)
			return std::vector<uint8_t>{};
		return std::vector<uint8_t>{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0};
	};
	WebServer server;
	assert(server.Start(cfg));
	const std::string token = server.Token();
	assert(Request(server.Port(), "", "/api/achievements", "GET", "http://other.example").find("403") != std::string::npos);
	assert(snapshot_calls == 0);
	auto response = Request(server.Port(), token, "/api/achievements");
	assert(response.find("200 OK") != std::string::npos && response.find("Inicie um jogo") != std::string::npos);
	assert(snapshot_calls == 0);
	response = Request(server.Port(), token, "/api/state");
	assert(response.find("\"language\":\"pt-BR\"") != std::string::npos);
	assert(response.find("\"achievements.title\":\"Conquistas\"") != std::string::npos);
	assert(response.find("Atualizar personalizado") != std::string::npos);
	server.SetNowPlaying("/fixture/game.iso");
	response = Request(server.Port(), token, "/api/achievements");
	assert(response.find("\"unlocked\":false") != std::string::npos);
	assert(response.find("Fixture \\\"title\\\"") != std::string::npos);
	assert(response.find("A line\\nwith quotes") != std::string::npos);
	assert(response.find("private-field") == std::string::npos);
	unlocked = true;
	response = Request(server.Port(), token, "/api/achievements");
	assert(response.find("\"unlocked\":true") != std::string::npos);
	assert(Request(server.Port(), "", "/api/achievement-badge?id=7", "GET", "http://other.example").find("403") != std::string::npos);
	assert(image_calls == 0);
	for (const auto& id : {"../file", "-1", "0", "4294967296", "7junk"})
	{
		assert(Request(server.Port(), token, std::string("/api/achievement-badge?id=") + id).find("400") != std::string::npos);
	}
	assert(image_calls == 0);
	response = Request(server.Port(), token, "/api/achievement-badge?id=7");
	assert(response.find("200 OK") != std::string::npos && response.find("image/png") != std::string::npos);
	assert(Request(server.Port(), token, "/api/achievement-badge?id=8").find("202") != std::string::npos);
	assert(Request(server.Port(), token, "/api/achievements", "POST").find("404") != std::string::npos);
	server.SetNowPlaying("");
	const int before = image_calls;
	assert(Request(server.Port(), token, "/api/achievement-badge?id=7").find("404") != std::string::npos);
	assert(image_calls == before);
	server.Stop();
}
