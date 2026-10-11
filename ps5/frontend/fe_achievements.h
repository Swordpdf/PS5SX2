// Controller-operated account panel, independent of the emulator/runtime (AI-assisted).
// 2026-10-05: redesigned (swordpdf: "it now looks trash. use the shell keyboard and make the ui for the inputs match our
// current ui"): the fields open the PS5's own keyboard when the app can (TextEntryService, fe_ps5.cpp); otherwise the
// panel's keyboard, now in pages (letters, capitals, symbols) with a row of Shift, symbols, Space, Delete and Done.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>

namespace fe
{
	struct AchievementAccountState
	{
		bool available = false;
		bool busy = false;
		bool authenticated = false;
		bool saved = false;
		std::string username;
		std::string message;
	};

	struct AchievementAccountService
	{
		std::function<AchievementAccountState()> state;
		std::function<bool(const std::string&, const std::string&)> login;
		std::function<void()> logout;
	};

	// The PS5's own keyboard over the app (2026-10-05). Unset, or open() false: there is none here, and the panel shows
	// its own.
	struct TextEntryService
	{
		// Opens the keyboard with this title and starting text; `password` hides what is typed.
		std::function<bool(const std::string& title, const std::string& text, bool password, unsigned max_length)> open;
		// While it is open: 0. Then 1 with the text typed, or -1 (cancelled or failed).
		std::function<int(std::string& text)> poll;
	};

	// All input is local to the console. No password passes through the LAN settings page.
	class AchievementAccountPanel
	{
	public:
		~AchievementAccountPanel() { ClearPassword(); }

		// The rows: signed out, the two fields and Sign in; signed in, Sign out alone.
		enum Row : int
		{
			RowUsername = 0,
			RowPassword = 1,
			RowSignIn = 2,
			RowSignOut = 0,
		};

		// The panel's keyboard: three pages of four rows each, every printable ASCII character on one of them (the
		// space is a key of the function row), and the function row under them.
		static constexpr int Pages = 3, CharRows = 4, Columns = 10;
		static constexpr const char* PageRows[Pages][CharRows] = {
			{"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"},
			{"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
			{"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?", "`~"},
		};
		enum Fn : int
		{
			FnShift = 0,
			FnSymbols,
			FnSpace,
			FnDelete,
			FnDone,
			FnCount
		};
		// The function row's widths, in keys (they add up to Columns).
		static constexpr float FnWidth[FnCount] = {1.5f, 1.5f, 4.0f, 1.5f, 1.5f};
		static constexpr int FnRow = CharRows; // the keyboard's last row
		static constexpr unsigned MaxUsername = 128, MaxPassword = 256;

		bool open = false;
		bool editing = false; // the panel's keyboard is up for `row`
		bool show_password = false;
		int row = 0;
		int page = 0, key_row = 1, key_col = 0; // the panel's keyboard: page and focused key
		double sign_out_armed_until = -1; // Sign out wants a second press until then
		std::string username;
		std::string password;
		AchievementAccountState account;

		bool SignedIn() const { return account.saved; }
		int RowCount() const { return SignedIn() ? 1 : 3; }
		bool CanSignIn() const { return !username.empty() && !password.empty() && !account.busy; }
		std::string& Field(int r) { return r == RowUsername ? username : password; }
		const std::string& Field(int r) const { return r == RowUsername ? username : password; }

		// The keys of a row of the panel's keyboard (FnCount for the function row).
		static int RowKeys(int page, int r) { return r == FnRow ? FnCount : static_cast<int>(std::strlen(PageRows[page][r])); }
		// A key's centre, in keys from the keyboard's left edge (the character rows are centred).
		static float KeyCentre(int page, int r, int c)
		{
			if (r == FnRow)
			{
				float x = 0;
				for (int i = 0; i < c; i++)
					x += FnWidth[i];
				return x + FnWidth[c] * 0.5f;
			}
			return (Columns - RowKeys(page, r)) * 0.5f + c + 0.5f;
		}

		void Poll(const AchievementAccountService& service)
		{
			if (service.state)
				account = service.state();
			row = std::clamp(row, 0, RowCount() - 1);
		}
		void Open()
		{
			open = true;
			editing = false;
			row = 0;
			sign_out_armed_until = -1;
			username = account.username;
			ClearPassword();
		}
		void Close()
		{
			open = editing = false;
			ClearPassword();
		}
		void Move(int dx, int dy)
		{
			if (account.busy)
				return;
			if (!editing)
			{
				row = std::clamp(row + dy, 0, RowCount() - 1);
				return;
			}
			if (dy != 0)
			{
				// Up and down land on the key nearest the one left.
				const int to = std::clamp(key_row + dy, 0, FnRow);
				if (to != key_row)
				{
					const float x = KeyCentre(page, key_row, key_col);
					int best = 0;
					for (int c = 1; c < RowKeys(page, to); c++)
						if (std::abs(KeyCentre(page, to, c) - x) < std::abs(KeyCentre(page, to, best) - x))
							best = c;
					key_row = to;
					key_col = best;
				}
			}
			if (dx != 0)
				key_col = std::clamp(key_col + dx, 0, RowKeys(page, key_row) - 1);
		}
		// Circle: the keyboard closes first, then the panel (never while signing in: the game must not start then).
		void Back()
		{
			if (account.busy)
				return;
			if (editing)
				editing = false;
			else
				Close();
		}
		void Erase()
		{
			if (!editing || account.busy)
				return;
			std::string& value = Field(row);
			if (!value.empty())
			{
				value.back() = '\0';
				value.pop_back();
			}
		}
		void TogglePasswordVisibility()
		{
			if (!account.busy)
				show_password = !show_password;
		}
		// The panel's keyboard for the focused field, on its letters.
		void StartKeyboard()
		{
			editing = true;
			page = 0;
			key_row = 1;
			key_col = 0;
		}
		// What the PS5's keyboard typed into a field.
		void SetField(int r, const std::string& text)
		{
			std::string& value = Field(r);
			std::fill(value.begin(), value.end(), '\0');
			value = text.substr(0, r == RowUsername ? MaxUsername : MaxPassword);
		}
		// Cross. On a field: true, and the caller opens a keyboard for `row` (the PS5's, or StartKeyboard()). On Sign in
		// and Sign out (a second press within 4 s), the service does it.
		bool Accept(const AchievementAccountService& service, double now = 0)
		{
			if (account.busy)
				return false;
			if (editing)
			{
				PressKey();
				return false;
			}
			if (SignedIn())
			{
				if (now > sign_out_armed_until)
				{
					sign_out_armed_until = now + 4.0;
					return false;
				}
				sign_out_armed_until = -1;
				ClearPassword();
				if (service.logout)
					service.logout();
				Poll(service);
				row = 0;
				return false;
			}
			if (row == RowUsername || row == RowPassword)
				return true;
			if (row == RowSignIn && CanSignIn() && service.login && service.login(username, password))
				Poll(service);
			return false;
		}
		bool SignOutArmed(double now) const { return SignedIn() && now <= sign_out_armed_until; }
		void ClearPassword()
		{
			show_password = false;
			std::fill(password.begin(), password.end(), '\0');
			password.clear();
		}

	private:
		void PressKey()
		{
			std::string& value = Field(row);
			const unsigned max = row == RowUsername ? MaxUsername : MaxPassword;
			if (key_row < FnRow)
			{
				if (value.size() < max)
					value += PageRows[page][key_row][key_col];
				return;
			}
			switch (key_col)
			{
				case FnShift: page = page == 0 ? 1 : 0; break;
				case FnSymbols: page = page == 2 ? 0 : 2; break;
				case FnSpace:
					if (value.size() < max)
						value += ' ';
					break;
				case FnDelete:
					if (!value.empty())
					{
						value.back() = '\0';
						value.pop_back();
					}
					break;
				case FnDone:
					editing = false;
					if (row < RowSignIn)
						row++; // username, then password, then Sign in
					break;
			}
		}
	};
} // namespace fe
