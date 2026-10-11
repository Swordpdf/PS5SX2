// Verify the platform adapter preserves titles/descriptions and filters unrelated OSD (AI-assisted).
// 2026-10-05: also the toast picture choice (a cached file, else an https URL, else none) and that the generic
// notifications pass a cached picture along with the system's default sound and channel.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "ps5/coreorbis/orbis-shims/ProsperoNotify.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <unistd.h>

namespace
{
	std::string last_title, last_description, last_icon, last_sound, last_channel;
	int rich_count = 0, plain_count = 0;
} // namespace
void pxOnAssertFail(const char*, int, const char*, const char*) { std::abort(); }
void OrbisNotifyPlain(const char* text)
{
	last_title = text;
	plain_count++;
}
void OrbisNotifyRich(const char* title, const char* description, const char* icon, const char* sound, const char* channel)
{
	last_title = title;
	last_description = description;
	last_icon = icon ? icon : "";
	last_sound = sound ? sound : "";
	last_channel = channel ? channel : "";
	rich_count++;
}

int main()
{
	// The picture: a cached file wins, an https URL stands in for one not cached yet, anything else is left out.
	char cached[] = "/tmp/ps5sx2-toast-XXXXXX";
	const int fd = mkstemp(cached);
	assert(fd >= 0);
	const ssize_t written = write(fd, "\x89PNG", 4);
	assert(written == 4);
	(void)written;
	close(fd);
	const std::string url = "https://media.retroachievements.org/Badge/100000.png";
	assert(OrbisToastIcon(cached, url) == cached);
	assert(OrbisToastIcon("/data/PCSX2/cache/achievement_images/missing.png", url) == url);
	assert(OrbisToastIcon("/data/PCSX2/cache/achievement_images/missing.png", "http://insecure.example/a.png").empty());
	assert(OrbisToastIcon("", "").empty());
	assert(OrbisToastIcon("/tmp", url) == url); // a folder is not a picture

	// Generic notifications (leaderboards, connection messages) keep their text and a cached picture, with the
	// system's default sound and channel. Unlocks and masteries don't come this way (Achievements.cpp, PS5Toast).
	ImGuiFullscreen::AddNotification("leaderboard_7", 5, "Synthetic leaderboard", "Synthetic description", cached);
	assert(rich_count == 1 && last_title == "Synthetic leaderboard" && last_description == "Synthetic description");
	assert(last_icon == cached && last_sound.empty() && last_channel.empty());
	ImGuiFullscreen::AddNotification("achievements_disconnect", 5, "Achievements Disconnected", "Retrying", "missing.png");
	assert(rich_count == 2 && last_icon.empty());
	unlink(cached);

	Host::AddOSDMessage("Unrelated renderer message", 5);
	assert(plain_count == 0);
	Host::AddOSDMessage("Achievements error: simulated network error", 5);
	assert(plain_count == 1);
	Host::AddKeyedOSDMessage("retroachievements_disc_read_failed", "Could not read the disc", 5);
	assert(plain_count == 2 && last_title == "Could not read the disc");
}
