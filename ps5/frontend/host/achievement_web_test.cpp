// In-game web API checks using a mutable runtime fixture (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../fe_web.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <string>
#include <atomic>
using namespace fe;
std::string Request(uint16_t port, const std::string& token, const std::string& target, const char* method = "GET")
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
	const std::string request = std::string(method) + " " + target + " HTTP/1.1\r\nHost: localhost\r\nX-Token: " + token + "\r\nConnection: close\r\n\r\n";
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
	assert(Request(server.Port(), "wrong", "/api/achievements").find("401") != std::string::npos);
	assert(snapshot_calls == 0);
	auto response = Request(server.Port(), token, "/api/achievements");
	assert(response.find("200 OK") != std::string::npos && response.find("Start a game") != std::string::npos);
	assert(snapshot_calls == 0);
	server.SetNowPlaying("/fixture/game.iso");
	response = Request(server.Port(), token, "/api/achievements");
	assert(response.find("\"unlocked\":false") != std::string::npos);
	assert(response.find("Fixture \\\"title\\\"") != std::string::npos);
	assert(response.find("A line\\nwith quotes") != std::string::npos);
	assert(response.find("private-field") == std::string::npos);
	unlocked = true;
	response = Request(server.Port(), token, "/api/achievements");
	assert(response.find("\"unlocked\":true") != std::string::npos);
	assert(Request(server.Port(), "wrong", "/api/achievement-badge?id=7").find("401") != std::string::npos);
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
