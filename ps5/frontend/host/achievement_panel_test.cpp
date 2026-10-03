// Controller login flow without Vulkan, a console or real credentials (AI-assisted).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../fe_achievements.h"

#include <cassert>

int main()
{
	fe::AchievementAccountState state;
	state.available = true;
	int attempts = 0, logouts = 0;
	fe::AchievementAccountService service{
		[&] { return state; },
		[&](const std::string& username, const std::string& password) {
			assert(username == "a" && password == "!");
			state.busy = true;
			attempts++;
			return true;
		},
		[&] { state = {}; logouts++; }};
	fe::AchievementAccountPanel panel;
	panel.Poll(service);
	panel.Open();
	panel.Accept(service); // username keyboard
	panel.key = fe::AchievementAccountPanel::KeyIndex('a');
	panel.Accept(service);
	panel.Back();
	panel.Move(0, 1);
	panel.Accept(service); // password keyboard
	panel.key = fe::AchievementAccountPanel::KeyIndex('!');
	panel.Accept(service);
	panel.Accept(service);
	panel.Erase();
	assert(panel.password == "!");
	assert(!panel.show_password);
	panel.TogglePasswordVisibility();
	assert(panel.show_password);
	panel.Back();
	panel.Move(0, 1);
	panel.Accept(service);
	assert(attempts == 1 && panel.password == "!" && panel.show_password && panel.account.busy);
	panel.TogglePasswordVisibility();
	assert(panel.show_password);
	panel.Accept(service);
	panel.Back();
	assert(attempts == 1 && panel.open); // cannot leave during login or submit twice
	// A failed response finishes the worker; retry uses the retained local password.
	state.busy = false;
	panel.Poll(service);
	panel.Accept(service);
	assert(attempts == 2 && panel.password == "!" && panel.account.busy);
	state.busy = false;
	state.saved = state.authenticated = true;
	state.username = "a";
	panel.Poll(service);
	panel.Move(0, 1);
	panel.Accept(service);
	assert(logouts == 1 && !panel.account.saved);
	panel.Close();
	assert(!panel.open && panel.password.empty());
	// Every printable ASCII key is reachable, including symbols needed by passwords.
	static_assert(sizeof(fe::AchievementAccountPanel::Keys) == fe::AchievementAccountPanel::KeyCount + 1);
	for (char value = ' '; value <= '~'; value++)
		assert(fe::AchievementAccountPanel::Keys[fe::AchievementAccountPanel::KeyIndex(value)] == value);
	panel.Open();
	panel.Accept(service);
	panel.key = 0;
	panel.Move(-1, -1);
	assert(panel.key == 0);
	panel.Move(100, 100);
	assert(panel.key == fe::AchievementAccountPanel::KeyCount - 1);
}
