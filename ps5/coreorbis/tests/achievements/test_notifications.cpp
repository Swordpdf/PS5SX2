// Verify the platform adapter preserves titles/descriptions and filters unrelated OSD (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"

#include <cassert>
#include <string>

namespace
{
	std::string last_title, last_description;
	int rich_count = 0, plain_count = 0;
} // namespace
void pxOnAssertFail(const char*, int, const char*, const char*) { std::abort(); }
void OrbisNotifyPlain(const char* text)
{
	last_title = text;
	plain_count++;
}
void OrbisNotifyRich(const char* title, const char* description, const char* icon)
{
	last_title = title;
	last_description = description;
	assert(std::string(icon).empty());
	rich_count++;
}

int main()
{
	ImGuiFullscreen::AddNotification("achievement_unlock_1", 5, "Synthetic achievement", "Synthetic description", "badge.png");
	assert(rich_count == 1 && last_title == "Synthetic achievement" && last_description == "Synthetic description");
	Host::AddOSDMessage("Unrelated renderer message", 5);
	assert(plain_count == 0);
	Host::AddOSDMessage("Achievements error: simulated network error", 5);
	assert(plain_count == 1);
	Host::AddKeyedOSDMessage("retroachievements_disc_read_failed", "Could not read the disc", 5);
	assert(plain_count == 2 && last_title == "Could not read the disc");
}
