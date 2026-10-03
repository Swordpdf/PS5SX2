// Read-only game achievement browser, independent of the running VM (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace fe
{
	struct GameAchievement
	{
		uint32_t id = 0, points = 0;
		bool unlocked = false;
		std::string title, description, image_url;
		std::shared_ptr<const std::vector<uint8_t>> image;
	};
	struct GameAchievementsState
	{
		bool busy = false;
	uint32_t game_id = 0;
		uint64_t revision = 0;
		std::string path, title, message;
		std::vector<GameAchievement> entries;
	};
	struct GameAchievementsService
	{
		std::function<GameAchievementsState()> state;
		std::function<bool(const std::string&)> load;
		std::function<void()> cancel;
	};
} // namespace fe
