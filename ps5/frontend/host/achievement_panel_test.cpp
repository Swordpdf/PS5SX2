// Controller login flow without Vulkan, a console or real credentials (AI-assisted).
// 2026-10-05: the redesigned panel: fields that ask for a keyboard, the paged keyboard, Sign out confirmed.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../fe_achievements.h"

#include <cassert>
#include <string>

namespace
{
	using Panel = fe::AchievementAccountPanel;

	// Moves the panel's keyboard to the key typing `c` (switching pages through the function row) and presses it.
	void Type(Panel& panel, const fe::AchievementAccountService& service, char c)
	{
		int want_page = -1, want_row = -1, want_col = -1;
		for (int p = 0; p < Panel::Pages && want_page < 0; p++)
			for (int r = 0; r < Panel::CharRows && want_page < 0; r++)
				for (int k = 0; Panel::PageRows[p][r][k]; k++)
					if (Panel::PageRows[p][r][k] == c)
					{
						want_page = p;
						want_row = r;
						want_col = k;
						break;
					}
		if (c == ' ')
		{
			panel.Move(0, 99);
			panel.key_col = Panel::FnSpace;
			panel.Accept(service);
			return;
		}
		assert(want_page >= 0);
		while (panel.page != want_page)
		{
			panel.Move(0, 99); // the function row
			panel.key_col = want_page == 2 || panel.page == 2 ? Panel::FnSymbols : Panel::FnShift;
			panel.Accept(service);
		}
		panel.key_row = want_row;
		panel.key_col = want_col;
		panel.Accept(service);
	}
} // namespace

int main()
{
	fe::AchievementAccountState state;
	state.available = true;
	int attempts = 0, logouts = 0;
	fe::AchievementAccountService service{
		[&] { return state; },
		[&](const std::string& username, const std::string& password) {
			assert(username == "aZ" && password == "!x y");
			state.busy = true;
			attempts++;
			return true;
		},
		[&] { state = {}; logouts++; }};
	Panel panel;
	panel.Poll(service);
	panel.Open();
	assert(panel.open && panel.row == Panel::RowUsername && panel.RowCount() == 3);

	// Cross on a field asks for a keyboard; this console has none of its own, so the panel's.
	assert(panel.Accept(service));
	panel.StartKeyboard();
	assert(panel.editing && panel.page == 0 && panel.key_row == 1 && panel.key_col == 0); // on "q"
	panel.Move(0, 1); // the nearest key below "q" is "a"
	assert(panel.key_row == 2 && panel.key_col == 0);
	panel.Accept(service);
	Type(panel, service, 'Z');
	assert(panel.username == "aZ" && panel.page == 1);
	// Done (the function row's last key) closes the keyboard and moves on to the password.
	panel.Move(0, 99);
	panel.key_col = Panel::FnDone;
	panel.Accept(service);
	assert(!panel.editing && panel.row == Panel::RowPassword);

	assert(panel.Accept(service));
	panel.StartKeyboard();
	Type(panel, service, '!');
	Type(panel, service, 'x');
	Type(panel, service, 'q');
	panel.Erase(); // Triangle
	Type(panel, service, ' ');
	Type(panel, service, 'y');
	assert(panel.password == "!x y");
	assert(!panel.show_password);
	panel.TogglePasswordVisibility();
	assert(panel.show_password);
	panel.Back(); // the keyboard closes, the panel stays
	assert(!panel.editing && panel.open);

	// Sign in.
	panel.Move(0, 1);
	assert(panel.row == Panel::RowSignIn && panel.CanSignIn());
	panel.Accept(service);
	assert(attempts == 1 && panel.account.busy && panel.password == "!x y");
	panel.Accept(service);
	panel.Back();
	panel.Move(0, -1);
	assert(attempts == 1 && panel.open && panel.row == Panel::RowSignIn); // nothing moves or leaves while signing in
	// A failed attempt ends the worker; the password stays for another try.
	state.busy = false;
	panel.Poll(service);
	panel.Accept(service);
	assert(attempts == 2 && panel.account.busy);

	// Signed in: one row, Sign out, which wants a second press within 4 s.
	state.busy = false;
	state.saved = state.authenticated = true;
	state.username = "aZ";
	panel.Poll(service);
	assert(panel.SignedIn() && panel.RowCount() == 1 && panel.row == 0);
	panel.Accept(service, 10.0);
	assert(logouts == 0 && panel.SignOutArmed(11.0));
	panel.Accept(service, 15.5); // too late: armed again instead
	assert(logouts == 0 && panel.SignOutArmed(16.0));
	panel.Accept(service, 17.0);
	assert(logouts == 1 && !panel.account.saved && panel.password.empty() && panel.RowCount() == 3);
	panel.Close();
	assert(!panel.open && panel.password.empty());

	// Every printable ASCII character is on a page (the space is a function key).
	for (char c = '!'; c <= '~'; c++)
	{
		int found = 0;
		for (int p = 0; p < Panel::Pages; p++)
			for (int r = 0; r < Panel::CharRows; r++)
				for (int k = 0; Panel::PageRows[p][r][k]; k++)
					found += Panel::PageRows[p][r][k] == c && !(p == 1 && r == 0); // the digits are on two pages
		assert(found >= 1);
	}
	// The function row fills the keyboard's width.
	float width = 0;
	for (float w : Panel::FnWidth)
		width += w;
	assert(width == Panel::Columns);

	// The keyboard stays inside its rows and keys.
	panel.Open();
	panel.Accept(service);
	panel.StartKeyboard();
	panel.Move(-100, -100);
	assert(panel.key_row == 0 && panel.key_col == 0);
	panel.Move(100, 100);
	assert(panel.key_row == Panel::FnRow && panel.key_col == Panel::FnCount - 1);
	// The PS5's keyboard's text, cut to the field's length.
	panel.SetField(Panel::RowUsername, std::string(500, 'u'));
	assert(panel.username.size() == Panel::MaxUsername);
	return 0;
}
